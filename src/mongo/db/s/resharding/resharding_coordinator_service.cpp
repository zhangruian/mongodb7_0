/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/resharding/resharding_coordinator_service.h"

#include "mongo/db/auth/authorization_session_impl.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_session_cache.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/config/initial_split_policy.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/resharding/resharding_coordinator_commit_monitor.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/sharding_util.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/flush_resharding_state_change_gen.h"
#include "mongo/s/shard_id.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_util.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

using namespace fmt::literals;

MONGO_FAIL_POINT_DEFINE(reshardingPauseCoordinatorBeforeCloning);
MONGO_FAIL_POINT_DEFINE(reshardingPauseCoordinatorInSteadyState);
MONGO_FAIL_POINT_DEFINE(reshardingPauseCoordinatorBeforeDecisionPersisted);
MONGO_FAIL_POINT_DEFINE(reshardingPauseCoordinatorBeforeCompletion);
MONGO_FAIL_POINT_DEFINE(reshardingPauseCoordinatorBeforeStartingErrorFlow);
MONGO_FAIL_POINT_DEFINE(reshardingPauseCoordinatorBeforePersistingStateTransition);

const std::string kReshardingCoordinatorActiveIndexName = "ReshardingCoordinatorActiveIndex";
const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

bool shouldStopAttemptingToCreateIndex(Status status, const CancellationToken& token) {
    return status.isOK() || token.isCanceled();
}

void assertNumDocsModifiedMatchesExpected(const BatchedCommandRequest& request,
                                          const BSONObj& response,
                                          int expected) {
    auto numDocsModified = response.getIntField("n");
    uassert(5030401,
            str::stream() << "Expected to match " << expected << " docs, but only matched "
                          << numDocsModified << " for write request " << request.toString(),
            expected == numDocsModified);
}

void appendShardEntriesToSetBuilder(const ReshardingCoordinatorDocument& coordinatorDoc,
                                    BSONObjBuilder& setBuilder) {
    BSONArrayBuilder donorShards(
        setBuilder.subarrayStart(ReshardingCoordinatorDocument::kDonorShardsFieldName));
    for (const auto& donorShard : coordinatorDoc.getDonorShards()) {
        donorShards.append(donorShard.toBSON());
    }
    donorShards.doneFast();

    BSONArrayBuilder recipientShards(
        setBuilder.subarrayStart(ReshardingCoordinatorDocument::kRecipientShardsFieldName));
    for (const auto& recipientShard : coordinatorDoc.getRecipientShards()) {
        recipientShards.append(recipientShard.toBSON());
    }
    recipientShards.doneFast();
}

void unsetInitializingFields(BSONObjBuilder& updateBuilder) {
    BSONObjBuilder unsetBuilder(updateBuilder.subobjStart("$unset"));
    unsetBuilder.append(ReshardingCoordinatorDocument::kPresetReshardedChunksFieldName, "");
    unsetBuilder.append(ReshardingCoordinatorDocument::kZonesFieldName, "");
    unsetBuilder.doneFast();
}

void writeToCoordinatorStateNss(OperationContext* opCtx,
                                const ReshardingCoordinatorDocument& coordinatorDoc,
                                TxnNumber txnNumber) {
    BatchedCommandRequest request([&] {
        auto nextState = coordinatorDoc.getState();
        switch (nextState) {
            case CoordinatorStateEnum::kInitializing:
                // Insert the new coordinator document.
                return BatchedCommandRequest::buildInsertOp(
                    NamespaceString::kConfigReshardingOperationsNamespace,
                    std::vector<BSONObj>{coordinatorDoc.toBSON()});
            case CoordinatorStateEnum::kDone:
                // Remove the coordinator document.
                return BatchedCommandRequest::buildDeleteOp(
                    NamespaceString::kConfigReshardingOperationsNamespace,
                    BSON("_id" << coordinatorDoc.getReshardingUUID()),  // query
                    false                                               // multi
                );
            default: {
                // Partially update the coordinator document.
                BSONObjBuilder updateBuilder;
                {
                    BSONObjBuilder setBuilder(updateBuilder.subobjStart("$set"));

                    // Always update the state field.
                    setBuilder.append(ReshardingCoordinatorDocument::kStateFieldName,
                                      CoordinatorState_serializer(coordinatorDoc.getState()));

                    if (auto cloneTimestamp = coordinatorDoc.getCloneTimestamp()) {
                        // If the cloneTimestamp exists, include it in the update.
                        setBuilder.append(ReshardingCoordinatorDocument::kCloneTimestampFieldName,
                                          *cloneTimestamp);
                    }

                    if (auto abortReason = coordinatorDoc.getAbortReason()) {
                        // If the abortReason exists, include it in the update.
                        setBuilder.append(ReshardingCoordinatorDocument::kAbortReasonFieldName,
                                          *abortReason);
                    }

                    if (auto approxBytesToCopy = coordinatorDoc.getApproxBytesToCopy()) {
                        // If the approxBytesToCopy exists, include it in the update.
                        setBuilder.append(
                            ReshardingCoordinatorDocument::kApproxBytesToCopyFieldName,
                            *approxBytesToCopy);
                    }

                    if (auto approxDocumentsToCopy = coordinatorDoc.getApproxDocumentsToCopy()) {
                        // If the approxDocumentsToCopy exists, include it in the update.
                        setBuilder.append(
                            ReshardingCoordinatorDocument::kApproxDocumentsToCopyFieldName,
                            *approxDocumentsToCopy);
                    }

                    if (nextState == CoordinatorStateEnum::kPreparingToDonate) {
                        appendShardEntriesToSetBuilder(coordinatorDoc, setBuilder);
                        setBuilder.doneFast();
                        unsetInitializingFields(updateBuilder);
                    }
                }

                return BatchedCommandRequest::buildUpdateOp(
                    NamespaceString::kConfigReshardingOperationsNamespace,
                    BSON("_id" << coordinatorDoc.getReshardingUUID()),
                    updateBuilder.obj(),
                    false,  // upsert
                    false   // multi
                );
            }
        }
    }());

    auto expectedNumModified = (request.getBatchType() == BatchedCommandRequest::BatchType_Insert)
        ? boost::none
        : boost::make_optional(1);
    auto res = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx, NamespaceString::kConfigReshardingOperationsNamespace, request, txnNumber);

    if (expectedNumModified) {
        assertNumDocsModifiedMatchesExpected(request, res, *expectedNumModified);
    }
}

/**
 * Creates reshardingFields.recipientFields for the resharding operation. Note: these should not
 * change once the operation has begun.
 */
TypeCollectionRecipientFields constructRecipientFields(
    const ReshardingCoordinatorDocument& coordinatorDoc) {
    std::vector<DonorShardFetchTimestamp> donorShards;

    for (const auto& donor : coordinatorDoc.getDonorShards()) {
        DonorShardFetchTimestamp donorFetchTimestamp(donor.getId());
        donorFetchTimestamp.setMinFetchTimestamp(donor.getMutableState().getMinFetchTimestamp());
        donorShards.push_back(std::move(donorFetchTimestamp));
    }

    TypeCollectionRecipientFields recipientFields(
        std::move(donorShards),
        coordinatorDoc.getSourceUUID(),
        coordinatorDoc.getSourceNss(),
        resharding::gReshardingMinimumOperationDurationMillis.load());

    emplaceCloneTimestampIfExists(recipientFields, coordinatorDoc.getCloneTimestamp());
    emplaceApproxBytesToCopyIfExists(recipientFields,
                                     coordinatorDoc.getReshardingApproxCopySizeStruct());

    return recipientFields;
}

