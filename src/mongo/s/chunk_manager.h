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

#include <map>
#include <set>
#include <string>
#include <vector>

#include "mongo/db/namespace_string.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/s/chunk.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/concurrency/ticketholder.h"

namespace mongo {

class CanonicalQuery;
struct QuerySolutionNode;
class OperationContext;
class ChunkManager;

struct ShardVersionTargetingInfo {
    // Indicates whether the shard is stale and thus needs a catalog cache refresh. Is false by
    // default.
    AtomicWord<bool> isStale;

    // Max chunk version for the shard.
    ChunkVersion shardVersion;

    ShardVersionTargetingInfo(const OID& epoch);
};

// Map from a shard to a struct indicating both the max chunk version on that shard and whether the
// shard is currently marked as needing a catalog cache refresh (stale).
using ShardVersionMap = std::map<ShardId, ShardVersionTargetingInfo>;

// This class serves as a Facade around how the mapping of ranges to chunks is represented. It also
// provides a simpler, high-level interface for domain specific operations without exposing the
// underlying implementation.
class ChunkMap {
    // Vector of chunks ordered by max key.
    using ChunkVector = std::vector<std::shared_ptr<ChunkInfo>>;

public:
    explicit ChunkMap(OID epoch, size_t initialCapacity = 0) : _collectionVersion(0, 0, epoch) {
        _chunkMap.reserve(initialCapacity);
    }

    size_t size() const {
        return _chunkMap.size();
    }

    ChunkVersion getVersion() const {
        return _collectionVersion;
    }

    template <typename Callable>
    void forEach(Callable&& handler, const BSONObj& shardKey = BSONObj()) const {
        auto it = shardKey.isEmpty() ? _chunkMap.begin() : _findIntersectingChunk(shardKey);

        for (; it != _chunkMap.end(); ++it) {
            if (!handler(*it))
                break;
        }
    }

    template <typename Callable>
    void forEachOverlappingChunk(const BSONObj& min,
                                 const BSONObj& max,
                                 bool isMaxInclusive,
                                 Callable&& handler) const {
        const auto bounds = _overlappingBounds(min, max, isMaxInclusive);

        for (auto it = bounds.first; it != bounds.second; ++it) {
            if (!handler(*it))
                break;
        }
    }

    ShardVersionMap constructShardVersionMap() const;
    std::shared_ptr<ChunkInfo> findIntersectingChunk(const BSONObj& shardKey) const;

    void appendChunk(const std::shared_ptr<ChunkInfo>& chunk);
    ChunkMap createMerged(const std::vector<std::shared_ptr<ChunkInfo>>& changedChunks);

    BSONObj toBSON() const;

private:
    ChunkVector::const_iterator _findIntersectingChunk(const BSONObj& shardKey,
                                                       bool isMaxInclusive = true) const;
    std::pair<ChunkVector::const_iterator, ChunkVector::const_iterator> _overlappingBounds(
        const BSONObj& min, const BSONObj& max, bool isMaxInclusive) const;

    ChunkVector _chunkMap;

    // Max version across all chunks
    ChunkVersion _collectionVersion;
};

/**
 * In-memory representation of the routing table for a single sharded collection at various points
 * in time.
 */
class RoutingTableHistory : public std::enable_shared_from_this<RoutingTableHistory> {
    RoutingTableHistory(const RoutingTableHistory&) = delete;
    RoutingTableHistory& operator=(const RoutingTableHistory&) = delete;

public:
    /**
     * Makes an instance with a routing table for collection "nss", sharded on
     * "shardKeyPattern".
     *
     * "defaultCollator" is the default collation for the collection, "unique" indicates whether
     * or not the shard key for each document will be globally unique, and "epoch" is the globally
     * unique identifier for this version of the collection.
     *
     * The "chunks" vector must contain the chunk routing information sorted in ascending order by
     * chunk version, and adhere to the requirements of the routing table update algorithm.
     */
    static std::shared_ptr<RoutingTableHistory> makeNew(
        NamespaceString nss,
        boost::optional<UUID>,
        KeyPattern shardKeyPattern,
        std::unique_ptr<CollatorInterface> defaultCollator,
        bool unique,
        OID epoch,
        const std::vector<ChunkType>& chunks);

    /**
     * Constructs a new instance with a routing table updated according to the changes described
     * in "changedChunks".
     *
     * The changes in "changedChunks" must be sorted in ascending order by chunk version, and adhere
     * to the requirements of the routing table update algorithm.
     */
    std::shared_ptr<RoutingTableHistory> makeUpdated(const std::vector<ChunkType>& changedChunks);

    /**
     * Returns an increasing number of the reload sequence number of this chunk manager.
     */
    unsigned long long getSequenceNumber() const {
        return _sequenceNumber;
    }

    const NamespaceString& getns() const {
        return _nss;
    }

    const ShardKeyPattern& getShardKeyPattern() const {
        return _shardKeyPattern;
    }

    const CollatorInterface* getDefaultCollator() const {
        return _defaultCollator.get();
    }

    bool isUnique() const {
        return _unique;
    }

