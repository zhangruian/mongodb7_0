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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/chunk_manager.h"

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/logv2/log.h"
#include "mongo/s/chunk_writes_tracker.h"
#include "mongo/s/mongos_server_parameters_gen.h"
#include "mongo/s/shard_invalidated_for_targeting_exception.h"

namespace mongo {
namespace {

// Used to generate sequence numbers to assign to each newly created RoutingTableHistory
AtomicWord<unsigned> nextCMSequenceNumber(0);

bool allElementsAreOfType(BSONType type, const BSONObj& obj) {
    for (auto&& elem : obj) {
        if (elem.type() != type) {
            return false;
        }
    }
    return true;
}

void checkAllElementsAreOfType(BSONType type, const BSONObj& o) {
    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Not all elements of " << o << " are of type " << typeName(type),
            allElementsAreOfType(type, o));
}

std::string extractKeyStringInternal(const BSONObj& shardKeyValue, Ordering ordering) {
    BSONObjBuilder strippedKeyValue;
    for (const auto& elem : shardKeyValue) {
        strippedKeyValue.appendAs(elem, ""_sd);
    }

    KeyString::Builder ks(KeyString::Version::V1, strippedKeyValue.done(), ordering);
    return {ks.getBuffer(), ks.getSize()};
}

}  // namespace

ShardVersionMap ChunkMap::constructShardVersionMap(const OID& epoch) const {
    ShardVersionMap shardVersions;
    ChunkInfoMap::const_iterator current = _chunkMap.cbegin();

    boost::optional<BSONObj> firstMin = boost::none;
    boost::optional<BSONObj> lastMax = boost::none;

    while (current != _chunkMap.cend()) {
        const auto& firstChunkInRange = current->second;
        const auto& currentRangeShardId = firstChunkInRange->getShardIdAt(boost::none);

        // Tracks the max shard version for the shard on which the current range will reside
        auto shardVersionIt = shardVersions.find(currentRangeShardId);
        if (shardVersionIt == shardVersions.end()) {
            shardVersionIt = shardVersions.emplace(currentRangeShardId, epoch).first;
        }

        auto& maxShardVersion = shardVersionIt->second.shardVersion;

        current =
            std::find_if(current,
                         _chunkMap.cend(),
                         [&currentRangeShardId,
                          &maxShardVersion](const ChunkInfoMap::value_type& chunkMapEntry) {
                             const auto& currentChunk = chunkMapEntry.second;

                             if (currentChunk->getShardIdAt(boost::none) != currentRangeShardId)
                                 return true;

                             if (currentChunk->getLastmod() > maxShardVersion)
                                 maxShardVersion = currentChunk->getLastmod();

                             return false;
                         });

        const auto rangeLast = std::prev(current);

        const auto& rangeMin = firstChunkInRange->getMin();
        const auto& rangeMax = rangeLast->second->getMax();

        // Check the continuity of the chunks map
        if (lastMax && !SimpleBSONObjComparator::kInstance.evaluate(*lastMax == rangeMin)) {
            if (SimpleBSONObjComparator::kInstance.evaluate(*lastMax < rangeMin))
                uasserted(ErrorCodes::ConflictingOperationInProgress,
                          str::stream()
                              << "Gap exists in the routing table between chunks "
                              << _chunkMap
                                     .at(extractKeyStringInternal(*lastMax, _shardKeyOrdering))
                                     ->getRange()
                                     .toString()
                              << " and " << rangeLast->second->getRange().toString());
            else
                uasserted(ErrorCodes::ConflictingOperationInProgress,
                          str::stream()
                              << "Overlap exists in the routing table between chunks "
                              << _chunkMap
                                     .at(extractKeyStringInternal(*lastMax, _shardKeyOrdering))
                                     ->getRange()
                                     .toString()
                              << " and " << rangeLast->second->getRange().toString());
        }

        if (!firstMin)
            firstMin = rangeMin;

        lastMax = rangeMax;

        // If a shard has chunks it must have a shard version, otherwise we have an invalid chunk
        // somewhere, which should have been caught at chunk load time
        invariant(maxShardVersion.isSet());
    }

    if (!_chunkMap.empty()) {
        invariant(!shardVersions.empty());
        invariant(firstMin.is_initialized());
        invariant(lastMax.is_initialized());

        checkAllElementsAreOfType(MinKey, firstMin.get());
        checkAllElementsAreOfType(MaxKey, lastMax.get());
    }

    return shardVersions;
}

