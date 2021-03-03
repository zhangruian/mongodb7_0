/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

#include "mongo/db/s/resharding/resharding_recipient_service.h"

#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/migration_destination_manager.h"
#include "mongo/db/s/resharding/resharding_collection_cloner.h"
#include "mongo/db/s/resharding/resharding_data_copy_util.h"
#include "mongo/db/s/resharding/resharding_donor_recipient_common.h"
#include "mongo/db/s/resharding/resharding_donor_service.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding/resharding_txn_cloner.h"
#include "mongo/db/s/resharding/resharding_txn_cloner_progress_gen.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/s/shard_key_util.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_shard_version_helpers.h"
#include "mongo/util/future_util.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(removeRecipientDocFailpoint);

namespace {

const WriteConcernOptions kNoWaitWriteConcern{1, WriteConcernOptions::SyncMode::UNSET, Seconds(0)};

std::shared_ptr<executor::ThreadPoolTaskExecutor> makeTaskExecutor(StringData name,
                                                                   size_t maxThreads) {
    ThreadPool::Limits threadPoolLimits;
    threadPoolLimits.maxThreads = maxThreads;

    ThreadPool::Options threadPoolOptions(std::move(threadPoolLimits));
    threadPoolOptions.threadNamePrefix = name + "-";
    threadPoolOptions.poolName = name + "ThreadPool";

    auto executor = std::make_shared<executor::ThreadPoolTaskExecutor>(
        std::make_unique<ThreadPool>(std::move(threadPoolOptions)),
        executor::makeNetworkInterface(name + "Network"));

    executor->startup();
    return executor;
}

/**
 * Fulfills the promise if it is not already. Otherwise, does nothing.
 */
void ensureFulfilledPromise(WithLock lk, SharedPromise<void>& sp) {
    if (!sp.getFuture().isReady()) {
        sp.emplaceValue();
    }
}

void ensureFulfilledPromise(WithLock lk, SharedPromise<Timestamp>& sp, Timestamp ts) {
    auto future = sp.getFuture();
    if (!future.isReady()) {
        sp.emplaceValue(ts);
    } else {
        // Ensure that we would only attempt to fulfill the promise with the same Timestamp value.
        invariant(future.get() == ts);
    }
}

}  // namespace