BSONObj createReshardingFieldsUpdateForOriginalNss(
    OperationContext* opCtx,
    const ReshardingCoordinatorDocument& coordinatorDoc,
    boost::optional<OID> newCollectionEpoch,
    boost::optional<Timestamp> newCollectionTimestamp) {
    auto nextState = coordinatorDoc.getState();
    switch (nextState) {
        case CoordinatorStateEnum::kInitializing: {
            // Append 'reshardingFields' to the config.collections entry for the original nss
            TypeCollectionReshardingFields originalEntryReshardingFields(
                coordinatorDoc.getReshardingUUID());
            originalEntryReshardingFields.setState(coordinatorDoc.getState());

            return BSON("$set" << BSON(CollectionType::kReshardingFieldsFieldName
                                       << originalEntryReshardingFields.toBSON()
                                       << CollectionType::kUpdatedAtFieldName
                                       << opCtx->getServiceContext()->getPreciseClockSource()->now()
                                       << CollectionType::kAllowMigrationsFieldName << false));
        }
        case CoordinatorStateEnum::kPreparingToDonate: {
            TypeCollectionDonorFields donorFields(
                coordinatorDoc.getTempReshardingNss(),
                coordinatorDoc.getReshardingKey(),
                extractShardIdsFromParticipantEntries(coordinatorDoc.getRecipientShards()));

            BSONObjBuilder updateBuilder;
            {
                BSONObjBuilder setBuilder(updateBuilder.subobjStart("$set"));
                {
                    setBuilder.append(CollectionType::kReshardingFieldsFieldName + "." +
                                          TypeCollectionReshardingFields::kStateFieldName,
                                      CoordinatorState_serializer(nextState));

                    setBuilder.append(CollectionType::kReshardingFieldsFieldName + "." +
                                          TypeCollectionReshardingFields::kDonorFieldsFieldName,
                                      donorFields.toBSON());

                    setBuilder.append(CollectionType::kUpdatedAtFieldName,
                                      opCtx->getServiceContext()->getPreciseClockSource()->now());
                }

                setBuilder.doneFast();
            }

            return updateBuilder.obj();
        }
        case CoordinatorStateEnum::kCommitting: {
            // Update the config.collections entry for the original nss to reflect
            // the new sharded collection. Set 'uuid' to the reshardingUUID, 'key' to the new shard
            // key, 'lastmodEpoch' to newCollectionEpoch, and 'timestamp' to
            // newCollectionTimestamp (if newCollectionTimestamp has a value; i.e. when the
            // gShardingFullDDLSupportTimestampedVersion feature flag is enabled). Also update the
            // 'state' field and add the 'recipientFields' to the 'reshardingFields' section.
            auto recipientFields = constructRecipientFields(coordinatorDoc);
            BSONObj setFields =
                BSON("uuid" << coordinatorDoc.getReshardingUUID() << "key"
                            << coordinatorDoc.getReshardingKey().toBSON() << "lastmodEpoch"
                            << newCollectionEpoch.get() << "lastmod"
                            << opCtx->getServiceContext()->getPreciseClockSource()->now()
                            << "reshardingFields.state"
                            << CoordinatorState_serializer(coordinatorDoc.getState()).toString()
                            << "reshardingFields.recipientFields" << recipientFields.toBSON());
            if (newCollectionTimestamp.has_value()) {
                setFields = setFields.addFields(BSON("timestamp" << newCollectionTimestamp.get()));
            }

            return BSON("$set" << setFields);
        }
        case mongo::CoordinatorStateEnum::kDone:
            // Remove 'reshardingFields' from the config.collections entry
            return BSON(
                "$unset" << BSON(CollectionType::kReshardingFieldsFieldName
                                 << "" << CollectionType::kAllowMigrationsFieldName << "")
                         << "$set"
                         << BSON(CollectionType::kUpdatedAtFieldName
                                 << opCtx->getServiceContext()->getPreciseClockSource()->now()));
        default: {
            // Update the 'state' field, and 'abortReason' field if it exists, in the
            // 'reshardingFields' section.
            BSONObjBuilder updateBuilder;
            {
                BSONObjBuilder setBuilder(updateBuilder.subobjStart("$set"));

                setBuilder.append("reshardingFields.state",
                                  CoordinatorState_serializer(nextState).toString());
                setBuilder.append("lastmod",
                                  opCtx->getServiceContext()->getPreciseClockSource()->now());

                if (auto abortReason = coordinatorDoc.getAbortReason()) {
                    // If the abortReason exists, include it in the update.
                    setBuilder.append("reshardingFields.abortReason", *abortReason);

                    auto abortStatus = getStatusFromAbortReason(coordinatorDoc);
                    setBuilder.append("reshardingFields.userCanceled",
                                      abortStatus == ErrorCodes::ReshardCollectionAborted);
                }

                setBuilder.doneFast();

                if (coordinatorDoc.getAbortReason()) {
                    updateBuilder.append("$unset",
                                         BSON(CollectionType::kAllowMigrationsFieldName << ""));
                }
            }

            return updateBuilder.obj();
        }
    }
}

void updateConfigCollectionsForOriginalNss(OperationContext* opCtx,
                                           const ReshardingCoordinatorDocument& coordinatorDoc,
                                           boost::optional<OID> newCollectionEpoch,
                                           boost::optional<Timestamp> newCollectionTimestamp,
                                           TxnNumber txnNumber) {
    auto writeOp = createReshardingFieldsUpdateForOriginalNss(
        opCtx, coordinatorDoc, newCollectionEpoch, newCollectionTimestamp);

    auto request = BatchedCommandRequest::buildUpdateOp(
        CollectionType::ConfigNS,
        BSON(CollectionType::kNssFieldName << coordinatorDoc.getSourceNss().ns()),  // query
        writeOp,
        false,  // upsert
        false   // multi
    );

    auto res = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx, CollectionType::ConfigNS, request, txnNumber);

    assertNumDocsModifiedMatchesExpected(request, res, 1 /* expected */);
}

void writeToConfigCollectionsForTempNss(OperationContext* opCtx,
                                        const ReshardingCoordinatorDocument& coordinatorDoc,
                                        boost::optional<ChunkVersion> chunkVersion,
                                        boost::optional<const BSONObj&> collation,
                                        TxnNumber txnNumber) {
    BatchedCommandRequest request([&] {
        auto nextState = coordinatorDoc.getState();
        switch (nextState) {
            case CoordinatorStateEnum::kPreparingToDonate: {
                // Insert new entry for the temporary nss into config.collections
                auto collType = resharding::createTempReshardingCollectionType(
                    opCtx, coordinatorDoc, chunkVersion.get(), collation.get());
                return BatchedCommandRequest::buildInsertOp(
                    CollectionType::ConfigNS, std::vector<BSONObj>{collType.toBSON()});
            }
            case CoordinatorStateEnum::kCloning: {
                // Update the 'state', 'donorShards', 'approxCopySize', and 'cloneTimestamp' fields
                // in the 'reshardingFields.recipient' section

                BSONArrayBuilder donorShardsBuilder;
                for (const auto& donor : coordinatorDoc.getDonorShards()) {
                    DonorShardFetchTimestamp donorShardFetchTimestamp(donor.getId());
                    donorShardFetchTimestamp.setMinFetchTimestamp(
                        donor.getMutableState().getMinFetchTimestamp());
                    donorShardsBuilder.append(donorShardFetchTimestamp.toBSON());
                }

                return BatchedCommandRequest::buildUpdateOp(
                    CollectionType::ConfigNS,
                    BSON(CollectionType::kNssFieldName
                         << coordinatorDoc.getTempReshardingNss().ns()),
                    BSON("$set" << BSON(
                             "reshardingFields.state"
                             << CoordinatorState_serializer(nextState).toString()
                             << "reshardingFields.recipientFields.approxDocumentsToCopy"
                             << coordinatorDoc.getApproxDocumentsToCopy().get()
                             << "reshardingFields.recipientFields.approxBytesToCopy"
                             << coordinatorDoc.getApproxBytesToCopy().get()
                             << "reshardingFields.recipientFields.cloneTimestamp"
                             << coordinatorDoc.getCloneTimestamp().get()
                             << "reshardingFields.recipientFields.donorShards"
                             << donorShardsBuilder.arr() << "lastmod"
                             << opCtx->getServiceContext()->getPreciseClockSource()->now())),
                    false,  // upsert
                    false   // multi
                );
            }
            case CoordinatorStateEnum::kCommitting:
                // Remove the entry for the temporary nss
                return BatchedCommandRequest::buildDeleteOp(
                    CollectionType::ConfigNS,
                    BSON(CollectionType::kNssFieldName
                         << coordinatorDoc.getTempReshardingNss().ns()),
                    false  // multi
                );
            default: {
                // Update the 'state' field, and 'abortReason' field if it exists, in the
                // 'reshardingFields' section.
                BSONObjBuilder updateBuilder;
                {
                    BSONObjBuilder setBuilder(updateBuilder.subobjStart("$set"));

                    setBuilder.append("reshardingFields.state",
                                      CoordinatorState_serializer(nextState).toString());
                    setBuilder.append("lastmod",
                                      opCtx->getServiceContext()->getPreciseClockSource()->now());

                    if (auto abortReason = coordinatorDoc.getAbortReason()) {
                        setBuilder.append("reshardingFields.abortReason", *abortReason);

                        auto abortStatus = getStatusFromAbortReason(coordinatorDoc);
                        setBuilder.append("reshardingFields.userCanceled",
                                          abortStatus == ErrorCodes::ReshardCollectionAborted);
                    }
                }

                return BatchedCommandRequest::buildUpdateOp(
                    CollectionType::ConfigNS,
                    BSON(CollectionType::kNssFieldName
                         << coordinatorDoc.getTempReshardingNss().ns()),
                    updateBuilder.obj(),
                    true,  // upsert
                    false  // multi
                );
            }
        }
    }());

    auto expectedNumModified = (request.getBatchType() == BatchedCommandRequest::BatchType_Insert)
        ? boost::none
        : boost::make_optional(1);

    auto res = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx, CollectionType::ConfigNS, request, txnNumber);

    if (expectedNumModified) {
        assertNumDocsModifiedMatchesExpected(request, res, *expectedNumModified);
    }
}

