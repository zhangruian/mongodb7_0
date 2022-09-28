/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "mongo/base/compare_numbers.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/datetime.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/datetime/date_time_support.h"

#include <absl/container/inlined_vector.h>

namespace mongo {
namespace sbe {
namespace vm {
template <typename Op>
std::pair<value::TypeTags, value::Value> genericCompare(
    value::TypeTags lhsTag,
    value::Value lhsValue,
    value::TypeTags rhsTag,
    value::Value rhsValue,
    const StringData::ComparatorInterface* comparator = nullptr,
    Op op = {}) {
    if (value::isNumber(lhsTag) && value::isNumber(rhsTag)) {
        switch (getWidestNumericalType(lhsTag, rhsTag)) {
            case value::TypeTags::NumberInt32: {
                auto result = op(value::numericCast<int32_t>(lhsTag, lhsValue),
                                 value::numericCast<int32_t>(rhsTag, rhsValue));
                return {value::TypeTags::Boolean, value::bitcastFrom<bool>(result)};
            }
            case value::TypeTags::NumberInt64: {
                auto result = op(value::numericCast<int64_t>(lhsTag, lhsValue),
                                 value::numericCast<int64_t>(rhsTag, rhsValue));
                return {value::TypeTags::Boolean, value::bitcastFrom<bool>(result)};
            }
            case value::TypeTags::NumberDouble: {
                auto result = [&]() {
                    if (lhsTag == value::TypeTags::NumberInt64) {
                        auto rhs = value::bitcastTo<double>(rhsValue);
                        if (std::isnan(rhs)) {
                            return false;
                        }
                        return op(compareLongToDouble(value::bitcastTo<int64_t>(lhsValue), rhs), 0);
                    } else if (rhsTag == value::TypeTags::NumberInt64) {
                        auto lhs = value::bitcastTo<double>(lhsValue);
                        if (std::isnan(lhs)) {
                            return false;
                        }
                        return op(compareDoubleToLong(lhs, value::bitcastTo<int64_t>(rhsValue)), 0);
                    } else {
                        return op(value::numericCast<double>(lhsTag, lhsValue),
                                  value::numericCast<double>(rhsTag, rhsValue));
                    }
                }();
                return {value::TypeTags::Boolean, value::bitcastFrom<bool>(result)};
            }
            case value::TypeTags::NumberDecimal: {
                auto result = [&]() {
                    if (lhsTag == value::TypeTags::NumberDouble) {
                        if (value::isNaN(lhsTag, lhsValue) || value::isNaN(rhsTag, rhsValue)) {
                            return false;
                        }
                        return op(compareDoubleToDecimal(value::bitcastTo<double>(lhsValue),
                                                         value::bitcastTo<Decimal128>(rhsValue)),
                                  0);
                    } else if (rhsTag == value::TypeTags::NumberDouble) {
                        if (value::isNaN(lhsTag, lhsValue) || value::isNaN(rhsTag, rhsValue)) {
                            return false;
                        }
                        return op(compareDecimalToDouble(value::bitcastTo<Decimal128>(lhsValue),
                                                         value::bitcastTo<double>(rhsValue)),
                                  0);
                    } else {
                        return op(value::numericCast<Decimal128>(lhsTag, lhsValue),
                                  value::numericCast<Decimal128>(rhsTag, rhsValue));
                    }
                }();
                return {value::TypeTags::Boolean, value::bitcastFrom<bool>(result)};
            }
            default:
                MONGO_UNREACHABLE;
        }
    } else if (isStringOrSymbol(lhsTag) && isStringOrSymbol(rhsTag)) {
        auto lhsStr = value::getStringOrSymbolView(lhsTag, lhsValue);
        auto rhsStr = value::getStringOrSymbolView(rhsTag, rhsValue);
        auto result =
            op(comparator ? comparator->compare(lhsStr, rhsStr) : lhsStr.compare(rhsStr), 0);

        return {value::TypeTags::Boolean, value::bitcastFrom<bool>(result)};
    } else if (lhsTag == value::TypeTags::Date && rhsTag == value::TypeTags::Date) {
        auto result = op(value::bitcastTo<int64_t>(lhsValue), value::bitcastTo<int64_t>(rhsValue));
        return {value::TypeTags::Boolean, value::bitcastFrom<bool>(result)};
    } else if (lhsTag == value::TypeTags::Timestamp && rhsTag == value::TypeTags::Timestamp) {
        auto result =
            op(value::bitcastTo<uint64_t>(lhsValue), value::bitcastTo<uint64_t>(rhsValue));
        return {value::TypeTags::Boolean, value::bitcastFrom<bool>(result)};
    } else if (lhsTag == value::TypeTags::Boolean && rhsTag == value::TypeTags::Boolean) {
        auto result = op(value::bitcastTo<bool>(lhsValue), value::bitcastTo<bool>(rhsValue));
        return {value::TypeTags::Boolean, value::bitcastFrom<bool>(result)};
    } else if (lhsTag == value::TypeTags::Null && rhsTag == value::TypeTags::Null) {
        // This is where Mongo differs from SQL.
        auto result = op(0, 0);
        return {value::TypeTags::Boolean, value::bitcastFrom<bool>(result)};
    } else if (lhsTag == value::TypeTags::MinKey && rhsTag == value::TypeTags::MinKey) {
        auto result = op(0, 0);
        return {value::TypeTags::Boolean, value::bitcastFrom<bool>(result)};
    } else if (lhsTag == value::TypeTags::MaxKey && rhsTag == value::TypeTags::MaxKey) {
        auto result = op(0, 0);
        return {value::TypeTags::Boolean, value::bitcastFrom<bool>(result)};
    } else if (lhsTag == value::TypeTags::bsonUndefined &&
               rhsTag == value::TypeTags::bsonUndefined) {
        auto result = op(0, 0);
        return {value::TypeTags::Boolean, value::bitcastFrom<bool>(result)};
    } else if ((value::isArray(lhsTag) && value::isArray(rhsTag)) ||
               (value::isObject(lhsTag) && value::isObject(rhsTag)) ||
               (value::isBinData(lhsTag) && value::isBinData(rhsTag))) {
        auto [tag, val] = value::compareValue(lhsTag, lhsValue, rhsTag, rhsValue, comparator);
        if (tag == value::TypeTags::NumberInt32) {
            auto result = op(value::bitcastTo<int32_t>(val), 0);
            return {value::TypeTags::Boolean, value::bitcastFrom<bool>(result)};
        }
    } else if (isObjectId(lhsTag) && isObjectId(rhsTag)) {
        auto lhsObjId = lhsTag == value::TypeTags::ObjectId
            ? value::getObjectIdView(lhsValue)->data()
            : value::bitcastTo<uint8_t*>(lhsValue);
        auto rhsObjId = rhsTag == value::TypeTags::ObjectId
            ? value::getObjectIdView(rhsValue)->data()
            : value::bitcastTo<uint8_t*>(rhsValue);
        auto threeWayResult = memcmp(lhsObjId, rhsObjId, sizeof(value::ObjectIdType));
        return {value::TypeTags::Boolean, value::bitcastFrom<bool>(op(threeWayResult, 0))};
    } else if (lhsTag == value::TypeTags::bsonRegex && rhsTag == value::TypeTags::bsonRegex) {
        auto lhsRegex = value::getBsonRegexView(lhsValue);
        auto rhsRegex = value::getBsonRegexView(rhsValue);

        if (auto threeWayResult = lhsRegex.pattern.compare(rhsRegex.pattern); threeWayResult != 0) {
            return {value::TypeTags::Boolean, value::bitcastFrom<bool>(op(threeWayResult, 0))};
        }

        auto threeWayResult = lhsRegex.flags.compare(rhsRegex.flags);
        return {value::TypeTags::Boolean, value::bitcastFrom<bool>(op(threeWayResult, 0))};
    } else if (lhsTag == value::TypeTags::bsonDBPointer &&
               rhsTag == value::TypeTags::bsonDBPointer) {
        auto lhsDBPtr = value::getBsonDBPointerView(lhsValue);
        auto rhsDBPtr = value::getBsonDBPointerView(rhsValue);
        if (lhsDBPtr.ns.size() != rhsDBPtr.ns.size()) {
            return {value::TypeTags::Boolean,
                    value::bitcastFrom<bool>(op(lhsDBPtr.ns.size(), rhsDBPtr.ns.size()))};
        }

        if (auto threeWayResult = lhsDBPtr.ns.compare(rhsDBPtr.ns); threeWayResult != 0) {
            return {value::TypeTags::Boolean, value::bitcastFrom<bool>(op(threeWayResult, 0))};
        }

        auto threeWayResult = memcmp(lhsDBPtr.id, rhsDBPtr.id, sizeof(value::ObjectIdType));
        return {value::TypeTags::Boolean, value::bitcastFrom<bool>(op(threeWayResult, 0))};
    } else if (lhsTag == value::TypeTags::bsonJavascript &&
               rhsTag == value::TypeTags::bsonJavascript) {
        auto lhsCode = value::getBsonJavascriptView(lhsValue);
        auto rhsCode = value::getBsonJavascriptView(rhsValue);
        return {value::TypeTags::Boolean,
                value::bitcastFrom<bool>(op(lhsCode.compare(rhsCode), 0))};
    } else if (lhsTag == value::TypeTags::bsonCodeWScope &&
               rhsTag == value::TypeTags::bsonCodeWScope) {
        auto lhsCws = value::getBsonCodeWScopeView(lhsValue);
        auto rhsCws = value::getBsonCodeWScopeView(rhsValue);
        if (auto threeWayResult = lhsCws.code.compare(rhsCws.code); threeWayResult != 0) {
            return {value::TypeTags::Boolean, value::bitcastFrom<bool>(op(threeWayResult, 0))};
        }

        // Special string comparison semantics do not apply to strings nested inside the
        // CodeWScope scope object, so we do not pass through the string comparator.
        auto [tag, val] = value::compareValue(value::TypeTags::bsonObject,
                                              value::bitcastFrom<const char*>(lhsCws.scope),
                                              value::TypeTags::bsonObject,
                                              value::bitcastFrom<const char*>(rhsCws.scope));
        if (tag == value::TypeTags::NumberInt32) {
            auto result = op(value::bitcastTo<int32_t>(val), 0);
            return {value::TypeTags::Boolean, value::bitcastFrom<bool>(result)};
        }
    }

    return {value::TypeTags::Nothing, 0};
}

template <typename Op>
std::pair<value::TypeTags, value::Value> genericCompare(value::TypeTags lhsTag,
                                                        value::Value lhsValue,
                                                        value::TypeTags rhsTag,
                                                        value::Value rhsValue,
                                                        value::TypeTags collTag,
                                                        value::Value collValue,
                                                        Op op = {}) {
    if (collTag != value::TypeTags::collator) {
        return {value::TypeTags::Nothing, 0};
    }

    auto comparator =
        static_cast<StringData::ComparatorInterface*>(value::getCollatorView(collValue));

    return genericCompare(lhsTag, lhsValue, rhsTag, rhsValue, comparator, op);
}

struct Instruction {
    enum Tags {
        pushConstVal,
        pushAccessVal,
        pushMoveVal,
        pushLocalVal,
        pushMoveLocalVal,
        pushLocalLambda,
        pop,
        swap,

