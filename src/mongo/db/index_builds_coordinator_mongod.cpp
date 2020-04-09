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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/index_builds_coordinator_mongod.h"

#include <algorithm>

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/index_build_entry_gen.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/concurrency/replication_state_transition_lock_guard.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index_build_entry_helpers.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/two_phase_index_build_knobs_gen.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

namespace mongo {

using namespace indexbuildentryhelpers;

namespace {

MONGO_FAIL_POINT_DEFINE(hangBeforeInitializingIndexBuild);
MONGO_FAIL_POINT_DEFINE(hangAfterInitializingIndexBuild);

const StringData kMaxNumActiveUserIndexBuildsServerParameterName = "maxNumActiveUserIndexBuilds"_sd;

/**
 * Constructs the options for the loader thread pool.
 */
ThreadPool::Options makeDefaultThreadPoolOptions() {
    ThreadPool::Options options;
    options.poolName = "IndexBuildsCoordinatorMongod";
    options.minThreads = 0;
    // Both the primary and secondary nodes will have an unlimited thread pool size. This is done to
    // allow secondary nodes to startup as many index builders as necessary in order to prevent
    // scheduling deadlocks during initial sync or oplog application. When commands are run from
    // user connections that need to create indexes, those commands will hang until there are less
    // than 'maxNumActiveUserIndexBuilds' running index build threads, or until the operation is
    // interrupted.
    options.maxThreads = ThreadPool::Options::kUnlimited;

    // Ensure all threads have a client.
    options.onCreateThread = [](const std::string& threadName) {
        Client::initThread(threadName.c_str());
    };

    return options;
}

}  // namespace

IndexBuildsCoordinatorMongod::IndexBuildsCoordinatorMongod()
    : _threadPool(makeDefaultThreadPoolOptions()) {
    _threadPool.startup();

    // Change the 'setOnUpdate' function for the server parameter to signal the condition variable
    // when the value changes.
    ServerParameter* serverParam =
        ServerParameterSet::getGlobal()->get(kMaxNumActiveUserIndexBuildsServerParameterName);
    static_cast<
        IDLServerParameterWithStorage<ServerParameterType::kStartupAndRuntime, AtomicWord<int>>*>(
        serverParam)
        ->setOnUpdate([this](const int) -> Status {
            _indexBuildFinished.notify_all();
            return Status::OK();
        });
}

void IndexBuildsCoordinatorMongod::shutdown(OperationContext* opCtx) {
    // Stop new scheduling.
    _threadPool.shutdown();

    // Wait for all active builds to stop.
    waitForAllIndexBuildsToStopForShutdown(opCtx);

    // Wait for active threads to finish.
    _threadPool.join();
}

StatusWith<SharedSemiFuture<ReplIndexBuildState::IndexCatalogStats>>
IndexBuildsCoordinatorMongod::startIndexBuild(OperationContext* opCtx,
                                              std::string dbName,
                                              CollectionUUID collectionUUID,
                                              const std::vector<BSONObj>& specs,
                                              const UUID& buildUUID,
                                              IndexBuildProtocol protocol,
                                              IndexBuildOptions indexBuildOptions) {
    const NamespaceStringOrUUID nssOrUuid{dbName, collectionUUID};

    {
        // Only operations originating from user connections need to wait while there are more than
        // 'maxNumActiveUserIndexBuilds' index builds currently running.
        if (opCtx->getClient()->isFromUserConnection()) {
            // Need to follow the locking order here by getting the global lock first followed by
            // the mutex. The global lock acquires the RSTL lock which we use to assert that we're
            // the primary node when running user operations.
            ShouldNotConflictWithSecondaryBatchApplicationBlock shouldNotConflictBlock(
                opCtx->lockState());
            Lock::GlobalLock globalLk(opCtx, MODE_IX);

            stdx::unique_lock<Latch> lk(_mutex);

            auto replCoord = repl::ReplicationCoordinator::get(opCtx);
            uassert(ErrorCodes::NotMaster,
                    "Not primary while waiting to start an index build",
                    replCoord->canAcceptWritesFor(opCtx, nssOrUuid));
            opCtx->waitForConditionOrInterrupt(_indexBuildFinished, lk, [&] {
                const int maxActiveBuilds = maxNumActiveUserIndexBuilds.load();
                if (_numActiveIndexBuilds < maxActiveBuilds) {
                    _numActiveIndexBuilds++;
                    return true;
                }

                LOGV2(4715500,
                      "Too many index builds running simultaneously, waiting until the number of "
                      "active index builds is below the threshold",
                      "numActiveIndexBuilds"_attr = _numActiveIndexBuilds,
                      "maxNumActiveUserIndexBuilds"_attr = maxActiveBuilds,
                      "indexSpecs"_attr = specs,
                      "buildUUID"_attr = buildUUID,
                      "collectionUUID"_attr = collectionUUID);
                return false;
            });
        } else {
            // System index builds have no limit and never wait, but do consume a slot.
            stdx::unique_lock<Latch> lk(_mutex);
            _numActiveIndexBuilds++;
        }
    }

    auto onScopeExitGuard = makeGuard([&] {
        stdx::unique_lock<Latch> lk(_mutex);
        _numActiveIndexBuilds--;
        _indexBuildFinished.notify_one();
    });

    if (indexBuildOptions.twoPhaseRecovery) {
        // Two phase index build recovery goes though a different set-up procedure because the
        // original index will be dropped first.
        invariant(protocol == IndexBuildProtocol::kTwoPhase);
        auto status =
            _setUpIndexBuildForTwoPhaseRecovery(opCtx, dbName, collectionUUID, specs, buildUUID);
        if (!status.isOK()) {
            return status;
        }
    } else {
        auto statusWithOptionalResult =
            _filterSpecsAndRegisterBuild(opCtx,
                                         dbName,
                                         collectionUUID,
                                         specs,
                                         buildUUID,
                                         protocol,
                                         indexBuildOptions.commitQuorum);
        if (!statusWithOptionalResult.isOK()) {
            return statusWithOptionalResult.getStatus();
        }

        if (statusWithOptionalResult.getValue()) {
            // TODO (SERVER-37644): when joining is implemented, the returned Future will no longer
            // always be set.
            invariant(statusWithOptionalResult.getValue()->isReady());
            // The requested index (specs) are already built or are being built. Return success
            // early (this is v4.0 behavior compatible).
            return statusWithOptionalResult.getValue().get();
        }
    }

    invariant(!opCtx->lockState()->isRSTLExclusive(), buildUUID.toString());

    // Copy over all necessary OperationContext state.

    // Task in thread pool should retain the caller's deadline.
    const auto deadline = opCtx->getDeadline();
    const auto timeoutError = opCtx->getTimeoutError();

    const auto nss = CollectionCatalog::get(opCtx).resolveNamespaceStringOrUUID(opCtx, nssOrUuid);

    const auto& oss = OperationShardingState::get(opCtx);
    const auto shardVersion = oss.getShardVersion(nss);
    const auto dbVersion = oss.getDbVersion(dbName);

    // Task in thread pool should have similar CurOp representation to the caller so that it can be
    // identified as a createIndexes operation.
    LogicalOp logicalOp = LogicalOp::opInvalid;
    BSONObj opDesc;
    {
        stdx::unique_lock<Client> lk(*opCtx->getClient());
        auto curOp = CurOp::get(opCtx);
        logicalOp = curOp->getLogicalOp();
        opDesc = curOp->opDescription().getOwned();
    }

    // If this index build was started during secondary batch application, it will have a commit
    // timestamp that must be copied over to timestamp the write to initialize the index build.
    const auto startTimestamp = opCtx->recoveryUnit()->getCommitTimestamp();

    // Use a promise-future pair to wait until the index build has been started. This future will
    // only return when the index build thread has started and the initial catalog write has been
    // written, or an error has been encountered otherwise.
    auto [startPromise, startFuture] = makePromiseFuture<void>();


    auto replState = invariant(_getIndexBuild(buildUUID));

    // The thread pool task will be responsible for signalling the condition variable when the index
    // build thread is done running.
    onScopeExitGuard.dismiss();
    _threadPool.schedule([
        this,
        buildUUID,
        dbName,
        nss,
        deadline,
        indexBuildOptions,
        logicalOp,
        opDesc,
        replState,
        startPromise = std::move(startPromise),
        startTimestamp,
        timeoutError,
        shardVersion,
        dbVersion
    ](auto status) mutable noexcept {
        auto onScopeExitGuard = makeGuard([&] {
            stdx::unique_lock<Latch> lk(_mutex);
            _numActiveIndexBuilds--;
            _indexBuildFinished.notify_one();
        });

        // Clean up if we failed to schedule the task.
        if (!status.isOK()) {
            stdx::unique_lock<Latch> lk(_mutex);
            _unregisterIndexBuild(lk, replState);
            startPromise.setError(status);
            return;
        }

        auto opCtx = Client::getCurrent()->makeOperationContext();
        opCtx->setDeadlineByDate(deadline, timeoutError);

        auto& oss = OperationShardingState::get(opCtx.get());
        oss.initializeClientRoutingVersions(nss, shardVersion, dbVersion);

        {
            stdx::unique_lock<Client> lk(*opCtx->getClient());
            auto curOp = CurOp::get(opCtx.get());
            curOp->setLogicalOp_inlock(logicalOp);
            curOp->setOpDescription_inlock(opDesc);
        }

        while (MONGO_unlikely(hangBeforeInitializingIndexBuild.shouldFail())) {
            sleepmillis(100);
        }

        // Index builds should never take the PBWM lock, even on a primary. This allows the
        // index build to continue running after the node steps down to a secondary.
        ShouldNotConflictWithSecondaryBatchApplicationBlock shouldNotConflictBlock(
            opCtx->lockState());

        if (!indexBuildOptions.twoPhaseRecovery) {
            status = _setUpIndexBuild(opCtx.get(), buildUUID, startTimestamp);
            if (!status.isOK()) {
                startPromise.setError(status);
                return;
            }
        }

        // Signal that the index build started successfully.
        startPromise.setWith([] {});

        while (MONGO_unlikely(hangAfterInitializingIndexBuild.shouldFail())) {
            sleepmillis(100);
        }

        // Runs the remainder of the index build. Sets the promise result and cleans up the index
        // build.
        _runIndexBuild(opCtx.get(), buildUUID, indexBuildOptions);

        // Do not exit with an incomplete future.
        invariant(replState->sharedPromise.getFuture().isReady());
    });

    // Waits until the index build has either been started or failed to start.
    // Ignore any interruption state in 'opCtx'.
    // If 'opCtx' is interrupted, the caller will be notified after startIndexBuild() returns when
    // it checks the future associated with 'sharedPromise'.
    auto status = startFuture.getNoThrow(Interruptible::notInterruptible());
    if (!status.isOK()) {
        return status;
    }
    return replState->sharedPromise.getFuture();
}

Status IndexBuildsCoordinatorMongod::voteCommitIndexBuild(OperationContext* opCtx,
                                                          const UUID& buildUUID,
                                                          const HostAndPort& votingNode) {
    auto swReplState = _getIndexBuild(buildUUID);
    if (!swReplState.isOK()) {
        // Index build might have got torn down.
        return swReplState.getStatus();
    }

    auto replState = swReplState.getValue();
    CommitQuorumOptions commitQuorum;
    {
        // TODO SERVER-46557: persistCommitReadyMemberInfo() should no longer update 'commitQuorum'
        // field in config.system.indexBuilds collection. So this block should be removed.
        stdx::unique_lock<Latch> lk(replState->mutex);
        invariant(replState->commitQuorum);
        commitQuorum = replState->commitQuorum.get();
    }

    Status upsertStatus = Status(ErrorCodes::InternalError, "Uninitialized value");
    IndexBuildEntry indexbuildEntry(
        buildUUID, replState->collectionUUID, commitQuorum, replState->indexNames);
    std::vector<HostAndPort> members{votingNode};
    indexbuildEntry.setCommitReadyMembers(members);

    {
        // Upserts doesn't need to acquire pbwm lock.
        ShouldNotConflictWithSecondaryBatchApplicationBlock noPBWMBlock(opCtx->lockState());
        upsertStatus = persistCommitReadyMemberInfo(opCtx, indexbuildEntry);
    }

    // 'DuplicateKey' error indicates that the commit quorum value read from replState does not
    // match on-disk commit quorum value.
    invariant(upsertStatus.code() != ErrorCodes::DuplicateKey);
    if (upsertStatus.isOK()) {
        _signalIfCommitQuorumIsSatisfied(opCtx, replState);
    }
    return upsertStatus;
}

void IndexBuildsCoordinatorMongod::setSignalAndCancelVoteRequestCbkIfActive(
    WithLock ReplIndexBuildStateLk,
    OperationContext* opCtx,
    std::shared_ptr<ReplIndexBuildState> replState,
    IndexBuildAction signal) {
    // set the signal
    replState->waitForNextAction->emplaceValue(signal);
    // Cancel the callback.
    if (replState->voteCmdCbkHandle.isValid()) {
        repl::ReplicationCoordinator::get(opCtx)->cancelCbkHandle(replState->voteCmdCbkHandle);
    }
}

void IndexBuildsCoordinatorMongod::_sendCommitQuorumSatisfiedSignal(
    WithLock ReplIndexBuildStateLk,
    OperationContext* opCtx,
    std::shared_ptr<ReplIndexBuildState> replState) {
    if (!replState->waitForNextAction->getFuture().isReady()) {
        setSignalAndCancelVoteRequestCbkIfActive(
            ReplIndexBuildStateLk, opCtx, replState, IndexBuildAction::kCommitQuorumSatisfied);
    } else {
        // This implies we already got a commit or abort signal by other ways. This might have
        // been signaled earlier with kPrimaryAbort or kCommitQuorumSatisfied. Or, it's also
        // possible the node got stepped down and received kOplogCommit/koplogAbort or got
        // kRollbackAbort. So, it's ok to skip signaling.
        auto action = replState->waitForNextAction->getFuture().get(opCtx);

        LOGV2(3856200,
              "Not signaling \"{signalAction}\" as it was previously signaled with "
              "\"{signalActionSet}\" for index build: {buildUUID}",
              "signalAction"_attr =
                  _indexBuildActionToString(IndexBuildAction::kCommitQuorumSatisfied),
              "signalActionSet"_attr = _indexBuildActionToString(action),
              "buildUUID"_attr = replState->buildUUID);
    }
}

void IndexBuildsCoordinatorMongod::_signalIfCommitQuorumIsSatisfied(
    OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) {
    while (true) {
        // Read the index builds entry from config.system.indexBuilds collection.
        auto swIndexBuildEntry = getIndexBuildEntry(opCtx, replState->buildUUID);
        // This can occur when no vote got received and stepup tries to check if commit quorum is
        // satisfied.
        if (swIndexBuildEntry == ErrorCodes::NoMatchingDocument)
            return;

        auto indexBuildEntry = invariantStatusOK(swIndexBuildEntry);

        auto voteMemberList = indexBuildEntry.getCommitReadyMembers();
        invariant(voteMemberList,
                  str::stream() << "'" << IndexBuildEntry::kCommitReadyMembersFieldName
                                << "' list is empty for index build: " << replState->buildUUID);
        auto onDiskcommitQuorum = indexBuildEntry.getCommitQuorum();
        bool commitQuorumSatisfied =
            repl::ReplicationCoordinator::get(opCtx)->isCommitQuorumSatisfied(onDiskcommitQuorum,
                                                                              voteMemberList.get());

        stdx::unique_lock<Latch> lk(replState->mutex);
        invariant(replState->commitQuorum,
                  str::stream() << "Commit quorum is missing for index build: "
                                << replState->buildUUID);
        if (onDiskcommitQuorum == replState->commitQuorum.get()) {
            if (commitQuorumSatisfied) {
                LOGV2(3856201,
                      "Index build commit quorum satisfied:",
                      "indexBuildEntry"_attr = indexBuildEntry);
                _sendCommitQuorumSatisfiedSignal(lk, opCtx, replState);
            }
            return;
        }
        // Try reading from system.indexBuilds collection again as the commit quorum value got
        // changed after the data is read from system.indexBuilds collection.
        LOGV2_DEBUG(
            4655300,
            1,
            "Commit Quorum value got changed after reading the value from \"{collName}\" "
            "collection for index build: {buildUUID}, current commit quorum : {currentVal}, old "
            "commit quorum: {oldVal}",
            "collName"_attr = NamespaceString::kIndexBuildEntryNamespace,
            "buildUUID"_attr = replState->buildUUID,
            "currentVal"_attr = replState->commitQuorum.get(),
            "oldVal"_attr = onDiskcommitQuorum);
        mongo::sleepmillis(10);
        continue;
    }
}

bool IndexBuildsCoordinatorMongod::_signalIfCommitQuorumNotEnabled(
    OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState, bool onStepup) {
    // TODO SERVER-46557: Revisit this logic to see if we can check replState->commitQuorum for
    // value to be zero to determine whether commit quorum is enabled or not for this index build.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);

    if (IndexBuildProtocol::kSinglePhase == replState->protocol) {
        // Single-phase builds don't support commit quorum, but they must go through the process of
        // updating their state to synchronize with concurrent abort operations.
        stdx::unique_lock<Latch> lk(replState->mutex);
        if (replState->waitForNextAction->getFuture().isReady()) {
            // If the signal action has been set, it should only be because a concurrent operation
            // already aborted the index build.
            auto action = replState->waitForNextAction->getFuture().get(opCtx);
            invariant(action == IndexBuildAction::kPrimaryAbort,
                      str::stream() << "action: " << _indexBuildActionToString(action)
                                    << ", buildUUID: " << replState->buildUUID);
            LOGV2(4639700,
                  "Not committing single-phase build because it has already been aborted",
                  "buildUUID"_attr = replState->buildUUID);
            return true;
        }
        replState->waitForNextAction->emplaceValue(IndexBuildAction::kSinglePhaseCommit);
        return true;
    } else if (!enableIndexBuildCommitQuorum) {
        const NamespaceStringOrUUID dbAndUUID(replState->dbName, replState->collectionUUID);
        repl::ReplicationStateTransitionLockGuard rstl(opCtx, MODE_IX);
        if (replCoord->canAcceptWritesFor(opCtx, dbAndUUID) || onStepup) {
            // Node is primary here.
            stdx::unique_lock<Latch> lk(replState->mutex);
            _sendCommitQuorumSatisfiedSignal(lk, opCtx, replState);
        }
        // No-op for secondaries.
        return true;
    }
    return false;
}

bool IndexBuildsCoordinatorMongod::_checkVoteCommitIndexCmdSucceeded(const BSONObj& response,
                                                                     const UUID& indexBuildUUID) {
    auto commandStatus = getStatusFromCommandResult(response);
    auto wcStatus = getWriteConcernStatusFromCommandResult(response);
    if (commandStatus.isOK() && wcStatus.isOK()) {
        return true;
    }
    LOGV2(3856202,
          "'voteCommitIndexBuild' command failed.",
          "indexBuildUUID"_attr = indexBuildUUID,
          "responseStatus"_attr = response);
    return false;
}

void IndexBuildsCoordinatorMongod::_signalPrimaryForCommitReadiness(
    OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);

