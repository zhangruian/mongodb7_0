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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/multi_index_block.h"

#include <ostream>

#include "mongo/base/error_codes.h"
#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/db/audit.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/multi_index_block_gen.h"
#include "mongo/db/catalog/uncommitted_collections.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/tenant_migration_committed_info.h"
#include "mongo/db/repl/tenant_migration_conflict_info.h"
#include "mongo/db/storage/execution_context.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/progress_meter.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/uuid.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangAfterSettingUpIndexBuild);
MONGO_FAIL_POINT_DEFINE(hangAfterSettingUpIndexBuildUnlocked);
MONGO_FAIL_POINT_DEFINE(hangAfterStartingIndexBuild);
MONGO_FAIL_POINT_DEFINE(hangAfterStartingIndexBuildUnlocked);
MONGO_FAIL_POINT_DEFINE(hangIndexBuildDuringCollectionScanPhaseBeforeInsertion);
MONGO_FAIL_POINT_DEFINE(hangIndexBuildDuringCollectionScanPhaseAfterInsertion);
MONGO_FAIL_POINT_DEFINE(leaveIndexBuildUnfinishedForShutdown);

MultiIndexBlock::~MultiIndexBlock() {
    invariant(_buildIsCleanedUp);
}

MultiIndexBlock::OnCleanUpFn MultiIndexBlock::kNoopOnCleanUpFn = []() {};

void MultiIndexBlock::abortIndexBuild(OperationContext* opCtx,
                                      CollectionWriter& collection,
                                      OnCleanUpFn onCleanUp) noexcept {
    if (_collectionUUID) {
        // init() was previously called with a collection pointer, so ensure that the same
        // collection is being provided for clean up and the interface in not being abused.
        invariant(_collectionUUID.get() == collection->uuid());
    }

    if (_buildIsCleanedUp) {
        return;
    }

    auto nss = collection->ns();
    UncommittedCollections::get(opCtx).invariantHasExclusiveAccessToCollection(opCtx, nss);

    while (true) {
        try {
            WriteUnitOfWork wunit(opCtx);
            // This cleans up all index builds. Because that may need to write, it is done inside of
            // a WUOW. Nothing inside this block can fail, and it is made fatal if it does.
            for (size_t i = 0; i < _indexes.size(); i++) {
                _indexes[i].block->fail(opCtx, collection.getWritableCollection());
                _indexes[i].block->finalizeTemporaryTables(
                    opCtx, TemporaryRecordStore::FinalizationAction::kDelete);
            }

            onCleanUp();

            wunit.commit();
            _buildIsCleanedUp = true;
            return;
        } catch (const WriteConflictException&) {
            continue;
        } catch (const DBException& e) {
            if (e.toStatus() == ErrorCodes::ExceededMemoryLimit)
                continue;
            LOGV2_ERROR(20393,
                        "Caught exception while cleaning up partially built indexes: {e}",
                        "Caught exception while cleaning up partially built indexes",
                        "error"_attr = redact(e));
        } catch (const std::exception& e) {
            LOGV2_ERROR(20394,
                        "Caught exception while cleaning up partially built indexes: {e_what}",
                        "Caught exception while cleaning up partially built indexes",
                        "error"_attr = e.what());
        } catch (...) {
            LOGV2_ERROR(20395,
                        "Caught unknown exception while cleaning up partially built indexes");
        }
        fassertFailed(18644);
    }
}

void MultiIndexBlock::ignoreUniqueConstraint() {
    _ignoreUnique = true;
}

MultiIndexBlock::OnInitFn MultiIndexBlock::kNoopOnInitFn =
    [](std::vector<BSONObj>& specs) -> Status { return Status::OK(); };

MultiIndexBlock::OnInitFn MultiIndexBlock::makeTimestampedIndexOnInitFn(OperationContext* opCtx,
                                                                        const CollectionPtr& coll) {
    return [opCtx, ns = coll->ns()](std::vector<BSONObj>& specs) -> Status {
        opCtx->getServiceContext()->getOpObserver()->onStartIndexBuildSinglePhase(opCtx, ns);
        return Status::OK();
    };
}

StatusWith<std::vector<BSONObj>> MultiIndexBlock::init(OperationContext* opCtx,
                                                       CollectionWriter& collection,
                                                       const BSONObj& spec,
                                                       OnInitFn onInit) {
    const auto indexes = std::vector<BSONObj>(1, spec);
    return init(opCtx, collection, indexes, onInit, boost::none);
}

