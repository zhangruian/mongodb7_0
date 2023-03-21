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


#include "mongo/platform/basic.h"

#include "mongo/db/op_observer/op_observer_impl.h"

#include <algorithm>
#include <limits>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/import_collection_oplog_entry_gen.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/change_stream_pre_images_collection_manager.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/create_indexes_gen.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/write_stage_common.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/keys_collection_document_gen.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/batched_write_context.h"
#include "mongo/db/op_observer/op_observer_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/repl/image_collection_entry_gen.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/tenant_migration_decoration.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_write_router.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_helpers.h"
#include "mongo/db/timeseries/timeseries_extended_range.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/transaction/transaction_participant_gen.h"
#include "mongo/db/views/util.h"
#include "mongo/db/views/view_catalog_helpers.h"
#include "mongo/logv2/log.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/scripting/engine.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
using repl::DurableOplogEntry;
using repl::MutableOplogEntry;
using ChangeStreamPreImageRecordingMode = repl::ReplOperation::ChangeStreamPreImageRecordingMode;

const OperationContext::Decoration<boost::optional<ShardId>> destinedRecipientDecoration =
    OperationContext::declareDecoration<boost::optional<ShardId>>();

namespace {

MONGO_FAIL_POINT_DEFINE(failCollectionUpdates);
MONGO_FAIL_POINT_DEFINE(hangAndFailUnpreparedCommitAfterReservingOplogSlot);

constexpr auto kNumRecordsFieldName = "numRecords"_sd;
constexpr auto kMsgFieldName = "msg"_sd;
constexpr long long kInvalidNumRecords = -1LL;

Date_t getWallClockTimeForOpLog(OperationContext* opCtx) {
    auto const clockSource = opCtx->getServiceContext()->getFastClockSource();
    return clockSource->now();
}

repl::OpTime logOperation(OperationContext* opCtx,
                          MutableOplogEntry* oplogEntry,
                          bool assignWallClockTime,
                          OplogWriter* oplogWriter) {
    if (assignWallClockTime) {
        oplogEntry->setWallClockTime(getWallClockTimeForOpLog(opCtx));
    }
    auto& times = OpObserver::Times::get(opCtx).reservedOpTimes;
    auto opTime = oplogWriter->logOp(opCtx, oplogEntry);
    times.push_back(opTime);
    return opTime;
}

/**
 * Generic function that logs an operation.
 * Intended to reduce branching at call-sites by accepting the least common denominator
 * type: a MutableOplogEntry.
 *
 * 'fromMigrate' is generally hard-coded to false, but is supplied by a few
 * scenarios from mongos related behavior.
 *
 * If in a transaction, returns a null OpTime. Otherwise, returns the OpTime the operation
 * was logged with.
 */
repl::OpTime logMutableOplogEntry(OperationContext* opCtx,
                                  MutableOplogEntry* entry,
                                  OplogWriter* oplogWriter,
                                  bool isRequiredInMultiDocumentTransaction = false) {
    auto txnParticipant = TransactionParticipant::get(opCtx);
    const bool inMultiDocumentTransaction =
        txnParticipant && opCtx->writesAreReplicated() && txnParticipant.transactionIsOpen();

    if (isRequiredInMultiDocumentTransaction) {
        invariant(inMultiDocumentTransaction);
    }

    if (inMultiDocumentTransaction) {
        txnParticipant.addTransactionOperation(opCtx, entry->toReplOperation());
        return {};
    } else {
        return logOperation(opCtx, entry, /*assignWallClockTime=*/true, oplogWriter);
    }
}

/**
 * Updates the session state with the last write timestamp and transaction for that session.
 *
 * In the case of writes with transaction/statement id, this method will be recursively entered a
 * second time for the actual write to the transactions table. Since this write does not generate an
 * oplog entry, the recursion will stop at this point.
 */
void onWriteOpCompleted(OperationContext* opCtx,
                        std::vector<StmtId> stmtIdsWritten,
                        SessionTxnRecord sessionTxnRecord) {
    if (sessionTxnRecord.getLastWriteOpTime().isNull())
        return;

    auto txnParticipant = TransactionParticipant::get(opCtx);
    if (!txnParticipant ||
        (!stmtIdsWritten.empty() && stmtIdsWritten.front() == kUninitializedStmtId))
        // If the first statement written in uninitialized, then all the statements are assumed to
        // be uninitialized.
        return;

    // We add these here since they may not exist if we return early.
    const auto lsid = *opCtx->getLogicalSessionId();
    sessionTxnRecord.setSessionId(lsid);
    if (isInternalSessionForRetryableWrite(lsid)) {
        sessionTxnRecord.setParentSessionId(*getParentSessionId(lsid));
    }
    sessionTxnRecord.setTxnNum(*opCtx->getTxnNumber());
    txnParticipant.onWriteOpCompletedOnPrimary(opCtx, std::move(stmtIdsWritten), sessionTxnRecord);
}

/**
 * Given the collection count from Collection::numRecords(), create and return the object for the
 * 'o2' field of a drop or rename oplog entry. If the collection count exceeds the upper limit of a
 * BSON NumberLong (long long), we will add a count of -1 and append a message with the original
 * collection count.
 *
 * Replication rollback uses this field to correct correction counts on drop-pending collections.
 */
BSONObj makeObject2ForDropOrRename(uint64_t numRecords) {
    BSONObjBuilder obj2Builder;
    if (numRecords > static_cast<uint64_t>(std::numeric_limits<long long>::max())) {
        obj2Builder.appendNumber(kNumRecordsFieldName, kInvalidNumRecords);
        std::string msg = str::stream() << "Collection count " << numRecords
                                        << " is larger than the "
                                           "maximum int64_t value. Setting numRecords to -1.";
        obj2Builder.append(kMsgFieldName, msg);
    } else {
        obj2Builder.appendNumber(kNumRecordsFieldName, static_cast<long long>(numRecords));
    }
    auto obj = obj2Builder.obj();
    return obj;
}

struct OpTimeBundle {
    repl::OpTime writeOpTime;
    repl::OpTime prePostImageOpTime;
    Date_t wallClockTime;
};

/**
 * Write oplog entry(ies) for the update operation.
 */
OpTimeBundle replLogUpdate(OperationContext* opCtx,
                           const OplogUpdateEntryArgs& args,
                           MutableOplogEntry* oplogEntry,
                           OplogWriter* oplogWriter) {
    oplogEntry->setTid(args.coll->ns().tenantId());
    oplogEntry->setNss(args.coll->ns());
    oplogEntry->setUuid(args.coll->uuid());

    repl::OplogLink oplogLink;
    oplogWriter->appendOplogEntryChainInfo(opCtx, oplogEntry, &oplogLink, args.updateArgs->stmtIds);

    OpTimeBundle opTimes;
    oplogEntry->setOpType(repl::OpTypeEnum::kUpdate);
    oplogEntry->setObject(args.updateArgs->update);
    oplogEntry->setObject2(args.updateArgs->criteria);
    oplogEntry->setFromMigrateIfTrue(args.updateArgs->source == OperationSource::kFromMigrate);
    if (!args.updateArgs->oplogSlots.empty()) {
        oplogEntry->setOpTime(args.updateArgs->oplogSlots.back());
    }
    opTimes.writeOpTime =
        logOperation(opCtx, oplogEntry, true /*assignWallClockTime*/, oplogWriter);
    opTimes.wallClockTime = oplogEntry->getWallClockTime();
    return opTimes;
}

/**
 * Write oplog entry(ies) for the delete operation.
 */
OpTimeBundle replLogDelete(OperationContext* opCtx,
                           const NamespaceString& nss,
                           MutableOplogEntry* oplogEntry,
                           const boost::optional<UUID>& uuid,
                           StmtId stmtId,
                           bool fromMigrate,
                           OplogWriter* oplogWriter) {
    oplogEntry->setTid(nss.tenantId());
    oplogEntry->setNss(nss);
    oplogEntry->setUuid(uuid);
    oplogEntry->setDestinedRecipient(destinedRecipientDecoration(opCtx));

    repl::OplogLink oplogLink;
    oplogWriter->appendOplogEntryChainInfo(opCtx, oplogEntry, &oplogLink, {stmtId});

    OpTimeBundle opTimes;
    oplogEntry->setOpType(repl::OpTypeEnum::kDelete);
    oplogEntry->setObject(repl::documentKeyDecoration(opCtx).value().getShardKeyAndId());
    oplogEntry->setFromMigrateIfTrue(fromMigrate);
    opTimes.writeOpTime =
        logOperation(opCtx, oplogEntry, true /*assignWallClockTime*/, oplogWriter);
    opTimes.wallClockTime = oplogEntry->getWallClockTime();
    return opTimes;
}

void writeToImageCollection(OperationContext* opCtx,
                            const LogicalSessionId& sessionId,
                            const repl::ReplOperation::ImageBundle& imageToWrite) {
    repl::ImageEntry imageEntry;
    imageEntry.set_id(sessionId);
    imageEntry.setTxnNumber(opCtx->getTxnNumber().value());
    imageEntry.setTs(imageToWrite.timestamp);
    imageEntry.setImageKind(imageToWrite.imageKind);
    imageEntry.setImage(imageToWrite.imageDoc);

    DisableDocumentValidation documentValidationDisabler(
        opCtx, DocumentValidationSettings::kDisableInternalValidation);

    // In practice, this lock acquisition on kConfigImagesNamespace cannot block. The only time a
    // stronger lock acquisition is taken on this namespace is during step up to create the
    // collection.
    AllowLockAcquisitionOnTimestampedUnitOfWork allowLockAcquisition(opCtx->lockState());
    AutoGetCollection imageCollectionRaii(
        opCtx, NamespaceString::kConfigImagesNamespace, LockMode::MODE_IX);
    auto curOp = CurOp::get(opCtx);
    const auto existingNs = curOp->getNSS();
    UpdateResult res =
        Helpers::upsert(opCtx, NamespaceString::kConfigImagesNamespace, imageEntry.toBSON());
    {
        stdx::lock_guard<Client> clientLock(*opCtx->getClient());
        curOp->setNS_inlock(existingNs);
    }

    invariant(res.numDocsModified == 1 || !res.upsertedId.isEmpty());
}

bool shouldTimestampIndexBuildSinglePhase(OperationContext* opCtx, const NamespaceString& nss) {
    // This function returns whether a timestamp for a catalog write when beginning an index build,
    // or aborting an index build is necessary. There are four scenarios:

    // 1. A timestamp is already set -- replication application sets a timestamp ahead of time.
    // This could include the phase of initial sync where it applies oplog entries.  Also,
    // primaries performing an index build via `applyOps` may have a wrapping commit timestamp.
    if (!opCtx->recoveryUnit()->getCommitTimestamp().isNull())
        return false;

    // 2. If the node is initial syncing, we do not set a timestamp.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord->isReplEnabled() && replCoord->getMemberState().startup2())
        return false;

    // 3. If the index build is on the local database, do not timestamp.
    if (nss.isLocal())
        return false;

    // 4. All other cases, we generate a timestamp by writing a no-op oplog entry.  This is
    // better than using a ghost timestamp.  Writing an oplog entry ensures this node is
    // primary.
    return true;
}

void logGlobalIndexDDLOperation(OperationContext* opCtx,
                                const NamespaceString& globalIndexNss,
                                const UUID& globalIndexUUID,
                                const StringData commandString,
                                boost::optional<long long> numKeys,
                                OplogWriter* oplogWriter) {
    invariant(!opCtx->inMultiDocumentTransaction());

    BSONObjBuilder builder;
    // The rollback implementation requires the collection name to list affected namespaces.
    builder.append(commandString, globalIndexNss.coll());

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry.setObject(builder.done());

    // On global index drops, persist the number of records into the 'o2' field similar to a
    // collection drop. This allows for efficiently restoring the index keys count after rollback
    // without forcing a collection scan.
    invariant((numKeys && commandString == "dropGlobalIndex") ||
              (!numKeys && commandString == "createGlobalIndex"));
    if (numKeys) {
        oplogEntry.setObject2(makeObject2ForDropOrRename(*numKeys));
    }

    // The 'ns' field is technically redundant as it can be derived from the uuid, however it's a
    // required oplog entry field.
    oplogEntry.setNss(globalIndexNss.getCommandNS());
    oplogEntry.setUuid(globalIndexUUID);

    constexpr StmtId stmtId = 0;
    if (TransactionParticipant::get(opCtx)) {
        // This is a retryable write: populate the lsid, txnNumber and stmtId fields.
        // The oplog link to previous statement is empty and the stmtId is zero because this is a
        // single-statement command replicating as a single createGlobalIndex/dropGlobalIndex oplog
        // entry.
        repl::OplogLink oplogLink;
        oplogWriter->appendOplogEntryChainInfo(opCtx, &oplogEntry, &oplogLink, {stmtId});
    }

    auto writeOpTime = logOperation(opCtx, &oplogEntry, true /*assignWallClockTime*/, oplogWriter);

    // Register the retryable write to in-memory transactions table.
    SessionTxnRecord sessionTxnRecord;
    sessionTxnRecord.setLastWriteOpTime(writeOpTime);
    sessionTxnRecord.setLastWriteDate(oplogEntry.getWallClockTime());
    onWriteOpCompleted(opCtx, {stmtId}, sessionTxnRecord);
}

}  // namespace

