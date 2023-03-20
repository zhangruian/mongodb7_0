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

#include "mongo/platform/basic.h"

#include <string>
#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection_operation_source.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/bulk_write_crud_op.h"
#include "mongo/db/commands/bulk_write_gen.h"
#include "mongo/db/commands/bulk_write_parser.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/exec/queued_data_stage.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/not_primary_error_tracker.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/ops/parsed_update.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/transaction_validation.h"
#include "mongo/logv2/log.h"
#include "mongo/util/log_and_backoff.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangBeforeBulkWritePerformsUpdate);

using UpdateCallback = std::function<void(OperationContext*,
                                          int,
                                          const Status&,
                                          const boost::optional<UpdateResult>&,
                                          const boost::optional<BSONObj>&)>;

using DeleteCallback = std::function<void(OperationContext*, int, const SingleWriteResult&)>;

/**
 * Class representing an InsertBatch. Maintains a reference to the request and a callback function
 * which gets passed the replies from the insert statements being executed.
 */
class InsertBatch {
public:
    using ReplyHandler =
        std::function<void(OperationContext*, size_t, write_ops_exec::WriteResult&)>;

    InsertBatch() = delete;
    InsertBatch(const BulkWriteCommandRequest& request, int capacity, ReplyHandler replyCallback)
        : _req(request), _replyFn(replyCallback), _currentNs(), _batch(), _firstOpIdx() {
        _batch.reserve(capacity);
    }

    bool empty() const {
        return _batch.empty();
    }

    // Returns true if the write was successful and did not encounter errors.
    bool flush(OperationContext* opCtx) noexcept {
        if (empty()) {
            return true;
        }

        invariant(_firstOpIdx);
        invariant(_isDifferentFromSavedNamespace(NamespaceInfoEntry()));

        write_ops_exec::WriteResult out;
        auto size = _batch.size();
        out.results.reserve(size);

        write_ops_exec::LastOpFixer lastOpFixer(opCtx, _currentNs.getNs());

        out.canContinue = write_ops_exec::insertBatchAndHandleErrors(opCtx,
                                                                     _currentNs.getNs(),
                                                                     _currentNs.getCollectionUUID(),
                                                                     _req.getOrdered(),
                                                                     _batch,
                                                                     &lastOpFixer,
                                                                     &out,
                                                                     OperationSource::kStandard);
        _batch.clear();
        _replyFn(opCtx, _firstOpIdx.get(), out);
        _currentNs = NamespaceInfoEntry();
        _firstOpIdx = boost::none;

        return out.canContinue;
    }

    // Returns true if add was successful and did not encounter errors. Any responses
    // (including errors) are handled by this function and do not need to be explicitly written
    // by the caller.
    bool addToBatch(OperationContext* opCtx,
                    size_t currentOpIdx,
                    int32_t stmtId,
                    const NamespaceInfoEntry& nsInfo,
                    const BSONObj& op) {
        // If this is a different namespace we have to flush the current batch.
        // TODO SERVER-72682 refactor insertBatchAndHandleErrors to batch across namespaces.
        if (_isDifferentFromSavedNamespace(nsInfo)) {
            // Write the current batch since we have a different namespace to process.
            if (!flush(opCtx)) {
                return false;
            }
            invariant(empty());
            _currentNs = nsInfo;
            _firstOpIdx = currentOpIdx;
        }

        if (_addInsertToBatch(opCtx, stmtId, op)) {
            if (!flush(opCtx)) {
                return false;
            }
        }
        return true;
    }

private:
    const BulkWriteCommandRequest& _req;
    ReplyHandler _replyFn;
    NamespaceInfoEntry _currentNs;
    std::vector<InsertStatement> _batch;
    boost::optional<int> _firstOpIdx;

    bool _addInsertToBatch(OperationContext* opCtx, const int stmtId, const BSONObj& toInsert) {
        _batch.emplace_back(stmtId, toInsert);

        // Return true when the batch is at maximum capacity and should be flushed.
        return _batch.size() == _batch.capacity();
    }

