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

#include "mongo/platform/basic.h"

#include "mongo/db/query/sbe_stage_builder_expression.h"
#include "mongo/db/query/util/make_data_structure.h"

#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/stages/branch.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/traverse.h"
#include "mongo/db/exec/sbe/stages/union.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/expression_visitor.h"
#include "mongo/db/pipeline/expression_walker.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/util/str.h"

namespace mongo::stage_builder {
namespace {
std::pair<sbe::value::TypeTags, sbe::value::Value> convertFrom(Value val) {
    // TODO: Either make this conversion unnecessary by changing the value representation in
    // ExpressionConstant, or provide a nicer way to convert directly from Document/Value to
    // sbe::Value.
    BSONObjBuilder bob;
    val.addToBsonObj(&bob, ""_sd);
    auto obj = bob.done();
    auto be = obj.objdata();
    auto end = be + sbe::value::readFromMemory<uint32_t>(be);
    return sbe::bson::convertFrom(false, be + 4, end, 0);
}

struct ExpressionVisitorContext {
    struct VarsFrame {
        std::deque<Variables::Id> variablesToBind;

        // Slots that have been used to bind $let variables. This list is necessary to know which
        // slots to remove from the environment when the $let goes out of scope.
        std::set<sbe::value::SlotId> slotsForLetVariables;

        template <class... Args>
        VarsFrame(Args&&... args)
            : variablesToBind{std::forward<Args>(args)...}, slotsForLetVariables{} {}
    };

    struct LogicalExpressionEvalFrame {
        std::unique_ptr<sbe::PlanStage> savedTraverseStage;
        sbe::value::SlotVector savedRelevantSlots;

        sbe::value::SlotId nextBranchResultSlot;

        std::vector<std::pair<sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>>> branches;

        // When traversing the branches of a $switch expression, the in-visitor will see each branch
        // of the $switch _twice_: once for the "case" part of the branch (the condition) and once
        // for the "then" part (the expression that the $switch will evaluate to if the condition
        // evaluates to true). During the first visit, we temporarily store the condition here so
        // that it is available to use during the second visit, which constructs the completed
        // EExpression for the branch and stores it in the 'branches' vector.
        boost::optional<std::pair<sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>>>
            switchBranchConditionalStage;

        LogicalExpressionEvalFrame(std::unique_ptr<sbe::PlanStage> traverseStage,
                                   const sbe::value::SlotVector& relevantSlots,
                                   sbe::value::SlotId nextBranchResultSlot)
            : savedTraverseStage(std::move(traverseStage)),
              savedRelevantSlots(relevantSlots),
              nextBranchResultSlot(nextBranchResultSlot) {}
    };

    struct FilterExpressionEvalFrame {
        std::unique_ptr<sbe::PlanStage> traverseStage;
        sbe::value::SlotVector relevantSlots;

        FilterExpressionEvalFrame(std::unique_ptr<sbe::PlanStage> traverseStage,
                                  const sbe::value::SlotVector& relevantSlots)
            : traverseStage(std::move(traverseStage)), relevantSlots(relevantSlots) {}
    };

    ExpressionVisitorContext(std::unique_ptr<sbe::PlanStage> inputStage,
                             sbe::value::SlotIdGenerator* slotIdGenerator,
                             sbe::value::FrameIdGenerator* frameIdGenerator,
                             sbe::value::SlotId rootSlot,
                             sbe::value::SlotVector* relevantSlots,
                             sbe::RuntimeEnvironment* env,
                             PlanNodeId planNodeId)
        : traverseStage(std::move(inputStage)),
          slotIdGenerator(slotIdGenerator),
          frameIdGenerator(frameIdGenerator),
          rootSlot(rootSlot),
          relevantSlots(relevantSlots),
          runtimeEnvironment(env),
          planNodeId(planNodeId) {}

    void ensureArity(size_t arity) {
        invariant(exprs.size() >= arity);
    }

    /**
     * Construct a UnionStage from the PlanStages in the 'branches' list and attach it to the inner
     * side of a LoopJoinStage, which iterates over each branch of the UnionStage until it finds one
     * that returns a result. Iteration ceases after the first branch that returns a result so that
     * the remaining branches are "short circuited" and we don't do unnecessary work for for MQL
     * expressions that are not evaluated.
     */
    void generateSubTreeForSelectiveExecution() {
        auto& logicalExpressionEvalFrame = logicalExpressionEvalFrameStack.top();

        std::vector<sbe::value::SlotVector> branchSlots;
        std::vector<std::unique_ptr<sbe::PlanStage>> branchStages;
        for (auto&& [slot, stage] : logicalExpressionEvalFrame.branches) {
            branchSlots.push_back(sbe::makeSV(slot));
            branchStages.push_back(std::move(stage));
        }

        auto unionStageResultSlot = slotIdGenerator->generate();
        auto unionOfBranches = sbe::makeS<sbe::UnionStage>(std::move(branchStages),
                                                           std::move(branchSlots),
                                                           sbe::makeSV(unionStageResultSlot),
                                                           planNodeId);

        // Restore 'relevantSlots' to the way it was before we started translating the logic
        // operator.
        *relevantSlots = std::move(logicalExpressionEvalFrame.savedRelevantSlots);

        // The LoopJoinStage we are creating here will not expose any of the slots from its outer
        // side except for the ones we explicity ask for. For that reason, we maintain the
        // 'relevantSlots' list of slots that may still be referenced above this stage. All of the
        // slots in 'letBindings' are relevant by this definition, but we track them separately,
        // which is why we need to add them in now.
        auto relevantSlotsWithLetBindings(*relevantSlots);
        for (auto&& [_, slot] : environment) {
            relevantSlotsWithLetBindings.push_back(slot);
        }

        // Put the union into a nested loop. The inner side of the nested loop will execute exactly
        // once, trying each branch of the union until one of them short circuits or until it
        // reaches the end. This process also restores the old 'traverseStage' value from before we
        // started translating the logic operator, by placing it below the new nested loop stage.
        auto stage = sbe::makeS<sbe::LoopJoinStage>(
            std::move(logicalExpressionEvalFrame.savedTraverseStage),
            sbe::makeS<sbe::LimitSkipStage>(std::move(unionOfBranches), 1, boost::none, planNodeId),
            relevantSlotsWithLetBindings,
            relevantSlotsWithLetBindings,
            nullptr /* predicate */,
            planNodeId);

        // We've already restored all necessary state from the top 'logicalExpressionEvalFrameStack'
        // entry, so we are done with it.
        logicalExpressionEvalFrameStack.pop();

        // The final result of the logic operator is stored in the 'branchResultSlot' slot.
        relevantSlots->push_back(unionStageResultSlot);
        pushExpr(sbe::makeE<sbe::EVariable>(unionStageResultSlot), std::move(stage));
    }

    std::unique_ptr<sbe::EExpression> popExpr() {
        auto expr = std::move(exprs.top());
        exprs.pop();
        return expr;
    }

    void pushExpr(std::unique_ptr<sbe::EExpression> expr) {
        exprs.push(std::move(expr));
    }

    void pushExpr(std::unique_ptr<sbe::EExpression> expr, std::unique_ptr<sbe::PlanStage> stage) {
        exprs.push(std::move(expr));
        traverseStage = std::move(stage);
    }

    /**
     * Temporarily reset 'traverseStage' and 'relevantSlots' so they are prepared for translating a
     * $and/$or branch. (They will be restored later using the saved values in the
     * 'logicalExpressionEvalFrameStack' top entry.) The new 'traverseStage' is actually a
     * projection that will evaluate to a constant false (for $and) or true (for $or). Once this
     * branch is fully contructed, it will have a filter stage that will either filter out the
     * constant (when branch evaluation does not short circuit) or produce the constant value
     * (therefore producing the short-circuit result). These branches are part of a union stage, so
     * each time a branch fails to produce a value, execution moves on to the next branch. A limit
     * stage above the union ensures that execution halts once one of the branches produces a
     * result.
     */
    void prepareToTranslateShortCircuitingBranch(sbe::EPrimBinary::Op logicOp,
                                                 sbe::value::SlotId branchResultSlot) {
        invariant(!logicalExpressionEvalFrameStack.empty());
        logicalExpressionEvalFrameStack.top().nextBranchResultSlot = branchResultSlot;

        auto shortCircuitVal = (logicOp == sbe::EPrimBinary::logicOr);
        auto shortCircuitExpr = sbe::makeE<sbe::EConstant>(
            sbe::value::TypeTags::Boolean, sbe::value::bitcastFrom<bool>(shortCircuitVal));
        traverseStage = sbe::makeProjectStage(
            sbe::makeS<sbe::LimitSkipStage>(
                sbe::makeS<sbe::CoScanStage>(planNodeId), 1, boost::none, planNodeId),
            planNodeId,
            branchResultSlot,
            std::move(shortCircuitExpr));

        // Slots created in a previous branch for this $and/$or are not accessible to any stages in
        // this new branch, so we clear them from the 'relevantSlots' list.
        *relevantSlots = logicalExpressionEvalFrameStack.top().savedRelevantSlots;

        // The 'branchResultSlot' is where the new branch will place its result in the event of a
        // short circuit, and it must be visible to the union stage after the branch executes.
        relevantSlots->push_back(branchResultSlot);
    }

    /**
     * Temporarily reset 'traverseStage' and 'relevantSlots' so they are prepared for translating a
     * $switch branch. They can be restored later using the 'logicalExpressionEvalFrameStack' top
     * entry. Once it is fully constructed, this branch will evaluate to the "then" part of the
     * branch if the condition is true or EOF otherwise. As with $and/$or branches (refer to the
     * comment describing 'prepareToTranslateShortCircuitingBranch()'), these branches will become
     * part of a UnionStage that executes the branches in turn until one yields a value.
     */
    void prepareToTranslateSwitchBranch(sbe::value::SlotId branchResultSlot) {
        invariant(!logicalExpressionEvalFrameStack.empty());
        logicalExpressionEvalFrameStack.top().nextBranchResultSlot = branchResultSlot;

        traverseStage = sbe::makeS<sbe::LimitSkipStage>(
            sbe::makeS<sbe::CoScanStage>(planNodeId), 1, boost::none, planNodeId);

        // Slots created in a previous branch for this $switch are not accessible to any stages in
        // this new branch, so we clear them from the 'relevantSlots' list.
        *relevantSlots = logicalExpressionEvalFrameStack.top().savedRelevantSlots;
    }

    /**
     * This does the same thing as 'prepareToTranslateShortCircuitingBranch' but is intended to the
     * last branch in an $and/$or, which cannot short circuit.
     */
    void prepareToTranslateConcludingLogicalBranch() {
        invariant(!logicalExpressionEvalFrameStack.empty());

        traverseStage = sbe::makeS<sbe::CoScanStage>(planNodeId);
        *relevantSlots = logicalExpressionEvalFrameStack.top().savedRelevantSlots;
    }

    std::tuple<sbe::value::SlotId,
               std::unique_ptr<sbe::EExpression>,
               std::unique_ptr<sbe::PlanStage>>
    done() {
        invariant(exprs.size() == 1);
        return {slotIdGenerator->generate(), popExpr(), std::move(traverseStage)};
    }

    std::unique_ptr<sbe::PlanStage> traverseStage;
    sbe::value::SlotIdGenerator* slotIdGenerator;
    sbe::value::FrameIdGenerator* frameIdGenerator;
    sbe::value::SlotId rootSlot;
    std::stack<std::unique_ptr<sbe::EExpression>> exprs;

    // The lexical environment for the expression being traversed. A variable reference takes the
    // form "$$variable_name" in MQL's concrete syntax and gets transformed into a numeric
    // identifier (Variables::Id) in the AST. During this translation, we directly translate any
    // such variable to an SBE slot using this mapping.
    std::map<Variables::Id, sbe::value::SlotId> environment;
    std::stack<VarsFrame> varsFrameStack;

    // TODO SERVER-51356: Replace these stacks with single stack of evaluation frames.
    std::stack<FilterExpressionEvalFrame> filterExpressionEvalFrameStack;
    std::stack<LogicalExpressionEvalFrame> logicalExpressionEvalFrameStack;

    // See the comment above the generateExpression() declaration for an explanation of the
    // 'relevantSlots' list.
    sbe::value::SlotVector* relevantSlots;
    sbe::RuntimeEnvironment* runtimeEnvironment;