    // Before voting see if we are eligible to skip voting and signal
    // to commit index build if the node is primary.
    if (_signalIfCommitQuorumNotEnabled(opCtx, replState)) {
        return;
    }

    // Yield locks and storage engine resources before blocking.
    opCtx->recoveryUnit()->abandonSnapshot();
    Lock::TempRelease release(opCtx->lockState());
    invariant(!opCtx->lockState()->isRSTLLocked());

    Backoff exponentialBackoff(Seconds(1), Seconds(2));

    auto onRemoteCmdScheduled = [&](executor::TaskExecutor::CallbackHandle handle) {
        stdx::unique_lock<Latch> lk(replState->mutex);
        // We have already received commit or abort signal, So skip voting.
        if (replState->waitForNextAction->getFuture().isReady()) {
            replCoord->cancelCbkHandle(handle);
        } else {
            invariant(!replState->voteCmdCbkHandle.isValid());
            replState->voteCmdCbkHandle = handle;
        }
    };

    auto onRemoteCmdComplete = [&](executor::TaskExecutor::CallbackHandle) {
        stdx::unique_lock<Latch> lk(replState->mutex);
        replState->voteCmdCbkHandle = executor::TaskExecutor::CallbackHandle();
    };

    auto needToVote = [&]() -> bool {
        stdx::unique_lock<Latch> lk(replState->mutex);
        return !replState->waitForNextAction->getFuture().isReady() ? true : false;
    };

