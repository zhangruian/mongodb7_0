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

#include <algorithm>

#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/migration_destination_manager.h"
#include "mongo/db/s/recoverable_critical_section_service.h"
#include "mongo/db/s/resharding/resharding_data_copy_util.h"
#include "mongo/db/s/resharding/resharding_future_util.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_oplog_applier.h"
#include "mongo/db/s/resharding/resharding_recipient_service_external_state.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/s/shard_key_util.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_shard_version_helpers.h"
#include "mongo/util/future_util.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(removeRecipientDocFailpoint);
MONGO_FAIL_POINT_DEFINE(reshardingPauseRecipientBeforeCloning);
MONGO_FAIL_POINT_DEFINE(reshardingPauseRecipientDuringCloning);
MONGO_FAIL_POINT_DEFINE(reshardingPauseRecipientDuringOplogApplication);

namespace {

const WriteConcernOptions kNoWaitWriteConcern{1, WriteConcernOptions::SyncMode::UNSET, Seconds(0)};

/**
 * Fulfills the promise if it is not already. Otherwise, does nothing.
 */
void ensureFulfilledPromise(WithLock lk, SharedPromise<void>& sp) {
    if (!sp.getFuture().isReady()) {
        sp.emplaceValue();
    }
}

void ensureFulfilledPromise(WithLock lk, SharedPromise<void>& sp, Status error) {
    if (!sp.getFuture().isReady()) {
        sp.setError(error);
    }
}

template <class T>
void ensureFulfilledPromise(WithLock lk, SharedPromise<T>& sp, T value) {
    auto future = sp.getFuture();
    if (!future.isReady()) {
        sp.emplaceValue(std::move(value));
    } else {
        // Ensure that we would only attempt to fulfill the promise with the same value.
        invariant(future.get() == value);
    }
}

}  // namespace

ThreadPool::Limits ReshardingRecipientService::getThreadPoolLimits() const {
    ThreadPool::Limits threadPoolLimit;
    threadPoolLimit.maxThreads = resharding::gReshardingRecipientServiceMaxThreadCount;
    return threadPoolLimit;
}

std::shared_ptr<repl::PrimaryOnlyService::Instance> ReshardingRecipientService::constructInstance(
    BSONObj initialState) {
    return std::make_shared<RecipientStateMachine>(
        this,
        ReshardingRecipientDocument::parse({"RecipientStateMachine"}, initialState),
        std::make_unique<RecipientStateMachineExternalStateImpl>(),
        ReshardingDataReplication::make);
}

ReshardingRecipientService::RecipientStateMachine::RecipientStateMachine(
    const ReshardingRecipientService* recipientService,
    const ReshardingRecipientDocument& recipientDoc,
    std::unique_ptr<RecipientStateMachineExternalState> externalState,
    ReshardingDataReplicationFactory dataReplicationFactory)
    : repl::PrimaryOnlyService::TypedInstance<RecipientStateMachine>(),
      _recipientService{recipientService},
      _metadata{recipientDoc.getCommonReshardingMetadata()},
      _minimumOperationDuration{Milliseconds{recipientDoc.getMinimumOperationDurationMillis()}},
      _recipientCtx{recipientDoc.getMutableState()},
      _donorShards{recipientDoc.getDonorShards()},
      _cloneTimestamp{recipientDoc.getCloneTimestamp()},
      _externalState{std::move(externalState)},
      _startConfigTxnCloneAt{recipientDoc.getStartConfigTxnCloneTime()},
      _markKilledExecutor(std::make_shared<ThreadPool>([] {
          ThreadPool::Options options;
          options.poolName = "RecipientStateMachineCancelableOpCtxPool";
          options.minThreads = 1;
          options.maxThreads = 1;
          return options;
      }())),
      _dataReplicationFactory{std::move(dataReplicationFactory)},
      _critSecReason(BSON("command"
                          << "resharding_recipient"
                          << "collection" << _metadata.getSourceNss().toString())),
      _isAlsoDonor([&]() {
          auto myShardId = _externalState->myShardId(getGlobalServiceContext());
          return std::find_if(_donorShards.begin(),
                              _donorShards.end(),
                              [&](const DonorShardFetchTimestamp& donor) {
                                  return donor.getShardId() == myShardId;
                              }) != _donorShards.end();
      }()) {
    invariant(_externalState);
}

ReshardingRecipientService::RecipientStateMachine::~RecipientStateMachine() {
    stdx::lock_guard<Latch> lg(_mutex);
    invariant(_completionPromise.getFuture().isReady());
}

