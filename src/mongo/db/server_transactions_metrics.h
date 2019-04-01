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

#include <set>

#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/service_context.h"
#include "mongo/db/transactions_stats_gen.h"
#include "mongo/util/concurrency/with_lock.h"

namespace mongo {

/**
 * Container for server-wide multi-document transaction statistics.
 */
class ServerTransactionsMetrics {
    ServerTransactionsMetrics(const ServerTransactionsMetrics&) = delete;
    ServerTransactionsMetrics& operator=(const ServerTransactionsMetrics&) = delete;

public:
    ServerTransactionsMetrics() = default;

    static ServerTransactionsMetrics* get(ServiceContext* service);
    static ServerTransactionsMetrics* get(OperationContext* opCtx);

    unsigned long long getCurrentActive() const;
    void decrementCurrentActive();
    void incrementCurrentActive();

    unsigned long long getCurrentInactive() const;
    void decrementCurrentInactive();
    void incrementCurrentInactive();

    unsigned long long getCurrentOpen() const;
    void decrementCurrentOpen();
    void incrementCurrentOpen();

    unsigned long long getTotalStarted() const;
    void incrementTotalStarted();

    unsigned long long getTotalAborted() const;
    void incrementTotalAborted();

    unsigned long long getTotalCommitted() const;
    void incrementTotalCommitted();

    unsigned long long getTotalPrepared() const;
    void incrementTotalPrepared();

    unsigned long long getTotalPreparedThenCommitted() const;
    void incrementTotalPreparedThenCommitted();

    unsigned long long getTotalPreparedThenAborted() const;
    void incrementTotalPreparedThenAborted();

    unsigned long long getCurrentPrepared() const;
    void incrementCurrentPrepared();
    void decrementCurrentPrepared();

    /**
     * Returns the OpTime of the oldest oplog entry written across all open transactions.
     * Returns boost::none if there are no transaction oplog entry OpTimes stored.
     */
    boost::optional<repl::OpTime> getOldestActiveOpTime() const;

    /**
     * Add the transaction's oplog entry OpTime to a set of OpTimes.
     */
    void addActiveOpTime(repl::OpTime oldestOplogEntryOpTime);

    /**
     * Remove the corresponding transaction oplog entry OpTime if the transaction commits or
     * aborts.
     */
    void removeActiveOpTime(repl::OpTime oldestOplogEntryOpTime);

    /**
     * Returns the number of transaction oplog entry OpTimes currently stored.
     */
    unsigned int getTotalActiveOpTimes() const;

    /**
     * Appends the accumulated stats to a transactions stats object.
     */
    void updateStats(TransactionsStats* stats, OperationContext* opCtx);

    /**
     * Invalidates the in-memory state of prepared transactions during replication rollback by
     * clearing _oldestActiveOplogEntryOpTimes. This data structure should be properly reconstructed
     * during replication recovery.
     */
    void clearOpTimes();

private:
    /**
     * Returns the oldest read timestamp in use by any open unprepared transaction. This will
     * return a null timestamp if there is no oldest open unprepared read timestamp to be
     * returned.
     */
    static Timestamp _getOldestOpenUnpreparedReadTimestamp(OperationContext* opCtx);

    //
    // Member variables, excluding atomic variables, are labeled with the following code to
    // indicate the synchronization rules for accessing them.
    //
    // (M)  Reads and writes guarded by _mutex
    //
    mutable stdx::mutex _mutex;

    // The number of multi-document transactions currently active.
    AtomicWord<unsigned long long> _currentActive{0};

    // The number of multi-document transactions currently inactive.
    AtomicWord<unsigned long long> _currentInactive{0};

    // The total number of open transactions.
    AtomicWord<unsigned long long> _currentOpen{0};

    // The total number of multi-document transactions started since the last server startup.
    AtomicWord<unsigned long long> _totalStarted{0};

    // The total number of multi-document transaction aborts.
    AtomicWord<unsigned long long> _totalAborted{0};

    // The total number of multi-document transaction commits.
    AtomicWord<unsigned long long> _totalCommitted{0};

    // The total number of prepared transactions since the last server startup.
    AtomicWord<unsigned long long> _totalPrepared{0};

    // The total number of prepared transaction commits.
    AtomicWord<unsigned long long> _totalPreparedThenCommitted{0};

    // The total number of prepared transaction aborts.
    AtomicWord<unsigned long long> _totalPreparedThenAborted{0};

    // The current number of transactions in the prepared state.
    AtomicWord<unsigned long long> _currentPrepared{0};

    // Maintain the oldest oplog entry OpTime across all active transactions. Currently, we only
    // write an oplog entry for an ongoing transaction if it is in the `prepare` state. By
    // maintaining an ordered set of OpTimes, the OpTime at the beginning will be the oldest.
    std::set<repl::OpTime> _oldestActiveOplogEntryOpTimes;  // (M)
};

}  // namespace mongo