StatusWith<std::vector<BSONObj>> MultiIndexBlock::init(
    OperationContext* opCtx,
    CollectionWriter& collection,
    const std::vector<BSONObj>& indexSpecs,
    OnInitFn onInit,
    const boost::optional<ResumeIndexInfo>& resumeInfo) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(collection->ns(), MODE_X),
              str::stream() << "Collection " << collection->ns() << " with UUID "
                            << collection->uuid() << " is holding the incorrect lock");
    _collectionUUID = collection->uuid();

    _buildIsCleanedUp = false;

    invariant(_indexes.empty());

    if (resumeInfo) {
        _phase = resumeInfo->getPhase();
    }

    // Guarantees that exceptions cannot be returned from index builder initialization except for
    // WriteConflictExceptions, which should be dealt with by the caller.
    try {
        WriteUnitOfWork wunit(opCtx);

        // On rollback in init(), cleans up _indexes so that ~MultiIndexBlock doesn't try to clean
        // up _indexes manually (since the changes were already rolled back). Due to this, it is
        // thus legal to call init() again after it fails.
        opCtx->recoveryUnit()->onRollback([this, opCtx]() {
            for (auto& index : _indexes) {
                index.block->finalizeTemporaryTables(
                    opCtx, TemporaryRecordStore::FinalizationAction::kDelete);
            }
            _indexes.clear();
            _buildIsCleanedUp = true;
        });

        for (const auto& info : indexSpecs) {
            if (info["background"].isBoolean() && !info["background"].Bool()) {
                LOGV2(20383,
                      "Ignoring obsolete { background: false } index build option because all "
                      "indexes are built in the background with the hybrid method");
            }
        }

        std::vector<BSONObj> indexInfoObjs;
        indexInfoObjs.reserve(indexSpecs.size());
        std::size_t eachIndexBuildMaxMemoryUsageBytes = 0;
        if (!indexSpecs.empty()) {
            eachIndexBuildMaxMemoryUsageBytes =
                static_cast<std::size_t>(maxIndexBuildMemoryUsageMegabytes.load()) * 1024 * 1024 /
                indexSpecs.size();
        }
        _eachIndexBuildMaxMemoryUsageBytes = eachIndexBuildMaxMemoryUsageBytes;

        // Initializing individual index build blocks below performs un-timestamped writes to the
        // durable catalog. It's possible for the onInit function to set multiple timestamps
        // depending on the index build codepath taken. Once to persist the index build entry in the
        // 'config.system.indexBuilds' collection and another time to log the operation using
        // onStartIndexBuild(). It's imperative that the durable catalog writes are timestamped at
        // the same time as onStartIndexBuild() is to avoid rollback issues.
        Status status = onInit(indexInfoObjs);
        if (!status.isOK()) {
            return status;
        }

        for (size_t i = 0; i < indexSpecs.size(); i++) {
            BSONObj info = indexSpecs[i];
            StatusWith<BSONObj> statusWithInfo =
                collection->getIndexCatalog()->prepareSpecForCreate(opCtx, info, resumeInfo);
            Status status = statusWithInfo.getStatus();
            if (!status.isOK()) {
                // If we were given two identical indexes to build, we will run into an error trying
                // to set up the same index a second time in this for-loop. This is the only way to
                // encounter this error because callers filter out ready/in-progress indexes and
                // start the build while holding a lock throughout.
                if (status == ErrorCodes::IndexBuildAlreadyInProgress) {
                    invariant(indexSpecs.size() > 1,
                              str::stream()
                                  << "Collection: " << collection->ns() << " (" << _collectionUUID
                                  << "), Index spec: " << indexSpecs.front());
                    return {
                        ErrorCodes::OperationFailed,
                        "Cannot build two identical indexes. Try again without duplicate indexes."};
                }
                return status;
            }
            info = statusWithInfo.getValue();
            indexInfoObjs.push_back(info);

            boost::optional<IndexStateInfo> stateInfo;
            auto& index = _indexes.emplace_back();
            index.block =
                std::make_unique<IndexBuildBlock>(collection->ns(), info, _method, _buildUUID);
            if (resumeInfo) {
                auto resumeInfoIndexes = resumeInfo->getIndexes();
                // Find the resume information that corresponds to this spec.
                auto stateInfoIt = std::find_if(resumeInfoIndexes.begin(),
                                                resumeInfoIndexes.end(),
                                                [&info](const IndexStateInfo& indexInfo) {
                                                    return info.woCompare(indexInfo.getSpec()) == 0;
                                                });
                uassert(ErrorCodes::NoSuchKey,
                        str::stream() << "Unable to locate resume information for " << info
                                      << " due to inconsistent resume information for index build "
                                      << _buildUUID << " on namespace " << collection->ns() << "("
                                      << _collectionUUID << ")",
                        stateInfoIt != resumeInfoIndexes.end());

                stateInfo = *stateInfoIt;
                status = index.block->initForResume(
                    opCtx, collection.getWritableCollection(), *stateInfo, resumeInfo->getPhase());
            } else {
                status = index.block->init(opCtx, collection.getWritableCollection());
            }
            if (!status.isOK())
                return status;

            auto indexCatalogEntry =
                index.block->getEntry(opCtx, collection.getWritableCollection());
            index.real = indexCatalogEntry->accessMethod();
            status = index.real->initializeAsEmpty(opCtx);
            if (!status.isOK())
                return status;

            index.bulk = index.real->initiateBulk(eachIndexBuildMaxMemoryUsageBytes, stateInfo);

            const IndexDescriptor* descriptor = indexCatalogEntry->descriptor();

            collection->getIndexCatalog()->prepareInsertDeleteOptions(
                opCtx, collection->ns(), descriptor, &index.options);

            // Index builds always relax constraints and check for violations at commit-time.
            index.options.getKeysMode = IndexAccessMethod::GetKeysMode::kRelaxConstraints;
            index.options.dupsAllowed = true;
            index.options.fromIndexBuilder = true;

            LOGV2(20384,
                  "Index build: starting",
                  logAttrs(collection->ns()),
                  "buildUUID"_attr = _buildUUID,
                  "properties"_attr = *descriptor,
                  "method"_attr = _method,
                  "maxTemporaryMemoryUsageMB"_attr =
                      eachIndexBuildMaxMemoryUsageBytes / 1024 / 1024);

            index.filterExpression = indexCatalogEntry->getFilterExpression();

            if (!resumeInfo) {
                // TODO SERVER-14888 Suppress this in cases we don't want to audit.
                audit::logCreateIndex(
                    opCtx->getClient(), &info, descriptor->indexName(), collection->ns().ns());
            }
        }

        opCtx->recoveryUnit()->onCommit([ns = collection->ns(), this](auto commitTs) {
            if (!_buildUUID) {
                return;
            }

            LOGV2(20346,
                  "Index build: initialized",
                  "buildUUID"_attr = _buildUUID,
                  logAttrs(ns),
                  "collectionUUID"_attr = _collectionUUID,
                  "initializationTimestamp"_attr = commitTs);
        });

        wunit.commit();
        return indexInfoObjs;
    } catch (const WriteConflictException&) {
        // Avoid converting WCE to Status.
        throw;
    } catch (const TenantMigrationConflictException&) {
        // Avoid converting TenantMigrationConflictException to Status.
        throw;
    } catch (const TenantMigrationCommittedException&) {
        // Avoid converting TenantMigrationCommittedException to Status.
        throw;
    } catch (...) {
        return exceptionToStatus().withContext(
            str::stream() << "Caught exception during index builder (" << _buildUUID
                          << ") initialization on namespace" << collection->ns() << " ("
                          << _collectionUUID << "). " << indexSpecs.size()
                          << " index specs provided. First index spec: "
                          << (indexSpecs.empty() ? BSONObj() : indexSpecs[0]));
    }
}