    // The id of the QuerySolutionNode to which the expression we are converting to SBE is attached.
    const PlanNodeId planNodeId;
};

std::pair<sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>> generateTraverseHelper(
    std::unique_ptr<sbe::PlanStage> inputStage,
    sbe::value::SlotId inputSlot,
    const FieldPath& fp,
    size_t level,
    PlanNodeId planNodeId,
    sbe::value::SlotIdGenerator* slotIdGenerator) {
    using namespace std::literals;

    invariant(level < fp.getPathLength());

    // The field we will be traversing at the current nested level.
    auto fieldSlot{slotIdGenerator->generate()};
    // The result coming from the 'in' branch of the traverse plan stage.
    auto outputSlot{slotIdGenerator->generate()};

    // Generate the projection stage to read a sub-field at the current nested level and bind it
    // to 'fieldSlot'.
    inputStage = sbe::makeProjectStage(
        std::move(inputStage),
        planNodeId,
        fieldSlot,
        sbe::makeE<sbe::EFunction>(
            "getField"sv,
            sbe::makeEs(sbe::makeE<sbe::EVariable>(inputSlot), sbe::makeE<sbe::EConstant>([&]() {
                            auto fieldName = fp.getFieldName(level);
                            return std::string_view{fieldName.rawData(), fieldName.size()};
                        }()))));

    std::unique_ptr<sbe::PlanStage> innerBranch;
    if (level == fp.getPathLength() - 1) {
        innerBranch = sbe::makeProjectStage(
            sbe::makeS<sbe::LimitSkipStage>(
                sbe::makeS<sbe::CoScanStage>(planNodeId), 1, boost::none, planNodeId),
            planNodeId,
            outputSlot,
            sbe::makeE<sbe::EVariable>(fieldSlot));
    } else {
        // Generate nested traversal.
        auto [slot, stage] = generateTraverseHelper(
            sbe::makeS<sbe::LimitSkipStage>(
                sbe::makeS<sbe::CoScanStage>(planNodeId), 1, boost::none, planNodeId),
            fieldSlot,
            fp,
            level + 1,
            planNodeId,
            slotIdGenerator);
        innerBranch = sbe::makeProjectStage(
            std::move(stage), planNodeId, outputSlot, sbe::makeE<sbe::EVariable>(slot));
    }

    // The final traverse stage for the current nested level.
    return {outputSlot,
            sbe::makeS<sbe::TraverseStage>(std::move(inputStage),
                                           std::move(innerBranch),
                                           fieldSlot,
                                           outputSlot,
                                           outputSlot,
                                           sbe::makeSV(),
                                           nullptr,
                                           nullptr,
                                           planNodeId,
                                           1)};
}

/**
 * For the given MatchExpression 'expr', generates a path traversal SBE plan stage sub-tree
 * implementing the comparison expression.
 */
std::pair<sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>> generateTraverse(
    std::unique_ptr<sbe::PlanStage> inputStage,
    sbe::value::SlotId inputSlot,
    bool expectsDocumentInputOnly,
    const FieldPath& fp,
    PlanNodeId planNodeId,
    sbe::value::SlotIdGenerator* slotIdGenerator) {
    if (expectsDocumentInputOnly) {
        // When we know for sure that 'inputSlot' will be a document and _not_ an array (such as
        // when traversing the root document), we can generate a simpler expression.
        return generateTraverseHelper(
            std::move(inputStage), inputSlot, fp, 0, planNodeId, slotIdGenerator);
    } else {
        // The general case: the value in the 'inputSlot' may be an array that will require
        // traversal.
        auto outputSlot{slotIdGenerator->generate()};
        auto [innerBranchOutputSlot, innerBranch] = generateTraverseHelper(
            sbe::makeS<sbe::LimitSkipStage>(
                sbe::makeS<sbe::CoScanStage>(planNodeId), 1, boost::none, planNodeId),
            inputSlot,
            fp,
            0,  // level
            planNodeId,
            slotIdGenerator);
        return {outputSlot,
                sbe::makeS<sbe::TraverseStage>(std::move(inputStage),
                                               std::move(innerBranch),
                                               inputSlot,
                                               outputSlot,
                                               innerBranchOutputSlot,
                                               sbe::makeSV(),
                                               nullptr,
                                               nullptr,
                                               planNodeId,
                                               1)};
    }
}

/**
 * Generates an EExpression that converts the input to upper or lower case.
 */
void generateStringCaseConversionExpression(ExpressionVisitorContext* _context,
                                            const std::string& caseConversionFunction) {
    auto frameId = _context->frameIdGenerator->generate();
    auto str = sbe::makeEs(_context->popExpr());
    sbe::EVariable inputRef(frameId, 0);
    uint32_t typeMask = (getBSONTypeMask(sbe::value::TypeTags::StringSmall) |
                         getBSONTypeMask(sbe::value::TypeTags::StringBig) |
                         getBSONTypeMask(sbe::value::TypeTags::bsonString) |
                         getBSONTypeMask(sbe::value::TypeTags::NumberInt32) |
                         getBSONTypeMask(sbe::value::TypeTags::NumberInt64) |
                         getBSONTypeMask(sbe::value::TypeTags::NumberDouble) |
                         getBSONTypeMask(sbe::value::TypeTags::NumberDecimal) |
                         getBSONTypeMask(sbe::value::TypeTags::Date) |
                         getBSONTypeMask(sbe::value::TypeTags::Timestamp));
    auto checkValidTypeExpr = sbe::makeE<sbe::ETypeMatch>(inputRef.clone(), typeMask);
    auto checkNullorMissing = generateNullOrMissing(inputRef);
    auto [emptyStrTag, emptyStrVal] = sbe::value::makeNewString("");

    auto caseConversionExpr = sbe::makeE<sbe::EIf>(
        std::move(checkValidTypeExpr),
        sbe::makeE<sbe::EFunction>(caseConversionFunction,
                                   sbe::makeEs(sbe::makeE<sbe::EFunction>(
                                       "coerceToString", sbe::makeEs(inputRef.clone())))),
        sbe::makeE<sbe::EFail>(ErrorCodes::Error{5066300},
                               str::stream() << "$" << caseConversionFunction
                                             << " input type is not supported"));

    auto totalCaseConversionExpr =
        sbe::makeE<sbe::EIf>(std::move(checkNullorMissing),
                             sbe::makeE<sbe::EConstant>(emptyStrTag, emptyStrVal),
                             std::move(caseConversionExpr));
    _context->pushExpr(
        sbe::makeE<sbe::ELocalBind>(frameId, std::move(str), std::move(totalCaseConversionExpr)));
}

/**
 * Generates an EExpression that checks if the input expression is not a string, _assuming that
 * it has already been verified to be neither null nor missing.
 */
std::unique_ptr<sbe::EExpression> generateNonStringCheck(const sbe::EVariable& var) {
    return sbe::makeE<sbe::EPrimUnary>(
        sbe::EPrimUnary::logicNot,
        sbe::makeE<sbe::EFunction>("isString", sbe::makeEs(var.clone())));
}

/**
 * Generates an EExpression that checks whether the input expression is null, missing, or
 * unable to be converted to the type NumberInt32.
 */
std::unique_ptr<sbe::EExpression> generateNullishOrNotRepresentableInt32Check(
    const sbe::EVariable& var) {
    auto numericConvert32 =
        sbe::makeE<sbe::ENumericConvert>(var.clone(), sbe::value::TypeTags::NumberInt32);
    return sbe::makeE<sbe::EPrimBinary>(
        sbe::EPrimBinary::logicOr,
        generateNullOrMissing(var),
        sbe::makeE<sbe::EPrimUnary>(
            sbe::EPrimUnary::logicNot,
            sbe::makeE<sbe::EFunction>("exists", sbe::makeEs(std::move(numericConvert32)))));
}

std::unique_ptr<sbe::EExpression> makeNot(std::unique_ptr<sbe::EExpression> e) {
    return sbe::makeE<sbe::EPrimUnary>(sbe::EPrimUnary::logicNot, std::move(e));
}

void buildArrayAccessByConstantIndex(ExpressionVisitorContext* context,
                                     const std::string& exprName,
                                     int32_t index) {
    context->ensureArity(1);

    auto array = context->popExpr();

    auto frameId = context->frameIdGenerator->generate();
    auto binds = sbe::makeEs(std::move(array));
    sbe::EVariable arrayRef{frameId, 0};

    auto indexExpr = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                sbe::value::bitcastFrom<int32_t>(index));
    auto argumentIsNotArray =
        makeNot(sbe::makeE<sbe::EFunction>("isArray", sbe::makeEs(arrayRef.clone())));
    auto resultExpr = buildMultiBranchConditional(
        CaseValuePair{generateNullOrMissing(arrayRef),
                      sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
        CaseValuePair{std::move(argumentIsNotArray),
                      sbe::makeE<sbe::EFail>(ErrorCodes::Error{5126704},
                                             exprName + " argument must be an array")},
        sbe::makeE<sbe::EFunction>("getElement",
                                   sbe::makeEs(arrayRef.clone(), std::move(indexExpr))));

    context->pushExpr(
        sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(resultExpr)));
}

class ExpressionPreVisitor final : public ExpressionVisitor {
public:
    ExpressionPreVisitor(ExpressionVisitorContext* context) : _context{context} {}

    void visit(ExpressionConstant* expr) final {}
    void visit(ExpressionAbs* expr) final {}
    void visit(ExpressionAdd* expr) final {}
    void visit(ExpressionAllElementsTrue* expr) final {}
    void visit(ExpressionAnd* expr) final {
        visitMultiBranchLogicExpression(expr, sbe::EPrimBinary::logicAnd);
    }
    void visit(ExpressionAnyElementTrue* expr) final {}
    void visit(ExpressionArray* expr) final {}
    void visit(ExpressionArrayElemAt* expr) final {}
    void visit(ExpressionFirst* expr) final {}
    void visit(ExpressionLast* expr) final {}
    void visit(ExpressionObjectToArray* expr) final {}
    void visit(ExpressionArrayToObject* expr) final {}
    void visit(ExpressionBsonSize* expr) final {}
    void visit(ExpressionCeil* expr) final {}
    void visit(ExpressionCoerceToBool* expr) final {}
    void visit(ExpressionCompare* expr) final {}
    void visit(ExpressionConcat* expr) final {}
    void visit(ExpressionConcatArrays* expr) final {}
    void visit(ExpressionCond* expr) final {
        visitConditionalExpression(expr);
    }
    void visit(ExpressionDateFromString* expr) final {}
    void visit(ExpressionDateFromParts* expr) final {}
    void visit(ExpressionDateToParts* expr) final {}
    void visit(ExpressionDateToString* expr) final {}
    void visit(ExpressionDivide* expr) final {}
    void visit(ExpressionExp* expr) final {}
    void visit(ExpressionFieldPath* expr) final {}
    void visit(ExpressionFilter* expr) final {}
    void visit(ExpressionFloor* expr) final {}
    void visit(ExpressionIfNull* expr) final {}
    void visit(ExpressionIn* expr) final {}
    void visit(ExpressionIndexOfArray* expr) final {}
    void visit(ExpressionIndexOfBytes* expr) final {}
    void visit(ExpressionIndexOfCP* expr) final {}
    void visit(ExpressionIsNumber* expr) final {}
    void visit(ExpressionLet* expr) final {
        _context->varsFrameStack.push(ExpressionVisitorContext::VarsFrame{
            std::begin(expr->getOrderedVariableIds()), std::end(expr->getOrderedVariableIds())});
    }
    void visit(ExpressionLn* expr) final {}
    void visit(ExpressionLog* expr) final {}
    void visit(ExpressionLog10* expr) final {}
    void visit(ExpressionMap* expr) final {}
    void visit(ExpressionMeta* expr) final {}
    void visit(ExpressionMod* expr) final {}
    void visit(ExpressionMultiply* expr) final {}
    void visit(ExpressionNot* expr) final {}
    void visit(ExpressionObject* expr) final {}
    void visit(ExpressionOr* expr) final {
        visitMultiBranchLogicExpression(expr, sbe::EPrimBinary::logicOr);
    }
    void visit(ExpressionPow* expr) final {}
    void visit(ExpressionRange* expr) final {}
    void visit(ExpressionReduce* expr) final {}
    void visit(ExpressionReplaceOne* expr) final {}
    void visit(ExpressionReplaceAll* expr) final {}
    void visit(ExpressionSetDifference* expr) final {}
    void visit(ExpressionSetEquals* expr) final {}
    void visit(ExpressionSetIntersection* expr) final {}
    void visit(ExpressionSetIsSubset* expr) final {}
    void visit(ExpressionSetUnion* expr) final {}
    void visit(ExpressionSize* expr) final {}
    void visit(ExpressionReverseArray* expr) final {}
    void visit(ExpressionSlice* expr) final {}
    void visit(ExpressionIsArray* expr) final {}
    void visit(ExpressionRound* expr) final {}
    void visit(ExpressionSplit* expr) final {}
    void visit(ExpressionSqrt* expr) final {}
    void visit(ExpressionStrcasecmp* expr) final {}
    void visit(ExpressionSubstrBytes* expr) final {}
    void visit(ExpressionSubstrCP* expr) final {}
    void visit(ExpressionStrLenBytes* expr) final {}
    void visit(ExpressionBinarySize* expr) final {}
    void visit(ExpressionStrLenCP* expr) final {}
    void visit(ExpressionSubtract* expr) final {}
    void visit(ExpressionSwitch* expr) final {
        visitConditionalExpression(expr);
    }
    void visit(ExpressionToLower* expr) final {}
    void visit(ExpressionToUpper* expr) final {}
    void visit(ExpressionTrim* expr) final {}
    void visit(ExpressionTrunc* expr) final {}
    void visit(ExpressionType* expr) final {}
    void visit(ExpressionZip* expr) final {}
    void visit(ExpressionConvert* expr) final {}
    void visit(ExpressionRegexFind* expr) final {}
    void visit(ExpressionRegexFindAll* expr) final {}
    void visit(ExpressionRegexMatch* expr) final {}
    void visit(ExpressionCosine* expr) final {}
    void visit(ExpressionSine* expr) final {}
    void visit(ExpressionTangent* expr) final {}
    void visit(ExpressionArcCosine* expr) final {}
    void visit(ExpressionArcSine* expr) final {}
    void visit(ExpressionArcTangent* expr) final {}
    void visit(ExpressionArcTangent2* expr) final {}
    void visit(ExpressionHyperbolicArcTangent* expr) final {}
    void visit(ExpressionHyperbolicArcCosine* expr) final {}
    void visit(ExpressionHyperbolicArcSine* expr) final {}
    void visit(ExpressionHyperbolicTangent* expr) final {}
    void visit(ExpressionHyperbolicCosine* expr) final {}
    void visit(ExpressionHyperbolicSine* expr) final {}
    void visit(ExpressionDegreesToRadians* expr) final {}
    void visit(ExpressionRadiansToDegrees* expr) final {}
    void visit(ExpressionDayOfMonth* expr) final {}
    void visit(ExpressionDayOfWeek* expr) final {}
    void visit(ExpressionDayOfYear* expr) final {}
    void visit(ExpressionHour* expr) final {}
    void visit(ExpressionMillisecond* expr) final {}
    void visit(ExpressionMinute* expr) final {}
    void visit(ExpressionMonth* expr) final {}
    void visit(ExpressionSecond* expr) final {}
    void visit(ExpressionWeek* expr) final {}
    void visit(ExpressionIsoWeekYear* expr) final {}
    void visit(ExpressionIsoDayOfWeek* expr) final {}
    void visit(ExpressionIsoWeek* expr) final {}
    void visit(ExpressionYear* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorAvg>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorMax>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorMin>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorStdDevPop>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorStdDevSamp>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorSum>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorMergeObjects>* expr) final {}
    void visit(ExpressionTests::Testable* expr) final {}
    void visit(ExpressionInternalJsEmit* expr) final {}
    void visit(ExpressionInternalFindSlice* expr) final {}
    void visit(ExpressionInternalFindPositional* expr) final {}
    void visit(ExpressionInternalFindElemMatch* expr) final {}
    void visit(ExpressionFunction* expr) final {}
    void visit(ExpressionRandom* expr) final {}
    void visit(ExpressionToHashedIndexKey* expr) final {}

private:
    void visitMultiBranchLogicExpression(Expression* expr, sbe::EPrimBinary::Op logicOp) {
        invariant(logicOp == sbe::EPrimBinary::logicOr || logicOp == sbe::EPrimBinary::logicAnd);

        if (expr->getChildren().size() < 2) {
            // All this bookkeeping is only necessary for short circuiting, so we can skip it if we
            // don't have two or more branches.
            return;
        }

        auto branchResultSlot = _context->slotIdGenerator->generate();
        _context->logicalExpressionEvalFrameStack.emplace(
            std::move(_context->traverseStage), *_context->relevantSlots, branchResultSlot);

        _context->prepareToTranslateShortCircuitingBranch(logicOp, branchResultSlot);
    }

    /**
     * Handle $switch and $cond, which have different syntax but are structurally identical in the
     * AST.
     */
    void visitConditionalExpression(Expression* expr) {
        auto branchResultSlot = _context->slotIdGenerator->generate();
        _context->logicalExpressionEvalFrameStack.emplace(
            std::move(_context->traverseStage), *_context->relevantSlots, branchResultSlot);

        _context->prepareToTranslateSwitchBranch(branchResultSlot);
    }

    ExpressionVisitorContext* _context;
};

class ExpressionInVisitor final : public ExpressionVisitor {
public:
    ExpressionInVisitor(ExpressionVisitorContext* context) : _context{context} {}