void ChunkMap::addChunk(const ChunkType& chunk) {
    const auto chunkMinKeyString = extractKeyStringInternal(chunk.getMin(), _shardKeyOrdering);
    const auto chunkMaxKeyString = extractKeyStringInternal(chunk.getMax(), _shardKeyOrdering);

    // Returns the first chunk with a max key that is > min - implies that the chunk overlaps
    // min
    const auto low = _chunkMap.upper_bound(chunkMinKeyString);

    // Returns the first chunk with a max key that is > max - implies that the next chunk cannot
    // not overlap max
    const auto high = _chunkMap.upper_bound(chunkMaxKeyString);

    // If we are in the middle of splitting a chunk, for the first few
    // chunks inserted, low == high, because both lookups will point to the
    // same chunk (the one being split). If we're inserting the last chunk
    // for the current chunk being split, low will point to the chunk that
    // we're splitting, and high will point to the next chunk past the one
    // we're splitting (which could be chunkMap.end()). In this case,
    // std::distance(low, high) == 1. Lastly, this does not apply during
    // the creation of the original routing table, in which case the map is
    // empty and the first chunk that is inserted will find that low ==
    // high, but low == chunkMap.end(), and we aren't doing a split in that
    // case.
    auto foundSingleChunk =
        ((low == high || std::distance(low, high) == 1) && low != _chunkMap.end());

    auto newChunk = std::make_shared<ChunkInfo>(chunk);
    if (foundSingleChunk) {
        auto chunkBeingReplacedBySplit = low->second;
        auto bytesInReplacedChunk =
            chunkBeingReplacedBySplit->getWritesTracker()->getBytesWritten();
        newChunk->getWritesTracker()->addBytesWritten(bytesInReplacedChunk);
    }

    // Erase all chunks from the map, which overlap the chunk we got from the persistent store
    _chunkMap.erase(low, high);

    // Insert only the chunk itself
    _chunkMap.insert(std::make_pair(chunkMaxKeyString, newChunk));
}

std::shared_ptr<ChunkInfo> ChunkMap::findIntersectingChunk(const BSONObj& shardKey) const {
    const auto it = _findIntersectingChunk(shardKey);

    if (it != _chunkMap.end())
        return it->second;

    return std::shared_ptr<ChunkInfo>();
}

ChunkMap::ChunkInfoMap::const_iterator ChunkMap::_findIntersectingChunk(
    const BSONObj& shardKey) const {
    return _chunkMap.upper_bound(extractKeyStringInternal(shardKey, _shardKeyOrdering));
}

std::pair<ChunkMap::ChunkInfoMap::const_iterator, ChunkMap::ChunkInfoMap::const_iterator>
ChunkMap::_overlappingBounds(const BSONObj& min, const BSONObj& max, bool isMaxInclusive) const {
    const auto itMin = _chunkMap.upper_bound(extractKeyStringInternal(min, _shardKeyOrdering));
    const auto itMax = [&]() {
        auto it = isMaxInclusive
            ? _chunkMap.upper_bound(extractKeyStringInternal(max, _shardKeyOrdering))
            : _chunkMap.lower_bound(extractKeyStringInternal(max, _shardKeyOrdering));
        return it == _chunkMap.end() ? it : ++it;
    }();

    return {itMin, itMax};
}

ShardVersionTargetingInfo::ShardVersionTargetingInfo(const OID& epoch)
    : shardVersion(0, 0, epoch) {}

RoutingTableHistory::RoutingTableHistory(NamespaceString nss,
                                         boost::optional<UUID> uuid,
                                         KeyPattern shardKeyPattern,
                                         std::unique_ptr<CollatorInterface> defaultCollator,
                                         bool unique,
                                         ChunkMap chunkMap,
                                         ChunkVersion collectionVersion)
    : _sequenceNumber(nextCMSequenceNumber.addAndFetch(1)),
      _nss(std::move(nss)),
      _uuid(uuid),
      _shardKeyPattern(shardKeyPattern),
      _defaultCollator(std::move(defaultCollator)),
      _unique(unique),
      _chunkMap(std::move(chunkMap)),
      _collectionVersion(collectionVersion),
      _shardVersions(_chunkMap.constructShardVersionMap(collectionVersion.epoch())) {}

