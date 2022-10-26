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

#include "mongo/db/s/collection_sharding_runtime.h"

#include "mongo/db/catalog_raii.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/range_deleter_service.h"
#include "mongo/db/s/sharding_runtime_d_params_gen.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/util/duration.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class UnshardedCollection : public ScopedCollectionDescription::Impl {
public:
    UnshardedCollection() = default;

    const CollectionMetadata& get() override {
        return _metadata;
    }

private:
    CollectionMetadata _metadata;
};

const auto kUnshardedCollection = std::make_shared<UnshardedCollection>();

boost::optional<ShardVersion> getOperationReceivedVersion(OperationContext* opCtx,
                                                          const NamespaceString& nss) {
    // If there is a version attached to the OperationContext, use it as the received version.
    if (OperationShardingState::isComingFromRouter(opCtx)) {
        return OperationShardingState::get(opCtx).getShardVersion(nss);
    }

    // There is no shard version information on the 'opCtx'. This means that the operation
    // represented by 'opCtx' is unversioned, and the shard version is always OK for unversioned
    // operations.
    return boost::none;
}

}  // namespace

CollectionShardingRuntime::ScopedCollectionShardingRuntime::ScopedCollectionShardingRuntime(
    ScopedCollectionShardingState&& scopedCss)
    : _scopedCss(std::move(scopedCss)) {}

CollectionShardingRuntime::CollectionShardingRuntime(
    ServiceContext* service,
    NamespaceString nss,
    std::shared_ptr<executor::TaskExecutor> rangeDeleterExecutor)
    : _serviceContext(service),
      _nss(std::move(nss)),
      _rangeDeleterExecutor(std::move(rangeDeleterExecutor)),
      _metadataType(_nss.isNamespaceAlwaysUnsharded() ? MetadataType::kUnsharded
                                                      : MetadataType::kUnknown) {}

CollectionShardingRuntime::ScopedCollectionShardingRuntime
CollectionShardingRuntime::assertCollectionLockedAndAcquire(OperationContext* opCtx,
                                                            const NamespaceString& nss,
                                                            CSRAcquisitionMode mode) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_IS));
    return ScopedCollectionShardingRuntime(
        ScopedCollectionShardingState::acquireScopedCollectionShardingState(
            opCtx, nss, mode == CSRAcquisitionMode::kShared ? MODE_IS : MODE_X));
}

ScopedCollectionFilter CollectionShardingRuntime::getOwnershipFilter(
    OperationContext* opCtx,
    OrphanCleanupPolicy orphanCleanupPolicy,
    bool supportNonVersionedOperations) {
    boost::optional<ShardVersion> optReceivedShardVersion = boost::none;
    if (!supportNonVersionedOperations) {
        optReceivedShardVersion = getOperationReceivedVersion(opCtx, _nss);
        // No operations should be calling getOwnershipFilter without a shard version
        invariant(optReceivedShardVersion,
                  "getOwnershipFilter called by operation that doesn't specify shard version");
    }

    auto metadata =
        _getMetadataWithVersionCheckAt(opCtx,
                                       repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime(),
                                       supportNonVersionedOperations);

    if (!supportNonVersionedOperations) {
        invariant(!ShardVersion::isIgnoredVersion(*optReceivedShardVersion) ||
                      !metadata->get().allowMigrations() || !metadata->get().isSharded(),
                  "For sharded collections getOwnershipFilter cannot be relied on without a valid "
                  "shard version");
    }

    return {std::move(metadata)};
}

ScopedCollectionDescription CollectionShardingRuntime::getCollectionDescription(
    OperationContext* opCtx) {
    // If the server has been started with --shardsvr, but hasn't been added to a cluster we should
    // consider all collections as unsharded
    if (!ShardingState::get(opCtx)->enabled())
        return {kUnshardedCollection};

    // Present the collection as unsharded to internal or direct commands against shards
    if (!OperationShardingState::isComingFromRouter(opCtx))
        return {kUnshardedCollection};

    auto& oss = OperationShardingState::get(opCtx);

    auto optMetadata = _getCurrentMetadataIfKnown(boost::none);
    const auto receivedShardVersion{oss.getShardVersion(_nss)};
    uassert(
        StaleConfigInfo(_nss,
                        receivedShardVersion ? *receivedShardVersion : ShardVersion::IGNORED(),
                        boost::none /* wantedVersion */,
                        ShardingState::get(_serviceContext)->shardId()),
        str::stream() << "sharding status of collection " << _nss.ns()
                      << " is not currently available for description and needs to be recovered "
                      << "from the config server",
        optMetadata);

    return {std::move(optMetadata)};
}