namespace resharding {

void createTemporaryReshardingCollectionLocally(OperationContext* opCtx,
                                                const NamespaceString& originalNss,
                                                const NamespaceString& reshardingNss,
                                                const UUID& reshardingUUID,
                                                const UUID& existingUUID,
                                                Timestamp fetchTimestamp) {
    LOGV2_DEBUG(
        5002300, 1, "Creating temporary resharding collection", "originalNss"_attr = originalNss);

    auto catalogCache = Grid::get(opCtx)->catalogCache();

    // Load the original collection's options from the database's primary shard.
    auto [collOptions, uuid] = shardVersionRetry(
        opCtx,
        catalogCache,
        reshardingNss,
        "loading collection options to create temporary resharding collection"_sd,
        [&]() -> MigrationDestinationManager::CollectionOptionsAndUUID {
            auto originalCm = uassertStatusOK(
                catalogCache->getShardedCollectionRoutingInfoWithRefresh(opCtx, originalNss));
            return MigrationDestinationManager::getCollectionOptions(
                opCtx,
                NamespaceStringOrUUID(originalNss.db().toString(), existingUUID),
                originalCm.dbPrimary(),
                originalCm,
                fetchTimestamp);
        });

    // Load the original collection's indexes from the shard that owns the global minimum chunk.
    auto [indexes, idIndex] =
        shardVersionRetry(opCtx,
                          catalogCache,
                          reshardingNss,
                          "loading indexes to create temporary resharding collection"_sd,
                          [&]() -> MigrationDestinationManager::IndexesAndIdIndex {
                              auto originalCm =
                                  catalogCache->getShardedCollectionRoutingInfo(opCtx, originalNss);
                              auto indexShardId = originalCm.getMinKeyShardIdWithSimpleCollation();
                              return MigrationDestinationManager::getCollectionIndexes(
                                  opCtx,
                                  NamespaceStringOrUUID(originalNss.db().toString(), existingUUID),
                                  indexShardId,
                                  originalCm,
                                  fetchTimestamp);
                          });

    // Set the temporary resharding collection's UUID to the resharding UUID. Note that
    // BSONObj::addFields() replaces any fields that already exist.
    collOptions = collOptions.addFields(BSON("uuid" << reshardingUUID));
    CollectionOptionsAndIndexes optionsAndIndexes = {reshardingUUID, indexes, idIndex, collOptions};
    MigrationDestinationManager::cloneCollectionIndexesAndOptions(
        opCtx, reshardingNss, optionsAndIndexes);
}

std::vector<NamespaceString> ensureStashCollectionsExist(OperationContext* opCtx,
                                                         const ChunkManager& cm,
                                                         const UUID& existingUUID,
                                                         std::vector<ShardId> donorShards) {
    // Use the same collation for the stash collections as the temporary resharding collection
    auto collator = cm.getDefaultCollator();
    BSONObj collationSpec = collator ? collator->getSpec().toBSON() : BSONObj();

    std::vector<NamespaceString> stashCollections;
    stashCollections.reserve(donorShards.size());

    {
        CollectionOptions options;
        options.collation = std::move(collationSpec);
        for (const auto& donor : donorShards) {
            stashCollections.emplace_back(ReshardingOplogApplier::ensureStashCollectionExists(
                opCtx, existingUUID, donor, options));
        }
    }

    return stashCollections;
}

ReshardingDonorOplogId getFetcherIdToResumeFrom(OperationContext* opCtx,
                                                NamespaceString oplogBufferNss,
                                                Timestamp fetchTimestamp) {
    AutoGetCollection collection(opCtx, oplogBufferNss, MODE_IS);
    if (!collection) {
        return ReshardingDonorOplogId{fetchTimestamp, fetchTimestamp};
    }

    auto highestOplogBufferId = resharding::data_copy::findHighestInsertedId(opCtx, *collection);
    return highestOplogBufferId.missing()
        ? ReshardingDonorOplogId{fetchTimestamp, fetchTimestamp}
        : ReshardingDonorOplogId::parse({"resharding::getFetcherIdToResumeFrom"},
                                        highestOplogBufferId.getDocument().toBson());
}

ReshardingDonorOplogId getApplierIdToResumeFrom(OperationContext* opCtx,
                                                ReshardingSourceId sourceId,
                                                Timestamp fetchTimestamp) {
    auto applierProgress = ReshardingOplogApplier::checkStoredProgress(opCtx, sourceId);
    return !applierProgress ? ReshardingDonorOplogId{fetchTimestamp, fetchTimestamp}
                            : applierProgress->getProgress();
}

}  // namespace resharding

std::shared_ptr<repl::PrimaryOnlyService::Instance> ReshardingRecipientService::constructInstance(
    BSONObj initialState) const {
    return std::make_shared<RecipientStateMachine>(std::move(initialState));
}

ReshardingRecipientService::RecipientStateMachine::RecipientStateMachine(
    const BSONObj& recipientDoc)
    : repl::PrimaryOnlyService::TypedInstance<RecipientStateMachine>(),
      _recipientDoc(ReshardingRecipientDocument::parse(
          IDLParserErrorContext("ReshardingRecipientDocument"), recipientDoc)),
      _id(_recipientDoc.getCommonReshardingMetadata().get_id()) {}

ReshardingRecipientService::RecipientStateMachine::~RecipientStateMachine() {
    stdx::lock_guard<Latch> lg(_mutex);
    invariant(_allDonorsPreparedToDonate.getFuture().isReady());
    invariant(_coordinatorHasDecisionPersisted.getFuture().isReady());
    invariant(_completionPromise.getFuture().isReady());
}

