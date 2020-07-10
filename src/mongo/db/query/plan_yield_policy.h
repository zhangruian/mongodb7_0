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

#include <functional>

#include "mongo/db/operation_context.h"
#include "mongo/util/duration.h"
#include "mongo/util/elapsed_tracker.h"

namespace mongo {

class ClockSource;

class PlanYieldPolicy {
public:
    enum class YieldPolicy {
        // Any call to getNext() may yield. In particular, the executor may die on any call to
        // getNext() due to a required index or collection becoming invalid during yield. If this
        // occurs, getNext() will produce an error during yield recovery and will return FAILURE.
        // Additionally, this will handle all WriteConflictExceptions that occur while processing
        // the query.  With this yield policy, it is possible for getNext() to return FAILURE with
        // locks released, if the operation is killed while yielding.
        YIELD_AUTO,

        // This will handle WriteConflictExceptions that occur while processing the query, but will
        // not yield locks. abandonSnapshot() will be called if a WriteConflictException occurs so
        // callers must be prepared to get a new snapshot. The caller must hold their locks
        // continuously from construction to destruction. Callers which do not want auto-yielding,
        // but may release their locks during query execution must use the YIELD_MANUAL policy.
        WRITE_CONFLICT_RETRY_ONLY,

        // Use this policy if you want to disable auto-yielding, but will release locks while using
        // the PlanExecutor. Any WriteConflictExceptions will be raised to the caller of getNext().
        //
        // With this policy, an explicit call must be made to saveState() before releasing locks,
        // and an explicit call to restoreState() must be made after reacquiring locks.
        // restoreState() will throw if the PlanExecutor is now invalid due to a catalog operation
        // (e.g. collection drop) during yield.
        YIELD_MANUAL,

        // Can be used in one of the following scenarios:
        //  - The caller will hold a lock continuously for the lifetime of this PlanExecutor.
        //  - This PlanExecutor doesn't logically belong to a Collection, and so does not need to be
        //    locked during execution. For example, this yield policy is used for PlanExecutors
        //    which unspool queued metadata ("virtual collection scans") for listCollections and
        //    listIndexes.
        NO_YIELD,

        // Will not yield locks or storage engine resources, but will check for interrupt.
        INTERRUPT_ONLY,

        // Used for testing, this yield policy will cause the PlanExecutor to time out on the first
        // yield, returning FAILURE with an error object encoding a ErrorCodes::ExceededTimeLimit
        // message.
        ALWAYS_TIME_OUT,

        // Used for testing, this yield policy will cause the PlanExecutor to be marked as killed on
        // the first yield, returning FAILURE with an error object encoding a
        // ErrorCodes::QueryPlanKilled message.
        ALWAYS_MARK_KILLED,
    };

    static std::string serializeYieldPolicy(YieldPolicy yieldPolicy) {
        switch (yieldPolicy) {
            case YieldPolicy::YIELD_AUTO:
                return "YIELD_AUTO";
            case YieldPolicy::WRITE_CONFLICT_RETRY_ONLY:
                return "WRITE_CONFLICT_RETRY_ONLY";
            case YieldPolicy::YIELD_MANUAL:
                return "YIELD_MANUAL";
            case YieldPolicy::NO_YIELD:
                return "NO_YIELD";
            case YieldPolicy::INTERRUPT_ONLY:
                return "INTERRUPT_ONLY";
            case YieldPolicy::ALWAYS_TIME_OUT:
                return "ALWAYS_TIME_OUT";
            case YieldPolicy::ALWAYS_MARK_KILLED:
                return "ALWAYS_MARK_KILLED";
        }
        MONGO_UNREACHABLE;
    }

    static YieldPolicy parseFromBSON(const StringData& element) {
        const std::string& yieldPolicy = element.toString();
        if (yieldPolicy == "YIELD_AUTO") {
            return YieldPolicy::YIELD_AUTO;
        }
        if (yieldPolicy == "WRITE_CONFLICT_RETRY_ONLY") {
            return YieldPolicy::WRITE_CONFLICT_RETRY_ONLY;
        }
        if (yieldPolicy == "YIELD_MANUAL") {
            return YieldPolicy::YIELD_MANUAL;
        }
        if (yieldPolicy == "NO_YIELD") {
            return YieldPolicy::NO_YIELD;
        }
        if (yieldPolicy == "INTERRUPT_ONLY") {
            return YieldPolicy::INTERRUPT_ONLY;
        }
        if (yieldPolicy == "ALWAYS_TIME_OUT") {
            return YieldPolicy::ALWAYS_TIME_OUT;
        }
        if (yieldPolicy == "ALWAYS_MARK_KILLED") {
            return YieldPolicy::ALWAYS_MARK_KILLED;
        }
        MONGO_UNREACHABLE;
    }

