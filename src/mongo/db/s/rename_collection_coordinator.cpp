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


#include "mongo/platform/basic.h"

#include "mongo/db/s/rename_collection_coordinator.h"

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_uuid_mismatch.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/sharded_index_catalog_commands_gen.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/s/sharding_index_catalog_ddl_util.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/sharding_recovery_service.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/vector_clock.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/index_version.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

boost::optional<CollectionType> getShardedCollection(OperationContext* opCtx,
                                                     const NamespaceString& nss) {
    try {
        return Grid::get(opCtx)->catalogClient()->getCollection(opCtx, nss);
    } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // The collection is unsharded or doesn't exist
        return boost::none;
    }
}

boost::optional<UUID> getCollectionUUID(OperationContext* opCtx,
                                        NamespaceString const& nss,
                                        boost::optional<CollectionType> const& optCollectionType,
                                        bool throwOnNotFound = true) {
    if (optCollectionType) {
        return optCollectionType->getUuid();
    }
    Lock::DBLock dbLock(opCtx, nss.dbName(), MODE_IS);
    Lock::CollectionLock collLock(opCtx, nss, MODE_IS);
    const auto collPtr = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);
    if (!collPtr && !throwOnNotFound) {
        return boost::none;
    }

    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Collection " << nss << " doesn't exist.",
            collPtr);

    return collPtr->uuid();
}

void renameIndexMetadataInShards(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 const RenameCollectionRequest& request,
                                 const OperationSessionInfo& osi,
                                 const std::shared_ptr<executor::TaskExecutor>& executor,
                                 RenameCollectionCoordinatorDocument* doc) {
    const auto [configTime, newIndexVersion] = [opCtx]() -> std::pair<LogicalTime, Timestamp> {
        VectorClock::VectorTime vt = VectorClock::get(opCtx)->getTime();
        return {vt.configTime(), vt.clusterTime().asTimestamp()};
    }();

    // Bump the index version only if there are indexes in the source
    // collection.
    auto optShardedCollInfo = doc->getOptShardedCollInfo();
    if (optShardedCollInfo && optShardedCollInfo->getIndexVersion()) {
        // Bump sharding catalog's index version on the config server if the source
        // collection is sharded. It will be updated later on.
        optShardedCollInfo->setIndexVersion({optShardedCollInfo->getUuid(), newIndexVersion});
        doc->setOptShardedCollInfo(optShardedCollInfo);
    }

    // Update global index metadata in shards.
    auto& toNss = request.getTo();

    auto participants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
    ShardsvrRenameIndexMetadata renameIndexCatalogReq(
        nss, toNss, {doc->getSourceUUID().value(), newIndexVersion});
    const auto renameIndexCatalogCmdObj =
        CommandHelpers::appendMajorityWriteConcern(renameIndexCatalogReq.toBSON({}));
    sharding_ddl_util::sendAuthenticatedCommandToShards(
        opCtx,
        toNss.db(),
        renameIndexCatalogCmdObj.addFields(osi.toBSON()),
        participants,
        executor);
}
}  // namespace

RenameCollectionCoordinator::RenameCollectionCoordinator(ShardingDDLCoordinatorService* service,
                                                         const BSONObj& initialState)
    : RecoverableShardingDDLCoordinator(service, "RenameCollectionCoordinator", initialState),
      _request(_doc.getRenameCollectionRequest()) {}

void RenameCollectionCoordinator::checkIfOptionsConflict(const BSONObj& doc) const {
    const auto otherDoc = RenameCollectionCoordinatorDocument::parse(
        IDLParserContext("RenameCollectionCoordinatorDocument"), doc);

    const auto& selfReq = _request.toBSON();
    const auto& otherReq = otherDoc.getRenameCollectionRequest().toBSON();

    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Another rename collection for namespace " << originalNss()
                          << " is being executed with different parameters: " << selfReq,
            SimpleBSONObjComparator::kInstance.evaluate(selfReq == otherReq));
}

std::vector<StringData> RenameCollectionCoordinator::_acquireAdditionalLocks(
    OperationContext* opCtx) {
    return {_request.getTo().ns()};
}

void RenameCollectionCoordinator::appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const {
    cmdInfoBuilder->appendElements(_request.toBSON());
}

