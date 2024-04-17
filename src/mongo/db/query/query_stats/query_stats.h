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

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/curop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/partitioned_cache.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/query_stats/key.h"
#include "mongo/db/query/query_stats/query_stats_entry.h"
#include "mongo/db/query/query_stats/rate_limiting.h"
#include "mongo/db/service_context.h"
#include "mongo/db/views/view.h"
#include <cstdint>
#include <memory>

namespace mongo::query_stats {

extern CounterMetric queryStatsStoreSizeEstimateBytesMetric;

struct QueryStatsPartitioner {
    // The partitioning function for use with the 'Partitioned' utility.
    std::size_t operator()(const std::size_t hash, const std::size_t nPartitions) const {
        return hash % nPartitions;
    }
};

struct QueryStatsStoreEntryBudgetor {
    size_t operator()(const std::size_t hash, const QueryStatsEntry& value) {
        return sizeof(decltype(value)) + sizeof(decltype(hash)) + value.key->size();
    }
};

/*
 * 'QueryStatsStore insertion and eviction listener implementation. This class adjusts the
 * 'queryStatsStoreSize' serverStatus metric when entries are inserted or evicted.
 */
struct QueryStatsStoreInsertionEvictionListener {
    void onInsert(const std::size_t&, const QueryStatsEntry&, size_t estimatedSize) {
        queryStatsStoreSizeEstimateBytesMetric.increment(estimatedSize);
    }

    void onEvict(const std::size_t&, const QueryStatsEntry&, size_t estimatedSize) {
        queryStatsStoreSizeEstimateBytesMetric.decrement(estimatedSize);
    }

    void onClear(size_t estimatedSize) {
        queryStatsStoreSizeEstimateBytesMetric.decrement(estimatedSize);
    }
};
using QueryStatsStore = PartitionedCache<std::size_t,
                                         QueryStatsEntry,
                                         QueryStatsStoreEntryBudgetor,
                                         QueryStatsPartitioner,
                                         QueryStatsStoreInsertionEvictionListener>;

/**
 * A manager for the queryStats store allows a "pointer swap" on the queryStats store itself. The
 * usage patterns are as follows:
 *
 * - Updating the queryStats store uses the `getQueryStatsStore()` method. The queryStats store
 *   instance is obtained, entries are looked up and mutated, or created anew.
 * - The queryStats store is "reset". This involves atomically allocating a new instance, once
 * there are no more updaters (readers of the store "pointer"), and returning the existing
 * instance.
 */
class QueryStatsStoreManager {
public:
    // The query stats store can be configured using these objects on a per-ServiceContext level.
    // This is essentially global, but can be manipulated by unit tests.
    static const ServiceContext::Decoration<std::unique_ptr<QueryStatsStoreManager>> get;
    static const ServiceContext::Decoration<std::unique_ptr<RateLimiting>> getRateLimiter;

    template <typename... QueryStatsStoreArgs>
    QueryStatsStoreManager(size_t cacheSize, size_t numPartitions)
        : _queryStatsStore(std::make_unique<QueryStatsStore>(cacheSize, numPartitions)),
          _maxSize(cacheSize) {}

    /**
     * Acquire the instance of the queryStats store.
     */
    QueryStatsStore& getQueryStatsStore() {
        return *_queryStatsStore;
    }

    size_t getMaxSize() {
        return _maxSize.load();
    }

    /**
     * Resize the queryStats store and return the number of evicted
     * entries.
     */
    size_t resetSize(size_t cacheSize) {
        _maxSize.store(cacheSize);
        return _queryStatsStore->reset(cacheSize);
    }

private:
    std::unique_ptr<QueryStatsStore> _queryStatsStore;

