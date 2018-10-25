
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kTransaction

#include "mongo/platform/basic.h"

#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/txn_two_phase_commit_cmds_gen.h"
#include "mongo/db/operation_context_session_mongod.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/transaction_coordinator_service.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(skipShardingPartsOfPrepareTransaction);

class PrepareTransactionCmd : public TypedCommand<PrepareTransactionCmd> {
public:
    class PrepareTimestamp {
    public:
        PrepareTimestamp(Timestamp timestamp) : _timestamp(std::move(timestamp)) {}
        void serialize(BSONObjBuilder* bob) const {
            bob->append("prepareTimestamp", _timestamp);
        }

    private:
        Timestamp _timestamp;
    };

    using Request = PrepareTransaction;
    using Response = PrepareTimestamp;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            // In production, only config servers or initialized shard servers can participate in a
            // sharded transaction. However, many test suites test the replication and storage parts
            // of prepareTransaction against a standalone replica set, so allow skipping the check.
            if (!MONGO_FAIL_POINT(skipShardingPartsOfPrepareTransaction)) {
                if (serverGlobalParams.clusterRole != ClusterRole::ConfigServer) {
                    uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());
                }
            }

            auto txnParticipant = TransactionParticipant::get(opCtx);
            uassert(ErrorCodes::CommandFailed,
                    "prepareTransaction must be run within a transaction",
                    txnParticipant);

            LOG(3)
                << "Participant shard received prepareTransaction for transaction with txnNumber "
                << opCtx->getTxnNumber() << " on session "
                << opCtx->getLogicalSessionId()->toBSON();

            uassert(ErrorCodes::CommandNotSupported,
                    "'prepareTransaction' is only supported in feature compatibility version 4.2",
                    (serverGlobalParams.featureCompatibility.getVersion() ==
                     ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo42));

            uassert(ErrorCodes::NoSuchTransaction,
                    "Transaction isn't in progress",
                    txnParticipant->inMultiDocumentTransaction());

            const auto& cmd = request();

            if (txnParticipant->transactionIsPrepared()) {
                auto& replClient = repl::ReplClientInfo::forClient(opCtx->getClient());
                auto prepareOpTime = txnParticipant->getPrepareOpTime();
                // Set the client optime to be prepareOpTime if it's not already later than
                // prepareOpTime. his ensures that we wait for writeConcern and that prepareOpTime
                // will be committed.
                if (prepareOpTime > replClient.getLastOp()) {
                    replClient.setLastOp(prepareOpTime);
                }

                invariant(opCtx->recoveryUnit()->getPrepareTimestamp() ==
                              prepareOpTime.getTimestamp(),
                          str::stream() << "recovery unit prepareTimestamp: "
                                        << opCtx->recoveryUnit()->getPrepareTimestamp().toString()
                                        << " participant prepareOpTime: "
                                        << prepareOpTime.toString());

                // A participant should re-send its vote if it re-received prepare.
                _sendVoteCommit(opCtx, prepareOpTime.getTimestamp(), cmd.getCoordinatorId());

                return PrepareTimestamp(prepareOpTime.getTimestamp());
            }

            // TODO (SERVER-36839): Pass coordinatorId into prepareTransaction() so that the
            // coordinatorId can be included in the write to config.transactions.
            const auto prepareTimestamp = txnParticipant->prepareTransaction(opCtx, {});
            _sendVoteCommit(opCtx, prepareTimestamp, cmd.getCoordinatorId());

            return PrepareTimestamp(prepareTimestamp);
        }

    private:
        void _sendVoteCommit(OperationContext* opCtx,
                             Timestamp prepareTimestamp,
                             ShardId coordinatorId) {
            // In a production cluster, a participant should always send its vote to the coordinator
            // as part of prepareTransaction. However, many test suites test the replication and
            // storage parts of prepareTransaction against a standalone replica set, so allow
            // skipping sending a vote.
            if (MONGO_FAIL_POINT(skipShardingPartsOfPrepareTransaction)) {
                return;
            }

            VoteCommitTransaction voteCommit;
            voteCommit.setDbName("admin");
            voteCommit.setShardId(ShardingState::get(opCtx)->shardId());
            voteCommit.setPrepareTimestamp(prepareTimestamp);
            BSONObj voteCommitObj = voteCommit.toBSON(
                BSON("lsid" << opCtx->getLogicalSessionId()->toBSON() << "txnNumber"
                            << *opCtx->getTxnNumber()
                            << "autocommit"
                            << false));
            _sendVote(opCtx, voteCommitObj, coordinatorId);
        }

        void _sendVoteAbort(OperationContext* opCtx, ShardId coordinatorId) {
            // In a production cluster, a participant should always send its vote to the coordinator
            // as part of prepareTransaction. However, many test suites test the replication and
            // storage parts of prepareTransaction against a standalone replica set, so allow
            // skipping sending a vote.
            if (MONGO_FAIL_POINT(skipShardingPartsOfPrepareTransaction)) {
                return;
            }

            VoteAbortTransaction voteAbort;
            voteAbort.setDbName("admin");
            voteAbort.setShardId(ShardingState::get(opCtx)->shardId());
            BSONObj voteAbortObj = voteAbort.toBSON(
                BSON("lsid" << opCtx->getLogicalSessionId()->toBSON() << "txnNumber"
                            << *opCtx->getTxnNumber()
                            << "autocommit"
                            << false));
            _sendVote(opCtx, voteAbortObj, coordinatorId);
        }

        void _sendVote(OperationContext* opCtx, const BSONObj& voteObj, ShardId coordinatorId) {
            try {
                // TODO (SERVER-37328): Participant should wait for writeConcern before sending its
                // vote.

                LOG(3) << "Participant shard sending " << voteObj << " to " << coordinatorId;

                const auto coordinatorPrimaryHost = [&] {
                    auto coordinatorShard = uassertStatusOK(
                        Grid::get(opCtx)->shardRegistry()->getShard(opCtx, coordinatorId));
                    return uassertStatusOK(coordinatorShard->getTargeter()->findHostNoWait(
                        ReadPreferenceSetting{ReadPreference::PrimaryOnly}));
                }();

                const executor::RemoteCommandRequest request(
                    coordinatorPrimaryHost,
                    NamespaceString::kAdminDb.toString(),
                    voteObj,
                    ReadPreferenceSetting{ReadPreference::PrimaryOnly}.toContainingBSON(),
                    opCtx,
                    executor::RemoteCommandRequest::kNoTimeout);

                auto noOp = [](const executor::TaskExecutor::RemoteCommandCallbackArgs&) {};
                uassertStatusOK(
                    Grid::get(opCtx)->getExecutorPool()->getFixedExecutor()->scheduleRemoteCommand(
                        request, noOp));
            } catch (const DBException& ex) {
                LOG(3) << "Participant shard failed to send " << voteObj << " to " << coordinatorId
                       << causedBy(ex.toStatus());
            }
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName(), "");
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {}
    };

    virtual bool adminOnly() const {
        return true;
    }

    std::string help() const override {
        return "Prepares a transaction on this shard; sent by a router or re-sent by the "
               "transaction commit coordinator for a cross-shard transaction";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
} prepareTransactionCmd;

