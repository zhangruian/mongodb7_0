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

#include "mongo/db/matcher/doc_validation_error.h"

#include <stack>

#include "mongo/base/init.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/matcher/doc_validation_util.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/match_expression_util.h"
#include "mongo/db/matcher/match_expression_walker.h"
#include "mongo/db/matcher/schema/expression_internal_schema_all_elem_match_from_index.h"
#include "mongo/db/matcher/schema/expression_internal_schema_cond.h"
#include "mongo/db/matcher/schema/expression_internal_schema_fmod.h"
#include "mongo/db/matcher/schema/expression_internal_schema_match_array_index.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_object_match.h"
#include "mongo/db/matcher/schema/expression_internal_schema_str_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_unique_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_xor.h"
#include "mongo/db/matcher/schema/json_schema_parser.h"

namespace mongo::doc_validation_error {
namespace {
MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(DocumentValidationFailureInfo);

using ErrorAnnotation = MatchExpression::ErrorAnnotation;
using AnnotationMode = ErrorAnnotation::Mode;
using LeafArrayBehavior = ElementPath::LeafArrayBehavior;
using NonLeafArrayBehavior = ElementPath::NonLeafArrayBehavior;

// Fail point which simulates an internal error for testing.
MONGO_FAIL_POINT_DEFINE(docValidationInternalErrorFailPoint);

/**
 * Enumerated type which describes whether an error should be described normally or in an
 * inverted sense when in a negated context. More precisely, when a MatchExpression fails to match a
 * document, the generated error will refer to failure unless the MatchExpression is nested
 * within another MatchExpression that expresses a logical negation, in which case the generated
 * error will refer to success.
 */
enum class InvertError { kNormal, kInverted };

/**
 * A struct which tracks error generation information for some node within the tree.
 */
struct ValidationErrorFrame {
    /**
     * Enumerated type which describes runtime information about a node participating in error
     * generation.
     */
    enum class RuntimeState {
        // This node contributes to error generation.
        kError,
        // Neither this node nor do any of its children contribute to error generation at all.
        kNoError,
        // This node contributes to error generation, but it needs more information about its child
        // nodes when generating an error. For instance, when generating an error for an AND in a
        // normal context, we need to discern which of its clauses failed.
        kErrorNeedChildrenInfo,
        // This node contributes to error generation, but none of its children will contribute to
        // the error output.
        kErrorIgnoreChildren,
    };

    ValidationErrorFrame(RuntimeState runtimeState, BSONObj currentDoc, InvertError inversion)
        : runtimeState(runtimeState), currentDoc(std::move(currentDoc)), inversion(inversion) {}

    // BSONBuilders which construct the generated error.
    BSONObjBuilder objBuilder;
    BSONArrayBuilder arrayBuilder;
    // Tracks the index of the current child expression.
    size_t childIndex = 0;
    // Tracks runtime information about how the current node should generate an error.
    RuntimeState runtimeState;
    // Tracks the current subdocument that an error should be generated over.
    BSONObj currentDoc;
    // Tracks whether the generated error should be described normally or in an inverted context.
    InvertError inversion;
    // Tracks whether the array of 'consideredValues' was truncated for this frame.
    bool consideredValuesTruncated = false;
};

using RuntimeState = ValidationErrorFrame::RuntimeState;

/**
 * A struct which tracks context during error generation.
 */
struct ValidationErrorContext {
    ValidationErrorContext(const BSONObj& rootDoc,
                           bool truncate,
                           const int maxDocValidationErrorSize,
                           const int maxConsideredValuesElements)
        : rootDoc(rootDoc),
          truncate(truncate),
          kMaxDocValidationErrorSize(maxDocValidationErrorSize),
          kMaxConsideredValuesElements(maxConsideredValuesElements) {
        invariant(kMaxConsideredValuesElements > 0);
        invariant(kMaxDocValidationErrorSize > 0);
    }

    /**
     * Utilities which add/remove ValidationErrorFrames from 'frames'.
     */
    void pushNewFrame(const MatchExpression& expr, const BSONObj& subDoc) {
        // Clear the last error that was generated.
        latestCompleteError = std::monostate();

        // If this is the first frame, then we know that we've failed validation, so we must be
        // generating an error.
        if (frames.empty()) {
            frames.emplace(RuntimeState::kError, subDoc, InvertError::kNormal);
            return;
        }

        auto parentRuntimeState = getCurrentRuntimeState();
        auto inversion = getCurrentInversion();

        // If we've determined at runtime or at parse time that this node shouldn't contribute to
        // error generation, then push a frame indicating that this node should not produce an
        // error and return.
        if (parentRuntimeState == RuntimeState::kNoError ||
            parentRuntimeState == RuntimeState::kErrorIgnoreChildren ||
            expr.getErrorAnnotation()->mode == AnnotationMode::kIgnore) {
            frames.emplace(RuntimeState::kNoError, subDoc, inversion);
            return;
        }

        // If our parent needs more information, call 'matches()' to determine whether the 'expr'
        // will contribute to error output.
        if (parentRuntimeState == RuntimeState::kErrorNeedChildrenInfo) {
            bool generateErrorValue = expr.matchesBSON(subDoc) ? inversion == InvertError::kInverted
                                                               : inversion == InvertError::kNormal;
            frames.emplace(generateErrorValue ? RuntimeState::kError : RuntimeState::kNoError,
                           subDoc,
                           inversion);
            return;
        }
        frames.emplace(RuntimeState::kError, subDoc, inversion);
    }
    void popFrame() {
        invariant(!frames.empty());
        frames.pop();
    }

    /**
     * Utilities which return members of the current ValidationContextFrame.
     */
    BSONObjBuilder& getCurrentObjBuilder() {
        invariant(!frames.empty());
        return frames.top().objBuilder;
    }
    BSONArrayBuilder& getCurrentArrayBuilder() {
        invariant(!frames.empty());
        return frames.top().arrayBuilder;
    }
    size_t getCurrentChildIndex() const {
        invariant(!frames.empty());
        return frames.top().childIndex;
    }
    void incrementCurrentChildIndex() {
        invariant(!frames.empty());
        ++frames.top().childIndex;
    }
    RuntimeState getCurrentRuntimeState() const {
        invariant(!frames.empty());
        return frames.top().runtimeState;
    }
    void setCurrentRuntimeState(RuntimeState runtimeState) {
        invariant(!frames.empty());

        // If a node has RuntimeState::kNoError, then its runtime state value should never be
        // modified since the node should never contribute to error generation.
        if (getCurrentRuntimeState() != RuntimeState::kNoError) {
            frames.top().runtimeState = runtimeState;
        }
    }
    const BSONObj& getCurrentDocument() {
        if (!frames.empty()) {
            return frames.top().currentDoc;
        }
        return rootDoc;
    }
    void setCurrentDocument(const BSONObj& document) {
        invariant(!frames.empty());
        frames.top().currentDoc = document;
    }
    InvertError getCurrentInversion() const {
        invariant(!frames.empty());
        return frames.top().inversion;
    }
    void setCurrentInversion(InvertError inversion) {
        invariant(!frames.empty());
        frames.top().inversion = inversion;
    }

    /**
     * Verify that the size of 'builder' combined with that of 'item' are of valid size before
     * appending the latter to the former; throws a BSONObjectTooLarge error otherwise.
     */
    template <class ItemType, class BuilderType>
    void verifySize(const ItemType& item, const BuilderType& builder) {
        uassert(ErrorCodes::BSONObjectTooLarge,
                "doc validation error builder exceeded maximum size",
                builder.len() + item.objsize() <= kMaxDocValidationErrorSize);
    }

    template <class BuilderType>
    void verifySize(const BSONElement& item, const BuilderType& builder) {
        uassert(ErrorCodes::BSONObjectTooLarge,
                "doc validation error builder exceeded maximum size",
                builder.len() + item.size() <= kMaxDocValidationErrorSize);
    }

    template <class ItemType, class BuilderType>
    void verifySizeAndAppend(const ItemType& item,
                             const std::string& fieldName,
                             BuilderType* builder) {
        verifySize(item, *builder);
        builder->append(fieldName, item);
    }

    template <class ItemType>
    void verifySizeAndAppend(const ItemType& item, BSONArrayBuilder* builder) {
        verifySize(item, *builder);
        builder->append(item);
    }

    template <class BuilderType>
    void verifySizeAndAppendAs(const BSONElement& item,
                               const std::string& fieldName,
                               BuilderType* builder) {
        verifySize(item, *builder);
        builder->appendAs(item, fieldName);
    }

    bool haveLatestCompleteError() {
        return !stdx::holds_alternative<std::monostate>(latestCompleteError);
    }

    /**
     * Appends the latest complete error to 'builder'.
     */
    void appendLatestCompleteError(BSONObjBuilder* builder) {
        const static std::string kDetailsString = "details";
        stdx::visit(
            visit_helper::Overloaded{
                [&](const auto& details) -> void {
                    verifySizeAndAppend(details, kDetailsString, builder);
                },
                [&](const std::monostate& state) -> void { MONGO_UNREACHABLE; },
                [&](const std::string& str) -> void { MONGO_UNREACHABLE; }},
            latestCompleteError);
    }
    /**
     * Appends the latest complete error to 'builder'. This should only be called by nodes which
     * construct an array as part of their error.
     */
    void appendLatestCompleteError(BSONArrayBuilder* builder) {
        stdx::visit(
            visit_helper::Overloaded{[&](const BSONObj& obj) -> void { builder->append(obj); },
                                     [&](const std::string& str) -> void { builder->append(str); },
                                     [&](const BSONArray& arr) -> void { MONGO_UNREACHABLE; },
                                     [&](const std::monostate& arr) -> void { MONGO_UNREACHABLE; }},
            latestCompleteError);
    }

