/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/bson/oid.h"
#include "mongo/platform/int128.h"

#include <array>
#include <boost/optional.hpp>
#include <cstdint>

namespace mongo {

/*
 * TypeCompressor: This class allows storing a wide variety of types using a series of compression
 * methods. Each supported type has an overloaded encode and decode method and a short comment
 * explaining the type of current compression used
 */
class Simple8bTypeUtil {
public:
    // These methods are for encoding a signed integer with simple8b. They move the signed bit from
    // the most significant bit position to the least significant bit to provide the ability to
    // store as an unsigned integer
    // the most significant bit position to the least significant bit and call simple8b as an
    // unsigned integer.
    static uint64_t encodeInt64(int64_t val);
    static int64_t decodeInt64(uint64_t val);
    static uint128_t encodeInt128(int128_t val);
    static int128_t decodeInt128(uint128_t val);

    // These methods are for encoding OID with simple8b. The unique identifier is not part of
    // the encoded integer and must thus be provided when decoding.
    // Re-organize the bytes so that most of the entropy is in the least significant bytes.
    // Since TS = Timestamp is in big endian and C = Counter is in big endian,
    // then rearrange the bytes to:
    // | Byte Usage | TS3 | C2 | TS2 | C1 | TS1 | C0 | TS0 |
    // | Byte Index |  0  |  1 |  2  | 3  |  4  | 5  |  6  |
    static int64_t encodeObjectId(const OID& oid);
    static OID decodeObjectId(int64_t val, OID::InstanceUnique processUnique);

    // These methods add floating point support for encoding and decoding with simple8b up to 8
    // decimal digits. They work by multiplying the floating point value by a multiple of 10 and
    // rounding to the nearest integer. They return a option that will not be valid in the case of a
    // value being greater than 8 decimal digits. Additionally, they will return a boost::none in
    // the cae that compression is not feasible.
    static boost::optional<uint8_t> calculateDecimalShiftMultiplier(double val);
    static boost::optional<int64_t> encodeDouble(double val, uint8_t scaleIndex);
    static double decodeDouble(int64_t val, uint8_t scaleIndex);

    // Array is a double as it will always be multiplied by a double and we don't want to do an
    // extra cast for
    static constexpr uint8_t kMemoryAsInteger = 5;
    static constexpr std::array<double, kMemoryAsInteger> kScaleMultiplier = {
        1, 10, 100, 10000, 100000000};
};
}  // namespace mongo
