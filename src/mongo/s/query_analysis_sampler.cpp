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
#include "mongo/s/grid.h"
#include "mongo/s/is_mongos.h"
#include "mongo/s/query_analysis_sample_counters.h"
#include "mongo/s/refresh_query_analyzer_configuration_cmd_gen.h"
#include "mongo/util/net/socket_utils.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace analyze_shard_key {

namespace {

using QuerySamplingOptions = OperationContext::QuerySamplingOptions;

MONGO_FAIL_POINT_DEFINE(disableQueryAnalysisSampler);
MONGO_FAIL_POINT_DEFINE(overwriteQueryAnalysisSamplerAvgLastCountToZero);

const auto getQueryAnalysisSampler = ServiceContext::declareDecoration<QueryAnalysisSampler>();

constexpr auto kActiveCollectionsFieldName = "activeCollections"_sd;

bool isApproximatelyEqual(double val0, double val1, double epsilon) {
    return std::fabs(val0 - val1) < (epsilon + std::numeric_limits<double>::epsilon());
}

}  // namespace

QueryAnalysisSampler& QueryAnalysisSampler::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

QueryAnalysisSampler& QueryAnalysisSampler::get(ServiceContext* serviceContext) {
    invariant(analyze_shard_key::supportsSamplingQueries(true /* ignoreFCV */));
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
            try {
                _refreshConfigurations(opCtx.get());
            } catch (DBException& ex) {
                LOGV2(7012500,
                      "Failed to refresh query analysis configurations, will try again at the next "
                      "interval",
                      "error"_attr = redact(ex));
            }
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

long long QueryAnalysisSampler::_getTotalQueriesCount() const {
    if (isMongos()) {
        return globalOpCounters.getQuery()->load() + globalOpCounters.getInsert()->load() +
            globalOpCounters.getUpdate()->load() + globalOpCounters.getDelete()->load() +
            globalOpCounters.getCommand()->load();
    } else if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
        return globalOpCounters.getNestedAggregate()->load();
    }
    MONGO_UNREACHABLE;
}

void QueryAnalysisSampler::_refreshQueryStats() {
    if (MONGO_unlikely(disableQueryAnalysisSampler.shouldFail())) {
        return;
    }

    long long newTotalCount = _getTotalQueriesCount();
    stdx::lock_guard<Latch> lk(_mutex);
    _queryStats.refreshTotalCount(newTotalCount);
}

double QueryAnalysisSampler::SampleRateLimiter::_getBurstCapacity(double numTokensPerSecond) {
    return std::max(1.0, gQueryAnalysisSamplerBurstMultiplier.load() * numTokensPerSecond);
}

void QueryAnalysisSampler::SampleRateLimiter::_refill(double numTokensPerSecond,
                                                      double burstCapacity) {
    if (numTokensPerSecond == 0.0) {
        return;
    }

    auto currTicks = _serviceContext->getTickSource()->getTicks();
    double numSecondsElapsed = _serviceContext->getTickSource()
                                   ->ticksTo<Nanoseconds>(currTicks - _lastRefillTimeTicks)
                                   .count() /
        1.0e9;
    if (numSecondsElapsed > 0) {
        _lastNumTokens =
            std::min(burstCapacity, numSecondsElapsed * numTokensPerSecond + _lastNumTokens);
        _lastRefillTimeTicks = currTicks;

        LOGV2_DEBUG(7372303,
                    2,
                    "Refilled the bucket",
                    "namespace"_attr = _nss,
                    "collectionUUID"_attr = _collUuid,
                    "numSecondsElapsed"_attr = numSecondsElapsed,
                    "numTokensPerSecond"_attr = numTokensPerSecond,
                    "burstCapacity"_attr = burstCapacity,
                    "lastNumTokens"_attr = _lastNumTokens,
                    "lastRefillTimeTicks"_attr = _lastRefillTimeTicks);
    }
}

bool QueryAnalysisSampler::SampleRateLimiter::tryConsume() {
    _refill(_numTokensPerSecond, _getBurstCapacity(_numTokensPerSecond));

    if (_lastNumTokens >= 1) {
        _lastNumTokens -= 1;
        LOGV2_DEBUG(7372304,
                    2,
                    "Successfully consumed one token",
                    "namespace"_attr = _nss,
                    "collectionUUID"_attr = _collUuid,
                    "lastNumTokens"_attr = _lastNumTokens);
        return true;
    } else if (isApproximatelyEqual(_lastNumTokens, 1, kEpsilon)) {
        // To avoid skipping queries that could have been sampled, allow one token to be consumed
        // if there is nearly one.
        _lastNumTokens = 0;
        LOGV2_DEBUG(7372305,
                    2,
                    "Successfully consumed approximately one token",
                    "namespace"_attr = _nss,
                    "collectionUUID"_attr = _collUuid,
                    "lastNumTokens"_attr = _lastNumTokens);
        return true;
    }
    LOGV2_DEBUG(7372306,
                2,
                "Failed to consume one token",
                "namespace"_attr = _nss,
                "collectionUUID"_attr = _collUuid,
                "lastNumTokens"_attr = _lastNumTokens);
    return false;
}

void QueryAnalysisSampler::SampleRateLimiter::refreshRate(double numTokensPerSecond) {
    // Fill the bucket with tokens created by the previous rate before setting a new rate.
    _refill(_numTokensPerSecond, _getBurstCapacity(numTokensPerSecond));
    _numTokensPerSecond = numTokensPerSecond;
}

