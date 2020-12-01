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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/config/sharding_catalog_manager.h"

#include "mongo/db/auth/authorization_session_impl.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/query/query_request.h"
#include "mongo/db/s/balancer/type_migration.h"
#include "mongo/db/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/config_server_version.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/s/catalog/type_lockpings.h"
#include "mongo/s/catalog/type_locks.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/sharded_collections_ddl_parameters_gen.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/transport/service_entry_point.h"

namespace mongo {
namespace {

const WriteConcernOptions kNoWaitWriteConcern(1, WriteConcernOptions::SyncMode::UNSET, Seconds(0));

// This value is initialized only if the node is running as a config server
const auto getShardingCatalogManager =
    ServiceContext::declareDecoration<boost::optional<ShardingCatalogManager>>();

OpMsg runCommandInLocalTxn(OperationContext* opCtx,
                           StringData db,
                           bool startTransaction,
                           TxnNumber txnNumber,
                           BSONObj cmdObj) {
    BSONObjBuilder bob(std::move(cmdObj));
    if (startTransaction) {
        bob.append("startTransaction", true);
    }
    bob.append("autocommit", false);
    bob.append(OperationSessionInfo::kTxnNumberFieldName, txnNumber);

    BSONObjBuilder lsidBuilder(bob.subobjStart("lsid"));
    opCtx->getLogicalSessionId()->serialize(&bob);
    lsidBuilder.doneFast();

    return OpMsg::parseOwned(
        opCtx->getServiceContext()
            ->getServiceEntryPoint()
            ->handleRequest(opCtx,
                            OpMsgRequest::fromDBAndBody(db.toString(), bob.obj()).serialize())
            .get()
            .response);
}

void startTransactionWithNoopFind(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  TxnNumber txnNumber) {
    BSONObjBuilder findCmdBuilder;
    QueryRequest qr(nss);
    qr.setBatchSize(0);
    qr.setWantMore(false);
    qr.asFindCommand(&findCmdBuilder);

    auto res = runCommandInLocalTxn(
                   opCtx, nss.db(), true /*startTransaction*/, txnNumber, findCmdBuilder.done())
                   .body;
    uassertStatusOK(getStatusFromCommandResult(res));
}

BSONObj commitOrAbortTransaction(OperationContext* opCtx,
                                 TxnNumber txnNumber,
                                 std::string cmdName) {
    // Swap out the clients in order to get a fresh opCtx. Previous operations in this transaction
    // that have been run on this opCtx would have set the timeout in the locker on the opCtx, but
    // commit should not have a lock timeout.
    auto newClient = getGlobalServiceContext()->makeClient("ShardingCatalogManager");
    AlternativeClientRegion acr(newClient);
    auto newOpCtx = cc().makeOperationContext();
    AuthorizationSession::get(newOpCtx.get()->getClient())
        ->grantInternalAuthorization(newOpCtx.get()->getClient());
    newOpCtx.get()->setLogicalSessionId(opCtx->getLogicalSessionId().get());
    newOpCtx.get()->setTxnNumber(txnNumber);

    BSONObjBuilder bob;
    bob.append(cmdName, true);
    bob.append("autocommit", false);
    bob.append(OperationSessionInfo::kTxnNumberFieldName, txnNumber);
    bob.append(WriteConcernOptions::kWriteConcernField, WriteConcernOptions::Majority);

    BSONObjBuilder lsidBuilder(bob.subobjStart("lsid"));
    newOpCtx->getLogicalSessionId()->serialize(&bob);
    lsidBuilder.doneFast();

    const auto cmdObj = bob.obj();

    const auto replyOpMsg =
        OpMsg::parseOwned(newOpCtx->getServiceContext()
                              ->getServiceEntryPoint()
                              ->handleRequest(newOpCtx.get(),
                                              OpMsgRequest::fromDBAndBody(
                                                  NamespaceString::kAdminDb.toString(), cmdObj)
                                                  .serialize())
                              .get()
                              .response);
    return replyOpMsg.body;
}

// Runs commit for the transaction with 'txnNumber'.
void commitTransaction(OperationContext* opCtx, TxnNumber txnNumber) {
    auto response = commitOrAbortTransaction(opCtx, txnNumber, "commitTransaction");
    uassertStatusOK(getStatusFromCommandResult(response));
    uassertStatusOK(getWriteConcernStatusFromCommandResult(response));
}

// Runs abort for the transaction with 'txnNumber'.
void abortTransaction(OperationContext* opCtx, TxnNumber txnNumber) {
    auto response = commitOrAbortTransaction(opCtx, txnNumber, "abortTransaction");

    auto status = getStatusFromCommandResult(response);
    if (status.code() != ErrorCodes::NoSuchTransaction) {
        uassertStatusOK(status);
        uassertStatusOK(getWriteConcernStatusFromCommandResult(response));
    }
}

}  // namespace

void ShardingCatalogManager::create(ServiceContext* serviceContext,
                                    std::unique_ptr<executor::TaskExecutor> addShardExecutor) {
    auto& shardingCatalogManager = getShardingCatalogManager(serviceContext);
    invariant(!shardingCatalogManager);

    shardingCatalogManager.emplace(serviceContext, std::move(addShardExecutor));
}

NamespaceSerializer::ScopedLock ShardingCatalogManager::serializeCreateOrDropDatabase(
    OperationContext* opCtx, StringData dbName) {
    return _namespaceSerializer.lock(opCtx, dbName);
}

NamespaceSerializer::ScopedLock ShardingCatalogManager::serializeCreateOrDropCollection(
    OperationContext* opCtx, const NamespaceString& nss) {
    return _namespaceSerializer.lock(opCtx, nss.ns());
}

void ShardingCatalogManager::clearForTests(ServiceContext* serviceContext) {
    auto& shardingCatalogManager = getShardingCatalogManager(serviceContext);
    invariant(shardingCatalogManager);

    shardingCatalogManager.reset();
}

ShardingCatalogManager* ShardingCatalogManager::get(ServiceContext* serviceContext) {
    auto& shardingCatalogManager = getShardingCatalogManager(serviceContext);
    invariant(shardingCatalogManager);

    return shardingCatalogManager.get_ptr();
}

ShardingCatalogManager* ShardingCatalogManager::get(OperationContext* operationContext) {
    return get(operationContext->getServiceContext());
}

ShardingCatalogManager::ShardingCatalogManager(
    ServiceContext* serviceContext, std::unique_ptr<executor::TaskExecutor> addShardExecutor)
    : _serviceContext(serviceContext),
      _executorForAddShard(std::move(addShardExecutor)),
      _kZoneOpLock("zoneOpLock"),
      _kChunkOpLock("chunkOpLock"),
      _kShardMembershipLock("shardMembershipLock") {
    startup();
}

ShardingCatalogManager::~ShardingCatalogManager() {
    shutDown();
}

void ShardingCatalogManager::startup() {
    stdx::lock_guard<Latch> lk(_mutex);
    if (_started) {
        return;
    }
    _started = true;
    _executorForAddShard->startup();

    Grid::get(_serviceContext)
        ->setCustomConnectionPoolStatsFn(
            [this](executor::ConnectionPoolStats* stats) { appendConnectionStats(stats); });
}

void ShardingCatalogManager::shutDown() {
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _inShutdown = true;
    }