boost::optional<CollectionMetadata> CollectionShardingRuntime::getCurrentMetadataIfKnown() {
    auto optMetadata = _getCurrentMetadataIfKnown(boost::none);
    if (!optMetadata)
        return boost::none;
    return optMetadata->get();
}

void CollectionShardingRuntime::checkShardVersionOrThrow(OperationContext* opCtx) {
    (void)_getMetadataWithVersionCheckAt(opCtx, boost::none);
}

void CollectionShardingRuntime::enterCriticalSectionCatchUpPhase(const BSONObj& reason) {
    _critSec.enterCriticalSectionCatchUpPhase(reason);

    if (_shardVersionInRecoverOrRefresh) {
        _shardVersionInRecoverOrRefresh->cancellationSource.cancel();
    }
}

void CollectionShardingRuntime::enterCriticalSectionCommitPhase(const BSONObj& reason) {
    _critSec.enterCriticalSectionCommitPhase(reason);
}

void CollectionShardingRuntime::rollbackCriticalSectionCommitPhaseToCatchUpPhase(
    const BSONObj& reason) {
    _critSec.rollbackCriticalSectionCommitPhaseToCatchUpPhase(reason);
}

void CollectionShardingRuntime::exitCriticalSection(const BSONObj& reason) {
    _critSec.exitCriticalSection(reason);
}

void CollectionShardingRuntime::exitCriticalSectionNoChecks() {
    _critSec.exitCriticalSectionNoChecks();
}

boost::optional<SharedSemiFuture<void>> CollectionShardingRuntime::getCriticalSectionSignal(
    OperationContext* opCtx, ShardingMigrationCriticalSection::Operation op) {
    return _critSec.getSignal(op);
}

void CollectionShardingRuntime::setFilteringMetadata(OperationContext* opCtx,
                                                     CollectionMetadata newMetadata) {
    invariant(!newMetadata.isSharded() || !_nss.isNamespaceAlwaysUnsharded(),
              str::stream() << "Namespace " << _nss.ns() << " must never be sharded.");

    stdx::lock_guard lk(_metadataManagerLock);

    if (!newMetadata.isSharded()) {
        LOGV2(21917,
              "Marking collection {namespace} as unsharded",
              "Marking collection as unsharded",
              "namespace"_attr = _nss.ns());
        _metadataType = MetadataType::kUnsharded;
        _metadataManager.reset();
        ++_numMetadataManagerChanges;
        return;
    }

    _metadataType = MetadataType::kSharded;
    if (!_metadataManager || !newMetadata.uuidMatches(_metadataManager->getCollectionUuid())) {
        _metadataManager = std::make_shared<MetadataManager>(
            opCtx->getServiceContext(), _nss, _rangeDeleterExecutor, newMetadata);
        ++_numMetadataManagerChanges;
    } else {
        _metadataManager->setFilteringMetadata(std::move(newMetadata));
    }
}

void CollectionShardingRuntime::_clearFilteringMetadata(OperationContext* opCtx,
                                                        bool clearMetadataManager) {
    if (_shardVersionInRecoverOrRefresh) {
        _shardVersionInRecoverOrRefresh->cancellationSource.cancel();
    }

    stdx::lock_guard lk(_metadataManagerLock);
    if (!_nss.isNamespaceAlwaysUnsharded()) {
        LOGV2_DEBUG(4798530,
                    1,
                    "Clearing metadata for collection {namespace}",
                    "Clearing collection metadata",
                    "namespace"_attr = _nss,
                    "clearMetadataManager"_attr = clearMetadataManager);
        _metadataType = MetadataType::kUnknown;
        if (clearMetadataManager)
            _metadataManager.reset();
    }
}

void CollectionShardingRuntime::clearFilteringMetadata(OperationContext* opCtx) {
    _clearFilteringMetadata(opCtx, /* clearMetadataManager */ false);
}

void CollectionShardingRuntime::clearFilteringMetadataForDroppedCollection(
    OperationContext* opCtx) {
    _clearFilteringMetadata(opCtx, /* clearMetadataManager */ true);
}

SharedSemiFuture<void> CollectionShardingRuntime::cleanUpRange(ChunkRange const& range,
                                                               CleanWhen when) {
    if (!feature_flags::gRangeDeleterService.isEnabledAndIgnoreFCV()) {
        stdx::lock_guard lk(_metadataManagerLock);
        invariant(_metadataType == MetadataType::kSharded);
        return _metadataManager->cleanUpRange(range, when == kDelayed);
    }

    // This method must never be called if the range deleter service feature flag is enabled
    MONGO_UNREACHABLE;
}

