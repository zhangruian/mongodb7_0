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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include <boost/optional.hpp>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/find_and_modify_common.h"
#include "mongo/db/commands/update_metrics.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/delete.h"
#include "mongo/db/exec/update_stage.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/delete_request_gen.h"
#include "mongo/db/ops/find_and_modify_result.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/ops/parsed_delete.h"
#include "mongo/db/ops/parsed_update.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/ops/write_ops_retryability.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/find_and_modify_request.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/retryable_writes_stats.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/db/write_concern.h"
#include "mongo/logv2/log.h"
#include "mongo/util/log_and_backoff.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangBeforeFindAndModifyPerformsUpdate);

namespace {

/**
 * If the operation succeeded, then returns either a document to return to the client, or
 * boost::none if no matching document to update/remove was found. If the operation failed, throws.
 */
boost::optional<BSONObj> advanceExecutor(OperationContext* opCtx,
                                         PlanExecutor* exec,
                                         bool isRemove) {
    BSONObj value;
    PlanExecutor::ExecState state;
    try {
        state = exec->getNext(&value, nullptr);
    } catch (DBException& exception) {
        auto&& explainer = exec->getPlanExplainer();
        auto&& [stats, _] = explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
        LOGV2_WARNING(23802,
                      "Plan executor error during findAndModify: {error}, stats: {stats}",
                      "Plan executor error during findAndModify",
                      "error"_attr = exception.toStatus(),
                      "stats"_attr = redact(stats));

        exception.addContext("Plan executor error during findAndModify");
        throw;
    }

    if (PlanExecutor::ADVANCED == state) {
        return {std::move(value)};
    }

    invariant(state == PlanExecutor::IS_EOF);
    return boost::none;
}

void makeUpdateRequest(OperationContext* opCtx,
                       const FindAndModifyRequest& args,
                       boost::optional<ExplainOptions::Verbosity> explain,
                       UpdateRequest* requestOut) {
    requestOut->setQuery(args.getQuery());
    requestOut->setProj(args.getFields());
    invariant(args.getUpdate());
    requestOut->setUpdateModification(*args.getUpdate());
    requestOut->setRuntimeConstants(
        args.getRuntimeConstants().value_or(Variables::generateRuntimeConstants(opCtx)));
    requestOut->setLetParameters(args.getLetParameters());
    requestOut->setSort(args.getSort());
    requestOut->setHint(args.getHint());
    requestOut->setCollation(args.getCollation());
    requestOut->setArrayFilters(args.getArrayFilters());
    requestOut->setUpsert(args.isUpsert());
    requestOut->setReturnDocs(args.shouldReturnNew() ? UpdateRequest::RETURN_NEW
                                                     : UpdateRequest::RETURN_OLD);
    requestOut->setMulti(false);
    requestOut->setExplain(explain);

    requestOut->setYieldPolicy(opCtx->inMultiDocumentTransaction()
                                   ? PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY
                                   : PlanYieldPolicy::YieldPolicy::YIELD_AUTO);
}

void makeDeleteRequest(OperationContext* opCtx,
                       const FindAndModifyRequest& args,
                       bool explain,
                       DeleteRequest* requestOut) {
    requestOut->setQuery(args.getQuery());
    requestOut->setProj(args.getFields());
    requestOut->setRuntimeConstants(
        args.getRuntimeConstants().value_or(Variables::generateRuntimeConstants(opCtx)));
    requestOut->setLet(args.getLetParameters());
    requestOut->setSort(args.getSort());
    requestOut->setHint(args.getHint());
    requestOut->setCollation(args.getCollation());
    requestOut->setMulti(false);
    requestOut->setReturnDeleted(true);  // Always return the old value.
    requestOut->setIsExplain(explain);

    requestOut->setYieldPolicy(opCtx->inMultiDocumentTransaction()
                                   ? PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY
                                   : PlanYieldPolicy::YieldPolicy::YIELD_AUTO);
}

void appendCommandResponse(const PlanExecutor* exec,
                           bool isRemove,
                           const boost::optional<BSONObj>& value,
                           BSONObjBuilder* result) {
    if (isRemove) {
        find_and_modify::serializeRemove(value, result);
    } else {
        const auto updateResult = exec->getUpdateResult();

        // Note we have to use the objInserted from the stats here, rather than 'value' because the
        // _id field could have been excluded by a projection.
        find_and_modify::serializeUpsert(
            !updateResult.upsertedId.isEmpty() ? 1 : updateResult.numMatched,
            value,
            updateResult.numMatched > 0,
            updateResult.upsertedId.isEmpty() ? BSONElement{}
                                              : updateResult.upsertedId.firstElement(),
            result);
    }
}

void assertCanWrite(OperationContext* opCtx, const NamespaceString& nsString) {
    uassert(ErrorCodes::NotWritablePrimary,
            str::stream() << "Not primary while running findAndModify command on collection "
                          << nsString.ns(),
            repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nsString));

