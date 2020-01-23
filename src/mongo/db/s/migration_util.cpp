/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/migration_util.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/query.h"
#include "mongo/db/catalog/collection_catalog_helper.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_session_cache.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/write_concern.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {
namespace migrationutil {
namespace {

const char kSourceShard[] = "source";
const char kDestinationShard[] = "destination";
const char kIsDonorShard[] = "isDonorShard";
const char kChunk[] = "chunk";
const char kCollection[] = "collection";

const WriteConcernOptions kMajorityWriteConcern(WriteConcernOptions::kMajority,
                                                WriteConcernOptions::SyncMode::UNSET,
                                                WriteConcernOptions::kNoTimeout);

template <typename Cmd>
void sendToRecipient(OperationContext* opCtx, const ShardId& recipientId, const Cmd& cmd) {
    auto recipientShard =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, recipientId));

    LOG(1) << "Sending request " << cmd.toBSON({}) << " to recipient.";

    auto response = recipientShard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        "config",
        cmd.toBSON(BSON(WriteConcernOptions::kWriteConcernField << WriteConcernOptions::Majority)),
        Shard::RetryPolicy::kIdempotent);

    uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(response));
}

}  // namespace

BSONObj makeMigrationStatusDocument(const NamespaceString& nss,
                                    const ShardId& fromShard,
                                    const ShardId& toShard,
                                    const bool& isDonorShard,
                                    const BSONObj& min,
                                    const BSONObj& max) {
    BSONObjBuilder builder;
    builder.append(kSourceShard, fromShard.toString());
    builder.append(kDestinationShard, toShard.toString());
    builder.append(kIsDonorShard, isDonorShard);
    builder.append(kChunk, BSON(ChunkType::min(min) << ChunkType::max(max)));
    builder.append(kCollection, nss.ns());
    return builder.obj();
}

Query overlappingRangeQuery(const ChunkRange& range, const UUID& uuid) {
    return QUERY(RangeDeletionTask::kCollectionUuidFieldName
                 << uuid << RangeDeletionTask::kRangeFieldName + "." + ChunkRange::kMinKey << LT
                 << range.getMax() << RangeDeletionTask::kRangeFieldName + "." + ChunkRange::kMaxKey
                 << GT << range.getMin());
}

bool checkForConflictingDeletions(OperationContext* opCtx,
                                  const ChunkRange& range,
                                  const UUID& uuid) {
    PersistentTaskStore<RangeDeletionTask> store(opCtx, NamespaceString::kRangeDeletionNamespace);

    return store.count(opCtx, overlappingRangeQuery(range, uuid)) > 0;
}

