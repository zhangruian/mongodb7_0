/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/query/sbe_stage_builder_filter.h"

#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/traverse.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_internal_expr_eq.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_text.h"
#include "mongo/db/matcher/expression_text_noop.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/expression_where.h"
#include "mongo/db/matcher/expression_where_noop.h"
#include "mongo/db/matcher/match_expression_walker.h"
#include "mongo/db/matcher/schema/expression_internal_schema_all_elem_match_from_index.h"
#include "mongo/db/matcher/schema/expression_internal_schema_allowed_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_cond.h"
#include "mongo/db/matcher/schema/expression_internal_schema_eq.h"
#include "mongo/db/matcher/schema/expression_internal_schema_fmod.h"
#include "mongo/db/matcher/schema/expression_internal_schema_match_array_index.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_object_match.h"
#include "mongo/db/matcher/schema/expression_internal_schema_root_doc_eq.h"
#include "mongo/db/matcher/schema/expression_internal_schema_unique_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_xor.h"
#include "mongo/logv2/log.h"
#include "mongo/util/str.h"

namespace mongo::stage_builder {
namespace {
/**
 * The various flavors of PathMatchExpressions require the same skeleton of traverse operators in
 * order to perform implicit path traversal, but may translate differently to an SBE expression that
 * actually applies the predicate against an individual array element.
 *
 * A function of this type can be called to generate an EExpression which applies a predicate to the
 * value found in 'inputSlot'.
 */
using MakePredicateEExprFn =
    std::function<std::unique_ptr<sbe::EExpression>(sbe::value::SlotId inputSlot)>;

/**
 * A struct for storing context across calls to visit() methods in MatchExpressionVisitor's.
 */
struct MatchExpressionVisitorContext {
    MatchExpressionVisitorContext(sbe::value::SlotIdGenerator* slotIdGenerator,
                                  std::unique_ptr<sbe::PlanStage> inputStage,
                                  sbe::value::SlotId inputVar)
        : slotIdGenerator{slotIdGenerator}, inputStage{std::move(inputStage)}, inputVar{inputVar} {}

    std::unique_ptr<sbe::PlanStage> done() {
        if (!predicateVars.empty()) {
            invariant(predicateVars.size() == 1);
            inputStage = sbe::makeS<sbe::FilterStage<false>>(
                std::move(inputStage), sbe::makeE<sbe::EVariable>(predicateVars.top()));
            predicateVars.pop();
        }
        return std::move(inputStage);
    }