    CollectionShardingState::get(opCtx, nsString)->checkShardVersionOrThrow(opCtx);
}

void recordStatsForTopCommand(OperationContext* opCtx) {
    auto curOp = CurOp::get(opCtx);
    Top::get(opCtx->getClient()->getServiceContext())
        .record(opCtx,
                curOp->getNS(),
                curOp->getLogicalOp(),
                Top::LockType::WriteLocked,
                durationCount<Microseconds>(curOp->elapsedTimeExcludingPauses()),
                curOp->isCommand(),
                curOp->getReadWriteType());
}

void checkIfTransactionOnCappedColl(const CollectionPtr& coll, bool inTransaction) {
    if (coll && coll->isCapped()) {
        uassert(
            ErrorCodes::OperationNotSupportedInTransaction,
            str::stream() << "Collection '" << coll->ns()
                          << "' is a capped collection. Writes in transactions are not allowed on "
                             "capped collections.",
            !inTransaction);
    }
}

class CmdFindAndModify : public BasicCommand {
public:
    CmdFindAndModify()
        : BasicCommand("findAndModify", "findandmodify"), _updateMetrics{"findAndModify"} {}

    const std::set<std::string>& apiVersions() const {
        return kApiVersions1;
    }

    std::string help() const override {
        return "{ findAndModify: \"collection\", query: {processed:false}, update: {$set: "
               "{processed:true}}, new: true}\n"
               "{ findAndModify: \"collection\", query: {processed:false}, remove: true, sort: "
               "{priority:-1}}\n"
               "Either update or remove is required, all other fields have default values.\n"
               "Output is in the \"value\" field\n";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    bool supportsReadMirroring(const BSONObj&) const override {
        return true;
    }

    bool collectsResourceConsumptionMetrics() const override {
        return true;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
        find_and_modify::addPrivilegesRequiredForFindAndModify(this, dbname, cmdObj, out);
    }

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kWrite;
    }