Status MultiIndexBlock::insertAllDocumentsInCollection(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    boost::optional<RecordId> resumeAfterRecordId) {
    invariant(!_buildIsCleanedUp);
    invariant(opCtx->lockState()->isNoop() || !opCtx->lockState()->inAWriteUnitOfWork());

    // UUIDs are not guaranteed during startup because the check happens after indexes are rebuilt.
    if (_collectionUUID) {
        invariant(_collectionUUID.get() == collection->uuid());
    }

    // Refrain from persisting any multikey updates as a result from building the index. Instead,
    // accumulate them in the `MultikeyPathTracker` and do the write as part of the update that
    // commits the index.
    auto stopTracker = makeGuard(
        [this, opCtx] { MultikeyPathTracker::get(opCtx).stopTrackingMultikeyPathInfo(); });
    if (MultikeyPathTracker::get(opCtx).isTrackingMultikeyPathInfo()) {
        stopTracker.dismiss();
    }
    MultikeyPathTracker::get(opCtx).startTrackingMultikeyPathInfo();

    const IndexCatalogEntry* refIdx = nullptr;
    if (gUseReferenceIndexForIndexBuild && _indexes.size() == 1 &&
        (refIdx = _findSmallestReferenceIdx(opCtx, collection))) {
        LOGV2(3620203,
              "Index Build: using existing index instead of scanning collection",
              "refIdx"_attr = refIdx->descriptor()->indexName(),
              "childIdx"_attr =
                  _indexes[0].block->getEntry(opCtx, collection)->descriptor()->indexName());
        uassertStatusOK(_scanReferenceIdxInsertAndCommit(opCtx, collection, refIdx));
        return Status::OK();
    }

    const char* curopMessage = "Index Build: scanning collection";
    const auto numRecords = collection->numRecords(opCtx);
    ProgressMeterHolder progress;
    {
        stdx::unique_lock<Client> lk(*opCtx->getClient());
        progress.set(CurOp::get(opCtx)->setProgress_inlock(curopMessage, numRecords));
    }

    if (MONGO_unlikely(hangAfterSettingUpIndexBuild.shouldFail())) {
        // Hang the build after the curOP info is set up.
        LOGV2(20387, "Hanging index build due to failpoint 'hangAfterSettingUpIndexBuild'");
        hangAfterSettingUpIndexBuild.pauseWhileSet();
    }

    if (MONGO_unlikely(hangAfterSettingUpIndexBuildUnlocked.shouldFail())) {
        uassert(4585200, "failpoint may not be set on foreground indexes", isBackgroundBuilding());

        // Unlock before hanging so replication recognizes we've completed.
        collection.yield();
        Locker::LockSnapshot lockInfo;
        invariant(opCtx->lockState()->saveLockStateAndUnlock(&lockInfo));

        LOGV2(4585201,
              "Hanging index build with no locks due to "
              "'hangAfterSettingUpIndexBuildUnlocked' failpoint");
        hangAfterSettingUpIndexBuildUnlocked.pauseWhileSet();

        opCtx->lockState()->restoreLockState(opCtx, lockInfo);
        opCtx->recoveryUnit()->abandonSnapshot();
        collection.restore();
    }

    Timer t;

    unsigned long long n = 0;

    PlanYieldPolicy::YieldPolicy yieldPolicy;
    if (isBackgroundBuilding()) {
        yieldPolicy = PlanYieldPolicy::YieldPolicy::YIELD_AUTO;
    } else {
        yieldPolicy = PlanYieldPolicy::YieldPolicy::WRITE_CONFLICT_RETRY_ONLY;
    }
    auto exec = collection->makePlanExecutor(
        opCtx, collection, yieldPolicy, Collection::ScanDirection::kForward, resumeAfterRecordId);

    // Hint to the storage engine that this collection scan should not keep data in the cache.
    bool readOnce = useReadOnceCursorsForIndexBuilds.load();
    opCtx->recoveryUnit()->setReadOnce(readOnce);

    try {
        // The phase will be kCollectionScan when resuming an index build from the collection scan
        // phase.
        invariant(_phase == IndexBuildPhaseEnum::kInitialized ||
                      _phase == IndexBuildPhaseEnum::kCollectionScan,
                  IndexBuildPhase_serializer(_phase).toString());
        _phase = IndexBuildPhaseEnum::kCollectionScan;

        BSONObj objToIndex;
        RecordId loc;
        PlanExecutor::ExecState state;
        while (PlanExecutor::ADVANCED == (state = exec->getNext(&objToIndex, &loc)) ||
               MONGO_unlikely(hangAfterStartingIndexBuild.shouldFail())) {
            opCtx->checkForInterrupt();

            if (PlanExecutor::ADVANCED != state) {
                continue;
            }

            progress->setTotalWhileRunning(collection->numRecords(opCtx));

            uassertStatusOK(
                _failPointHangDuringBuild(opCtx,
                                          &hangIndexBuildDuringCollectionScanPhaseBeforeInsertion,
                                          "before",
                                          objToIndex,
                                          n));

            // The external sorter is not part of the storage engine and therefore does not need a
            // WriteUnitOfWork to write keys.
            uassertStatusOK(_insert(opCtx, objToIndex, loc));

            _failPointHangDuringBuild(opCtx,
                                      &hangIndexBuildDuringCollectionScanPhaseAfterInsertion,
                                      "after",
                                      objToIndex,
                                      n)
                .ignore();

            // Go to the next document.
            progress->hit();
            n++;
        }
    } catch (DBException& ex) {
        if (ex.isA<ErrorCategory::Interruption>() || ex.isA<ErrorCategory::ShutdownError>() ||
            ErrorCodes::IndexBuildAborted == ex.code()) {
            // If the collection scan is stopped because due to an interrupt or shutdown event, we
            // leave the internal state intact to ensure we have the correct information for
            // resuming this index build during startup and rollback.
        } else {
            // Restore pre-collection scan state.
            _phase = IndexBuildPhaseEnum::kInitialized;
        }

        auto readSource = opCtx->recoveryUnit()->getTimestampReadSource();
        LOGV2(4984704,
              "Index build: collection scan stopped",
              "buildUUID"_attr = _buildUUID,
              "totalRecords"_attr = n,
              "duration"_attr = duration_cast<Milliseconds>(Seconds(t.seconds())),
              "phase"_attr = IndexBuildPhase_serializer(_phase),
              "collectionScanPosition"_attr = _lastRecordIdInserted,
              "readSource"_attr = RecoveryUnit::toString(readSource),
              "error"_attr = ex);
        ex.addContext(str::stream()
                      << "collection scan stopped. totalRecords: " << n
                      << "; durationMillis: " << duration_cast<Milliseconds>(Seconds(t.seconds()))
                      << "; phase: " << IndexBuildPhase_serializer(_phase)
                      << "; collectionScanPosition: " << _lastRecordIdInserted
                      << "; readSource: " << RecoveryUnit::toString(readSource));
        return ex.toStatus();
    }

    if (MONGO_unlikely(leaveIndexBuildUnfinishedForShutdown.shouldFail())) {
        LOGV2(20389,
              "Index build interrupted due to 'leaveIndexBuildUnfinishedForShutdown' failpoint. "
              "Mimicking shutdown error code");
        return Status(
            ErrorCodes::InterruptedAtShutdown,
            "background index build interrupted due to failpoint. returning a shutdown error.");
    }

    if (MONGO_unlikely(hangAfterStartingIndexBuildUnlocked.shouldFail())) {
        // Unlock before hanging so replication recognizes we've completed.
        collection.yield();
        Locker::LockSnapshot lockInfo;
        invariant(opCtx->lockState()->saveLockStateAndUnlock(&lockInfo));

        LOGV2(20390,
              "Hanging index build with no locks due to "
              "'hangAfterStartingIndexBuildUnlocked' failpoint");
        hangAfterStartingIndexBuildUnlocked.pauseWhileSet();

        if (isBackgroundBuilding()) {
            opCtx->lockState()->restoreLockState(opCtx, lockInfo);
            opCtx->recoveryUnit()->abandonSnapshot();
        } else {
            invariant(!"the hangAfterStartingIndexBuildUnlocked failpoint can't be turned off for foreground index builds");
        }
        collection.restore();
    }

    progress->finished();

    LOGV2(20391,
          "Index build: collection scan done. scanned {n} total records in {t_seconds} seconds",
          "Index build: collection scan done",
          "buildUUID"_attr = _buildUUID,
          "totalRecords"_attr = n,
          "readSource"_attr =
              RecoveryUnit::toString(opCtx->recoveryUnit()->getTimestampReadSource()),
          "duration"_attr = duration_cast<Milliseconds>(Seconds(t.seconds())));

    Status ret = dumpInsertsFromBulk(opCtx, collection);
    if (!ret.isOK())
        return ret;

    return Status::OK();
}