SemiFuture<void> ReshardingRecipientService::RecipientStateMachine::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancelationToken& cancelToken) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, executor] {
            _metrics()->onStart();
            return _awaitAllDonorsPreparedToDonateThenTransitionToCreatingCollection(executor);
        })
        .then([this] { _createTemporaryReshardingCollectionThenTransitionToCloning(); })
        .then([this, executor, cancelToken] {
            return _cloneThenTransitionToApplying(executor, cancelToken);
        })
        .then([this] { return _applyThenTransitionToSteadyState(); })
        .then([this, executor] {
            return _awaitAllDonorsBlockingWritesThenTransitionToStrictConsistency(executor);
        })
        .then([this, executor] {
            return _awaitCoordinatorHasDecisionPersistedThenTransitionToRenaming(executor);
        })
        .then([this] { _renameTemporaryReshardingCollection(); })
        .onError([this](Status status) {
            LOGV2(4956500,
                  "Resharding operation recipient state machine failed",
                  "namespace"_attr = _recipientDoc.getNss().ns(),
                  "reshardingId"_attr = _id,
                  "error"_attr = status);
            _transitionState(RecipientStateEnum::kError, boost::none, status);
            _updateCoordinator();

            // TODO SERVER-52838: Ensure all local collections that may have been created for
            // resharding are removed, with the exception of the ReshardingRecipientDocument, before
            // transitioning to kDone.
            _transitionState(RecipientStateEnum::kDone, boost::none, status);
            _updateCoordinator();
            return status;
        })
        .onCompletion([this, self = shared_from_this()](Status status) {
            {
                stdx::lock_guard<Latch> lg(_mutex);
                if (_completionPromise.getFuture().isReady()) {
                    // interrupt() was called before we got here.
                    _metrics()->onCompletion(ReshardingMetrics::OperationStatus::kCanceled);
                    return;
                }
            }

            if (status.isOK()) {
                // The shared_ptr stored in the PrimaryOnlyService's map for the
                // ReshardingRecipientService Instance is removed when the recipient state document
                // tied to the instance is deleted. It is necessary to use shared_from_this() to
                // extend the lifetime so the code can safely finish executing.

                {
                    auto opCtx = cc().makeOperationContext();
                    removeRecipientDocFailpoint.pauseWhileSet(opCtx.get());
                }

                _removeRecipientDocument();
                _metrics()->onCompletion(ReshardingMetrics::OperationStatus::kSucceeded);
                stdx::lock_guard<Latch> lg(_mutex);
                if (!_completionPromise.getFuture().isReady()) {
                    _completionPromise.emplaceValue();
                }
            } else {
                _metrics()->onCompletion(ErrorCodes::isCancelationError(status)
                                             ? ReshardingMetrics::OperationStatus::kCanceled
                                             : ReshardingMetrics::OperationStatus::kFailed);
                stdx::lock_guard<Latch> lg(_mutex);
                if (!_completionPromise.getFuture().isReady()) {
                    _completionPromise.setError(status);
                }
            }
        })
        .semi();
}

void ReshardingRecipientService::RecipientStateMachine::interrupt(Status status) {
    // Resolve any unresolved promises to avoid hanging.
    stdx::lock_guard<Latch> lk(_mutex);
    _onAbortOrStepdown(lk, status);

    if (!_completionPromise.getFuture().isReady()) {
        _completionPromise.setError(status);
    }
}

boost::optional<BSONObj> ReshardingRecipientService::RecipientStateMachine::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode,
    MongoProcessInterface::CurrentOpSessionsMode) noexcept {
    ReshardingMetrics::ReporterOptions options(ReshardingMetrics::ReporterOptions::Role::kRecipient,
                                               _id,
                                               _recipientDoc.getNss(),
                                               _recipientDoc.getReshardingKey().toBSON(),
                                               false);
    return _metrics()->reportForCurrentOp(options);
}

void ReshardingRecipientService::RecipientStateMachine::onReshardingFieldsChanges(
    OperationContext* opCtx, const TypeCollectionReshardingFields& reshardingFields) {
    stdx::lock_guard<Latch> lk(_mutex);
    if (reshardingFields.getAbortReason()) {
        auto status = getStatusFromAbortReason(reshardingFields);
        _onAbortOrStepdown(lk, status);
        return;
    }

    auto coordinatorState = reshardingFields.getState();

    if (coordinatorState >= CoordinatorStateEnum::kCloning) {
        auto fetchTimestamp = reshardingFields.getRecipientFields()->getFetchTimestamp();
        invariant(fetchTimestamp);
        ensureFulfilledPromise(lk, _allDonorsPreparedToDonate, *fetchTimestamp);
    }

    if (coordinatorState >= CoordinatorStateEnum::kDecisionPersisted) {
        ensureFulfilledPromise(lk, _coordinatorHasDecisionPersisted);
    }
}