ExecutorFuture<void>
ReshardingRecipientService::RecipientStateMachine::_runUntilStrictConsistencyOrErrored(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& abortToken) noexcept {
    return resharding::WithAutomaticRetry([this, executor, abortToken] {
               return ExecutorFuture(**executor)
                   .then([this, executor, abortToken] {
                       return _awaitAllDonorsPreparedToDonateThenTransitionToCreatingCollection(
                           executor, abortToken);
                   })
                   .then([this] { _createTemporaryReshardingCollectionThenTransitionToCloning(); })
                   .then([this, executor, abortToken] {
                       return _cloneThenTransitionToApplying(executor, abortToken);
                   })
                   .then([this, executor, abortToken] {
                       return _applyThenTransitionToSteadyState(executor, abortToken);
                   })
                   .then([this, executor, abortToken] {
                       return _awaitAllDonorsBlockingWritesThenTransitionToStrictConsistency(
                           executor, abortToken);
                   });
           })
        .onTransientError([](const Status& status) {
            LOGV2(5551100,
                  "Recipient _runUntilStrictConsistencyOrErrored encountered transient error",
                  "error"_attr = status);
        })
        .onUnrecoverableError([](const Status& status) {})
        .until([abortToken](const Status& status) { return status.isOK(); })
        .on(**executor, abortToken)
        .onError([this, executor, abortToken](Status status) {
            if (abortToken.isCanceled()) {
                return ExecutorFuture<void>(**executor, status);
            }

            LOGV2(4956500,
                  "Resharding operation recipient state machine failed",
                  "namespace"_attr = _metadata.getSourceNss(),
                  "reshardingUUID"_attr = _metadata.getReshardingUUID(),
                  "error"_attr = status);

            return resharding::WithAutomaticRetry([this, status] {
                       // It is illegal to transition into kError if the state has already surpassed
                       // kStrictConsistency.
                       invariant(_recipientCtx.getState() < RecipientStateEnum::kStrictConsistency);
                       _transitionToError(status);

                       // Intentionally swallow the error - by transitioning to kError, the
                       // recipient effectively recovers from encountering the error and
                       // should continue running in the future chain.
                   })
                .onTransientError([](const Status& status) {
                    LOGV2(5551104,
                          "Recipient _runUntilStrictConsistencyOrErrored encountered transient "
                          "error while transitioning to state kError",
                          "error"_attr = status);
                })
                .onUnrecoverableError([](const Status& status) {})
                .until([](const Status& retryStatus) { return retryStatus.isOK(); })
                .on(**executor, abortToken);
        });
}

ExecutorFuture<void>
ReshardingRecipientService::RecipientStateMachine::_notifyCoordinatorAndAwaitDecision(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& abortToken) noexcept {
    if (_recipientCtx.getState() > RecipientStateEnum::kStrictConsistency) {
        // The recipient has progressed past the point where it needs to update the coordinator in
        // order for the coordinator to make its decision.
        return ExecutorFuture(**executor);
    }

    return resharding::WithAutomaticRetry([this, executor] {
               auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
               return _updateCoordinator(opCtx.get(), executor);
           })
        .onTransientError([](const Status& status) {
            LOGV2(5551102,
                  "Transient error while notifying coordinator of recipient state for the "
                  "coordinator's decision",
                  "error"_attr = status);
        })
        .onUnrecoverableError([](const Status& status) {})
        .until([](const Status& status) { return status.isOK(); })
        .on(**executor, abortToken)
        .then([this, abortToken] {
            return future_util::withCancellation(_coordinatorHasDecisionPersisted.getFuture(),
                                                 abortToken);
        });
}