Status MultiIndexBlock::insertSingleDocumentForInitialSyncOrRecovery(OperationContext* opCtx,
                                                                     const BSONObj& doc,
                                                                     const RecordId& loc) {
    return _insert(opCtx, doc, loc);
}

Status MultiIndexBlock::_insert(OperationContext* opCtx, const BSONObj& doc, const RecordId& loc) {
    invariant(!_buildIsCleanedUp);
    for (size_t i = 0; i < _indexes.size(); i++) {
        if (_indexes[i].filterExpression && !_indexes[i].filterExpression->matchesBSON(doc)) {
            continue;
        }

        Status idxStatus = Status::OK();

        // When calling insert, BulkBuilderImpl's Sorter performs file I/O that may result in an
        // exception.
        try {
            idxStatus = _indexes[i].bulk->insert(opCtx, doc, loc, _indexes[i].options);
        } catch (...) {
            return exceptionToStatus();
        }

        if (!idxStatus.isOK())
            return idxStatus;
    }

    _lastRecordIdInserted = loc;

    return Status::OK();
}

Status MultiIndexBlock::dumpInsertsFromBulk(OperationContext* opCtx,
                                            const CollectionPtr& collection) {
    return dumpInsertsFromBulk(opCtx, collection, nullptr);
}

Status MultiIndexBlock::dumpInsertsFromBulk(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const IndexAccessMethod::RecordIdHandlerFn& onDuplicateRecord) {
    invariant(!_buildIsCleanedUp);
    invariant(opCtx->lockState()->isNoop() || !opCtx->lockState()->inAWriteUnitOfWork());

    // Initial sync adds documents to the sorter using
    // insertSingleDocumentForInitialSyncOrRecovery() instead of delegating to
    // insertDocumentsInCollection() to scan and insert the contents of the collection.
    // Therefore, it is possible for the phase of this MultiIndexBlock to be kInitialized
    // rather than kCollection when this function is called. The phase will be kBulkLoad when
    // resuming an index build from the bulk load phase.
    invariant(_phase == IndexBuildPhaseEnum::kInitialized ||
                  _phase == IndexBuildPhaseEnum::kCollectionScan ||
                  _phase == IndexBuildPhaseEnum::kBulkLoad,
              IndexBuildPhase_serializer(_phase).toString());
    _phase = IndexBuildPhaseEnum::kBulkLoad;

    for (size_t i = 0; i < _indexes.size(); i++) {
        // When onDuplicateRecord is passed, 'dupsAllowed' should be passed to reflect whether or
        // not the index is unique.
        bool dupsAllowed = (onDuplicateRecord)
            ? !_indexes[i].block->getEntry(opCtx, collection)->descriptor()->unique()
            : _indexes[i].options.dupsAllowed;
        const IndexCatalogEntry* entry = _indexes[i].block->getEntry(opCtx, collection);
        LOGV2_DEBUG(20392,
                    1,
                    "Index build: inserting from external sorter into index",
                    "index"_attr = entry->descriptor()->indexName(),
                    "buildUUID"_attr = _buildUUID);

        // SERVER-41918 This call to commitBulk() results in file I/O that may result in an
        // exception.
        try {
            Status status = _indexes[i].real->commitBulk(
                opCtx,
                _indexes[i].bulk.get(),
                dupsAllowed,
                [=](const KeyString::Value& duplicateKey) {
                    // Do not record duplicates when explicitly ignored. This may be the case on
                    // secondaries.
                    return writeConflictRetry(
                        opCtx, "recordingDuplicateKey", entry->getNSSFromCatalog(opCtx).ns(), [&] {
                            if (dupsAllowed && !onDuplicateRecord && !_ignoreUnique &&
                                entry->indexBuildInterceptor()) {
                                WriteUnitOfWork wuow(opCtx);
                                Status status = entry->indexBuildInterceptor()->recordDuplicateKey(
                                    opCtx, duplicateKey);
                                if (!status.isOK()) {
                                    return status;
                                }
                                wuow.commit();
                            }
                            return Status::OK();
                        });
                },
                onDuplicateRecord);

            if (!status.isOK()) {
                return status;
            }
        } catch (...) {
            return exceptionToStatus();
        }
    }

    return Status::OK();
}