class VoteCommitTransactionCmd : public TypedCommand<VoteCommitTransactionCmd> {
public:
    using Request = VoteCommitTransaction;
    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            // Only config servers or initialized shard servers can act as transaction coordinators.
            if (serverGlobalParams.clusterRole != ClusterRole::ConfigServer) {
                uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());
            }

            uassert(
                ErrorCodes::CommandNotSupported,
                "'voteCommitTransaction' is only supported in feature compatibility version 4.2",
                (serverGlobalParams.featureCompatibility.getVersion() ==
                 ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo42));

            const auto& cmd = request();

            LOG(3) << "Coordinator shard received voteCommit from " << cmd.getShardId()
                   << " with prepare timestamp " << cmd.getPrepareTimestamp() << " for transaction "
                   << opCtx->getTxnNumber() << " on session "
                   << opCtx->getLogicalSessionId()->toBSON();

            TransactionCoordinatorService::get(opCtx)->voteCommit(
                opCtx,
                opCtx->getLogicalSessionId().get(),
                opCtx->getTxnNumber().get(),
                cmd.getShardId(),
                cmd.getPrepareTimestamp());
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName(), "");
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {}
    };

    virtual bool adminOnly() const {
        return true;
    }

    std::string help() const override {
        return "Votes to commit a transaction; sent by a transaction participant to the "
               "transaction commit coordinator for a cross-shard transaction";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
} voteCommitTransactionCmd;

class VoteAbortTransactionCmd : public TypedCommand<VoteAbortTransactionCmd> {
public:
    using Request = VoteAbortTransaction;
    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            // Only config servers or initialized shard servers can act as transaction coordinators.
            if (serverGlobalParams.clusterRole != ClusterRole::ConfigServer) {
                uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());
            }

            uassert(ErrorCodes::CommandNotSupported,
                    "'voteAbortTransaction' is only supported in feature compatibility version 4.2",
                    (serverGlobalParams.featureCompatibility.getVersion() ==
                     ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo42));

            const auto& cmd = request();

            LOG(3) << "Coordinator shard received voteAbort from " << cmd.getShardId()
                   << " for transaction " << opCtx->getTxnNumber() << " on session "
                   << opCtx->getLogicalSessionId()->toBSON();

            TransactionCoordinatorService::get(opCtx)->voteAbort(opCtx,
                                                                 opCtx->getLogicalSessionId().get(),
                                                                 opCtx->getTxnNumber().get(),
                                                                 cmd.getShardId());
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName(), "");
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {}
    };

    virtual bool adminOnly() const {
        return true;
    }

    std::string help() const override {
        return "Votes to abort a transaction; sent by a transaction participant to the transaction "
               "commit coordinator for a cross-shard transaction";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
} voteAbortTransactionCmd;