ExecutorFuture<void> ReshardingRecipientService::RecipientStateMachine::
    _awaitAllDonorsPreparedToDonateThenTransitionToCreatingCollection(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_recipientDoc.getState() > RecipientStateEnum::kAwaitingFetchTimestamp) {
        invariant(_recipientDoc.getFetchTimestamp());
        return ExecutorFuture(**executor);
    }

    return _allDonorsPreparedToDonate.getFuture()
        .thenRunOn(**executor)
        .then([this](Timestamp fetchTimestamp) {
            _transitionState(RecipientStateEnum::kCreatingCollection, fetchTimestamp);
        });
}

void ReshardingRecipientService::RecipientStateMachine::
    _createTemporaryReshardingCollectionThenTransitionToCloning() {
    if (_recipientDoc.getState() > RecipientStateEnum::kCreatingCollection) {
        return;
    }

    {
        auto opCtx = cc().makeOperationContext();
        auto tempNss = constructTemporaryReshardingNss(_recipientDoc.getNss().db(),
                                                       _recipientDoc.getExistingUUID());

        resharding::createTemporaryReshardingCollectionLocally(opCtx.get(),
                                                               _recipientDoc.getNss(),
                                                               tempNss,
                                                               _recipientDoc.get_id(),
                                                               _recipientDoc.getExistingUUID(),
                                                               *_recipientDoc.getFetchTimestamp());

        ShardKeyPattern shardKeyPattern(_recipientDoc.getReshardingKey());

        auto catalogCache = Grid::get(opCtx.get())->catalogCache();
        shardVersionRetry(opCtx.get(),
                          catalogCache,
                          tempNss,
                          "validating shard key index for reshardCollection"_sd,
                          [&] {
                              shardkeyutil::validateShardKeyIndexExistsOrCreateIfPossible(
                                  opCtx.get(),
                                  tempNss,
                                  shardKeyPattern.toBSON(),
                                  shardKeyPattern,
                                  CollationSpec::kSimpleSpec,
                                  false,
                                  shardkeyutil::ValidationBehaviorsShardCollection(opCtx.get()));
                          });
    }

    _transitionState(RecipientStateEnum::kCloning);
}

void ReshardingRecipientService::RecipientStateMachine::_initTxnCloner(
    OperationContext* opCtx, const Timestamp& fetchTimestamp) {
    auto catalogCache = Grid::get(opCtx)->catalogCache();
    auto routingInfo = catalogCache->getShardedCollectionRoutingInfo(opCtx, _recipientDoc.getNss());
    std::set<ShardId> shardList;

    const auto myShardId = ShardingState::get(opCtx)->shardId();
    routingInfo.getAllShardIds(&shardList);
    shardList.erase(myShardId);

    for (const auto& shard : shardList) {
        _txnCloners.push_back(
            std::make_unique<ReshardingTxnCloner>(ReshardingSourceId(_id, shard), fetchTimestamp));
    }
}

