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

#include "mongo/db/query/cqf_get_executor.h"

#include "mongo/db/curop.h"
#include "mongo/db/exec/sbe/abt/abt_lower.h"
#include "mongo/db/pipeline/abt/canonical_query_translation.h"
#include "mongo/db/pipeline/abt/document_source_visitor.h"
#include "mongo/db/pipeline/abt/match_expression_visitor.h"
#include "mongo/db/pipeline/abt/utils.h"
#include "mongo/db/query/ce/ce_histogram.h"
#include "mongo/db/query/ce/ce_sampling.h"
#include "mongo/db/query/ce/collection_statistics_impl.h"
#include "mongo/db/query/ce_mode_parameter.h"
#include "mongo/db/query/cqf_command_utils.h"
#include "mongo/db/query/optimizer/cascades/ce_heuristic.h"
#include "mongo/db/query/optimizer/cascades/cost_derivation.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/node.h"
#include "mongo/db/query/optimizer/opt_phase_manager.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/sbe_stage_builder.h"
#include "mongo/db/query/yield_policy_callbacks_impl.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

using namespace optimizer;

static opt::unordered_map<std::string, optimizer::IndexDefinition> buildIndexSpecsOptimizer(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const boost::optional<BSONObj>& indexHint,
    const optimizer::ProjectionName& scanProjName,
    const DisableIndexOptions disableIndexOptions,
    bool& disableScan) {
    using namespace optimizer;

    if (disableIndexOptions == DisableIndexOptions::DisableAll) {
        return {};
    }

    std::string indexHintName;
    if (indexHint) {
        const BSONElement element = indexHint->firstElement();
        const StringData fieldName = element.fieldNameStringData();
        if (fieldName == "$natural"_sd) {
            if (!element.isNumber() || element.numberInt() != 1) {
                uasserted(6624255, "Unsupported hint option");
            }
            // Do not add indexes.
            return {};
        } else if (fieldName == "$hint"_sd && element.type() == BSONType::String) {
            indexHintName = element.valueStringData().toString();
        }

        disableScan = true;
    }

    const IndexCatalog& indexCatalog = *collection->getIndexCatalog();
    opt::unordered_map<std::string, IndexDefinition> result;
    auto indexIterator =
        indexCatalog.getIndexIterator(opCtx, IndexCatalog::InclusionPolicy::kReady);

    while (indexIterator->more()) {
        const IndexCatalogEntry& catalogEntry = *indexIterator->next();
        const IndexDescriptor& descriptor = *catalogEntry.descriptor();

        if (descriptor.hidden()) {
            // Index is hidden; don't consider it.
            continue;
        }

        if (descriptor.isSparse() || descriptor.getIndexType() != IndexType::INDEX_BTREE ||
            !descriptor.collation().isEmpty()) {
            uasserted(ErrorCodes::InternalErrorNotSupported, "Unsupported index type");
        }

        if (indexHint) {
            if (indexHintName.empty()) {
                if (!SimpleBSONObjComparator::kInstance.evaluate(descriptor.keyPattern() ==
                                                                 *indexHint)) {
                    // Index key pattern does not match hint.
                    continue;
                }
            } else if (indexHintName != descriptor.indexName()) {
                // Index name does not match hint.
                continue;
            }
        }

        const bool isMultiKey = catalogEntry.isMultikey(opCtx, collection);
        const MultikeyPaths& multiKeyPaths = catalogEntry.getMultikeyPaths(opCtx, collection);
        uassert(6624251, "Multikey paths cannot be empty.", !multiKeyPaths.empty());

        // SBE version is base 0.
        const int64_t version = static_cast<int>(descriptor.version()) - 1;

        uint32_t orderingBits = 0;
        {
            const Ordering ordering = catalogEntry.ordering();
            for (int i = 0; i < descriptor.getNumFields(); i++) {
                if ((ordering.get(i) == -1)) {
                    orderingBits |= (1ull << i);
                }
            }
        }

        IndexCollationSpec indexCollationSpec;
        bool useIndex = true;
        size_t elementIdx = 0;
        for (const auto& element : descriptor.keyPattern()) {
            FieldPathType fieldPath;
            FieldPath path(element.fieldName());

            for (size_t i = 0; i < path.getPathLength(); i++) {
                const std::string& fieldName = path.getFieldName(i).toString();
                if (fieldName == "$**") {
                    // TODO: For now disallow wildcard indexes.
                    useIndex = false;
                    break;
                }
                fieldPath.emplace_back(fieldName);
            }
            if (!useIndex) {
                break;
            }

            const int direction = element.numberInt();
            if (direction != -1 && direction != 1) {
                // Invalid value?
                useIndex = false;
                break;
            }

            const CollationOp collationOp =
                (direction == 1) ? CollationOp::Ascending : CollationOp::Descending;

            // Construct an ABT path for each index component (field path).
            const MultikeyComponents& elementMultiKeyInfo = multiKeyPaths[elementIdx];
            ABT abtPath = make<PathIdentity>();
            for (size_t i = fieldPath.size(); i-- > 0;) {
                if (isMultiKey && elementMultiKeyInfo.find(i) != elementMultiKeyInfo.cend()) {
                    // This is a multikey element of the path.
                    abtPath = make<PathTraverse>(std::move(abtPath), PathTraverse::kSingleLevel);
                }
                abtPath = make<PathGet>(fieldPath.at(i), std::move(abtPath));
            }
            indexCollationSpec.emplace_back(std::move(abtPath), collationOp);
            ++elementIdx;
        }
        if (!useIndex) {
            continue;
        }

        PartialSchemaRequirements partialIndexReqMap;
        if (descriptor.isPartial() &&
            disableIndexOptions != DisableIndexOptions::DisablePartialOnly) {
            auto expr = MatchExpressionParser::parseAndNormalize(
                descriptor.partialFilterExpression(),
                expCtx,
                ExtensionsCallbackNoop(),
                MatchExpressionParser::kBanAllSpecialFeatures);

            // We need a non-empty root projection name.
            ABT exprABT = generateMatchExpression(expr.get(),
                                                  false /*allowAggExpression*/,
                                                  "<root>" /*rootProjection*/,
                                                  "" /*uniquePrefix*/);
            exprABT = make<EvalFilter>(std::move(exprABT), make<Variable>(scanProjName));

            // TODO: simplify expression.

            auto conversion = convertExprToPartialSchemaReq(
                exprABT, true /*isFilterContext*/, {} /*pathToIntervalFn*/);
            if (!conversion) {
                // TODO: should this conversion be always possible?
                continue;
            }
            tassert(6624257,
                    "Should not be seeing a partial index filter where we need to over-approximate",
                    !conversion->_retainPredicate);

            partialIndexReqMap = std::move(conversion->_reqMap);
        }

        // For now we assume distribution is Centralized.
        result.emplace(descriptor.indexName(),
                       IndexDefinition(std::move(indexCollationSpec),
                                       version,
                                       orderingBits,
                                       isMultiKey,
                                       DistributionType::Centralized,
                                       std::move(partialIndexReqMap)));
    }

    return result;
}