        add,
        sub,
        mul,
        div,
        idiv,
        mod,
        negate,
        numConvert,

        logicNot,

        less,
        lessEq,
        greater,
        greaterEq,
        eq,
        neq,

        // 3 way comparison (spaceship) with bson woCompare semantics.
        cmp3w,

        // collation-aware comparison instructions
        collLess,
        collLessEq,
        collGreater,
        collGreaterEq,
        collEq,
        collNeq,
        collCmp3w,

        fillEmpty,
        fillEmptyConst,
        getField,
        getFieldConst,
        getElement,
        collComparisonKey,
        getFieldOrElement,
        traverseP,  // traverse projection paths
        traversePConst,
        traverseF,  // traverse filter paths
        traverseFConst,
        setField,
        getArraySize,  // number of elements

        aggSum,
        aggMin,
        aggMax,
        aggFirst,
        aggLast,

        aggCollMin,
        aggCollMax,

        exists,
        isNull,
        isObject,
        isArray,
        isString,
        isNumber,
        isBinData,
        isDate,
        isNaN,
        isInfinity,
        isRecordId,
        isMinKey,
        isMaxKey,
        isTimestamp,

        function,
        functionSmall,

        jmp,  // offset is calculated from the end of instruction
        jmpTrue,
        jmpNothing,
        ret,  // used only by simple local lambdas

