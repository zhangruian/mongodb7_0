/**
 *    Copyright (C) 2018 MongoDB Inc.
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

#include <memory>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/transaction_coordinator_catalog.h"


namespace mongo {

class ShardId;
class OperationContext;
class ServiceContext;

// TODO (SERVER-37017): Update comments to reflect asynchronicity of relevant methods.
class TransactionCoordinatorService final {
    MONGO_DISALLOW_COPYING(TransactionCoordinatorService);

public:
    enum class CommitDecision {
        kCommit,
        kAbort,
    };

    TransactionCoordinatorService();
    ~TransactionCoordinatorService();

    /**
     * Retrieves the TransactionCoordinatorService associated with the service or operation context.
     */
    static TransactionCoordinatorService* get(OperationContext* opCtx);
    static TransactionCoordinatorService* get(ServiceContext* serviceContext);

    /**
     * Creates a new TransactionCoordinator for the given session id and transaction number, with a
     * deadline for the commit decision. If the coordinator has not decided to commit by that
     * deadline, it will abort.
     */
    void createCoordinator(LogicalSessionId lsid, TxnNumber txnNumber, Date_t commitDeadline);

    /**
     * Delivers coordinateCommit to the TransactionCoordinator.
     *
     * TODO (SERVER-36640): Return Notification<CommitDecision>.
     */
    CommitDecision coordinateCommit(OperationContext* opCtx,
                                    LogicalSessionId lsid,
                                    TxnNumber txnNumber,
                                    const std::set<ShardId>& participantList);

    /**
     * Delivers voteCommit to the TransactionCoordinator.
     */
    void voteCommit(OperationContext* opCtx,
                    LogicalSessionId lsid,
                    TxnNumber txnNumber,
                    const ShardId& shardId,
                    Timestamp prepareTimestamp);

    /**
     * Delivers voteAbort on the TransactionCoordinator.
     */
    void voteAbort(OperationContext* opCtx,
                   LogicalSessionId lsid,
                   TxnNumber txnNumber,
                   const ShardId& shardId);

    /**
     * Attempts to abort the coordinator for the given session id and transaction
     * number. Will not abort a coordinator which has already decided to commit.
     */
    void tryAbort(OperationContext* opCtx, LogicalSessionId lsid, TxnNumber txnNumber);

private:
    TransactionCoordinatorCatalog _coordinatorCatalog;
};

}  // namespace mongo
