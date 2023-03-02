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

#include "mongo/s/write_ops/write_without_shard_key_util.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/internal_transactions_feature_flag_gen.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/db/update/update_util.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/cluster_commands_without_shard_key_gen.h"
#include "mongo/s/shard_key_pattern_query_util.h"
#include "mongo/s/transaction_router_resource_yielder.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace write_without_shard_key {
namespace {

constexpr auto kIdFieldName = "_id"_sd;
const FieldRef idFieldRef(kIdFieldName);

// Used to do query validation for the _id field.
const ShardKeyPattern kVirtualIdShardKey(BSON(kIdFieldName << 1));

/**
 * This returns "does the query have an _id field" and "is the _id field querying for a direct
 * value" e.g. _id : 3 and not _id : { $gt : 3 }.
 */
bool isExactIdQuery(OperationContext* opCtx,
                    const NamespaceString& nss,
                    const BSONObj query,
                    const BSONObj collation,
                    bool hasDefaultCollation) {
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(query);
    if (!collation.isEmpty()) {
        findCommand->setCollation(collation);
    }
    const auto cq = CanonicalQuery::canonicalize(opCtx,
                                                 std::move(findCommand),
                                                 false, /* isExplain */
                                                 nullptr,
                                                 ExtensionsCallbackNoop(),
                                                 MatchExpressionParser::kAllowAllSpecialFeatures);
    if (cq.isOK()) {
        // Only returns a shard key iff a query has a full shard key with direct/equality matches on
        // all shard key fields.
        auto shardKey = extractShardKeyFromQuery(kVirtualIdShardKey, *cq.getValue());
        BSONElement idElt = shardKey["_id"];

        if (!idElt) {
            return false;
        }

        if (CollationIndexKey::isCollatableType(idElt.type()) && !collation.isEmpty() &&
            !hasDefaultCollation) {
            // The collation applies to the _id field, but the user specified a collation which
            // doesn't match the collection default.
            return false;
        }
        return true;
    }

    return false;
}

bool shardKeyHasCollatableType(const BSONObj& shardKey) {
    for (BSONElement elt : shardKey) {
        if (CollationIndexKey::isCollatableType(elt.type())) {
            return true;
        }
    }
    return false;
}
}  // namespace

BSONObj generateUpsertDocument(OperationContext* opCtx, const UpdateRequest& updateRequest) {
    ExtensionsCallbackNoop extensionsCallback = ExtensionsCallbackNoop();
    ParsedUpdate parsedUpdate(opCtx, &updateRequest, extensionsCallback);
    uassertStatusOK(parsedUpdate.parseRequest());

    const CanonicalQuery* canonicalQuery =
        parsedUpdate.hasParsedQuery() ? parsedUpdate.getParsedQuery() : nullptr;
    FieldRefSet immutablePaths;
    immutablePaths.insert(&idFieldRef);
    update::produceDocumentForUpsert(opCtx,
                                     &updateRequest,
                                     parsedUpdate.getDriver(),
                                     canonicalQuery,
                                     immutablePaths,
                                     parsedUpdate.getDriver()->getDocument());

    return parsedUpdate.getDriver()->getDocument().getObject();
}

bool useTwoPhaseProtocol(OperationContext* opCtx,
                         NamespaceString nss,
                         bool isUpdateOrDelete,
                         const BSONObj& query,
                         const BSONObj& collation) {
    if (!feature_flags::gFeatureFlagUpdateOneWithoutShardKey.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        return false;
    }

    auto [cm, _] =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));

    // Unsharded collections always target the primary shard.
    if (!cm.isSharded()) {
        return false;
    }

    // Check if the query has specified a different collation than the default collation.
    auto collator = collation.isEmpty()
        ? nullptr  // If no collation is specified we return a nullptr signifying the simple
                   // collation.
        : uassertStatusOK(
              CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collation));
    auto hasDefaultCollation =
        CollatorInterface::collatorsMatch(collator.get(), cm.getDefaultCollator());

    // updateOne and deleteOne do not use the two phase protocol for single writes that specify
    // _id in their queries. An exact _id match requires default collation if the _id value is a
    // collatable type.
    if (isUpdateOrDelete && query.hasField("_id") &&
        isExactIdQuery(opCtx, nss, query, collation, hasDefaultCollation)) {
        return false;
    }

    auto shardKey =
        uassertStatusOK(extractShardKeyFromBasicQuery(opCtx, nss, cm.getShardKeyPattern(), query));

    // 'shardKey' will only be populated only if a full equality shard key is extracted.
    if (shardKey.isEmpty()) {
        return true;
    } else {
        // If the default collection collation is not used and any field of the shard key is a
        // collatable type, then we will use the two phase write protocol since we cannot target
        // directly to a shard.
        if (!hasDefaultCollation && shardKeyHasCollatableType(shardKey)) {
            return true;
        } else {
            return false;
        }
    }

    return true;
}