    sbe::value::SlotIdGenerator* slotIdGenerator;
    std::unique_ptr<sbe::PlanStage> inputStage;
    std::stack<sbe::value::SlotId> predicateVars;
    std::stack<std::pair<const MatchExpression*, size_t>> nestedLogicalExprs;
    sbe::value::SlotId inputVar;
};

/**
 * A helper function to generate a path traversal plan stage at the given nested 'level' of the
 * traversal path. For example, for a dotted path expression {'a.b': 2}, the traversal sub-tree will
 * look like this:
 *
 *     traverse
 *          traversePredicateVar // the global traversal result
 *          elemPredicateVar1 // the result coming from the 'in' branch
 *          fieldVar1 // field 'a' projected in the 'from' branch, this is the field we will be
 *                    // traversing
 *          {traversePredicateVar || elemPredicateVar1} // the folding expression - combining
 *                                                      // results for each element
 *          {traversePredicateVar} // final (early out) expression - when we hit the 'true' value,
 *                                 // we don't have to traverse the whole array
 *      in
 *          project [elemPredicateVar1 = traversePredicateVar]
 *          traverse // nested traversal
 *              traversePredicateVar // the global traversal result
 *              elemPredicateVar2 // the result coming from the 'in' branch
 *              fieldVar2 // field 'b' projected in the 'from' branch, this is the field we will be
 *                        // traversing
 *              {traversePredicateVar || elemPredicateVar2} // the folding expression
 *              {traversePredicateVar} // final (early out) expression
 *          in
 *              project [elemPredicateVar2 = fieldVar2==2] // compare the field 'b' to 2 and store
 *                                                         // the bool result in elemPredicateVar2
 *              limit 1
 *              coscan
 *          from
 *              project [fieldVar2=getField(fieldVar1, 'b')] // project field 'b' from the document
 *                                                           // bound to 'fieldVar1', which is
 *                                                           // field 'a'
 *              limit 1
 *              coscan
 *      from
 *         project [fieldVar1=getField(inputVar, 'a')] // project field 'a' from the document bound
 *                                                     // to 'inputVar'
 *         <inputStage>  // e.g., COLLSCAN
 */
std::unique_ptr<sbe::PlanStage> generateTraverseHelper(MatchExpressionVisitorContext* context,
                                                       std::unique_ptr<sbe::PlanStage> inputStage,
                                                       sbe::value::SlotId inputVar,
                                                       const PathMatchExpression* expr,
                                                       MakePredicateEExprFn makeEExprCallback,
                                                       size_t level) {
    using namespace std::literals;

    FieldPath path{expr->path()};
    invariant(level < path.getPathLength());

    // The global traversal result.
    const auto& traversePredicateVar = context->predicateVars.top();
    // The field we will be traversing at the current nested level.
    auto fieldVar{context->slotIdGenerator->generate()};
    // The result coming from the 'in' branch of the traverse plan stage.
    auto elemPredicateVar{context->slotIdGenerator->generate()};

    // Generate the projection stage to read a sub-field at the current nested level and bind it
    // to 'fieldVar'.
    inputStage = sbe::makeProjectStage(
        std::move(inputStage),
        fieldVar,
        sbe::makeE<sbe::EFunction>(
            "getField"sv,
            sbe::makeEs(sbe::makeE<sbe::EVariable>(inputVar), sbe::makeE<sbe::EConstant>([&]() {
                            auto fieldName = path.getFieldName(level);
                            return std::string_view{fieldName.rawData(), fieldName.size()};
                        }()))));

    std::unique_ptr<sbe::PlanStage> innerBranch;
    if (level == path.getPathLength() - 1u) {
        innerBranch = sbe::makeProjectStage(
            sbe::makeS<sbe::LimitSkipStage>(sbe::makeS<sbe::CoScanStage>(), 1, boost::none),
            elemPredicateVar,
            makeEExprCallback(fieldVar));
    } else {
        // Generate nested traversal.
        innerBranch = sbe::makeProjectStage(
            generateTraverseHelper(
                context,
                sbe::makeS<sbe::LimitSkipStage>(sbe::makeS<sbe::CoScanStage>(), 1, boost::none),
                fieldVar,
                expr,
                makeEExprCallback,
                level + 1),
            elemPredicateVar,
            sbe::makeE<sbe::EVariable>(traversePredicateVar));
    }

    // The final traverse stage for the current nested level.
    return sbe::makeS<sbe::TraverseStage>(
        std::move(inputStage),
        std::move(innerBranch),
        fieldVar,
        traversePredicateVar,
        elemPredicateVar,
        sbe::makeSV(),
        sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::logicOr,
                                     sbe::makeE<sbe::EVariable>(traversePredicateVar),
                                     sbe::makeE<sbe::EVariable>(elemPredicateVar)),
        sbe::makeE<sbe::EVariable>(traversePredicateVar),
        2);
}

/**
 * For the given PathMatchExpression 'expr', generates a path traversal SBE plan stage sub-tree
 * implementing the expression. Generates a sequence of nested traverse operators in order to
 * perform nested array traversal, and then calls 'makeEExprCallback' in order to generate an SBE
 * expression responsible for applying the predicate to individual array elements.
 */
void generateTraverse(MatchExpressionVisitorContext* context,
                      const PathMatchExpression* expr,
                      MakePredicateEExprFn makeEExprCallback) {
    context->predicateVars.push(context->slotIdGenerator->generate());
    context->inputStage = generateTraverseHelper(context,
                                                 std::move(context->inputStage),
                                                 context->inputVar,
                                                 expr,
                                                 std::move(makeEExprCallback),
                                                 0);

    // If this comparison expression is a branch of a logical $and expression, but not the last
    // one, inject a filter stage to bail out early from the $and predicate without the need to
    // evaluate all branches. If this is the last branch of the $and expression, or if it's not
    // within a logical expression at all, just keep the predicate var on the top on the stack
    // and let the parent expression process it.
    if (!context->nestedLogicalExprs.empty() && context->nestedLogicalExprs.top().second > 1 &&
        context->nestedLogicalExprs.top().first->matchType() == MatchExpression::AND) {
        context->inputStage = sbe::makeS<sbe::FilterStage<false>>(
            std::move(context->inputStage),
            sbe::makeE<sbe::EVariable>(context->predicateVars.top()));
        context->predicateVars.pop();
    }
}

/**
 * Generates a path traversal SBE plan stage sub-tree which implments the comparison match
 * expression 'expr'. The comparison itself executes using the given 'binaryOp'.
 */
void generateTraverseForComparisonPredicate(MatchExpressionVisitorContext* context,
                                            const ComparisonMatchExpression* expr,
                                            sbe::EPrimBinary::Op binaryOp) {
    auto makeEExprFn = [expr, binaryOp](sbe::value::SlotId inputSlot) {
        const auto& rhs = expr->getData();
        auto [tagView, valView] = sbe::bson::convertFrom(
            true, rhs.rawdata(), rhs.rawdata() + rhs.size(), rhs.fieldNameSize() - 1);

        // SBE EConstant assumes ownership of the value so we have to make a copy here.
        auto [tag, val] = sbe::value::copyValue(tagView, valView);

        return sbe::makeE<sbe::EPrimBinary>(
            binaryOp, sbe::makeE<sbe::EVariable>(inputSlot), sbe::makeE<sbe::EConstant>(tag, val));
    };
    generateTraverse(context, expr, std::move(makeEExprFn));
}

/**
 * Generates an SBE plan stage sub-tree implementing a logical $or expression.
 */
void generateLogicalOr(MatchExpressionVisitorContext* context, const OrMatchExpression* expr) {
    invariant(!context->predicateVars.empty());
    invariant(context->predicateVars.size() >= expr->numChildren());

    auto filter = sbe::makeE<sbe::EVariable>(context->predicateVars.top());
    context->predicateVars.pop();

    auto numOrBranches = expr->numChildren() - 1;
    for (size_t childNum = 0; childNum < numOrBranches; ++childNum) {
        filter =
            sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::logicOr,
                                         std::move(filter),
                                         sbe::makeE<sbe::EVariable>(context->predicateVars.top()));
        context->predicateVars.pop();
    }

