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

#include "mongo/db/commands/run_aggregate.h"

#include <boost/optional.hpp>
#include <memory>
#include <vector>

#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection_uuid_mismatch.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/external_data_source_scope_guard.h"
#include "mongo/db/change_stream_change_collection_manager.h"
#include "mongo/db/change_stream_pre_images_collection_manager.h"
#include "mongo/db/change_stream_serverless_helpers.h"
#include "mongo/db/curop.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/disk_use_options_gen.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/change_stream_invalidation_info.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_exchange.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/pipeline/plan_executor_pipeline.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/pipeline/search_helper.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/query/query_stats/agg_key.h"
#include "mongo/db/query/query_stats/key.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/speculative_majority_read_info.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/query_analysis_writer.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/resource_consumption_metrics.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/views/view.h"
#include "mongo/db/views/view_catalog_helpers.h"
#include "mongo/logv2/log.h"
#include "mongo/s/query_analysis_sampler_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/string_map.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

using boost::intrusive_ptr;
using std::shared_ptr;
using std::string;
using std::stringstream;
using std::unique_ptr;

CounterMetric allowDiskUseFalseCounter("query.allowDiskUseFalse");

namespace {

MONGO_FAIL_POINT_DEFINE(hangAfterCreatingAggregationPlan);

/**
 * If a pipeline is empty (assuming that a $cursor stage hasn't been created yet), it could mean
 * that we were able to absorb all pipeline stages and pull them into a single PlanExecutor. So,
 * instead of creating a whole pipeline to do nothing more than forward the results of its cursor
 * document source, we can optimize away the entire pipeline and answer the request using the query
 * engine only. This function checks if such optimization is possible.
 */
bool canOptimizeAwayPipeline(const Pipeline* pipeline,
                             const PlanExecutor* exec,
                             const AggregateCommandRequest& request,
                             bool hasGeoNearStage,
                             bool hasChangeStreamStage) {
    return pipeline && exec && !hasGeoNearStage && !hasChangeStreamStage &&
        pipeline->getSources().empty() &&
        // For exchange we will create a number of pipelines consisting of a single
        // DocumentSourceExchange stage, so cannot not optimize it away.
        !request.getExchange();
}

/**
 * Returns true if we need to keep a ClientCursor saved for this pipeline (for future getMore
 * requests). Otherwise, returns false. The passed 'nsForCursor' is only used to determine the
 * namespace used in the returned cursor, which will be registered with the global cursor manager,
 * and thus will be different from that in 'request'.
 */
bool handleCursorCommand(OperationContext* opCtx,
                         boost::intrusive_ptr<ExpressionContext> expCtx,
                         const NamespaceString& nsForCursor,
                         std::vector<ClientCursor*> cursors,
                         const AggregateCommandRequest& request,
                         const BSONObj& cmdObj,
                         rpc::ReplyBuilderInterface* result) {
    invariant(!cursors.empty());
    long long batchSize =
        request.getCursor().getBatchSize().value_or(aggregation_request_helper::kDefaultBatchSize);

    if (cursors.size() > 1) {
        uassert(
            ErrorCodes::BadValue, "the exchange initial batch size must be zero", batchSize == 0);

        BSONArrayBuilder cursorsBuilder;
        for (size_t idx = 0; idx < cursors.size(); ++idx) {
            invariant(cursors[idx]);

            BSONObjBuilder cursorResult;
            appendCursorResponseObject(cursors[idx]->cursorid(),
                                       nsForCursor,
                                       BSONArray(),
                                       cursors[idx]->getExecutor()->getExecutorType(),
                                       &cursorResult);
            cursorResult.appendBool("ok", 1);

            cursorsBuilder.append(cursorResult.obj());

            // If a time limit was set on the pipeline, remaining time is "rolled over" to the
            // cursor (for use by future getmore ops).
            cursors[idx]->setLeftoverMaxTimeMicros(opCtx->getRemainingMaxTimeMicros());

            // Cursor needs to be in a saved state while we yield locks for getmore. State
            // will be restored in getMore().
            cursors[idx]->getExecutor()->saveState();
            cursors[idx]->getExecutor()->detachFromOperationContext();
        }

        auto bodyBuilder = result->getBodyBuilder();
        bodyBuilder.appendArray("cursors", cursorsBuilder.obj());

        return true;
    }

    CursorResponseBuilder::Options options;
    options.isInitialResponse = true;
    if (!opCtx->inMultiDocumentTransaction()) {
        options.atClusterTime = repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime();
    }
    CursorResponseBuilder responseBuilder(result, options);

    auto curOp = CurOp::get(opCtx);
    auto cursor = cursors[0];
    invariant(cursor);
    auto exec = cursor->getExecutor();
    invariant(exec);
    ResourceConsumption::DocumentUnitCounter docUnitsReturned;

    bool stashedResult = false;
    // We are careful to avoid ever calling 'getNext()' on the PlanExecutor when the batchSize is
    // zero to avoid doing any query execution work.
    for (int objCount = 0; objCount < batchSize; objCount++) {
        PlanExecutor::ExecState state;
        BSONObj nextDoc;

        try {
            state = exec->getNext(&nextDoc, nullptr);
        } catch (const ExceptionFor<ErrorCodes::CloseChangeStream>&) {
            // This exception is thrown when a $changeStream stage encounters an event that
            // invalidates the cursor. We should close the cursor and return without error.
            cursor = nullptr;
            exec = nullptr;
            break;
        } catch (const ExceptionFor<ErrorCodes::ChangeStreamInvalidated>& ex) {
            // This exception is thrown when a change-stream cursor is invalidated. Set the PBRT
            // to the resume token of the invalidating event, and mark the cursor response as
            // invalidated. We expect ExtraInfo to always be present for this exception.
            const auto extraInfo = ex.extraInfo<ChangeStreamInvalidationInfo>();
            tassert(5493701, "Missing ChangeStreamInvalidationInfo on exception", extraInfo);

            responseBuilder.setPostBatchResumeToken(extraInfo->getInvalidateResumeToken());
            responseBuilder.setInvalidated();

            cursor = nullptr;
            exec = nullptr;
            break;
        } catch (DBException& exception) {
            auto&& explainer = exec->getPlanExplainer();
            auto&& [stats, _] =
                explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
            LOGV2_WARNING(23799,
                          "Aggregate command executor error",
                          "error"_attr = exception.toStatus(),
                          "stats"_attr = redact(stats),
                          "cmd"_attr = cmdObj);

            exception.addContext("PlanExecutor error during aggregation");
            throw;
        }

        if (state == PlanExecutor::IS_EOF) {
            // If this executor produces a postBatchResumeToken, add it to the cursor response. We
            // call this on EOF because the PBRT may advance even when there are no further results.
            responseBuilder.setPostBatchResumeToken(exec->getPostBatchResumeToken());

            if (!cursor->isTailable()) {
                // Make it an obvious error to use cursor or executor after this point.
                cursor = nullptr;
                exec = nullptr;
            }
            break;
        }

        invariant(state == PlanExecutor::ADVANCED);

        // If adding this object will cause us to exceed the message size limit, then we stash it
        // for later.

        if (!FindCommon::haveSpaceForNext(nextDoc, objCount, responseBuilder.bytesUsed())) {
            exec->stashResult(nextDoc);
            stashedResult = true;
            break;
        }

        // If this executor produces a postBatchResumeToken, add it to the cursor response.
        responseBuilder.setPostBatchResumeToken(exec->getPostBatchResumeToken());
        responseBuilder.append(nextDoc);
        docUnitsReturned.observeOne(nextDoc.objsize());
    }

    if (cursor) {
        invariant(cursor->getExecutor() == exec);

        // For empty batches, or in the case where the final result was added to the batch rather
        // than being stashed, we update the PBRT to ensure that it is the most recent available.
        if (!stashedResult) {
            responseBuilder.setPostBatchResumeToken(exec->getPostBatchResumeToken());
        }
        // If a time limit was set on the pipeline, remaining time is "rolled over" to the
        // cursor (for use by future getmore ops).
        cursor->setLeftoverMaxTimeMicros(opCtx->getRemainingMaxTimeMicros());

        curOp->debug().cursorid = cursor->cursorid();

        // Cursor needs to be in a saved state while we yield locks for getmore. State
        // will be restored in getMore().
        exec->saveState();
        exec->detachFromOperationContext();
    } else {
        curOp->debug().cursorExhausted = true;
    }

    const CursorId cursorId = cursor ? cursor->cursorid() : 0LL;
    responseBuilder.done(cursorId, nsForCursor);

    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
    metricsCollector.incrementDocUnitsReturned(curOp->getNS(), docUnitsReturned);

    return static_cast<bool>(cursor);
}

StatusWith<StringMap<ExpressionContext::ResolvedNamespace>> resolveInvolvedNamespaces(
    OperationContext* opCtx, const AggregateCommandRequest& request) {
    const LiteParsedPipeline liteParsedPipeline(request);
    const auto& pipelineInvolvedNamespaces = liteParsedPipeline.getInvolvedNamespaces();

    // If there are no involved namespaces, return before attempting to take any locks. This is
    // important for collectionless aggregations, which may be expected to run without locking.
    if (pipelineInvolvedNamespaces.empty()) {
        return {StringMap<ExpressionContext::ResolvedNamespace>()};
    }

    // Acquire a single const view of the CollectionCatalog and use it for all view and collection
    // lookups and view definition resolutions that follow. This prevents the view definitions
    // cached in 'resolvedNamespaces' from changing relative to those in the acquired ViewCatalog.
    // The resolution of the view definitions below might lead into an endless cycle if any are
    // allowed to change.
    auto catalog = CollectionCatalog::get(opCtx);

    std::deque<NamespaceString> involvedNamespacesQueue(pipelineInvolvedNamespaces.begin(),
                                                        pipelineInvolvedNamespaces.end());
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;

    while (!involvedNamespacesQueue.empty()) {
        auto involvedNs = std::move(involvedNamespacesQueue.front());
        involvedNamespacesQueue.pop_front();

        if (resolvedNamespaces.find(involvedNs.coll()) != resolvedNamespaces.end()) {
            continue;
        }

        // If 'ns' refers to a view namespace, then we resolve its definition.
        auto resolveViewDefinition = [&](const NamespaceString& ns) -> Status {
            auto resolvedView = view_catalog_helpers::resolveView(opCtx, catalog, ns, boost::none);
            if (!resolvedView.isOK()) {
                return resolvedView.getStatus().withContext(
                    str::stream() << "Failed to resolve view '" << involvedNs.ns());
            }

            auto&& underlyingNs = resolvedView.getValue().getNamespace();
            // Attempt to acquire UUID of the underlying collection using lock free method.
            auto uuid = catalog->lookupUUIDByNSS(opCtx, underlyingNs);
            resolvedNamespaces[ns.coll()] = {
                underlyingNs, resolvedView.getValue().getPipeline(), uuid};

            // We parse the pipeline corresponding to the resolved view in case we must resolve
            // other view namespaces that are also involved.
            LiteParsedPipeline resolvedViewLitePipeline(resolvedView.getValue().getNamespace(),
                                                        resolvedView.getValue().getPipeline());

            const auto& resolvedViewInvolvedNamespaces =
                resolvedViewLitePipeline.getInvolvedNamespaces();
            involvedNamespacesQueue.insert(involvedNamespacesQueue.end(),
                                           resolvedViewInvolvedNamespaces.begin(),
                                           resolvedViewInvolvedNamespaces.end());
            return Status::OK();
        };

        // If the involved namespace is not in the same database as the aggregation, it must be
        // from a $lookup/$graphLookup into a tenant migration donor's oplog view or from an
        // $out/$merge to a collection in a different database.
        if (involvedNs.db() != request.getNamespace().db()) {
            if (involvedNs == NamespaceString::kTenantMigrationOplogView) {
                // For tenant migrations, we perform an aggregation on 'config.transactions' but
                // require a lookup stage involving a view on the 'local' database.
                // If the involved namespace is 'local.system.tenantMigration.oplogView', resolve
                // its view definition.
                auto status = resolveViewDefinition(involvedNs);
                if (!status.isOK()) {
                    return status;
                }
            } else {
                // SERVER-51886: It is not correct to assume that we are reading from a collection
                // because the collection targeted by $out/$merge on a given database can have the
                // same name as a view on the source database. As such, we determine whether the
                // collection name references a view on the aggregation request's database. Note
                // that the inverse scenario (mistaking a view for a collection) is not an issue
                // because $merge/$out cannot target a view.
                auto nssToCheck = NamespaceStringUtil::parseNamespaceFromRequest(
                    request.getNamespace().dbName(), involvedNs.coll());
                if (catalog->lookupView(opCtx, nssToCheck)) {
                    auto status = resolveViewDefinition(nssToCheck);
                    if (!status.isOK()) {
                        return status;
                    }
                } else {
                    resolvedNamespaces[involvedNs.coll()] = {involvedNs, std::vector<BSONObj>{}};
                }
            }
        } else if (catalog->lookupCollectionByNamespace(opCtx, involvedNs)) {
            // Attempt to acquire UUID of the collection using lock free method.
            auto uuid = catalog->lookupUUIDByNSS(opCtx, involvedNs);
            // If 'involvedNs' refers to a collection namespace, then we resolve it as an empty
            // pipeline in order to read directly from the underlying collection.
            resolvedNamespaces[involvedNs.coll()] = {involvedNs, std::vector<BSONObj>{}, uuid};
        } else if (catalog->lookupView(opCtx, involvedNs)) {
            auto status = resolveViewDefinition(involvedNs);
            if (!status.isOK()) {
                return status;
            }
        } else {
            // 'involvedNs' is neither a view nor a collection, so resolve it as an empty pipeline
            // to treat it as reading from a non-existent collection.
            resolvedNamespaces[involvedNs.coll()] = {involvedNs, std::vector<BSONObj>{}};
        }
    }

    return resolvedNamespaces;
}

/**
 * Returns Status::OK if each view namespace in 'pipeline' has a default collator equivalent to
 * 'collator'. Otherwise, returns ErrorCodes::OptionNotSupportedOnView.
 */
Status collatorCompatibleWithPipeline(OperationContext* opCtx,
                                      const CollatorInterface* collator,
                                      const LiteParsedPipeline& liteParsedPipeline) {
    auto catalog = CollectionCatalog::get(opCtx);
    for (auto&& potentialViewNs : liteParsedPipeline.getInvolvedNamespaces()) {
        if (catalog->lookupCollectionByNamespace(opCtx, potentialViewNs)) {
            continue;
        }

        auto view = catalog->lookupView(opCtx, potentialViewNs);
        if (!view) {
            continue;
        }
        if (!CollatorInterface::collatorsMatch(view->defaultCollator(), collator)) {
            return {ErrorCodes::OptionNotSupportedOnView,
                    str::stream() << "Cannot override a view's default collation"
                                  << potentialViewNs.ns()};
        }
    }
    return Status::OK();
}

boost::intrusive_ptr<ExpressionContext> makeExpressionContext(
    OperationContext* opCtx,
    const AggregateCommandRequest& request,
    std::unique_ptr<CollatorInterface> collator,
    boost::optional<UUID> uuid,
    ExpressionContext::CollationMatchesDefault collationMatchesDefault) {
    boost::intrusive_ptr<ExpressionContext> expCtx =
        new ExpressionContext(opCtx,
                              request,
                              std::move(collator),
                              MongoProcessInterface::create(opCtx),
                              uassertStatusOK(resolveInvolvedNamespaces(opCtx, request)),
                              uuid,
                              CurOp::get(opCtx)->dbProfileLevel() > 0,
                              allowDiskUseByDefault.load());
    expCtx->tempDir = storageGlobalParams.dbpath + "/_tmp";
    expCtx->collationMatchesDefault = collationMatchesDefault;

    // If the request explicitly specified NOT to use v2 resume tokens for change streams, set this
    // on the expCtx. This can happen if a the request originated from 6.0 mongos, or in test mode.
    if (request.getGenerateV2ResumeTokens().has_value()) {
        // We only ever expect an explicit $_generateV2ResumeTokens to be false.
        uassert(6528200, "Invalid request for v2 tokens", !request.getGenerateV2ResumeTokens());
        expCtx->changeStreamTokenVersion = 1;
    }

    return expCtx;
}

/**
 * Upconverts the read concern for a change stream aggregation, if necesssary.
 *
 * If there is no given read concern level on the given object, upgrades the level to 'majority' and
 * waits for read concern. If a read concern level is already specified on the given read concern
 * object, this method does nothing.
 */
void _adjustChangeStreamReadConcern(OperationContext* opCtx) {
    repl::ReadConcernArgs& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    // There is already a non-default read concern level set. Do nothing.
    if (readConcernArgs.hasLevel() && !readConcernArgs.getProvenance().isImplicitDefault()) {
        return;
    }
    // We upconvert an empty read concern to 'majority'.
    {
        // We must obtain the client lock to set the ReadConcernArgs on the operation
        // context as it may be concurrently read by CurrentOp.
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        readConcernArgs = repl::ReadConcernArgs(repl::ReadConcernLevel::kMajorityReadConcern);

        // Change streams are allowed to use the speculative majority read mechanism, if
        // the storage engine doesn't support majority reads directly.
        if (!serverGlobalParams.enableMajorityReadConcern) {
            readConcernArgs.setMajorityReadMechanism(
                repl::ReadConcernArgs::MajorityReadMechanism::kSpeculative);
        }
    }

    // Wait for read concern again since we changed the original read concern.
    uassertStatusOK(waitForReadConcern(opCtx, readConcernArgs, DatabaseName(), true));
    setPrepareConflictBehaviorForReadConcern(
        opCtx, readConcernArgs, PrepareConflictBehavior::kIgnoreConflicts);
}

/**
 * If the aggregation 'request' contains an exchange specification, create a new pipeline for each
 * consumer and put it into the resulting vector. Otherwise, return the original 'pipeline' as a
 * single vector element.
 */
std::vector<std::unique_ptr<Pipeline, PipelineDeleter>> createExchangePipelinesIfNeeded(
    OperationContext* opCtx,
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const AggregateCommandRequest& request,
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
    boost::optional<UUID> uuid) {
    std::vector<std::unique_ptr<Pipeline, PipelineDeleter>> pipelines;

    if (request.getExchange() && !expCtx->explain) {
        boost::intrusive_ptr<Exchange> exchange =
            new Exchange(request.getExchange().value(), std::move(pipeline));

        for (size_t idx = 0; idx < exchange->getConsumers(); ++idx) {
            // For every new pipeline we have create a new ExpressionContext as the context
            // cannot be shared between threads. There is no synchronization for pieces of
            // the execution machinery above the Exchange, so nothing above the Exchange can be
            // shared between different exchange-producer cursors.
            expCtx = makeExpressionContext(opCtx,
                                           request,
                                           expCtx->getCollator() ? expCtx->getCollator()->clone()
                                                                 : nullptr,
                                           uuid,
                                           expCtx->collationMatchesDefault);

            // Create a new pipeline for the consumer consisting of a single
            // DocumentSourceExchange.
            boost::intrusive_ptr<DocumentSource> consumer = new DocumentSourceExchange(
                expCtx,
                exchange,
                idx,
                // Assumes this is only called from the 'aggregate' or 'getMore' commands.  The code
                // which relies on this parameter does not distinguish/care about the difference so
                // we simply always pass 'aggregate'.
                expCtx->mongoProcessInterface->getResourceYielder("aggregate"_sd));
            pipelines.emplace_back(Pipeline::create({consumer}, expCtx));
        }
    } else {
        pipelines.emplace_back(std::move(pipeline));
    }

    return pipelines;
}

/**
 * Creates additional pipelines if needed to serve the aggregation. This includes additional
 * pipelines for exchange optimization and search commands that generate metadata. Returns
 * a vector of all pipelines needed for the query, including the original one.
 *
 * Takes ownership of the original, passed in, pipeline.
 */
std::vector<std::unique_ptr<Pipeline, PipelineDeleter>> createAdditionalPipelinesIfNeeded(
    OperationContext* opCtx,
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const AggregateCommandRequest& request,
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
    boost::optional<UUID> collUUID,
    const std::function<void(void)>& resetContextFn) {

    std::vector<std::unique_ptr<Pipeline, PipelineDeleter>> pipelines;
    // Exchange is not allowed to be specified if there is a $search stage.
    if (getSearchHelpers(opCtx->getServiceContext())->isSearchPipeline(pipeline.get())) {
        // Release locks early, before we generate the search pipeline, so that we don't hold them
        // during network calls to mongot. This is fine for search pipelines since they are not
        // reading any local (lock-protected) data in the main pipeline.
        resetContextFn();
        pipelines.push_back(std::move(pipeline));

        if (auto metadataPipe = getSearchHelpers(opCtx->getServiceContext())
                                    ->generateMetadataPipelineAndAttachCursorsForSearch(
                                        opCtx, expCtx, request, pipelines.back().get(), collUUID)) {
            pipelines.push_back(std::move(metadataPipe));
        }
    } else {
        // Takes ownership of 'pipeline'.
        pipelines =
            createExchangePipelinesIfNeeded(opCtx, expCtx, request, std::move(pipeline), collUUID);
    }
    return pipelines;
}

/**
 * Performs validations related to API versioning, time-series stages, and general command
 * validation.
 * Throws UserAssertion if any of the validations fails
 *     - validation of API versioning on each stage on the pipeline
 *     - validation of API versioning on 'AggregateCommandRequest' request
 *     - validation of time-series related stages
 *     - validation of command parameters
 */
void performValidationChecks(const OperationContext* opCtx,
                             const AggregateCommandRequest& request,
                             const LiteParsedPipeline& liteParsedPipeline) {
    liteParsedPipeline.validate(opCtx);
    aggregation_request_helper::validateRequestForAPIVersion(opCtx, request);
    aggregation_request_helper::validateRequestFromClusterQueryWithoutShardKey(request);
}

std::vector<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> createExecutor(
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
    const LiteParsedPipeline& liteParsedPipeline,
    const NamespaceString& nss,
    const MultipleCollectionAccessor& collections,
    const AggregateCommandRequest& request,
    CurOp* curOp,
    const std::function<void(void)>& resetContextFn) {
    const auto expCtx = pipeline->getContext();
    // Check if the pipeline has a $geoNear stage, as it will be ripped away during the build query
    // executor phase below (to be replaced with a $geoNearCursorStage later during the executor
    // attach phase).
    auto hasGeoNearStage = !pipeline->getSources().empty() &&
        dynamic_cast<DocumentSourceGeoNear*>(pipeline->peekFront());

    // Prepare a PlanExecutor to provide input into the pipeline, if needed.
    auto attachExecutorCallback =
        PipelineD::buildInnerQueryExecutor(collections, nss, &request, pipeline.get());

    std::vector<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> execs;
    if (canOptimizeAwayPipeline(pipeline.get(),
                                attachExecutorCallback.second.get(),
                                request,
                                hasGeoNearStage,
                                liteParsedPipeline.hasChangeStream())) {
        // This pipeline is currently empty, but once completed it will have only one source,
        // which is a DocumentSourceCursor. Instead of creating a whole pipeline to do nothing
        // more than forward the results of its cursor document source, we can use the
        // PlanExecutor by itself. The resulting cursor will look like what the client would
        // have gotten from find command.
        execs.emplace_back(std::move(attachExecutorCallback.second));
    } else {
        getSearchHelpers(expCtx->opCtx->getServiceContext())
            ->injectSearchShardFiltererIfNeeded(pipeline.get());
        // Complete creation of the initial $cursor stage, if needed.
        PipelineD::attachInnerQueryExecutorToPipeline(collections,
                                                      attachExecutorCallback.first,
                                                      std::move(attachExecutorCallback.second),
                                                      pipeline.get());

        auto pipelines = createAdditionalPipelinesIfNeeded(
            expCtx->opCtx, expCtx, request, std::move(pipeline), expCtx->uuid, resetContextFn);
        for (auto&& pipelineIt : pipelines) {
            // There are separate ExpressionContexts for each exchange pipeline, so make sure to
            // pass the pipeline's ExpressionContext to the plan executor factory.
            auto pipelineExpCtx = pipelineIt->getContext();
            execs.emplace_back(
                plan_executor_factory::make(std::move(pipelineExpCtx),
                                            std::move(pipelineIt),
                                            aggregation_request_helper::getResumableScanType(
                                                request, liteParsedPipeline.hasChangeStream())));
        }

        // With the pipelines created, we can relinquish locks as they will manage the locks
        // internally further on. We still need to keep the lock for an optimized away pipeline
        // though, as we will be changing its lock policy to 'kLockExternally' (see details
        // below), and in order to execute the initial getNext() call in 'handleCursorCommand',
        // we need to hold the collection lock.
        resetContextFn();
    }
    return execs;
}

Status _runAggregate(OperationContext* opCtx,
                     AggregateCommandRequest& request,
                     const LiteParsedPipeline& liteParsedPipeline,
                     const BSONObj& cmdObj,
                     const PrivilegeVector& privileges,
                     rpc::ReplyBuilderInterface* result,
                     std::shared_ptr<ExternalDataSourceScopeGuard> externalDataSourceGuard,
                     boost::optional<const ResolvedView&> resolvedView,
                     boost::optional<const AggregateCommandRequest&> origRequest);

Status runAggregateOnView(OperationContext* opCtx,
                          const NamespaceString& origNss,
                          const AggregateCommandRequest& request,
                          const MultipleCollectionAccessor& collections,
                          boost::optional<std::unique_ptr<CollatorInterface>> collatorToUse,
                          const ViewDefinition* view,
                          std::shared_ptr<const CollectionCatalog> catalog,
                          const PrivilegeVector& privileges,
                          rpc::ReplyBuilderInterface* result,
                          const std::function<void(void)>& resetContextFn) {
    auto nss = request.getNamespace();

    uassert(ErrorCodes::CommandNotSupportedOnView,
            "mapReduce on a view is not supported",
            !request.getIsMapReduceCommand());

    // Check that the default collation of 'view' is compatible with the operation's
    // collation. The check is skipped if the request did not specify a collation.
    if (!request.getCollation().get_value_or(BSONObj()).isEmpty()) {
        invariant(collatorToUse);  // Should already be resolved at this point.
        if (!CollatorInterface::collatorsMatch(view->defaultCollator(), collatorToUse->get()) &&
            !view->timeseries()) {

            return {ErrorCodes::OptionNotSupportedOnView,
                    "Cannot override a view's default collation"};
        }
    }

    // Queries on timeseries views may specify non-default collation whereas queries
    // on all other types of views must match the default collator (the collation use
    // to originally create that collections). Thus in the case of operations on TS
    // views, we use the request's collation.
    auto timeSeriesCollator = view->timeseries() ? request.getCollation() : boost::none;

    auto resolvedView =
        uassertStatusOK(view_catalog_helpers::resolveView(opCtx, catalog, nss, timeSeriesCollator));

    // With the view & collation resolved, we can relinquish locks.
    resetContextFn();

    // Set this operation's shard version for the underlying collection to unsharded.
    // This is prerequisite for future shard versioning checks.
    boost::optional<ScopedSetShardRole> scopeSetShardRole;
    if (!serverGlobalParams.clusterRole.has(ClusterRole::None)) {
        scopeSetShardRole.emplace(opCtx,
                                  resolvedView.getNamespace(),
                                  ShardVersion::UNSHARDED() /* shardVersion */,
                                  boost::none /* databaseVersion */);
    };
    uassert(std::move(resolvedView),
            "Explain of a resolved view must be executed by mongos",
            !ShardingState::get(opCtx)->enabled() || !request.getExplain());

    // Parse the resolved view into a new aggregation request.
    auto newRequest = resolvedView.asExpandedViewAggregation(request);
    auto newCmd = aggregation_request_helper::serializeToCommandObj(newRequest);

    auto status{Status::OK()};
    try {
        status = _runAggregate(opCtx,
                               newRequest,
                               {newRequest},
                               newCmd,
                               privileges,
                               result,
                               nullptr,
                               resolvedView,
                               request);
    } catch (const ExceptionForCat<ErrorCategory::StaleShardVersionError>& ex) {
        // Since we expect the view to be UNSHARDED, if we reached to this point there are
        // two possibilities:
        //   1. The shard doesn't know what its shard version/state is and needs to recover
        //      it (in which case we throw so that the shard can run recovery)
        //   2. The collection references by the view is actually SHARDED, in which case the
        //      router must execute it
        if (const auto staleInfo{ex.extraInfo<StaleConfigInfo>()}) {
            uassert(std::move(resolvedView),
                    "Resolved views on sharded collections must be executed by mongos",
                    !staleInfo->getVersionWanted());
        }
        throw;
    }

    {
        // Set the namespace of the curop back to the view namespace so ctx records
        // stats on this view namespace on destruction.
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setNS_inlock(nss);
    }

    return status;
}

/**
 * Determines the collection type of the query by precedence of various configurations. The order
 * of these checks is critical since there may be overlap (e.g., a view over a virtual collection
 * is classified as a view).
 */
query_shape::CollectionType determineCollectionType(
    const boost::optional<AutoGetCollectionForReadCommandMaybeLockFree>& ctx,
    boost::optional<const ResolvedView&> resolvedView,
    bool hasChangeStream,
    bool isCollectionless) {
    if (resolvedView.has_value()) {
        if (resolvedView->timeseries()) {
            return query_shape::CollectionType::kTimeseries;
        }
        return query_shape::CollectionType::kView;
    }
    if (isCollectionless) {
        return query_shape::CollectionType::kVirtual;
    }
    if (hasChangeStream) {
        return query_shape::CollectionType::kChangeStream;
    }
    return ctx ? ctx->getCollectionType() : query_shape::CollectionType::kUnknown;
}

std::unique_ptr<Pipeline, PipelineDeleter> parsePipelineAndRegisterQueryStats(
    OperationContext* opCtx,
    const NamespaceString& origNss,
    const AggregateCommandRequest& request,
    const boost::optional<AutoGetCollectionForReadCommandMaybeLockFree>& ctx,
    std::unique_ptr<CollatorInterface> collator,
    boost::optional<UUID> uuid,
    ExpressionContext::CollationMatchesDefault collationMatchesDefault,
    const MultipleCollectionAccessor& collections,
    stdx::unordered_set<NamespaceString> pipelineInvolvedNamespaces,
    bool hasChangeStream,
    bool isCollectionless,
    boost::optional<const ResolvedView&> resolvedView,
    boost::optional<const AggregateCommandRequest&> origRequest) {
    // If we're operating over a view, we first parse just the original user-given request
    // for the sake of registering query stats. Then, we'll parse the view pipeline and stitch
    // the two pipelines together below.
    auto expCtx =
        makeExpressionContext(opCtx, request, std::move(collator), uuid, collationMatchesDefault);
    // If any involved collection contains extended-range data, set a flag which individual
    // DocumentSource parsers can check.
    collections.forEach([&](const CollectionPtr& coll) {
        if (coll->getRequiresTimeseriesExtendedRangeSupport())
            expCtx->setRequiresTimeseriesExtendedRangeSupport(true);
    });

    auto requestForQueryStats = origRequest.has_value() ? *origRequest : request;
    expCtx->startExpressionCounters();
    auto pipeline = Pipeline::parse(requestForQueryStats.getPipeline(), expCtx);
    expCtx->stopExpressionCounters();

    // Register query stats with the pre-optimized pipeline. Exclude queries against collections
    // with encrypted fields. We still collect query stats on collection-less aggregations.
    bool hasEncryptedFields = ctx && ctx->getCollection() &&
        ctx->getCollection()->getCollectionOptions().encryptedFieldConfig;
    if (!hasEncryptedFields) {
        // If this is a query over a resolved view, we want to register query stats with the
        // original user-given request and pipeline, rather than the new request generated when
        // resolving the view.
        auto collectionType =
            determineCollectionType(ctx, resolvedView, hasChangeStream, isCollectionless);

        query_stats::registerRequest(opCtx, origNss, [&]() {
            return std::make_unique<query_stats::AggKey>(requestForQueryStats,
                                                         *pipeline,
                                                         expCtx,
                                                         pipelineInvolvedNamespaces,
                                                         origNss,
                                                         collectionType);
        });
    }

    if (resolvedView.has_value()) {
        expCtx->startExpressionCounters();

        if (resolvedView->timeseries()) {
            // For timeseries, there may have been rewrites done on the raw BSON pipeline
            // during view resolution. We must parse the request's full resolved pipeline
            // which will account for those rewrites.
            // TODO SERVER-82101 Re-organize timeseries rewrites so timeseries can follow the
            // same pattern here as other views
            pipeline = Pipeline::parse(request.getPipeline(), expCtx);
        } else {
            // Parse the view pipeline, then stitch the user pipeline and view pipeline together
            // to build the total aggregation pipeline.
            auto userPipeline = std::move(pipeline);
            pipeline = Pipeline::parse(resolvedView->getPipeline(), expCtx);
            pipeline->appendPipeline(std::move(userPipeline));
        }

        expCtx->stopExpressionCounters();
    }

    // After parsing to detect if $$USER_ROLES is referenced in the query, set the value of
    // $$USER_ROLES for the aggregation.
    expCtx->setUserRoles();
    return pipeline;
}


Status _runAggregate(OperationContext* opCtx,
                     AggregateCommandRequest& request,
                     const LiteParsedPipeline& liteParsedPipeline,
                     const BSONObj& cmdObj,
                     const PrivilegeVector& privileges,
                     rpc::ReplyBuilderInterface* result,
                     std::shared_ptr<ExternalDataSourceScopeGuard> externalDataSourceGuard,
                     boost::optional<const ResolvedView&> resolvedView,
                     boost::optional<const AggregateCommandRequest&> origRequest) {
    auto origNss = origRequest.has_value() ? origRequest->getNamespace() : request.getNamespace();
    // Perform some validations on the LiteParsedPipeline and request before continuing with the
    // aggregation command.
    performValidationChecks(opCtx, request, liteParsedPipeline);

    // If we are running a retryable write without shard key, check if the write was applied on this
    // shard, and if so, return early with an empty cursor with $_wasStatementExecuted
    // set to true. The isRetryableWrite() check here is to check that the client executed write was
    // a retryable write (which would've spawned an internal session for a retryable write to
    // execute the two phase write without shard key protocol), otherwise we skip the retryable
    // write check.
    auto isClusterQueryWithoutShardKeyCmd = request.getIsClusterQueryWithoutShardKeyCmd();
    if (opCtx->isRetryableWrite() && isClusterQueryWithoutShardKeyCmd) {
        auto stmtId = request.getStmtId();
        tassert(7058100, "StmtId must be set for a retryable write without shard key", stmtId);
        if (TransactionParticipant::get(opCtx).checkStatementExecuted(opCtx, *stmtId)) {
            CursorResponseBuilder::Options options;
            options.isInitialResponse = true;
            CursorResponseBuilder responseBuilder(result, options);
            responseBuilder.setWasStatementExecuted(true);
            responseBuilder.done(0LL, origNss);
            return Status::OK();
        }
    }

    // For operations on views, this will be the underlying namespace.
    NamespaceString nss = request.getNamespace();

    // Determine if this aggregation has foreign collections that the execution subsystem needs
    // to be aware of.
    std::vector<NamespaceStringOrUUID> secondaryExecNssList =
        liteParsedPipeline.getForeignExecutionNamespaces();

    // The collation to use for this aggregation. boost::optional to distinguish between the case
    // where the collation has not yet been resolved, and where it has been resolved to nullptr.
    boost::optional<std::unique_ptr<CollatorInterface>> collatorToUse;
    ExpressionContext::CollationMatchesDefault collatorToUseMatchesDefault;

    // The UUID of the collection for the execution namespace of this aggregation.
    boost::optional<UUID> uuid;

    // If emplaced, AutoGetCollectionForReadCommand will throw if the sharding version for this
    // connection is out of date. If the namespace is a view, the lock will be released before
    // re-running the expanded aggregation.
    boost::optional<AutoGetCollectionForReadCommandMaybeLockFree> ctx;
    MultipleCollectionAccessor collections;

    // Going forward this operation must never ignore interrupt signals while waiting for lock
    // acquisition. This InterruptibleLockGuard will ensure that waiting for lock re-acquisition
    // after yielding will not ignore interrupt signals. This is necessary to avoid deadlocking with
    // replication rollback, which at the storage layer waits for all cursors to be closed under the
    // global MODE_X lock, after having sent interrupt signals to read operations. This operation
    // must never hold open storage cursors while ignoring interrupt.
    InterruptibleLockGuard interruptibleLockAcquisition(opCtx->lockState());

    auto initContext = [&](auto_get_collection::ViewMode m) -> void {
        ctx.emplace(
            opCtx,
            nss,
            AutoGetCollection::Options{}.viewMode(m).secondaryNssOrUUIDs(secondaryExecNssList),
            AutoStatsTracker::LogMode::kUpdateTopAndCurOp);
        collections = MultipleCollectionAccessor(opCtx,
                                                 &ctx->getCollection(),
                                                 ctx->getNss(),
                                                 ctx->isAnySecondaryNamespaceAViewOrSharded(),
                                                 secondaryExecNssList);
    };

    auto resetContext = [&]() -> void {
        ctx.reset();
        collections.clear();
    };

    std::vector<unique_ptr<PlanExecutor, PlanExecutor::Deleter>> execs;
    boost::intrusive_ptr<ExpressionContext> expCtx;
    auto curOp = CurOp::get(opCtx);
    auto catalog = CollectionCatalog::get(opCtx);

    {
        // If we are in a transaction, check whether the parsed pipeline supports being in
        // a transaction and if the transaction's read concern is supported.
        if (opCtx->inMultiDocumentTransaction()) {
            liteParsedPipeline.assertSupportsMultiDocumentTransaction(request.getExplain());
            liteParsedPipeline.assertSupportsReadConcern(opCtx, request.getExplain());
        }

        const auto& pipelineInvolvedNamespaces = liteParsedPipeline.getInvolvedNamespaces();

        // If this is a collectionless aggregation, we won't create 'ctx' but will still need an
        // AutoStatsTracker to record CurOp and Top entries.
        boost::optional<AutoStatsTracker> statsTracker;

        // If this is a change stream, perform special checks and change the execution namespace.
        if (liteParsedPipeline.hasChangeStream()) {
            uassert(4928900,
                    str::stream() << AggregateCommandRequest::kCollectionUUIDFieldName
                                  << " is not supported for a change stream",
                    !request.getCollectionUUID());

            // Replace the execution namespace with the oplog.
            nss = NamespaceString::kRsOplogNamespace;

            // In case of serverless the change stream will be opened on the change collection.
            if (change_stream_serverless_helpers::isServerlessEnvironment()) {
                const auto tenantId =
                    change_stream_serverless_helpers::resolveTenantId(origNss.tenantId());

                uassert(ErrorCodes::BadValue,
                        "Change streams cannot be used without tenant id",
                        tenantId);

                uassert(ErrorCodes::ChangeStreamNotEnabled,
                        "Change streams must be enabled before being used.",
                        change_stream_serverless_helpers::isChangeStreamEnabled(opCtx, *tenantId));


                nss = NamespaceString::makeChangeCollectionNSS(tenantId);
            }

            // Assert that a change stream on the config server is always opened on the oplog.
            tassert(
                6763400,
                str::stream() << "Change stream was unexpectedly opened on the namespace: " << nss
                              << " in the config server",
                !serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer) || nss.isOplog());

            // Upgrade and wait for read concern if necessary.
            _adjustChangeStreamReadConcern(opCtx);

            // Raise an error if 'origNss' is a view. We do not need to check this if we are opening
            // a stream on an entire db or across the cluster.
            if (!origNss.isCollectionlessAggregateNS()) {
                auto view = catalog->lookupView(opCtx, origNss);
                uassert(ErrorCodes::CommandNotSupportedOnView,
                        str::stream() << "Cannot run aggregation on timeseries with namespace "
                                      << origNss.ns(),
                        !view || !view->timeseries());
                uassert(ErrorCodes::CommandNotSupportedOnView,
                        str::stream()
                            << "Namespace " << origNss.ns() << " is a view, not a collection",
                        !view);
            }

            // If the user specified an explicit collation, adopt it; otherwise, use the simple
            // collation. We do not inherit the collection's default collation or UUID, since
            // the stream may be resuming from a point before the current UUID existed.
            auto [collator, match] = PipelineD::resolveCollator(
                opCtx, request.getCollation().get_value_or(BSONObj()), CollectionPtr());
            collatorToUse.emplace(std::move(collator));
            collatorToUseMatchesDefault = match;

            // Obtain collection locks on the execution namespace; that is, the oplog.
            initContext(auto_get_collection::ViewMode::kViewsForbidden);
        } else if (nss.isCollectionlessAggregateNS() && pipelineInvolvedNamespaces.empty()) {
            uassert(4928901,
                    str::stream() << AggregateCommandRequest::kCollectionUUIDFieldName
                                  << " is not supported for a collectionless aggregation",
                    !request.getCollectionUUID());

            // If this is a collectionless agg with no foreign namespaces, don't acquire any locks.
            statsTracker.emplace(opCtx,
                                 nss,
                                 Top::LockType::NotLocked,
                                 AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                 catalog->getDatabaseProfileLevel(nss.dbName()));
            auto [collator, match] = PipelineD::resolveCollator(
                opCtx, request.getCollation().get_value_or(BSONObj()), CollectionPtr());
            collatorToUse.emplace(std::move(collator));
            collatorToUseMatchesDefault = match;
            tassert(6235101,
                    "A collection-less aggregate should not take any locks",
                    ctx == boost::none);
        } else {
            // This is a regular aggregation. Lock the collection or view.
            initContext(auto_get_collection::ViewMode::kViewsPermitted);
            auto [collator, match] =
                PipelineD::resolveCollator(opCtx,
                                           request.getCollation().get_value_or(BSONObj()),
                                           collections.getMainCollection());
            collatorToUse.emplace(std::move(collator));
            collatorToUseMatchesDefault = match;
            if (collections.hasMainCollection()) {
                uuid = collections.getMainCollection()->uuid();
            }
        }

        // If collectionUUID was provided, verify the collection exists and has the expected UUID.
        checkCollectionUUIDMismatch(
            opCtx, nss, collections.getMainCollection(), request.getCollectionUUID());

        // If this is a view, resolve it by finding the underlying collection and stitching view
        // pipelines and this request's pipeline together. We then release our locks before
        // recursively calling runAggregate(), which will re-acquire locks on the underlying
        // collection.  (The lock must be released because recursively acquiring locks on the
        // database will prohibit yielding.)
        // We do not need to expand the view pipeline when there is a $collStats stage, as
        // $collStats is supported on a view namespace. For a time-series collection, however, the
        // view is abstracted out for the users, so we needed to resolve the namespace to get the
        // underlying bucket collection.
        if (ctx && ctx->getView() &&
            (!liteParsedPipeline.startsWithCollStats() || ctx->getView()->timeseries())) {
            return runAggregateOnView(opCtx,
                                      origNss,
                                      request,
                                      collections,
                                      std::move(collatorToUse),
                                      ctx->getView(),
                                      catalog,
                                      privileges,
                                      result,
                                      resetContext);
        }

        invariant(collatorToUse);
        auto pipeline = parsePipelineAndRegisterQueryStats(opCtx,
                                                           origNss,
                                                           request,
                                                           ctx,
                                                           std::move(*collatorToUse),
                                                           uuid,
                                                           collatorToUseMatchesDefault,
                                                           collections,
                                                           pipelineInvolvedNamespaces,
                                                           liteParsedPipeline.hasChangeStream(),
                                                           nss.isCollectionlessAggregateNS(),
                                                           resolvedView,
                                                           origRequest);
        expCtx = pipeline->getContext();

        CurOp::get(opCtx)->beginQueryPlanningTimer();

        if (!request.getAllowDiskUse().value_or(true)) {
            allowDiskUseFalseCounter.increment();
        }

        // Check that the view's collation matches the collation of any views involved in the
        // pipeline.
        if (!pipelineInvolvedNamespaces.empty()) {
            auto pipelineCollationStatus =
                collatorCompatibleWithPipeline(opCtx, expCtx->getCollator(), liteParsedPipeline);
            if (!pipelineCollationStatus.isOK()) {
                return pipelineCollationStatus;
            }
        }

        // If the aggregate command supports encrypted collections, do rewrites of the pipeline to
        // support querying against encrypted fields.
        if (shouldDoFLERewrite(request)) {
            {
                stdx::lock_guard<Client> lk(*opCtx->getClient());
                CurOp::get(opCtx)->setShouldOmitDiagnosticInformation_inlock(lk, true);
            }

            if (!request.getEncryptionInformation()->getCrudProcessed().value_or(false)) {
                pipeline = processFLEPipelineD(
                    opCtx, nss, request.getEncryptionInformation().value(), std::move(pipeline));
                request.getEncryptionInformation()->setCrudProcessed(true);
            }
        }

        pipeline->optimizePipeline();

        constexpr bool alreadyOptimized = true;
        pipeline->validateCommon(alreadyOptimized);

        if (auto sampleId = analyze_shard_key::getOrGenerateSampleId(
                opCtx,
                expCtx->ns,
                analyze_shard_key::SampledCommandNameEnum::kAggregate,
                request)) {
            analyze_shard_key::QueryAnalysisWriter::get(opCtx)
                ->addAggregateQuery(*sampleId,
                                    expCtx->ns,
                                    pipeline->getInitialQuery(),
                                    expCtx->getCollatorBSON(),
                                    request.getLet())
                .getAsync([](auto) {});
        }

        execs = createExecutor(std::move(pipeline),
                               liteParsedPipeline,
                               nss,
                               collections,
                               request,
                               curOp,
                               resetContext);
        tassert(6624353, "No executors", !execs.empty());

        {
            auto planSummary = execs[0]->getPlanExplainer().getPlanSummary();
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            curOp->setPlanSummary_inlock(std::move(planSummary));
            curOp->debug().queryFramework = execs[0]->getQueryFramework();
        }
    }

