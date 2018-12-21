
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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/run_aggregate.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/find.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

const auto kTermField = "term"_sd;

/**
 * A command for running .find() queries.
 */
class FindCmd final : public Command {
public:
    FindCmd() : Command("find") {}

    std::unique_ptr<CommandInvocation> parse(OperationContext* opCtx,
                                             const OpMsgRequest& opMsgRequest) override {
        // TODO: Parse into a QueryRequest here.
        return std::make_unique<Invocation>(this, opMsgRequest, opMsgRequest.getDatabase());
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext* context) const override {
        return AllowedOnSecondary::kOptIn;
    }

    bool maintenanceOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return false;
    }

    std::string help() const override {
        return "query for documents";
    }

    LogicalOp getLogicalOp() const override {
        return LogicalOp::opQuery;
    }

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kRead;
    }

    std::size_t reserveBytesForReply() const override {
        return FindCommon::kInitReplyBufferSize;
    }

    /**
     * A find command does not increment the command counter, but rather increments the
     * query counter.
     */
    bool shouldAffectCommandCounter() const override {
        return false;
    }

    class Invocation final : public CommandInvocation {
    public:
        Invocation(const FindCmd* definition, const OpMsgRequest& request, StringData dbName)
            : CommandInvocation(definition), _request(request), _dbName(dbName) {}

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        bool supportsReadConcern(repl::ReadConcernLevel level) const final {
            return true;
        }

        bool allowsSpeculativeMajorityReads() const override {
            // Find queries are only allowed to use speculative behavior if the 'allowsSpeculative'
            // flag is passed. The find command will check for this flag internally and fail if
            // necessary.
            return true;
        }

        NamespaceString ns() const override {
            // TODO get the ns from the parsed QueryRequest.
            return NamespaceString(CommandHelpers::parseNsFromCommand(_dbName, _request.body));
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            AuthorizationSession* authSession = AuthorizationSession::get(opCtx->getClient());

            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    authSession->isAuthorizedToParseNamespaceElement(_request.body.firstElement()));

            const auto hasTerm = _request.body.hasField(kTermField);
            uassertStatusOK(authSession->checkAuthForFind(
                AutoGetCollection::resolveNamespaceStringOrUUID(
                    opCtx, CommandHelpers::parseNsOrUUID(_dbName, _request.body)),
                hasTerm));
        }

        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* result) override {
            // Acquire locks and resolve possible UUID. The RAII object is optional, because in the
            // case of a view, the locks need to be released.
            boost::optional<AutoGetCollectionForReadCommand> ctx;
            ctx.emplace(opCtx,
                        CommandHelpers::parseNsOrUUID(_dbName, _request.body),
                        AutoGetCollection::ViewMode::kViewsPermitted);
            const auto nss = ctx->getNss();

            // Parse the command BSON to a QueryRequest.
            const bool isExplain = true;
            auto qr =
                uassertStatusOK(QueryRequest::makeFromFindCommand(nss, _request.body, isExplain));

            // Finish the parsing step by using the QueryRequest to create a CanonicalQuery.
            const ExtensionsCallbackReal extensionsCallback(opCtx, &nss);
            const boost::intrusive_ptr<ExpressionContext> expCtx;
            auto cq = uassertStatusOK(
                CanonicalQuery::canonicalize(opCtx,
                                             std::move(qr),
                                             expCtx,
                                             extensionsCallback,
                                             MatchExpressionParser::kAllowAllSpecialFeatures));

            if (ctx->getView()) {
                // Relinquish locks. The aggregation command will re-acquire them.
                ctx.reset();

                // Convert the find command into an aggregation using $match (and other stages, as
                // necessary), if possible.
                const auto& qr = cq->getQueryRequest();
                auto viewAggregationCommand = uassertStatusOK(qr.asAggregationCommand());

                // Create the agg request equivalent of the find operation, with the explain
                // verbosity included.
                auto aggRequest = uassertStatusOK(
                    AggregationRequest::parseFromBSON(nss, viewAggregationCommand, verbosity));

                try {
                    uassertStatusOK(
                        runAggregate(opCtx, nss, aggRequest, viewAggregationCommand, result));
                } catch (DBException& error) {
                    if (error.code() == ErrorCodes::InvalidPipelineOperator) {
                        uasserted(ErrorCodes::InvalidPipelineOperator,
                                  str::stream() << "Unsupported in view pipeline: "
                                                << error.what());
                    }
                    throw;
                }
                return;
            }

            // The collection may be NULL. If so, getExecutor() should handle it by returning an
            // execution tree with an EOFStage.
            Collection* const collection = ctx->getCollection();

            // We have a parsed query. Time to get the execution plan for it.
            auto exec = uassertStatusOK(getExecutorFind(opCtx, collection, nss, std::move(cq)));

            auto bodyBuilder = result->getBodyBuilder();
            // Got the execution tree. Explain it.
            Explain::explainStages(exec.get(), collection, verbosity, &bodyBuilder);
        }

        /**
         * Runs a query using the following steps:
         *   --Parsing.
         *   --Acquire locks.
         *   --Plan query, obtaining an executor that can run it.
         *   --Generate the first batch.
         *   --Save state for getMore, transferring ownership of the executor to a ClientCursor.
         *   --Generate response to send to the client.
         */
        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* result) {
            // Although it is a command, a find command gets counted as a query.
            globalOpCounters.gotQuery();

            // Parse the command BSON to a QueryRequest.
            const bool isExplain = false;
            // Pass parseNs to makeFromFindCommand in case _request.body does not have a UUID.
            auto qr = uassertStatusOK(QueryRequest::makeFromFindCommand(
                NamespaceString(CommandHelpers::parseNsFromCommand(_dbName, _request.body)),
                _request.body,
                isExplain));

            // Only allow speculative majority for internal commands that specify the correct flag.
            uassert(ErrorCodes::ReadConcernMajorityNotEnabled,
                    "Majority read concern is not enabled.",
                    !(repl::ReadConcernArgs::get(opCtx).isSpeculativeMajority() &&
                      !qr->allowSpeculativeMajorityRead()));

            auto replCoord = repl::ReplicationCoordinator::get(opCtx);
            const auto txnParticipant = TransactionParticipant::get(opCtx);
            uassert(ErrorCodes::InvalidOptions,
                    "It is illegal to open a tailable cursor in a transaction",
                    !txnParticipant ||
                        !(txnParticipant->inMultiDocumentTransaction() && qr->isTailable()));

            uassert(ErrorCodes::OperationNotSupportedInTransaction,
                    "The 'readOnce' option is not supported within a transaction.",
                    !txnParticipant ||
                        !txnParticipant->inActiveOrKilledMultiDocumentTransaction() ||
                        !qr->isReadOnce());

            // Validate term before acquiring locks, if provided.
            if (auto term = qr->getReplicationTerm()) {
                // Note: updateTerm returns ok if term stayed the same.
                uassertStatusOK(replCoord->updateTerm(opCtx, *term));
            }

            // Acquire locks. If the query is on a view, we release our locks and convert the query
            // request into an aggregation command.
            boost::optional<AutoGetCollectionForReadCommand> ctx;
            ctx.emplace(opCtx,
                        CommandHelpers::parseNsOrUUID(_dbName, _request.body),
                        AutoGetCollection::ViewMode::kViewsPermitted);
            const auto& nss = ctx->getNss();

            qr->refreshNSS(opCtx);

            // Check whether we are allowed to read from this node after acquiring our locks.
            uassertStatusOK(replCoord->checkCanServeReadsFor(
                opCtx, nss, ReadPreferenceSetting::get(opCtx).canRunOnSecondary()));

            // Fill out curop information.
            //
            // We pass negative values for 'ntoreturn' and 'ntoskip' to indicate that these values
            // should be omitted from the log line. Limit and skip information is already present in
            // the find command parameters, so these fields are redundant.
            const int ntoreturn = -1;
            const int ntoskip = -1;
            beginQueryOp(opCtx, nss, _request.body, ntoreturn, ntoskip);

            // Finish the parsing step by using the QueryRequest to create a CanonicalQuery.
            const ExtensionsCallbackReal extensionsCallback(opCtx, &nss);
            const boost::intrusive_ptr<ExpressionContext> expCtx;
            auto cq = uassertStatusOK(
                CanonicalQuery::canonicalize(opCtx,
                                             std::move(qr),
                                             expCtx,
                                             extensionsCallback,
                                             MatchExpressionParser::kAllowAllSpecialFeatures));

            if (ctx->getView()) {
                // Relinquish locks. The aggregation command will re-acquire them.
                ctx.reset();

                // Convert the find command into an aggregation using $match (and other stages, as
                // necessary), if possible.
                const auto& qr = cq->getQueryRequest();
                auto viewAggregationCommand = uassertStatusOK(qr.asAggregationCommand());

                BSONObj aggResult = CommandHelpers::runCommandDirectly(
                    opCtx, OpMsgRequest::fromDBAndBody(_dbName, std::move(viewAggregationCommand)));
                auto status = getStatusFromCommandResult(aggResult);
                if (status.code() == ErrorCodes::InvalidPipelineOperator) {
                    uasserted(ErrorCodes::InvalidPipelineOperator,
                              str::stream() << "Unsupported in view pipeline: " << status.reason());
                }
                uassertStatusOK(status);
                result->getBodyBuilder().appendElements(aggResult);
                return;
            }

            Collection* const collection = ctx->getCollection();

            if (cq->getQueryRequest().isReadOnce()) {
                // The readOnce option causes any storage-layer cursors created during plan
                // execution to assume read data will not be needed again and need not be cached.
                opCtx->recoveryUnit()->setReadOnce(true);
            }

            // Get the execution plan for the query.
            auto exec = uassertStatusOK(getExecutorFind(opCtx, collection, nss, std::move(cq)));

            {
                stdx::lock_guard<Client> lk(*opCtx->getClient());
                CurOp::get(opCtx)->setPlanSummary_inlock(Explain::getPlanSummary(exec.get()));
            }

            if (!collection) {
                // No collection. Just fill out curop indicating that there were zero results and
                // there is no ClientCursor id, and then return.
                const long long numResults = 0;
                const CursorId cursorId = 0;
                endQueryOp(opCtx, collection, *exec, numResults, cursorId);
                auto bodyBuilder = result->getBodyBuilder();
                appendCursorResponseObject(cursorId, nss.ns(), BSONArray(), &bodyBuilder);
                return;
            }

            CurOpFailpointHelpers::waitWhileFailPointEnabled(
                &waitInFindBeforeMakingBatch, opCtx, "waitInFindBeforeMakingBatch");

            const QueryRequest& originalQR = exec->getCanonicalQuery()->getQueryRequest();

            // Stream query results, adding them to a BSONArray as we go.
            CursorResponseBuilder::Options options;
            options.isInitialResponse = true;
            CursorResponseBuilder firstBatch(result, options);
            BSONObj obj;
            PlanExecutor::ExecState state = PlanExecutor::ADVANCED;
            std::uint64_t numResults = 0;
            while (!FindCommon::enoughForFirstBatch(originalQR, numResults) &&
                   PlanExecutor::ADVANCED == (state = exec->getNext(&obj, nullptr))) {
                // If we can't fit this result inside the current batch, then we stash it for later.
                if (!FindCommon::haveSpaceForNext(obj, numResults, firstBatch.bytesUsed())) {
                    exec->enqueue(obj);
                    break;
                }

                // Add result to output buffer.
                firstBatch.append(obj);
                numResults++;
            }

            // Throw an assertion if query execution fails for any reason.
            if (PlanExecutor::FAILURE == state || PlanExecutor::DEAD == state) {
                firstBatch.abandon();
                LOG(1) << "Plan executor error during find command: "
                       << PlanExecutor::statestr(state)
                       << ", stats: " << redact(Explain::getWinningPlanStats(exec.get()));

                uassertStatusOK(WorkingSetCommon::getMemberObjectStatus(obj).withContext(
                    "Executor error during find command"));
            }

            // Before saving the cursor, ensure that whatever plan we established happened with the
            // expected collection version
            auto css = CollectionShardingState::get(opCtx, nss);
            css->checkShardVersionOrThrow(opCtx);

            // Set up the cursor for getMore.
            CursorId cursorId = 0;
            if (shouldSaveCursor(opCtx, collection, state, exec.get())) {
                // Create a ClientCursor containing this plan executor and register it with the
                // cursor manager.
                ClientCursorPin pinnedCursor = collection->getCursorManager()->registerCursor(
                    opCtx,
                    {std::move(exec),
                     nss,
                     AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserNames(),
                     repl::ReadConcernArgs::get(opCtx),
                     _request.body});
                cursorId = pinnedCursor.getCursor()->cursorid();

                invariant(!exec);
                PlanExecutor* cursorExec = pinnedCursor.getCursor()->getExecutor();

                // State will be restored on getMore.
                cursorExec->saveState();
                cursorExec->detachFromOperationContext();

                // We assume that cursors created through a DBDirectClient are always used from
                // their original OperationContext, so we do not need to move time to and from the
                // cursor.
                if (!opCtx->getClient()->isInDirectClient()) {
                    pinnedCursor.getCursor()->setLeftoverMaxTimeMicros(
                        opCtx->getRemainingMaxTimeMicros());
                }
                pinnedCursor.getCursor()->setNReturnedSoFar(numResults);
                pinnedCursor.getCursor()->incNBatches();

                // Fill out curop based on the results.
                endQueryOp(opCtx, collection, *cursorExec, numResults, cursorId);
            } else {
                endQueryOp(opCtx, collection, *exec, numResults, cursorId);
            }

            // Generate the response object to send to the client.
            firstBatch.done(cursorId, nss.ns());
        }

    private:
        const OpMsgRequest& _request;
        const StringData _dbName;
    };

} findCmd;

}  // namespace
}  // namespace mongo