    void visit(ExpressionConstant* expr) final {}
    void visit(ExpressionAbs* expr) final {}
    void visit(ExpressionAdd* expr) final {}
    void visit(ExpressionAllElementsTrue* expr) final {}
    void visit(ExpressionAnd* expr) final {
        visitMultiBranchLogicExpression(expr, sbe::EPrimBinary::logicAnd);
    }
    void visit(ExpressionAnyElementTrue* expr) final {}
    void visit(ExpressionArray* expr) final {}
    void visit(ExpressionArrayElemAt* expr) final {}
    void visit(ExpressionFirst* expr) final {}
    void visit(ExpressionLast* expr) final {}
    void visit(ExpressionObjectToArray* expr) final {}
    void visit(ExpressionArrayToObject* expr) final {}
    void visit(ExpressionBsonSize* expr) final {}
    void visit(ExpressionCeil* expr) final {}
    void visit(ExpressionCoerceToBool* expr) final {}
    void visit(ExpressionCompare* expr) final {}
    void visit(ExpressionConcat* expr) final {}
    void visit(ExpressionConcatArrays* expr) final {}
    void visit(ExpressionCond* expr) final {
        visitConditionalExpression(expr);
    }
    void visit(ExpressionDateFromString* expr) final {}
    void visit(ExpressionDateFromParts* expr) final {}
    void visit(ExpressionDateToParts* expr) final {}
    void visit(ExpressionDateToString* expr) final {}
    void visit(ExpressionDivide* expr) final {}
    void visit(ExpressionExp* expr) final {}
    void visit(ExpressionFieldPath* expr) final {}
    void visit(ExpressionFilter* expr) final {
        // This visitor executes after visiting the expression that will evaluate to the array for
        // filtering and before visiting the filter condition expression.
        auto variableId = expr->getVariableId();
        invariant(_context->environment.find(variableId) == _context->environment.end());

        auto currentElementSlot = _context->slotIdGenerator->generate();
        _context->environment.insert({variableId, currentElementSlot});

        // Temporarily reset 'traverseStage' with limit 1/coscan tree to prevent from being
        // rewritten by filter predicate's generated sub-tree.
        _context->filterExpressionEvalFrameStack.emplace(std::move(_context->traverseStage),
                                                         *_context->relevantSlots);
        _context->traverseStage = makeLimitCoScanTree(_context->planNodeId);
    }
    void visit(ExpressionFloor* expr) final {}
    void visit(ExpressionIfNull* expr) final {}
    void visit(ExpressionIn* expr) final {}
    void visit(ExpressionIndexOfArray* expr) final {}
    void visit(ExpressionIndexOfBytes* expr) final {}
    void visit(ExpressionIndexOfCP* expr) final {}
    void visit(ExpressionIsNumber* expr) final {}
    void visit(ExpressionLet* expr) final {
        // This visitor fires after each variable definition in a $let expression. The top of the
        // _context's expression stack will be an expression defining the variable initializer. We
        // use a separate frame stack ('varsFrameStack') to keep track of which variable we are
        // visiting, so we can appropriately bind the initializer.
        invariant(!_context->varsFrameStack.empty());
        auto& currentFrame = _context->varsFrameStack.top();

        invariant(!currentFrame.variablesToBind.empty());

        auto varToBind = currentFrame.variablesToBind.front();
        currentFrame.variablesToBind.pop_front();

        // We create two bindings. First, the initializer result is bound to a slot when this
        // ProjectStage executes.
        auto slotToBind = _context->slotIdGenerator->generate();
        _context->traverseStage = sbe::makeProjectStage(std::move(_context->traverseStage),
                                                        _context->planNodeId,
                                                        slotToBind,
                                                        _context->popExpr());
        currentFrame.slotsForLetVariables.insert(slotToBind);

        // Second, we bind this variables AST-level name (with type Variable::Id) to the SlotId that
        // will be used for compilation and execution. Once this "stage builder" finishes, these
        // Variable::Id bindings will no longer be relevant.
        invariant(_context->environment.find(varToBind) == _context->environment.end());
        _context->environment.insert({varToBind, slotToBind});
    }
    void visit(ExpressionLn* expr) final {}
    void visit(ExpressionLog* expr) final {}
    void visit(ExpressionLog10* expr) final {}
    void visit(ExpressionMap* expr) final {}
    void visit(ExpressionMeta* expr) final {}
    void visit(ExpressionMod* expr) final {}
    void visit(ExpressionMultiply* expr) final {}
    void visit(ExpressionNot* expr) final {}
    void visit(ExpressionObject* expr) final {}
    void visit(ExpressionOr* expr) final {
        visitMultiBranchLogicExpression(expr, sbe::EPrimBinary::logicOr);
    }
    void visit(ExpressionPow* expr) final {}
    void visit(ExpressionRange* expr) final {}
    void visit(ExpressionReduce* expr) final {}
    void visit(ExpressionReplaceOne* expr) final {}
    void visit(ExpressionReplaceAll* expr) final {}
    void visit(ExpressionSetDifference* expr) final {}
    void visit(ExpressionSetEquals* expr) final {}
    void visit(ExpressionSetIntersection* expr) final {}
    void visit(ExpressionSetIsSubset* expr) final {}
    void visit(ExpressionSetUnion* expr) final {}
    void visit(ExpressionSize* expr) final {}
    void visit(ExpressionReverseArray* expr) final {}
    void visit(ExpressionSlice* expr) final {}
    void visit(ExpressionIsArray* expr) final {}
    void visit(ExpressionRound* expr) final {}
    void visit(ExpressionSplit* expr) final {}
    void visit(ExpressionSqrt* expr) final {}
    void visit(ExpressionStrcasecmp* expr) final {}
    void visit(ExpressionSubstrBytes* expr) final {}
    void visit(ExpressionSubstrCP* expr) final {}
    void visit(ExpressionStrLenBytes* expr) final {}
    void visit(ExpressionBinarySize* expr) final {}
    void visit(ExpressionStrLenCP* expr) final {}
    void visit(ExpressionSubtract* expr) final {}
    void visit(ExpressionSwitch* expr) final {
        visitConditionalExpression(expr);
    }
    void visit(ExpressionToLower* expr) final {}
    void visit(ExpressionToUpper* expr) final {}
    void visit(ExpressionTrim* expr) final {}
    void visit(ExpressionTrunc* expr) final {}
    void visit(ExpressionType* expr) final {}
    void visit(ExpressionZip* expr) final {}
    void visit(ExpressionConvert* expr) final {}
    void visit(ExpressionRegexFind* expr) final {}
    void visit(ExpressionRegexFindAll* expr) final {}
    void visit(ExpressionRegexMatch* expr) final {}
    void visit(ExpressionCosine* expr) final {}
    void visit(ExpressionSine* expr) final {}
    void visit(ExpressionTangent* expr) final {}
    void visit(ExpressionArcCosine* expr) final {}
    void visit(ExpressionArcSine* expr) final {}
    void visit(ExpressionArcTangent* expr) final {}
    void visit(ExpressionArcTangent2* expr) final {}
    void visit(ExpressionHyperbolicArcTangent* expr) final {}
    void visit(ExpressionHyperbolicArcCosine* expr) final {}
    void visit(ExpressionHyperbolicArcSine* expr) final {}
    void visit(ExpressionHyperbolicTangent* expr) final {}
    void visit(ExpressionHyperbolicCosine* expr) final {}
    void visit(ExpressionHyperbolicSine* expr) final {}
    void visit(ExpressionDegreesToRadians* expr) final {}
    void visit(ExpressionRadiansToDegrees* expr) final {}
    void visit(ExpressionDayOfMonth* expr) final {}
    void visit(ExpressionDayOfWeek* expr) final {}
    void visit(ExpressionDayOfYear* expr) final {}
    void visit(ExpressionHour* expr) final {}
    void visit(ExpressionMillisecond* expr) final {}
    void visit(ExpressionMinute* expr) final {}
    void visit(ExpressionMonth* expr) final {}
    void visit(ExpressionSecond* expr) final {}
    void visit(ExpressionWeek* expr) final {}
    void visit(ExpressionIsoWeekYear* expr) final {}
    void visit(ExpressionIsoDayOfWeek* expr) final {}
    void visit(ExpressionIsoWeek* expr) final {}
    void visit(ExpressionYear* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorAvg>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorMax>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorMin>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorStdDevPop>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorStdDevSamp>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorSum>* expr) final {}
    void visit(ExpressionFromAccumulator<AccumulatorMergeObjects>* expr) final {}
    void visit(ExpressionTests::Testable* expr) final {}
    void visit(ExpressionInternalJsEmit* expr) final {}
    void visit(ExpressionInternalFindSlice* expr) final {}
    void visit(ExpressionInternalFindPositional* expr) final {}
    void visit(ExpressionInternalFindElemMatch* expr) final {}
    void visit(ExpressionFunction* expr) final {}
    void visit(ExpressionRandom* expr) final {}
    void visit(ExpressionToHashedIndexKey* expr) final {}

private:
    void visitMultiBranchLogicExpression(Expression* expr, sbe::EPrimBinary::Op logicOp) {
        // The infix visitor should only visit expressions with more than one child.
        invariant(expr->getChildren().size() >= 2);
        invariant(logicOp == sbe::EPrimBinary::logicOr || logicOp == sbe::EPrimBinary::logicAnd);

        auto frameId = _context->frameIdGenerator->generate();
        auto branchExpr = generateCoerceToBoolExpression(sbe::EVariable{frameId, 0});
        std::unique_ptr<sbe::EExpression> shortCircuitCondition;
        if (logicOp == sbe::EPrimBinary::logicAnd) {
            // The filter should take the short circuit path when the branch resolves to _false_, so
            // we invert the filter condition.
            shortCircuitCondition = sbe::makeE<sbe::ELocalBind>(
                frameId,
                sbe::makeEs(_context->popExpr()),
                sbe::makeE<sbe::EPrimUnary>(sbe::EPrimUnary::logicNot, std::move(branchExpr)));
        } else {
            // For $or, keep the filter condition as is; the filter will take the short circuit path
            // when the branch resolves to true.
            shortCircuitCondition = sbe::makeE<sbe::ELocalBind>(
                frameId, sbe::makeEs(_context->popExpr()), std::move(branchExpr));
        }

        auto branchStage = sbe::makeS<sbe::FilterStage<false>>(std::move(_context->traverseStage),
                                                               std::move(shortCircuitCondition),
                                                               _context->planNodeId);

        auto& currentFrameStack = _context->logicalExpressionEvalFrameStack.top();
        currentFrameStack.branches.emplace_back(
            std::make_pair(currentFrameStack.nextBranchResultSlot, std::move(branchStage)));

        if (currentFrameStack.branches.size() < (expr->getChildren().size() - 1)) {
            _context->prepareToTranslateShortCircuitingBranch(
                logicOp, _context->slotIdGenerator->generate());
        } else {
            // We have already translated all but one of the branches, meaning the next branch we
            // translate will be the final one and does not need an short-circuit logic.
            _context->prepareToTranslateConcludingLogicalBranch();
        }
    }

    /**
     * Handle $switch and $cond, which have different syntax but are structurally identical in the
     * AST.
     */
    void visitConditionalExpression(Expression* expr) {
        invariant(_context->logicalExpressionEvalFrameStack.size() > 0);

        auto& logicalExpressionEvalFrame = _context->logicalExpressionEvalFrameStack.top();
        auto& switchBranchConditionalStage =
            logicalExpressionEvalFrame.switchBranchConditionalStage;

        if (switchBranchConditionalStage == boost::none) {
            // Here, _context->popExpr() represents the $switch branch's "case" child.
            auto frameId = _context->frameIdGenerator->generate();
            auto branchExpr = generateCoerceToBoolExpression(sbe::EVariable{frameId, 0});
            auto conditionExpr = sbe::makeE<sbe::ELocalBind>(
                frameId, sbe::makeEs(_context->popExpr()), std::move(branchExpr));

            auto conditionalEvalStage =
                sbe::makeProjectStage(std::move(_context->traverseStage),
                                      _context->planNodeId,
                                      logicalExpressionEvalFrame.nextBranchResultSlot,
                                      std::move(conditionExpr));

            // Store this case eval stage for use when visiting the $switch branch's "then" child.
            switchBranchConditionalStage.emplace(logicalExpressionEvalFrame.nextBranchResultSlot,
                                                 std::move(conditionalEvalStage));
        } else {
            // Here, _context->popExpr() represents the $switch branch's "then" child.

            // Get the "case" child to form the outer part of the Loop Join.
            auto [conditionalEvalStageSlot, conditionalEvalStage] =
                std::move(*switchBranchConditionalStage);
            switchBranchConditionalStage = boost::none;

            // Create the "then" child (a BranchStage) to form the inner nlj stage.
            auto branchStageResultSlot = logicalExpressionEvalFrame.nextBranchResultSlot;
            auto thenStageResultSlot = _context->slotIdGenerator->generate();
            auto unusedElseStageResultSlot = _context->slotIdGenerator->generate();

            // Construct a BranchStage tree that will bind the evaluated "then" expression if the
            // "case" expression evalautes to true and will EOF otherwise.
            auto branchStage = sbe::makeS<sbe::BranchStage>(
                sbe::makeProjectStage(std::move(_context->traverseStage),
                                      _context->planNodeId,
                                      thenStageResultSlot,
                                      _context->popExpr()),
                sbe::makeS<sbe::LimitSkipStage>(
                    sbe::makeProjectStage(
                        sbe::makeS<sbe::CoScanStage>(_context->planNodeId),
                        _context->planNodeId,
                        unusedElseStageResultSlot,
                        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Nothing, 0)),
                    0,
                    boost::none,
                    _context->planNodeId),
                sbe::makeE<sbe::EVariable>(conditionalEvalStageSlot),
                sbe::makeSV(thenStageResultSlot),
                sbe::makeSV(unusedElseStageResultSlot),
                sbe::makeSV(branchStageResultSlot),
                _context->planNodeId);

            // Get a list of slots that are used by $let expressions. These slots need to be
            // available to the inner side of the LoopJoinStage, in case any of the branches want to
            // reference one of the variables bound by the $let.
            sbe::value::SlotVector outerCorrelated;
            for (auto&& [_, slot] : _context->environment) {
                outerCorrelated.push_back(slot);
            }

            // The true/false result of the condition, which is evaluated in the outer side of the
            // LoopJoinStage, must be available to the inner side.
            outerCorrelated.push_back(conditionalEvalStageSlot);

            // Create a LoopJoinStage that will evaluate its outer child exactly once, to compute
            // the true/false result of the branch condition, and then execute its inner child
            // with the result of that condition bound to a correlated slot.
            auto loopJoinStage = sbe::makeS<sbe::LoopJoinStage>(std::move(conditionalEvalStage),
                                                                std::move(branchStage),
                                                                outerCorrelated,
                                                                outerCorrelated,
                                                                nullptr /* predicate */,
                                                                _context->planNodeId);

            logicalExpressionEvalFrame.branches.emplace_back(std::make_pair(
                logicalExpressionEvalFrame.nextBranchResultSlot, std::move(loopJoinStage)));
        }

        _context->prepareToTranslateSwitchBranch(_context->slotIdGenerator->generate());
    }

    ExpressionVisitorContext* _context;
};


struct DoubleBound {
    DoubleBound(double b, bool isInclusive) : bound(b), inclusive(isInclusive) {}

    static DoubleBound minInfinity() {
        return DoubleBound(-std::numeric_limits<double>::infinity(), false);
    }
    static DoubleBound plusInfinity() {
        return DoubleBound(std::numeric_limits<double>::infinity(), false);
    }
    std::string printLowerBound() const {
        return str::stream() << (inclusive ? "[" : "(") << bound;
    }
    std::string printUpperBound() const {
        return str::stream() << bound << (inclusive ? "]" : ")");
    }
    double bound;
    bool inclusive;
};

class ExpressionPostVisitor final : public ExpressionVisitor {
public:
    ExpressionPostVisitor(ExpressionVisitorContext* context) : _context{context} {}

    void visit(ExpressionConstant* expr) final {
        auto [tag, val] = convertFrom(expr->getValue());
        _context->pushExpr(sbe::makeE<sbe::EConstant>(tag, val));
    }