        fail,

        applyClassicMatcher,  // Instruction which calls into the classic engine MatchExpression.

        lastInstruction  // this is just a marker used to calculate number of instructions
    };

    enum Constants : uint8_t {
        Nothing,
        Null,
        False,
        True,
        Int32One,
    };

    static const char* toStringConstants(Constants k) {
        switch (k) {
            case Nothing:
                return "Nothing";
            case Null:
                return "Null";
            case True:
                return "True";
            case False:
                return "False";
            case Int32One:
                return "1";
            default:
                return "unknown";
        }
    }

    // Make sure that values in this arrays are always in-sync with the enum.
    static int stackOffset[];

    uint8_t tag;

    const char* toString() const {
        switch (tag) {
            case pushConstVal:
                return "pushConstVal";
            case pushAccessVal:
                return "pushAccessVal";
            case pushMoveVal:
                return "pushMoveVal";
            case pushLocalVal:
                return "pushLocalVal";
            case pushMoveLocalVal:
                return "pushMoveLocalVal";
            case pushLocalLambda:
                return "pushLocalLambda";
            case pop:
                return "pop";
            case swap:
                return "swap";
            case add:
                return "add";
            case sub:
                return "sub";
            case mul:
                return "mul";
            case div:
                return "div";
            case idiv:
                return "idiv";
            case mod:
                return "mod";
            case negate:
                return "negate";
            case numConvert:
                return "numConvert";
            case logicNot:
                return "logicNot";
            case less:
                return "less";
            case lessEq:
                return "lessEq";
            case greater:
                return "greater";
            case greaterEq:
                return "greaterEq";
            case eq:
                return "eq";
            case neq:
                return "neq";
            case cmp3w:
                return "cmp3w";
            case collLess:
                return "collLess";
            case collLessEq:
                return "collLessEq";
            case collGreater:
                return "collGreater";
            case collGreaterEq:
                return "collGreaterEq";
            case collEq:
                return "collEq";
            case collNeq:
                return "collNeq";
            case collCmp3w:
                return "collCmp3w";
            case fillEmpty:
                return "fillEmpty";
            case fillEmptyConst:
                return "fillEmptyConst";
            case getField:
                return "getField";
            case getFieldConst:
                return "getFieldConst";
            case getElement:
                return "getElement";
            case collComparisonKey:
                return "collComparisonKey";
            case getFieldOrElement:
                return "getFieldOrElement";
            case traverseP:
                return "traverseP";
            case traversePConst:
                return "traversePConst";
            case traverseF:
                return "traverseF";
            case traverseFConst:
                return "traverseFConst";
            case setField:
                return "setField";
            case getArraySize:
                return "getArraySize";
            case aggSum:
                return "aggSum";
            case aggMin:
                return "aggMin";
            case aggMax:
                return "aggMax";
            case aggFirst:
                return "aggFirst";
            case aggLast:
                return "aggLast";
            case aggCollMin:
                return "aggCollMin";
            case aggCollMax:
                return "aggCollMax";
            case exists:
                return "exists";
            case isNull:
                return "isNull";
            case isObject:
                return "isObject";
            case isArray:
                return "isArray";
            case isString:
                return "isString";
            case isNumber:
                return "isNumber";
            case isBinData:
                return "isBinData";
            case isDate:
                return "isDate";
            case isNaN:
                return "isNaN";
            case isInfinity:
                return "isInfinity";
            case isRecordId:
                return "isRecordId";
            case isMinKey:
                return "isMinKey";
            case isMaxKey:
                return "isMaxKey";
            case isTimestamp:
                return "isTimestamp";
            case function:
                return "function";
            case functionSmall:
                return "functionSmall";
            case jmp:
                return "jmp";
            case jmpTrue:
                return "jmpTrue";
            case jmpNothing:
                return "jmpNothing";
            case ret:
                return "ret";
            case fail:
                return "fail";
            case applyClassicMatcher:
                return "applyClassicMatcher";
            default:
                return "unrecognized";
        }
    }
};
static_assert(sizeof(Instruction) == sizeof(uint8_t));

enum class Builtin : uint8_t {
    split,
    regexMatch,
    replaceOne,
    dateDiff,
    dateParts,
    dateToParts,
    isoDateToParts,
    dayOfYear,
    dayOfMonth,
    dayOfWeek,
    datePartsWeekYear,
    dropFields,
    newArray,
    keepFields,
    newArrayFromRange,
    newObj,
    ksToString,  // KeyString to string
    newKs,       // new KeyString
    collNewKs,   // new KeyString (with collation)
    abs,         // absolute value
    ceil,
    floor,
    trunc,
    exp,
    ln,
    log10,
    sqrt,
    addToArray,        // agg function to append to an array
    addToArrayCapped,  // agg function to append to an array, fails when the array reaches specified
                       // size
    mergeObjects,      // agg function to merge BSON documents
    addToSet,          // agg function to append to a set
    addToSetCapped,    // agg function to append to a set, fails when the set reaches specified size
    collAddToSet,      // agg function to append to a set (with collation)
    collAddToSetCapped,  // agg function to append to a set (with collation), fails when the set
                         // reaches specified size
    doubleDoubleSum,     // special double summation
    aggDoubleDoubleSum,
    doubleDoubleSumFinalize,
    doubleDoublePartialSumFinalize,
    aggStdDev,
    stdDevPopFinalize,
    stdDevSampFinalize,
    bitTestZero,      // test bitwise mask & value is zero
    bitTestMask,      // test bitwise mask & value is mask
    bitTestPosition,  // test BinData with a bit position list
    bsonSize,         // implements $bsonSize
    toUpper,
    toLower,
    coerceToString,
    concat,
    acos,
    acosh,
    asin,
    asinh,
    atan,
    atanh,
    atan2,
    cos,
    cosh,
    degreesToRadians,
    radiansToDegrees,
    sin,
    sinh,
    tan,
    tanh,
    round,
    isMember,
    collIsMember,
    indexOfBytes,
    indexOfCP,
    isDayOfWeek,
    isTimeUnit,
    isTimezone,
    setUnion,
    setIntersection,
    setDifference,
    setEquals,
    collSetUnion,
    collSetIntersection,
    collSetDifference,
    collSetEquals,
    runJsPredicate,
    regexCompile,  // compile <pattern, options> into value::pcreRegex
    regexFind,
    regexFindAll,
    shardFilter,
    shardHash,
    extractSubArray,
    isArrayEmpty,
    reverseArray,
    sortArray,
    dateAdd,
    hasNullBytes,
    getRegexPattern,
    getRegexFlags,
    hash,
    ftsMatch,
    generateSortKey,
    makeBsonObj,
    tsSecond,
    tsIncrement,
    typeMatch,
    dateTrunc,
};

/**
 * This enum defines indices into an 'Array' that returns the partial sum result when 'needsMerge'
 * is requested.
 *
 * See 'builtinDoubleDoubleSumFinalize()' for more details.
 */
enum class AggPartialSumElems { kTotal, kError, kSizeOfArray };

/**
 * This enum defines indices into an 'Array' that accumulates $stdDevPop and $stdDevSamp results.
 *
 * The array contains 3 elements:
 * - The element at index `kCount` keeps track of the total number of values processd
 * - The elements at index `kRunningMean` keeps track of the mean of all the values that have been
 * processed.
 * - The elements at index `kRunningM2` keeps track of running M2 value (defined within:
 * https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Welford's_online_algorithm)
 * for all the values that have been processed.
 *
 * See 'aggStdDevImpl()'/'aggStdDev()'/'stdDevPopFinalize() / stdDevSampFinalize()' for more
 * details.
 */
enum AggStdDevValueElems {
    kCount,
    kRunningMean,
    kRunningM2,
    // This is actually not an index but represents the number of elements stored
    kSizeOfArray
};

/**
 * This enum defines indices into an 'Array' that returns the result of accumulators that track the
 * size of accumulated values, such as 'addToArrayCapped' and 'addToSetCapped'.
 */
enum class AggArrayWithSize { kValues = 0, kSizeOfValues, kLast = kSizeOfValues + 1 };

using SmallArityType = uint8_t;
using ArityType = uint32_t;

class CodeFragment {
public:
    auto& instrs() {
        return _instrs;
    }
    const auto& instrs() const {
        return _instrs;
    }
    auto stackSize() const {
        return _stackSize;
    }
    void removeFixup(FrameId frameId);

