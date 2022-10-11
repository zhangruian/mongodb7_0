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

#include "mongo/s/query_analysis_sampler.h"

#include "mongo/db/stats/counters.h"
#include "mongo/logv2/log.h"
#include "mongo/s/analyze_shard_key_feature_flag_gen.h"
#include "mongo/s/grid.h"
#include "mongo/s/is_mongos.h"
#include "mongo/util/net/socket_utils.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace analyze_shard_key {

namespace {

MONGO_FAIL_POINT_DEFINE(disableQueryAnalysisSampler);

const auto getQueryAnalysisSampler = ServiceContext::declareDecoration<QueryAnalysisSampler>();

}  // namespace

QueryAnalysisSampler& QueryAnalysisSampler::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

QueryAnalysisSampler& QueryAnalysisSampler::get(ServiceContext* serviceContext) {
    invariant(analyze_shard_key::gFeatureFlagAnalyzeShardKey.isEnabled(
                  serverGlobalParams.featureCompatibility),
              "Only support analyzing queries when the feature flag is enabled");
    invariant(isMongos(), "Only support analyzing queries on a sharded cluster");
    return getQueryAnalysisSampler(serviceContext);
}

void QueryAnalysisSampler::onStartup() {
    auto serviceContext = getQueryAnalysisSampler.owner(this);
    auto periodicRunner = serviceContext->getPeriodicRunner();
    invariant(periodicRunner);

    PeriodicRunner::PeriodicJob queryStatsRefresherJob(
        "QueryAnalysisQueryStatsRefresher",
        [this](Client* client) { _refreshQueryStats(); },
        Seconds(1));
    _periodicQueryStatsRefresher = periodicRunner->makeJob(std::move(queryStatsRefresherJob));
    _periodicQueryStatsRefresher.start();

    PeriodicRunner::PeriodicJob configurationsRefresherJob(
        "QueryAnalysisConfigurationsRefresher",
        [this](Client* client) {
            auto opCtx = client->makeOperationContext();
            _refreshConfigurations(opCtx.get());
        },
        Seconds(gQueryAnalysisSamplerConfigurationRefreshSecs));
    _periodicConfigurationsRefresher =
        periodicRunner->makeJob(std::move(configurationsRefresherJob));
    _periodicConfigurationsRefresher.start();
}

void QueryAnalysisSampler::onShutdown() {
    if (_periodicQueryStatsRefresher.isValid()) {
        _periodicQueryStatsRefresher.stop();
    }
    if (_periodicConfigurationsRefresher.isValid()) {
        _periodicConfigurationsRefresher.stop();
    }
}

double QueryAnalysisSampler::QueryStats::_calculateExponentialMovingAverage(
    double prevAvg, long long newVal) const {
    return (1 - _smoothingFactor) * prevAvg + _smoothingFactor * newVal;
}

void QueryAnalysisSampler::QueryStats::refreshTotalCount(long long newTotalCount) {
    invariant(newTotalCount >= _lastTotalCount, "Total number of queries cannot decrease");
    long long newCount = newTotalCount - _lastTotalCount;
    // The average is only calculated after the initial count is known.
    _lastAvgCount =
        _lastAvgCount ? _calculateExponentialMovingAverage(*_lastAvgCount, newCount) : newCount;
    _lastTotalCount = newTotalCount;
}

void QueryAnalysisSampler::_refreshQueryStats() {
    if (MONGO_unlikely(disableQueryAnalysisSampler.shouldFail())) {
        return;
    }

    long long newTotalCount = globalOpCounters.getQuery()->load() +
        globalOpCounters.getInsert()->load() + globalOpCounters.getUpdate()->load() +
        globalOpCounters.getDelete()->load() + globalOpCounters.getCommand()->load();

    stdx::lock_guard<Latch> lk(_mutex);
    _queryStats.refreshTotalCount(newTotalCount);
}

void QueryAnalysisSampler::_refreshConfigurations(OperationContext* opCtx) {
    if (MONGO_unlikely(disableQueryAnalysisSampler.shouldFail())) {
        return;
    }

    if (!_queryStats.getLastAvgCount()) {
        // The average number of queries executed per second has not been calculated yet.
        return;
    }

    RefreshQueryAnalyzerConfiguration cmd;
    cmd.setDbName(NamespaceString::kAdminDb);
    cmd.setName(getHostNameCached() + ":" + std::to_string(serverGlobalParams.port));
    cmd.setNumQueriesExecutedPerSecond(*_queryStats.getLastAvgCount());

    const auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto swResponse = configShard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        NamespaceString::kAdminDb.toString(),
        cmd.toBSON({}),
        Shard::RetryPolicy::kIdempotent);
    auto status = Shard::CommandResponse::getEffectiveStatus(swResponse);

    if (!status.isOK()) {
        LOGV2(6973904,
              "Failed to refresh query analysis configurations, will try again at the next "
              "refresh interval",
              "error"_attr = redact(status));
        return;
    }

    auto response = RefreshQueryAnalyzerConfigurationResponse::parse(
        IDLParserContext("configurationRefresher"), swResponse.getValue().response);

    stdx::lock_guard<Latch> lk(_mutex);
    _configurations = response.getConfigurations();
}

}  // namespace analyze_shard_key
}  // namespace mongo