Status CollectionShardingRuntime::waitForClean(OperationContext* opCtx,
                                               const NamespaceString& nss,
                                               const UUID& collectionUuid,
                                               ChunkRange orphanRange,
                                               Date_t deadline) {
    while (true) {
        const StatusWith<SharedSemiFuture<void>> swOrphanCleanupFuture =
            [&]() -> StatusWith<SharedSemiFuture<void>> {
            AutoGetCollection autoColl(opCtx, nss, MODE_IX);
            auto self = CollectionShardingRuntime::assertCollectionLockedAndAcquire(
                opCtx, nss, CSRAcquisitionMode::kShared);
            stdx::lock_guard lk(self->_metadataManagerLock);

            // If the metadata was reset, or the collection was dropped and recreated since the
            // metadata manager was created, return an error.
            if (self->_metadataType != MetadataType::kSharded ||
                (collectionUuid != self->_metadataManager->getCollectionUuid())) {
                return {ErrorCodes::ConflictingOperationInProgress,
                        "Collection being migrated was dropped and created or otherwise had its "
                        "metadata reset"};
            }

            if (feature_flags::gRangeDeleterService.isEnabledAndIgnoreFCV()) {
                return RangeDeleterService::get(opCtx)->getOverlappingRangeDeletionsFuture(
                    self->_metadataManager->getCollectionUuid(), orphanRange);
            } else {
                return self->_metadataManager->trackOrphanedDataCleanup(orphanRange);
            }
        }();

        if (!swOrphanCleanupFuture.isOK()) {
            return swOrphanCleanupFuture.getStatus();
        }

        auto orphanCleanupFuture = std::move(swOrphanCleanupFuture.getValue());
        if (orphanCleanupFuture.isReady()) {
            LOGV2_OPTIONS(21918,
                          {logv2::LogComponent::kShardingMigration},
                          "Finished waiting for deletion of {namespace} range {orphanRange}",
                          "Finished waiting for deletion of orphans",
                          "namespace"_attr = nss.ns(),
                          "orphanRange"_attr = redact(orphanRange.toString()));
            return Status::OK();
        }

        LOGV2_OPTIONS(21919,
                      {logv2::LogComponent::kShardingMigration},
                      "Waiting for deletion of {namespace} range {orphanRange}",
                      "Waiting for deletion of orphans",
                      "namespace"_attr = nss.ns(),
                      "orphanRange"_attr = orphanRange);
        try {
            opCtx->runWithDeadline(
                deadline, ErrorCodes::ExceededTimeLimit, [&] { orphanCleanupFuture.get(opCtx); });
        } catch (const DBException& ex) {
            auto result = ex.toStatus();
            // Swallow RangeDeletionAbandonedBecauseCollectionWithUUIDDoesNotExist error since the
            // collection could either never exist or get dropped directly from the shard after the
            // range deletion task got scheduled.
            if (result != ErrorCodes::RangeDeletionAbandonedBecauseCollectionWithUUIDDoesNotExist) {
                return result.withContext(str::stream() << "Failed to delete orphaned " << nss.ns()
                                                        << " range " << orphanRange.toString());
            }
        }
    }

    MONGO_UNREACHABLE;
}

SharedSemiFuture<void> CollectionShardingRuntime::getOngoingQueriesCompletionFuture(
    const UUID& collectionUuid, ChunkRange const& range) {
    stdx::lock_guard lk(_metadataManagerLock);

    if (!_metadataManager || _metadataManager->getCollectionUuid() != collectionUuid) {
        return SemiFuture<void>::makeReady().share();
    }
    return _metadataManager->getOngoingQueriesCompletionFuture(range);
}


std::shared_ptr<ScopedCollectionDescription::Impl>
CollectionShardingRuntime::_getCurrentMetadataIfKnown(
    const boost::optional<LogicalTime>& atClusterTime) {
    stdx::lock_guard lk(_metadataManagerLock);
    switch (_metadataType) {
        case MetadataType::kUnknown:
            // Until user collections can be sharded in serverless, the sessions collection will be
            // the only sharded collection.
            if (getGlobalReplSettings().isServerless() &&
                _nss != NamespaceString::kLogicalSessionsNamespace) {
                return kUnshardedCollection;
            }
            return nullptr;
        case MetadataType::kUnsharded:
            return kUnshardedCollection;
        case MetadataType::kSharded:
            return _metadataManager->getActiveMetadata(atClusterTime);
    };
    MONGO_UNREACHABLE;
}