    /**
     * Constructs a PlanYieldPolicy of the given 'policy' type. This class uses an ElapsedTracker
     * to keep track of elapsed time, which is initialized from the parameters 'cs',
     * 'yieldIterations' and 'yieldPeriod'.
     */
    PlanYieldPolicy(YieldPolicy policy,
                    ClockSource* cs,
                    int yieldIterations,
                    Milliseconds yieldPeriod);

    virtual ~PlanYieldPolicy() = default;

    /**
     * Periodically returns true to indicate that it is time to check for interrupt (in the case of
     * YIELD_AUTO and INTERRUPT_ONLY) or release locks or storage engine state (in the case of
     * auto-yielding plans).
     */
    virtual bool shouldYieldOrInterrupt(OperationContext* opCtx);

    /**
     * Resets the yield timer so that we wait for a while before yielding/interrupting again.
     */
    void resetTimer();

    /**
     * Used to cause a plan executor to check for interrupt (in the case of YIELD_AUTO and
     * INTERRUPT_ONLY) or release locks or storage engine state (in the case of auto-yielding
     * plans). The PlanExecutor must *not* be in saved state. Handles calls to save/restore state
     * internally.
     *
     * Returns Status::OK() if the executor was restored successfully and is still alive. Returns
     * ErrorCodes::QueryPlanKilled if the executor got killed during yield, and
     * ErrorCodes::ExceededTimeLimit if the operation has exceeded the time limit.
     *
     * Calls 'whileYieldingFn' after relinquishing locks and before reacquiring the locks that have
     * been relinquished.
     */
    Status yieldOrInterrupt(OperationContext* opCtx,
                            std::function<void()> whileYieldingFn = nullptr);

    /**
     * All calls to shouldYieldOrInterrupt() will return true until the next call to
     * yieldOrInterrupt(). This must only be called for auto-yielding plans, to force a yield. It
     * cannot be used to force an interrupt for INTERRUPT_ONLY plans.
     */
    void forceYield() {
        dassert(canAutoYield());
        _forceYield = true;
    }

    /**
     * Returns true if there is a possibility that a collection lock will be yielded at some point
     * during this PlanExecutor's lifetime.
     */
    bool canReleaseLocksDuringExecution() const {
        switch (_policy) {
            case YieldPolicy::YIELD_AUTO:
            case YieldPolicy::YIELD_MANUAL:
            case YieldPolicy::ALWAYS_TIME_OUT:
            case YieldPolicy::ALWAYS_MARK_KILLED: {
                return true;
            }
            case YieldPolicy::NO_YIELD:
            case YieldPolicy::WRITE_CONFLICT_RETRY_ONLY:
            case YieldPolicy::INTERRUPT_ONLY: {
                return false;
            }
        }
        MONGO_UNREACHABLE;
    }

    /**
     * Returns true if this yield policy performs automatic yielding. Note 'yielding' here refers to
     * either releasing storage engine resources via abandonSnapshot() OR yielding LockManager
     * locks.
     */
    bool canAutoYield() const {
        switch (_policy) {
            case YieldPolicy::YIELD_AUTO:
            case YieldPolicy::WRITE_CONFLICT_RETRY_ONLY:
            case YieldPolicy::ALWAYS_TIME_OUT:
            case YieldPolicy::ALWAYS_MARK_KILLED: {
                return true;
            }
            case YieldPolicy::NO_YIELD:
            case YieldPolicy::YIELD_MANUAL:
            case YieldPolicy::INTERRUPT_ONLY:
                return false;
        }
        MONGO_UNREACHABLE;
    }

    PlanYieldPolicy::YieldPolicy getPolicy() const {
        return _policy;
    }

private:
    /**
     * Yields locks and calls 'abandonSnapshot()'. Calls 'whileYieldingFn()', if provided, while
     * locks are not held.
     */
    virtual Status yield(OperationContext* opCtx,
                         std::function<void()> whileYieldingFn = nullptr) = 0;

    /**
     * If the yield policy is INTERRUPT_ONLY, this is called prior to checking for interrupt.
     */
    virtual void preCheckInterruptOnly(OperationContext* opCtx) {}

    const YieldPolicy _policy;

    bool _forceYield = false;
    ElapsedTracker _elapsedTracker;
};

}  // namespace mongo