void RoutingTableHistory::setShardStale(const ShardId& shardId) {
    if (gEnableFinerGrainedCatalogCacheRefresh) {
        auto it = _shardVersions.find(shardId);
        if (it != _shardVersions.end()) {
            it->second.isStale.store(true);
        }
    }
}

void RoutingTableHistory::setAllShardsRefreshed() {
    if (gEnableFinerGrainedCatalogCacheRefresh) {
        for (auto& [shard, targetingInfo] : _shardVersions) {
            targetingInfo.isStale.store(false);
        }
    }
}

Chunk ChunkManager::findIntersectingChunk(const BSONObj& shardKey, const BSONObj& collation) const {
    const bool hasSimpleCollation = (collation.isEmpty() && !_rt->getDefaultCollator()) ||
        SimpleBSONObjComparator::kInstance.evaluate(collation == CollationSpec::kSimpleSpec);
    if (!hasSimpleCollation) {
        for (BSONElement elt : shardKey) {
            uassert(ErrorCodes::ShardKeyNotFound,
                    str::stream() << "Cannot target single shard due to collation of key "
                                  << elt.fieldNameStringData() << " for namespace " << getns(),
                    !CollationIndexKey::isCollatableType(elt.type()));
        }
    }

    auto chunkInfo = _rt->findIntersectingChunk(shardKey);

    uassert(ErrorCodes::ShardKeyNotFound,
            str::stream() << "Cannot target single shard using key " << shardKey
                          << " for namespace " << getns(),
            chunkInfo && chunkInfo->containsKey(shardKey));

    return Chunk(*chunkInfo, _clusterTime);
}

bool ChunkManager::keyBelongsToShard(const BSONObj& shardKey, const ShardId& shardId) const {
    if (shardKey.isEmpty())
        return false;

    auto chunkInfo = _rt->findIntersectingChunk(shardKey);
    if (!chunkInfo)
        return false;

    invariant(chunkInfo->containsKey(shardKey));

    return chunkInfo->getShardIdAt(_clusterTime) == shardId;
}

void ChunkManager::getShardIdsForQuery(OperationContext* opCtx,
                                       const BSONObj& query,
                                       const BSONObj& collation,
                                       std::set<ShardId>* shardIds) const {
    auto qr = std::make_unique<QueryRequest>(_rt->getns());
    qr->setFilter(query);

    if (!collation.isEmpty()) {
        qr->setCollation(collation);
    } else if (_rt->getDefaultCollator()) {
        qr->setCollation(_rt->getDefaultCollator()->getSpec().toBSON());
    }

    const boost::intrusive_ptr<ExpressionContext> expCtx;
    auto cq = uassertStatusOK(
        CanonicalQuery::canonicalize(opCtx,
                                     std::move(qr),
                                     expCtx,
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures));

    // Fast path for targeting equalities on the shard key.
    auto shardKeyToFind = _rt->getShardKeyPattern().extractShardKeyFromQuery(*cq);
    if (!shardKeyToFind.isEmpty()) {
        try {
            auto chunk = findIntersectingChunk(shardKeyToFind, collation);
            shardIds->insert(chunk.getShardId());
            return;
        } catch (const DBException&) {
            // The query uses multiple shards
        }
    }

    // Transforms query into bounds for each field in the shard key
    // for example :
    //   Key { a: 1, b: 1 },
    //   Query { a : { $gte : 1, $lt : 2 },
    //            b : { $gte : 3, $lt : 4 } }
    //   => Bounds { a : [1, 2), b : [3, 4) }
    IndexBounds bounds = getIndexBoundsForQuery(_rt->getShardKeyPattern().toBSON(), *cq);

    // Transforms bounds for each shard key field into full shard key ranges
    // for example :
    //   Key { a : 1, b : 1 }
    //   Bounds { a : [1, 2), b : [3, 4) }
    //   => Ranges { a : 1, b : 3 } => { a : 2, b : 4 }
    BoundList ranges = _rt->getShardKeyPattern().flattenBounds(bounds);

    for (BoundList::const_iterator it = ranges.begin(); it != ranges.end(); ++it) {
        getShardIdsForRange(it->first /*min*/, it->second /*max*/, shardIds);

        // Once we know we need to visit all shards no need to keep looping.
        // However, this optimization does not apply when we are reading from a snapshot
        // because _shardVersions contains shards with chunks and is built based on the last
        // refresh. Therefore, it is possible for _shardVersions to have fewer entries if a shard
        // no longer owns chunks when it used to at _clusterTime.
        if (!_clusterTime && shardIds->size() == _rt->_shardVersions.size()) {
            break;
        }
    }

    // SERVER-4914 Some clients of getShardIdsForQuery() assume at least one shard will be returned.
    // For now, we satisfy that assumption by adding a shard with no matches rather than returning
    // an empty set of shards.
    if (shardIds->empty()) {
        _rt->forEachChunk([&](const std::shared_ptr<ChunkInfo>& chunkInfo) {
            shardIds->insert(chunkInfo->getShardIdAt(_clusterTime));
            return false;
        });
    }
}