ExecutorFuture<void> RenameCollectionCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then(_buildPhaseHandler(
            Phase::kCheckPreconditions,
            [this, executor = executor, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                const auto& fromNss = nss();
                const auto& toNss = _request.getTo();

                const auto criticalSectionReason =
                    sharding_ddl_util::getCriticalSectionReasonForRename(fromNss, toNss);

                try {
                    uassert(ErrorCodes::IllegalOperation,
                            "Renaming a timeseries collection is not allowed",
                            !fromNss.isTimeseriesBucketsCollection());

                    uassert(ErrorCodes::IllegalOperation,
                            "Renaming to a bucket namespace is not allowed",
                            !toNss.isTimeseriesBucketsCollection());

                    uassert(ErrorCodes::InvalidOptions,
                            "Cannot provide an expected collection UUID when renaming between "
                            "databases",
                            fromNss.db() == toNss.db() ||
                                (!_doc.getExpectedSourceUUID() && !_doc.getExpectedTargetUUID()));
                    {
                        AutoGetCollection coll{
                            opCtx,
                            fromNss,
                            MODE_IS,
                            AutoGetCollection::Options{}
                                .viewMode(auto_get_collection::ViewMode::kViewsPermitted)
                                .expectedUUID(_doc.getExpectedSourceUUID())};

                        uassert(ErrorCodes::CommandNotSupportedOnView,
                                str::stream() << "Can't rename source collection `" << fromNss
                                              << "` because it is a view.",
                                !CollectionCatalog::get(opCtx)->lookupView(opCtx, fromNss));

                        checkCollectionUUIDMismatch(
                            opCtx, fromNss, *coll, _doc.getExpectedSourceUUID());

                        uassert(ErrorCodes::NamespaceNotFound,
                                str::stream() << "Collection " << fromNss << " doesn't exist.",
                                coll.getCollection());

                        uassert(ErrorCodes::IllegalOperation,
                                "Cannot rename an encrypted collection",
                                !coll || !coll->getCollectionOptions().encryptedFieldConfig ||
                                    _doc.getAllowEncryptedCollectionRename().value_or(false));
                    }

                    // Make sure the source collection exists
                    const auto optSourceCollType = getShardedCollection(opCtx, fromNss);
                    const bool sourceIsSharded = (bool)optSourceCollType;

                    _doc.setSourceUUID(getCollectionUUID(opCtx, fromNss, optSourceCollType));
                    if (sourceIsSharded) {
                        uassert(ErrorCodes::CommandFailed,
                                str::stream() << "Source and destination collections must be on "
                                                 "the same database because "
                                              << fromNss << " is sharded.",
                                fromNss.db() == toNss.db());
                        _doc.setOptShardedCollInfo(optSourceCollType);
                    } else if (fromNss.db() != toNss.db()) {
                        sharding_ddl_util::checkDbPrimariesOnTheSameShard(opCtx, fromNss, toNss);
                    }

                    const auto optTargetCollType = getShardedCollection(opCtx, toNss);
                    const bool targetIsSharded = (bool)optTargetCollType;
                    _doc.setTargetIsSharded(targetIsSharded);
                    _doc.setTargetUUID(getCollectionUUID(
                        opCtx, toNss, optTargetCollType, /*throwNotFound*/ false));

                    if (!targetIsSharded) {
                        // (SERVER-67325) Acquire critical section on the target collection in order
                        // to disallow concurrent `createCollection`. In case the collection does
                        // not exist, it will be later released by the rename participant. In case
                        // the collection exists and is unsharded, the critical section can be
                        // released right away as the participant will re-acquire it when needed.
                        auto criticalSection = ShardingRecoveryService::get(opCtx);
                        criticalSection->acquireRecoverableCriticalSectionBlockWrites(
                            opCtx,
                            toNss,
                            criticalSectionReason,
                            ShardingCatalogClient::kLocalWriteConcern);
                        criticalSection->promoteRecoverableCriticalSectionToBlockAlsoReads(
                            opCtx,
                            toNss,
                            criticalSectionReason,
                            ShardingCatalogClient::kLocalWriteConcern);

                        // Make sure the target namespace is not a view
                        uassert(ErrorCodes::NamespaceExists,
                                str::stream() << "a view already exists with that name: " << toNss,
                                !CollectionCatalog::get(opCtx)->lookupView(opCtx, toNss));

                        if (CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx,
                                                                                       toNss)) {
                            // Release the critical section because the unsharded target collection
                            // already exists, hence no risk of concurrent `createCollection`
                            criticalSection->releaseRecoverableCriticalSection(
                                opCtx,
                                toNss,
                                criticalSectionReason,
                                WriteConcerns::kLocalWriteConcern);
                        }
                    }

                    sharding_ddl_util::checkRenamePreconditions(
                        opCtx, sourceIsSharded, toNss, _doc.getDropTarget());

                    sharding_ddl_util::checkCatalogConsistencyAcrossShardsForRename(
                        opCtx, fromNss, toNss, _doc.getDropTarget(), executor);

                    {
                        AutoGetCollection coll{opCtx,
                                               toNss,
                                               MODE_IS,
                                               AutoGetCollection::Options{}.expectedUUID(
                                                   _doc.getExpectedTargetUUID())};
                        uassert(ErrorCodes::IllegalOperation,
                                "Cannot rename to an existing encrypted collection",
                                !coll || !coll->getCollectionOptions().encryptedFieldConfig ||
                                    _doc.getAllowEncryptedCollectionRename().value_or(false));
                    }

                } catch (const DBException&) {
                    auto criticalSection = ShardingRecoveryService::get(opCtx);
                    criticalSection->releaseRecoverableCriticalSection(
                        opCtx,
                        toNss,
                        criticalSectionReason,
                        WriteConcerns::kLocalWriteConcern,
                        false /* throwIfReasonDiffers */);
                    _completeOnError = true;
                    throw;
                }
            }))
        .then(_buildPhaseHandler(
            Phase::kFreezeMigrations,
            [this, executor = executor, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                const auto& fromNss = nss();
                const auto& toNss = _request.getTo();

                ShardingLogging::get(opCtx)->logChange(
                    opCtx,
                    "renameCollection.start",
                    fromNss.ns(),
                    BSON("source" << fromNss.toString() << "destination" << toNss.toString()),
                    ShardingCatalogClient::kMajorityWriteConcern);

                // Block migrations on involved sharded collections
                if (_doc.getOptShardedCollInfo()) {
                    sharding_ddl_util::stopMigrations(opCtx, fromNss, _doc.getSourceUUID());
                }

                if (_doc.getTargetIsSharded()) {
                    sharding_ddl_util::stopMigrations(opCtx, toNss, _doc.getTargetUUID());
                }
            }))
        .then(_buildPhaseHandler(
            Phase::kBlockCrudAndRename,
            [this, executor = executor, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                if (!_firstExecution) {
                    _updateSession(opCtx);
                    _performNoopRetryableWriteOnAllShardsAndConfigsvr(
                        opCtx, getCurrentSession(), **executor);
                }

                const auto& fromNss = nss();

                _updateSession(opCtx);
                const OperationSessionInfo osi = getCurrentSession();

                // On participant shards:
                // - Block CRUD on source and target collection in case at least one
                // of such collections is currently sharded.
                // - Locally drop the target collection
                // - Locally rename source to target
                ShardsvrRenameCollectionParticipant renameCollParticipantRequest(
                    fromNss, _doc.getSourceUUID().value());
                renameCollParticipantRequest.setDbName(fromNss.db());
                renameCollParticipantRequest.setTargetUUID(_doc.getTargetUUID());
                renameCollParticipantRequest.setRenameCollectionRequest(_request);
                const auto cmdObj = CommandHelpers::appendMajorityWriteConcern(
                                        renameCollParticipantRequest.toBSON({}))
                                        .addFields(osi.toBSON());

                // We need to send the command to all the shards because both
                // movePrimary and moveChunk leave garbage behind for sharded
                // collections.
                // At the same time, the primary shard needs to be last participant to perfom its
                // local rename operation: this will ensure that the op entries generated by the
                // collections being renamed/dropped will be generated at points in time where all
                // shards have a consistent view of the metadata and no concurrent writes are being
                // performed.
                const auto primaryShardId = ShardingState::get(opCtx)->shardId();
                auto participants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
                participants.erase(
                    std::remove(participants.begin(), participants.end(), primaryShardId),
                    participants.end());

                sharding_ddl_util::sendAuthenticatedCommandToShards(
                    opCtx, fromNss.db(), cmdObj, participants, **executor);

                sharding_ddl_util::sendAuthenticatedCommandToShards(
                    opCtx, fromNss.db(), cmdObj, {primaryShardId}, **executor);
            }))
        .then(_buildPhaseHandler(
            Phase::kRenameMetadata,
            [this, executor = executor, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                // For an unsharded collection the CSRS server can not verify the targetUUID.
                // Use the session ID + txnNumber to ensure no stale requests get through.
                _updateSession(opCtx);

                if (!_firstExecution) {
                    _performNoopRetryableWriteOnAllShardsAndConfigsvr(
                        opCtx, getCurrentSession(), **executor);
                }

                if (!_isPre63Compatible() &&
                    (_doc.getTargetIsSharded() || _doc.getOptShardedCollInfo())) {
                    renameIndexMetadataInShards(
                        opCtx, nss(), _request, getCurrentSession(), **executor, &_doc);
                }

                ConfigsvrRenameCollectionMetadata req(nss(), _request.getTo());
                req.setOptFromCollection(_doc.getOptShardedCollInfo());
                const auto cmdObj = CommandHelpers::appendMajorityWriteConcern(req.toBSON({}));
                const auto& configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

                uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(
                    configShard->runCommand(opCtx,
                                            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                            "admin",
                                            cmdObj.addFields(getCurrentSession().toBSON()),
                                            Shard::RetryPolicy::kIdempotent)));

                // (SERVER-67730) Delete potential orphaned chunk entries from CSRS since
                // ConfigsvrRenameCollectionMetadata is not idempotent in case of a CSRS step-down
                auto uuid = _doc.getTargetUUID();
                if (uuid) {
                    auto query = BSON("uuid" << *uuid);
                    uassertStatusOK(Grid::get(opCtx)->catalogClient()->removeConfigDocuments(
                        opCtx,
                        ChunkType::ConfigNS,
                        query,
                        ShardingCatalogClient::kMajorityWriteConcern));
                }
            }))
        .then(_buildPhaseHandler(
            Phase::kUnblockCRUD,
            [this, executor = executor, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                if (!_firstExecution) {
                    _updateSession(opCtx);
                    _performNoopRetryableWriteOnAllShardsAndConfigsvr(
                        opCtx, getCurrentSession(), **executor);
                }

                const auto& fromNss = nss();
                // On participant shards:
                // - Unblock CRUD on participants for both source and destination collections
                ShardsvrRenameCollectionUnblockParticipant unblockParticipantRequest(
                    fromNss, _doc.getSourceUUID().value());
                unblockParticipantRequest.setDbName(fromNss.db());
                unblockParticipantRequest.setRenameCollectionRequest(_request);
                auto const cmdObj = CommandHelpers::appendMajorityWriteConcern(
                    unblockParticipantRequest.toBSON({}));
                auto participants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);

                _updateSession(opCtx);
                const OperationSessionInfo osi = getCurrentSession();

                sharding_ddl_util::sendAuthenticatedCommandToShards(
                    opCtx, fromNss.db(), cmdObj.addFields(osi.toBSON()), participants, **executor);
            }))
        .then(_buildPhaseHandler(
            Phase::kSetResponse,
            [this, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                // Retrieve the new collection version
                const auto catalog = Grid::get(opCtx)->catalogCache();
                const auto cri = uassertStatusOK(
                    catalog->getCollectionRoutingInfoWithRefresh(opCtx, _request.getTo()));
                _response = RenameCollectionResponse(
                    cri.cm.isSharded() ? cri.getCollectionVersion() : ShardVersion::UNSHARDED());

                ShardingLogging::get(opCtx)->logChange(
                    opCtx,
                    "renameCollection.end",
                    nss().ns(),
                    BSON("source" << nss().toString() << "destination"
                                  << _request.getTo().toString()),
                    ShardingCatalogClient::kMajorityWriteConcern);
                LOGV2(5460504, "Collection renamed", "namespace"_attr = nss());
            }))
        .onError([this, anchor = shared_from_this()](const Status& status) {
            if (!status.isA<ErrorCategory::NotPrimaryError>() &&
                !status.isA<ErrorCategory::ShutdownError>()) {
                LOGV2_ERROR(5460505,
                            "Error running rename collection",
                            "namespace"_attr = nss(),
                            "error"_attr = redact(status));
            }

            return status;
        });
}

}  // namespace mongo
