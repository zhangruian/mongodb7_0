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

#pragma once

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/plan_summary_stats.h"

namespace mongo {
/**
 * The maximum size of a serialized BSON document that details the plan selected by the query
 * planner.
 */
static constexpr int kMaxExplainStatsBSONSizeMB = 10 * 1024 * 1024;

/**
 * This interface defines an API to provide information on the execution plans generated by the
 * query planner for a user query in various formats.
 */
class PlanExplainer {
public:
    /**
     * This pair holds a serialized BSON document that details the plan selected by the query
     * planner, and optional summary stats for an execution tree if the verbosity level for the
     * generated stats is 'executionStats' or higher. The format of these stats are opaque to the
     * caller, and different implementations may choose to provide different stats.
     */
    using PlanStatsDetails = std::pair<BSONObj, boost::optional<PlanSummaryStats>>;

    virtual ~PlanExplainer() = default;

    /**
     * Returns 'true' if this PlanExplainer can provide information on the winning plan and rejected
     * candidate plans, meaning that the QueryPlanner generated multiple candidate plans and the
     * winning plan was chosen by the multi-planner.
     */
    virtual bool isMultiPlan() const = 0;

    /**
     * Returns a short string, suitable for the logs, which summarizes the execution plan.
     */
    virtual std::string getPlanSummary() const = 0;

    /**
     * Fills out 'statsOut' with summary stats collected during the execution of the underlying
     * plan. This is a lightweight alternative which is useful when operations want to request a
     * summary of the available debug information without generating complete explain output.
     *
     * The summary stats are consumed by debug mechanisms such as the profiler and the slow query
     * log.
     */
    virtual void getSummaryStats(PlanSummaryStats* statsOut) const = 0;

    /**
     * Returns statistics that detail the winning plan selected by the multi-planner, or, if no
     * multi-planning has been performed, for the single plan selected by the QueryPlanner.
     *
     * The 'verbosity' level parameter determines the amount of information to be returned.
     */
    virtual PlanStatsDetails getWinningPlanStats(ExplainOptions::Verbosity verbosity) const = 0;

    /**
     * Returns statistics that detail candidate plans rejected by the multi-planner. If no
     * multi-planning has been performed, an empty vector is returned.
     *
     * The 'verbosity' level parameter determines the amount of information to be returned.
     */
    virtual std::vector<PlanStatsDetails> getRejectedPlansStats(
        ExplainOptions::Verbosity verbosity) const = 0;

    /**
     * Serializes plan cache entry debug info into the provided BSONObjBuilder. The output format is
     * intended to be human readable, and useful for debugging query performance problems related to
     * the plan cache.
     */
    virtual std::vector<PlanStatsDetails> getCachedPlanStats(
        const PlanCacheEntry::DebugInfo& debugInfo, ExplainOptions::Verbosity verbosity) const = 0;
};
}  // namespace mongo
