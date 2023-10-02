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


#include "mongo/platform/basic.h"

#include "mongo/db/s/balancer/balancer_policy.h"

#include <random>

#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/logv2/log.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/grid.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

MONGO_FAIL_POINT_DEFINE(balancerShouldReturnRandomMigrations);

using std::map;
using std::numeric_limits;
using std::set;
using std::string;
using std::vector;
using namespace fmt::literals;

namespace {

ChunkType makeChunkType(const UUID& collUUID, const Chunk& chunk) {
    ChunkType ct{collUUID, chunk.getRange(), chunk.getLastmod(), chunk.getShardId()};
    ct.setJumbo(chunk.isJumbo());
    return ct;
}

/**
 * Return a vector of zones after they have been normalized according to the given chunk
 * configuration.
 *
 * If a zone covers only partially a chunk, boundaries of that zone will be shrank so that the
 * normalized zone won't overlap with that chunk. The boundaries of a normalized zone will never
 * fall in the middle of a chunk.
 *
 * Additionally the vector will contain also zones for the "NoZone",
 */
std::vector<ZoneRange> normalizeZones(const ChunkManager& cm, const ZoneInfo& zoneInfo) {
    std::vector<ZoneRange> normalizedRanges;

    auto lastMax = cm.getShardKeyPattern().getKeyPattern().globalMin();

    for (const auto& [max, zoneRange] : zoneInfo.zoneRanges()) {
        const auto& minChunk = cm.findIntersectingChunkWithSimpleCollation(zoneRange.min);
        const auto gtMin =
            SimpleBSONObjComparator::kInstance.evaluate(zoneRange.min > minChunk.getMin());
        const auto& normalizedMin = gtMin ? minChunk.getMax() : zoneRange.min;


        const auto& maxChunk = cm.findIntersectingChunkWithSimpleCollation(zoneRange.max);
        const auto gtMax =
            SimpleBSONObjComparator::kInstance.evaluate(zoneRange.max > maxChunk.getMin()) &&
            SimpleBSONObjComparator::kInstance.evaluate(
                zoneRange.max != cm.getShardKeyPattern().getKeyPattern().globalMax());
        const auto& normalizedMax = gtMax ? maxChunk.getMin() : zoneRange.max;


        if (SimpleBSONObjComparator::kInstance.evaluate(normalizedMin == normalizedMax)) {
            // This zone does not fully contain any chunk thus we can ignore it
            continue;
        }

        if (SimpleBSONObjComparator::kInstance.evaluate(normalizedMin != lastMax)) {
            // The zone is not contiguous with the previous one so we add a kNoZoneRange
            // does not fully contain any chunk so we will ignore it
            normalizedRanges.emplace_back(lastMax, normalizedMin, ZoneInfo::kNoZoneName);
        }

        normalizedRanges.emplace_back(normalizedMin, normalizedMax, zoneRange.zone);
        lastMax = normalizedMax;
    }

    const auto& globalMaxKey = cm.getShardKeyPattern().getKeyPattern().globalMax();
    if (SimpleBSONObjComparator::kInstance.evaluate(lastMax != globalMaxKey)) {
        normalizedRanges.emplace_back(lastMax, globalMaxKey, ZoneInfo::kNoZoneName);
    }
    return normalizedRanges;
}

}  // namespace

DistributionStatus::DistributionStatus(NamespaceString nss,
                                       ZoneInfo zoneInfo,
                                       const ChunkManager& chunkMngr)
    : _nss(std::move(nss)), _zoneInfo(std::move(zoneInfo)), _chunkMngr(chunkMngr) {

    _normalizedZones = normalizeZones(_chunkMngr, _zoneInfo);

    for (const auto& zoneRange : _normalizedZones) {
        chunkMngr.forEachOverlappingChunk(
            zoneRange.min, zoneRange.max, false /* isMaxInclusive */, [&](const auto& chunkInfo) {
                _shardToZoneSizeMap[chunkInfo.getShardId()][zoneRange.zone]++;
                return true;
            });
    }
}

