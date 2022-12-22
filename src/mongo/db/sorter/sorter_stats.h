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

#include "mongo/platform/atomic_word.h"

namespace mongo {

/**
 * For collecting cumulative stats of all sorters.
 */
struct SorterTracker {
    AtomicWord<long long> spilledRanges{0};
    AtomicWord<long long> bytesSpilled{0};
    AtomicWord<long long> bytesSpilledUncompressed{0};
    AtomicWord<long long> numSorted{0};
    AtomicWord<long long> bytesSorted{0};
};

/**
 * For collecting file usage metrics.
 */
class SorterFileStats {
public:
    SorterFileStats(SorterTracker* sorterTracker);

    void addSpilledDataSize(long long data);
    void addSpilledDataSizeUncompressed(long long data);

    AtomicWord<long long> opened;
    AtomicWord<long long> closed;

    long long bytesSpilled() const {
        return _bytesSpilled;
    }

private:
    SorterTracker* _sorterTracker;

    long long _bytesSpilled = 0;
};

/**
 * For collecting individual sorter stats.
 */
class SorterStats {
public:
    SorterStats(SorterTracker* sorterTracker);

    void incrementSpilledRanges();
    /**
     * Sets the number of spilled ranges to the specified amount. Cannot be called after
     * incrementSpilledRanges.
     */
    void setSpilledRanges(uint64_t spills);
    uint64_t spilledRanges() const;

    void incrementNumSorted(uint64_t sortedKeys = 1);
    uint64_t numSorted() const;

    void incrementBytesSorted(uint64_t bytes);
    uint64_t bytesSorted() const;

private:
    uint64_t _spilledRanges = 0;  // Number of spills.
    uint64_t _numSorted = 0;      // Number of keys sorted.
    uint64_t _bytesSorted = 0;    // Total bytes of data sorted.

    // All SorterStats update the SorterTracker to report sorter statistics for the
    // server.
    SorterTracker* _sorterTracker;
};
}  // namespace mongo
