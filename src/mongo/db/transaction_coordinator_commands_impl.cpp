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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kTransaction

#include "mongo/platform/basic.h"

#include "mongo/db/transaction_coordinator_commands_impl.h"

#include "mongo/db/operation_context_session_mongod.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {

std::vector<ShardId> sendCommit(OperationContext* opCtx, std::set<ShardId>& nonAckedParticipants) {
    StringBuilder ss;
    ss << "[";

    std::vector<AsyncRequestsSender::Request> requests;
    for (const auto& shardId : nonAckedParticipants) {
        // TODO (SERVER-36584): Use the commitTransaction IDL to create the command BSON.
        requests.emplace_back(shardId,
                              BSON("commitTransaction" << 1 << "lsid"
                                                       << opCtx->getLogicalSessionId()->toBSON()
                                                       << "txnNumber"
                                                       << *opCtx->getTxnNumber()
                                                       << "autocommit"
                                                       << false));
        ss << shardId << " ";
    }

    // TODO (SERVER-36687): Remove log line or demote to lower log level once cross-shard
    // transactions are stable.
    ss << "]";
    LOG(0) << "Coordinator shard sending commitTransaction to " << ss.str();

    // TODO (SERVER-36638): Change to arbitrary task executor? Unit test only supports fixed
    // executor.
    AsyncRequestsSender ars(opCtx,
                            Grid::get(opCtx)->getExecutorPool()->getFixedExecutor(),
                            "admin",
                            requests,
                            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                            Shard::RetryPolicy::kIdempotent);

    std::vector<ShardId> ackedParticipants;
    while (!ars.done()) {
        auto response = ars.next();

        if (response.swResponse.getStatus().isOK()) {
            auto commandStatus = getStatusFromCommandResult(response.swResponse.getValue().data);

            // TODO (SERVER-36642): Also interpret TransactionTooOld as acknowledgment.
            if (commandStatus.isOK()) {
                ackedParticipants.push_back(response.shardId);
            }

            // TODO (SERVER-36687): Remove log line or demote to lower log level once cross-shard
            // transactions are stable.
            LOG(0) << "Coordinator shard got response " << commandStatus
                   << " for commitTransaction to " << response.shardId;
        } else {
            // TODO (SERVER-36687): Remove log line or demote to lower log level once cross-shard
            // transactions are stable.
            LOG(0) << "Coordinator shard got response " << response.swResponse.getStatus()
                   << " for commitTransaction to " << response.shardId;
        }
    }
    return ackedParticipants;
}

std::vector<ShardId> sendAbort(OperationContext* opCtx, std::set<ShardId>& nonAckedParticipants) {
    StringBuilder ss;
    ss << "[";

    std::vector<AsyncRequestsSender::Request> requests;
    for (const auto& shardId : nonAckedParticipants) {
        // TODO (SERVER-36584) Use IDL to create command BSON.
        requests.emplace_back(shardId,
                              BSON("abortTransaction" << 1 << "lsid"
                                                      << opCtx->getLogicalSessionId()->toBSON()
                                                      << "txnNumber"
                                                      << *opCtx->getTxnNumber()
                                                      << "autocommit"
                                                      << false));
        ss << shardId << " ";
    }

    // TODO (SERVER-36687): Remove log line or demote to lower log level once cross-shard
    // transactions are stable.
    ss << "]";
    LOG(0) << "Coordinator shard sending abortTransaction to " << ss.str();

    // TODO (SERVER-36638): Change to arbitrary task executor? Unit test only supports fixed
    // executor.
    AsyncRequestsSender ars(opCtx,
                            Grid::get(opCtx)->getExecutorPool()->getFixedExecutor(),
                            "admin",
                            requests,
                            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                            Shard::RetryPolicy::kIdempotent);

    // TODO (SERVER-36638): The ARS does not currently support "fire-and-forget" messages; the ARS
    // uses the caller's thread to send the messages over the network inside calls to next().
    std::vector<ShardId> ackedParticipants;
    while (!ars.done()) {
        auto response = ars.next();

        if (response.swResponse.getStatus().isOK()) {
            auto commandStatus = getStatusFromCommandResult(response.swResponse.getValue().data);

            // TODO (SERVER-36642): Also interpret NoSuchTransaction and TransactionTooOld as
            // acknowledgment.
            if (commandStatus.isOK()) {
                ackedParticipants.push_back(response.shardId);
            }

            // TODO (SERVER-36687): Remove log line or demote to lower log level once cross-shard
            // transactions are stable.
            LOG(0) << "Coordinator shard got response " << commandStatus
                   << " for abortTransaction to " << response.shardId;
        } else {
            // TODO (SERVER-36687): Remove log line or demote to lower log level once cross-shard
            // transactions are stable.
            LOG(0) << "Coordinator shard got response " << response.swResponse.getStatus()
                   << " for abortTransaction to " << response.shardId;
        }
    }
    return ackedParticipants;
}

