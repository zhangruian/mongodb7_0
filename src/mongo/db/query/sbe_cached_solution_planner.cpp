/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/query/sbe_cached_solution_planner.h"

#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/plan_cache_key_factory.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/sbe_multi_planner.h"
#include "mongo/db/query/stage_builder_util.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/logv2/log.h"

namespace mongo::sbe {
CandidatePlans CachedSolutionPlanner::plan(
    std::vector<std::unique_ptr<QuerySolution>> solutions,
    std::vector<std::pair<std::unique_ptr<PlanStage>, stage_builder::PlanStageData>> roots) {
    invariant(solutions.size() == 1);
    invariant(solutions.size() == roots.size());

    const double evictionRatio = internalQueryCacheEvictionRatio;
    const size_t maxReadsBeforeReplan = evictionRatio * _decisionReads;
    // In cached solution planning we collect execution stats with an upper bound on reads allowed
    // per trial run computed based on previous decision reads. If the trial run ends before
    // reaching EOF, it will use the 'checkNumReads' function to determine if it should continue
    // executing or immediately terminate execution.
    auto candidate = collectExecutionStatsForCachedPlan(std::move(solutions[0]),
                                                        std::move(roots[0].first),
                                                        std::move(roots[0].second),
                                                        maxReadsBeforeReplan);
    auto explainer = plan_explainer_factory::make(
        candidate.root.get(), &candidate.data, candidate.solution.get());

    if (!candidate.status.isOK()) {
        // On failure, fall back to replanning the whole query. We neither evict the existing cache
        // entry, nor cache the result of replanning.
        LOGV2_DEBUG(2057901,
                    1,
                    "Execution of cached plan failed, falling back to replan",
                    "query"_attr = redact(_cq.toStringShort()),
                    "planSummary"_attr = explainer->getPlanSummary());
        return replan(false, str::stream() << "cached plan returned: " << candidate.status);
    }

    auto visitor = PlanStatsNumReadsVisitor{};
    candidate.root->accumulate(kEmptyPlanNodeId, &visitor);
    auto numReads = visitor.numReads;

    // If the trial run executed in 'collectExecutionStats()' did not determine that a replan is
    // necessary, then return that plan as is. The executor can continue using it. All results
    // generated during the trial are stored with the plan so that the executor can return those to
    // the user as well.
    if (!candidate.needsReplanning) {
        tassert(590800,
                "Cached plan exited early without 'needsReplanning' set.",
                !candidate.exitedEarly);
        return {makeVector(std::move(candidate)), 0};
    }

    // If we're here, the trial period took more than 'maxReadsBeforeReplan' physical reads. This
    // plan may not be efficient any longer, so we replan from scratch.
    LOGV2_DEBUG(
        2058001,
        1,
        "Evicting cache entry for a query and replanning it since the number of required reads "
        "mismatch the number of cached reads",
        "maxReadsBeforeReplan"_attr = maxReadsBeforeReplan,
        "decisionReads"_attr = _decisionReads,
        "query"_attr = redact(_cq.toStringShort()),
        "planSummary"_attr = explainer->getPlanSummary());
    return replan(
        true,
        str::stream()
            << "cached plan was less efficient than expected: expected trial execution to take "
            << _decisionReads << " reads but it took at least " << numReads << " reads");
}

plan_ranker::CandidatePlan CachedSolutionPlanner::collectExecutionStatsForCachedPlan(
    std::unique_ptr<QuerySolution> solution,
    std::unique_ptr<PlanStage> root,
    stage_builder::PlanStageData data,
    size_t maxTrialPeriodNumReads) {
    const auto maxNumResults{trial_period::getTrialPeriodNumToReturn(_cq)};

    plan_ranker::CandidatePlan candidate{std::move(solution),
                                         std::move(root),
                                         std::move(data),
                                         false /* exitedEarly*/,
                                         false /* needsReplanning */,
                                         Status::OK()};

    ON_BLOCK_EXIT([rootPtr = candidate.root.get()] { rootPtr->detachFromTrialRunTracker(); });

    auto needsReplanningCheck = [maxTrialPeriodNumReads](PlanStage* candidateRoot) {
        auto visitor = PlanStatsNumReadsVisitor{};
        candidateRoot->accumulate(kEmptyPlanNodeId, &visitor);
        return visitor.numReads > maxTrialPeriodNumReads;
    };
    auto trackerRequirementCheck = [&needsReplanningCheck, &candidate]() {
        bool shouldExitEarly = needsReplanningCheck(candidate.root.get());
        if (!shouldExitEarly) {
            candidate.root->detachFromTrialRunTracker();
        }
        candidate.needsReplanning = (candidate.needsReplanning || shouldExitEarly);
        return shouldExitEarly;
    };
    auto tracker = std::make_unique<TrialRunTracker>(
        std::move(trackerRequirementCheck), maxNumResults, maxTrialPeriodNumReads);

    candidate.root->attachToTrialRunTracker(std::move(tracker.get()));

    auto candidateDone = executeCandidateTrial(&candidate, maxNumResults);
    if (candidate.status.isOK() && !candidateDone && !candidate.needsReplanning) {
        candidate.needsReplanning =
            candidate.needsReplanning || needsReplanningCheck(candidate.root.get());
    }

    return candidate;
}

CandidatePlans CachedSolutionPlanner::replan(bool shouldCache, std::string reason) const {
    // The plan drawn from the cache is being discarded, and should no longer be registered with the
    // yield policy.
    _yieldPolicy->clearRegisteredPlans();

    // We're planning from scratch, using the original set of indexes provided in '_queryParams'.
    // Therefore, if any of the collection's indexes have been dropped, the query should fail with
    // a 'QueryPlanKilled' error.
    _indexExistenceChecker.check();

    if (shouldCache) {
        // Deactivate the current cache entry.
        auto cache = CollectionQueryInfo::get(_collection).getPlanCache();
        cache->deactivate(plan_cache_key_factory::make<mongo::PlanCacheKey>(_cq, _collection));
    }

    auto buildExecutableTree = [&](const QuerySolution& sol) {
        auto [root, data] = stage_builder::buildSlotBasedExecutableTree(
            _opCtx, _collection, _cq, sol, _yieldPolicy);
        data.replanReason.emplace(reason);
        return std::make_pair(std::move(root), std::move(data));
    };

    // Use the query planning module to plan the whole query.
    auto statusWithMultiPlanSolns = QueryPlanner::plan(_cq, _queryParams);
    auto solutions = uassertStatusOK(std::move(statusWithMultiPlanSolns));

    if (solutions.size() == 1) {
        // Only one possible plan. Build the stages from the solution.
        auto [root, data] = buildExecutableTree(*solutions[0]);
        auto status = prepareExecutionPlan(root.get(), &data);
        uassertStatusOK(status);
        auto [result, recordId, exitedEarly] = status.getValue();
        tassert(
            5323800, "cached planner unexpectedly exited early during prepare phase", !exitedEarly);

        auto explainer = plan_explainer_factory::make(root.get(), &data, solutions[0].get());
        LOGV2_DEBUG(
            2058101,
            1,
            "Replanning of query resulted in a single query solution, which will not be cached. ",
            "query"_attr = redact(_cq.toStringShort()),
            "planSummary"_attr = explainer->getPlanSummary(),
            "shouldCache"_attr = (shouldCache ? "yes" : "no"));
        return {makeVector<plan_ranker::CandidatePlan>(plan_ranker::CandidatePlan{
                    std::move(solutions[0]), std::move(root), std::move(data)}),
                0};
    }

    // Many solutions. Build a plan stage tree for each solution and create a multi planner to pick
    // the best, update the cache, and so on.
    std::vector<std::pair<std::unique_ptr<PlanStage>, stage_builder::PlanStageData>> roots;
    for (auto&& solution : solutions) {
        if (solution->cacheData.get()) {
            solution->cacheData->indexFilterApplied = _queryParams.indexFiltersApplied;
        }

        roots.push_back(buildExecutableTree(*solution));
    }

    const auto cachingMode =
        shouldCache ? PlanCachingMode::AlwaysCache : PlanCachingMode::NeverCache;
    MultiPlanner multiPlanner{_opCtx, _collection, _cq, cachingMode, _yieldPolicy};
    auto&& [candidates, winnerIdx] = multiPlanner.plan(std::move(solutions), std::move(roots));
    auto explainer = plan_explainer_factory::make(candidates[winnerIdx].root.get(),
                                                  &candidates[winnerIdx].data,
                                                  candidates[winnerIdx].solution.get());
    LOGV2_DEBUG(2058201,
                1,
                "Query plan after replanning and its cache status",
                "query"_attr = redact(_cq.toStringShort()),
                "planSummary"_attr = explainer->getPlanSummary(),
                "shouldCache"_attr = (shouldCache ? "yes" : "no"));
    return {std::move(candidates), winnerIdx};
}
}  // namespace mongo::sbe