    void append(CodeFragment&& code);
    void appendNoStack(CodeFragment&& code);
    void append(CodeFragment&& lhs, CodeFragment&& rhs);
    void appendConstVal(value::TypeTags tag, value::Value val);
    void appendAccessVal(value::SlotAccessor* accessor);
    void appendMoveVal(value::SlotAccessor* accessor);
    void appendLocalVal(FrameId frameId, int stackOffset, bool moveFrom);
    void appendLocalLambda(int codePosition);
    void appendPop() {
        appendSimpleInstruction(Instruction::pop);
    }
    void appendSwap() {
        appendSimpleInstruction(Instruction::swap);
    }
    void appendAdd();
    void appendSub();
    void appendMul();
    void appendDiv();
    void appendIDiv();
    void appendMod();
    void appendNegate();
    void appendNot();
    void appendLess() {
        appendSimpleInstruction(Instruction::less);
    }
    void appendLessEq() {
        appendSimpleInstruction(Instruction::lessEq);
    }
    void appendGreater() {
        appendSimpleInstruction(Instruction::greater);
    }
    void appendGreaterEq() {
        appendSimpleInstruction(Instruction::greaterEq);
    }
    void appendEq() {
        appendSimpleInstruction(Instruction::eq);
    }
    void appendNeq() {
        appendSimpleInstruction(Instruction::neq);
    }
    void appendCmp3w() {
        appendSimpleInstruction(Instruction::cmp3w);
    }
    void appendCollLess() {
        appendSimpleInstruction(Instruction::collLess);
    }
    void appendCollLessEq() {
        appendSimpleInstruction(Instruction::collLessEq);
    }
    void appendCollGreater() {
        appendSimpleInstruction(Instruction::collGreater);
    }
    void appendCollGreaterEq() {
        appendSimpleInstruction(Instruction::collGreaterEq);
    }
    void appendCollEq() {
        appendSimpleInstruction(Instruction::collEq);
    }
    void appendCollNeq() {
        appendSimpleInstruction(Instruction::collNeq);
    }
    void appendCollCmp3w() {
        appendSimpleInstruction(Instruction::collCmp3w);
    }
    void appendFillEmpty() {
        appendSimpleInstruction(Instruction::fillEmpty);
    }
    void appendFillEmpty(Instruction::Constants k);
    void appendGetField();
    void appendGetField(value::TypeTags tag, value::Value val);
    void appendGetElement();
    void appendCollComparisonKey();
    void appendGetFieldOrElement();
    void appendTraverseP() {
        appendSimpleInstruction(Instruction::traverseP);
    }
    void appendTraverseP(int codePosition, Instruction::Constants k);
    void appendTraverseF() {
        appendSimpleInstruction(Instruction::traverseF);
    }
    void appendTraverseF(int codePosition, Instruction::Constants k);
    void appendSetField() {
        appendSimpleInstruction(Instruction::setField);
    }
    void appendGetArraySize();

