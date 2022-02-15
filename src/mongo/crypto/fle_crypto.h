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

#include <array>
#include <boost/optional.hpp>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/data_range.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/uuid.h"


namespace mongo {

using PrfBlock = std::array<std::uint8_t, 32>;
using KeyMaterial = std::array<std::uint8_t, 32>;

// u = [1, max parallel clients)
using FLEContentionFactor = std::uint64_t;
using FLECounter = std::uint64_t;

/**
 * There are two types of keys that are user supplied.
 * 1. Index, aka S - this encrypts the index structures
 * 2. User, aka K - this encrypts the user data. It can be the same as Index.
 *
 * These keys only exist on the client, they are never on the server-side.
 */
enum class FLEKeyType {
    Index,  // i.e. S
    User,   // i.e. K
};

/**
 * Template class to ensure unique C++ types for each key.
 */
template <FLEKeyType KeyT>
struct FLEKey {
    FLEKey() = default;

    FLEKey(KeyMaterial dataIn) : data(std::move(dataIn)) {}

    ConstDataRange toCDR() const {
        return ConstDataRange(data.data(), data.data() + data.size());
    }

    // Actual type of the key
    FLEKeyType type{KeyT};

    // Raw bytes of the key
    KeyMaterial data;
};

using FLEIndexKey = FLEKey<FLEKeyType::Index>;
using FLEUserKey = FLEKey<FLEKeyType::User>;

/**
 * Key Material and its UUID id.
 *
 * The UUID is persisted into the serialized structures so that decryption is self-describing.
 */
template <FLEKeyType KeyT>
struct FLEKeyAndId {

    FLEKeyAndId(KeyMaterial material, UUID uuid) : key(material), keyId(uuid) {}

    FLEKey<KeyT> key;
    UUID keyId;
};

using FLEIndexKeyAndId = FLEKeyAndId<FLEKeyType::Index>;
using FLEUserKeyAndId = FLEKeyAndId<FLEKeyType::User>;


/*
 * The many token types are derived from the index key
 *
 * Terminology
 * f = field
 * v = value
 * u == 0 if field has no contention otherwise u = random secure sample {1, .. max contention}.
 *
 * CollectionsLevel1Token = HMAC(IndexKey, 1) = K_{f,1}
 * ServerDataEncryptionLevel1Token = HMAC(IndexKey, 3) = K_{f,3} = Fs[f,3]
 *
 * EDCToken = HMAC(CollectionsLevel1Token, 1) = K^{edc}_f
 * ESCToken = HMAC(CollectionsLevel1Token, 2) = K^{esc}_f
 * ECCToken = HMAC(CollectionsLevel1Token, 3) = K^{ecc}_f
 * ECOCToken = HMAC(CollectionsLevel1Token, 4) = K^{ecoc}_f = Fs[f,1,4]
 *
 * EDCDerivedFromDataToken = HMAC(EDCToken, v) = K^{edc}_{f,v} = Fs[f,1,1,v]
 * ESCDerivedFromDataToken = HMAC(ESCToken, v) = K^{esc}_{f,v} = Fs[f,1,2,v]
 * ECCDerivedFromDataToken = HMAC(ECCToken, v) = K^{ecc}_{f,v} = Fs[f,1,3,v]
 *
 * EDCDerivedFromDataTokenAndContentionFactorToken = HMAC(EDCDerivedFromDataToken, u) =
 * Fs[f,1,1,v,u] ESCDerivedFromDataTokenAndContentionFactorToken = HMAC(ESCDerivedFromDataToken, u)
 * = Fs[f,1,2,v,u] ECCDerivedFromDataTokenAndContentionFactorToken = HMAC(ECCDerivedFromDataToken,
 * u) = Fs[f,1,3,v,u]
 *
 * EDCTwiceDerivedToken = HMAC(EDCDerivedFromDataTokenAndContentionFactorToken, 1) = Fs_edc(1)
 * ESCTwiceDerivedTagToken = HMAC(ESCDerivedFromDataTokenAndContentionFactorToken, 1) = Fs_esc(1)
 * ESCTwiceDerivedValueToken = HMAC(ESCDerivedFromDataTokenAndContentionFactorToken, 2) = Fs_esc(2)
 * ECCTwiceDerivedTagToken = HMAC(ECCDerivedFromDataTokenAndContentionFactorToken, 1) = Fs_ecc(1)
 * ECCTwiceDerivedValueToken = HMAC(ECCDerivedFromDataTokenAndContentionFactorToken, 2) = Fs_ecc(2)
 */
enum class FLETokenType {
    CollectionsLevel1Token,
    ServerDataEncryptionLevel1Token,