    /**
     * Returns the latest complete error generated as an object. Should only be called when the
     * caller expects an object.
     */
    BSONObj getLatestCompleteErrorObject() const {
        return stdx::get<BSONObj>(latestCompleteError);
    }

    BSONArray getLatestCompleteErrorArray() const {
        return stdx::get<BSONArray>(latestCompleteError);
    }

    /**
     * Returns whether 'expr' will produce an array as an error.
     */
    bool producesArray(const MatchExpression& expr) {
        auto& tag = expr.getErrorAnnotation()->tag;
        return tag == "_subschema" || tag == "_propertiesExistList";
    }
    bool isConsideredValuesTruncated() const {
        invariant(!frames.empty());
        return frames.top().consideredValuesTruncated;
    }
    void markConsideredValuesAsTruncated() {
        invariant(!frames.empty());
        frames.top().consideredValuesTruncated = true;
    }

    /**
     * Finishes error for 'expr' by stashing its generated error if it made one and popping the
     * frame that it created.
     */
    void finishCurrentError(const MatchExpression* expr) {
        if (shouldGenerateError(*expr)) {
            if (producesArray(*expr)) {
                latestCompleteError = getCurrentArrayBuilder().arr();
            } else {
                latestCompleteError = getCurrentObjBuilder().obj();
            }
        }
        popFrame();
    }

    /**
     * Sets 'inversion' to the opposite of its current value.
     */
    void flipInversion() {
        getCurrentInversion() == InvertError::kNormal ? setCurrentInversion(InvertError::kInverted)
                                                      : setCurrentInversion(InvertError::kNormal);
    }

    /**
     * Returns whether 'expr' should generate an error.
     */
    bool shouldGenerateError(const MatchExpression& expr) {
        return expr.getErrorAnnotation()->mode == AnnotationMode::kGenerateError &&
            getCurrentRuntimeState() != RuntimeState::kNoError;
    }