size_t DistributionStatus::numberOfChunksInShard(const ShardId& shardId) const {
    const auto shardZonesIt = _shardToZoneSizeMap.find(shardId);
    if (shardZonesIt == _shardToZoneSizeMap.end()) {
        return 0;
    }
    size_t total = 0;
    for (const auto& [_, numChunks] : shardZonesIt->second) {
        total += numChunks;
    }
    return total;
}

const StringMap<size_t>& DistributionStatus::getChunksPerZoneMap(const ShardId& shardId) const {
    static const StringMap<size_t> emptyMap;
    const auto shardZonesIt = _shardToZoneSizeMap.find(shardId);
    if (shardZonesIt == _shardToZoneSizeMap.end()) {
        return emptyMap;
    }
    return shardZonesIt->second;
}

const string ZoneInfo::kNoZoneName = "";

ZoneInfo::ZoneInfo()
    : _zoneRanges(SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<ZoneRange>()) {}

Status ZoneInfo::addRangeToZone(const ZoneRange& range) {
    const auto minIntersect = _zoneRanges.upper_bound(range.min);
    const auto maxIntersect = _zoneRanges.upper_bound(range.max);

    // Check for partial overlap
    if (minIntersect != maxIntersect) {
        invariant(minIntersect != _zoneRanges.end());
        const auto& intersectingRange =
            (SimpleBSONObjComparator::kInstance.evaluate(minIntersect->second.min < range.max))
            ? minIntersect->second
            : maxIntersect->second;

        if (SimpleBSONObjComparator::kInstance.evaluate(intersectingRange.min == range.min) &&
            SimpleBSONObjComparator::kInstance.evaluate(intersectingRange.max == range.max) &&
            intersectingRange.zone == range.zone) {
            return Status::OK();
        }

        return {ErrorCodes::RangeOverlapConflict,
                str::stream() << "Zone range: " << range.toString()
                              << " is overlapping with existing: " << intersectingRange.toString()};
    }

    // Check for containment
    if (minIntersect != _zoneRanges.end()) {
        const ZoneRange& nextRange = minIntersect->second;
        if (SimpleBSONObjComparator::kInstance.evaluate(range.max > nextRange.min)) {
            invariant(SimpleBSONObjComparator::kInstance.evaluate(range.max < nextRange.max));
            return {ErrorCodes::RangeOverlapConflict,
                    str::stream() << "Zone range: " << range.toString()
                                  << " is overlapping with existing: " << nextRange.toString()};
        }
    }

    // This must be a new entry
    _zoneRanges.emplace(range.max.getOwned(), range);
    _allZones.insert(range.zone);
    return Status::OK();
}

string ZoneInfo::getZoneForRange(const ChunkRange& chunk) const {
    const auto minIntersect = _zoneRanges.upper_bound(chunk.getMin());
    const auto maxIntersect = _zoneRanges.lower_bound(chunk.getMax());

    // We should never have a partial overlap with a chunk range. If it happens, treat it as if this
    // chunk doesn't belong to a zone
    if (minIntersect != maxIntersect) {
        return ZoneInfo::kNoZoneName;
    }

    if (minIntersect == _zoneRanges.end()) {
        return ZoneInfo::kNoZoneName;
    }

    const ZoneRange& intersectRange = minIntersect->second;

    // Check for containment
    if (SimpleBSONObjComparator::kInstance.evaluate(intersectRange.min <= chunk.getMin()) &&
        SimpleBSONObjComparator::kInstance.evaluate(chunk.getMax() <= intersectRange.max)) {
        return intersectRange.zone;
    }

    return ZoneInfo::kNoZoneName;
}

/**
 * read all tags for collection via the catalog client and add to the zoneInfo
 */
StatusWith<ZoneInfo> createCollectionZoneInfo(OperationContext* opCtx,
                                              const NamespaceString& nss,
                                              const KeyPattern& keyPattern) {
    const auto swCollectionZones =
        ShardingCatalogManager::get(opCtx)->localCatalogClient()->getTagsForCollection(opCtx, nss);
    if (!swCollectionZones.isOK()) {
        return swCollectionZones.getStatus().withContext(
            str::stream() << "Unable to load zones for collection " << nss);
    }
    const auto& collectionZones = swCollectionZones.getValue();

    ZoneInfo zoneInfo;

    for (const auto& zone : collectionZones) {
        auto status =
            zoneInfo.addRangeToZone(ZoneRange(keyPattern.extendRangeBound(zone.getMinKey(), false),
                                              keyPattern.extendRangeBound(zone.getMaxKey(), false),
                                              zone.getTag()));

        if (!status.isOK()) {
            return status;
        }
    }
    return {std::move(zoneInfo)};
}

