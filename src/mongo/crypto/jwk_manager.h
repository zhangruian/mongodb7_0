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

#pragma once

#include <map>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/crypto/jws_validator.h"


namespace mongo::crypto {

class JWKManager {
public:
    using SharedValidator = std::shared_ptr<JWSValidator>;
    using KeyMap = std::map<std::string, BSONObj>;

    /**
     * Fetch a JWKS file from the specified URL, parse them as keys, and instantiate JWSValidator
     * instances if loadAtStartup is set as true. Otherwise the keys will be fetched from the source
     * and JWSValidators initiated during the next JIT refresh.
     */
    explicit JWKManager(StringData source, bool loadAtStartup);

    /**
     * Parse a BSONObj array of keys, and instantiate JWSValidator instances.
     * This was added for testing purposes.
     */
    explicit JWKManager(BSONObj keys);

    /**
     * Fetch a specific JWSValidator from the JWKManager by keyId.
     * If the keyId does not exist, it will refresh _keyMaterial and _validators and retry.
     * If it still cannot be found, it will return an error.
     */
    StatusWith<SharedValidator> getValidator(StringData keyId);

    std::size_t size() const {
        return _validators->size();
    }

    /**
     * Get current keys.
     */
    const KeyMap& getKeys() const {
        return *_keyMaterial;
    }

    bool getIsKeyModified() const {
        return _isKeyModified;
    }

    /**
     * Serialize the JWKs stored in this key manager into the BSONObjBuilder.
     */
    void serialize(BSONObjBuilder* bob) const;

private:
    void _setAndValidateKeys(const BSONObj& keys);

    void _loadKeysFromUri();

    bool _haveKeysBeenModified(const KeyMap& newKeyMaterial) const;

private:
    // Stores the current key material of the manager, which may have been refreshed.
    std::shared_ptr<KeyMap> _keyMaterial;

    boost::optional<std::string> _keyURI;
    std::shared_ptr<std::map<std::string, SharedValidator>> _validators;

    // If an existing key got deleted or modified while doing a just in time refresh, we activate
    // this flag to indicate that a refresh occurred during this JWKManager's lifetime.
    bool _isKeyModified;
};

}  // namespace mongo::crypto
