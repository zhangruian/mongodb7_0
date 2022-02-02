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

#include "mongo/db/s/balancer/balancer_defragmentation_policy_impl.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/s/balancer/cluster_statistics.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/logv2/log.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/grid.h"

#include <fmt/format.h>

using namespace fmt::literals;

namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(beforeTransitioningDefragmentationPhase);
MONGO_FAIL_POINT_DEFINE(afterBuildingNextDefragmentationPhase);

using ShardStatistics = ClusterStatistics::ShardStatistics;

const std::string kCurrentPhase("currentPhase");
const std::string kProgress("progress");
const std::string kNoPhase("none");
const std::string kRemainingChunksToProcess("remainingChunksToProcess");

ChunkVersion getShardVersion(OperationContext* opCtx,
                             const ShardId& shardId,
                             const NamespaceString& nss) {
    auto cm = Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfo(opCtx, nss);
    return cm.getVersion(shardId);
}

std::vector<ChunkType> getCollectionChunks(OperationContext* opCtx, const CollectionType& coll) {
    return uassertStatusOK(Grid::get(opCtx)->catalogClient()->getChunks(
        opCtx,
        BSON(ChunkType::collectionUUID() << coll.getUuid()) /*query*/,
        BSON(ChunkType::min() << 1) /*sort*/,
        boost::none /*limit*/,
        nullptr /*opTime*/,
        coll.getEpoch(),
        coll.getTimestamp(),
        repl::ReadConcernLevel::kLocalReadConcern,
        boost::none));
}

uint64_t getCollectionMaxChunkSizeBytes(OperationContext* opCtx, const CollectionType& coll) {
    const auto balancerConfig = Grid::get(opCtx)->getBalancerConfiguration();
    uassertStatusOK(balancerConfig->refreshAndCheck(opCtx));
    return coll.getMaxChunkSizeBytes().value_or(balancerConfig->getMaxChunkSizeBytes());
}

ZoneInfo getCollectionZones(OperationContext* opCtx, const CollectionType& coll) {
    ZoneInfo zones;
    uassertStatusOK(
        ZoneInfo::addTagsFromCatalog(opCtx, coll.getNss(), coll.getKeyPattern(), zones));
    return zones;
}

bool isRetriableForDefragmentation(const Status& error) {
    return (ErrorCodes::isA<ErrorCategory::RetriableError>(error) ||
            error == ErrorCodes::StaleShardVersion || error == ErrorCodes::StaleConfig);
}

void handleActionResult(OperationContext* opCtx,
                        const NamespaceString& nss,
                        const UUID& uuid,
                        const DefragmentationPhaseEnum currentPhase,
                        const Status& status,
                        std::function<void()> onSuccess,
                        std::function<void()> onRetriableError,
                        std::function<void()> onNonRetriableError) {
    if (status.isOK()) {
        onSuccess();
        return;
    }

    if (status.isA<ErrorCategory::StaleShardVersionError>()) {
        if (auto staleInfo = status.extraInfo<StaleConfigInfo>()) {
            Grid::get(opCtx)
                ->catalogCache()
                ->invalidateShardOrEntireCollectionEntryForShardedCollection(
                    nss, staleInfo->getVersionWanted(), staleInfo->getShardId());
        }
    }

    if (isRetriableForDefragmentation(status)) {
        LOGV2_DEBUG(6261701,
                    1,
                    "Hit retriable error while defragmenting collection",
                    "namespace"_attr = nss,
                    "uuid"_attr = uuid,
                    "currentPhase"_attr = currentPhase,
                    "error"_attr = redact(status));
        onRetriableError();
    } else {
        LOGV2_ERROR(6258601,
                    "Defragmentation for collection hit non-retriable error",
                    "namespace"_attr = nss,
                    "uuid"_attr = uuid,
                    "currentPhase"_attr = currentPhase,
                    "error"_attr = redact(status));
        onNonRetriableError();
    }
}

bool areMergeable(const ChunkType& firstChunk,
                  const ChunkType& secondChunk,
                  const ZoneInfo& collectionZones) {
    return firstChunk.getShard() == secondChunk.getShard() &&
        collectionZones.getZoneForChunk(firstChunk.getRange()) ==
        collectionZones.getZoneForChunk(secondChunk.getRange()) &&
        SimpleBSONObjComparator::kInstance.evaluate(firstChunk.getMax() == secondChunk.getMin());
}

void checkForWriteErrors(const write_ops::UpdateCommandReply& response) {
    const auto& writeErrors = response.getWriteErrors();
    if (writeErrors) {
        BSONObj firstWriteError = writeErrors->front();
        uasserted(ErrorCodes::Error(firstWriteError.getIntField("code")),
                  firstWriteError.getStringField("errmsg"));
    }
}

class MergeAndMeasureChunksPhase : public DefragmentationPhase {
public:
    static std::unique_ptr<MergeAndMeasureChunksPhase> build(OperationContext* opCtx,
                                                             const CollectionType& coll) {
        auto collectionChunks = getCollectionChunks(opCtx, coll);
        const auto collectionZones = getCollectionZones(opCtx, coll);

        stdx::unordered_map<ShardId, PendingActions> pendingActionsByShards;
        // Find ranges of chunks; for single-chunk ranges, request DataSize; for multi-range, issue
        // merge
        while (!collectionChunks.empty()) {
            auto upperRangeBound = std::prev(collectionChunks.cend());
            auto lowerRangeBound = upperRangeBound;
            while (lowerRangeBound != collectionChunks.cbegin() &&
                   areMergeable(*std::prev(lowerRangeBound), *lowerRangeBound, collectionZones)) {
                --lowerRangeBound;
            }
            if (lowerRangeBound != upperRangeBound) {
                pendingActionsByShards[upperRangeBound->getShard()].rangesToMerge.emplace_back(
                    lowerRangeBound->getMin(), upperRangeBound->getMax());
            } else {
                if (!upperRangeBound->getEstimatedSizeBytes().has_value()) {
                    pendingActionsByShards[upperRangeBound->getShard()]
                        .rangesWithoutDataSize.emplace_back(upperRangeBound->getMin(),
                                                            upperRangeBound->getMax());
                }
            }
            collectionChunks.erase(lowerRangeBound, std::next(upperRangeBound));
        }
        return std::unique_ptr<MergeAndMeasureChunksPhase>(
            new MergeAndMeasureChunksPhase(coll.getNss(),
                                           coll.getUuid(),
                                           coll.getKeyPattern().toBSON(),
                                           std::move(pendingActionsByShards)));
    }

    DefragmentationPhaseEnum getType() const override {
        return DefragmentationPhaseEnum::kMergeAndMeasureChunks;
    }

    DefragmentationPhaseEnum getNextPhase() const override {
        return _nextPhase;
    }

    boost::optional<DefragmentationAction> popNextStreamableAction(
        OperationContext* opCtx) override {
        boost::optional<DefragmentationAction> nextAction = boost::none;
        if (!_pendingActionsByShards.empty()) {
            auto it = _shardToProcess ? _pendingActionsByShards.find(*_shardToProcess)
                                      : _pendingActionsByShards.begin();

            invariant(it != _pendingActionsByShards.end());

            auto& [shardId, pendingActions] = *it;
            auto shardVersion = getShardVersion(opCtx, shardId, _nss);

            if (pendingActions.rangesWithoutDataSize.size() > pendingActions.rangesToMerge.size()) {
                const auto& rangeToMeasure = pendingActions.rangesWithoutDataSize.back();
                nextAction = boost::optional<DefragmentationAction>(DataSizeInfo(
                    shardId, _nss, _uuid, rangeToMeasure, shardVersion, _shardKey, false));
                pendingActions.rangesWithoutDataSize.pop_back();
            } else if (!pendingActions.rangesToMerge.empty()) {
                const auto& rangeToMerge = pendingActions.rangesToMerge.back();
                nextAction = boost::optional<DefragmentationAction>(
                    MergeInfo(shardId, _nss, _uuid, shardVersion, rangeToMerge));
                pendingActions.rangesToMerge.pop_back();
            }
            if (nextAction.has_value()) {
                ++_outstandingActions;
                if (pendingActions.rangesToMerge.empty() &&
                    pendingActions.rangesWithoutDataSize.empty()) {
                    it = _pendingActionsByShards.erase(it, std::next(it));
                } else {
                    ++it;
                }
            }
            if (it != _pendingActionsByShards.end()) {
                _shardToProcess = it->first;
            } else {
                _shardToProcess = boost::none;
            }
        }
        return nextAction;
    }

    boost::optional<MigrateInfo> popNextMigration(
        OperationContext* opCtx, stdx::unordered_set<ShardId>* usedShards) override {
        return boost::none;
    }