    Status explain(OperationContext* opCtx,
                   const OpMsgRequest& request,
                   ExplainOptions::Verbosity verbosity,
                   rpc::ReplyBuilderInterface* result) const override {
        std::string dbName = request.getDatabase().toString();
        const BSONObj& cmdObj = request.body;
        const auto args(uassertStatusOK(FindAndModifyRequest::parseFromBSON(
            CommandHelpers::parseNsCollectionRequired(dbName, cmdObj), cmdObj)));
        const NamespaceString& nsString = args.getNamespaceString();
        uassertStatusOK(userAllowedWriteNS(nsString));
        auto const curOp = CurOp::get(opCtx);
        OpDebug* const opDebug = &curOp->debug();

        if (args.isRemove()) {
            auto request = DeleteRequest{};
            request.setNsString(nsString);
            const bool isExplain = true;
            makeDeleteRequest(opCtx, args, isExplain, &request);

            ParsedDelete parsedDelete(opCtx, &request);
            uassertStatusOK(parsedDelete.parseRequest());

            // Explain calls of the findAndModify command are read-only, but we take write
            // locks so that the timing information is more accurate.
            AutoGetCollection collection(opCtx, nsString, MODE_IX);
            uassert(ErrorCodes::NamespaceNotFound,
                    str::stream() << "database " << dbName << " does not exist",
                    collection.getDb());

            CollectionShardingState::get(opCtx, nsString)->checkShardVersionOrThrow(opCtx);

            const auto exec = uassertStatusOK(
                getExecutorDelete(opDebug, &collection.getCollection(), &parsedDelete, verbosity));

            auto bodyBuilder = result->getBodyBuilder();
            Explain::explainStages(
                exec.get(), collection.getCollection(), verbosity, BSONObj(), &bodyBuilder);
        } else {
            auto request = UpdateRequest();
            request.setNamespaceString(nsString);
            makeUpdateRequest(opCtx, args, verbosity, &request);

            const ExtensionsCallbackReal extensionsCallback(opCtx, &request.getNamespaceString());
            ParsedUpdate parsedUpdate(opCtx, &request, extensionsCallback);
            uassertStatusOK(parsedUpdate.parseRequest());

            // Explain calls of the findAndModify command are read-only, but we take write
            // locks so that the timing information is more accurate.
            AutoGetCollection collection(opCtx, nsString, MODE_IX);
            uassert(ErrorCodes::NamespaceNotFound,
                    str::stream() << "database " << dbName << " does not exist",
                    collection.getDb());

            CollectionShardingState::get(opCtx, nsString)->checkShardVersionOrThrow(opCtx);

            const auto exec = uassertStatusOK(
                getExecutorUpdate(opDebug, &collection.getCollection(), &parsedUpdate, verbosity));

            auto bodyBuilder = result->getBodyBuilder();
            Explain::explainStages(
                exec.get(), collection.getCollection(), verbosity, BSONObj(), &bodyBuilder);
        }

        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const auto args(uassertStatusOK(FindAndModifyRequest::parseFromBSON(
            CommandHelpers::parseNsCollectionRequired(dbName, cmdObj), cmdObj)));
        const NamespaceString& nsString = args.getNamespaceString();
        uassertStatusOK(userAllowedWriteNS(nsString));
        auto const curOp = CurOp::get(opCtx);
        OpDebug* const opDebug = &curOp->debug();

        // Collect metrics.
        _updateMetrics.collectMetrics(cmdObj);

        boost::optional<DisableDocumentValidation> maybeDisableValidation;
        if (shouldBypassDocumentValidationForCommand(cmdObj)) {
            maybeDisableValidation.emplace(opCtx);
        }

        const auto inTransaction = opCtx->inMultiDocumentTransaction();
        uassert(50781,
                str::stream() << "Cannot write to system collection " << nsString.ns()
                              << " within a transaction.",
                !(inTransaction && nsString.isSystem()));

        const auto replCoord = repl::ReplicationCoordinator::get(opCtx->getServiceContext());
        uassert(50777,
                str::stream() << "Cannot write to unreplicated collection " << nsString.ns()
                              << " within a transaction.",
                !(inTransaction && replCoord->isOplogDisabledFor(opCtx, nsString)));


        const auto stmtId = 0;
        if (opCtx->getTxnNumber() && !inTransaction) {
            const auto txnParticipant = TransactionParticipant::get(opCtx);
            if (auto entry = txnParticipant.checkStatementExecuted(opCtx, stmtId)) {
                RetryableWritesStats::get(opCtx)->incrementRetriedCommandsCount();
                RetryableWritesStats::get(opCtx)->incrementRetriedStatementsCount();
                parseOplogEntryForFindAndModify(opCtx, args, *entry, &result);

                // Make sure to wait for writeConcern on the opTime that will include this write.
                // Needs to set to the system last opTime to get the latest term in an event when
                // an election happened after the actual write.
                auto& replClient = repl::ReplClientInfo::forClient(opCtx->getClient());
                replClient.setLastOpToSystemLastOpTime(opCtx);

                return true;
            }
        }

        // Although usually the PlanExecutor handles WCE internally, it will throw WCEs when it is
        // executing a findAndModify. This is done to ensure that we can always match, modify, and
        // return the document under concurrency, if a matching document exists.
        return writeConflictRetry(opCtx, "findAndModify", nsString.ns(), [&] {
            if (args.isRemove()) {
                return writeConflictRetryRemove(
                    opCtx, nsString, args, stmtId, curOp, opDebug, inTransaction, result);
            } else {
                if (MONGO_unlikely(hangBeforeFindAndModifyPerformsUpdate.shouldFail())) {
                    CurOpFailpointHelpers::waitWhileFailPointEnabled(
                        &hangBeforeFindAndModifyPerformsUpdate,
                        opCtx,
                        "hangBeforeFindAndModifyPerformsUpdate");
                }

                // Nested retry loop to handle concurrent conflicting upserts with equality match.
                int retryAttempts = 0;
                for (;;) {
                    auto request = UpdateRequest();
                    request.setNamespaceString(nsString);
                    const auto verbosity = boost::none;
                    makeUpdateRequest(opCtx, args, verbosity, &request);

                    if (opCtx->getTxnNumber()) {
                        request.setStmtId(stmtId);
                    }

                    const ExtensionsCallbackReal extensionsCallback(opCtx,
                                                                    &request.getNamespaceString());
                    ParsedUpdate parsedUpdate(opCtx, &request, extensionsCallback);
                    uassertStatusOK(parsedUpdate.parseRequest());

                    try {
                        return writeConflictRetryUpsert(opCtx,
                                                        nsString,
                                                        args,
                                                        curOp,
                                                        opDebug,
                                                        inTransaction,
                                                        &parsedUpdate,
                                                        result);
                    } catch (const ExceptionFor<ErrorCodes::DuplicateKey>& ex) {
                        if (!parsedUpdate.hasParsedQuery()) {
                            uassertStatusOK(parsedUpdate.parseQueryToCQ());
                        }

                        if (!write_ops_exec::shouldRetryDuplicateKeyException(
                                parsedUpdate, *ex.extraInfo<DuplicateKeyErrorInfo>())) {
                            throw;
                        }

                        ++retryAttempts;
                        logAndBackoff(4721200,
                                      ::mongo::logv2::LogComponent::kWrite,
                                      logv2::LogSeverity::Debug(1),
                                      retryAttempts,
                                      "Caught DuplicateKey exception during findAndModify upsert",
                                      "namespace"_attr = nsString.ns());
                    }
                }
            }

            return true;
        });
    }

