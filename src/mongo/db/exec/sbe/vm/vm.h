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

#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"

namespace mongo {
namespace sbe {
namespace vm {
template <typename Op>
std::pair<value::TypeTags, value::Value> genericNumericCompare(value::TypeTags lhsTag,
                                                               value::Value lhsValue,
                                                               value::TypeTags rhsTag,
                                                               value::Value rhsValue,
                                                               Op op) {

    if (value::isNumber(lhsTag) && value::isNumber(rhsTag)) {
        switch (getWidestNumericalType(lhsTag, rhsTag)) {
            case value::TypeTags::NumberInt32: {
                auto result = op(value::numericCast<int32_t>(lhsTag, lhsValue),
                                 value::numericCast<int32_t>(rhsTag, rhsValue));
                return {value::TypeTags::Boolean, value::bitcastFrom(result)};
            }
            case value::TypeTags::NumberInt64: {
                auto result = op(value::numericCast<int64_t>(lhsTag, lhsValue),
                                 value::numericCast<int64_t>(rhsTag, rhsValue));
                return {value::TypeTags::Boolean, value::bitcastFrom(result)};
            }
            case value::TypeTags::NumberDouble: {
                auto result = op(value::numericCast<double>(lhsTag, lhsValue),
                                 value::numericCast<double>(rhsTag, rhsValue));
                return {value::TypeTags::Boolean, value::bitcastFrom(result)};
            }
            case value::TypeTags::NumberDecimal: {
                auto result = op(value::numericCast<Decimal128>(lhsTag, lhsValue),
                                 value::numericCast<Decimal128>(rhsTag, rhsValue));
                return {value::TypeTags::Boolean, value::bitcastFrom(result)};
            }
            default:
                MONGO_UNREACHABLE;
        }
    } else if (isString(lhsTag) && isString(rhsTag)) {
        auto lhsStr = getStringView(lhsTag, lhsValue);
        auto rhsStr = getStringView(rhsTag, rhsValue);
        auto result = op(lhsStr.compare(rhsStr), 0);
        return {value::TypeTags::Boolean, value::bitcastFrom(result)};
    } else if (lhsTag == value::TypeTags::Date && rhsTag == value::TypeTags::Date) {
        auto result = op(value::bitcastTo<int64_t>(lhsValue), value::bitcastTo<int64_t>(rhsValue));
        return {value::TypeTags::Boolean, value::bitcastFrom(result)};
    } else if (lhsTag == value::TypeTags::Timestamp && rhsTag == value::TypeTags::Timestamp) {
        auto result =
            op(value::bitcastTo<uint64_t>(lhsValue), value::bitcastTo<uint64_t>(rhsValue));
        return {value::TypeTags::Boolean, value::bitcastFrom(result)};
    } else if (lhsTag == value::TypeTags::Boolean && rhsTag == value::TypeTags::Boolean) {
        auto result = op(lhsValue != 0, rhsValue != 0);
        return {value::TypeTags::Boolean, value::bitcastFrom(result)};
    } else if (lhsTag == value::TypeTags::Null && rhsTag == value::TypeTags::Null) {
        // This is where Mongo differs from SQL.
        auto result = op(0, 0);
        return {value::TypeTags::Boolean, value::bitcastFrom(result)};
    }

    return {value::TypeTags::Nothing, 0};
}

struct Instruction {
    enum Tags {
        pushConstVal,
        pushAccessVal,
        pushMoveVal,
        pushLocalVal,
        pop,
        swap,

        add,
        sub,
        mul,
        div,
        negate,

        logicNot,

        less,
        lessEq,
        greater,
        greaterEq,
        eq,
        neq,

        // 3 way comparison (spaceship) with bson woCompare semantics.
        cmp3w,

        fillEmpty,

        getField,

        aggSum,
        aggMin,
        aggMax,
        aggFirst,
        aggLast,

        exists,
        isNull,
        isObject,
        isArray,
        isString,
        isNumber,

        function,

        jmp,  // offset is calculated from the end of instruction
        jmpTrue,
        jmpNothing,

        fail,

        lastInstruction  // this is just a marker used to calculate number of instructions
    };