ExecutorFuture<void> ReshardingRecipientService::RecipientStateMachine::_finishReshardingOperation(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& stepdownToken,
    bool aborted) noexcept {
    return resharding::WithAutomaticRetry([this, executor, aborted, stepdownToken] {
               return ExecutorFuture<void>(**executor)
                   .then([this, executor, aborted, stepdownToken] {
                       if (aborted) {
                           return future_util::withCancellation(
                                      _dataReplicationQuiesced.thenRunOn(**executor), stepdownToken)
                               .thenRunOn(**executor)
                               .onError([](Status status) {
                                   // Wait for all of the data replication components to halt. We
                                   // ignore any errors because resharding is known to have failed
                                   // already.
                                   return Status::OK();
                               });
                       } else {
                           _renameTemporaryReshardingCollection();
                           return ExecutorFuture<void>(**executor, Status::OK());
                       }
                   })
                   .then([this, aborted] {
                       // It is safe to drop the oplog collections once either (1) the
                       // collection is renamed or (2) the operation is aborting.
                       invariant(_recipientCtx.getState() >= RecipientStateEnum::kRenaming ||
                                 aborted);
                       _cleanupReshardingCollections(aborted);
                   })
                   .then([this, aborted] {
                       if (_recipientCtx.getState() != RecipientStateEnum::kDone) {
                           // If a failover occured before removing the recipient document, the
                           // recipient could already be in state done.
                           _transitionState(RecipientStateEnum::kDone);
                       }

                       if (!aborted && !_isAlsoDonor) {
                           // An aborted operation will already have released the critical section.
                           auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                           RecoverableCriticalSectionService::get(opCtx.get())
                               ->releaseRecoverableCriticalSection(
                                   opCtx.get(),
                                   _metadata.getSourceNss(),
                                   _critSecReason,
                                   ShardingCatalogClient::kLocalWriteConcern);
                       }
                   })
                   .then([this, executor] {
                       auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                       return _updateCoordinator(opCtx.get(), executor);
                   })
                   .then([this] {
                       {
                           auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                           removeRecipientDocFailpoint.pauseWhileSet(opCtx.get());
                       }
                       _removeRecipientDocument();
                   });
           })
        .onTransientError([](const Status& status) {
            LOGV2(5551103,
                  "Transient error while finishing resharding operation",
                  "error"_attr = status);
        })
        .onUnrecoverableError([](const Status& status) {})
        .until([](const Status& status) { return status.isOK(); })
        .on(**executor, stepdownToken);
}

SemiFuture<void> ReshardingRecipientService::RecipientStateMachine::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& stepdownToken) noexcept {
    auto abortToken = _initAbortSource(stepdownToken);
    _markKilledExecutor->startup();
    _cancelableOpCtxFactory.emplace(abortToken, _markKilledExecutor);

    return ExecutorFuture<void>(**executor)
        .then([this] { _metrics()->onStart(); })
        .then([this, executor, abortToken] {
            return _runUntilStrictConsistencyOrErrored(executor, abortToken);
        })
        .then([this, executor, abortToken] {
            return _notifyCoordinatorAndAwaitDecision(executor, abortToken);
        })
        .onCompletion([this, executor, stepdownToken, abortToken](Status status) {
            _cancelableOpCtxFactory.emplace(stepdownToken, _markKilledExecutor);
            if (stepdownToken.isCanceled()) {
                // Propagate any errors from the recipient stepping down.
                return ExecutorFuture<bool>(**executor, status);
            }

            if (!status.isOK() && !abortToken.isCanceled()) {
                // Propagate any errors from the recipient failing to notify the coordinator.
                return ExecutorFuture<bool>(**executor, status);
            }

            return ExecutorFuture(**executor, abortToken.isCanceled());
        })
        .then([this, executor, stepdownToken](bool aborted) {
            return _finishReshardingOperation(executor, stepdownToken, aborted);
        })
        .onError([this, stepdownToken](Status status) {
            if (stepdownToken.isCanceled()) {
                // The operation will continue on a new RecipientStateMachine.
                return status;
            }

            LOGV2_FATAL(5551101,
                        "Unrecoverable error occurred past the point recipient was prepared to "
                        "complete the resharding operation",
                        "error"_attr = redact(status));
        })
        .thenRunOn(_recipientService->getInstanceCleanupExecutor())
        // The shared_ptr stored in the PrimaryOnlyService's map for the ReshardingRecipientService
        // Instance is removed when the donor state document tied to the instance is deleted. It is
        // necessary to use shared_from_this() to extend the lifetime so the all earlier code can
        // safely finish executing.
        .onCompletion([this, self = shared_from_this(), stepdownToken](Status status) {
            if (stepdownToken.isCanceled()) {
                // Interrupt occured, ensure the metrics get shut down.
                // TODO SERVER-56500: Don't use ReshardingOperationStatusEnum::kCanceled here if it
                // is not meant for failover cases.
                _metrics()->onCompletion(ReshardingOperationStatusEnum::kCanceled);
            }

            return status;
        })
        .semi();
}