    // Frames which construct the generated error. Each frame corresponds to the information needed
    // to generate an error for one node. As such, each node must call 'pushNewFrame' as part of
    // its pre-visit and 'popFrame' as part of its post-visit.
    std::stack<ValidationErrorFrame> frames;
    // Tracks the most recently completed error. The error can be one of several types:
    // - std::monostate indicates that no error was produced.
    // - Nodes can return their error as a std::string if they do not need to generate error
    // details as a structured BSONObj. For example, consider the schema {required: [a,b,c]}. Each
    // property in the 'required' array is represented as its own ExistsMatchExpression and will
    // simply report its path if it is missing from the document which failed to match.
    // - BSONArray indicates multiple errors produced by an expression which does not correspond
    // to a user-facing operator. For example, consider the subschema {minimum: 2, multipleOf: 2}.
    // Both schema operators can fail and produce errors, but the schema that they belong to
    // doesn't correspond to an operator that the user specified. As such, the errors are stored
    // in an array and passed to the parent expression.
    // - Finally, BSONObj indicates the most common case of an error: a detailed object which
    // describes the reasons for failure. The final error will be of this type.
    stdx::variant<std::monostate, std::string, BSONObj, BSONArray> latestCompleteError =
        std::monostate();
    // Document which failed to match against the collection's validator.
    const BSONObj& rootDoc;
    // Tracks whether the generated error should omit appending 'specifiedAs' and
    // 'consideredValues' to avoid generating an error larger than the maximum BSONObj size.
    const bool truncate = false;
    // The maximum allowed size for a doc validation error.
    const int kMaxDocValidationErrorSize;
    // Tracks the maximum number of values that will be reported in the 'consideredValues' array
    // for leaf operators.
    const int kMaxConsideredValuesElements;
};

/**
 * Append the error generated by one of 'expr's children to the current array builder of 'expr'
 * if said child generated an error.
 */
void finishLogicalOperatorChildError(const ListOfMatchExpression* expr,
                                     ValidationErrorContext* ctx) {
    if (ctx->shouldGenerateError(*expr) &&
        ctx->getCurrentRuntimeState() != RuntimeState::kErrorIgnoreChildren) {
        auto tag = expr->getErrorAnnotation()->tag;
        // Only provide the indexes of non-matching clauses for certain named operators in the
        // user's query.
        static const stdx::unordered_set<std::string> operatorsWithOrderedClauses = {
            "$and", "$or", "$nor", "allOf", "anyOf", "oneOf"};
        if (ctx->haveLatestCompleteError()) {
            if (operatorsWithOrderedClauses.find(tag) != operatorsWithOrderedClauses.end()) {
                BSONObjBuilder subBuilder = ctx->getCurrentArrayBuilder().subobjStart();
                subBuilder.appendNumber("index", ctx->getCurrentChildIndex());
                ctx->appendLatestCompleteError(&subBuilder);
                subBuilder.done();
            } else {
                ctx->appendLatestCompleteError(&ctx->getCurrentArrayBuilder());
            }
        }
    }
    ctx->incrementCurrentChildIndex();
}

/**
 * Enumerated type to encode JSON Schema array keyword "items" and "additionalItems", and their
 * variants.
 */
enum class ItemsKeywordType {
    kItems,                  // 'items': {schema}
    kAdditionalItemsFalse,   // 'additionalItems': false
    kAdditionalItemsSchema,  // 'additionalItems': {schema}
};

/**
 * Decodes the JSON Schema "items"/"additionalItems" keyword type from an error annotation of
 * expression 'expr'.
 */
ItemsKeywordType toItemsKeywordType(
    const InternalSchemaAllElemMatchFromIndexMatchExpression& expr) {
    auto* errorAnnotation = expr.getErrorAnnotation();
    if ("items" == errorAnnotation->tag) {
        return ItemsKeywordType::kItems;
    }
    if ("additionalItems" == errorAnnotation->tag) {
        switch (errorAnnotation->annotation.firstElementType()) {
            case BSONType::Bool:
                return ItemsKeywordType::kAdditionalItemsFalse;
            case BSONType::Object:
                return ItemsKeywordType::kAdditionalItemsSchema;
            default:
                MONGO_UNREACHABLE;
        }
    }
    MONGO_UNREACHABLE;
}

/**
 * Visitor which is primarily responsible for error generation.
 */
class ValidationErrorPreVisitor final : public MatchExpressionConstVisitor {
public:
    ValidationErrorPreVisitor(ValidationErrorContext* context) : _context(context) {}
    void visit(const AlwaysFalseMatchExpression* expr) final {
        generateAlwaysBooleanError(*expr);
    }
    void visit(const AlwaysTrueMatchExpression* expr) final {
        generateAlwaysBooleanError(*expr);
    }
    void visit(const AndMatchExpression* expr) final {
        auto&& tag = expr->getErrorAnnotation()->tag;
        // $all is treated as a leaf operator.
        if (tag == "$all") {
            static constexpr auto kNormalReason = "array did not contain all specified values";
            static constexpr auto kInvertedReason = "array did contain all specified values";
            generateLogicalLeafError(*expr, kNormalReason, kInvertedReason);
        } else if (tag == "items") {
            // $and only gets annotated as an "items" only for JSON Schema keyword "items" set to an
            // array of subschemas.
            generateJSONSchemaItemsSchemaArrayError(*expr);
        } else {
            preVisitTreeOperator(expr);
            // An AND needs its children to call 'matches' in a normal context to discern which
            // clauses failed.
            if (_context->getCurrentInversion() == InvertError::kNormal) {
                _context->setCurrentRuntimeState(RuntimeState::kErrorNeedChildrenInfo);
            }
            // If this is the root of a $jsonSchema and we're in an inverted context, do not attempt
            // to provide a detailed error.
            if (tag == "$jsonSchema" && _context->getCurrentInversion() == InvertError::kInverted) {
                _context->setCurrentRuntimeState(RuntimeState::kErrorIgnoreChildren);
                static constexpr auto kInvertedReason = "schema matched";
                appendErrorReason("", kInvertedReason);
            }
        }
    }
    void visit(const BitsAllClearMatchExpression* expr) final {
        generateError(expr);
    }
    void visit(const BitsAllSetMatchExpression* expr) final {
        generateError(expr);
    }
    void visit(const BitsAnyClearMatchExpression* expr) final {
        generateError(expr);
    }
    void visit(const BitsAnySetMatchExpression* expr) final {
        generateError(expr);
    }
    void visit(const ElemMatchObjectMatchExpression* expr) final {
        generateElemMatchError(expr);
    }
    void visit(const ElemMatchValueMatchExpression* expr) final {
        generateElemMatchError(expr);
    }
    void visit(const EqualityMatchExpression* expr) final {
        generateComparisonError(expr);
    }
    void visit(const ExistsMatchExpression* expr) final {
        static constexpr auto kNormalReason = "path does not exist";
        static constexpr auto kInvertedReason = "path does exist";
        _context->pushNewFrame(*expr, _context->getCurrentDocument());
        // Only generate an error if this node is tagged with an MQL operator name. The
        // '_propertyExists' tag indicates that this node is implementing a JSONSchema feature.
        if (_context->shouldGenerateError(*expr) &&
            expr->getErrorAnnotation()->tag != "_propertyExists") {
            appendErrorDetails(*expr);
            appendErrorReason(kNormalReason, kInvertedReason);
        }
    }
    void visit(const ExprMatchExpression* expr) final {
        static constexpr auto kNormalReason = "$expr did not match";
        static constexpr auto kInvertedReason = "$expr did match";
        _context->pushNewFrame(*expr, _context->getCurrentDocument());
        if (_context->shouldGenerateError(*expr)) {
            appendErrorDetails(*expr);
            appendErrorReason(kNormalReason, kInvertedReason);
            BSONObjBuilder& bob = _context->getCurrentObjBuilder();
            // Append the result of $expr's aggregate expression. The result of the
            // aggregate expression can be determined from the current inversion.
            bob.append("expressionResult",
                       _context->getCurrentInversion() == InvertError::kInverted);
        }
    }
    void visit(const GTEMatchExpression* expr) final {
        generateComparisonError(expr);
    }
    void visit(const GTMatchExpression* expr) final {
        generateComparisonError(expr);
    }
    void visit(const GeoMatchExpression* expr) final {
        static const std::set<BSONType> kExpectedTypes{BSONType::Array, BSONType::Object};
        switch (expr->getGeoExpression().getPred()) {
            case GeoExpression::Predicate::WITHIN: {
                static constexpr auto kNormalReason =
                    "none of considered geometries was contained within the expression’s geometry";
                static constexpr auto kInvertedReason =
                    "at least one of considered geometries was contained within the expression’s "
                    "geometry";
                generatePathError(*expr, kNormalReason, kInvertedReason, &kExpectedTypes);
            } break;
            case GeoExpression::Predicate::INTERSECT: {
                static constexpr auto kNormalReason =
                    "none of considered geometries intersected the expression’s geometry";
                static constexpr auto kInvertedReason =
                    "at least one of considered geometries intersected the expression’s geometry";
                generatePathError(*expr, kNormalReason, kInvertedReason, &kExpectedTypes);
            } break;
            default:
                MONGO_UNREACHABLE;
        }
    }
    void visit(const GeoNearMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }
    void visit(const InMatchExpression* expr) final {
        static constexpr auto kNormalReason = "no matching value found in array";
        static constexpr auto kInvertedReason = "matching value found in array";
        generatePathError(*expr, kNormalReason, kInvertedReason);
    }
    void visit(const InternalExprEqMatchExpression* expr) final {}
    void visit(const InternalSchemaAllElemMatchFromIndexMatchExpression* expr) final {
        switch (toItemsKeywordType(*expr)) {
            case ItemsKeywordType::kItems: {
                static constexpr auto kNormalReason =
                    "At least one item did not match the sub-schema";
                generateJSONSchemaArraySingleSchemaError(expr, kNormalReason, "");
            } break;
            case ItemsKeywordType::kAdditionalItemsSchema: {
                static constexpr auto kNormalReason =
                    "At least one additional item did not match the sub-schema";
                generateJSONSchemaArraySingleSchemaError(expr, kNormalReason, "");
            } break;
            case ItemsKeywordType::kAdditionalItemsFalse:
                generateJSONSchemaAdditionalItemsFalseError(expr);
                break;
            default:
                MONGO_UNREACHABLE;
        }
    }
    void visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaBinDataEncryptedTypeExpression* expr) final {
        static constexpr auto kNormalReason = "encrypted value has wrong type";
        // This node will never generate an error in the inverted case.
        static constexpr auto kInvertedReason = "";
        _context->pushNewFrame(*expr, _context->getCurrentDocument());
        if (_context->shouldGenerateError(*expr)) {
            ElementPath path(expr->path(), LeafArrayBehavior::kNoTraversal);
            BSONMatchableDocument doc(_context->getCurrentDocument());
            MatchableDocument::IteratorHolder cursor(&doc, &path);
            invariant(cursor->more());
            auto elem = cursor->next().element();
            // Only generate an error in the normal case since if the value exists and it is
            // encrypted, in the inverted case, this node's sibling expression will generate an
            // appropriate error.
            if (elem.type() == BSONType::BinData && elem.binDataType() == BinDataType::Encrypt &&
                _context->getCurrentInversion() == InvertError::kNormal) {
                appendOperatorName(*expr);
                appendErrorReason(kNormalReason, kInvertedReason);
            } else {
                _context->setCurrentRuntimeState(RuntimeState::kNoError);
            }
        }
    }
    void visit(const InternalSchemaBinDataSubTypeExpression* expr) final {
        static constexpr auto kNormalReason = "value was not encrypted";
        static constexpr auto kInvertedReason = "value was encrypted";
        _context->pushNewFrame(*expr, _context->getCurrentDocument());
        if (_context->shouldGenerateError(*expr)) {
            appendOperatorName(*expr);
            appendErrorReason(kNormalReason, kInvertedReason);
        }
    }
    void visit(const InternalSchemaCondMatchExpression* expr) final {
        _context->pushNewFrame(*expr, _context->getCurrentDocument());
        if (_context->shouldGenerateError(*expr)) {
            // Since 'expr' represents a conditional expression corresponding to a single
            // $jsonSchema dependency whose else branch always evaluates to 'true', 'expr' can only
            // fail if its 'condition' expression evaluates to true and its then branch evaluates to
            // false. Therefore, if 'condition' evaluates to false, we conclude that this node will
            // not contribute to error generation.
            if (!expr->condition()->matchesBSON(_context->getCurrentDocument())) {
                _context->setCurrentRuntimeState(RuntimeState::kNoError);
            }
        }
    }
    void visit(const InternalSchemaEqMatchExpression* expr) final {}
    void visit(const InternalSchemaFmodMatchExpression* expr) final {
        static constexpr auto kNormalReason =
            "considered value is not a multiple of the specified value";
        static constexpr auto kInvertedReason =
            "considered value is a multiple of the specified value";
        static const std::set<BSONType> kExpectedTypes{BSONType::NumberLong,
                                                       BSONType::NumberDouble,
                                                       BSONType::NumberDecimal,
                                                       BSONType::NumberInt};
        generatePathError(*expr,
                          kNormalReason,
                          kInvertedReason,
                          &kExpectedTypes,
                          LeafArrayBehavior::kNoTraversal);
    }
    void visit(const InternalSchemaMatchArrayIndexMatchExpression* expr) final {
        _context->pushNewFrame(*expr, _context->getCurrentDocument());
        if (_context->shouldGenerateError(*expr)) {
            // Get an element of an array.
            ElementPath path(
                expr->path(), LeafArrayBehavior::kNoTraversal, NonLeafArrayBehavior::kNoTraversal);
            auto attributeValue = getValueAt(path);

            // Attribute should be present and be an array, since it has been ensured by handling of
            // AndMatchExpression with error annotation "items".
            invariant(attributeValue.type() == BSONType::Array);
            auto valueAsArray = BSONArray(attributeValue.embeddedObject());

            // If array is shorter than the index the match expression applies to, then document
            // validation should not fail.
            invariant(expr->arrayIndex() < valueAsArray.nFields());

            // Append information about array element to the error.
            BSONElement arrayElement = valueAsArray[expr->arrayIndex()];
            BSONObjBuilder& bob = _context->getCurrentObjBuilder();
            bob.append("itemIndex"_sd, expr->arrayIndex());

            // Build a document corresponding to the array element for the child expression to
            // operate on.
            _context->setCurrentDocument(toObjectWithPlaceholder(arrayElement));
        }
    }
    void visit(const InternalSchemaMaxItemsMatchExpression* expr) final {
        generateJSONSchemaMinItemsMaxItemsError(expr);
    }
    void visit(const InternalSchemaMaxLengthMatchExpression* expr) final {
        generateStringLengthError(*expr);
    }
    void visit(const InternalSchemaMaxPropertiesMatchExpression* expr) final {
        generateNumPropertiesError(*expr);
    }
    void visit(const InternalSchemaMinItemsMatchExpression* expr) final {
        generateJSONSchemaMinItemsMaxItemsError(expr);
    }
    void visit(const InternalSchemaMinLengthMatchExpression* expr) final {
        generateStringLengthError(*expr);
    }
    void visit(const InternalSchemaMinPropertiesMatchExpression* expr) final {
        generateNumPropertiesError(*expr);
    }
    void visit(const InternalSchemaObjectMatchExpression* expr) final {
        // This node should never be responsible for generating an error directly.
        invariant(expr->getErrorAnnotation()->mode != AnnotationMode::kGenerateError);
        // As part of pushing a new frame onto the stack, the runtime state may be set to
        // 'kNoError' if 'expr' matches the current document.
        _context->pushNewFrame(*expr, _context->getCurrentDocument());
        // Only attempt to find a subdocument if this node failed to match.
        if (_context->getCurrentRuntimeState() != RuntimeState::kNoError) {
            ElementPath path(expr->path(), LeafArrayBehavior::kNoTraversal);
            BSONMatchableDocument doc(_context->getCurrentDocument());
            MatchableDocument::IteratorHolder cursor(&doc, &path);
            invariant(cursor->more());
            auto elem = cursor->next().element();

            // If we do not find an object at expr's path, then the subtree rooted at this node will
            // not contribute to error generation as there will either be an explicit
            // ExistsMatchExpression which will explain a missing path error or an explicit
            // InternalSchemaTypeExpression that will explain a type did not match error.
            bool ignoreSubTree = false;
            if (elem.type() == BSONType::Object) {
                _context->setCurrentDocument(elem.embeddedObject());
            } else {
                ignoreSubTree = true;
            }

            // This expression should match exactly one object; if there are any more elements, then
            // ignore the subtree.
            if (cursor->more()) {
                ignoreSubTree = true;
            }
            if (ignoreSubTree) {
                _context->setCurrentRuntimeState(RuntimeState::kNoError);
            }
        }
    }
    void visit(const InternalSchemaRootDocEqMatchExpression* expr) final {}
    void visit(const InternalSchemaTypeExpression* expr) final {
        generateTypeError(expr, LeafArrayBehavior::kNoTraversal);
    }
    void visit(const InternalSchemaUniqueItemsMatchExpression* expr) final {
        static constexpr auto normalReason = "found a duplicate item";
        _context->pushNewFrame(*expr, _context->getCurrentDocument());
        if (auto attributeValue = getValueForArrayKeywordExpressionIfShouldGenerateError(*expr)) {
            appendErrorDetails(*expr);
            appendErrorReason(normalReason, "");
            auto attributeValueAsArray = BSONArray(attributeValue.embeddedObject());
            appendConsideredValue(attributeValueAsArray);
            auto duplicateValue = expr->findFirstDuplicateValue(attributeValueAsArray);
            invariant(duplicateValue);
            _context->verifySizeAndAppendAs(
                duplicateValue, "duplicatedValue", &_context->getCurrentObjBuilder());
        } else {
            _context->setCurrentRuntimeState(RuntimeState::kNoError);
        }
    }
    void visit(const InternalSchemaXorMatchExpression* expr) final {
        preVisitTreeOperator(expr);
        _context->setCurrentRuntimeState(RuntimeState::kErrorNeedChildrenInfo);
        if (_context->shouldGenerateError(*expr)) {
            auto currentDoc = _context->getCurrentDocument();

            // If 'oneOf' has more than one matching subschema, then the generated error should
            // be in terms of the subschemas which matched, not the ones which failed to match.
            std::vector<int> matchingClauses;
            for (size_t childIndex = 0; childIndex < expr->numChildren(); ++childIndex) {
                auto child = expr->getChild(childIndex);
                if (child->matchesBSON(currentDoc)) {
                    matchingClauses.push_back(childIndex);
                }
            }
            if (!matchingClauses.empty()) {
                _context->flipInversion();
                _context->setCurrentRuntimeState(RuntimeState::kErrorIgnoreChildren);
                auto& builder = _context->getCurrentObjBuilder();
                // We only report the matching schema reason in an inverted context, so there is
                // no need for a reason string in the normal case.
                static constexpr auto kNormalReason = "";
                static constexpr auto kInvertedReason = "more than one subschema matched";
                appendErrorReason(kNormalReason, kInvertedReason);
                builder.append("matchingSchemaIndexes", matchingClauses);
            }
        }
    }
    void visit(const LTEMatchExpression* expr) final {
        generateComparisonError(expr);
    }
    void visit(const LTMatchExpression* expr) final {
        generateComparisonError(expr);
    }
    void visit(const ModMatchExpression* expr) final {
        static constexpr auto kNormalReason = "$mod did not evaluate to expected remainder";
        static constexpr auto kInvertedReason = "$mod did evaluate to expected remainder";
        static const std::set<BSONType> kExpectedTypes{BSONType::NumberLong,
                                                       BSONType::NumberDouble,
                                                       BSONType::NumberDecimal,
                                                       BSONType::NumberInt};
        generatePathError(*expr, kNormalReason, kInvertedReason, &kExpectedTypes);
    }
    void visit(const NorMatchExpression* expr) final {
        preVisitTreeOperator(expr);
        // A NOR needs its children to call 'matches' in a normal context to discern which
        // clauses matched.
        if (_context->getCurrentInversion() == InvertError::kNormal) {
            _context->setCurrentRuntimeState(RuntimeState::kErrorNeedChildrenInfo);
        }
        _context->flipInversion();
    }
    void visit(const NotMatchExpression* expr) final {
        preVisitTreeOperator(expr);
        _context->flipInversion();
        // If this is a $jsonSchema not, then expr's children will not contribute to the error
        // output.
        if (_context->shouldGenerateError(*expr) && expr->getErrorAnnotation()->tag == "not") {
            static constexpr auto kInvertedReason = "child expression matched";
            appendErrorReason("", kInvertedReason);
            _context->setCurrentRuntimeState(RuntimeState::kErrorIgnoreChildren);
        }
    }
    void visit(const OrMatchExpression* expr) final {
        // The jsonSchema keyword 'enum' is treated as a leaf operator.
        if (expr->getErrorAnnotation()->tag == "enum") {
            static constexpr auto kNormalReason = "value was not found in enum";
            static constexpr auto kInvertedReason = "value was found in enum";
            generateLogicalLeafError(*expr, kNormalReason, kInvertedReason);
        } else {
            preVisitTreeOperator(expr);
            // An OR needs its children to call 'matches' in an inverted context to discern which
            // clauses matched.
            if (_context->getCurrentInversion() == InvertError::kInverted) {
                _context->setCurrentRuntimeState(RuntimeState::kErrorNeedChildrenInfo);
            }
        }
    }
    void visit(const RegexMatchExpression* expr) final {
        static constexpr auto kNormalReason = "regular expression did not match";
        static constexpr auto kInvertedReason = "regular expression did match";
        static const std::set<BSONType> kExpectedTypes{
            BSONType::String, BSONType::Symbol, BSONType::RegEx};
        generatePathError(*expr, kNormalReason, kInvertedReason, &kExpectedTypes);
    }
    void visit(const SizeMatchExpression* expr) final {
        static constexpr auto kNormalReason = "array length was not equal to given size";
        static constexpr auto kInvertedReason = "array length was equal to given size";
        generateArrayError(expr, kNormalReason, kInvertedReason);
    }
    void visit(const TextMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }
    void visit(const TextNoOpMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }
    void visit(const TwoDPtInAnnulusExpression* expr) final {}
    void visit(const TypeMatchExpression* expr) final {
        // Although $type predicate can match an array field, we are only interested in implicitly
        // traversed array elements as considered values since, when we have predicate "{$type:
        // 'array'}" and a field is an array, that is a match. Therefore we use
        // LeafArrayBehavior::kTraverseOmitArray as the traversal behavior.
        generateTypeError(expr, LeafArrayBehavior::kTraverseOmitArray);
    }
    void visit(const WhereMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }
    void visit(const WhereNoOpMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }

private:
    // Set of utilities responsible for appending various fields to build a descriptive error.
    void appendOperatorName(const MatchExpression& expr) {
        auto tag = expr.getErrorAnnotation()->tag;
        // Only append the operator name if 'annotation' has one.
        if (!tag.empty()) {
            // An underscore-prefixed tag describes an internal entity, not an MQL operator.
            invariant(tag[0] != '_');
            _context->getCurrentObjBuilder().append("operatorName", tag);
        }
    }
    void appendSpecifiedAs(const ErrorAnnotation& annotation, BSONObjBuilder* bob) {
        // Omit 'specifiedAs' if we are generating a truncated error.
        if (_context->truncate) {
            return;
        }
        // Since this function can append values that are proportional to the size of the
        // original validator expression, verify that the current builders do not exceed the
        // maximum allowed validation error size.
        _context->verifySizeAndAppend(annotation.annotation, "specifiedAs", bob);
    }
    void appendErrorDetails(const MatchExpression& expr) {
        auto annotation = expr.getErrorAnnotation();
        BSONObjBuilder& bob = _context->getCurrentObjBuilder();
        appendOperatorName(expr);
        appendSpecifiedAs(*annotation, &bob);
    }