    EDCToken,
    ESCToken,
    ECCToken,
    ECOCToken,

    EDCDerivedFromDataToken,
    ESCDerivedFromDataToken,
    ECCDerivedFromDataToken,

    EDCDerivedFromDataTokenAndContentionFactorToken,
    ESCDerivedFromDataTokenAndContentionFactorToken,
    ECCDerivedFromDataTokenAndContentionFactorToken,

    EDCTwiceDerivedToken,
    ESCTwiceDerivedTagToken,
    ESCTwiceDerivedValueToken,
    ECCTwiceDerivedTagToken,
    ECCTwiceDerivedValueToken,

};

/**
 * Templated C++ class that contains a token. A templated class is used to create a strongly typed
 * API that is hard to misuse.
 */
template <FLETokenType TokenT>
struct FLEToken {
    FLEToken() = default;

    FLEToken(PrfBlock dataIn) : data(std::move(dataIn)) {}

    ConstDataRange toCDR() const {
        return ConstDataRange(data.data(), data.data() + data.size());
    }

    FLETokenType type{TokenT};
    PrfBlock data;
};


using CollectionsLevel1Token = FLEToken<FLETokenType::CollectionsLevel1Token>;
using ServerDataEncryptionLevel1Token = FLEToken<FLETokenType::ServerDataEncryptionLevel1Token>;
using EDCToken = FLEToken<FLETokenType::EDCToken>;
using ESCToken = FLEToken<FLETokenType::ESCToken>;
using ECCToken = FLEToken<FLETokenType::ECCToken>;
using ECOCToken = FLEToken<FLETokenType::ECOCToken>;
using EDCDerivedFromDataToken = FLEToken<FLETokenType::EDCDerivedFromDataToken>;
using ESCDerivedFromDataToken = FLEToken<FLETokenType::ESCDerivedFromDataToken>;
using ECCDerivedFromDataToken = FLEToken<FLETokenType::ECCDerivedFromDataToken>;
using EDCDerivedFromDataTokenAndContentionFactorToken =
    FLEToken<FLETokenType::EDCDerivedFromDataTokenAndContentionFactorToken>;
using ESCDerivedFromDataTokenAndContentionFactorToken =
    FLEToken<FLETokenType::ESCDerivedFromDataTokenAndContentionFactorToken>;
using ECCDerivedFromDataTokenAndContentionFactorToken =
    FLEToken<FLETokenType::ECCDerivedFromDataTokenAndContentionFactorToken>;
using EDCTwiceDerivedToken = FLEToken<FLETokenType::EDCTwiceDerivedToken>;
using ESCTwiceDerivedTagToken = FLEToken<FLETokenType::ESCTwiceDerivedTagToken>;
using ESCTwiceDerivedValueToken = FLEToken<FLETokenType::ESCTwiceDerivedValueToken>;
using ECCTwiceDerivedTagToken = FLEToken<FLETokenType::ECCTwiceDerivedTagToken>;
using ECCTwiceDerivedValueToken = FLEToken<FLETokenType::ECCTwiceDerivedValueToken>;


/**
 * Generate tokens from the Index Key
 */
class FLELevel1TokenGenerator {
public:
    /**
     * CollectionsLevel1Token = HMAC(IndexKey, 1) = K_{f,1}
     */
    static CollectionsLevel1Token generateCollectionsLevel1Token(FLEIndexKey indexKey);

    /**
     * CollectionsLevel1Token =HMAC(IndexKey, 3) = K_{f,3}
     */
    static ServerDataEncryptionLevel1Token generateServerDataEncryptionLevel1Token(
        FLEIndexKey indexKey);
};

/**
 * Generate tokens from the CollectionsLevel1Token for use with the various collections.
 */
class FLECollectionTokenGenerator {
public:
    /**
     * EDCToken = HMAC(CollectionsLevel1Token, 1) = K^{edc}_f
     */
    static EDCToken generateEDCToken(CollectionsLevel1Token token);

    /**
     * ESCToken = HMAC(CollectionsLevel1Token, 2) = K^{esc}_f
     */
    static ESCToken generateESCToken(CollectionsLevel1Token token);