    // Retry 'voteCommitIndexBuild' command on error until we have been signaled either with commit
    // or abort. This way, we can make sure majority of nodes will never stop voting and wait for
    // commit or abort signal until they have received commit or abort signal.
    while (needToVote()) {
        // check for any interrupts before starting the voting process.
        opCtx->checkForInterrupt();

        // Don't hammer the network.
        sleepFor(exponentialBackoff.nextSleep());
        // When index build started during startup recovery can try to get it's address when
        // rsConfig is uninitialized. So, retry till it gets initialized. Also, it's important, when
        // we retry, we check if we have received commit or abort signal to ensure liveness. For
        // e.g., consider a case where  index build gets restarted on startup recovery and
        // indexBuildsCoordinator thread waits for valid address w/o checking commit or abort
        // signal. Now, things can go wrong if we try to replay commitIndexBuild oplog entry for
        // that index build on startup recovery. Oplog applier would get stuck waiting on the
        // indexBuildsCoordinator thread. As a result, we won't be able to transition to secondary
        // state, get stuck on startup state.
        auto myAddress = replCoord->getMyHostAndPort();
        if (myAddress.empty()) {
            continue;
        }
        auto const voteCmdRequest =
            BSON("voteCommitIndexBuild" << replState->buildUUID << "hostAndPort"
                                        << myAddress.toString() << "writeConcern"
                                        << BSON("w"
                                                << "majority"));

        BSONObj voteCmdResponse;
        try {
            voteCmdResponse = replCoord->runCmdOnPrimaryAndAwaitResponse(
                opCtx, "admin", voteCmdRequest, onRemoteCmdScheduled, onRemoteCmdComplete);
        } catch (DBException& ex) {
            if (ex.isA<ErrorCategory::ShutdownError>()) {
                throw;
            }

            // All other errors including CallbackCanceled and network errors should be retried.
            // If ErrorCodes::CallbackCanceled is due to shutdown, then checkForInterrupt() at the
            // beginning of this loop will catch it and throw an error to the caller. Or, if we
            // received the CallbackCanceled error because the index build was signaled with abort
            // or commit signal, then needToVote() would return false and we don't retry the voting
            // process.
            LOGV2_DEBUG(4666400,
                        1,
                        "Failed to run 'voteCommitIndexBuild' command.",
                        "indexBuildUUID"_attr = replState->buildUUID,
                        "errorMsg"_attr = ex);
            continue;
        }

        // Command error and write concern error have to be retried.
        if (_checkVoteCommitIndexCmdSucceeded(voteCmdResponse, replState->buildUUID)) {
            break;
        }
    }
    return;
}