    // Having released the collection lock, we can now create a cursor that returns results from the
    // pipeline. This cursor owns no collection state, and thus we register it with the global
    // cursor manager. The global cursor manager does not deliver invalidations or kill
    // notifications; the underlying PlanExecutor(s) used by the pipeline will be receiving
    // invalidations and kill notifications themselves, not the cursor we create here.
    hangAfterCreatingAggregationPlan.executeIf(
        [](const auto&) { hangAfterCreatingAggregationPlan.pauseWhileSet(); },
        [&](const BSONObj& data) { return uuid && UUID::parse(data["uuid"]) == *uuid; });

    std::vector<ClientCursorPin> pins;
    std::vector<ClientCursor*> cursors;

    ScopeGuard cursorFreer([&] {
        for (auto& p : pins) {
            p.deleteUnderlying();
        }
    });

    // We disallowed external data sources in queries with multiple plan executors due to a data
    // race (see SERVER-85453 for more details).
    tassert(8545301,
            "External data sources are not currently compatible with queries that use multiple "
            "plan executors.",
            externalDataSourceGuard == nullptr || execs.size() == 1);
    for (auto&& exec : execs) {
        // TODO SERVER-79373: Do not create a cursor if results can fit in a single batch.
        ClientCursorParams cursorParams(
            std::move(exec),
            origNss,
            AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserName(),
            APIParameters::get(opCtx),
            opCtx->getWriteConcern(),
            repl::ReadConcernArgs::get(opCtx),
            ReadPreferenceSetting::get(opCtx),
            cmdObj,
            privileges);
        cursorParams.setTailableMode(expCtx->tailableMode);

        auto pin = CursorManager::get(opCtx)->registerCursor(opCtx, std::move(cursorParams));

        pin->incNBatches();
        cursors.emplace_back(pin.getCursor());
        if (externalDataSourceGuard) {
            ExternalDataSourceScopeGuard::get(pin.getCursor()) = externalDataSourceGuard;
        }
        pins.emplace_back(std::move(pin));
    }