Status BalancerPolicy::isShardSuitableReceiver(const ClusterStatistics::ShardStatistics& stat,
                                               const string& chunkZone) {
    if (stat.isDraining) {
        return {ErrorCodes::IllegalOperation,
                str::stream() << stat.shardId << " is currently draining."};
    }

    if (chunkZone != ZoneInfo::kNoZoneName && !stat.shardZones.count(chunkZone)) {
        return {ErrorCodes::IllegalOperation,
                str::stream() << stat.shardId << " is not in the correct zone " << chunkZone};
    }

    return Status::OK();
}

std::tuple<ShardId, int64_t> BalancerPolicy::_getLeastLoadedReceiverShard(
    const ShardStatisticsVector& shardStats,
    const CollectionDataSizeInfoForBalancing& collDataSizeInfo,
    const string& zone,
    const stdx::unordered_set<ShardId>& availableShards) {
    ShardId best;
    int64_t currentMin = numeric_limits<int64_t>::max();

    for (const auto& stat : shardStats) {
        if (!availableShards.count(stat.shardId))
            continue;

        auto status = isShardSuitableReceiver(stat, zone);
        if (!status.isOK()) {
            continue;
        }

        const auto& shardSizeIt = collDataSizeInfo.shardToDataSizeMap.find(stat.shardId);
        if (shardSizeIt == collDataSizeInfo.shardToDataSizeMap.end()) {
            // Skip if stats not available (may happen if add|remove shard during a round)
            continue;
        }

        const auto shardSize = shardSizeIt->second;
        if (shardSize < currentMin) {
            best = stat.shardId;
            currentMin = shardSize;
        }
    }

    return {best, currentMin};
}

std::tuple<ShardId, int64_t> BalancerPolicy::_getMostOverloadedShard(
    const ShardStatisticsVector& shardStats,
    const CollectionDataSizeInfoForBalancing& collDataSizeInfo,
    const string& chunkZone,
    const stdx::unordered_set<ShardId>& availableShards) {
    ShardId worst;
    long long currentMax = numeric_limits<long long>::min();

    for (const auto& stat : shardStats) {
        if (!availableShards.count(stat.shardId))
            continue;

        const auto& shardSizeIt = collDataSizeInfo.shardToDataSizeMap.find(stat.shardId);
        if (shardSizeIt == collDataSizeInfo.shardToDataSizeMap.end()) {
            // Skip if stats not available (may happen if add|remove shard during a round)
            continue;
        }

        const auto shardSize = shardSizeIt->second;
        if (shardSize > currentMax) {
            worst = stat.shardId;
            currentMax = shardSize;
        }
    }

    return {worst, currentMax};
}

// Returns a random integer in [0, max) using a uniform random distribution.
int getRandomIndex(int max) {
    std::default_random_engine gen(time(nullptr));
    std::uniform_int_distribution<int> dist(0, max - 1);

    return dist(gen);
}