ExecutorFuture<void>
ReshardingRecipientService::RecipientStateMachine::_cloneThenTransitionToApplying(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancelationToken& cancelToken) {
    if (_recipientDoc.getState() > RecipientStateEnum::kCloning) {
        return ExecutorFuture(**executor);
    }

    auto* serviceContext = Client::getCurrent()->getServiceContext();
    auto fetchTimestamp = *_recipientDoc.getFetchTimestamp();
    auto tempNss = constructTemporaryReshardingNss(_recipientDoc.getNss().db(),
                                                   _recipientDoc.getExistingUUID());

    _collectionCloner = std::make_unique<ReshardingCollectionCloner>(
        std::make_unique<ReshardingCollectionCloner::Env>(_metrics()),
        ShardKeyPattern(_recipientDoc.getReshardingKey()),
        _recipientDoc.getNss(),
        _recipientDoc.getExistingUUID(),
        ShardingState::get(serviceContext)->shardId(),
        fetchTimestamp,
        std::move(tempNss));

    {
        auto scopedOpCtx = cc().makeOperationContext();
        auto opCtx = scopedOpCtx.get();

        _initTxnCloner(opCtx, *_recipientDoc.getFetchTimestamp());
    }

    auto numDonors = _recipientDoc.getDonorShards().size();
    _oplogFetchers.reserve(numDonors);
    _oplogFetcherFutures.reserve(numDonors);

    {
        stdx::lock_guard<Latch> lk(_mutex);
        _oplogFetcherExecutor = makeTaskExecutor("ReshardingOplogFetcher"_sd, numDonors);
    }

    const auto& recipientId = ShardingState::get(serviceContext)->shardId();
    for (const auto& donor : _recipientDoc.getDonorShards()) {
        auto oplogBufferNss = getLocalOplogBufferNamespace(_recipientDoc.getExistingUUID(), donor);
        auto opCtx = cc().makeOperationContext();
        auto idToResumeFrom =
            resharding::getFetcherIdToResumeFrom(opCtx.get(), oplogBufferNss, fetchTimestamp);
        invariant((idToResumeFrom >= ReshardingDonorOplogId{fetchTimestamp, fetchTimestamp}));

        stdx::lock_guard<Latch> lk(_mutex);
        _oplogFetchers.emplace_back(std::make_unique<ReshardingOplogFetcher>(
            std::make_unique<ReshardingOplogFetcher::Env>(getGlobalServiceContext(), _metrics()),
            _recipientDoc.get_id(),
            _recipientDoc.getExistingUUID(),
            // The recipient fetches oplog entries from the donor starting from the largest _id
            // value in the oplog buffer. Otherwise, it starts at fetchTimestamp, which corresponds
            // to {clusterTime: fetchTimestamp, ts: fetchTimestamp} as a resume token value.
            std::move(idToResumeFrom),
            donor,
            recipientId,
            std::move(oplogBufferNss)));

        _oplogFetcherFutures.emplace_back(
            _oplogFetchers.back()
                ->schedule(_oplogFetcherExecutor, cancelToken)
                .onError([](Status status) {
                    LOGV2(5259300, "Error fetching oplog entries", "error"_attr = redact(status));
                    return status;
                }));
    }

    return _collectionCloner->run(**executor, cancelToken)
        .then([this, executor, cancelToken] {
            if (_txnCloners.empty()) {
                return SemiFuture<void>::makeReady();
            }

            auto serviceContext = Client::getCurrent()->getServiceContext();

            std::vector<ExecutorFuture<void>> txnClonerFutures;
            for (auto&& txnCloner : _txnCloners) {
                txnClonerFutures.push_back(txnCloner->run(serviceContext, **executor, cancelToken));
            }

            return whenAllSucceed(std::move(txnClonerFutures));
        })
        .then([this] {
            // ReshardingTxnCloners must complete before the recipient transitions to kApplying to
            // avoid errors caused by donor shards unpinning the fetchTimestamp.
            _transitionState(RecipientStateEnum::kApplying);
            _updateCoordinator();
        });
}

void ReshardingRecipientService::RecipientStateMachine::_applyThenTransitionToSteadyState() {
    if (_recipientDoc.getState() > RecipientStateEnum::kApplying) {
        return;
    }

    // The contents of the temporary resharding collection are already consistent because the
    // ReshardingCollectionCloner uses atClusterTime. Using replication's initial sync nomenclature,
    // resharding has immediately finished the "apply phase" as soon as the
    // ReshardingCollectionCloner has finished. This is why it is acceptable to not call
    // applyUntilCloneFinishedTs() here and to only do so in
    // _awaitAllDonorsBlockingWritesThenTransitionToStrictConsistency() instead.
    //
    // TODO: Consider removing _applyThenTransitionToSteadyState() and changing
    // _cloneThenTransitionToApplying() to call _transitionState/_updateCoordinator(kSteadyState).

    _transitionState(RecipientStateEnum::kSteadyState);
    _updateCoordinator();
}