    void visit(ExpressionAbs* expr) final {
        auto frameId = _context->frameIdGenerator->generate();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        auto absExpr = buildMultiBranchConditional(
            CaseValuePair{generateNullOrMissing(inputRef),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{generateNonNumericCheck(inputRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4903700},
                                                 "$abs only supports numeric types")},
            CaseValuePair{generateLongLongMinCheck(inputRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4903701},
                                                 "can't take $abs of long long min")},
            sbe::makeE<sbe::EFunction>("abs", sbe::makeEs(inputRef.clone())));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(absExpr)));
    }

    void visit(ExpressionAdd* expr) final {
        size_t arity = expr->getChildren().size();
        _context->ensureArity(arity);
        auto frameId = _context->frameIdGenerator->generate();

        auto generateNotNumberOrDate = [frameId](const sbe::value::SlotId slotId) {
            sbe::EVariable var{frameId, slotId};
            return sbe::makeE<sbe::EPrimBinary>(
                sbe::EPrimBinary::logicAnd,
                sbe::makeE<sbe::EPrimUnary>(
                    sbe::EPrimUnary::logicNot,
                    sbe::makeE<sbe::EFunction>("isNumber", sbe::makeEs(var.clone()))),
                sbe::makeE<sbe::EPrimUnary>(
                    sbe::EPrimUnary::logicNot,
                    sbe::makeE<sbe::EFunction>("isDate", sbe::makeEs(var.clone()))));
        };

        if (arity == 2) {
            auto rhs = _context->popExpr();
            auto lhs = _context->popExpr();
            auto binds = sbe::makeEs(std::move(lhs), std::move(rhs));
            sbe::EVariable lhsVar{frameId, 0};
            sbe::EVariable rhsVar{frameId, 1};

            auto addExpr = sbe::makeE<sbe::EIf>(
                sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::logicOr,
                                             generateNullOrMissing(frameId, 0),
                                             generateNullOrMissing(frameId, 1)),
                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0),
                sbe::makeE<sbe::EIf>(
                    sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::logicOr,
                                                 generateNotNumberOrDate(0),
                                                 generateNotNumberOrDate(1)),
                    sbe::makeE<sbe::EFail>(
                        ErrorCodes::Error{4974201},
                        "only numbers and dates are allowed in an $add expression"),
                    sbe::makeE<sbe::EIf>(
                        sbe::makeE<sbe::EPrimBinary>(
                            sbe::EPrimBinary::logicAnd,
                            sbe::makeE<sbe::EFunction>("isDate", sbe::makeEs(lhsVar.clone())),
                            sbe::makeE<sbe::EFunction>("isDate", sbe::makeEs(rhsVar.clone()))),
                        sbe::makeE<sbe::EFail>(ErrorCodes::Error{4974202},
                                               "only one date allowed in an $add expression"),
                        sbe::makeE<sbe::EPrimBinary>(
                            sbe::EPrimBinary::add, lhsVar.clone(), rhsVar.clone()))));

            _context->pushExpr(
                sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(addExpr)));
        } else {
            std::vector<std::unique_ptr<sbe::EExpression>> binds;
            std::vector<std::unique_ptr<sbe::EExpression>> argVars;
            std::vector<std::unique_ptr<sbe::EExpression>> checkExprsNull;
            std::vector<std::unique_ptr<sbe::EExpression>> checkExprsNotNumberOrDate;
            binds.reserve(arity);
            argVars.reserve(arity);
            checkExprsNull.reserve(arity);
            checkExprsNotNumberOrDate.reserve(arity);
            for (size_t idx = 0; idx < arity; ++idx) {
                binds.push_back(_context->popExpr());
                argVars.push_back(sbe::makeE<sbe::EVariable>(frameId, idx));

                checkExprsNull.push_back(generateNullOrMissing(frameId, idx));
                checkExprsNotNumberOrDate.push_back(generateNotNumberOrDate(idx));
            }

            // At this point 'binds' vector contains arguments of $add expression in the reversed
            // order. We need to reverse it back to perform summation in the right order below.
            // Summation in different order can lead to different result because of accumulated
            // precision errors from floating point types.
            std::reverse(std::begin(binds), std::end(binds));

            using iter_t = std::vector<std::unique_ptr<sbe::EExpression>>::iterator;
            auto checkNullAllArguments =
                std::accumulate(std::move_iterator<iter_t>(checkExprsNull.begin() + 1),
                                std::move_iterator<iter_t>(checkExprsNull.end()),
                                std::move(checkExprsNull.front()),
                                [](auto&& acc, auto&& ex) {
                                    return sbe::makeE<sbe::EPrimBinary>(
                                        sbe::EPrimBinary::logicOr, std::move(acc), std::move(ex));
                                });
            auto checkNotNumberOrDateAllArguments =
                std::accumulate(std::move_iterator<iter_t>(checkExprsNotNumberOrDate.begin() + 1),
                                std::move_iterator<iter_t>(checkExprsNotNumberOrDate.end()),
                                std::move(checkExprsNotNumberOrDate.front()),
                                [](auto&& acc, auto&& ex) {
                                    return sbe::makeE<sbe::EPrimBinary>(
                                        sbe::EPrimBinary::logicOr, std::move(acc), std::move(ex));
                                });
            auto addExpr = sbe::makeE<sbe::EIf>(
                std::move(checkNullAllArguments),
                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0),
                sbe::makeE<sbe::EIf>(
                    std::move(checkNotNumberOrDateAllArguments),
                    sbe::makeE<sbe::EFail>(
                        ErrorCodes::Error{4974203},
                        "only numbers and dates are allowed in an $add expression"),
                    sbe::makeE<sbe::EFunction>("doubleDoubleSum", std::move(argVars))));
            _context->pushExpr(
                sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(addExpr)));
        }
    }

    void visit(ExpressionAllElementsTrue* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionAnd* expr) final {
        visitMultiBranchLogicExpression(expr, sbe::EPrimBinary::logicAnd);
    }
    void visit(ExpressionAnyElementTrue* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionArray* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionArrayElemAt* expr) final {
        _context->ensureArity(2);

        auto index = _context->popExpr();
        auto array = _context->popExpr();

        auto frameId = _context->frameIdGenerator->generate();
        auto binds = sbe::makeEs(std::move(array), std::move(index));
        sbe::EVariable arrayRef{frameId, 0};
        sbe::EVariable indexRef{frameId, 1};

        auto int32Index = [&]() {
            auto convertedIndex = sbe::makeE<sbe::ENumericConvert>(
                indexRef.clone(), sbe::value::TypeTags::NumberInt32);
            auto frameId = _context->frameIdGenerator->generate();
            auto binds = sbe::makeEs(std::move(convertedIndex));
            sbe::EVariable convertedIndexRef{frameId, 0};

            auto inExpression = sbe::makeE<sbe::EIf>(
                sbe::makeE<sbe::EFunction>("exists", sbe::makeEs(convertedIndexRef.clone())),
                convertedIndexRef.clone(),
                sbe::makeE<sbe::EFail>(
                    ErrorCodes::Error{5126703},
                    "$arrayElemAt second argument cannot be represented as a 32-bit integer"));

            return sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(inExpression));
        }();

        auto anyOfArgumentsIsNullish =
            sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::logicOr,
                                         generateNullOrMissing(arrayRef),
                                         generateNullOrMissing(indexRef));
        auto firstArgumentIsNotArray =
            makeNot(sbe::makeE<sbe::EFunction>("isArray", sbe::makeEs(arrayRef.clone())));
        auto secondArgumentIsNotNumeric = generateNonNumericCheck(indexRef);
        auto arrayElemAtExpr = buildMultiBranchConditional(
            CaseValuePair{std::move(anyOfArgumentsIsNullish),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{std::move(firstArgumentIsNotArray),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{5126701},
                                                 "$arrayElemAt first argument must be an array")},
            CaseValuePair{std::move(secondArgumentIsNotNumeric),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{5126702},
                                                 "$arrayElemAt second argument must be a number")},
            sbe::makeE<sbe::EFunction>("getElement",
                                       sbe::makeEs(arrayRef.clone(), std::move(int32Index))));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(arrayElemAtExpr)));
    }
    void visit(ExpressionFirst* expr) final {
        buildArrayAccessByConstantIndex(_context, expr->getOpName(), 0);
    }
    void visit(ExpressionLast* expr) final {
        buildArrayAccessByConstantIndex(_context, expr->getOpName(), -1);
    }
    void visit(ExpressionObjectToArray* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionArrayToObject* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionBsonSize* expr) final {
        // Build an expression which evaluates the size of a BSON document and validates the input
        // argument.
        // 1. If the argument is null or empty, return null.
        // 2. Else, if the argument is a BSON document, return its size.
        // 3. Else, raise an error.

        auto frameId = _context->frameIdGenerator->generate();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        auto bsonSizeExpr = buildMultiBranchConditional(
            CaseValuePair{generateNullOrMissing(inputRef),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{generateNonObjectCheck(inputRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{5043001},
                                                 "$bsonSize requires a document input")},
            sbe::makeE<sbe::EFunction>("bsonSize", sbe::makeEs(inputRef.clone())));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(bsonSizeExpr)));
    }
    void visit(ExpressionCeil* expr) final {
        auto frameId = _context->frameIdGenerator->generate();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        auto ceilExpr = buildMultiBranchConditional(
            CaseValuePair{generateNullOrMissing(inputRef),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{generateNonNumericCheck(inputRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4903702},
                                                 "$ceil only supports numeric types")},
            sbe::makeE<sbe::EFunction>("ceil", sbe::makeEs(inputRef.clone())));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(ceilExpr)));
    }
    void visit(ExpressionCoerceToBool* expr) final {
        // Since $coerceToBool is internal-only and there are not yet any input expressions that
        // generate an ExpressionCoerceToBool expression, we will leave it as unreachable for now.
        MONGO_UNREACHABLE;
    }
    void visit(ExpressionCompare* expr) final {
        _context->ensureArity(2);
        std::vector<std::unique_ptr<sbe::EExpression>> operands(2);
        for (auto it = operands.rbegin(); it != operands.rend(); ++it) {
            *it = _context->popExpr();
        }

        auto frameId = _context->frameIdGenerator->generate();
        sbe::EVariable lhsRef(frameId, 0);
        sbe::EVariable rhsRef(frameId, 1);

        auto comparisonOperator = [expr]() {
            switch (expr->getOp()) {
                case ExpressionCompare::CmpOp::EQ:
                    return sbe::EPrimBinary::eq;
                case ExpressionCompare::CmpOp::NE:
                    return sbe::EPrimBinary::neq;
                case ExpressionCompare::CmpOp::GT:
                    return sbe::EPrimBinary::greater;
                case ExpressionCompare::CmpOp::GTE:
                    return sbe::EPrimBinary::greaterEq;
                case ExpressionCompare::CmpOp::LT:
                    return sbe::EPrimBinary::less;
                case ExpressionCompare::CmpOp::LTE:
                    return sbe::EPrimBinary::lessEq;
                case ExpressionCompare::CmpOp::CMP:
                    return sbe::EPrimBinary::cmp3w;
            }
            MONGO_UNREACHABLE;
        }();

        // We use the "cmp3e" primitive for every comparison, because it "type brackets" its
        // comparisons (for example, a number will always compare as less than a string). The other
        // comparison primitives are designed for comparing values of the same type.
        auto cmp3w =
            sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::cmp3w, lhsRef.clone(), rhsRef.clone());
        auto cmp = (comparisonOperator == sbe::EPrimBinary::cmp3w)
            ? std::move(cmp3w)
            : sbe::makeE<sbe::EPrimBinary>(
                  comparisonOperator,
                  std::move(cmp3w),
                  sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                             sbe::value::bitcastFrom<int32_t>(0)));

        // If either operand evaluates to "Nothing," then the entire operation expressed by 'cmp'
        // will also evaluate to "Nothing." MQL comparisons, however, treat "Nothing" as if it is a
        // value that is less than everything other than MinKey. (Notably, two expressions that
        // evaluate to "Nothing" are considered equal to each other.)
        auto nothingFallbackCmp = sbe::makeE<sbe::EPrimBinary>(
            comparisonOperator,
            sbe::makeE<sbe::EFunction>("exists", sbe::makeEs(lhsRef.clone())),
            sbe::makeE<sbe::EFunction>("exists", sbe::makeEs(rhsRef.clone())));

        auto cmpWithFallback = sbe::makeE<sbe::EFunction>(
            "fillEmpty", sbe::makeEs(std::move(cmp), std::move(nothingFallbackCmp)));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(operands), std::move(cmpWithFallback)));
    }

    void visit(ExpressionConcat* expr) final {
        auto arity = expr->getChildren().size();
        _context->ensureArity(arity);
        auto frameId = _context->frameIdGenerator->generate();

        std::vector<std::unique_ptr<sbe::EExpression>> binds;
        std::vector<std::unique_ptr<sbe::EExpression>> checkNullArg;
        std::vector<std::unique_ptr<sbe::EExpression>> checkStringArg;
        std::vector<std::unique_ptr<sbe::EExpression>> argVars;
        sbe::value::SlotId slot{0};
        for (size_t idx = 0; idx < arity; ++idx, ++slot) {
            sbe::EVariable var(frameId, slot);
            binds.push_back(_context->popExpr());
            checkNullArg.push_back(generateNullOrMissing(frameId, slot));
            checkStringArg.push_back(
                sbe::makeE<sbe::EFunction>("isString", sbe::makeEs(var.clone())));
            argVars.push_back(var.clone());
        }
        std::reverse(std::begin(binds), std::end(binds));

        using iter_t = std::vector<std::unique_ptr<sbe::EExpression>>::iterator;
        auto checkNullAnyArgument =
            std::accumulate(std::move_iterator<iter_t>(checkNullArg.begin() + 1),
                            std::move_iterator<iter_t>(checkNullArg.end()),
                            std::move(checkNullArg.front()),
                            [](auto&& acc, auto&& ex) {
                                return sbe::makeE<sbe::EPrimBinary>(
                                    sbe::EPrimBinary::logicOr, std::move(acc), std::move(ex));
                            });

        auto checkStringAllArguments =
            std::accumulate(std::move_iterator<iter_t>(checkStringArg.begin() + 1),
                            std::move_iterator<iter_t>(checkStringArg.end()),
                            std::move(checkStringArg.front()),
                            [](auto&& acc, auto&& ex) {
                                return sbe::makeE<sbe::EPrimBinary>(
                                    sbe::EPrimBinary::logicAnd, std::move(acc), std::move(ex));
                            });
        auto concatExpr = sbe::makeE<sbe::EIf>(
            std::move(checkNullAnyArgument),
            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0),
            sbe::makeE<sbe::EIf>(std::move(checkStringAllArguments),
                                 sbe::makeE<sbe::EFunction>("concat", std::move(argVars)),
                                 sbe::makeE<sbe::EFail>(ErrorCodes::Error{5073001},
                                                        "$concat supports only strings")));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(concatExpr)));
    }

    void visit(ExpressionConcatArrays* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionCond* expr) final {
        visitConditionalExpression(expr);
    }
    void visit(ExpressionDateFromString* expr) final {
        unsupportedExpression("$dateFromString");
    }
    void visit(ExpressionDateFromParts* expr) final {
        // This expression can carry null children depending on the set of fields provided,
        // to compute a date from parts so we only need to pop if a child exists.
        auto children = expr->getChildren();
        invariant(children.size() == 11);

        auto eTimezone = children[10] ? _context->popExpr() : nullptr;
        auto eIsoDayOfWeek = children[9] ? _context->popExpr() : nullptr;
        auto eIsoWeek = children[8] ? _context->popExpr() : nullptr;
        auto eIsoWeekYear = children[7] ? _context->popExpr() : nullptr;
        auto eMillisecond = children[6] ? _context->popExpr() : nullptr;
        auto eSecond = children[5] ? _context->popExpr() : nullptr;
        auto eMinute = children[4] ? _context->popExpr() : nullptr;
        auto eHour = children[3] ? _context->popExpr() : nullptr;
        auto eDay = children[2] ? _context->popExpr() : nullptr;
        auto eMonth = children[1] ? _context->popExpr() : nullptr;
        auto eYear = children[0] ? _context->popExpr() : nullptr;

        // Save a flag to determine if we are in the case of an iso
        // week year. Note that the agg expression parser ensures that one of date or
        // isoWeekYear inputs are provided so we don't need to enforce that at this depth.
        auto isIsoWeekYear = eIsoWeekYear ? true : false;

        auto frameId = _context->frameIdGenerator->generate();
        sbe::EVariable yearRef(frameId, 0);
        sbe::EVariable monthRef(frameId, 1);
        sbe::EVariable dayRef(frameId, 2);
        sbe::EVariable hourRef(frameId, 3);
        sbe::EVariable minRef(frameId, 4);
        sbe::EVariable secRef(frameId, 5);
        sbe::EVariable millisecRef(frameId, 6);
        sbe::EVariable timeZoneRef(frameId, 7);

        // Build a chain of nested bounds checks for each date part that is provided in the
        // expression. We elide the checks in the case that default values are used. These bound
        // checks are then used by folding over pairs of ite tests and else branches to implement
        // short-circuiting in the case that checks fail. To emulate the control flow of MQL for
        // this expression we interleave type conversion checks with time component bound checks.
        const auto minInt16 = std::numeric_limits<int16_t>::lowest();
        const auto maxInt16 = std::numeric_limits<int16_t>::max();

        // Constructs an expression that does a bound check of var over a closed interval [lower,
        // upper].
        auto boundedCheck =
            [](sbe::EExpression& var, int16_t lower, int16_t upper, const std::string& varName) {
                str::stream errMsg;
                if (varName == "year" || varName == "isoWeekYear") {
                    errMsg << "'" << varName << "'"
                           << " must evaluate to an integer in the range " << lower << " to "
                           << upper;
                } else {
                    errMsg << "'" << varName << "'"
                           << " must evaluate to a value in the range [" << lower << ", " << upper
                           << "]";
                }
                return std::make_pair(
                    sbe::makeE<sbe::EPrimBinary>(
                        sbe::EPrimBinary::logicAnd,
                        sbe::makeE<sbe::EPrimBinary>(
                            sbe::EPrimBinary::greaterEq,
                            var.clone(),
                            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                       sbe::value::bitcastFrom<int32_t>(lower))),
                        sbe::makeE<sbe::EPrimBinary>(
                            sbe::EPrimBinary::lessEq,
                            var.clone(),
                            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                       sbe::value::bitcastFrom<int32_t>(upper)))),
                    sbe::makeE<sbe::EFail>(ErrorCodes::Error{4848972}, errMsg));
            };

        // Here we want to validate each field that is provided as input to the agg expression. To
        // do this we implement the following checks:
        //
        // 1) Check if the value in a given slot null or missing. If so bind null to l1.0, and
        // continue to the next binding. Otherwise, do check 2 below.
        //
        // 2) Check if the value in a given slot is an integral int64. This test is done by
        // computing a lossless conversion of the value in s1 to an int64. The exposed
        // conversion function by the vm returns a value if there is no loss of precsision,
        // otherwise it returns Nothing. In both the valid or Nothing case, we can store the result
        // of the conversion in l2.0 of the inner let binding and test for existence. If the
        // existence check fails we know the conversion is lossy and we can fail the query.
        // Otherwise, the inner let evaluates to the converted value which is then bound to the
        // outer let.
        //
        // Each invocation of fieldConversionBinding will produce a nested let of the form.
        //
        // let [l1.0 = s1] in
        //   if (isNull(l1.0) || !exists(l1.0), null,
        //     let [l2.0 = convert(l1.0, int)] in
        //       if (exists(l2.0), l2.0, fail("... must evaluate to an integer")]), ...]
        //  in ...
        auto fieldConversionBinding = [](std::unique_ptr<sbe::EExpression> expr,
                                         sbe::value::FrameIdGenerator* frameIdGenerator,
                                         const std::string& varName) {
            auto outerFrameId = frameIdGenerator->generate();
            auto innerFrameId = frameIdGenerator->generate();
            sbe::EVariable outerSlotRef(outerFrameId, 0);
            sbe::EVariable convertedFieldRef(innerFrameId, 0);
            return sbe::makeE<sbe::ELocalBind>(
                outerFrameId,
                sbe::makeEs(expr->clone()),
                sbe::makeE<sbe::EIf>(
                    sbe::makeE<sbe::EPrimBinary>(
                        sbe::EPrimBinary::logicOr,
                        sbe::makeE<sbe::EPrimUnary>(
                            sbe::EPrimUnary::logicNot,
                            sbe::makeE<sbe::EFunction>("exists",
                                                       sbe::makeEs(outerSlotRef.clone()))),
                        sbe::makeE<sbe::EFunction>("isNull", sbe::makeEs(outerSlotRef.clone()))),
                    sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0),
                    sbe::makeE<sbe::ELocalBind>(
                        innerFrameId,
                        sbe::makeEs(sbe::makeE<sbe::ENumericConvert>(
                            outerSlotRef.clone(), sbe::value::TypeTags::NumberInt64)),
                        sbe::makeE<sbe::EIf>(
                            sbe::makeE<sbe::EFunction>("exists",
                                                       sbe::makeEs(convertedFieldRef.clone())),
                            convertedFieldRef.clone(),
                            sbe::makeE<sbe::EFail>(ErrorCodes::Error{4848979},
                                                   str::stream()
                                                       << "'" << varName << "'"
                                                       << " must evaluate to an integer")))));
        };

        // Build two vectors on the fly to elide bound and conversion for defaulted values.
        std::vector<std::pair<std::unique_ptr<sbe::EExpression>, std::unique_ptr<sbe::EExpression>>>
            boundChecks;  // checks for lower and upper bounds of date fields.

        // Operands is for the outer let bindings.
        std::vector<std::unique_ptr<sbe::EExpression>> operands;
        if (isIsoWeekYear) {
            if (!eIsoWeekYear) {
                eIsoWeekYear = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                          sbe::value::bitcastFrom<int32_t>(1970));
                operands.push_back(std::move(eIsoWeekYear));
            } else {
                boundChecks.push_back(boundedCheck(yearRef, 1, 9999, "isoWeekYear"));
                operands.push_back(fieldConversionBinding(
                    std::move(eIsoWeekYear), _context->frameIdGenerator, "isoWeekYear"));
            }
            if (!eIsoWeek) {
                eIsoWeek = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                      sbe::value::bitcastFrom<int32_t>(1));
                operands.push_back(std::move(eIsoWeek));
            } else {
                boundChecks.push_back(boundedCheck(monthRef, minInt16, maxInt16, "isoWeek"));
                operands.push_back(fieldConversionBinding(
                    std::move(eIsoWeek), _context->frameIdGenerator, "isoWeek"));
            }
            if (!eIsoDayOfWeek) {
                eIsoDayOfWeek = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                           sbe::value::bitcastFrom<int32_t>(1));
                operands.push_back(std::move(eIsoDayOfWeek));
            } else {
                boundChecks.push_back(boundedCheck(dayRef, minInt16, maxInt16, "isoDayOfWeek"));
                operands.push_back(fieldConversionBinding(
                    std::move(eIsoDayOfWeek), _context->frameIdGenerator, "isoDayOfWeek"));
            }
        } else {
            // The regular year/month/day case.
            if (!eYear) {
                eYear = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                   sbe::value::bitcastFrom<int32_t>(1970));
                operands.push_back(std::move(eYear));
            } else {
                boundChecks.push_back(boundedCheck(yearRef, 1, 9999, "year"));
                operands.push_back(
                    fieldConversionBinding(std::move(eYear), _context->frameIdGenerator, "year"));
            }
            if (!eMonth) {
                eMonth = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                    sbe::value::bitcastFrom<int32_t>(1));
                operands.push_back(std::move(eMonth));
            } else {
                boundChecks.push_back(boundedCheck(monthRef, minInt16, maxInt16, "month"));
                operands.push_back(
                    fieldConversionBinding(std::move(eMonth), _context->frameIdGenerator, "month"));
            }
            if (!eDay) {
                eDay = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                  sbe::value::bitcastFrom<int32_t>(1));
                operands.push_back(std::move(eDay));
            } else {
                boundChecks.push_back(boundedCheck(dayRef, minInt16, maxInt16, "day"));
                operands.push_back(
                    fieldConversionBinding(std::move(eDay), _context->frameIdGenerator, "day"));
            }
        }
        if (!eHour) {
            eHour = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                               sbe::value::bitcastFrom<int32_t>(0));
            operands.push_back(std::move(eHour));
        } else {
            boundChecks.push_back(boundedCheck(hourRef, minInt16, maxInt16, "hour"));
            operands.push_back(
                fieldConversionBinding(std::move(eHour), _context->frameIdGenerator, "hour"));
        }
        if (!eMinute) {
            eMinute = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                 sbe::value::bitcastFrom<int32_t>(0));
            operands.push_back(std::move(eMinute));
        } else {
            boundChecks.push_back(boundedCheck(minRef, minInt16, maxInt16, "minute"));
            operands.push_back(
                fieldConversionBinding(std::move(eMinute), _context->frameIdGenerator, "minute"));
        }
        if (!eSecond) {
            eSecond = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                 sbe::value::bitcastFrom<int32_t>(0));
            operands.push_back(std::move(eSecond));
        } else {
            // MQL doesn't place bound restrictions on the second field, because seconds carry over
            // to minutes and can be large ints such as 71,841,012 or even unix epochs.
            operands.push_back(
                fieldConversionBinding(std::move(eSecond), _context->frameIdGenerator, "second"));
        }
        if (!eMillisecond) {
            eMillisecond = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                      sbe::value::bitcastFrom<int32_t>(0));
            operands.push_back(std::move(eMillisecond));
        } else {
            // MQL doesn't enforce bound restrictions on millisecond fields because milliseconds
            // carry over to seconds.
            operands.push_back(fieldConversionBinding(
                std::move(eMillisecond), _context->frameIdGenerator, "millisecond"));
        }
        if (!eTimezone) {
            eTimezone = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::StringSmall, 0);
            operands.push_back(std::move(eTimezone));
        } else {
            // Validate that eTimezone is a string.
            auto tzFrameId = _context->frameIdGenerator->generate();
            sbe::EVariable timezoneRef(tzFrameId, 0);
            operands.push_back(sbe::makeE<sbe::ELocalBind>(
                tzFrameId,
                sbe::makeEs(std::move(eTimezone)),
                sbe::makeE<sbe::EIf>(
                    sbe::makeE<sbe::EFunction>("isString", sbe::makeEs(timeZoneRef.clone())),
                    timezoneRef.clone(),
                    sbe::makeE<sbe::EFail>(ErrorCodes::Error{4848980},
                                           str::stream()
                                               << "'timezone' must evaluate to a string"))));
        }

        // Make a disjunction of null checks for each date part by over this vector. These checks
        // are necessary after the initial conversion computation because we need have the outer let
        // binding evaluate to null if any field is null.
        auto nullExprs =
            makeVector<std::unique_ptr<sbe::EExpression>>(generateNullOrMissing(frameId, 7),
                                                          generateNullOrMissing(frameId, 6),
                                                          generateNullOrMissing(frameId, 5),
                                                          generateNullOrMissing(frameId, 4),
                                                          generateNullOrMissing(frameId, 3),
                                                          generateNullOrMissing(frameId, 2),
                                                          generateNullOrMissing(frameId, 1),
                                                          generateNullOrMissing(frameId, 0));

        using iter_t = std::vector<std::unique_ptr<sbe::EExpression>>::iterator;
        auto checkPartsForNull =
            std::accumulate(std::move_iterator<iter_t>(nullExprs.begin() + 1),
                            std::move_iterator<iter_t>(nullExprs.end()),
                            std::move(nullExprs.front()),
                            [](auto&& acc, auto&& b) {
                                return sbe::makeE<sbe::EPrimBinary>(
                                    sbe::EPrimBinary::logicOr, std::move(acc), std::move(b));
                            });

        // Invocation of the datePartsWeekYear and dateParts functions depend on a TimeZoneDatabase
        // for datetime computation. This global object is registered as an unowned value in the
        // runtime environment so we pass the corresponding slot to the datePartsWeekYear and
        // dateParts functions as a variable.
        auto timeZoneDBSlot = _context->runtimeEnvironment->getSlot("timeZoneDB"_sd);
        auto computeDate =
            sbe::makeE<sbe::EFunction>(isIsoWeekYear ? "datePartsWeekYear" : "dateParts",
                                       sbe::makeEs(sbe::makeE<sbe::EVariable>(timeZoneDBSlot),
                                                   yearRef.clone(),
                                                   monthRef.clone(),
                                                   dayRef.clone(),
                                                   hourRef.clone(),
                                                   minRef.clone(),
                                                   secRef.clone(),
                                                   millisecRef.clone(),
                                                   timeZoneRef.clone()));

        using iterPair_t = std::vector<std::pair<std::unique_ptr<sbe::EExpression>,
                                                 std::unique_ptr<sbe::EExpression>>>::iterator;
        auto computeBoundChecks =
            std::accumulate(std::move_iterator<iterPair_t>(boundChecks.begin()),
                            std::move_iterator<iterPair_t>(boundChecks.end()),
                            std::move(computeDate),
                            [](auto&& acc, auto&& b) {
                                return sbe::makeE<sbe::EIf>(
                                    std::move(b.first), std::move(acc), std::move(b.second));
                            });

        // This final ite expression allows short-circuting of the null field case. If the nullish,
        // checks pass, then we check the bounds of each field and invoke the builtins if all checks
        // pass.
        auto computeDateOrNull =
            sbe::makeE<sbe::EIf>(std::move(checkPartsForNull),
                                 sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0),
                                 std::move(computeBoundChecks));

        _context->pushExpr(sbe::makeE<sbe::ELocalBind>(
            frameId, std::move(operands), std::move(computeDateOrNull)));
    }

    void visit(ExpressionDateToParts* expr) final {
        auto frameId = _context->frameIdGenerator->generate();
        auto children = expr->getChildren();
        std::unique_ptr<sbe::EExpression> date, timezone, isoflag;
        std::unique_ptr<sbe::EExpression> totalExprDateToParts;
        std::vector<std::unique_ptr<sbe::EExpression>> args;
        std::vector<std::unique_ptr<sbe::EExpression>> isoargs;
        std::vector<std::unique_ptr<sbe::EExpression>> operands;
        sbe::EVariable dateRef(frameId, 0);
        sbe::EVariable timezoneRef(frameId, 1);
        sbe::EVariable isoflagRef(frameId, 2);

        // Initialize arguments with values from stack or default values.
        if (children[2]) {
            isoflag = _context->popExpr();
        } else {
            isoflag = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean, false);
        }
        if (children[1]) {
            timezone = _context->popExpr();
        } else {
            auto [utcTag, utcVal] = sbe::value::makeNewString("UTC");
            timezone = sbe::makeE<sbe::EConstant>(utcTag, utcVal);
        }
        if (children[0]) {
            date = _context->popExpr();
        } else {
            _context->pushExpr(sbe::makeE<sbe::EFail>(ErrorCodes::Error{4997700},
                                                      "$dateToParts must include a date"));
            return;
        }

        // Add timezoneDB to arguments.
        args.push_back(
            sbe::makeE<sbe::EVariable>(_context->runtimeEnvironment->getSlot("timeZoneDB"_sd)));
        isoargs.push_back(
            sbe::makeE<sbe::EVariable>(_context->runtimeEnvironment->getSlot("timeZoneDB"_sd)));

        // Add date to arguments.
        uint32_t dateTypeMask = (getBSONTypeMask(sbe::value::TypeTags::Date) |
                                 getBSONTypeMask(sbe::value::TypeTags::Timestamp) |
                                 getBSONTypeMask(sbe::value::TypeTags::ObjectId) |
                                 getBSONTypeMask(sbe::value::TypeTags::bsonObjectId));
        operands.push_back(std::move(date));
        args.push_back(dateRef.clone());
        isoargs.push_back(dateRef.clone());

        // Add timezone to arguments.
        operands.push_back(std::move(timezone));
        args.push_back(timezoneRef.clone());
        isoargs.push_back(timezoneRef.clone());

        // Add iso8601 to arguments.
        uint32_t isoTypeMask = getBSONTypeMask(sbe::value::TypeTags::Boolean);
        operands.push_back(std::move(isoflag));
        args.push_back(isoflagRef.clone());
        isoargs.push_back(isoflagRef.clone());

        // Determine whether to call dateToParts or isoDateToParts.
        auto checkIsoflagValue = buildMultiBranchConditional(
            CaseValuePair{sbe::makeE<sbe::EPrimBinary>(
                              sbe::EPrimBinary::eq,
                              isoflagRef.clone(),
                              sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean, false)),
                          sbe::makeE<sbe::EFunction>("dateToParts", std::move(args))},
            sbe::makeE<sbe::EFunction>("isoDateToParts", std::move(isoargs)));

        // Check that each argument exists, is not null, and is the correct type.
        auto totalDateToPartsFunc = buildMultiBranchConditional(
            CaseValuePair{generateNullOrMissing(frameId, 1),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{
                sbe::makeE<sbe::EPrimUnary>(
                    sbe::EPrimUnary::logicNot,
                    sbe::makeE<sbe::EFunction>("isString", sbe::makeEs(timezoneRef.clone()))),
                sbe::makeE<sbe::EFail>(ErrorCodes::Error{4997701},
                                       "$dateToParts timezone must be a string")},
            CaseValuePair{
                sbe::makeE<sbe::EPrimUnary>(
                    sbe::EPrimUnary::logicNot,
                    sbe::makeE<sbe::EFunction>(
                        "isTimezone",
                        sbe::makeEs(sbe::makeE<sbe::EVariable>(
                                        _context->runtimeEnvironment->getSlot("timeZoneDB"_sd)),
                                    timezoneRef.clone()))),
                sbe::makeE<sbe::EFail>(ErrorCodes::Error{4997704},
                                       "$dateToParts timezone must be a valid timezone")},
            CaseValuePair{generateNullOrMissing(frameId, 2),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{sbe::makeE<sbe::EPrimUnary>(
                              sbe::EPrimUnary::logicNot,
                              sbe::makeE<sbe::ETypeMatch>(isoflagRef.clone(), isoTypeMask)),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4997702},
                                                 "$dateToParts iso8601 must be a boolean")},
            CaseValuePair{generateNullOrMissing(frameId, 0),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{
                sbe::makeE<sbe::EPrimUnary>(
                    sbe::EPrimUnary::logicNot,
                    sbe::makeE<sbe::ETypeMatch>(dateRef.clone(), dateTypeMask)),
                sbe::makeE<sbe::EFail>(ErrorCodes::Error{4997703},
                                       "$dateToParts date must have the format of a date")},
            std::move(checkIsoflagValue));
        _context->pushExpr(sbe::makeE<sbe::ELocalBind>(
            frameId, std::move(operands), std::move(totalDateToPartsFunc)));
    }
    void visit(ExpressionDateToString* expr) final {
        unsupportedExpression("$dateFromString");
    }
    void visit(ExpressionDivide* expr) final {
        _context->ensureArity(2);

        auto rhs = _context->popExpr();
        auto lhs = _context->popExpr();

        auto frameId = _context->frameIdGenerator->generate();
        auto binds = sbe::makeEs(std::move(lhs), std::move(rhs));
        sbe::EVariable lhsRef{frameId, 0};
        sbe::EVariable rhsRef{frameId, 1};

        auto checkIsNumber = sbe::makeE<sbe::EPrimBinary>(
            sbe::EPrimBinary::logicAnd,
            sbe::makeE<sbe::EFunction>("isNumber", sbe::makeEs(lhsRef.clone())),
            sbe::makeE<sbe::EFunction>("isNumber", sbe::makeEs(rhsRef.clone())));

        auto checkIsNullOrMissing = sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::logicOr,
                                                                 generateNullOrMissing(lhsRef),
                                                                 generateNullOrMissing(rhsRef));

        auto divideExpr = buildMultiBranchConditional(
            CaseValuePair{std::move(checkIsNullOrMissing),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{std::move(checkIsNumber),
                          sbe::makeE<sbe::EPrimBinary>(
                              sbe::EPrimBinary::div, lhsRef.clone(), rhsRef.clone())},
            sbe::makeE<sbe::EFail>(ErrorCodes::Error{5073101},
                                   "$divide only supports numeric types"));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(divideExpr)));
    }
    void visit(ExpressionExp* expr) final {
        auto frameId = _context->frameIdGenerator->generate();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        auto expExpr = buildMultiBranchConditional(
            CaseValuePair{generateNullOrMissing(inputRef),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{generateNonNumericCheck(inputRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4903703},
                                                 "$exp only supports numeric types")},
            sbe::makeE<sbe::EFunction>("exp", sbe::makeEs(inputRef.clone())));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(expExpr)));
    }
    void visit(ExpressionFieldPath* expr) final {
        if (expr->getVariableId() == Variables::kRemoveId) {
            // The case of $$REMOVE. Note that MQL allows a path in this situation (e.g.,
            // "$$REMOVE.foo.bar") but ignores it.
            _context->pushExpr(sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Nothing, 0));
            return;
        }

        sbe::value::SlotId slotId;
        if (expr->isRootFieldPath()) {
            slotId = _context->rootSlot;
        } else {
            auto it = _context->environment.find(expr->getVariableId());
            invariant(it != _context->environment.end());
            slotId = it->second;
        }

        if (expr->getFieldPath().getPathLength() == 1) {
            // A solo variable reference (e.g.: "$$ROOT" or "$$myvar") that doesn't need any
            // traversal.
            _context->pushExpr(sbe::makeE<sbe::EVariable>(slotId));
            return;
        }

        // Dereference a dotted path, which may contain arrays requiring implicit traversal.
        const bool expectsDocumentInputOnly = slotId == _context->rootSlot;
        auto [outputSlot, stage] = generateTraverse(std::move(_context->traverseStage),
                                                    slotId,
                                                    expectsDocumentInputOnly,
                                                    expr->getFieldPathWithoutCurrentPrefix(),
                                                    _context->planNodeId,
                                                    _context->slotIdGenerator);
        _context->pushExpr(sbe::makeE<sbe::EVariable>(outputSlot), std::move(stage));
        _context->relevantSlots->push_back(outputSlot);
    }
    void visit(ExpressionFilter* expr) final {
        _context->ensureArity(2);

        auto filterPredicate = _context->popExpr();
        auto input = _context->popExpr();

        // Extract 'traverseStage' generated for filter predicate.
        auto filterTraverseStage = std::move(_context->traverseStage);

        // Restore old value of 'traverseStage' and 'relevantSlots' after filter predicate tree
        // was built.
        auto& filterPredicateEvalFrame = _context->filterExpressionEvalFrameStack.top();
        _context->traverseStage = std::move(filterPredicateEvalFrame.traverseStage);
        *_context->relevantSlots = filterPredicateEvalFrame.relevantSlots;
        _context->filterExpressionEvalFrameStack.pop();

        // Filter predicate of $filter expression expects current array element to be stored in the
        // specific variable. We already allocated slot for it in the "in" visitor, now we just need
        // to retrieve it from the environment.
        // This slot will be used in the traverse stage twice - to store the input array and to
        // store current element in this array.
        auto currentElementVariable = expr->getVariableId();
        invariant(_context->environment.count(currentElementVariable));
        auto inputArraySlot = _context->environment.at(currentElementVariable);

        // We no longer need this mapping because filter predicate which expects it was already
        // compiled.
        _context->environment.erase(currentElementVariable);

        // Construct 'from' branch of traverse stage. SBE tree stored in 'fromBranch' variable looks
        // like this:
        //
        // project inputIsNotNullishSlot = !(isNull(inputArraySlot) || !exists(inputArraySlot))
        // project inputArraySlot = (
        //   let inputRef = input
        //   in
        //       if isArray(inputRef) || isNull(inputRef) || !exists(inputRef)
        //         inputRef
        //       else
        //         fail()
        // )
        // _context->traverseStage
        auto frameId = _context->frameIdGenerator->generate();
        auto binds = sbe::makeEs(std::move(input));
        sbe::EVariable inputRef(frameId, 0);

        auto inputIsArrayOrNullish = sbe::makeE<sbe::EPrimBinary>(
            sbe::EPrimBinary::logicOr,
            generateNullOrMissing(inputRef),
            sbe::makeE<sbe::EFunction>("isArray", sbe::makeEs(inputRef.clone())));
        auto checkInputArrayType =
            sbe::makeE<sbe::EIf>(std::move(inputIsArrayOrNullish),
                                 inputRef.clone(),
                                 sbe::makeE<sbe::EFail>(ErrorCodes::Error{5073201},
                                                        "input to $filter must be an array"));
        auto inputArray =
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(checkInputArrayType));

        sbe::EVariable inputArrayVariable{inputArraySlot};
        auto projectInputArray = sbe::makeProjectStage(std::move(_context->traverseStage),
                                                       _context->planNodeId,
                                                       inputArraySlot,
                                                       std::move(inputArray));

        auto inputIsNotNullish = makeNot(generateNullOrMissing(inputArrayVariable));
        auto inputIsNotNullishSlot = _context->slotIdGenerator->generate();
        auto fromBranch = sbe::makeProjectStage(std::move(projectInputArray),
                                                _context->planNodeId,
                                                inputIsNotNullishSlot,
                                                std::move(inputIsNotNullish));

        // Construct 'in' branch of traverse stage. SBE tree stored in 'inBranch' variable looks
        // like this:
        //
        // cfilter Variable{inputIsNotNullishSlot}
        // filter filterPredicate
        // filterTraverseStage
        //
        // Filter predicate can return non-boolean values. To fix this, we generate expression to
        // coerce it to bool type.
        frameId = _context->frameIdGenerator->generate();
        auto boolFilterPredicate =
            sbe::makeE<sbe::ELocalBind>(frameId,
                                        sbe::makeEs(std::move(filterPredicate)),
                                        generateCoerceToBoolExpression(sbe::EVariable{frameId, 0}));
        auto filterWithPredicate = sbe::makeS<sbe::FilterStage<false>>(
            std::move(filterTraverseStage), std::move(boolFilterPredicate), _context->planNodeId);

        // If input array is null or missing, we do not evaluate filter predicate and return EOF.
        auto innerBranch =
            sbe::makeS<sbe::FilterStage<true>>(std::move(filterWithPredicate),
                                               sbe::makeE<sbe::EVariable>(inputIsNotNullishSlot),
                                               _context->planNodeId);

        // Relevant slots from the _context->traverseStage might be used in the traverse 'in' branch
        // by filter predicate through path expressions and variables. We need to pass them
        // explicitly as correlated to traverse 'from' branch.
        auto outerCorrelatedSlots = *_context->relevantSlots;

        // Add all variables from the environment.
        for (const auto& item : _context->environment) {
            outerCorrelatedSlots.push_back(item.second);
        }

        // inputIsNotNullishSlot is used explicitly by cfilter stage added on top of traverse 'in'
        // branch.
        outerCorrelatedSlots.push_back(inputIsNotNullishSlot);

        // Construct traverse stage with the following slots:
        // * inputArraySlot - slot containing input array of $filter expression
        // * filteredArraySlot - slot containing the array with items on which filter predicate has
        //   evaluated to true
        // * inputArraySlot - slot where 'in' branch of traverse stage stores current array
        //   element if it satisfies the filter predicate
        auto filteredArraySlot = _context->slotIdGenerator->generate();
        auto traverseStage =
            sbe::makeS<sbe::TraverseStage>(std::move(fromBranch),
                                           std::move(innerBranch),
                                           inputArraySlot /* inField */,
                                           filteredArraySlot /* outField */,
                                           inputArraySlot /* outFieldInner */,
                                           std::move(outerCorrelatedSlots) /* outerCorrelated */,
                                           nullptr /* foldExpr */,
                                           nullptr /* finalExpr */,
                                           _context->planNodeId,
                                           1 /* nestedArraysDepth */);

        // If input array is null or missing, 'in' stage of traverse will return EOF. In this case
        // traverse sets output slot (filteredArraySlot) to Nothing. We replace it with Null to
        // match $filter expression behaviour.
        auto result = sbe::makeE<sbe::EFunction>(
            "fillEmpty",
            sbe::makeEs(sbe::makeE<sbe::EVariable>(filteredArraySlot),
                        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)));

        _context->pushExpr(std::move(result), std::move(traverseStage));
    }
    void visit(ExpressionFloor* expr) final {
        auto frameId = _context->frameIdGenerator->generate();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        auto floorExpr = buildMultiBranchConditional(
            CaseValuePair{generateNullOrMissing(inputRef),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{generateNonNumericCheck(inputRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4903704},
                                                 "$floor only supports numeric types")},
            sbe::makeE<sbe::EFunction>("floor", sbe::makeEs(inputRef.clone())));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(floorExpr)));
    }
    void visit(ExpressionIfNull* expr) final {
        _context->ensureArity(2);

        auto replacementIfNull = _context->popExpr();
        auto input = _context->popExpr();

        auto frameId = _context->frameIdGenerator->generate();
        auto binds = sbe::makeEs(std::move(input));
        sbe::EVariable inputRef(frameId, 0);

        // If input is null or missing, return replacement expression. Otherwise, return input.
        auto ifNullExpr = sbe::makeE<sbe::EIf>(
            generateNullOrMissing(frameId, 0), std::move(replacementIfNull), inputRef.clone());

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(ifNullExpr)));
    }
    void visit(ExpressionIn* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionIndexOfArray* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(ExpressionIndexOfBytes* expr) final {
        visitIndexOfFunction(expr, _context, "indexOfBytes");
    }

    void visit(ExpressionIndexOfCP* expr) final {
        visitIndexOfFunction(expr, _context, "indexOfCP");
    }
    void visit(ExpressionIsNumber* expr) final {
        auto frameId = _context->frameIdGenerator->generate();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        auto exprIsNum = sbe::makeE<sbe::EIf>(
            sbe::makeE<sbe::EFunction>("exists", sbe::makeEs(inputRef.clone())),
            sbe::makeE<sbe::EFunction>("isNumber", sbe::makeEs(inputRef.clone())),
            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean,
                                       sbe::value::bitcastFrom<bool>(false)));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(exprIsNum)));
    }
    void visit(ExpressionLet* expr) final {
        // The evaluated result of the $let is the evaluated result of its "in" field, which is
        // already on top of the stack. The "infix" visitor has already popped the variable
        // initializers off the expression stack.
        _context->ensureArity(1);

        // We should have bound all the variables from this $let expression.
        invariant(!_context->varsFrameStack.empty());
        auto& currentFrame = _context->varsFrameStack.top();
        invariant(currentFrame.variablesToBind.empty());

        // Pop the lexical frame for this $let and remove all its bindings, which are now out of
        // scope.
        auto it = _context->environment.begin();
        while (it != _context->environment.end()) {
            if (currentFrame.slotsForLetVariables.count(it->second)) {
                it = _context->environment.erase(it);
            } else {
                ++it;
            }
        }
        _context->varsFrameStack.pop();

        // Note that there is no need to remove SlotId bindings from the the _context's environment.
        // The AST parser already enforces scope rules.
    }
    void visit(ExpressionLn* expr) final {
        auto frameId = _context->frameIdGenerator->generate();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        auto lnExpr = buildMultiBranchConditional(
            CaseValuePair{generateNullOrMissing(inputRef),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{generateNonNumericCheck(inputRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4903705},
                                                 "$ln only supports numeric types")},
            // Note: In MQL, $ln on a NumberDecimal NaN historically evaluates to a NumberDouble
            // NaN.
            CaseValuePair{generateNaNCheck(inputRef),
                          sbe::makeE<sbe::ENumericConvert>(inputRef.clone(),
                                                           sbe::value::TypeTags::NumberDouble)},
            CaseValuePair{generateNonPositiveCheck(inputRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4903706},
                                                 "$ln's argument must be a positive number")},
            sbe::makeE<sbe::EFunction>("ln", sbe::makeEs(inputRef.clone())));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(lnExpr)));
    }
    void visit(ExpressionLog* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionLog10* expr) final {
        auto frameId = _context->frameIdGenerator->generate();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        auto log10Expr = buildMultiBranchConditional(
            CaseValuePair{generateNullOrMissing(inputRef),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{generateNonNumericCheck(inputRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4903707},
                                                 "$log10 only supports numeric types")},
            // Note: In MQL, $log10 on a NumberDecimal NaN historically evaluates to a NumberDouble
            // NaN.
            CaseValuePair{generateNaNCheck(inputRef),
                          sbe::makeE<sbe::ENumericConvert>(inputRef.clone(),
                                                           sbe::value::TypeTags::NumberDouble)},
            CaseValuePair{generateNonPositiveCheck(inputRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4903708},
                                                 "$log10's argument must be a positive number")},
            sbe::makeE<sbe::EFunction>("log10", sbe::makeEs(inputRef.clone())));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(log10Expr)));
    }
    void visit(ExpressionMap* expr) final {
        unsupportedExpression("$map");
    }
    void visit(ExpressionMeta* expr) final {
        unsupportedExpression("$meta");
    }
    void visit(ExpressionMod* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionMultiply* expr) final {
        auto arity = expr->getChildren().size();
        _context->ensureArity(arity);
        auto frameId = _context->frameIdGenerator->generate();

        std::vector<std::unique_ptr<sbe::EExpression>> binds;
        std::vector<std::unique_ptr<sbe::EExpression>> variables;
        std::vector<std::unique_ptr<sbe::EExpression>> checkExprsNull;
        std::vector<std::unique_ptr<sbe::EExpression>> checkExprsNumber;
        binds.reserve(arity);
        variables.reserve(arity);
        checkExprsNull.reserve(arity);
        checkExprsNumber.reserve(arity);
        sbe::value::SlotId slot{0};
        for (size_t idx = 0; idx < arity; ++idx, ++slot) {
            binds.push_back(_context->popExpr());
            sbe::EVariable currentVariable{frameId, slot};
            variables.push_back(currentVariable.clone());

            checkExprsNull.push_back(generateNullOrMissing(currentVariable));
            checkExprsNumber.push_back(
                sbe::makeE<sbe::EFunction>("isNumber", sbe::makeEs(currentVariable.clone())));
        }

        // At this point 'binds' vector contains arguments of $multiply expression in the reversed
        // order. We need to reverse it back to perform multiplication in the right order below.
        // Multiplication in different order can lead to different result because of accumulated
        // precision errors from floating point types.
        std::reverse(std::begin(binds), std::end(binds));

        using iter_t = std::vector<std::unique_ptr<sbe::EExpression>>::iterator;
        auto checkNullAnyArgument =
            std::accumulate(std::move_iterator<iter_t>(checkExprsNull.begin() + 1),
                            std::move_iterator<iter_t>(checkExprsNull.end()),
                            std::move(checkExprsNull.front()),
                            [](auto&& acc, auto&& ex) {
                                return sbe::makeE<sbe::EPrimBinary>(
                                    sbe::EPrimBinary::logicOr, std::move(acc), std::move(ex));
                            });

        auto checkNumberAllArguments =
            std::accumulate(std::move_iterator<iter_t>(checkExprsNumber.begin() + 1),
                            std::move_iterator<iter_t>(checkExprsNumber.end()),
                            std::move(checkExprsNumber.front()),
                            [](auto&& acc, auto&& ex) {
                                return sbe::makeE<sbe::EPrimBinary>(
                                    sbe::EPrimBinary::logicAnd, std::move(acc), std::move(ex));
                            });

        auto multiplication =
            std::accumulate(std::move_iterator<iter_t>(variables.begin() + 1),
                            std::move_iterator<iter_t>(variables.end()),
                            std::move(variables.front()),
                            [](auto&& acc, auto&& ex) {
                                return sbe::makeE<sbe::EPrimBinary>(
                                    sbe::EPrimBinary::mul, std::move(acc), std::move(ex));
                            });

        auto multiplyExpr = buildMultiBranchConditional(
            CaseValuePair{std::move(checkNullAnyArgument),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{std::move(checkNumberAllArguments), std::move(multiplication)},
            sbe::makeE<sbe::EFail>(ErrorCodes::Error{5073102},
                                   "only numbers are allowed in an $multiply expression"));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(multiplyExpr)));
    }
    void visit(ExpressionNot* expr) final {
        auto frameId = _context->frameIdGenerator->generate();
        auto binds = sbe::makeEs(_context->popExpr());

        auto notExpr = sbe::makeE<sbe::EPrimUnary>(sbe::EPrimUnary::logicNot,
                                                   generateCoerceToBoolExpression({frameId, 0}));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(notExpr)));
    }
    void visit(ExpressionObject* expr) final {
        unsupportedExpression("$object");
    }
    void visit(ExpressionOr* expr) final {
        visitMultiBranchLogicExpression(expr, sbe::EPrimBinary::logicOr);
    }
    void visit(ExpressionPow* expr) final {
        unsupportedExpression("$pow");
    }
    void visit(ExpressionRange* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionReduce* expr) final {
        unsupportedExpression("$reduce");
    }
    void visit(ExpressionReplaceOne* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionReplaceAll* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionSetDifference* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionSetEquals* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionSetIntersection* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionSetIsSubset* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionSetUnion* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionSize* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionReverseArray* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionSlice* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionIsArray* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionRound* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionSplit* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionSqrt* expr) final {
        auto frameId = _context->frameIdGenerator->generate();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        auto lnExpr = buildMultiBranchConditional(
            CaseValuePair{generateNullOrMissing(inputRef),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{generateNonNumericCheck(inputRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{4903709},
                                                 "$sqrt only supports numeric types")},
            CaseValuePair{
                generateNegativeCheck(inputRef),
                sbe::makeE<sbe::EFail>(ErrorCodes::Error{4903710},
                                       "$sqrt's argument must be greater than or equal to 0")},
            sbe::makeE<sbe::EFunction>("sqrt", sbe::makeEs(inputRef.clone())));

        _context->pushExpr(
            sbe::makeE<sbe::ELocalBind>(frameId, std::move(binds), std::move(lnExpr)));
    }
    void visit(ExpressionStrcasecmp* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionSubstrBytes* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionSubstrCP* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionStrLenBytes* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionBinarySize* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionStrLenCP* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionSubtract* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionSwitch* expr) final {
        visitConditionalExpression(expr);
    }
    void visit(ExpressionToLower* expr) final {
        generateStringCaseConversionExpression(_context, "toLower");
    }
    void visit(ExpressionToUpper* expr) final {
        generateStringCaseConversionExpression(_context, "toUpper");
    }
    void visit(ExpressionTrim* expr) final {
        unsupportedExpression("$trim");
    }
    void visit(ExpressionTrunc* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionType* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionZip* expr) final {
        unsupportedExpression("$zip");
    }
    void visit(ExpressionConvert* expr) final {
        unsupportedExpression("$convert");
    }
    void visit(ExpressionRegexFind* expr) final {
        unsupportedExpression("$regexFind");
    }
    void visit(ExpressionRegexFindAll* expr) final {
        unsupportedExpression("$regexFind");
    }
    void visit(ExpressionRegexMatch* expr) final {
        unsupportedExpression("$regexFind");
    }
    void visit(ExpressionCosine* expr) final {
        generateTrigonometricExpressionWithBounds(
            "cos", DoubleBound::minInfinity(), DoubleBound::plusInfinity());
    }
    void visit(ExpressionSine* expr) final {
        generateTrigonometricExpressionWithBounds(
            "sin", DoubleBound::minInfinity(), DoubleBound::plusInfinity());
    }
    void visit(ExpressionTangent* expr) final {
        generateTrigonometricExpressionWithBounds(
            "tan", DoubleBound::minInfinity(), DoubleBound::plusInfinity());
    }
    void visit(ExpressionArcCosine* expr) final {
        generateTrigonometricExpressionWithBounds(
            "acos", DoubleBound(-1.0, true), DoubleBound(1.0, true));
    }
    void visit(ExpressionArcSine* expr) final {
        generateTrigonometricExpressionWithBounds(
            "asin", DoubleBound(-1.0, true), DoubleBound(1.0, true));
    }
    void visit(ExpressionArcTangent* expr) final {
        generateTrigonometricExpression("atan");
    }
    void visit(ExpressionArcTangent2* expr) final {
        generateTrigonometricExpression("atan2");
    }
    void visit(ExpressionHyperbolicArcTangent* expr) final {
        generateTrigonometricExpressionWithBounds(
            "atanh", DoubleBound(-1.0, true), DoubleBound(1.0, true));
    }
    void visit(ExpressionHyperbolicArcCosine* expr) final {
        generateTrigonometricExpressionWithBounds(
            "acosh", DoubleBound(1.0, true), DoubleBound::plusInfinity());
    }
    void visit(ExpressionHyperbolicArcSine* expr) final {
        generateTrigonometricExpression("asinh");
    }
    void visit(ExpressionHyperbolicCosine* expr) final {
        generateTrigonometricExpression("cosh");
    }
    void visit(ExpressionHyperbolicSine* expr) final {
        generateTrigonometricExpression("sinh");
    }
    void visit(ExpressionHyperbolicTangent* expr) final {
        generateTrigonometricExpression("tanh");
    }
    void visit(ExpressionDegreesToRadians* expr) final {
        generateTrigonometricExpression("degreesToRadians");
    }
    void visit(ExpressionRadiansToDegrees* expr) final {
        generateTrigonometricExpression("radiansToDegrees");
    }
    void visit(ExpressionDayOfMonth* expr) final {
        unsupportedExpression("$dayOfMonth");
    }
    void visit(ExpressionDayOfWeek* expr) final {
        unsupportedExpression("$dayOfWeek");
    }
    void visit(ExpressionDayOfYear* expr) final {
        unsupportedExpression("$dayOfYear");
    }
    void visit(ExpressionHour* expr) final {
        unsupportedExpression("$hour");
    }
    void visit(ExpressionMillisecond* expr) final {
        unsupportedExpression("$millisecond");
    }
    void visit(ExpressionMinute* expr) final {
        unsupportedExpression("$minute");
    }
    void visit(ExpressionMonth* expr) final {
        unsupportedExpression("$month");
    }
    void visit(ExpressionSecond* expr) final {
        unsupportedExpression("$second");
    }
    void visit(ExpressionWeek* expr) final {
        unsupportedExpression("$week");
    }
    void visit(ExpressionIsoWeekYear* expr) final {
        unsupportedExpression("$isoWeekYear");
    }
    void visit(ExpressionIsoDayOfWeek* expr) final {
        unsupportedExpression("$isoDayOfWeek");
    }
    void visit(ExpressionIsoWeek* expr) final {
        unsupportedExpression("$isoWeek");
    }
    void visit(ExpressionYear* expr) final {
        unsupportedExpression("$year");
    }
    void visit(ExpressionFromAccumulator<AccumulatorAvg>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionFromAccumulator<AccumulatorMax>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionFromAccumulator<AccumulatorMin>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionFromAccumulator<AccumulatorStdDevPop>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionFromAccumulator<AccumulatorStdDevSamp>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionFromAccumulator<AccumulatorSum>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionFromAccumulator<AccumulatorMergeObjects>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(ExpressionTests::Testable* expr) final {
        unsupportedExpression("$test");
    }
    void visit(ExpressionInternalJsEmit* expr) final {
        unsupportedExpression("$internalJsEmit");
    }
    void visit(ExpressionInternalFindSlice* expr) final {
        unsupportedExpression("$internalFindSlice");
    }
    void visit(ExpressionInternalFindPositional* expr) final {
        unsupportedExpression("$internalFindPositional");
    }
    void visit(ExpressionInternalFindElemMatch* expr) final {
        unsupportedExpression("$internalFindElemMatch");
    }
    void visit(ExpressionFunction* expr) final {
        unsupportedExpression("$function");
    }

    void visit(ExpressionRandom* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(ExpressionToHashedIndexKey* expr) final {
        unsupportedExpression("$toHashedIndexKey");
    }

private:
    /**
     * Shared logic for $and, $or. Converts each child into an EExpression that evaluates to Boolean
     * true or false, based on MQL rules for $and and $or branches, and then chains the branches
     * together using binary and/or EExpressions so that the result has MQL's short-circuit
     * semantics.
     */
    void visitMultiBranchLogicExpression(Expression* expr, sbe::EPrimBinary::Op logicOp) {
        invariant(logicOp == sbe::EPrimBinary::logicAnd || logicOp == sbe::EPrimBinary::logicOr);

        if (expr->getChildren().size() == 0) {
            // Empty $and and $or always evaluate to their logical operator's identity value: true
            // and false, respectively.
            auto logicIdentityVal = (logicOp == sbe::EPrimBinary::logicAnd);
            _context->pushExpr(sbe::makeE<sbe::EConstant>(
                sbe::value::TypeTags::Boolean, sbe::value::bitcastFrom<bool>(logicIdentityVal)));
            return;
        } else if (expr->getChildren().size() == 1) {
            // No need for short circuiting logic in a singleton $and/$or. Just execute the branch
            // and return its result as a bool.
            auto frameId = _context->frameIdGenerator->generate();
            _context->pushExpr(sbe::makeE<sbe::ELocalBind>(
                frameId,
                sbe::makeEs(_context->popExpr()),
                generateCoerceToBoolExpression(sbe::EVariable{frameId, 0})));

            return;
        }

        auto& logicalExpressionEvalFrame = _context->logicalExpressionEvalFrameStack.top();

        // The last branch works differently from the others. It just uses a project stage to
        // produce a true or false value for the branch result.
        auto frameId = _context->frameIdGenerator->generate();
        auto lastBranchExpr =
            sbe::makeE<sbe::ELocalBind>(frameId,
                                        sbe::makeEs(_context->popExpr()),
                                        generateCoerceToBoolExpression(sbe::EVariable{frameId, 0}));
        auto lastBranchResultSlot = _context->slotIdGenerator->generate();
        auto lastBranch = sbe::makeProjectStage(std::move(_context->traverseStage),
                                                _context->planNodeId,
                                                lastBranchResultSlot,
                                                std::move(lastBranchExpr));
        logicalExpressionEvalFrame.branches.emplace_back(
            std::make_pair(lastBranchResultSlot, std::move(lastBranch)));

        _context->generateSubTreeForSelectiveExecution();
    }

    /**
     * Handle $switch and $cond, which have different syntax but are structurally identical in the
     * AST.
     */
    void visitConditionalExpression(Expression* expr) {
        invariant(_context->logicalExpressionEvalFrameStack.size() > 0);
        auto& logicalExpressionEvalFrame = _context->logicalExpressionEvalFrameStack.top();

        // If this is not boost::none, that would mean the AST somehow had a branch with a "case"
        // condition but without a "then" value.
        invariant(logicalExpressionEvalFrame.switchBranchConditionalStage == boost::none);

        // The default case is always the last child in the ExpressionSwitch. If it is unspecified
        // in the user's query, it is a nullptr. In ExpressionCond, the last child is the "else"
        // branch, and it is guaranteed not to be nullptr.
        auto defaultExpr = expr->getChildren().back() == nullptr
            ? sbe::makeE<sbe::EFail>(ErrorCodes::Error{4934200},
                                     "$switch could not find a matching branch for an "
                                     "input, and no default was specified.")
            : this->_context->popExpr();

        auto defaultBranchStage =
            sbe::makeProjectStage(std::move(_context->traverseStage),
                                  _context->planNodeId,
                                  logicalExpressionEvalFrame.nextBranchResultSlot,
                                  std::move(defaultExpr));

        logicalExpressionEvalFrame.branches.emplace_back(std::make_pair(
            logicalExpressionEvalFrame.nextBranchResultSlot, std::move(defaultBranchStage)));

        _context->generateSubTreeForSelectiveExecution();
    }

    /**
     * Shared expression building logic for trignometric expressions to make sure the operand
     * is numeric and is not null.
     */
    void generateTrigonometricExpression(StringData exprName) {
        auto frameId = _context->frameIdGenerator->generate();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        auto genericTrignomentricExpr = sbe::makeE<sbe::EIf>(
            generateNullOrMissing(frameId, 0),
            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0),
            sbe::makeE<sbe::EIf>(
                sbe::makeE<sbe::EFunction>("isNumber", sbe::makeEs(inputRef.clone())),
                sbe::makeE<sbe::EFunction>(exprName.toString(), sbe::makeEs(inputRef.clone())),
                sbe::makeE<sbe::EFail>(ErrorCodes::Error{4995501},
                                       str::stream() << "$" << exprName.toString()
                                                     << " supports only numeric types")));

        _context->pushExpr(sbe::makeE<sbe::ELocalBind>(
            frameId, std::move(binds), std::move(genericTrignomentricExpr)));
    }

    /**
     * Shared expression building logic for trignometric expressions with bounds for the valid
     * values of the argument.
     */
    void generateTrigonometricExpressionWithBounds(StringData exprName,
                                                   const DoubleBound& lowerBound,
                                                   const DoubleBound& upperBound) {
        auto frameId = _context->frameIdGenerator->generate();
        auto binds = sbe::makeEs(_context->popExpr());
        sbe::EVariable inputRef(frameId, 0);

        sbe::EPrimBinary::Op lowerCmp =
            lowerBound.inclusive ? sbe::EPrimBinary::greaterEq : sbe::EPrimBinary::greater;
        sbe::EPrimBinary::Op upperCmp =
            upperBound.inclusive ? sbe::EPrimBinary::lessEq : sbe::EPrimBinary::less;
        auto checkBounds = sbe::makeE<sbe::EPrimBinary>(
            sbe::EPrimBinary::logicAnd,
            sbe::makeE<sbe::EPrimBinary>(
                lowerCmp,
                inputRef.clone(),
                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberDouble,
                                           sbe::value::bitcastFrom<double>(lowerBound.bound))),
            sbe::makeE<sbe::EPrimBinary>(
                upperCmp,
                inputRef.clone(),
                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberDouble,
                                           sbe::value::bitcastFrom<double>(upperBound.bound))));

        auto genericTrignomentricExpr = sbe::makeE<sbe::EIf>(
            generateNullOrMissing(frameId, 0),
            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0),
            sbe::makeE<sbe::EIf>(
                sbe::makeE<sbe::EPrimUnary>(
                    sbe::EPrimUnary::logicNot,
                    sbe::makeE<sbe::EFunction>("isNumber", sbe::makeEs(inputRef.clone()))),
                sbe::makeE<sbe::EFail>(ErrorCodes::Error{4995502},
                                       str::stream() << "$" << exprName.toString()
                                                     << " supports only numeric types"),
                sbe::makeE<sbe::EIf>(
                    std::move(checkBounds),
                    sbe::makeE<sbe::EFunction>(exprName.toString(), sbe::makeEs(inputRef.clone())),
                    sbe::makeE<sbe::EFail>(ErrorCodes::Error{4995503},
                                           str::stream() << "Cannot apply $" << exprName.toString()
                                                         << ", value must be in "
                                                         << lowerBound.printLowerBound() << ", "
                                                         << upperBound.printUpperBound()))));

        _context->pushExpr(sbe::makeE<sbe::ELocalBind>(
            frameId, std::move(binds), std::move(genericTrignomentricExpr)));
    }

    /*
     * Generates an EExpression that returns an index for $indexOfBytes or $indexOfCP.
     */
    void visitIndexOfFunction(Expression* expr,
                              ExpressionVisitorContext* _context,
                              const std::string& indexOfFunction) {
        auto frameId = _context->frameIdGenerator->generate();
        auto children = expr->getChildren();
        auto operandSize = children.size() <= 3 ? 3 : 4;
        std::vector<std::unique_ptr<sbe::EExpression>> operands(operandSize);
        std::vector<std::unique_ptr<sbe::EExpression>> bindings;
        sbe::EVariable strRef(frameId, 0);
        sbe::EVariable substrRef(frameId, 1);
        boost::optional<sbe::EVariable> startIndexRef;
        boost::optional<sbe::EVariable> endIndexRef;

        // Get arguments from stack.
        switch (children.size()) {
            case 2: {
                operands[2] = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                                         sbe::value::bitcastFrom<int64_t>(0));
                operands[1] = _context->popExpr();
                operands[0] = _context->popExpr();
                startIndexRef.emplace(frameId, 2);
                break;
            }
            case 3: {
                operands[2] = _context->popExpr();
                operands[1] = _context->popExpr();
                operands[0] = _context->popExpr();
                startIndexRef.emplace(frameId, 2);
                break;
            }
            case 4: {
                operands[3] = _context->popExpr();
                operands[2] = _context->popExpr();
                operands[1] = _context->popExpr();
                operands[0] = _context->popExpr();
                startIndexRef.emplace(frameId, 2);
                endIndexRef.emplace(frameId, 3);
                break;
            }
            default:
                MONGO_UNREACHABLE;
        }

        // Add string and substring operands.
        bindings.push_back(strRef.clone());
        bindings.push_back(substrRef.clone());

        // Add start index operand.
        if (startIndexRef) {
            auto numericConvert64 = sbe::makeE<sbe::ENumericConvert>(
                startIndexRef->clone(), sbe::value::TypeTags::NumberInt64);
            auto checkValidStartIndex = buildMultiBranchConditional(
                CaseValuePair{generateNullishOrNotRepresentableInt32Check(*startIndexRef),
                              sbe::makeE<sbe::EFail>(
                                  ErrorCodes::Error{5075303},
                                  str::stream() << "$" << indexOfFunction
                                                << " start index must resolve to a number")},
                CaseValuePair{generateNegativeCheck(*startIndexRef),
                              sbe::makeE<sbe::EFail>(ErrorCodes::Error{5075304},
                                                     str::stream()
                                                         << "$" << indexOfFunction
                                                         << " start index must be positive")},
                std::move(numericConvert64));
            bindings.push_back(std::move(checkValidStartIndex));
        }
        // Add end index operand.
        if (endIndexRef) {
            auto numericConvert64 = sbe::makeE<sbe::ENumericConvert>(
                endIndexRef->clone(), sbe::value::TypeTags::NumberInt64);
            auto checkValidEndIndex = buildMultiBranchConditional(
                CaseValuePair{generateNullishOrNotRepresentableInt32Check(*endIndexRef),
                              sbe::makeE<sbe::EFail>(ErrorCodes::Error{5075305},
                                                     str::stream()
                                                         << "$" << indexOfFunction
                                                         << " end index must resolve to a number")},
                CaseValuePair{generateNegativeCheck(*endIndexRef),
                              sbe::makeE<sbe::EFail>(ErrorCodes::Error{5075306},
                                                     str::stream()
                                                         << "$" << indexOfFunction
                                                         << " end index must be positive")},
                std::move(numericConvert64));
            bindings.push_back(std::move(checkValidEndIndex));
        }

        // Check if string or substring are null or missing before calling indexOfFunction.
        auto checkStringNullOrMissing = generateNullOrMissing(frameId, 0);
        auto checkSubstringNullOrMissing = generateNullOrMissing(frameId, 1);
        auto exprIndexOfFunction = sbe::makeE<sbe::EFunction>(indexOfFunction, std::move(bindings));

        auto totalExprIndexOfFunction = buildMultiBranchConditional(
            CaseValuePair{std::move(checkStringNullOrMissing),
                          sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0)},
            CaseValuePair{generateNonStringCheck(strRef),
                          sbe::makeE<sbe::EFail>(
                              ErrorCodes::Error{5075300},
                              str::stream() << "$" << indexOfFunction
                                            << " string must resolve to a string or null")},
            CaseValuePair{std::move(checkSubstringNullOrMissing),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{5075301},
                                                 str::stream()
                                                     << "$" << indexOfFunction
                                                     << " substring must resolve to a string")},
            CaseValuePair{generateNonStringCheck(substrRef),
                          sbe::makeE<sbe::EFail>(ErrorCodes::Error{5075302},
                                                 str::stream()
                                                     << "$" << indexOfFunction
                                                     << " substring must resolve to a string")},
            std::move(exprIndexOfFunction));
        _context->pushExpr(sbe::makeE<sbe::ELocalBind>(
            frameId, std::move(operands), std::move(totalExprIndexOfFunction)));
    }

    void unsupportedExpression(const char* op) const {
        uasserted(ErrorCodes::InternalErrorNotSupported,
                  str::stream() << "Expression is not supported in SBE: " << op);
    }

    ExpressionVisitorContext* _context;
};  // namespace

