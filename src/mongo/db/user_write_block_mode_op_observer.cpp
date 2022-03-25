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

#include "mongo/db/user_write_block_mode_op_observer.h"

#include "mongo/db/s/global_user_write_block_state.h"
#include "mongo/db/s/user_writes_critical_section_document_gen.h"
#include "mongo/db/s/user_writes_recoverable_critical_section_service.h"

namespace mongo {
namespace {

const auto documentIdDecoration = OperationContext::declareDecoration<BSONObj>();

bool isStandaloneOrPrimary(OperationContext* opCtx) {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    return replCoord->canAcceptWritesForDatabase(opCtx, NamespaceString::kAdminDb);
}

}  // namespace

void UserWriteBlockModeOpObserver::onInserts(OperationContext* opCtx,
                                             const NamespaceString& nss,
                                             const UUID& uuid,
                                             std::vector<InsertStatement>::const_iterator first,
                                             std::vector<InsertStatement>::const_iterator last,
                                             bool fromMigrate) {
    if (!fromMigrate) {
        _checkWriteAllowed(opCtx, nss);
    }

    if (nss == NamespaceString::kUserWritesCriticalSectionsNamespace &&
        !user_writes_recoverable_critical_section_util::inRecoveryMode(opCtx)) {
        for (auto it = first; it != last; ++it) {
            const auto& insertedDoc = it->doc;

            const auto collCSDoc = UserWriteBlockingCriticalSectionDocument::parse(
                IDLParserErrorContext("UserWriteBlockOpObserver"), insertedDoc);
            opCtx->recoveryUnit()->onCommit(
                [opCtx,
                 blockShardedDDL = collCSDoc.getBlockNewUserShardedDDL(),
                 blockWrites = collCSDoc.getBlockUserWrites(),
                 insertedNss = collCSDoc.getNss()](boost::optional<Timestamp>) {
                    invariant(insertedNss.isEmpty());
                    boost::optional<Lock::GlobalLock> globalLockIfNotPrimary;
                    if (!isStandaloneOrPrimary(opCtx)) {
                        globalLockIfNotPrimary.emplace(opCtx, MODE_IX);
                    }

                    if (blockShardedDDL) {
                        GlobalUserWriteBlockState::get(opCtx)->enableUserShardedDDLBlocking(opCtx);
                    }

                    if (blockWrites) {
                        GlobalUserWriteBlockState::get(opCtx)->enableUserWriteBlocking(opCtx);
                    }
                });
        }
    }
}

void UserWriteBlockModeOpObserver::onUpdate(OperationContext* opCtx,
                                            const OplogUpdateEntryArgs& args) {
    if (args.updateArgs->source != OperationSource::kFromMigrate) {
        _checkWriteAllowed(opCtx, args.nss);
    }

    if (args.nss == NamespaceString::kUserWritesCriticalSectionsNamespace &&
        !user_writes_recoverable_critical_section_util::inRecoveryMode(opCtx)) {
        const auto collCSDoc = UserWriteBlockingCriticalSectionDocument::parse(
            IDLParserErrorContext("UserWriteBlockOpObserver"), args.updateArgs->updatedDoc);

        opCtx->recoveryUnit()->onCommit(
            [opCtx,
             updatedNss = collCSDoc.getNss(),
             blockShardedDDL = collCSDoc.getBlockNewUserShardedDDL(),
             blockWrites = collCSDoc.getBlockUserWrites(),
             insertedNss = collCSDoc.getNss()](boost::optional<Timestamp>) {
                invariant(updatedNss.isEmpty());
                boost::optional<Lock::GlobalLock> globalLockIfNotPrimary;
                if (!isStandaloneOrPrimary(opCtx)) {
                    globalLockIfNotPrimary.emplace(opCtx, MODE_IX);
                }

                if (blockShardedDDL) {
                    GlobalUserWriteBlockState::get(opCtx)->enableUserShardedDDLBlocking(opCtx);
                } else {
                    GlobalUserWriteBlockState::get(opCtx)->disableUserShardedDDLBlocking(opCtx);
                }

                if (blockWrites) {
                    GlobalUserWriteBlockState::get(opCtx)->enableUserWriteBlocking(opCtx);
                } else {
                    GlobalUserWriteBlockState::get(opCtx)->disableUserWriteBlocking(opCtx);
                }
            });
    }
}

void UserWriteBlockModeOpObserver::aboutToDelete(OperationContext* opCtx,
                                                 NamespaceString const& nss,
                                                 const UUID& uuid,
                                                 BSONObj const& doc) {
    if (nss == NamespaceString::kUserWritesCriticalSectionsNamespace) {
        documentIdDecoration(opCtx) = doc;
    }
}

void UserWriteBlockModeOpObserver::onDelete(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            const UUID& uuid,
                                            StmtId stmtId,
                                            const OplogDeleteEntryArgs& args) {
    if (!args.fromMigrate) {
        _checkWriteAllowed(opCtx, nss);
    }

    if (nss == NamespaceString::kUserWritesCriticalSectionsNamespace &&
        !user_writes_recoverable_critical_section_util::inRecoveryMode(opCtx)) {
        auto& documentId = documentIdDecoration(opCtx);
        invariant(!documentId.isEmpty());

        const auto& deletedDoc = documentId;
        const auto collCSDoc = UserWriteBlockingCriticalSectionDocument::parse(
            IDLParserErrorContext("UserWriteBlockOpObserver"), deletedDoc);

        opCtx->recoveryUnit()->onCommit(
            [opCtx, deletedNss = collCSDoc.getNss()](boost::optional<Timestamp>) {
                invariant(deletedNss.isEmpty());
                boost::optional<Lock::GlobalLock> globalLockIfNotPrimary;
                if (!isStandaloneOrPrimary(opCtx)) {
                    globalLockIfNotPrimary.emplace(opCtx, MODE_IX);
                }

                GlobalUserWriteBlockState::get(opCtx)->disableUserShardedDDLBlocking(opCtx);
                GlobalUserWriteBlockState::get(opCtx)->disableUserWriteBlocking(opCtx);
            });
    }
}

void UserWriteBlockModeOpObserver::_onReplicationRollback(OperationContext* opCtx,
                                                          const RollbackObserverInfo& rbInfo) {
    if (rbInfo.rollbackNamespaces.find(NamespaceString::kUserWritesCriticalSectionsNamespace) !=
        rbInfo.rollbackNamespaces.end()) {
        UserWritesRecoverableCriticalSectionService::get(opCtx)->recoverRecoverableCriticalSections(
            opCtx);
    }
}

void UserWriteBlockModeOpObserver::_checkWriteAllowed(OperationContext* opCtx,
                                                      const NamespaceString& nss) {
    if (isStandaloneOrPrimary(opCtx)) {
        GlobalUserWriteBlockState::get(opCtx)->checkUserWritesAllowed(opCtx, nss);
    }
}

}  // namespace mongo
