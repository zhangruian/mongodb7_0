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

#include "mongo/db/storage/devnull/devnull_kv_engine.h"

#include <memory>

#include "mongo/db/storage/devnull/ephemeral_catalog_record_store.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/sorted_data_interface.h"

namespace mongo {

class EmptyRecordCursor final : public SeekableRecordCursor {
public:
    boost::optional<Record> next() final {
        return {};
    }
    boost::optional<Record> seekExact(const RecordId& id) final {
        return {};
    }
    boost::optional<Record> seekNear(const RecordId& id) final {
        return {};
    }
    void save() final {}
    bool restore(bool tolerateCappedRepositioning = true) final {
        return true;
    }
    void detachFromOperationContext() final {}
    void reattachToOperationContext(OperationContext* opCtx) final {}
    void setSaveStorageCursorOnDetachFromOperationContext(bool) override {}
};

class DevNullRecordStore : public RecordStore {
public:
    DevNullRecordStore(StringData ns,
                       StringData identName,
                       const CollectionOptions& options,
                       KeyFormat keyFormat)
        : RecordStore(ns, identName), _options(options), _keyFormat(keyFormat) {
        _numInserts = 0;
        _dummy = BSON("_id" << 1);
    }

    virtual const char* name() const {
        return "devnull";
    }

    virtual void setCappedCallback(CappedCallback*) {}

    virtual long long dataSize(OperationContext* opCtx) const {
        return 0;
    }

    virtual long long numRecords(OperationContext* opCtx) const {
        return 0;
    }

    virtual bool isCapped() const {
        return _options.capped;
    }

    virtual KeyFormat keyFormat() const {
        return _keyFormat;
    }

    virtual int64_t storageSize(OperationContext* opCtx,
                                BSONObjBuilder* extraInfo = nullptr,
                                int infoLevel = 0) const {
        return 0;
    }

    virtual bool findRecord(OperationContext* opCtx, const RecordId& loc, RecordData* rd) const {
        return false;
    }

    virtual void deleteRecord(OperationContext* opCtx, const RecordId& dl) {}

    virtual Status insertRecords(OperationContext* opCtx,
                                 std::vector<Record>* inOutRecords,
                                 const std::vector<Timestamp>& timestamps) {
        _numInserts += inOutRecords->size();
        for (auto& record : *inOutRecords) {
            record.id = RecordId(6, 4);
        }
        return Status::OK();
    }

    virtual Status updateRecord(OperationContext* opCtx,
                                const RecordId& oldLocation,
                                const char* data,
                                int len) {
        return Status::OK();
    }

    virtual bool updateWithDamagesSupported() const {
        return false;
    }

    virtual StatusWith<RecordData> updateWithDamages(OperationContext* opCtx,
                                                     const RecordId& loc,
                                                     const RecordData& oldRec,
                                                     const char* damageSource,
                                                     const mutablebson::DamageVector& damages) {
        MONGO_UNREACHABLE;
    }


    std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                    bool forward) const final {
        return std::make_unique<EmptyRecordCursor>();
    }

    virtual Status truncate(OperationContext* opCtx) {
        return Status::OK();
    }

    virtual void cappedTruncateAfter(OperationContext* opCtx, RecordId end, bool inclusive) {}

    virtual void appendNumericCustomStats(OperationContext* opCtx,
                                          BSONObjBuilder* result,
                                          double scale) const {
        result->appendNumber("numInserts", _numInserts);
    }

    virtual void updateStatsAfterRepair(OperationContext* opCtx,
                                        long long numRecords,
                                        long long dataSize) {}

protected:
    void waitForAllEarlierOplogWritesToBeVisibleImpl(OperationContext* opCtx) const override {}

private:
    CollectionOptions _options;
    KeyFormat _keyFormat;
    long long _numInserts;
    BSONObj _dummy;
};

class DevNullSortedDataBuilderInterface : public SortedDataBuilderInterface {
    DevNullSortedDataBuilderInterface(const DevNullSortedDataBuilderInterface&) = delete;
    DevNullSortedDataBuilderInterface& operator=(const DevNullSortedDataBuilderInterface&) = delete;

public:
    DevNullSortedDataBuilderInterface() {}

    virtual Status addKey(const KeyString::Value& keyString) {
        return Status::OK();
    }
};

class DevNullSortedDataInterface : public SortedDataInterface {
public:
    DevNullSortedDataInterface(StringData identName)
        : SortedDataInterface(identName,
                              KeyString::Version::kLatestVersion,
                              Ordering::make(BSONObj()),
                              KeyFormat::Long) {}