    /**
     * ECCToken = HMAC(CollectionsLevel1Token, 3) = K^{ecc}_f
     */
    static ECCToken generateECCToken(CollectionsLevel1Token token);

    /**
     * ECOCToken = HMAC(CollectionsLevel1Token, 4) = K^{ecoc}_f
     */
    static ECOCToken generateECOCToken(CollectionsLevel1Token token);
};


/**
 * Generate tokens for the various collections derived from the user data.
 */
class FLEDerivedFromDataTokenGenerator {
public:
    /**
     * EDCDerivedFromDataToken = HMAC(EDCToken, v) = K^{edc}_{f,v}
     */
    static EDCDerivedFromDataToken generateEDCDerivedFromDataToken(EDCToken token,
                                                                   ConstDataRange value);

    /**
     * ESCDerivedFromDataToken = HMAC(ESCToken, v) = K^{esc}_{f,v}
     */
    static ESCDerivedFromDataToken generateESCDerivedFromDataToken(ESCToken token,
                                                                   ConstDataRange value);

    /**
     * ECCDerivedFromDataToken = HMAC(ECCToken, v) = K^{ecc}_{f,v}
     */
    static ECCDerivedFromDataToken generateECCDerivedFromDataToken(ECCToken token,
                                                                   ConstDataRange value);
};

/**
 * Generate tokens for the various collections derived from the user data and a contention factor.
 */
class FLEDerivedFromDataTokenAndContentionFactorTokenGenerator {
public:
    /**
     * EDCDerivedFromDataTokenAndContentionFactorToken = HMAC(EDCDerivedFromDataToken, u)
     */
    static EDCDerivedFromDataTokenAndContentionFactorToken
    generateEDCDerivedFromDataTokenAndContentionFactorToken(EDCDerivedFromDataToken token,
                                                            FLECounter counter);

    /**
     * ESCDerivedFromDataTokenAndContentionFactorToken = HMAC(ESCDerivedFromDataToken, u)
     */
    static ESCDerivedFromDataTokenAndContentionFactorToken
    generateESCDerivedFromDataTokenAndContentionFactorToken(ESCDerivedFromDataToken token,
                                                            FLECounter counter);

    /**
     * ECCDerivedFromDataTokenAndContentionFactorToken = HMAC(ECCDerivedFromDataToken, u)
     */
    static ECCDerivedFromDataTokenAndContentionFactorToken
    generateECCDerivedFromDataTokenAndContentionFactorToken(ECCDerivedFromDataToken token,
                                                            FLECounter counter);
};

/**
 * Generate tokens for the various collections derived from counter tokens.
 */
class FLETwiceDerivedTokenGenerator {
public:
    /**
     * EDCTwiceDerivedToken = HMAC(EDCDerivedFromDataTokenAndContentionFactorToken, 1)
     */
    static EDCTwiceDerivedToken generateEDCTwiceDerivedToken(
        EDCDerivedFromDataTokenAndContentionFactorToken token);

    /**
     * ESCTwiceDerivedTagToken = HMAC(ESCDerivedFromDataTokenAndContentionFactorToken, 1)
     */
    static ESCTwiceDerivedTagToken generateESCTwiceDerivedTagToken(
        ESCDerivedFromDataTokenAndContentionFactorToken token);

    /**
     * ESCTwiceDerivedValueToken = HMAC(ESCDerivedFromDataTokenAndContentionFactorToken, 2)
     */
    static ESCTwiceDerivedValueToken generateESCTwiceDerivedValueToken(
        ESCDerivedFromDataTokenAndContentionFactorToken token);

    /**
     * ECCTwiceDerivedTagToken = HMAC(ECCDerivedFromDataTokenAndContentionFactorToken, 1)
     */
    static ECCTwiceDerivedTagToken generateECCTwiceDerivedTagToken(
        ECCDerivedFromDataTokenAndContentionFactorToken token);

