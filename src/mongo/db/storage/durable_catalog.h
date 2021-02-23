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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/bson_collection_catalog_entry.h"
#include "mongo/db/storage/storage_engine.h"

namespace mongo {

/**
 * An interface to modify the on-disk catalog metadata.
 */
class DurableCatalog {
    DurableCatalog(const DurableCatalog&) = delete;
    DurableCatalog& operator=(const DurableCatalog&) = delete;
    DurableCatalog(DurableCatalog&&) = delete;
    DurableCatalog& operator=(DurableCatalog&&) = delete;

protected:
    DurableCatalog() = default;

public:
    /**
     * `Entry` ties together the common identifiers of a single `_mdb_catalog` document.
     */
    struct Entry {
        Entry() {}
        Entry(RecordId catalogId, std::string ident, NamespaceString nss)
            : catalogId(catalogId), ident(std::move(ident)), nss(std::move(nss)) {}
        RecordId catalogId;
        std::string ident;
        NamespaceString nss;
    };

    virtual ~DurableCatalog() {}

    static DurableCatalog* get(OperationContext* opCtx) {
        return opCtx->getServiceContext()->getStorageEngine()->getCatalog();
    }

    virtual void init(OperationContext* opCtx) = 0;

    virtual std::vector<Entry> getAllCatalogEntries(OperationContext* opCtx) const = 0;

    virtual Entry getEntry(RecordId catalogId) const = 0;

    virtual std::string getIndexIdent(OperationContext* opCtx,
                                      RecordId id,
                                      StringData idxName) const = 0;

    virtual BSONObj getCatalogEntry(OperationContext* opCtx, RecordId catalogId) const = 0;

    virtual BSONCollectionCatalogEntry::MetaData getMetaData(OperationContext* opCtx,
                                                             RecordId id) const = 0;

    /**
     * Updates the catalog entry for the collection 'nss' with the fields specified in 'md'. If
     * 'md.indexes' contains a new index entry, then this method generates a new index ident and
     * adds it to the catalog entry.
     */
    virtual void putMetaData(OperationContext* opCtx,
                             RecordId id,
                             BSONCollectionCatalogEntry::MetaData& md) = 0;

    /**
     * Checks that the metadata for the index exists and matches the given spec.
     */
    virtual Status checkMetaDataForIndex(OperationContext* opCtx,
                                         RecordId catalogId,
                                         const std::string& indexName,
                                         const BSONObj& spec) = 0;

    virtual std::vector<std::string> getAllIdents(OperationContext* opCtx) const = 0;

    virtual bool isUserDataIdent(StringData ident) const = 0;

    virtual bool isInternalIdent(StringData ident) const = 0;

    virtual bool isCollectionIdent(StringData ident) const = 0;

    virtual RecordStore* getRecordStore() = 0;

    /**
     * Create an entry in the catalog for an orphaned collection found in the
     * storage engine. Return the generated ns of the collection.
     * Note that this function does not recreate the _id index on the collection because it does not
     * have access to index catalog.
     */
    virtual StatusWith<std::string> newOrphanedIdent(OperationContext* opCtx,
                                                     std::string ident) = 0;

    virtual std::string getFilesystemPathForDb(const std::string& dbName) const = 0;

    /**
     * Generate an internal ident name.
     */
    virtual std::string newInternalIdent() = 0;

    /**
     * Generate an internal resumable index build ident name.
     */
    virtual std::string newInternalResumableIndexBuildIdent() = 0;

    /**
     * On success, returns the RecordId which identifies the new record store in the durable catalog
     * in addition to ownership of the new RecordStore.
     */
    virtual StatusWith<std::pair<RecordId, std::unique_ptr<RecordStore>>> createCollection(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const CollectionOptions& options,
        bool allocateDefaultSpace) = 0;