    bool _isDifferentFromSavedNamespace(const NamespaceInfoEntry& newNs) const {
        if (newNs.getNs().ns().compare(_currentNs.getNs().ns()) == 0) {
            auto newUUID = newNs.getCollectionUUID();
            auto currentUUID = _currentNs.getCollectionUUID();
            if (newUUID && currentUUID) {
                return newUUID.get() != currentUUID.get();
            }
        }
        return true;
    }
};

/**
 * BulkWriteReplies maintains the BulkWriteReplyItems and provides an interface to add either
 * Insert or Update/Delete replies.
 */
class BulkWriteReplies {
public:
    BulkWriteReplies() = delete;
    BulkWriteReplies(const BulkWriteCommandRequest& request, int capacity)
        : _req(request), _replies() {
        _replies.reserve(capacity);
    }

    void addInsertReplies(OperationContext* opCtx,
                          size_t firstOpIdx,
                          write_ops_exec::WriteResult& writes) {
        invariant(!writes.results.empty());

        for (size_t i = 0; i < writes.results.size(); ++i) {
            auto idx = firstOpIdx + i;
            // We do not pass in a proper numErrors since it causes unwanted truncation in error
            // message generation.
            if (auto error = write_ops_exec::generateError(
                    opCtx, writes.results[i].getStatus(), idx, 0 /* numErrors */)) {
                auto replyItem = BulkWriteReplyItem(idx, error.get().getStatus());
                _replies.emplace_back(replyItem);
            } else {
                auto replyItem = BulkWriteReplyItem(idx);
                replyItem.setN(writes.results[i].getValue().getN());
                _replies.emplace_back(replyItem);
            }
        }
    }

    void addUpdateReply(OperationContext* opCtx,
                        size_t currentOpIdx,
                        const Status& status,
                        const boost::optional<UpdateResult>& result,
                        const boost::optional<BSONObj>& value) {
        auto replyItem = BulkWriteReplyItem(currentOpIdx, status);
        if (status.isOK()) {
            invariant(result);
            replyItem.setNModified(result->numDocsModified);
            if (!result->upsertedId.isEmpty()) {
                replyItem.setUpserted(
                    write_ops::Upserted(0, IDLAnyTypeOwned(result->upsertedId.firstElement())));
            }
            if (value) {
                replyItem.setValue(value);
            }
        }
        _replies.emplace_back(replyItem);
    }

    void addDeleteReply(OperationContext* opCtx,
                        size_t currentOpIdx,
                        const SingleWriteResult& write) {}

    std::vector<BulkWriteReplyItem>& getReplies() {
        return _replies;
    }

private:
    const BulkWriteCommandRequest& _req;
    std::vector<BulkWriteReplyItem> _replies;
};

int32_t getStatementId(OperationContext* opCtx,
                       const BulkWriteCommandRequest& req,
                       const size_t currentOpIdx) {
    if (opCtx->isRetryableWrite()) {
        auto stmtId = req.getStmtId();
        auto stmtIds = req.getStmtIds();

        if (stmtIds) {
            return stmtIds->at(currentOpIdx);
        }

        const int32_t firstStmtId = stmtId ? *stmtId : 0;
        return firstStmtId + currentOpIdx;
    }

    return kUninitializedStmtId;
}

bool handleInsertOp(OperationContext* opCtx,
                    const BulkWriteInsertOp* op,
                    const BulkWriteCommandRequest& req,
                    size_t currentOpIdx,
                    InsertBatch& batch) noexcept {
    const auto& nsInfo = req.getNsInfo();
    auto idx = op->getInsert();

    auto stmtId = getStatementId(opCtx, req, currentOpIdx);
    bool containsDotsAndDollarsField = false;
    auto fixedDoc = fixDocumentForInsert(opCtx, op->getDocument(), &containsDotsAndDollarsField);
    BSONObj toInsert =
        fixedDoc.getValue().isEmpty() ? op->getDocument() : std::move(fixedDoc.getValue());

    // TODO handle !fixedDoc.isOk() condition like in write_ops_exec::performInserts.
    return batch.addToBatch(opCtx, currentOpIdx, stmtId, nsInfo[idx], toInsert);
}