    void applyActionResult(OperationContext* opCtx,
                           const DefragmentationAction& action,
                           const DefragmentationActionResponse& response) override {
        ScopeGuard scopedGuard([&] { --_outstandingActions; });
        if (_aborted) {
            return;
        }
        stdx::visit(
            visit_helper::Overloaded{
                [&](const MergeInfo& mergeAction) {
                    auto& mergeResponse = stdx::get<Status>(response);
                    auto& shardingPendingActions = _pendingActionsByShards[mergeAction.shardId];
                    handleActionResult(
                        opCtx,
                        _nss,
                        _uuid,
                        getType(),
                        mergeResponse,
                        [&]() {
                            shardingPendingActions.rangesWithoutDataSize.emplace_back(
                                mergeAction.chunkRange);
                        },
                        [&]() {
                            shardingPendingActions.rangesToMerge.emplace_back(
                                mergeAction.chunkRange);
                        },
                        [&]() { _abort(getType()); });
                },
                [&](const DataSizeInfo& dataSizeAction) {
                    auto& dataSizeResponse = stdx::get<StatusWith<DataSizeResponse>>(response);
                    handleActionResult(
                        opCtx,
                        _nss,
                        _uuid,
                        getType(),
                        dataSizeResponse.getStatus(),
                        [&]() {
                            ChunkType chunk(dataSizeAction.uuid,
                                            dataSizeAction.chunkRange,
                                            dataSizeAction.version,
                                            dataSizeAction.shardId);
                            auto catalogManager = ShardingCatalogManager::get(opCtx);
                            catalogManager->setChunkEstimatedSize(
                                opCtx,
                                chunk,
                                dataSizeResponse.getValue().sizeBytes,
                                ShardingCatalogClient::kMajorityWriteConcern);
                        },
                        [&]() {
                            auto& shardingPendingActions =
                                _pendingActionsByShards[dataSizeAction.shardId];
                            shardingPendingActions.rangesWithoutDataSize.emplace_back(
                                dataSizeAction.chunkRange);
                        },
                        [&]() { _abort(getType()); });
                },
                [&](const AutoSplitVectorInfo& _) {
                    uasserted(ErrorCodes::BadValue, "Unexpected action type");
                },
                [&](const SplitInfoWithKeyPattern& _) {
                    uasserted(ErrorCodes::BadValue, "Unexpected action type");
                },
                [&](const MigrateInfo& _) {
                    uasserted(ErrorCodes::BadValue, "Unexpected action type");
                },
                [&](const EndOfActionStream& _) {
                    uasserted(ErrorCodes::BadValue, "Unexpected action type");
                }},
            action);
    }

    bool isComplete() const override {
        return _pendingActionsByShards.empty() && _outstandingActions == 0;
    }

    void userAbort() override {
        _abort(DefragmentationPhaseEnum::kSplitChunks);
    }

    BSONObj reportProgress() const override {

        size_t rangesToMerge = 0, rangesWithoutDataSize = 0;
        for (const auto& [_, pendingActions] : _pendingActionsByShards) {
            rangesToMerge += pendingActions.rangesToMerge.size();
            rangesWithoutDataSize += pendingActions.rangesWithoutDataSize.size();
        }
        auto remainingChunksToProcess =
            static_cast<long long>(_outstandingActions + rangesToMerge + rangesWithoutDataSize);

        return BSON(kRemainingChunksToProcess << remainingChunksToProcess);
    }

private:
    struct PendingActions {
        std::vector<ChunkRange> rangesToMerge;
        std::vector<ChunkRange> rangesWithoutDataSize;
    };
    MergeAndMeasureChunksPhase(
        const NamespaceString& nss,
        const UUID& uuid,
        const BSONObj& shardKey,
        stdx::unordered_map<ShardId, PendingActions>&& pendingActionsByShards)
        : _nss(nss),
          _uuid(uuid),
          _shardKey(shardKey),
          _pendingActionsByShards(std::move(pendingActionsByShards)) {}

    void _abort(const DefragmentationPhaseEnum nextPhase) {
        _aborted = true;
        _nextPhase = nextPhase;
        _pendingActionsByShards.clear();
    }

    const NamespaceString _nss;
    const UUID _uuid;
    const BSONObj _shardKey;
    stdx::unordered_map<ShardId, PendingActions> _pendingActionsByShards;
    boost::optional<ShardId> _shardToProcess;
    size_t _outstandingActions{0};
    bool _aborted{false};
    DefragmentationPhaseEnum _nextPhase{DefragmentationPhaseEnum::kMoveAndMergeChunks};
};

class MoveAndMergeChunksPhase : public DefragmentationPhase {
public:
    static std::unique_ptr<MoveAndMergeChunksPhase> build(
        OperationContext* opCtx,
        const CollectionType& coll,
        std::vector<ShardStatistics>&& collectionShardStats) {
        auto collectionZones = getCollectionZones(opCtx, coll);

        stdx::unordered_map<ShardId, ShardInfo> shardInfos;
        for (const auto& shardStats : collectionShardStats) {
            shardInfos.emplace(shardStats.shardId,
                               ShardInfo(shardStats.currSizeBytes,
                                         shardStats.maxSizeBytes,
                                         shardStats.isDraining));
        }

        auto collectionChunks = getCollectionChunks(opCtx, coll);
        const auto maxChunkSizeBytes = getCollectionMaxChunkSizeBytes(opCtx, coll);
        const uint64_t smallChunkSizeThresholdBytes =
            (maxChunkSizeBytes / 100) * kSmallChunkSizeThresholdPctg;

        return std::unique_ptr<MoveAndMergeChunksPhase>(
            new MoveAndMergeChunksPhase(coll.getNss(),
                                        coll.getUuid(),
                                        std::move(collectionChunks),
                                        std::move(shardInfos),
                                        std::move(collectionZones),
                                        smallChunkSizeThresholdBytes));
    }

    DefragmentationPhaseEnum getType() const override {
        return DefragmentationPhaseEnum::kMoveAndMergeChunks;
    }

    DefragmentationPhaseEnum getNextPhase() const override {
        return _nextPhase;
    }

    boost::optional<DefragmentationAction> popNextStreamableAction(
        OperationContext* opCtx) override {
        if (_actionableMerges.empty()) {
            return boost::none;
        }

        _outstandingMerges.push_back(_actionableMerges.front());
        _actionableMerges.pop_front();
        const auto& nextRequest = _outstandingMerges.back();
        auto version = getShardVersion(opCtx, nextRequest.getDestinationShard(), _nss);
        return boost::optional<DefragmentationAction>(
            nextRequest.asMergeInfo(_uuid, _nss, version));
    }

    boost::optional<MigrateInfo> popNextMigration(
        OperationContext* opCtx, stdx::unordered_set<ShardId>* usedShards) override {
        for (const auto& shardId : _shardProcessingOrder) {
            if (usedShards->count(shardId) != 0) {
                // the shard is already busy in a migration
                continue;
            }

            ChunkRangeInfoIterator nextSmallChunk;
            std::list<ChunkRangeInfoIterator> candidateSiblings;
            if (!_findNextSmallChunkInShard(
                    shardId, *usedShards, &nextSmallChunk, &candidateSiblings)) {
                // there isn't a chunk in this shard that can currently be moved and merged with one
                // of its siblings.
                continue;
            }

            // We have a chunk that can be moved&merged with at least one sibling. Choose one...
            invariant(candidateSiblings.size() <= 2);
            auto targetSibling = candidateSiblings.front();
            if (auto challenger = candidateSiblings.back(); targetSibling != challenger) {
                auto targetScore = _rankMergeableSibling(*nextSmallChunk, *targetSibling);
                auto challengerScore = _rankMergeableSibling(*nextSmallChunk, *challenger);
                if (challengerScore > targetScore ||
                    (challengerScore == targetScore &&
                     _shardInfos.at(challenger->shard).currentSizeBytes <
                         _shardInfos.at(targetSibling->shard).currentSizeBytes)) {
                    targetSibling = challenger;
                }
            }

            // ... then build up the migration request, marking the needed resources as busy.
            nextSmallChunk->busyInOperation = true;
            targetSibling->busyInOperation = true;
            usedShards->insert(nextSmallChunk->shard);
            usedShards->insert(targetSibling->shard);
            auto smallChunkVersion = getShardVersion(opCtx, nextSmallChunk->shard, _nss);
            _outstandingMigrations.emplace_back(nextSmallChunk, targetSibling);
            return _outstandingMigrations.back().asMigrateInfo(_uuid, _nss, smallChunkVersion);
        }

        return boost::none;
    }

