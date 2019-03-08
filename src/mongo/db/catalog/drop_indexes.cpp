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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/drop_indexes.h"

#include "mongo/db/background.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

// Field name in dropIndexes command for indexes to drop. This field can contain one of:
// 1) '*' - drop all indexes.
// 2) <index name> - name of single index to drop.
// 3) <index key pattern> - BSON document representing key pattern of index to drop.
// 4) [<index name 1>, <index name 2>, ...] - array containing names of indexes to drop.
constexpr auto kIndexFieldName = "index"_sd;

/**
 * Drops single index by name.
 */
Status dropIndexByName(OperationContext* opCtx,
                       Collection* collection,
                       IndexCatalog* indexCatalog,
                       const std::string& indexToDelete) {
    auto desc = indexCatalog->findIndexByName(opCtx, indexToDelete);
    if (!desc) {
        return Status(ErrorCodes::IndexNotFound,
                      str::stream() << "index not found with name [" << indexToDelete << "]");
    }

    if (desc->isIdIndex()) {
        return Status(ErrorCodes::InvalidOptions, "cannot drop _id index");
    }

    auto s = indexCatalog->dropIndex(opCtx, desc);
    if (!s.isOK()) {
        return s;
    }

    opCtx->getServiceContext()->getOpObserver()->onDropIndex(
        opCtx, collection->ns(), collection->uuid(), desc->indexName(), desc->infoObj());

    return Status::OK();
}

Status wrappedRun(OperationContext* opCtx,
                  Collection* collection,
                  const BSONObj& jsobj,
                  BSONObjBuilder* anObjBuilder) {

    IndexCatalog* indexCatalog = collection->getIndexCatalog();
    anObjBuilder->appendNumber("nIndexesWas", indexCatalog->numIndexesTotal(opCtx));

    BSONElement indexElem = jsobj.getField(kIndexFieldName);
    if (indexElem.type() == String) {
        std::string indexToDelete = indexElem.valuestr();

        if (indexToDelete == "*") {
            indexCatalog->dropAllIndexes(
                opCtx, false, [opCtx, collection](const IndexDescriptor* desc) {
                    opCtx->getServiceContext()->getOpObserver()->onDropIndex(opCtx,
                                                                             collection->ns(),
                                                                             collection->uuid(),
                                                                             desc->indexName(),
                                                                             desc->infoObj());

                });

            anObjBuilder->append("msg", "non-_id indexes dropped for collection");
            return Status::OK();
        }

        return dropIndexByName(opCtx, collection, indexCatalog, indexToDelete);
    }

    if (indexElem.type() == Object) {
        std::vector<const IndexDescriptor*> indexes;
        collection->getIndexCatalog()->findIndexesByKeyPattern(
            opCtx, indexElem.embeddedObject(), false, &indexes);
        if (indexes.empty()) {
            return Status(ErrorCodes::IndexNotFound,
                          str::stream() << "can't find index with key: "
                                        << indexElem.embeddedObject());
        } else if (indexes.size() > 1) {
            return Status(ErrorCodes::AmbiguousIndexKeyPattern,
                          str::stream() << indexes.size() << " indexes found for key: "
                                        << indexElem.embeddedObject()
                                        << ", identify by name instead."
                                        << " Conflicting indexes: "
                                        << indexes[0]->infoObj()
                                        << ", "
                                        << indexes[1]->infoObj());
        }

        const IndexDescriptor* desc = indexes[0];
        if (desc->isIdIndex()) {
            return Status(ErrorCodes::InvalidOptions, "cannot drop _id index");
        }

        if (desc->indexName() == "*") {
            // Dropping an index named '*' results in an drop-index oplog entry with a name of '*',
            // which in 3.6 and later is interpreted by replication as meaning "drop all indexes on
            // this collection".
            return Status(ErrorCodes::InvalidOptions,
                          "cannot drop an index named '*' by key pattern.  You must drop the "
                          "entire collection, drop all indexes on the collection by using an index "
                          "name of '*', or downgrade to 3.4 to drop only this index.");
        }

        Status s = indexCatalog->dropIndex(opCtx, desc);
        if (!s.isOK()) {
            return s;
        }

        opCtx->getServiceContext()->getOpObserver()->onDropIndex(
            opCtx, collection->ns(), collection->uuid(), desc->indexName(), desc->infoObj());

        return Status::OK();
    }

    // The 'index' field contains a list of names of indexes to drop.
    // Drops all or none of the indexes due to the enclosing WriteUnitOfWork.
    if (indexElem.type() == Array) {
        for (auto indexNameElem : indexElem.Array()) {
            if (indexNameElem.type() != String) {
                return Status(ErrorCodes::TypeMismatch,
                              str::stream() << "dropIndexes " << collection->ns() << " ("
                                            << collection->uuid()
                                            << ") failed to drop multiple indexes "
                                            << indexElem.toString(false)
                                            << ": index name must be a string");
            }

            auto indexToDelete = indexNameElem.String();
            auto status = dropIndexByName(opCtx, collection, indexCatalog, indexToDelete);
            if (!status.isOK()) {
                return status.withContext(str::stream() << "dropIndexes " << collection->ns()
                                                        << " ("
                                                        << collection->uuid()
                                                        << ") failed to drop multiple indexes "
                                                        << indexElem.toString(false)
                                                        << ": "
                                                        << indexToDelete);
            }
        }

        return Status::OK();
    }

    return Status(ErrorCodes::IndexNotFound,
                  str::stream() << "invalid index name spec: " << indexElem.toString(false));
}

}  // namespace

Status dropIndexes(OperationContext* opCtx,
                   const NamespaceString& nss,
                   const BSONObj& cmdObj,
                   BSONObjBuilder* result) {
    return writeConflictRetry(opCtx, "dropIndexes", nss.db(), [opCtx, &nss, &cmdObj, result] {
        AutoGetDb autoDb(opCtx, nss.db(), MODE_X);

        bool userInitiatedWritesAndNotPrimary = opCtx->writesAreReplicated() &&
            !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss);

        if (userInitiatedWritesAndNotPrimary) {
            return Status(ErrorCodes::NotMaster,
                          str::stream() << "Not primary while dropping indexes in " << nss);
        }

        if (!serverGlobalParams.quiet.load()) {
            LOG(0) << "CMD: dropIndexes " << nss << ": " << cmdObj[kIndexFieldName].toString(false);
        }

        // If db/collection does not exist, short circuit and return.
        Database* db = autoDb.getDb();
        Collection* collection = db ? db->getCollection(opCtx, nss) : nullptr;
        if (!db || !collection) {
            if (db && ViewCatalog::get(db)->lookup(opCtx, nss.ns())) {
                return Status(ErrorCodes::CommandNotSupportedOnView,
                              str::stream() << "Cannot drop indexes on view " << nss);
            }

            return Status(ErrorCodes::NamespaceNotFound, "ns not found");
        }

        WriteUnitOfWork wunit(opCtx);
        OldClientContext ctx(opCtx, nss.ns());
        BackgroundOperation::assertNoBgOpInProgForNs(nss);

        Status status = wrappedRun(opCtx, collection, cmdObj, result);
        if (!status.isOK()) {
            return status;
        }

        wunit.commit();
        return Status::OK();
    });
}

}  // namespace mongo