StatusWith<ClusterWriteWithoutShardKeyResponse> runTwoPhaseWriteProtocol(OperationContext* opCtx,
                                                                         NamespaceString nss,
                                                                         BSONObj cmdObj) {
    if (opCtx->isRetryableWrite()) {
        tassert(7260900,
                "Retryable writes must have an explicit stmtId",
                cmdObj.hasField(write_ops::WriteCommandRequestBase::kStmtIdsFieldName) ||
                    cmdObj.hasField(write_ops::WriteCommandRequestBase::kStmtIdFieldName));
    }

    // Shared state for the transaction below.
    struct SharedBlock {
        SharedBlock(NamespaceString nss_, BSONObj cmdObj_)
            : nss(std::move(nss_)), cmdObj(cmdObj_) {}
        NamespaceString nss;
        BSONObj cmdObj;
        ClusterWriteWithoutShardKeyResponse clusterWriteResponse;
    };

    auto txn = txn_api::SyncTransactionWithRetries(
        opCtx,
        Grid::get(opCtx)->getExecutorPool()->getFixedExecutor(),
        TransactionRouterResourceYielder::makeForLocalHandoff());

    auto sharedBlock = std::make_shared<SharedBlock>(nss, cmdObj);
    auto swResult = txn.runNoThrow(
        opCtx, [sharedBlock](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            ClusterQueryWithoutShardKey clusterQueryWithoutShardKeyCommand(sharedBlock->cmdObj);
            auto queryRes = txnClient
                                .runCommand(sharedBlock->nss.dbName(),
                                            clusterQueryWithoutShardKeyCommand.toBSON({}))
                                .get();
            uassertStatusOK(getStatusFromCommandResult(queryRes));

            ClusterQueryWithoutShardKeyResponse queryResponse =
                ClusterQueryWithoutShardKeyResponse::parseOwned(
                    IDLParserContext("_clusterQueryWithoutShardKeyResponse"), std::move(queryRes));

            // If there's no matching document and upsert:false, then no modification needs to be
            // made.
            if (!queryResponse.getTargetDoc()) {
                return SemiFuture<void>::makeReady();
            }

            BSONObjBuilder bob(sharedBlock->cmdObj);
            ClusterWriteWithoutShardKey clusterWriteWithoutShardKeyCommand(
                bob.obj(),
                std::string(*queryResponse.getShardId()) /* shardId */,
                *queryResponse.getTargetDoc() /* targetDocId */);

            auto writeRes = txnClient
                                .runCommand(sharedBlock->nss.dbName(),
                                            clusterWriteWithoutShardKeyCommand.toBSON(BSONObj()))
                                .get();
            uassertStatusOK(getStatusFromCommandResult(writeRes));

            sharedBlock->clusterWriteResponse = ClusterWriteWithoutShardKeyResponse::parseOwned(
                IDLParserContext("_clusterWriteWithoutShardKeyResponse"), std::move(writeRes));
            return SemiFuture<void>::makeReady();
        });

    if (swResult.isOK()) {
        if (swResult.getValue().getEffectiveStatus().isOK()) {
            return StatusWith<ClusterWriteWithoutShardKeyResponse>(
                sharedBlock->clusterWriteResponse);
        } else {
            return StatusWith<ClusterWriteWithoutShardKeyResponse>(
                swResult.getValue().getEffectiveStatus());
        }
    } else {
        return StatusWith<ClusterWriteWithoutShardKeyResponse>(swResult.getStatus());
    }
}
}  // namespace write_without_shard_key
}  // namespace mongo