void insertChunkAndTagDocsForTempNss(OperationContext* opCtx,
                                     std::vector<ChunkType> initialChunks,
                                     std::vector<BSONObj> newZones,
                                     TxnNumber txnNumber) {
    // Insert new initial chunk documents for temp nss
    std::vector<BSONObj> initialChunksBSON(initialChunks.size());
    std::transform(initialChunks.begin(),
                   initialChunks.end(),
                   initialChunksBSON.begin(),
                   [](ChunkType chunk) { return chunk.toConfigBSON(); });

    ShardingCatalogManager::get(opCtx)->insertConfigDocumentsInTxn(
        opCtx, ChunkType::ConfigNS, std::move(initialChunksBSON), txnNumber);

    ShardingCatalogManager::get(opCtx)->insertConfigDocumentsInTxn(
        opCtx, TagsType::ConfigNS, newZones, txnNumber);
}

void removeChunkAndTagsDocs(OperationContext* opCtx,
                            const NamespaceString& ns,
                            const boost::optional<UUID>& collUUID,
                            TxnNumber txnNumber) {
    // Remove all chunk documents for the original nss. We do not know how many chunk docs currently
    // exist, so cannot pass a value for expectedNumModified
    const auto chunksQuery = [&]() {
        if (collUUID) {
            return BSON(ChunkType::collectionUUID() << *collUUID);
        } else {
            return BSON(ChunkType::ns(ns.ns()));
        }
    }();

    ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx,
        ChunkType::ConfigNS,
        BatchedCommandRequest::buildDeleteOp(ChunkType::ConfigNS,
                                             chunksQuery,
                                             true  // multi
                                             ),
        txnNumber);

    // Remove all tag documents for the original nss. We do not know how many tag docs currently
    // exist, so cannot pass a value for expectedNumModified
    ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx,
        TagsType::ConfigNS,
        BatchedCommandRequest::buildDeleteOp(TagsType::ConfigNS,
                                             BSON(ChunkType::ns(ns.ns())),  // query
                                             true                           // multi
                                             ),
        txnNumber);
}

void removeConfigMetadataForTempNss(OperationContext* opCtx,
                                    const ReshardingCoordinatorDocument& coordinatorDoc,
                                    TxnNumber txnNumber) {
    auto delCollEntryRequest = BatchedCommandRequest::buildDeleteOp(
        CollectionType::ConfigNS,
        BSON(CollectionType::kNssFieldName << coordinatorDoc.getTempReshardingNss().ns()),  // query
        false                                                                               // multi
    );

    (void)ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx, CollectionType::ConfigNS, delCollEntryRequest, txnNumber);

    boost::optional<UUID> reshardingTempUUID;
    if (feature_flags::gShardingFullDDLSupportTimestampedVersion.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        reshardingTempUUID = coordinatorDoc.getReshardingUUID();
    }

    removeChunkAndTagsDocs(
        opCtx, coordinatorDoc.getTempReshardingNss(), reshardingTempUUID, txnNumber);
}

void updateChunkAndTagsDocsForTempNss(OperationContext* opCtx,
                                      const ReshardingCoordinatorDocument& coordinatorDoc,
                                      OID newCollectionEpoch,
                                      boost::optional<Timestamp> newCollectionTimestamp,
                                      TxnNumber txnNumber) {
    // Update all chunk documents that currently have 'ns' as the temporary collection namespace
    // such that 'ns' is now the original collection namespace and 'lastmodEpoch' is
    // newCollectionEpoch.
    const auto chunksQuery = [&]() {
        if (newCollectionTimestamp) {
            return BSON(ChunkType::collectionUUID() << coordinatorDoc.getReshardingUUID());
        } else {
            return BSON(ChunkType::ns(coordinatorDoc.getTempReshardingNss().ns()));
        }
    }();
    const auto chunksUpdate = [&]() {
        if (newCollectionTimestamp) {
            return BSON("$set" << BSON(ChunkType::epoch << newCollectionEpoch
                                                        << ChunkType::timestamp
                                                        << *newCollectionTimestamp));
        } else {
            return BSON("$set" << BSON(ChunkType::ns << coordinatorDoc.getSourceNss().ns()
                                                     << ChunkType::epoch << newCollectionEpoch));
        }
    }();
    auto chunksRequest = BatchedCommandRequest::buildUpdateOp(ChunkType::ConfigNS,
                                                              chunksQuery,   // query
                                                              chunksUpdate,  // update
                                                              false,         // upsert
                                                              true           // multi
    );

    auto chunksRes = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx, ChunkType::ConfigNS, chunksRequest, txnNumber);

    auto tagsRequest = BatchedCommandRequest::buildUpdateOp(
        TagsType::ConfigNS,
        BSON(TagsType::ns(coordinatorDoc.getTempReshardingNss().ns())),    // query
        BSON("$set" << BSON("ns" << coordinatorDoc.getSourceNss().ns())),  // update
        false,                                                             // upsert
        true                                                               // multi
    );

    // Update the 'ns' field to be the original collection namespace for all tags documents that
    // currently have 'ns' as the temporary collection namespace
    auto tagsRes = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx, TagsType::ConfigNS, tagsRequest, txnNumber);
}

/**
 * Executes metadata changes in a transaction without bumping the collection version.
 */
void executeMetadataChangesInTxn(
    OperationContext* opCtx,
    unique_function<void(OperationContext*, TxnNumber)> changeMetadataFunc) {
    ShardingCatalogManager::withTransaction(opCtx,
                                            NamespaceString::kConfigReshardingOperationsNamespace,
                                            [&](OperationContext* opCtx, TxnNumber txnNumber) {
                                                changeMetadataFunc(opCtx, txnNumber);
                                            });
}

}  // namespace

namespace resharding {
CollectionType createTempReshardingCollectionType(
    OperationContext* opCtx,
    const ReshardingCoordinatorDocument& coordinatorDoc,
    const ChunkVersion& chunkVersion,
    const BSONObj& collation) {
    CollectionType collType(coordinatorDoc.getTempReshardingNss(),
                            chunkVersion.epoch(),
                            opCtx->getServiceContext()->getPreciseClockSource()->now(),
                            coordinatorDoc.getReshardingUUID());
    collType.setKeyPattern(coordinatorDoc.getReshardingKey());
    collType.setDefaultCollation(collation);
    collType.setUnique(false);
    collType.setTimestamp(chunkVersion.getTimestamp());

    TypeCollectionReshardingFields tempEntryReshardingFields(coordinatorDoc.getReshardingUUID());
    tempEntryReshardingFields.setState(coordinatorDoc.getState());

    auto recipientFields = constructRecipientFields(coordinatorDoc);
    tempEntryReshardingFields.setRecipientFields(std::move(recipientFields));
    collType.setReshardingFields(std::move(tempEntryReshardingFields));
    collType.setAllowMigrations(false);
    return collType;
}

void writeDecisionPersistedState(OperationContext* opCtx,
                                 const ReshardingCoordinatorDocument& coordinatorDoc,
                                 OID newCollectionEpoch,
                                 boost::optional<Timestamp> newCollectionTimestamp) {
    // No need to bump originalNss version because its epoch will be changed.
    executeMetadataChangesInTxn(opCtx, [&](OperationContext* opCtx, TxnNumber txnNumber) {
        // Update the config.reshardingOperations entry
        writeToCoordinatorStateNss(opCtx, coordinatorDoc, txnNumber);

        // Remove the config.collections entry for the temporary collection
        writeToConfigCollectionsForTempNss(
            opCtx, coordinatorDoc, boost::none, boost::none, txnNumber);

        // Update the config.collections entry for the original namespace to reflect the new
        // shard key, new epoch, and new UUID
        updateConfigCollectionsForOriginalNss(
            opCtx, coordinatorDoc, newCollectionEpoch, newCollectionTimestamp, txnNumber);

        // Remove all chunk and tag documents associated with the original collection, then
        // update the chunk and tag docs currently associated with the temp nss to be associated
        // with the original nss

        boost::optional<UUID> collUUID;
        if (newCollectionTimestamp) {
            collUUID = coordinatorDoc.getSourceUUID();
        }

        removeChunkAndTagsDocs(opCtx, coordinatorDoc.getSourceNss(), collUUID, txnNumber);
        updateChunkAndTagsDocsForTempNss(
            opCtx, coordinatorDoc, newCollectionEpoch, newCollectionTimestamp, txnNumber);
    });
}
}  // namespace resharding

