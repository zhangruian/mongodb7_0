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

#include "mongo/db/query/ce/histogram_predicate_estimation.h"

#include "mongo/db/exec/sbe/abt/abt_lower.h"
#include "mongo/db/pipeline/abt/utils.h"

#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/query/optimizer/syntax/expr.h"
#include "mongo/db/query/optimizer/utils/ce_math.h"
#include "mongo/db/query/optimizer/utils/interval_utils.h"
#include "mongo/db/query/stats/value_utils.h"

namespace mongo::optimizer::ce {
namespace value = sbe::value;

using stats::ArrayHistogram;
using stats::Bucket;
using stats::compareValues;
using stats::sameTypeBracket;
using stats::ScalarHistogram;
using stats::valueToDouble;

std::pair<value::TypeTags, value::Value> getConstTypeVal(const ABT& abt) {
    const auto* constant = abt.cast<Constant>();
    tassert(7051102, "Interval ABTs passed in for estimation must have Constant bounds.", constant);
    return constant->get();
};

boost::optional<std::pair<value::TypeTags, value::Value>> getBound(
    const BoundRequirement& boundReq) {
    const ABT& bound = boundReq.getBound();
    if (bound.is<Constant>()) {
        return getConstTypeVal(bound);
    }
    return boost::none;
};

IntervalRequirement getMinMaxIntervalForType(value::TypeTags type) {
    // Note: This function works based on the assumption that there are no intervals that include
    // values from more than one type. That is why the MinMax interval of a type will include all
    // possible intervals over that type.

    auto&& [min, minInclusive] = getMinMaxBoundForType(true /*isMin*/, type);
    tassert(7051103, str::stream() << "Type " << type << " has no minimum", min);

    auto&& [max, maxInclusive] = getMinMaxBoundForType(false /*isMin*/, type);
    tassert(7051104, str::stream() << "Type " << type << " has no maximum", max);

    return IntervalRequirement{BoundRequirement(minInclusive, *min),
                               BoundRequirement(maxInclusive, *max)};
}

bool isIntervalSubsetOfType(const IntervalRequirement& interval, value::TypeTags type) {
    // Create a conjunction of the interval and the min-max interval for the type as input for the
    // intersection function.
    auto intervals =
        IntervalReqExpr::make<IntervalReqExpr::Disjunction>(IntervalReqExpr::NodeVector{
            IntervalReqExpr::make<IntervalReqExpr::Conjunction>(IntervalReqExpr::NodeVector{
                IntervalReqExpr::make<IntervalReqExpr::Atom>(interval),
                IntervalReqExpr::make<IntervalReqExpr::Atom>(getMinMaxIntervalForType(type))})});

    return intersectDNFIntervals(intervals, ConstEval::constFold).has_value();
}

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