OpObserverImpl::OpObserverImpl(std::unique_ptr<OplogWriter> oplogWriter)
    : _oplogWriter(std::move(oplogWriter)) {}

void OpObserverImpl::onCreateGlobalIndex(OperationContext* opCtx,
                                         const NamespaceString& globalIndexNss,
                                         const UUID& globalIndexUUID) {
    constexpr StringData commandString = "createGlobalIndex"_sd;
    logGlobalIndexDDLOperation(opCtx,
                               globalIndexNss,
                               globalIndexUUID,
                               commandString,
                               boost::none /* numKeys */,
                               _oplogWriter.get());
}

void OpObserverImpl::onDropGlobalIndex(OperationContext* opCtx,
                                       const NamespaceString& globalIndexNss,
                                       const UUID& globalIndexUUID,
                                       long long numKeys) {
    constexpr StringData commandString = "dropGlobalIndex"_sd;
    logGlobalIndexDDLOperation(
        opCtx, globalIndexNss, globalIndexUUID, commandString, numKeys, _oplogWriter.get());
}

void OpObserverImpl::onCreateIndex(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   const UUID& uuid,
                                   BSONObj indexDoc,
                                   bool fromMigrate) {
    BSONObjBuilder builder;
    builder.append(CreateIndexesCommand::kCommandName, nss.coll());
    builder.appendElements(indexDoc);

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry.setTid(nss.tenantId());
    oplogEntry.setNss(nss.getCommandNS());
    oplogEntry.setUuid(uuid);
    oplogEntry.setObject(builder.obj());
    oplogEntry.setFromMigrateIfTrue(fromMigrate);

    auto opTime = logMutableOplogEntry(opCtx, &oplogEntry, _oplogWriter.get());

    if (opCtx->writesAreReplicated()) {
        if (opTime.isNull()) {
            LOGV2(7360100,
                  "Added oplog entry for createIndexes to transaction",
                  "namespace"_attr = oplogEntry.getNss(),
                  "uuid"_attr = oplogEntry.getUuid(),
                  "object"_attr = oplogEntry.getObject());
        } else {
            LOGV2(7360101,
                  "Wrote oplog entry for createIndexes",
                  "namespace"_attr = oplogEntry.getNss(),
                  "uuid"_attr = oplogEntry.getUuid(),
                  "opTime"_attr = opTime,
                  "object"_attr = oplogEntry.getObject());
        }
    }
}

void OpObserverImpl::onStartIndexBuild(OperationContext* opCtx,
                                       const NamespaceString& nss,
                                       const UUID& collUUID,
                                       const UUID& indexBuildUUID,
                                       const std::vector<BSONObj>& indexes,
                                       bool fromMigrate) {
    BSONObjBuilder oplogEntryBuilder;
    oplogEntryBuilder.append("startIndexBuild", nss.coll());

    indexBuildUUID.appendToBuilder(&oplogEntryBuilder, "indexBuildUUID");

    BSONArrayBuilder indexesArr(oplogEntryBuilder.subarrayStart("indexes"));
    for (const auto& indexDoc : indexes) {
        indexesArr.append(indexDoc);
    }
    indexesArr.done();

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);

    oplogEntry.setTid(nss.tenantId());
    oplogEntry.setNss(nss.getCommandNS());
    oplogEntry.setUuid(collUUID);
    oplogEntry.setObject(oplogEntryBuilder.done());
    oplogEntry.setFromMigrateIfTrue(fromMigrate);
    logOperation(opCtx, &oplogEntry, true /*assignWallClockTime*/, _oplogWriter.get());
}

void OpObserverImpl::onStartIndexBuildSinglePhase(OperationContext* opCtx,
                                                  const NamespaceString& nss) {
    if (!shouldTimestampIndexBuildSinglePhase(opCtx, nss)) {
        return;
    }


    onInternalOpMessage(
        opCtx,
        {},
        boost::none,
        BSON("msg" << std::string(str::stream() << "Creating indexes. Coll: " << nss)),
        boost::none,
        boost::none,
        boost::none,
        boost::none,
        boost::none);
}

void OpObserverImpl::onAbortIndexBuildSinglePhase(OperationContext* opCtx,
                                                  const NamespaceString& nss) {
    if (!shouldTimestampIndexBuildSinglePhase(opCtx, nss)) {
        return;
    }

    onInternalOpMessage(
        opCtx,
        {},
        boost::none,
        BSON("msg" << std::string(str::stream() << "Aborting indexes. Coll: " << nss)),
        boost::none,
        boost::none,
        boost::none,
        boost::none,
        boost::none);
}

void OpObserverImpl::onCommitIndexBuild(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        const UUID& collUUID,
                                        const UUID& indexBuildUUID,
                                        const std::vector<BSONObj>& indexes,
                                        bool fromMigrate) {
    BSONObjBuilder oplogEntryBuilder;
    oplogEntryBuilder.append("commitIndexBuild", nss.coll());

    indexBuildUUID.appendToBuilder(&oplogEntryBuilder, "indexBuildUUID");

    BSONArrayBuilder indexesArr(oplogEntryBuilder.subarrayStart("indexes"));
    for (const auto& indexDoc : indexes) {
        indexesArr.append(indexDoc);
    }
    indexesArr.done();

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);

    oplogEntry.setTid(nss.tenantId());
    oplogEntry.setNss(nss.getCommandNS());
    oplogEntry.setUuid(collUUID);
    oplogEntry.setObject(oplogEntryBuilder.done());
    oplogEntry.setFromMigrateIfTrue(fromMigrate);
    logOperation(opCtx, &oplogEntry, true /*assignWallClockTime*/, _oplogWriter.get());
}

void OpObserverImpl::onAbortIndexBuild(OperationContext* opCtx,
                                       const NamespaceString& nss,
                                       const UUID& collUUID,
                                       const UUID& indexBuildUUID,
                                       const std::vector<BSONObj>& indexes,
                                       const Status& cause,
                                       bool fromMigrate) {
    BSONObjBuilder oplogEntryBuilder;
    oplogEntryBuilder.append("abortIndexBuild", nss.coll());

    indexBuildUUID.appendToBuilder(&oplogEntryBuilder, "indexBuildUUID");

    BSONArrayBuilder indexesArr(oplogEntryBuilder.subarrayStart("indexes"));
    for (const auto& indexDoc : indexes) {
        indexesArr.append(indexDoc);
    }
    indexesArr.done();

    BSONObjBuilder causeBuilder(oplogEntryBuilder.subobjStart("cause"));
    // Some functions that extract a Status from a BSONObj, such as getStatusFromCommandResult(),
    // expect the 'ok' field.
    causeBuilder.appendBool("ok", 0);
    cause.serializeErrorToBSON(&causeBuilder);
    causeBuilder.done();

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);

    oplogEntry.setTid(nss.tenantId());
    oplogEntry.setNss(nss.getCommandNS());
    oplogEntry.setUuid(collUUID);
    oplogEntry.setObject(oplogEntryBuilder.done());
    oplogEntry.setFromMigrateIfTrue(fromMigrate);
    logOperation(opCtx, &oplogEntry, true /*assignWallClockTime*/, _oplogWriter.get());
}