ChunkVersion calculateChunkVersionForInitialChunks(OperationContext* opCtx) {
    boost::optional<Timestamp> timestamp;
    if (feature_flags::gShardingFullDDLSupportTimestampedVersion.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        const auto now = VectorClock::get(opCtx)->getTime();
        timestamp = now.clusterTime().asTimestamp();
    }

    return ChunkVersion(1, 0, OID::gen(), timestamp);
}

std::vector<DonorShardEntry> constructDonorShardEntries(const std::set<ShardId>& donorShardIds) {
    std::vector<DonorShardEntry> donorShards;
    std::transform(donorShardIds.begin(),
                   donorShardIds.end(),
                   std::back_inserter(donorShards),
                   [](const ShardId& shardId) -> DonorShardEntry {
                       DonorShardContext donorCtx;
                       donorCtx.setState(DonorStateEnum::kUnused);
                       return DonorShardEntry{shardId, std::move(donorCtx)};
                   });
    return donorShards;
}

std::vector<RecipientShardEntry> constructRecipientShardEntries(
    const std::set<ShardId>& recipientShardIds) {
    std::vector<RecipientShardEntry> recipientShards;
    std::transform(recipientShardIds.begin(),
                   recipientShardIds.end(),
                   std::back_inserter(recipientShards),
                   [](const ShardId& shardId) -> RecipientShardEntry {
                       RecipientShardContext recipientCtx;
                       recipientCtx.setState(RecipientStateEnum::kUnused);
                       return RecipientShardEntry{shardId, std::move(recipientCtx)};
                   });
    return recipientShards;
}

void ReshardingCoordinatorExternalStateImpl::insertCoordDocAndChangeOrigCollEntry(
    OperationContext* opCtx, const ReshardingCoordinatorDocument& coordinatorDoc) {

    ShardingCatalogManager::get(opCtx)->bumpCollectionVersionAndChangeMetadataInTxn(
        opCtx, coordinatorDoc.getSourceNss(), [&](OperationContext* opCtx, TxnNumber txnNumber) {
            // Insert the coordinator document to config.reshardingOperations.
            invariant(coordinatorDoc.getActive());
            try {
                writeToCoordinatorStateNss(opCtx, coordinatorDoc, txnNumber);
            } catch (const ExceptionFor<ErrorCodes::DuplicateKey>& ex) {
                auto extraInfo = ex.extraInfo<DuplicateKeyErrorInfo>();
                if (extraInfo->getKeyPattern().woCompare(BSON("active" << 1)) == 0) {
                    uasserted(ErrorCodes::ReshardCollectionInProgress,
                              str::stream()
                                  << "Only one resharding operation is allowed to be active at a "
                                     "time, aborting resharding op for "
                                  << coordinatorDoc.getSourceNss());
                }

                throw;
            }

            // Update the config.collections entry for the original collection to include
            // 'reshardingFields'
            updateConfigCollectionsForOriginalNss(
                opCtx, coordinatorDoc, boost::none, boost::none, txnNumber);
        });
}

ReshardingCoordinatorExternalState::ParticipantShardsAndChunks
ReshardingCoordinatorExternalStateImpl::calculateParticipantShardsAndChunks(
    OperationContext* opCtx, const ReshardingCoordinatorDocument& coordinatorDoc) {
    const auto cm = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(
            opCtx, coordinatorDoc.getSourceNss()));

    std::set<ShardId> donorShardIds;
    cm.getAllShardIds(&donorShardIds);

    std::set<ShardId> recipientShardIds;
    std::vector<ChunkType> initialChunks;

    // The database primary must always be a recipient to ensure it ends up with consistent
    // collection metadata.
    recipientShardIds.emplace(cm.dbPrimary());

    if (const auto& chunks = coordinatorDoc.getPresetReshardedChunks()) {
        auto version = calculateChunkVersionForInitialChunks(opCtx);

        // Use the provided shardIds from presetReshardedChunks to construct the
        // recipient list.
        for (const BSONObj& obj : *chunks) {
            recipientShardIds.emplace(
                obj.getStringField(ReshardedChunk::kRecipientShardIdFieldName));

            auto reshardedChunk =
                ReshardedChunk::parse(IDLParserErrorContext("ReshardedChunk"), obj);
            if (version.getTimestamp()) {
                initialChunks.emplace_back(
                    coordinatorDoc.getReshardingUUID(),
                    ChunkRange{reshardedChunk.getMin(), reshardedChunk.getMax()},
                    version,
                    reshardedChunk.getRecipientShardId());
            } else {
                initialChunks.emplace_back(
                    coordinatorDoc.getTempReshardingNss(),
                    ChunkRange{reshardedChunk.getMin(), reshardedChunk.getMax()},
                    version,
                    reshardedChunk.getRecipientShardId());
            }
            version.incMinor();
        }
    } else {
        int numInitialChunks = coordinatorDoc.getNumInitialChunks()
            ? *coordinatorDoc.getNumInitialChunks()
            : cm.numChunks();

        ShardKeyPattern shardKey(coordinatorDoc.getReshardingKey());
        const auto tempNs = coordinatorDoc.getTempReshardingNss();

        boost::optional<std::vector<mongo::TagsType>> parsedZones;
        if (auto rawBSONZones = coordinatorDoc.getZones()) {
            parsedZones.emplace();
            parsedZones->reserve(rawBSONZones->size());

            for (const auto& zone : *rawBSONZones) {
                ChunkRange range(zone.getMin(), zone.getMax());
                TagsType tag(
                    coordinatorDoc.getTempReshardingNss(), zone.getZone().toString(), range);

                parsedZones->push_back(tag);
            }
        }

        auto initialSplitter = ReshardingSplitPolicy::make(opCtx,
                                                           coordinatorDoc.getSourceNss(),
                                                           tempNs,
                                                           shardKey,
                                                           numInitialChunks,
                                                           std::move(parsedZones));

        // Note: The resharding initial split policy doesn't care about what is the real primary
        // shard, so just pass in a random shard.
        const SplitPolicyParams splitParams{
            tempNs,
            coordinatorDoc.getReshardingUUID(),
            *donorShardIds.begin(),
            ChunkEntryFormat::getForVersionCallerGuaranteesFCVStability(
                ServerGlobalParams::FeatureCompatibility::Version::kVersion50)};
        auto splitResult = initialSplitter.createFirstChunks(opCtx, shardKey, splitParams);
        initialChunks = std::move(splitResult.chunks);

        for (const auto& chunk : initialChunks) {
            recipientShardIds.insert(chunk.getShard());
        }
    }

    return {constructDonorShardEntries(donorShardIds),
            constructRecipientShardEntries(recipientShardIds),
            initialChunks};
}

void ReshardingCoordinatorExternalStateImpl::writeParticipantShardsAndTempCollInfo(
    OperationContext* opCtx,
    const ReshardingCoordinatorDocument& updatedCoordinatorDoc,
    std::vector<ChunkType> initialChunks,
    std::vector<BSONObj> zones) {
    ShardingCatalogManager::get(opCtx)->bumpCollectionVersionAndChangeMetadataInTxn(
        opCtx,
        updatedCoordinatorDoc.getSourceNss(),
        [&](OperationContext* opCtx, TxnNumber txnNumber) {
            // Update on-disk state to reflect latest state transition.
            writeToCoordinatorStateNss(opCtx, updatedCoordinatorDoc, txnNumber);
            updateConfigCollectionsForOriginalNss(
                opCtx, updatedCoordinatorDoc, boost::none, boost::none, txnNumber);

            // Insert the config.collections entry for the temporary resharding collection. The
            // chunks all have the same epoch, so picking the last chunk here is arbitrary.
            auto chunkVersion = initialChunks.back().getVersion();
            writeToConfigCollectionsForTempNss(
                opCtx, updatedCoordinatorDoc, chunkVersion, CollationSpec::kSimpleSpec, txnNumber);

            insertChunkAndTagDocsForTempNss(opCtx, std::move(initialChunks), zones, txnNumber);
        });
}

