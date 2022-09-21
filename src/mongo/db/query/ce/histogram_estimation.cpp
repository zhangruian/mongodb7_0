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

#include "mongo/db/query/ce/histogram_estimation.h"
#include "mongo/db/exec/sbe/abt/abt_lower.h"
#include "mongo/db/query/ce/value_utils.h"
#include "mongo/db/query/optimizer/syntax/expr.h"

namespace mongo::ce {
using namespace sbe;

EstimationResult getTotals(const ScalarHistogram& h) {
    if (h.empty()) {
        return {0.0, 0.0};
    }

    const Bucket& last = h.getBuckets().back();
    return {last._cumulativeFreq, last._cumulativeNDV};
}

/**
 * Helper function that uses linear interpolation to estimate the cardinality and NDV for a value
 * that falls inside of a histogram bucket.
 */
EstimationResult interpolateEstimateInBucket(const ScalarHistogram& h,
                                             value::TypeTags tag,
                                             value::Value val,
                                             EstimationType type,
                                             size_t bucketIndex) {

    const Bucket& bucket = h.getBuckets().at(bucketIndex);
    const auto [boundTag, boundVal] = h.getBounds().getAt(bucketIndex);

    double resultCard = bucket._cumulativeFreq - bucket._equalFreq - bucket._rangeFreq;
    double resultNDV = bucket._cumulativeNDV - bucket._ndv - 1.0;

    // Check if the estimate is at the point of type brackets switch. If the current bucket is the
    // first bucket of a new type bracket and the value is of another type, estimate cardinality
    // from the current bucket as 0.
    //
    // For example, let bound 1 = 1000, bound 2 = "abc". The value 100000000 falls in bucket 2, the
    // first bucket for strings, but should not get cardinality/ ndv fraction from it.
    if (!sameTypeBracket(tag, boundTag)) {
        if (type == EstimationType::kEqual) {
            return {0.0, 0.0};
        } else {
            return {resultCard, resultNDV};
        }
    }

    // Estimate for equality frequency inside of the bucket.
    const double innerEqFreq = (bucket._ndv == 0.0) ? 0.0 : bucket._rangeFreq / bucket._ndv;

    if (type == EstimationType::kEqual) {
        return {innerEqFreq, 1.0};
    }

    // For $lt and $lte operations use linear interpolation to take a fraction of the bucket
    // cardinality and NDV if there is a preceeding bucket with bound of the same type. Use half of
    // the bucket estimates otherwise.
    double ratio = 0.5;
    if (bucketIndex > 0) {
        const auto [lowBoundTag, lowBoundVal] = h.getBounds().getAt(bucketIndex - 1);
        if (sameTypeBracket(lowBoundTag, boundTag)) {
            double doubleLowBound = valueToDouble(lowBoundTag, lowBoundVal);
            double doubleUpperBound = valueToDouble(boundTag, boundVal);
            double doubleVal = valueToDouble(tag, val);
            ratio = (doubleVal - doubleLowBound) / (doubleUpperBound - doubleLowBound);
        }
    }

    resultCard += bucket._rangeFreq * ratio;
    resultNDV += bucket._ndv * ratio;

    if (type == EstimationType::kLess) {
        // Subtract from the estimate the cardinality and ndv corresponding to the equality
        // operation.
        const double innerEqNdv = (bucket._ndv * ratio <= 1.0) ? 0.0 : 1.0;
        resultCard -= innerEqFreq;
        resultNDV -= innerEqNdv;
    }
    return {resultCard, resultNDV};
}

EstimationResult estimate(const ScalarHistogram& h,
                          value::TypeTags tag,
                          value::Value val,
                          EstimationType type) {
    switch (type) {
        case EstimationType::kGreater:
            return getTotals(h) - estimate(h, tag, val, EstimationType::kLessOrEqual);

        case EstimationType::kGreaterOrEqual:
            return getTotals(h) - estimate(h, tag, val, EstimationType::kLess);

        default:
            // Continue.
            break;
    }

    size_t bucketIndex = 0;
    {
        size_t len = h.getBuckets().size();
        while (len > 0) {
            const size_t half = len >> 1;
            const auto [boundTag, boundVal] = h.getBounds().getAt(bucketIndex + half);

            if (compareValues(boundTag, boundVal, tag, val) < 0) {
                bucketIndex += half + 1;
                len -= half + 1;
            } else {
                len = half;
            }
        }
    }
    if (bucketIndex == h.getBuckets().size()) {
        // Value beyond the largest endpoint.
        switch (type) {
            case EstimationType::kEqual:
                return {0.0, 0.0};

            case EstimationType::kLess:
            case EstimationType::kLessOrEqual:
                return getTotals(h);

            default:
                MONGO_UNREACHABLE;
        }
    }

    const Bucket& bucket = h.getBuckets().at(bucketIndex);
    const auto [boundTag, boundVal] = h.getBounds().getAt(bucketIndex);
    const bool isEndpoint = compareValues(boundTag, boundVal, tag, val) == 0;

    if (isEndpoint) {
        switch (type) {
            case EstimationType::kEqual: {
                return {bucket._equalFreq, 1.0};
            }

            case EstimationType::kLess: {
                double resultCard = bucket._cumulativeFreq - bucket._equalFreq;
                double resultNDV = bucket._cumulativeNDV - 1.0;
                return {resultCard, resultNDV};
            }

            case EstimationType::kLessOrEqual: {
                double resultCard = bucket._cumulativeFreq;
                double resultNDV = bucket._cumulativeNDV;
                return {resultCard, resultNDV};
            }

            default:
                MONGO_UNREACHABLE;
        }
    } else {
        return interpolateEstimateInBucket(h, tag, val, type, bucketIndex);
    }
}

double estimateCardEq(const ArrayHistogram& ah,
                      value::TypeTags tag,
                      value::Value val,
                      bool includeScalar) {
    if (tag != value::TypeTags::Null) {
        double card = 0.0;
        if (includeScalar) {
            card = estimate(ah.getScalar(), tag, val, EstimationType::kEqual).card;
        }
        if (ah.isArray()) {
            card += estimate(ah.getArrayUnique(), tag, val, EstimationType::kEqual).card;
        }
        return card;
    } else {
        // Predicate: {field: null}
        // Count the values that are either null or that do not contain the field.
        // TODO:
        // This prototype doesn't have the concept of missing values. It can be added easily
        // by adding a cardinality estimate that is >= the number of values.
        // Estimation of $exists can be built on top of this estimate:
        // {$exists: true} matches the documents that contain the field, including those where the
        // field value is null.
        // {$exists: false} matches only the documents that do not contain the field.
        auto findNull = ah.getTypeCounts().find(value::TypeTags::Null);
        if (findNull != ah.getTypeCounts().end()) {
            return findNull->second;
        }
        return 0.0;
    }
}

static EstimationResult estimateRange(const ScalarHistogram& histogram,
                                      bool lowInclusive,
                                      value::TypeTags tagLow,
                                      value::Value valLow,
                                      bool highInclusive,
                                      value::TypeTags tagHigh,
                                      value::Value valHigh) {
    const EstimationType highType =
        highInclusive ? EstimationType::kLessOrEqual : EstimationType::kLess;
    const EstimationResult highEstimate = estimate(histogram, tagHigh, valHigh, highType);

    const EstimationType lowType =
        lowInclusive ? EstimationType::kLess : EstimationType::kLessOrEqual;
    const EstimationResult lowEstimate = estimate(histogram, tagLow, valLow, lowType);

    return highEstimate - lowEstimate;
}

/**
 * Compute an estimate for range query on array data with formula:
 * Card(ArrayMin(a < valHigh)) - Card(ArrayMax(a < valLow))
 */
static EstimationResult estimateRangeQueryOnArray(const ScalarHistogram& histogramAmin,
                                                  const ScalarHistogram& histogramAmax,
                                                  bool lowInclusive,
                                                  value::TypeTags tagLow,
                                                  value::Value valLow,
                                                  bool highInclusive,
                                                  value::TypeTags tagHigh,
                                                  value::Value valHigh) {
    const EstimationType highType =
        highInclusive ? EstimationType::kLessOrEqual : EstimationType::kLess;
    const EstimationResult highEstimate = estimate(histogramAmin, tagHigh, valHigh, highType);

    const EstimationType lowType =
        lowInclusive ? EstimationType::kLess : EstimationType::kLessOrEqual;
    const EstimationResult lowEstimate = estimate(histogramAmax, tagLow, valLow, lowType);

    return highEstimate - lowEstimate;
}

double estimateCardRange(const ArrayHistogram& ah,
                         /* Define lower bound. */
                         bool lowInclusive,
                         value::TypeTags tagLow,
                         value::Value valLow,
                         /* Define upper bound. */
                         bool highInclusive,
                         value::TypeTags tagHigh,
                         value::Value valHigh,
                         bool includeScalar,
                         EstimationAlgo estimationAlgo) {
    uassert(6695701,
            "Low bound must not be higher than high",
            compareValues(tagLow, valLow, tagHigh, valHigh) <= 0);

    // Helper lambda to shorten code for legibility.
    auto estRange = [&](const ScalarHistogram& h) {
        return estimateRange(h, lowInclusive, tagLow, valLow, highInclusive, tagHigh, valHigh);
    };

    double result = 0.0;
    if (ah.isArray()) {

        if (includeScalar) {
            // Range query on array data.
            const EstimationResult rangeCardOnArray = estimateRangeQueryOnArray(ah.getArrayMin(),
                                                                                ah.getArrayMax(),
                                                                                lowInclusive,
                                                                                tagLow,
                                                                                valLow,
                                                                                highInclusive,
                                                                                tagHigh,
                                                                                valHigh);
            result += rangeCardOnArray.card;
        } else {
            // $elemMatch query on array data.
            const auto arrayMinEst = estRange(ah.getArrayMin());
            const auto arrayMaxEst = estRange(ah.getArrayMax());
            const auto arrayUniqueEst = estRange(ah.getArrayUnique());

            const double totalArrayCount = getTotals(ah.getArrayMin()).card;
            uassert(
                6715101, "Array histograms should contain at least one array", totalArrayCount > 0);
            switch (estimationAlgo) {
                case EstimationAlgo::HistogramV1: {
                    const double arrayUniqueDensity = (arrayUniqueEst.ndv == 0.0)
                        ? 0.0
                        : (arrayUniqueEst.card / std::sqrt(arrayUniqueEst.ndv));
                    result =
                        std::max(std::max(arrayMinEst.card, arrayMaxEst.card), arrayUniqueDensity);
                    break;
                }
                case EstimationAlgo::HistogramV2: {
                    const double avgArraySize =
                        getTotals(ah.getArrayUnique()).card / totalArrayCount;
                    const double adjustedUniqueCard = (avgArraySize == 0.0)
                        ? 0.0
                        : std::min(arrayUniqueEst.card / pow(avgArraySize, 0.2), totalArrayCount);
                    result =
                        std::max(std::max(arrayMinEst.card, arrayMaxEst.card), adjustedUniqueCard);
                    break;
                }
                case EstimationAlgo::HistogramV3: {
                    const double adjustedUniqueCard =
                        0.85 * std::min(arrayUniqueEst.card, totalArrayCount);
                    result =
                        std::max(std::max(arrayMinEst.card, arrayMaxEst.card), adjustedUniqueCard);
                    break;
                }
                default:
                    MONGO_UNREACHABLE;
            }
        }
    }

    if (includeScalar) {
        const auto scalarEst = estRange(ah.getScalar());
        result += scalarEst.card;
    }

    return result;
}

double estimateIntervalCardinality(const ce::ArrayHistogram& ah,
                                   const optimizer::IntervalRequirement& interval,
                                   optimizer::CEType childResult,
                                   bool includeScalar) {
    auto getBound = [](const optimizer::BoundRequirement& boundReq) {
        return boundReq.getBound().cast<optimizer::Constant>()->get();
    };

    if (interval.isFullyOpen()) {
        return childResult;
    } else if (interval.isEquality()) {
        auto [tag, val] = getBound(interval.getLowBound());
        return estimateCardEq(ah, tag, val, includeScalar);
    }

    // Otherwise, we have a range.
    auto lowBound = interval.getLowBound();
    auto [lowTag, lowVal] = getBound(lowBound);

    auto highBound = interval.getHighBound();
    auto [highTag, highVal] = getBound(highBound);

    return estimateCardRange(ah,
                             lowBound.isInclusive(),
                             lowTag,
                             lowVal,
                             highBound.isInclusive(),
                             highTag,
                             highVal,
                             includeScalar);
}

}  // namespace mongo::ce