Status MultiIndexBlock::drainBackgroundWrites(
    OperationContext* opCtx,
    RecoveryUnit::ReadSource readSource,
    IndexBuildInterceptor::DrainYieldPolicy drainYieldPolicy) {
    invariant(!_buildIsCleanedUp);
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());

    // Background writes are drained three times (once without blocking writes and twice blocking
    // writes), so we may either be coming from the bulk load phase or be already in the drain
    // writes phase.
    invariant(_phase == IndexBuildPhaseEnum::kBulkLoad ||
                  _phase == IndexBuildPhaseEnum::kDrainWrites,
              IndexBuildPhase_serializer(_phase).toString());
    _phase = IndexBuildPhaseEnum::kDrainWrites;

    ReadSourceScope readSourceScope(opCtx, readSource);

    const CollectionPtr& coll =
        CollectionCatalog::get(opCtx).lookupCollectionByUUID(opCtx, _collectionUUID.get());

    // Drain side-writes table for each index. This only drains what is visible. Assuming intent
    // locks are held on the user collection, more writes can come in after this drain completes.
    // Callers are responsible for stopping writes by holding an S or X lock while draining before
    // completing the index build.
    for (size_t i = 0; i < _indexes.size(); i++) {
        auto interceptor = _indexes[i].block->getEntry(opCtx, coll)->indexBuildInterceptor();
        if (!interceptor)
            continue;

        // Track duplicates for later constraint checking for all index builds, except when
        // _ignoreUnique is set explicitly.
        auto trackDups = !_ignoreUnique ? IndexBuildInterceptor::TrackDuplicates::kTrack
                                        : IndexBuildInterceptor::TrackDuplicates::kNoTrack;
        auto status = interceptor->drainWritesIntoIndex(
            opCtx, coll, _indexes[i].options, trackDups, drainYieldPolicy);
        if (!status.isOK()) {
            return status;
        }
    }
    return Status::OK();
}

Status MultiIndexBlock::retrySkippedRecords(OperationContext* opCtx,
                                            const CollectionPtr& collection) {
    invariant(!_buildIsCleanedUp);
    for (auto&& index : _indexes) {
        auto interceptor = index.block->getEntry(opCtx, collection)->indexBuildInterceptor();
        if (!interceptor)
            continue;

        auto status = interceptor->retrySkippedRecords(opCtx, collection);
        if (!status.isOK()) {
            return status;
        }
    }
    return Status::OK();
}

Status MultiIndexBlock::checkConstraints(OperationContext* opCtx, const CollectionPtr& collection) {
    invariant(!_buildIsCleanedUp);

    // For each index that may be unique, check that no recorded duplicates still exist. This can
    // only check what is visible on the index. Callers are responsible for ensuring all writes to
    // the collection are visible.
    for (size_t i = 0; i < _indexes.size(); i++) {
        auto interceptor = _indexes[i].block->getEntry(opCtx, collection)->indexBuildInterceptor();
        if (!interceptor)
            continue;

        auto status = interceptor->checkDuplicateKeyConstraints(opCtx);
        if (!status.isOK()) {
            return status;
        }
    }
    return Status::OK();
}

MultiIndexBlock::OnCreateEachFn MultiIndexBlock::kNoopOnCreateEachFn = [](const BSONObj& spec) {};
MultiIndexBlock::OnCommitFn MultiIndexBlock::kNoopOnCommitFn = []() {};