void ReshardingRecipientService::RecipientStateMachine::interrupt(Status status) {
    // Resolve any unresolved promises to avoid hanging.
    stdx::lock_guard<Latch> lk(_mutex);
    if (_dataReplication) {
        _dataReplication->shutdown();
    }

    if (!_completionPromise.getFuture().isReady()) {
        _completionPromise.setError(status);
    }
}

boost::optional<BSONObj> ReshardingRecipientService::RecipientStateMachine::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode,
    MongoProcessInterface::CurrentOpSessionsMode) noexcept {
    ReshardingMetrics::ReporterOptions options(ReshardingMetrics::ReporterOptions::Role::kRecipient,
                                               _metadata.getReshardingUUID(),
                                               _metadata.getSourceNss(),
                                               _metadata.getReshardingKey().toBSON(),
                                               false);
    return _metrics()->reportForCurrentOp(options);
}

void ReshardingRecipientService::RecipientStateMachine::onReshardingFieldsChanges(
    OperationContext* opCtx, const TypeCollectionReshardingFields& reshardingFields) {
    if (reshardingFields.getState() == CoordinatorStateEnum::kAborting) {
        auto abortReason = Status(ErrorCodes::ReshardCollectionAborted, "aborted");
        _onAbortEncountered(opCtx, abortReason);
        return;
    }

    stdx::lock_guard<Latch> lk(_mutex);
    auto coordinatorState = reshardingFields.getState();

    if (coordinatorState >= CoordinatorStateEnum::kCloning) {
        auto recipientFields = *reshardingFields.getRecipientFields();
        invariant(recipientFields.getCloneTimestamp());
        invariant(recipientFields.getApproxDocumentsToCopy());
        invariant(recipientFields.getApproxBytesToCopy());
        ensureFulfilledPromise(lk,
                               _allDonorsPreparedToDonate,
                               {*recipientFields.getCloneTimestamp(),
                                *recipientFields.getApproxDocumentsToCopy(),
                                *recipientFields.getApproxBytesToCopy(),
                                recipientFields.getDonorShards()});
    }

    if (coordinatorState >= CoordinatorStateEnum::kCommitting) {
        ensureFulfilledPromise(lk, _coordinatorHasDecisionPersisted);
    }
}

ExecutorFuture<void> ReshardingRecipientService::RecipientStateMachine::
    _awaitAllDonorsPreparedToDonateThenTransitionToCreatingCollection(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& abortToken) {
    if (_recipientCtx.getState() > RecipientStateEnum::kAwaitingFetchTimestamp) {
        invariant(_cloneTimestamp);
        return ExecutorFuture(**executor);
    }

    return future_util::withCancellation(_allDonorsPreparedToDonate.getFuture(), abortToken)
        .thenRunOn(**executor)
        .then([this, executor](
                  ReshardingRecipientService::RecipientStateMachine::CloneDetails cloneDetails) {
            _transitionToCreatingCollection(cloneDetails,
                                            (*executor)->now() + _minimumOperationDuration);
            _metrics()->setDocumentsToCopy(cloneDetails.approxDocumentsToCopy,
                                           cloneDetails.approxBytesToCopy);
        });
}

void ReshardingRecipientService::RecipientStateMachine::
    _createTemporaryReshardingCollectionThenTransitionToCloning() {
    if (_recipientCtx.getState() > RecipientStateEnum::kCreatingCollection) {
        return;
    }

    {
        auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());

        _externalState->ensureTempReshardingCollectionExistsWithIndexes(
            opCtx.get(), _metadata, *_cloneTimestamp);

        _externalState->withShardVersionRetry(
            opCtx.get(),
            _metadata.getTempReshardingNss(),
            "validating shard key index for reshardCollection"_sd,
            [&] {
                shardkeyutil::validateShardKeyIndexExistsOrCreateIfPossible(
                    opCtx.get(),
                    _metadata.getTempReshardingNss(),
                    ShardKeyPattern{_metadata.getReshardingKey()},
                    CollationSpec::kSimpleSpec,
                    false /* unique */,
                    shardkeyutil::ValidationBehaviorsShardCollection(opCtx.get()));
            });
    }

    _transitionState(RecipientStateEnum::kCloning);
}

std::unique_ptr<ReshardingDataReplicationInterface>
ReshardingRecipientService::RecipientStateMachine::_makeDataReplication(OperationContext* opCtx,
                                                                        bool cloningDone) {
    invariant(_cloneTimestamp);

    auto myShardId = _externalState->myShardId(opCtx->getServiceContext());
    auto sourceChunkMgr =
        _externalState->getShardedCollectionRoutingInfo(opCtx, _metadata.getSourceNss());

    return _dataReplicationFactory(opCtx,
                                   _metrics(),
                                   _metadata,
                                   _donorShards,
                                   *_cloneTimestamp,
                                   cloningDone,
                                   std::move(myShardId),
                                   std::move(sourceChunkMgr));
}