void QueryAnalysisSampler::_refreshConfigurations(OperationContext* opCtx) {
    if (MONGO_unlikely(disableQueryAnalysisSampler.shouldFail())) {
        return;
    }

    boost::optional<double> lastAvgCount;
    {
        stdx::lock_guard<Latch> lk(_mutex);
        lastAvgCount = (MONGO_unlikely(overwriteQueryAnalysisSamplerAvgLastCountToZero.shouldFail())
                            ? 0
                            : _queryStats.getLastAvgCount());
    }

    if (!lastAvgCount) {
        // The average number of queries executed per second has not been calculated yet.
        return;
    }

    RefreshQueryAnalyzerConfiguration cmd;
    cmd.setDbName(DatabaseName::kAdmin);
    cmd.setName(getHostNameCached() + ":" + std::to_string(serverGlobalParams.port));
    cmd.setNumQueriesExecutedPerSecond(*lastAvgCount);

    const auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto swResponse = configShard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        DatabaseName::kAdmin.toString(),
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

    LOGV2_DEBUG(6876103,
                2,
                "Refreshed query analyzer configurations",
                "numQueriesExecutedPerSecond"_attr = lastAvgCount,
                "response"_attr = response);
    if (response.getConfigurations().size() != _sampleRateLimiters.size()) {
        LOGV2(7362407,
              "Refreshed query analyzer configurations. The number of collections with active "
              "sampling has changed.",
              "before"_attr = _sampleRateLimiters.size(),
              "after"_attr = response.getConfigurations().size(),
              "response"_attr = response);
    }

    stdx::lock_guard<Latch> lk(_mutex);
    std::map<NamespaceString, SampleRateLimiter> sampleRateLimiters;

    for (const auto& configuration : response.getConfigurations()) {
        auto it = _sampleRateLimiters.find(configuration.getNs());
        if (it == _sampleRateLimiters.end() ||
            it->second.getCollectionUuid() != configuration.getCollectionUuid()) {
            // There is no existing SampleRateLimiter for the collection with this specific
            // collection uuid so create one for it.
            sampleRateLimiters.emplace(configuration.getNs(),
                                       SampleRateLimiter{opCtx->getServiceContext(),
                                                         configuration.getNs(),
                                                         configuration.getCollectionUuid(),
                                                         configuration.getSampleRate()});
        } else {
            auto rateLimiter = it->second;
            if (it->second.getNss() != configuration.getNs()) {
                // Nss changed due to collection rename.
                // TODO SERVER-73990: Test collection renaming during query sampling
                it->second.setNss(configuration.getNs());
            }
            rateLimiter.refreshRate(configuration.getSampleRate());
            sampleRateLimiters.emplace(configuration.getNs(), std::move(rateLimiter));
        }
    }
    _sampleRateLimiters = std::move(sampleRateLimiters);

    QueryAnalysisSampleCounters::get(opCtx).refreshConfigurations(response.getConfigurations());
}

void QueryAnalysisSampler::_incrementCounters(OperationContext* opCtx,
                                              const NamespaceString& nss,
                                              const SampledCommandNameEnum cmdName) {
    switch (cmdName) {
        case SampledCommandNameEnum::kFind:
        case SampledCommandNameEnum::kAggregate:
        case SampledCommandNameEnum::kCount:
        case SampledCommandNameEnum::kDistinct:
            QueryAnalysisSampleCounters::get(opCtx).incrementReads(nss);
            break;
        case SampledCommandNameEnum::kInsert:
        case SampledCommandNameEnum::kUpdate:
        case SampledCommandNameEnum::kDelete:
        case SampledCommandNameEnum::kFindAndModify:
            QueryAnalysisSampleCounters::get(opCtx).incrementWrites(nss);
            break;
        default:
            MONGO_UNREACHABLE;
    }
}

boost::optional<UUID> QueryAnalysisSampler::tryGenerateSampleId(OperationContext* opCtx,
                                                                const NamespaceString& nss,
                                                                SampledCommandNameEnum cmdName) {
    auto opts = opCtx->getQuerySamplingOptions();

    if (!opCtx->getClient()->session() && opts != QuerySamplingOptions::kOptIn) {
        // Do not generate a sample id for an internal query unless it has explicitly opted into
        // query sampling.
        return boost::none;
    }
    if (opts == QuerySamplingOptions::kOptOut) {
        // Do not generate a sample id for a query that has explicitly opted out of query sampling.
        return boost::none;
    }

    stdx::lock_guard<Latch> lk(_mutex);
    auto it = _sampleRateLimiters.find(nss);

    if (it == _sampleRateLimiters.end()) {
        return boost::none;
    }

    auto& rateLimiter = it->second;
    if (rateLimiter.tryConsume()) {
        _incrementCounters(opCtx, nss, cmdName);
        return UUID::gen();
    }
    return boost::none;
}

void QueryAnalysisSampler::appendInfoForServerStatus(BSONObjBuilder* bob) const {
    stdx::lock_guard<Latch> lk(_mutex);
    bob->append(kActiveCollectionsFieldName, static_cast<int>(_sampleRateLimiters.size()));
}

}  // namespace analyze_shard_key
}  // namespace mongo