    /**
     * Import a collection by inserting the given metadata into the durable catalog and instructing
     * the storage engine to import the corresponding idents. The metadata object should be a valid
     * catalog entry and contain the following fields:
     * "md": A document representing the BSONCollectionCatalogEntry::MetaData of the collection.
     * "idxIdent": A document containing {<index_name>: <index_ident>} pairs for all indexes.
     * "ns": Namespace of the collection being imported.
     * "ident": Ident of the collection file.
     *
     * On success, returns an ImportResult structure containing the RecordId which identifies the
     * new record store in the durable catalog, ownership of the new RecordStore and the UUID of the
     * collection imported.
     *
     * The collection must be locked in MODE_X when calling this function.
     */
    struct ImportResult {
        ImportResult(RecordId catalogId, std::unique_ptr<RecordStore> rs, UUID uuid)
            : catalogId(catalogId), rs(std::move(rs)), uuid(uuid) {}
        RecordId catalogId;
        std::unique_ptr<RecordStore> rs;
        UUID uuid;
    };
    enum class ImportCollectionUUIDOption { kKeepOld, kGenerateNew };
    virtual StatusWith<ImportResult> importCollection(OperationContext* opCtx,
                                                      const NamespaceString& nss,
                                                      const BSONObj& metadata,
                                                      const BSONObj& storageMetadata,
                                                      ImportCollectionUUIDOption uuidOption) = 0;

    virtual Status renameCollection(OperationContext* opCtx,
                                    RecordId catalogId,
                                    const NamespaceString& toNss,
                                    bool stayTemp) = 0;

    /**
     * Deletes the persisted collection catalog entry identified by 'catalogId'.
     *
     * Expects (invariants) that all of the index catalog entries have been removed already via
     * removeIndex.
     */
    virtual Status dropCollection(OperationContext* opCtx, RecordId catalogId) = 0;

    /**
     * Updates size of a capped Collection.
     */
    virtual void updateCappedSize(OperationContext* opCtx, RecordId catalogId, long long size) = 0;

    /**
     * Updates the expireAfterSeconds option on the clustered index. If no expireAfterSeconds value
     * is passed in then TTL deletions will be stopped on the clustered index.
     */
    virtual void updateClusteredIndexTTLSetting(OperationContext* opCtx,
                                                RecordId catalogId,
                                                boost::optional<int64_t> expireAfterSeconds) = 0;

    /*
     * Updates the expireAfterSeconds field of the given index to the value in newExpireSecs.
     * The specified index must already contain an expireAfterSeconds field, and the value in
     * that field and newExpireSecs must both be numeric.
     */
    virtual void updateTTLSetting(OperationContext* opCtx,
                                  RecordId catalogId,
                                  StringData idxName,
                                  long long newExpireSeconds) = 0;

    /*
     * Hide or unhide the given index. A hidden index will not be considered for use by the
     * query planner.
     */
    virtual void updateHiddenSetting(OperationContext* opCtx,
                                     RecordId catalogId,
                                     StringData idxName,
                                     bool hidden) = 0;

    /**
     * Compares the UUID argument to the UUID obtained from the metadata. Returns true if they are
     * equal, false otherwise.
     */
    virtual bool isEqualToMetadataUUID(OperationContext* opCtx,
                                       RecordId catalogId,
                                       const UUID& uuid) = 0;

    /**
     * Updates the 'temp' setting for this collection.
     */
    virtual void setIsTemp(OperationContext* opCtx, RecordId catalogId, bool isTemp) = 0;

    /**
     * Updates whether updates/deletes should store their pre-images in the opLog.
     */
    virtual void setRecordPreImages(OperationContext* opCtx, RecordId catalogId, bool val) = 0;

    /**
     * Updates the validator for this collection.
     *
     * An empty validator removes all validation.
     */
    virtual void updateValidator(OperationContext* opCtx,
                                 RecordId catalogId,
                                 const BSONObj& validator,
                                 boost::optional<ValidationLevelEnum> newLevel,
                                 boost::optional<ValidationActionEnum> newAction) = 0;

    /**
     * Removes the index 'indexName' from the persisted collection catalog entry identified by
     * 'catalogId'.
     */
    virtual void removeIndex(OperationContext* opCtx, RecordId catalogId, StringData indexName) = 0;

    /**
     * Updates the persisted catalog entry for 'ns' with the new index and creates the index on
     * disk.
     *
     * A passed 'buildUUID' implies that the index is part of a two-phase index build.
     */
    virtual Status prepareForIndexBuild(OperationContext* opCtx,
                                        RecordId catalogId,
                                        const IndexDescriptor* spec,
                                        boost::optional<UUID> buildUUID,
                                        bool isBackgroundSecondaryBuild) = 0;