void ReshardingRecipientService::RecipientStateMachine::_ensureDataReplicationStarted(
    OperationContext* opCtx,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& abortToken) {
    const bool cloningDone = _recipientCtx.getState() > RecipientStateEnum::kCloning;

    if (!_dataReplication) {
        auto dataReplication = _makeDataReplication(opCtx, cloningDone);
        const auto txnCloneTime = _startConfigTxnCloneAt;
        invariant(txnCloneTime);
        _dataReplicationQuiesced =
            dataReplication
                ->runUntilStrictlyConsistent(**executor,
                                             _recipientService->getInstanceCleanupExecutor(),
                                             abortToken,
                                             *_cancelableOpCtxFactory,
                                             txnCloneTime.get())
                .share();

        stdx::lock_guard lk(_mutex);
        _dataReplication = std::move(dataReplication);
    }

    if (cloningDone) {
        _dataReplication->startOplogApplication();
    }
}

ExecutorFuture<void>
ReshardingRecipientService::RecipientStateMachine::_cloneThenTransitionToApplying(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& abortToken) {
    if (_recipientCtx.getState() > RecipientStateEnum::kCloning) {
        return ExecutorFuture(**executor);
    }

    {
        auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
        reshardingPauseRecipientBeforeCloning.pauseWhileSet(opCtx.get());
    }

    {
        auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
        _ensureDataReplicationStarted(opCtx.get(), executor, abortToken);
    }

    {
        auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
        reshardingPauseRecipientDuringCloning.pauseWhileSet(opCtx.get());
    }

    return future_util::withCancellation(_dataReplication->awaitCloningDone(), abortToken)
        .thenRunOn(**executor)
        .then([this] { _transitionState(RecipientStateEnum::kApplying); });
}

ExecutorFuture<void>
ReshardingRecipientService::RecipientStateMachine::_applyThenTransitionToSteadyState(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& abortToken) {
    if (_recipientCtx.getState() > RecipientStateEnum::kApplying) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    _ensureDataReplicationStarted(opCtx.get(), executor, abortToken);

    return _updateCoordinator(opCtx.get(), executor).then([this] {
        _transitionState(RecipientStateEnum::kSteadyState);
    });
}

ExecutorFuture<void> ReshardingRecipientService::RecipientStateMachine::
    _awaitAllDonorsBlockingWritesThenTransitionToStrictConsistency(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& abortToken) {
    if (_recipientCtx.getState() > RecipientStateEnum::kSteadyState) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    {
        auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
        _ensureDataReplicationStarted(opCtx.get(), executor, abortToken);
    }

    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    return _updateCoordinator(opCtx.get(), executor)
        .then([this, abortToken] {
            {
                auto opCtx = cc().makeOperationContext();
                reshardingPauseRecipientDuringOplogApplication.pauseWhileSet(opCtx.get());
            }

            return future_util::withCancellation(_dataReplication->awaitStrictlyConsistent(),
                                                 abortToken);
        })
        .then([this] {
            auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
            for (const auto& donor : _donorShards) {
                auto stashNss =
                    getLocalConflictStashNamespace(_metadata.getSourceUUID(), donor.getShardId());
                AutoGetCollection stashColl(opCtx.get(), stashNss, MODE_IS);
                uassert(5356800,
                        "Resharding completed with non-empty stash collections",
                        !stashColl || stashColl->isEmpty(opCtx.get()));
            }
        })
        .then([this] {
            if (!_isAlsoDonor) {
                auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                RecoverableCriticalSectionService::get(opCtx.get())
                    ->acquireRecoverableCriticalSectionBlockWrites(
                        opCtx.get(),
                        _metadata.getSourceNss(),
                        _critSecReason,
                        ShardingCatalogClient::kMajorityWriteConcern);
            }

            _transitionState(RecipientStateEnum::kStrictConsistency);
        });
}

