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


#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/cloner.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/write_block_bypass.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/clone_catalog_data_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

void cloneDatabase(OperationContext* opCtx,
                   const std::string& dbName,
                   StringData from,
                   BSONObjBuilder& result) {
    auto const catalogClient = Grid::get(opCtx)->catalogClient();
    auto shardedColls = catalogClient->getAllShardedCollectionsForDb(
        opCtx, dbName, repl::ReadConcernLevel::kMajorityReadConcern, {});

    DisableDocumentValidation disableValidation(opCtx);

    // Clone the non-ignored collections.
    std::set<std::string> clonedColls;

    Cloner cloner;
    uassertStatusOK(cloner.copyDb(opCtx, dbName, from.toString(), shardedColls, &clonedColls));

    {
        BSONArrayBuilder cloneBarr = result.subarrayStart("clonedColls");
        cloneBarr.append(clonedColls);
    }
}

/**
 * Currently, _shardsvrCloneCatalogData will clone all data (including metadata). In the second part
 * of
 * PM-1017 (Introduce Database Versioning in Sharding Config) this command will be changed to only
 * clone catalog metadata, as the name would suggest.
 */
class CloneCatalogDataCommand : public BasicCommand {
public:
    CloneCatalogDataCommand() : BasicCommand("_shardsvrCloneCatalogData", "_cloneCatalogData") {}

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    virtual bool supportsRetryableWrite() const override {
        return true;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        if (!AuthorizationSession::get(opCtx->getClient())
                 ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                    ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto shardingState = ShardingState::get(opCtx);
        uassertStatusOK(shardingState->canAcceptShardedCommands());

        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "_shardsvrCloneCatalogData can only be run on shard servers",
                serverGlobalParams.clusterRole.has(ClusterRole::ShardServer));

        CommandHelpers::uassertCommandRunWithMajority(getName(), opCtx->getWriteConcern());

        const auto cloneCatalogDataRequest =
            CloneCatalogData::parse(IDLParserContext("_shardsvrCloneCatalogData"), cmdObj);
        const auto dbname = cloneCatalogDataRequest.getCommandParameter().toString();

        uassert(
            ErrorCodes::InvalidNamespace,
            str::stream() << "invalid db name specified: " << dbname,
            NamespaceString::validDBName(dbname, NamespaceString::DollarInDbNameBehavior::Allow));

        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "Can't clone catalog data for " << dbname << " database",
                dbname != DatabaseName::kAdmin.db() && dbname != DatabaseName::kConfig.db() &&
                    dbname != DatabaseName::kLocal.db());

        auto from = cloneCatalogDataRequest.getFrom();

        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "Can't run _shardsvrCloneCatalogData without a source",
                !from.empty());

        // For newer versions, execute the operation in another operation context with local write
        // concern to prevent doing waits while we're holding resources (we have a session checked
        // out).
        if (TransactionParticipant::get(opCtx)) {
            {
                // Use ACR to have a thread holding the session while we do the cloning.
                auto newClient = opCtx->getServiceContext()->makeClient("CloneCatalogCommand");
                {
                    stdx::lock_guard<Client> lk(*newClient.get());
                    newClient->setSystemOperationKillableByStepdown(lk);
                }

                AlternativeClientRegion acr(newClient);
                auto executor =
                    Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor();
                auto newOpCtxPtr = CancelableOperationContext(
                    cc().makeOperationContext(), opCtx->getCancellationToken(), executor);

                AuthorizationSession::get(newOpCtxPtr.get()->getClient())
                    ->grantInternalAuthorization(newOpCtxPtr.get()->getClient());
                newOpCtxPtr->setWriteConcern(ShardingCatalogClient::kLocalWriteConcern);
                WriteBlockBypass::get(newOpCtxPtr.get()).set(true);
                cloneDatabase(newOpCtxPtr.get(), dbname, from, result);
            }
            // Since no write happened on this txnNumber, we need to make a dummy write to protect
            // against older requests with old txnNumbers.
            DBDirectClient client(opCtx);
            client.update(NamespaceString::kServerConfigurationNamespace,
                          BSON("_id"
                               << "CloneCatalogDataStats"),
                          BSON("$inc" << BSON("count" << 1)),
                          true /* upsert */,
                          false /* multi */);
        } else {
            cloneDatabase(opCtx, dbname, from, result);
        }
        return true;
    }

} cloneCatalogDataCmd;

}  // namespace
}  // namespace mongo
