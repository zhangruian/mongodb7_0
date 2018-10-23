
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/process_interface_standalone.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/stats/fill_locker_info.h"
#include "mongo/db/stats/storage_stats.h"
#include "mongo/db/storage/backup_cursor_hooks.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/util/log.h"

namespace mongo {

using boost::intrusive_ptr;
using std::shared_ptr;
using std::string;
using std::unique_ptr;
using write_ops::Insert;
using write_ops::Update;
using write_ops::UpdateOpEntry;

namespace {

// Returns true if the field names of 'keyPattern' are exactly those in 'uniqueKeyPaths', and each
// of the elements of 'keyPattern' is numeric, i.e. not "text", "$**", or any other special type of
// index.
bool keyPatternNamesExactPaths(const BSONObj& keyPattern,
                               const std::set<FieldPath>& uniqueKeyPaths) {
    size_t nFieldsMatched = 0;
    for (auto&& elem : keyPattern) {
        if (!elem.isNumber()) {
            return false;
        }
        if (uniqueKeyPaths.find(elem.fieldNameStringData()) == uniqueKeyPaths.end()) {
            return false;
        }
        ++nFieldsMatched;
    }
    return nFieldsMatched == uniqueKeyPaths.size();
}

bool supportsUniqueKey(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                       const IndexCatalogEntry* index,
                       const std::set<FieldPath>& uniqueKeyPaths) {
    return (index->descriptor()->unique() && !index->descriptor()->isPartial() &&
            keyPatternNamesExactPaths(index->descriptor()->keyPattern(), uniqueKeyPaths) &&
            CollatorInterface::collatorsMatch(index->getCollator(), expCtx->getCollator()));
}

}  // namespace

MongoInterfaceStandalone::MongoInterfaceStandalone(OperationContext* opCtx) : _client(opCtx) {}

void MongoInterfaceStandalone::setOperationContext(OperationContext* opCtx) {
    _client.setOpCtx(opCtx);
}

DBClientBase* MongoInterfaceStandalone::directClient() {
    return &_client;
}

bool MongoInterfaceStandalone::isSharded(OperationContext* opCtx, const NamespaceString& nss) {
    AutoGetCollectionForReadCommand autoColl(opCtx, nss);
    auto const css = CollectionShardingState::get(opCtx, nss);
    return css->getMetadata(opCtx)->isSharded();
}

Insert MongoInterfaceStandalone::buildInsertOp(const NamespaceString& nss,
                                               std::vector<BSONObj>&& objs,
                                               bool bypassDocValidation) {
    Insert insertOp(nss);
    insertOp.setDocuments(std::move(objs));
    insertOp.setWriteCommandBase([&] {
        write_ops::WriteCommandBase wcb;
        wcb.setOrdered(false);
        wcb.setBypassDocumentValidation(bypassDocValidation);
        return wcb;
    }());
    return insertOp;
}

Update MongoInterfaceStandalone::buildUpdateOp(const NamespaceString& nss,
                                               std::vector<BSONObj>&& queries,
                                               std::vector<BSONObj>&& updates,
                                               bool upsert,
                                               bool multi,
                                               bool bypassDocValidation) {
    Update updateOp(nss);
    updateOp.setUpdates([&] {
        std::vector<UpdateOpEntry> updateEntries;
        for (size_t index = 0; index < queries.size(); ++index) {
            updateEntries.push_back([&] {
                UpdateOpEntry entry;
                entry.setQ(std::move(queries[index]));
                entry.setU(std::move(updates[index]));
                entry.setUpsert(upsert);
                entry.setMulti(multi);
                return entry;
            }());
        }
        return updateEntries;
    }());
    updateOp.setWriteCommandBase([&] {
        write_ops::WriteCommandBase wcb;
        wcb.setOrdered(false);
        wcb.setBypassDocumentValidation(bypassDocValidation);
        return wcb;
    }());
    return updateOp;
}

void MongoInterfaceStandalone::insert(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                      const NamespaceString& ns,
                                      std::vector<BSONObj>&& objs) {
    auto writeResults = performInserts(
        expCtx->opCtx, buildInsertOp(ns, std::move(objs), expCtx->bypassDocumentValidation));

    // Need to check each result in the batch since the writes are unordered.
    uassertStatusOKWithContext(
        [&writeResults]() {
            for (const auto& result : writeResults.results) {
                if (result.getStatus() != Status::OK()) {
                    return result.getStatus();
                }
            }
            return Status::OK();
        }(),
        "Insert failed: ");
}

void MongoInterfaceStandalone::update(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                      const NamespaceString& ns,
                                      std::vector<BSONObj>&& queries,
                                      std::vector<BSONObj>&& updates,
                                      bool upsert,
                                      bool multi) {
    auto writeResults = performUpdates(expCtx->opCtx,
                                       buildUpdateOp(ns,
                                                     std::move(queries),
                                                     std::move(updates),
                                                     upsert,
                                                     multi,
                                                     expCtx->bypassDocumentValidation));

    // Need to check each result in the batch since the writes are unordered.
    uassertStatusOKWithContext(
        [&writeResults]() {
            for (const auto& result : writeResults.results) {
                if (result.getStatus() != Status::OK()) {
                    return result.getStatus();
                }
            }
            return Status::OK();
        }(),
        "Update failed: ");
}

CollectionIndexUsageMap MongoInterfaceStandalone::getIndexStats(OperationContext* opCtx,
                                                                const NamespaceString& ns) {
    AutoGetCollectionForReadCommand autoColl(opCtx, ns);

    Collection* collection = autoColl.getCollection();
    if (!collection) {
        LOG(2) << "Collection not found on index stats retrieval: " << ns.ns();
        return CollectionIndexUsageMap();
    }

    return collection->infoCache()->getIndexUsageStats();
}

void MongoInterfaceStandalone::appendLatencyStats(OperationContext* opCtx,
                                                  const NamespaceString& nss,
                                                  bool includeHistograms,
                                                  BSONObjBuilder* builder) const {
    Top::get(opCtx->getServiceContext()).appendLatencyStats(nss.ns(), includeHistograms, builder);
}

Status MongoInterfaceStandalone::appendStorageStats(OperationContext* opCtx,
                                                    const NamespaceString& nss,
                                                    const BSONObj& param,
                                                    BSONObjBuilder* builder) const {
    return appendCollectionStorageStats(opCtx, nss, param, builder);
}

Status MongoInterfaceStandalone::appendRecordCount(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   BSONObjBuilder* builder) const {
    return appendCollectionRecordCount(opCtx, nss, builder);
}

BSONObj MongoInterfaceStandalone::getCollectionOptions(const NamespaceString& nss) {
    const auto infos = _client.getCollectionInfos(nss.db().toString(), BSON("name" << nss.coll()));
    return infos.empty() ? BSONObj() : infos.front().getObjectField("options").getOwned();
}

void MongoInterfaceStandalone::renameIfOptionsAndIndexesHaveNotChanged(
    OperationContext* opCtx,
    const BSONObj& renameCommandObj,
    const NamespaceString& targetNs,
    const BSONObj& originalCollectionOptions,
    const std::list<BSONObj>& originalIndexes) {
    Lock::GlobalWrite globalLock(opCtx);

    uassert(ErrorCodes::CommandFailed,
            str::stream() << "collection options of target collection " << targetNs.ns()
                          << " changed during processing. Original options: "
                          << originalCollectionOptions
                          << ", new options: "
                          << getCollectionOptions(targetNs),
            SimpleBSONObjComparator::kInstance.evaluate(originalCollectionOptions ==
                                                        getCollectionOptions(targetNs)));

    auto currentIndexes = _client.getIndexSpecs(targetNs.ns());
    uassert(ErrorCodes::CommandFailed,
            str::stream() << "indexes of target collection " << targetNs.ns()
                          << " changed during processing.",
            originalIndexes.size() == currentIndexes.size() &&
                std::equal(originalIndexes.begin(),
                           originalIndexes.end(),
                           currentIndexes.begin(),
                           SimpleBSONObjComparator::kInstance.makeEqualTo()));

    BSONObj info;
    uassert(ErrorCodes::CommandFailed,
            str::stream() << "renameCollection failed: " << info,
            _client.runCommand("admin", renameCommandObj, info));
}

StatusWith<std::unique_ptr<Pipeline, PipelineDeleter>> MongoInterfaceStandalone::makePipeline(
    const std::vector<BSONObj>& rawPipeline,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const MakePipelineOptions opts) {
    auto pipeline = Pipeline::parse(rawPipeline, expCtx);
    if (!pipeline.isOK()) {
        return pipeline.getStatus();
    }

    if (opts.optimize) {
        pipeline.getValue()->optimizePipeline();
    }

    Status cursorStatus = Status::OK();

    if (opts.attachCursorSource) {
        cursorStatus = attachCursorSourceToPipeline(expCtx, pipeline.getValue().get());
    }

    return cursorStatus.isOK() ? std::move(pipeline) : cursorStatus;
}

Status MongoInterfaceStandalone::attachCursorSourceToPipeline(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, Pipeline* pipeline) {
    invariant(pipeline->getSources().empty() ||
              !dynamic_cast<DocumentSourceCursor*>(pipeline->getSources().front().get()));

    boost::optional<AutoGetCollectionForReadCommand> autoColl;
    if (expCtx->uuid) {
        try {
            autoColl.emplace(expCtx->opCtx,
                             NamespaceStringOrUUID{expCtx->ns.db().toString(), *expCtx->uuid});
        } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>& ex) {
            // The UUID doesn't exist anymore
            return ex.toStatus();
        }
    } else {
        autoColl.emplace(expCtx->opCtx, expCtx->ns);
    }

