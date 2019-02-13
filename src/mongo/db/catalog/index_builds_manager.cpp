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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/index_builds_manager.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace {

/**
 * Returns basic info on index builders.
 */
std::string toSummary(const std::map<UUID, std::shared_ptr<MultiIndexBlock>>& builders) {
    str::stream ss;
    ss << "Number of builders: " << builders.size() << ": [";
    bool first = true;
    for (const auto& pair : builders) {
        if (!first) {
            ss << ", ";
        }
        ss << pair.first;
        first = false;
    }
    ss << "]";
    return ss;
}

}  // namespace

IndexBuildsManager::~IndexBuildsManager() {
    invariant(_builders.empty(),
              str::stream() << "Index builds still active: " << toSummary(_builders));
}

Status IndexBuildsManager::setUpIndexBuild(OperationContext* opCtx,
                                           Collection* collection,
                                           const std::vector<BSONObj>& specs,
                                           const UUID& buildUUID,
                                           OnInitFn onInit) {
    _registerIndexBuild(opCtx, collection, buildUUID);

    const auto& nss = collection->ns();
    invariant(opCtx->lockState()->isCollectionLockedForMode(nss.ns(), MODE_X),
              str::stream() << "Unable to set up index build " << buildUUID << ": collection "
                            << nss.ns()
                            << " is not locked in exclusive mode.");

    auto builder = _getBuilder(buildUUID);

    auto initResult = writeConflictRetry(
        opCtx, "IndexBuildsManager::setUpIndexBuild", nss.ns(), [opCtx, builder, &onInit, &specs] {
            return builder->init(specs, onInit);
        });

    if (!initResult.isOK()) {
        return initResult.getStatus();
    }

    log() << "Index build initialized: " << buildUUID << ": " << nss << " (" << *collection->uuid()
          << " ): indexes: " << initResult.getValue().size();

    return Status::OK();
}

StatusWith<IndexBuildRecoveryState> IndexBuildsManager::recoverIndexBuild(
    const NamespaceString& nss, const UUID& buildUUID, std::vector<std::string> indexNames) {

    // TODO: Not yet implemented.

    return IndexBuildRecoveryState::Building;
}

Status IndexBuildsManager::startBuildingIndex(const UUID& buildUUID) {
    auto builder = _getBuilder(buildUUID);

    return builder->insertAllDocumentsInCollection();
}

StatusWith<std::pair<long long, long long>> IndexBuildsManager::startBuildingIndexForRecovery(
    OperationContext* opCtx, NamespaceString ns, const UUID& buildUUID) {
    auto builder = _getBuilder(buildUUID);

    auto const storageEngine = opCtx->getServiceContext()->getStorageEngine();
    auto dbCatalogEntry = storageEngine->getDatabaseCatalogEntry(opCtx, ns.db());
    auto rs = dbCatalogEntry->getRecordStore(ns.ns());

    // Iterate all records in the collection. Delete them if they aren't valid BSON. Index them
    // if they are.
    long long numRecords = 0;
    long long dataSize = 0;

    auto cursor = rs->getCursor(opCtx);
    auto record = cursor->next();
    while (record) {
        opCtx->checkForInterrupt();
        // Cursor is left one past the end of the batch inside writeConflictRetry
        auto beginBatchId = record->id;
        Status status = writeConflictRetry(opCtx, "repairDatabase", ns.ns(), [&] {
            // In the case of WCE in a partial batch, we need to go back to the beginning
            if (!record || (beginBatchId != record->id)) {
                record = cursor->seekExact(beginBatchId);
            }
            WriteUnitOfWork wunit(opCtx);
            for (int i = 0; record && i < internalInsertMaxBatchSize.load(); i++) {
                RecordId id = record->id;
                RecordData& data = record->data;
                // Use the latest BSON validation version. We retain decimal data when repairing
                // database even if decimal is disabled.
                auto validStatus = validateBSON(data.data(), data.size(), BSONVersion::kLatest);
                if (!validStatus.isOK()) {
                    warning() << "Invalid BSON detected at " << id << ": " << redact(validStatus)
                              << ". Deleting.";
                    rs->deleteRecord(opCtx, id);
                } else {
                    numRecords++;
                    dataSize += data.size();
                    auto insertStatus = builder->insert(data.releaseToBson(), id);
                    if (!insertStatus.isOK()) {
                        return insertStatus;
                    }
                }
                record = cursor->next();
            }
            cursor->save();  // Can't fail per API definition
            // When this exits via success or WCE, we need to restore the cursor
            ON_BLOCK_EXIT([opCtx, ns, &cursor]() {
                // restore CAN throw WCE per API
                writeConflictRetry(
                    opCtx, "retryRestoreCursor", ns.ns(), [&cursor] { cursor->restore(); });
            });
            wunit.commit();
            return Status::OK();
        });
        if (!status.isOK()) {
            return status;
        }
    }

    Status status = builder->dumpInsertsFromBulk();
    if (!status.isOK()) {
        return status;
    }
    return std::make_pair(numRecords, dataSize);
}