    static bool writeConflictRetryRemove(OperationContext* opCtx,
                                         const NamespaceString& nsString,
                                         const FindAndModifyRequest& args,
                                         const int stmtId,
                                         CurOp* const curOp,
                                         OpDebug* const opDebug,
                                         const bool inTransaction,
                                         BSONObjBuilder& result) {
        auto request = DeleteRequest{};
        request.setNsString(nsString);
        const bool isExplain = false;
        makeDeleteRequest(opCtx, args, isExplain, &request);

        if (opCtx->getTxnNumber()) {
            request.setStmtId(stmtId);
        }

        ParsedDelete parsedDelete(opCtx, &request);
        uassertStatusOK(parsedDelete.parseRequest());

        AutoGetCollection collection(opCtx, nsString, MODE_IX);

        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            CurOp::get(opCtx)->enter_inlock(
                nsString.ns().c_str(),
                CollectionCatalog::get(opCtx).getDatabaseProfileLevel(nsString.db()));
        }

        assertCanWrite(opCtx, nsString);

        checkIfTransactionOnCappedColl(collection.getCollection(), inTransaction);

        const auto exec = uassertStatusOK(getExecutorDelete(
            opDebug, &collection.getCollection(), &parsedDelete, boost::none /* verbosity */));

        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            CurOp::get(opCtx)->setPlanSummary_inlock(exec->getPlanExplainer().getPlanSummary());
        }

        auto docFound = advanceExecutor(opCtx, exec.get(), args.isRemove());
        // Nothing after advancing the plan executor should throw a WriteConflictException,
        // so the following bookkeeping with execution stats won't end up being done
        // multiple times.

        PlanSummaryStats summaryStats;
        exec->getPlanExplainer().getSummaryStats(&summaryStats);
        if (const auto& coll = collection.getCollection()) {
            CollectionQueryInfo::get(coll).notifyOfQuery(opCtx, coll, summaryStats);
        }
        opDebug->setPlanSummaryMetrics(summaryStats);

        // Fill out OpDebug with the number of deleted docs.
        opDebug->additiveMetrics.ndeleted = docFound ? 1 : 0;

        if (curOp->shouldDBProfile(opCtx)) {
            auto&& explainer = exec->getPlanExplainer();
            auto&& [stats, _] =
                explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
            curOp->debug().execStats = std::move(stats);
        }
        recordStatsForTopCommand(opCtx);

        appendCommandResponse(exec.get(), args.isRemove(), docFound, &result);

        return true;
    }

    static bool writeConflictRetryUpsert(OperationContext* opCtx,
                                         const NamespaceString& nsString,
                                         const FindAndModifyRequest& args,
                                         CurOp* const curOp,
                                         OpDebug* const opDebug,
                                         const bool inTransaction,
                                         ParsedUpdate* parsedUpdate,
                                         BSONObjBuilder& result) {
        AutoGetCollection autoColl(opCtx, nsString, MODE_IX);
        Database* db = autoColl.ensureDbExists();

        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            CurOp::get(opCtx)->enter_inlock(
                nsString.ns().c_str(),
                CollectionCatalog::get(opCtx).getDatabaseProfileLevel(nsString.db()));
        }

        assertCanWrite(opCtx, nsString);

        CollectionPtr createdCollection;
        const CollectionPtr* collectionPtr = &autoColl.getCollection();

        // TODO SERVER-50983: Create abstraction for creating collection when using
        // AutoGetCollection Create the collection if it does not exist when performing an upsert
        // because the update stage does not create its own collection
        if (!*collectionPtr && args.isUpsert()) {
            assertCanWrite(opCtx, nsString);

            createdCollection =
                CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, nsString);

            // If someone else beat us to creating the collection, do nothing
            if (!createdCollection) {
                uassertStatusOK(userAllowedCreateNS(nsString));
                WriteUnitOfWork wuow(opCtx);
                CollectionOptions defaultCollectionOptions;
                uassertStatusOK(db->userCreateNS(opCtx, nsString, defaultCollectionOptions));
                wuow.commit();

                createdCollection =
                    CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, nsString);
            }

            invariant(createdCollection);
            collectionPtr = &createdCollection;
        }
        const auto& collection = *collectionPtr;

        checkIfTransactionOnCappedColl(collection, inTransaction);

        const auto exec = uassertStatusOK(
            getExecutorUpdate(opDebug, &collection, parsedUpdate, boost::none /* verbosity */));

        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            CurOp::get(opCtx)->setPlanSummary_inlock(exec->getPlanExplainer().getPlanSummary());
        }

        auto docFound = advanceExecutor(opCtx, exec.get(), args.isRemove());
        // Nothing after advancing the plan executor should throw a WriteConflictException,
        // so the following bookkeeping with execution stats won't end up being done
        // multiple times.

        PlanSummaryStats summaryStats;
        auto&& explainer = exec->getPlanExplainer();
        explainer.getSummaryStats(&summaryStats);
        if (collection) {
            CollectionQueryInfo::get(collection).notifyOfQuery(opCtx, collection, summaryStats);
        }
        write_ops_exec::recordUpdateResultInOpDebug(exec->getUpdateResult(), opDebug);
        opDebug->setPlanSummaryMetrics(summaryStats);

        if (curOp->shouldDBProfile(opCtx)) {
            auto&& [stats, _] =
                explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
            curOp->debug().execStats = std::move(stats);
        }
        recordStatsForTopCommand(opCtx);

        appendCommandResponse(exec.get(), args.isRemove(), docFound, &result);

        return true;
    }

    void appendMirrorableRequest(BSONObjBuilder* bob, const BSONObj& cmdObj) const override {
        // Filter the keys that can be mirrored
        static const auto kMirrorableKeys = [] {
            BSONObjBuilder keyBob;
            keyBob.append("sort", 1);
            keyBob.append("collation", 1);
            return keyBob.obj();
        }();

        bob->append("find", cmdObj.firstElement().String());
        if (cmdObj.hasField("query")) {
            bob->append("filter", cmdObj["query"].Obj());
        }

        cmdObj.filterFieldsUndotted(bob, kMirrorableKeys, true);

        // Prevent the find from returning multiple documents since we can
        bob->append("batchSize", 1);
        bob->append("singleBatch", true);
    }

private:
    // Update related command execution metrics.
    UpdateMetrics _updateMetrics;
} cmdFindAndModify;

}  // namespace
}  // namespace mongo
