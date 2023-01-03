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

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/s/analyze_shard_key_cmd_gen.h"
#include "mongo/s/analyze_shard_key_feature_flag_gen.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

namespace {

/**
 * Returns a new command object with shard version and/or database version appended to it based on
 * the given routing info.
 */
BSONObj makeVersionedCmdObj(const CollectionRoutingInfo& cri,
                            const BSONObj& unversionedCmdObj,
                            ShardId shardId) {
    if (cri.cm.isSharded()) {
        return appendShardVersion(
            unversionedCmdObj,
            ShardVersion(cri.cm.getVersion(shardId),
                         cri.gii ? boost::make_optional(cri.gii->getCollectionIndexes())
                                 : boost::none));
    }
    auto versionedCmdObj = appendShardVersion(unversionedCmdObj, ShardVersion::UNSHARDED());
    return appendDbVersionIfPresent(versionedCmdObj, cri.cm.dbVersion());
}

class AnalyzeShardKeyCmd : public TypedCommand<AnalyzeShardKeyCmd> {
public:
    using Request = AnalyzeShardKey;
    using Response = AnalyzeShardKeyResponse;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            const auto& nss = ns();
            const auto& catalogCache = Grid::get(opCtx)->catalogCache();
            const auto cri = uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, nss));

            std::set<ShardId> candidateShardIds;
            if (cri.cm.isSharded()) {
                cri.cm.getAllShardIds(&candidateShardIds);
            } else {
                candidateShardIds.insert(cri.cm.dbPrimary());
            }

            PseudoRandom random{SecureRandom{}.nextInt64()};
            const auto unversionedCmdObj =
                CommandHelpers::filterCommandRequestForPassthrough(request().toBSON({}));
            while (true) {
                // Select a random shard.
                invariant(!candidateShardIds.empty());
                auto it = candidateShardIds.begin();
                std::advance(it, random.nextInt64(candidateShardIds.size()));
                auto shardId = *it;
                candidateShardIds.erase(shardId);

                uassert(ErrorCodes::IllegalOperation,
                        "Cannot analyze a shard key for a collection on the config server",
                        shardId != ShardId::kConfigServerId);

                // Build a versioned command for the selected shard.
                auto versionedCmdObj = makeVersionedCmdObj(cri, unversionedCmdObj, shardId);

                // Execute the command against the shard.
                auto shard =
                    uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId));
                auto swResponse = shard->runCommandWithFixedRetryAttempts(
                    opCtx,
                    ReadPreferenceSetting{ReadPreference::SecondaryPreferred},
                    NamespaceString::kAdminDb.toString(),
                    versionedCmdObj,
                    Shard::RetryPolicy::kIdempotent);
                auto status = Shard::CommandResponse::getEffectiveStatus(swResponse);

                if (status == ErrorCodes::CollectionIsEmptyLocally) {
                    uassert(ErrorCodes::InvalidOptions,
                            "Cannot analyze a shard key for an empty collection",
                            !candidateShardIds.empty());

                    LOGV2(6875300,
                          "Failed to analyze shard key on the selected shard since it did not "
                          "have any documents for the collection locally. Retrying on a different "
                          "shard.",
                          "nss"_attr = nss,
                          "key"_attr = request().getKey(),
                          "shardId"_attr = shardId,
                          "error"_attr = status);
                    continue;
                }

                uassertStatusOK(status);
                auto response = AnalyzeShardKeyResponse::parse(
                    IDLParserContext("clusterAnalyzeShardKey"), swResponse.getValue().response);
                return response;
            }
        }

    private:
        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(ns()),
                                                           ActionType::shardCollection));
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kWrite;
    }

    std::string help() const override {
        return "Returns metrics for evaluating a shard key for a collection.";
    }
};

MONGO_REGISTER_FEATURE_FLAGGED_COMMAND(AnalyzeShardKeyCmd,
                                       analyze_shard_key::gFeatureFlagAnalyzeShardKey);

}  // namespace

}  // namespace mongo
