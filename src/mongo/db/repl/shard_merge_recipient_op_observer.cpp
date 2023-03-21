/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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


#include "mongo/db/repl/shard_merge_recipient_op_observer.h"

#include <fmt/format.h>

#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/drop_database.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/repl/shard_merge_recipient_service.h"
#include "mongo/db/repl/tenant_file_importer_service.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/tenant_migration_decoration.h"
#include "mongo/db/repl/tenant_migration_recipient_access_blocker.h"
#include "mongo/db/repl/tenant_migration_shard_merge_util.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/db/repl/tenant_migration_util.h"
#include "mongo/db/serverless/serverless_operation_lock_registry.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

namespace mongo::repl {
using namespace fmt;
namespace {
void deleteTenantDataWhenMergeAborts(OperationContext* opCtx,
                                     const ShardMergeRecipientDocument& doc) {
    invariant(doc.getAbortOpTime());
    invariant(opCtx->lockState()->inAWriteUnitOfWork());

    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    const auto dropOpTime = *doc.getAbortOpTime();

    UnreplicatedWritesBlock writeBlock{opCtx};
    AllowLockAcquisitionOnTimestampedUnitOfWork allowAcquisitionOfLocks(opCtx->lockState());

    for (const auto& tenantId : doc.getTenantIds()) {
        std::vector<DatabaseName> databases;
        if (gMultitenancySupport) {
            databases = storageEngine->listDatabases(tenantId);
        } else {
            auto allDatabases = storageEngine->listDatabases();
            std::copy_if(allDatabases.begin(),
                         allDatabases.end(),
                         std::back_inserter(databases),
                         [tenant = tenantId.toString() + "_"](const DatabaseName& db) {
                             return StringData{db.db()}.startsWith(tenant);
                         });
        }

        for (const auto& database : databases) {
            AutoGetDb autoDb{opCtx, database, MODE_X};
            Database* db = autoDb.getDb();
            if (!db) {
                continue;
            }

            LOGV2(7221802,
                  "Dropping tenant database for shard merge garbage collection",
                  "tenant"_attr = tenantId,
                  "database"_attr = database,
                  "migrationId"_attr = doc.getId(),
                  "abortOpTime"_attr = *doc.getAbortOpTime());

            IndexBuildsCoordinator::get(opCtx)->assertNoBgOpInProgForDb(db->name());

            auto catalog = CollectionCatalog::get(opCtx);
            for (auto collIt = catalog->begin(opCtx, db->name()); collIt != catalog->end(opCtx);
                 ++collIt) {
                auto collection = *collIt;
                if (!collection) {
                    break;
                }

                uassertStatusOK(
                    db->dropCollectionEvenIfSystem(opCtx, collection->ns(), dropOpTime));
            }

            auto databaseHolder = DatabaseHolder::get(opCtx);
            databaseHolder->close(opCtx, db->name());
        }
    }
}

void onShardMergeRecipientsNssInsert(OperationContext* opCtx,
                                     std::vector<InsertStatement>::const_iterator first,
                                     std::vector<InsertStatement>::const_iterator last) {
    if (tenant_migration_access_blocker::inRecoveryMode(opCtx))
        return;

    for (auto it = first; it != last; it++) {
        auto recipientStateDoc =
            ShardMergeRecipientDocument::parse(IDLParserContext("recipientStateDoc"), it->doc);
        switch (recipientStateDoc.getState()) {
            case ShardMergeRecipientStateEnum::kStarted: {
                invariant(!recipientStateDoc.getStartGarbageCollect());

                auto migrationId = recipientStateDoc.getId();
                ServerlessOperationLockRegistry::get(opCtx->getServiceContext())
                    .acquireLock(ServerlessOperationLockRegistry::LockType::kMergeRecipient,
                                 migrationId);

                auto mtab = std::make_shared<TenantMigrationRecipientAccessBlocker>(
                    opCtx->getServiceContext(), migrationId);
                TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                    .add(recipientStateDoc.getTenantIds(), mtab);

                opCtx->recoveryUnit()->onRollback([migrationId](OperationContext* opCtx) {
                    TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                        .removeAccessBlockersForMigration(
                            migrationId, TenantMigrationAccessBlocker::BlockerType::kRecipient);
                    ServerlessOperationLockRegistry::get(opCtx->getServiceContext())
                        .releaseLock(ServerlessOperationLockRegistry::LockType::kMergeRecipient,
                                     migrationId);
                });

                opCtx->recoveryUnit()->onCommit([migrationId](OperationContext* opCtx, auto _) {
                    repl::TenantFileImporterService::get(opCtx)->startMigration(migrationId);
                });
            } break;
            case ShardMergeRecipientStateEnum::kCommitted:
            case ShardMergeRecipientStateEnum::kAborted:
                invariant(recipientStateDoc.getStartGarbageCollect());
                break;
            default:
                MONGO_UNREACHABLE;
        }
    }
}

void onDonatedFilesCollNssInsert(OperationContext* opCtx,
                                 std::vector<InsertStatement>::const_iterator first,
                                 std::vector<InsertStatement>::const_iterator last) {
    if (tenant_migration_access_blocker::inRecoveryMode(opCtx))
        return;

    for (auto it = first; it != last; it++) {
        const auto& metadataDoc = it->doc;
        auto migrationId =
            uassertStatusOK(UUID::parse(metadataDoc[shard_merge_utils::kMigrationIdFieldName]));
        repl::TenantFileImporterService::get(opCtx)->learnedFilename(migrationId, metadataDoc);
    }
}

void assertStateTransitionIsValid(ShardMergeRecipientStateEnum prevState,
                                  ShardMergeRecipientStateEnum nextState) {

    auto validPrevStates = [&]() -> stdx::unordered_set<ShardMergeRecipientStateEnum> {
        switch (nextState) {
            case ShardMergeRecipientStateEnum::kStarted:
                return {ShardMergeRecipientStateEnum::kStarted};
            case ShardMergeRecipientStateEnum::kLearnedFilenames:
                return {ShardMergeRecipientStateEnum::kStarted,
                        ShardMergeRecipientStateEnum::kLearnedFilenames};
            case ShardMergeRecipientStateEnum::kConsistent:
                return {ShardMergeRecipientStateEnum::kLearnedFilenames,
                        ShardMergeRecipientStateEnum::kConsistent};
            case ShardMergeRecipientStateEnum::kCommitted:
                return {ShardMergeRecipientStateEnum::kConsistent,
                        ShardMergeRecipientStateEnum::kCommitted};
            case ShardMergeRecipientStateEnum::kAborted:
                return {ShardMergeRecipientStateEnum::kStarted,
                        ShardMergeRecipientStateEnum::kLearnedFilenames,
                        ShardMergeRecipientStateEnum::kConsistent,
                        ShardMergeRecipientStateEnum::kAborted};
            default:
                MONGO_UNREACHABLE;
        }
    }();

    uassert(7339766, "Invalid state transition", validPrevStates.contains(prevState));
}

void onTransitioningToLearnedFilenames(OperationContext* opCtx,
                                       const ShardMergeRecipientDocument& recipientStateDoc) {
    opCtx->recoveryUnit()->onCommit(
        [migrationId = recipientStateDoc.getId()](OperationContext* opCtx, auto _) {
            repl::TenantFileImporterService::get(opCtx)->learnedAllFilenames(migrationId);
        });
}

void onTransitioningToConsistent(OperationContext* opCtx,
                                 const ShardMergeRecipientDocument& recipientStateDoc) {
    if (recipientStateDoc.getRejectReadsBeforeTimestamp()) {
        opCtx->recoveryUnit()->onCommit([recipientStateDoc](OperationContext* opCtx, auto _) {
            auto mtab = tenant_migration_access_blocker::getRecipientAccessBlockerForMigration(
                opCtx->getServiceContext(), recipientStateDoc.getId());
            invariant(mtab);
            mtab->startRejectingReadsBefore(
                recipientStateDoc.getRejectReadsBeforeTimestamp().get());
        });
    }
}

void onTransitioningToCommitted(OperationContext* opCtx,
                                const ShardMergeRecipientDocument& recipientStateDoc) {
    auto migrationId = recipientStateDoc.getId();
    // It's safe to do interrupt outside of onCommit hook as the decision to forget a migration or
    // the migration decision is not reversible.
    repl::TenantFileImporterService::get(opCtx)->interrupt(migrationId);

    auto markedGCAfterMigrationStart = [&] {
        return !recipientStateDoc.getStartGarbageCollect() && recipientStateDoc.getExpireAt();
    }();

    if (markedGCAfterMigrationStart) {
        opCtx->recoveryUnit()->onCommit([migrationId](OperationContext* opCtx, auto _) {
            auto mtab = tenant_migration_access_blocker::getRecipientAccessBlockerForMigration(
                opCtx->getServiceContext(), migrationId);
            invariant(mtab);
            // Once the migration is committed and state doc is marked garbage collectable,
            // the TTL deletions should be unblocked for the imported donor collections.
            mtab->stopBlockingTTL();

            ServerlessOperationLockRegistry::get(opCtx->getServiceContext())
                .releaseLock(ServerlessOperationLockRegistry::LockType::kMergeRecipient,
                             migrationId);
        });
    }
}

void onTransitioningToAborted(OperationContext* opCtx,
                              const ShardMergeRecipientDocument& recipientStateDoc) {
    auto migrationId = recipientStateDoc.getId();
    // It's safe to do interrupt outside of onCommit hook as the decision to forget a migration or
    // the migration decision is not reversible.
    repl::TenantFileImporterService::get(opCtx)->interrupt(migrationId);

    auto markedGCAfterMigrationStart = [&] {
        return !recipientStateDoc.getStartGarbageCollect() && recipientStateDoc.getExpireAt();
    }();

    if (markedGCAfterMigrationStart) {
        opCtx->recoveryUnit()->onCommit([migrationId](OperationContext* opCtx, auto _) {
            // Remove access blocker and release locks to allow faster migration retry.
            // (Note: Not needed to unblock TTL deletions as we would have already dropped all
            // imported donor collections immediately on transitioning to `kAborted`).

            TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                .removeAccessBlockersForMigration(
                    migrationId, TenantMigrationAccessBlocker::BlockerType::kRecipient);

            ServerlessOperationLockRegistry::get(opCtx->getServiceContext())
                .releaseLock(ServerlessOperationLockRegistry::LockType::kMergeRecipient,
                             migrationId);
        });

        tenantMigrationInfo(opCtx) =
            boost::make_optional<TenantMigrationInfo>(recipientStateDoc.getId());
        deleteTenantDataWhenMergeAborts(opCtx, recipientStateDoc);
    }
}
}  // namespace

void ShardMergeRecipientOpObserver::onCreateCollection(OperationContext* opCtx,
                                                       const CollectionPtr& coll,
                                                       const NamespaceString& collectionName,
                                                       const CollectionOptions& options,
                                                       const BSONObj& idIndex,
                                                       const OplogSlot& createOpTime,
                                                       bool fromMigrate) {
    if (!shard_merge_utils::isDonatedFilesCollection(collectionName) ||
        tenant_migration_access_blocker::inRecoveryMode(opCtx)) {
        return;
    }

    auto collString = collectionName.coll().toString();
    auto migrationUUID = uassertStatusOK(UUID::parse(collString.substr(collString.find('.') + 1)));
    auto fileClonerTempDirPath = shard_merge_utils::fileClonerTempDir(migrationUUID);

    boost::system::error_code ec;
    bool createdNewDir = boost::filesystem::create_directory(fileClonerTempDirPath, ec);
    uassert(7339767,
            str::stream() << "Failed to create WT temp directory:: "
                          << fileClonerTempDirPath.generic_string() << ", Error:: " << ec.message(),
            !ec);
    uassert(7339768, str::stream() << "WT temp directory already exists", !createdNewDir);
}

void ShardMergeRecipientOpObserver::onInserts(OperationContext* opCtx,
                                              const CollectionPtr& coll,
                                              std::vector<InsertStatement>::const_iterator first,
                                              std::vector<InsertStatement>::const_iterator last,
                                              bool fromMigrate) {
    if (coll->ns() == NamespaceString::kShardMergeRecipientsNamespace) {
        onShardMergeRecipientsNssInsert(opCtx, first, last);
        return;
    }

    if (shard_merge_utils::isDonatedFilesCollection(coll->ns())) {
        onDonatedFilesCollNssInsert(opCtx, first, last);
        return;
    }
}

void ShardMergeRecipientOpObserver::onUpdate(OperationContext* opCtx,
                                             const OplogUpdateEntryArgs& args) {

    if (args.coll->ns() != NamespaceString::kShardMergeRecipientsNamespace ||
        tenant_migration_access_blocker::inRecoveryMode(opCtx)) {
        return;
    }

    auto prevState = ShardMergeRecipientState_parse(
        IDLParserContext("preImageRecipientStateDoc"),
        args.updateArgs->preImageDoc[ShardMergeRecipientDocument::kStateFieldName]
            .valueStringData());
    auto recipientStateDoc = ShardMergeRecipientDocument::parse(
        IDLParserContext("recipientStateDoc"), args.updateArgs->updatedDoc);
    auto nextState = recipientStateDoc.getState();

    assertStateTransitionIsValid(prevState, nextState);

    switch (nextState) {
        case ShardMergeRecipientStateEnum::kStarted:
            break;
        case ShardMergeRecipientStateEnum::kLearnedFilenames:
            onTransitioningToLearnedFilenames(opCtx, recipientStateDoc);
            break;
        case ShardMergeRecipientStateEnum::kConsistent:
            onTransitioningToConsistent(opCtx, recipientStateDoc);
            break;
        case ShardMergeRecipientStateEnum::kCommitted:
            onTransitioningToCommitted(opCtx, recipientStateDoc);
            break;
        case ShardMergeRecipientStateEnum::kAborted:
            onTransitioningToAborted(opCtx, recipientStateDoc);
            break;
        default:
            MONGO_UNREACHABLE;
    }
}

void ShardMergeRecipientOpObserver::aboutToDelete(OperationContext* opCtx,
                                                  const CollectionPtr& coll,
                                                  BSONObj const& doc) {
    if (coll->ns() != NamespaceString::kShardMergeRecipientsNamespace ||
        tenant_migration_access_blocker::inRecoveryMode(opCtx)) {
        return;
    }

    auto recipientStateDoc =
        ShardMergeRecipientDocument::parse(IDLParserContext("recipientStateDoc"), doc);

    bool isDocMarkedGarbageCollectable = [&] {
        auto state = recipientStateDoc.getState();
        auto expireAtIsSet = recipientStateDoc.getExpireAt().has_value();
        invariant(!expireAtIsSet || state == ShardMergeRecipientStateEnum::kCommitted ||
                  state == ShardMergeRecipientStateEnum::kAborted);
        return expireAtIsSet;
    }();

    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "Cannot delete the recipient state document "
                          << " since it has not been marked as garbage collectable: "
                          << tenant_migration_util::redactStateDoc(recipientStateDoc.toBSON()),
            isDocMarkedGarbageCollectable);