IndexBuildAction IndexBuildsCoordinatorMongod::_drainSideWritesUntilNextActionIsAvailable(
    OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) {
    auto future = [&] {
        stdx::unique_lock<Latch> lk(replState->mutex);
        invariant(replState->waitForNextAction);
        return replState->waitForNextAction->getFuture();
    }();

    // Waits until the promise is fulfilled or the deadline expires.
    IndexBuildAction nextAction;
    auto waitUntilNextActionIsReady = [&]() {
        // Don't perform a blocking wait while holding locks or storage engine resources.
        opCtx->recoveryUnit()->abandonSnapshot();
        Lock::TempRelease release(opCtx->lockState());

        auto deadline = Date_t::now() + Milliseconds(1000);
        auto timeoutError = opCtx->getTimeoutError();

        try {
            nextAction =
                opCtx->runWithDeadline(deadline, timeoutError, [&] { return future.get(opCtx); });
        } catch (const ExceptionForCat<ErrorCategory::ExceededTimeLimitError>& e) {
            if (e.code() == timeoutError) {
                return false;
            }
            throw;
        }
        return true;
    };

    // Continuously drain incoming writes until the future is ready. This is an optimization that
    // allows the critical section of committing, which must drain the remainder of the side writes,
    // to be as short as possible.
    while (!waitUntilNextActionIsReady()) {
        _insertKeysFromSideTablesWithoutBlockingWrites(opCtx, replState);
    }
    return nextAction;
}