    /**
     * Returns an enumeration of values of a field at path 'fieldPath' in the current document as an
     * array if the path is present. A return value of empty array means that the path was present,
     * but the value associated with that path was the empty array. If the path is not present, then
     * returns 'boost::none'. 'leafArrayBehavior' determines how the values are enumerated when the
     * leaf value of the path is an array.
     */
    boost::optional<BSONArray> createValuesArray(const StringData fieldPath,
                                                 LeafArrayBehavior leafArrayBehavior) {
        // Empty path means that the match is against the root document.
        if (fieldPath.empty())
            return BSON_ARRAY(_context->rootDoc);
        BSONMatchableDocument doc(_context->getCurrentDocument());
        ElementPath path{fieldPath, leafArrayBehavior};
        MatchableDocument::IteratorHolder valueIterator(&doc, &path);
        BSONArrayBuilder bab;
        auto maxConsideredElements = _context->kMaxConsideredValuesElements;
        while (valueIterator->more() && bab.arrSize() < maxConsideredElements) {
            auto elem = valueIterator->next().element();
            if (elem.eoo()) {
                break;
            } else {
                bab.append(elem);
            }
        }

        // Indicate that 'consideredValues' has been truncated if there are non eoo elements left
        // in 'valueIterator'.
        if (valueIterator->more() && bab.arrSize() == maxConsideredElements) {
            auto elem = valueIterator->next().element();
            if (!elem.eoo()) {
                _context->markConsideredValuesAsTruncated();
            }
        }

        // When the iterator 'valueIterator' returns no values, there are two possible cases: either
        // the path does not exist, or the path exists and contains an empty array. In this case we
        // perform a check for field existence to disambiguate those two cases.
        if (bab.arrSize() == 0 && !pathExists(fieldPath)) {
            return boost::none;
        }
        return bab.arr();
    }