    tenantMigrationInfo(opCtx) = TenantMigrationInfo(recipientStateDoc.getId());
}

void ShardMergeRecipientOpObserver::onDelete(OperationContext* opCtx,
                                             const CollectionPtr& coll,
                                             StmtId stmtId,
                                             const OplogDeleteEntryArgs& args) {
    if (coll->ns() != NamespaceString::kShardMergeRecipientsNamespace ||
        tenant_migration_access_blocker::inRecoveryMode(opCtx)) {
        return;
    }

    if (auto tmi = tenantMigrationInfo(opCtx)) {
        opCtx->recoveryUnit()->onCommit([migrationId = tmi->uuid](OperationContext* opCtx, auto _) {
            LOGV2_INFO(7339765,
                       "Removing expired recipient access blocker",
                       "migrationId"_attr = migrationId);
            TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                .removeAccessBlockersForMigration(
                    migrationId, TenantMigrationAccessBlocker::BlockerType::kRecipient);
        });
    }
}

repl::OpTime ShardMergeRecipientOpObserver::onDropCollection(OperationContext* opCtx,
                                                             const NamespaceString& collectionName,
                                                             const UUID& uuid,
                                                             std::uint64_t numRecords,
                                                             const CollectionDropType dropType) {
    if (collectionName == NamespaceString::kShardMergeRecipientsNamespace &&
        !tenant_migration_access_blocker::inRecoveryMode(opCtx)) {

        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "Cannot drop "
                              << NamespaceString::kShardMergeRecipientsNamespace.ns()
                              << " collection as it is not empty",
                !numRecords);
    }
    return OpTime();
}

}  // namespace mongo::repl
