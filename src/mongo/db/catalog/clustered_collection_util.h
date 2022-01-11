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

#pragma once

#include "mongo/db/catalog/clustered_collection_options_gen.h"
#include "mongo/db/operation_context.h"

namespace mongo {
namespace clustered_util {

/**
 * Constructs ClusteredCollectionInfo assuming legacy format {clusteredIndex: <bool>}. The
 * collection defaults to being clustered by '_id'
 */
ClusteredCollectionInfo makeCanonicalClusteredInfoForLegacyFormat();

/**
 * Generates the default _id clustered index.
 */
ClusteredCollectionInfo makeDefaultClusteredIdIndex();

/**
 * Constructs ClusteredCollectionInfo according to the 'indexSpec'. Constructs a 'name' by default
 * if the field is not yet defined. Stores the information is provided in the non-legacy format.
 */
ClusteredCollectionInfo makeCanonicalClusteredInfo(ClusteredIndexSpec indexSpec);

boost::optional<ClusteredCollectionInfo> parseClusteredInfo(const BSONElement& elem);

/**
 * Commands like createIndex() can implicitly create collections. If the index specifies the
 * 'clustered' field, then generate ClusteredCollectionInfo for the collection. Throws if invalid
 * specs are provided.
 */
boost::optional<ClusteredCollectionInfo> createClusteredInfoForNewCollection(
    const BSONObj& indexSpec);

/**
 * Returns true if legacy format is required for the namespace.
 */
bool requiresLegacyFormat(const NamespaceString& nss);

/**
 * listIndexes requires the ClusteredIndexSpec be formatted with an additional field 'clustered:
 * true' to indicate it is a clustered index.
 */
BSONObj formatClusterKeyForListIndexes(const ClusteredCollectionInfo& collInfo);

/**
 * Returns true if the BSON object matches the collection's cluster key. Caller's should ensure
 * keyPatternObj is the 'key' of the index spec of interest, not the entire index spec BSON.
 */
bool matchesClusterKey(const BSONObj& keyPatternObj,
                       const boost::optional<ClusteredCollectionInfo>& collInfo);

/**
 * Returns an error if any field of the indexSpec conflicts with the field of the clusteredIndex.
 */
Status checkSpecDoesNotConflictWithClusteredIndex(const BSONObj& indexSpec,
                                                  const ClusteredIndexSpec& clusteredIndexSpec);

/**
 * Returns true if the collection is clustered on the _id field.
 */
bool isClusteredOnId(const boost::optional<ClusteredCollectionInfo>& collInfo);

/**
 * Returns the field name of a cluster key.
 */
StringData getClusterKeyFieldName(const ClusteredIndexSpec& indexSpec);

}  // namespace clustered_util
}  // namespace mongo