std::shared_ptr<ScopedCollectionDescription::Impl>
CollectionShardingRuntime::_getMetadataWithVersionCheckAt(
    OperationContext* opCtx,
    const boost::optional<mongo::LogicalTime>& atClusterTime,
    bool supportNonVersionedOperations) {
    // If the server has been started with --shardsvr, but hasn't been added to a cluster we should
    // consider all collections as unsharded
    if (!ShardingState::get(opCtx)->enabled())
        return kUnshardedCollection;

    if (repl::ReadConcernArgs::get(opCtx).getLevel() ==
        repl::ReadConcernLevel::kAvailableReadConcern)
        return kUnshardedCollection;

    const auto optReceivedShardVersion = getOperationReceivedVersion(opCtx, _nss);
    if (!optReceivedShardVersion && !supportNonVersionedOperations)
        return kUnshardedCollection;

    // Assume that the received shard version was IGNORED if the current operation wasn't versioned
    const auto& receivedShardVersion =
        optReceivedShardVersion ? *optReceivedShardVersion : ShardVersion::IGNORED();

    {
        auto criticalSectionSignal = _critSec.getSignal(
            opCtx->lockState()->isWriteLocked() ? ShardingMigrationCriticalSection::kWrite
                                                : ShardingMigrationCriticalSection::kRead);
        std::string reason = _critSec.getReason() ? _critSec.getReason()->toString() : "unknown";
        uassert(StaleConfigInfo(_nss,
                                receivedShardVersion,
                                boost::none /* wantedVersion */,
                                ShardingState::get(opCtx)->shardId(),
                                std::move(criticalSectionSignal),
                                opCtx->lockState()->isWriteLocked()
                                    ? StaleConfigInfo::OperationType::kWrite
                                    : StaleConfigInfo::OperationType::kRead),
                str::stream() << "The critical section for " << _nss.ns()
                              << " is acquired with reason: " << reason,
                !criticalSectionSignal);
    }

    auto optCurrentMetadata = _getCurrentMetadataIfKnown(atClusterTime);
    uassert(StaleConfigInfo(_nss,
                            receivedShardVersion,
                            boost::none /* wantedVersion */,
                            ShardingState::get(opCtx)->shardId()),
            str::stream() << "sharding status of collection " << _nss.ns()
                          << " is not currently known and needs to be recovered",
            optCurrentMetadata);

    const auto& currentMetadata = optCurrentMetadata->get();

    const auto wantedPlacementVersion = currentMetadata.getShardVersion();
    const auto wantedShardVersion =
        ShardVersion(wantedPlacementVersion, boost::optional<CollectionIndexes>(boost::none));
    const ChunkVersion receivedPlacementVersion = receivedShardVersion.placementVersion();

    if (wantedPlacementVersion.isWriteCompatibleWith(receivedPlacementVersion) ||
        receivedShardVersion == ShardVersion::IGNORED())
        return optCurrentMetadata;

    StaleConfigInfo sci(
        _nss, receivedShardVersion, wantedShardVersion, ShardingState::get(opCtx)->shardId());

    uassert(std::move(sci),
            str::stream() << "timestamp mismatch detected for " << _nss.ns(),
            wantedPlacementVersion.isSameCollection(receivedPlacementVersion));

    if (!wantedPlacementVersion.isSet() && receivedPlacementVersion.isSet()) {
        uasserted(std::move(sci),
                  str::stream() << "this shard no longer contains chunks for " << _nss.ns() << ", "
                                << "the collection may have been dropped");
    }

    if (wantedPlacementVersion.isSet() && !receivedPlacementVersion.isSet()) {
        uasserted(std::move(sci),
                  str::stream() << "this shard contains chunks for " << _nss.ns() << ", "
                                << "but the client expects unsharded collection");
    }

    if (wantedPlacementVersion.majorVersion() != receivedPlacementVersion.majorVersion()) {
        // Could be > or < - wanted is > if this is the source of a migration, wanted < if this is
        // the target of a migration
        uasserted(std::move(sci), str::stream() << "version mismatch detected for " << _nss.ns());
    }

    // Those are all the reasons the versions can mismatch
    MONGO_UNREACHABLE;
}

void CollectionShardingRuntime::appendShardVersion(BSONObjBuilder* builder) {
    auto optCollDescr = getCurrentMetadataIfKnown();
    if (optCollDescr) {
        builder->appendTimestamp(_nss.ns(), optCollDescr->getShardVersion().toLong());
    }
}