    // makePipeline() is only called to perform secondary aggregation requests and expects the
    // collection representing the document source to be not-sharded. We confirm sharding state
    // here to avoid taking a collection lock elsewhere for this purpose alone.
    // TODO SERVER-27616: This check is incorrect in that we don't acquire a collection cursor
    // until after we release the lock, leaving room for a collection to be sharded in-between.
    auto css = CollectionShardingState::get(expCtx->opCtx, expCtx->ns);
    uassert(4567,
            str::stream() << "from collection (" << expCtx->ns.ns() << ") cannot be sharded",
            !css->getMetadata(expCtx->opCtx)->isSharded());

    PipelineD::prepareCursorSource(autoColl->getCollection(), expCtx->ns, nullptr, pipeline);

    // Optimize again, since there may be additional optimizations that can be done after adding
    // the initial cursor stage.
    pipeline->optimizePipeline();

    return Status::OK();
}

std::string MongoInterfaceStandalone::getShardName(OperationContext* opCtx) const {
    if (ShardingState::get(opCtx)->enabled()) {
        return ShardingState::get(opCtx)->shardId().toString();
    }

    return std::string();
}

std::pair<std::vector<FieldPath>, bool> MongoInterfaceStandalone::collectDocumentKeyFields(
    OperationContext* opCtx, NamespaceStringOrUUID nssOrUUID) const {
    return {{"_id"}, false};  // Nothing is sharded.
}