void ReshardingRecipientService::RecipientStateMachine::_renameTemporaryReshardingCollection() {
    if (_recipientCtx.getState() > RecipientStateEnum::kRenaming) {
        return;
    }

    if (_recipientCtx.getState() != RecipientStateEnum::kRenaming) {
        // TODO SERVER-56816: remove this if statement altogether.
        _transitionState(RecipientStateEnum::kRenaming);
    }

    if (!_isAlsoDonor) {
        auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());

        RecoverableCriticalSectionService::get(opCtx.get())
            ->promoteRecoverableCriticalSectionToBlockAlsoReads(
                opCtx.get(),
                _metadata.getSourceNss(),
                _critSecReason,
                ShardingCatalogClient::kLocalWriteConcern);

        resharding::data_copy::ensureTemporaryReshardingCollectionRenamed(opCtx.get(), _metadata);
    }
}

void ReshardingRecipientService::RecipientStateMachine::_cleanupReshardingCollections(
    bool aborted) {
    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    resharding::data_copy::ensureOplogCollectionsDropped(
        opCtx.get(), _metadata.getReshardingUUID(), _metadata.getSourceUUID(), _donorShards);

    if (aborted) {
        resharding::data_copy::ensureCollectionDropped(
            opCtx.get(), _metadata.getTempReshardingNss(), _metadata.getReshardingUUID());
    }
}

void ReshardingRecipientService::RecipientStateMachine::_transitionState(
    RecipientStateEnum newState) {
    invariant(newState != RecipientStateEnum::kCreatingCollection &&
              newState != RecipientStateEnum::kError);

    auto newRecipientCtx = _recipientCtx;
    newRecipientCtx.setState(newState);
    _transitionState(std::move(newRecipientCtx), boost::none, boost::none);
}

void ReshardingRecipientService::RecipientStateMachine::_transitionState(
    RecipientShardContext&& newRecipientCtx,
    boost::optional<ReshardingRecipientService::RecipientStateMachine::CloneDetails>&& cloneDetails,
    boost::optional<mongo::Date_t> configStartTime) {
    invariant(newRecipientCtx.getState() != RecipientStateEnum::kAwaitingFetchTimestamp);

    // For logging purposes.
    auto oldState = _recipientCtx.getState();
    auto newState = newRecipientCtx.getState();

    _updateRecipientDocument(
        std::move(newRecipientCtx), std::move(cloneDetails), std::move(configStartTime));

    _metrics()->setRecipientState(newState);

    LOGV2_INFO(5279506,
               "Transitioned resharding recipient state",
               "newState"_attr = RecipientState_serializer(newState),
               "oldState"_attr = RecipientState_serializer(oldState),
               "namespace"_attr = _metadata.getSourceNss(),
               "collectionUUID"_attr = _metadata.getSourceUUID(),
               "reshardingUUID"_attr = _metadata.getReshardingUUID());
}

void ReshardingRecipientService::RecipientStateMachine::_transitionToCreatingCollection(
    ReshardingRecipientService::RecipientStateMachine::CloneDetails cloneDetails,
    const boost::optional<mongo::Date_t> startConfigTxnCloneTime) {
    auto newRecipientCtx = _recipientCtx;
    newRecipientCtx.setState(RecipientStateEnum::kCreatingCollection);
    _transitionState(
        std::move(newRecipientCtx), std::move(cloneDetails), std::move(startConfigTxnCloneTime));
}

void ReshardingRecipientService::RecipientStateMachine::_transitionToError(Status abortReason) {
    auto newRecipientCtx = _recipientCtx;
    newRecipientCtx.setState(RecipientStateEnum::kError);
    emplaceAbortReasonIfExists(newRecipientCtx, abortReason);
    _transitionState(std::move(newRecipientCtx), boost::none, boost::none);
}

/**
 * Returns a query filter of the form
 * {
 *     _id: <reshardingUUID>,
 *     recipientShards: {$elemMatch: {
 *         id: <this recipient's ShardId>,
 *         "mutableState.state: {$in: [ <list of valid current states> ]},
 *     }},
 * }
 */