Status IndexBuildsManager::drainBackgroundWrites(const UUID& buildUUID) {
    auto builder = _getBuilder(buildUUID);

    return builder->drainBackgroundWrites();
}

Status IndexBuildsManager::finishBuildingPhase(const UUID& buildUUID) {
    auto multiIndexBlockPtr = _getBuilder(buildUUID);
    // TODO: verify that the index builder is in the expected state.

    // TODO: Not yet implemented.

    return Status::OK();
}

Status IndexBuildsManager::checkIndexConstraintViolations(const UUID& buildUUID) {
    auto builder = _getBuilder(buildUUID);

    return builder->checkConstraints();
}

Status IndexBuildsManager::commitIndexBuild(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            const UUID& buildUUID,
                                            MultiIndexBlock::OnCreateEachFn onCreateEachFn,
                                            MultiIndexBlock::OnCommitFn onCommitFn) {
    auto builder = _getBuilder(buildUUID);

    return writeConflictRetry(opCtx,
                              "IndexBuildsManager::commitIndexBuild",
                              nss.ns(),
                              [builder, opCtx, &onCreateEachFn, &onCommitFn] {
                                  WriteUnitOfWork wunit(opCtx);
                                  auto status = builder->commit(onCreateEachFn, onCommitFn);
                                  if (!status.isOK()) {
                                      return status;
                                  }

                                  wunit.commit();
                                  return Status::OK();
                              });
}

bool IndexBuildsManager::abortIndexBuild(const UUID& buildUUID, const std::string& reason) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto builderIt = _builders.find(buildUUID);
    if (builderIt == _builders.end()) {
        return false;
    }
    builderIt->second->abort(reason);
    return true;
}

bool IndexBuildsManager::interruptIndexBuild(const UUID& buildUUID, const std::string& reason) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto builderIt = _builders.find(buildUUID);
    if (builderIt == _builders.end()) {
        return false;
    }

    // TODO: Not yet implemented.
    return true;
}

void IndexBuildsManager::tearDownIndexBuild(const UUID& buildUUID) {
    // TODO verify that the index builder is in a finished state before allowing its destruction.
    _unregisterIndexBuild(buildUUID);
}

bool IndexBuildsManager::isBackgroundBuilding(const UUID& buildUUID) {
    auto builder = _getBuilder(buildUUID);
    return builder->isBackgroundBuilding();
}

void IndexBuildsManager::initializeIndexesWithoutCleanupForRecovery(
    OperationContext* opCtx, Collection* collection, const std::vector<BSONObj>& indexSpecs) {
    // Sanity check to ensure we're in recovery mode.
    invariant(opCtx->lockState()->isW());
    invariant(indexSpecs.size() > 0);

    MultiIndexBlock indexer(opCtx, collection);
    WriteUnitOfWork wuow(opCtx);
    invariant(indexer.init(indexSpecs, MultiIndexBlock::kNoopOnInitFn).isOK());
    wuow.commit();
}

void IndexBuildsManager::verifyNoIndexBuilds_forTestOnly() {
    invariant(_builders.empty());
}

void IndexBuildsManager::_registerIndexBuild(OperationContext* opCtx,
                                             Collection* collection,
                                             UUID buildUUID) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    std::shared_ptr<MultiIndexBlock> mib = std::make_shared<MultiIndexBlock>(opCtx, collection);
    invariant(_builders.insert(std::make_pair(buildUUID, mib)).second);
}

void IndexBuildsManager::_unregisterIndexBuild(const UUID& buildUUID) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto builderIt = _builders.find(buildUUID);
    invariant(builderIt != _builders.end());
    _builders.erase(builderIt);
}

std::shared_ptr<MultiIndexBlock> IndexBuildsManager::_getBuilder(const UUID& buildUUID) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    auto builderIt = _builders.find(buildUUID);
    invariant(builderIt != _builders.end());
    return builderIt->second;
}

}  // namespace mongo