// Returns a randomly chosen pair of source -> destination shards for testing.
boost::optional<MigrateInfo> chooseRandomMigration(stdx::unordered_set<ShardId>* availableShards,
                                                   const DistributionStatus& distribution) {

    if (availableShards->size() < 2) {
        return boost::none;
    }

    std::vector<ShardId> shards;
    std::copy(availableShards->begin(), availableShards->end(), std::back_inserter(shards));
    std::default_random_engine rng(time(nullptr));
    std::shuffle(shards.begin(), shards.end(), rng);

    // Get a random shard with chunks as the donor shard and another random shard as the recipient
    boost::optional<ShardId> donorShard;
    boost::optional<ShardId> recipientShard;
    for (auto i = 0U; i < shards.size(); ++i) {
        if (distribution.numberOfChunksInShard(shards[i]) != 0) {
            donorShard = shards[i];

            if (i == shards.size() - 1) {
                recipientShard = shards[0];
            } else {
                recipientShard = shards[i + 1];
            }
            break;
        }
    }

    if (!donorShard) {
        return boost::none;
    }
    invariant(recipientShard);

    LOGV2_DEBUG(21880,
                1,
                "balancerShouldReturnRandomMigrations: source: {fromShardId} dest: {toShardId}",
                "balancerShouldReturnRandomMigrations",
                "fromShardId"_attr = donorShard.get(),
                "toShardId"_attr = recipientShard.get());

    const auto& randomChunk = [&] {
        const auto numChunksOnDonorShard = distribution.numberOfChunksInShard(donorShard.get());
        const auto rndChunkIdx = getRandomIndex(numChunksOnDonorShard);
        ChunkType rndChunk;

        int idx{0};
        distribution.getChunkManager().forEachChunk([&](const auto& chunk) {
            if (chunk.getShardId() == donorShard.get() && idx++ == rndChunkIdx) {
                rndChunk = makeChunkType(distribution.getChunkManager().getUUID(), chunk);
                return false;
            }
            return true;
        });

        invariant(rndChunk.getShard().isValid());
        return rndChunk;
    }();


    return MigrateInfo{
        recipientShard.get(), distribution.nss(), randomChunk, ForceJumbo::kDoNotForce};
}