ExecutorFuture<bool> submitRangeDeletionTask(OperationContext* opCtx,
                                             const RangeDeletionTask& deletionTask) {
    const auto serviceContext = opCtx->getServiceContext();
    // TODO (SERVER-45577): Use the Grid's fixed executor once the refresh is done asynchronously.
    // An arbitrary executor is being used temporarily because unit tests have only one thread in
    // the fixed executor, and that thread is needed to respond to the refresh.
    return ExecutorFuture<void>(
               Grid::get(serviceContext)->getExecutorPool()->getArbitraryExecutor())
        .then([=] {
            ThreadClient tc(kRangeDeletionThreadName, serviceContext);
            {
                stdx::lock_guard<Client> lk(*tc.get());
                tc->setSystemOperationKillable(lk);
            }
            auto uniqueOpCtx = tc->makeOperationContext();
            auto opCtx = uniqueOpCtx.get();

            boost::optional<AutoGetCollection> autoColl;
            autoColl.emplace(opCtx, deletionTask.getNss(), MODE_IS);

            auto css = CollectionShardingRuntime::get(opCtx, deletionTask.getNss());
            if (!css->getCurrentMetadataIfKnown() ||
                !css->getCurrentMetadata()->uuidMatches(deletionTask.getCollectionUuid())) {
                // If the collection's filtering metadata is not known or its UUID does not match
                // the UUID of the deletion task, force a filtering metadata refresh once, because
                // this node may have just stepped up and therefore may have a stale cache.
                LOG(0) << "Filtering metadata for namespace in deletion task "
                       << deletionTask.toBSON()
                       << (css->getCurrentMetadataIfKnown()
                               ? "has UUID that does not match UUID of the deletion task"
                               : "is not known")
                       << ", forcing a refresh of " << deletionTask.getNss();

                // TODO (SERVER-45577): Add an asynchronous version of
                // forceShardFilteringMetadataRefresh to avoid blocking on the network in the
                // thread pool.
                autoColl.reset();
                forceShardFilteringMetadataRefresh(opCtx, deletionTask.getNss(), true);
            }

            autoColl.emplace(opCtx, deletionTask.getNss(), MODE_IS);
            if (!css->getCurrentMetadataIfKnown() ||
                !css->getCurrentMetadata()->uuidMatches(deletionTask.getCollectionUuid())) {
                LOG(0) << "Even after forced refresh, filtering metadata for namespace in deletion "
                          "task "
                       << deletionTask.toBSON()
                       << (css->getCurrentMetadataIfKnown()
                               ? "has UUID that does not match UUID of the deletion task"
                               : "is not known")
                       << ", deleting the task.";

                autoColl.reset();
                deleteRangeDeletionTaskLocally(
                    opCtx, deletionTask.getId(), ShardingCatalogClient::kLocalWriteConcern);
                return false;
            }

            LOG(0) << "Submitting range deletion task " << deletionTask.toBSON();

            const auto whenToClean = deletionTask.getWhenToClean() == CleanWhenEnum::kNow
                ? CollectionShardingRuntime::kNow
                : CollectionShardingRuntime::kDelayed;

            auto cleanupCompleteFuture = css->cleanUpRange(deletionTask.getRange(), whenToClean);

            if (cleanupCompleteFuture.isReady() &&
                !cleanupCompleteFuture.getNoThrow(opCtx).isOK()) {
                LOG(0) << "Failed to submit range deletion task " << deletionTask.toBSON()
                       << causedBy(cleanupCompleteFuture.getNoThrow(opCtx));
                return false;
            }
            return true;
        });
}

void submitPendingDeletions(OperationContext* opCtx) {
    PersistentTaskStore<RangeDeletionTask> store(opCtx, NamespaceString::kRangeDeletionNamespace);

    auto query = QUERY("pending" << BSON("$exists" << false));

    std::vector<RangeDeletionTask> invalidRanges;
    store.forEach(opCtx, query, [&opCtx, &invalidRanges](const RangeDeletionTask& deletionTask) {
        migrationutil::submitRangeDeletionTask(opCtx, deletionTask);
        return true;
    });
}

void resubmitRangeDeletionsOnStepUp(ServiceContext* serviceContext) {
    LOG(0) << "Starting pending deletion submission thread.";

    auto executor = Grid::get(serviceContext)->getExecutorPool()->getFixedExecutor();

    ExecutorFuture<void>(executor).getAsync([serviceContext](const Status& status) {
        ThreadClient tc("ResubmitRangeDeletions", serviceContext);
        {
            stdx::lock_guard<Client> lk(*tc.get());
            tc->setSystemOperationKillable(lk);
        }

        auto opCtx = tc->makeOperationContext();

        submitPendingDeletions(opCtx.get());
    });
}

void dropRangeDeletionsCollection(OperationContext* opCtx) {
    DBDirectClient client(opCtx);
    client.dropCollection(NamespaceString::kRangeDeletionNamespace.toString(),
                          WriteConcerns::kMajorityWriteConcern);
}

