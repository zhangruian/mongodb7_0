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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/client/shard_registry.h"

#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/client.h"
#include "mongo/db/logical_time_metadata_hook.h"
#include "mongo/db/vector_clock.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/grid.h"
#include "mongo/util/str.h"

namespace mongo {

namespace {

const Seconds kRefreshPeriod(30);

/**
 * Whether or not the actual topologyTime should be used.  When this is false, the
 * topologyTime part of the cache's Time will stay fixed and not advance.
 */
bool useActualTopologyTime() {
    return serverGlobalParams.featureCompatibility.isVersionInitialized() &&
        serverGlobalParams.featureCompatibility.isGreaterThanOrEqualTo(
            ServerGlobalParams::FeatureCompatibility::Version::kVersion47);
}

}  // namespace

using CallbackArgs = executor::TaskExecutor::CallbackArgs;

const ShardId ShardRegistry::kConfigServerShardId = ShardId("config");

ShardRegistry::ShardRegistry(std::unique_ptr<ShardFactory> shardFactory,
                             const ConnectionString& configServerCS,
                             std::vector<ShardRemovalHook> shardRemovalHooks)
    : _shardFactory(std::move(shardFactory)),
      _initConfigServerCS(configServerCS),
      _shardRemovalHooks(std::move(shardRemovalHooks)),
      _threadPool([] {
          ThreadPool::Options options;
          options.poolName = "ShardRegistry";
          options.minThreads = 0;
          options.maxThreads = 1;
          return options;
      }()) {
    invariant(_initConfigServerCS.isValid());
    _threadPool.startup();
}

ShardRegistry::~ShardRegistry() {
    shutdown();
}

void ShardRegistry::init(ServiceContext* service) {
    invariant(!_isInitialized.load());

    invariant(!_service);
    _service = service;

    auto lookupFn = [this](OperationContext* opCtx,
                           const Singleton& key,
                           const Cache::ValueHandle& cachedData,
                           const Time& timeInStore) {
        return _lookup(opCtx, key, cachedData, timeInStore);
    };

    _cache =
        std::make_unique<Cache>(_cacheMutex, _service, _threadPool, lookupFn, 1 /* cacheSize */);

    {
        stdx::lock_guard<Latch> lk(_mutex);
        _configShardData = ShardRegistryData::createWithConfigShardOnly(
            _shardFactory->createShard(kConfigServerShardId, _initConfigServerCS));
    }

    _isInitialized.store(true);
}

ShardRegistry::Cache::LookupResult ShardRegistry::_lookup(OperationContext* opCtx,
                                                          const Singleton& key,
                                                          const Cache::ValueHandle& cachedData,
                                                          const Time& timeInStore) {
    invariant(key == _kSingleton);
    invariant(cachedData, "ShardRegistry::_lookup called but the cache is empty");

    LOGV2_DEBUG(4620250,
                2,
                "Starting ShardRegistry::_lookup",
                "cachedData"_attr = cachedData->toBSON(),
                "cachedData.getTime()"_attr = cachedData.getTime().toBSON(),
                "timeInStore"_attr = timeInStore.toBSON());

    // Check if we need to refresh from the configsvrs.  If so, then do that and get the results,
    // otherwise (this is a lookup only to incorporate updated connection strings from the RSM),
    // then get the equivalent values from the previously cached data.
    auto [returnData,
          returnTopologyTime,
          returnForceReloadIncrement,
          removedShards,
          fetchedFromConfigServers] = [&]()
        -> std::tuple<ShardRegistryData, Timestamp, Increment, ShardRegistryData::ShardMap, bool> {
        if (timeInStore.topologyTime > cachedData.getTime().topologyTime ||
            timeInStore.forceReloadIncrement > cachedData.getTime().forceReloadIncrement) {
            auto [reloadedData, maxTopologyTime] =
                ShardRegistryData::createFromCatalogClient(opCtx, _shardFactory.get());
            if (!useActualTopologyTime()) {
                // If not using the actual topology time, then just use the topologyTime currently
                // in the cache, instead of the maximum topologyTime value from config.shards.  This
                // is necessary during upgrade/downgrade when topologyTime might not be gossiped by
                // all nodes (and so isn't being used).
                maxTopologyTime = cachedData.getTime().topologyTime;
            }

            auto [mergedData, removedShards] =
                ShardRegistryData::mergeExisting(*cachedData, reloadedData);

            return {
                mergedData, maxTopologyTime, timeInStore.forceReloadIncrement, removedShards, true};
        } else {
            return {*cachedData,
                    cachedData.getTime().topologyTime,
                    cachedData.getTime().forceReloadIncrement,
                    {},
                    false};
        }
    }();

    // Always apply the latest conn strings.
    auto [latestConnStrings, rsmIncrementForConnStrings] = _getLatestConnStrings();

    for (const auto& latestConnString : latestConnStrings) {
        // TODO SERVER-50909: Optimise by only doing this work if the latest conn string differs.

        auto shard = returnData.findByRSName(latestConnString.first.toString());
        if (!shard) {
            continue;
        }

        auto newData = ShardRegistryData::createFromExisting(
            returnData, latestConnString.second, _shardFactory.get());
        returnData = newData;
    }

    // Remove RSMs that are not in the catalog any more.
    for (auto& pair : removedShards) {
        auto& shardId = pair.first;
        auto& shard = pair.second;
        invariant(shard);

        auto name = shard->getConnString().getSetName();
        ReplicaSetMonitor::remove(name);
        for (auto& callback : _shardRemovalHooks) {
            // Run callbacks asynchronously.
            // TODO SERVER-50906: Consider running these callbacks synchronously.
            ExecutorFuture<void>(Grid::get(opCtx)->getExecutorPool()->getFixedExecutor())
                .getAsync([=](const Status&) { callback(shardId); });
        }
    }

    // The registry is "up" once there has been a successful lookup from the config servers.
    if (fetchedFromConfigServers) {
        _isUp.store(true);
    }

    Time returnTime{returnTopologyTime, rsmIncrementForConnStrings, returnForceReloadIncrement};
    LOGV2_DEBUG(4620251,
                2,
                "Finished ShardRegistry::_lookup",
                "returnData"_attr = returnData.toBSON(),
                "returnTime"_attr = returnTime);
    return Cache::LookupResult{returnData, returnTime};
}

void ShardRegistry::startupPeriodicReloader(OperationContext* opCtx) {
    invariant(_isInitialized.load());
    // startupPeriodicReloader() must be called only once
    invariant(!_executor);

    auto hookList = std::make_unique<rpc::EgressMetadataHookList>();
    hookList->addHook(std::make_unique<rpc::LogicalTimeMetadataHook>(opCtx->getServiceContext()));

    // construct task executor
    auto net = executor::makeNetworkInterface("ShardRegistryUpdater", nullptr, std::move(hookList));
    auto netPtr = net.get();
    _executor = std::make_unique<executor::ThreadPoolTaskExecutor>(
        std::make_unique<executor::NetworkInterfaceThreadPool>(netPtr), std::move(net));
    LOGV2_DEBUG(22724, 1, "Starting up task executor for periodic reloading of ShardRegistry");
    _executor->startup();

    auto status =
        _executor->scheduleWork([this](const CallbackArgs& cbArgs) { _periodicReload(cbArgs); });

    if (status.getStatus() == ErrorCodes::ShutdownInProgress) {
        LOGV2_DEBUG(
            22725, 1, "Can't schedule Shard Registry reload. Executor shutdown in progress");
        return;
    }

    if (!status.isOK()) {
        LOGV2_FATAL(40252,
                    "Error scheduling shard registry reload caused by {error}",
                    "Error scheduling shard registry reload",
                    "error"_attr = redact(status.getStatus()));
    }
}

void ShardRegistry::shutdownPeriodicReloader() {
    if (_executor) {
        LOGV2_DEBUG(22723, 1, "Shutting down task executor for reloading shard registry");
        _executor->shutdown();
        _executor->join();
        _executor.reset();
    }
}

void ShardRegistry::shutdown() {
    shutdownPeriodicReloader();

    if (!_isShutdown.load()) {
        LOGV2_DEBUG(4620235, 1, "Shutting down shard registry");
        _threadPool.shutdown();
        _threadPool.join();
        _isShutdown.store(true);
    }
}

void ShardRegistry::_periodicReload(const CallbackArgs& cbArgs) {
    LOGV2_DEBUG(22726, 1, "Reloading shardRegistry");
    if (!cbArgs.status.isOK()) {
        LOGV2_WARNING(22734,
                      "Error reloading shard registry caused by {error}",
                      "Error reloading shard registry",
                      "error"_attr = redact(cbArgs.status));
        return;
    }

    ThreadClient tc("shard-registry-reload", getGlobalServiceContext());

    auto opCtx = tc->makeOperationContext();

    auto refreshPeriod = kRefreshPeriod;

    try {
        reload(opCtx.get());
    } catch (const DBException& e) {
        if (e.code() == ErrorCodes::ReadConcernMajorityNotAvailableYet) {
            refreshPeriod = Seconds(1);
        }
        LOGV2(22727,
              "Error running periodic reload of shard registry caused by {error}; will retry after "
              "{shardRegistryReloadInterval}",
              "Error running periodic reload of shard registry",
              "error"_attr = redact(e),
              "shardRegistryReloadInterval"_attr = refreshPeriod);
    }

    // reschedule itself
    auto status =
        _executor->scheduleWorkAt(_executor->now() + refreshPeriod,
                                  [this](const CallbackArgs& cbArgs) { _periodicReload(cbArgs); });

    if (status.getStatus() == ErrorCodes::ShutdownInProgress) {
        LOGV2_DEBUG(
            22728, 1, "Error scheduling shard registry reload. Executor shutdown in progress");
        return;
    }

    if (!status.isOK()) {
        LOGV2_FATAL(40253,
                    "Error scheduling shard registry reload caused by {error}",
                    "Error scheduling shard registry reload",
                    "error"_attr = redact(status.getStatus()));
    }
}

ConnectionString ShardRegistry::getConfigServerConnectionString() const {
    return getConfigShard()->getConnString();
}

std::shared_ptr<Shard> ShardRegistry::getConfigShard() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _configShardData.findShard(kConfigServerShardId);
}

