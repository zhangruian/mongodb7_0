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

#include <memory>
#include <queue>

#include "mongo/executor/task_executor.h"
#include "mongo/s/query/cluster_client_cursor.h"
#include "mongo/s/query/cluster_client_cursor_guard.h"
#include "mongo/s/query/cluster_client_cursor_params.h"
#include "mongo/s/query/cluster_query_result.h"
#include "mongo/s/query/router_exec_stage.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

class RouterStageMock;

class ClusterClientCursorImpl final : public ClusterClientCursor {
    ClusterClientCursorImpl(const ClusterClientCursorImpl&) = delete;
    ClusterClientCursorImpl& operator=(const ClusterClientCursorImpl&) = delete;

public:
    /**
     * Constructs a cluster query plan and CCC from the given parameters whose safe cleanup is
     * ensured by an RAII object.
     */
    static ClusterClientCursorGuard make(OperationContext* opCtx,
                                         std::shared_ptr<executor::TaskExecutor> executor,
                                         ClusterClientCursorParams&& params);

    /**
     * Constructs a CCC from the given execution tree 'root'. The CCC's safe cleanup is ensured by
     * an RAII object.
     */
    static ClusterClientCursorGuard make(OperationContext* opCtx,
                                         std::unique_ptr<RouterExecStage> root,
                                         ClusterClientCursorParams&& params);

    StatusWith<ClusterQueryResult> next() final;

    void kill(OperationContext* opCtx) final;

    void reattachToOperationContext(OperationContext* opCtx) final;

    void detachFromOperationContext() final;

    OperationContext* getCurrentOperationContext() const final;

    bool isTailable() const final;

    bool isTailableAndAwaitData() const final;

    BSONObj getOriginatingCommand() const final;

    const PrivilegeVector& getOriginatingPrivileges() const& final;
    void getOriginatingPrivileges() && = delete;

    bool partialResultsReturned() const final;

    std::size_t getNumRemotes() const final;

    BSONObj getPostBatchResumeToken() const final;

    long long getNumReturnedSoFar() const final;

    void queueResult(const ClusterQueryResult& result) final;

    bool remotesExhausted() final;

    Status setAwaitDataTimeout(Milliseconds awaitDataTimeout) final;

    boost::optional<LogicalSessionId> getLsid() const final;

    boost::optional<TxnNumber> getTxnNumber() const final;

    APIParameters getAPIParameters() const final;

    boost::optional<ReadPreferenceSetting> getReadPreference() const final;

    boost::optional<repl::ReadConcernArgs> getReadConcern() const final;

    Date_t getCreatedDate() const final;

    Date_t getLastUseDate() const final;

    void setLastUseDate(Date_t now) final;

    boost::optional<uint32_t> getQueryHash() const final;

public:
    /**
     * Constructs a CCC whose result set is generated by a mock execution stage.
     */
    ClusterClientCursorImpl(OperationContext* opCtx,
                            std::unique_ptr<RouterExecStage> root,
                            ClusterClientCursorParams&& params,
                            boost::optional<LogicalSessionId> lsid);

    /**
     * Constructs a cluster client cursor.
     */
    ClusterClientCursorImpl(OperationContext* opCtx,
                            std::shared_ptr<executor::TaskExecutor> executor,
                            ClusterClientCursorParams&& params,
                            boost::optional<LogicalSessionId> lsid);

    ~ClusterClientCursorImpl() final;

private:
    /**
     * Constructs the pipeline of MergerPlanStages which will be used to answer the query.
     */
    std::unique_ptr<RouterExecStage> buildMergerPlan(
        OperationContext* opCtx,
        std::shared_ptr<executor::TaskExecutor> executor,
        ClusterClientCursorParams* params);

    ClusterClientCursorParams _params;

    // Number of documents already returned by next().
    long long _numReturnedSoFar = 0;

    // The root stage of the pipeline used to return the result set, merged from the remote nodes.
    std::unique_ptr<RouterExecStage> _root;

    // Stores documents queued by queueResult(). BSONObjs within the stashed results must be owned.
    std::queue<ClusterQueryResult> _stash;

    // Stores the logical session id for this cursor.
    boost::optional<LogicalSessionId> _lsid;

    // The OperationContext that we're executing within. This can be updated if necessary by using
    // detachFromOperationContext() and reattachToOperationContext().
    OperationContext* _opCtx = nullptr;

    // The time the cursor was created.
    Date_t _createdDate;

    // The time when the cursor was last unpinned, i.e. the end of the last getMore.
    Date_t _lastUseDate;

    // The hash of the query shape to be used for slow query logging;
    boost::optional<uint32_t> _queryHash;

    // Whether ClusterClientCursor::next() was interrupted due to MaxTimeMSExpired.
    bool _maxTimeMSExpired = false;

    // If boost::none, telemetry should not be collected for this cursor.
    boost::optional<BSONObj> _telemetryStoreKey;

    // Tracks if kill() has been called on the cursor. Multiple calls to kill() are treated as a
    // noop.
    // TODO SERVER-74482 investigate where kill() is called multiple times and remove unnecessary
    // calls
    bool _hasBeenKilled = false;
};

}  // namespace mongo