    void applyActionResult(OperationContext* opCtx,
                           const DefragmentationAction& action,
                           const DefragmentationActionResponse& response) override {
        stdx::visit(
            visit_helper::Overloaded{
                [&](const MigrateInfo& migrationAction) {
                    auto& migrationResponse = stdx::get<Status>(response);
                    auto match =
                        std::find_if(_outstandingMigrations.begin(),
                                     _outstandingMigrations.end(),
                                     [&migrationAction](const MoveAndMergeRequest& request) {
                                         return (migrationAction.minKey.woCompare(
                                                     request.getMigrationMinKey()) == 0);
                                     });
                    invariant(match != _outstandingMigrations.end());
                    MoveAndMergeRequest moveRequest(std::move(*match));
                    _outstandingMigrations.erase(match);

                    if (_aborted) {
                        return;
                    }

                    if (migrationResponse.isOK()) {
                        Grid::get(opCtx)
                            ->catalogCache()
                            ->invalidateShardOrEntireCollectionEntryForShardedCollection(
                                _nss, boost::none, moveRequest.getDestinationShard());

                        auto transferredAmount = moveRequest.getMovedDataSizeBytes();
                        _shardInfos.at(moveRequest.getSourceShard()).currentSizeBytes -=
                            transferredAmount;
                        _shardInfos.at(moveRequest.getDestinationShard()).currentSizeBytes +=
                            transferredAmount;
                        _shardProcessingOrder.sort([this](const ShardId& lhs, const ShardId& rhs) {
                            return _shardInfos.at(lhs).currentSizeBytes >=
                                _shardInfos.at(rhs).currentSizeBytes;
                        });
                        _actionableMerges.push_back(std::move(moveRequest));
                        return;
                    }

                    LOGV2_DEBUG(6290000,
                                1,
                                "Migration failed during collection defragmentation",
                                "namespace"_attr = _nss,
                                "uuid"_attr = _uuid,
                                "currentPhase"_attr = getType(),
                                "error"_attr = redact(migrationResponse));

                    moveRequest.chunkToMove->busyInOperation = false;
                    moveRequest.chunkToMergeWith->busyInOperation = false;

                    if (isRetriableForDefragmentation(migrationResponse)) {
                        // The migration will be eventually retried
                        return;
                    }

                    const auto exceededTimeLimit = [&] {
                        // All errors thrown by the migration destination shard are converted
                        // into OperationFailed. Thus we need to inspect the error message to
                        // match the real error code.

                        // TODO SERVER-62990 introduce and propagate specific error code for
                        // migration failed due to range deletion pending
                        return migrationResponse == ErrorCodes::OperationFailed &&
                            migrationResponse.reason().find(ErrorCodes::errorString(
                                ErrorCodes::ExceededTimeLimit)) != std::string::npos;
                    };

                    if (exceededTimeLimit()) {
                        // The migration failed because there is still a range deletion
                        // pending on the recipient.
                        moveRequest.chunkToMove->shardsToAvoid.emplace(
                            moveRequest.getDestinationShard());
                        return;
                    }

                    LOGV2_ERROR(6290001,
                                "Encountered non-retriable error on migration during "
                                "collection defragmentation",
                                "namespace"_attr = _nss,
                                "uuid"_attr = _uuid,
                                "currentPhase"_attr = getType(),
                                "error"_attr = redact(migrationResponse));
                    _abort(DefragmentationPhaseEnum::kMergeAndMeasureChunks);
                },
                [&](const MergeInfo& mergeAction) {
                    auto& mergeResponse = stdx::get<Status>(response);
                    auto match = std::find_if(_outstandingMerges.begin(),
                                              _outstandingMerges.end(),
                                              [&mergeAction](const MoveAndMergeRequest& request) {
                                                  return mergeAction.chunkRange.containsKey(
                                                      request.getMigrationMinKey());
                                              });
                    invariant(match != _outstandingMerges.end());
                    MoveAndMergeRequest mergeRequest(std::move(*match));
                    _outstandingMerges.erase(match);

                    auto onSuccess = [&] {
                        // The sequence is complete; update the state of the merged chunk...
                        auto& mergedChunk = mergeRequest.chunkToMergeWith;

                        Grid::get(opCtx)
                            ->catalogCache()
                            ->invalidateShardOrEntireCollectionEntryForShardedCollection(
                                _nss, boost::none, mergedChunk->shard);

                        auto& chunkToDelete = mergeRequest.chunkToMove;
                        mergedChunk->range = mergeRequest.asMergedRange();
                        mergedChunk->estimatedSizeBytes += chunkToDelete->estimatedSizeBytes;
                        mergedChunk->busyInOperation = false;
                        // the collection...
                        auto deletedChunkShard = chunkToDelete->shard;
                        _collectionChunks.erase(chunkToDelete);
                        //... and the lookup data structures.
                        _removeIteratorFromSmallChunks(chunkToDelete, deletedChunkShard);
                        if (mergedChunk->estimatedSizeBytes > _smallChunkSizeThresholdBytes) {
                            _removeIteratorFromSmallChunks(mergedChunk, mergedChunk->shard);
                        } else {
                            // Keep the list of small chunk iterators in the recipient sorted
                            auto match = _smallChunksByShard.find(mergedChunk->shard);
                            if (match != _smallChunksByShard.end()) {
                                auto& [_, smallChunksInRecipient] = *match;
                                smallChunksInRecipient.sort(compareChunkRangeInfoIterators);
                            }
                        }
                    };

                    auto onRetriableError = [&] {
                        _actionableMerges.push_back(std::move(mergeRequest));
                    };

                    auto onNonRetriableError = [&]() {
                        _abort(DefragmentationPhaseEnum::kMergeAndMeasureChunks);
                    };

                    if (!_aborted) {
                        handleActionResult(opCtx,
                                           _nss,
                                           _uuid,
                                           getType(),
                                           mergeResponse,
                                           onSuccess,
                                           onRetriableError,
                                           onNonRetriableError);
                    }
                },
                [&](const DataSizeInfo& dataSizeAction) {
                    uasserted(ErrorCodes::BadValue, "Unexpected action type");
                },
                [&](const AutoSplitVectorInfo& _) {
                    uasserted(ErrorCodes::BadValue, "Unexpected action type");
                },
                [&](const SplitInfoWithKeyPattern& _) {
                    uasserted(ErrorCodes::BadValue, "Unexpected action type");
                },
                [&](const EndOfActionStream& _) {
                    uasserted(ErrorCodes::BadValue, "Unexpected action type");
                }},
            action);
    }

    bool isComplete() const override {
        return _smallChunksByShard.empty() && _outstandingMigrations.empty() &&
            _actionableMerges.empty() && _outstandingMerges.empty();
    }

    void userAbort() override {
        _abort(DefragmentationPhaseEnum::kSplitChunks);
    }

    BSONObj reportProgress() const override {
        size_t numSmallChunks = 0;
        for (const auto& [shardId, smallChunks] : _smallChunksByShard) {
            numSmallChunks += smallChunks.size();
        }
        return BSON(kRemainingChunksToProcess << static_cast<long long>(numSmallChunks));
    }

private:
    // Internal representation of the chunk metadata required to generate a MoveAndMergeRequest
    struct ChunkRangeInfo {
        ChunkRangeInfo(ChunkRange&& range, const ShardId& shard, long long estimatedSizeBytes)
            : range(std::move(range)),
              shard(shard),
              estimatedSizeBytes(estimatedSizeBytes),
              busyInOperation(false) {}
        ChunkRange range;
        const ShardId shard;
        long long estimatedSizeBytes;
        bool busyInOperation;
        // Last time we failed to find a suitable destination shard due to temporary constraints
        boost::optional<Date_t> lastFailedAttemptTime;
        // Shards that still have a deletion pending for this range
        stdx::unordered_set<ShardId> shardsToAvoid;
    };

    struct ShardInfo {
        ShardInfo(uint64_t currentSizeBytes, uint64_t maxSizeBytes, bool draining)
            : currentSizeBytes(currentSizeBytes), maxSizeBytes(maxSizeBytes), draining(draining) {}

        bool isDraining() const {
            return draining;
        }

        bool hasCapacityFor(uint64_t newDataSize) const {
            return (maxSizeBytes == 0 || currentSizeBytes + newDataSize < maxSizeBytes);
        }

        uint64_t currentSizeBytes;
        const uint64_t maxSizeBytes;
        const bool draining;
    };

    using ChunkRangeInfos = std::list<ChunkRangeInfo>;
    using ChunkRangeInfoIterator = std::list<ChunkRangeInfo>::iterator;

    static bool compareChunkRangeInfoIterators(const ChunkRangeInfoIterator& lhs,
                                               const ChunkRangeInfoIterator& rhs) {
        // Small chunks are ordered by decreasing order of estimatedSizeBytes
        // except the ones that we failed to move due to temporary constraints that will be at the
        // end of the list ordered by last attempt time
        return lhs->lastFailedAttemptTime.value_or(Date_t::min()) <=
            rhs->lastFailedAttemptTime.value_or(Date_t::min()) &&
            lhs->estimatedSizeBytes < rhs->estimatedSizeBytes;
    }