static QueryHints getHintsFromQueryKnobs() {
    QueryHints hints;

    hints._disableScan = internalCascadesOptimizerDisableScan.load();
    hints._disableIndexes = internalCascadesOptimizerDisableIndexes.load()
        ? DisableIndexOptions::DisableAll
        : DisableIndexOptions::Enabled;
    hints._disableHashJoinRIDIntersect =
        internalCascadesOptimizerDisableHashJoinRIDIntersect.load();
    hints._disableMergeJoinRIDIntersect =
        internalCascadesOptimizerDisableMergeJoinRIDIntersect.load();
    hints._disableGroupByAndUnionRIDIntersect =
        internalCascadesOptimizerDisableGroupByAndUnionRIDIntersect.load();
    hints._keepRejectedPlans = internalCascadesOptimizerKeepRejectedPlans.load();
    hints._disableBranchAndBound = internalCascadesOptimizerDisableBranchAndBound.load();
    hints._fastIndexNullHandling = internalCascadesOptimizerFastIndexNullHandling.load();
    hints._disableYieldingTolerantPlans =
        internalCascadesOptimizerDisableYieldingTolerantPlans.load();

    return hints;
}

static std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> optimizeAndCreateExecutor(
    OptPhaseManager& phaseManager,
    ABT abt,
    OperationContext* opCtx,
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const NamespaceString& nss,
    const CollectionPtr& collection,
    std::unique_ptr<CanonicalQuery> cq,
    const bool requireRID) {

    phaseManager.optimize(abt);

    {
        const auto& memo = phaseManager.getMemo();
        const auto& memoStats = memo.getStats();
        OPTIMIZER_DEBUG_LOG(6264800,
                            5,
                            "Optimizer stats",
                            "memoGroups"_attr = memo.getGroupCount(),
                            "memoLogicalNodes"_attr = memo.getLogicalNodeCount(),
                            "memoPhysNodes"_attr = memo.getPhysicalNodeCount(),
                            "memoIntegrations"_attr = memoStats._numIntegrations,
                            "physPlansExplored"_attr = memoStats._physPlanExplorationCount,
                            "physMemoChecks"_attr = memoStats._physMemoCheckCount);
    }

    {
        const std::string explain = ExplainGenerator::explainV2Compact(
            make<MemoPhysicalDelegatorNode>(phaseManager.getPhysicalNodeId()),
            true /*displayPhysicalProperties*/,
            &phaseManager.getMemo());
        OPTIMIZER_DEBUG_LOG(6264801, 5, "Optimized ABT", "explain"_attr = explain);
    }

    auto env = VariableEnvironment::build(abt);
    SlotVarMap slotMap;
    sbe::value::SlotIdGenerator ids;
    boost::optional<sbe::value::SlotId> ridSlot;
    SBENodeLowering g{env,
                      slotMap,
                      ridSlot,
                      ids,
                      phaseManager.getMetadata(),
                      phaseManager.getNodeToGroupPropsMap(),
                      phaseManager.getRIDProjections(),
                      false /*randomScan*/};
    auto sbePlan = g.optimize(abt);
    tassert(6624262, "Unexpected rid slot", !requireRID || ridSlot);

    uassert(6624253, "Lowering failed: did not produce a plan.", sbePlan != nullptr);
    uassert(6624254, "Lowering failed: did not produce any output slots.", !slotMap.empty());

    {
        sbe::DebugPrinter p;
        OPTIMIZER_DEBUG_LOG(6264802, 5, "Lowered SBE plan", "plan"_attr = p.print(*sbePlan.get()));
    }

    stage_builder::PlanStageData data{std::make_unique<sbe::RuntimeEnvironment>()};
    data.outputs.set(stage_builder::PlanStageSlots::kResult, slotMap.begin()->second);
    if (requireRID) {
        data.outputs.set(stage_builder::PlanStageSlots::kRecordId, *ridSlot);
    }

    sbePlan->attachToOperationContext(opCtx);
    if (expCtx->explain || expCtx->mayDbProfile) {
        sbePlan->markShouldCollectTimingInfo();
    }

    auto yieldPolicy =
        std::make_unique<PlanYieldPolicySBE>(PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                             opCtx->getServiceContext()->getFastClockSource(),
                                             internalQueryExecYieldIterations.load(),
                                             Milliseconds{internalQueryExecYieldPeriodMS.load()},
                                             nullptr,
                                             std::make_unique<YieldPolicyCallbacksImpl>(nss));

    sbePlan->prepare(data.ctx);
    auto planExec = uassertStatusOK(plan_executor_factory::make(
        opCtx,
        std::move(cq),
        nullptr /*solution*/,
        {std::move(sbePlan), std::move(data)},
        std::make_unique<ABTPrinter>(std::move(abt), phaseManager.getNodeToGroupPropsMap()),
        MultipleCollectionAccessor(collection),
        QueryPlannerParams::Options::DEFAULT,
        nss,
        std::move(yieldPolicy)));
    return planExec;
}