void doAction(OperationContext* opCtx, TransactionCoordinator::StateMachine::Action action) {
    switch (action) {
        case TransactionCoordinator::StateMachine::Action::kSendCommit: {
            std::set<ShardId> nonAckedParticipants;
            {
                OperationContextSessionMongod checkOutSession(
                    opCtx, true, false, boost::none, true);
                nonAckedParticipants =
                    TransactionCoordinator::get(opCtx)->getNonAckedCommitParticipants();
            }

            // TODO (SERVER-36638): Spawn a separate thread to do this so that the client's thread
            // does not block.

            auto ackedParticipants = sendCommit(opCtx, nonAckedParticipants);

            {
                OperationContextSessionMongod checkOutSession(
                    opCtx, true, false, boost::none, true);
                auto& coordinator = TransactionCoordinator::get(opCtx);
                for (auto& participant : ackedParticipants) {
                    coordinator->recvCommitAck(participant);
                }
            }

            return;
        }
        case TransactionCoordinator::StateMachine::Action::kSendAbort: {
            std::set<ShardId> nonAckedParticipants;
            {
                OperationContextSessionMongod checkOutSession(
                    opCtx, true, false, boost::none, true);
                nonAckedParticipants =
                    TransactionCoordinator::get(opCtx)->getNonAckedAbortParticipants();
            }

            // TODO (SERVER-36638): Spawn a separate thread to do this so that the client's thread
            // does not block.

            auto ackedParticipants = sendAbort(opCtx, nonAckedParticipants);

            {
                OperationContextSessionMongod checkOutSession(
                    opCtx, true, false, boost::none, true);
                auto& coordinator = TransactionCoordinator::get(opCtx);
                for (auto& participant : ackedParticipants) {
                    coordinator->recvAbortAck(participant);
                }
            }

            return;
        }
        case TransactionCoordinator::StateMachine::Action::kNone:
            return;
    }
    MONGO_UNREACHABLE;
}

}  // namespace

namespace txn {

void recvCoordinateCommit(OperationContext* opCtx, const std::set<ShardId>& participantList) {
    // TODO (SERVER-36687): Remove log line or demote to lower log level once cross-shard
    // transactions are stable.
    StringBuilder ss;
    ss << "[";
    for (const auto& shardId : participantList) {
        ss << shardId << " ";
    }
    ss << "]";
    LOG(0) << "Coordinator shard received participant list with shards " << ss.str();

    TransactionCoordinator::StateMachine::Action action;
    {
        OperationContextSessionMongod checkOutSession(opCtx, true, false, boost::none, true);
        action = TransactionCoordinator::get(opCtx)->recvCoordinateCommit(participantList);
    }

    doAction(opCtx, action);

    // TODO (SERVER-36640): Wait for decision to be made.
}

void recvVoteCommit(OperationContext* opCtx, const ShardId& shardId, int prepareTimestamp) {
    // TODO (SERVER-36687): Remove log line or demote to lower log level once cross-shard
    // transactions are stable.
    LOG(0) << "Coordinator shard received voteCommit from " << shardId;

    TransactionCoordinator::StateMachine::Action action;
    {
        OperationContextSessionMongod checkOutSession(opCtx, true, false, boost::none, true);
        action = TransactionCoordinator::get(opCtx)->recvVoteCommit(shardId, prepareTimestamp);
    }

    doAction(opCtx, action);
}

void recvVoteAbort(OperationContext* opCtx, const ShardId& shardId) {
    // TODO (SERVER-36687): Remove log line or demote to lower log level once cross-shard
    // transactions are stable.
    LOG(0) << "Coordinator shard received voteAbort from " << shardId;

    TransactionCoordinator::StateMachine::Action action;
    {
        OperationContextSessionMongod checkOutSession(opCtx, true, false, boost::none, true);
        action = TransactionCoordinator::get(opCtx)->recvVoteAbort(shardId);
    }

    doAction(opCtx, action);
}

}  // namespace txn
}  // namespace mongo