    /**
     * Drops the provided ident and recreates it as empty for use in resuming an index build.
     */
    virtual Status dropAndRecreateIndexIdentForResume(OperationContext* opCtx,
                                                      RecordId catalogId,
                                                      const IndexDescriptor* spec,
                                                      StringData ident) = 0;

    /**
     * Returns a UUID if the index is being built with the two-phase index build procedure.
     */
    virtual boost::optional<UUID> getIndexBuildUUID(OperationContext* opCtx,
                                                    RecordId catalogId,
                                                    StringData indexName) const = 0;

    /**
     * Indicate that an index build is completed and the index is ready to use.
     */
    virtual void indexBuildSuccess(OperationContext* opCtx,
                                   RecordId catalogId,
                                   StringData indexName) = 0;

    /**
     * Returns true if the index identified by 'indexName' is multikey, and returns false otherwise.
     *
     * If the 'multikeyPaths' pointer is non-null, then it must point to an empty vector. If this
     * index type supports tracking path-level multikey information in the catalog, then this
     * function sets 'multikeyPaths' as the path components that cause this index to be multikey.
     *
     * In particular, if this function returns false and the index supports tracking path-level
     * multikey information, then 'multikeyPaths' is initialized as a vector with size equal to the
     * number of elements in the index key pattern of empty sets.
     */
    virtual bool isIndexMultikey(OperationContext* opCtx,
                                 RecordId catalogId,
                                 StringData indexName,
                                 MultikeyPaths* multikeyPaths) const = 0;

    /**
     * Sets the index identified by 'indexName' to be multikey.
     *
     * If 'multikeyPaths' is non-empty, then it must be a vector with size equal to the number of
     * elements in the index key pattern. Additionally, at least one path component of the indexed
     * fields must cause this index to be multikey.
     *
     * This function returns true if the index metadata has changed, and returns false otherwise.
     */
    virtual bool setIndexIsMultikey(OperationContext* opCtx,
                                    RecordId catalogId,
                                    StringData indexName,
                                    const MultikeyPaths& multikeyPaths) = 0;

    /**
     * Sets the index to be multikey with the provided paths. This performs minimal validation of
     * the inputs and is intended to be used internally to "correct" multikey metadata that drifts
     * from the underlying collection data.
     *
     * When isMultikey is false, ignores multikeyPaths and resets the metadata appropriately based
     * on the index descriptor. Otherwise, overwrites the existing multikeyPaths with the ones
     * provided. This only writes multikey paths if the index type supports path-level tracking, and
     * only sets the multikey boolean flag otherwise.
     */
    virtual void forceSetIndexIsMultikey(OperationContext* opCtx,
                                         RecordId catalogId,
                                         const IndexDescriptor* desc,
                                         bool isMultikey,
                                         const MultikeyPaths& multikeyPaths) = 0;

    virtual CollectionOptions getCollectionOptions(OperationContext* opCtx,
                                                   RecordId catalogId) const = 0;

    virtual int getTotalIndexCount(OperationContext* opCtx, RecordId catalogId) const = 0;

    virtual int getCompletedIndexCount(OperationContext* opCtx, RecordId catalogId) const = 0;

    virtual BSONObj getIndexSpec(OperationContext* opCtx,
                                 RecordId catalogId,
                                 StringData indexName) const = 0;

    virtual void getAllIndexes(OperationContext* opCtx,
                               RecordId catalogId,
                               std::vector<std::string>* names) const = 0;

    virtual void getReadyIndexes(OperationContext* opCtx,
                                 RecordId catalogId,
                                 std::vector<std::string>* names) const = 0;

    virtual bool isIndexPresent(OperationContext* opCtx,
                                RecordId catalogId,
                                StringData indexName) const = 0;

    virtual bool isIndexReady(OperationContext* opCtx,
                              RecordId catalogId,
                              StringData indexName) const = 0;

    virtual void setRand_forTest(const std::string& rand) = 0;

    virtual std::string getRand_forTest() const = 0;
};
}  // namespace mongo