template <typename Callable>
void forEachOrphanRange(OperationContext* opCtx, const NamespaceString& nss, Callable&& handler) {
    AutoGetCollection autoColl(opCtx, nss, MODE_IX);

    const auto css = CollectionShardingRuntime::get(opCtx, nss);
    const auto metadata = css->getCurrentMetadata();
    const auto emptyChunkMap =
        RangeMap{SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<BSONObj>()};

    if (!metadata->isSharded()) {
        LOG(0) << "Upgrade: skipping orphaned range enumeration for " << nss
               << ", collection is not sharded";
        return;
    }

    auto startingKey = metadata->getMinKey();

    while (true) {
        auto range = metadata->getNextOrphanRange(emptyChunkMap, startingKey);
        if (!range) {
            LOG(2) << "Upgrade: Completed orphaned range enumeration for " << nss.toString()
                   << " starting from " << redact(startingKey) << ", no orphan ranges remain";

            return;
        }

        handler(*range);

        startingKey = range->getMax();
    }
}

void submitOrphanRanges(OperationContext* opCtx, const NamespaceString& nss, const UUID& uuid) {
    try {
        auto version = forceShardFilteringMetadataRefresh(opCtx, nss, true);

        if (version == ChunkVersion::UNSHARDED())
            return;

        LOG(2) << "Upgrade: Cleaning up existing orphans for " << nss << " : " << uuid;

        std::vector<RangeDeletionTask> deletions;
        forEachOrphanRange(opCtx, nss, [&deletions, &opCtx, &nss, &uuid](const auto& range) {
            // Since this is not part of an active migration, the migration UUID and the donor shard
            // are set to unused values so that they don't conflict.
            RangeDeletionTask task(
                UUID::gen(), nss, uuid, ShardId("fromFCVUpgrade"), range, CleanWhenEnum::kDelayed);
            deletions.emplace_back(task);
        });

        if (deletions.empty())
            return;

        PersistentTaskStore<RangeDeletionTask> store(opCtx,
                                                     NamespaceString::kRangeDeletionNamespace);

        for (const auto& task : deletions) {
            LOG(2) << "Upgrade: Submitting range for cleanup: " << task.getRange() << " from "
                   << nss;
            store.add(opCtx, task);
        }
    } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>& e) {
        LOG(0) << "Upgrade: Failed to cleanup orphans for " << nss
               << " because the namespace was not found: " << e.what()
               << ", the collection must have been dropped";
    }
}

void submitOrphanRangesForCleanup(OperationContext* opCtx) {
    auto& catalog = CollectionCatalog::get(opCtx);
    const auto& dbs = catalog.getAllDbNames();

    for (const auto& dbName : dbs) {
        if (dbName == NamespaceString::kLocalDb)
            continue;

        for (auto collIt = catalog.begin(dbName); collIt != catalog.end(); ++collIt) {
            auto uuid = collIt.uuid().get();
            auto nss = catalog.lookupNSSByUUID(opCtx, uuid).get();
            LOG(2) << "Upgrade: processing collection: " << nss;

            submitOrphanRanges(opCtx, nss, uuid);
        }
    }
}

void persistMigrationCoordinatorLocally(OperationContext* opCtx,
                                        const MigrationCoordinatorDocument& migrationDoc) {
    PersistentTaskStore<MigrationCoordinatorDocument> store(
        opCtx, NamespaceString::kMigrationCoordinatorsNamespace);
    try {
        store.add(opCtx, migrationDoc);
    } catch (const ExceptionFor<ErrorCodes::DuplicateKey>&) {
        // Convert a DuplicateKey error to an anonymous error.
        uasserted(
            31374,
            str::stream() << "While attempting to write migration information for migration "
                          << ", found document with the same migration id. Attempted migration: "
                          << migrationDoc.toBSON());
    }
}

void persistRangeDeletionTaskLocally(OperationContext* opCtx,
                                     const RangeDeletionTask& deletionTask) {
    PersistentTaskStore<RangeDeletionTask> store(opCtx, NamespaceString::kRangeDeletionNamespace);
    try {
        store.add(opCtx, deletionTask);
    } catch (const ExceptionFor<ErrorCodes::DuplicateKey>&) {
        // Convert a DuplicateKey error to an anonymous error.
        uasserted(31375,
                  str::stream() << "While attempting to write range deletion task for migration "
                                << ", found document with the same migration id. Attempted range "
                                   "deletion task: "
                                << deletionTask.toBSON());
    }
}

