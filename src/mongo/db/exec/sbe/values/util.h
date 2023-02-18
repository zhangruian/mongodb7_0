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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value.h"

namespace mongo {
namespace sbe {
namespace value {
/*
 * Similar to std::any_of, for SBE arrays.
 */
template <class Cb>
bool arrayAny(TypeTags tag, Value val, const Cb& cb) {
    if (tag == TypeTags::bsonArray) {
        auto bson = getRawPointerView(val);
        const auto* cur = bson + 4;
        const auto* end = bson + ConstDataView(bson).read<LittleEndian<uint32_t>>();

        while (cur != end - 1) {
            auto* fieldName = bson::fieldNameRaw(cur);
            size_t keySize = TinyStrHelpers::strlen(fieldName);
            auto [elemTag, elemVal] = bson::convertFrom<true>(cur, end, keySize);

            if (cb(elemTag, elemVal)) {
                return true;
            }

            cur = bson::advance(cur, keySize);
        }
    } else if (tag == TypeTags::Array) {
        auto array = getArrayView(val);
        for (size_t i = 0; i < array->size(); ++i) {
            auto [t, v] = array->getAt(i);
            if (cb(t, v)) {
                return true;
            }
        }
    } else if (tag == TypeTags::ArraySet) {
        auto arraySet = getArraySetView(val);
        for (auto [t, v] : arraySet->values()) {
            if (cb(t, v)) {
                return true;
            }
        }
    } else {
        MONGO_UNREACHABLE;
    }
    return false;
}

/*
 * Invokes callback on each element of the given array.
 */
template <class Cb>
void arrayForEach(TypeTags tag, Value val, const Cb& cb) {
    if (tag == TypeTags::bsonArray) {
        auto bson = getRawPointerView(val);
        const auto* cur = bson + 4;
        const auto* end = bson + ConstDataView(bson).read<LittleEndian<uint32_t>>();

        while (cur != end - 1) {
            auto* fieldName = bson::fieldNameRaw(cur);
            size_t keySize = TinyStrHelpers::strlen(fieldName);
            auto [elemTag, elemVal] = bson::convertFrom<true>(cur, end, keySize);

            cb(elemTag, elemVal);
            cur = bson::advance(cur, keySize);
        }
    } else if (tag == TypeTags::Array) {
        auto array = getArrayView(val);
        for (size_t i = 0; i < array->size(); ++i) {
            auto [t, v] = array->getAt(i);
            cb(t, v);
        }
    } else if (tag == TypeTags::ArraySet) {
        auto arraySet = getArraySetView(val);
        for (auto [t, v] : arraySet->values()) {
            cb(t, v);
        }
    } else {
        MONGO_UNREACHABLE;
    }
}
}  // namespace value
}  // namespace sbe
}  // namespace mongo