StatusWith<std::shared_ptr<Shard>> ShardRegistry::getShard(OperationContext* opCtx,
                                                           const ShardId& shardId) {
    // First check if this is a config shard lookup.
    {
        stdx::lock_guard<Latch> lk(_mutex);
        if (auto shard = _configShardData.findShard(shardId)) {
            return shard;
        }
    }

    if (auto shard = _getData(opCtx)->findShard(shardId)) {
        return shard;
    }

    // Reload and try again if the shard was not in the registry
    reload(opCtx);
    if (auto shard = _getData(opCtx)->findShard(shardId)) {
        return shard;
    }

    return {ErrorCodes::ShardNotFound, str::stream() << "Shard " << shardId << " not found"};
}

void ShardRegistry::getAllShardIds(OperationContext* opCtx, std::vector<ShardId>* all) {
    std::set<ShardId> seen;
    auto data = _getData(opCtx);
    data->getAllShardIds(seen);
    if (seen.empty()) {
        reload(opCtx);
        data = _getData(opCtx);
        data->getAllShardIds(seen);
    }
    all->assign(seen.begin(), seen.end());
}

int ShardRegistry::getNumShards(OperationContext* opCtx) {
    std::set<ShardId> seen;
    auto data = _getData(opCtx);
    data->getAllShardIds(seen);
    return seen.size();
}