bool handleUpdateOp(OperationContext* opCtx,
                    const BulkWriteUpdateOp* op,
                    const BulkWriteCommandRequest& req,
                    size_t currentOpIdx,
                    UpdateCallback replyCB) noexcept {
    try {
        const auto& nsInfo = req.getNsInfo();

        auto idx = op->getUpdate();

        if (op->getMulti()) {
            uassert(ErrorCodes::InvalidOptions,
                    "May not specify both multi and return in bulkWrite command.",
                    !op->getReturn());
        }

        if (op->getReturnFields()) {
            uassert(ErrorCodes::InvalidOptions,
                    "Must specify return if returnFields is provided in bulkWrite command.",
                    op->getReturn());
        }

        const NamespaceString& nsString = nsInfo[idx].getNs();
        uassertStatusOK(userAllowedWriteNS(opCtx, nsString));
        auto const curOp = CurOp::get(opCtx);
        OpDebug* const opDebug = &curOp->debug();

        doTransactionValidationForWrites(opCtx, nsString);

        auto updateRequest = UpdateRequest();
        updateRequest.setNamespaceString(nsString);
        updateRequest.setQuery(op->getFilter());
        updateRequest.setProj(op->getReturnFields().value_or(BSONObj()));
        updateRequest.setUpdateModification(op->getUpdateMods());
        updateRequest.setLegacyRuntimeConstants(Variables::generateRuntimeConstants(opCtx));
        updateRequest.setLetParameters(op->getLet());
        updateRequest.setSort(op->getSort().value_or(BSONObj()));
        updateRequest.setHint(op->getHint());
        updateRequest.setCollation(op->getCollation().value_or(BSONObj()));
        updateRequest.setArrayFilters(op->getArrayFilters().value_or(std::vector<BSONObj>()));
        updateRequest.setUpsert(op->getUpsert());
        if (op->getReturn()) {
            updateRequest.setReturnDocs((op->getReturn().get() == "pre")
                                            ? UpdateRequest::RETURN_OLD
                                            : UpdateRequest::RETURN_NEW);
        } else {
            updateRequest.setReturnDocs(UpdateRequest::RETURN_NONE);
        }
        updateRequest.setMulti(op->getMulti());

        updateRequest.setYieldPolicy(opCtx->inMultiDocumentTransaction()
                                         ? PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY
                                         : PlanYieldPolicy::YieldPolicy::YIELD_AUTO);

        if (req.getStmtIds()) {
            updateRequest.setStmtIds(*req.getStmtIds());
        } else if (req.getStmtId()) {
            updateRequest.setStmtIds({*req.getStmtId()});
        }

        const bool inTransaction = opCtx->inMultiDocumentTransaction();

        // Although usually the PlanExecutor handles WCE internally, it will throw WCEs when it
        // is executing an update. This is done to ensure that we can always match,
        // modify, and return the document under concurrency, if a matching document exists.
        return writeConflictRetry(opCtx, "bulkWriteUpdate", nsString.ns(), [&] {
            if (MONGO_unlikely(hangBeforeBulkWritePerformsUpdate.shouldFail())) {
                CurOpFailpointHelpers::waitWhileFailPointEnabled(
                    &hangBeforeBulkWritePerformsUpdate, opCtx, "hangBeforeBulkWritePerformsUpdate");
            }

            // Nested retry loop to handle concurrent conflicting upserts with equality
            // match.
            int retryAttempts = 0;
            for (;;) {
                const ExtensionsCallbackReal extensionsCallback(
                    opCtx, &updateRequest.getNamespaceString());
                ParsedUpdate parsedUpdate(opCtx, &updateRequest, extensionsCallback);
                uassertStatusOK(parsedUpdate.parseRequest());

                try {
                    boost::optional<BSONObj> docFound;
                    auto result = write_ops_exec::writeConflictRetryUpsert(opCtx,
                                                                           nsString,
                                                                           curOp,
                                                                           opDebug,
                                                                           inTransaction,
                                                                           false,
                                                                           updateRequest.isUpsert(),
                                                                           docFound,
                                                                           &parsedUpdate);
                    replyCB(opCtx, currentOpIdx, Status::OK(), result, docFound);
                    return true;
                } catch (const ExceptionFor<ErrorCodes::DuplicateKey>& ex) {
                    if (!parsedUpdate.hasParsedQuery()) {
                        uassertStatusOK(parsedUpdate.parseQueryToCQ());
                    }

                    if (!write_ops_exec::shouldRetryDuplicateKeyException(
                            parsedUpdate, *ex.extraInfo<DuplicateKeyErrorInfo>())) {
                        throw;
                    }

                    ++retryAttempts;
                    logAndBackoff(7276500,
                                  ::mongo::logv2::LogComponent::kWrite,
                                  logv2::LogSeverity::Debug(1),
                                  retryAttempts,
                                  "Caught DuplicateKey exception during bulkWrite update",
                                  "namespace"_attr = updateRequest.getNamespaceString().ns());
                }
            }
        });
    } catch (const DBException& ex) {
        replyCB(opCtx, currentOpIdx, ex.toStatus(), boost::none, boost::none);
        return !req.getOrdered();
    }
}