    // Helper class to generate the Migration and Merge actions required to join together the chunks
    // specified in the constructor
    struct MoveAndMergeRequest {
    public:
        MoveAndMergeRequest(const ChunkRangeInfoIterator& chunkToMove,
                            const ChunkRangeInfoIterator& chunkToMergeWith)
            : chunkToMove(chunkToMove),
              chunkToMergeWith(chunkToMergeWith),
              _isChunkToMergeLeftSibling(
                  chunkToMergeWith->range.getMax().woCompare(chunkToMove->range.getMin()) == 0) {}

        MigrateInfo asMigrateInfo(const UUID& collUuid,
                                  const NamespaceString& nss,
                                  const ChunkVersion& version) const {
            return MigrateInfo(chunkToMergeWith->shard,
                               nss,
                               ChunkType(collUuid, chunkToMove->range, version, chunkToMove->shard),
                               MoveChunkRequest::ForceJumbo::kForceBalancer,
                               MigrateInfo::chunksImbalance);
        }

        ChunkRange asMergedRange() const {
            return ChunkRange(_isChunkToMergeLeftSibling ? chunkToMergeWith->range.getMin()
                                                         : chunkToMove->range.getMin(),
                              _isChunkToMergeLeftSibling ? chunkToMove->range.getMax()
                                                         : chunkToMergeWith->range.getMax());
        }

        MergeInfo asMergeInfo(const UUID& collUuid,
                              const NamespaceString& nss,
                              const ChunkVersion& version) const {
            return MergeInfo(chunkToMergeWith->shard, nss, collUuid, version, asMergedRange());
        }

        const ShardId& getSourceShard() const {
            return chunkToMove->shard;
        }

        const ShardId& getDestinationShard() const {
            return chunkToMergeWith->shard;
        }

        const BSONObj& getMigrationMinKey() const {
            return chunkToMove->range.getMin();
        }

        uint64_t getMovedDataSizeBytes() const {
            return chunkToMove->estimatedSizeBytes;
        }

        ChunkRangeInfoIterator chunkToMove;
        ChunkRangeInfoIterator chunkToMergeWith;

    private:
        bool _isChunkToMergeLeftSibling;
    };

    static constexpr uint64_t kSmallChunkSizeThresholdPctg = 25;

    const NamespaceString _nss;

    const UUID _uuid;

    // The collection routing table - expressed in ChunkRangeInfo
    ChunkRangeInfos _collectionChunks;

    // List of indexes to elements in _collectionChunks that are eligible to be moved.
    std::map<ShardId, std::list<ChunkRangeInfoIterator>> _smallChunksByShard;

    stdx::unordered_map<ShardId, ShardInfo> _shardInfos;

    // Sorted list of shard IDs by decreasing current size (@see _shardInfos)
    std::list<ShardId> _shardProcessingOrder;

    // Set of attributes representing the currently active move&merge sequences
    std::list<MoveAndMergeRequest> _outstandingMigrations;
    std::list<MoveAndMergeRequest> _actionableMerges;
    std::list<MoveAndMergeRequest> _outstandingMerges;

    ZoneInfo _zoneInfo;

    const int64_t _smallChunkSizeThresholdBytes;

    bool _aborted{false};

    DefragmentationPhaseEnum _nextPhase{DefragmentationPhaseEnum::kMergeChunks};

    MoveAndMergeChunksPhase(const NamespaceString& nss,
                            const UUID& uuid,
                            std::vector<ChunkType>&& collectionChunks,
                            stdx::unordered_map<ShardId, ShardInfo>&& shardInfos,
                            ZoneInfo&& collectionZones,
                            uint64_t smallChunkSizeThresholdBytes)
        : _nss(nss),
          _uuid(uuid),
          _collectionChunks(),
          _smallChunksByShard(),
          _shardInfos(std::move(shardInfos)),
          _shardProcessingOrder(),
          _outstandingMigrations(),
          _actionableMerges(),
          _outstandingMerges(),
          _zoneInfo(std::move(collectionZones)),
          _smallChunkSizeThresholdBytes(smallChunkSizeThresholdBytes) {

        // Load the collection routing table in a std::list to ease later manipulation
        for (auto&& chunk : collectionChunks) {
            if (!chunk.getEstimatedSizeBytes().has_value()) {
                LOGV2_WARNING(
                    6172701,
                    "Chunk with no estimated size detected while building MoveAndMergeChunksPhase",
                    "namespace"_attr = _nss,
                    "uuid"_attr = _uuid,
                    "range"_attr = chunk.getRange());
                _abort(DefragmentationPhaseEnum::kMergeAndMeasureChunks);
                return;
            }
            const uint64_t estimatedChunkSize = chunk.getEstimatedSizeBytes().get();
            _collectionChunks.emplace_back(chunk.getRange(), chunk.getShard(), estimatedChunkSize);
        }

        // Compose the index of small chunks
        for (auto chunkIt = _collectionChunks.begin(); chunkIt != _collectionChunks.end();
             ++chunkIt) {
            if (chunkIt->estimatedSizeBytes <= _smallChunkSizeThresholdBytes) {
                _smallChunksByShard[chunkIt->shard].emplace_back(chunkIt);
            }
        }
        // Each small chunk within a shard must be sorted by increasing chunk size
        for (auto& [_, smallChunksInShard] : _smallChunksByShard) {
            smallChunksInShard.sort(compareChunkRangeInfoIterators);
        }

        // Set the initial shard processing order
        for (const auto& [shardId, _] : _shardInfos) {
            _shardProcessingOrder.push_back(shardId);
        }
        _shardProcessingOrder.sort([this](const ShardId& lhs, const ShardId& rhs) {
            return _shardInfos.at(lhs).currentSizeBytes >= _shardInfos.at(rhs).currentSizeBytes;
        });
    }

    void _abort(const DefragmentationPhaseEnum nextPhase) {
        _aborted = true;
        _nextPhase = nextPhase;
        _actionableMerges.clear();
        _smallChunksByShard.clear();
        _shardProcessingOrder.clear();
    }

    // Returns the list of siblings that are eligible to be move&merged with the specified chunk,
    // based  on shard zones and data capacity. (It does NOT take into account whether chunks are
    // currently involved in a move/merge operation).
    std::list<ChunkRangeInfoIterator> _getChunkSiblings(
        const ChunkRangeInfoIterator& chunkIt) const {
        std::list<ChunkRangeInfoIterator> siblings;
        auto canBeMoveAndMerged = [this](const ChunkRangeInfoIterator& chunkIt,
                                         const ChunkRangeInfoIterator& siblingIt) {
            auto onSameZone = _zoneInfo.getZoneForChunk(chunkIt->range) ==
                _zoneInfo.getZoneForChunk(siblingIt->range);
            auto destinationAvailable = chunkIt->shard == siblingIt->shard ||
                !_shardInfos.at(siblingIt->shard).isDraining();
            return (onSameZone && destinationAvailable);
        };

        if (auto rightSibling = std::next(chunkIt);
            rightSibling != _collectionChunks.end() && canBeMoveAndMerged(chunkIt, rightSibling)) {
            siblings.push_back(rightSibling);
        }
        if (chunkIt != _collectionChunks.begin()) {
            auto leftSibling = std::prev(chunkIt);
            if (canBeMoveAndMerged(chunkIt, leftSibling)) {
                siblings.push_back(leftSibling);
            }
        }
        return siblings;
    }