std::pair<std::vector<ShardRegistry::LatestConnStrings::value_type>, ShardRegistry::Increment>
ShardRegistry::_getLatestConnStrings() const {
    stdx::unique_lock<Latch> lock(_mutex);
    return {{_latestConnStrings.begin(), _latestConnStrings.end()}, _rsmIncrement.load()};
}

void ShardRegistry::updateReplSetHosts(const ConnectionString& givenConnString,
                                       ConnectionStringUpdateType updateType) {
    invariant(givenConnString.type() == ConnectionString::SET ||
              givenConnString.type() == ConnectionString::CUSTOM);  // For dbtests

    stdx::lock_guard<Latch> lk(_mutex);
    ConnectionString newConnString =
        (updateType == ConnectionStringUpdateType::kPossible &&
         _latestConnStrings.find(givenConnString.getSetName()) != _latestConnStrings.end())
        ? _latestConnStrings[givenConnString.getSetName()].makeUnionWith(givenConnString)
        : givenConnString;
    if (auto shard = _configShardData.findByRSName(newConnString.getSetName())) {
        auto newData = ShardRegistryData::createFromExisting(
            _configShardData, newConnString, _shardFactory.get());
        _configShardData = newData;

    } else {
        // Stash the new connection string and bump the RSM increment.
        _latestConnStrings[newConnString.getSetName()] = newConnString;
        auto value = _rsmIncrement.addAndFetch(1);
        LOGV2_DEBUG(4620252,
                    2,
                    "ShardRegistry stashed new connection string",
                    "newConnString"_attr = newConnString,
                    "newRSMIncrement"_attr = value);
    }

    // Schedule a lookup, to incorporate the new connection string.
    // TODO SERVER-50910: To avoid needing to use a separate thread to schedule the lookup, make
    // _getData() async.
    auto status = Grid::get(_service)->getExecutorPool()->getFixedExecutor()->scheduleWork(
        [this](const CallbackArgs& cbArgs) {
            ThreadClient tc("shard-registry-rsm-reload", _service);

            auto opCtx = tc->makeOperationContext();

            try {
                _getData(opCtx.get());
            } catch (const DBException& e) {
                LOGV2(4620201,
                      "Error running reload of ShardRegistry for RSM update, caused by {error}",
                      "Error running reload of ShardRegistry for RSM update",
                      "error"_attr = redact(e));
            }
        });

    if (status.getStatus() == ErrorCodes::ShutdownInProgress) {
        LOGV2_DEBUG(
            4620202,
            1,
            "Can't schedule ShardRegistry reload for RSM update, executor shutdown in progress");
        return;
    }

    if (!status.isOK()) {
        LOGV2_FATAL(4620203,
                    "Error scheduling ShardRegistry reload for RSM update, caused by {error}",
                    "Error scheduling ShardRegistry reload for RSM update",
                    "error"_attr = redact(status.getStatus()));
    }
}

