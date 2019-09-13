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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/validate_state.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_consistency.h"
#include "mongo/db/catalog/validate_adaptor.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangDuringYieldingLocksForValidation);

namespace CollectionValidation {

ValidateState::ValidateState(OperationContext* opCtx,
                             const NamespaceString& nss,
                             bool background,
                             bool fullValidate)
    : _nss(nss), _background(background), _fullValidate(fullValidate) {
    _databaseLock.emplace(opCtx, _nss.db(), MODE_IX);
    _database = _databaseLock->getDb() ? _databaseLock->getDb() : nullptr;

    // Subsequent re-locks will use the UUID when 'background' is true.
    if (_background) {
        _collectionLock.emplace(opCtx, _nss, MODE_IX);
    } else {
        _collectionLock.emplace(opCtx, _nss, MODE_X);
    }
    _collection = _database ? _database->getCollection(opCtx, _nss) : nullptr;

    if (!_collection) {
        if (_database && ViewCatalog::get(_database)->lookup(opCtx, _nss.ns())) {
            uasserted(ErrorCodes::CommandNotSupportedOnView, "Cannot validate a view");
        }

        uasserted(ErrorCodes::NamespaceNotFound,
                  str::stream() << "Collection '" << _nss << "' does not exist to validate.");
    }

    _uuid = _collection->uuid();
}

void ValidateState::yieldLocks(OperationContext* opCtx) {
    invariant(_background);

    // Save all the cursors.
    for (const auto& indexCursor : _indexCursors) {
        indexCursor.second->save();
    }

    _traverseRecordStoreCursor->save();
    _seekRecordStoreCursor->save();

    // Drop and reacquire the locks.
    _relockDatabaseAndCollection(opCtx);

    // Check if any of the indexes we were validating were dropped. Indexes created while
    // yielding will be ignored.
    for (const auto& index : _indexes) {
        uassert(ErrorCodes::Interrupted,
                str::stream()
                    << "Interrupted due to: index being validated was dropped from collection: "
                    << _nss << " (" << *_uuid << "), index: " << index->descriptor()->indexName(),
                !index->isDropped());
    }

    // Restore all the cursors.
    for (const auto& indexCursor : _indexCursors) {
        indexCursor.second->restore();
    }

    uassert(
        ErrorCodes::Interrupted,
        str::stream() << "Interrupted due to: cursor cannot be restored after yield on collection: "
                      << _nss.db() << " (" << *_uuid << ")",
        _traverseRecordStoreCursor->restore());

    uassert(
        ErrorCodes::Interrupted,
        str::stream() << "Interrupted due to: cursor cannot be restored after yield on collection: "
                      << _nss.db() << " (" << *_uuid << ")",
        _seekRecordStoreCursor->restore());
};

void ValidateState::initializeCursors(OperationContext* opCtx) {
    invariant(!_traverseRecordStoreCursor && !_seekRecordStoreCursor && _indexCursors.size() == 0 &&
              _indexes.size() == 0);

    // We want to share the same data throttle instance across all the cursors used during this
    // validation. Validations started on other collections will not share the same data
    // throttle instance.
    if (!_background) {
        _dataThrottle.turnThrottlingOff();
    }

    const IndexCatalog* indexCatalog = _collection->getIndexCatalog();
    const std::unique_ptr<IndexCatalog::IndexIterator> it =
        indexCatalog->getIndexIterator(opCtx, false);
    while (it->more()) {
        const IndexCatalogEntry* entry = it->next();
        const IndexDescriptor* desc = entry->descriptor();
        _indexes.push_back(indexCatalog->getEntryShared(desc));
        _indexCursors.emplace(desc->indexName(),
                              std::make_unique<SortedDataInterfaceThrottleCursor>(
                                  opCtx, entry->accessMethod(), &_dataThrottle));
    }

    _traverseRecordStoreCursor = std::make_unique<SeekableRecordThrottleCursor>(
        opCtx, _collection->getRecordStore(), &_dataThrottle);
    _seekRecordStoreCursor = std::make_unique<SeekableRecordThrottleCursor>(
        opCtx, _collection->getRecordStore(), &_dataThrottle);

    // Because SeekableRecordCursors don't have a method to reset to the start, we save and then
    // use a seek to the first RecordId to reset the cursor (and reuse it) as needed. When
    // iterating through a Record Store cursor, we initialize the loop (and obtain the first
    // Record) with a seek to the first Record (using firstRecordId). Subsequent loop iterations
    // use cursor->next() to get subsequent Records. However, if the Record Store is empty,
    // there is no first record. In this case, we set the first Record Id to an invalid RecordId
    // (RecordId()), which will halt iteration at the initialization step.
    const boost::optional<Record> record = _traverseRecordStoreCursor->next(opCtx);
    _firstRecordId = record ? record->id : RecordId();
}

void ValidateState::_relockDatabaseAndCollection(OperationContext* opCtx) {
    invariant(_background);

    _collectionLock.reset();
    _databaseLock.reset();

    if (MONGO_unlikely(hangDuringYieldingLocksForValidation.shouldFail())) {
        log() << "Hanging on fail point 'hangDuringYieldingLocksForValidation'";
        hangDuringYieldingLocksForValidation.pauseWhileSet();
    }

    std::string dbErrMsg = str::stream()
        << "Interrupted due to: database drop: " << _nss.db()
        << " while validating collection: " << _nss << " (" << *_uuid << ")";

    _databaseLock.emplace(opCtx, _nss.db(), MODE_IX);
    _database = DatabaseHolder::get(opCtx)->getDb(opCtx, _nss.db());
    uassert(ErrorCodes::Interrupted, dbErrMsg, _database);
    uassert(ErrorCodes::Interrupted, dbErrMsg, !_database->isDropPending(opCtx));

    std::string collErrMsg = str::stream() << "Interrupted due to: collection drop: " << _nss
                                           << " (" << *_uuid << ") while validating the collection";

    try {
        NamespaceStringOrUUID nssOrUUID(std::string(_nss.db()), *_uuid);
        _collectionLock.emplace(opCtx, nssOrUUID, MODE_IX);
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        uasserted(ErrorCodes::Interrupted, collErrMsg);
    }

    _collection = CollectionCatalog::get(opCtx).lookupCollectionByUUID(*_uuid);
    uassert(ErrorCodes::Interrupted, collErrMsg, _collection);

    // The namespace of the collection can be changed during a same database collection rename.
    _nss = _collection->ns();
}

}  // namespace CollectionValidation

}  // namespace mongo