Status MultiIndexBlock::commit(OperationContext* opCtx,
                               Collection* collection,
                               OnCreateEachFn onCreateEach,
                               OnCommitFn onCommit) {
    invariant(!_buildIsCleanedUp);
    invariant(opCtx->lockState()->isCollectionLockedForMode(collection->ns(), MODE_X),
              str::stream() << "Collection " << collection->ns() << " with UUID "
                            << collection->uuid() << " is holding the incorrect lock");

    // UUIDs are not guaranteed during startup because the check happens after indexes are rebuilt.
    if (_collectionUUID) {
        invariant(_collectionUUID.get() == collection->uuid());
    }

    // Do not interfere with writing multikey information when committing index builds.
    auto restartTracker = makeGuard(
        [this, opCtx] { MultikeyPathTracker::get(opCtx).startTrackingMultikeyPathInfo(); });
    if (!MultikeyPathTracker::get(opCtx).isTrackingMultikeyPathInfo()) {
        restartTracker.dismiss();
    }
    MultikeyPathTracker::get(opCtx).stopTrackingMultikeyPathInfo();

    for (size_t i = 0; i < _indexes.size(); i++) {
        onCreateEach(_indexes[i].block->getSpec());

        // Do this before calling success(), which unsets the interceptor pointer on the index
        // catalog entry. The interceptor will write multikey metadata keys into the index during
        // IndexBuildInterceptor::sideWrite, so we only need to pass the cached MultikeyPaths into
        // IndexCatalogEntry::setMultikey here.
        auto indexCatalogEntry = _indexes[i].block->getEntry(opCtx, collection);
        auto interceptor = indexCatalogEntry->indexBuildInterceptor();
        if (interceptor) {
            auto multikeyPaths = interceptor->getMultikeyPaths();
            if (multikeyPaths) {
                indexCatalogEntry->setMultikey(opCtx, collection, {}, multikeyPaths.get());
            }
        }

        _indexes[i].block->success(opCtx, collection);

        // The bulk builder will track multikey information itself, and will write cached multikey
        // metadata keys into the index just before committing. We therefore only need to pass the
        // MultikeyPaths into IndexCatalogEntry::setMultikey here.
        const auto& bulkBuilder = _indexes[i].bulk;
        if (bulkBuilder->isMultikey()) {
            indexCatalogEntry->setMultikey(opCtx, collection, {}, bulkBuilder->getMultikeyPaths());
        }

        // The commit() function can be called multiple times on write conflict errors. Dropping the
        // temp tables cannot be rolled back, so do it only after the WUOW commits.
        opCtx->recoveryUnit()->onCommit([opCtx, i, this](auto commitTs) {
            _indexes[i].block->finalizeTemporaryTables(
                opCtx, TemporaryRecordStore::FinalizationAction::kDelete);
        });
    }

    onCommit();

    CollectionQueryInfo::get(collection).clearQueryCache(opCtx, collection);
    opCtx->recoveryUnit()->onCommit(
        [this](boost::optional<Timestamp> commitTime) { _buildIsCleanedUp = true; });

    return Status::OK();
}

bool MultiIndexBlock::isBackgroundBuilding() const {
    return _method == IndexBuildMethod::kHybrid;
}

void MultiIndexBlock::setIndexBuildMethod(IndexBuildMethod indexBuildMethod) {
    _method = indexBuildMethod;
}

void MultiIndexBlock::abortWithoutCleanup(OperationContext* opCtx,
                                          const CollectionPtr& collection,
                                          bool isResumable) {
    invariant(!_buildIsCleanedUp);
    UninterruptibleLockGuard noInterrupt(opCtx->lockState());
    // Lock if it's not already locked, to ensure storage engine cannot be destructed out from
    // underneath us.
    boost::optional<Lock::GlobalLock> lk;
    if (!opCtx->lockState()->isWriteLocked()) {
        lk.emplace(opCtx, MODE_IX);
    }

    auto action = TemporaryRecordStore::FinalizationAction::kDelete;

    if (isResumable) {
        invariant(_buildUUID);
        invariant(_method == IndexBuildMethod::kHybrid);

        // Index builds do not yield locks during the bulk load phase so it is not possible for
        // rollback to interrupt an index build during this phase.
        if (!ErrorCodes::isShutdownError(opCtx->checkForInterruptNoAssert())) {
            invariant(IndexBuildPhaseEnum::kBulkLoad != _phase, str::stream() << *_buildUUID);
        }

        _writeStateToDisk(opCtx, collection);
        action = TemporaryRecordStore::FinalizationAction::kKeep;
    }

    for (auto& index : _indexes) {
        index.block->finalizeTemporaryTables(opCtx, action);
    }

    _buildIsCleanedUp = true;
}

void MultiIndexBlock::_writeStateToDisk(OperationContext* opCtx,
                                        const CollectionPtr& collection) const {
    auto obj = _constructStateObject(opCtx, collection);
    auto rs = opCtx->getServiceContext()
                  ->getStorageEngine()
                  ->makeTemporaryRecordStoreForResumableIndexBuild(opCtx);

    WriteUnitOfWork wuow(opCtx);

    auto status = rs->rs()->insertRecord(opCtx, obj.objdata(), obj.objsize(), Timestamp());
    if (!status.isOK()) {
        LOGV2_ERROR(4841501,
                    "Failed to write resumable index build state to disk",
                    "buildUUID"_attr = *_buildUUID,
                    "details"_attr = obj,
                    "error"_attr = status.getStatus());
        dassert(status,
                str::stream() << "Failed to write resumable index build state to disk. UUID: "
                              << *_buildUUID);

        rs->finalizeTemporaryTable(opCtx, TemporaryRecordStore::FinalizationAction::kDelete);
        return;
    }

    wuow.commit();

    LOGV2(4841502,
          "Index build: wrote resumable state to disk",
          "buildUUID"_attr = *_buildUUID,
          "details"_attr = obj);

    rs->finalizeTemporaryTable(opCtx, TemporaryRecordStore::FinalizationAction::kKeep);
}