BSONObj ReshardingRecipientService::RecipientStateMachine::_makeQueryForCoordinatorUpdate(
    const ShardId& shardId, RecipientStateEnum newState) {
    // The recipient only updates the coordinator when it transitions to states which the
    // coordinator depends on for its own transitions. The table maps the recipient states which
    // could be updated on the coordinator to the only states the recipient could have already
    // persisted to the current coordinator document in order for its transition to the newState to
    // be valid.
    static const stdx::unordered_map<RecipientStateEnum, std::vector<RecipientStateEnum>>
        validPreviousStateMap = {
            {RecipientStateEnum::kApplying, {RecipientStateEnum::kUnused}},
            {RecipientStateEnum::kSteadyState, {RecipientStateEnum::kApplying}},
            {RecipientStateEnum::kStrictConsistency, {RecipientStateEnum::kSteadyState}},
            {RecipientStateEnum::kError,
             {RecipientStateEnum::kUnused,
              RecipientStateEnum::kApplying,
              RecipientStateEnum::kSteadyState}},
            {RecipientStateEnum::kDone,
             {RecipientStateEnum::kUnused,
              RecipientStateEnum::kApplying,
              RecipientStateEnum::kSteadyState,
              RecipientStateEnum::kStrictConsistency,
              RecipientStateEnum::kError}},
        };

    auto it = validPreviousStateMap.find(newState);
    invariant(it != validPreviousStateMap.end());

    // The network isn't perfectly reliable so it is possible for update commands sent by
    // _updateCoordinator() to be received out of order by the coordinator. To overcome this
    // behavior, the recipient shard includes the list of valid current states as part of
    // the update to transition to the next state. This way, the update from a delayed
    // message won't match the document if it or any later state transitions have already
    // occurred.
    BSONObjBuilder queryBuilder;
    {
        _metadata.getReshardingUUID().appendToBuilder(
            &queryBuilder, ReshardingCoordinatorDocument::kReshardingUUIDFieldName);

        BSONObjBuilder recipientShardsBuilder(
            queryBuilder.subobjStart(ReshardingCoordinatorDocument::kRecipientShardsFieldName));
        {
            BSONObjBuilder elemMatchBuilder(recipientShardsBuilder.subobjStart("$elemMatch"));
            {
                elemMatchBuilder.append(RecipientShardEntry::kIdFieldName, shardId);

                BSONObjBuilder mutableStateBuilder(
                    elemMatchBuilder.subobjStart(RecipientShardEntry::kMutableStateFieldName + "." +
                                                 RecipientShardContext::kStateFieldName));
                {
                    BSONArrayBuilder inBuilder(mutableStateBuilder.subarrayStart("$in"));
                    for (const auto& state : it->second) {
                        inBuilder.append(RecipientState_serializer(state));
                    }
                }
            }
        }
    }

    return queryBuilder.obj();
}

ExecutorFuture<void> ReshardingRecipientService::RecipientStateMachine::_updateCoordinator(
    OperationContext* opCtx, const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
    auto clientOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    return WaitForMajorityService::get(opCtx->getServiceContext())
        .waitUntilMajority(clientOpTime, CancellationToken::uncancelable())
        .thenRunOn(**executor)
        .then([this] {
            auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
            auto shardId = _externalState->myShardId(opCtx->getServiceContext());

            BSONObjBuilder updateBuilder;
            {
                BSONObjBuilder setBuilder(updateBuilder.subobjStart("$set"));
                {
                    setBuilder.append(ReshardingCoordinatorDocument::kRecipientShardsFieldName +
                                          ".$." + RecipientShardEntry::kMutableStateFieldName,
                                      _recipientCtx.toBSON());
                }
            }

            _externalState->updateCoordinatorDocument(
                opCtx.get(),
                _makeQueryForCoordinatorUpdate(shardId, _recipientCtx.getState()),
                updateBuilder.done());
        });
}

void ReshardingRecipientService::RecipientStateMachine::insertStateDocument(
    OperationContext* opCtx, const ReshardingRecipientDocument& recipientDoc) {
    PersistentTaskStore<ReshardingRecipientDocument> store(
        NamespaceString::kRecipientReshardingOperationsNamespace);
    store.add(opCtx, recipientDoc, kNoWaitWriteConcern);
}