size_t CollectionShardingRuntime::numberOfRangesScheduledForDeletion() const {
    stdx::lock_guard lk(_metadataManagerLock);
    if (_metadataType == MetadataType::kSharded) {
        return _metadataManager->numberOfRangesScheduledForDeletion();
    }
    return 0;
}


void CollectionShardingRuntime::setShardVersionRecoverRefreshFuture(
    SharedSemiFuture<void> future, CancellationSource cancellationSource) {
    invariant(!_shardVersionInRecoverOrRefresh);
    _shardVersionInRecoverOrRefresh.emplace(std::move(future), std::move(cancellationSource));
}

boost::optional<SharedSemiFuture<void>>
CollectionShardingRuntime::getShardVersionRecoverRefreshFuture(OperationContext* opCtx) {
    return _shardVersionInRecoverOrRefresh
        ? boost::optional<SharedSemiFuture<void>>(_shardVersionInRecoverOrRefresh->future)
        : boost::none;
}

void CollectionShardingRuntime::resetShardVersionRecoverRefreshFuture() {
    invariant(_shardVersionInRecoverOrRefresh);
    _shardVersionInRecoverOrRefresh = boost::none;
}

boost::optional<Timestamp> CollectionShardingRuntime::getIndexVersion(OperationContext* opCtx) {
    return _globalIndexesInfo ? _globalIndexesInfo->getVersion() : boost::none;
}

boost::optional<GlobalIndexesCache>& CollectionShardingRuntime::getIndexes(
    OperationContext* opCtx) {
    return _globalIndexesInfo;
}

void CollectionShardingRuntime::addIndex(OperationContext* opCtx,
                                         const IndexCatalogType& index,
                                         const Timestamp& indexVersion) {
    if (_globalIndexesInfo) {
        _globalIndexesInfo->add(index, indexVersion);
    } else {
        IndexCatalogTypeMap indexMap;
        indexMap.emplace(index.getName(), index);
        _globalIndexesInfo.emplace(indexVersion, std::move(indexMap));
    }
}

void CollectionShardingRuntime::removeIndex(OperationContext* opCtx,
                                            const std::string& name,
                                            const Timestamp& indexVersion) {
    tassert(
        7019500, "Index information does not exist on CSR", _globalIndexesInfo.is_initialized());
    _globalIndexesInfo->remove(name, indexVersion);
}

void CollectionShardingRuntime::clearIndexes(OperationContext* opCtx) {
    _globalIndexesInfo = boost::none;
}

CollectionCriticalSection::CollectionCriticalSection(OperationContext* opCtx,
                                                     NamespaceString nss,
                                                     BSONObj reason)
    : _opCtx(opCtx), _nss(std::move(nss)), _reason(std::move(reason)) {
    // This acquisition is performed with collection lock MODE_S in order to ensure that any ongoing
    // writes have completed and become visible
    AutoGetCollection autoColl(_opCtx,
                               _nss,
                               MODE_S,
                               AutoGetCollection::Options{}.deadline(
                                   _opCtx->getServiceContext()->getPreciseClockSource()->now() +
                                   Milliseconds(migrationLockAcquisitionMaxWaitMS.load())));
    auto scopedCsr = CollectionShardingRuntime::assertCollectionLockedAndAcquire(
        _opCtx, _nss, CSRAcquisitionMode::kExclusive);
    invariant(scopedCsr->getCurrentMetadataIfKnown());
    scopedCsr->enterCriticalSectionCatchUpPhase(_reason);
}

CollectionCriticalSection::~CollectionCriticalSection() {
    UninterruptibleLockGuard noInterrupt(_opCtx->lockState());
    AutoGetCollection autoColl(_opCtx, _nss, MODE_IX);
    auto scopedCsr = CollectionShardingRuntime::assertCollectionLockedAndAcquire(
        _opCtx, _nss, CSRAcquisitionMode::kExclusive);
    scopedCsr->exitCriticalSection(_reason);
}

void CollectionCriticalSection::enterCommitPhase() {
    AutoGetCollection autoColl(_opCtx,
                               _nss,
                               MODE_X,
                               AutoGetCollection::Options{}.deadline(
                                   _opCtx->getServiceContext()->getPreciseClockSource()->now() +
                                   Milliseconds(migrationLockAcquisitionMaxWaitMS.load())));
    auto scopedCsr = CollectionShardingRuntime::assertCollectionLockedAndAcquire(
        _opCtx, _nss, CSRAcquisitionMode::kExclusive);
    invariant(scopedCsr->getCurrentMetadataIfKnown());
    scopedCsr->enterCriticalSectionCommitPhase(_reason);
}

}  // namespace mongo