ExecutorFuture<void> ReshardingRecipientService::RecipientStateMachine::
    _awaitAllDonorsBlockingWritesThenTransitionToStrictConsistency(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_recipientDoc.getState() > RecipientStateEnum::kSteadyState) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    auto numDonors = _recipientDoc.getDonorShards().size();
    _oplogAppliers.reserve(numDonors);
    _oplogApplierWorkers.reserve(numDonors);

    const auto& sourceChunkMgr = [&] {
        auto opCtx = cc().makeOperationContext();
        auto catalogCache = Grid::get(opCtx.get())->catalogCache();
        return catalogCache->getShardedCollectionRoutingInfo(opCtx.get(), _recipientDoc.getNss());
    }();

    auto stashCollections = [&] {
        auto opCtx = cc().makeOperationContext();
        return resharding::ensureStashCollectionsExist(opCtx.get(),
                                                       sourceChunkMgr,
                                                       _recipientDoc.getExistingUUID(),
                                                       _recipientDoc.getDonorShards());
    }();

    auto futuresToWaitOn = std::move(_oplogFetcherFutures);
    for (size_t donorIdx = 0; donorIdx < _recipientDoc.getDonorShards().size(); ++donorIdx) {
        const auto& donor = _recipientDoc.getDonorShards()[donorIdx];
        {
            stdx::lock_guard<Latch> lk(_mutex);
            _oplogApplierWorkers.emplace_back(
                repl::makeReplWriterPool(resharding::gReshardingWriterThreadCount,
                                         "ReshardingOplogApplierWorker",
                                         true /* isKillableByStepdown */));
        }

        auto sourceId = ReshardingSourceId{_recipientDoc.get_id(), donor};
        const auto& oplogBufferNss =
            getLocalOplogBufferNamespace(_recipientDoc.getExistingUUID(), donor);
        auto fetchTimestamp = *_recipientDoc.getFetchTimestamp();
        auto idToResumeFrom = [&] {
            auto opCtx = cc().makeOperationContext();
            return resharding::getApplierIdToResumeFrom(opCtx.get(), sourceId, fetchTimestamp);
        }();
        invariant((idToResumeFrom >= ReshardingDonorOplogId{fetchTimestamp, fetchTimestamp}));

        _oplogAppliers.emplace_back(std::make_unique<ReshardingOplogApplier>(
            std::make_unique<ReshardingOplogApplier::Env>(Client::getCurrent()->getServiceContext(),
                                                          _metrics()),
            std::move(sourceId),
            oplogBufferNss,
            _recipientDoc.getNss(),
            _recipientDoc.getExistingUUID(),
            stashCollections,
            donorIdx,
            fetchTimestamp,
            // The recipient applies oplog entries from the donor starting from the progress value
            // in progress_applier. Otherwise, it starts at fetchTimestamp, which corresponds to
            // {clusterTime: fetchTimestamp, ts: fetchTimestamp} as a resume token value.
            std::make_unique<ReshardingDonorOplogIterator>(
                oplogBufferNss, std::move(idToResumeFrom), _oplogFetchers[donorIdx].get()),
            sourceChunkMgr,
            **executor,
            _oplogApplierWorkers.back().get()));

        // The contents of the temporary resharding collection are already consistent because the
        // ReshardingCollectionCloner uses atClusterTime. Using replication's initial sync
        // nomenclature, resharding has immediately finished the "apply phase" as soon as the
        // ReshardingCollectionCloner has finished. This is why applyUntilCloneFinishedTs() and
        // applyUntilDone() are both called here in sequence.
        auto* applier = _oplogAppliers.back().get();
        futuresToWaitOn.emplace_back(applier->applyUntilCloneFinishedTs().then(
            [applier] { return applier->applyUntilDone(); }));
    }

    return whenAllSucceed(std::move(futuresToWaitOn))
        .thenRunOn(**executor)
        .then([this, stashCollections] {
            auto opCtxRaii = cc().makeOperationContext();

            for (auto&& stashNss : stashCollections) {
                AutoGetCollection autoCollOutput(opCtxRaii.get(), stashNss, MODE_IS);
                uassert(5356800,
                        "Resharding completed with non-empty stash collections",
                        autoCollOutput->isEmpty(opCtxRaii.get()));
            }
        })
        .then([this] {
            _transitionState(RecipientStateEnum::kStrictConsistency);

            bool isDonor = [& id = _recipientDoc.get_id()] {
                auto opCtx = cc().makeOperationContext();
                auto instance = resharding::tryGetReshardingStateMachine<
                    ReshardingDonorService,
                    ReshardingDonorService::DonorStateMachine,
                    ReshardingDonorDocument>(opCtx.get(), id);

                return !!instance;
            }();

            if (!isDonor) {
                _critSec.emplace(cc().getServiceContext(), _recipientDoc.getNss());
            }

            _updateCoordinator();
        });
}