Timestamp IndexBuildsCoordinatorMongod::_waitForNextIndexBuildAction(
    OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) {
    Timestamp commitIndexBuildTimestamp;

    LOGV2(3856203,
          "Index build waiting for next action before completing final phase: {buildUUID}",
          "buildUUID"_attr = replState->buildUUID);

    while (true) {
        // Future wait can be interrupted. This function will yield locks while waiting for the
        // future to be fulfilled.
        const auto nextAction = _drainSideWritesUntilNextActionIsAvailable(opCtx, replState);
        LOGV2(3856204,
              "Index build received signal for build uuid: {buildUUID} , action: {action}",
              "buildUUID"_attr = replState->buildUUID,
              "action"_attr = _indexBuildActionToString(nextAction));

        bool needsToRetryWait = false;

        // Ensure RSTL is acquired before checking replication state. This is only necessary for
        // single-phase builds on secondaries. Everywhere else, the RSTL is already held and this is
        // should never block.
        repl::ReplicationStateTransitionLockGuard rstl(opCtx, MODE_IX);

        const NamespaceStringOrUUID dbAndUUID(replState->dbName, replState->collectionUUID);
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        auto isMaster = replCoord->canAcceptWritesFor(opCtx, dbAndUUID);

        stdx::unique_lock<Latch> lk(replState->mutex);
        switch (nextAction) {
            case IndexBuildAction::kNoAction:
                break;
            case IndexBuildAction::kOplogCommit:
                invariant(replState->protocol == IndexBuildProtocol::kTwoPhase);

                // Sanity check
                // This signal can be received during primary (drain phase), secondary,
                // startup( startup recovery) and startup2 (initial sync).
                invariant(!isMaster && replState->indexBuildState.isCommitPrepared(),
                          str::stream()
                              << "Index build: " << replState->buildUUID
                              << ",  index build state: " << replState->indexBuildState.toString());
                invariant(replState->indexBuildState.getTimestamp(),
                          replState->buildUUID.toString());
                // set the commit timestamp
                commitIndexBuildTimestamp = replState->indexBuildState.getTimestamp().get();
                LOGV2(3856205,
                      "Committing index build",
                      "buildUUID"_attr = replState->buildUUID,
                      "commitTimestamp"_attr = replState->indexBuildState.getTimestamp().get(),
                      "collectionUUID"_attr = replState->collectionUUID);
                break;
            case IndexBuildAction::kOplogAbort:
                invariant(replState->protocol == IndexBuildProtocol::kTwoPhase);
                // Sanity check
                // This signal can be received during primary (drain phase), secondary,
                // startup( startup recovery) and startup2 (initial sync).
                invariant(!isMaster && replState->indexBuildState.isAbortPrepared(),
                          str::stream()
                              << "Index build: " << replState->buildUUID
                              << ",  index build state: " << replState->indexBuildState.toString());
                invariant(replState->indexBuildState.getTimestamp() &&
                              replState->indexBuildState.getAbortReason(),
                          replState->buildUUID.toString());
                LOGV2(3856206,
                      "Aborting index build",
                      "buildUUID"_attr = replState->buildUUID,
                      "abortTimestamp"_attr = replState->indexBuildState.getTimestamp().get(),
                      "abortReason"_attr = replState->indexBuildState.getAbortReason().get(),
                      "collectionUUID"_attr = replState->collectionUUID);
                break;
            case IndexBuildAction::kRollbackAbort:
                invariant(replState->protocol == IndexBuildProtocol::kTwoPhase);
                invariant(replCoord->getMemberState().rollback());

                uassertStatusOK(Status(
                    ErrorCodes::IndexBuildAborted,
                    str::stream() << "Aborting index build, index build uuid:"
                                  << replState->buildUUID << " , abort reason:"
                                  << replState->indexBuildState.getAbortReason().get_value_or("")));
                break;
            case IndexBuildAction::kPrimaryAbort:
                // There are chances when the index build got aborted, it only existed in the
                // coordinator, So, we missed marking the index build aborted on manager. So, it's
                // important, we exit from here if we are still primary. Otherwise, the index build
                // gets committed, though our index build was marked aborted.

                // Single-phase builds do not replicate abort oplog entries. We do not need to be
                // primary to abort the index build, and we must continue aborting even in the event
                // of a state transition because this build will not receive another signal.
                if (isMaster || IndexBuildProtocol::kSinglePhase == replState->protocol) {
                    uassertStatusOK(Status(
                        ErrorCodes::IndexBuildAborted,
                        str::stream()
                            << "Index build aborted for index build: " << replState->buildUUID
                            << " , abort reason:"
                            << replState->indexBuildState.getAbortReason().get_value_or("")));
                }
                // Intentionally continue to next case. If we are no longer primary while processing
                // kPrimaryAbort, fall back to the kCommitQuorumSatisfied case and reset our
                // 'waitForNextAction'.
            case IndexBuildAction::kCommitQuorumSatisfied:
                if (!isMaster) {
                    // Reset the promise as the node has stepped down,
                    // wait for the new primary to coordinate the index build and send the new
                    // signal/action.
                    LOGV2(3856207,
                          "No longer primary, so will be waiting again for next action before "
                          "completing final phase: {buildUUID}",
                          "buildUUID"_attr = replState->buildUUID);
                    replState->waitForNextAction =
                        std::make_unique<SharedPromise<IndexBuildAction>>();
                    needsToRetryWait = true;
                }
                break;
            case IndexBuildAction::kSinglePhaseCommit:
                invariant(replState->protocol == IndexBuildProtocol::kSinglePhase);
                break;
        }

        if (!needsToRetryWait) {
            break;
        }
    }
    return commitIndexBuildTimestamp;
}

