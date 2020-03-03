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

/**
 * Constructs the options for the loader thread pool.
 */
ThreadPool::Options makeDefaultThreadPoolOptions() {
    ThreadPool::Options options;
    options.poolName = "IndexBuildsCoordinatorMongod";
    options.minThreads = 0;
    // We depend on thread pool sizes being equal between primaries and secondaries. If a secondary
    // has fewer resources than a primary, index build oplog entries can replicate in an order that
    // the secondary is unable to fulfill, leading to deadlocks. See SERVER-44250.
    options.maxThreads = 3;

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
}
IndexBuildsCoordinatorMongod::IndexBuildsCoordinatorMongod(ThreadPool::Options options)
    : _threadPool(std::move(options)) {
    _threadPool.startup();
}

void IndexBuildsCoordinatorMongod::shutdown() {
    // Stop new scheduling.
    _threadPool.shutdown();

    // Wait for all active builds to stop.
    waitForAllIndexBuildsToStopForShutdown();

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

    const NamespaceStringOrUUID nssOrUuid{dbName, collectionUUID};
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
    _threadPool.schedule([
        this,
        buildUUID,
        collectionUUID,
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
    auto status = startFuture.getNoThrow(opCtx);
    if (!status.isOK()) {
        return status;
    }
    return replState->sharedPromise.getFuture();
}

Status IndexBuildsCoordinatorMongod::voteCommitIndexBuild(OperationContext* opCtx,
                                                          const UUID& buildUUID,
                                                          const HostAndPort& hostAndPort) {

    auto swReplState = _getIndexBuild(buildUUID);
    if (!swReplState.isOK()) {
        // Index build might have got torn down.
        return swReplState.getStatus();
    }

    Status upsertStatus = Status::OK();
    std::vector<HostAndPort> members;
    members.push_back(hostAndPort);
    auto replState = swReplState.getValue();

    {
        stdx::unique_lock<Latch> lk(replState->mutex);
        // This indicates the index build was successfully able to commit or abort, and about to
        // write 'commitIndexBuild' or 'abortIndexBuild' oplog entry. In such case, we should throw
        // a retryable error code to secondary and not try to persist the votes. Otherwise a
        // deadlock can happen if a commit/abortIndexBuild oplog entry is followed by write to
        // "config.system.indexBuilds" collection. In that case, voteCommitIndexBuild cmd on primary
        // will be waiting for the system.indexBuilds write to be majority replicated. But, then,
        // secondary oplog will be stuck waiting on commit/abortIndexBuild oplog entry. And,
        // commit/abortIndexBuild oplog entry will be waiting on the secondary indexBuildCoordinator
        // thread to join. But, the indexBuildCoordinator thread will be waiting for the
        // voteCommitIndexBuild response.
        if (replState->indexBuildState.isSet(IndexBuildState::kCommitted |
                                             IndexBuildState::kAborted)) {
            return Status{ErrorCodes::CommandFailed,
                          str::stream()
                              << "Index build state : " << replState->indexBuildState.toString()};
        }

        invariant(replState->commitQuorum);
        IndexBuildEntry indexbuildEntry(buildUUID,
                                        replState->collectionUUID,
                                        replState->commitQuorum.get(),
                                        replState->indexNames);
        indexbuildEntry.setCommitReadyMembers(members);

        // Persist the vote with replState mutex lock held to make sure that node will not write the
        // commit/abortIndexBuild oplog entry.
        upsertStatus = persistCommitReadyMemberInfo(opCtx, indexbuildEntry);
        // 'DuplicateKey' error indicates that the commit quorum value read from replState does not
        // match on-disk commit quorum value. Since, we persist the vote with replState mutex lock
        // held, there is no way this can happen. We basically don't want something like this,
        // SetIndexCommitQuorum command changes the commit quorum from 3 to 5. And, the
        // voteCommitIndexBuild resets the commit quorum value to be 3 while updating the voter's
        // info.
        invariant(upsertStatus.code() != ErrorCodes::DuplicateKey);
    }

    if (upsertStatus.isOK()) {
        _signalIfCommitQuorumIsSatisfied(opCtx, replState);
    }
    return upsertStatus;
}

void IndexBuildsCoordinatorMongod::_sendCommitQuorumSatisfiedSignal(
    WithLock lk, OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) {
    if (!replState->waitForNextAction->getFuture().isReady()) {
        replState->waitForNextAction->emplaceValue(IndexBuildAction::kCommitQuorumSatisfied);
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
        int voteReceived = voteMemberList->size();

        auto onDiskcommitQuorum = indexBuildEntry.getCommitQuorum();
        int requiredQuorumCount = onDiskcommitQuorum.numNodes;
        if (onDiskcommitQuorum.mode == CommitQuorumOptions::kMajority) {
            requiredQuorumCount =
                repl::ReplicationCoordinator::get(opCtx)->getConfig().getWriteMajority();
        }

        stdx::unique_lock<Latch> lk(replState->mutex);
        invariant(replState->commitQuorum,
                  str::stream() << "Commit quorum is missing for index build: "
                                << replState->buildUUID);
        if (onDiskcommitQuorum == replState->commitQuorum.get()) {
            if (voteReceived >= requiredQuorumCount) {
                LOGV2(3856201,
                      "Index build Commit Quorum Satisfied: {indexBuildEntry}",
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
    // Locking order is important here to avoid deadlocks i.e, rstl followed by ReplIndexBuildState
    // mutex.
    invariant(opCtx->lockState()->isRSTLLocked());

    // TODO SERVER-46557: Revisit this logic to see if we can check replState->commitQuorum for
    // value to be zero to determine whether commit quorum is enabled or not for this index build.
    if (!enableIndexBuildCommitQuorum) {
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        const NamespaceStringOrUUID dbAndUUID(replState->dbName, replState->collectionUUID);
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

bool IndexBuildsCoordinatorMongod::_checkVoteCommitIndexCmdSucceeded(const BSONObj& response) {
    auto commandStatus = getStatusFromCommandResult(response);
    auto wcStatus = getWriteConcernStatusFromCommandResult(response);
    if (commandStatus.isOK() && wcStatus.isOK()) {
        return true;
    }
    LOGV2(3856202,
          "'voteCommitIndexBuild' command failed with response : {response}",
          "response"_attr = response);
    return false;
}

void IndexBuildsCoordinatorMongod::_signalPrimaryForCommitReadiness(
    OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (!replCoord->getSettings().usingReplSets()) {
        // Standalones does not support commit quorum.
        return;
    }

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
        auto future = replState->waitForNextAction->getFuture();
        // Don't set the callback handle if we have been signaled with kRollbackAbort.
        // Otherwise, it can violate liveness property. Consider a case, where the bgsync
        // thread has signaled aborted and waits for the secondary indexBuildCoordinator
        // thread to join. But, the indexBuildCoordinator thread will be waiting for the
        // remote "voteCommitIndexBuild" command's response. And, the primary will be
        // waiting for 'voteCommitIndexBuild' command's write to be majority replicated. But,
        // gets stuck waiting for the rollback node to transition out to secondary.
        if (future.isReady() && future.get(opCtx) == IndexBuildAction::kRollbackAbort) {
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
        // Needs comment.
        return !replState->waitForNextAction->getFuture().isReady() ? true : false;
    };

    auto convertToNonFatalStatus = [&](Status origStatus) -> Status {
        auto errCode = ErrorCodes::InterruptedAtShutdown;

        stdx::unique_lock<Latch> lk(replState->mutex);
        if (replState->indexBuildState.isAbortPrepared()) {
            errCode = ErrorCodes::IndexBuildAborted;
        }

        return Status{errCode, origStatus.reason()};
    };


    // Retry 'voteCommitIndexBuild' command on error until we have been signaled either with commit
    // or abort. This way, we can make sure majority of nodes will never stop voting and wait for
    // commit or abort signal until they have received commit or abort signal.
    while (needToVote()) {
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
            if (ex.isA<ErrorCategory::ShutdownError>() ||
                ex.isA<ErrorCategory::CancelationError>()) {
                // This includes error like ErrorCodes::CallbackCanceled,
                // ErrorCodes::ShutdownInProgress We might have either received
                // ErrorCodes::CallbackCanceled due to rollback or shutdown. converting the status
                // to non-fatal
                uassertStatusOK(convertToNonFatalStatus(ex.toStatus()));
            }
            // All other error including network errors should be retried.
            continue;
        }

        // Command error and write concern error have to be retried.
        if (_checkVoteCommitIndexCmdSucceeded(voteCmdResponse)) {
            break;
        }
    }
    return;
}

Timestamp IndexBuildsCoordinatorMongod::_waitForNextIndexBuildAction(
    OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) {
    Timestamp commitIndexBuildTimestamp;

    invariant(replState->protocol == IndexBuildProtocol::kTwoPhase);

    // standalones doesn't need to wait for commit or abort index build oplog entry.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (!replCoord->getSettings().usingReplSets()) {
        return commitIndexBuildTimestamp;
    };

    // Yield locks and storage engine resources before blocking.
    opCtx->recoveryUnit()->abandonSnapshot();
    Lock::TempRelease release(opCtx->lockState());

    LOGV2(3856203,
          "Index build waiting for next action before completing final phase: {buildUUID}",
          "buildUUID"_attr = replState->buildUUID);

    while (true) {
        // Future wait should ignore state transition.
        invariant(!opCtx->lockState()->isRSTLLocked(),
                  str::stream()
                      << "failed to yield locks for index build while waiting for commit or abort: "
                      << replState->buildUUID);

        // future wait should get interrupted if the node shutdowns.
        const auto nextAction = replState->waitForNextAction->getFuture().get(opCtx);
        LOGV2(3856204,
              "Index build received signal for build uuid: {buildUUID} , action: {action}",
              "buildUUID"_attr = replState->buildUUID,
              "action"_attr = _indexBuildActionToString(nextAction));

        bool needsToRetryWait = false;

        // Reacquire RSTL lock
        repl::ReplicationStateTransitionLockGuard rstl(opCtx, MODE_IX);
        const NamespaceStringOrUUID dbAndUUID(replState->dbName, replState->collectionUUID);
        auto isMaster = replCoord->canAcceptWritesFor(opCtx, dbAndUUID);

        stdx::unique_lock<Latch> lk(replState->mutex);
        switch (nextAction) {
            case IndexBuildAction::kOplogCommit:
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
            case IndexBuildAction::kRollbackAbort:
                // Currently, We abort the index build before transitioning the state to rollback.
                // So, we can check if the node state is rollback.
                break;
            case IndexBuildAction::kPrimaryAbort:
                // There are chances when the index build got aborted, it only existed in the
                // coordinator, So, we missed marking the index build aborted on manager. So, it's
                // important, we exit from here if we are still primary. Otherwise, the index build
                // gets committed, though our index build was marked aborted.
                if (isMaster) {
                    uassertStatusOK(Status(
                        ErrorCodes::IndexBuildAborted,
                        str::stream()
                            << "Index build aborted for index build: " << replState->buildUUID
                            << " , abort reason:"
                            << replState->indexBuildState.getAbortReason().get_value_or("")));
                }
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
            default:
                MONGO_UNREACHABLE;
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
    auto collectionIt = _collectionIndexBuilds.find(collectionUUID);
    if (collectionIt == _collectionIndexBuilds.end()) {
        return Status(ErrorCodes::IndexNotFound,
                      str::stream() << "No index builds found on collection '" << nss << "'.");
    }

    if (!collectionIt->second->hasIndexBuildState(lk, indexNames.front())) {
        return Status(ErrorCodes::IndexNotFound,
                      str::stream() << "Cannot find an index build on collection '" << nss
                                    << "' with the provided index names");
    }

    // Use the first index to get the ReplIndexBuildState.
    std::shared_ptr<ReplIndexBuildState> buildState =
        collectionIt->second->getIndexBuildState(lk, indexNames.front());

    // Ensure the ReplIndexBuildState has the same indexes as 'indexNames'.
    bool equal = std::equal(
        buildState->indexNames.begin(), buildState->indexNames.end(), indexNames.begin());
    if (buildState->indexNames.size() != indexNames.size() || !equal) {
        return Status(ErrorCodes::IndexNotFound,
                      str::stream()
                          << "Provided indexes are not all being "
                          << "built by the same index builder in collection '" << nss << "'.");
    }

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