void OpObserverImpl::onInserts(OperationContext* opCtx,
                               const CollectionPtr& coll,
                               std::vector<InsertStatement>::const_iterator first,
                               std::vector<InsertStatement>::const_iterator last,
                               bool fromMigrate) {
    auto txnParticipant = TransactionParticipant::get(opCtx);
    const bool inMultiDocumentTransaction =
        txnParticipant && opCtx->writesAreReplicated() && txnParticipant.transactionIsOpen();

    const auto& nss = coll->ns();
    const auto& uuid = coll->uuid();

    std::vector<repl::OpTime> opTimeList;
    repl::OpTime lastOpTime;

    ShardingWriteRouter shardingWriteRouter(opCtx, nss, Grid::get(opCtx)->catalogCache());

    auto& batchedWriteContext = BatchedWriteContext::get(opCtx);
    const bool inBatchedWrite = batchedWriteContext.writesAreBatched();

    if (inBatchedWrite) {
        invariant(!fromMigrate);

        write_stage_common::PreWriteFilter preWriteFilter(opCtx, nss);

        for (auto iter = first; iter != last; iter++) {
            const auto docKey = repl::getDocumentKey(opCtx, coll, iter->doc).getShardKeyAndId();
            auto operation = MutableOplogEntry::makeInsertOperation(nss, uuid, iter->doc, docKey);
            operation.setDestinedRecipient(
                shardingWriteRouter.getReshardingDestinedRecipient(iter->doc));

            if (!OperationShardingState::isComingFromRouter(opCtx) &&
                preWriteFilter.computeAction(Document(iter->doc)) ==
                    write_stage_common::PreWriteFilter::Action::kWriteAsFromMigrate) {
                LOGV2_DEBUG(6585800,
                            3,
                            "Marking insert operation of orphan document with the 'fromMigrate' "
                            "flag to prevent a wrong change stream event",
                            "namespace"_attr = nss,
                            "document"_attr = iter->doc);

                operation.setFromMigrate(true);
            }

            batchedWriteContext.addBatchedOperation(opCtx, operation);
        }
    } else if (inMultiDocumentTransaction) {
        invariant(!fromMigrate);

        // Do not add writes to the profile collection to the list of transaction operations, since
        // these are done outside the transaction. There is no top-level WriteUnitOfWork when we are
        // in a SideTransactionBlock.
        if (!opCtx->getWriteUnitOfWork()) {
            invariant(nss.isSystemDotProfile());
            return;
        }

        const bool inRetryableInternalTransaction =
            isInternalSessionForRetryableWrite(*opCtx->getLogicalSessionId());
        write_stage_common::PreWriteFilter preWriteFilter(opCtx, nss);

        for (auto iter = first; iter != last; iter++) {
            const auto docKey = repl::getDocumentKey(opCtx, coll, iter->doc).getShardKeyAndId();
            auto operation = MutableOplogEntry::makeInsertOperation(nss, uuid, iter->doc, docKey);
            if (inRetryableInternalTransaction) {
                operation.setInitializedStatementIds(iter->stmtIds);
            }
            operation.setDestinedRecipient(
                shardingWriteRouter.getReshardingDestinedRecipient(iter->doc));

            if (!OperationShardingState::isComingFromRouter(opCtx) &&
                preWriteFilter.computeAction(Document(iter->doc)) ==
                    write_stage_common::PreWriteFilter::Action::kWriteAsFromMigrate) {
                LOGV2_DEBUG(6585801,
                            3,
                            "Marking insert operation of orphan document with the 'fromMigrate' "
                            "flag to prevent a wrong change stream event",
                            "namespace"_attr = nss,
                            "document"_attr = iter->doc);

                operation.setFromMigrate(true);
            }

            txnParticipant.addTransactionOperation(opCtx, operation);
        }
    } else {
        std::function<boost::optional<ShardId>(const BSONObj& doc)> getDestinedRecipientFn =
            [&shardingWriteRouter](const BSONObj& doc) {
                return shardingWriteRouter.getReshardingDestinedRecipient(doc);
            };

        MutableOplogEntry oplogEntryTemplate;
        oplogEntryTemplate.setTid(nss.tenantId());
        oplogEntryTemplate.setNss(nss);
        oplogEntryTemplate.setUuid(uuid);
        oplogEntryTemplate.setFromMigrateIfTrue(fromMigrate);
        Date_t lastWriteDate = getWallClockTimeForOpLog(opCtx);
        oplogEntryTemplate.setWallClockTime(lastWriteDate);

        opTimeList = _oplogWriter->logInsertOps(
            opCtx, &oplogEntryTemplate, first, last, getDestinedRecipientFn, coll);
        if (!opTimeList.empty())
            lastOpTime = opTimeList.back();

        auto& times = OpObserver::Times::get(opCtx).reservedOpTimes;
        using std::begin;
        using std::end;
        times.insert(end(times), begin(opTimeList), end(opTimeList));

        std::vector<StmtId> stmtIdsWritten;
        std::for_each(first, last, [&](const InsertStatement& stmt) {
            stmtIdsWritten.insert(stmtIdsWritten.end(), stmt.stmtIds.begin(), stmt.stmtIds.end());
        });

        SessionTxnRecord sessionTxnRecord;
        sessionTxnRecord.setLastWriteOpTime(lastOpTime);
        sessionTxnRecord.setLastWriteDate(lastWriteDate);
        onWriteOpCompleted(opCtx, stmtIdsWritten, sessionTxnRecord);
    }

    shardObserveInsertsOp(opCtx,
                          nss,
                          first,
                          last,
                          opTimeList,
                          shardingWriteRouter,
                          fromMigrate,
                          inMultiDocumentTransaction);

    if (nss.coll() == "system.js") {
        Scope::storedFuncMod(opCtx);
    } else if (nss.isSystemDotViews()) {
        try {
            for (auto it = first; it != last; it++) {
                view_util::validateViewDefinitionBSON(opCtx, it->doc, nss.dbName());

                uassertStatusOK(CollectionCatalog::get(opCtx)->createView(
                    opCtx,
                    NamespaceStringUtil::deserialize(nss.dbName().tenantId(),
                                                     it->doc.getStringField("_id")),
                    {nss.dbName(), it->doc.getStringField("viewOn")},
                    BSONArray{it->doc.getObjectField("pipeline")},
                    view_catalog_helpers::validatePipeline,
                    it->doc.getObjectField("collation"),
                    ViewsForDatabase::Durability::kAlreadyDurable));
            }
        } catch (const DBException&) {
            // If a previous operation left the view catalog in an invalid state, our inserts can
            // fail even if all the definitions are valid. Reloading may help us reset the state.
            CollectionCatalog::get(opCtx)->reloadViews(opCtx, nss.dbName());
        }
    } else if (nss == NamespaceString::kSessionTransactionsTableNamespace && !lastOpTime.isNull()) {
        for (auto it = first; it != last; it++) {
            auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
            mongoDSessionCatalog->observeDirectWriteToConfigTransactions(opCtx, it->doc);
        }
    } else if (nss == NamespaceString::kConfigSettingsNamespace) {
        for (auto it = first; it != last; it++) {
            ReadWriteConcernDefaults::get(opCtx).observeDirectWriteToConfigSettings(
                opCtx, it->doc["_id"], it->doc);
        }
    } else if (nss == NamespaceString::kExternalKeysCollectionNamespace) {
        for (auto it = first; it != last; it++) {
            auto externalKey =
                ExternalKeysCollectionDocument::parse(IDLParserContext("externalKey"), it->doc);
            opCtx->recoveryUnit()->onCommit(
                [this, externalKey = std::move(externalKey)](OperationContext* opCtx,
                                                             boost::optional<Timestamp>) mutable {
                    auto validator = LogicalTimeValidator::get(opCtx);
                    if (validator) {
                        validator->cacheExternalKey(externalKey);
                    }
                });
        }
    } else if (nss.isTimeseriesBucketsCollection()) {
        // Check if the bucket _id is sourced from a date outside the standard range. If our writes
        // end up erroring out or getting rolled back, then this flag will stay set. This is okay
        // though, as it only disables some query optimizations and won't result in any correctness
        // issues if the flag is set when it doesn't need to be (as opposed to NOT being set when it
        // DOES need to be -- that will cause correctness issues). Additionally, if the user tried
        // to insert measurements with dates outside the standard range, chances are they will do so
        // again, and we will have only set the flag a little early.
        invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_IX));
        // Hold reference to the catalog for collection lookup without locks to be safe.
        auto catalog = CollectionCatalog::get(opCtx);
        auto bucketsColl = catalog->lookupCollectionByNamespace(opCtx, nss);
        tassert(6905201, "Could not find collection for write", bucketsColl);
        auto timeSeriesOptions = bucketsColl->getTimeseriesOptions();
        if (timeSeriesOptions.has_value()) {
            if (auto currentSetting = bucketsColl->getRequiresTimeseriesExtendedRangeSupport();
                !currentSetting &&
                timeseries::bucketsHaveDateOutsideStandardRange(
                    timeSeriesOptions.value(), first, last)) {
                bucketsColl->setRequiresTimeseriesExtendedRangeSupport(opCtx);
            }
        }
    }
}

void OpObserverImpl::onInsertGlobalIndexKey(OperationContext* opCtx,
                                            const NamespaceString& globalIndexNss,
                                            const UUID& globalIndexUuid,
                                            const BSONObj& key,
                                            const BSONObj& docKey) {
    if (!opCtx->writesAreReplicated()) {
        return;
    }

    invariant(!opCtx->isRetryableWrite());

    // _shardsvrInsertGlobalIndexKey must run inside a multi-doc transaction.
    bool isRequiredInMultiDocumentTransaction = true;

    MutableOplogEntry oplogEntry = MutableOplogEntry::makeGlobalIndexCrudOperation(
        repl::OpTypeEnum::kInsertGlobalIndexKey, globalIndexNss, globalIndexUuid, key, docKey);
    logMutableOplogEntry(
        opCtx, &oplogEntry, _oplogWriter.get(), isRequiredInMultiDocumentTransaction);
}

void OpObserverImpl::onDeleteGlobalIndexKey(OperationContext* opCtx,
                                            const NamespaceString& globalIndexNss,
                                            const UUID& globalIndexUuid,
                                            const BSONObj& key,
                                            const BSONObj& docKey) {
    if (!opCtx->writesAreReplicated()) {
        return;
    }

    invariant(!opCtx->isRetryableWrite());

    // _shardsvrDeleteGlobalIndexKey must run inside a multi-doc transaction.
    bool isRequiredInMultiDocumentTransaction = true;

    MutableOplogEntry oplogEntry = MutableOplogEntry::makeGlobalIndexCrudOperation(
        repl::OpTypeEnum::kDeleteGlobalIndexKey, globalIndexNss, globalIndexUuid, key, docKey);
    logMutableOplogEntry(
        opCtx, &oplogEntry, _oplogWriter.get(), isRequiredInMultiDocumentTransaction);
}

void OpObserverImpl::onUpdate(OperationContext* opCtx, const OplogUpdateEntryArgs& args) {
    failCollectionUpdates.executeIf(
        [&](const BSONObj&) {
            uasserted(40654,
                      str::stream()
                          << "failCollectionUpdates failpoint enabled, namespace: "
                          << args.coll->ns().ns() << ", update: " << args.updateArgs->update
                          << " on document with " << args.updateArgs->criteria);
        },
        [&](const BSONObj& data) {
            // If the failpoint specifies no collection or matches the existing one, fail.
            auto collElem = data["collectionNS"];
            return !collElem || args.coll->ns().ns() == collElem.String();
        });

    // Do not log a no-op operation; see SERVER-21738
    if (args.updateArgs->update.isEmpty()) {
        return;
    }

    auto txnParticipant = TransactionParticipant::get(opCtx);
    const bool inMultiDocumentTransaction =
        txnParticipant && opCtx->writesAreReplicated() && txnParticipant.transactionIsOpen();

    ShardingWriteRouter shardingWriteRouter(
        opCtx, args.coll->ns(), Grid::get(opCtx)->catalogCache());

    OpTimeBundle opTime;
    auto& batchedWriteContext = BatchedWriteContext::get(opCtx);
    const bool inBatchedWrite = batchedWriteContext.writesAreBatched();

    if (inBatchedWrite) {
        auto operation = MutableOplogEntry::makeUpdateOperation(
            args.coll->ns(), args.coll->uuid(), args.updateArgs->update, args.updateArgs->criteria);
        operation.setDestinedRecipient(
            shardingWriteRouter.getReshardingDestinedRecipient(args.updateArgs->updatedDoc));
        operation.setFromMigrateIfTrue(args.updateArgs->source == OperationSource::kFromMigrate);
        batchedWriteContext.addBatchedOperation(opCtx, operation);
    } else if (inMultiDocumentTransaction) {
        const bool inRetryableInternalTransaction =
            isInternalSessionForRetryableWrite(*opCtx->getLogicalSessionId());

        auto operation = MutableOplogEntry::makeUpdateOperation(
            args.coll->ns(), args.coll->uuid(), args.updateArgs->update, args.updateArgs->criteria);

        if (inRetryableInternalTransaction) {
            operation.setInitializedStatementIds(args.updateArgs->stmtIds);
            if (args.updateArgs->storeDocOption == CollectionUpdateArgs::StoreDocOption::PreImage) {
                invariant(!args.updateArgs->preImageDoc.isEmpty());
                operation.setPreImage(args.updateArgs->preImageDoc.getOwned());
                operation.setPreImageRecordedForRetryableInternalTransaction();
                if (args.retryableFindAndModifyLocation ==
                    RetryableFindAndModifyLocation::kSideCollection) {
                    operation.setNeedsRetryImage(repl::RetryImageEnum::kPreImage);
                }
            }
            if (args.updateArgs->storeDocOption ==
                CollectionUpdateArgs::StoreDocOption::PostImage) {
                invariant(!args.updateArgs->updatedDoc.isEmpty());
                operation.setPostImage(args.updateArgs->updatedDoc.getOwned());
                if (args.retryableFindAndModifyLocation ==
                    RetryableFindAndModifyLocation::kSideCollection) {
                    operation.setNeedsRetryImage(repl::RetryImageEnum::kPostImage);
                }
            }
        }

        if (args.updateArgs->changeStreamPreAndPostImagesEnabledForCollection) {
            invariant(!args.updateArgs->preImageDoc.isEmpty());
            operation.setPreImage(args.updateArgs->preImageDoc.getOwned());
            operation.setChangeStreamPreImageRecordingMode(
                ChangeStreamPreImageRecordingMode::kPreImagesCollection);
        }

        const auto& scopedCollectionDescription = shardingWriteRouter.getCollDesc();
        // ShardingWriteRouter only has boost::none scopedCollectionDescription when not in a
        // sharded cluster.
        if (scopedCollectionDescription && scopedCollectionDescription->isSharded()) {
            operation.setPostImageDocumentKey(
                scopedCollectionDescription->extractDocumentKey(args.updateArgs->updatedDoc)
                    .getOwned());
        }

        operation.setDestinedRecipient(
            shardingWriteRouter.getReshardingDestinedRecipient(args.updateArgs->updatedDoc));
        operation.setFromMigrateIfTrue(args.updateArgs->source == OperationSource::kFromMigrate);
        txnParticipant.addTransactionOperation(opCtx, operation);
    } else {
        MutableOplogEntry oplogEntry;
        oplogEntry.setDestinedRecipient(
            shardingWriteRouter.getReshardingDestinedRecipient(args.updateArgs->updatedDoc));

        if (args.retryableFindAndModifyLocation ==
            RetryableFindAndModifyLocation::kSideCollection) {
            // If we've stored a preImage:
            if (args.updateArgs->storeDocOption == CollectionUpdateArgs::StoreDocOption::PreImage) {
                oplogEntry.setNeedsRetryImage({repl::RetryImageEnum::kPreImage});
            } else if (args.updateArgs->storeDocOption ==
                       CollectionUpdateArgs::StoreDocOption::PostImage) {
                // Or if we're storing a postImage.
                oplogEntry.setNeedsRetryImage({repl::RetryImageEnum::kPostImage});
            }
        }

        opTime = replLogUpdate(opCtx, args, &oplogEntry, _oplogWriter.get());

        if (oplogEntry.getNeedsRetryImage()) {
            // If the oplog entry has `needsRetryImage`, copy the image into image collection.
            const BSONObj& dataImage = [&]() {
                if (oplogEntry.getNeedsRetryImage().value() == repl::RetryImageEnum::kPreImage) {
                    return args.updateArgs->preImageDoc;
                } else {
                    return args.updateArgs->updatedDoc;
                }
            }();
            auto imageToWrite =
                repl::ReplOperation::ImageBundle{oplogEntry.getNeedsRetryImage().value(),
                                                 dataImage,
                                                 opTime.writeOpTime.getTimestamp()};
            writeToImageCollection(opCtx, *opCtx->getLogicalSessionId(), imageToWrite);
        }

        // Write a pre-image to the change streams pre-images collection when following conditions
        // are met:
        // 1. The collection has 'changeStreamPreAndPostImages' enabled.
        // 2. The node wrote the oplog entry for the corresponding operation.
        // 3. The request to write the pre-image does not come from chunk-migrate event, i.e. source
        //    of the request is not 'fromMigrate'. The 'fromMigrate' events are filtered out by
        //    change streams and storing them in pre-image collection is redundant.
        // 4. a request to update is not on a temporary resharding collection. This update request
        //    does not result in change streams events. Recording pre-images from temporary
        //    resharing collection could result in incorrect pre-image getting recorded due to the
        //    temporary resharding collection not being consistent until writes are blocked (initial
        //    sync mode application).
        if (args.updateArgs->changeStreamPreAndPostImagesEnabledForCollection &&
            !opTime.writeOpTime.isNull() &&
            args.updateArgs->source != OperationSource::kFromMigrate &&
            !args.coll->ns().isTemporaryReshardingCollection()) {
            const auto& preImageDoc = args.updateArgs->preImageDoc;
            tassert(5868600, "PreImage must be set", !preImageDoc.isEmpty());

            ChangeStreamPreImageId id(args.coll->uuid(), opTime.writeOpTime.getTimestamp(), 0);
            ChangeStreamPreImage preImage(id, opTime.wallClockTime, preImageDoc);

            ChangeStreamPreImagesCollectionManager::insertPreImage(
                opCtx, args.coll->ns().tenantId(), preImage);
        }

        SessionTxnRecord sessionTxnRecord;
        sessionTxnRecord.setLastWriteOpTime(opTime.writeOpTime);
        sessionTxnRecord.setLastWriteDate(opTime.wallClockTime);
        onWriteOpCompleted(opCtx, args.updateArgs->stmtIds, sessionTxnRecord);
    }

    if (args.coll->ns() != NamespaceString::kSessionTransactionsTableNamespace) {
        if (args.updateArgs->source != OperationSource::kFromMigrate) {
            shardObserveUpdateOp(opCtx,
                                 args.coll->ns(),
                                 args.updateArgs->preImageDoc,
                                 args.updateArgs->updatedDoc,
                                 opTime.writeOpTime,
                                 shardingWriteRouter,
                                 opTime.prePostImageOpTime,
                                 inMultiDocumentTransaction);
        }
    }

    if (args.coll->ns().coll() == "system.js") {
        Scope::storedFuncMod(opCtx);
    } else if (args.coll->ns().isSystemDotViews()) {
        CollectionCatalog::get(opCtx)->reloadViews(opCtx, args.coll->ns().dbName());
    } else if (args.coll->ns() == NamespaceString::kSessionTransactionsTableNamespace &&
               !opTime.writeOpTime.isNull()) {
        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
        mongoDSessionCatalog->observeDirectWriteToConfigTransactions(opCtx,
                                                                     args.updateArgs->updatedDoc);
    } else if (args.coll->ns() == NamespaceString::kConfigSettingsNamespace) {
        ReadWriteConcernDefaults::get(opCtx).observeDirectWriteToConfigSettings(
            opCtx, args.updateArgs->updatedDoc["_id"], args.updateArgs->updatedDoc);
    } else if (args.coll->ns().isTimeseriesBucketsCollection()) {
        if (args.updateArgs->source != OperationSource::kTimeseriesInsert) {
            OID bucketId = args.updateArgs->updatedDoc["_id"].OID();
            timeseries::bucket_catalog::handleDirectWrite(opCtx, args.coll->ns(), bucketId);
        }
    }
}