std::unique_ptr<Shard> ShardRegistry::createConnection(const ConnectionString& connStr) const {
    return _shardFactory->createUniqueShard(ShardId("<unnamed>"), connStr);
}

bool ShardRegistry::isUp() const {
    return _isUp.load();
}

void ShardRegistry::toBSON(BSONObjBuilder* result) const {
    BSONObjBuilder map;
    BSONObjBuilder hosts;
    BSONObjBuilder connStrings;
    auto data = _getCachedData();
    data->toBSON(&map, &hosts, &connStrings);
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _configShardData.toBSON(&map, &hosts, &connStrings);
    }
    result->append("map", map.obj());
    result->append("hosts", hosts.obj());
    result->append("connStrings", connStrings.obj());
}

bool ShardRegistry::reload(OperationContext* opCtx) {
    // Make the next acquire do a lookup.
    auto value = _forceReloadIncrement.addAndFetch(1);
    LOGV2_DEBUG(4620253, 2, "Forcing ShardRegistry reload", "newForceReloadIncrement"_attr = value);

    // Force it to actually happen now.
    _getData(opCtx);

    return true;
}

void ShardRegistry::clearEntries() {
    _cache->invalidateAll();
}

void ShardRegistry::updateReplicaSetOnConfigServer(ServiceContext* serviceContext,
                                                   const ConnectionString& connStr) noexcept {
    ThreadClient tc("UpdateReplicaSetOnConfigServer", serviceContext);

    auto opCtx = tc->makeOperationContext();
    auto const grid = Grid::get(opCtx.get());

    std::shared_ptr<Shard> s =
        grid->shardRegistry()->_getShardForRSNameNoReload(connStr.getSetName());
    if (!s) {
        LOGV2_DEBUG(22730,
                    1,
                    "Error updating replica set on config server. Couldn't find shard for "
                    "replica set {replicaSetConnectionStr}",
                    "Error updating replica set on config servers. Couldn't find shard",
                    "replicaSetConnectionStr"_attr = connStr);
        return;
    }

    if (s->isConfig()) {
        // No need to tell the config servers their own connection string.
        return;
    }

    auto swWasUpdated = grid->catalogClient()->updateConfigDocument(
        opCtx.get(),
        ShardType::ConfigNS,
        BSON(ShardType::name(s->getId().toString())),
        BSON("$set" << BSON(ShardType::host(connStr.toString()))),
        false,
        ShardingCatalogClient::kMajorityWriteConcern);
    auto status = swWasUpdated.getStatus();
    if (!status.isOK()) {
        LOGV2_ERROR(22736,
                    "Error updating replica set {replicaSetConnectionStr} on config server caused "
                    "by {error}",
                    "Error updating replica set on config server",
                    "replicaSetConnectionStr"_attr = connStr,
                    "error"_attr = redact(status));
    }
}