    /**
     * Mark the given shard as stale, indicating that requests targetted to this shard (for this
     * namespace) need to block on a catalog cache refresh.
     */
    void setShardStale(const ShardId& shardId);

    /**
     * Mark all shards as not stale, indicating that a refresh has happened and requests targeted
     * to all shards (for this namespace) do not currently need to block on a catalog cache refresh.
     */
    void setAllShardsRefreshed();

    ChunkVersion getVersion() const {
        return _chunkMap.getVersion();
    }

    /**
     * Retrieves the shard version for the given shard. Will throw a ShardInvalidatedForTargeting
     * exception if the shard is marked as stale.
     */
    ChunkVersion getVersion(const ShardId& shardId) const;

    /**
     * Retrieves the shard version for the given shard. Will not throw if the shard is marked as
     * stale. Only use when logging the given chunk version -- if the caller must execute logic
     * based on the returned version, use getVersion() instead.
     */
    ChunkVersion getVersionForLogging(const ShardId& shardId) const;

    size_t numChunks() const {
        return _chunkMap.size();
    }

    template <typename Callable>
    void forEachChunk(Callable&& handler, const BSONObj& shardKey = BSONObj()) const {
        _chunkMap.forEach(std::forward<Callable>(handler), shardKey);
    }

    template <typename Callable>
    void forEachOverlappingChunk(const BSONObj& min,
                                 const BSONObj& max,
                                 bool isMaxInclusive,
                                 Callable&& handler) const {
        _chunkMap.forEachOverlappingChunk(
            min, max, isMaxInclusive, std::forward<Callable>(handler));
    }

    std::shared_ptr<ChunkInfo> findIntersectingChunk(const BSONObj& shardKey) const {
        return _chunkMap.findIntersectingChunk(shardKey);
    }

    /**
     * Returns the ids of all shards on which the collection has any chunks.
     */
    void getAllShardIds(std::set<ShardId>* all) const;

    /**
     * Returns the number of shards on which the collection has any chunks
     */
    int getNShardsOwningChunks() const;

    /**
     * Returns true if, for this shard, the chunks are identical in both chunk managers
     */
    bool compatibleWith(const RoutingTableHistory& other, const ShardId& shard) const;

    std::string toString() const;

    bool uuidMatches(UUID uuid) const {
        return _uuid && *_uuid == uuid;
    }

    boost::optional<UUID> getUUID() const {
        return _uuid;
    }

private:
    RoutingTableHistory(NamespaceString nss,
                        boost::optional<UUID> uuid,
                        KeyPattern shardKeyPattern,
                        std::unique_ptr<CollatorInterface> defaultCollator,
                        bool unique,
                        ChunkMap chunkMap);

    ChunkVersion _getVersion(const ShardId& shardName, bool throwOnStaleShard) const;

    // The shard versioning mechanism hinges on keeping track of the number of times we reload
    // ChunkManagers.
    const unsigned long long _sequenceNumber;

    // Namespace to which this routing information corresponds
    const NamespaceString _nss;

    // The invariant UUID of the collection.  This is optional in 3.6, except in change streams.
    const boost::optional<UUID> _uuid;

    // The key pattern used to shard the collection
    const ShardKeyPattern _shardKeyPattern;

    // Default collation to use for routing data queries for this collection
    const std::unique_ptr<CollatorInterface> _defaultCollator;

    // Whether the sharding key is unique
    const bool _unique;

    // Map from the max for each chunk to an entry describing the chunk. The union of all chunks'
    // ranges must cover the complete space from [MinKey, MaxKey).
    ChunkMap _chunkMap;

    // The representation of shard versions and staleness indicators for this namespace. If a
    // shard does not exist, it will not have an entry in the map.
    // Note: this declaration must not be moved before _chunkMap since it is initialized by using
    // the _chunkMap instance.
    ShardVersionMap _shardVersions;

    friend class ChunkManager;
};

// This will be renamed to RoutingTableHistory and the original RoutingTableHistory will be
// ChunkHistoryMap
class ChunkManager : public std::enable_shared_from_this<ChunkManager> {
    ChunkManager(const ChunkManager&) = delete;
    ChunkManager& operator=(const ChunkManager&) = delete;

public:
    ChunkManager(std::shared_ptr<RoutingTableHistory> rt, boost::optional<Timestamp> clusterTime)
        : _rt(std::move(rt)), _clusterTime(std::move(clusterTime)) {}

    /**
     * Returns an increasing number of the reload sequence number of this chunk manager.
     */
    unsigned long long getSequenceNumber() const {
        return _rt->getSequenceNumber();
    }

    const NamespaceString& getns() const {
        return _rt->getns();
    }

    const ShardKeyPattern& getShardKeyPattern() const {
        return _rt->getShardKeyPattern();
    }

    const CollatorInterface* getDefaultCollator() const {
        return _rt->getDefaultCollator();
    }

    bool isUnique() const {
        return _rt->isUnique();
    }

    ChunkVersion getVersion() const {
        return _rt->getVersion();
    }

    ChunkVersion getVersion(const ShardId& shardId) const {
        return _rt->getVersion(shardId);
    }