void OpObserverImpl::aboutToDelete(OperationContext* opCtx,
                                   const CollectionPtr& coll,
                                   BSONObj const& doc) {
    repl::documentKeyDecoration(opCtx).emplace(repl::getDocumentKey(opCtx, coll, doc));

    ShardingWriteRouter shardingWriteRouter(opCtx, coll->ns(), Grid::get(opCtx)->catalogCache());

    repl::DurableReplOperation op;
    op.setDestinedRecipient(shardingWriteRouter.getReshardingDestinedRecipient(doc));
    destinedRecipientDecoration(opCtx) = op.getDestinedRecipient();

    shardObserveAboutToDelete(opCtx, coll->ns(), doc);

    if (coll->ns().isTimeseriesBucketsCollection()) {
        OID bucketId = doc["_id"].OID();
        timeseries::bucket_catalog::handleDirectWrite(opCtx, coll->ns(), bucketId);
    }
}

void OpObserverImpl::onDelete(OperationContext* opCtx,
                              const CollectionPtr& coll,
                              StmtId stmtId,
                              const OplogDeleteEntryArgs& args) {
    const auto& nss = coll->ns();
    const auto uuid = coll->uuid();
    auto optDocKey = repl::documentKeyDecoration(opCtx);
    invariant(optDocKey, nss.ns());
    auto& documentKey = optDocKey.value();

    auto txnParticipant = TransactionParticipant::get(opCtx);
    const bool inMultiDocumentTransaction =
        txnParticipant && opCtx->writesAreReplicated() && txnParticipant.transactionIsOpen();

    auto& batchedWriteContext = BatchedWriteContext::get(opCtx);
    const bool inBatchedWrite = batchedWriteContext.writesAreBatched();

    OpTimeBundle opTime;
    if (inBatchedWrite) {
        if (nss == NamespaceString::kSessionTransactionsTableNamespace) {
            auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
            mongoDSessionCatalog->observeDirectWriteToConfigTransactions(opCtx,
                                                                         documentKey.getId());
        }
        auto operation =
            MutableOplogEntry::makeDeleteOperation(nss, uuid, documentKey.getShardKeyAndId());
        operation.setDestinedRecipient(destinedRecipientDecoration(opCtx));
        operation.setFromMigrateIfTrue(args.fromMigrate);
        batchedWriteContext.addBatchedOperation(opCtx, operation);
    } else if (inMultiDocumentTransaction) {
        const bool inRetryableInternalTransaction =
            isInternalSessionForRetryableWrite(*opCtx->getLogicalSessionId());

        tassert(5868700,
                "Attempted a retryable write within a non-retryable multi-document transaction",
                inRetryableInternalTransaction ||
                    args.retryableFindAndModifyLocation == RetryableFindAndModifyLocation::kNone);

        auto operation =
            MutableOplogEntry::makeDeleteOperation(nss, uuid, documentKey.getShardKeyAndId());

        if (inRetryableInternalTransaction) {
            operation.setInitializedStatementIds({stmtId});
            if (args.retryableFindAndModifyLocation ==
                RetryableFindAndModifyLocation::kSideCollection) {
                tassert(6054000,
                        "Deleted document must be present for pre-image recording",
                        args.deletedDoc);
                operation.setPreImage(args.deletedDoc->getOwned());
                operation.setPreImageRecordedForRetryableInternalTransaction();
                operation.setNeedsRetryImage(repl::RetryImageEnum::kPreImage);
            }
        }

        if (args.changeStreamPreAndPostImagesEnabledForCollection) {
            tassert(5869400,
                    "Deleted document must be present for pre-image recording",
                    args.deletedDoc);
            operation.setPreImage(args.deletedDoc->getOwned());
            operation.setChangeStreamPreImageRecordingMode(
                ChangeStreamPreImageRecordingMode::kPreImagesCollection);
        }

        operation.setDestinedRecipient(destinedRecipientDecoration(opCtx));
        operation.setFromMigrateIfTrue(args.fromMigrate);
        txnParticipant.addTransactionOperation(opCtx, operation);
    } else {
        MutableOplogEntry oplogEntry;
        boost::optional<BSONObj> deletedDocForOplog = boost::none;

        if (args.retryableFindAndModifyLocation ==
            RetryableFindAndModifyLocation::kSideCollection) {
            tassert(5868703,
                    "Deleted document must be present for pre-image recording",
                    args.deletedDoc);
            invariant(opCtx->getTxnNumber());

            oplogEntry.setNeedsRetryImage({repl::RetryImageEnum::kPreImage});
            if (!args.oplogSlots.empty()) {
                oplogEntry.setOpTime(args.oplogSlots.back());
            }
        }
        opTime = replLogDelete(
            opCtx, nss, &oplogEntry, uuid, stmtId, args.fromMigrate, _oplogWriter.get());

        if (oplogEntry.getNeedsRetryImage()) {
            auto imageDoc = *(args.deletedDoc);
            auto imageToWrite = repl::ReplOperation::ImageBundle{
                repl::RetryImageEnum::kPreImage, imageDoc, opTime.writeOpTime.getTimestamp()};
            writeToImageCollection(opCtx, *opCtx->getLogicalSessionId(), imageToWrite);
        }

        // Write a pre-image to the change streams pre-images collection when following conditions
        // are met:
        // 1. The collection has 'changeStreamPreAndPostImages' enabled.
        // 2. The node wrote the oplog entry for the corresponding operation.
        // 3. The request to write the pre-image does not come from chunk-migrate event, i.e. source
        //    of the request is not 'fromMigrate'. The 'fromMigrate' events are filtered out by
        //    change streams and storing them in pre-image collection is redundant.
        // 4. a request to delete is not on a temporary resharding collection. This delete request
        //    does not result in change streams events. Recording pre-images from temporary
        //    resharing collection could result in incorrect pre-image getting recorded due to the
        //    temporary resharding collection not being consistent until writes are blocked (initial
        //    sync mode application).
        if (args.changeStreamPreAndPostImagesEnabledForCollection && !opTime.writeOpTime.isNull() &&
            !args.fromMigrate && !nss.isTemporaryReshardingCollection()) {
            tassert(5868704, "Deleted document must be set", args.deletedDoc);

            ChangeStreamPreImageId id(uuid, opTime.writeOpTime.getTimestamp(), 0);
            ChangeStreamPreImage preImage(id, opTime.wallClockTime, *args.deletedDoc);

            ChangeStreamPreImagesCollectionManager::insertPreImage(opCtx, nss.tenantId(), preImage);
        }

        SessionTxnRecord sessionTxnRecord;
        sessionTxnRecord.setLastWriteOpTime(opTime.writeOpTime);
        sessionTxnRecord.setLastWriteDate(opTime.wallClockTime);
        onWriteOpCompleted(opCtx, std::vector<StmtId>{stmtId}, sessionTxnRecord);
    }

    if (nss != NamespaceString::kSessionTransactionsTableNamespace) {
        if (!args.fromMigrate) {
            ShardingWriteRouter shardingWriteRouter(opCtx, nss, Grid::get(opCtx)->catalogCache());
            shardObserveDeleteOp(opCtx,
                                 nss,
                                 documentKey.getShardKeyAndId(),
                                 opTime.writeOpTime,
                                 shardingWriteRouter,
                                 opTime.prePostImageOpTime,
                                 inMultiDocumentTransaction);
        }
    }

    if (nss.coll() == "system.js") {
        Scope::storedFuncMod(opCtx);
    } else if (nss.isSystemDotViews()) {
        CollectionCatalog::get(opCtx)->reloadViews(opCtx, nss.dbName());
    } else if (nss == NamespaceString::kSessionTransactionsTableNamespace &&
               !opTime.writeOpTime.isNull()) {
        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
        mongoDSessionCatalog->observeDirectWriteToConfigTransactions(opCtx, documentKey.getId());
    } else if (nss == NamespaceString::kConfigSettingsNamespace) {
        ReadWriteConcernDefaults::get(opCtx).observeDirectWriteToConfigSettings(
            opCtx, documentKey.getId().firstElement(), boost::none);
    }
}

