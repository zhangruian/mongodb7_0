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


#include "mongo/db/index/columns_access_method.h"

#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/bulk_builder_common.h"
#include "mongo/db/index/column_cell.h"
#include "mongo/db/index/column_key_generator.h"
#include "mongo/db/index/column_store_sorter.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/logv2/log.h"
#include "mongo/util/progress_meter.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex


namespace mongo {
namespace {
inline void inc(int64_t* counter) {
    if (counter)
        ++*counter;
};
}  // namespace

ColumnStoreAccessMethod::ColumnStoreAccessMethod(IndexCatalogEntry* ice,
                                                 std::unique_ptr<ColumnStore> store)
    : _store(std::move(store)),
      _indexCatalogEntry(ice),
      _descriptor(ice->descriptor()),
      _keyGen(_descriptor->keyPattern(), _descriptor->pathProjection()) {
    // Normalize the 'columnstoreProjection' index option to facilitate its comparison as part of
    // index signature.
    if (!_descriptor->pathProjection().isEmpty()) {
        auto* projExec = getColumnstoreProjection()->exec();
        ice->descriptor()->_setNormalizedPathProjection(
            projExec->serializeTransformation(boost::none).toBson());
    }
}

class ColumnStoreAccessMethod::BulkBuilder final
    : public BulkBuilderCommon<ColumnStoreAccessMethod::BulkBuilder> {
public:
    BulkBuilder(ColumnStoreAccessMethod* index, size_t maxMemoryUsageBytes, StringData dbName);

    BulkBuilder(ColumnStoreAccessMethod* index,
                size_t maxMemoryUsageBytes,
                const IndexStateInfo& stateInfo,
                StringData dbName);

    //
    // Generic APIs
    //

    Status insert(OperationContext* opCtx,
                  const CollectionPtr& collection,
                  SharedBufferFragmentBuilder& pooledBuilder,
                  const BSONObj& obj,
                  const RecordId& rid,
                  const InsertDeleteOptions& options,
                  const std::function<void()>& saveCursorBeforeWrite,
                  const std::function<void()>& restoreCursorAfterWrite) final;

    const MultikeyPaths& getMultikeyPaths() const final;

    bool isMultikey() const final;

    int64_t getKeysInserted() const;

    IndexStateInfo persistDataForShutdown() final;
    std::unique_ptr<ColumnStoreSorter::Iterator> finalizeSort();

    std::unique_ptr<ColumnStore::BulkBuilder> setUpBulkInserter(OperationContext* opCtx,
                                                                bool dupsAllowed);
    void debugEnsureSorted(const std::pair<ColumnStoreSorter::Key, ColumnStoreSorter::Value>& data);

    bool duplicateCheck(OperationContext* opCtx,
                        const std::pair<ColumnStoreSorter::Key, ColumnStoreSorter::Value>& data,
                        bool dupsAllowed,
                        const RecordIdHandlerFn& onDuplicateRecord);

    void insertKey(std::unique_ptr<ColumnStore::BulkBuilder>& inserter,
                   const std::pair<ColumnStoreSorter::Key, ColumnStoreSorter::Value>& data);

    Status keyCommitted(const KeyHandlerFn& onDuplicateKeyInserted,
                        const std::pair<ColumnStoreSorter::Key, ColumnStoreSorter::Value>& data,
                        bool isDup);

private:
    ColumnStoreAccessMethod* const _columnsAccess;

    ColumnStoreSorter _sorter;
    BufBuilder _cellBuilder;

    boost::optional<std::pair<PathValue, RowId>> _previousPathAndRowId;
};

ColumnStoreAccessMethod::BulkBuilder::BulkBuilder(ColumnStoreAccessMethod* index,
                                                  size_t maxMemoryUsageBytes,
                                                  StringData dbName)
    : BulkBuilderCommon(0,
                        "Index Build: inserting keys from external sorter into columnstore index",
                        index->_descriptor->indexName()),
      _columnsAccess(index),
      _sorter(maxMemoryUsageBytes, dbName, bulkBuilderFileStats(), bulkBuilderTracker()) {
    countNewBuildInStats();
}

ColumnStoreAccessMethod::BulkBuilder::BulkBuilder(ColumnStoreAccessMethod* index,
                                                  size_t maxMemoryUsageBytes,
                                                  const IndexStateInfo& stateInfo,
                                                  StringData dbName)
    : BulkBuilderCommon(stateInfo.getNumKeys().value_or(0),
                        "Index Build: inserting keys from external sorter into columnstore index",
                        index->_descriptor->indexName()),
      _columnsAccess(index),
      _sorter(maxMemoryUsageBytes,
              dbName,
              bulkBuilderFileStats(),
              stateInfo.getFileName()->toString(),
              *stateInfo.getRanges(),
              bulkBuilderTracker()) {
    countResumedBuildInStats();
}

Status ColumnStoreAccessMethod::BulkBuilder::insert(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    SharedBufferFragmentBuilder& pooledBuilder,
    const BSONObj& obj,
    const RecordId& rid,
    const InsertDeleteOptions& options,
    const std::function<void()>& saveCursorBeforeWrite,
    const std::function<void()>& restoreCursorAfterWrite) {
    _columnsAccess->_keyGen.visitCellsForInsert(
        obj, [&](PathView path, const column_keygen::UnencodedCellView& cell) {
            _cellBuilder.reset();
            column_keygen::writeEncodedCell(cell, &_cellBuilder);
            tassert(6762300, "RecordID cannot be a string for column store indexes", !rid.isStr());
            _sorter.add(path, rid.getLong(), CellView(_cellBuilder.buf(), _cellBuilder.len()));

            ++_keysInserted;
        });

    return Status::OK();
}

// The "multikey" property does not apply to columnstore indexes, because the array key does not
// represent a field in a document and
const MultikeyPaths& ColumnStoreAccessMethod::BulkBuilder::getMultikeyPaths() const {
    const static MultikeyPaths empty;
    return empty;
}

bool ColumnStoreAccessMethod::BulkBuilder::isMultikey() const {
    return false;
}

int64_t ColumnStoreAccessMethod::BulkBuilder::getKeysInserted() const {
    return _keysInserted;
}

IndexStateInfo ColumnStoreAccessMethod::BulkBuilder::persistDataForShutdown() {
    auto state = _sorter.persistDataForShutdown();

    IndexStateInfo stateInfo;
    stateInfo.setFileName(StringData(state.fileName));
    stateInfo.setNumKeys(_keysInserted);
    stateInfo.setRanges(std::move(state.ranges));

    return stateInfo;
}

std::unique_ptr<ColumnStoreSorter::Iterator> ColumnStoreAccessMethod::BulkBuilder::finalizeSort() {
    return std::unique_ptr<ColumnStoreSorter::Iterator>(_sorter.done());
}

std::unique_ptr<ColumnStore::BulkBuilder> ColumnStoreAccessMethod::BulkBuilder::setUpBulkInserter(
    OperationContext* opCtx, bool dupsAllowed) {
    _ns = _columnsAccess->_indexCatalogEntry->getNSSFromCatalog(opCtx);
    return _columnsAccess->_store->makeBulkBuilder(opCtx);
}

void ColumnStoreAccessMethod::BulkBuilder::debugEnsureSorted(
    const std::pair<ColumnStoreSorter::Key, ColumnStoreSorter::Value>& data) {
    // In debug mode only, assert that keys are retrieved from the sorter in strictly
    // increasing order.
    const auto& key = data.first;
    if (_previousPathAndRowId &&
        !(ColumnStoreSorter::Key{_previousPathAndRowId->first, _previousPathAndRowId->second} <
          key)) {
        LOGV2_FATAL_NOTRACE(6548100,
                            "Out-of-order result from sorter for column store bulk loader",
                            "prevPathName"_attr = _previousPathAndRowId->first,
                            "prevRecordId"_attr = _previousPathAndRowId->second,
                            "nextPathName"_attr = key.path,
                            "nextRecordId"_attr = key.rowId,
                            "index"_attr = _indexName);
    }
    // It is not safe to safe to directly store the 'key' object, because it includes a
    // PathView, which may be invalid the next time we read it.
    _previousPathAndRowId.emplace(key.path, key.rowId);
}

bool ColumnStoreAccessMethod::BulkBuilder::duplicateCheck(
    OperationContext* opCtx,
    const std::pair<ColumnStoreSorter::Key, ColumnStoreSorter::Value>& data,
    bool dupsAllowed,
    const RecordIdHandlerFn& onDuplicateRecord) {
    // no duplicates in a columnstore index
    return false;
}

void ColumnStoreAccessMethod::BulkBuilder::insertKey(
    std::unique_ptr<ColumnStore::BulkBuilder>& inserter,
    const std::pair<ColumnStoreSorter::Key, ColumnStoreSorter::Value>& data) {

    auto& [columnStoreKey, columnStoreValue] = data;
    inserter->addCell(columnStoreKey.path, columnStoreKey.rowId, columnStoreValue.cell);
}

Status ColumnStoreAccessMethod::BulkBuilder::keyCommitted(
    const KeyHandlerFn& onDuplicateKeyInserted,
    const std::pair<ColumnStoreSorter::Key, ColumnStoreSorter::Value>& data,
    bool isDup) {
    // nothing to do for columnstore indexes
    return Status::OK();
}

Status ColumnStoreAccessMethod::insert(OperationContext* opCtx,
                                       SharedBufferFragmentBuilder& pooledBufferBuilder,
                                       const CollectionPtr& coll,
                                       const std::vector<BsonRecord>& bsonRecords,
                                       const InsertDeleteOptions& options,
                                       int64_t* keysInsertedOut) {
    try {
        PooledFragmentBuilder buf(pooledBufferBuilder);
        auto cursor = _store->newWriteCursor(opCtx);
        _keyGen.visitCellsForInsert(
            bsonRecords,
            [&](StringData path,
                const BsonRecord& rec,
                const column_keygen::UnencodedCellView& cell) {
                if (!rec.ts.isNull()) {
                    uassertStatusOK(opCtx->recoveryUnit()->setTimestamp(rec.ts));
                }

                buf.reset();
                column_keygen::writeEncodedCell(cell, &buf);
                invariant(!rec.id.isStr());
                cursor->insert(path, rec.id.getLong(), CellView{buf.buf(), size_t(buf.len())});

                inc(keysInsertedOut);
            });
        return Status::OK();
    } catch (const AssertionException& ex) {
        return ex.toStatus();
    }
}

void ColumnStoreAccessMethod::remove(OperationContext* opCtx,
                                     SharedBufferFragmentBuilder& pooledBufferBuilder,
                                     const CollectionPtr& coll,
                                     const BSONObj& obj,
                                     const RecordId& rid,
                                     bool logIfError,
                                     const InsertDeleteOptions& options,
                                     int64_t* keysDeletedOut,
                                     CheckRecordId checkRecordId) {
    auto cursor = _store->newWriteCursor(opCtx);
    _keyGen.visitPathsForDelete(obj, [&](StringData path) {
        tassert(6762301, "RecordID cannot be a string for column store indexes", !rid.isStr());
        cursor->remove(path, rid.getLong());
        inc(keysDeletedOut);
    });
}

Status ColumnStoreAccessMethod::update(OperationContext* opCtx,
                                       SharedBufferFragmentBuilder& pooledBufferBuilder,
                                       const BSONObj& oldDoc,
                                       const BSONObj& newDoc,
                                       const RecordId& rid,
                                       const CollectionPtr& coll,
                                       const InsertDeleteOptions& options,
                                       int64_t* keysInsertedOut,
                                       int64_t* keysDeletedOut) {
    PooledFragmentBuilder buf(pooledBufferBuilder);
    auto cursor = _store->newWriteCursor(opCtx);
    _keyGen.visitDiffForUpdate(
        oldDoc,
        newDoc,
        [&](column_keygen::ColumnKeyGenerator::DiffAction diffAction,
            StringData path,
            const column_keygen::UnencodedCellView* cell) {
            if (diffAction == column_keygen::ColumnKeyGenerator::DiffAction::kDelete) {
                tassert(
                    6762302, "RecordID cannot be a string for column store indexes", !rid.isStr());
                cursor->remove(path, rid.getLong());
                inc(keysDeletedOut);
                return;
            }

            // kInsert and kUpdate are handled almost identically. If we switch to using
            // `overwrite=true` cursors in WT, we could consider making them the same, although that
            // might disadvantage other implementations of the storage engine API.
            buf.reset();
            column_keygen::writeEncodedCell(*cell, &buf);

            const auto method = diffAction == column_keygen::ColumnKeyGenerator::DiffAction::kInsert
                ? &ColumnStore::WriteCursor::insert
                : &ColumnStore::WriteCursor::update;
            tassert(6762303, "RecordID cannot be a string for column store indexes", !rid.isStr());
            (cursor.get()->*method)(path, rid.getLong(), CellView{buf.buf(), size_t(buf.len())});

            inc(keysInsertedOut);
        });
    return Status::OK();
}

Status ColumnStoreAccessMethod::initializeAsEmpty(OperationContext* opCtx) {
    return Status::OK();
}

void ColumnStoreAccessMethod::validate(OperationContext* opCtx,
                                       int64_t* numKeys,
                                       IndexValidateResults* fullResults) const {
    _store->fullValidate(opCtx, numKeys, fullResults);
}

bool ColumnStoreAccessMethod::appendCustomStats(OperationContext* opCtx,
                                                BSONObjBuilder* result,
                                                double scale) const {
    return _store->appendCustomStats(opCtx, result, scale);
}

long long ColumnStoreAccessMethod::getSpaceUsedBytes(OperationContext* opCtx) const {
    return _store->getSpaceUsedBytes(opCtx);
}

long long ColumnStoreAccessMethod::getFreeStorageBytes(OperationContext* opCtx) const {
    return _store->getFreeStorageBytes(opCtx);
}

Status ColumnStoreAccessMethod::compact(OperationContext* opCtx) {
    return _store->compact(opCtx);
}


std::unique_ptr<IndexAccessMethod::BulkBuilder> ColumnStoreAccessMethod::initiateBulk(
    size_t maxMemoryUsageBytes,
    const boost::optional<IndexStateInfo>& stateInfo,
    StringData dbName) {
    return stateInfo ? std::make_unique<BulkBuilder>(this, maxMemoryUsageBytes, *stateInfo, dbName)
                     : std::make_unique<BulkBuilder>(this, maxMemoryUsageBytes, dbName);
}

std::shared_ptr<Ident> ColumnStoreAccessMethod::getSharedIdent() const {
    return _store->getSharedIdent();
}

void ColumnStoreAccessMethod::setIdent(std::shared_ptr<Ident> ident) {
    _store->setIdent(std::move(ident));
}

}  // namespace mongo