BSONObj MultiIndexBlock::_constructStateObject(OperationContext* opCtx,
                                               const CollectionPtr& collection) const {
    BSONObjBuilder builder;
    _buildUUID->appendToBuilder(&builder, "_id");
    builder.append("phase", IndexBuildPhase_serializer(_phase));

    if (_collectionUUID) {
        _collectionUUID->appendToBuilder(&builder, "collectionUUID");
    }

    // We can be interrupted by shutdown before inserting the first document from the collection
    // scan, in which case there is no _lastRecordIdInserted.
    if (_phase == IndexBuildPhaseEnum::kCollectionScan && _lastRecordIdInserted)
        builder.append("collectionScanPosition", _lastRecordIdInserted->repr());

    BSONArrayBuilder indexesArray(builder.subarrayStart("indexes"));
    for (const auto& index : _indexes) {
        BSONObjBuilder indexInfo(indexesArray.subobjStart());

        if (_phase != IndexBuildPhaseEnum::kDrainWrites) {
            // Persist the data to disk so that we see all of the data that has been inserted into
            // the Sorter.
            auto state = index.bulk->persistDataForShutdown();

            indexInfo.append("fileName", state.fileName);
            indexInfo.append("numKeys", index.bulk->getKeysInserted());

            BSONArrayBuilder ranges(indexInfo.subarrayStart("ranges"));
            for (const auto& rangeInfo : state.ranges) {
                BSONObjBuilder range(ranges.subobjStart());
                range.append("startOffset", rangeInfo.getStartOffset());
                range.append("endOffset", rangeInfo.getEndOffset());
                range.append("checksum", rangeInfo.getChecksum());
            }
        }

        auto indexBuildInterceptor =
            index.block->getEntry(opCtx, collection)->indexBuildInterceptor();
        indexInfo.append("sideWritesTable", indexBuildInterceptor->getSideWritesTableIdent());

        if (auto duplicateKeyTrackerTableIdent =
                indexBuildInterceptor->getDuplicateKeyTrackerTableIdent())
            indexInfo.append("duplicateKeyTrackerTable", *duplicateKeyTrackerTableIdent);

        if (auto skippedRecordTrackerTableIdent =
                indexBuildInterceptor->getSkippedRecordTracker()->getTableIdent())
            indexInfo.append("skippedRecordTrackerTable", *skippedRecordTrackerTableIdent);

        indexInfo.append("spec", index.block->getSpec());
        indexInfo.append("isMultikey", index.bulk->isMultikey());

        BSONArrayBuilder multikeyPaths(indexInfo.subarrayStart("multikeyPaths"));
        for (const auto& multikeyPath : index.bulk->getMultikeyPaths()) {
            BSONObjBuilder multikeyPathObj(multikeyPaths.subobjStart());
            BSONArrayBuilder multikeyComponents(
                multikeyPathObj.subarrayStart("multikeyComponents"));

            for (const auto& multikeyComponent : multikeyPath) {
                multikeyComponents.append(multikeyComponent);
            }
        }
    }
    indexesArray.done();

    return builder.obj();
}

Status MultiIndexBlock::_failPointHangDuringBuild(OperationContext* opCtx,
                                                  FailPoint* fp,
                                                  StringData where,
                                                  const BSONObj& doc,
                                                  unsigned long long iteration) const {
    try {
        fp->executeIf(
            [=, &doc](const BSONObj& data) {
                LOGV2(20386,
                      "Hanging index build during collection scan phase",
                      "where"_attr = where,
                      "doc"_attr = doc,
                      "buildUUID"_attr = _buildUUID);

                fp->pauseWhileSet(opCtx);
            },
            [&doc, iteration, buildUUID = _buildUUID](const BSONObj& data) {
                if (data.hasField("fieldsToMatch")) {
                    auto fieldsToMatch = data.getObjectField("fieldsToMatch");
                    return std::all_of(
                        fieldsToMatch.begin(), fieldsToMatch.end(), [&doc](const auto& elem) {
                            return SimpleBSONElementComparator::kInstance.evaluate(
                                elem == doc[elem.fieldName()]);
                        });
                }

                if (!buildUUID)
                    return false;

                auto buildUUIDs = data.getObjectField("buildUUIDs");
                return iteration ==
                    static_cast<unsigned long long>(data["iteration"].numberLong()) &&
                    std::any_of(buildUUIDs.begin(),
                                buildUUIDs.end(),
                                [buildUUID = *buildUUID](const auto& elem) {
                                    return UUID::parse(elem.String()) == buildUUID;
                                });
            });
    } catch (const ExceptionForCat<ErrorCategory::Interruption>& ex) {
        return ex.toStatus(str::stream() << "Interrupted failpoint " << fp->getName());
    }

    return Status::OK();
}

const IndexCatalogEntry* MultiIndexBlock::_findSmallestReferenceIdx(
    OperationContext* opCtx, const CollectionPtr& collection) const {
    // Find a suitable reference index for the first index we are trying to build. First make
    // sure we're not trying to build a partial, sparse or unique index; the logic to handle these
    // cases is complicated (we'll have to make sure whether a partial reference index covers the
    // partial index we're trying to build, and a unique reference index will not necessarily
    // yield a unique child index, etc.). We do not support multi-key indexes either.
    for (size_t i = 0; i < _indexes.size(); i++) {
        auto entry = _indexes[i].block->getEntry(opCtx, collection);
        auto descriptor = entry->descriptor();
        if (descriptor->isPartial() || descriptor->isSparse() || descriptor->unique())
            return nullptr;
    }

    const IndexCatalogEntry* smallestRefIdx = nullptr;

    int smallestSize = -1;
    auto it = collection->getIndexCatalog()->getIndexIterator(opCtx,
                                                              false /* includeUnfinishedIndexes */);
    while (it->more()) {
        const auto candidateEntry = it->next();
        const auto candidateDescriptor = candidateEntry->descriptor();
        // A partial / sparse reference index may or may not cover the index we're trying to build.
        // More complex logic is required to check if the reference index covers the one we're
        // building, and so as a simplification, we're avoiding using partial / sparse ref indexes.
        // A candidate reference index being unique, however, is not a problem.
        if (candidateEntry->isMultikey() || candidateDescriptor->isPartial() ||
            candidateDescriptor->isSparse())
            continue;

        // Make sure the candidate we are looking at is compatible with all the indexes we are
        // attempting to build:
        bool compatible = true;
        for (size_t i = 0; i < _indexes.size(); i++) {
            auto descriptor = _indexes[i].block->getEntry(opCtx, collection)->descriptor();
            compatible = compatible &&
                descriptor->keyPattern().isPrefixOf(candidateDescriptor->keyPattern(),
                                                    SimpleBSONElementComparator::kInstance);
            if (!compatible)
                break;
        }

        if (compatible) {
            int candidateSize = candidateDescriptor->keyPattern().nFields();
            if (smallestRefIdx && smallestSize <= candidateSize)
                continue;

            smallestSize = candidateSize;
            smallestRefIdx = candidateEntry;
        }
    }

    return smallestRefIdx;
}