    // Computes whether there is a chunk in the specified shard that can be moved&merged with one or
    // both of its siblings. Chunks/siblings that are currently being moved/merged are not eligible.
    //
    // The function also clears the internal state from elements that cannot be processed by the
    // phase (chunks with no siblings, shards with no small chunks).
    //
    // Returns true on success (storing the related info in nextSmallChunk + smallChunkSiblings),
    // false otherwise.
    bool _findNextSmallChunkInShard(const ShardId& shard,
                                    const stdx::unordered_set<ShardId>& usedShards,
                                    ChunkRangeInfoIterator* nextSmallChunk,
                                    std::list<ChunkRangeInfoIterator>* smallChunkSiblings) {
        auto matchingShardInfo = _smallChunksByShard.find(shard);
        if (matchingShardInfo == _smallChunksByShard.end()) {
            return false;
        }
        auto& smallChunksInShard = matchingShardInfo->second;
        for (auto candidateIt = smallChunksInShard.begin();
             candidateIt != smallChunksInShard.end();) {
            if ((*candidateIt)->busyInOperation) {
                ++candidateIt;
                continue;
            }
            auto candidateSiblings = _getChunkSiblings(*candidateIt);
            if (candidateSiblings.empty()) {
                // The current chunk cannot be processed by the algorithm - remove it.
                candidateIt = smallChunksInShard.erase(candidateIt);
                continue;
            }

            size_t siblingsDiscardedDueToRangeDeletion = 0;

            for (const auto& sibling : candidateSiblings) {
                if (sibling->busyInOperation || usedShards.count(sibling->shard)) {
                    continue;
                }
                if ((*candidateIt)->shardsToAvoid.count(sibling->shard)) {
                    ++siblingsDiscardedDueToRangeDeletion;
                    continue;
                }
                smallChunkSiblings->push_back(sibling);
            }

            if (!smallChunkSiblings->empty()) {
                *nextSmallChunk = *candidateIt;
                return true;
            }


            if (siblingsDiscardedDueToRangeDeletion == candidateSiblings.size()) {
                // All the siblings have been discarded because an overlapping range deletion is
                // still pending on the destination shard.
                if (!(*candidateIt)->lastFailedAttemptTime) {
                    // This is the first time we discard this chunk due to overlapping range
                    // deletions pending. Enqueue it back on the list so we will try to move it
                    // again when we will have drained all the other chunks for this shard.
                    LOGV2_DEBUG(6290002,
                                1,
                                "Postponing small chunk processing due to pending range deletion "
                                "on recipient shard(s)",
                                "namespace"_attr = _nss,
                                "uuid"_attr = _uuid,
                                "range"_attr = (*candidateIt)->range,
                                "estimatedSizeBytes"_attr = (*candidateIt)->estimatedSizeBytes,
                                "numCandidateSiblings"_attr = candidateSiblings.size());
                    (*candidateIt)->lastFailedAttemptTime = Date_t::now();
                    (*candidateIt)->shardsToAvoid.clear();
                    smallChunksInShard.emplace_back(*candidateIt);
                } else {
                    LOGV2(6290003,
                          "Discarding small chunk due to pending range deletion on recipient shard",
                          "namespace"_attr = _nss,
                          "uuid"_attr = _uuid,
                          "range"_attr = (*candidateIt)->range,
                          "estimatedSizeBytes"_attr = (*candidateIt)->estimatedSizeBytes,
                          "numCandidateSiblings"_attr = candidateSiblings.size(),
                          "lastFailedAttempt"_attr = (*candidateIt)->lastFailedAttemptTime);
                }
                candidateIt = smallChunksInShard.erase(candidateIt);
                continue;
            }

            ++candidateIt;
        }
        // No candidate could be found - clear the shard entry if needed
        if (smallChunksInShard.empty()) {
            _smallChunksByShard.erase(matchingShardInfo);
        }
        return false;
    }

    uint32_t _rankMergeableSibling(const ChunkRangeInfo& chunkTobeMovedAndMerged,
                                   const ChunkRangeInfo& mergeableSibling) {
        static constexpr uint32_t kNoMoveRequired = 1 << 4;
        static constexpr uint32_t kDestinationNotMaxedOut = 1 << 3;
        static constexpr uint32_t kConvenientMove = 1 << 2;
        static constexpr uint32_t kMergeSolvesTwoPendingChunks = 1 << 1;
        static constexpr uint32_t kMergeSolvesOnePendingChunk = 1;
        uint32_t ranking = 0;
        if (chunkTobeMovedAndMerged.shard == mergeableSibling.shard) {
            ranking += kNoMoveRequired;
        } else if (chunkTobeMovedAndMerged.estimatedSizeBytes <
                   mergeableSibling.estimatedSizeBytes) {
            ranking += kConvenientMove;
        }
        auto estimatedMergedSize =
            chunkTobeMovedAndMerged.estimatedSizeBytes + mergeableSibling.estimatedSizeBytes;
        if (estimatedMergedSize > _smallChunkSizeThresholdBytes) {
            ranking += mergeableSibling.estimatedSizeBytes < _smallChunkSizeThresholdBytes
                ? kMergeSolvesTwoPendingChunks
                : kMergeSolvesOnePendingChunk;
        }
        if (_shardInfos.at(mergeableSibling.shard)
                .hasCapacityFor(chunkTobeMovedAndMerged.estimatedSizeBytes)) {
            ranking += kDestinationNotMaxedOut;
        }
        return ranking;
    }

    void _removeIteratorFromSmallChunks(const ChunkRangeInfoIterator& chunkIt,
                                        const ShardId& parentShard) {
        auto matchingShardIt = _smallChunksByShard.find(parentShard);
        if (matchingShardIt == _smallChunksByShard.end()) {
            return;
        }
        auto& smallChunksInShard = matchingShardIt->second;
        auto match = std::find(smallChunksInShard.begin(), smallChunksInShard.end(), chunkIt);
        if (match == smallChunksInShard.end()) {
            return;
        }
        smallChunksInShard.erase(match);
        if (smallChunksInShard.empty()) {
            _smallChunksByShard.erase(parentShard);
        }
    }
};

class MergeChunksPhase : public DefragmentationPhase {
public:
    static std::unique_ptr<MergeChunksPhase> build(OperationContext* opCtx,
                                                   const CollectionType& coll) {
        auto collectionChunks = getCollectionChunks(opCtx, coll);
        const auto collectionZones = getCollectionZones(opCtx, coll);

        // Find ranges of mergeable chunks
        stdx::unordered_map<ShardId, std::vector<ChunkRange>> unmergedRangesByShard;
        while (!collectionChunks.empty()) {
            auto upperRangeBound = std::prev(collectionChunks.cend());
            auto lowerRangeBound = upperRangeBound;
            while (lowerRangeBound != collectionChunks.cbegin() &&
                   areMergeable(*std::prev(lowerRangeBound), *lowerRangeBound, collectionZones)) {
                --lowerRangeBound;
            }
            if (lowerRangeBound != upperRangeBound) {
                unmergedRangesByShard[upperRangeBound->getShard()].emplace_back(
                    lowerRangeBound->getMin(), upperRangeBound->getMax());
            }

            collectionChunks.erase(lowerRangeBound, std::next(upperRangeBound));
        }
        return std::unique_ptr<MergeChunksPhase>(
            new MergeChunksPhase(coll.getNss(), coll.getUuid(), std::move(unmergedRangesByShard)));
    }

    DefragmentationPhaseEnum getType() const override {
        return DefragmentationPhaseEnum::kMergeChunks;
    }

    DefragmentationPhaseEnum getNextPhase() const override {
        return _nextPhase;
    }

    boost::optional<DefragmentationAction> popNextStreamableAction(
        OperationContext* opCtx) override {
        if (_unmergedRangesByShard.empty()) {
            return boost::none;
        }

        auto it = _shardToProcess ? _unmergedRangesByShard.find(*_shardToProcess)
                                  : _unmergedRangesByShard.begin();

        invariant(it != _unmergedRangesByShard.end());

        auto& [shardId, unmergedRanges] = *it;
        invariant(!unmergedRanges.empty());
        auto shardVersion = getShardVersion(opCtx, shardId, _nss);
        const auto& rangeToMerge = unmergedRanges.back();
        boost::optional<DefragmentationAction> nextAction = boost::optional<DefragmentationAction>(
            MergeInfo(shardId, _nss, _uuid, shardVersion, rangeToMerge));
        unmergedRanges.pop_back();
        ++_outstandingActions;
        if (unmergedRanges.empty()) {
            it = _unmergedRangesByShard.erase(it, std::next(it));
        } else {
            ++it;
        }
        if (it != _unmergedRangesByShard.end()) {
            _shardToProcess = it->first;
        } else {
            _shardToProcess = boost::none;
        }

        return nextAction;
    }

    boost::optional<MigrateInfo> popNextMigration(
        OperationContext* opCtx, stdx::unordered_set<ShardId>* usedShards) override {
        return boost::none;
    }

    void applyActionResult(OperationContext* opCtx,
                           const DefragmentationAction& action,
                           const DefragmentationActionResponse& response) override {
        ScopeGuard scopedGuard([&] { --_outstandingActions; });
        if (_aborted) {
            return;
        }
        stdx::visit(
            visit_helper::Overloaded{
                [&](const MergeInfo& mergeAction) {
                    auto& mergeResponse = stdx::get<Status>(response);
                    auto onSuccess = [] {};
                    auto onRetriableError = [&] {
                        _unmergedRangesByShard[mergeAction.shardId].emplace_back(
                            mergeAction.chunkRange);
                    };
                    auto onNonretriableError = [this] { _abort(getType()); };
                    handleActionResult(opCtx,
                                       _nss,
                                       _uuid,
                                       getType(),
                                       mergeResponse,
                                       onSuccess,
                                       onRetriableError,
                                       onNonretriableError);
                },
                [&](const DataSizeInfo& _) {
                    uasserted(ErrorCodes::BadValue, "Unexpected action type");
                },
                [&](const AutoSplitVectorInfo& _) {
                    uasserted(ErrorCodes::BadValue, "Unexpected action type");
                },
                [&](const SplitInfoWithKeyPattern& _) {
                    uasserted(ErrorCodes::BadValue, "Unexpected action type");
                },
                [&](const MigrateInfo& _) {
                    uasserted(ErrorCodes::BadValue, "Unexpected action type");
                },
                [&](const EndOfActionStream& _) {
                    uasserted(ErrorCodes::BadValue, "Unexpected action type");
                }},
            action);
    }

