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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/auto_split_vector.h"

#include "mongo/base/status_with.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace {

constexpr int estimatedAdditionalBytesPerItemInBSONArray{2};

BSONObj prettyKey(const BSONObj& keyPattern, const BSONObj& key) {
    return key.replaceFieldNames(keyPattern).clientReadable();
}

/*
 * Takes the given min/max BSON objects that are a prefix of the shardKey and return two new BSON
 * object extended to cover the entire shardKey. See KeyPattern::extendRangeBound documentation for
 * some examples.
 */
const std::tuple<BSONObj, BSONObj> getMinMaxExtendedBounds(const IndexDescriptor* shardKeyIdx,
                                                           const BSONObj& min,
                                                           const BSONObj& max) {
    KeyPattern kp(shardKeyIdx->keyPattern());

    // Extend min to get (min, MinKey, MinKey, ....)
    BSONObj minKey = Helpers::toKeyFormat(kp.extendRangeBound(min, false /* upperInclusive */));
    BSONObj maxKey;
    if (max.isEmpty()) {
        // if max not specified, make it (MaxKey, Maxkey, MaxKey...)
        maxKey = Helpers::toKeyFormat(kp.extendRangeBound(max, true /* upperInclusive */));
    } else {
        // otherwise make it (max,MinKey,MinKey...) so that bound is non-inclusive
        maxKey = Helpers::toKeyFormat(kp.extendRangeBound(max, false /* upperInclusive*/));
    }

    return {minKey, maxKey};
}

/*
 * Returns true if the final key in the range is the same as the first key, false otherwise.
 */
bool maxKeyEqualToMinKey(OperationContext* opCtx,
                         const CollectionPtr* collection,
                         const IndexDescriptor* shardKeyIdx,
                         const BSONObj& minBound,
                         const BSONObj& maxBound,
                         const BSONObj& minKeyInChunk) {
    BSONObj maxKeyInChunk;
    {
        auto exec = InternalPlanner::indexScan(opCtx,
                                               collection,
                                               shardKeyIdx,
                                               maxBound,
                                               minBound,
                                               BoundInclusion::kIncludeEndKeyOnly,
                                               PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                               InternalPlanner::BACKWARD);

        PlanExecutor::ExecState state = exec->getNext(&maxKeyInChunk, nullptr);
        uassert(ErrorCodes::OperationFailed,
                "can't open a cursor to find final key in range (desired range is possibly empty)",
                state == PlanExecutor::ADVANCED);
    }

    if (minKeyInChunk.woCompare(maxKeyInChunk) == 0) {
        // Range contains only documents with a single key value.  So we cannot possibly find a
        // split point, and there is no need to scan any further.
        LOGV2_WARNING(
            5865001,
            "Possible low cardinality key detected in range. Range contains only a single key.",
            "namespace"_attr = collection->get()->ns(),
            "minKey"_attr = redact(prettyKey(shardKeyIdx->keyPattern(), minBound)),
            "maxKey"_attr = redact(prettyKey(shardKeyIdx->keyPattern(), maxBound)),
            "key"_attr = redact(prettyKey(shardKeyIdx->keyPattern(), minKeyInChunk)));
        return true;
    }

    return false;
}

}  // namespace

