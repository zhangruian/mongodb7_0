/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/db/s/sharding_runtime_d_params_gen.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/util/future_util.h"

namespace mongo {

class RangeDeleterService : public ReplicaSetAwareServiceShardSvr<RangeDeleterService> {
public:
    RangeDeleterService() = default;

    static RangeDeleterService* get(ServiceContext* serviceContext);

    static RangeDeleterService* get(OperationContext* opCtx);

private:
    /*
     * In memory representation of registered range deletion tasks. To each non-pending range
     * deletion task corresponds a registered task on the service.
     */
    class RangeDeletion : public ChunkRange {
    public:
        RangeDeletion(const RangeDeletionTask& task)
            : ChunkRange(task.getRange().getMin(), task.getRange().getMax()) {}

        ~RangeDeletion() {
            if (!_completionPromise.getFuture().isReady()) {
                _completionPromise.setError(
                    Status{ErrorCodes::Interrupted, "Range deletion interrupted"});
            }
        }

        SharedSemiFuture<void> getCompletionFuture() const {
            return _completionPromise.getFuture().semi().share();
        }

        void makeReady() {
            _completionPromise.emplaceValue();
        }

    private:
        // Marked ready once the range deletion has been fully processed
        SharedPromise<void> _completionPromise;
    };

    /*
     * Internal comparator to sort ranges in _rangeDeletionTasks's sets.
     *
     * NB: it ONLY makes sense to use this on ranges that are comparable, meaning
     * the ones based on the same key pattern (aka the ones belonging to the same
     * sharded collection).
     */
    struct RANGES_COMPARATOR {
        bool operator()(const std::shared_ptr<ChunkRange>& a,
                        const std::shared_ptr<ChunkRange>& b) const {
            return a->getMin().woCompare(b->getMin()) < 0;
        }
    };

    /*
     * Class enclosing a thread continuously processing "ready" range deletions, meaning tasks
     * that are allowed to be processed (already drained ongoing queries and already waited for
     * `orphanCleanupDelaySecs`).
     */
    class ReadyRangeDeletionsProcessor {
    public:
        ReadyRangeDeletionsProcessor(OperationContext* opCtx)
            : _thread(stdx::thread([this] { _runRangeDeletions(); })) {
            stdx::unique_lock<Latch> lock(_mutex);
            opCtx->waitForConditionOrInterrupt(
                _condVar, lock, [&] { return _threadOpCtxHolder.is_initialized(); });
        }

        ~ReadyRangeDeletionsProcessor() {
            {
                stdx::unique_lock<Latch> lock(_mutex);
                // The `_threadOpCtxHolder` may have been already reset/interrupted in case the
                // thread got interrupted due to stepdown
                if (_threadOpCtxHolder) {
                    stdx::lock_guard<Client> scopedClientLock(*(*_threadOpCtxHolder)->getClient());
                    if ((*_threadOpCtxHolder)->checkForInterruptNoAssert().isOK()) {
                        (*_threadOpCtxHolder)->markKilled(ErrorCodes::Interrupted);
                    }
                }
                _condVar.notify_all();
            }

            if (_thread.joinable()) {
                _thread.join();
            }
        }

        /*
         * Schedule a range deletion at the end of the queue
         */
        void emplaceRangeDeletion(const RangeDeletionTask& rdt) {
            stdx::unique_lock<Latch> lock(_mutex);
            _queue.push(rdt);
            _condVar.notify_all();
        }

    private:
        /*
         * Remove a range deletion from the head of the queue. Supposed to be called only once a
         * range deletion successfully finishes.
         */
        void _completedRangeDeletion() {
            stdx::unique_lock<Latch> lock(_mutex);
            dassert(!_queue.empty());
            _queue.pop();
        }

        /*
         * Code executed by the internal thread
         */
        void _runRangeDeletions();

        /* Queue containing scheduled range deletions */
        std::queue<RangeDeletionTask> _queue;
        /* Thread consuming the range deletions queue */
        stdx::thread _thread;
        /* Pointer to the (one and only) operation context used by the thread */
        boost::optional<ServiceContext::UniqueOperationContext> _threadOpCtxHolder;
        /*
         * Condition variable notified when:
         * - The component has been initialized (the operation context has been instantiated)
         * - The instance is shutting down (the operation context has been marked killed)
         * - A new range deletion is scheduled (the queue size has increased by one)
         */
        stdx::condition_variable _condVar;