    bool isComplete() const override {
        return _unmergedRangesByShard.empty() && _outstandingActions == 0;
    }

    void userAbort() override {
        _abort(DefragmentationPhaseEnum::kSplitChunks);
    }

    BSONObj reportProgress() const override {
        size_t rangesToMerge = 0;
        for (const auto& [_, unmergedRanges] : _unmergedRangesByShard) {
            rangesToMerge += unmergedRanges.size();
        }
        auto remainingRangesToProcess = static_cast<long long>(_outstandingActions + rangesToMerge);

        return BSON(kRemainingChunksToProcess << remainingRangesToProcess);
    }

private:
    MergeChunksPhase(const NamespaceString& nss,
                     const UUID& uuid,
                     stdx::unordered_map<ShardId, std::vector<ChunkRange>>&& unmergedRangesByShard)
        : _nss(nss), _uuid(uuid), _unmergedRangesByShard(std::move(unmergedRangesByShard)) {}

    void _abort(const DefragmentationPhaseEnum nextPhase) {
        _aborted = true;
        _nextPhase = nextPhase;
        _unmergedRangesByShard.clear();
    }

    const NamespaceString _nss;
    const UUID _uuid;
    stdx::unordered_map<ShardId, std::vector<ChunkRange>> _unmergedRangesByShard;
    boost::optional<ShardId> _shardToProcess;
    size_t _outstandingActions{0};
    bool _aborted{false};
    DefragmentationPhaseEnum _nextPhase{DefragmentationPhaseEnum::kSplitChunks};
};

class SplitChunksPhase : public DefragmentationPhase {
public:
    static std::unique_ptr<SplitChunksPhase> build(OperationContext* opCtx,
                                                   const CollectionType& coll) {
        auto collectionChunks = uassertStatusOK(Grid::get(opCtx)->catalogClient()->getChunks(
            opCtx,
            BSON(ChunkType::collectionUUID() << coll.getUuid()) /*query*/,
            BSON(ChunkType::min() << 1) /*sort*/,
            boost::none /*limit*/,
            nullptr /*opTime*/,
            coll.getEpoch(),
            coll.getTimestamp(),
            repl::ReadConcernLevel::kLocalReadConcern,
            boost::none));

        stdx::unordered_map<ShardId, PendingActions> pendingActionsByShards;

        uint64_t maxChunkSizeBytes = getCollectionMaxChunkSizeBytes(opCtx, coll);

        // Issue AutoSplitVector for all chunks with estimated size greater than max chunk size or
        // with no estimated size.
        for (const auto& chunk : collectionChunks) {
            auto chunkSize = chunk.getEstimatedSizeBytes();
            if (!chunkSize || (uint64_t)chunkSize.get() > maxChunkSizeBytes) {
                pendingActionsByShards[chunk.getShard()].rangesToFindSplitPoints.emplace_back(
                    chunk.getMin(), chunk.getMax());
            }
        }

        return std::unique_ptr<SplitChunksPhase>(
            new SplitChunksPhase(coll.getNss(),
                                 coll.getUuid(),
                                 coll.getKeyPattern().toBSON(),
                                 maxChunkSizeBytes,
                                 std::move(pendingActionsByShards)));
    }

    DefragmentationPhaseEnum getType() const override {
        return DefragmentationPhaseEnum::kSplitChunks;
    }

    DefragmentationPhaseEnum getNextPhase() const override {
        return _nextPhase;
    }

    boost::optional<DefragmentationAction> popNextStreamableAction(
        OperationContext* opCtx) override {
        boost::optional<DefragmentationAction> nextAction = boost::none;
        if (!_pendingActionsByShards.empty()) {
            auto it = _shardToProcess ? _pendingActionsByShards.find(*_shardToProcess)
                                      : _pendingActionsByShards.begin();

            invariant(it != _pendingActionsByShards.end());

            auto& [shardId, pendingActions] = *it;
            auto shardVersion = getShardVersion(opCtx, shardId, _nss);

            if (!pendingActions.rangesToSplit.empty()) {
                const auto& [rangeToSplit, splitPoints] = pendingActions.rangesToSplit.back();
                nextAction = boost::optional<DefragmentationAction>(
                    SplitInfoWithKeyPattern(shardId,
                                            _nss,
                                            shardVersion,
                                            rangeToSplit.getMin(),
                                            rangeToSplit.getMax(),
                                            splitPoints,
                                            _uuid,
                                            _shardKey));
                pendingActions.rangesToSplit.pop_back();
            } else if (!pendingActions.rangesToFindSplitPoints.empty()) {
                const auto& rangeToAutoSplit = pendingActions.rangesToFindSplitPoints.back();
                nextAction = boost::optional<DefragmentationAction>(
                    AutoSplitVectorInfo(shardId,
                                        _nss,
                                        _uuid,
                                        shardVersion,
                                        _shardKey,
                                        rangeToAutoSplit.getMin(),
                                        rangeToAutoSplit.getMax(),
                                        _maxChunkSizeBytes));
                pendingActions.rangesToFindSplitPoints.pop_back();
            }
            if (nextAction.has_value()) {
                ++_outstandingActions;
                if (pendingActions.rangesToFindSplitPoints.empty() &&
                    pendingActions.rangesToSplit.empty()) {
                    it = _pendingActionsByShards.erase(it, std::next(it));
                } else {
                    ++it;
                }
            }
            if (it != _pendingActionsByShards.end()) {
                _shardToProcess = it->first;
            } else {
                _shardToProcess = boost::none;
            }
        }
        return nextAction;
    }

    boost::optional<MigrateInfo> popNextMigration(
        OperationContext* opCtx, stdx::unordered_set<ShardId>* usedShards) override {
        return boost::none;
    }

    bool moreSplitPointsToReceive(const SplitPoints& splitPoints) {
        auto addBSONSize = [](const int& size, const BSONObj& obj) { return size + obj.objsize(); };
        int totalSize = std::accumulate(splitPoints.begin(), splitPoints.end(), 0, addBSONSize);
        return totalSize >= BSONObjMaxUserSize - 4096;
    }

    void applyActionResult(OperationContext* opCtx,
                           const DefragmentationAction& action,
                           const DefragmentationActionResponse& response) override {
        ScopeGuard scopedGuard([&] { --_outstandingActions; });
        if (_aborted) {
            return;
        }
        stdx::visit(
            visit_helper::Overloaded{
                [&](const MergeInfo& _) {
                    uasserted(ErrorCodes::BadValue, "Unexpected action type");
                },
                [&](const DataSizeInfo& _) {
                    uasserted(ErrorCodes::BadValue, "Unexpected action type");
                },
                [&](const AutoSplitVectorInfo& autoSplitVectorAction) {
                    auto& splitVectorResponse = stdx::get<StatusWith<SplitPoints>>(response);
                    handleActionResult(
                        opCtx,
                        _nss,
                        _uuid,
                        getType(),
                        splitVectorResponse.getStatus(),
                        [&]() {
                            auto& splitPoints = splitVectorResponse.getValue();
                            if (!splitPoints.empty()) {
                                auto& pendingActions =
                                    _pendingActionsByShards[autoSplitVectorAction.shardId];
                                pendingActions.rangesToSplit.push_back(
                                    std::make_pair(ChunkRange(autoSplitVectorAction.minKey,
                                                              autoSplitVectorAction.maxKey),
                                                   splitVectorResponse.getValue()));
                                // TODO (SERVER-61678): replace with check for continuation flag
                                if (moreSplitPointsToReceive(splitPoints)) {
                                    pendingActions.rangesToFindSplitPoints.emplace_back(
                                        splitPoints.back(), autoSplitVectorAction.maxKey);
                                }
                            }
                        },
                        [&]() {
                            auto& pendingActions =
                                _pendingActionsByShards[autoSplitVectorAction.shardId];
                            pendingActions.rangesToFindSplitPoints.emplace_back(
                                autoSplitVectorAction.minKey, autoSplitVectorAction.maxKey);
                        },
                        [&]() { _abort(getType()); });
                },
                [&](const SplitInfoWithKeyPattern& splitAction) {
                    auto& splitResponse = stdx::get<Status>(response);
                    handleActionResult(
                        opCtx,
                        _nss,
                        _uuid,
                        getType(),
                        splitResponse,
                        []() {},
                        [&]() {
                            auto& pendingActions =
                                _pendingActionsByShards[splitAction.info.shardId];
                            pendingActions.rangesToSplit.push_back(std::make_pair(
                                ChunkRange(splitAction.info.minKey, splitAction.info.maxKey),
                                splitAction.info.splitKeys));
                        },
                        [&]() { _abort(getType()); });
                },
                [&](const MigrateInfo& _) {
                    uasserted(ErrorCodes::BadValue, "Unexpected action type");
                },
                [&](const EndOfActionStream& _) {
                    uasserted(ErrorCodes::BadValue, "Unexpected action type");
                }},
            action);
    }