void ReshardingRecipientService::RecipientStateMachine::_updateRecipientDocument(
    RecipientShardContext&& newRecipientCtx,
    boost::optional<ReshardingRecipientService::RecipientStateMachine::CloneDetails>&& cloneDetails,
    boost::optional<mongo::Date_t> configStartTime) {
    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    PersistentTaskStore<ReshardingRecipientDocument> store(
        NamespaceString::kRecipientReshardingOperationsNamespace);

    BSONObjBuilder updateBuilder;
    {
        BSONObjBuilder setBuilder(updateBuilder.subobjStart("$set"));
        setBuilder.append(ReshardingRecipientDocument::kMutableStateFieldName,
                          newRecipientCtx.toBSON());

        if (cloneDetails) {
            setBuilder.append(ReshardingRecipientDocument::kCloneTimestampFieldName,
                              cloneDetails->cloneTimestamp);

            BSONArrayBuilder donorShardsArrayBuilder;
            for (const auto& donor : cloneDetails->donorShards) {
                donorShardsArrayBuilder.append(donor.toBSON());
            }

            setBuilder.append(ReshardingRecipientDocument::kDonorShardsFieldName,
                              donorShardsArrayBuilder.arr());
        }

        if (configStartTime) {
            setBuilder.append(ReshardingRecipientDocument::kStartConfigTxnCloneTimeFieldName,
                              *configStartTime);
        }

        setBuilder.doneFast();
    }

    store.update(opCtx.get(),
                 BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName
                      << _metadata.getReshardingUUID()),
                 updateBuilder.done(),
                 kNoWaitWriteConcern);

    _recipientCtx = newRecipientCtx;

    if (cloneDetails) {
        _cloneTimestamp = cloneDetails->cloneTimestamp;
        _donorShards = std::move(cloneDetails->donorShards);
    }

    if (configStartTime) {
        _startConfigTxnCloneAt = *configStartTime;
    }
}

void ReshardingRecipientService::RecipientStateMachine::_removeRecipientDocument() {
    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());

    const auto& nss = NamespaceString::kRecipientReshardingOperationsNamespace;
    writeConflictRetry(
        opCtx.get(), "RecipientStateMachine::_removeRecipientDocument", nss.toString(), [&] {
            AutoGetCollection coll(opCtx.get(), nss, MODE_IX);

            if (!coll) {
                return;
            }

            WriteUnitOfWork wuow(opCtx.get());

            opCtx->recoveryUnit()->onCommit([this](boost::optional<Timestamp> unusedCommitTime) {
                stdx::lock_guard<Latch> lk(_mutex);
                if (_abortReason) {
                    _metrics()->onCompletion(ErrorCodes::isCancellationError(_abortReason.get())
                                                 ? ReshardingOperationStatusEnum::kCanceled
                                                 : ReshardingOperationStatusEnum::kFailure);
                } else {
                    _metrics()->onCompletion(ReshardingOperationStatusEnum::kSuccess);
                }

                _completionPromise.emplaceValue();
            });

            deleteObjects(opCtx.get(),
                          *coll,
                          nss,
                          BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName
                               << _metadata.getReshardingUUID()),
                          true /* justOne */);

            wuow.commit();
        });
}

ReshardingMetrics* ReshardingRecipientService::RecipientStateMachine::_metrics() const {
    return ReshardingMetrics::get(cc().getServiceContext());
}

CancellationToken ReshardingRecipientService::RecipientStateMachine::_initAbortSource(
    const CancellationToken& stepdownToken) {
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _abortSource = CancellationSource(stepdownToken);
    }

    if (auto future = _coordinatorHasDecisionPersisted.getFuture(); future.isReady()) {
        if (auto status = future.getNoThrow(); !status.isOK()) {
            // onReshardingFieldsChanges() missed canceling _abortSource because _initAbortSource()
            // hadn't been called yet. We used an error status stored in
            // _coordinatorHasDecisionPersisted as an indication that an abort had been received.
            // Canceling _abortSource immediately allows callers to use the returned abortToken as a
            // definitive means of checking whether the operation has been aborted.
            _abortSource->cancel();
        }
    }

    return _abortSource->token();
}

void ReshardingRecipientService::RecipientStateMachine::_onAbortEncountered(
    OperationContext* opCtx, const Status& abortReason) {
    auto abortSource = [&]() -> boost::optional<CancellationSource> {
        stdx::lock_guard<Latch> lk(_mutex);
        _abortReason = abortReason;
        invariant(!_abortReason->isOK());

        if (_dataReplication) {
            _dataReplication->shutdown();
        }

        if (_abortSource) {
            return _abortSource;
        } else {
            // run() hasn't been called, notify the operation should be aborted by setting an
            // error.
            invariant(!_coordinatorHasDecisionPersisted.getFuture().isReady());
            _coordinatorHasDecisionPersisted.setError(_abortReason.get());
            return boost::none;
        }
    }();

    if (abortSource) {
        abortSource->cancel();
    }


    if (!_isAlsoDonor) {
        RecoverableCriticalSectionService::get(opCtx)->releaseRecoverableCriticalSection(
            opCtx,
            _metadata.getSourceNss(),
            _critSecReason,
            ShardingCatalogClient::kMajorityWriteConcern);
    }
}

}  // namespace mongo
