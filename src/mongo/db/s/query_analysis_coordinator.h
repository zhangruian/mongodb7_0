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

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/service_context.h"
#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/util/periodic_runner.h"

namespace mongo {
namespace analyze_shard_key {

/**
 * Keeps track of all samplers in the cluster and assigns sample rates to each sampler based on its
 * view of the query distribution across the samplers.
 *
 * Currently, query sampling is only supported on a sharded cluster. So a sampler must be a mongos
 * and the coordinator must be the config server's primary mongod.
 */
class QueryAnalysisCoordinator : public ReplicaSetAwareService<QueryAnalysisCoordinator> {
public:
    QueryAnalysisCoordinator() = default;

    /**
     * Obtains the service-wide QueryAnalysisCoordinator instance.
     */
    static QueryAnalysisCoordinator* get(OperationContext* opCtx);
    static QueryAnalysisCoordinator* get(ServiceContext* serviceContext);

    void onStartup(OperationContext* opCtx) override final;

    void onStepUpBegin(OperationContext* opCtx, long long term) override final{};

    /**
     * Creates, updates and deletes the configuration for the collection with the given
     * config.queryAnalyzers document.
     */
    void onConfigurationInsert(const BSONObj& doc);
    void onConfigurationUpdate(const BSONObj& doc);
    void onConfigurationDelete(const BSONObj& doc);

    std::map<UUID, CollectionQueryAnalyzerConfiguration> getConfigurationsForTest() const {
        stdx::lock_guard<Latch> lk(_mutex);
        return _configurations;
    }

    void clearConfigurationsForTest() {
        stdx::lock_guard<Latch> lk(_mutex);
        _configurations.clear();
    }

private:
    bool shouldRegisterReplicaSetAwareService() const override final;

    void onInitialDataAvailable(OperationContext* opCtx,
                                bool isMajorityDataAvailable) override final {}

    void onShutdown() override final {}

    void onStepUpComplete(OperationContext* opCtx, long long term) override final {}

    void onStepDown() override final {}

    void onBecomeArbiter() override final {}

    mutable Mutex _mutex = MONGO_MAKE_LATCH("QueryAnalysisCoordinator::_mutex");
    std::map<UUID, CollectionQueryAnalyzerConfiguration> _configurations;
};

}  // namespace analyze_shard_key
}  // namespace mongo