    bool isComplete() const override {
        return _pendingActionsByShards.empty() && _outstandingActions == 0;
    }

    void userAbort() override {}

    BSONObj reportProgress() const override {
        size_t rangesToFindSplitPoints = 0, rangesToSplit = 0;
        for (const auto& [shardId, pendingActions] : _pendingActionsByShards) {
            rangesToFindSplitPoints += pendingActions.rangesToFindSplitPoints.size();
            rangesToSplit += pendingActions.rangesToSplit.size();
        }
        auto remainingChunksToProcess =
            static_cast<long long>(_outstandingActions + rangesToFindSplitPoints + rangesToSplit);
        return BSON(kRemainingChunksToProcess << remainingChunksToProcess);
    }

private:
    struct PendingActions {
        std::vector<ChunkRange> rangesToFindSplitPoints;
        std::vector<std::pair<ChunkRange, SplitPoints>> rangesToSplit;
    };
    SplitChunksPhase(const NamespaceString& nss,
                     const UUID& uuid,
                     const BSONObj& shardKey,
                     const long long& maxChunkSizeBytes,
                     stdx::unordered_map<ShardId, PendingActions>&& pendingActionsByShards)
        : _nss(nss),
          _uuid(uuid),
          _shardKey(shardKey),
          _maxChunkSizeBytes(maxChunkSizeBytes),
          _pendingActionsByShards(std::move(pendingActionsByShards)) {}

    void _abort(const DefragmentationPhaseEnum nextPhase) {
        _aborted = true;
        _nextPhase = nextPhase;
        _pendingActionsByShards.clear();
    }

    const NamespaceString _nss;
    const UUID _uuid;
    const BSONObj _shardKey;
    const long long _maxChunkSizeBytes;
    stdx::unordered_map<ShardId, PendingActions> _pendingActionsByShards;
    boost::optional<ShardId> _shardToProcess;
    size_t _outstandingActions{0};
    bool _aborted{false};
    DefragmentationPhaseEnum _nextPhase{DefragmentationPhaseEnum::kFinished};
};

}  // namespace

void BalancerDefragmentationPolicyImpl::refreshCollectionDefragmentationStatus(
    OperationContext* opCtx, const CollectionType& coll) {
    stdx::lock_guard<Latch> lk(_stateMutex);
    const auto& uuid = coll.getUuid();
    if (coll.getDefragmentCollection() && !_defragmentationStates.contains(uuid)) {
        _initializeCollectionState(lk, opCtx, coll);
        _yieldNextStreamingAction(lk, opCtx);
    }
}

void BalancerDefragmentationPolicyImpl::abortCollectionDefragmentation(OperationContext* opCtx,
                                                                       const NamespaceString& nss) {
    stdx::lock_guard<Latch> lk(_stateMutex);
    auto coll = Grid::get(opCtx)->catalogClient()->getCollection(opCtx, nss, {});
    if (coll.getDefragmentCollection()) {
        if (_defragmentationStates.contains(coll.getUuid())) {
            // Notify phase to abort current phase
            _defragmentationStates.at(coll.getUuid())->userAbort();
        }
        // Change persisted phase to kSplitChunks
        _persistPhaseUpdate(opCtx, DefragmentationPhaseEnum::kSplitChunks, coll.getUuid());
    }
}

BSONObj BalancerDefragmentationPolicyImpl::reportProgressOn(const UUID& uuid) {
    stdx::lock_guard<Latch> lk(_stateMutex);
    auto match = _defragmentationStates.find(uuid);
    if (match == _defragmentationStates.end() || !match->second) {
        return BSON(kCurrentPhase << kNoPhase);
    }
    const auto& collDefragmentationPhase = match->second;
    return BSON(
        kCurrentPhase << DefragmentationPhase_serializer(collDefragmentationPhase->getType())
                      << kProgress << collDefragmentationPhase->reportProgress());
}

MigrateInfoVector BalancerDefragmentationPolicyImpl::selectChunksToMove(
    OperationContext* opCtx, stdx::unordered_set<ShardId>* usedShards) {
    MigrateInfoVector chunksToMove;
    stdx::lock_guard<Latch> lk(_stateMutex);
    // TODO (SERVER-61635) evaluate fairness
    bool done = false;
    while (!done) {
        auto selectedChunksFromPreviousRound = chunksToMove.size();
        for (auto it = _defragmentationStates.begin(); it != _defragmentationStates.end();) {
            try {
                const auto phaseAdvanced = _refreshDefragmentationPhaseFor(opCtx, it->first);
                auto& collDefragmentationPhase = it->second;
                if (!collDefragmentationPhase) {
                    it = _defragmentationStates.erase(it, std::next(it));
                    continue;
                }
                auto actionableMigration =
                    collDefragmentationPhase->popNextMigration(opCtx, usedShards);
                if (actionableMigration.has_value()) {
                    chunksToMove.push_back(std::move(*actionableMigration));
                } else if (phaseAdvanced) {
                    _yieldNextStreamingAction(lk, opCtx);
                }
                ++it;
            } catch (DBException& e) {
                // Catch getCollection and getShardVersion errors. Should only occur if collection
                // has been removed.
                LOGV2_ERROR(6172700,
                            "Error while getting next migration",
                            "uuid"_attr = it->first,
                            "error"_attr = redact(e));
                it = _defragmentationStates.erase(it, std::next(it));
            }
        }
        done = (chunksToMove.size() == selectedChunksFromPreviousRound);
    }
    return chunksToMove;
}

SemiFuture<DefragmentationAction> BalancerDefragmentationPolicyImpl::getNextStreamingAction(
    OperationContext* opCtx) {
    stdx::lock_guard<Latch> lk(_stateMutex);
    if (_concurrentStreamingOps < kMaxConcurrentOperations) {
        if (auto action = _nextStreamingAction(opCtx)) {
            _concurrentStreamingOps++;
            return SemiFuture<DefragmentationAction>::makeReady(*action);
        }
    }
    auto [promise, future] = makePromiseFuture<DefragmentationAction>();
    _nextStreamingActionPromise = std::move(promise);
    return std::move(future).semi();
}

bool BalancerDefragmentationPolicyImpl::_refreshDefragmentationPhaseFor(OperationContext* opCtx,
                                                                        const UUID& collUuid) {
    auto& currentPhase = _defragmentationStates.at(collUuid);
    auto currentPhaseCompleted = [&currentPhase] {
        return currentPhase && currentPhase->isComplete();
    };

    if (!currentPhaseCompleted()) {
        return false;
    }

    auto coll = Grid::get(opCtx)->catalogClient()->getCollection(opCtx, collUuid);
    while (currentPhaseCompleted()) {
        currentPhase = _transitionPhases(opCtx, coll, currentPhase->getNextPhase());
    }

    return true;
}

boost::optional<DefragmentationAction> BalancerDefragmentationPolicyImpl::_nextStreamingAction(
    OperationContext* opCtx) {
    // TODO (SERVER-61635) validate fairness through collections
    for (auto it = _defragmentationStates.begin(); it != _defragmentationStates.end();) {
        try {
            _refreshDefragmentationPhaseFor(opCtx, it->first);
            auto& currentCollectionDefragmentationState = it->second;
            if (!currentCollectionDefragmentationState) {
                it = _defragmentationStates.erase(it, std::next(it));
                continue;
            }
            // Get next action
            auto nextAction = currentCollectionDefragmentationState->popNextStreamableAction(opCtx);
            if (nextAction) {
                return nextAction;
            }
            ++it;
        } catch (DBException& e) {
            // Catch getCollection and getShardVersion errors. Should only occur if collection has
            // been removed.
            LOGV2_ERROR(6153301,
                        "Error while getting next defragmentation action",
                        "uuid"_attr = it->first,
                        "error"_attr = redact(e));
            it = _defragmentationStates.erase(it, std::next(it));
        }
    }

    boost::optional<DefragmentationAction> noAction;
    if (_streamClosed) {
        noAction = boost::optional<EndOfActionStream>();
    }
    return noAction;
}

void BalancerDefragmentationPolicyImpl::acknowledgeMergeResult(OperationContext* opCtx,
                                                               MergeInfo action,
                                                               const Status& result) {
    stdx::lock_guard<Latch> lk(_stateMutex);
    // Check if collection defragmentation has been canceled
    if (!_defragmentationStates.contains(action.uuid)) {
        return;
    }

    _defragmentationStates.at(action.uuid)->applyActionResult(opCtx, action, result);

    _processEndOfAction(lk, opCtx);
}

void BalancerDefragmentationPolicyImpl::acknowledgeDataSizeResult(
    OperationContext* opCtx, DataSizeInfo action, const StatusWith<DataSizeResponse>& result) {
    stdx::lock_guard<Latch> lk(_stateMutex);
    // Check if collection defragmentation has been canceled
    if (!_defragmentationStates.contains(action.uuid)) {
        return;
    }

    _defragmentationStates.at(action.uuid)->applyActionResult(opCtx, action, result);

    _processEndOfAction(lk, opCtx);
}