// Inserts the initial empty ShardRegistryData into the cache, if the cache is empty.
void ShardRegistry::_initializeCacheIfNecessary() const {
    if (!_cache->peekLatestCached(_kSingleton)) {
        stdx::lock_guard<Latch> lk(_mutex);
        if (!_cache->peekLatestCached(_kSingleton)) {
            _cache->insertOrAssign(_kSingleton, {}, Date_t::now(), Time());
        }
    }
}

ShardRegistry::Cache::ValueHandle ShardRegistry::_getData(OperationContext* opCtx) {
    _initializeCacheIfNecessary();

    // If the forceReloadIncrement is 0, then we've never done a lookup, so we should be sure to do
    // one now.
    Increment uninitializedIncrement{0};
    _forceReloadIncrement.compareAndSwap(&uninitializedIncrement, 1);

    // Update the time the cache should be aiming for.
    auto now = VectorClock::get(opCtx)->getTime();
    // The topologyTime should be advanced to either the actual topologyTime (if it is being
    // gossiped), or else the previously cached topologyTime value (so that this part of the cache's
    // time doesn't advance, if topologyTime isn't being gossiped).
    Timestamp topologyTime = useActualTopologyTime()
        ? now.topologyTime().asTimestamp()
        : _cache->peekLatestCached(_kSingleton).getTime().topologyTime;
    _cache->advanceTimeInStore(
        _kSingleton, Time(topologyTime, _rsmIncrement.load(), _forceReloadIncrement.load()));

    return _cache->acquire(opCtx, _kSingleton, CacheCausalConsistency::kLatestKnown);
}

// TODO SERVER-50206: Remove usage of these non-causally consistent accessors.

ShardRegistry::Cache::ValueHandle ShardRegistry::_getCachedData() const {
    _initializeCacheIfNecessary();
    return _cache->peekLatestCached(_kSingleton);
}

std::shared_ptr<Shard> ShardRegistry::getShardNoReload(const ShardId& shardId) const {
    // First check if this is a config shard lookup.
    {
        stdx::lock_guard<Latch> lk(_mutex);
        if (auto shard = _configShardData.findShard(shardId)) {
            return shard;
        }
    }
    auto data = _getCachedData();
    return data->findShard(shardId);
}

std::shared_ptr<Shard> ShardRegistry::getShardForHostNoReload(const HostAndPort& host) const {
    // First check if this is a config shard lookup.
    {
        stdx::lock_guard<Latch> lk(_mutex);
        if (auto shard = _configShardData.findByHostAndPort(host)) {
            return shard;
        }
    }
    auto data = _getCachedData();
    return data->findByHostAndPort(host);
}