ExecutorFuture<void> ReshardingRecipientService::RecipientStateMachine::
    _awaitCoordinatorHasDecisionPersistedThenTransitionToRenaming(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_recipientDoc.getState() > RecipientStateEnum::kStrictConsistency) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _coordinatorHasDecisionPersisted.getFuture().thenRunOn(**executor).then([this]() {
        _transitionState(RecipientStateEnum::kRenaming);
    });
}

void ReshardingRecipientService::RecipientStateMachine::_renameTemporaryReshardingCollection() {
    if (_recipientDoc.getState() > RecipientStateEnum::kRenaming) {
        return;
    }

    {
        auto opCtx = cc().makeOperationContext();

        auto reshardingNss = constructTemporaryReshardingNss(_recipientDoc.getNss().db(),
                                                             _recipientDoc.getExistingUUID());

        RenameCollectionOptions options;
        options.dropTarget = true;
        uassertStatusOK(
            renameCollection(opCtx.get(), reshardingNss, _recipientDoc.getNss(), options));

        _dropOplogCollections(opCtx.get());

        _critSec.reset();
    }

    _transitionState(RecipientStateEnum::kDone);
    _updateCoordinator();
}

void ReshardingRecipientService::RecipientStateMachine::_transitionState(
    RecipientStateEnum endState,
    boost::optional<Timestamp> fetchTimestamp,
    boost::optional<Status> abortReason) {
    invariant(endState != RecipientStateEnum::kAwaitingFetchTimestamp);
    ReshardingRecipientDocument replacementDoc(_recipientDoc);
    replacementDoc.setState(endState);

    emplaceFetchTimestampIfExists(replacementDoc, std::move(fetchTimestamp));
    emplaceAbortReasonIfExists(replacementDoc, std::move(abortReason));

    // For logging purposes.
    auto oldState = _recipientDoc.getState();
    auto newState = replacementDoc.getState();

    _updateRecipientDocument(std::move(replacementDoc));
    _metrics()->setRecipientState(endState);

    LOGV2_INFO(5279506,
               "Transitioned resharding recipient state",
               "newState"_attr = RecipientState_serializer(newState),
               "oldState"_attr = RecipientState_serializer(oldState),
               "ns"_attr = _recipientDoc.getNss(),
               "collectionUUID"_attr = _recipientDoc.getExistingUUID(),
               "reshardingUUID"_attr = _recipientDoc.get_id());
}

void ReshardingRecipientService::RecipientStateMachine::_updateCoordinator() {

    auto opCtx = cc().makeOperationContext();

    auto shardId = ShardingState::get(opCtx.get())->shardId();

    BSONObjBuilder updateBuilder;
    updateBuilder.append("recipientShards.$.state",
                         RecipientState_serializer(_recipientDoc.getState()));
    if (_recipientDoc.getAbortReason()) {
        updateBuilder.append("recipientShards.$.abortReason", _recipientDoc.getAbortReason().get());
    }

    uassertStatusOK(
        Grid::get(opCtx.get())
            ->catalogClient()
            ->updateConfigDocument(
                opCtx.get(),
                NamespaceString::kConfigReshardingOperationsNamespace,
                BSON("_id" << _recipientDoc.get_id() << "recipientShards.id" << shardId),
                BSON("$set" << updateBuilder.done()),
                false /* upsert */,
                ShardingCatalogClient::kMajorityWriteConcern));
}