    /**
     * Returns a value at path 'path' in the current document, or an empty (End-Of-Object type)
     * element if the value is not present. Illegal to call if, due to implicit array traversal,
     * 'path' would result in multiple elements.
     */
    BSONElement getValueAt(const ElementPath& path) {
        BSONMatchableDocument doc(_context->getCurrentDocument());
        MatchableDocument::IteratorHolder cursor(&doc, &path);
        if (cursor->more()) {
            auto element = cursor->next().element();
            invariant(!cursor->more());  // We expect only 1 item.
            return element;
        } else {
            return {};
        }
    }

    /**
     * Appends a missing field error if 'arr' does not contain a value.
     */
    void appendMissingField(const boost::optional<BSONArray>& arr) {
        BSONObjBuilder& bob = _context->getCurrentObjBuilder();
        if (!arr) {
            bob.append("reason", "field was missing");
        }
    }

    /**
     * Appends a type mismatch error if no elements in 'arr' have one of the expected types.
     */
    void appendTypeMismatch(const boost::optional<BSONArray>& arr,
                            const std::set<BSONType>* expectedTypes) {
        if (!arr) {
            return;  // The field is not present.
        }
        BSONObjBuilder& bob = _context->getCurrentObjBuilder();
        if (bob.hasField("reason")) {
            return;  // there's already a reason for failure
        }
        if (!expectedTypes) {
            return;  // this operator accepts all types
        }
        for (auto&& elem : *arr) {
            if (expectedTypes->count(elem.type())) {
                return;  // an element has one of the expected types
            }
        }
        bob.append("reason", "type did not match");
        appendConsideredTypes(arr);
        std::set<std::string> types;
        for (auto&& elem : *expectedTypes) {
            types.insert(typeName(elem));
        }
        if (types.size() == 1) {
            bob.append("expectedType", *types.begin());
        } else {
            bob.append("expectedTypes", types);
        }
    }

    /**
     * Given 'normalReason' and 'invertedReason' strings, appends the reason for failure to the
     * current object builder tracked by 'ctx'.
     */
    void appendErrorReason(const std::string& normalReason, const std::string& invertedReason) {
        if (normalReason.empty()) {
            invariant(_context->getCurrentInversion() == InvertError::kInverted);
        } else if (invertedReason.empty()) {
            invariant(_context->getCurrentInversion() == InvertError::kNormal);
        }
        BSONObjBuilder& bob = _context->getCurrentObjBuilder();
        if (bob.hasField("reason")) {
            return;  // there's already a reason for failure
        }
        if (_context->getCurrentInversion() == InvertError::kNormal) {
            bob.append("reason", normalReason);
        } else {
            bob.append("reason", invertedReason);
        }
    }
    void appendConsideredValue(const BSONArray& array) {
        _context->verifySizeAndAppend(array, "consideredValue", &_context->getCurrentObjBuilder());
    }

    /**
     * Appends values of 'arr' array to the current object builder if 'arr' contains a value.
     */
    void appendConsideredValues(const boost::optional<BSONArray>& arr) {
        // Return if there is no field or if we are generating a truncated error.
        if (!arr || _context->truncate) {
            return;
        }
        auto arraySize = arr->nFields();
        BSONObjBuilder& bob = _context->getCurrentObjBuilder();
        if (arraySize == 1) {
            _context->verifySizeAndAppendAs((*arr)[0], "consideredValue", &bob);
        } else {
            _context->verifySizeAndAppend(*arr, "consideredValues", &bob);
        }

        if (_context->isConsideredValuesTruncated()) {
            bob.append("consideredValuesTruncated", true);
        }
    }

    /**
     * Appends types of values of 'arr' array to the current object builder if 'arr' contains a
     * value.
     */
    void appendConsideredTypes(const boost::optional<BSONArray>& arr) {
        if (!arr || arr->isEmpty()) {
            return;  // The field is not present or the array is empty.
        }
        BSONObjBuilder& bob = _context->getCurrentObjBuilder();
        std::set<std::string> types;
        for (auto&& elem : *arr) {
            types.insert(typeName(elem.type()));
        }
        if (types.size() == 1) {
            bob.append("consideredType", *types.begin());
        } else {
            bob.append("consideredTypes", types);
        }
    }

    /**
     * Returns 'true' if a field exists at path 'fieldPath' in the current document.
     */
    bool pathExists(StringData fieldPath) {
        ElementPath path(fieldPath,
                         LeafArrayBehavior::kTraverse);  // Use kTraverse to return at least one
                                                         // item if the field exists.
        BSONMatchableDocument doc(_context->getCurrentDocument());
        MatchableDocument::IteratorHolder valueIterator(&doc, &path);
        return valueIterator->more() && valueIterator->next().element();
    }

    /**
     * Given a pointer to a PathMatchExpression 'expr', appends details to the current
     * BSONObjBuilder tracked by '_context' describing why the document failed to match against
     * 'expr'. In particular:
     * - Appends "reason: field was missing" if expr's path is missing from the document.
     * - Appends "reason: type did not match" along with 'expectedTypes' and 'consideredTypes' if
     * none of the values at expr's path match any of the types specified in 'expectedTypes'.
     * - Appends the specified 'reason' along with 'consideredValue' if the 'path' in the
     * document resolves to a single value.
     * - Appends the specified 'reason' along with 'consideredValues' if the 'path' in the
     * document resolves to an array of values that is implicitly traversed by 'expr'.
     */
    void generatePathError(
        const PathMatchExpression& expr,
        const std::string& normalReason,
        const std::string& invertedReason,
        const std::set<BSONType>* expectedTypes = nullptr,
        LeafArrayBehavior leafArrayBehavior = LeafArrayBehavior::kTraverseOmitArray) {
        _context->pushNewFrame(expr, _context->getCurrentDocument());
        if (_context->shouldGenerateError(expr)) {
            appendErrorDetails(expr);
            auto arr = createValuesArray(expr.path(), leafArrayBehavior);
            appendMissingField(arr);
            appendTypeMismatch(arr, expectedTypes);
            appendErrorReason(normalReason, invertedReason);
            appendConsideredValues(arr);
        }
    }

    void generateComparisonError(const ComparisonMatchExpression* expr) {
        static constexpr auto kNormalReason = "comparison failed";
        static constexpr auto kInvertedReason = "comparison succeeded";
        generatePathError(*expr, kNormalReason, kInvertedReason);
    }

    void generateElemMatchError(const ArrayMatchingMatchExpression* expr) {
        static constexpr auto kNormalReason = "array did not satisfy the child predicate";
        static constexpr auto kInvertedReason = "array did satisfy the child predicate";
        generateArrayError(expr, kNormalReason, kInvertedReason);
    }

    void generateArrayError(const ArrayMatchingMatchExpression* expr,
                            const std::string& normalReason,
                            const std::string& invertedReason) {
        static const std::set<BSONType> expectedTypes{BSONType::Array};
        generatePathError(
            *expr, normalReason, invertedReason, &expectedTypes, LeafArrayBehavior::kNoTraversal);
    }

    template <class T>
    void generateTypeError(const TypeMatchExpressionBase<T>* expr, LeafArrayBehavior behavior) {
        _context->pushNewFrame(*expr, _context->getCurrentDocument());
        static constexpr auto kNormalReason = "type did not match";
        static constexpr auto kInvertedReason = "type did match";
        if (_context->shouldGenerateError(*expr)) {
            appendErrorDetails(*expr);
            auto arr = createValuesArray(expr->path(), behavior);
            appendMissingField(arr);
            appendErrorReason(kNormalReason, kInvertedReason);
            appendConsideredValues(arr);
            appendConsideredTypes(arr);
        }
    }

    /**
     * Generates a document validation error for a bit test expression 'expr'.
     */
    void generateError(const BitTestMatchExpression* expr) {
        static constexpr auto kNormalReason = "bitwise operator failed to match";
        static constexpr auto kInvertedReason = "bitwise operator matched successfully";
        static const std::set<BSONType> kExpectedTypes{BSONType::NumberInt,
                                                       BSONType::NumberLong,
                                                       BSONType::NumberDouble,
                                                       BSONType::NumberDecimal,
                                                       BSONType::BinData};
        generatePathError(*expr, kNormalReason, kInvertedReason, &kExpectedTypes);
    }

    /**
     * Performs the setup necessary to generate an error for 'expr'.
     */
    void preVisitTreeOperator(const MatchExpression* expr) {
        invariant(expr->getCategory() == MatchExpression::MatchCategory::kLogical);
        _context->pushNewFrame(*expr, _context->getCurrentDocument());
        if (_context->shouldGenerateError(*expr)) {
            auto annotation = expr->getErrorAnnotation();
            // Only append the operator name if it will produce an object error corresponding to
            // a user-facing operator.
            if (!_context->producesArray(*expr))
                appendOperatorName(*expr);
            _context->getCurrentObjBuilder().appendElements(annotation->annotation);
        }
    }
    /**
     * Utility to generate an error for logical operators which are treated like leaves for the
     * purposes of error reporting.
     */
    void generateLogicalLeafError(const ListOfMatchExpression& expr,
                                  const std::string& normalReason,
                                  const std::string& invertedReason) {
        _context->pushNewFrame(expr, _context->getCurrentDocument());
        if (_context->shouldGenerateError(expr)) {
            // $all with no children should not translate to an 'AndMatchExpression' and 'enum'
            // must have non-zero children.
            invariant(expr.numChildren() > 0);
            appendErrorDetails(expr);
            auto childExpr = expr.getChild(0);
            auto arr = createValuesArray(childExpr->path(), LeafArrayBehavior::kNoTraversal);
            appendMissingField(arr);
            appendErrorReason(normalReason, invertedReason);
            appendConsideredValues(arr);
        }
    }