    void appendSum();
    void appendMin();
    void appendMax();
    void appendFirst();
    void appendLast();
    void appendCollMin();
    void appendCollMax();
    void appendExists();
    void appendIsNull();
    void appendIsObject();
    void appendIsArray();
    void appendIsString();
    void appendIsNumber();
    void appendIsBinData();
    void appendIsDate();
    void appendIsNaN();
    void appendIsInfinity();
    void appendIsRecordId();
    void appendIsMinKey() {
        appendSimpleInstruction(Instruction::isMinKey);
    }
    void appendIsMaxKey() {
        appendSimpleInstruction(Instruction::isMaxKey);
    }
    void appendIsTimestamp() {
        appendSimpleInstruction(Instruction::isTimestamp);
    }
    void appendFunction(Builtin f, ArityType arity);
    void appendJump(int jumpOffset);
    void appendJumpTrue(int jumpOffset);
    void appendJumpNothing(int jumpOffset);
    void appendRet() {
        appendSimpleInstruction(Instruction::ret);
    }
    void appendFail() {
        appendSimpleInstruction(Instruction::fail);
    }
    void appendNumericConvert(value::TypeTags targetTag);
    void appendApplyClassicMatcher(const MatchExpression*);

    void fixup(int offset);

    // For printing from an interactive debugger.
    std::string toString() const;

private:
    void appendSimpleInstruction(Instruction::Tags tag);
    auto allocateSpace(size_t size) {
        auto oldSize = _instrs.size();
        _instrs.resize(oldSize + size);
        return _instrs.data() + oldSize;
    }

    void adjustStackSimple(const Instruction& i);
    void copyCodeAndFixup(CodeFragment&& from);

private:
    absl::InlinedVector<uint8_t, 16> _instrs;

    /**
     * Local variables bound by the let expressions live on the stack and are accessed by knowing an
     * offset from the top of the stack. As CodeFragments are appened together the offsets must be
     * fixed up to account for movement of the top of the stack.
     * The FixUp structure holds a "pointer" to the bytecode where we have to adjust the stack
     * offset.
     */
    struct FixUp {
        FrameId frameId;
        size_t offset;
    };
    std::vector<FixUp> _fixUps;