    // Report usage statistics for each stage in the pipeline.
    liteParsedPipeline.tickGlobalStageCounters();

    // If both explain and cursor are specified, explain wins.
    if (expCtx->explain) {
        auto explainExecutor = pins[0]->getExecutor();
        auto bodyBuilder = result->getBodyBuilder();
        if (auto pipelineExec = dynamic_cast<PlanExecutorPipeline*>(explainExecutor)) {
            Explain::explainPipeline(
                pipelineExec, true /* executePipeline */, *(expCtx->explain), cmdObj, &bodyBuilder);
        } else {
            invariant(explainExecutor->getOpCtx() == opCtx);
            // The explainStages() function for a non-pipeline executor may need to execute the plan
            // to collect statistics. If the PlanExecutor uses kLockExternally policy, the
            // appropriate collection lock must be already held. Make sure it has not been released
            // yet.
            invariant(ctx);
            Explain::explainStages(explainExecutor,
                                   collections,
                                   *(expCtx->explain),
                                   BSON("optimizedPipeline" << true),
                                   cmdObj,
                                   &bodyBuilder);
        }
    } else {
        // Cursor must be specified, if explain is not.
        const bool keepCursor = handleCursorCommand(
            opCtx, expCtx, origNss, std::move(cursors), request, cmdObj, result);
        if (keepCursor) {
            cursorFreer.dismiss();
        }

        const auto& planExplainer = pins[0].getCursor()->getExecutor()->getPlanExplainer();
        PlanSummaryStats stats;
        planExplainer.getSummaryStats(&stats);
        curOp->debug().setPlanSummaryMetrics(stats);
        curOp->setEndOfOpMetrics(stats.nReturned);

        collectQueryStatsMongod(opCtx, pins[0]);

        // For an optimized away pipeline, signal the cache that a query operation has completed.
        // For normal pipelines this is done in DocumentSourceCursor.
        if (ctx) {
            // Due to yielding, the collection pointers saved in MultipleCollectionAccessor might
            // have become invalid. We will need to refresh them here.
            collections = MultipleCollectionAccessor(opCtx,
                                                     &ctx->getCollection(),
                                                     ctx->getNss(),
                                                     ctx->isAnySecondaryNamespaceAViewOrSharded(),
                                                     secondaryExecNssList);

            if (const auto& coll = ctx->getCollection()) {
                CollectionQueryInfo::get(coll).notifyOfQuery(opCtx, coll, stats);
            }
            // For SBE pushed down pipelines, we may need to report stats saved for secondary
            // collections separately.
            for (const auto& [secondaryNss, coll] : collections.getSecondaryCollections()) {
                if (coll) {
                    PlanSummaryStats secondaryStats;
                    planExplainer.getSecondarySummaryStats(secondaryNss.toString(),
                                                           &secondaryStats);
                    CollectionQueryInfo::get(coll).notifyOfQuery(opCtx, coll, secondaryStats);
                }
            }
        }
    }