    /**
     * For an AlwaysBooleanMatchExpression, we simply output the error information obtained at
     * parse time.
     */
    void generateAlwaysBooleanError(const AlwaysBooleanMatchExpression& expr) {
        _context->pushNewFrame(expr, _context->getCurrentDocument());
        if (_context->shouldGenerateError(expr)) {
            // An AlwaysBooleanMatchExpression can only contribute to error generation when the
            // inversion matches the value of the 'expr'. More precisely, it is only possible
            // to generate an error for 'expr' if it evaluates to false in a normal context or
            // if it evaluates to true an inverted context.
            if (expr.isTriviallyFalse()) {
                invariant(_context->getCurrentInversion() == InvertError::kNormal);
            } else {
                invariant(_context->getCurrentInversion() == InvertError::kInverted);
            }
            appendErrorDetails(expr);
            static constexpr auto kNormalReason = "expression always evaluates to false";
            static constexpr auto kInvertedReason = "expression always evaluates to true";
            appendErrorReason(kNormalReason, kInvertedReason);
        }
    }

    void generateStringLengthError(const InternalSchemaStrLengthMatchExpression& expr) {
        static constexpr auto kNormalReason = "specified string length was not satisfied";
        static constexpr auto kInvertedReason = "specified string length was satisfied";
        static const std::set<BSONType> expectedTypes{BSONType::String};
        generatePathError(
            expr, kNormalReason, kInvertedReason, &expectedTypes, LeafArrayBehavior::kNoTraversal);
    }

    /**
     * Determines if a validation error should be generated for a JSON Schema array keyword match
     * expression 'expr' given the current document validation context and returns the array 'expr'
     * expression applies over. If a validation error should not be generated, then the
     * End-Of-Object (EOO) value is returned. If a validation error should be generated, then the
     * type of the value of the returned BSONElement is always an array.
     */
    BSONElement getValueForArrayKeywordExpressionIfShouldGenerateError(
        const MatchExpression& expr) {
        if (!_context->shouldGenerateError(expr)) {
            return {};
        }
        if (InvertError::kInverted == _context->getCurrentInversion()) {
            // Inverted errors are not supported.
            return {};
        }

        // Determine what value does 'expr' expression apply over.
        ElementPath path(
            expr.path(), LeafArrayBehavior::kNoTraversal, NonLeafArrayBehavior::kNoTraversal);
        auto attributeValue = getValueAt(path);

        // If attribute value is either not present or is not an array, do not generate an error,
        // since related match expressions do that instead. There are 4 cases of how an array
        // keyword can be defined in combination with 'required' and 'type' keywords (in the
        // explanation below parameter 'expr' corresponds to '(array keyword match expression)'):
        //
        // 1) 'required' is not present, {type: 'array'} is not present. In this case the expression
        // tree corresponds to ((array keyword match expression) OR NOT (is array)) OR (NOT
        // (attribute exists)). This tree can fail to match only if the attribute is present and is
        // an array.
        //
        // 2) 'required' is not present, {type: 'array'} is present. In this case the expression
        // tree corresponds to ((array keyword match expression) AND (is array)) OR (NOT (attribute
        // exists)). If the input is an attribute of a non-array type, then both (array keyword
        // match expression) and (is array) expressions fail to match and are asked to contribute to
        // the validation error. We expect only (is array) expression, not an (array keyword match
        // expression), to report a type mismatch, since otherwise the error would contain redundant
        // elements.
        //
        // 3) 'required' is present, {type: 'array'} is not present. In this case the expression
        // tree corresponds to ((array keyword match expression) OR NOT (is array)) AND (attribute
        // exists). This tree can fail to match if the attribute is present and is an array, and
        // fails to match when the attribute is not present. In the latter case expression part
        // ((array keyword match expression) OR NOT (is array)) matches and (array keyword match
        // expression) is not asked to contribute to the error.
        //
        // 4) 'required' is present, {type: 'array'} is present. In this case the expression tree
        // corresponds to ((array keyword match expression) AND (is array)) AND (attribute exists).
        // This tree can fail to match if the attribute is present and is an array, and fails to
        // match when the attribute is not present or is not an array. In the case when the
        // attribute is not present all parts of the expression fail to match and are asked to
        // contribute to the error, but we expect only (attribute exists) expression to contribute,
        // since otherwise the error would contain redundant elements.
        return (attributeValue.type() == BSONType::Array) ? attributeValue : BSONElement{};
    }

    /**
     * Generates an error for JSON Schema "minItems"/"maxItems" keyword match expression 'expr'.
     */
    void generateJSONSchemaMinItemsMaxItemsError(
        const InternalSchemaNumArrayItemsMatchExpression* expr) {
        static constexpr auto normalReason = "array did not match specified length";
        _context->pushNewFrame(*expr, _context->getCurrentDocument());
        if (auto attributeValue = getValueForArrayKeywordExpressionIfShouldGenerateError(*expr)) {
            appendErrorDetails(*expr);
            appendErrorReason(normalReason, "");
            auto attributeValueAsArray = BSONArray(attributeValue.embeddedObject());
            appendConsideredValue(attributeValueAsArray);
        } else {
            _context->setCurrentRuntimeState(RuntimeState::kNoError);
        }
    }

    /**
     * Generates an error for JSON Schema "additionalItems" keyword set to 'false'.
     */
    void generateJSONSchemaAdditionalItemsFalseError(
        const InternalSchemaAllElemMatchFromIndexMatchExpression* expr) {
        static constexpr auto normalReason = "found additional items";
        _context->pushNewFrame(*expr, _context->getCurrentDocument());
        if (auto attributeValue = getValueForArrayKeywordExpressionIfShouldGenerateError(*expr)) {
            appendErrorDetails(*expr);
            appendErrorReason(normalReason, "");
            appendAdditionalItems(BSONArray(attributeValue.embeddedObject()), expr->startIndex());
        } else {
            _context->setCurrentRuntimeState(RuntimeState::kNoError);
        }
    }

    /**
     * Generates an error for JSON Schema "items" keyword set to an array of subschemas that is used
     * to validate elements of the array.
     */
    void generateJSONSchemaItemsSchemaArrayError(const AndMatchExpression& expr) {
        _context->pushNewFrame(expr, _context->getCurrentDocument());

        // Determine if we need to generate an error using a child of the "$and" expression, which
        // must be of InternalSchemaMatchArrayIndexMatchExpression type, since "$and" does not have
        // a path associated with it.

        // If 'expr' does not have any children then we have 'items':[] case and we don't need to
        // generate an error.
        if (expr.numChildren() == 0) {
            return;
        }
        invariant(expr.getChild(0)->matchType() ==
                  MatchExpression::MatchType::INTERNAL_SCHEMA_MATCH_ARRAY_INDEX);
        if (getValueForArrayKeywordExpressionIfShouldGenerateError(*expr.getChild(0))) {
            appendOperatorName(expr);

            // Since the "items" keyword set to an array of subschemas logically behaves as "$and",
            // it needs its children to call 'matches' to discern which clauses failed.
            _context->setCurrentRuntimeState(RuntimeState::kErrorNeedChildrenInfo);
        } else {
            // Force children match expressions to not generate any errors.
            _context->setCurrentRuntimeState(RuntimeState::kNoError);
        }
    }

    /**
     * Builds a BSON object from a BSON element 'element' using the same name placeholder as the
     * JSON Schema match expressions.
     */
    BSONObj toObjectWithPlaceholder(BSONElement element) {
        return BSON(JSONSchemaParser::kNamePlaceholder << element);
    }

    /**
     * Adds elements starting from index 'startIndex' from array 'array' to the current object as
     * "additionalItems" attribute.
     */
    void appendAdditionalItems(const mongo::BSONArray& array, size_t startIndex) {
        auto it = BSONObjIterator(array);

        // Skip first 'startIndex' elements.
        match_expression_util::advanceBy(startIndex, it);

        // Add remaining array elements as "additionalItems" attribute.
        auto& detailsArrayBuilder = _context->getCurrentArrayBuilder();
        while (it.more()) {
            detailsArrayBuilder.append(it.next());
        }
        _context->verifySizeAndAppend(
            detailsArrayBuilder.arr(), "additionalItems", &_context->getCurrentObjBuilder());
    }