    /**
     * ECCTwiceDerivedValueToken = HMAC(ECCDerivedFromDataTokenAndContentionFactorToken, 2)
     */
    static ECCTwiceDerivedValueToken generateECCTwiceDerivedValueToken(
        ECCDerivedFromDataTokenAndContentionFactorToken token);
};


/**
 * ESC Collection schema
 * {
 *    _id : HMAC(ESCTwiceDerivedTagToken, type || pos )
 *    value : Encrypt(ESCTwiceDerivedValueToken,  count_type || count)
 * }
 *
 * where
 *  type = uint64_t
 *  pos = uint64_t
 *  count_type = uint64_t
 *  count = uint64_t
 *
 * where type
 *   0 - null record
 *   1 - insert record, positional record, or compaction record
 *
 * where count_type:
 *   0 - regular count
 *   [1, UINT64_MAX) = position
 *   UINT64_MAX - compaction placeholder
 *
 * Record types:
 *
 * Document Counts
 * Null: 0 or 1
 * Insert: 0 or more
 * Positional: 0 or more
 * Compaction: 0 or 1
 *
 * Null record:
 * {
 *    _id : HMAC(ESCTwiceDerivedTagToken, null )
 *    value : Encrypt(ESCTwiceDerivedValueToken,  pos || count)
 * }
 *
 * Insert record:
 * {
 *    _id : HMAC(ESCTwiceDerivedTagToken, pos )
 *    value : Encrypt(ESCTwiceDerivedValueToken,  0 || count)
 * }
 *
 * Positional record:
 * {
 *    _id : HMAC(ESCTwiceDerivedTagToken, pos )
 *    value : Encrypt(ESCTwiceDerivedValueToken,  pos' || count)
 * }
 *
 * Compaction placeholder record:
 * {
 *    _id : HMAC(ESCTwiceDerivedTagToken, pos )
 *    value : Encrypt(ESCTwiceDerivedValueToken,  UINT64_MAX || 0)
 * }
 *
 * PlainText of _id
 * struct {
 *    uint64_t type;
 *    uint64_t pos;
 * }
 *
 * PlainText of value
 * struct {
 *    uint64_t count_type;
 *    uint64_t count;
 * }
 */


struct ESCNullDocument {
    // Id is not included as it is HMAC generated and cannot be reversed
    uint64_t pos;
    uint64_t count;
};


struct ESCDocument {
    // Id is not included as it is HMAC generated and cannot be reversed
    bool compactionPlaceholder;
    uint64_t position;
    uint64_t count;
};


/**
 * Interface for reading from a collection for the "EmuBinary" algorithm
 */
class FLEStateCollectionReader {
public:
    virtual ~FLEStateCollectionReader() = default;

    /**
     * Get a count of documents in the collection.
     *
     * TODO - how perfect does it need to be ? Is too high or too low ok if it is just an estimate?
     */
    virtual uint64_t getDocumentCount() = 0;

    /**
     * Get a document by its _id.
     */
    virtual BSONObj getById(PrfBlock block) = 0;
};

class ESCCollection {
public:
    /**
     * Generate the _id value
     */
    static PrfBlock generateId(ESCTwiceDerivedTagToken tagToken, boost::optional<uint64_t> index);

    /**
     * Generate a null document which will be the "first" document for a given field.
     */
    static BSONObj generateNullDocument(ESCTwiceDerivedTagToken tagToken,
                                        ESCTwiceDerivedValueToken valueToken,
                                        uint64_t pos,
                                        uint64_t count);

    /**
     * Generate a insert ESC document.
     */
    static BSONObj generateInsertDocument(ESCTwiceDerivedTagToken tagToken,
                                          ESCTwiceDerivedValueToken valueToken,
                                          uint64_t index,
                                          uint64_t count);

    /**
     * Generate a positional ESC document.
     */
    static BSONObj generatePositionalDocument(ESCTwiceDerivedTagToken tagToken,
                                              ESCTwiceDerivedValueToken valueToken,
                                              uint64_t index,
                                              uint64_t pos,
                                              uint64_t count);

    /**
     * Generate a compaction placeholder ESC document.
     */
    static BSONObj generateCompactionPlaceholderDocument(ESCTwiceDerivedTagToken tagToken,
                                                         ESCTwiceDerivedValueToken valueToken,
                                                         uint64_t index);

    /**
     * Decrypt the null document.
     */
    static StatusWith<ESCNullDocument> decryptNullDocument(ESCTwiceDerivedValueToken valueToken,
                                                           BSONObj& doc);

    /**
     * Decrypt a regular document.
     */
    static StatusWith<ESCDocument> decryptDocument(ESCTwiceDerivedValueToken valueToken,
                                                   BSONObj& doc);

