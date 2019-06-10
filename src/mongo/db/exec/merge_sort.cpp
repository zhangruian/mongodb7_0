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

#include "mongo/db/exec/merge_sort.h"

#include <memory>

#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/util/str.h"

namespace mongo {

using std::list;
using std::string;
using std::unique_ptr;
using std::vector;

// static
const char* MergeSortStage::kStageType = "SORT_MERGE";

MergeSortStage::MergeSortStage(OperationContext* opCtx,
                               const MergeSortStageParams& params,
                               WorkingSet* ws)
    : PlanStage(kStageType, opCtx),
      _ws(ws),
      _pattern(params.pattern),
      _collator(params.collator),
      _dedup(params.dedup),
      _merging(StageWithValueComparison(ws, params.pattern, params.collator)) {}

void MergeSortStage::addChild(PlanStage* child) {
    _children.emplace_back(child);

    // We have to call work(...) on every child before we can pick a min.
    _noResultToMerge.push(child);
}

bool MergeSortStage::isEOF() {
    // If we have no more results to return, and we have no more children that we can call
    // work(...) on to get results, we're done.
    return _merging.empty() && _noResultToMerge.empty();
}

PlanStage::StageState MergeSortStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    if (!_noResultToMerge.empty()) {
        // We have some child that we don't have a result from.  Each child must have a result
        // in order to pick the minimum result among all our children.  Work a child.
        PlanStage* child = _noResultToMerge.front();
        WorkingSetID id = WorkingSet::INVALID_ID;
        StageState code = child->work(&id);

        if (PlanStage::ADVANCED == code) {
            WorkingSetMember* member = _ws->get(id);

            // If we're deduping...
            if (_dedup) {
                if (!member->hasRecordId()) {
                    // Can't dedup data unless there's a RecordId.  We go ahead and use its
                    // result.
                    _noResultToMerge.pop();
                } else {
                    ++_specificStats.dupsTested;
                    // ...and there's a RecordId and and we've seen the RecordId before
                    if (_seen.end() != _seen.find(member->recordId)) {
                        // ...drop it.
                        _ws->free(id);
                        ++_specificStats.dupsDropped;
                        return PlanStage::NEED_TIME;
                    } else {
                        // Otherwise, note that we've seen it.
                        _seen.insert(member->recordId);
                        // We're going to use the result from the child, so we remove it from
                        // the queue of children without a result.
                        _noResultToMerge.pop();
                    }
                }
            } else {
                // Not deduping.  We use any result we get from the child.  Remove the child
                // from the queue of things without a result.
                _noResultToMerge.pop();
            }

            // Store the result in our list.
            StageWithValue value;
            value.id = id;
            value.stage = child;
            // Ensure that the BSONObj underlying the WorkingSetMember is owned in case we yield.
            member->makeObjOwnedIfNeeded();
            _mergingData.push_front(value);

            // Insert the result (indirectly) into our priority queue.
            _merging.push(_mergingData.begin());

            return PlanStage::NEED_TIME;
        } else if (PlanStage::IS_EOF == code) {
            // There are no more results possible from this child.  Don't bother with it
            // anymore.
            _noResultToMerge.pop();
            return PlanStage::NEED_TIME;
        } else if (PlanStage::FAILURE == code) {
            // The stage which produces a failure is responsible for allocating a working set member
            // with error details.
            invariant(WorkingSet::INVALID_ID != id);
            *out = id;
            return code;
        } else if (PlanStage::NEED_YIELD == code) {
            *out = id;
            return code;
        } else {
            return code;
        }
    }

    // If we're here, for each non-EOF child, we have a valid WSID.
    verify(!_merging.empty());

    // Get the 'min' WSID.  _merging is a priority queue so its top is the smallest.
    MergingRef top = _merging.top();
    _merging.pop();

    // Since we're returning the WSID that came from top->stage, we need to work(...) it again
    // to get a new result.
    _noResultToMerge.push(top->stage);

    // Save the ID that we're returning and remove the returned result from our data.
    WorkingSetID idToTest = top->id;
    _mergingData.erase(top);

    // Return the min.
    *out = idToTest;

    return PlanStage::ADVANCED;
}

// Is lhs less than rhs?  Note that priority_queue is a max heap by default so we invert
// the return from the expected value.
bool MergeSortStage::StageWithValueComparison::operator()(const MergingRef& lhs,
                                                          const MergingRef& rhs) {
    WorkingSetMember* lhsMember = _ws->get(lhs->id);
    WorkingSetMember* rhsMember = _ws->get(rhs->id);

    BSONObjIterator it(_pattern);
    while (it.more()) {
        BSONElement patternElt = it.next();
        string fn = patternElt.fieldName();

        BSONElement lhsElt;
        verify(lhsMember->getFieldDotted(fn, &lhsElt));

        BSONElement rhsElt;
        verify(rhsMember->getFieldDotted(fn, &rhsElt));

        // false means don't compare field name.
        int x = lhsElt.woCompare(rhsElt, false, _collator);
        if (-1 == patternElt.number()) {
            x = -x;
        }
        if (x != 0) {
            return x > 0;
        }
    }

    // A comparator for use with sort is required to model a strict weak ordering, so
    // to satisfy irreflexivity we must return 'false' for elements that we consider
    // equivalent under the pattern.
    return false;
}

unique_ptr<PlanStageStats> MergeSortStage::getStats() {
    _commonStats.isEOF = isEOF();

    _specificStats.sortPattern = _pattern;

    unique_ptr<PlanStageStats> ret =
        std::make_unique<PlanStageStats>(_commonStats, STAGE_SORT_MERGE);
    ret->specific = std::make_unique<MergeSortStats>(_specificStats);
    for (size_t i = 0; i < _children.size(); ++i) {
        ret->children.emplace_back(_children[i]->getStats());
    }
    return ret;
}

const SpecificStats* MergeSortStage::getSpecificStats() const {
    return &_specificStats;
}

}  // namespace mongo