void ChunkManager::getShardIdsForRange(const BSONObj& min,
                                       const BSONObj& max,
                                       std::set<ShardId>* shardIds) const {
    // If our range is [MinKey, MaxKey], we can simply return all shard ids right away. However,
    // this optimization does not apply when we are reading from a snapshot because _shardVersions
    // contains shards with chunks and is built based on the last refresh. Therefore, it is
    // possible for _shardVersions to have fewer entries if a shard no longer owns chunks when it
    // used to at _clusterTime.
    if (!_clusterTime && allElementsAreOfType(MinKey, min) && allElementsAreOfType(MaxKey, max)) {
        getAllShardIds(shardIds);
        return;
    }

    _rt->forEachOverlappingChunk(min, max, true, [&](auto& chunkInfo) {
        shardIds->insert(chunkInfo->getShardIdAt(_clusterTime));

        // No need to iterate through the rest of the ranges, because we already know we need to use
        // all shards. However, this optimization does not apply when we are reading from a snapshot
        // because _shardVersions contains shards with chunks and is built based on the last
        // refresh. Therefore, it is possible for _shardVersions to have fewer entries if a shard
        // no longer owns chunks when it used to at _clusterTime.
        if (!_clusterTime && shardIds->size() == _rt->_shardVersions.size()) {
            return false;
        }

        return true;
    });
}

bool ChunkManager::rangeOverlapsShard(const ChunkRange& range, const ShardId& shardId) const {
    bool overlapFound = false;

    _rt->forEachOverlappingChunk(range.getMin(), range.getMax(), false, [&](auto& chunkInfo) {
        if (chunkInfo->getShardIdAt(_clusterTime) == shardId) {
            overlapFound = true;
            return false;
        }

        return true;
    });

    return overlapFound;
}

boost::optional<Chunk> ChunkManager::getNextChunkOnShard(const BSONObj& shardKey,
                                                         const ShardId& shardId) const {
    boost::optional<Chunk> chunk;

    _rt->forEachChunk(
        [&](auto& chunkInfo) {
            if (chunkInfo->getShardIdAt(_clusterTime) == shardId) {
                chunk.emplace(*chunkInfo, _clusterTime);
                return false;
            }
            return true;
        },
        shardKey);

    return chunk;
}

ShardId ChunkManager::getMinKeyShardIdWithSimpleCollation() const {
    auto minKey = getShardKeyPattern().getKeyPattern().globalMin();
    return findIntersectingChunkWithSimpleCollation(minKey).getShardId();
}

void RoutingTableHistory::getAllShardIds(std::set<ShardId>* all) const {
    std::transform(_shardVersions.begin(),
                   _shardVersions.end(),
                   std::inserter(*all, all->begin()),
                   [](const ShardVersionMap::value_type& pair) { return pair.first; });
}

int RoutingTableHistory::getNShardsOwningChunks() const {
    return _shardVersions.size();
}