void ShardRegistry::getAllShardIdsNoReload(std::vector<ShardId>* all) const {
    std::set<ShardId> seen;
    auto data = _getCachedData();
    data->getAllShardIds(seen);
    all->assign(seen.begin(), seen.end());
}

int ShardRegistry::getNumShardsNoReload() const {
    std::set<ShardId> seen;
    auto data = _getCachedData();
    data->getAllShardIds(seen);
    return seen.size();
}

std::shared_ptr<Shard> ShardRegistry::_getShardForRSNameNoReload(const std::string& name) const {
    // First check if this is a config shard lookup.
    {
        stdx::lock_guard<Latch> lk(_mutex);
        if (auto shard = _configShardData.findByRSName(name)) {
            return shard;
        }
    }
    auto data = _getCachedData();
    return data->findByRSName(name);
}

////////////// ShardRegistryData //////////////////

ShardRegistryData ShardRegistryData::createWithConfigShardOnly(std::shared_ptr<Shard> configShard) {
    ShardRegistryData data;
    data._addShard(configShard, true);
    return data;
}

std::pair<ShardRegistryData, Timestamp> ShardRegistryData::createFromCatalogClient(
    OperationContext* opCtx, ShardFactory* shardFactory) {
    auto const catalogClient = Grid::get(opCtx)->catalogClient();

    auto readConcern = repl::ReadConcernLevel::kMajorityReadConcern;

    // ShardRemote requires a majority read. We can only allow a non-majority read if we are a
    // config server.
    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer &&
        !repl::ReadConcernArgs::get(opCtx).isEmpty()) {
        readConcern = repl::ReadConcernArgs::get(opCtx).getLevel();
    }

    auto shardsAndOpTime =
        uassertStatusOKWithContext(catalogClient->getAllShards(opCtx, readConcern),
                                   "could not get updated shard list from config server");

    auto shards = std::move(shardsAndOpTime.value);
    auto reloadOpTime = std::move(shardsAndOpTime.opTime);

    LOGV2_DEBUG(22731,
                1,
                "Found {shardsNumber} shards listed on config server(s) with lastVisibleOpTime: "
                "{lastVisibleOpTime}",
                "Succesfully retrieved updated shard list from config server",
                "shardsNumber"_attr = shards.size(),
                "lastVisibleOpTime"_attr = reloadOpTime);

    // Ensure targeter exists for all shards and take shard connection string from the targeter.
    // Do this before re-taking the mutex to avoid deadlock with the ReplicaSetMonitor updating
    // hosts for a given shard.
    std::vector<std::tuple<std::string, ConnectionString>> shardsInfo;
    Timestamp maxTopologyTime;
    for (const auto& shardType : shards) {
        // This validation should ideally go inside the ShardType::validate call. However, doing
        // it there would prevent us from loading previously faulty shard hosts, which might have
        // been stored (i.e., the entire getAllShards call would fail).
        auto shardHostStatus = ConnectionString::parse(shardType.getHost());
        if (!shardHostStatus.isOK()) {
            LOGV2_WARNING(22735,
                          "Error parsing shard host caused by {error}",
                          "Error parsing shard host",
                          "error"_attr = redact(shardHostStatus.getStatus()));
            continue;
        }

        if (auto thisTopologyTime = shardType.getTopologyTime();
            maxTopologyTime < thisTopologyTime) {
            maxTopologyTime = thisTopologyTime;
        }

        shardsInfo.push_back(std::make_tuple(shardType.getName(), shardHostStatus.getValue()));
    }

    ShardRegistryData data;
    for (auto& shardInfo : shardsInfo) {
        if (std::get<0>(shardInfo) == "config") {
            continue;
        }

        auto shard = shardFactory->createShard(std::move(std::get<0>(shardInfo)),
                                               std::move(std::get<1>(shardInfo)));

        data._addShard(std::move(shard), false);
    }
    return {data, maxTopologyTime};
}