static void populateAdditionalScanDefs(
    OperationContext* opCtx,
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const stdx::unordered_set<NamespaceString>& involvedCollections,
    const boost::optional<BSONObj>& indexHint,
    const size_t numberOfPartitions,
    PrefixId& prefixId,
    opt::unordered_map<std::string, ScanDefinition>& scanDefs,
    const DisableIndexOptions disableIndexOptions,
    bool& disableScan) {
    for (const auto& involvedNss : involvedCollections) {
        // TODO handle views?
        AutoGetCollectionForReadCommandMaybeLockFree ctx(
            opCtx, involvedNss, auto_get_collection::ViewMode::kViewsForbidden);
        const CollectionPtr& collection = ctx ? ctx.getCollection() : CollectionPtr::null;
        const bool collectionExists = collection != nullptr;
        const std::string uuidStr =
            collectionExists ? collection->uuid().toString() : "<missing_uuid>";

        const std::string collNameStr = involvedNss.coll().toString();
        // TODO: We cannot add the uuidStr suffix because the pipeline translation does not have
        // access to the metadata so it generates a scan over just the collection name.
        const std::string scanDefName = collNameStr;

        opt::unordered_map<std::string, optimizer::IndexDefinition> indexDefs;
        const ProjectionName& scanProjName = prefixId.getNextId("scan");
        if (collectionExists) {
            // TODO: add locks on used indexes?
            indexDefs = buildIndexSpecsOptimizer(expCtx,
                                                 opCtx,
                                                 collection,
                                                 indexHint,
                                                 scanProjName,
                                                 disableIndexOptions,
                                                 disableScan);
        }

        // For now handle only local parallelism (no over-the-network exchanges).
        DistributionAndPaths distribution{(numberOfPartitions == 1)
                                              ? DistributionType::Centralized
                                              : DistributionType::UnknownPartitioning};

        const CEType collectionCE = collectionExists ? collection->numRecords(opCtx) : -1.0;
        scanDefs[scanDefName] =
            ScanDefinition({{"type", "mongod"},
                            {"database", involvedNss.db().toString()},
                            {"uuid", uuidStr},
                            {ScanNode::kDefaultCollectionNameSpec, collNameStr}},
                           std::move(indexDefs),
                           std::move(distribution),
                           collectionExists,
                           collectionCE);
    }
}