void persistCommitDecision(OperationContext* opCtx, const UUID& migrationId) {
    PersistentTaskStore<MigrationCoordinatorDocument> store(
        opCtx, NamespaceString::kMigrationCoordinatorsNamespace);
    store.update(
        opCtx,
        QUERY(MigrationCoordinatorDocument::kIdFieldName << migrationId),
        BSON("$set" << BSON(MigrationCoordinatorDocument::kDecisionFieldName << "committed")));
}

void persistAbortDecision(OperationContext* opCtx, const UUID& migrationId) {
    PersistentTaskStore<MigrationCoordinatorDocument> store(
        opCtx, NamespaceString::kMigrationCoordinatorsNamespace);
    store.update(
        opCtx,
        QUERY(MigrationCoordinatorDocument::kIdFieldName << migrationId),
        BSON("$set" << BSON(MigrationCoordinatorDocument::kDecisionFieldName << "aborted")));
}

void deleteRangeDeletionTaskOnRecipient(OperationContext* opCtx,
                                        const ShardId& recipientId,
                                        const UUID& migrationId) {
    write_ops::Delete deleteOp(NamespaceString::kRangeDeletionNamespace);
    write_ops::DeleteOpEntry query(BSON(RangeDeletionTask::kIdFieldName << migrationId),
                                   false /*multi*/);
    deleteOp.setDeletes({query});

    sendToRecipient(opCtx, recipientId, deleteOp);
}

void deleteRangeDeletionTaskLocally(OperationContext* opCtx,
                                    const UUID& deletionTaskId,
                                    const WriteConcernOptions& writeConcern) {
    PersistentTaskStore<RangeDeletionTask> store(opCtx, NamespaceString::kRangeDeletionNamespace);
    store.remove(opCtx, QUERY(RangeDeletionTask::kIdFieldName << deletionTaskId), writeConcern);
}

void deleteRangeDeletionTasksForCollectionLocally(OperationContext* opCtx,
                                                  const UUID& collectionUuid) {
    PersistentTaskStore<RangeDeletionTask> store(opCtx, NamespaceString::kRangeDeletionNamespace);
    store.remove(opCtx, QUERY(RangeDeletionTask::kCollectionUuidFieldName << collectionUuid));
}

void markAsReadyRangeDeletionTaskOnRecipient(OperationContext* opCtx,
                                             const ShardId& recipientId,
                                             const UUID& migrationId) {
    write_ops::Update updateOp(NamespaceString::kRangeDeletionNamespace);
    auto queryFilter = BSON(RangeDeletionTask::kIdFieldName << migrationId);
    auto updateModification = write_ops::UpdateModification(
        BSON("$unset" << BSON(RangeDeletionTask::kPendingFieldName << "")));
    write_ops::UpdateOpEntry updateEntry(queryFilter, updateModification);
    updateEntry.setMulti(false);
    updateEntry.setUpsert(false);
    updateOp.setUpdates({updateEntry});

    sendToRecipient(opCtx, recipientId, updateOp);
}

void markAsReadyRangeDeletionTaskLocally(OperationContext* opCtx, const UUID& migrationId) {
    PersistentTaskStore<RangeDeletionTask> store(opCtx, NamespaceString::kRangeDeletionNamespace);
    auto query = QUERY(RangeDeletionTask::kIdFieldName << migrationId);
    auto update = BSON("$unset" << BSON(RangeDeletionTask::kPendingFieldName << ""));

    store.update(opCtx, query, update);
}

void deleteMigrationCoordinatorDocumentLocally(OperationContext* opCtx, const UUID& migrationId) {
    PersistentTaskStore<MigrationCoordinatorDocument> store(
        opCtx, NamespaceString::kMigrationCoordinatorsNamespace);
    store.remove(opCtx,
                 QUERY(MigrationCoordinatorDocument::kIdFieldName << migrationId),
                 {1, WriteConcernOptions::SyncMode::UNSET, Seconds(0)});
}
}  // namespace migrationutil
}  // namespace mongo