void OpObserverImpl::onInternalOpMessage(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const boost::optional<UUID>& uuid,
    const BSONObj& msgObj,
    const boost::optional<BSONObj> o2MsgObj,
    const boost::optional<repl::OpTime> preImageOpTime,
    const boost::optional<repl::OpTime> postImageOpTime,
    const boost::optional<repl::OpTime> prevWriteOpTimeInTransaction,
    const boost::optional<OplogSlot> slot) {
    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kNoop);

    oplogEntry.setTid(nss.tenantId());
    oplogEntry.setNss(nss);
    oplogEntry.setUuid(uuid);
    oplogEntry.setObject(msgObj);
    oplogEntry.setObject2(o2MsgObj);
    oplogEntry.setPreImageOpTime(preImageOpTime);
    oplogEntry.setPostImageOpTime(postImageOpTime);
    oplogEntry.setPrevWriteOpTimeInTransaction(prevWriteOpTimeInTransaction);
    if (slot) {
        oplogEntry.setOpTime(*slot);
    }
    logOperation(opCtx, &oplogEntry, true /*assignWallClockTime*/, _oplogWriter.get());
}

void OpObserverImpl::onCreateCollection(OperationContext* opCtx,
                                        const CollectionPtr& coll,
                                        const NamespaceString& collectionName,
                                        const CollectionOptions& options,
                                        const BSONObj& idIndex,
                                        const OplogSlot& createOpTime,
                                        bool fromMigrate) {
    // do not replicate system.profile modifications
    if (collectionName.isSystemDotProfile()) {
        return;
    }

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry.setTid(collectionName.tenantId());
    oplogEntry.setNss(collectionName.getCommandNS());
    oplogEntry.setUuid(options.uuid);
    oplogEntry.setObject(MutableOplogEntry::makeCreateCollCmdObj(collectionName, options, idIndex));
    oplogEntry.setFromMigrateIfTrue(fromMigrate);

    if (!createOpTime.isNull()) {
        oplogEntry.setOpTime(createOpTime);
    }
    auto opTime = logMutableOplogEntry(opCtx, &oplogEntry, _oplogWriter.get());
    if (opCtx->writesAreReplicated()) {
        if (opTime.isNull()) {
            LOGV2(7360102,
                  "Added oplog entry for create to transaction",
                  "namespace"_attr = oplogEntry.getNss(),
                  "uuid"_attr = oplogEntry.getUuid(),
                  "object"_attr = oplogEntry.getObject());
        } else {
            LOGV2(7360103,
                  "Wrote oplog entry for create",
                  "namespace"_attr = oplogEntry.getNss(),
                  "uuid"_attr = oplogEntry.getUuid(),
                  "opTime"_attr = opTime,
                  "object"_attr = oplogEntry.getObject());
        }
    }
}

void OpObserverImpl::onCollMod(OperationContext* opCtx,
                               const NamespaceString& nss,
                               const UUID& uuid,
                               const BSONObj& collModCmd,
                               const CollectionOptions& oldCollOptions,
                               boost::optional<IndexCollModInfo> indexInfo) {

    if (!nss.isSystemDotProfile()) {
        // do not replicate system.profile modifications

        // Create the 'o2' field object. We save the old collection metadata and TTL expiration.
        BSONObjBuilder o2Builder;
        o2Builder.append("collectionOptions_old", oldCollOptions.toBSON());
        if (indexInfo) {
            BSONObjBuilder oldIndexOptions;
            if (indexInfo->oldExpireAfterSeconds) {
                auto oldExpireAfterSeconds =
                    durationCount<Seconds>(indexInfo->oldExpireAfterSeconds.value());
                oldIndexOptions.append("expireAfterSeconds", oldExpireAfterSeconds);
            }
            if (indexInfo->oldHidden) {
                auto oldHidden = indexInfo->oldHidden.value();
                oldIndexOptions.append("hidden", oldHidden);
            }
            if (indexInfo->oldPrepareUnique) {
                auto oldPrepareUnique = indexInfo->oldPrepareUnique.value();
                oldIndexOptions.append("prepareUnique", oldPrepareUnique);
            }
            o2Builder.append("indexOptions_old", oldIndexOptions.obj());
        }

        MutableOplogEntry oplogEntry;
        oplogEntry.setOpType(repl::OpTypeEnum::kCommand);

        oplogEntry.setTid(nss.tenantId());
        oplogEntry.setNss(nss.getCommandNS());
        oplogEntry.setUuid(uuid);
        oplogEntry.setObject(repl::makeCollModCmdObj(collModCmd, oldCollOptions, indexInfo));
        oplogEntry.setObject2(o2Builder.done());
        auto opTime =
            logOperation(opCtx, &oplogEntry, true /*assignWallClockTime*/, _oplogWriter.get());
        if (opCtx->writesAreReplicated()) {
            LOGV2(7360104,
                  "Wrote oplog entry for collMod",
                  "namespace"_attr = oplogEntry.getNss(),
                  "uuid"_attr = oplogEntry.getUuid(),
                  "opTime"_attr = opTime,
                  "object"_attr = oplogEntry.getObject());
        }
    }

    // Make sure the UUID values in the Collection metadata, the Collection object, and the UUID
    // catalog are all present and equal.
    invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_X));
    auto databaseHolder = DatabaseHolder::get(opCtx);
    auto db = databaseHolder->getDb(opCtx, nss.dbName());
    // Some unit tests call the op observer on an unregistered Database.
    if (!db) {
        return;
    }
    const Collection* coll = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);

    invariant(coll->uuid() == uuid);
}

void OpObserverImpl::onDropDatabase(OperationContext* opCtx, const DatabaseName& dbName) {
    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);

    oplogEntry.setTid(dbName.tenantId());
    oplogEntry.setNss({dbName, "$cmd"});
    oplogEntry.setObject(BSON("dropDatabase" << 1));
    auto opTime =
        logOperation(opCtx, &oplogEntry, true /*assignWallClockTime*/, _oplogWriter.get());
    if (opCtx->writesAreReplicated()) {
        LOGV2(7360105,
              "Wrote oplog entry for dropDatabase",
              "namespace"_attr = oplogEntry.getNss(),
              "opTime"_attr = opTime,
              "object"_attr = oplogEntry.getObject());
    }

    uassert(50714,
            "dropping the admin database is not allowed.",
            dbName.db() != DatabaseName::kAdmin.db());

    if (dbName.db() == NamespaceString::kSessionTransactionsTableNamespace.db()) {
        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
        mongoDSessionCatalog->invalidateAllSessions(opCtx);
    }

    auto& bucketCatalog = timeseries::bucket_catalog::BucketCatalog::get(opCtx);
    clear(bucketCatalog, dbName.db());
}

repl::OpTime OpObserverImpl::onDropCollection(OperationContext* opCtx,
                                              const NamespaceString& collectionName,
                                              const UUID& uuid,
                                              std::uint64_t numRecords,
                                              CollectionDropType dropType) {
    return onDropCollection(
        opCtx, collectionName, uuid, numRecords, dropType, false /* markFromMigrate */);
}

repl::OpTime OpObserverImpl::onDropCollection(OperationContext* opCtx,
                                              const NamespaceString& collectionName,
                                              const UUID& uuid,
                                              std::uint64_t numRecords,
                                              const CollectionDropType dropType,
                                              bool markFromMigrate) {
    if (!collectionName.isSystemDotProfile() && opCtx->writesAreReplicated()) {
        // Do not replicate system.profile modifications.
        MutableOplogEntry oplogEntry;
        oplogEntry.setOpType(repl::OpTypeEnum::kCommand);

        oplogEntry.setTid(collectionName.tenantId());
        oplogEntry.setNss(collectionName.getCommandNS());
        oplogEntry.setUuid(uuid);
        oplogEntry.setFromMigrateIfTrue(markFromMigrate);
        oplogEntry.setObject(BSON("drop" << collectionName.coll()));
        oplogEntry.setObject2(makeObject2ForDropOrRename(numRecords));
        auto opTime =
            logOperation(opCtx, &oplogEntry, true /*assignWallClockTime*/, _oplogWriter.get());
        LOGV2(7360106,
              "Wrote oplog entry for drop",
              "namespace"_attr = oplogEntry.getNss(),
              "uuid"_attr = oplogEntry.getUuid(),
              "opTime"_attr = opTime,
              "object"_attr = oplogEntry.getObject());
    }

    uassert(50715,
            "dropping the server configuration collection (admin.system.version) is not allowed.",
            collectionName != NamespaceString::kServerConfigurationNamespace);

    if (collectionName.isSystemDotViews()) {
        CollectionCatalog::get(opCtx)->clearViews(opCtx, collectionName.dbName());
    } else if (collectionName == NamespaceString::kSessionTransactionsTableNamespace) {
        // Disallow this drop if there are currently prepared transactions.
        const auto sessionCatalog = SessionCatalog::get(opCtx);
        SessionKiller::Matcher matcherAllSessions(
            KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});
        bool noPreparedTxns = true;
        sessionCatalog->scanSessions(matcherAllSessions, [&](const ObservableSession& session) {
            auto txnParticipant = TransactionParticipant::get(session);
            if (txnParticipant.transactionIsPrepared()) {
                noPreparedTxns = false;
            }
        });
        uassert(4852500,
                "Unable to drop transactions table (config.transactions) while prepared "
                "transactions are present.",
                noPreparedTxns);

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
        mongoDSessionCatalog->invalidateAllSessions(opCtx);
    } else if (collectionName == NamespaceString::kConfigSettingsNamespace) {
        ReadWriteConcernDefaults::get(opCtx).invalidate();
    } else if (collectionName.isTimeseriesBucketsCollection()) {
        auto& bucketCatalog = timeseries::bucket_catalog::BucketCatalog::get(opCtx);
        clear(bucketCatalog, collectionName.getTimeseriesViewNamespace());
    } else if (collectionName.isSystemDotJavascript()) {
        // Inform the JavaScript engine of the change to system.js.
        Scope::storedFuncMod(opCtx);
    }

    return {};
}

void OpObserverImpl::onDropIndex(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 const UUID& uuid,
                                 const std::string& indexName,
                                 const BSONObj& indexInfo) {
    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);

    oplogEntry.setTid(nss.tenantId());
    oplogEntry.setNss(nss.getCommandNS());
    oplogEntry.setUuid(uuid);
    oplogEntry.setObject(BSON("dropIndexes" << nss.coll() << "index" << indexName));
    oplogEntry.setObject2(indexInfo);
    auto opTime =
        logOperation(opCtx, &oplogEntry, true /*assignWallClockTime*/, _oplogWriter.get());
    if (opCtx->writesAreReplicated()) {
        LOGV2(7360107,
              "Wrote oplog entry for dropIndexes",
              "namespace"_attr = oplogEntry.getNss(),
              "uuid"_attr = oplogEntry.getUuid(),
              "opTime"_attr = opTime,
              "object"_attr = oplogEntry.getObject());
    }
}

repl::OpTime OpObserverImpl::preRenameCollection(OperationContext* const opCtx,
                                                 const NamespaceString& fromCollection,
                                                 const NamespaceString& toCollection,
                                                 const UUID& uuid,
                                                 const boost::optional<UUID>& dropTargetUUID,
                                                 std::uint64_t numRecords,
                                                 bool stayTemp) {
    return preRenameCollection(opCtx,
                               fromCollection,
                               toCollection,
                               uuid,
                               dropTargetUUID,
                               numRecords,
                               stayTemp,
                               false /* markFromMigrate */);
}

repl::OpTime OpObserverImpl::preRenameCollection(OperationContext* const opCtx,
                                                 const NamespaceString& fromCollection,
                                                 const NamespaceString& toCollection,
                                                 const UUID& uuid,
                                                 const boost::optional<UUID>& dropTargetUUID,
                                                 std::uint64_t numRecords,
                                                 bool stayTemp,
                                                 bool markFromMigrate) {
    BSONObjBuilder builder;

    builder.append("renameCollection", NamespaceStringUtil::serialize(fromCollection));
    builder.append("to", NamespaceStringUtil::serialize(toCollection));
    builder.append("stayTemp", stayTemp);
    if (dropTargetUUID) {
        dropTargetUUID->appendToBuilder(&builder, "dropTarget");
    }

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);

    oplogEntry.setTid(fromCollection.tenantId());
    oplogEntry.setNss(fromCollection.getCommandNS());
    oplogEntry.setUuid(uuid);
    oplogEntry.setFromMigrateIfTrue(markFromMigrate);
    oplogEntry.setObject(builder.done());
    if (dropTargetUUID)
        oplogEntry.setObject2(makeObject2ForDropOrRename(numRecords));
    auto opTime =
        logOperation(opCtx, &oplogEntry, true /*assignWallClockTime*/, _oplogWriter.get());
    if (opCtx->writesAreReplicated()) {
        LOGV2(7360108,
              "Wrote oplog entry for renameCollection",
              "namespace"_attr = oplogEntry.getNss(),
              "uuid"_attr = oplogEntry.getUuid(),
              "opTime"_attr = opTime,
              "object"_attr = oplogEntry.getObject());
    }
    return {};
}