MigrateInfosWithReason BalancerPolicy::balance(
    const ShardStatisticsVector& shardStats,
    const DistributionStatus& distribution,
    const CollectionDataSizeInfoForBalancing& collDataSizeInfo,
    stdx::unordered_set<ShardId>* availableShards,
    bool forceJumbo) {
    vector<MigrateInfo> migrations;
    MigrationReason firstReason = MigrationReason::none;

    if (MONGO_unlikely(balancerShouldReturnRandomMigrations.shouldFail()) &&
        !distribution.nss().isConfigDB()) {
        LOGV2_DEBUG(21881, 1, "balancerShouldReturnRandomMigrations failpoint is set");

        auto migration = chooseRandomMigration(availableShards, distribution);

        if (migration) {
            migrations.push_back(migration.get());
            firstReason = MigrationReason::chunksImbalance;

            invariant(availableShards->erase(migration.get().from));
            invariant(availableShards->erase(migration.get().to));
        }

        return std::make_pair(std::move(migrations), firstReason);
    }

    // 1) Check for shards, which are in draining mode
    {
        for (const auto& stat : shardStats) {
            if (!stat.isDraining)
                continue;

            if (!availableShards->count(stat.shardId))
                continue;

            // Now we know we need to move to chunks off this shard, but only if permitted by the
            // zones policy
            unsigned numJumboChunks = 0;

            const auto& chunksPerZoneMap = distribution.getChunksPerZoneMap(stat.shardId);
            for (const auto& zoneIt : chunksPerZoneMap) {
                const auto& zoneName = zoneIt.first;
                for (const auto& zoneRange : distribution.getNormalizedZones()) {
                    if (zoneRange.zone != zoneName) {
                        continue;
                    }

                    distribution.getChunkManager().forEachOverlappingChunk(
                        zoneRange.min,
                        zoneRange.max,
                        false /* isMaxInclusive */,
                        [&](const auto& chunk) {
                            if (chunk.getShardId() != stat.shardId) {
                                return true;  // continue
                            }
                            if (chunk.isJumbo()) {
                                numJumboChunks++;
                                return true;  // continue
                            }

                            const auto [to, _] = _getLeastLoadedReceiverShard(
                                shardStats, collDataSizeInfo, zoneName, *availableShards);
                            if (!to.isValid()) {
                                if (migrations.empty()) {
                                    LOGV2_WARNING(
                                        21889,
                                        "Chunk {chunk} is on a draining shard, but no appropriate "
                                        "recipient found",
                                        "Chunk is on a draining shard, but no appropriate "
                                        "recipient found",
                                        "chunk"_attr = redact(
                                            makeChunkType(distribution.getChunkManager().getUUID(),
                                                          chunk)
                                                .toString()));
                                }
                                return true;  // continue
                            }
                            invariant(to != stat.shardId);

                            migrations.emplace_back(
                                to,
                                chunk.getShardId(),
                                distribution.nss(),
                                distribution.getChunkManager().getUUID(),
                                chunk.getMin(),
                                boost::none /* max */,
                                chunk.getLastmod(),
                                // Always force jumbo chunks to be migrated off draining shards
                                ForceJumbo::kForceBalancer,
                                collDataSizeInfo.maxChunkSizeBytes);

                            if (firstReason == MigrationReason::none) {
                                firstReason = MigrationReason::drain;
                            }
                            invariant(availableShards->erase(stat.shardId));
                            invariant(availableShards->erase(to));
                            return false;  // break
                        });

                    if (migrations.empty()) {
                        LOGV2_WARNING(21890,
                                      "Unable to find any chunk to move from draining shard "
                                      "{shardId}. numJumboChunks: {numJumboChunks}",
                                      "Unable to find any chunk to move from draining shard",
                                      "shardId"_attr = stat.shardId,
                                      "numJumboChunks"_attr = numJumboChunks);
                    }

                    if (availableShards->size() < 2) {
                        return std::make_pair(std::move(migrations), firstReason);
                    }
                }
            }
        }
    }

    // 2) Check for chunks, which are on the wrong shard and must be moved off of it
    if (!distribution.zones().empty()) {
        for (const auto& stat : shardStats) {

            if (!availableShards->count(stat.shardId))
                continue;

            const auto& chunksPerZoneMap = distribution.getChunksPerZoneMap(stat.shardId);
            for (const auto& zoneIt : chunksPerZoneMap) {
                const auto& zoneName = zoneIt.first;

                if (zoneName == ZoneInfo::kNoZoneName)
                    continue;

                if (stat.shardZones.count(zoneName))
                    continue;

                for (const auto& zoneRange : distribution.getNormalizedZones()) {
                    if (zoneRange.zone != zoneName) {
                        continue;
                    }
                    distribution.getChunkManager().forEachOverlappingChunk(
                        zoneRange.min,
                        zoneRange.max,
                        false /* isMaxInclusive */,
                        [&](const auto& chunk) {
                            if (chunk.getShardId() != stat.shardId) {
                                return true;  // continue
                            }
                            if (chunk.isJumbo()) {
                                LOGV2_WARNING(
                                    21891,
                                    "Chunk {chunk} violates zone {zone}, but it is jumbo and "
                                    "cannot be moved",
                                    "Chunk violates zone, but it is jumbo and cannot be moved",
                                    "chunk"_attr =
                                        redact(makeChunkType(
                                                   distribution.getChunkManager().getUUID(), chunk)
                                                   .toString()),
                                    "zone"_attr = redact(zoneName));
                                return true;  // continue
                            }

                            const auto [to, _] = _getLeastLoadedReceiverShard(
                                shardStats, collDataSizeInfo, zoneName, *availableShards);
                            if (!to.isValid()) {
                                if (migrations.empty()) {
                                    LOGV2_WARNING(
                                        21892,
                                        "Chunk {chunk} violates zone {zone}, but no appropriate "
                                        "recipient found",
                                        "Chunk violates zone, but no appropriate recipient found",
                                        "chunk"_attr = redact(
                                            makeChunkType(distribution.getChunkManager().getUUID(),
                                                          chunk)
                                                .toString()),
                                        "zone"_attr = redact(zoneName));
                                }
                                return true;  // continue
                            }
                            invariant(to != stat.shardId);

                            migrations.emplace_back(to,
                                                    chunk.getShardId(),
                                                    distribution.nss(),
                                                    distribution.getChunkManager().getUUID(),
                                                    chunk.getMin(),
                                                    boost::none /* max */,
                                                    chunk.getLastmod(),
                                                    forceJumbo ? ForceJumbo::kForceBalancer
                                                               : ForceJumbo::kDoNotForce,
                                                    collDataSizeInfo.maxChunkSizeBytes);

                            if (firstReason == MigrationReason::none) {
                                firstReason = MigrationReason::zoneViolation;
                            }
                            invariant(availableShards->erase(stat.shardId));
                            invariant(availableShards->erase(to));
                            return false;  // break
                        });
                }

                if (availableShards->size() < 2) {
                    return std::make_pair(std::move(migrations), firstReason);
                }
            }
        }
    }

    // 3) for each zone balance

    vector<string> zonesPlusEmpty(distribution.zones().begin(), distribution.zones().end());
    zonesPlusEmpty.push_back(ZoneInfo::kNoZoneName);

    for (const auto& zone : zonesPlusEmpty) {
        size_t numShardsInZone = 0;
        int64_t totalDataSizeOfShardsWithZone = 0;

        for (const auto& stat : shardStats) {
            if (zone == ZoneInfo::kNoZoneName || stat.shardZones.count(zone)) {
                const auto& shardSizeIt = collDataSizeInfo.shardToDataSizeMap.find(stat.shardId);
                if (shardSizeIt == collDataSizeInfo.shardToDataSizeMap.end()) {
                    // Skip if stats not available (may happen if add|remove shard during a round)
                    continue;
                }
                totalDataSizeOfShardsWithZone += shardSizeIt->second;
                numShardsInZone++;
            }
        }

        // Skip zones which have no shards assigned to them. This situation is not harmful, but
        // should not be possible so warn the operator to correct it.
        if (numShardsInZone == 0) {
            if (zone != ZoneInfo::kNoZoneName) {
                LOGV2_WARNING(21893,
                              "Zone {zone} in collection {namespace} has no assigned shards and "
                              "chunks which fall into it cannot be balanced. This should be "
                              "corrected by either assigning shards to the zone or by deleting it.",
                              "Zone in collection has no assigned shards and chunks which fall "
                              "into it cannot be balanced. This should be corrected by either "
                              "assigning shards to the zone or by deleting it.",
                              "zone"_attr = redact(zone),
                              logAttrs(distribution.nss()));
            }
            continue;
        }

        tassert(ErrorCodes::BadValue,
                str::stream() << "Total data size for shards in zone " << zone << " and collection "
                              << distribution.nss() << " must be greater or equal than zero but is "
                              << totalDataSizeOfShardsWithZone,
                totalDataSizeOfShardsWithZone >= 0);

        if (totalDataSizeOfShardsWithZone == 0) {
            // No data to balance within this zone
            continue;
        }

        const int64_t idealDataSizePerShardForZone =
            totalDataSizeOfShardsWithZone / numShardsInZone;

        while (_singleZoneBalanceBasedOnDataSize(shardStats,
                                                 distribution,
                                                 collDataSizeInfo,
                                                 zone,
                                                 idealDataSizePerShardForZone,
                                                 &migrations,
                                                 availableShards,
                                                 forceJumbo ? ForceJumbo::kForceBalancer
                                                            : ForceJumbo::kDoNotForce)) {
            if (firstReason == MigrationReason::none) {
                firstReason = MigrationReason::chunksImbalance;
            }
        }
    }

    return std::make_pair(std::move(migrations), firstReason);
}