    /**
     * Generates an error for JSON Schema array keyword set to a single schema value that is used
     * to validate elements of the array.
     */
    void generateJSONSchemaArraySingleSchemaError(
        const InternalSchemaAllElemMatchFromIndexMatchExpression* expr,
        const std::string& normalReason,
        const std::string& invertedReason) {
        _context->pushNewFrame(*expr, _context->getCurrentDocument());
        if (auto attributeValue = getValueForArrayKeywordExpressionIfShouldGenerateError(*expr)) {
            appendOperatorName(*expr);
            appendErrorReason(normalReason, invertedReason);
            auto failingElement =
                expr->findFirstMismatchInArray(attributeValue.embeddedObject(), nullptr);
            invariant(failingElement);
            _context->getCurrentObjBuilder().appendNumber(
                "itemIndex"_sd, std::stoll(failingElement.fieldNameStringData().toString()));
            _context->setCurrentDocument(toObjectWithPlaceholder(failingElement));
        } else {
            // Disable error generation by the child expression of 'expr'.
            _context->setCurrentRuntimeState(RuntimeState::kNoError);
        }
    }
    void generateNumPropertiesError(const MatchExpression& numPropertiesExpr) {
        static constexpr auto kNormalReason = "specified number of properties was not satisfied";
        static constexpr auto kInvertedReason = "";
        _context->pushNewFrame(numPropertiesExpr, _context->getCurrentDocument());
        if (_context->shouldGenerateError(numPropertiesExpr)) {
            appendErrorDetails(numPropertiesExpr);
            appendErrorReason(kNormalReason, kInvertedReason);
            auto& objBuilder = _context->getCurrentObjBuilder();
            objBuilder.append("numberOfProperties", _context->getCurrentDocument().nFields());
        }
    }


    ValidationErrorContext* _context;
};

/**
 * Visitor which maintains state for tree MatchExpressions in between visiting each child.
 */
class ValidationErrorInVisitor final : public MatchExpressionConstVisitor {
public:
    ValidationErrorInVisitor(ValidationErrorContext* context) : _context(context) {}
    void visit(const AlwaysFalseMatchExpression* expr) final {}
    void visit(const AlwaysTrueMatchExpression* expr) final {}
    void visit(const AndMatchExpression* expr) final {
        inVisitTreeOperator(expr);
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
    void visit(const GeoNearMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }
    void visit(const InMatchExpression* expr) final {}
    void visit(const InternalExprEqMatchExpression* expr) final {}
    void visit(const InternalSchemaAllElemMatchFromIndexMatchExpression* expr) final {}
    void visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaBinDataEncryptedTypeExpression* expr) final {}
    void visit(const InternalSchemaBinDataSubTypeExpression* expr) final {}
    void visit(const InternalSchemaCondMatchExpression* expr) final {
        if (_context->shouldGenerateError(*expr)) {
            generateSingleDependencyError(*expr);
        }
        _context->incrementCurrentChildIndex();
    }
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
    void visit(const InternalSchemaXorMatchExpression* expr) final {
        // Only check for child errors when we're in a normal context, that is, when none of expr's
        // subschemas matched, as opposed to the inverted context, where more than one subschema
        // matched.
        if (_context->getCurrentInversion() == InvertError::kNormal) {
            inVisitTreeOperator(expr);
        }
    }
    void visit(const LTEMatchExpression* expr) final {}
    void visit(const LTMatchExpression* expr) final {}
    void visit(const ModMatchExpression* expr) final {}
    void visit(const NorMatchExpression* expr) final {
        inVisitTreeOperator(expr);
    }
    void visit(const NotMatchExpression* expr) final {}
    void visit(const OrMatchExpression* expr) final {
        inVisitTreeOperator(expr);
    }
    void visit(const RegexMatchExpression* expr) final {}
    void visit(const SizeMatchExpression* expr) final {}
    void visit(const TextMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }
    void visit(const TextNoOpMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }
    void visit(const TwoDPtInAnnulusExpression* expr) final {}
    void visit(const TypeMatchExpression* expr) final {}
    void visit(const WhereMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }
    void visit(const WhereNoOpMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }

private:
    /**
     * Generates an error for a single $jsonSchema dependency represented by 'expr'.
     */
    void generateSingleDependencyError(const InternalSchemaCondMatchExpression& expr) {
        auto childIndex = _context->getCurrentChildIndex();
        auto& builder = _context->getCurrentObjBuilder();
        auto&& tag = expr.getErrorAnnotation()->tag;
        // When generating an error for 'InternalSchemaCondMatchExpression', that is, a single
        // jsonSchema dependency, we can only generate an error for the 'then' branch (expr's child
        // at index 1). This is because the only way that a jsonSchema dependency can fail is if
        // expr's condition (expr's child at index 0) evaluates to true and the 'then' branch
        // evaluates to false. Additionally, the else branch (expr's child at index 2) is never
        // considered because it always evaluates to true and detailed inverted errors in the
        // context of $jsonSchema are not supported.
        if (_context->haveLatestCompleteError() && childIndex == 1) {
            builder.append(
                "conditionalProperty",
                expr.getErrorAnnotation()->annotation.firstElement().fieldNameStringData());
            if (tag == "_schemaDependency") {
                // In the case of a schema dependency (i.e. {dependencies: {a: {<subschema>}}}),
                // we simply append the subschema's generated failure.
                _context->appendLatestCompleteError(&builder);
            } else if (tag == "_propertyDependency") {
                // In the case of a property dependency (i.e. {dependencies: {a:
                // [<set of dependant properties>]}}), we append an array of missing properties.
                builder.append("missingProperties", _context->getLatestCompleteErrorArray());
            }
        }
    }

    void inVisitTreeOperator(const ListOfMatchExpression* expr) {
        finishLogicalOperatorChildError(expr, _context);
    }
    ValidationErrorContext* _context;
};

/**
 * Visitor which finalizes the generated error for the current MatchExpression.
 */
class ValidationErrorPostVisitor final : public MatchExpressionConstVisitor {
public:
    ValidationErrorPostVisitor(ValidationErrorContext* context) : _context(context) {}
    void visit(const AlwaysFalseMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const AlwaysTrueMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const AndMatchExpression* expr) final {
        auto tag = expr->getErrorAnnotation()->tag;
        auto inversion = _context->getCurrentInversion();
        // Clean up the frame for this node if we're finishing the error for an $all, an inverted
        // $jsonSchema, or this node shouldn't generate an error.
        if (tag == "$all" || (tag == "$jsonSchema" && inversion == InvertError::kInverted)) {
            _context->finishCurrentError(expr);
            return;
        }
        // Specify a different details string based on the tag in expr's annotation where
        // the first entry is the details string in the normal case and the second is the string
        // for the inverted case.
        static const StringMap<std::pair<std::string, std::string>> detailsStringMap = {
            {"$and", {"clausesNotSatisfied", "clausesSatisfied"}},
            {"allOf", {"schemasNotSatisfied", ""}},
            {"properties", {"propertiesNotSatisfied", ""}},
            {"$jsonSchema", {"schemaRulesNotSatisfied", ""}},
            {"_subschema", {"", ""}},
            {"_propertiesExistList", {"", ""}},
            {"items", {"details", ""}},
            {"dependencies", {"failingDependencies", ""}},
            {"", {"details", ""}}};
        auto detailsStringPair = detailsStringMap.find(tag);
        invariant(detailsStringPair != detailsStringMap.end());
        auto&& stringPair = detailsStringPair->second;
        if (inversion == InvertError::kNormal) {
            postVisitTreeOperator(expr, stringPair.first);
        } else {
            postVisitTreeOperator(expr, stringPair.second);
        }
    }
    void visit(const BitsAllClearMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const BitsAllSetMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const BitsAnyClearMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const BitsAnySetMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const ElemMatchObjectMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const ElemMatchValueMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const EqualityMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const ExistsMatchExpression* expr) final {
        // If this node reports a path as its error, set 'latestCompleteError' appropriately.
        if (_context->shouldGenerateError(*expr) &&
            expr->getErrorAnnotation()->tag == "_propertyExists") {
            _context->latestCompleteError = expr->path().toString();
            _context->popFrame();
        } else {
            _context->finishCurrentError(expr);
        }
    }
    void visit(const ExprMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const GTEMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const GTMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const GeoMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const GeoNearMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }
    void visit(const InMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const InternalExprEqMatchExpression* expr) final {}
    void visit(const InternalSchemaAllElemMatchFromIndexMatchExpression* expr) final {
        switch (toItemsKeywordType(*expr)) {
            case ItemsKeywordType::kItems:
            case ItemsKeywordType::kAdditionalItemsSchema:
                if (_context->shouldGenerateError(*expr)) {
                    _context->appendLatestCompleteError(&_context->getCurrentObjBuilder());
                }
                break;
            case ItemsKeywordType::kAdditionalItemsFalse:
                break;
            default:
                MONGO_UNREACHABLE;
        }
        _context->finishCurrentError(expr);
    }
    void visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaBinDataEncryptedTypeExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const InternalSchemaBinDataSubTypeExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const InternalSchemaCondMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const InternalSchemaEqMatchExpression* expr) final {}
    void visit(const InternalSchemaFmodMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const InternalSchemaMatchArrayIndexMatchExpression* expr) final {
        // If generating an error, append the error details.
        if (_context->shouldGenerateError(*expr)) {
            _context->appendLatestCompleteError(&_context->getCurrentObjBuilder());
        }
        _context->finishCurrentError(expr);
    }
    void visit(const InternalSchemaMaxItemsMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const InternalSchemaMaxLengthMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const InternalSchemaMaxPropertiesMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const InternalSchemaMinItemsMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const InternalSchemaMinLengthMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const InternalSchemaMinPropertiesMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const InternalSchemaObjectMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const InternalSchemaRootDocEqMatchExpression* expr) final {}
    void visit(const InternalSchemaTypeExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const InternalSchemaUniqueItemsMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const InternalSchemaXorMatchExpression* expr) final {
        static constexpr auto normalDetailString = "schemasNotSatisfied";
        if (_context->getCurrentInversion() == InvertError::kNormal) {
            postVisitTreeOperator(expr, normalDetailString);
        } else {
            // In the inverted case, we treat 'oneOf' as a leaf.
            _context->finishCurrentError(expr);
        }
    }
    void visit(const LTEMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const LTMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const ModMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const NorMatchExpression* expr) final {
        static constexpr auto kNormalDetailsString = "clausesNotSatisfied";
        static constexpr auto kInvertedDetailsString = "clausesSatisfied";
        if (_context->getCurrentInversion() == InvertError::kNormal) {
            postVisitTreeOperator(expr, kNormalDetailsString);
        } else {
            postVisitTreeOperator(expr, kInvertedDetailsString);
        }
    }
    void visit(const NotMatchExpression* expr) final {
        // In the case of a $jsonSchema "not", we do not report any error details
        // explaining why the subschema did match.
        if (_context->shouldGenerateError(*expr) && expr->getErrorAnnotation()->tag != "not") {
            _context->appendLatestCompleteError(&_context->getCurrentObjBuilder());
        }
        _context->finishCurrentError(expr);
    }
    void visit(const OrMatchExpression* expr) final {
        auto tag = expr->getErrorAnnotation()->tag;
        // Clean up the frame for this node if we're finishing the error for an 'enum' or this node
        // shouldn't generate an error.
        if (tag == "enum" || !_context->shouldGenerateError(*expr)) {
            _context->finishCurrentError(expr);
            return;
        }
        // Specify a different details string based on the tag in expr's annotation where the first
        // entry is the details string in the normal case and the second is the string for the
        // inverted case.
        static const StringMap<std::pair<std::string, std::string>> detailsStringMap = {
            {"$or", {"clausesNotSatisfied", "clausesSatisfied"}},
            {"anyOf", {"schemasNotSatisfied", ""}}};
        auto detailsStringPair = detailsStringMap.find(tag);
        invariant(detailsStringPair != detailsStringMap.end());
        auto stringPair = detailsStringPair->second;
        if (_context->getCurrentInversion() == InvertError::kNormal) {
            postVisitTreeOperator(expr, stringPair.first);
        } else {
            postVisitTreeOperator(expr, stringPair.second);
        }
    }
    void visit(const RegexMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const SizeMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const TextMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }
    void visit(const TextNoOpMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }
    void visit(const TwoDPtInAnnulusExpression* expr) final {}
    void visit(const TypeMatchExpression* expr) final {
        _context->finishCurrentError(expr);
    }
    void visit(const WhereMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }
    void visit(const WhereNoOpMatchExpression* expr) final {
        MONGO_UNREACHABLE;
    }

private:
    void postVisitTreeOperator(const ListOfMatchExpression* expr,
                               const std::string& detailsString) {
        finishLogicalOperatorChildError(expr, _context);
        // Append the result of the current array builder to the current object builder under the
        // field name 'detailsString' unless this node produces an array (i.e. in the case of a
        // subschema).
        if (_context->shouldGenerateError(*expr) && !_context->producesArray(*expr)) {
            auto failedClauses = _context->getCurrentArrayBuilder().arr();
            _context->getCurrentObjBuilder().append(detailsString, failedClauses);
        }
        _context->finishCurrentError(expr);
    }