Status IndexBuildsCoordinatorMongod::setCommitQuorum(OperationContext* opCtx,
                                                     const NamespaceString& nss,
                                                     const std::vector<StringData>& indexNames,
                                                     const CommitQuorumOptions& newCommitQuorum) {
    if (indexNames.empty()) {
        return Status(ErrorCodes::IndexNotFound,
                      str::stream()
                          << "Cannot set a new commit quorum on an index build in collection '"
                          << nss << "' without providing any indexes.");
    }

    AutoGetCollectionForRead autoColl(opCtx, nss);
    Collection* collection = autoColl.getCollection();
    if (!collection) {
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << "Collection '" << nss << "' was not found.");
    }

    UUID collectionUUID = collection->uuid();

    stdx::unique_lock<Latch> lk(_mutex);
    auto pred = [&](const auto& replState) {
        if (collectionUUID != replState.collectionUUID) {
            return false;
        }
        if (indexNames.size() != replState.indexNames.size()) {
            return false;
        }
        // Ensure the ReplIndexBuildState has the same indexes as 'indexNames'.
        return std::equal(
            replState.indexNames.begin(), replState.indexNames.end(), indexNames.begin());
    };
    auto collIndexBuilds = _filterIndexBuilds_inlock(lk, pred);
    if (collIndexBuilds.empty()) {
        return Status(ErrorCodes::IndexNotFound,
                      str::stream() << "Cannot find an index build on collection '" << nss
                                    << "' with the provided index names");
    }
    invariant(
        1U == collIndexBuilds.size(),
        str::stream() << "Found multiple index builds with the same index names on collection "
                      << nss << " (" << collectionUUID
                      << "): first index name: " << indexNames.front());

    auto buildState = collIndexBuilds.front();

    // See if the new commit quorum is satisfiable.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    Status status = replCoord->checkIfCommitQuorumCanBeSatisfied(newCommitQuorum);
    if (!status.isOK()) {
        return status;
    }

    // Persist the new commit quorum for the index build and write it to the collection.
    buildState->commitQuorum = newCommitQuorum;
    // TODO (SERVER-40807): disabling the following code for the v4.2 release so it does not have
    // downstream impact.
    /*
    return indexbuildentryhelpers::setCommitQuorum(opCtx, buildState->buildUUID, newCommitQuorum);
    */
    return Status::OK();
}

Status IndexBuildsCoordinatorMongod::_finishScanningPhase() {
    // TODO: implement.
    return Status::OK();
}

Status IndexBuildsCoordinatorMongod::_finishVerificationPhase() {
    // TODO: implement.
    return Status::OK();
}

Status IndexBuildsCoordinatorMongod::_finishCommitPhase() {
    // TODO: implement.
    return Status::OK();
}

StatusWith<bool> IndexBuildsCoordinatorMongod::_checkCommitQuorum(
    const BSONObj& commitQuorum, const std::vector<HostAndPort>& confirmedMembers) {
    // TODO: not yet implemented.
    return false;
}

void IndexBuildsCoordinatorMongod::_refreshReplStateFromPersisted(OperationContext* opCtx,
                                                                  const UUID& buildUUID) {
    // TODO: not yet implemented.
}

}  // namespace mongo