    /**
     * Search for the highest document id for a given field/value pair based on the token.
     */
    static uint64_t emuBinary(FLEStateCollectionReader* reader,
                              ESCTwiceDerivedTagToken tagToken,
                              ESCTwiceDerivedValueToken valueToken);
};


/**
 * ECC Collection
 * - a record of deleted documents
 *
 * {
 *    _id : HMAC(ECCTwiceDerivedTagToken, type || pos )
 *    value : Encrypt(ECCTwiceDerivedValueToken,  count OR start || end)
 * }
 *
 * where
 *  type = uint64_t
 *  pos = uint64_t
 *  value is either:
 *       count = uint64_t  // Null records
 *    OR
 *       start = uint64_t  // Other records
 *       end = uint64_t
 *
 * where type:
 *   0 - null record
 *   1 - regular record or compaction record
 *
 * where start and end:
 *   [0..UINT_64_MAX) - regular start and end
 *   UINT64_MAX - compaction placeholder
 *
 * Record types:
 *
 * Document Counts
 * Null: 0 or 1
 * Regular: 0 or more
 * Compaction: 0 or 1
 *
 * Null record:
 * {
 *    _id : HMAC(ECCTwiceDerivedTagToken, null )
 *    value : Encrypt(ECCTwiceDerivedValueToken,  count)
 * }
 *
 * Regular record:
 * {
 *    _id : HMAC(ECCTwiceDerivedTagToken, pos )
 *    value : Encrypt(ECCTwiceDerivedValueToken,  start || end)
 * }
 *
 * Compaction placeholder record:
 * {
 *    _id : HMAC(ECCTwiceDerivedTagToken, pos )
 *    value : Encrypt(ECCTwiceDerivedValueToken,  UINT64_MAX || UINT64_MAX)
 * }
 *
 * PlainText of tag
 * struct {
 *    uint64_t type;
 *    uint64_t pos;
 * }
 *
 * PlainText of value for null records
 * struct {
 *    uint64_t count;
 * }
 *
 * PlainText of value for non-null records
 * struct {
 *    uint64_t start;
 *    uint64_t end;
 * }
 */
enum class ECCValueType : uint64_t {
    kNormal = 0,
    kCompactionPlaceholder = 1,
};


struct ECCNullDocument {
    // Id is not included as it HMAC generated and cannot be reversed
    uint64_t pos;
};


struct ECCDocument {
    // Id is not included as it HMAC generated and cannot be reversed
    ECCValueType valueType;
    uint64_t start;
    uint64_t end;
};

class ECCCollection {
public:
    /**
     * Generate the _id value
     */
    static PrfBlock generateId(ECCTwiceDerivedTagToken tagToken, boost::optional<uint64_t> index);

    /**
     * Generate a null document which will be the "first" document for a given field.
     */
    static BSONObj generateNullDocument(ECCTwiceDerivedTagToken tagToken,
                                        ECCTwiceDerivedValueToken valueToken,
                                        uint64_t count);

    /**
     * Generate a regular ECC document for (count).
     *
     * Note: it is stored as (count, count)
     */
    static BSONObj generateDocument(ECCTwiceDerivedTagToken tagToken,
                                    ECCTwiceDerivedValueToken valueToken,
                                    uint64_t index,
                                    uint64_t count);

    /**
     * Generate a regular ECC document for (start, end)
     */
    static BSONObj generateDocument(ECCTwiceDerivedTagToken tagToken,
                                    ECCTwiceDerivedValueToken valueToken,
                                    uint64_t index,
                                    uint64_t start,
                                    uint64_t end);

    /**
     * Generate a compaction ECC document.
     */
    static BSONObj generateCompactionDocument(ECCTwiceDerivedTagToken tagToken,
                                              ECCTwiceDerivedValueToken valueToken,
                                              uint64_t index);

    /**
     * Decrypt the null document.
     */
    static StatusWith<ECCNullDocument> decryptNullDocument(ECCTwiceDerivedValueToken valueToken,
                                                           BSONObj& doc);

    /**
     * Decrypt a regular document.
     */
    static StatusWith<ECCDocument> decryptDocument(ECCTwiceDerivedValueToken valueToken,
                                                   BSONObj& doc);

    /**
     * Search for the highest document id for a given field/value pair based on the token.
     */
    static uint64_t emuBinary(FLEStateCollectionReader* reader,
                              ECCTwiceDerivedTagToken tagToken,
                              ECCTwiceDerivedValueToken valueToken);
};
}  // namespace mongo