void ReshardingCoordinatorExternalStateImpl::
    writeStateTransitionAndCatalogUpdatesThenBumpShardVersions(
        OperationContext* opCtx, const ReshardingCoordinatorDocument& coordinatorDoc) {
    // Run updates to config.reshardingOperations and config.collections in a transaction
    auto nextState = coordinatorDoc.getState();

    std::vector<NamespaceString> collNames = {coordinatorDoc.getSourceNss()};
    if (nextState < CoordinatorStateEnum::kCommitting) {
        collNames.emplace_back(coordinatorDoc.getTempReshardingNss());
    }

    ShardingCatalogManager::get(opCtx)->bumpMultipleCollectionVersionsAndChangeMetadataInTxn(
        opCtx, collNames, [&](OperationContext* opCtx, TxnNumber txnNumber) {
            // Update the config.reshardingOperations entry
            writeToCoordinatorStateNss(opCtx, coordinatorDoc, txnNumber);

            // Update the config.collections entry for the original collection
            updateConfigCollectionsForOriginalNss(
                opCtx, coordinatorDoc, boost::none, boost::none, txnNumber);

            // Update the config.collections entry for the temporary resharding collection. If we've
            // already successfully committed that the operation will succeed, we've removed the
            // entry for the temporary collection and updated the entry with original namespace to
            // have the new shard key, UUID, and epoch
            if (nextState < CoordinatorStateEnum::kCommitting) {
                writeToConfigCollectionsForTempNss(
                    opCtx, coordinatorDoc, boost::none, boost::none, txnNumber);
            }
        });
}

void ReshardingCoordinatorExternalStateImpl::removeCoordinatorDocAndReshardingFields(
    OperationContext* opCtx,
    const ReshardingCoordinatorDocument& coordinatorDoc,
    boost::optional<Status> abortReason) {
    // If the coordinator needs to abort and isn't in kInitializing, additional collections need to
    // be cleaned up in the final transaction. Otherwise, cleanup for abort and success are the
    // same.
    const bool wasDecisionPersisted =
        coordinatorDoc.getState() == CoordinatorStateEnum::kCommitting;
    invariant((wasDecisionPersisted && !abortReason) || abortReason);

    ReshardingCoordinatorDocument updatedCoordinatorDoc = coordinatorDoc;
    updatedCoordinatorDoc.setState(CoordinatorStateEnum::kDone);
    emplaceAbortReasonIfExists(updatedCoordinatorDoc, abortReason);

    ShardingCatalogManager::get(opCtx)->bumpCollectionVersionAndChangeMetadataInTxn(
        opCtx,
        updatedCoordinatorDoc.getSourceNss(),
        [&](OperationContext* opCtx, TxnNumber txnNumber) {
            // Remove entry for this resharding operation from config.reshardingOperations
            writeToCoordinatorStateNss(opCtx, updatedCoordinatorDoc, txnNumber);

            // Remove the resharding fields from the config.collections entry
            updateConfigCollectionsForOriginalNss(
                opCtx, updatedCoordinatorDoc, boost::none, boost::none, txnNumber);

            // Once the decision has been persisted, the coordinator would have modified the
            // config.chunks and config.collections entry. This means that the UUID of the
            // non-temp collection is now the UUID of what was previously the UUID of the temp
            // collection. So don't try to call remove as it will end up removing the metadata
            // for the real collection.
            if (!wasDecisionPersisted) {
                removeConfigMetadataForTempNss(opCtx, updatedCoordinatorDoc, txnNumber);
            }
        });
}

ThreadPool::Limits ReshardingCoordinatorService::getThreadPoolLimits() const {
    ThreadPool::Limits threadPoolLimit;
    threadPoolLimit.maxThreads = resharding::gReshardingCoordinatorServiceMaxThreadCount;
    return threadPoolLimit;
}

std::shared_ptr<repl::PrimaryOnlyService::Instance> ReshardingCoordinatorService::constructInstance(
    BSONObj initialState) {
    return std::make_shared<ReshardingCoordinator>(
        this, std::move(initialState), std::make_shared<ReshardingCoordinatorExternalStateImpl>());
}

ExecutorFuture<void> ReshardingCoordinatorService::_rebuildService(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& token) {
    return AsyncTry([this] {
               auto nss = getStateDocumentsNS();

               AllowOpCtxWhenServiceRebuildingBlock allowOpCtxBlock(Client::getCurrent());
               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();
               DBDirectClient client(opCtx);
               BSONObj result;
               client.runCommand(
                   nss.db().toString(),
                   BSON("createIndexes"
                        << nss.coll().toString() << "indexes"
                        << BSON_ARRAY(BSON("key" << BSON("active" << 1) << "name"
                                                 << kReshardingCoordinatorActiveIndexName
                                                 << "unique" << true))),
                   result);
               uassertStatusOK(getStatusFromCommandResult(result));
           })
        .until([token](Status status) { return shouldStopAttemptingToCreateIndex(status, token); })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, CancellationToken::uncancelable());
}

ReshardingCoordinatorService::ReshardingCoordinator::ReshardingCoordinator(
    const ReshardingCoordinatorService* coordinatorService,
    const BSONObj& state,
    std::shared_ptr<ReshardingCoordinatorExternalState> externalState)
    : PrimaryOnlyService::TypedInstance<ReshardingCoordinator>(),
      _id(state["_id"].wrap().getOwned()),
      _coordinatorService(coordinatorService),
      _coordinatorDoc(ReshardingCoordinatorDocument::parse(
          IDLParserErrorContext("ReshardingCoordinatorStateDoc"), state)),
      _markKilledExecutor(std::make_shared<ThreadPool>([] {
          ThreadPool::Options options;
          options.poolName = "ReshardingCoordinatorCancelableOpCtxPool";
          options.minThreads = 1;
          options.maxThreads = 1;
          return options;
      }())),
      _reshardingCoordinatorExternalState(externalState) {
    _reshardingCoordinatorObserver = std::make_shared<ReshardingCoordinatorObserver>();
}

ReshardingCoordinatorService::ReshardingCoordinator::~ReshardingCoordinator() {
    invariant(_completionPromise.getFuture().isReady());
}

void ReshardingCoordinatorService::ReshardingCoordinator::installCoordinatorDoc(
    OperationContext* opCtx, const ReshardingCoordinatorDocument& doc) noexcept {
    invariant(doc.getReshardingUUID() == _coordinatorDoc.getReshardingUUID());

    BSONObjBuilder bob;
    bob.append("newState", CoordinatorState_serializer(doc.getState()));
    bob.append("oldState", CoordinatorState_serializer(_coordinatorDoc.getState()));
    bob.append("namespace", doc.getSourceNss().toString());
    bob.append("collectionUUID", doc.getSourceUUID().toString());
    bob.append("reshardingUUID", doc.getReshardingUUID().toString());
    ShardingLogging::get(opCtx)->logChange(opCtx,
                                           "resharding.coordinator.transition",
                                           doc.getSourceNss().toString(),
                                           bob.obj(),
                                           ShardingCatalogClient::kMajorityWriteConcern);

    LOGV2_INFO(5343001,
               "Transitioned resharding coordinator state",
               "newState"_attr = CoordinatorState_serializer(doc.getState()),
               "oldState"_attr = CoordinatorState_serializer(_coordinatorDoc.getState()),
               "namespace"_attr = doc.getSourceNss(),
               "collectionUUID"_attr = doc.getSourceUUID(),
               "reshardingUUID"_attr = doc.getReshardingUUID());

    _coordinatorDoc = doc;
}

void markCompleted(const Status& status) {
    auto metrics = ReshardingMetrics::get(cc().getServiceContext());
    if (status.isOK())
        metrics->onCompletion(ReshardingOperationStatusEnum::kSuccess);
    else if (status == ErrorCodes::ReshardCollectionAborted)
        metrics->onCompletion(ReshardingOperationStatusEnum::kCanceled);
    else
        metrics->onCompletion(ReshardingOperationStatusEnum::kFailure);
}