boost::optional<MigrateInfo> BalancerPolicy::balanceSingleChunk(
    const ChunkType& chunk,
    const ShardStatisticsVector& shardStats,
    const DistributionStatus& distribution,
    const CollectionDataSizeInfoForBalancing& collDataSizeInfo) {
    const auto& zone = distribution.getZoneInfo().getZoneForRange(chunk.getRange());

    stdx::unordered_set<ShardId> availableShards;
    std::transform(shardStats.begin(),
                   shardStats.end(),
                   std::inserter(availableShards, availableShards.end()),
                   [](const ClusterStatistics::ShardStatistics& shardStatistics) -> ShardId {
                       return shardStatistics.shardId;
                   });

    const auto [newShardId, _] = _getLeastLoadedReceiverShard(
        shardStats, collDataSizeInfo, zone, stdx::unordered_set<ShardId>());
    if (!newShardId.isValid() || newShardId == chunk.getShard()) {
        return boost::optional<MigrateInfo>();
    }

    return MigrateInfo(newShardId, distribution.nss(), chunk, ForceJumbo::kDoNotForce);
}

bool BalancerPolicy::_singleZoneBalanceBasedOnDataSize(
    const ShardStatisticsVector& shardStats,
    const DistributionStatus& distribution,
    const CollectionDataSizeInfoForBalancing& collDataSizeInfo,
    const string& zone,
    const int64_t idealDataSizePerShardForZone,
    vector<MigrateInfo>* migrations,
    stdx::unordered_set<ShardId>* availableShards,
    ForceJumbo forceJumbo) {
    const auto [from, fromSize] =
        _getMostOverloadedShard(shardStats, collDataSizeInfo, zone, *availableShards);
    if (!from.isValid())
        return false;

    const auto [to, toSize] =
        _getLeastLoadedReceiverShard(shardStats, collDataSizeInfo, zone, *availableShards);
    if (!to.isValid()) {
        if (migrations->empty()) {
            LOGV2(6581600, "No available shards to take chunks for zone", "zone"_attr = zone);
        }
        return false;
    }

    if (from == to) {
        return false;
    }

    LOGV2_DEBUG(7548100,
                1,
                "Balancing single zone",
                logAttrs(distribution.nss()),
                "zone"_attr = zone,
                "idealDataSizePerShardForZone"_attr = idealDataSizePerShardForZone,
                "fromShardId"_attr = from,
                "fromShardDataSize"_attr = fromSize,
                "toShardId"_attr = to,
                "toShardDataSize"_attr = toSize,
                "maxChunkSizeBytes"_attr = collDataSizeInfo.maxChunkSizeBytes);

    if (fromSize <= idealDataSizePerShardForZone) {
        return false;
    }

    if (fromSize - toSize < 3 * collDataSizeInfo.maxChunkSizeBytes) {
        // Do not balance if the collection's size differs too few between the chosen shards
        return false;
    }

    unsigned numJumboChunks = 0;
    bool chunkFound = false;

    const auto& fromShardId = from;
    const auto& toShardId = to;

    for (const auto& zoneRange : distribution.getNormalizedZones()) {
        if (zoneRange.zone != zone) {
            continue;
        }

        distribution.getChunkManager().forEachOverlappingChunk(
            zoneRange.min, zoneRange.max, false /* isMaxInclusive */, [&](const auto& chunk) {
                if (chunk.getShardId() != fromShardId) {
                    return true;  // continue
                }

                if (chunk.isJumbo()) {
                    numJumboChunks++;
                    return true;  // continue
                }

                migrations->emplace_back(toShardId,
                                         chunk.getShardId(),
                                         distribution.nss(),
                                         distribution.getChunkManager().getUUID(),
                                         chunk.getMin(),
                                         boost::none /* max */,
                                         chunk.getLastmod(),
                                         forceJumbo,
                                         collDataSizeInfo.maxChunkSizeBytes);
                invariant(availableShards->erase(chunk.getShardId()));
                invariant(availableShards->erase(toShardId));
                chunkFound = true;
                return false;  // break
            });

        if (chunkFound) {
            return chunkFound;
        }
    }

    invariant(!chunkFound);

    if (numJumboChunks) {
        LOGV2_WARNING(6581602,
                      "Shard has only jumbo chunks for this collection and cannot be balanced",
                      logAttrs(distribution.nss()),
                      "shardId"_attr = from,
                      "zone"_attr = zone,
                      "numJumboChunks"_attr = numJumboChunks);
    }

    return false;
}