    size_t _stackSize{0};
};

namespace {
template <typename T>
T readFromMemory(const uint8_t* ptr) noexcept {
    static_assert(!IsEndian<T>::value);

    T val;
    memcpy(&val, ptr, sizeof(T));
    return val;
}

template <typename T>
size_t writeToMemory(uint8_t* ptr, const T val) noexcept {
    static_assert(!IsEndian<T>::value);

    memcpy(ptr, &val, sizeof(T));
    return sizeof(T);
}
}  // namespace

class ByteCode {
    static constexpr size_t sizeOfElement =
        sizeof(bool) + sizeof(value::TypeTags) + sizeof(value::Value);
    static_assert(sizeOfElement == 10);
    static_assert(std::is_trivially_copyable_v<FastTuple<bool, value::TypeTags, value::Value>>);

public:
    ByteCode() {
        _argStack = reinterpret_cast<uint8_t*>(mongoMalloc(sizeOfElement * 4));
        _argStackEnd = _argStack + sizeOfElement * 4;
        _argStackTop = _argStack - sizeOfElement;
    }

    ~ByteCode() {
        std::free(_argStack);
    }

    FastTuple<bool, value::TypeTags, value::Value> run(const CodeFragment* code);
    bool runPredicate(const CodeFragment* code);

private:
    void runInternal(const CodeFragment* code, int64_t position);
    void runLambdaInternal(const CodeFragment* code, int64_t position);

    MONGO_COMPILER_NORETURN void runFailInstruction();
    void runClassicMatcher(const MatchExpression* matcher);

    template <typename T>
    void runTagCheck(T&& predicate);
    void runTagCheck(value::TypeTags tagRhs);