BSONObj createFlushReshardingStateChangeCommand(const NamespaceString& nss) {
    _flushReshardingStateChange cmd(nss);
    cmd.setDbName(nss.db());
    return cmd.toBSON(
        BSON(WriteConcernOptions::kWriteConcernField << WriteConcernOptions::Majority));
}

ExecutorFuture<ReshardingCoordinatorDocument>
ReshardingCoordinatorService::ReshardingCoordinator::_runUntilReadyToPersistDecision(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, executor] { _insertCoordDocAndChangeOrigCollEntry(); })
        .then([this, executor] { _calculateParticipantsAndChunksThenWriteToDisk(); })
        .onCompletion([this, executor](Status status) {
            if (_ctHolder->isSteppingOrShuttingDown()) {
                // Propagate any errors from the coordinator stepping down.
                return ExecutorFuture<void>(**executor, status);
            }

            if (_coordinatorDoc.getState() < CoordinatorStateEnum::kPreparingToDonate) {
                // Propagate any errors if the coordinator failed before transitioning to
                // kPreparingToDonate, meaning participants were never and should never be made
                // aware of this failed resharding operation.
                invariant(!status.isOK());
                return ExecutorFuture<void>(**executor, status);
            }

            // Regardless of error or non-error, guarantee that once the coordinator completes its
            // transition to kPreparingToDonate, participants are aware of the resharding operation
            // and their state machines are created.
            return ExecutorFuture<void>(**executor)
                .then([this, executor] { _tellAllDonorsToRefresh(executor); })
                .then([this, executor] { _tellAllRecipientsToRefresh(executor); })
                .then([status] { return status; });
        })
        .then([this, executor] { return _awaitAllDonorsReadyToDonate(executor); })
        .then([this, executor] { _tellAllRecipientsToRefresh(executor); })
        .then([this, executor] { return _awaitAllRecipientsFinishedCloning(executor); })
        .then([this, executor] { _tellAllDonorsToRefresh(executor); })
        .then([this, executor] { return _awaitAllRecipientsFinishedApplying(executor); })
        .then([this, executor] { _tellAllDonorsToRefresh(executor); })
        .then([this, executor] { return _awaitAllRecipientsInStrictConsistency(executor); })
        .onCompletion([this](auto passthroughFuture) {
            _cancelableOpCtxFactory.emplace(_ctHolder->getStepdownToken(), _markKilledExecutor);
            return passthroughFuture;
        })
        .onError([this, self = shared_from_this(), executor](
                     Status status) -> StatusWith<ReshardingCoordinatorDocument> {
            {
                auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                reshardingPauseCoordinatorBeforeStartingErrorFlow.pauseWhileSet(opCtx.get());
            }

            if (_ctHolder->isSteppingOrShuttingDown()) {
                return status;
            }

            // If the abort cancellation token was triggered, implying that a user ran the abort
            // command, override with the abort error code.
            if (_ctHolder->isAborted()) {
                status = {ErrorCodes::ReshardCollectionAborted, status.reason()};
            }

            auto nss = _coordinatorDoc.getSourceNss();
            LOGV2(4956902,
                  "Resharding failed",
                  "namespace"_attr = nss.ns(),
                  "newShardKeyPattern"_attr = _coordinatorDoc.getReshardingKey(),
                  "error"_attr = status);

            if (_coordinatorDoc.getState() == CoordinatorStateEnum::kUnused) {
                return status;
            }

            if (_coordinatorDoc.getState() < CoordinatorStateEnum::kPreparingToDonate) {
                // Participants were never made aware of the resharding opeartion. Abort without
                // waiting for participant acknowledgement.
                _onAbortCoordinatorOnly(executor, status);
            } else {
                _onAbortCoordinatorAndParticipants(executor, status);
            }
            return status;
        });
}

ExecutorFuture<void>
ReshardingCoordinatorService::ReshardingCoordinator::_persistDecisionAndFinishReshardOperation(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const ReshardingCoordinatorDocument& updatedCoordinatorDoc) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, self = shared_from_this(), executor, updatedCoordinatorDoc] {
            return _persistDecision(updatedCoordinatorDoc);
        })
        .then([this, self = shared_from_this(), executor] {
            _tellAllParticipantsToRefresh(_coordinatorDoc.getSourceNss(), executor);
        })
        .then([this, self = shared_from_this(), executor] {
            // The shared_ptr maintaining the ReshardingCoordinatorService Instance object gets
            // deleted from the PrimaryOnlyService's map. Thus, shared_from_this() is necessary to
            // keep 'this' pointer alive for the remaining callbacks.
            return _awaitAllParticipantShardsDone(executor);
        })
        .onError([this, self = shared_from_this(), executor](Status status) {
            {
                auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                reshardingPauseCoordinatorBeforeStartingErrorFlow.pauseWhileSet(opCtx.get());
            }

            if (_ctHolder->isSteppingOrShuttingDown()) {
                return status;
            }

            LOGV2_FATAL(5277000,
                        "Unrecoverable error past the point resharding was guaranteed to succeed",
                        "error"_attr = redact(status));
        });
}
SemiFuture<void> ReshardingCoordinatorService::ReshardingCoordinator::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& stepdownToken) noexcept {
    _ctHolder = std::make_unique<CoordinatorCancellationTokenHolder>(stepdownToken);
    _markKilledExecutor->startup();
    _cancelableOpCtxFactory.emplace(_ctHolder->getAbortToken(), _markKilledExecutor);

    return _runUntilReadyToPersistDecision(executor)
        .then([this, self = shared_from_this(), executor](
                  const ReshardingCoordinatorDocument& updatedCoordinatorDoc) {
            return _persistDecisionAndFinishReshardOperation(executor, updatedCoordinatorDoc);
        })
        .onCompletion([this, self = shared_from_this(), executor](Status status) {
            if (!_ctHolder->isSteppingOrShuttingDown() &&
                _coordinatorDoc.getState() != CoordinatorStateEnum::kUnused) {
                // Notify `ReshardingMetrics` as the operation is now complete for external
                // observers.
                markCompleted(status);
            }

            auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
            reshardingPauseCoordinatorBeforeCompletion.pauseWhileSetAndNotCanceled(
                opCtx.get(), _ctHolder->getStepdownToken());

            {
                auto lg = stdx::lock_guard(_fulfillmentMutex);
                if (status.isOK()) {
                    _completionPromise.emplaceValue();
                } else {
                    _completionPromise.setError(status);
                }
            }

            if (_criticalSectionTimeoutCbHandle) {
                (*executor)->cancel(*_criticalSectionTimeoutCbHandle);
            }

            return status;
        })
        .thenRunOn(_coordinatorService->getInstanceCleanupExecutor())
        .onCompletion([this, self = shared_from_this()](Status status) {
            // On stepdown or shutdown, the _scopedExecutor may have already been shut down.
            // Schedule cleanup work on the parent executor.
            if (_ctHolder->isSteppingOrShuttingDown()) {
                ReshardingMetrics::get(cc().getServiceContext())->onStepDown();
            }

            if (!status.isOK()) {
                {
                    auto lg = stdx::lock_guard(_fulfillmentMutex);
                    if (!_completionPromise.getFuture().isReady()) {
                        _completionPromise.setError(status);
                    }
                }
                _reshardingCoordinatorObserver->interrupt(status);
            }
        })
        .semi();
}

void ReshardingCoordinatorService::ReshardingCoordinator::_onAbortCoordinatorOnly(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor, const Status& status) {
    if (_coordinatorDoc.getState() == CoordinatorStateEnum::kUnused) {
        // No work to be done.
        return;
    }

    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());

    // The temporary collection and its corresponding entries were never created. Only the
    // coordinator document and reshardingFields require cleanup.
    _reshardingCoordinatorExternalState->removeCoordinatorDocAndReshardingFields(
        opCtx.get(), _coordinatorDoc, status);
    return;
}