    // If this $or expression is a branch of another $and expression, or is a top-level logical
    // expression we can just inject a filter stage without propagating the result of the predicate
    // evaluation to the parent expression, to form a sub-tree of stage->FILTER->stage->FILTER plan
    // stages to support early exit for the $and branches. Otherwise, just project out the result
    // of the predicate evaluation and let the parent expression handle it.
    if (context->nestedLogicalExprs.empty() ||
        context->nestedLogicalExprs.top().first->matchType() == MatchExpression::AND) {
        context->inputStage =
            sbe::makeS<sbe::FilterStage<false>>(std::move(context->inputStage), std::move(filter));
    } else {
        context->predicateVars.push(context->slotIdGenerator->generate());
        context->inputStage = sbe::makeProjectStage(
            std::move(context->inputStage), context->predicateVars.top(), std::move(filter));
    }
}

/**
 * Generates an SBE plan stage sub-tree implementing a logical $and expression.
 */
void generateLogicalAnd(MatchExpressionVisitorContext* context, const AndMatchExpression* expr) {
    auto filter = [&]() {
        if (expr->numChildren() > 0) {
            invariant(!context->predicateVars.empty());
            auto predicateVar = context->predicateVars.top();
            context->predicateVars.pop();
            return sbe::makeE<sbe::EVariable>(predicateVar);
        } else {
            return sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean, 1);
        }
    }();

    // If this $and expression is a branch of another $and expression, or is a top-level logical
    // expression we can just inject a filter stage without propagating the result of the predicate
    // evaluation to the parent expression, to form a sub-tree of stage->FILTER->stage->FILTER plan
    // stages to support early exit for the $and branches. Otherwise, just project out the result
    // of the predicate evaluation and let the parent expression handle it.
    if (context->nestedLogicalExprs.empty() ||
        context->nestedLogicalExprs.top().first->matchType() == MatchExpression::AND) {
        context->inputStage =
            sbe::makeS<sbe::FilterStage<false>>(std::move(context->inputStage), std::move(filter));
    } else {
        context->predicateVars.push(context->slotIdGenerator->generate());
        context->inputStage = sbe::makeProjectStage(
            std::move(context->inputStage), context->predicateVars.top(), std::move(filter));
    }
}

