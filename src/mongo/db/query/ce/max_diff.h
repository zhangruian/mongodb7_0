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

#include <utility>
#include <vector>

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/ce/array_histogram.h"
#include "mongo/db/query/ce/scalar_histogram.h"
#include "mongo/db/query/ce/value_utils.h"

namespace mongo::ce {

struct ValFreq {
    ValFreq(size_t idx, size_t freq) : _idx(idx), _freq(freq), _area(-1.0), _normArea(-1) {}

    std::string toString() const {
        std::ostringstream os;
        os << "idx: " << _idx << ", freq: " << _freq << ", area: " << _area
           << ", normArea: " << _normArea;
        return os.str();
    }

    size_t _idx;       // Original index according to value order.
    size_t _freq;      // Frequency of the value.
    double _area;      // Derived as: spread * frequency
    double _normArea;  // Area normalized to the maximum in a type class.
};

struct DataDistribution {
    std::vector<SBEValue> _bounds;
    std::vector<ValFreq> _freq;
    // The min/max areas of each type class. The key is the index of the last boundary of the class.
    std::map<size_t, double> typeClassBounds;
};

/**
    Given a set of values sorted in BSON order, generate a data distribution consisting of
    counts for each value with the values in sorted order
*/
DataDistribution getDataDistribution(const std::vector<SBEValue>& sortedInput);

/**
    Given a data distribution, generate a scalar histogram with the supplied number of buckets
*/
ScalarHistogram genMaxDiffHistogram(const DataDistribution& dataDistrib, size_t numBuckets);

/**
    Given a vector containing SBEValues, generate a set of statistics to summarize the supplied
    data. Histograms will use the supplied number of buckets.
*/
ArrayHistogram createArrayEstimator(const std::vector<SBEValue>& arrayData, size_t nBuckets);

}  // namespace mongo::ce