    Grid::get(_serviceContext)->setCustomConnectionPoolStatsFn(nullptr);

    _executorForAddShard->shutdown();
    _executorForAddShard->join();
}

Status ShardingCatalogManager::initializeConfigDatabaseIfNeeded(OperationContext* opCtx) {
    {
        stdx::lock_guard<Latch> lk(_mutex);
        if (_configInitialized) {
            return {ErrorCodes::AlreadyInitialized,
                    "Config database was previously loaded into memory"};
        }
    }

    Status status = _initConfigIndexes(opCtx);
    if (!status.isOK()) {
        return status;
    }

    // Make sure to write config.version last since we detect rollbacks of config.version and
    // will re-run initializeConfigDatabaseIfNeeded if that happens, but we don't detect rollback
    // of the index builds.
    status = _initConfigVersion(opCtx);
    if (!status.isOK()) {
        return status;
    }

    stdx::lock_guard<Latch> lk(_mutex);
    _configInitialized = true;

    return Status::OK();
}

void ShardingCatalogManager::discardCachedConfigDatabaseInitializationState() {
    stdx::lock_guard<Latch> lk(_mutex);
    _configInitialized = false;
}

Status ShardingCatalogManager::_initConfigVersion(OperationContext* opCtx) {
    const auto catalogClient = Grid::get(opCtx)->catalogClient();

    auto versionStatus =
        catalogClient->getConfigVersion(opCtx, repl::ReadConcernLevel::kLocalReadConcern);
    if (!versionStatus.isOK()) {
        return versionStatus.getStatus();
    }

    const auto& versionInfo = versionStatus.getValue();
    if (versionInfo.getMinCompatibleVersion() > CURRENT_CONFIG_VERSION) {
        return {ErrorCodes::IncompatibleShardingConfigVersion,
                str::stream() << "current version v" << CURRENT_CONFIG_VERSION
                              << " is older than the cluster min compatible v"
                              << versionInfo.getMinCompatibleVersion()};
    }

    if (versionInfo.getCurrentVersion() == UpgradeHistory_EmptyVersion) {
        VersionType newVersion;
        newVersion.setClusterId(OID::gen());
        newVersion.setMinCompatibleVersion(MIN_COMPATIBLE_CONFIG_VERSION);
        newVersion.setCurrentVersion(CURRENT_CONFIG_VERSION);

        BSONObj versionObj(newVersion.toBSON());
        auto insertStatus = catalogClient->insertConfigDocument(
            opCtx, VersionType::ConfigNS, versionObj, kNoWaitWriteConcern);

        return insertStatus;
    }

    if (versionInfo.getCurrentVersion() == UpgradeHistory_UnreportedVersion) {
        return {ErrorCodes::IncompatibleShardingConfigVersion,
                "Assuming config data is old since the version document cannot be found in the "
                "config server and it contains databases besides 'local' and 'admin'. "
                "Please upgrade if this is the case. Otherwise, make sure that the config "
                "server is clean."};
    }

    if (versionInfo.getCurrentVersion() < CURRENT_CONFIG_VERSION) {
        return {ErrorCodes::IncompatibleShardingConfigVersion,
                str::stream() << "need to upgrade current cluster version to v"
                              << CURRENT_CONFIG_VERSION << "; currently at v"
                              << versionInfo.getCurrentVersion()};
    }

    return Status::OK();
}

