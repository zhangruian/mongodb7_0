/**
 *    Copyright (C) 2018 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/optional.hpp>
#include <map>
#include <memory>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/shard_id.h"
#include "mongo/util/string_map.h"

namespace mongo {

/**
 * Keeps track of the transaction state. A session is in use when it is being used by a request.
 */
class TransactionRouter {
public:
    // The default value to use as the statement id of the first command in the transaction if none
    // was sent.
    static const StmtId kDefaultFirstStmtId = 0;

    /**
     * Represents the options for a transaction that are shared across all participants. These
     * cannot be changed without restarting the transactions that may have already been begun on
     * every participant, i.e. clearing the current participant list.
     */
    struct SharedTransactionOptions {
        // Set for all distributed transactions.
        TxnNumber txnNumber;
        repl::ReadConcernArgs readConcernArgs;

        // Only set for transactions with snapshot level read concern.
        boost::optional<LogicalTime> atClusterTime;
    };

    /**
     * Represents a shard participant in a distributed transaction. Lives only for the duration of
     * the transaction that created it.
     */
    class Participant {
    public:
        explicit Participant(bool isCoordinator,
                             StmtId stmtIdCreatedAt,
                             SharedTransactionOptions sharedOptions);

        enum class State {
            // Next transaction should include startTransaction.
            kMustStart,
            // startTransaction already sent to this participant.
            kStarted,
        };

        /**
         * Attaches necessary fields if this is participating in a multi statement transaction.
         */
        BSONObj attachTxnFieldsIfNeeded(BSONObj cmd) const;

        /**
         * True if the participant has been chosen as the coordinator for its transaction.
         */
        bool isCoordinator() const;

        /**
         * True if the represented shard has not been sent a command with startTransaction=true.
         */
        bool mustStartTransaction() const;

        /**
         * Mark this participant as a node that has been successful sent a command with
         * startTransaction=true.
         */
        void markAsCommandSent();

        /**
         * Returns the highest statement id of the command during which this participant was
         * created.
         */
        StmtId getStmtIdCreatedAt() const;

    private:
        State _state{State::kMustStart};
        const bool _isCoordinator{false};

        // The highest statement id of the request during which this participant was created.
        const StmtId _stmtIdCreatedAt{kUninitializedStmtId};

        const SharedTransactionOptions _sharedOptions;
    };

    TransactionRouter(LogicalSessionId sessionId);

    /**
     * Starts a fresh transaction in this session or continue an existing one. Also cleans up the
     * previous transaction state.
     */
    void beginOrContinueTxn(OperationContext* opCtx, TxnNumber txnNumber, bool startTransaction);

    /**
     * Returns the participant for this transaction. Creates a new one if it doesn't exist.
     */
    Participant& getOrCreateParticipant(const ShardId& shard);

    void checkIn();
    void checkOut();

    /**
     * Updates the transaction state to allow for a retry of the current command on a stale version
     * error. Will throw if the transaction cannot be continued.
     */
    void onStaleShardOrDbError(StringData cmdName);

    /**
     * Resets the transaction state to allow for a retry attempt. This includes clearing all
     * participants, clearing the coordinator, and resetting the global read timestamp. Will throw
     * if the transaction cannot be continued.
     */
    void onSnapshotError();

    /**
     * Computes and sets the atClusterTime for the current transaction. Does nothing if the given
     * query is not the first statement that this transaction runs (i.e. if the atClusterTime
     * has already been set).
     */
    void computeAtClusterTime(OperationContext* opCtx,
                              bool mustRunOnAll,
                              const std::set<ShardId>& shardIds,
                              const NamespaceString& nss,
                              const BSONObj query,
                              const BSONObj collation);

    /**
     * Computes and sets the atClusterTime for the current transaction if it targets the
     * given shard during its first statement. Does nothing if the atClusterTime has already
     * been set.
     */
    void computeAtClusterTimeForOneShard(OperationContext* opCtx, const ShardId& shardId);