bool handleDeleteOp(OperationContext* opCtx,
                    const BulkWriteDeleteOp* op,
                    const BulkWriteCommandRequest& req,
                    size_t currentOpIdx,
                    DeleteCallback replyCB) noexcept {
    // Perform the update operation then call replyCB with the SingleWriteResult and currentOpIdx
    // to save the response to be used in cursor creation.
    // see write_ops_exec::performDeletes for reference.
    return false;
}

std::vector<BulkWriteReplyItem> performWrites(OperationContext* opCtx,
                                              const BulkWriteCommandRequest& req) {
    const auto& ops = req.getOps();
    const auto& bypassDocumentValidation = req.getBypassDocumentValidation();

    DisableDocumentSchemaValidationIfTrue docSchemaValidationDisabler(opCtx,
                                                                      bypassDocumentValidation);

    DisableSafeContentValidationIfTrue safeContentValidationDisabler(
        opCtx, bypassDocumentValidation, false);

    auto responses = BulkWriteReplies(req, ops.size());

    // Construct reply handler callbacks.
    auto insertCB = [&responses](OperationContext* opCtx,
                                 int currentOpIdx,
                                 write_ops_exec::WriteResult& writes) {
        responses.addInsertReplies(opCtx, currentOpIdx, writes);
    };
    auto updateCB = [&responses](OperationContext* opCtx,
                                 int currentOpIdx,
                                 const Status& status,
                                 const boost::optional<UpdateResult>& result,
                                 const boost::optional<BSONObj>& value) {
        responses.addUpdateReply(opCtx, currentOpIdx, status, result, value);
    };
    auto deleteCB =
        [&responses](OperationContext* opCtx, int currentOpIdx, const SingleWriteResult& write) {
            responses.addDeleteReply(opCtx, currentOpIdx, write);
        };

    // Create a current insert batch.
    const size_t maxBatchSize = internalInsertMaxBatchSize.load();
    auto batch = InsertBatch(req, std::min(ops.size(), maxBatchSize), insertCB);

    size_t idx = 0;

    for (; idx < ops.size(); ++idx) {
        auto op = BulkWriteCRUDOp(ops[idx]);
        auto opType = op.getType();

        if (opType == BulkWriteCRUDOp::kInsert) {
            if (!handleInsertOp(opCtx, op.getInsert(), req, idx, batch)) {
                // Insert write failed can no longer continue.
                break;
            }
        } else if (opType == BulkWriteCRUDOp::kUpdate) {
            // Flush insert ops before handling update ops.
            if (!batch.flush(opCtx)) {
                break;
            }
            if (!handleUpdateOp(opCtx, op.getUpdate(), req, idx, updateCB)) {
                // Update write failed can no longer continue.
                break;
            }
        } else {
            // Flush insert ops before handling delete ops.
            if (!batch.flush(opCtx)) {
                break;
            }
            if (!handleDeleteOp(opCtx, op.getDelete(), req, idx, deleteCB)) {
                // Delete write failed can no longer continue.
                break;
            }
        }
    }

    // It does not matter if this final flush had errors or not since we finished processing
    // the last op already.
    batch.flush(opCtx);

    invariant(batch.empty());

    return responses.getReplies();
}

