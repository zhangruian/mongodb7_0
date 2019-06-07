/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <functional>
#include <set>
#include <string>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/string_map.h"

namespace mongo {

using findBSONTypeAliasFun = std::function<boost::optional<BSONType>(StringData)>;

/**
 * Represents a set of types or of type aliases in the match language. The set consists of the BSON
 * types as well as "number", which is an alias for all numeric BSON types (NumberInt, NumberLong,
 * and so on).
 */
struct MatcherTypeSet {
    static constexpr StringData kMatchesAllNumbersAlias = "number"_sd;

    // Maps from the set of JSON Schema primitive types to the corresponding BSON types. Excludes
    // "number" since this alias maps to a set of BSON types, and "integer" since it is not
    // supported.
    static const StringMap<BSONType> kJsonSchemaTypeAliasMap;

    static boost::optional<BSONType> findJsonSchemaTypeAlias(StringData key);

    /**
     * Given a mapping from string alias to BSON type, creates a MatcherTypeSet from a
     * BSONElement. This BSON alias may either represent a single type (via numerical type code or
     * string alias), or may be an array of types.
     *
     * Returns an error if the element cannot be parsed to a set of types.
     */
    static StatusWith<MatcherTypeSet> parse(BSONElement);

    /**
     * Given a set of string type alias and a mapping from string alias to BSON type, returns the
     * corresponding MatcherTypeSet.
     *
     * Returns an error if any of the string aliases are unknown.
     */
    static StatusWith<MatcherTypeSet> fromStringAliases(std::set<StringData> typeAliases,
                                                        const findBSONTypeAliasFun& aliasMapFind);

    /**
     * Constructs an empty type set.
     */
    MatcherTypeSet() = default;

    /* implicit */ MatcherTypeSet(BSONType bsonType) : bsonTypes({bsonType}) {}

    /**
     * Returns true if 'bsonType' is present in the set.
     */
    bool hasType(BSONType bsonType) const {
        return (allNumbers && isNumericBSONType(bsonType)) ||
            bsonTypes.find(bsonType) != bsonTypes.end();
    }

    /**
     * Returns true if this set contains a single type or type alias. For instance, returns true if
     * the set is {"number"} or {"int"}, but not if the set is empty or {"number", "string"}.
     */
    bool isSingleType() const {
        return (allNumbers && bsonTypes.empty()) || (!allNumbers && bsonTypes.size() == 1u);
    }

    bool isEmpty() const {
        return !allNumbers && bsonTypes.empty();
    }

    void toBSONArray(BSONArrayBuilder* builder) const;

    BSONArray toBSONArray() const {
        BSONArrayBuilder builder;
        toBSONArray(&builder);
        return builder.arr();
    }

    bool operator==(const MatcherTypeSet& other) const {
        return allNumbers == other.allNumbers && bsonTypes == other.bsonTypes;
    }

    bool operator!=(const MatcherTypeSet& other) const {
        return !(*this == other);
    }

    bool allNumbers = false;
    std::set<BSONType> bsonTypes;
};

/**
 * An IDL-compatible wrapper class for MatcherTypeSet for BSON type aliases.
 * It represents a set of types or of type aliases in the match language.
 */
class BSONTypeSet {
public:
    static BSONTypeSet parseFromBSON(const BSONElement& element);

    BSONTypeSet(MatcherTypeSet typeSet) : _typeSet(std::move(typeSet)) {}

    void serializeToBSON(StringData fieldName, BSONObjBuilder* builder) const;

    const MatcherTypeSet& typeSet() const {
        return _typeSet;
    }

    bool operator==(const BSONTypeSet& other) const {
        return _typeSet == other._typeSet;
    }

    /**
     * IDL requires overload of all comparison operators, however for this class the only viable
     * comparison is equality. These should be removed once SERVER-39677 is implemented.
     */
    bool operator>(const BSONTypeSet& other) const {
        MONGO_UNREACHABLE;
    }

    bool operator<(const BSONTypeSet& other) const {
        MONGO_UNREACHABLE;
    }

private:
    MatcherTypeSet _typeSet;
};
}  // namespace mongo