std::vector<GenericCursor> MongoInterfaceStandalone::getIdleCursors(
    const intrusive_ptr<ExpressionContext>& expCtx, CurrentOpUserMode userMode) const {
    return CursorManager::getIdleCursors(expCtx->opCtx, userMode);
}

boost::optional<Document> MongoInterfaceStandalone::lookupSingleDocument(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    UUID collectionUUID,
    const Document& documentKey,
    boost::optional<BSONObj> readConcern) {
    invariant(!readConcern);  // We don't currently support a read concern on mongod - it's only
                              // expected to be necessary on mongos.

    std::unique_ptr<Pipeline, PipelineDeleter> pipeline;
    try {
        // Be sure to do the lookup using the collection default collation
        auto foreignExpCtx = expCtx->copyWith(
            nss,
            collectionUUID,
            _getCollectionDefaultCollator(expCtx->opCtx, nss.db(), collectionUUID));
        pipeline = uassertStatusOK(makePipeline({BSON("$match" << documentKey)}, foreignExpCtx));
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        return boost::none;
    }

    auto lookedUpDocument = pipeline->getNext();
    if (auto next = pipeline->getNext()) {
        uasserted(ErrorCodes::TooManyMatchingDocuments,
                  str::stream() << "found more than one document with document key "
                                << documentKey.toString()
                                << " ["
                                << lookedUpDocument->toString()
                                << ", "
                                << next->toString()
                                << "]");
    }
    return lookedUpDocument;
}

BackupCursorState MongoInterfaceStandalone::openBackupCursor(OperationContext* opCtx) {
    auto backupCursorHooks = BackupCursorHooks::get(opCtx->getServiceContext());
    if (backupCursorHooks->enabled()) {
        return backupCursorHooks->openBackupCursor(opCtx);
    } else {
        uasserted(50956, "Backup cursors are an enterprise only feature.");
    }
}

void MongoInterfaceStandalone::closeBackupCursor(OperationContext* opCtx, std::uint64_t cursorId) {
    auto backupCursorHooks = BackupCursorHooks::get(opCtx->getServiceContext());
    if (backupCursorHooks->enabled()) {
        backupCursorHooks->closeBackupCursor(opCtx, cursorId);
    } else {
        uasserted(50955, "Backup cursors are an enterprise only feature.");
    }
}

std::vector<BSONObj> MongoInterfaceStandalone::getMatchingPlanCacheEntryStats(
    OperationContext* opCtx, const NamespaceString& nss, const MatchExpression* matchExp) const {
    const auto serializer = [](const PlanCacheEntry& entry) {
        BSONObjBuilder out;
        Explain::planCacheEntryToBSON(entry, &out);
        return out.obj();
    };

    const auto predicate = [&matchExp](const BSONObj& obj) {
        return !matchExp ? true : matchExp->matchesBSON(obj);
    };

    AutoGetCollection autoColl(opCtx, nss, MODE_IS);
    const auto collection = autoColl.getCollection();
    uassert(
        50933, str::stream() << "collection '" << nss.toString() << "' does not exist", collection);

    const auto infoCache = collection->infoCache();
    invariant(infoCache);
    const auto planCache = infoCache->getPlanCache();
    invariant(planCache);

    return planCache->getMatchingStats(serializer, predicate);
}