std::pair<ShardRegistryData, ShardRegistryData::ShardMap> ShardRegistryData::mergeExisting(
    const ShardRegistryData& alreadyCachedData, const ShardRegistryData& configServerData) {
    ShardRegistryData mergedData(configServerData);

    // For connstrings and hosts, prefer values from alreadyCachedData to whatever might have been
    // fetched from the configsvrs.
    for (auto it = alreadyCachedData._connStringLookup.begin();
         it != alreadyCachedData._connStringLookup.end();
         ++it) {
        mergedData._connStringLookup[it->first] = it->second;
    }
    for (auto it = alreadyCachedData._hostLookup.begin(); it != alreadyCachedData._hostLookup.end();
         ++it) {
        mergedData._hostLookup[it->first] = it->second;
    }

    // Find the shards that are no longer present.
    ShardMap removedShards;
    for (auto i = alreadyCachedData._shardIdLookup.begin();
         i != alreadyCachedData._shardIdLookup.end();
         ++i) {
        invariant(i->second);
        if (mergedData._shardIdLookup.find(i->second->getId()) == mergedData._shardIdLookup.end()) {
            removedShards[i->second->getId()] = i->second;
        }
    }

    return {mergedData, removedShards};
}

ShardRegistryData ShardRegistryData::createFromExisting(const ShardRegistryData& existingData,
                                                        const ConnectionString& newConnString,
                                                        ShardFactory* shardFactory) {
    ShardRegistryData data(existingData);

    auto it = data._rsLookup.find(newConnString.getSetName());
    if (it == data._rsLookup.end()) {
        return data;
    }
    invariant(it->second);
    auto updatedShard = shardFactory->createShard(it->second->getId(), newConnString);
    data._addShard(updatedShard, true);

    return data;
}

std::shared_ptr<Shard> ShardRegistryData::findByRSName(const std::string& name) const {
    auto i = _rsLookup.find(name);
    return (i != _rsLookup.end()) ? i->second : nullptr;
}

std::shared_ptr<Shard> ShardRegistryData::_findByConnectionString(
    const ConnectionString& connectionString) const {
    auto i = _connStringLookup.find(connectionString);
    return (i != _connStringLookup.end()) ? i->second : nullptr;
}

std::shared_ptr<Shard> ShardRegistryData::findByHostAndPort(const HostAndPort& hostAndPort) const {
    auto i = _hostLookup.find(hostAndPort);
    return (i != _hostLookup.end()) ? i->second : nullptr;
}

std::shared_ptr<Shard> ShardRegistryData::_findByShardId(const ShardId& shardId) const {
    auto i = _shardIdLookup.find(shardId);
    return (i != _shardIdLookup.end()) ? i->second : nullptr;
}

std::shared_ptr<Shard> ShardRegistryData::findShard(const ShardId& shardId) const {
    auto shard = _findByShardId(shardId);
    if (shard) {
        return shard;
    }

    StatusWith<ConnectionString> swConnString = ConnectionString::parse(shardId.toString());
    if (swConnString.isOK()) {
        shard = _findByConnectionString(swConnString.getValue());
        if (shard) {
            return shard;
        }
    }

    StatusWith<HostAndPort> swHostAndPort = HostAndPort::parse(shardId.toString());
    if (swHostAndPort.isOK()) {
        shard = findByHostAndPort(swHostAndPort.getValue());
        if (shard) {
            return shard;
        }
    }

    return nullptr;
}

void ShardRegistryData::getAllShards(std::vector<std::shared_ptr<Shard>>& result) const {
    result.reserve(_shardIdLookup.size());
    for (auto&& shard : _shardIdLookup) {
        result.emplace_back(shard.second);
    }
}

void ShardRegistryData::getAllShardIds(std::set<ShardId>& seen) const {
    for (auto i = _shardIdLookup.begin(); i != _shardIdLookup.end(); ++i) {
        const auto& s = i->second;
        if (s->getId().toString() == "config") {
            continue;
        }
        seen.insert(s->getId());
    }
}