    // The aggregation pipeline may change the namespace of the curop and we need to set it back to
    // the original namespace to correctly report command stats. One example when the namespace can
    // be changed is when the pipeline contains an $out stage, which executes an internal command to
    // create a temp collection, changing the curop namespace to the name of this temp collection.
    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        curOp->setNS_inlock(origNss);
    }

    return Status::OK();
}
}  // namespace

Status runAggregate(OperationContext* opCtx,
                    AggregateCommandRequest& request,
                    const BSONObj& cmdObj,
                    const PrivilegeVector& privileges,
                    rpc::ReplyBuilderInterface* result,
                    boost::optional<const ResolvedView&> resolvedView,
                    boost::optional<const AggregateCommandRequest&> origRequest) {
    return _runAggregate(
        opCtx, request, {request}, cmdObj, privileges, result, nullptr, resolvedView, origRequest);
}

Status runAggregate(
    OperationContext* opCtx,
    AggregateCommandRequest& request,
    const LiteParsedPipeline& liteParsedPipeline,
    const BSONObj& cmdObj,
    const PrivilegeVector& privileges,
    rpc::ReplyBuilderInterface* result,
    const std::vector<std::pair<NamespaceString, std::vector<ExternalDataSourceInfo>>>&
        usedExternalDataSources,
    boost::optional<const ResolvedView&> resolvedView,
    boost::optional<const AggregateCommandRequest&> origRequest) {
    // Create virtual collections and drop them when aggregate command is done.
    // If a cursor is registered, the ExternalDataSourceScopeGuard will be stored in the cursor;
    // when the cursor is later destroyed, the scope guard will also be destroyed, and any virtual
    // collections will be dropped by the destructor of ExternalDataSourceScopeGuard.
    // We create this scope guard prior to taking locks in _runAggregate so that, if no cursor is
    // registered, the virtual collections will be dropped after releasing our read locks, avoiding
    // a lock upgrade.
    auto extDataSrcGuard = usedExternalDataSources.size() > 0
        ? std::make_shared<ExternalDataSourceScopeGuard>(opCtx, usedExternalDataSources)
        : nullptr;
    return _runAggregate(opCtx,
                         request,
                         liteParsedPipeline,
                         cmdObj,
                         privileges,
                         result,
                         extDataSrcGuard,
                         resolvedView,
                         origRequest);
}

}  // namespace mongo