    // Make sure that values in this arrays are always in-sync with the enum.
    static int stackOffset[];

    uint8_t tag;
};
static_assert(sizeof(Instruction) == sizeof(uint8_t));

enum class Builtin : uint8_t {
    split,
    regexMatch,
    dropFields,
    newObj,
    ksToString,  // KeyString to string
    newKs,       // new KeyString
    abs,         // absolute value
    addToArray,  // agg function to append to an array
    addToSet,    // agg function to append to a set
};

class CodeFragment {
public:
    auto& instrs() {
        return _instrs;
    }
    auto stackSize() const {
        return _stackSize;
    }
    void removeFixup(FrameId frameId);

    void append(std::unique_ptr<CodeFragment> code);
    void append(std::unique_ptr<CodeFragment> lhs, std::unique_ptr<CodeFragment> rhs);
    void appendConstVal(value::TypeTags tag, value::Value val);
    void appendAccessVal(value::SlotAccessor* accessor);
    void appendMoveVal(value::SlotAccessor* accessor);
    void appendLocalVal(FrameId frameId, int stackOffset);
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
    void appendFillEmpty() {
        appendSimpleInstruction(Instruction::fillEmpty);
    }
    void appendGetField();
    void appendSum();
    void appendMin();
    void appendMax();
    void appendFirst();
    void appendLast();
    void appendExists();
    void appendIsNull();
    void appendIsObject();
    void appendIsArray();
    void appendIsString();
    void appendIsNumber();
    void appendFunction(Builtin f, uint8_t arity);
    void appendJump(int jumpOffset);
    void appendJumpTrue(int jumpOffset);
    void appendJumpNothing(int jumpOffset);
    void appendFail() {
        appendSimpleInstruction(Instruction::fail);
    }

private:
    void appendSimpleInstruction(Instruction::Tags tag);
    auto allocateSpace(size_t size) {
        auto oldSize = _instrs.size();
        _instrs.resize(oldSize + size);
        return _instrs.data() + oldSize;
    }

    void adjustStackSimple(const Instruction& i);
    void fixup(int offset);
    void copyCodeAndFixup(const CodeFragment& from);

    std::vector<uint8_t> _instrs;

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

    int _stackSize{0};
};

class ByteCode {
public:
    ~ByteCode();

    std::tuple<uint8_t, value::TypeTags, value::Value> run(CodeFragment* code);
    bool runPredicate(CodeFragment* code);

private:
    std::vector<uint8_t> _argStackOwned;
    std::vector<value::TypeTags> _argStackTags;
    std::vector<value::Value> _argStackVals;

    std::tuple<bool, value::TypeTags, value::Value> genericAdd(value::TypeTags lhsTag,
                                                               value::Value lhsValue,
                                                               value::TypeTags rhsTag,
                                                               value::Value rhsValue);
    std::tuple<bool, value::TypeTags, value::Value> genericSub(value::TypeTags lhsTag,
                                                               value::Value lhsValue,
                                                               value::TypeTags rhsTag,
                                                               value::Value rhsValue);
    std::tuple<bool, value::TypeTags, value::Value> genericMul(value::TypeTags lhsTag,
                                                               value::Value lhsValue,
                                                               value::TypeTags rhsTag,
                                                               value::Value rhsValue);
    std::tuple<bool, value::TypeTags, value::Value> genericDiv(value::TypeTags lhsTag,
                                                               value::Value lhsValue,
                                                               value::TypeTags rhsTag,
                                                               value::Value rhsValue);
    std::tuple<bool, value::TypeTags, value::Value> genericAbs(value::TypeTags operandTag,
                                                               value::Value operandValue);
    std::tuple<bool, value::TypeTags, value::Value> genericNot(value::TypeTags tag,
                                                               value::Value value);
    template <typename Op>
    std::pair<value::TypeTags, value::Value> genericCompare(value::TypeTags lhsTag,
                                                            value::Value lhsValue,
                                                            value::TypeTags rhsTag,
                                                            value::Value rhsValue,
                                                            Op op = {}) {
        return genericNumericCompare(lhsTag, lhsValue, rhsTag, rhsValue, op);
    }