Status ShardingCatalogManager::_initConfigIndexes(OperationContext* opCtx) {
    const bool unique = true;
    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    Status result = configShard->createIndexOnConfig(
        opCtx, ChunkType::ConfigNS, BSON(ChunkType::ns() << 1 << ChunkType::min() << 1), unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create ns_1_min_1 index on config db");
    }

    result = configShard->createIndexOnConfig(
        opCtx,
        ChunkType::ConfigNS,
        BSON(ChunkType::ns() << 1 << ChunkType::shard() << 1 << ChunkType::min() << 1),
        unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create ns_1_shard_1_min_1 index on config db");
    }

    result =
        configShard->createIndexOnConfig(opCtx,
                                         ChunkType::ConfigNS,
                                         BSON(ChunkType::ns() << 1 << ChunkType::lastmod() << 1),
                                         unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create ns_1_lastmod_1 index on config db");
    }

    result = configShard->createIndexOnConfig(
        opCtx,
        MigrationType::ConfigNS,
        BSON(MigrationType::ns() << 1 << MigrationType::min() << 1),
        unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create ns_1_min_1 index on config.migrations");
    }

    result = configShard->createIndexOnConfig(
        opCtx, ShardType::ConfigNS, BSON(ShardType::host() << 1), unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create host_1 index on config db");
    }

    result = configShard->createIndexOnConfig(
        opCtx, LocksType::ConfigNS, BSON(LocksType::lockID() << 1), !unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create lock id index on config db");
    }

    result =
        configShard->createIndexOnConfig(opCtx,
                                         LocksType::ConfigNS,
                                         BSON(LocksType::state() << 1 << LocksType::process() << 1),
                                         !unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create state and process id index on config db");
    }

    result = configShard->createIndexOnConfig(
        opCtx, LockpingsType::ConfigNS, BSON(LockpingsType::ping() << 1), !unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create lockping ping time index on config db");
    }

    result = configShard->createIndexOnConfig(
        opCtx, TagsType::ConfigNS, BSON(TagsType::ns() << 1 << TagsType::min() << 1), unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create ns_1_min_1 index on config db");
    }

    result = configShard->createIndexOnConfig(
        opCtx, TagsType::ConfigNS, BSON(TagsType::ns() << 1 << TagsType::tag() << 1), !unique);
    if (!result.isOK()) {
        return result.withContext("couldn't create ns_1_tag_1 index on config db");
    }

    return Status::OK();
}

