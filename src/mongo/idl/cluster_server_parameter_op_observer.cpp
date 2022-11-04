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

#include "mongo/idl/cluster_server_parameter_op_observer.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/idl/cluster_server_parameter_initializer.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {

namespace {
constexpr auto kIdField = "_id"_sd;
constexpr auto kOplog = "oplog"_sd;

/**
 * Per-operation scratch space indicating the document being deleted and the tenantId of the tenant
 * associated. This is used in the aboutToDelte/onDelete handlers since the document is not
 * necessarily available in the latter.
 */
const auto aboutToDeleteDoc = OperationContext::declareDecoration<std::string>();
const auto tenantIdToDelete = OperationContext::declareDecoration<boost::optional<TenantId>>();

bool isConfigNamespace(const NamespaceString& nss) {
    return nss == NamespaceString::makeClusterParametersNSS(nss.dbName().tenantId());
}

}  // namespace

void ClusterServerParameterOpObserver::onInserts(OperationContext* opCtx,
                                                 const CollectionPtr& coll,
                                                 std::vector<InsertStatement>::const_iterator first,
                                                 std::vector<InsertStatement>::const_iterator last,
                                                 bool fromMigrate) {
    if (!isConfigNamespace(coll->ns())) {
        return;
    }

    for (auto it = first; it != last; ++it) {
        ClusterServerParameterInitializer::get(opCtx)->updateParameter(
            opCtx, it->doc, kOplog, coll->ns().dbName().tenantId());
    }
}

void ClusterServerParameterOpObserver::onUpdate(OperationContext* opCtx,
                                                const OplogUpdateEntryArgs& args) {
    auto updatedDoc = args.updateArgs->updatedDoc;
    if (!isConfigNamespace(args.coll->ns()) || args.updateArgs->update.isEmpty()) {
        return;
    }

    ClusterServerParameterInitializer::get(opCtx)->updateParameter(
        opCtx, updatedDoc, kOplog, args.coll->ns().dbName().tenantId());
}

void ClusterServerParameterOpObserver::aboutToDelete(OperationContext* opCtx,
                                                     const CollectionPtr& coll,
                                                     const BSONObj& doc) {
    std::string docBeingDeleted;

    if (isConfigNamespace(coll->ns())) {
        // Store the tenantId associated with the doc to be deleted.
        tenantIdToDelete(opCtx) = coll->ns().dbName().tenantId();
        auto elem = doc[kIdField];
        if (elem.type() == String) {
            docBeingDeleted = elem.str();
        } else {
            // This delete makes no sense,
            // but it's safe to ignore since the insert/update
            // would not have resulted in an in-memory update anyway.
            LOGV2_DEBUG(6226304,
                        3,
                        "Deleting a cluster-wide server parameter with non-string name",
                        "name"_attr = elem);
        }
    }

    // Stash the name of the config doc being deleted (if any)
    // in an opCtx decoration for use in the onDelete() hook below
    // since OpLogDeleteEntryArgs isn't guaranteed to have the deleted doc.
    aboutToDeleteDoc(opCtx) = std::move(docBeingDeleted);
}

void ClusterServerParameterOpObserver::onDelete(OperationContext* opCtx,
                                                const CollectionPtr& coll,
                                                StmtId stmtId,
                                                const OplogDeleteEntryArgs& args) {
    const auto& docName = aboutToDeleteDoc(opCtx);
    if (!docName.empty()) {
        ClusterServerParameterInitializer::get(opCtx)->clearParameter(
            opCtx, docName, tenantIdToDelete(opCtx));
    }
}

void ClusterServerParameterOpObserver::onDropDatabase(OperationContext* opCtx,
                                                      const DatabaseName& dbName) {
    if (dbName.db() == NamespaceString::kConfigDb) {
        // Entire config DB deleted, reset to default state.
        ClusterServerParameterInitializer::get(opCtx)->clearAllTenantParameters(opCtx,
                                                                                dbName.tenantId());
    }
}

repl::OpTime ClusterServerParameterOpObserver::onDropCollection(
    OperationContext* opCtx,
    const NamespaceString& collectionName,
    const UUID& uuid,
    std::uint64_t numRecords,
    CollectionDropType dropType) {
    if (isConfigNamespace(collectionName)) {
        // Entire collection deleted, reset to default state.
        ClusterServerParameterInitializer::get(opCtx)->clearAllTenantParameters(
            opCtx, collectionName.dbName().tenantId());
    }

    return {};
}

void ClusterServerParameterOpObserver::postRenameCollection(
    OperationContext* opCtx,
    const NamespaceString& fromCollection,
    const NamespaceString& toCollection,
    const UUID& uuid,
    const boost::optional<UUID>& dropTargetUUID,
    bool stayTemp) {
    if (isConfigNamespace(fromCollection)) {
        // Same as collection dropped from a config point of view.
        ClusterServerParameterInitializer::get(opCtx)->clearAllTenantParameters(
            opCtx, fromCollection.dbName().tenantId());
    }

    if (isConfigNamespace(toCollection)) {
        // Potentially many documents now set, perform full scan.
        if (dropTargetUUID) {
            // Possibly lost configurations in overwrite.
            ClusterServerParameterInitializer::get(opCtx)->resynchronizeAllTenantParametersFromDisk(
                opCtx, toCollection.dbName().tenantId());
        } else {
            // Collection did not exist prior to rename.
            ClusterServerParameterInitializer::get(opCtx)->initializeAllTenantParametersFromDisk(
                opCtx, toCollection.dbName().tenantId());
        }
    }
}

void ClusterServerParameterOpObserver::onImportCollection(OperationContext* opCtx,
                                                          const UUID& importUUID,
                                                          const NamespaceString& nss,
                                                          long long numRecords,
                                                          long long dataSize,
                                                          const BSONObj& catalogEntry,
                                                          const BSONObj& storageMetadata,
                                                          bool isDryRun) {
    if (!isDryRun && (numRecords > 0) && isConfigNamespace(nss)) {
        // Something was imported, do a full collection scan to sync up.
        // No need to apply rollback rules since nothing will have been deleted.
        ClusterServerParameterInitializer::get(opCtx)->initializeAllTenantParametersFromDisk(
            opCtx, nss.dbName().tenantId());
    }
}

void ClusterServerParameterOpObserver::_onReplicationRollback(OperationContext* opCtx,
                                                              const RollbackObserverInfo& rbInfo) {
    for (const auto& nss : rbInfo.rollbackNamespaces) {
        if (isConfigNamespace(nss)) {
            ClusterServerParameterInitializer::get(opCtx)->resynchronizeAllTenantParametersFromDisk(
                opCtx, nss.dbName().tenantId());
        }
    }
}

}  // namespace mongo