void OpObserverImpl::postRenameCollection(OperationContext* const opCtx,
                                          const NamespaceString& fromCollection,
                                          const NamespaceString& toCollection,
                                          const UUID& uuid,
                                          const boost::optional<UUID>& dropTargetUUID,
                                          bool stayTemp) {
    if (fromCollection.isSystemDotViews())
        CollectionCatalog::get(opCtx)->reloadViews(opCtx, fromCollection.dbName());
    if (toCollection.isSystemDotViews())
        CollectionCatalog::get(opCtx)->reloadViews(opCtx, toCollection.dbName());
}

void OpObserverImpl::onRenameCollection(OperationContext* const opCtx,
                                        const NamespaceString& fromCollection,
                                        const NamespaceString& toCollection,
                                        const UUID& uuid,
                                        const boost::optional<UUID>& dropTargetUUID,
                                        std::uint64_t numRecords,
                                        bool stayTemp) {
    onRenameCollection(opCtx,
                       fromCollection,
                       toCollection,
                       uuid,
                       dropTargetUUID,
                       numRecords,
                       stayTemp,
                       false /* markFromMigrate */);
}

void OpObserverImpl::onRenameCollection(OperationContext* const opCtx,
                                        const NamespaceString& fromCollection,
                                        const NamespaceString& toCollection,
                                        const UUID& uuid,
                                        const boost::optional<UUID>& dropTargetUUID,
                                        std::uint64_t numRecords,
                                        bool stayTemp,
                                        bool markFromMigrate) {
    preRenameCollection(opCtx,
                        fromCollection,
                        toCollection,
                        uuid,
                        dropTargetUUID,
                        numRecords,
                        stayTemp,
                        markFromMigrate);
    postRenameCollection(opCtx, fromCollection, toCollection, uuid, dropTargetUUID, stayTemp);
}

void OpObserverImpl::onImportCollection(OperationContext* opCtx,
                                        const UUID& importUUID,
                                        const NamespaceString& nss,
                                        long long numRecords,
                                        long long dataSize,
                                        const BSONObj& catalogEntry,
                                        const BSONObj& storageMetadata,
                                        bool isDryRun) {
    ImportCollectionOplogEntry importCollection(
        nss, importUUID, numRecords, dataSize, catalogEntry, storageMetadata, isDryRun);

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);

    oplogEntry.setTid(nss.tenantId());
    oplogEntry.setNss(nss.getCommandNS());
    oplogEntry.setObject(importCollection.toBSON());
    logOperation(opCtx, &oplogEntry, true /*assignWallClockTime*/, _oplogWriter.get());
}

void OpObserverImpl::onApplyOps(OperationContext* opCtx,
                                const DatabaseName& dbName,
                                const BSONObj& applyOpCmd) {
    MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);

    oplogEntry.setTid(dbName.tenantId());
    oplogEntry.setNss({dbName, "$cmd"});
    oplogEntry.setObject(applyOpCmd);
    logOperation(opCtx, &oplogEntry, true /*assignWallClockTime*/, _oplogWriter.get());
}

void OpObserverImpl::onEmptyCapped(OperationContext* opCtx,
                                   const NamespaceString& collectionName,
                                   const UUID& uuid) {
    if (!collectionName.isSystemDotProfile()) {
        // Do not replicate system.profile modifications
        MutableOplogEntry oplogEntry;
        oplogEntry.setOpType(repl::OpTypeEnum::kCommand);

        oplogEntry.setTid(collectionName.tenantId());
        oplogEntry.setNss(collectionName.getCommandNS());
        oplogEntry.setUuid(uuid);
        oplogEntry.setObject(BSON("emptycapped" << collectionName.coll()));
        logOperation(opCtx, &oplogEntry, true /*assignWallClockTime*/, _oplogWriter.get());
    }
}

namespace {

/**
 * Writes pre-images for update/replace/delete operations packed into a single "applyOps" entry to
 * the change stream pre-images collection if required. The operations are defined by sequence
 * ['stmtBegin', 'stmtEnd'). 'applyOpsTimestamp' and 'operationTime' are the timestamp and the wall
 * clock time, respectively, of the "applyOps" entry. A pre-image is recorded for an operation only
 * if pre-images are enabled for the collection the operation is issued on.
 */
void writeChangeStreamPreImagesForApplyOpsEntries(
    OperationContext* opCtx,
    std::vector<repl::ReplOperation>::const_iterator stmtBegin,
    std::vector<repl::ReplOperation>::const_iterator stmtEnd,
    Timestamp applyOpsTimestamp,
    Date_t operationTime) {
    int64_t applyOpsIndex{0};
    for (auto stmtIterator = stmtBegin; stmtIterator != stmtEnd; ++stmtIterator) {
        auto& operation = *stmtIterator;
        if (operation.isChangeStreamPreImageRecordedInPreImagesCollection() &&
            !operation.getNss().isTemporaryReshardingCollection()) {
            invariant(operation.getUuid());
            invariant(!operation.getPreImage().isEmpty());

            ChangeStreamPreImagesCollectionManager::insertPreImage(
                opCtx,
                operation.getTid(),
                ChangeStreamPreImage{
                    ChangeStreamPreImageId{*operation.getUuid(), applyOpsTimestamp, applyOpsIndex},
                    operationTime,
                    operation.getPreImage()});
        }
        ++applyOpsIndex;
    }
}

/**
 * Returns maximum number of operations to pack into a single oplog entry,
 * when multi-oplog format for transactions is in use.
 *
 * Stop packing when either number of transaction operations is reached, or when the
 * next one would make the total size of operations larger than the maximum BSON Object
 * User Size. We rely on the headroom between BSONObjMaxUserSize and
 * BSONObjMaxInternalSize to cover the BSON overhead and the other "applyOps" entry
 * fields. But if a single operation in the set exceeds BSONObjMaxUserSize, we still fit
 * it, as a single max-length operation should be able to be packed into an "applyOps"
 * entry.
 */
std::size_t getMaxNumberOfTransactionOperationsInSingleOplogEntry() {
    tassert(6278503,
            "gMaxNumberOfTransactionOperationsInSingleOplogEntry should be positive number",
            gMaxNumberOfTransactionOperationsInSingleOplogEntry > 0);
    return static_cast<std::size_t>(gMaxNumberOfTransactionOperationsInSingleOplogEntry);
}

/**
 * Returns maximum size (bytes) of operations to pack into a single oplog entry,
 * when multi-oplog format for transactions is in use.
 *
 * Refer to getMaxNumberOfTransactionOperationsInSingleOplogEntry() comments for a
 * description on packing transaction operations into "applyOps" entries.
 */
std::size_t getMaxSizeOfTransactionOperationsInSingleOplogEntryBytes() {
    return static_cast<std::size_t>(BSONObjMaxUserSize);
}

/**
 * Returns maximum number of operations to pack into a single oplog entry,
 * when multi-oplog format for batched writes is in use.
 */
std::size_t getMaxNumberOfBatchedOperationsInSingleOplogEntry() {
    // IDL validation defined for this startup parameter ensures that we have a positive number.
    return static_cast<std::size_t>(gMaxNumberOfBatchedOperationsInSingleOplogEntry);
}

/**
 * Returns maximum size (bytes) of operations to pack into a single oplog entry,
 * when multi-oplog format for batched writes is in use.
 */
std::size_t getMaxSizeOfBatchedOperationsInSingleOplogEntryBytes() {
    // IDL validation defined for this startup parameter ensures that we have a positive number.
    return static_cast<std::size_t>(gMaxSizeOfBatchedOperationsInSingleOplogEntryBytes);
}

/**
 * Writes change stream pre-images for transaction 'operations'. The 'applyOpsOperationAssignment'
 * contains a representation of "applyOps" entries to be written for the transaction. The
 * 'operationTime' is wall clock time of the operations used for the pre-image documents.
 */
void writeChangeStreamPreImagesForTransaction(
    OperationContext* opCtx,
    const std::vector<repl::ReplOperation>& operations,
    const OpObserver::ApplyOpsOplogSlotAndOperationAssignment& applyOpsOperationAssignment,
    Date_t operationTime) {
    // This function must be called from an outer WriteUnitOfWork in order to be rolled back upon
    // reaching the exception.
    invariant(opCtx->lockState()->inAWriteUnitOfWork());

    auto applyOpsEntriesIt = applyOpsOperationAssignment.applyOpsEntries.begin();
    for (auto operationIter = operations.begin(); operationIter != operations.end();) {
        tassert(6278507,
                "Unexpected end of applyOps entries vector",
                applyOpsEntriesIt != applyOpsOperationAssignment.applyOpsEntries.end());
        const auto& applyOpsEntry = *applyOpsEntriesIt++;
        const auto operationSequenceEnd = operationIter + applyOpsEntry.operations.size();
        writeChangeStreamPreImagesForApplyOpsEntries(opCtx,
                                                     operationIter,
                                                     operationSequenceEnd,
                                                     applyOpsEntry.oplogSlot.getTimestamp(),
                                                     operationTime);
        operationIter = operationSequenceEnd;
    }
}

// Logs one applyOps entry on a prepared transaction, or an unprepared transaction's commit, or on
// committing a WUOW that is not necessarily tied to a multi-document transaction. It may update the
// transactions table on multi-document transactions. Assumes that the given BSON builder object
// already has  an 'applyOps' field appended pointing to the desired array of ops i.e. { "applyOps"
// : [op1, op2, ...] }
//
// @param txnState the 'state' field of the transaction table entry update.  @param startOpTime the
// optime of the 'startOpTime' field of the transaction table entry update. If boost::none, no
// 'startOpTime' field will be included in the new transaction table entry. Only meaningful if
// 'updateTxnTable' is true.  @param updateTxnTable determines whether the transactions table will
// updated after the oplog entry is written.
//
// Returns the optime of the written oplog entry.
repl::OpTime logApplyOps(OperationContext* opCtx,
                         MutableOplogEntry* oplogEntry,
                         DurableTxnStateEnum txnState,
                         boost::optional<repl::OpTime> startOpTime,
                         std::vector<StmtId> stmtIdsWritten,
                         const bool updateTxnTable,
                         OplogWriter* oplogWriter) {
    if (!stmtIdsWritten.empty()) {
        invariant(isInternalSessionForRetryableWrite(*opCtx->getLogicalSessionId()));
    }

    const auto txnRetryCounter = opCtx->getTxnRetryCounter();

    invariant(bool(txnRetryCounter) == bool(TransactionParticipant::get(opCtx)));

    // Batched writes (that is, WUOWs with 'groupOplogEntries') are not associated with a txnNumber,
    // so do not emit an lsid either.
    oplogEntry->setSessionId(opCtx->getTxnNumber() ? opCtx->getLogicalSessionId() : boost::none);
    oplogEntry->setTxnNumber(opCtx->getTxnNumber());
    if (txnRetryCounter && !isDefaultTxnRetryCounter(*txnRetryCounter)) {
        oplogEntry->getOperationSessionInfo().setTxnRetryCounter(*txnRetryCounter);
    }

    try {
        auto writeOpTime =
            logOperation(opCtx, oplogEntry, false /*assignWallClockTime*/, oplogWriter);
        if (updateTxnTable) {
            SessionTxnRecord sessionTxnRecord;
            sessionTxnRecord.setLastWriteOpTime(writeOpTime);
            sessionTxnRecord.setLastWriteDate(oplogEntry->getWallClockTime());
            sessionTxnRecord.setState(txnState);
            sessionTxnRecord.setStartOpTime(startOpTime);
            if (txnRetryCounter && !isDefaultTxnRetryCounter(*txnRetryCounter)) {
                sessionTxnRecord.setTxnRetryCounter(*txnRetryCounter);
            }
            onWriteOpCompleted(opCtx, std::move(stmtIdsWritten), sessionTxnRecord);
        }
        return writeOpTime;
    } catch (const AssertionException& e) {
        // Change the error code to TransactionTooLarge if it is BSONObjectTooLarge.
        uassert(ErrorCodes::TransactionTooLarge,
                e.reason(),
                e.code() != ErrorCodes::BSONObjectTooLarge);
        throw;
    }
    MONGO_UNREACHABLE;
}

void logCommitOrAbortForPreparedTransaction(OperationContext* opCtx,
                                            MutableOplogEntry* oplogEntry,
                                            DurableTxnStateEnum durableState,
                                            OplogWriter* oplogWriter) {
    const auto txnRetryCounter = *opCtx->getTxnRetryCounter();

    oplogEntry->setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry->setNss(NamespaceString::kAdminCommandNamespace);
    oplogEntry->setSessionId(opCtx->getLogicalSessionId());
    oplogEntry->setTxnNumber(opCtx->getTxnNumber());
    if (!isDefaultTxnRetryCounter(txnRetryCounter)) {
        oplogEntry->getOperationSessionInfo().setTxnRetryCounter(txnRetryCounter);
    }
    oplogEntry->setPrevWriteOpTimeInTransaction(
        TransactionParticipant::get(opCtx).getLastWriteOpTime());

    // There should not be a parent WUOW outside of this one. This guarantees the safety of the
    // write conflict retry loop.
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());