ZoneRange::ZoneRange(const BSONObj& a_min, const BSONObj& a_max, const std::string& _zone)
    : min(a_min.getOwned()), max(a_max.getOwned()), zone(_zone) {}

string ZoneRange::toString() const {
    return str::stream() << min << " -->> " << max << "  on  " << zone;
}

MigrateInfo::MigrateInfo(const ShardId& a_to,
                         const NamespaceString& a_nss,
                         const ChunkType& a_chunk,
                         const ForceJumbo a_forceJumbo,
                         boost::optional<int64_t> maxChunkSizeBytes)
    : nss(a_nss), uuid(a_chunk.getCollectionUUID()) {
    invariant(a_to.isValid());

    to = a_to;

    from = a_chunk.getShard();
    minKey = a_chunk.getMin();
    maxKey = a_chunk.getMax();
    version = a_chunk.getVersion();
    forceJumbo = a_forceJumbo;
    optMaxChunkSizeBytes = maxChunkSizeBytes;
}

MigrateInfo::MigrateInfo(const ShardId& a_to,
                         const ShardId& a_from,
                         const NamespaceString& a_nss,
                         const UUID& a_uuid,
                         const BSONObj& a_min,
                         const boost::optional<BSONObj>& a_max,
                         const ChunkVersion& a_version,
                         const ForceJumbo a_forceJumbo,
                         boost::optional<int64_t> maxChunkSizeBytes)
    : nss(a_nss),
      uuid(a_uuid),
      minKey(a_min),
      maxKey(a_max),
      version(a_version),
      forceJumbo(a_forceJumbo),
      optMaxChunkSizeBytes(maxChunkSizeBytes) {
    invariant(a_to.isValid());
    invariant(a_from.isValid());

    to = a_to;
    from = a_from;
}