Status ShardingCatalogManager::setFeatureCompatibilityVersionOnShards(OperationContext* opCtx,
                                                                      const BSONObj& cmdObj) {

    // No shards should be added until we have forwarded featureCompatibilityVersion to all shards.
    Lock::SharedLock lk(opCtx->lockState(), _kShardMembershipLock);

    // We do a direct read of the shards collection with local readConcern so no shards are missed,
    // but don't go through the ShardRegistry to prevent it from caching data that may be rolled
    // back.
    const auto opTimeWithShards = uassertStatusOK(Grid::get(opCtx)->catalogClient()->getAllShards(
        opCtx, repl::ReadConcernLevel::kLocalReadConcern));

    for (const auto& shardType : opTimeWithShards.value) {
        const auto shardStatus =
            Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardType.getName());
        if (!shardStatus.isOK()) {
            continue;
        }
        const auto shard = shardStatus.getValue();

        auto response = shard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            "admin",
            cmdObj,
            Shard::RetryPolicy::kIdempotent);
        if (!response.isOK()) {
            return response.getStatus();
        }
        if (!response.getValue().commandStatus.isOK()) {
            return response.getValue().commandStatus;
        }
        if (!response.getValue().writeConcernStatus.isOK()) {
            return response.getValue().writeConcernStatus;
        }
    }

    return Status::OK();
}

void ShardingCatalogManager::removePre49LegacyMetadata(OperationContext* opCtx) {
    const auto catalogClient = Grid::get(opCtx)->catalogClient();
    DBDirectClient client(opCtx);

    // Delete all documents which have {dropped: true} from config.collections
    uassertStatusOK(
        catalogClient->removeConfigDocuments(opCtx,
                                             CollectionType::ConfigNS,
                                             BSON("dropped" << true),
                                             ShardingCatalogClient::kLocalWriteConcern));

    // Clear the {dropped:true} and {distributionMode:sharded} fields from config.collections
    write_ops::Update clearDroppedAndDistributionMode(CollectionType::ConfigNS, [] {
        write_ops::UpdateOpEntry u;
        u.setQ({});
        u.setU(write_ops::UpdateModification::parseFromClassicUpdate(BSON("$unset"
                                                                          << BSON("dropped"
                                                                                  << "")
                                                                          << "$unset"
                                                                          << BSON("distributionMode"
                                                                                  << ""))));
        u.setMulti(true);
        return std::vector{u};
    }());
    clearDroppedAndDistributionMode.setWriteCommandBase([] {
        write_ops::WriteCommandBase base;
        base.setOrdered(false);
        return base;
    }());

    auto commandResult = client.runCommand(
        OpMsgRequest::fromDBAndBody(CollectionType::ConfigNS.db(),
                                    clearDroppedAndDistributionMode.toBSON(
                                        ShardingCatalogClient::kMajorityWriteConcern.toBSON())));
    uassertStatusOK([&] {
        BatchedCommandResponse response;
        std::string unusedErrmsg;
        response.parseBSON(
            commandResult->getCommandReply(),
            &unusedErrmsg);  // Return value intentionally ignored, because response.toStatus() will
                             // contain any errors in more detail
        return response.toStatus();
    }());
    uassertStatusOK(getWriteConcernStatusFromCommandResult(commandResult->getCommandReply()));
}