/**
 * A match expression pre-visitor used for maintaining nested logical expressions while traversing
 * the match expression tree.
 */
class MatchExpressionPreVisitor final : public MatchExpressionConstVisitor {
public:
    MatchExpressionPreVisitor(MatchExpressionVisitorContext* context) : _context(context) {}

    void visit(const AlwaysFalseMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const AlwaysTrueMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const AndMatchExpression* expr) final {
        _context->nestedLogicalExprs.push({expr, expr->numChildren()});
    }
    void visit(const BitsAllClearMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const BitsAllSetMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const BitsAnyClearMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const BitsAnySetMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const ElemMatchObjectMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const ElemMatchValueMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const EqualityMatchExpression* expr) final {}
    void visit(const ExistsMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const ExprMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const GTEMatchExpression* expr) final {}
    void visit(const GTMatchExpression* expr) final {}
    void visit(const GeoMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const GeoNearMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalExprEqMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaAllElemMatchFromIndexMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaBinDataEncryptedTypeExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaBinDataSubTypeExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaCondMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaEqMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaFmodMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaMatchArrayIndexMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaMaxItemsMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaMaxLengthMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaMaxPropertiesMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaMinItemsMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaMinLengthMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaMinPropertiesMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaObjectMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaRootDocEqMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaTypeExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaUniqueItemsMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const InternalSchemaXorMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const LTEMatchExpression* expr) final {}
    void visit(const LTMatchExpression* expr) final {}
    void visit(const ModMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const NorMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const NotMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const OrMatchExpression* expr) final {
        _context->nestedLogicalExprs.push({expr, expr->numChildren()});
    }
    void visit(const RegexMatchExpression* expr) final {}
    void visit(const SizeMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const TextMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const TextNoOpMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const TwoDPtInAnnulusExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const TypeMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const WhereMatchExpression* expr) final {
        unsupportedExpression(expr);
    }
    void visit(const WhereNoOpMatchExpression* expr) final {
        unsupportedExpression(expr);
    }

private:
    void unsupportedExpression(const MatchExpression* expr) const {
        uasserted(4822878,
                  str::stream() << "Match expression is not supported in SBE: "
                                << expr->matchType());
    }

    MatchExpressionVisitorContext* _context;
};

/**
 * A match expression post-visitor which does all the job to translate the match expression tree
 * into an SBE plan stage sub-tree.
 */
class MatchExpressionPostVisitor final : public MatchExpressionConstVisitor {
public:
    MatchExpressionPostVisitor(MatchExpressionVisitorContext* context) : _context(context) {}