std::string MigrateInfo::getName() const {
    // Generates a unique name for a MigrateInfo based on the namespace and the lower bound of the
    // chunk being moved.
    StringBuilder buf;
    buf << uuid << "-";

    BSONObjIterator i(minKey);
    while (i.more()) {
        BSONElement e = i.next();
        buf << e.fieldName() << "_" << e.toString(false, true);
    }

    return buf.str();
}

string MigrateInfo::toString() const {
    return str::stream() << uuid << ": [" << minKey << ", " << maxKey << "), from " << from
                         << ", to " << to;
}

boost::optional<int64_t> MigrateInfo::getMaxChunkSizeBytes() const {
    return optMaxChunkSizeBytes;
}

SplitInfo::SplitInfo(const ShardId& inShardId,
                     const NamespaceString& inNss,
                     const ChunkVersion& inCollectionPlacementVersion,
                     const ChunkVersion& inChunkVersion,
                     const BSONObj& inMinKey,
                     const BSONObj& inMaxKey,
                     std::vector<BSONObj> inSplitKeys)
    : shardId(inShardId),
      nss(inNss),
      collectionPlacementVersion(inCollectionPlacementVersion),
      chunkVersion(inChunkVersion),
      minKey(inMinKey),
      maxKey(inMaxKey),
      splitKeys(std::move(inSplitKeys)) {}

std::string SplitInfo::toString() const {
    StringBuilder splitKeysBuilder;
    for (const auto& splitKey : splitKeys) {
        splitKeysBuilder << splitKey.toString() << ", ";
    }

    return "Splitting chunk in {} [ {}, {} ), residing on {} at [ {} ] with version {} and collection placement version {}"_format(
        nss.ns(),
        minKey.toString(),
        maxKey.toString(),
        shardId.toString(),
        splitKeysBuilder.str(),
        chunkVersion.toString(),
        collectionPlacementVersion.toString());
}

MergeInfo::MergeInfo(const ShardId& shardId,
                     const NamespaceString& nss,
                     const UUID& uuid,
                     const ChunkVersion& collectionPlacementVersion,
                     const ChunkRange& chunkRange)
    : shardId(shardId),
      nss(nss),
      uuid(uuid),
      collectionPlacementVersion(collectionPlacementVersion),
      chunkRange(chunkRange) {}

std::string MergeInfo::toString() const {
    return "Merging chunk range {} in {} residing on {} with collection placement version {}"_format(
        chunkRange.toString(),
        nss.toString(),
        shardId.toString(),
        collectionPlacementVersion.toString());
}

MergeAllChunksOnShardInfo::MergeAllChunksOnShardInfo(const ShardId& shardId,
                                                     const NamespaceString& nss)
    : shardId(shardId), nss(nss) {}

std::string MergeAllChunksOnShardInfo::toString() const {
    return "Merging all contiguous chunks residing on shard {} for collection {}"_format(
        shardId.toString(), nss.toString());
}

DataSizeInfo::DataSizeInfo(const ShardId& shardId,
                           const NamespaceString& nss,
                           const UUID& uuid,
                           const ChunkRange& chunkRange,
                           const ShardVersion& version,
                           const KeyPattern& keyPattern,
                           bool estimatedValue,
                           int64_t maxSize)
    : shardId(shardId),
      nss(nss),
      uuid(uuid),
      chunkRange(chunkRange),
      version(version),
      keyPattern(keyPattern),
      estimatedValue(estimatedValue),
      maxSize(maxSize) {}

}  // namespace mongo