    ValidationErrorContext* _context;
};

/**
 * Returns true if each node in the tree rooted at 'validatorExpr' has an error annotation, false
 * otherwise.
 */
bool hasErrorAnnotations(const MatchExpression& validatorExpr) {
    if (!validatorExpr.getErrorAnnotation())
        return false;
    for (const auto childExpr : validatorExpr) {
        if (!childExpr || !hasErrorAnnotations(*childExpr)) {
            return false;
        }
    }
    return true;
}

/**
 * Appends the object id of 'doc' to 'builder' under the 'failingDocumentId' field.
 */
void appendDocumentId(const BSONObj& doc, BSONObjBuilder* builder) {
    BSONElement objectIdElement;
    invariant(doc.getObjectID(objectIdElement));
    builder->appendAs(objectIdElement, "failingDocumentId"_sd);
}

/**
 * Returns true if 'generatedError' is of valid depth; false otherwise.
 */
bool checkValidationErrorDepth(const BSONObj& generatedError) {
    const auto maxDepth = computeMaxAllowedValidationErrorDepth();
    // Implemented iteratively to avoid creating too many stack frames.
    std::stack<BSONObjIterator> stack;
    stack.emplace(generatedError);
    while (!stack.empty()) {
        if (stack.size() > maxDepth) {
            return false;
        }
        auto next = stack.top().next();
        if (next.type() == BSONType::Object || next.type() == BSONType::Array) {
            stack.emplace(next.embeddedObject());
        }
        if (!stack.top().more()) {
            stack.pop();
        }
    }
    return true;
}

/**
 * Generates a document validation error using match expression 'validatorExpr' for document
 * 'doc'.
 */
BSONObj generateErrorHelper(const MatchExpression& validatorExpr,
                            const BSONObj& doc,
                            bool truncate,
                            const int maxDocValidationErrorSize,
                            const int maxConsideredValues) {
    // Throw if 'docValidationInternalErrorFailPoint' is enabled.
    uassert(4944300,
            "docValidationInternalErrorFailPoint is enabled",
            !docValidationInternalErrorFailPoint.shouldFail());

    ValidationErrorContext context(doc, truncate, maxDocValidationErrorSize, maxConsideredValues);
    ValidationErrorPreVisitor preVisitor{&context};
    ValidationErrorInVisitor inVisitor{&context};
    ValidationErrorPostVisitor postVisitor{&context};

    // TODO SERVER-49446: Once all nodes have ErrorAnnotations, this check should be converted to an
    // invariant check that all nodes have an annotation. Also add an invariant to the
    // DocumentValidationFailureInfo constructor to check that it is initialized with a non-empty
    // object.
    if (!hasErrorAnnotations(validatorExpr)) {
        return BSONObj();
    }
    MatchExpressionWalker walker{&preVisitor, &inVisitor, &postVisitor};
    tree_walker::walk<true, MatchExpression>(&validatorExpr, &walker);

    // There should be no frames when error generation is complete as the finished error will be
    // stored in 'context'.
    invariant(context.frames.empty());
    auto error = context.getLatestCompleteErrorObject();
    invariant(!error.isEmpty());

    // Add document id to the error object.
    BSONObjBuilder objBuilder;
    appendDocumentId(doc, &objBuilder);

    // Record whether the generated error was truncated.
    if (truncate)
        objBuilder.append("truncated", true);
    // Add errors from match expressions.
    objBuilder.append("details"_sd, std::move(error));

    auto finalError = objBuilder.obj();
    // Verify that the generated error is of valid depth.
    if (!checkValidationErrorDepth(finalError)) {
        BSONObjBuilder errorDetails;
        static constexpr auto kDeeplyNestedError = "generated error was too deeply nested";
        errorDetails.append("reason", kDeeplyNestedError);
        errorDetails.append("truncated", true);
        return errorDetails.obj();
    }
    return finalError;
}
}  // namespace

std::shared_ptr<const ErrorExtraInfo> DocumentValidationFailureInfo::parse(const BSONObj& obj) {
    if (!obj.hasField("errInfo"_sd)) {
        // TODO SERVER-50524: remove this block when 5.0 becomes last-lts.
        return nullptr;
    }
    auto errInfo = obj["errInfo"];
    uassert(4878100,
            "DocumentValidationFailureInfo must have a field 'errInfo' of type object",
            errInfo.type() == BSONType::Object);
    return std::make_shared<DocumentValidationFailureInfo>(errInfo.embeddedObject());
}

void DocumentValidationFailureInfo::serialize(BSONObjBuilder* bob) const {
    bob->append("errInfo", _details);
}
const BSONObj& DocumentValidationFailureInfo::getDetails() const {
    return _details;
}

BSONObj generateError(const MatchExpression& validatorExpr,
                      const BSONObj& doc,
                      const int maxDocValidationErrorSize,
                      const int maxConsideredValues) {
    // Attempt twice to generate a detailed document validation error before reporting to the user
    // that the generated error grew too large.
    constexpr static auto kNoteString = "note";
    bool truncate = false;
    for (auto attempt = 0; attempt < 2; ++attempt) {
        try {
            auto error = generateErrorHelper(
                validatorExpr, doc, truncate, maxDocValidationErrorSize, maxConsideredValues);
            uassert(ErrorCodes::BSONObjectTooLarge,
                    "doc validation error exceeded maximum size",
                    error.objsize() <= maxDocValidationErrorSize);
            return error;
        } catch (const ExceptionFor<ErrorCodes::BSONObjectTooLarge>&) {
            // Try again, but this time omit details such as 'consideredValues' or 'specifiedAs'
            // that are proportional to the size of the validator expression or the failed document.
            truncate = true;
        } catch (const DBException& e) {
            BSONObjBuilder error;
            appendDocumentId(doc, &error);
            static constexpr auto kErrorReason = "failed to generate document validation error";
            error.append(kNoteString, kErrorReason);
            BSONObjBuilder subBuilder = error.subobjStart("details");
            e.serialize(&subBuilder);
            subBuilder.done();
            return error.obj();
        }
    }
    // If we've reached here, both attempts failed to generate a sufficiently small error. Return
    // an error indicating as much to the user.
    BSONObjBuilder error;
    appendDocumentId(doc, &error);
    static constexpr auto kTruncationReason = "detailed error was too large";
    error.append(kNoteString, kTruncationReason);
    return error.obj();
}
}  // namespace mongo::doc_validation_error