IndexBounds ChunkManager::getIndexBoundsForQuery(const BSONObj& key,
                                                 const CanonicalQuery& canonicalQuery) {
    // $text is not allowed in planning since we don't have text index on mongos.
    // TODO: Treat $text query as a no-op in planning on mongos. So with shard key {a: 1},
    //       the query { a: 2, $text: { ... } } will only target to {a: 2}.
    if (QueryPlannerCommon::hasNode(canonicalQuery.root(), MatchExpression::TEXT)) {
        IndexBounds bounds;
        IndexBoundsBuilder::allValuesBounds(key, &bounds);  // [minKey, maxKey]
        return bounds;
    }

    // Similarly, ignore GEO_NEAR queries in planning, since we do not have geo indexes on mongos.
    if (QueryPlannerCommon::hasNode(canonicalQuery.root(), MatchExpression::GEO_NEAR)) {
        IndexBounds bounds;
        IndexBoundsBuilder::allValuesBounds(key, &bounds);
        return bounds;
    }

    // Consider shard key as an index
    std::string accessMethod = IndexNames::findPluginName(key);
    dassert(accessMethod == IndexNames::BTREE || accessMethod == IndexNames::HASHED);
    const auto indexType = IndexNames::nameToType(accessMethod);

    // Use query framework to generate index bounds
    QueryPlannerParams plannerParams;
    // Must use "shard key" index
    plannerParams.options = QueryPlannerParams::NO_TABLE_SCAN;
    IndexEntry indexEntry(key,
                          indexType,
                          // The shard key index cannot be multikey.
                          false,
                          // Empty multikey paths, since the shard key index cannot be multikey.
                          MultikeyPaths{},
                          // Empty multikey path set, since the shard key index cannot be multikey.
                          {},
                          false /* sparse */,
                          false /* unique */,
                          IndexEntry::Identifier{"shardkey"},
                          nullptr /* filterExpr */,
                          BSONObj(),
                          nullptr, /* collator */
                          nullptr /* projExec */);
    plannerParams.indices.push_back(std::move(indexEntry));

    auto plannerResult = QueryPlanner::plan(canonicalQuery, plannerParams);
    if (plannerResult.getStatus().code() != ErrorCodes::NoQueryExecutionPlans) {
        auto solutions = uassertStatusOK(std::move(plannerResult));

        // Pick any solution that has non-trivial IndexBounds. bounds.size() == 0 represents a
        // trivial IndexBounds where none of the fields' values are bounded.
        for (auto&& soln : solutions) {
            IndexBounds bounds = collapseQuerySolution(soln->root.get());
            if (bounds.size() > 0) {
                return bounds;
            }
        }
    }

    // We cannot plan the query without collection scan, so target to all shards.
    IndexBounds bounds;
    IndexBoundsBuilder::allValuesBounds(key, &bounds);  // [minKey, maxKey]
    return bounds;
}

IndexBounds ChunkManager::collapseQuerySolution(const QuerySolutionNode* node) {
    if (node->children.empty()) {
        invariant(node->getType() == STAGE_IXSCAN);

        const IndexScanNode* ixNode = static_cast<const IndexScanNode*>(node);
        return ixNode->bounds;
    }

    if (node->children.size() == 1) {
        // e.g. FETCH -> IXSCAN
        return collapseQuerySolution(node->children.front());
    }

    // children.size() > 1, assert it's OR / SORT_MERGE.
    if (node->getType() != STAGE_OR && node->getType() != STAGE_SORT_MERGE) {
        // Unexpected node. We should never reach here.
        LOGV2_ERROR(23833,
                    "could not generate index bounds on query solution tree: {node}",
                    "node"_attr = redact(node->toString()));
        dassert(false);  // We'd like to know this error in testing.

        // Bail out with all shards in production, since this isn't a fatal error.
        return IndexBounds();
    }

    IndexBounds bounds;

    for (std::vector<QuerySolutionNode*>::const_iterator it = node->children.begin();
         it != node->children.end();
         it++) {
        // The first branch under OR
        if (it == node->children.begin()) {
            invariant(bounds.size() == 0);
            bounds = collapseQuerySolution(*it);
            if (bounds.size() == 0) {  // Got unexpected node in query solution tree
                return IndexBounds();
            }
            continue;
        }

        IndexBounds childBounds = collapseQuerySolution(*it);
        if (childBounds.size() == 0) {
            // Got unexpected node in query solution tree
            return IndexBounds();
        }

        invariant(childBounds.size() == bounds.size());

        for (size_t i = 0; i < bounds.size(); i++) {
            bounds.fields[i].intervals.insert(bounds.fields[i].intervals.end(),
                                              childBounds.fields[i].intervals.begin(),
                                              childBounds.fields[i].intervals.end());
        }
    }

    for (size_t i = 0; i < bounds.size(); i++) {
        IndexBoundsBuilder::unionize(&bounds.fields[i]);
    }

    return bounds;
}