class ExpressionWalker final {
public:
    ExpressionWalker(ExpressionVisitor* preVisitor,
                     ExpressionVisitor* inVisitor,
                     ExpressionVisitor* postVisitor)
        : _preVisitor{preVisitor}, _inVisitor{inVisitor}, _postVisitor{postVisitor} {}

    void preVisit(Expression* expr) {
        expr->acceptVisitor(_preVisitor);
    }

    void inVisit(long long count, Expression* expr) {
        expr->acceptVisitor(_inVisitor);
    }

    void postVisit(Expression* expr) {
        expr->acceptVisitor(_postVisitor);
    }

private:
    ExpressionVisitor* _preVisitor;
    ExpressionVisitor* _inVisitor;
    ExpressionVisitor* _postVisitor;
};
}  // namespace

std::unique_ptr<sbe::EExpression> generateCoerceToBoolExpression(sbe::EVariable branchRef) {
    // Make an expression that compares the value in 'branchRef' to the result of evaluating the
    // 'valExpr' expression. The comparison uses cmp3w, so that can handle comparisons between
    // values with different types.
    auto makeNeqCheck = [&branchRef](std::unique_ptr<sbe::EExpression> valExpr) {
        return sbe::makeE<sbe::EPrimBinary>(
            sbe::EPrimBinary::neq,
            sbe::makeE<sbe::EPrimBinary>(
                sbe::EPrimBinary::cmp3w, branchRef.clone(), std::move(valExpr)),
            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                       sbe::value::bitcastFrom<int64_t>(0)));
    };

    // If any of these are false, the branch is considered false for the purposes of the
    // any logical expression.
    auto checkExists = sbe::makeE<sbe::EFunction>("exists", sbe::makeEs(branchRef.clone()));
    auto checkNotNull = sbe::makeE<sbe::EPrimUnary>(
        sbe::EPrimUnary::logicNot,
        sbe::makeE<sbe::EFunction>("isNull", sbe::makeEs(branchRef.clone())));
    auto checkNotFalse = makeNeqCheck(sbe::makeE<sbe::EConstant>(
        sbe::value::TypeTags::Boolean, sbe::value::bitcastFrom<bool>(false)));
    auto checkNotZero = makeNeqCheck(sbe::makeE<sbe::EConstant>(
        sbe::value::TypeTags::NumberInt64, sbe::value::bitcastFrom<int64_t>(0)));

    return sbe::makeE<sbe::EPrimBinary>(
        sbe::EPrimBinary::logicAnd,
        std::move(checkExists),
        sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::logicAnd,
                                     std::move(checkNotNull),
                                     sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::logicAnd,
                                                                  std::move(checkNotFalse),
                                                                  std::move(checkNotZero))));
}

