/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/query/collection_query_info.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/collection_index_usage_tracker.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_settings.h"
#include "mongo/db/update_index_data.h"

namespace mongo {

class IndexDescriptor;
class OperationContext;

/**
 * this is for storing things that you want to cache about a single collection
 * life cycle is managed for you from inside Collection
 */
class CollectionQueryInfo {
public:
    CollectionQueryInfo();

    inline static const auto get = Collection::declareDecoration<CollectionQueryInfo>();

    /**
     * Get the PlanCache for this collection.
     */
    PlanCache* getPlanCache() const;

    /**
     * Get the QuerySettings for this collection.
     */
    QuerySettings* getQuerySettings() const;

    /* get set of index keys for this namespace.  handy to quickly check if a given
       field is indexed (Note it might be a secondary component of a compound index.)
    */
    const UpdateIndexData& getIndexKeys(OperationContext* opCtx) const;

    /**
     * Returns cached index usage statistics for this collection.  The map returned will contain
     * entry for each index in the collection along with both a usage counter and a timestamp
     * representing the date/time the counter is valid from.
     *
     * Note for performance that this method returns a copy of a StringMap.
     */
    CollectionIndexUsageMap getIndexUsageStats() const;

    CollectionIndexUsageTracker::CollectionScanStats getCollectionScanStats() const;

    /**
     * Builds internal cache state based on the current state of the Collection's IndexCatalog.
     */
    void init(OperationContext* opCtx, Collection* coll);

    /**
     * Register a newly-created index with the cache.  Must be called whenever an index is
     * built on the associated collection.
     *
     * Must be called under exclusive collection lock.
     */
    void addedIndex(OperationContext* opCtx, Collection* coll, const IndexDescriptor* desc);

    /**
     * Deregister a newly-dropped index with the cache.  Must be called whenever an index is
     * dropped on the associated collection.
     *
     * Must be called under exclusive collection lock.
     */
    void droppedIndex(OperationContext* opCtx, Collection* coll, StringData indexName);

    /**
     * Removes all cached query plans.
     */
    void clearQueryCache(const Collection* coll);

    void notifyOfQuery(OperationContext* opCtx,
                       Collection* coll,
                       const PlanSummaryStats& summaryStats);

private:
    void computeIndexKeys(OperationContext* opCtx, Collection* coll);
    void updatePlanCacheIndexEntries(OperationContext* opCtx, Collection* coll);

    /**
     * Rebuilds cached information that is dependent on index composition. Must be called
     * when index composition changes.
     */
    void rebuildIndexData(OperationContext* opCtx, Collection* coll);

    // ---  index keys cache
    bool _keysComputed;
    UpdateIndexData _indexedPaths;

    // A cache for query plans.
    std::unique_ptr<PlanCache> _planCache;

    // Query settings.
    // Includes index filters.
    std::unique_ptr<QuerySettings> _querySettings;

    // Tracks index usage statistics for this collection.
    CollectionIndexUsageTracker _indexUsageTracker;
};

}  // namespace mongo