// TODO (SERVER-37440): Make coordinateCommit idempotent.
class CoordinateCommitTransactionCmd : public TypedCommand<CoordinateCommitTransactionCmd> {
public:
    using Request = CoordinateCommitTransaction;
    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            // Only config servers or initialized shard servers can act as transaction coordinators.
            if (serverGlobalParams.clusterRole != ClusterRole::ConfigServer) {
                uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());
            }

            uassert(ErrorCodes::CommandNotSupported,
                    "'coordinateCommitTransaction' is only supported in feature compatibility "
                    "version 4.2",
                    (serverGlobalParams.featureCompatibility.getVersion() ==
                     ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo42));

            const auto& cmd = request();

            // Convert the participant list array into a set, and assert that all participants in
            // the list are unique.
            // TODO (PM-564): Propagate the 'readOnly' flag down into the TransactionCoordinator.
            std::set<ShardId> participantList;
            StringBuilder ss;
            ss << "[";
            for (const auto& participant : cmd.getParticipants()) {
                const auto shardId = participant.getShardId();
                uassert(ErrorCodes::InvalidOptions,
                        str::stream() << "participant list contained duplicate shardId " << shardId,
                        std::find(participantList.begin(), participantList.end(), shardId) ==
                            participantList.end());
                participantList.insert(shardId);
                ss << shardId << " ";
            }
            ss << "]";
            LOG(3) << "Coordinator shard received participant list with shards " << ss.str()
                   << " for transaction " << opCtx->getTxnNumber() << " on session "
                   << opCtx->getLogicalSessionId()->toBSON();

            auto commitDecisionFuture = TransactionCoordinatorService::get(opCtx)->coordinateCommit(
                opCtx,
                opCtx->getLogicalSessionId().get(),
                opCtx->getTxnNumber().get(),
                participantList);

            // If the commit decision is already available before we prepare locally, it means the
            // transaction has completed and we should skip preparing locally.
            //
            // TODO (SERVER-37440): Reconsider when coordinateCommit is made idempotent.
            if (!commitDecisionFuture.isReady()) {
                // Execute the 'prepare' logic on the local participant (the router does not send a
                // separate 'prepare' message to the coordinator shard).
                _callPrepareOnLocalParticipant(opCtx);
            }

            // Block waiting for the commit decision.
            auto commitDecision = commitDecisionFuture.get(opCtx);

            // If the decision was abort, propagate NoSuchTransaction exception back to mongos.
            uassert(ErrorCodes::NoSuchTransaction,
                    "Transaction was aborted",
                    commitDecision != TransactionCoordinatorService::CommitDecision::kAbort);
        }

    private:
        void _callPrepareOnLocalParticipant(OperationContext* opCtx) {
            auto localParticipantPrepareTimestamp = [&]() -> Timestamp {
                OperationSessionInfoFromClient sessionInfo;
                sessionInfo.setAutocommit(false);
                sessionInfo.setCoordinator(false);
                OperationContextSessionMongod checkOutSession(opCtx, true, sessionInfo);

                auto txnParticipant = TransactionParticipant::get(opCtx);

                txnParticipant->unstashTransactionResources(opCtx, "prepareTransaction");
                ScopeGuard guard = MakeGuard([&txnParticipant, opCtx]() {
                    txnParticipant->abortActiveUnpreparedOrStashPreparedTransaction(opCtx);
                });

                auto prepareTimestamp = txnParticipant->prepareTransaction(opCtx, {});

                txnParticipant->stashTransactionResources(opCtx);
                guard.Dismiss();
                return prepareTimestamp;
            }();

            LOG(3) << "Participant shard delivering voteCommit with prepareTimestamp "
                   << localParticipantPrepareTimestamp << " to local coordinator for transaction "
                   << opCtx->getTxnNumber() << " on session "
                   << opCtx->getLogicalSessionId()->toBSON();

            // Deliver the local participant's vote to the coordinator.
            TransactionCoordinatorService::get(opCtx)->voteCommit(
                opCtx,
                opCtx->getLogicalSessionId().get(),
                opCtx->getTxnNumber().get(),
                ShardingState::get(opCtx)->shardId(),
                localParticipantPrepareTimestamp);
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName(), "");
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {}
    };

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Coordinates the commit for a transaction. Only called by mongos.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
} coordinateCommitTransactionCmd;

}  // namespace
}  // namespace mongo