bool RoutingTableHistory::compatibleWith(const RoutingTableHistory& other,
                                         const ShardId& shardName) const {
    // Return true if the shard version is the same in the two chunk managers
    // TODO: This doesn't need to be so strong, just major vs
    return other.getVersion(shardName) == getVersion(shardName);
}

ChunkVersion RoutingTableHistory::_getVersion(const ShardId& shardName,
                                              bool throwOnStaleShard) const {
    auto it = _shardVersions.find(shardName);
    if (it == _shardVersions.end()) {
        // Shards without explicitly tracked shard versions (meaning they have no chunks) always
        // have a version of (0, 0, epoch)
        return ChunkVersion(0, 0, _collectionVersion.epoch());
    }

    if (throwOnStaleShard && gEnableFinerGrainedCatalogCacheRefresh) {
        uassert(ShardInvalidatedForTargetingInfo(_nss),
                "shard has been marked stale",
                !it->second.isStale.load());
    }

    return it->second.shardVersion;
}

ChunkVersion RoutingTableHistory::getVersion(const ShardId& shardName) const {
    return _getVersion(shardName, true);
}

ChunkVersion RoutingTableHistory::getVersionForLogging(const ShardId& shardName) const {
    return _getVersion(shardName, false);
}

std::string RoutingTableHistory::toString() const {
    StringBuilder sb;
    sb << "RoutingTableHistory: " << _nss.ns() << " key: " << _shardKeyPattern.toString() << '\n';

    sb << "Chunks:\n";
    _chunkMap.forEach([&sb](const auto& chunk) {
        sb << "\t" << chunk->toString() << '\n';
        return true;
    });

    sb << "Shard versions:\n";
    for (const auto& entry : _shardVersions) {
        sb << "\t" << entry.first << ": " << entry.second.shardVersion.toString() << '\n';
    }

    return sb.str();
}

std::shared_ptr<RoutingTableHistory> RoutingTableHistory::makeNew(
    NamespaceString nss,
    boost::optional<UUID> uuid,
    KeyPattern shardKeyPattern,
    std::unique_ptr<CollatorInterface> defaultCollator,
    bool unique,
    OID epoch,
    const std::vector<ChunkType>& chunks) {
    auto ordering = Ordering::make(shardKeyPattern.toBSON());
    return RoutingTableHistory(std::move(nss),
                               std::move(uuid),
                               std::move(shardKeyPattern),
                               std::move(defaultCollator),
                               std::move(unique),
                               {std::move(ordering)},
                               {0, 0, epoch})
        .makeUpdated(chunks);
}

std::shared_ptr<RoutingTableHistory> RoutingTableHistory::makeUpdated(
    const std::vector<ChunkType>& changedChunks) {

    const auto startingCollectionVersion = getVersion();
    auto chunkMap = _chunkMap;

    ChunkVersion collectionVersion = startingCollectionVersion;
    for (const auto& chunk : changedChunks) {
        const auto& chunkVersion = chunk.getVersion();

        uassert(ErrorCodes::ConflictingOperationInProgress,
                str::stream() << "Chunk with namespace " << chunk.getNS().ns() << " and min key "
                              << chunk.getMin()
                              << " has epoch different from that of the collection "
                              << chunkVersion.epoch(),
                collectionVersion.epoch() == chunkVersion.epoch());

        // Chunks must always come in increasing sorted order
        invariant(chunkVersion >= collectionVersion);
        collectionVersion = chunkVersion;

        chunkMap.addChunk(chunk);
    }

    // If at least one diff was applied, the metadata is correct, but it might not have changed so
    // in this case there is no need to recreate the chunk manager.
    //
    // NOTE: In addition to the above statement, it is also important that we return the same chunk
    // manager object, because the write commands' code relies on changes of the chunk manager's
    // sequence number to detect batch writes not making progress because of chunks moving across
    // shards too frequently.
    if (collectionVersion == startingCollectionVersion) {
        return shared_from_this();
    }

    return std::shared_ptr<RoutingTableHistory>(
        new RoutingTableHistory(_nss,
                                _uuid,
                                KeyPattern(getShardKeyPattern().getKeyPattern()),
                                CollatorInterface::cloneCollator(getDefaultCollator()),
                                isUnique(),
                                std::move(chunkMap),
                                collectionVersion));
}

}  // namespace mongo