    // If the value is minimal for its type, and the operation is $lt or $lte return cardinality up
    // to the previous bucket.
    auto&& [minConstant, inclusive] = getMinMaxBoundForType(true /*isMin*/, tag);
    auto [minTag, minVal] = getConstTypeVal(*minConstant);
    if (compareValues(minTag, minVal, tag, val) == 0) {
        return {resultCard, resultNDV};
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

    const double bucketFreqRatio = bucket._rangeFreq * ratio;
    resultCard += bucketFreqRatio;
    resultNDV += bucket._ndv * ratio;

    if (type == EstimationType::kLess) {
        // Subtract from the estimate the cardinality and ndv corresponding to the equality
        // operation, if they are larger than the ratio taken from this bucket.
        const double innerEqFreqCorrection = (bucketFreqRatio < innerEqFreq) ? 0.0 : innerEqFreq;
        const double innerEqNdv = (bucket._ndv * ratio <= 1.0) ? 0.0 : 1.0;
        resultCard -= innerEqFreqCorrection;
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

/**
 * Returns how many values of the given type are known by the array histogram.
 */
double getTypeCard(const ArrayHistogram& ah,
                   value::TypeTags tag,
                   value::Value val,
                   bool includeScalar) {
    double count = 0.0;

    if (includeScalar) {
        // Include scalar type count estimate.
        switch (tag) {
            case value::TypeTags::Boolean: {
                // In the case of booleans, we have separate true/false counters we can use.
                const bool estTrue = value::bitcastTo<bool>(val);
                if (estTrue) {
                    count += ah.getTrueCount();
                } else {
                    count += ah.getFalseCount();
                }
                break;
            }
            case value::TypeTags::Array: {
                // Note that if we are asked by the optimizer to estimate an interval whose bounds
                // are arrays, this means we are trying to estimate equality on nested arrays. In
                // this case, we do not want to include the "scalar" type counter for the array
                // type, because this will cause us to estimate the nested array case as counting
                // all arrays, regardless of whether or not they are nested.
                break;
            }
            // TODO SERVER-71377: Use both missing & null counters for null equality.
            // case value::TypeTags::Null: {}
            default: { count += ah.getTypeCount(tag); }
        }
    }

    if (ah.isArray()) {
        // Include array type count estimate.
        count += ah.getArrayTypeCount(tag);
    }

    return count;
}

/**
 * Estimates equality to the given tag/value using histograms.
 */
CEType estimateCardEq(const ArrayHistogram& ah,
                      value::TypeTags tag,
                      value::Value val,
                      bool includeScalar) {
    double card = 0.0;
    if (includeScalar) {
        card = estimate(ah.getScalar(), tag, val, EstimationType::kEqual).card;
    }
    if (ah.isArray()) {
        card += estimate(ah.getArrayUnique(), tag, val, EstimationType::kEqual).card;
    }
    return {card};
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

CEType estimateCardRange(const ArrayHistogram& ah,
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

            // ToDo: try using ah.getArrayCount() - ah.getEmptyArrayCount();
            // when the number of empty arrays is provided by the statistics.
            const double totalArrayCount = ah.getArrayCount();

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

    return {result};
}

CEType estimateIntervalCardinality(const ArrayHistogram& ah,
                                   const IntervalRequirement& interval,
                                   CEType childResult,
                                   bool includeScalar) {
    if (interval.isFullyOpen()) {
        return childResult;
    } else if (interval.isEquality()) {
        auto maybeConstBound = getBound(interval.getLowBound());
        if (!maybeConstBound) {
            return {kInvalidEstimate};
        }

        auto [tag, val] = *maybeConstBound;
        if (stats::canEstimateTypeViaHistogram(tag)) {
            return estimateCardEq(ah, tag, val, includeScalar);
        }

        // Otherwise, we return the cardinality for the type of the intervals.
        return {getTypeCard(ah, tag, val, includeScalar)};
    }

    // Otherwise, we have a range.
    auto lowBound = interval.getLowBound();
    auto maybeConstLowBound = getBound(lowBound);
    if (!maybeConstLowBound) {
        return {kInvalidEstimate};
    }

    auto highBound = interval.getHighBound();
    auto maybeConstHighBound = getBound(highBound);
    if (!maybeConstHighBound) {
        return {kInvalidEstimate};
    }

    auto [lowTag, lowVal] = *maybeConstLowBound;
    auto [highTag, highVal] = *maybeConstHighBound;

    // Check if we estimated this interval using histograms. One of the tags may not be of a type we
    // know how to estimate using histograms; however, it should still be possible to estimate the
    // interval if the other one is of the appropriate type.
    if (stats::canEstimateTypeViaHistogram(lowTag) || stats::canEstimateTypeViaHistogram(highTag)) {
        return estimateCardRange(ah,
                                 lowBound.isInclusive(),
                                 lowTag,
                                 lowVal,
                                 highBound.isInclusive(),
                                 highTag,
                                 highVal,
                                 includeScalar);
    }

    // Otherwise, this interval was not in our histogram. We may be able to estimate this interval
    // via type counts- if so, we just return the total count for the type.

    // If the bound tags are equal, we can estimate this in the same way that we do equalities on
    // non-histogrammable types. Otherwise, we need to figure out which type(s) are included by this
    // range.
    if (lowTag == highTag || isIntervalSubsetOfType(interval, lowTag)) {
        return {getTypeCard(ah, lowTag, lowVal, includeScalar)};
    } else if (isIntervalSubsetOfType(interval, highTag)) {
        return {getTypeCard(ah, highTag, highVal, includeScalar)};
    }

    // If we reach here, we've given up estimating, because our interval intersected both high & low
    // type intervals (and possibly more types).
    // TODO: could we aggregate type counts across all intersected types here?
    return {0.0};
}

}  // namespace mongo::optimizer::ce