std::vector<BSONObj> autoSplitVector(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     const BSONObj& keyPattern,
                                     const BSONObj& min,
                                     const BSONObj& max,
                                     long long maxChunkSizeBytes) {
    std::vector<BSONObj> splitKeys;

    int elapsedMillisToFindSplitPoints;

    // Contains each key appearing multiple times and estimated to be able to fill-in a chunk alone
    auto tooFrequentKeys = SimpleBSONObjComparator::kInstance.makeBSONObjSet();

    {
        AutoGetCollection collection(opCtx, nss, MODE_IS);

        uassert(ErrorCodes::NamespaceNotFound, "ns not found", collection);

        // Get the size estimate for this namespace
        const long long totalLocalCollDocuments = collection->numRecords(opCtx);
        const long long dataSize = collection->dataSize(opCtx);

        // Return empty vector if current estimated data size is less than max chunk size
        if (dataSize < maxChunkSizeBytes || totalLocalCollDocuments == 0) {
            return {};
        }

        // Allow multiKey based on the invariant that shard keys must be single-valued. Therefore,
        // any multi-key index prefixed by shard key cannot be multikey over the shard key fields.
        auto catalog = collection->getIndexCatalog();
        auto shardKeyIdx = catalog->findShardKeyPrefixedIndex(
            opCtx, *collection, keyPattern, /*requireSingleKey=*/false);
        uassert(ErrorCodes::IndexNotFound,
                str::stream() << "couldn't find index over splitting key "
                              << keyPattern.clientReadable().toString(),
                shardKeyIdx);

        const auto [minKey, maxKey] = getMinMaxExtendedBounds(shardKeyIdx, min, max);

        // Setup the index scanner that will be used to find the split points
        auto exec = InternalPlanner::indexScan(opCtx,
                                               &(*collection),
                                               shardKeyIdx,
                                               minKey,
                                               maxKey,
                                               BoundInclusion::kIncludeStartKeyOnly,
                                               PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                               InternalPlanner::FORWARD);

        // Get minimum key belonging to the chunk
        BSONObj minKeyInOriginalChunk;
        {
            PlanExecutor::ExecState state = exec->getNext(&minKeyInOriginalChunk, nullptr);
            uassert(ErrorCodes::OperationFailed,
                    "can't open a cursor to scan the range (desired range is possibly empty)",
                    state == PlanExecutor::ADVANCED);
        }

        // Return empty vector if chunk's min and max keys are the same.
        if (maxKeyEqualToMinKey(opCtx,
                                &collection.getCollection(),
                                shardKeyIdx,
                                minKey,
                                maxKey,
                                minKeyInOriginalChunk)) {
            return {};
        }

        LOGV2(5865000,
              "Requested split points lookup for chunk",
              "namespace"_attr = nss,
              "minKey"_attr = redact(prettyKey(keyPattern, minKey)),
              "maxKey"_attr = redact(prettyKey(keyPattern, maxKey)));

        // Use the average document size and number of documents to find the approximate number of
        // keys each chunk should contain
        const long long avgDocSize = dataSize / totalLocalCollDocuments;

        // Split at half the max chunk size
        long long maxDocsPerSplittedChunk = maxChunkSizeBytes / (2 * avgDocSize);

        BSONObj currentKey;               // Last key seen during the index scan
        long long numScannedKeys = 1;     // minKeyInOriginalChunk has already been scanned
        std::size_t resultArraySize = 0;  // Approximate size in bytes of the split points array

        // Reference to last split point that needs to be checked in order to avoid adding duplicate
        // split points. Initialized to the min of the first chunk being split.
        auto lastSplitPoint = dotted_path_support::extractElementsBasedOnTemplate(
            prettyKey(shardKeyIdx->keyPattern(), minKeyInOriginalChunk.getOwned()), keyPattern);

        Timer timer;  // To measure time elapsed while searching split points

        // Traverse the index and add the maxDocsPerSplittedChunk-th key to the result vector
        while (exec->getNext(&currentKey, nullptr) == PlanExecutor::ADVANCED) {
            numScannedKeys++;

            if (numScannedKeys > maxDocsPerSplittedChunk) {
                currentKey = dotted_path_support::extractElementsBasedOnTemplate(
                    prettyKey(shardKeyIdx->keyPattern(), currentKey.getOwned()), keyPattern);

                if (currentKey.woCompare(lastSplitPoint) == 0) {
                    // Do not add again the same split point in case of frequent shard key.
                    tooFrequentKeys.insert(currentKey.getOwned());
                    continue;
                }

                const auto additionalKeySize =
                    currentKey.objsize() + estimatedAdditionalBytesPerItemInBSONArray;
                if (resultArraySize + additionalKeySize > BSONObjMaxUserSize) {
                    if (splitKeys.empty()) {
                        // Keep trying until finding at least one split point that isn't above
                        // the max object user size. Very improbable corner case: the shard key
                        // size for the chosen split point is exactly 16MB.
                        continue;
                    }

                    LOGV2(5865002,
                          "Max BSON response size reached for split vector before the end "
                          "of chunk",
                          "namespace"_attr = nss,
                          "minKey"_attr = redact(prettyKey(shardKeyIdx->keyPattern(), minKey)),
                          "maxKey"_attr = redact(prettyKey(shardKeyIdx->keyPattern(), maxKey)));
                    break;
                }

                resultArraySize += additionalKeySize;
                splitKeys.push_back(currentKey.getOwned());
                lastSplitPoint = splitKeys.back();
                numScannedKeys = 0;

                LOGV2_DEBUG(5865003, 4, "Picked a split key", "key"_attr = redact(currentKey));
            }
        }

        elapsedMillisToFindSplitPoints = timer.millis();
    }

    // Emit a warning for each frequent key
    for (const auto& frequentKey : tooFrequentKeys) {
        LOGV2_WARNING(5865004,
                      "Possible low cardinality key detected",
                      "namespace"_attr = nss,
                      "key"_attr = redact(prettyKey(keyPattern, frequentKey)));
    }

    if (elapsedMillisToFindSplitPoints > serverGlobalParams.slowMS) {
        LOGV2_WARNING(5865005,
                      "Finding the auto split vector completed",
                      "namespace"_attr = nss,
                      "keyPattern"_attr = redact(keyPattern),
                      "numSplits"_attr = splitKeys.size(),
                      "duration"_attr = Milliseconds(elapsedMillisToFindSplitPoints));
    }

    // TODO SERVER-58750: investigate if it is really needed to sort the vector
    // Make sure splitKeys is in ascending order
    std::sort(
        splitKeys.begin(), splitKeys.end(), SimpleBSONObjComparator::kInstance.makeLessThan());

    return splitKeys;
}

}  // namespace mongo