    virtual ~DevNullSortedDataInterface() {}

    virtual std::unique_ptr<SortedDataBuilderInterface> makeBulkBuilder(OperationContext* opCtx,
                                                                        bool dupsAllowed) {
        return {};
    }

    virtual StatusWith<bool> insert(OperationContext* opCtx,
                                    const KeyString::Value& keyString,
                                    bool dupsAllowed) {
        return true;
    }

    virtual void unindex(OperationContext* opCtx,
                         const KeyString::Value& keyString,
                         bool dupsAllowed) {}

    virtual Status dupKeyCheck(OperationContext* opCtx, const KeyString::Value& keyString) {
        return Status::OK();
    }

    virtual boost::optional<RecordId> findLoc(OperationContext* opCtx,
                                              const KeyString::Value& keyString) const override {
        return boost::none;
    }

    virtual void fullValidate(OperationContext* opCtx,
                              long long* numKeysOut,
                              IndexValidateResults* fullResults) const {}

    virtual bool appendCustomStats(OperationContext* opCtx,
                                   BSONObjBuilder* output,
                                   double scale) const {
        return false;
    }

    virtual long long getSpaceUsedBytes(OperationContext* opCtx) const {
        return 0;
    }

    virtual long long getFreeStorageBytes(OperationContext* opCtx) const {
        return 0;
    }

    virtual bool isEmpty(OperationContext* opCtx) {
        return true;
    }

    virtual std::unique_ptr<SortedDataInterface::Cursor> newCursor(OperationContext* opCtx,
                                                                   bool isForward) const {
        return {};
    }

    virtual Status initAsEmpty(OperationContext* opCtx) {
        return Status::OK();
    }
};


std::unique_ptr<RecordStore> DevNullKVEngine::getRecordStore(OperationContext* opCtx,
                                                             StringData ns,
                                                             StringData ident,
                                                             const CollectionOptions& options) {
    if (ident == "_mdb_catalog") {
        return std::make_unique<EphemeralForTestRecordStore>(ns, ident, &_catalogInfo);
    }
    return std::make_unique<DevNullRecordStore>(ns, ident, options, KeyFormat::Long);
}

std::unique_ptr<RecordStore> DevNullKVEngine::makeTemporaryRecordStore(OperationContext* opCtx,
                                                                       StringData ident,
                                                                       KeyFormat keyFormat) {
    return std::make_unique<DevNullRecordStore>("" /* ns */, ident, CollectionOptions(), keyFormat);
}

std::unique_ptr<SortedDataInterface> DevNullKVEngine::getSortedDataInterface(
    OperationContext* opCtx,
    const CollectionOptions& collOptions,
    StringData ident,
    const IndexDescriptor* desc) {
    return std::make_unique<DevNullSortedDataInterface>(ident);
}

namespace {

class StreamingCursorImpl : public StorageEngine::StreamingCursor {
public:
    StreamingCursorImpl() = delete;
    StreamingCursorImpl(StorageEngine::BackupOptions options,
                        std::vector<StorageEngine::BackupBlock> backupBlocks)
        : StorageEngine::StreamingCursor(options), _backupBlocks(std::move(backupBlocks)) {
        _exhaustCursor = false;
    };

    ~StreamingCursorImpl() = default;

    BSONObj getMetadataObject(UUID backupId) {
        return BSONObj();
    }

    StatusWith<std::vector<StorageEngine::BackupBlock>> getNextBatch(const std::size_t batchSize) {
        if (_exhaustCursor) {
            std::vector<StorageEngine::BackupBlock> emptyVector;
            return emptyVector;
        }
        _exhaustCursor = true;
        return _backupBlocks;
    }

private:
    std::vector<StorageEngine::BackupBlock> _backupBlocks;
    bool _exhaustCursor;
};

}  // namespace

StatusWith<std::unique_ptr<StorageEngine::StreamingCursor>> DevNullKVEngine::beginNonBlockingBackup(
    OperationContext* opCtx, const StorageEngine::BackupOptions& options) {
    return std::make_unique<StreamingCursorImpl>(options, _mockBackupBlocks);
}

StatusWith<std::vector<std::string>> DevNullKVEngine::extendBackupCursor(OperationContext* opCtx) {
    std::vector<std::string> filesToCopy = {"journal/WiredTigerLog.999"};
    return filesToCopy;
}

}  // namespace mongo