bool haveSpaceForNext(const BSONObj& nextDoc, long long numDocs, size_t bytesBuffered) {
    invariant(numDocs >= 0);
    if (!numDocs) {
        // Allow the first output document to exceed the limit to ensure we can always make
        // progress.
        return true;
    }

    return (bytesBuffered + nextDoc.objsize()) <= BSONObjMaxUserSize;
}

class BulkWriteCmd : public BulkWriteCmdVersion1Gen<BulkWriteCmd> {
public:
    bool adminOnly() const final {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool supportsRetryableWrite() const final {
        return true;
    }

    bool allowedInTransactions() const final {
        return true;
    }

    ReadWriteType getReadWriteType() const final {
        return Command::ReadWriteType::kWrite;
    }

    bool collectsResourceConsumptionMetrics() const final {
        return true;
    }

    bool shouldAffectCommandCounter() const final {
        return false;
    }

    std::string help() const override {
        return "command to apply inserts, updates and deletes in bulk";
    }

    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;

        bool supportsWriteConcern() const final {
            return true;
        }

        NamespaceString ns() const final {
            return NamespaceString(request().getDbName());
        }

        Reply typedRun(OperationContext* opCtx) final {
            uassert(
                ErrorCodes::CommandNotSupported,
                "BulkWrite may not be run without featureFlagBulkWriteCommand enabled",
                gFeatureFlagBulkWriteCommand.isEnabled(serverGlobalParams.featureCompatibility));

            auto& req = request();
            const auto& ops = req.getOps();
            const auto& nsInfo = req.getNsInfo();

            uassert(ErrorCodes::InvalidOptions,
                    str::stream()
                        << "May not specify both stmtId and stmtIds in bulkWrite command. Got "
                        << BSON("stmtId" << *req.getStmtId() << "stmtIds" << *req.getStmtIds())
                        << ". BulkWrite command: " << req.toBSON({}),
                    !(req.getStmtId() && req.getStmtIds()));

            if (const auto& stmtIds = req.getStmtIds()) {
                uassert(
                    ErrorCodes::InvalidLength,
                    str::stream()
                        << "Number of statement ids must match the number of batch entries. Got "
                        << stmtIds->size() << " statement ids but " << ops.size()
                        << " operations. Statement ids: " << BSON("stmtIds" << *stmtIds)
                        << ". BulkWrite command: " << req.toBSON({}),
                    stmtIds->size() == ops.size());
            }

            // Validate that every ops entry has a valid nsInfo index.
            for (const auto& op : ops) {
                const auto& bulkWriteOp = BulkWriteCRUDOp(op);
                unsigned int nsInfoIdx = bulkWriteOp.getNsInfoIdx();
                uassert(ErrorCodes::BadValue,
                        str::stream() << "BulkWrite ops entry " << bulkWriteOp.toBSON()
                                      << " has an invalid nsInfo index.",
                        nsInfoIdx < nsInfo.size());
            }

            // Apply all of the write operations.
            auto replies = performWrites(opCtx, req);

            return _populateCursorReply(opCtx, req, std::move(replies));
        }

        void doCheckAuthorization(OperationContext* opCtx) const final try {
            auto session = AuthorizationSession::get(opCtx->getClient());
            auto privileges = _getPrivileges();

            // Make sure all privileges are authorized.
            uassert(ErrorCodes::Unauthorized,
                    "unauthorized",
                    session->isAuthorizedForPrivileges(privileges));
        } catch (const DBException& ex) {
            NotPrimaryErrorTracker::get(opCtx->getClient()).recordError(ex.code());
            throw;
        }

    private:
        std::vector<Privilege> _getPrivileges() const {
            const auto& ops = request().getOps();
            const auto& nsInfo = request().getNsInfo();

            std::vector<Privilege> privileges;
            privileges.reserve(nsInfo.size());
            ActionSet actions;
            if (request().getBypassDocumentValidation()) {
                actions.addAction(ActionType::bypassDocumentValidation);
            }

            // Create initial Privilege entry for each nsInfo entry.
            for (const auto& ns : nsInfo) {
                privileges.emplace_back(ResourcePattern::forExactNamespace(ns.getNs()), actions);
            }

            // Iterate over each op and assign the appropriate actions to the namespace privilege.
            for (const auto& op : ops) {
                const auto& bulkWriteOp = BulkWriteCRUDOp(op);
                ActionSet newActions = bulkWriteOp.getActions();
                unsigned int nsInfoIdx = bulkWriteOp.getNsInfoIdx();
                uassert(ErrorCodes::BadValue,
                        str::stream() << "BulkWrite ops entry " << bulkWriteOp.toBSON()
                                      << " has an invalid nsInfo index.",
                        nsInfoIdx < nsInfo.size());

                auto& privilege = privileges[nsInfoIdx];
                privilege.addActions(newActions);
            }

            return privileges;
        }

        Reply _populateCursorReply(OperationContext* opCtx,
                                   const BulkWriteCommandRequest& req,
                                   std::vector<BulkWriteReplyItem> replies) {
            const NamespaceString cursorNss = NamespaceString::makeBulkWriteNSS();
            auto expCtx = make_intrusive<ExpressionContext>(
                opCtx, std::unique_ptr<CollatorInterface>(nullptr), ns());

            std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec;
            auto ws = std::make_unique<WorkingSet>();
            auto root = std::make_unique<QueuedDataStage>(expCtx.get(), ws.get());

            for (auto& reply : replies) {
                WorkingSetID id = ws->allocate();
                WorkingSetMember* member = ws->get(id);
                member->keyData.clear();
                member->recordId = RecordId();
                member->resetDocument(SnapshotId(), reply.toBSON());
                member->transitionToOwnedObj();
                root->pushBack(id);
            }

            exec = uassertStatusOK(
                plan_executor_factory::make(expCtx,
                                            std::move(ws),
                                            std::move(root),
                                            &CollectionPtr::null,
                                            PlanYieldPolicy::YieldPolicy::NO_YIELD,
                                            false, /* whether owned BSON must be returned */
                                            cursorNss));


            long long batchSize = std::numeric_limits<long long>::max();
            if (req.getCursor() && req.getCursor()->getBatchSize()) {
                batchSize = *req.getCursor()->getBatchSize();
            }

            size_t numReplies = 0;
            size_t bytesBuffered = 0;
            for (long long objCount = 0; objCount < batchSize; objCount++) {
                BSONObj nextDoc;
                PlanExecutor::ExecState state = exec->getNext(&nextDoc, nullptr);
                if (state == PlanExecutor::IS_EOF) {
                    break;
                }
                invariant(state == PlanExecutor::ADVANCED);

                // If we can't fit this result inside the current batch, then we stash it for
                // later.
                if (!haveSpaceForNext(nextDoc, objCount, bytesBuffered)) {
                    exec->stashResult(nextDoc);
                    break;
                }

                numReplies++;
                bytesBuffered += nextDoc.objsize();
            }
            if (exec->isEOF()) {
                invariant(numReplies == replies.size());
                return BulkWriteCommandReply(BulkWriteCommandResponseCursor(
                    0, std::vector<BulkWriteReplyItem>(std::move(replies))));
            }

            exec->saveState();
            exec->detachFromOperationContext();

            auto pinnedCursor = CursorManager::get(opCtx)->registerCursor(
                opCtx,
                {std::move(exec),
                 cursorNss,
                 AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserName(),
                 APIParameters::get(opCtx),
                 opCtx->getWriteConcern(),
                 repl::ReadConcernArgs::get(opCtx),
                 ReadPreferenceSetting::get(opCtx),
                 unparsedRequest().body,
                 _getPrivileges()});
            auto cursorId = pinnedCursor.getCursor()->cursorid();

            pinnedCursor->incNBatches();
            pinnedCursor->incNReturnedSoFar(replies.size());

            replies.resize(numReplies);
            return BulkWriteCommandReply(BulkWriteCommandResponseCursor(
                cursorId, std::vector<BulkWriteReplyItem>(std::move(replies))));
        }
    };

} bulkWriteCmd;

}  // namespace
}  // namespace mongo