void validateCommandOptions(const CollectionPtr& collection,
                            const boost::optional<BSONObj>& indexHint,
                            const stdx::unordered_set<NamespaceString>& involvedCollections) {
    if (indexHint && !involvedCollections.empty()) {
        uasserted(6624256,
                  "For now we can apply hints only for queries involving a single collection");
    }
    // Unsupported command/collection options.
    uassert(ErrorCodes::InternalErrorNotSupported,
            "Collection-default collation is not supported",
            !collection || collection->getCollectionOptions().collation.isEmpty());

    uassert(ErrorCodes::InternalErrorNotSupported,
            "Clustered collections are not supported",
            !collection || !collection->isClustered());

    uassert(ErrorCodes::InternalErrorNotSupported,
            "Timeseries collections are not supported",
            !collection || !collection->getTimeseriesOptions());
}

Metadata populateMetadata(boost::intrusive_ptr<ExpressionContext> expCtx,
                          const CollectionPtr& collection,
                          const stdx::unordered_set<NamespaceString>& involvedCollections,
                          const NamespaceString& nss,
                          const boost::optional<BSONObj>& indexHint,
                          const ProjectionName& scanProjName,
                          const std::string& uuidStr,
                          const std::string& scanDefName,
                          QueryHints& queryHints,
                          PrefixId& prefixId) {
    auto opCtx = expCtx->opCtx;
    const bool collectionExists = collection != nullptr;

    // Add the base collection metadata.
    opt::unordered_map<std::string, optimizer::IndexDefinition> indexDefs;
    if (collectionExists) {
        // TODO: add locks on used indexes?
        indexDefs = buildIndexSpecsOptimizer(expCtx,
                                             opCtx,
                                             collection,
                                             indexHint,
                                             scanProjName,
                                             queryHints._disableIndexes,
                                             queryHints._disableScan);
    }

    const size_t numberOfPartitions = internalQueryDefaultDOP.load();
    // For now handle only local parallelism (no over-the-network exchanges).
    DistributionAndPaths distribution{(numberOfPartitions == 1)
                                          ? DistributionType::Centralized
                                          : DistributionType::UnknownPartitioning};

    opt::unordered_map<std::string, ScanDefinition> scanDefs;
    const int64_t numRecords = collectionExists ? collection->numRecords(opCtx) : -1;
    scanDefs.emplace(scanDefName,
                     ScanDefinition({{"type", "mongod"},
                                     {"database", nss.db().toString()},
                                     {"uuid", uuidStr},
                                     {ScanNode::kDefaultCollectionNameSpec, nss.coll().toString()}},
                                    std::move(indexDefs),
                                    std::move(distribution),
                                    collectionExists,
                                    static_cast<CEType>(numRecords)));

    // Add a scan definition for all involved collections. Note that the base namespace has already
    // been accounted for above and isn't included here.
    populateAdditionalScanDefs(opCtx,
                               expCtx,
                               involvedCollections,
                               indexHint,
                               numberOfPartitions,
                               prefixId,
                               scanDefs,
                               queryHints._disableIndexes,
                               queryHints._disableScan);

    return {std::move(scanDefs), numberOfPartitions};
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> getSBEExecutorViaCascadesOptimizer(
    OperationContext* opCtx,
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const NamespaceString& nss,
    const CollectionPtr& collection,
    const boost::optional<BSONObj>& indexHint,
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
    std::unique_ptr<CanonicalQuery> canonicalQuery,
    const bool requireRID) {

    // Ensure that either pipeline or canonicalQuery is set.
    tassert(624070,
            "getSBEExecutorViaCascadesOptimizer expects exactly one of the following to be set: "
            "canonicalQuery, pipeline",
            static_cast<bool>(pipeline) != static_cast<bool>(canonicalQuery));

    stdx::unordered_set<NamespaceString> involvedCollections;
    if (pipeline) {
        involvedCollections = pipeline->getInvolvedCollections();
    }

    validateCommandOptions(collection, indexHint, involvedCollections);

    auto curOp = CurOp::get(opCtx);
    curOp->debug().cqfUsed = true;

    const bool collectionExists = collection != nullptr;
    const std::string uuidStr = collectionExists ? collection->uuid().toString() : "<missing_uuid>";
    const std::string collNameStr = nss.coll().toString();
    const std::string scanDefName = collNameStr + "_" + uuidStr;
    PrefixId prefixId;
    const ProjectionName& scanProjName = prefixId.getNextId("scan");
    QueryHints queryHints = getHintsFromQueryKnobs();

    auto metadata = populateMetadata(expCtx,
                                     collection,
                                     involvedCollections,
                                     nss,
                                     indexHint,
                                     scanProjName,
                                     uuidStr,
                                     scanDefName,
                                     queryHints,
                                     prefixId);

    ABT abt = collectionExists
        ? make<ScanNode>(scanProjName, scanDefName)
        : make<ValueScanNode>(ProjectionNameVector{scanProjName},
                              createInitialScanProps(scanProjName, scanDefName));

    if (pipeline) {
        abt = translatePipelineToABT(metadata, *pipeline, scanProjName, std::move(abt), prefixId);
    } else {
        abt = translateCanonicalQueryToABT(
            metadata, *canonicalQuery, scanProjName, std::move(abt), prefixId);
    }

    OPTIMIZER_DEBUG_LOG(
        6264803, 5, "Translated ABT", "explain"_attr = ExplainGenerator::explainV2Compact(abt));

    const int64_t numRecords = collectionExists ? collection->numRecords(opCtx) : -1;

    // TODO SERVER-68919: Move OptPhaseManager construction to its own function.
    if (internalQueryCardinalityEstimatorMode == ce::kSampling && collectionExists &&
        numRecords > 0) {
        Metadata metadataForSampling = metadata;
        // Do not use indexes for sampling.
        for (auto& entry : metadataForSampling._scanDefs) {
            entry.second.getIndexDefs().clear();
        }

        // TODO: consider a limited rewrite set.
        OptPhaseManager phaseManagerForSampling(OptPhaseManager::getAllRewritesSet(),
                                                prefixId,
                                                false /*requireRID*/,
                                                std::move(metadataForSampling),
                                                std::make_unique<HeuristicCE>(),
                                                std::make_unique<DefaultCosting>(),
                                                defaultConvertPathToInterval,
                                                DebugInfo::kDefaultForProd,
                                                {});

        OptPhaseManager phaseManager{
            OptPhaseManager::getAllRewritesSet(),
            prefixId,
            requireRID,
            std::move(metadata),
            std::make_unique<CESamplingTransport>(opCtx, phaseManagerForSampling, numRecords),
            std::make_unique<DefaultCosting>(),
            defaultConvertPathToInterval,
            DebugInfo::kDefaultForProd,
            std::move(queryHints)};

        return optimizeAndCreateExecutor(phaseManager,
                                         std::move(abt),
                                         opCtx,
                                         expCtx,
                                         nss,
                                         collection,
                                         std::move(canonicalQuery),
                                         requireRID);

    } else if (internalQueryCardinalityEstimatorMode == ce::kHistogram) {
        auto ceDerivation =
            std::make_unique<CEHistogramTransport>(std::shared_ptr<ce::CollectionStatistics>(
                new ce::CollectionStatisticsImpl(numRecords, nss)));
        OptPhaseManager phaseManager{OptPhaseManager::getAllRewritesSet(),
                                     prefixId,
                                     requireRID,
                                     std::move(metadata),
                                     std::move(ceDerivation),
                                     std::make_unique<DefaultCosting>(),
                                     defaultConvertPathToInterval,
                                     DebugInfo::kDefaultForProd,
                                     std::move(queryHints)};

        return optimizeAndCreateExecutor(phaseManager,
                                         std::move(abt),
                                         opCtx,
                                         expCtx,
                                         nss,
                                         collection,
                                         std::move(canonicalQuery),
                                         requireRID);
    }
    // Default to using heuristics.
    OptPhaseManager phaseManager{OptPhaseManager::getAllRewritesSet(),
                                 prefixId,
                                 requireRID,
                                 std::move(metadata),
                                 std::make_unique<HeuristicCE>(),
                                 std::make_unique<DefaultCosting>(),
                                 defaultConvertPathToInterval,
                                 DebugInfo::kDefaultForProd,
                                 std::move(queryHints)};

    return optimizeAndCreateExecutor(phaseManager,
                                     std::move(abt),
                                     opCtx,
                                     expCtx,
                                     nss,
                                     collection,
                                     std::move(canonicalQuery),
                                     requireRID);
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> getSBEExecutorViaCascadesOptimizer(
    const CollectionPtr& collection, std::unique_ptr<CanonicalQuery> query, const bool requireRID) {

    boost::optional<BSONObj> indexHint = query->getFindCommandRequest().getHint().isEmpty()
        ? boost::none
        : boost::make_optional(query->getFindCommandRequest().getHint());


    auto opCtx = query->getOpCtx();
    auto expCtx = query->getExpCtx();
    auto nss = query->nss();

    return getSBEExecutorViaCascadesOptimizer(opCtx,
                                              expCtx,
                                              nss,
                                              collection,
                                              indexHint,
                                              nullptr /* pipeline */,
                                              std::move(query),
                                              requireRID);
}

}  // namespace mongo