void ReshardingCoordinatorService::ReshardingCoordinator::_onAbortCoordinatorAndParticipants(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor, const Status& status) {
    // Participants should never be waited upon to complete the abort if they were never made aware
    // of the resharding operation (the coordinator flushing its state change to
    // kPreparingToDonate).
    invariant(_coordinatorDoc.getState() >= CoordinatorStateEnum::kPreparingToDonate);

    // The coordinator only transitions into kAborting if there are participants to wait on before
    // transitioning to kDone.
    _updateCoordinatorDocStateAndCatalogEntries(
        CoordinatorStateEnum::kAborting, _coordinatorDoc, boost::none, boost::none, status);

    auto nss = _coordinatorDoc.getSourceNss();
    _tellAllParticipantsToRefresh(nss, executor);

    // Wait for all participants to acknowledge the operation reached an unrecoverable
    // error.
    future_util::withCancellation(_awaitAllParticipantShardsDone(executor),
                                  _ctHolder->getStepdownToken())
        .get();
}

void ReshardingCoordinatorService::ReshardingCoordinator::abort() {
    _ctHolder->abort();
}

boost::optional<BSONObj> ReshardingCoordinatorService::ReshardingCoordinator::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode,
    MongoProcessInterface::CurrentOpSessionsMode) noexcept {
    ReshardingMetrics::ReporterOptions options(
        ReshardingMetrics::ReporterOptions::Role::kCoordinator,
        _coordinatorDoc.getReshardingUUID(),
        _coordinatorDoc.getSourceNss(),
        _coordinatorDoc.getReshardingKey().toBSON(),
        false);
    return ReshardingMetrics::get(cc().getServiceContext())->reportForCurrentOp(options);
}

std::shared_ptr<ReshardingCoordinatorObserver>
ReshardingCoordinatorService::ReshardingCoordinator::getObserver() {
    return _reshardingCoordinatorObserver;
}

void ReshardingCoordinatorService::ReshardingCoordinator::onOkayToEnterCritical() {
    auto lg = stdx::lock_guard(_fulfillmentMutex);
    if (_canEnterCritical.getFuture().isReady())
        return;
    LOGV2(5391601, "Marking resharding operation okay to enter critical section");
    _canEnterCritical.emplaceValue();
}

void ReshardingCoordinatorService::ReshardingCoordinator::_insertCoordDocAndChangeOrigCollEntry() {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kUnused) {
        ReshardingMetrics::get(cc().getServiceContext())->onStepUp();
        return;
    }

    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    ReshardingCoordinatorDocument updatedCoordinatorDoc = _coordinatorDoc;
    updatedCoordinatorDoc.setState(CoordinatorStateEnum::kInitializing);
    _reshardingCoordinatorExternalState->insertCoordDocAndChangeOrigCollEntry(
        opCtx.get(), updatedCoordinatorDoc);
    installCoordinatorDoc(opCtx.get(), updatedCoordinatorDoc);

    // TODO SERVER-53914 to accommodate loading metrics for the coordinator.
    ReshardingMetrics::get(cc().getServiceContext())->onStart();
}

void ReshardingCoordinatorService::ReshardingCoordinator::
    _calculateParticipantsAndChunksThenWriteToDisk() {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kInitializing) {
        return;
    }

    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    ReshardingCoordinatorDocument updatedCoordinatorDoc = _coordinatorDoc;

    auto shardsAndChunks = _reshardingCoordinatorExternalState->calculateParticipantShardsAndChunks(
        opCtx.get(), updatedCoordinatorDoc);

    updatedCoordinatorDoc.setDonorShards(std::move(shardsAndChunks.donorShards));
    updatedCoordinatorDoc.setRecipientShards(std::move(shardsAndChunks.recipientShards));
    updatedCoordinatorDoc.setState(CoordinatorStateEnum::kPreparingToDonate);

    // Remove the presetReshardedChunks and zones from the coordinator document to reduce
    // the possibility of the document reaching the BSONObj size constraint.
    std::vector<BSONObj> zones;
    if (updatedCoordinatorDoc.getZones()) {
        zones = buildTagsDocsFromZones(updatedCoordinatorDoc.getTempReshardingNss(),
                                       *updatedCoordinatorDoc.getZones());
    }
    updatedCoordinatorDoc.setPresetReshardedChunks(boost::none);
    updatedCoordinatorDoc.setZones(boost::none);

    _reshardingCoordinatorExternalState->writeParticipantShardsAndTempCollInfo(
        opCtx.get(),
        updatedCoordinatorDoc,
        std::move(shardsAndChunks.initialChunks),
        std::move(zones));
    installCoordinatorDoc(opCtx.get(), updatedCoordinatorDoc);
}

ReshardingApproxCopySize computeApproxCopySize(ReshardingCoordinatorDocument& coordinatorDoc) {
    const auto numRecipients = coordinatorDoc.getRecipientShards().size();
    iassert(ErrorCodes::BadValue,
            "Expected to find at least one recipient in the coordinator document",
            numRecipients > 0);

    // Compute the aggregate for the number of documents and bytes to copy.
    long aggBytesToCopy = 0, aggDocumentsToCopy = 0;
    for (auto donor : coordinatorDoc.getDonorShards()) {
        if (const auto bytesToClone = donor.getMutableState().getBytesToClone()) {
            aggBytesToCopy += *bytesToClone;
        }

        if (const auto documentsToClone = donor.getMutableState().getDocumentsToClone()) {
            aggDocumentsToCopy += *documentsToClone;
        }
    }

    // Calculate the approximate number of documents and bytes that each recipient will clone.
    ReshardingApproxCopySize approxCopySize;
    approxCopySize.setApproxBytesToCopy(aggBytesToCopy / numRecipients);
    approxCopySize.setApproxDocumentsToCopy(aggDocumentsToCopy / numRecipients);
    return approxCopySize;
}

ExecutorFuture<void>
ReshardingCoordinatorService::ReshardingCoordinator::_awaitAllDonorsReadyToDonate(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kPreparingToDonate) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return future_util::withCancellation(
               _reshardingCoordinatorObserver->awaitAllDonorsReadyToDonate(),
               _ctHolder->getAbortToken())
        .thenRunOn(**executor)
        .then([this](ReshardingCoordinatorDocument coordinatorDocChangedOnDisk) {
            {
                auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                reshardingPauseCoordinatorBeforeCloning.pauseWhileSetAndNotCanceled(
                    opCtx.get(), _ctHolder->getAbortToken());
            }

            auto highestMinFetchTimestamp =
                getHighestMinFetchTimestamp(coordinatorDocChangedOnDisk.getDonorShards());
            _updateCoordinatorDocStateAndCatalogEntries(
                CoordinatorStateEnum::kCloning,
                coordinatorDocChangedOnDisk,
                highestMinFetchTimestamp,
                computeApproxCopySize(coordinatorDocChangedOnDisk));
        });
}

ExecutorFuture<void>
ReshardingCoordinatorService::ReshardingCoordinator::_awaitAllRecipientsFinishedCloning(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kCloning) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return future_util::withCancellation(
               _reshardingCoordinatorObserver->awaitAllRecipientsFinishedCloning(),
               _ctHolder->getAbortToken())
        .thenRunOn(**executor)
        .then([this](ReshardingCoordinatorDocument coordinatorDocChangedOnDisk) {
            this->_updateCoordinatorDocStateAndCatalogEntries(CoordinatorStateEnum::kApplying,
                                                              coordinatorDocChangedOnDisk);
        });
}

void ReshardingCoordinatorService::ReshardingCoordinator::_startCommitMonitor(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    _ctHolder->getAbortToken().onCancel().thenRunOn(**executor).getAsync([this](Status status) {
        if (status.isOK())
            _commitMonitorCancellationSource.cancel();
    });

    auto commitMonitor = std::make_shared<resharding::CoordinatorCommitMonitor>(
        _coordinatorDoc.getSourceNss(),
        extractShardIdsFromParticipantEntries(_coordinatorDoc.getRecipientShards()),
        **executor,
        _commitMonitorCancellationSource.token());

    commitMonitor->waitUntilRecipientsAreWithinCommitThreshold()
        .thenRunOn(**executor)
        .getAsync([this](Status) { onOkayToEnterCritical(); });
}