        Mutex _mutex = MONGO_MAKE_LATCH("ReadyRangeDeletionsProcessor");
    };

    // Keeping track of per-collection registered range deletion tasks
    stdx::unordered_map<UUID, std::set<std::shared_ptr<ChunkRange>, RANGES_COMPARATOR>, UUID::Hash>
        _rangeDeletionTasks;

    // Mono-threaded executor processing range deletion tasks
    std::shared_ptr<executor::TaskExecutor> _executor;

    enum State { kInitializing, kUp, kDown };

    AtomicWord<State> _state{kDown};

    // Future markes as ready when the state changes to "up"
    SemiFuture<void> _stepUpCompletedFuture;

    /* Acquire mutex only if service is up (for "user" operation) */
    [[nodiscard]] stdx::unique_lock<Latch> _acquireMutexFailIfServiceNotUp() {
        stdx::unique_lock<Latch> lg(_mutex_DO_NOT_USE_DIRECTLY);
        uassert(
            ErrorCodes::NotYetInitialized, "Range deleter service not up", _state.load() == kUp);
        return lg;
    }

    /* Unconditionally acquire mutex (for internal operations) */
    [[nodiscard]] stdx::unique_lock<Latch> _acquireMutexUnconditionally() {
        stdx::unique_lock<Latch> lg(_mutex_DO_NOT_USE_DIRECTLY);
        return lg;
    }

    // TODO SERVER-67642 implement fine-grained per-collection locking
    // Protecting the access to all class members (DO NOT USE DIRECTLY: rely on
    // `_acquireMutexUnconditionally` and `_acquireMutexFailIfServiceNotUp`)
    Mutex _mutex_DO_NOT_USE_DIRECTLY = MONGO_MAKE_LATCH("RangeDeleterService::_mutex");

public:
    /*
     * Register a task on the range deleter service.
     * Returns a future that will be marked ready once the range deletion will be completed.
     *
     * In case of trying to register an already existing task, the future will contain an error.
     */
    SharedSemiFuture<void> registerTask(
        const RangeDeletionTask& rdt,
        SemiFuture<void>&& waitForActiveQueriesToComplete = SemiFuture<void>::makeReady(),
        bool fromResubmitOnStepUp = false);

    /*
     * Deregister a task from the range deleter service.
     */
    void deregisterTask(const UUID& collUUID, const ChunkRange& range);

    /*
     * Returns the number of registered range deletion tasks for a collection
     */
    int getNumRangeDeletionTasksForCollection(const UUID& collectionUUID);

    /*
     * Returns a future marked as ready when all overlapping range deletion tasks complete.
     *
     * NB: in case an overlapping range deletion task is registered AFTER invoking this method,
     * it will not be taken into account. Handling this scenario is responsibility of the caller.
     * */
    SharedSemiFuture<void> getOverlappingRangeDeletionsFuture(const UUID& collectionUUID,
                                                              const ChunkRange& range);

    /* ReplicaSetAwareServiceShardSvr implemented methods */
    void onStepUpComplete(OperationContext* opCtx, long long term) override;
    void onStepDown() override;
    void onShutdown() override;

    /*
     * Returns the RangeDeleterService state with the following schema:
     *     {collectionUUIDA: [{min: x, max: y}, {min: w, max: z}....], collectionUUIDB: ......}
     */
    BSONObj dumpState();

    /*
     * Returns the total number of range deletion tasks registered on the service.
     */
    long long totalNumOfRegisteredTasks();

    /* ONLY FOR TESTING: wait for the state to become "up" */
    void _waitForRangeDeleterServiceUp_FOR_TESTING() {
        _stepUpCompletedFuture.get();
    }

    std::unique_ptr<ReadyRangeDeletionsProcessor> _readyRangeDeletionsProcessorPtr;

private:
    /* Asynchronously register range deletions on the service. To be called on on step-up */
    void _recoverRangeDeletionsOnStepUp(OperationContext* opCtx);

    /* Called by shutdown/stepdown hooks to reset the service */
    void _stopService(bool joinExecutor);

    /* ReplicaSetAwareServiceShardSvr "empty implemented" methods */
    void onStartup(OperationContext* opCtx) override final{};
    void onInitialDataAvailable(OperationContext* opCtx,
                                bool isMajorityDataAvailable) override final {}
    void onStepUpBegin(OperationContext* opCtx, long long term) override final {}
    void onBecomeArbiter() override final {}
};

}  // namespace mongo