std::tuple<sbe::value::SlotId, std::unique_ptr<sbe::EExpression>, std::unique_ptr<sbe::PlanStage>>
generateExpression(OperationContext* opCtx,
                   Expression* expr,
                   std::unique_ptr<sbe::PlanStage> stage,
                   sbe::value::SlotIdGenerator* slotIdGenerator,
                   sbe::value::FrameIdGenerator* frameIdGenerator,
                   sbe::value::SlotId rootSlot,
                   sbe::RuntimeEnvironment* env,
                   PlanNodeId planNodeId,
                   sbe::value::SlotVector* relevantSlots) {
    auto tempRelevantSlots = sbe::makeSV(rootSlot);
    relevantSlots = relevantSlots ? relevantSlots : &tempRelevantSlots;

    ExpressionVisitorContext context(std::move(stage),
                                     slotIdGenerator,
                                     frameIdGenerator,
                                     rootSlot,
                                     relevantSlots,
                                     env,
                                     planNodeId);

    ExpressionPreVisitor preVisitor{&context};
    ExpressionInVisitor inVisitor{&context};
    ExpressionPostVisitor postVisitor{&context};
    ExpressionWalker walker{&preVisitor, &inVisitor, &postVisitor};
    expression_walker::walk(&walker, expr);
    return context.done();
}
}  // namespace mongo::stage_builder