void ReshardingRecipientService::RecipientStateMachine::insertStateDocument(
    OperationContext* opCtx, const ReshardingRecipientDocument& recipientDoc) {
    PersistentTaskStore<ReshardingRecipientDocument> store(
        NamespaceString::kRecipientReshardingOperationsNamespace);
    store.add(opCtx, recipientDoc, kNoWaitWriteConcern);
}

void ReshardingRecipientService::RecipientStateMachine::_updateRecipientDocument(
    ReshardingRecipientDocument&& replacementDoc) {
    auto opCtx = cc().makeOperationContext();
    PersistentTaskStore<ReshardingRecipientDocument> store(
        NamespaceString::kRecipientReshardingOperationsNamespace);
    store.update(opCtx.get(),
                 BSON(ReshardingRecipientDocument::k_idFieldName << _id),
                 replacementDoc.toBSON(),
                 WriteConcerns::kMajorityWriteConcern);

    _recipientDoc = replacementDoc;
}

void ReshardingRecipientService::RecipientStateMachine::_removeRecipientDocument() {
    auto opCtx = cc().makeOperationContext();
    PersistentTaskStore<ReshardingRecipientDocument> store(
        NamespaceString::kRecipientReshardingOperationsNamespace);
    store.remove(opCtx.get(),
                 BSON(ReshardingRecipientDocument::k_idFieldName << _id),
                 WriteConcerns::kMajorityWriteConcern);
    _recipientDoc = {};
}

void ReshardingRecipientService::RecipientStateMachine::_dropOplogCollections(
    OperationContext* opCtx) {
    for (const auto& donor : _recipientDoc.getDonorShards()) {
        auto reshardingSourceId = ReshardingSourceId{_recipientDoc.get_id(), donor};

        // Remove the oplog applier progress doc for this donor.
        PersistentTaskStore<ReshardingOplogApplierProgress> oplogApplierProgressStore(
            NamespaceString::kReshardingApplierProgressNamespace);
        oplogApplierProgressStore.remove(
            opCtx,
            QUERY(ReshardingOplogApplierProgress::kOplogSourceIdFieldName
                  << reshardingSourceId.toBSON()),
            WriteConcernOptions());

        // Remove the txn cloner progress doc for this donor.
        PersistentTaskStore<ReshardingTxnClonerProgress> txnClonerProgressStore(
            NamespaceString::kReshardingTxnClonerProgressNamespace);
        txnClonerProgressStore.remove(
            opCtx,
            QUERY(ReshardingTxnClonerProgress::kSourceIdFieldName << reshardingSourceId.toBSON()),
            WriteConcernOptions());

        // Drop the conflict stash collection for this donor.
        auto stashNss = getLocalConflictStashNamespace(_recipientDoc.getExistingUUID(), donor);
        resharding::data_copy::ensureCollectionDropped(opCtx, stashNss);

        // Drop the oplog buffer collection for this donor.
        auto oplogBufferNss = getLocalOplogBufferNamespace(_recipientDoc.getExistingUUID(), donor);
        resharding::data_copy::ensureCollectionDropped(opCtx, oplogBufferNss);
    }
}

ReshardingMetrics* ReshardingRecipientService::RecipientStateMachine::_metrics() const {
    return ReshardingMetrics::get(cc().getServiceContext());
}

void ReshardingRecipientService::RecipientStateMachine::_onAbortOrStepdown(WithLock,
                                                                           Status status) {
    if (_oplogFetcherExecutor) {
        _oplogFetcherExecutor->shutdown();
    }

    for (auto&& fetcher : _oplogFetchers) {
        fetcher->interrupt(status);
    }

    for (auto&& threadPool : _oplogApplierWorkers) {
        threadPool->shutdown();
    }

    if (!_allDonorsPreparedToDonate.getFuture().isReady()) {
        _allDonorsPreparedToDonate.setError(status);
    }

    if (!_coordinatorHasDecisionPersisted.getFuture().isReady()) {
        _coordinatorHasDecisionPersisted.setError(status);
    }
}

}  // namespace mongo