ExecutorFuture<void>
ReshardingCoordinatorService::ReshardingCoordinator::_awaitAllRecipientsFinishedApplying(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kApplying) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return future_util::withCancellation(
               _reshardingCoordinatorObserver->awaitAllRecipientsFinishedApplying(),
               _ctHolder->getAbortToken())
        .thenRunOn(**executor)
        .then([this, executor](ReshardingCoordinatorDocument coordinatorDocChangedOnDisk) {
            {
                auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                reshardingPauseCoordinatorInSteadyState.pauseWhileSetAndNotCanceled(
                    opCtx.get(), _ctHolder->getAbortToken());
            }

            _startCommitMonitor(executor);

            LOGV2(5391602, "Resharding operation waiting for an okay to enter critical section");
            return _canEnterCritical.getFuture()
                .thenRunOn(**executor)
                .then([this, doc = std::move(coordinatorDocChangedOnDisk)] {
                    _commitMonitorCancellationSource.cancel();
                    LOGV2(5391603, "Resharding operation is okay to enter critical section");
                    return doc;
                });
        })
        .then([this, executor](ReshardingCoordinatorDocument coordinatorDocChangedOnDisk) {
            this->_updateCoordinatorDocStateAndCatalogEntries(CoordinatorStateEnum::kBlockingWrites,
                                                              coordinatorDocChangedOnDisk);
            const auto criticalSectionTimeout =
                Milliseconds(resharding::gReshardingCriticalSectionTimeoutMillis.load());
            const auto criticalSectionExpiresAt = (*executor)->now() + criticalSectionTimeout;
            LOGV2_INFO(
                5573001, "Engaging critical section", "timeoutAt"_attr = criticalSectionExpiresAt);

            auto swCbHandle = (*executor)->scheduleWorkAt(
                criticalSectionExpiresAt,
                [this](const executor::TaskExecutor::CallbackArgs& cbData) {
                    if (!cbData.status.isOK()) {
                        return;
                    }
                    _reshardingCoordinatorObserver->onCriticalSectionTimeout();
                });

            if (!swCbHandle.isOK()) {
                _reshardingCoordinatorObserver->interrupt(swCbHandle.getStatus());
            }

            _criticalSectionTimeoutCbHandle = swCbHandle.getValue();
        });
}

ExecutorFuture<ReshardingCoordinatorDocument>
ReshardingCoordinatorService::ReshardingCoordinator::_awaitAllRecipientsInStrictConsistency(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kBlockingWrites) {
        // If in recovery, just return the existing _stateDoc.
        return ExecutorFuture<ReshardingCoordinatorDocument>(**executor, _coordinatorDoc);
    }

    return future_util::withCancellation(
               _reshardingCoordinatorObserver->awaitAllRecipientsInStrictConsistency(),
               _ctHolder->getAbortToken())
        .thenRunOn(**executor);
}

Future<void> ReshardingCoordinatorService::ReshardingCoordinator::_persistDecision(
    const ReshardingCoordinatorDocument& coordinatorDoc) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kBlockingWrites) {
        return Status::OK();
    }

    ReshardingCoordinatorDocument updatedCoordinatorDoc = coordinatorDoc;
    updatedCoordinatorDoc.setState(CoordinatorStateEnum::kCommitting);

    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    reshardingPauseCoordinatorBeforeDecisionPersisted.pauseWhileSetAndNotCanceled(
        opCtx.get(), _ctHolder->getAbortToken());

    // The new epoch and timestamp to use for the resharded collection to indicate that the
    // collection is a new incarnation of the namespace
    auto newCollectionEpoch = OID::gen();
    boost::optional<Timestamp> newCollectionTimestamp;
    if (feature_flags::gShardingFullDDLSupportTimestampedVersion.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        auto now = VectorClock::get(opCtx.get())->getTime();
        newCollectionTimestamp = now.clusterTime().asTimestamp();
    }

    resharding::writeDecisionPersistedState(
        opCtx.get(), updatedCoordinatorDoc, newCollectionEpoch, newCollectionTimestamp);

    // Update the in memory state
    installCoordinatorDoc(opCtx.get(), updatedCoordinatorDoc);

    return Status::OK();
}

ExecutorFuture<void>
ReshardingCoordinatorService::ReshardingCoordinator::_awaitAllParticipantShardsDone(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    std::vector<ExecutorFuture<ReshardingCoordinatorDocument>> futures;
    futures.emplace_back(
        _reshardingCoordinatorObserver->awaitAllRecipientsDone().thenRunOn(**executor));
    futures.emplace_back(
        _reshardingCoordinatorObserver->awaitAllDonorsDone().thenRunOn(**executor));

    // We only allow the stepdown token to cancel operations after progressing past
    // kCommitting.
    return future_util::withCancellation(whenAllSucceed(std::move(futures)),
                                         _ctHolder->getStepdownToken())
        .thenRunOn(**executor)
        .then([this, executor](const auto& coordinatorDocsChangedOnDisk) {
            auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
            auto& coordinatorDoc = coordinatorDocsChangedOnDisk[1];

            boost::optional<Status> abortReason;
            if (coordinatorDoc.getAbortReason()) {
                abortReason = getStatusFromAbortReason(coordinatorDoc);
            }

            _reshardingCoordinatorExternalState->removeCoordinatorDocAndReshardingFields(
                opCtx.get(), coordinatorDoc, abortReason);
        });
}

void ReshardingCoordinatorService::ReshardingCoordinator::
    _updateCoordinatorDocStateAndCatalogEntries(
        CoordinatorStateEnum nextState,
        ReshardingCoordinatorDocument coordinatorDoc,
        boost::optional<Timestamp> cloneTimestamp,
        boost::optional<ReshardingApproxCopySize> approxCopySize,
        boost::optional<Status> abortReason) {
    // Build new state doc for coordinator state update
    ReshardingCoordinatorDocument updatedCoordinatorDoc = coordinatorDoc;
    updatedCoordinatorDoc.setState(nextState);
    emplaceApproxBytesToCopyIfExists(updatedCoordinatorDoc, std::move(approxCopySize));
    emplaceCloneTimestampIfExists(updatedCoordinatorDoc, std::move(cloneTimestamp));
    emplaceAbortReasonIfExists(updatedCoordinatorDoc, abortReason);

    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    _reshardingCoordinatorExternalState->writeStateTransitionAndCatalogUpdatesThenBumpShardVersions(
        opCtx.get(), updatedCoordinatorDoc);

    // Update in-memory coordinator doc
    installCoordinatorDoc(opCtx.get(), updatedCoordinatorDoc);
}

void ReshardingCoordinatorService::ReshardingCoordinator::_tellAllRecipientsToRefresh(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    auto recipientIds = extractShardIdsFromParticipantEntries(_coordinatorDoc.getRecipientShards());

    NamespaceString nssToRefresh;
    // Refresh the temporary namespace if the coordinator is in a state prior to 'kCommitting'.
    // A refresh of recipients while in 'kCommitting' should be accompanied by a refresh of
    // all participants for the original namespace to ensure correctness.
    if (_coordinatorDoc.getState() < CoordinatorStateEnum::kCommitting) {
        nssToRefresh = _coordinatorDoc.getTempReshardingNss();
    } else {
        nssToRefresh = _coordinatorDoc.getSourceNss();
    }

    auto refreshCmd = createFlushReshardingStateChangeCommand(nssToRefresh);
    sharding_util::sendCommandToShards(opCtx.get(),
                                       NamespaceString::kAdminDb,
                                       refreshCmd,
                                       {recipientIds.begin(), recipientIds.end()},
                                       **executor);
}

void ReshardingCoordinatorService::ReshardingCoordinator::_tellAllDonorsToRefresh(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    auto donorIds = extractShardIdsFromParticipantEntries(_coordinatorDoc.getDonorShards());

    auto refreshCmd = createFlushReshardingStateChangeCommand(_coordinatorDoc.getSourceNss());
    sharding_util::sendCommandToShards(opCtx.get(),
                                       NamespaceString::kAdminDb,
                                       refreshCmd,
                                       {donorIds.begin(), donorIds.end()},
                                       **executor);
}

void ReshardingCoordinatorService::ReshardingCoordinator::_tellAllParticipantsToRefresh(
    const NamespaceString& nss, const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());

    auto donorShardIds = extractShardIdsFromParticipantEntries(_coordinatorDoc.getDonorShards());
    auto recipientShardIds =
        extractShardIdsFromParticipantEntries(_coordinatorDoc.getRecipientShards());
    std::set<ShardId> participantShardIds{donorShardIds.begin(), donorShardIds.end()};
    participantShardIds.insert(recipientShardIds.begin(), recipientShardIds.end());

    auto refreshCmd = createFlushReshardingStateChangeCommand(nss);
    sharding_util::sendCommandToShards(opCtx.get(),
                                       NamespaceString::kAdminDb,
                                       refreshCmd,
                                       {participantShardIds.begin(), participantShardIds.end()},
                                       **executor);
}

}  // namespace mongo