    void visit(const AlwaysFalseMatchExpression* expr) final {}
    void visit(const AlwaysTrueMatchExpression* expr) final {}
    void visit(const AndMatchExpression* expr) final {
        _context->nestedLogicalExprs.pop();
        generateLogicalAnd(_context, expr);
    }
    void visit(const BitsAllClearMatchExpression* expr) final {}
    void visit(const BitsAllSetMatchExpression* expr) final {}
    void visit(const BitsAnyClearMatchExpression* expr) final {}
    void visit(const BitsAnySetMatchExpression* expr) final {}
    void visit(const ElemMatchObjectMatchExpression* expr) final {}
    void visit(const ElemMatchValueMatchExpression* expr) final {}
    void visit(const EqualityMatchExpression* expr) final {
        generateTraverseForComparisonPredicate(_context, expr, sbe::EPrimBinary::eq);
    }
    void visit(const ExistsMatchExpression* expr) final {}
    void visit(const ExprMatchExpression* expr) final {}
    void visit(const GTEMatchExpression* expr) final {
        generateTraverseForComparisonPredicate(_context, expr, sbe::EPrimBinary::greaterEq);
    }
    void visit(const GTMatchExpression* expr) final {
        generateTraverseForComparisonPredicate(_context, expr, sbe::EPrimBinary::greater);
    }
    void visit(const GeoMatchExpression* expr) final {}
    void visit(const GeoNearMatchExpression* expr) final {}
    void visit(const InMatchExpression* expr) final {}
    void visit(const InternalExprEqMatchExpression* expr) final {}
    void visit(const InternalSchemaAllElemMatchFromIndexMatchExpression* expr) final {}
    void visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaBinDataEncryptedTypeExpression* expr) final {}
    void visit(const InternalSchemaBinDataSubTypeExpression* expr) final {}
    void visit(const InternalSchemaCondMatchExpression* expr) final {}
    void visit(const InternalSchemaEqMatchExpression* expr) final {}
    void visit(const InternalSchemaFmodMatchExpression* expr) final {}
    void visit(const InternalSchemaMatchArrayIndexMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxLengthMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaMinItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaMinLengthMatchExpression* expr) final {}
    void visit(const InternalSchemaMinPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaObjectMatchExpression* expr) final {}
    void visit(const InternalSchemaRootDocEqMatchExpression* expr) final {}
    void visit(const InternalSchemaTypeExpression* expr) final {}
    void visit(const InternalSchemaUniqueItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaXorMatchExpression* expr) final {}
    void visit(const LTEMatchExpression* expr) final {
        generateTraverseForComparisonPredicate(_context, expr, sbe::EPrimBinary::lessEq);
    }
    void visit(const LTMatchExpression* expr) final {
        generateTraverseForComparisonPredicate(_context, expr, sbe::EPrimBinary::less);
    }
    void visit(const ModMatchExpression* expr) final {}
    void visit(const NorMatchExpression* expr) final {}
    void visit(const NotMatchExpression* expr) final {}
    void visit(const OrMatchExpression* expr) final {
        _context->nestedLogicalExprs.pop();
        generateLogicalOr(_context, expr);
    }

    void visit(const RegexMatchExpression* expr) final {
        auto makeEExprFn = [expr](sbe::value::SlotId inputSlot) {
            auto regex = RegexMatchExpression::makeRegex(expr->getString(), expr->getFlags());
            auto ownedRegexVal = sbe::value::bitcastFrom(regex.release());

            // The "regexMatch" function returns Nothing when given any non-string input, so we need
            // an explicit string check in the expression in order to capture the MQL semantics of
            // regex returning false for non-strings. We generate the following expression:
            //
            //                    and
            //    +----------------+----------------+
            //  isString                       regexMatch
            //    |                    +------------+----------+
            //   var (inputSlot)   constant (regex)    var (inputSlot)
            //
            // TODO: In the future, this needs to account for the fact that the regex match
            // expression matches strings, but also matches stored regexes. For example,
            // {$match: {a: /foo/}} matches the document {a: /foo/} in addition to {a: "foobar"}.
            return sbe::makeE<sbe::EPrimBinary>(
                sbe::EPrimBinary::logicAnd,
                sbe::makeE<sbe::EFunction>("isString",
                                           sbe::makeEs(sbe::makeE<sbe::EVariable>(inputSlot))),
                sbe::makeE<sbe::EFunction>(
                    "regexMatch",
                    sbe::makeEs(
                        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::pcreRegex, ownedRegexVal),
                        sbe::makeE<sbe::EVariable>(inputSlot))));
        };

        generateTraverse(_context, expr, std::move(makeEExprFn));
    }

    void visit(const SizeMatchExpression* expr) final {}
    void visit(const TextMatchExpression* expr) final {}
    void visit(const TextNoOpMatchExpression* expr) final {}
    void visit(const TwoDPtInAnnulusExpression* expr) final {}
    void visit(const TypeMatchExpression* expr) final {}
    void visit(const WhereMatchExpression* expr) final {}
    void visit(const WhereNoOpMatchExpression* expr) final {}

private:
    MatchExpressionVisitorContext* _context;
};

/**
 * A match expression in-visitor used for maintaining the counter of the processed child expressions
 * of the nested logical expressions in the match expression tree being traversed.
 */
class MatchExpressionInVisitor final : public MatchExpressionConstVisitor {
public:
    MatchExpressionInVisitor(MatchExpressionVisitorContext* context) : _context(context) {}

