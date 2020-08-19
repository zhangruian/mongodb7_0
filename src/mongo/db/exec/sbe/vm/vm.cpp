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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/sbe/vm/vm.h"

#include <pcrecpp.h>

#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/summation.h"

MONGO_FAIL_POINT_DEFINE(failOnPoisonedFieldLookup);

namespace mongo {
namespace sbe {
namespace vm {

/*
 * This table must be kept in sync with Instruction::Tags. It encodes how the instruction affects
 * the stack; i.e. push(+1), pop(-1), or no effect.
 */
int Instruction::stackOffset[Instruction::Tags::lastInstruction] = {
    1,   // pushConstVal
    1,   // pushAccessVal
    1,   // pushMoveVal
    1,   // pushLocalVal
    -1,  // pop
    0,   // swap

    -1,  // add
    -1,  // sub
    -1,  // mul
    -1,  // div
    -1,  // idiv
    -1,  // mod
    0,   // negate
    0,   // numConvert

    0,  // logicNot

    -1,  // less
    -1,  // lessEq
    -1,  // greater
    -1,  // greaterEq
    -1,  // eq
    -1,  // neq
    -1,  // cmp3w

    -1,  // fillEmpty

    -1,  // getField
    -1,  // getElement

    -1,  // sum
    -1,  // min
    -1,  // max
    -1,  // first
    -1,  // last

    0,  // exists
    0,  // isNull
    0,  // isObject
    0,  // isArray
    0,  // isString
    0,  // isNumber
    0,  // typeMatch

    0,  // function is special, the stack offset is encoded in the instruction itself

    0,   // jmp
    -1,  // jmpTrue
    0,   // jmpNothing

    -1,  // fail
};

void CodeFragment::adjustStackSimple(const Instruction& i) {
    _stackSize += Instruction::stackOffset[i.tag];
}

void CodeFragment::fixup(int offset) {
    for (auto fixUp : _fixUps) {
        auto ptr = instrs().data() + fixUp.offset;
        int newOffset = value::readFromMemory<int>(ptr) + offset;
        value::writeToMemory(ptr, newOffset);
    }
}

void CodeFragment::removeFixup(FrameId frameId) {
    _fixUps.erase(std::remove_if(_fixUps.begin(),
                                 _fixUps.end(),
                                 [frameId](const auto& f) { return f.frameId == frameId; }),
                  _fixUps.end());
}

void CodeFragment::copyCodeAndFixup(const CodeFragment& from) {
    for (auto fixUp : from._fixUps) {
        fixUp.offset += _instrs.size();
        _fixUps.push_back(fixUp);
    }

    _instrs.insert(_instrs.end(), from._instrs.begin(), from._instrs.end());
}

void CodeFragment::append(std::unique_ptr<CodeFragment> code) {
    // Fixup before copying.
    code->fixup(_stackSize);

    copyCodeAndFixup(*code);

    _stackSize += code->_stackSize;
}

void CodeFragment::append(std::unique_ptr<CodeFragment> lhs, std::unique_ptr<CodeFragment> rhs) {
    invariant(lhs->stackSize() == rhs->stackSize());

    // Fixup before copying.
    lhs->fixup(_stackSize);
    rhs->fixup(_stackSize);

    copyCodeAndFixup(*lhs);
    copyCodeAndFixup(*rhs);

    _stackSize += lhs->_stackSize;
}

void CodeFragment::appendConstVal(value::TypeTags tag, value::Value val) {
    Instruction i;
    i.tag = Instruction::pushConstVal;
    adjustStackSimple(i);

    auto offset = allocateSpace(sizeof(Instruction) + sizeof(tag) + sizeof(val));

    offset += value::writeToMemory(offset, i);
    offset += value::writeToMemory(offset, tag);
    offset += value::writeToMemory(offset, val);
}

void CodeFragment::appendAccessVal(value::SlotAccessor* accessor) {
    Instruction i;
    i.tag = Instruction::pushAccessVal;
    adjustStackSimple(i);

    auto offset = allocateSpace(sizeof(Instruction) + sizeof(accessor));

    offset += value::writeToMemory(offset, i);
    offset += value::writeToMemory(offset, accessor);
}

void CodeFragment::appendMoveVal(value::SlotAccessor* accessor) {
    Instruction i;
    i.tag = Instruction::pushMoveVal;
    adjustStackSimple(i);

    auto offset = allocateSpace(sizeof(Instruction) + sizeof(accessor));

    offset += value::writeToMemory(offset, i);
    offset += value::writeToMemory(offset, accessor);
}

void CodeFragment::appendLocalVal(FrameId frameId, int stackOffset) {
    Instruction i;
    i.tag = Instruction::pushLocalVal;
    adjustStackSimple(i);

    auto fixUpOffset = _instrs.size() + sizeof(Instruction);
    _fixUps.push_back(FixUp{frameId, fixUpOffset});

    auto offset = allocateSpace(sizeof(Instruction) + sizeof(stackOffset));

    offset += value::writeToMemory(offset, i);
    offset += value::writeToMemory(offset, stackOffset);
}

void CodeFragment::appendAdd() {
    appendSimpleInstruction(Instruction::add);
}

void CodeFragment::appendNumericConvert(value::TypeTags targetTag) {
    Instruction i;
    i.tag = Instruction::numConvert;
    adjustStackSimple(i);

    auto offset = allocateSpace(sizeof(Instruction) + sizeof(targetTag));

    offset += value::writeToMemory(offset, i);
    offset += value::writeToMemory(offset, targetTag);
}

void CodeFragment::appendSub() {
    appendSimpleInstruction(Instruction::sub);
}

void CodeFragment::appendMul() {
    appendSimpleInstruction(Instruction::mul);
}

void CodeFragment::appendDiv() {
    appendSimpleInstruction(Instruction::div);
}

void CodeFragment::appendIDiv() {
    appendSimpleInstruction(Instruction::idiv);
}

void CodeFragment::appendMod() {
    appendSimpleInstruction(Instruction::mod);
}

void CodeFragment::appendNegate() {
    appendSimpleInstruction(Instruction::negate);
}

void CodeFragment::appendNot() {
    appendSimpleInstruction(Instruction::logicNot);
}

void CodeFragment::appendSimpleInstruction(Instruction::Tags tag) {
    Instruction i;
    i.tag = tag;
    adjustStackSimple(i);

    auto offset = allocateSpace(sizeof(Instruction));

    offset += value::writeToMemory(offset, i);
}

void CodeFragment::appendGetField() {
    appendSimpleInstruction(Instruction::getField);
}

void CodeFragment::appendGetElement() {
    appendSimpleInstruction(Instruction::getElement);
}

void CodeFragment::appendSum() {
    appendSimpleInstruction(Instruction::aggSum);
}

void CodeFragment::appendMin() {
    appendSimpleInstruction(Instruction::aggMin);
}

void CodeFragment::appendMax() {
    appendSimpleInstruction(Instruction::aggMax);
}

void CodeFragment::appendFirst() {
    appendSimpleInstruction(Instruction::aggFirst);
}

void CodeFragment::appendLast() {
    appendSimpleInstruction(Instruction::aggLast);
}

void CodeFragment::appendExists() {
    appendSimpleInstruction(Instruction::exists);
}

void CodeFragment::appendIsNull() {
    appendSimpleInstruction(Instruction::isNull);
}

void CodeFragment::appendIsObject() {
    appendSimpleInstruction(Instruction::isObject);
}

void CodeFragment::appendIsArray() {
    appendSimpleInstruction(Instruction::isArray);
}

void CodeFragment::appendIsString() {
    appendSimpleInstruction(Instruction::isString);
}

void CodeFragment::appendIsNumber() {
    appendSimpleInstruction(Instruction::isNumber);
}

void CodeFragment::appendTypeMatch(uint32_t typeMask) {
    Instruction i;
    i.tag = Instruction::typeMatch;
    adjustStackSimple(i);

    auto offset = allocateSpace(sizeof(Instruction) + sizeof(typeMask));

    offset += value::writeToMemory(offset, i);
    offset += value::writeToMemory(offset, typeMask);
}

void CodeFragment::appendFunction(Builtin f, uint8_t arity) {
    Instruction i;
    i.tag = Instruction::function;

    // Account for consumed arguments
    _stackSize -= arity;
    // and the return value.
    _stackSize += 1;

    auto offset = allocateSpace(sizeof(Instruction) + sizeof(f) + sizeof(arity));

    offset += value::writeToMemory(offset, i);
    offset += value::writeToMemory(offset, f);
    offset += value::writeToMemory(offset, arity);
}

void CodeFragment::appendJump(int jumpOffset) {
    Instruction i;
    i.tag = Instruction::jmp;
    adjustStackSimple(i);

    auto offset = allocateSpace(sizeof(Instruction) + sizeof(jumpOffset));

    offset += value::writeToMemory(offset, i);
    offset += value::writeToMemory(offset, jumpOffset);
}

void CodeFragment::appendJumpTrue(int jumpOffset) {
    Instruction i;
    i.tag = Instruction::jmpTrue;
    adjustStackSimple(i);

    auto offset = allocateSpace(sizeof(Instruction) + sizeof(jumpOffset));

    offset += value::writeToMemory(offset, i);
    offset += value::writeToMemory(offset, jumpOffset);
}

void CodeFragment::appendJumpNothing(int jumpOffset) {
    Instruction i;
    i.tag = Instruction::jmpNothing;
    adjustStackSimple(i);

    auto offset = allocateSpace(sizeof(Instruction) + sizeof(jumpOffset));

    offset += value::writeToMemory(offset, i);
    offset += value::writeToMemory(offset, jumpOffset);
}

ByteCode::~ByteCode() {
    auto size = _argStackOwned.size();
    invariant(_argStackTags.size() == size);
    invariant(_argStackVals.size() == size);
    for (size_t i = 0; i < size; ++i) {
        if (_argStackOwned[i]) {
            value::releaseValue(_argStackTags[i], _argStackVals[i]);
        }
    }
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::getField(value::TypeTags objTag,
                                                                   value::Value objValue,
                                                                   value::TypeTags fieldTag,
                                                                   value::Value fieldValue) {
    if (!value::isString(fieldTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto fieldStr = value::getStringView(fieldTag, fieldValue);

    if (MONGO_unlikely(failOnPoisonedFieldLookup.shouldFail())) {
        uassert(4623399, "Lookup of $POISON", fieldStr != "POISON");
    }

    if (objTag == value::TypeTags::Object) {
        auto [tag, val] = value::getObjectView(objValue)->getField(fieldStr);
        return {false, tag, val};
    } else if (objTag == value::TypeTags::bsonObject) {
        auto be = value::bitcastTo<const char*>(objValue);
        auto end = be + ConstDataView(be).read<LittleEndian<uint32_t>>();
        ;
        // Skip document length.
        be += 4;
        while (*be != 0) {
            auto sv = bson::fieldNameView(be);

            if (sv == fieldStr) {
                auto [tag, val] = bson::convertFrom(true, be, end, sv.size());
                return {false, tag, val};
            }

            be = bson::advance(be, sv.size());
        }
    }
    return {false, value::TypeTags::Nothing, 0};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::getElement(value::TypeTags arrTag,
                                                                     value::Value arrValue,
                                                                     value::TypeTags idxTag,
                                                                     value::Value idxValue) {
    if (arrTag != value::TypeTags::Array && arrTag != value::TypeTags::bsonArray) {
        return {false, value::TypeTags::Nothing, 0};
    }

    // Bail out if the `idx` parameter isn't a number, or if it can't be converted to a 64-bit
    // integer, or if it's outside of the range where the `lhsTag` type can represent consecutive
    // integers precisely.
    auto [numTag, numVal] = genericNumConvertToPreciseInt64(idxTag, idxValue);
    if (numTag != value::TypeTags::NumberInt64) {
        return {false, value::TypeTags::Nothing, 0};
    }
    int64_t numInt64 = value::bitcastTo<int64_t>(numVal);
    // Cast the `idx` parameter to size_t. Bail out if its negative or if it's too big for size_t.
    if (numInt64 < 0 ||
        (sizeof(size_t) < sizeof(int64_t) &&
         numInt64 > static_cast<int64_t>(std::numeric_limits<size_t>::max()))) {
        return {false, value::TypeTags::Nothing, 0};
    }
    size_t idx = static_cast<size_t>(numInt64);

    if (arrTag == value::TypeTags::Array) {
        // If `arr` is an SBE array, use Array::getAt() to retrieve the element at index `idx`.
        auto [tag, val] = value::getArrayView(arrValue)->getAt(idx);
        return {false, tag, val};
    } else if (arrTag == value::TypeTags::bsonArray) {
        // If `arr` is a BSON array, loop over the elements until we reach the idx-th element.
        auto be = value::bitcastTo<const char*>(arrValue);
        auto end = be + ConstDataView(be).read<LittleEndian<uint32_t>>();
        // Skip document length.
        be += 4;
        // The field names of an array are always be 0 thru N-1 in order. Therefore we don't need to
        // inspect the field names (aside from determining their length so we can skip over them).
        for (size_t currentIdx = 0; *be != 0; ++currentIdx) {
            size_t fieldNameLength = strlen(be + 1);
            if (currentIdx == idx) {
                auto [tag, val] = bson::convertFrom(true, be, end, fieldNameLength);
                return {false, tag, val};
            }
            be = bson::advance(be, fieldNameLength);
        }
        // If the array didn't have an element at index `idx`, return Nothing.
        return {false, value::TypeTags::Nothing, 0};
    } else {
        // Earlier in this function we bailed out if the `arrTag` wasn't Array or bsonArray, so it
        // should be impossible to reach this point.
        MONGO_UNREACHABLE
    }
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::aggSum(value::TypeTags accTag,
                                                                 value::Value accValue,
                                                                 value::TypeTags fieldTag,
                                                                 value::Value fieldValue) {
    // Skip aggregation step if we don't have the input.
    if (fieldTag == value::TypeTags::Nothing) {
        auto [tag, val] = value::copyValue(accTag, accValue);
        return {true, tag, val};
    }

    // Initialize the accumulator.
    if (accTag == value::TypeTags::Nothing) {
        accTag = value::TypeTags::NumberInt64;
        accValue = 0;
    }

    return genericAdd(accTag, accValue, fieldTag, fieldValue);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::aggMin(value::TypeTags accTag,
                                                                 value::Value accValue,
                                                                 value::TypeTags fieldTag,
                                                                 value::Value fieldValue) {
    // Skip aggregation step if we don't have the input.
    if (fieldTag == value::TypeTags::Nothing) {
        auto [tag, val] = value::copyValue(accTag, accValue);
        return {true, tag, val};
    }

    // Initialize the accumulator.
    if (accTag == value::TypeTags::Nothing) {
        auto [tag, val] = value::copyValue(fieldTag, fieldValue);
        return {true, tag, val};
    }

    auto [tag, val] = genericCompare<std::less<>>(accTag, accValue, fieldTag, fieldValue);
    if (tag == value::TypeTags::Boolean && val) {
        auto [tag, val] = value::copyValue(accTag, accValue);
        return {true, tag, val};
    } else {
        auto [tag, val] = value::copyValue(fieldTag, fieldValue);
        return {true, tag, val};
    }
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::aggMax(value::TypeTags accTag,
                                                                 value::Value accValue,
                                                                 value::TypeTags fieldTag,
                                                                 value::Value fieldValue) {
    // Skip aggregation step if we don't have the input.
    if (fieldTag == value::TypeTags::Nothing) {
        auto [tag, val] = value::copyValue(accTag, accValue);
        return {true, tag, val};
    }

    // Initialize the accumulator.
    if (accTag == value::TypeTags::Nothing) {
        auto [tag, val] = value::copyValue(fieldTag, fieldValue);
        return {true, tag, val};
    }

    auto [tag, val] = genericCompare<std::greater<>>(accTag, accValue, fieldTag, fieldValue);
    if (tag == value::TypeTags::Boolean && val) {
        auto [tag, val] = value::copyValue(accTag, accValue);
        return {true, tag, val};
    } else {
        auto [tag, val] = value::copyValue(fieldTag, fieldValue);
        return {true, tag, val};
    }
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::aggFirst(value::TypeTags accTag,
                                                                   value::Value accValue,
                                                                   value::TypeTags fieldTag,
                                                                   value::Value fieldValue) {
    // Skip aggregation step if we don't have the input.
    if (fieldTag == value::TypeTags::Nothing) {
        auto [tag, val] = value::copyValue(accTag, accValue);
        return {true, tag, val};
    }

    // Initialize the accumulator.
    if (accTag == value::TypeTags::Nothing) {
        auto [tag, val] = value::copyValue(fieldTag, fieldValue);
        return {true, tag, val};
    }

    // Disregard the next value, always return the first one.
    auto [tag, val] = value::copyValue(accTag, accValue);
    return {true, tag, val};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::aggLast(value::TypeTags accTag,
                                                                  value::Value accValue,
                                                                  value::TypeTags fieldTag,
                                                                  value::Value fieldValue) {
    // Skip aggregation step if we don't have the input.
    if (fieldTag == value::TypeTags::Nothing) {
        auto [tag, val] = value::copyValue(accTag, accValue);
        return {true, tag, val};
    }

    // Initialize the accumulator.
    if (accTag == value::TypeTags::Nothing) {
        auto [tag, val] = value::copyValue(fieldTag, fieldValue);
        return {true, tag, val};
    }

    // Disregard the accumulator, always return the next value.
    auto [tag, val] = value::copyValue(fieldTag, fieldValue);
    return {true, tag, val};
}


bool hasSeparatorAt(size_t idx, std::string_view input, std::string_view separator) {
    if (separator.size() + idx > input.size()) {
        return false;
    }

    return input.compare(idx, separator.size(), separator) == 0;
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinSplit(uint8_t arity) {
    auto [ownedSeparator, tagSeparator, valSeparator] = getFromStack(1);
    auto [ownedInput, tagInput, valInput] = getFromStack(0);

    if (!value::isString(tagSeparator) || !value::isString(tagInput)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto input = value::getStringView(tagInput, valInput);
    auto separator = value::getStringView(tagSeparator, valSeparator);

    auto [tag, val] = value::makeNewArray();
    auto arr = value::getArrayView(val);
    value::ValueGuard guard{tag, val};

    size_t splitStart = 0;
    size_t splitPos;
    while ((splitPos = input.find(separator, splitStart)) != std::string_view::npos) {
        auto [tag, val] = value::makeNewString(input.substr(splitStart, splitPos - splitStart));
        arr->push_back(tag, val);

        splitPos += separator.size();
        splitStart = splitPos;
    }

    // This is the last string.
    {
        auto [tag, val] = value::makeNewString(input.substr(splitStart, input.size() - splitStart));
        arr->push_back(tag, val);
    }

    guard.reset();
    return {true, tag, val};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinDropFields(uint8_t arity) {
    auto [ownedSeparator, tagInObj, valInObj] = getFromStack(0);

    // We operate only on objects.
    if (!value::isObject(tagInObj)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    // Build the set of fields to drop.
    std::set<std::string, std::less<>> restrictFieldsSet;
    for (uint8_t idx = 1; idx < arity; ++idx) {
        auto [owned, tag, val] = getFromStack(idx);

        if (!value::isString(tag)) {
            return {false, value::TypeTags::Nothing, 0};
        }

        restrictFieldsSet.emplace(value::getStringView(tag, val));
    }

    auto [tag, val] = value::makeNewObject();
    auto obj = value::getObjectView(val);
    value::ValueGuard guard{tag, val};

    if (tagInObj == value::TypeTags::bsonObject) {
        auto be = value::bitcastTo<const char*>(valInObj);
        auto end = be + ConstDataView(be).read<LittleEndian<uint32_t>>();
        ;
        // Skip document length.
        be += 4;
        while (*be != 0) {
            auto sv = bson::fieldNameView(be);

            if (restrictFieldsSet.count(sv) == 0) {
                auto [tag, val] = bson::convertFrom(false, be, end, sv.size());
                obj->push_back(sv, tag, val);
            }

            be = bson::advance(be, sv.size());
        }
    } else if (tagInObj == value::TypeTags::Object) {
        auto objRoot = value::getObjectView(valInObj);
        for (size_t idx = 0; idx < objRoot->size(); ++idx) {
            std::string_view sv(objRoot->field(idx));

            if (restrictFieldsSet.count(sv) == 0) {

                auto [tag, val] = objRoot->getAt(idx);
                auto [copyTag, copyVal] = value::copyValue(tag, val);
                obj->push_back(sv, copyTag, copyVal);
            }
        }
    }

    guard.reset();
    return {true, tag, val};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinNewObj(uint8_t arity) {
    std::vector<value::TypeTags> typeTags;
    std::vector<value::Value> values;
    std::vector<std::string> names;

    for (uint8_t idx = 0; idx < arity; idx += 2) {
        {
            auto [owned, tag, val] = getFromStack(idx);

            if (!value::isString(tag)) {
                return {false, value::TypeTags::Nothing, 0};
            }

            names.emplace_back(value::getStringView(tag, val));
        }
        {
            auto [owned, tag, val] = getFromStack(idx + 1);
            typeTags.push_back(tag);
            values.push_back(val);
        }
    }

    auto [tag, val] = value::makeNewObject();
    auto obj = value::getObjectView(val);
    value::ValueGuard guard{tag, val};

    for (size_t idx = 0; idx < typeTags.size(); ++idx) {
        auto [tagCopy, valCopy] = value::copyValue(typeTags[idx], values[idx]);
        obj->push_back(names[idx], tagCopy, valCopy);
    }

    guard.reset();
    return {true, tag, val};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinKeyStringToString(uint8_t arity) {
    auto [owned, tagInKey, valInKey] = getFromStack(0);

    // We operate only on keys.
    if (tagInKey != value::TypeTags::ksValue) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto key = value::getKeyStringView(valInKey);

    auto [tagStr, valStr] = value::makeNewString(key->toString());

    return {true, tagStr, valStr};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinNewKeyString(uint8_t arity) {
    auto [_, tagInVersion, valInVersion] = getFromStack(0);

    if (!value::isNumber(tagInVersion) ||
        !(value::numericCast<int64_t>(tagInVersion, valInVersion) == 0 ||
          value::numericCast<int64_t>(tagInVersion, valInVersion) == 1)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    KeyString::Version version =
        static_cast<KeyString::Version>(value::numericCast<int64_t>(tagInVersion, valInVersion));

    auto [__, tagInOrdering, valInOrdering] = getFromStack(1);
    if (!value::isNumber(tagInOrdering)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto orderingBits = value::numericCast<int32_t>(tagInOrdering, valInOrdering);
    BSONObjBuilder bb;
    for (size_t i = 0; i < Ordering::kMaxCompoundIndexKeys; ++i) {
        bb.append(""_sd, (orderingBits & (1 << i)) ? 1 : 0);
    }

    KeyString::HeapBuilder kb{version, Ordering::make(bb.done())};

    for (size_t idx = 2; idx < arity - 1u; ++idx) {
        auto [_, tag, val] = getFromStack(idx);
        if (value::isNumber(tag)) {
            auto num = value::numericCast<int64_t>(tag, val);
            kb.appendNumberLong(num);
        } else if (value::isString(tag)) {
            auto str = value::getStringView(tag, val);
            kb.appendString(StringData{str.data(), str.length()});
        } else {
            uasserted(4822802, "unsuppored key string type");
        }
    }

    auto [___, tagInDisrim, valInDiscrim] = getFromStack(arity - 1);
    if (!value::isNumber(tagInDisrim)) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto discrimNum = value::numericCast<int64_t>(tagInDisrim, valInDiscrim);
    if (discrimNum < 0 || discrimNum > 2) {
        return {false, value::TypeTags::Nothing, 0};
    }

    kb.appendDiscriminator(static_cast<KeyString::Discriminator>(discrimNum));

    return {true, value::TypeTags::ksValue, value::bitcastFrom(new KeyString::Value(kb.release()))};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinAbs(uint8_t arity) {
    invariant(arity == 1);

    auto [_, tagOperand, valOperand] = getFromStack(0);

    return genericAbs(tagOperand, valOperand);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinAddToArray(uint8_t arity) {
    auto [ownAgg, tagAgg, valAgg] = getFromStack(0);
    auto [_, tagField, valField] = getFromStack(1);

    // Create a new array is it does not exist yet.
    if (tagAgg == value::TypeTags::Nothing) {
        auto [tagNewAgg, valNewAgg] = value::makeNewArray();
        ownAgg = true;
        tagAgg = tagNewAgg;
        valAgg = valNewAgg;
    } else {
        // Take ownership of the accumulator.
        topStack(false, value::TypeTags::Nothing, 0);
    }
    value::ValueGuard guard{tagAgg, valAgg};

    invariant(ownAgg && tagAgg == value::TypeTags::Array);
    auto arr = value::getArrayView(valAgg);

    // And push back the value. Note that array will ignore Nothing.
    auto [tagCopy, valCopy] = value::copyValue(tagField, valField);
    arr->push_back(tagCopy, valCopy);

    guard.reset();
    return {ownAgg, tagAgg, valAgg};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinAddToSet(uint8_t arity) {
    auto [ownAgg, tagAgg, valAgg] = getFromStack(0);
    auto [_, tagField, valField] = getFromStack(1);

    // Create a new array is it does not exist yet.
    if (tagAgg == value::TypeTags::Nothing) {
        auto [tagNewAgg, valNewAgg] = value::makeNewArraySet();
        ownAgg = true;
        tagAgg = tagNewAgg;
        valAgg = valNewAgg;
    } else {
        // Take ownership of the accumulator
        topStack(false, value::TypeTags::Nothing, 0);
    }
    value::ValueGuard guard{tagAgg, valAgg};

    invariant(ownAgg && tagAgg == value::TypeTags::ArraySet);
    auto arr = value::getArraySetView(valAgg);

    // And push back the value. Note that array will ignore Nothing.
    auto [tagCopy, valCopy] = value::copyValue(tagField, valField);
    arr->push_back(tagCopy, valCopy);

    guard.reset();
    return {ownAgg, tagAgg, valAgg};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinRegexMatch(uint8_t arity) {
    invariant(arity == 2);

    auto [ownedPcreRegex, typeTagPcreRegex, valuePcreRegex] = getFromStack(0);
    auto [ownedInputStr, typeTagInputStr, valueInputStr] = getFromStack(1);

    if (!value::isString(typeTagInputStr) || typeTagPcreRegex != value::TypeTags::pcreRegex) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto stringView = value::getStringView(typeTagInputStr, valueInputStr);
    pcrecpp::StringPiece pcreStringView{stringView.data(), static_cast<int>(stringView.size())};

    auto pcreRegex = value::getPcreRegexView(valuePcreRegex);
    auto regexMatchResult = pcreRegex->PartialMatch(pcreStringView);

    return {false, value::TypeTags::Boolean, regexMatchResult};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinDoubleDoubleSum(uint8_t arity) {
    invariant(arity > 0);

    value::TypeTags resultTag = value::TypeTags::NumberInt32;
    bool haveDate = false;

    // Sweep across all tags and pick the result type.
    for (size_t idx = 0; idx < arity; ++idx) {
        auto [own, tag, val] = getFromStack(idx);
        if (tag == value::TypeTags::Date) {
            if (haveDate) {
                uassert(4848404, "only one date allowed in an $add expression", !haveDate);
            }
            // Date is a simple 64 bit integer.
            haveDate = true;
            tag = value::TypeTags::NumberInt64;
        }
        if (value::isNumber(tag)) {
            resultTag = value::getWidestNumericalType(resultTag, tag);
        } else if (tag == value::TypeTags::Nothing || tag == value::TypeTags::Null) {
            // What to do about null and nothing?
            return {false, value::TypeTags::Nothing, 0};
        } else {
            // What to do about non-numeric types like arrays and objects?
            return {false, value::TypeTags::Nothing, 0};
        }
    }

    if (resultTag == value::TypeTags::NumberDecimal) {
        Decimal128 sum;
        for (size_t idx = 0; idx < arity; ++idx) {
            auto [own, tag, val] = getFromStack(idx);
            if (tag == value::TypeTags::Date) {
                sum.add(Decimal128(value::bitcastTo<int64_t>(val)));
            } else {
                sum.add(value::numericCast<Decimal128>(tag, val));
            }
        }
        if (haveDate) {
            return {false, value::TypeTags::Date, value::bitcastFrom(sum.toLong())};
        } else {
            auto [tag, val] = value::makeCopyDecimal(sum);
            return {true, tag, val};
        }
    } else {
        DoubleDoubleSummation sum;
        for (size_t idx = 0; idx < arity; ++idx) {
            auto [own, tag, val] = getFromStack(idx);
            if (tag == value::TypeTags::NumberInt32) {
                sum.addInt(value::numericCast<int32_t>(tag, val));
            } else if (tag == value::TypeTags::NumberInt64 || tag == value::TypeTags::Date) {
                sum.addLong(value::numericCast<int64_t>(tag, val));
            } else if (tag == value::TypeTags::NumberDouble) {
                sum.addDouble(value::numericCast<double>(tag, val));
            } else if (tag == value::TypeTags::Date) {
                sum.addLong(value::bitcastTo<int64_t>(val));
            }
        }
        if (haveDate) {
            uassert(ErrorCodes::Overflow, "date overflow in $add", sum.fitsLong());
            return {false, value::TypeTags::Date, value::bitcastFrom(sum.getLong())};
        } else {
            switch (resultTag) {
                case value::TypeTags::NumberInt32: {
                    auto result = sum.getLong();
                    if (sum.fitsLong() && result >= std::numeric_limits<int32_t>::min() &&
                        result <= std::numeric_limits<int32_t>::max()) {
                        return {false, value::TypeTags::NumberInt32, value::bitcastFrom(result)};
                    }
                    // Fall through to the larger type.
                }
                case value::TypeTags::NumberInt64: {
                    if (sum.fitsLong()) {
                        return {
                            false, value::TypeTags::NumberInt64, value::bitcastFrom(sum.getLong())};
                    }
                    // Fall through to the larger type.
                }
                case value::TypeTags::NumberDouble: {
                    return {
                        false, value::TypeTags::NumberDouble, value::bitcastFrom(sum.getDouble())};
                }
                default:
                    MONGO_UNREACHABLE;
            }
        }
    }
    return {false, value::TypeTags::Nothing, 0};
}

/**
 * A helper for the bultinDate method. The formal parameters yearOrWeekYear and monthOrWeek carry
 * values depending on wether the date is a year-month-day or ISOWeekYear.
 */
using DateFn = std::function<Date_t(
    TimeZone, long long, long long, long long, long long, long long, long long, long long)>;
std::tuple<bool, value::TypeTags, value::Value> builtinDateHelper(
    DateFn computeDateFn,
    std::tuple<bool, value::TypeTags, value::Value> tzdb,
    std::tuple<bool, value::TypeTags, value::Value> yearOrWeekYear,
    std::tuple<bool, value::TypeTags, value::Value> monthOrWeek,
    std::tuple<bool, value::TypeTags, value::Value> day,
    std::tuple<bool, value::TypeTags, value::Value> hour,
    std::tuple<bool, value::TypeTags, value::Value> minute,
    std::tuple<bool, value::TypeTags, value::Value> second,
    std::tuple<bool, value::TypeTags, value::Value> millisecond,
    std::tuple<bool, value::TypeTags, value::Value> timezone) {

    auto [ownedTzdb, typeTagTzdb, valueTzdb] = tzdb;
    auto [ownedYearOrWeekYear, typeTagYearOrWeekYear, valueYearOrWeekYear] = yearOrWeekYear;
    auto [ownedMonthOrWeek, typeTagMonthOrWeek, valueMonthOrWeek] = monthOrWeek;
    auto [ownedDay, typeTagDay, valueDay] = day;
    auto [ownedHr, typeTagHr, valueHr] = hour;
    auto [ownedMin, typeTagMin, valueMin] = minute;
    auto [ownedSec, typeTagSec, valueSec] = second;
    auto [ownedMillis, typeTagMillis, valueMillis] = millisecond;
    auto [ownedTz, typeTagTz, valueTz] = timezone;

    if (typeTagTzdb != value::TypeTags::timeZoneDB || !value::isNumber(typeTagYearOrWeekYear) ||
        !value::isNumber(typeTagMonthOrWeek) || !value::isNumber(typeTagDay) ||
        !value::isNumber(typeTagHr) || !value::isNumber(typeTagMin) ||
        !value::isNumber(typeTagSec) || !value::isNumber(typeTagMillis) ||
        !value::isString(typeTagTz)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto timeZoneDB = value::getTimeZoneDBView(valueTzdb);
    invariant(timeZoneDB);

    auto tzString = value::getStringView(typeTagTz, valueTz);
    const auto tz = tzString == ""
        ? timeZoneDB->utcZone()
        : timeZoneDB->getTimeZone(StringData{tzString.data(), tzString.size()});

    auto date =
        computeDateFn(tz,
                      value::numericCast<int64_t>(typeTagYearOrWeekYear, valueYearOrWeekYear),
                      value::numericCast<int64_t>(typeTagMonthOrWeek, valueMonthOrWeek),
                      value::numericCast<int64_t>(typeTagDay, valueDay),
                      value::numericCast<int64_t>(typeTagHr, valueHr),
                      value::numericCast<int64_t>(typeTagMin, valueMin),
                      value::numericCast<int64_t>(typeTagSec, valueSec),
                      value::numericCast<int64_t>(typeTagMillis, valueMillis));
    return {false, value::TypeTags::Date, date.asInt64()};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinDate(uint8_t arity) {
    auto timeZoneDBTuple = getFromStack(0);
    auto yearTuple = getFromStack(1);
    auto monthTuple = getFromStack(2);
    auto dayTuple = getFromStack(3);
    auto hourTuple = getFromStack(4);
    auto minuteTuple = getFromStack(5);
    auto secondTuple = getFromStack(6);
    auto millisTuple = getFromStack(7);
    auto timezoneTuple = getFromStack(8);

    return builtinDateHelper(
        [](TimeZone tz,
           long long year,
           long long month,
           long long day,
           long long hour,
           long long min,
           long long sec,
           long long millis) -> Date_t {
            return tz.createFromDateParts(year, month, day, hour, min, sec, millis);
        },
        timeZoneDBTuple,
        yearTuple,
        monthTuple,
        dayTuple,
        hourTuple,
        minuteTuple,
        secondTuple,
        millisTuple,
        timezoneTuple);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::builtinDateWeekYear(uint8_t arity) {
    auto timeZoneDBTuple = getFromStack(0);
    auto yearTuple = getFromStack(1);
    auto weekTuple = getFromStack(2);
    auto dayTuple = getFromStack(3);
    auto hourTuple = getFromStack(4);
    auto minuteTuple = getFromStack(5);
    auto secondTuple = getFromStack(6);
    auto millisTuple = getFromStack(7);
    auto timezoneTuple = getFromStack(8);

    return builtinDateHelper(
        [](TimeZone tz,
           long long year,
           long long month,
           long long day,
           long long hour,
           long long min,
           long long sec,
           long long millis) -> Date_t {
            return tz.createFromIso8601DateParts(year, month, day, hour, min, sec, millis);
        },
        timeZoneDBTuple,
        yearTuple,
        weekTuple,
        dayTuple,
        hourTuple,
        minuteTuple,
        secondTuple,
        millisTuple,
        timezoneTuple);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::dispatchBuiltin(Builtin f,
                                                                          uint8_t arity) {
    switch (f) {
        case Builtin::dateParts:
            return builtinDate(arity);
        case Builtin::datePartsWeekYear:
            return builtinDateWeekYear(arity);
        case Builtin::split:
            return builtinSplit(arity);
        case Builtin::regexMatch:
            return builtinRegexMatch(arity);
        case Builtin::dropFields:
            return builtinDropFields(arity);
        case Builtin::newObj:
            return builtinNewObj(arity);
        case Builtin::ksToString:
            return builtinKeyStringToString(arity);
        case Builtin::newKs:
            return builtinNewKeyString(arity);
        case Builtin::abs:
            return builtinAbs(arity);
        case Builtin::addToArray:
            return builtinAddToArray(arity);
        case Builtin::addToSet:
            return builtinAddToSet(arity);
        case Builtin::doubleDoubleSum:
            return builtinDoubleDoubleSum(arity);
    }

    MONGO_UNREACHABLE;
}

std::tuple<uint8_t, value::TypeTags, value::Value> ByteCode::run(const CodeFragment* code) {
    auto pcPointer = code->instrs().data();
    auto pcEnd = pcPointer + code->instrs().size();

    for (;;) {
        if (pcPointer == pcEnd) {
            break;
        } else {
            Instruction i = value::readFromMemory<Instruction>(pcPointer);
            pcPointer += sizeof(i);
            switch (i.tag) {
                case Instruction::pushConstVal: {
                    auto tag = value::readFromMemory<value::TypeTags>(pcPointer);
                    pcPointer += sizeof(tag);
                    auto val = value::readFromMemory<value::Value>(pcPointer);
                    pcPointer += sizeof(val);

                    pushStack(false, tag, val);

                    break;
                }
                case Instruction::pushAccessVal: {
                    auto accessor = value::readFromMemory<value::SlotAccessor*>(pcPointer);
                    pcPointer += sizeof(accessor);

                    auto [tag, val] = accessor->getViewOfValue();
                    pushStack(false, tag, val);

                    break;
                }
                case Instruction::pushMoveVal: {
                    auto accessor = value::readFromMemory<value::SlotAccessor*>(pcPointer);
                    pcPointer += sizeof(accessor);

                    auto [tag, val] = accessor->copyOrMoveValue();
                    pushStack(true, tag, val);

                    break;
                }
                case Instruction::pushLocalVal: {
                    auto stackOffset = value::readFromMemory<int>(pcPointer);
                    pcPointer += sizeof(stackOffset);

                    auto [owned, tag, val] = getFromStack(stackOffset);

                    pushStack(false, tag, val);

                    break;
                }
                case Instruction::pop: {
                    auto [owned, tag, val] = getFromStack(0);
                    popStack();

                    if (owned) {
                        value::releaseValue(tag, val);
                    }

                    break;
                }
                case Instruction::swap: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(1);

                    // Swap values only if they are not physically same.
                    // Note - this has huge consequences for the memory management, it allows to
                    // return owned values from the let expressions.
                    if (!(rhsTag == lhsTag && rhsVal == lhsVal)) {
                        setStack(0, lhsOwned, lhsTag, lhsVal);
                        setStack(1, rhsOwned, rhsTag, rhsVal);
                    } else {
                        // The values are physically same then the top of the stack must never ever
                        // be owned.
                        invariant(!rhsOwned);
                    }

                    break;
                }
                case Instruction::add: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [owned, tag, val] = genericAdd(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(owned, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::sub: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [owned, tag, val] = genericSub(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(owned, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::mul: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [owned, tag, val] = genericMul(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(owned, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::div: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [owned, tag, val] = genericDiv(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(owned, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::idiv: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [owned, tag, val] = genericIDiv(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(owned, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::mod: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [owned, tag, val] = genericMod(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(owned, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::negate: {
                    auto [owned, tag, val] = getFromStack(0);

                    auto [resultOwned, resultTag, resultVal] =
                        genericSub(value::TypeTags::NumberInt32, 0, tag, val);

                    topStack(resultOwned, resultTag, resultVal);

                    if (owned) {
                        value::releaseValue(resultTag, resultVal);
                    }

                    break;
                }
                case Instruction::numConvert: {
                    auto tag = value::readFromMemory<value::TypeTags>(pcPointer);
                    pcPointer += sizeof(tag);

                    auto [owned, lhsTag, lhsVal] = getFromStack(0);

                    auto [rhsOwned, rhsTag, rhsVal] = genericNumConvert(lhsTag, lhsVal, tag);

                    topStack(rhsOwned, rhsTag, rhsVal);

                    if (owned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }

                    break;
                }
                case Instruction::logicNot: {
                    auto [owned, tag, val] = getFromStack(0);

                    auto [resultOwned, resultTag, resultVal] = genericNot(tag, val);

                    topStack(resultOwned, resultTag, resultVal);

                    if (owned) {
                        value::releaseValue(tag, val);
                    }
                    break;
                }
                case Instruction::less: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [tag, val] = genericCompare<std::less<>>(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(false, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::lessEq: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [tag, val] =
                        genericCompare<std::less_equal<>>(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(false, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::greater: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [tag, val] =
                        genericCompare<std::greater<>>(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(false, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::greaterEq: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [tag, val] =
                        genericCompare<std::greater_equal<>>(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(false, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::eq: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [tag, val] = genericCompareEq(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(false, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::neq: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [tag, val] = genericCompareNeq(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(false, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::cmp3w: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [tag, val] = compare3way(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(false, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::fillEmpty: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    if (lhsTag == value::TypeTags::Nothing) {
                        topStack(rhsOwned, rhsTag, rhsVal);

                        if (lhsOwned) {
                            value::releaseValue(lhsTag, lhsVal);
                        }
                    } else {
                        if (rhsOwned) {
                            value::releaseValue(rhsTag, rhsVal);
                        }
                    }
                    break;
                }
                case Instruction::getField: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [owned, tag, val] = getField(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(owned, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::getElement: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [owned, tag, val] = getElement(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(owned, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::aggSum: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [owned, tag, val] = aggSum(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(owned, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::aggMin: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [owned, tag, val] = aggMin(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(owned, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::aggMax: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [owned, tag, val] = aggMax(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(owned, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::aggFirst: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [owned, tag, val] = aggFirst(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(owned, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::aggLast: {
                    auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
                    popStack();
                    auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

                    auto [owned, tag, val] = aggLast(lhsTag, lhsVal, rhsTag, rhsVal);

                    topStack(owned, tag, val);

                    if (rhsOwned) {
                        value::releaseValue(rhsTag, rhsVal);
                    }
                    if (lhsOwned) {
                        value::releaseValue(lhsTag, lhsVal);
                    }
                    break;
                }
                case Instruction::exists: {
                    auto [owned, tag, val] = getFromStack(0);

                    topStack(false, value::TypeTags::Boolean, tag != value::TypeTags::Nothing);

                    if (owned) {
                        value::releaseValue(tag, val);
                    }
                    break;
                }
                case Instruction::isNull: {
                    auto [owned, tag, val] = getFromStack(0);

                    if (tag != value::TypeTags::Nothing) {
                        topStack(false, value::TypeTags::Boolean, tag == value::TypeTags::Null);
                    }

                    if (owned) {
                        value::releaseValue(tag, val);
                    }
                    break;
                }
                case Instruction::isObject: {
                    auto [owned, tag, val] = getFromStack(0);

                    if (tag != value::TypeTags::Nothing) {
                        topStack(false, value::TypeTags::Boolean, value::isObject(tag));
                    }

                    if (owned) {
                        value::releaseValue(tag, val);
                    }
                    break;
                }
                case Instruction::isArray: {
                    auto [owned, tag, val] = getFromStack(0);

                    if (tag != value::TypeTags::Nothing) {
                        topStack(false, value::TypeTags::Boolean, value::isArray(tag));
                    }

                    if (owned) {
                        value::releaseValue(tag, val);
                    }
                    break;
                }
                case Instruction::isString: {
                    auto [owned, tag, val] = getFromStack(0);

                    if (tag != value::TypeTags::Nothing) {
                        topStack(false, value::TypeTags::Boolean, value::isString(tag));
                    }

                    if (owned) {
                        value::releaseValue(tag, val);
                    }
                    break;
                }
                case Instruction::isNumber: {
                    auto [owned, tag, val] = getFromStack(0);

                    if (tag != value::TypeTags::Nothing) {
                        topStack(false, value::TypeTags::Boolean, value::isNumber(tag));
                    }

                    if (owned) {
                        value::releaseValue(tag, val);
                    }
                    break;
                }
                case Instruction::typeMatch: {
                    auto typeMask = value::readFromMemory<uint32_t>(pcPointer);
                    pcPointer += sizeof(typeMask);

                    auto [owned, tag, val] = getFromStack(0);

                    if (tag != value::TypeTags::Nothing) {
                        bool matches = static_cast<bool>(getBSONTypeMask(tag) & typeMask);
                        topStack(false, value::TypeTags::Boolean, matches);
                    }

                    if (owned) {
                        value::releaseValue(tag, val);
                    }
                    break;
                }
                case Instruction::function: {
                    auto f = value::readFromMemory<Builtin>(pcPointer);
                    pcPointer += sizeof(f);
                    auto arity = value::readFromMemory<uint8_t>(pcPointer);
                    pcPointer += sizeof(arity);

                    auto [owned, tag, val] = dispatchBuiltin(f, arity);

                    for (uint8_t cnt = 0; cnt < arity; ++cnt) {
                        auto [owned, tag, val] = getFromStack(0);
                        popStack();
                        if (owned) {
                            value::releaseValue(tag, val);
                        }
                    }

                    pushStack(owned, tag, val);

                    break;
                }
                case Instruction::jmp: {
                    auto jumpOffset = value::readFromMemory<int>(pcPointer);
                    pcPointer += sizeof(jumpOffset);

                    pcPointer += jumpOffset;
                    break;
                }
                case Instruction::jmpTrue: {
                    auto jumpOffset = value::readFromMemory<int>(pcPointer);
                    pcPointer += sizeof(jumpOffset);

                    auto [owned, tag, val] = getFromStack(0);
                    popStack();

                    if (tag == value::TypeTags::Boolean && val) {
                        pcPointer += jumpOffset;
                    }

                    if (owned) {
                        value::releaseValue(tag, val);
                    }
                    break;
                }
                case Instruction::jmpNothing: {
                    auto jumpOffset = value::readFromMemory<int>(pcPointer);
                    pcPointer += sizeof(jumpOffset);

                    auto [owned, tag, val] = getFromStack(0);
                    if (tag == value::TypeTags::Nothing) {
                        pcPointer += jumpOffset;
                    }
                    break;
                }
                case Instruction::fail: {
                    auto [ownedCode, tagCode, valCode] = getFromStack(1);
                    invariant(tagCode == value::TypeTags::NumberInt64);

                    auto [ownedMsg, tagMsg, valMsg] = getFromStack(0);
                    invariant(value::isString(tagMsg));

                    ErrorCodes::Error code{
                        static_cast<ErrorCodes::Error>(value::bitcastTo<int64_t>(valCode))};
                    std::string message{value::getStringView(tagMsg, valMsg)};

                    uasserted(code, message);

                    break;
                }
                default:
                    MONGO_UNREACHABLE;
            }
        }
    }
    uassert(
        4822801, "The evaluation stack must hold only a single value", _argStackOwned.size() == 1);

    auto owned = _argStackOwned[0];
    auto tag = _argStackTags[0];
    auto val = _argStackVals[0];

    _argStackOwned.clear();
    _argStackTags.clear();
    _argStackVals.clear();

    return {owned, tag, val};
}

bool ByteCode::runPredicate(const CodeFragment* code) {
    auto [owned, tag, val] = run(code);

    bool pass = (tag == value::TypeTags::Boolean) && (val != 0);

    if (owned) {
        value::releaseValue(tag, val);
    }

    return pass;
}
}  // namespace vm
}  // namespace sbe
}  // namespace mongo