    /**
     * Max size of the queryStats store. Tracked here to avoid having to recompute after it's
     * divided up into partitions.
     */
    AtomicWord<size_t> _maxSize;
};

/**
 * Acquire a reference to the global queryStats store.
 */
QueryStatsStore& getQueryStatsStore(OperationContext* opCtx);

/**
 * Indicates whether or not query stats is enabled via the feature flags. If
 * requiresFullQueryStatsFeatureFlag is true, it will only return true if featureFlagQueryStats is
 * enabled. Otherwise, it will return true if either featureFlagQueryStats or
 * featureFlagQueryStatsFindCommand is enabled.
 */
bool isQueryStatsFeatureEnabled(bool requiresFullQueryStatsFeatureFlag);

/**
 * Registers a request for query stats collection. The function may decide not to collect anything,
 * so this should be called for all requests. The decision is made based on the feature flag and
 * query stats rate limiting.
 *
 * The originating command/query does not persist through the end of query execution due to
 * optimizations made to the original query and the expiration of OpCtx across getMores. In order
 * to pair the query stats metrics that are collected at the end of execution with the original
 * query, it is necessary to store the original query during planning and persist it through
 * getMores.
 *
 * During planning, registerRequest is called to serialize the query stats key and save it to
 * OpDebug. If a query's execution is complete within the original operation,
 * collectQueryStatsMongod/collectQueryStatsMongos will call writeQueryStats() and pass along the
 * query stats key to be saved in the query stats store alongside metrics collected.
 *
 * However, OpDebug does not persist through cursor iteration, so if a query's execution will span
 * more than one request/operation, it's necessary to save the query stats context to the cursor
 * upon cursor registration. In these cases, collectQueryStatsMongod/collectQueryStatsMongos will
 * aggregate each operation's metrics within the cursor. Once the request is eventually complete,
 * the cursor calls writeQueryStats() on its destruction.
 *
 * Notes:
 * - It's important to call registerRequest with the original request, before canonicalizing or
 *   optimizing it, in order to preserve the user's input for the query shape.
 * - Calling this affects internal state. It should be called exactly once for each request for
 *   which query stats may be collected.
 * - The std::function argument to construct an abstracted Key is provided to break
 *   library cycles so this library does not need to know how to parse everything. It is done as a
 *   deferred construction callback to ensure that this feature does not impact performance if
 *   collecting stats is not needed due to the feature being disabled or the request being rate
 *   limited.
 * - Since we currently have 2 feature flags (one for full query stats, and one for
 *   find-command-only query stats), we use the requiresFullQueryStatsFeatureFlag parameter to
 * denote which requests should only be registered when the full feature flag is enabled. TODO
 * SERVER-79494 Remove requiresFullQueryStatsFeatureFlag parameter.
 */
void registerRequest(OperationContext* opCtx,
                     const NamespaceString& collection,
                     std::function<std::unique_ptr<Key>(void)> makeKey,
                     bool requiresFullQueryStatsFeatureFlag = true,
                     bool willNeverExhaust = false);

/**
 * Writes query stats to the query stats store for the operation identified by `queryStatsKeyHash`.
 *
 * Direct calls to writeQueryStats in new code should be avoided in favor of calling existing
 * functions:
 *  - collectQueryStatsMongod/collectQueryStatsMongos in the case of requests that span one
 *    operation
 *  - writeQueryStatsOnCursorDisposeOrKill() in the case of requests that span
 *    multiple operations (via getMore)
 */
void writeQueryStats(OperationContext* opCtx,
                     boost::optional<size_t> queryStatsKeyHash,
                     std::unique_ptr<Key> key,
                     uint64_t queryExecMicros,
                     uint64_t firstResponseExecMicros,
                     uint64_t docsReturned,
                     bool willNeverExhaust = false);

/**
 * Called from ClientCursor::dispose/ClusterClientCursorImpl::kill to set up and writeQueryStats()
 * at the end of life of a cursor.
 */
void writeQueryStatsOnCursorDisposeOrKill(OperationContext* opCtx,
                                          boost::optional<size_t> queryStatsKeyHash,
                                          std::unique_ptr<Key> key,
                                          bool willNeverExhaust,
                                          uint64_t queryExecMicros,
                                          uint64_t firstResponseExecMicros,
                                          uint64_t docsReturned);
}  // namespace mongo::query_stats