    // We must not have a maximum lock timeout, since writing the commit or abort oplog entry for a
    // prepared transaction must always succeed.
    invariant(!opCtx->lockState()->hasMaxLockTimeout());

    writeConflictRetry(
        opCtx, "onPreparedTransactionCommitOrAbort", NamespaceString::kRsOplogNamespace.ns(), [&] {
            // Writes to the oplog only require a Global intent lock. Guaranteed by
            // OplogSlotReserver.
            invariant(opCtx->lockState()->isWriteLocked());

            WriteUnitOfWork wuow(opCtx);
            const auto oplogOpTime =
                logOperation(opCtx, oplogEntry, true /*assignWallClockTime*/, oplogWriter);
            invariant(oplogEntry->getOpTime().isNull() || oplogEntry->getOpTime() == oplogOpTime);

            SessionTxnRecord sessionTxnRecord;
            sessionTxnRecord.setLastWriteOpTime(oplogOpTime);
            sessionTxnRecord.setLastWriteDate(oplogEntry->getWallClockTime());
            sessionTxnRecord.setState(durableState);
            if (!isDefaultTxnRetryCounter(txnRetryCounter)) {
                sessionTxnRecord.setTxnRetryCounter(txnRetryCounter);
            }
            onWriteOpCompleted(opCtx, {}, sessionTxnRecord);
            wuow.commit();
        });
}

}  // namespace

void OpObserverImpl::onTransactionStart(OperationContext* opCtx) {}

void OpObserverImpl::onUnpreparedTransactionCommit(
    OperationContext* opCtx, const TransactionOperations& transactionOperations) {
    const auto& statements = transactionOperations.getOperationsForOpObserver();
    auto numberOfPrePostImagesToWrite = transactionOperations.getNumberOfPrePostImagesToWrite();

    invariant(opCtx->getTxnNumber());

    if (!opCtx->writesAreReplicated()) {
        return;
    }

    // It is possible that the transaction resulted in no changes.  In that case, we should
    // not write an empty applyOps entry.
    if (statements.empty())
        return;

    repl::OpTime commitOpTime;
    // Reserve all the optimes in advance, so we only need to get the optime mutex once.  We
    // reserve enough entries for all statements in the transaction.
    auto oplogSlots =
        _oplogWriter->getNextOpTimes(opCtx, statements.size() + numberOfPrePostImagesToWrite);

    // Throw TenantMigrationConflict error if the database for the transaction statements is being
    // migrated. We only need check the namespace of the first statement since a transaction's
    // statements must all be for the same tenant.
    tenant_migration_access_blocker::checkIfCanWriteOrThrow(
        opCtx, statements.begin()->getNss().dbName(), oplogSlots.back().getTimestamp());

    if (MONGO_unlikely(hangAndFailUnpreparedCommitAfterReservingOplogSlot.shouldFail())) {
        hangAndFailUnpreparedCommitAfterReservingOplogSlot.pauseWhileSet(opCtx);
        uasserted(51268, "hangAndFailUnpreparedCommitAfterReservingOplogSlot fail point enabled");
    }

    // Serialize transaction statements to BSON and determine their assignment to "applyOps"
    // entries.
    const auto applyOpsOplogSlotAndOperationAssignment = transactionOperations.getApplyOpsInfo(
        oplogSlots,
        getMaxNumberOfTransactionOperationsInSingleOplogEntry(),
        getMaxSizeOfTransactionOperationsInSingleOplogEntryBytes(),
        /*prepare=*/false);
    invariant(!applyOpsOplogSlotAndOperationAssignment.prepare);
    const auto wallClockTime = getWallClockTimeForOpLog(opCtx);

    // Storage transaction commit is the last place inside a transaction that can throw an
    // exception. In order to safely allow exceptions to be thrown at that point, this function must
    // be called from an outer WriteUnitOfWork in order to be rolled back upon reaching the
    // exception.
    invariant(opCtx->lockState()->inAWriteUnitOfWork());

    // Writes to the oplog only require a Global intent lock. Guaranteed by
    // OplogSlotReserver.
    invariant(opCtx->lockState()->isWriteLocked());

    if (const auto& info = applyOpsOplogSlotAndOperationAssignment;
        info.applyOpsEntries.size() > 1U ||           // partial transaction
        info.numOperationsWithNeedsRetryImage > 0) {  // pre/post image to store in image collection
        // Partial transactions and unprepared transactions with pre or post image stored in the
        // image collection create/reserve multiple oplog entries in the same WriteUnitOfWork.
        // Because of this, such transactions will set multiple timestamps, violating the
        // multi timestamp constraint. It's safe to ignore the multi timestamp constraints here
        // as additional rollback logic is in place for this case. See SERVER-48771.
        opCtx->recoveryUnit()->ignoreAllMultiTimestampConstraints();
    }

    auto logApplyOpsForUnpreparedTransaction =
        [opCtx, &oplogSlots, oplogWriter = _oplogWriter.get()](repl::MutableOplogEntry* oplogEntry,
                                                               bool firstOp,
                                                               bool lastOp,
                                                               std::vector<StmtId> stmtIdsWritten) {
            return logApplyOps(
                opCtx,
                oplogEntry,
                /*txnState=*/
                (lastOp ? DurableTxnStateEnum::kCommitted : DurableTxnStateEnum::kInProgress),
                /*startOpTime=*/boost::make_optional(!lastOp, oplogSlots.front()),
                std::move(stmtIdsWritten),
                /*updateTxnTable=*/(firstOp || lastOp),
                oplogWriter);
        };

    // Log in-progress entries for the transaction along with the implicit commit.
    boost::optional<repl::ReplOperation::ImageBundle> imageToWrite;
    auto numOplogEntries =
        transactionOperations.logOplogEntries(oplogSlots,
                                              applyOpsOplogSlotAndOperationAssignment,
                                              wallClockTime,
                                              logApplyOpsForUnpreparedTransaction,
                                              &imageToWrite);
    invariant(numOplogEntries > 0);

    // Write change stream pre-images. At this point the pre-images will be written at the
    // transaction commit timestamp as driven (implicitly) by the last written "applyOps" oplog
    // entry.
    writeChangeStreamPreImagesForTransaction(
        opCtx, statements, applyOpsOplogSlotAndOperationAssignment, wallClockTime);

    if (imageToWrite) {
        writeToImageCollection(opCtx, *opCtx->getLogicalSessionId(), *imageToWrite);
    }

    commitOpTime = oplogSlots[numOplogEntries - 1];
    invariant(!commitOpTime.isNull());
    shardObserveTransactionPrepareOrUnpreparedCommit(opCtx, statements, commitOpTime);
}

void OpObserverImpl::onBatchedWriteStart(OperationContext* opCtx) {
    auto& batchedWriteContext = BatchedWriteContext::get(opCtx);
    batchedWriteContext.setWritesAreBatched(true);
}

void OpObserverImpl::onBatchedWriteCommit(OperationContext* opCtx) {
    if (repl::ReplicationCoordinator::get(opCtx)->getReplicationMode() !=
            repl::ReplicationCoordinator::modeReplSet ||
        !opCtx->writesAreReplicated()) {
        return;
    }

    auto& batchedWriteContext = BatchedWriteContext::get(opCtx);
    auto* batchedOps = batchedWriteContext.getBatchedOperations(opCtx);

    if (batchedOps->isEmpty()) {
        return;
    }

    // Reserve all the optimes in advance, so we only need to get the optime mutex once.  We
    // reserve enough entries for all statements in the transaction.
    auto oplogSlots = _oplogWriter->getNextOpTimes(opCtx, batchedOps->numOperations());

    // Throw TenantMigrationConflict error if the database for the transaction statements is being
    // migrated. We only need check the namespace of the first statement since a transaction's
    // statements must all be for the same tenant.
    const auto& statements = batchedOps->getOperationsForOpObserver();
    const auto& firstOpNss = statements.begin()->getNss();
    tenant_migration_access_blocker::checkIfCanWriteOrThrow(
        opCtx, firstOpNss.dbName(), oplogSlots.back().getTimestamp());

    boost::optional<repl::ReplOperation::ImageBundle> noPrePostImage;

    // Serialize batched statements to BSON and determine their assignment to "applyOps"
    // entries.
    // By providing limits on operation count and size, this makes the processing of batched writes
    // more consistent with our treatment of multi-doc transactions.
    const auto applyOpsOplogSlotAndOperationAssignment =
        batchedOps->getApplyOpsInfo(oplogSlots,
                                    getMaxNumberOfBatchedOperationsInSingleOplogEntry(),
                                    getMaxSizeOfBatchedOperationsInSingleOplogEntryBytes(),
                                    /*prepare=*/false);

    if (!gFeatureFlagInternalWritesAreReplicatedTransactionally.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        // Before SERVER-70765, we relied on packTransactionStatementsForApplyOps() to check if the
        // batch of operations could fit in a single applyOps entry. Now, we pass the size limit to
        // TransactionOperations::getApplyOpsInfo() and are now able to return an error earlier.
        // Previously, this used to be a tripwire assertion (tassert). This is now a uassert to be
        // consistent with packTransactionStatementsForApplyOps().
        uassert(ErrorCodes::TransactionTooLarge,
                "batched writes must generate a single applyOps entry",
                applyOpsOplogSlotAndOperationAssignment.applyOpsEntries.size() == 1);
    } else if (applyOpsOplogSlotAndOperationAssignment.applyOpsEntries.size() > 1) {
        // Batched writes spanning multiple oplog entries create/reserve multiple oplog entries in
        // the same WriteUnitOfWork. Because of this, such batched writes will set multiple
        // timestamps, violating the multi timestamp constraint. It's safe to ignore the multi
        // timestamp constraints here.
        // TODO(SERVER-72723): implement rollback logic for batched writes spanning multiple
        // entries.
        opCtx->recoveryUnit()->ignoreAllMultiTimestampConstraints();
    }

    // Storage transaction commit is the last place inside a transaction that can throw an
    // exception. In order to safely allow exceptions to be thrown at that point, this function must
    // be called from an outer WriteUnitOfWork in order to be rolled back upon reaching the
    // exception.
    invariant(opCtx->lockState()->inAWriteUnitOfWork());

    // Writes to the oplog only require a Global intent lock. Guaranteed by
    // OplogSlotReserver.
    invariant(opCtx->lockState()->isWriteLocked());

    // Batched writes do not violate the multiple timestamp constraint because they do not
    // replicate over multiple applyOps oplog entries or write pre/post images to the
    // image collection. However, multi-doc transactions may be replicated as a chain of
    // applyOps oplog entries in addition to potentially writing to the image collection.
    // Therefore, there are cases where the multiple timestamp constraint has to be relaxed
    // in order to replicate multi-doc transactions.
    // See onTransactionPrepare() and onUnpreparedTransactionCommit().
    invariant(applyOpsOplogSlotAndOperationAssignment.numOperationsWithNeedsRetryImage == 0,
              "batched writes must not contain pre/post images to store in image collection");

    auto logApplyOpsForBatchedWrite =
        [opCtx, oplogWriter = _oplogWriter.get()](repl::MutableOplogEntry* oplogEntry,
                                                  bool firstOp,
                                                  bool lastOp,
                                                  std::vector<StmtId> stmtIdsWritten) {
            // Remove 'prevOpTime' when replicating as a single applyOps oplog entry.
            // This preserves backwards compatibility with the legacy atomic applyOps oplog
            // entry format that we use to replicate batched writes.
            // OplogApplierImpl::_deriveOpsAndFillWriterVectors() enforces this restriction
            // using an invariant added in SERVER-43651.
            // For batched writes that replicate over a chain of applyOps oplog entries, we include
            // 'prevOpTime' so that oplog application is able to consume all the linked operations,
            // similar to large multi-document transactions. See SERVER-70572.
            if (firstOp && lastOp) {
                oplogEntry->setPrevWriteOpTimeInTransaction(boost::none);
            }
            return logApplyOps(opCtx,
                               oplogEntry,
                               /*txnState=*/DurableTxnStateEnum::kCommitted,  // unused
                               /*startOpTime=*/boost::none,
                               std::move(stmtIdsWritten),
                               /*updateTxnTable=*/false,
                               oplogWriter);
        };

    const auto wallClockTime = getWallClockTimeForOpLog(opCtx);
    invariant(!applyOpsOplogSlotAndOperationAssignment.prepare);

    (void)batchedOps->logOplogEntries(oplogSlots,
                                      applyOpsOplogSlotAndOperationAssignment,
                                      wallClockTime,
                                      logApplyOpsForBatchedWrite,
                                      &noPrePostImage);
}