bool MongoInterfaceStandalone::uniqueKeyIsSupportedByIndex(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    const std::set<FieldPath>& uniqueKeyPaths) const {
    auto* opCtx = expCtx->opCtx;
    // We purposefully avoid a helper like AutoGetCollection here because we don't want to check the
    // db version or do anything else. We simply want to protect against concurrent modifications to
    // the catalog.
    Lock::DBLock dbLock(opCtx, nss.db(), MODE_IS);
    Lock::CollectionLock collLock(opCtx->lockState(), nss.ns(), MODE_IS);
    const auto* collection = [&]() -> Collection* {
        auto db = DatabaseHolder::getDatabaseHolder().get(opCtx, nss.db());
        return db ? db->getCollection(opCtx, nss) : nullptr;
    }();
    if (!collection) {
        return uniqueKeyPaths == std::set<FieldPath>{"_id"};
    }

    auto indexIterator = collection->getIndexCatalog()->getIndexIterator(opCtx, false);
    while (indexIterator.more()) {
        IndexDescriptor* descriptor = indexIterator.next();
        if (supportsUniqueKey(expCtx, indexIterator.catalogEntry(descriptor), uniqueKeyPaths)) {
            return true;
        }
    }
    return false;
}

BSONObj MongoInterfaceStandalone::_reportCurrentOpForClient(
    OperationContext* opCtx, Client* client, CurrentOpTruncateMode truncateOps) const {
    BSONObjBuilder builder;

    CurOp::reportCurrentOpForClient(
        opCtx, client, (truncateOps == CurrentOpTruncateMode::kTruncateOps), &builder);

    OperationContext* clientOpCtx = client->getOperationContext();

    if (clientOpCtx) {
        if (auto txnParticipant = TransactionParticipant::get(clientOpCtx)) {
            txnParticipant->reportUnstashedState(repl::ReadConcernArgs::get(clientOpCtx), &builder);
        }

        // Append lock stats before returning.
        if (auto lockerInfo = clientOpCtx->lockState()->getLockerInfo(
                CurOp::get(*clientOpCtx)->getLockStatsBase())) {
            fillLockerInfo(*lockerInfo, builder);
        }
    }

    return builder.obj();
}

void MongoInterfaceStandalone::_reportCurrentOpsForIdleSessions(OperationContext* opCtx,
                                                                CurrentOpUserMode userMode,
                                                                std::vector<BSONObj>* ops) const {
    auto sessionCatalog = SessionCatalog::get(opCtx);

    const bool authEnabled =
        AuthorizationSession::get(opCtx->getClient())->getAuthorizationManager().isAuthEnabled();

    // If the user is listing only their own ops, we use makeSessionFilterForAuthenticatedUsers to
    // create a pattern that will match against all authenticated usernames for the current client.
    // If the user is listing ops for all users, we create an empty pattern; constructing an
    // instance of SessionKiller::Matcher with this empty pattern will return all sessions.
    auto sessionFilter = (authEnabled && userMode == CurrentOpUserMode::kExcludeOthers
                              ? makeSessionFilterForAuthenticatedUsers(opCtx)
                              : KillAllSessionsByPatternSet{{}});

    sessionCatalog->scanSessions(
        opCtx,
        {std::move(sessionFilter)},
        [&](OperationContext* opCtx, Session* session) {
            auto op =
                TransactionParticipant::getFromNonCheckedOutSession(session)->reportStashedState();
            if (!op.isEmpty()) {
                ops->emplace_back(op);
            }
        });
}

std::unique_ptr<CollatorInterface> MongoInterfaceStandalone::_getCollectionDefaultCollator(
    OperationContext* opCtx, StringData dbName, UUID collectionUUID) {
    auto it = _collatorCache.find(collectionUUID);
    if (it == _collatorCache.end()) {
        auto collator = [&]() -> std::unique_ptr<CollatorInterface> {
            AutoGetCollection autoColl(opCtx, {dbName.toString(), collectionUUID}, MODE_IS);
            if (!autoColl.getCollection()) {
                // This collection doesn't exist, so assume a nullptr default collation
                return nullptr;
            } else {
                auto defaultCollator = autoColl.getCollection()->getDefaultCollator();
                // Clone the collator so that we can safely use the pointer if the collection
                // disappears right after we release the lock.
                return defaultCollator ? defaultCollator->clone() : nullptr;
            }
        }();

        it = _collatorCache.emplace(collectionUUID, std::move(collator)).first;
    }

    auto& collator = it->second;
    return collator ? collator->clone() : nullptr;
}

}  // namespace mongo
