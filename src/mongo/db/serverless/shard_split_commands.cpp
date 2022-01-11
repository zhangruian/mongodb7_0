/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/client/replica_set_monitor_server_parameters.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/serverless/shard_split_commands_gen.h"
#include "mongo/db/serverless/shard_split_donor_service.h"
#include "mongo/util/fail_point.h"

namespace mongo {
namespace {

class CommitShardSplitCmd : public TypedCommand<CommitShardSplitCmd> {
public:
    using Request = CommitShardSplit;
    using Response = CommitShardSplitResponse;

    class Invocation : public InvocationBase {

    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            uassert(6057900,
                    "feature \"shard split\" not supported",
                    repl::feature_flags::gShardSplit.isEnabled(
                        serverGlobalParams.featureCompatibility));
            uassert(ErrorCodes::IllegalOperation,
                    "shard split is not available on config servers",
                    serverGlobalParams.clusterRole == ClusterRole::None ||
                        serverGlobalParams.clusterRole == ClusterRole::ShardServer);
            // TODO SERVER-62079 : Remove check for scanning RSM as it does not exist anymore.
            uassert(
                6142502,
                "feature \"shard split\" not supported when started with \"scanning\" replica set "
                "monitor mode.",
                gReplicaSetMonitorProtocol != ReplicaSetMonitorProtocol::kScanning);

            const auto& cmd = request();
            auto stateDoc = ShardSplitDonorDocument(cmd.getMigrationId());
            stateDoc.setTenantIds(cmd.getTenantIds());
            stateDoc.setRecipientConnectionString(cmd.getRecipientConnectionString());

            opCtx->setAlwaysInterruptAtStepDownOrUp();

            auto donorService = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext())
                                    ->lookupServiceByName(ShardSplitDonorService::kServiceName);

            auto donorPtr = ShardSplitDonorService::DonorStateMachine::getOrCreate(
                opCtx, donorService, stateDoc.toBSON());
            invariant(donorPtr);

            uassertStatusOK(donorPtr->checkIfOptionsConflict(stateDoc));

            auto state = donorPtr->completionFuture().get(opCtx);

            auto response = Response(state.state);
            if (state.abortReason) {
                BSONObjBuilder bob;
                state.abortReason->serializeErrorToBSON(&bob);
                response.setAbortReason(bob.obj());
            }

            return response;
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const final {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::runTenantMigration));
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        NamespaceString ns() const {
            return NamespaceString(request().getDbName(), "");
        }
    };

    std::string help() const {
        return "Start an opereation to split a shard into its own slice.";
    }

    bool adminOnly() const override {
        return true;
    }

    BasicCommand::AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return BasicCommand::AllowedOnSecondary::kNever;
    }
} commitShardSplitCmd;

class AbortShardSplitCmd : public TypedCommand<AbortShardSplitCmd> {
public:
    using Request = AbortShardSplit;

    class Invocation : public InvocationBase {

    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(6057902,
                    "feature \"shard split\" not supported",
                    repl::feature_flags::gShardSplit.isEnabled(
                        serverGlobalParams.featureCompatibility));
            // TODO SERVER-62079 : Remove check for scanning RSM as it does not exist anymore.
            uassert(
                6142506,
                "feature \"shard split\" not supported when started with \"scanning\" replica set "
                "monitor mode.",
                gReplicaSetMonitorProtocol != ReplicaSetMonitorProtocol::kScanning);

            const RequestType& cmd = request();

            opCtx->setAlwaysInterruptAtStepDownOrUp();

            auto splitService = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext())
                                    ->lookupServiceByName(ShardSplitDonorService::kServiceName);
            auto instance = ShardSplitDonorService::DonorStateMachine::getOrCreate(
                opCtx, splitService, BSON("_id" << cmd.getMigrationId()));

            invariant(instance);

            instance->tryAbort();

            auto state = instance->completionFuture().get(opCtx);

            uassert(ErrorCodes::CommandFailed,
                    "Failed to abort shard split",
                    state.abortReason &&
                        state.abortReason.get() == ErrorCodes::TenantMigrationAborted);

            uassert(ErrorCodes::TenantMigrationCommitted,
                    "Failed to abort : shard split already committed",
                    state.state == ShardSplitDonorStateEnum::kAborted);
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const final {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::runTenantMigration));
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        NamespaceString ns() const {
            return NamespaceString(request().getDbName(), "");
        }
    };

    std::string help() const override {
        return "Abort a shard split operation.";
    }

    bool adminOnly() const override {
        return true;
    }

    BasicCommand::AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return BasicCommand::AllowedOnSecondary::kNever;
    }
} abortShardSplitCmd;

}  // namespace
}  // namespace mongo