    /**
     * Sets the atClusterTime for the current transaction to the latest time in the router's logical
     * clock.
     */
    void setAtClusterTimeToLatestTime(OperationContext* opCtx);

    bool isCheckedOut();

    const LogicalSessionId& getSessionId() const;

    boost::optional<ShardId> getCoordinatorId() const;

    /**
     * Commits the transaction. For transactions with multiple participants, this will initiate
     * the two phase commit procedure.
     */
    Shard::CommandResponse commitTransaction(OperationContext* opCtx);

    /**
     * Sends abort to all participants and returns the responses from all shards.
     */
    std::vector<AsyncRequestsSender::Response> abortTransaction(OperationContext* opCtx);

    /**
     * Extract the runtimne state attached to the operation context. Returns nullptr if none is
     * attached.
     */
    static TransactionRouter* get(OperationContext* opCtx);

private:
    /**
     * Run basic commit for transactions that touched a single shard.
     */
    Shard::CommandResponse _commitSingleShardTransaction(OperationContext* opCtx);

    /**
     * Run two phase commit for transactions that touched multiple shards.
     */
    Shard::CommandResponse _commitMultiShardTransaction(OperationContext* opCtx);

    /**
     * Returns true if the current transaction can retry on a stale version error from a contacted
     * shard. This is always true except for an error received by a write that is not the first
     * overall statement in the sharded transaction. This is because the entire command will be
     * retried, and shards that were not stale and are targeted again may incorrectly execute the
     * command a second time.
     *
     * Note: Even if this method returns true, the retry attempt may still fail, e.g. if one of the
     * shards that returned a stale version error was involved in a previously completed a statement
     * for this transaction.
     *
     * TODO SERVER-37207: Change batch writes to retry only the failed writes in a batch, to allow
     * retrying writes beyond the first overall statement.
     */
    bool _canContinueOnStaleShardOrDbError(StringData cmdName) const;

    /**
     * Returns true if the current transaction can retry on a snapshot error. This is only true on
     * the first command recevied for a transaction.
     */
    bool _canContinueOnSnapshotError() const;

    /**
     * Removes all participants created during the current statement from the participant list.
     */
    void _clearPendingParticipants();

    const LogicalSessionId _sessionId;
    TxnNumber _txnNumber{kUninitializedTxnNumber};

    // True if this is currently being used by a request.
    bool _isCheckedOut{false};

    // Map of current participants of the current transaction.
    StringMap<Participant> _participants;

    // The id of coordinator participant, used to construct prepare requests.
    boost::optional<ShardId> _coordinatorId;

    // The read concern the current transaction was started with.
    repl::ReadConcernArgs _readConcernArgs;

    // The cluster time of the timestamp all participant shards in the current transaction with
    // snapshot level read concern must read from. Selected during the first statement of the
    // transaction. Should not be changed after the first statement has completed successfully.
    boost::optional<LogicalTime> _atClusterTime;

    // The statement id of the latest received command for this transaction. For batch writes, this
    // will be the highest stmtId contained in the batch. Incremented by one if new commands do not
    // contain statement ids.
    StmtId _latestStmtId = kUninitializedStmtId;

    // The statement id of the command that began this transaction. Defaults to zero if no statement
    // id was included in the first command.
    StmtId _firstStmtId = kUninitializedStmtId;
};

/**
 * Scoped object, which checks out the session specified in the passed operation context and stores
 * it for later access by the command. The session is installed at construction time and is removed
 * at destruction. This can only be used for multi-statement transactions.
 */
class ScopedRouterSession {
    MONGO_DISALLOW_COPYING(ScopedRouterSession);

public:
    ScopedRouterSession(OperationContext* opCtx);
    ~ScopedRouterSession();

private:
    OperationContext* const _opCtx;
};

}  // namespace mongo