void OpObserverImpl::onBatchedWriteAbort(OperationContext* opCtx) {
    auto& batchedWriteContext = BatchedWriteContext::get(opCtx);
    batchedWriteContext.clearBatchedOperations(opCtx);
    batchedWriteContext.setWritesAreBatched(false);
}

void OpObserverImpl::onPreparedTransactionCommit(
    OperationContext* opCtx,
    OplogSlot commitOplogEntryOpTime,
    Timestamp commitTimestamp,
    const std::vector<repl::ReplOperation>& statements) noexcept {
    invariant(opCtx->getTxnNumber());

    if (!opCtx->writesAreReplicated()) {
        return;
    }

    invariant(!commitTimestamp.isNull());

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpTime(commitOplogEntryOpTime);

    CommitTransactionOplogObject cmdObj;
    cmdObj.setCommitTimestamp(commitTimestamp);
    oplogEntry.setObject(cmdObj.toBSON());

    logCommitOrAbortForPreparedTransaction(
        opCtx, &oplogEntry, DurableTxnStateEnum::kCommitted, _oplogWriter.get());
}

std::unique_ptr<OpObserver::ApplyOpsOplogSlotAndOperationAssignment>
OpObserverImpl::preTransactionPrepare(OperationContext* opCtx,
                                      const std::vector<OplogSlot>& reservedSlots,
                                      const TransactionOperations& transactionOperations,
                                      Date_t wallClockTime) {
    auto applyOpsOplogSlotAndOperationAssignment = transactionOperations.getApplyOpsInfo(
        reservedSlots,
        getMaxNumberOfTransactionOperationsInSingleOplogEntry(),
        getMaxSizeOfTransactionOperationsInSingleOplogEntryBytes(),
        /*prepare=*/true);
    const auto& statements = transactionOperations.getOperationsForOpObserver();
    writeChangeStreamPreImagesForTransaction(
        opCtx, statements, applyOpsOplogSlotAndOperationAssignment, wallClockTime);
    return std::make_unique<OpObserver::ApplyOpsOplogSlotAndOperationAssignment>(
        std::move(applyOpsOplogSlotAndOperationAssignment));
}

void OpObserverImpl::onTransactionPrepare(
    OperationContext* opCtx,
    const std::vector<OplogSlot>& reservedSlots,
    const TransactionOperations& transactionOperations,
    const ApplyOpsOplogSlotAndOperationAssignment& applyOpsOperationAssignment,
    size_t numberOfPrePostImagesToWrite,
    Date_t wallClockTime) {
    invariant(!reservedSlots.empty());
    const auto prepareOpTime = reservedSlots.back();
    invariant(opCtx->getTxnNumber());
    invariant(!prepareOpTime.isNull());

    const auto& statements = transactionOperations.getOperationsForOpObserver();

    // Don't write oplog entry on secondaries.
    if (!opCtx->writesAreReplicated()) {
        return;
    }

    {
        // We should have reserved enough slots.
        invariant(reservedSlots.size() >= statements.size());
        TransactionParticipant::SideTransactionBlock sideTxn(opCtx);

        writeConflictRetry(
            opCtx, "onTransactionPrepare", NamespaceString::kRsOplogNamespace.ns(), [&] {
                // Writes to the oplog only require a Global intent lock. Guaranteed by
                // OplogSlotReserver.
                invariant(opCtx->lockState()->isWriteLocked());

                WriteUnitOfWork wuow(opCtx);
                // It is possible that the transaction resulted in no changes, In that case, we
                // should not write any operations other than the prepare oplog entry.
                if (!statements.empty()) {
                    // Storage transaction commit is the last place inside a transaction that can
                    // throw an exception. In order to safely allow exceptions to be thrown at that
                    // point, this function must be called from an outer WriteUnitOfWork in order to
                    // be rolled back upon reaching the exception.
                    invariant(opCtx->lockState()->inAWriteUnitOfWork());

                    // Writes to the oplog only require a Global intent lock. Guaranteed by
                    // OplogSlotReserver.
                    invariant(opCtx->lockState()->isWriteLocked());

                    if (applyOpsOperationAssignment.applyOpsEntries.size() > 1U) {
                        // Partial transactions create/reserve multiple oplog entries in the same
                        // WriteUnitOfWork. Because of this, such transactions will set multiple
                        // timestamps, violating the multi timestamp constraint. It's safe to ignore
                        // the multi timestamp constraints here as additional rollback logic is in
                        // place for this case. See SERVER-48771.
                        opCtx->recoveryUnit()->ignoreAllMultiTimestampConstraints();
                    }

                    // This is set for every oplog entry, except for the last one, in the applyOps
                    // chain of an unprepared multi-doc transaction.
                    // For a single prepare oplog entry, choose the last oplog slot for the first
                    // optime of the transaction. The first optime corresponds to the 'startOpTime'
                    // field in SessionTxnRecord that is persisted in config.transactions.
                    // See SERVER-40678.
                    auto startOpTime = applyOpsOperationAssignment.applyOpsEntries.size() == 1U
                        ? reservedSlots.back()
                        : reservedSlots.front();

                    auto logApplyOpsForPreparedTransaction =
                        [opCtx, oplogWriter = _oplogWriter.get(), startOpTime](
                            repl::MutableOplogEntry* oplogEntry,
                            bool firstOp,
                            bool lastOp,
                            std::vector<StmtId> stmtIdsWritten) {
                            return logApplyOps(opCtx,
                                               oplogEntry,
                                               /*txnState=*/
                                               (lastOp ? DurableTxnStateEnum::kPrepared
                                                       : DurableTxnStateEnum::kInProgress),
                                               startOpTime,
                                               std::move(stmtIdsWritten),
                                               /*updateTxnTable=*/(firstOp || lastOp),
                                               oplogWriter);
                        };

                    // We had reserved enough oplog slots for the worst case where each operation
                    // produced one oplog entry.  When operations are smaller and can be packed, we
                    // will waste the extra slots.  The implicit prepare oplog entry will still use
                    // the last reserved slot, because the transaction participant has already used
                    // that as the prepare time.
                    boost::optional<repl::ReplOperation::ImageBundle> imageToWrite;
                    invariant(applyOpsOperationAssignment.prepare);
                    (void)transactionOperations.logOplogEntries(reservedSlots,
                                                                applyOpsOperationAssignment,
                                                                wallClockTime,
                                                                logApplyOpsForPreparedTransaction,
                                                                &imageToWrite);
                    if (imageToWrite) {
                        writeToImageCollection(opCtx, *opCtx->getLogicalSessionId(), *imageToWrite);
                    }
                } else {
                    // Log an empty 'prepare' oplog entry.
                    // We need to have at least one reserved slot.
                    invariant(reservedSlots.size() > 0);
                    BSONObjBuilder applyOpsBuilder;
                    BSONArrayBuilder opsArray(applyOpsBuilder.subarrayStart("applyOps"_sd));
                    opsArray.done();
                    applyOpsBuilder.append("prepare", true);

                    auto oplogSlot = reservedSlots.front();
                    MutableOplogEntry oplogEntry;
                    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
                    oplogEntry.setNss(NamespaceString::kAdminCommandNamespace);
                    oplogEntry.setOpTime(oplogSlot);
                    oplogEntry.setPrevWriteOpTimeInTransaction(repl::OpTime());
                    oplogEntry.setObject(applyOpsBuilder.done());
                    oplogEntry.setWallClockTime(wallClockTime);

                    // TODO SERVER-69286: set the top-level tenantId here

                    logApplyOps(opCtx,
                                &oplogEntry,
                                DurableTxnStateEnum::kPrepared,
                                /*startOpTime=*/oplogSlot,
                                /*stmtIdsWritten=*/{},
                                /*updateTxnTable=*/true,
                                _oplogWriter.get());
                }
                wuow.commit();
            });
    }

    shardObserveTransactionPrepareOrUnpreparedCommit(opCtx, statements, prepareOpTime);
}

void OpObserverImpl::onTransactionPrepareNonPrimary(OperationContext* opCtx,
                                                    const std::vector<repl::OplogEntry>& statements,
                                                    const repl::OpTime& prepareOpTime) {
    shardObserveNonPrimaryTransactionPrepare(opCtx, statements, prepareOpTime);
}

void OpObserverImpl::onTransactionAbort(OperationContext* opCtx,
                                        boost::optional<OplogSlot> abortOplogEntryOpTime) {
    invariant(opCtx->getTxnNumber());

    if (!opCtx->writesAreReplicated()) {
        return;
    }

    auto txnParticipant = TransactionParticipant::get(opCtx);
    invariant(txnParticipant);

    if (!abortOplogEntryOpTime) {
        invariant(!txnParticipant.transactionIsCommitted());
        return;
    }

    MutableOplogEntry oplogEntry;
    oplogEntry.setOpTime(*abortOplogEntryOpTime);

    AbortTransactionOplogObject cmdObj;
    oplogEntry.setObject(cmdObj.toBSON());

    logCommitOrAbortForPreparedTransaction(
        opCtx, &oplogEntry, DurableTxnStateEnum::kAborted, _oplogWriter.get());
}

void OpObserverImpl::onModifyCollectionShardingIndexCatalog(OperationContext* opCtx,
                                                            const NamespaceString& nss,
                                                            const UUID& uuid,
                                                            BSONObj opDoc) {
    repl::MutableOplogEntry oplogEntry;
    auto obj = BSON(kShardingIndexCatalogOplogEntryName << nss.toString()).addFields(opDoc);
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry.setNss(nss);
    oplogEntry.setUuid(uuid);
    oplogEntry.setObject(obj);

    logOperation(opCtx, &oplogEntry, true, _oplogWriter.get());
}

void OpObserverImpl::_onReplicationRollback(OperationContext* opCtx,
                                            const RollbackObserverInfo& rbInfo) {
    // Reset the key manager cache.
    auto validator = LogicalTimeValidator::get(opCtx);
    if (validator) {
        validator->resetKeyManagerCache();
    }

    // Check if the shard identity document rolled back.
    if (rbInfo.shardIdentityRolledBack) {
        fassertFailedNoTrace(50712);
    }

    // Force the default read/write concern cache to reload on next access in case the defaults
    // document was rolled back.
    ReadWriteConcernDefaults::get(opCtx).invalidate();

    stdx::unordered_set<NamespaceString> timeseriesNamespaces;
    for (const auto& ns : rbInfo.rollbackNamespaces) {
        if (ns.isTimeseriesBucketsCollection()) {
            timeseriesNamespaces.insert(ns.getTimeseriesViewNamespace());
        }
    }
    auto& bucketCatalog = timeseries::bucket_catalog::BucketCatalog::get(opCtx);
    clear(bucketCatalog,
          [timeseriesNamespaces = std::move(timeseriesNamespaces)](
              const NamespaceString& bucketNs) { return timeseriesNamespaces.contains(bucketNs); });
}

}  // namespace mongo