    ChunkVersion getVersionForLogging(const ShardId& shardId) const {
        return _rt->getVersionForLogging(shardId);
    }

    template <typename Callable>
    void forEachChunk(Callable&& handler) const {
        _rt->forEachChunk(
            [this, handler = std::forward<Callable>(handler)](const auto& chunkInfo) mutable {
                if (!handler(Chunk{*chunkInfo, _clusterTime}))
                    return false;

                return true;
            });
    }

    int numChunks() const {
        return _rt->numChunks();
    }

    /**
     * Returns true if a document with the given "shardKey" is owned by the shard with the given
     * "shardId" in this routing table. If "shardKey" is empty returns false. If "shardKey" is not a
     * valid shard key, the behaviour is undefined.
     */
    bool keyBelongsToShard(const BSONObj& shardKey, const ShardId& shardId) const;

    /**
     * Returns true if any chunk owned by the shard with the given "shardId" overlaps "range".
     */
    bool rangeOverlapsShard(const ChunkRange& range, const ShardId& shardId) const;

    /**
     * Given a shardKey, returns the first chunk which is owned by shardId and overlaps or sorts
     * after that shardKey. If the return value is empty, this means no such chunk exists.
     */
    boost::optional<Chunk> getNextChunkOnShard(const BSONObj& shardKey,
                                               const ShardId& shardId) const;

    /**
     * Given a shard key (or a prefix) that has been extracted from a document, returns the chunk
     * that contains that key.
     *
     * Example: findIntersectingChunk({a : hash('foo')}) locates the chunk for document
     *          {a: 'foo', b: 'bar'} if the shard key is {a : 'hashed'}.
     *
     * If 'collation' is empty, we use the collection default collation for targeting.
     *
     * Throws a DBException with the ShardKeyNotFound code if unable to target a single shard due to
     * collation or due to the key not matching the shard key pattern.
     */
    Chunk findIntersectingChunk(const BSONObj& shardKey, const BSONObj& collation) const;

    /**
     * Same as findIntersectingChunk, but assumes the simple collation.
     */
    Chunk findIntersectingChunkWithSimpleCollation(const BSONObj& shardKey) const {
        return findIntersectingChunk(shardKey, CollationSpec::kSimpleSpec);
    }

    /**
     * Finds the shard id of the shard that owns the chunk minKey belongs to, assuming the simple
     * collation because shard keys do not support non-simple collations.
     */
    ShardId getMinKeyShardIdWithSimpleCollation() const;

    /**
     * Finds the shard IDs for a given filter and collation. If collation is empty, we use the
     * collection default collation for targeting.
     */
    void getShardIdsForQuery(boost::intrusive_ptr<ExpressionContext> expCtx,
                             const BSONObj& query,
                             const BSONObj& collation,
                             std::set<ShardId>* shardIds) const;

    /**
     * Returns all shard ids which contain chunks overlapping the range [min, max]. Please note the
     * inclusive bounds on both sides (SERVER-20768).
     */
    void getShardIdsForRange(const BSONObj& min,
                             const BSONObj& max,
                             std::set<ShardId>* shardIds) const;

    /**
     * Returns the ids of all shards on which the collection has any chunks.
     */
    void getAllShardIds(std::set<ShardId>* all) const {
        _rt->getAllShardIds(all);
    }

    /**
     * Returns the number of shards on which the collection has any chunks
     */
    int getNShardsOwningChunks() {
        return _rt->getNShardsOwningChunks();
    }

    // Transforms query into bounds for each field in the shard key
    // for example :
    //   Key { a: 1, b: 1 },
    //   Query { a : { $gte : 1, $lt : 2 },
    //            b : { $gte : 3, $lt : 4 } }
    //   => Bounds { a : [1, 2), b : [3, 4) }
    static IndexBounds getIndexBoundsForQuery(const BSONObj& key,
                                              const CanonicalQuery& canonicalQuery);

    // Collapse query solution tree.
    //
    // If it has OR node, the result could be a superset of the index bounds generated.
    // Since to give a single IndexBounds, this gives the union of bounds on each field.
    // for example:
    //   OR: { a: (0, 1), b: (0, 1) },
    //       { a: (2, 3), b: (2, 3) }
    //   =>  { a: (0, 1), (2, 3), b: (0, 1), (2, 3) }
    static IndexBounds collapseQuerySolution(const QuerySolutionNode* node);

    /**
     * Returns true if, for this shard, the chunks are identical in both chunk managers
     */
    bool compatibleWith(const ChunkManager& other, const ShardId& shard) const {
        return _rt->compatibleWith(*other._rt, shard);
    }

    std::string toString() const {
        return _rt->toString();
    }

    bool uuidMatches(UUID uuid) const {
        return _rt->uuidMatches(uuid);
    }

    auto getRoutingHistory() const {
        return _rt;
    }

    boost::optional<UUID> getUUID() const {
        return _rt->getUUID();
    }

private:
    std::shared_ptr<RoutingTableHistory> _rt;
    boost::optional<Timestamp> _clusterTime;
};

}  // namespace mongo
