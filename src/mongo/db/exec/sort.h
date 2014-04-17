/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/scoped_ptr.hpp>
#include <vector>
#include <set>

#include "mongo/db/diskloc.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/platform/unordered_map.h"

namespace mongo {

    class BtreeKeyGenerator;

    // Parameters that must be provided to a SortStage
    class SortStageParams {
    public:
        SortStageParams() : collection( NULL), limit(0) { }

        Collection* collection;

        // How we're sorting.
        BSONObj pattern;

        // The query.  Used to create the IndexBounds for the sorting.
        BSONObj query;

        // Equal to 0 for no limit.
        size_t limit;
    };

    /**
     * Maps a WSM value to a BSONObj key that can then be sorted via BSONObjCmp.
     */
    class SortStageKeyGenerator {
    public:
        /**
         * 'sortSpec' is the BSONObj in the .sort(...) clause.
         *
         * 'queryObj' is the BSONObj in the .find(...) clause.  For multikey arrays we have to
         * ensure that the value we select to sort by is within bounds generated by
         * executing 'queryObj' using the virtual index with key pattern 'sortSpec'.
         */
        SortStageKeyGenerator(Collection* collection,
                              const BSONObj& sortSpec,
                              const BSONObj& queryObj);

        /**
         * Returns the key used to sort 'member'.
         */
        Status getSortKey(const WorkingSetMember& member,
                          BSONObj* objOut) const;

        /**
         * Passed to std::sort and used to sort the keys that are returned from getSortKey.
         *
         * Returned reference lives as long as 'this'.
         */
        const BSONObj& getSortComparator() const { return _comparatorObj; }

    private:
        Status getBtreeKey(const BSONObj& memberObj, BSONObj* objOut) const;

        /**
         * In order to emulate the existing sort behavior we must make unindexed sort behavior as
         * consistent as possible with indexed sort behavior.  As such, we must only consider index
         * keys that we would encounter if we were answering the query using the sort-providing
         * index.
         *
         * Populates _hasBounds and _bounds.
         */
        void getBoundsForSort(const BSONObj& queryObj,
                              const BSONObj& sortObj);

        Collection* _collection;

        // The object that we use to call woCompare on our resulting key.  Is equal to _rawSortSpec
        // unless we have some $meta expressions.  Each $meta expression has a default sort order.
        BSONObj _comparatorObj;

        // The raw object in .sort()
        BSONObj _rawSortSpec;

        // The sort pattern with any non-Btree sort pulled out.
        BSONObj _btreeObj;

        // If we're not sorting with a $meta value we can short-cut some work.
        bool _sortHasMeta;

        // True if the bounds are valid.
        bool _hasBounds;

        // The bounds generated from the query we're sorting.
        IndexBounds _bounds;

        // Helper to extract sorting keys from documents.
        boost::scoped_ptr<BtreeKeyGenerator> _keyGen;

        // Helper to filter keys, ensuring keys generated with _keyGen are within _bounds.
        boost::scoped_ptr<IndexBoundsChecker> _boundsChecker;
    };

    /**
     * Sorts the input received from the child according to the sort pattern provided.
     *
     * Preconditions: For each field in 'pattern', all inputs in the child must handle a
     * getFieldDotted for that field.
     */
    class SortStage : public PlanStage {
    public:
        SortStage(const SortStageParams& params, WorkingSet* ws, PlanStage* child);

        virtual ~SortStage();

        virtual bool isEOF();
        virtual StageState work(WorkingSetID* out);

        virtual void prepareToYield();
        virtual void recoverFromYield();
        virtual void invalidate(const DiskLoc& dl, InvalidationType type);

        PlanStageStats* getStats();

    private:
        void getBoundsForSort(const BSONObj& queryObj, const BSONObj& sortObj);

        //
        // Query Stage
        //

        Collection* _collection;

        // Not owned by us.
        WorkingSet* _ws;

        // Where we're reading data to sort from.
        boost::scoped_ptr<PlanStage> _child;

        // The raw sort _pattern as expressed by the user
        BSONObj _pattern;

        // The raw query as expressed by the user
        BSONObj _query;

        // Equal to 0 for no limit.
        size_t _limit;

        //
        // Sort key generation
        //
        boost::scoped_ptr<SortStageKeyGenerator> _sortKeyGen;

        //
        // Data storage
        //

        // Have we sorted our data? If so, we can access _resultIterator. If not,
        // we're still populating _data.
        bool _sorted;

        // Collection of working set members to sort with their respective sort key.
        struct SortableDataItem {
            WorkingSetID wsid;
            BSONObj sortKey;
            // Since we must replicate the behavior of a covered sort as much as possible we use the
            // DiskLoc to break sortKey ties.
            // See sorta.js.
            DiskLoc loc;
        };

        // Comparison object for data buffers (vector and set).
        // Items are compared on (sortKey, loc). This is also how the items are
        // ordered in the indices.
        // Keys are compared using BSONObj::woCompare() with DiskLoc as a tie-breaker.
        struct WorkingSetComparator {
            explicit WorkingSetComparator(BSONObj p);

            bool operator()(const SortableDataItem& lhs, const SortableDataItem& rhs) const;

            BSONObj pattern;
        };

        /**
         * Inserts one item into data buffer (vector or set).
         * If limit is exceeded, remove item with lowest key.
         */
        void addToBuffer(const SortableDataItem& item);

        /**
         * Sorts data buffer.
         * Assumes no more items will be added to buffer.
         * If data is stored in set, copy set
         * contents to vector and clear set.
         */
        void sortBuffer();

        // Comparator for data buffer
        // Initialization follows sort key generator
        scoped_ptr<WorkingSetComparator> _sortKeyComparator;

        // The data we buffer and sort.
        // _data will contain sorted data when all data is gathered
        // and sorted.
        // When _limit is greater than 1 and not all data has been gathered from child stage,
        // _dataSet is used instead to maintain an ordered set of the incomplete data set.
        // When the data set is complete, we copy the items from _dataSet to _data which will
        // be used to provide the results of this stage through _resultIterator.
        vector<SortableDataItem> _data;
        typedef std::set<SortableDataItem, WorkingSetComparator> SortableDataItemSet;
        scoped_ptr<SortableDataItemSet> _dataSet;

        // Iterates through _data post-sort returning it.
        vector<SortableDataItem>::iterator _resultIterator;

        // We buffer a lot of data and we want to look it up by DiskLoc quickly upon invalidation.
        typedef unordered_map<DiskLoc, WorkingSetID, DiskLoc::Hasher> DataMap;
        DataMap _wsidByDiskLoc;

        //
        // Stats
        //

        CommonStats _commonStats;
        SortStats _specificStats;

        // The usage in bytes of all buffered data that we're sorting.
        size_t _memUsage;
    };

}  // namespace mongo