    std::pair<value::TypeTags, value::Value> genericCompareEq(value::TypeTags lhsTag,
                                                              value::Value lhsValue,
                                                              value::TypeTags rhsTag,
                                                              value::Value rhsValue);

    std::pair<value::TypeTags, value::Value> genericCompareNeq(value::TypeTags lhsTag,
                                                               value::Value lhsValue,
                                                               value::TypeTags rhsTag,
                                                               value::Value rhsValue);

    std::pair<value::TypeTags, value::Value> compare3way(value::TypeTags lhsTag,
                                                         value::Value lhsValue,
                                                         value::TypeTags rhsTag,
                                                         value::Value rhsValue);

    std::tuple<bool, value::TypeTags, value::Value> getField(value::TypeTags objTag,
                                                             value::Value objValue,
                                                             value::TypeTags fieldTag,
                                                             value::Value fieldValue);

    std::tuple<bool, value::TypeTags, value::Value> aggSum(value::TypeTags accTag,
                                                           value::Value accValue,
                                                           value::TypeTags fieldTag,
                                                           value::Value fieldValue);

    std::tuple<bool, value::TypeTags, value::Value> aggMin(value::TypeTags accTag,
                                                           value::Value accValue,
                                                           value::TypeTags fieldTag,
                                                           value::Value fieldValue);

    std::tuple<bool, value::TypeTags, value::Value> aggMax(value::TypeTags accTag,
                                                           value::Value accValue,
                                                           value::TypeTags fieldTag,
                                                           value::Value fieldValue);

    std::tuple<bool, value::TypeTags, value::Value> aggFirst(value::TypeTags accTag,
                                                             value::Value accValue,
                                                             value::TypeTags fieldTag,
                                                             value::Value fieldValue);

    std::tuple<bool, value::TypeTags, value::Value> aggLast(value::TypeTags accTag,
                                                            value::Value accValue,
                                                            value::TypeTags fieldTag,
                                                            value::Value fieldValue);

    std::tuple<bool, value::TypeTags, value::Value> builtinSplit(uint8_t arity);
    std::tuple<bool, value::TypeTags, value::Value> builtinRegexMatch(uint8_t arity);
    std::tuple<bool, value::TypeTags, value::Value> builtinDropFields(uint8_t arity);
    std::tuple<bool, value::TypeTags, value::Value> builtinNewObj(uint8_t arity);
    std::tuple<bool, value::TypeTags, value::Value> builtinKeyStringToString(uint8_t arity);
    std::tuple<bool, value::TypeTags, value::Value> builtinNewKeyString(uint8_t arity);
    std::tuple<bool, value::TypeTags, value::Value> builtinAbs(uint8_t arity);
    std::tuple<bool, value::TypeTags, value::Value> builtinAddToArray(uint8_t arity);
    std::tuple<bool, value::TypeTags, value::Value> builtinAddToSet(uint8_t arity);

    std::tuple<bool, value::TypeTags, value::Value> dispatchBuiltin(Builtin f, uint8_t arity);

    std::tuple<bool, value::TypeTags, value::Value> getFromStack(size_t offset) {
        auto backOffset = _argStackOwned.size() - 1 - offset;
        auto owned = _argStackOwned[backOffset];
        auto tag = _argStackTags[backOffset];
        auto val = _argStackVals[backOffset];

        return {owned, tag, val};
    }

    void setStack(size_t offset, bool owned, value::TypeTags tag, value::Value val) {
        auto backOffset = _argStackOwned.size() - 1 - offset;
        _argStackOwned[backOffset] = owned;
        _argStackTags[backOffset] = tag;
        _argStackVals[backOffset] = val;
    }

    void pushStack(bool owned, value::TypeTags tag, value::Value val) {
        _argStackOwned.push_back(owned);
        _argStackTags.push_back(tag);
        _argStackVals.push_back(val);
    }

    void topStack(bool owned, value::TypeTags tag, value::Value val) {
        _argStackOwned.back() = owned;
        _argStackTags.back() = tag;
        _argStackVals.back() = val;
    }

    void popStack() {
        _argStackOwned.pop_back();
        _argStackTags.pop_back();
        _argStackVals.pop_back();
    }
};
}  // namespace vm
}  // namespace sbe
}  // namespace mongo