void ShardRegistryData::_addShard(std::shared_ptr<Shard> shard, bool useOriginalCS) {
    const ShardId shardId = shard->getId();

    const ConnectionString connString =
        useOriginalCS ? shard->originalConnString() : shard->getConnString();

    auto currentShard = findShard(shardId);
    if (currentShard) {
        auto oldConnString = currentShard->originalConnString();

        if (oldConnString.toString() != connString.toString()) {
            LOGV2(22732,
                  "Updating shard registry connection string for shard {shardId} to "
                  "{newShardConnectionString} from {oldShardConnectionString}",
                  "Updating shard connection string on shard registry",
                  "shardId"_attr = currentShard->getId(),
                  "newShardConnectionString"_attr = connString,
                  "oldShardConnectionString"_attr = oldConnString);
        }

        for (const auto& host : oldConnString.getServers()) {
            _hostLookup.erase(host);
        }
        _connStringLookup.erase(oldConnString);
    }

    _shardIdLookup[shard->getId()] = shard;

    LOGV2_DEBUG(22733,
                3,
                "Adding new shard {shardId} with connection string {shardConnectionString} to "
                "shard registry",
                "Adding new shard to shard registry",
                "shardId"_attr = shard->getId(),
                "shardConnectionString"_attr = connString);
    if (connString.type() == ConnectionString::SET) {
        _rsLookup[connString.getSetName()] = shard;
    } else if (connString.type() == ConnectionString::CUSTOM) {
        // CUSTOM connection strings (ie "$dummy:10000) become DBDirectClient connections which
        // always return "localhost" as their response to getServerAddress().  This is just for
        // making dbtest work.
        _shardIdLookup[ShardId("localhost")] = shard;
        _hostLookup[HostAndPort("localhost")] = shard;
    }

    _connStringLookup[connString] = shard;

    for (const HostAndPort& hostAndPort : connString.getServers()) {
        _hostLookup[hostAndPort] = shard;
    }
}

void ShardRegistryData::toBSON(BSONObjBuilder* map,
                               BSONObjBuilder* hosts,
                               BSONObjBuilder* connStrings) const {
    std::vector<std::shared_ptr<Shard>> shards;
    getAllShards(shards);

    std::sort(std::begin(shards),
              std::end(shards),
              [](std::shared_ptr<const Shard> lhs, std::shared_ptr<const Shard> rhs) {
                  return lhs->getId() < rhs->getId();
              });

    if (map) {
        for (auto&& shard : shards) {
            map->append(shard->getId(), shard->getConnString().toString());
        }
    }

    if (hosts) {
        for (const auto& hostIt : _hostLookup) {
            hosts->append(hostIt.first.toString(), hostIt.second->getId());
        }
    }

    if (connStrings) {
        for (const auto& connStringIt : _connStringLookup) {
            connStrings->append(connStringIt.first.toString(), connStringIt.second->getId());
        }
    }
}

void ShardRegistryData::toBSON(BSONObjBuilder* result) const {
    std::vector<std::shared_ptr<Shard>> shards;
    getAllShards(shards);

    std::sort(std::begin(shards),
              std::end(shards),
              [](std::shared_ptr<const Shard> lhs, std::shared_ptr<const Shard> rhs) {
                  return lhs->getId() < rhs->getId();
              });

    BSONObjBuilder mapBob(result->subobjStart("map"));
    for (auto&& shard : shards) {
        mapBob.append(shard->getId(), shard->getConnString().toString());
    }
    mapBob.done();

    BSONObjBuilder hostsBob(result->subobjStart("hosts"));
    for (const auto& hostIt : _hostLookup) {
        hostsBob.append(hostIt.first.toString(), hostIt.second->getId());
    }
    hostsBob.done();

    BSONObjBuilder connStringsBob(result->subobjStart("connStrings"));
    for (const auto& connStringIt : _connStringLookup) {
        connStringsBob.append(connStringIt.first.toString(), connStringIt.second->getId());
    }
    connStringsBob.done();
}

BSONObj ShardRegistryData::toBSON() const {
    BSONObjBuilder bob;
    toBSON(&bob);
    return bob.obj();
}

}  // namespace mongo