void BalancerDefragmentationPolicyImpl::acknowledgeAutoSplitVectorResult(
    OperationContext* opCtx, AutoSplitVectorInfo action, const StatusWith<SplitPoints>& result) {
    stdx::lock_guard<Latch> lk(_stateMutex);
    // Check if collection defragmentation has been canceled
    if (!_defragmentationStates.contains(action.uuid)) {
        return;
    }

    _defragmentationStates.at(action.uuid)->applyActionResult(opCtx, action, result);

    _processEndOfAction(lk, opCtx);
}

void BalancerDefragmentationPolicyImpl::acknowledgeSplitResult(OperationContext* opCtx,
                                                               SplitInfoWithKeyPattern action,
                                                               const Status& result) {
    stdx::lock_guard<Latch> lk(_stateMutex);
    // Check if collection defragmentation has been canceled
    if (!_defragmentationStates.contains(action.uuid)) {
        return;
    }

    _defragmentationStates.at(action.uuid)->applyActionResult(opCtx, action, result);

    _processEndOfAction(lk, opCtx);
}

void BalancerDefragmentationPolicyImpl::acknowledgeMoveResult(OperationContext* opCtx,
                                                              MigrateInfo action,
                                                              const Status& result) {
    stdx::lock_guard<Latch> lk(_stateMutex);
    // Check if collection defragmentation has been canceled
    if (!_defragmentationStates.contains(action.uuid)) {
        return;
    }

    _defragmentationStates.at(action.uuid)->applyActionResult(opCtx, action, result);
    _processEndOfAction(lk, opCtx);
}

void BalancerDefragmentationPolicyImpl::closeActionStream() {
    stdx::lock_guard<Latch> lk(_stateMutex);
    _defragmentationStates.clear();
    if (_nextStreamingActionPromise) {
        _nextStreamingActionPromise.get().setFrom(EndOfActionStream());
        _nextStreamingActionPromise = boost::none;
    }
    _streamClosed = true;
}

void BalancerDefragmentationPolicyImpl::_processEndOfAction(WithLock lk, OperationContext* opCtx) {
    --_concurrentStreamingOps;
    _yieldNextStreamingAction(lk, opCtx);
}

void BalancerDefragmentationPolicyImpl::_yieldNextStreamingAction(WithLock,
                                                                  OperationContext* opCtx) {
    if (_nextStreamingActionPromise) {
        auto nextStreamingAction = _nextStreamingAction(opCtx);
        if (nextStreamingAction) {
            ++_concurrentStreamingOps;
            _nextStreamingActionPromise.get().setWith([&] { return *nextStreamingAction; });
            _nextStreamingActionPromise = boost::none;
        }
    }
}

std::unique_ptr<DefragmentationPhase> BalancerDefragmentationPolicyImpl::_transitionPhases(
    OperationContext* opCtx,
    const CollectionType& coll,
    DefragmentationPhaseEnum nextPhase,
    bool shouldPersistPhase) {
    beforeTransitioningDefragmentationPhase.pauseWhileSet();
    std::unique_ptr<DefragmentationPhase> nextPhaseObject(nullptr);
    try {
        if (shouldPersistPhase) {
            _persistPhaseUpdate(opCtx, nextPhase, coll.getUuid());
        }
        switch (nextPhase) {
            case DefragmentationPhaseEnum::kMergeAndMeasureChunks:
                nextPhaseObject = MergeAndMeasureChunksPhase::build(opCtx, coll);
                break;
            case DefragmentationPhaseEnum::kMoveAndMergeChunks: {
                auto collectionShardStats =
                    uassertStatusOK(_clusterStats->getCollStats(opCtx, coll.getNss()));
                nextPhaseObject =
                    MoveAndMergeChunksPhase::build(opCtx, coll, std::move(collectionShardStats));
            } break;
            case DefragmentationPhaseEnum::kMergeChunks:
                nextPhaseObject = MergeChunksPhase::build(opCtx, coll);
                break;
            case DefragmentationPhaseEnum::kSplitChunks:
                nextPhaseObject = SplitChunksPhase::build(opCtx, coll);
                break;
            case DefragmentationPhaseEnum::kFinished:
                _clearDefragmentationState(opCtx, coll.getUuid());
                break;
        }
        afterBuildingNextDefragmentationPhase.pauseWhileSet();
        LOGV2(6172702,
              "Collection defragmentation transitioning to new phase",
              "namespace"_attr = coll.getNss(),
              "phase"_attr = nextPhaseObject
                  ? DefragmentationPhase_serializer(nextPhaseObject->getType())
                  : kNoPhase,
              "details"_attr = nextPhaseObject ? nextPhaseObject->reportProgress() : BSONObj());
    } catch (const DBException& e) {
        LOGV2_ERROR(6153101,
                    "Error while building defragmentation phase on collection",
                    "namespace"_attr = coll.getNss(),
                    "uuid"_attr = coll.getUuid(),
                    "phase"_attr = nextPhase,
                    "error"_attr = e);
    }
    return nextPhaseObject;
}

void BalancerDefragmentationPolicyImpl::_initializeCollectionState(WithLock,
                                                                   OperationContext* opCtx,
                                                                   const CollectionType& coll) {
    auto phaseToBuild = coll.getDefragmentationPhase()
        ? coll.getDefragmentationPhase().get()
        : DefragmentationPhaseEnum::kMergeAndMeasureChunks;
    auto collectionPhase = _transitionPhases(
        opCtx, coll, phaseToBuild, !coll.getDefragmentationPhase().is_initialized());
    while (collectionPhase && collectionPhase->isComplete()) {
        collectionPhase = _transitionPhases(opCtx, coll, collectionPhase->getNextPhase());
    }
    if (collectionPhase) {
        auto [_, inserted] =
            _defragmentationStates.insert_or_assign(coll.getUuid(), std::move(collectionPhase));
        dassert(inserted);
    }
}

void BalancerDefragmentationPolicyImpl::_persistPhaseUpdate(OperationContext* opCtx,
                                                            DefragmentationPhaseEnum phase,
                                                            const UUID& uuid) {
    DBDirectClient dbClient(opCtx);
    write_ops::UpdateCommandRequest updateOp(CollectionType::ConfigNS);
    updateOp.setUpdates({[&] {
        write_ops::UpdateOpEntry entry;
        entry.setQ(BSON(CollectionType::kUuidFieldName << uuid));
        entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
            BSON("$set" << BSON(CollectionType::kDefragmentationPhaseFieldName
                                << DefragmentationPhase_serializer(phase)))));
        return entry;
    }()});
    auto response = dbClient.update(updateOp);
    checkForWriteErrors(response);
    uassert(ErrorCodes::NoMatchingDocument,
            "Collection {} not found while persisting phase change"_format(uuid.toString()),
            response.getN() > 0);
    WriteConcernResult ignoreResult;
    const auto latestOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    uassertStatusOK(waitForWriteConcern(
        opCtx, latestOpTime, WriteConcerns::kMajorityWriteConcernShardingTimeout, &ignoreResult));
}

void BalancerDefragmentationPolicyImpl::_clearDefragmentationState(OperationContext* opCtx,
                                                                   const UUID& uuid) {
    DBDirectClient dbClient(opCtx);
    // Clear datasize estimates from chunks
    write_ops::UpdateCommandRequest removeDataSize(ChunkType::ConfigNS);
    removeDataSize.setUpdates({[&] {
        write_ops::UpdateOpEntry entry;
        entry.setQ(BSON(CollectionType::kUuidFieldName << uuid));
        entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
            BSON("$unset" << BSON(ChunkType::estimatedSizeBytes.name() << ""))));
        entry.setMulti(true);
        return entry;
    }()});
    checkForWriteErrors(dbClient.update(removeDataSize));
    // Clear defragmentation phase and defragmenting flag from collection
    write_ops::UpdateCommandRequest removeCollectionFlags(CollectionType::ConfigNS);
    removeCollectionFlags.setUpdates({[&] {
        write_ops::UpdateOpEntry entry;
        entry.setQ(BSON(CollectionType::kUuidFieldName << uuid));
        entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
            BSON("$unset" << BSON(CollectionType::kDefragmentCollectionFieldName
                                  << "" << CollectionType::kDefragmentationPhaseFieldName << ""))));
        return entry;
    }()});
    auto response = dbClient.update(removeCollectionFlags);
    checkForWriteErrors(response);
    WriteConcernResult ignoreResult;
    const auto latestOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    uassertStatusOK(waitForWriteConcern(
        opCtx, latestOpTime, WriteConcerns::kMajorityWriteConcernShardingTimeout, &ignoreResult));
}

}  // namespace mongo