void ShardingCatalogManager::createCollectionTimestampsFor49(OperationContext* opCtx) {
    LOGV2(5258800, "Starting upgrade of config.collections");

    const auto catalogClient = Grid::get(opCtx)->catalogClient();
    auto const catalogCache = Grid::get(opCtx)->catalogCache();
    auto const configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    const auto collectionDocs =
        uassertStatusOK(configShard->exhaustiveFindOnConfig(
                            opCtx,
                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                            repl::ReadConcernLevel::kLocalReadConcern,
                            CollectionType::ConfigNS,
                            BSON(CollectionType::kTimestampFieldName << BSON("$exists" << false)),
                            BSONObj(),
                            boost::none))
            .docs;


    for (const auto& doc : collectionDocs) {
        const CollectionType coll(doc);
        const auto nss = coll.getNss();

        auto now = VectorClock::get(opCtx)->getTime();
        auto clusterTime = now.clusterTime().asTimestamp();

        uassertStatusOK(catalogClient->updateConfigDocument(
            opCtx,
            CollectionType::ConfigNS,
            BSON(CollectionType::kNssFieldName << nss.ns()),
            BSON("$set" << BSON(CollectionType::kTimestampFieldName << clusterTime)),
            false /* upsert */,
            ShardingCatalogClient::kMajorityWriteConcern));

        catalogCache->invalidateCollectionEntry_LINEARIZABLE(nss);
    }

    LOGV2(5258801, "Successfully upgraded config.collections");
}

void ShardingCatalogManager::downgradeConfigCollectionEntriesToPre49(OperationContext* opCtx) {
    if (feature_flags::gShardingFullDDLSupport.isEnabledAndIgnoreFCV()) {
        DBDirectClient client(opCtx);

        // Clear the 'timestamp' fields from config.collections
        write_ops::Update unsetTimestamp(CollectionType::ConfigNS, [] {
            write_ops::UpdateOpEntry u;
            u.setQ({});
            u.setU(write_ops::UpdateModification::parseFromClassicUpdate(
                BSON("$unset" << BSON(CollectionType::kTimestampFieldName << ""))));
            u.setMulti(true);
            return std::vector{u};
        }());
        unsetTimestamp.setWriteCommandBase([] {
            write_ops::WriteCommandBase base;
            base.setOrdered(false);
            return base;
        }());

        auto commandResult = client.runCommand(OpMsgRequest::fromDBAndBody(
            CollectionType::ConfigNS.db(),
            unsetTimestamp.toBSON(ShardingCatalogClient::kMajorityWriteConcern.toBSON())));

        uassertStatusOK([&] {
            BatchedCommandResponse response;
            std::string unusedErrmsg;
            response.parseBSON(
                commandResult->getCommandReply(),
                &unusedErrmsg);  // Return value intentionally ignored, because response.toStatus()
                                 // will contain any errors in more detail
            return response.toStatus();
        }());
        uassertStatusOK(getWriteConcernStatusFromCommandResult(commandResult->getCommandReply()));
    }
}

Lock::ExclusiveLock ShardingCatalogManager::lockZoneMutex(OperationContext* opCtx) {
    Lock::ExclusiveLock lk(opCtx->lockState(), _kZoneOpLock);
    return lk;
}