Status MultiIndexBlock::_scanReferenceIdxInsertAndCommit(OperationContext* opCtx,
                                                         const CollectionPtr& collection,
                                                         const IndexCatalogEntry* refIdx) {
    auto cursor = refIdx->accessMethod()->newCursor(opCtx);
    KeyString::Value startKeyString = IndexEntryComparison::makeKeyStringFromBSONKeyForSeek(
        kMinBSONKey,
        refIdx->accessMethod()->getSortedDataInterface()->getKeyStringVersion(),
        refIdx->accessMethod()->getSortedDataInterface()->getOrdering(),
        true /* forward */,
        false /* inclusive */);

    auto childIdx = _indexes[0].block->getEntry(opCtx, collection);
    auto childDescriptor = childIdx->descriptor();
    invariant(!childDescriptor->isPartial() && !childDescriptor->isSparse() &&
              !childDescriptor->unique() && _indexes[0].options.dupsAllowed);

    auto bulkLoader = _indexes[0].real->makeBulkBuilder(opCtx, _indexes[0].options.dupsAllowed);

    // We are returning by value. "keys" within the lambda clears itself on destruction, and
    // returning by reference would return a reference pointing to a deleted object.
    auto produceKey = [&](const BSONObj& key, const RecordId& loc) -> KeyString::Value {
        auto& executionCtx = StorageExecutionContext::get(opCtx);
        auto keys = executionCtx.keys();
        auto multikeyMetadataKeys = executionCtx.multikeyMetadataKeys();
        auto multikeyPaths = executionCtx.multikeyPaths();
        auto obj = IndexKeyEntry::rehydrateKey(refIdx->descriptor()->keyPattern(), key);
        childIdx->accessMethod()->getKeys(executionCtx.pooledBufferBuilder(),
                                          obj,
                                          _indexes[0].options.getKeysMode,
                                          IndexAccessMethod::GetKeysContext::kAddingKeys,
                                          keys.get(),
                                          multikeyMetadataKeys.get(),
                                          multikeyPaths.get(),
                                          loc,
                                          IndexAccessMethod::kNoopOnSuppressedErrorFn);

        // Should produce only one key:
        invariant(keys->size() == 1, "Expected one key, got " + std::to_string(keys->size()));
        return *(keys->begin());
    };

    auto dumpSorter = [&]() {
        std::unique_ptr<IndexAccessMethod::BulkBuilder::Sorter::Iterator> it(
            _indexes[0].bulk->done());

        WriteUnitOfWork wuow(opCtx);
        while (it->more()) {
            auto key = it->next().first;
            auto stat = bulkLoader->addKey(key);
            if (!stat.isOK())
                return stat;
        }
        wuow.commit();

        return Status::OK();
    };

    // We "refresh" the sorter (create a new one) to empty it out. We require an empty sorter for
    // every key class we encounter. A key class is a contiguous group of keys that are in order in
    // the reference index, but may be out of order in the child index due to the record ID.
    // For example, consider a reference index { a: 1, b: 1 } with their corresponding keyStrings:
    //
    // RecordID 2: { a: "a", b: "blue" }  -> "a,blue,2"
    // RecordID 1: { a: "a", b: "red" }   -> "a,red,1"
    //
    // Note that in the reference index, the above are in order (sorted by { a: 1, b: 1 }), but the
    // document with a greater record ID appears first.
    //
    // When trying to build a child index { a: 1 }, we produce these corresponding keyStrings:
    // "a,2"
    // "a,1"
    // Though the keyStrings were in order in the reference index, they are not in order when it
    // comes to the child index. As a result, we need to sort each set of keys that differ only in
    // their record IDs. We're calling this set of keys a key class.
    auto refreshSorter = [&]() {
        _indexes[0].bulk =
            _indexes[0].real->initiateBulk(_eachIndexBuildMaxMemoryUsageBytes, boost::none);
    };

    auto addToSorter = [&](const KeyString::Value& keyString) {
        _indexes[0].bulk->addToSorter(keyString);
    };

    auto insertBulkBypassingSorter = [&](const KeyString::Value& keyString) {
        uassertStatusOK(bulkLoader->addKey(keyString));
    };

    auto refIdxEntry = cursor->seek(startKeyString);

    if (!refIdxEntry) {
        LOGV2(3620204,
              "Reference index is empty.",
              "refIdx"_attr = refIdx->descriptor()->indexName());
        _phase = IndexBuildPhaseEnum::kBulkLoad;
        WriteUnitOfWork wuow(opCtx);
        // Allow the commit operation to be interruptable:
        bulkLoader->commit(true);
        wuow.commit();
        return Status::OK();
    }

    KeyString::Value currKS = produceKey(refIdxEntry->key, refIdxEntry->loc);
    KeyString::Value nextKS;
    bool processingKeyClass = false;

    while ((refIdxEntry = cursor->next())) {
        nextKS = produceKey(refIdxEntry->key, refIdxEntry->loc);
        if (currKS.compareWithoutRecordId(nextKS) == 0) {
            addToSorter(currKS);
            processingKeyClass = true;
            currKS = nextKS;
            continue;
        }

        if (processingKeyClass) {
            addToSorter(currKS);
            auto stat = dumpSorter();
            if (!stat.isOK())
                return stat;
            refreshSorter();
            currKS = nextKS;
            processingKeyClass = false;
            continue;
        }

        insertBulkBypassingSorter(currKS);
        currKS = nextKS;
    }

    if (processingKeyClass) {
        addToSorter(currKS);
        auto stat = dumpSorter();
        if (!stat.isOK())
            return stat;
    } else {
        insertBulkBypassingSorter(currKS);
    }

    _phase = IndexBuildPhaseEnum::kBulkLoad;

    WriteUnitOfWork wuow(opCtx);
    // Allow the commit operation to be interruptable:
    bulkLoader->commit(true);
    wuow.commit();

    return Status::OK();
}

}  // namespace mongo