    FastTuple<bool, value::TypeTags, value::Value> genericDiv(value::TypeTags lhsTag,
                                                              value::Value lhsValue,
                                                              value::TypeTags rhsTag,
                                                              value::Value rhsValue);
    FastTuple<bool, value::TypeTags, value::Value> genericIDiv(value::TypeTags lhsTag,
                                                               value::Value lhsValue,
                                                               value::TypeTags rhsTag,
                                                               value::Value rhsValue);
    FastTuple<bool, value::TypeTags, value::Value> genericMod(value::TypeTags lhsTag,
                                                              value::Value lhsValue,
                                                              value::TypeTags rhsTag,
                                                              value::Value rhsValue);
    FastTuple<bool, value::TypeTags, value::Value> genericAbs(value::TypeTags operandTag,
                                                              value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericCeil(value::TypeTags operandTag,
                                                               value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericFloor(value::TypeTags operandTag,
                                                                value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericTrunc(value::TypeTags operandTag,
                                                                value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericExp(value::TypeTags operandTag,
                                                              value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericLn(value::TypeTags operandTag,
                                                             value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericLog10(value::TypeTags operandTag,
                                                                value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericSqrt(value::TypeTags operandTag,
                                                               value::Value operandValue);
    std::pair<value::TypeTags, value::Value> genericNot(value::TypeTags tag, value::Value value);
    std::pair<value::TypeTags, value::Value> genericIsMember(value::TypeTags lhsTag,
                                                             value::Value lhsVal,
                                                             value::TypeTags rhsTag,
                                                             value::Value rhsVal,
                                                             CollatorInterface* collator = nullptr);
    std::pair<value::TypeTags, value::Value> genericIsMember(value::TypeTags lhsTag,
                                                             value::Value lhsVal,
                                                             value::TypeTags rhsTag,
                                                             value::Value rhsVal,
                                                             value::TypeTags collTag,
                                                             value::Value collVal);
    FastTuple<bool, value::TypeTags, value::Value> genericNumConvert(value::TypeTags lhsTag,
                                                                     value::Value lhsValue,
                                                                     value::TypeTags rhsTag);

    std::pair<value::TypeTags, value::Value> compare3way(
        value::TypeTags lhsTag,
        value::Value lhsValue,
        value::TypeTags rhsTag,
        value::Value rhsValue,
        const StringData::ComparatorInterface* comparator = nullptr);

    std::pair<value::TypeTags, value::Value> compare3way(value::TypeTags lhsTag,
                                                         value::Value lhsValue,
                                                         value::TypeTags rhsTag,
                                                         value::Value rhsValue,
                                                         value::TypeTags collTag,
                                                         value::Value collValue);

    FastTuple<bool, value::TypeTags, value::Value> getField(value::TypeTags objTag,
                                                            value::Value objValue,
                                                            value::TypeTags fieldTag,
                                                            value::Value fieldValue);

    FastTuple<bool, value::TypeTags, value::Value> getField(value::TypeTags objTag,
                                                            value::Value objValue,
                                                            StringData fieldStr);

    FastTuple<bool, value::TypeTags, value::Value> getElement(value::TypeTags objTag,
                                                              value::Value objValue,
                                                              value::TypeTags fieldTag,
                                                              value::Value fieldValue);
    FastTuple<bool, value::TypeTags, value::Value> getFieldOrElement(value::TypeTags objTag,
                                                                     value::Value objValue,
                                                                     value::TypeTags fieldTag,
                                                                     value::Value fieldValue);

    void traverseP(const CodeFragment* code);
    void traverseP(const CodeFragment* code, int64_t position, int64_t maxDepth);
    void traverseP_nested(const CodeFragment* code,
                          int64_t position,
                          value::TypeTags tag,
                          value::Value val,
                          int64_t maxDepth);

    void traverseF(const CodeFragment* code);
    void traverseF(const CodeFragment* code, int64_t position, bool compareArray);
    void traverseFInArray(const CodeFragment* code, int64_t position, bool compareArray);
    FastTuple<bool, value::TypeTags, value::Value> setField();

    FastTuple<bool, value::TypeTags, value::Value> getArraySize(value::TypeTags tag,
                                                                value::Value val);

    FastTuple<bool, value::TypeTags, value::Value> aggSum(value::TypeTags accTag,
                                                          value::Value accValue,
                                                          value::TypeTags fieldTag,
                                                          value::Value fieldValue);

    void aggDoubleDoubleSumImpl(value::Array* arr, value::TypeTags rhsTag, value::Value rhsValue);

    // This is an implementation of the following algorithm:
    // https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Welford's_online_algorithm
    void aggStdDevImpl(value::Array* arr, value::TypeTags rhsTag, value::Value rhsValue);

    FastTuple<bool, value::TypeTags, value::Value> aggStdDevFinalizeImpl(value::Value fieldValue,
                                                                         bool isSamp);

    FastTuple<bool, value::TypeTags, value::Value> aggMin(value::TypeTags accTag,
                                                          value::Value accValue,
                                                          value::TypeTags fieldTag,
                                                          value::Value fieldValue,
                                                          CollatorInterface* collator = nullptr);

    FastTuple<bool, value::TypeTags, value::Value> aggMax(value::TypeTags accTag,
                                                          value::Value accValue,
                                                          value::TypeTags fieldTag,
                                                          value::Value fieldValue,
                                                          CollatorInterface* collator = nullptr);

    FastTuple<bool, value::TypeTags, value::Value> aggFirst(value::TypeTags accTag,
                                                            value::Value accValue,
                                                            value::TypeTags fieldTag,
                                                            value::Value fieldValue);

    FastTuple<bool, value::TypeTags, value::Value> aggLast(value::TypeTags accTag,
                                                           value::Value accValue,
                                                           value::TypeTags fieldTag,
                                                           value::Value fieldValue);

    FastTuple<bool, value::TypeTags, value::Value> genericAcos(value::TypeTags operandTag,
                                                               value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericAcosh(value::TypeTags operandTag,
                                                                value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericAsin(value::TypeTags operandTag,
                                                               value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericAsinh(value::TypeTags operandTag,
                                                                value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericAtan(value::TypeTags operandTag,
                                                               value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericAtanh(value::TypeTags operandTag,
                                                                value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericAtan2(value::TypeTags operandTag1,
                                                                value::Value operandValue1,
                                                                value::TypeTags operandTag2,
                                                                value::Value operandValue2);
    FastTuple<bool, value::TypeTags, value::Value> genericCos(value::TypeTags operandTag,
                                                              value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericCosh(value::TypeTags operandTag,
                                                               value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericDegreesToRadians(
        value::TypeTags operandTag, value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericRadiansToDegrees(
        value::TypeTags operandTag, value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericSin(value::TypeTags operandTag,
                                                              value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericSinh(value::TypeTags operandTag,
                                                               value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericTan(value::TypeTags operandTag,
                                                              value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericTanh(value::TypeTags operandTag,
                                                               value::Value operandValue);

    FastTuple<bool, value::TypeTags, value::Value> genericDayOfYear(value::TypeTags timezoneDBTag,
                                                                    value::Value timezoneDBValue,
                                                                    value::TypeTags dateTag,
                                                                    value::Value dateValue,
                                                                    value::TypeTags timezoneTag,
                                                                    value::Value timezoneValue);
    FastTuple<bool, value::TypeTags, value::Value> genericDayOfMonth(value::TypeTags timezoneDBTag,
                                                                     value::Value timezoneDBValue,
                                                                     value::TypeTags dateTag,
                                                                     value::Value dateValue,
                                                                     value::TypeTags timezoneTag,
                                                                     value::Value timezoneValue);
    FastTuple<bool, value::TypeTags, value::Value> genericDayOfWeek(value::TypeTags timezoneDBTag,
                                                                    value::Value timezoneDBValue,
                                                                    value::TypeTags dateTag,
                                                                    value::Value dateValue,
                                                                    value::TypeTags timezoneTag,
                                                                    value::Value timezoneValue);
    FastTuple<bool, value::TypeTags, value::Value> genericNewKeyString(
        ArityType arity, CollatorInterface* collator = nullptr);

    FastTuple<bool, value::TypeTags, value::Value> builtinSplit(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinDate(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinDateWeekYear(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinDateDiff(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinDateToParts(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinIsoDateToParts(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinDayOfYear(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinDayOfMonth(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinDayOfWeek(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinRegexMatch(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinKeepFields(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinReplaceOne(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinDropFields(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinNewArray(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinNewArrayFromRange(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinNewObj(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinKeyStringToString(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinNewKeyString(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCollNewKeyString(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAbs(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCeil(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinFloor(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinTrunc(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinExp(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinLn(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinLog10(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinSqrt(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAddToArray(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAddToArrayCapped(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinMergeObjects(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAddToSet(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCollAddToSet(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> addToSetCappedImpl(value::TypeTags tagNewElem,
                                                                      value::Value valNewElem,
                                                                      int32_t sizeCap,
                                                                      CollatorInterface* collator);
    FastTuple<bool, value::TypeTags, value::Value> builtinAddToSetCapped(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCollAddToSetCapped(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinDoubleDoubleSum(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggDoubleDoubleSum(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinDoubleDoubleSumFinalize(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinDoubleDoublePartialSumFinalize(
        ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggStdDev(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinStdDevPopFinalize(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinStdDevSampFinalize(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinBitTestZero(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinBitTestMask(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinBitTestPosition(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinBsonSize(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinToUpper(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinToLower(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCoerceToString(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAcos(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAcosh(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAsin(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAsinh(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAtan(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAtanh(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAtan2(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCos(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCosh(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinDegreesToRadians(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinRadiansToDegrees(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinSin(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinSinh(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinTan(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinTanh(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinRound(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinConcat(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinIsMember(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCollIsMember(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinIndexOfBytes(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinIndexOfCP(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinIsDayOfWeek(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinIsTimeUnit(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinIsTimezone(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinSetUnion(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinSetIntersection(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinSetDifference(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinSetEquals(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCollSetUnion(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCollSetIntersection(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCollSetDifference(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCollSetEquals(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinRunJsPredicate(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinRegexCompile(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinRegexFind(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinRegexFindAll(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinShardFilter(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinShardHash(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinExtractSubArray(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinIsArrayEmpty(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinReverseArray(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinSortArray(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinDateAdd(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinHasNullBytes(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinGetRegexPattern(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinGetRegexFlags(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinHash(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinFtsMatch(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinGenerateSortKey(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinMakeBsonObj(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinTsSecond(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinTsIncrement(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinTypeMatch(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinDateTrunc(ArityType arity);

    FastTuple<bool, value::TypeTags, value::Value> dispatchBuiltin(Builtin f, ArityType arity);

    static constexpr size_t offsetOwned = 0;
    static constexpr size_t offsetTag = 1;
    static constexpr size_t offsetVal = 2;

    MONGO_COMPILER_ALWAYS_INLINE FastTuple<bool, value::TypeTags, value::Value> readTuple(
        uint8_t* ptr) noexcept {
        auto owned = readFromMemory<bool>(ptr + offsetOwned);
        auto tag = readFromMemory<value::TypeTags>(ptr + offsetTag);
        auto val = readFromMemory<value::Value>(ptr + offsetVal);
        return {owned, tag, val};
    }

    MONGO_COMPILER_ALWAYS_INLINE void writeTuple(uint8_t* ptr,
                                                 bool owned,
                                                 value::TypeTags tag,
                                                 value::Value val) noexcept {
        writeToMemory(ptr + offsetOwned, owned);
        writeToMemory(ptr + offsetTag, tag);
        writeToMemory(ptr + offsetVal, val);
    }
    MONGO_COMPILER_ALWAYS_INLINE FastTuple<bool, value::TypeTags, value::Value> getFromStack(
        size_t offset) noexcept {
        if (MONGO_likely(offset == 0)) {
            return readTuple(_argStackTop);
        } else {
            return readTuple(_argStackTop - offset * sizeOfElement);
        }
    }

    MONGO_COMPILER_ALWAYS_INLINE FastTuple<bool, value::TypeTags, value::Value> moveFromStack(
        size_t offset) noexcept {
        if (MONGO_likely(offset == 0)) {
            auto [owned, tag, val] = readTuple(_argStackTop);
            writeToMemory(_argStackTop + offsetOwned, false);
            return {owned, tag, val};
        } else {
            auto ptr = _argStackTop - offset * sizeOfElement;
            auto [owned, tag, val] = readTuple(ptr);
            writeToMemory(ptr + offsetOwned, false);
            return {owned, tag, val};
        }
    }

    MONGO_COMPILER_ALWAYS_INLINE std::pair<value::TypeTags, value::Value> moveOwnedFromStack(
        size_t offset) {
        auto [owned, tag, val] = moveFromStack(offset);
        if (!owned) {
            std::tie(tag, val) = value::copyValue(tag, val);
        }

        return {tag, val};
    }

    MONGO_COMPILER_ALWAYS_INLINE void setStack(size_t offset,
                                               bool owned,
                                               value::TypeTags tag,
                                               value::Value val) noexcept {
        if (MONGO_likely(offset == 0)) {
            topStack(owned, tag, val);
        } else {
            writeTuple(_argStackTop - offset * sizeOfElement, owned, tag, val);
        }
    }

    MONGO_COMPILER_ALWAYS_INLINE void pushStack(bool owned,
                                                value::TypeTags tag,
                                                value::Value val) noexcept {
        _argStackTop += sizeOfElement;
        if (MONGO_unlikely(_argStackTop == _argStackEnd)) {
            growAndResize();
        }
        topStack(owned, tag, val);
    }

    MONGO_COMPILER_ALWAYS_INLINE void topStack(bool owned,
                                               value::TypeTags tag,
                                               value::Value val) noexcept {
        writeTuple(_argStackTop, owned, tag, val);
    }

    MONGO_COMPILER_ALWAYS_INLINE void popStack() noexcept {
        _argStackTop -= sizeOfElement;
    }

    MONGO_COMPILER_ALWAYS_INLINE void popAndReleaseStack() noexcept {
        auto [owned, tag, val] = getFromStack(0);
        if (owned) {
            value::releaseValue(tag, val);
        }

        popStack();
    }

    void stackReset() noexcept {
        _argStackTop = _argStack - sizeOfElement;
    }

    void growAndResize() noexcept;

    void swapStack();

    uint8_t* _argStackTop{nullptr};
    uint8_t* _argStack{nullptr};
    uint8_t* _argStackEnd{nullptr};
};
}  // namespace vm
}  // namespace sbe
}  // namespace mongo
