/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/crypto/rsa_public_key.h"

#include <iterator>

#include "fmt/format.h"
#include "mongo/base/string_data.h"
#include "mongo/util/base64.h"

namespace mongo::crypto {

RsaPublicKey::RsaPublicKey(StringData keyId, StringData e, StringData n)
    : _keyId(keyId.toString()) {
    fmt::memory_buffer eBuffer;
    base64url::decode(eBuffer, e);
    std::copy(eBuffer.begin(), eBuffer.end(), std::back_inserter(_e));
    fmt::memory_buffer nBuffer;
    base64url::decode(nBuffer, n);
    std::copy(nBuffer.begin(), nBuffer.end(), std::back_inserter(_n));
}
}  // namespace mongo::crypto