    void visit(const AlwaysFalseMatchExpression* expr) final {}
    void visit(const AlwaysTrueMatchExpression* expr) final {}
    void visit(const AndMatchExpression* expr) final {
        invariant(_context->nestedLogicalExprs.top().first == expr);
        _context->nestedLogicalExprs.top().second--;
    }
    void visit(const BitsAllClearMatchExpression* expr) final {}
    void visit(const BitsAllSetMatchExpression* expr) final {}
    void visit(const BitsAnyClearMatchExpression* expr) final {}
    void visit(const BitsAnySetMatchExpression* expr) final {}
    void visit(const ElemMatchObjectMatchExpression* expr) final {}
    void visit(const ElemMatchValueMatchExpression* expr) final {}
    void visit(const EqualityMatchExpression* expr) final {}
    void visit(const ExistsMatchExpression* expr) final {}
    void visit(const ExprMatchExpression* expr) final {}
    void visit(const GTEMatchExpression* expr) final {}
    void visit(const GTMatchExpression* expr) final {}
    void visit(const GeoMatchExpression* expr) final {}
    void visit(const GeoNearMatchExpression* expr) final {}
    void visit(const InMatchExpression* expr) final {}
    void visit(const InternalExprEqMatchExpression* expr) final {}
    void visit(const InternalSchemaAllElemMatchFromIndexMatchExpression* expr) final {}
    void visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaBinDataEncryptedTypeExpression* expr) final {}
    void visit(const InternalSchemaBinDataSubTypeExpression* expr) final {}
    void visit(const InternalSchemaCondMatchExpression* expr) final {}
    void visit(const InternalSchemaEqMatchExpression* expr) final {}
    void visit(const InternalSchemaFmodMatchExpression* expr) final {}
    void visit(const InternalSchemaMatchArrayIndexMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxLengthMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaMinItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaMinLengthMatchExpression* expr) final {}
    void visit(const InternalSchemaMinPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaObjectMatchExpression* expr) final {}
    void visit(const InternalSchemaRootDocEqMatchExpression* expr) final {}
    void visit(const InternalSchemaTypeExpression* expr) final {}
    void visit(const InternalSchemaUniqueItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaXorMatchExpression* expr) final {}
    void visit(const LTEMatchExpression* expr) final {}
    void visit(const LTMatchExpression* expr) final {}
    void visit(const ModMatchExpression* expr) final {}
    void visit(const NorMatchExpression* expr) final {}
    void visit(const NotMatchExpression* expr) final {}
    void visit(const OrMatchExpression* expr) final {
        invariant(_context->nestedLogicalExprs.top().first == expr);
        _context->nestedLogicalExprs.top().second--;
    }
    void visit(const RegexMatchExpression* expr) final {}
    void visit(const SizeMatchExpression* expr) final {}
    void visit(const TextMatchExpression* expr) final {}
    void visit(const TextNoOpMatchExpression* expr) final {}
    void visit(const TwoDPtInAnnulusExpression* expr) final {}
    void visit(const TypeMatchExpression* expr) final {}
    void visit(const WhereMatchExpression* expr) final {}
    void visit(const WhereNoOpMatchExpression* expr) final {}

private:
    MatchExpressionVisitorContext* _context;
};
}  // namespace

std::unique_ptr<sbe::PlanStage> generateFilter(const MatchExpression* root,
                                               std::unique_ptr<sbe::PlanStage> stage,
                                               sbe::value::SlotIdGenerator* slotIdGenerator,
                                               sbe::value::SlotId inputVar) {
    // The planner adds an $and expression without the operands if the query was empty. We can bail
    // out early without generating the filter plan stage if this is the case.
    if (root->matchType() == MatchExpression::AND && root->numChildren() == 0) {
        return stage;
    }

    MatchExpressionVisitorContext context{slotIdGenerator, std::move(stage), inputVar};
    MatchExpressionPreVisitor preVisitor{&context};
    MatchExpressionInVisitor inVisitor{&context};
    MatchExpressionPostVisitor postVisitor{&context};
    MatchExpressionWalker walker{&preVisitor, &inVisitor, &postVisitor};
    tree_walker::walk<true, MatchExpression>(root, &walker);
    return context.done();
}
}  // namespace mongo::stage_builder