StatusWith<bool> ShardingCatalogManager::_isShardRequiredByZoneStillInUse(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const std::string& shardName,
    const std::string& zoneName) {
    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto findShardStatus =
        configShard->exhaustiveFindOnConfig(opCtx,
                                            readPref,
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            ShardType::ConfigNS,
                                            BSON(ShardType::tags() << zoneName),
                                            BSONObj(),
                                            2);

    if (!findShardStatus.isOK()) {
        return findShardStatus.getStatus();
    }

    const auto shardDocs = findShardStatus.getValue().docs;

    if (shardDocs.size() == 0) {
        // The zone doesn't exists.
        return false;
    }

    if (shardDocs.size() == 1) {
        auto shardDocStatus = ShardType::fromBSON(shardDocs.front());
        if (!shardDocStatus.isOK()) {
            return shardDocStatus.getStatus();
        }

        auto shardDoc = shardDocStatus.getValue();
        if (shardDoc.getName() != shardName) {
            // The last shard that belongs to this zone is a different shard.
            return false;
        }

        auto findChunkRangeStatus =
            configShard->exhaustiveFindOnConfig(opCtx,
                                                readPref,
                                                repl::ReadConcernLevel::kLocalReadConcern,
                                                TagsType::ConfigNS,
                                                BSON(TagsType::tag() << zoneName),
                                                BSONObj(),
                                                1);

        if (!findChunkRangeStatus.isOK()) {
            return findChunkRangeStatus.getStatus();
        }

        return findChunkRangeStatus.getValue().docs.size() > 0;
    }

    return false;
}

BSONObj ShardingCatalogManager::writeToConfigDocumentInTxn(OperationContext* opCtx,
                                                           const NamespaceString& nss,
                                                           const BatchedCommandRequest& request,
                                                           TxnNumber txnNumber) {
    invariant(nss.db() == NamespaceString::kConfigDb);
    auto response = runCommandInLocalTxn(
                        opCtx, nss.db(), false /* startTransaction */, txnNumber, request.toBSON())
                        .body;

    uassertStatusOK(getStatusFromCommandResult(response));
    uassertStatusOK(getWriteConcernStatusFromCommandResult(response));

    return response;
}

void ShardingCatalogManager::insertConfigDocumentsInTxn(OperationContext* opCtx,
                                                        const NamespaceString& nss,
                                                        std::vector<BSONObj> docs,
                                                        TxnNumber txnNumber) {
    invariant(nss.db() == NamespaceString::kConfigDb);

    std::vector<BSONObj> workingBatch;
    size_t workingBatchItemSize = 0;
    int workingBatchDocSize = 0;

    auto doBatchInsert = [&]() {
        BatchedCommandRequest request([&] {
            write_ops::Insert insertOp(nss);
            insertOp.setDocuments(workingBatch);
            return insertOp;
        }());

        writeToConfigDocumentInTxn(opCtx, nss, request, txnNumber);
    };

    while (!docs.empty()) {
        BSONObj toAdd = docs.back();
        docs.pop_back();

        const int docSizePlusOverhead =
            toAdd.objsize() + write_ops::kRetryableAndTxnBatchWriteBSONSizeOverhead;
        // Check if pushing this object will exceed the batch size limit or the max object size
        if ((workingBatchItemSize + 1 > write_ops::kMaxWriteBatchSize) ||
            (workingBatchDocSize + docSizePlusOverhead > BSONObjMaxUserSize)) {
            doBatchInsert();

            workingBatch.clear();
            workingBatchItemSize = 0;
            workingBatchDocSize = 0;
        }

        workingBatch.push_back(toAdd);
        ++workingBatchItemSize;
        workingBatchDocSize += docSizePlusOverhead;
    }

    if (!workingBatch.empty())
        doBatchInsert();
}

void ShardingCatalogManager::withTransaction(
    OperationContext* opCtx,
    const NamespaceString& namespaceForInitialFind,
    unique_function<void(OperationContext*, TxnNumber)> func) {
    AlternativeSessionRegion asr(opCtx);
    AuthorizationSession::get(asr.opCtx()->getClient())
        ->grantInternalAuthorization(asr.opCtx()->getClient());
    TxnNumber txnNumber = 0;

    auto guard = makeGuard([opCtx = asr.opCtx(), txnNumber] {
        try {
            abortTransaction(opCtx, txnNumber);
        } catch (DBException& e) {
            LOGV2_WARNING(5192100,
                          "Failed to abort transaction in AlternativeSessionRegion",
                          "error"_attr = redact(e));
        }
    });

    startTransactionWithNoopFind(asr.opCtx(), namespaceForInitialFind, txnNumber);
    func(asr.opCtx(), txnNumber);
    commitTransaction(asr.opCtx(), txnNumber);
    guard.dismiss();
}

}  // namespace mongo
