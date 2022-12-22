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

#include "mongo/db/query/ce/histogram_estimator.h"
#include "mongo/db/query/ce/histogram_predicate_estimation.h"
#include "mongo/db/query/ce/test_utils.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/db/query/stats/collection_statistics_mock.h"
#include "mongo/db/query/stats/max_diff.h"
#include "mongo/unittest/unittest.h"

namespace mongo::optimizer::ce {
namespace {
namespace value = sbe::value;

using stats::ArrayHistogram;
using stats::Bucket;
using stats::CollectionStatistics;
using stats::CollectionStatisticsMock;
using stats::ScalarHistogram;
using stats::TypeCounts;

std::string collName("test");

class CEHistogramTester : public CETester {
public:
    CEHistogramTester(std::string collName, CEType collCard)
        : CETester(collName, collCard), _stats{new CollectionStatisticsMock(collCard._value)} {}

    void addHistogram(const std::string& path, std::shared_ptr<const ArrayHistogram> histogram) {
        _stats->addHistogram(path, histogram);
    }

protected:
    std::unique_ptr<cascades::CardinalityEstimator> getEstimator(
        bool /*forValidation*/) const override {
        // making a copy of CollecitonStatistics to override
        return std::make_unique<HistogramEstimator>(_stats, makeHeuristicCE());
    }

private:
    std::shared_ptr<CollectionStatistics> _stats;
};

struct TestBucket {
    Value val;
    double equalFreq;
    double rangeFreq = 0.0;
    double ndv = 1.0; /* ndv including bucket boundary*/
};
using TestBuckets = std::vector<TestBucket>;

ScalarHistogram getHistogramFromData(TestBuckets testBuckets) {
    sbe::value::Array bounds;
    std::vector<Bucket> buckets;

    double cumulativeFreq = 0.0;
    double cumulativeNDV = 0.0;
    for (const auto& b : testBuckets) {
        // Add bucket boundary value to bounds.
        auto [tag, val] = stage_builder::makeValue(b.val);
        bounds.push_back(tag, val);

        cumulativeFreq += b.equalFreq + b.rangeFreq;
        cumulativeNDV += b.ndv;

        // Create a histogram bucket.
        buckets.emplace_back(b.equalFreq,
                             b.rangeFreq,
                             cumulativeFreq,
                             b.ndv - 1, /* ndv excluding bucket boundary*/
                             cumulativeNDV);
    }

    return ScalarHistogram::make(std::move(bounds), std::move(buckets));
}

TypeCounts getTypeCountsFromData(TestBuckets testBuckets) {
    TypeCounts typeCounts;
    for (const auto& b : testBuckets) {
        // Add bucket boundary value to bounds.
        auto sbeVal = stage_builder::makeValue(b.val);
        auto [tag, val] = sbeVal;

        // Increment count of values for each type tag.
        if (auto it = typeCounts.find(tag); it != typeCounts.end()) {
            it->second += b.equalFreq + b.rangeFreq;
        } else {
            typeCounts[tag] = b.equalFreq + b.rangeFreq;
        }
    }
    return typeCounts;
}

std::shared_ptr<const ArrayHistogram> getArrayHistogramFromData(
    TestBuckets testBuckets,
    TypeCounts additionalScalarData = {},
    double trueCount = 0,
    double falseCount = 0) {
    TypeCounts dataTypeCounts = getTypeCountsFromData(testBuckets);
    dataTypeCounts.merge(additionalScalarData);
    return ArrayHistogram::make(
        getHistogramFromData(testBuckets), std::move(dataTypeCounts), trueCount, falseCount);
}

std::shared_ptr<const ArrayHistogram> getArrayHistogramFromData(
    TestBuckets scalarBuckets,
    TestBuckets arrayUniqueBuckets,
    TestBuckets arrayMinBuckets,
    TestBuckets arrayMaxBuckets,
    TypeCounts arrayTypeCounts,
    double totalArrayCount,
    double emptyArrayCount = 0,
    TypeCounts additionalScalarData = {},
    double trueCount = 0,
    double falseCount = 0) {

    // Set up scalar type counts.
    TypeCounts dataTypeCounts = getTypeCountsFromData(scalarBuckets);
    dataTypeCounts[value::TypeTags::Array] = totalArrayCount;
    dataTypeCounts.merge(additionalScalarData);

    // Set up histograms.
    auto arrayMinHist = getHistogramFromData(arrayMinBuckets);
    auto arrayMaxHist = getHistogramFromData(arrayMaxBuckets);
    return ArrayHistogram::make(getHistogramFromData(scalarBuckets),
                                std::move(dataTypeCounts),
                                getHistogramFromData(arrayUniqueBuckets),
                                std::move(arrayMinHist),
                                std::move(arrayMaxHist),
                                std::move(arrayTypeCounts),
                                emptyArrayCount,
                                trueCount,
                                falseCount);
}

void addHistogramFromValues(CEHistogramTester& t,
                            const std::string& fieldName,
                            const std::vector<stats::SBEValue>& values,
                            double numBuckets) {
    auto ah = stats::createArrayEstimator(values, numBuckets);
    t.addHistogram(fieldName, ah);
    t.setIndexes(
        {{"index_" + fieldName,
          makeIndexDefinition(FieldNameType{fieldName}, CollationOp::Ascending, ah->isArray())}});

    if (kCETestLogOnly) {
        std::cout << ah->serialize() << std::endl;
    }
}

TEST(CEHistogramTest, AssertSmallMaxDiffHistogramEstimatesAtomicPredicates) {
    constexpr CEType kCollCard{8.0};
    CEHistogramTester t(collName, kCollCard);

    // Construct a histogram with two buckets: one for 3 ints equal to 1, another for 5 strings
    // equal to "ing".
    const std::string& str = "ing";
    t.addHistogram("a",
                   getArrayHistogramFromData({
                       {Value(1), 3 /* frequency */},
                       {Value(str), 5 /* frequency */},
                   }));

    // Test $eq.
    ASSERT_MATCH_CE(t, "{a: {$eq: 1}}", 3.0);
    ASSERT_MATCH_CE(t, "{a: {$eq: 2}}", 0.0);
    ASSERT_MATCH_CE(t, "{a: {$eq: \"ing\"}}", 5.0);
    ASSERT_MATCH_CE(t, "{a: {$eq: \"foo\"}}", 0.0);

    // Test case when field doesn't match fieldpath of histogram. This falls back to heuristics.
    ASSERT_MATCH_CE(t, "{b: {$eq: 1}}", 2.82843);

    // Test $gt.
    ASSERT_MATCH_CE(t, "{a: {$gt: 3}}", 0.0);
    ASSERT_MATCH_CE(t, "{a: {$gt: 1}}", 0.0);
    ASSERT_MATCH_CE(t, "{a: {$gt: 0}}", 3.0);
    ASSERT_MATCH_CE(t, "{a: {$gt: \"bar\"}}", 5.0);
    ASSERT_MATCH_CE(t, "{a: {$gt: \"ing\"}}", 0.0);
    ASSERT_MATCH_CE(t, "{a: {$gt: \"zap\"}}", 0.0);

    // Test $lt.
    ASSERT_MATCH_CE(t, "{a: {$lt: 3}}", 3.0);
    ASSERT_MATCH_CE(t, "{a: {$lt: 1}}", 0.0);
    ASSERT_MATCH_CE(t, "{a: {$lt: 0}}", 0.0);
    ASSERT_MATCH_CE(t, "{a: {$lt: \"bar\"}}", 0.0);
    ASSERT_MATCH_CE(t, "{a: {$lt: \"ing\"}}", 0.0);
    ASSERT_MATCH_CE(t, "{a: {$lt: \"zap\"}}", 5.0);

    // Test $gte.
    ASSERT_MATCH_CE(t, "{a: {$gte: 3}}", 0.0);
    ASSERT_MATCH_CE(t, "{a: {$gte: 1}}", 3.0);
    ASSERT_MATCH_CE(t, "{a: {$gte: 0}}", 3.0);
    ASSERT_MATCH_CE(t, "{a: {$gte: \"bar\"}}", 5.0);
    ASSERT_MATCH_CE(t, "{a: {$gte: \"ing\"}}", 5.0);
    ASSERT_MATCH_CE(t, "{a: {$gte: \"zap\"}}", 0.0);

    // Test $lte.
    ASSERT_MATCH_CE(t, "{a: {$lte: 3}}", 3.0);
    ASSERT_MATCH_CE(t, "{a: {$lte: 1}}", 3.0);
    ASSERT_MATCH_CE(t, "{a: {$lte: 0}}", 0.0);
    ASSERT_MATCH_CE(t, "{a: {$lte: \"bar\"}}", 0.0);
    ASSERT_MATCH_CE(t, "{a: {$lte: \"ing\"}}", 5.0);
    ASSERT_MATCH_CE(t, "{a: {$lte: \"zap\"}}", 5.0);
}

TEST(CEHistogramTest, AssertSmallHistogramEstimatesComplexPredicates) {
    constexpr CEType kCollCard{9.0};
    CEHistogramTester t(collName, kCollCard);

    // Construct a histogram with three int buckets for field 'a'.
    t.addHistogram("a",
                   getArrayHistogramFromData({
                       {Value(1), 3 /* frequency */},
                       {Value(2), 5 /* frequency */},
                       {Value(3), 1 /* frequency */},
                   }));

    // Construct a histogram with two int buckets for field 'b'.
    t.addHistogram("b",
                   getArrayHistogramFromData({
                       {Value(22), 3 /* frequency */},
                       {Value(33), 6 /* frequency */},
                   }));


    // Test simple conjunctions on one field. Note the first example: the range we expect to see
    // here is (1, 3); however, the structure in the SargableNode gives us a conjunction of two
    // intervals instead: (1, "") ^ (nan, 3) This is then estimated using exponential backoff to
    // give us a less accurate result. The correct cardinality here would be 5.
    ASSERT_MATCH_CE(t, "{a: {$gt: 1}, a: {$lt: 3}}", 5.66);
    ASSERT_MATCH_CE(t, "{a: {$gt: 1}, a: {$lte: 3}}", 6.0);
    ASSERT_MATCH_CE(t, "{a: {$gte: 1}, a: {$lt: 3}}", 8.0);
    ASSERT_MATCH_CE(t, "{a: {$gte: 1}, a: {$lte: 3}}", 9.0);

    // Test ranges which exclude each other.
    ASSERT_MATCH_CE(t, "{a: {$lt: 1}, a: {$gt: 3}}", 0.0);

    // Test overlapping ranges. This is a similar case to {a: {$gt: 1}, a: {$lt: 3}} above: we
    // expect to see the range [2, 2]; instead, we see the range [nan, 2] ^ [2, "").
    ASSERT_MATCH_CE(t, "{a: {$lte: 2}, a: {$gte: 2}}", 5.66);

    // Test conjunctions over multiple fields for which we have histograms. Here we expect a
    // cardinality estimated by exponential backoff.
    ASSERT_MATCH_CE(t, "{a: {$eq: 2}, b: {$eq: 22}}", 2.24);
    ASSERT_MATCH_CE(t, "{a: {$eq: 11}, b: {$eq: 22}}", 0.0);
    ASSERT_MATCH_CE(t, "{a: {$gt: 11}, a: {$lte: 100}, b: {$eq: 22}}", 0.0);
    ASSERT_MATCH_CE(t, "{a: {$lt: 3}, a: {$gte: 1}, b: {$lt: 100}, b: {$gt: 30}}", 5.66);

    // Test conjunctions over multiple fields for which we may not have histograms. This falls back
    // to heuristic estimation.
    ASSERT_MATCH_CE(t, "{a: {$eq: 2}, c: {$eq: 1}}", 1.73205);
    ASSERT_MATCH_CE(t, "{c: {$eq: 2}, d: {$eq: 22}}", 1.73205);
}

TEST(CEHistogramTest, SanityTestEmptyHistogram) {
    constexpr CEType kCollCard{0.0};
    CEHistogramTester t(collName, kCollCard);
    t.addHistogram("empty", ArrayHistogram::make());

    ASSERT_MATCH_CE(t, "{empty: {$eq: 1.0}}", 0.0);
    ASSERT_MATCH_CE(t, "{empty: {$lt: 1.0}, empty: {$gt: 0.0}}", 0.0);
    ASSERT_MATCH_CE(t, "{empty: {$eq: 1.0}, other: {$eq: \"anything\"}}", 0.0);
    ASSERT_MATCH_CE(t, "{other: {$eq: \"anything\"}, empty: {$eq: 1.0}}", 0.0);
}

TEST(CEHistogramTest, TestOneBucketOneIntHistogram) {
    constexpr CEType kCollCard{50.0};
    CEHistogramTester t(collName, kCollCard);

    // Create a histogram with a single bucket that contains exactly one int (42) with a frequency
    // of 50 (equal to the collection cardinality).
    t.addHistogram("soloInt",
                   getArrayHistogramFromData({
                       {Value(42), kCollCard._value /* frequency */},
                   }));

    // Check against a variety of intervals that include 42 as a bound.
    ASSERT_MATCH_CE(t, "{soloInt: {$eq: 42}}", kCollCard);
    ASSERT_MATCH_CE(t, "{soloInt: {$lt: 42}}", 0.0);
    ASSERT_MATCH_CE(t, "{soloInt: {$lte: 42}}", kCollCard);
    ASSERT_MATCH_CE(t, "{soloInt: {$gt: 42}}", 0.0);
    ASSERT_MATCH_CE(t, "{soloInt: {$gte: 42}}", kCollCard);
    ASSERT_MATCH_CE(t, "{soloInt: {$gt: 42}, soloInt: {$lt: 42}}", 0.0);
    ASSERT_MATCH_CE(t, "{soloInt: {$gt: 42}, soloInt: {$lte: 42}}", 0.0);
    ASSERT_MATCH_CE(t, "{soloInt: {$gte: 42}, soloInt: {$lt: 42}}", 0.0);
    ASSERT_MATCH_CE(t, "{soloInt: {$gte: 42}, soloInt: {$lte: 42}}", kCollCard);

    // Check against a variety of intervals that include 42 only as one bound.
    ASSERT_MATCH_CE(t, "{soloInt: {$gt: 42}, soloInt: {$lt: 43}}", 0.0);
    ASSERT_MATCH_CE(t, "{soloInt: {$gt: 42}, soloInt: {$lte: 43}}", 0.0);
    ASSERT_MATCH_CE(t, "{soloInt: {$gte: 42}, soloInt: {$lt: 43}}", kCollCard);
    ASSERT_MATCH_CE(t, "{soloInt: {$gte: 42}, soloInt: {$lte: 43}}", kCollCard);
    ASSERT_MATCH_CE(t, "{soloInt: {$gt: 41}, soloInt: {$lt: 42}}", 0.0);
    ASSERT_MATCH_CE(t, "{soloInt: {$gt: 41}, soloInt: {$lte: 42}}", kCollCard);
    ASSERT_MATCH_CE(t, "{soloInt: {$gte: 41}, soloInt: {$lt: 42}}", 0.0);
    ASSERT_MATCH_CE(t, "{soloInt: {$gte: 41}, soloInt: {$lte: 42}}", kCollCard);

    // Check against a variety of intervals close to 42 using a lower bound of 41 and a higher bound
    // of 43.
    ASSERT_MATCH_CE(t, "{soloInt: {$eq: 41}}", 0.0);
    ASSERT_MATCH_CE(t, "{soloInt: {$eq: 43}}", 0.0);
    ASSERT_MATCH_CE(t, "{soloInt: {$lt: 43}}", kCollCard);
    ASSERT_MATCH_CE(t, "{soloInt: {$lte: 43}}", kCollCard);
    ASSERT_MATCH_CE(t, "{soloInt: {$gt: 41}}", kCollCard);
    ASSERT_MATCH_CE(t, "{soloInt: {$gte: 41}}", kCollCard);
    ASSERT_MATCH_CE(t, "{soloInt: {$gt: 41}, soloInt: {$lt: 43}}", kCollCard);
    ASSERT_MATCH_CE(t, "{soloInt: {$gte: 41}, soloInt: {$lt: 43}}", kCollCard);
    ASSERT_MATCH_CE(t, "{soloInt: {$gt: 41}, soloInt: {$lte: 43}}", kCollCard);
    ASSERT_MATCH_CE(t, "{soloInt: {$gte: 41}, soloInt: {$lte: 43}}", kCollCard);

    // Check against different types.
    ASSERT_MATCH_CE(t, "{soloInt: {$eq: \"42\"}}", 0.0);
    ASSERT_MATCH_CE(t, "{soloInt: {$lt: \"42\"}}", 0.0);
    ASSERT_MATCH_CE(t, "{soloInt: {$lt: 42.1}}", kCollCard);
}

TEST(CEHistogramTest, TestOneBoundIntRangeHistogram) {
    constexpr CEType kCollCard{51.0};
    CEHistogramTester t(collName, kCollCard);
    t.addHistogram("intRange",
                   getArrayHistogramFromData({
                       {Value(10), 5 /* frequency */},
                       {Value(20), 1 /* frequency */, 45 /* range frequency */, 10 /* ndv */},
                   }));

    // Test ranges that overlap only with the lower bound.
    // Note: 5 values equal 10.
    ASSERT_MATCH_CE(t, "{intRange: {$eq: 10}}", 5.0);
    ASSERT_MATCH_CE(t, "{intRange: {$lte: 10}}", 5.0);
    ASSERT_MATCH_CE(t, "{intRange: {$lte: 10}, intRange: {$gte: 10}}", 5.0);

    // Test ranges that overlap only with the upper bound.
    ASSERT_MATCH_CE(t, "{intRange: {$eq: 11}}", 5.0);
    ASSERT_MATCH_CE(t, "{intRange: {$eq: 15}}", 5.0);
    ASSERT_MATCH_CE(t, "{intRange: {$eq: 15.5}}", 5.0);
    ASSERT_MATCH_CE(t, "{intRange: {$eq: 20}}", 1.0);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 20}}", 1.0);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 10}}", 46.0);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 15}}", 28.5);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 15}}", 23.5);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 11}, intRange: {$lte: 20}}", 41.5);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 11}, intRange: {$lte: 20}}", 41.5);

    // Test ranges that partially overlap with the entire histogram.
    ASSERT_MATCH_CE(t, "{intRange: {$lt: 11}}", 9.5);
    ASSERT_MATCH_CE(t, "{intRange: {$lt: 15}}", 22.5);
    ASSERT_MATCH_CE(t, "{intRange: {$lte: 15}}", 27.5);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 8}, intRange: {$lte: 15}}", 27.5);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 8}, intRange: {$lte: 15}}", 27.5);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 8}, intRange: {$lt: 15}}", 22.5);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 8}, intRange: {$lte: 15}}", 27.5);

    // Test ranges that include all values in the histogram.
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 10}, intRange: {$lte: 20}}", kCollCard);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 1}, intRange: {$lte: 30}}", kCollCard);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 1}, intRange: {$lt: 30}}", kCollCard);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 1}, intRange: {$lte: 30}}", kCollCard);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 1}, intRange: {$lt: 30}}", kCollCard);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 0}}", kCollCard);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 0}}", kCollCard);
    ASSERT_MATCH_CE(t, "{intRange: {$lt: 100}}", kCollCard);
    ASSERT_MATCH_CE(t, "{intRange: {$lte: 100}}", kCollCard);

    // Test ranges that are fully included in the histogram.
    ASSERT_MATCH_CE(t, "{intRange: {$eq: 10.5}}", 5.0);
    ASSERT_MATCH_CE(t, "{intRange: {$eq: 12.5}}", 5.0);
    ASSERT_MATCH_CE(t, "{intRange: {$eq: 19.36}}", 5.0);

    // Test ranges that don't overlap with the histogram.
    ASSERT_MATCH_CE(t, "{intRange: {$lt: 10}}", 0.0);
    ASSERT_MATCH_CE(t, "{intRange: {$lt: 5}}", 0.0);
    ASSERT_MATCH_CE(t, "{intRange: {$lte: 5}}", 0.0);
    ASSERT_MATCH_CE(t, "{intRange: {$eq: 20.1}}", 0.0);
    ASSERT_MATCH_CE(t, "{intRange: {$eq: 21}}", 0.0);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 21}}", 0.0);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 20}}", 0.0);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 100}}", 0.0);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 30}, intRange: {$lte: 50}}", 0.0);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 30}, intRange: {$lt: 50}}", 0.0);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 30}, intRange: {$lt: 50}}", 0.0);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 30}, intRange: {$lte: 50}}", 0.0);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 0}, intRange: {$lte: 5}}", 0.0);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 0}, intRange: {$lt: 5}}", 0.0);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 0}, intRange: {$lt: 5}}", 0.0);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 0}, intRange: {$lte: 5}}", 0.0);

    // Because we don't specify any indexes here, these intervals do not go through simplification.
    // This means that instead of having one key in the requirements map of the generated sargable
    // node corresponding to the path "intRange", we have two keys and two ranges, both
    // corresponding to the same path. As a consequence, we combine the estimates for the intervals
    // using exponential backoff, which results in an overestimate.
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 11}, intRange: {$lt: 20}}", 41.09);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 11}, intRange: {$lt: 20}}", 41.09);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 12}, intRange: {$lt: 15}}", 19.16);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 12}, intRange: {$lt: 15}}", 20.42);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 12}, intRange: {$lte: 15}}", 23.42);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 12}, intRange: {$lte: 15}}", 24.96);
    ASSERT_MATCH_CE(t, "{intRange: {$lt: 19}, intRange: {$gt: 11}}", 36.53);

    // When we specify that there is a non-multikey index on 'intRange', we expect to see interval
    // simplification occurring, which should provide a better estimate for the following ranges.
    t.setIndexes(
        {{"intRangeIndex",
          makeIndexDefinition("intRange", CollationOp::Ascending, /* isMultiKey */ false)}});
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 11}, intRange: {$lt: 20}}", 40.5);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 11}, intRange: {$lt: 20}}", 40.5);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 12}, intRange: {$lt: 15}}", 8.5);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 12}, intRange: {$lt: 15}}", 13.5);
    ASSERT_MATCH_CE(t, "{intRange: {$gt: 12}, intRange: {$lte: 15}}", 13.5);
    ASSERT_MATCH_CE(t, "{intRange: {$gte: 12}, intRange: {$lte: 15}}", 18.5);
    ASSERT_MATCH_CE(t, "{intRange: {$lt: 19}, intRange: {$gt: 11}}", 31.0);
}

TEST(CEHistogramTest, TestHistogramOnNestedPaths) {
    constexpr CEType kCollCard{50.0};
    CEHistogramTester t(collName, kCollCard);

    // Create a histogram with a single bucket that contains exactly one int (42) with a frequency
    // of 50 (equal to the collection cardinality).
    t.addHistogram("path",
                   getArrayHistogramFromData({
                       {Value(42), kCollCard._value /* frequency */},
                   }));
    t.addHistogram("a.histogram.path",
                   getArrayHistogramFromData({
                       {Value(42), kCollCard._value /* frequency */},
                   }));

    ASSERT_MATCH_CE(t, "{\"not.a.histogram.path\": {$eq: 42}}", 7.071 /* heuristic */);
    ASSERT_MATCH_CE(t, "{\"a.histogram.path\": {$eq: 42}}", kCollCard);
    ASSERT_MATCH_CE(
        t, "{\"a.histogram.path.with.no.histogram\": {$eq: 42}}", 7.071 /* heuristic */);

    // When a predicate can't be precisely translated to a SargableNode (such as $elemMatch on a
    // dotted path), we may still be able to translate an over-approximation. We generate a
    // SargableNode with all predicates marked perfOnly, and keep the original Filter. The Filter
    // ensures the results are correct, while the SargableNode hopefully will be answerable by an
    // index.
    //
    // On the logical level, perfOnly predicates don't do anything, so we don't consider them in
    // cardinality estimates. But when we split a SargableNode into an indexed part and a fetch
    // part, we remove the perfOnly flag from the indexed part, and we should consider them to
    // estimate how many index keys are returned.
    //
    // In this test, we want to exercise the histogram estimate for the SargableNode generated by
    // $elemMatch on a dotted path. So we create an index on this field to ensure the SargableNode
    // is split, and the predicates marked non-perfOnly.
    //
    // We also mark the index multikey, to prevent non-CE rewrites from removing the predicate
    // entirely. (This scenario could happen if you remove all the arrays, and refresh the
    // statistics.)
    IndexDefinition ix{
        IndexCollationSpec{
            IndexCollationEntry{
                makeIndexPath({"a", "histogram", "path"}),
                CollationOp::Ascending,
            },
        },
        true /* isMultiKey */,
    };
    t.setIndexes({{"a_histogram_path_1", std::move(ix)}});
    ASSERT_MATCH_CE_NODE(t, "{\"a.histogram.path\": {$elemMatch: {$eq: 42}}}", 0.0, isSargable2);
}

TEST(CEHistogramTest, TestArrayHistogramOnAtomicPredicates) {
    constexpr CEType kCollCard{6.0};
    CEHistogramTester t(collName, kCollCard);
    t.addHistogram(
        "a",
        // Generate a histogram for this data:
        // {a: 1}, {a: 2}, {a: [1, 2, 3, 2, 2]}, {a: [10]}, {a: [2, 3, 3, 4, 5, 5, 6]}, {a: []}
        //  - scalars: [1, 2]
        //  - unique values: [1, 2, 3], [10], [2, 3, 4, 5, 6]
        //      -> [1, 2, 2, 3, 3, 4, 5, 6, 10]
        //  - min values: [1], [10], [2] -> [1, 1, 2, 2, 10]
        //  - max values: [3], [10], [6] -> [1, 2, 3, 6, 10]
        getArrayHistogramFromData(
            {// Scalar buckets.
             {Value(1), 1 /* frequency */},
             {Value(2), 1 /* frequency */}},
            {
                // Array unique buckets.
                {Value(1), 1 /* frequency */},
                {Value(2), 2 /* frequency */},
                {Value(3), 2 /* frequency */},
                {Value(4), 1 /* frequency */},
                {Value(5), 1 /* frequency */},
                {Value(6), 1 /* frequency */},
                {Value(10), 1 /* frequency */},
            },
            {
                // Array min buckets.
                {Value(1), 1 /* frequency */},
                {Value(2), 1 /* frequency */},
                {Value(10), 1 /* frequency */},
            },
            {
                // Array max buckets.
                {Value(3), 1 /* frequency */},
                {Value(6), 1 /* frequency */},
                {Value(10), 1 /* frequency */},
            },
            {{sbe::value::TypeTags::NumberInt32, 3}},  // Array type counts (3 arrays with ints).
            4,                                         // 4 arrays (including []).
            1                                          // 1 empty array.
            ));

    // Test simple predicates against 'a'. Note: in the $elemMatch case, we exclude scalar
    // estimates. Without $elemMatch, we add the array histogram and scalar histogram estimates
    // together.

    // Test equality predicates.
    ASSERT_EQ_ELEMMATCH_CE(t, 0.0 /* CE */, 0.0 /* $elemMatch CE */, "a", "{$eq: 0}");
    ASSERT_EQ_ELEMMATCH_CE(t, 2.0 /* CE */, 1.0 /* $elemMatch CE */, "a", "{$eq: 1}");
    ASSERT_EQ_ELEMMATCH_CE(t, 3.0 /* CE */, 2.0 /* $elemMatch CE */, "a", "{$eq: 2}");
    ASSERT_EQ_ELEMMATCH_CE(t, 2.0 /* CE */, 2.0 /* $elemMatch CE */, "a", "{$eq: 3}");
    ASSERT_EQ_ELEMMATCH_CE(t, 1.0 /* CE */, 1.0 /* $elemMatch CE */, "a", "{$eq: 4}");
    ASSERT_EQ_ELEMMATCH_CE(t, 1.0 /* CE */, 1.0 /* $elemMatch CE */, "a", "{$eq: 5}");
    ASSERT_EQ_ELEMMATCH_CE(t, 1.0 /* CE */, 1.0 /* $elemMatch CE */, "a", "{$eq: 6}");
    ASSERT_EQ_ELEMMATCH_CE(t, 1.0 /* CE */, 1.0 /* $elemMatch CE */, "a", "{$eq: 10}");
    ASSERT_EQ_ELEMMATCH_CE(t, 0.0 /* CE */, 0.0 /* $elemMatch CE */, "a", "{$eq: 11}");

    // Test histogram boundary values.
    ASSERT_EQ_ELEMMATCH_CE(t, 0.0 /* CE */, 0.0 /* $elemMatch CE */, "a", "{$lt: 1}");
    ASSERT_EQ_ELEMMATCH_CE(t, 2.0 /* CE */, 1.0 /* $elemMatch CE */, "a", "{$lte: 1}");
    ASSERT_EQ_ELEMMATCH_CE(t, 0.0 /* CE */, 0.0 /* $elemMatch CE */, "a", "{$gt: 10}");
    ASSERT_EQ_ELEMMATCH_CE(t, 1.0 /* CE */, 1.0 /* $elemMatch CE */, "a", "{$gte: 10}");

    ASSERT_EQ_ELEMMATCH_CE(t, 5.0 /* CE */, 4.0 /* $elemMatch CE */, "a", "{$lte: 10}");
    ASSERT_EQ_ELEMMATCH_CE(t, 4.0 /* CE */, 4.0 /* $elemMatch CE */, "a", "{$lt: 10}");
    ASSERT_EQ_ELEMMATCH_CE(t, 4.0 /* CE */, 4.0 /* $elemMatch CE */, "a", "{$gt: 1}");
    ASSERT_EQ_ELEMMATCH_CE(t, 5.0 /* CE */, 4.0 /* $elemMatch CE */, "a", "{$gte: 1}");

    ASSERT_EQ_ELEMMATCH_CE(t, 4.0 /* CE */, 4.0 /* $elemMatch CE */, "a", "{$lte: 5}");
    ASSERT_EQ_ELEMMATCH_CE(t, 4.0 /* CE */, 4.0 /* $elemMatch CE */, "a", "{$lt: 5}");
    ASSERT_EQ_ELEMMATCH_CE(t, 2.0 /* CE */, 2.0 /* $elemMatch CE */, "a", "{$gt: 5}");
    ASSERT_EQ_ELEMMATCH_CE(t, 2.0 /* CE */, 2.55085 /* $elemMatch CE */, "a", "{$gte: 5}");

    ASSERT_EQ_ELEMMATCH_CE(t, 2.45 /* CE */, 2.55085 /* $elemMatch CE */, "a", "{$gt: 2, $lt: 5}");
    ASSERT_EQ_ELEMMATCH_CE(t, 3.27 /* CE */, 4.0 /* $elemMatch CE */, "a", "{$gte: 2, $lt: 5}");
    ASSERT_EQ_ELEMMATCH_CE(t, 2.45 /* CE */, 3.40113 /* $elemMatch CE */, "a", "{$gt: 2, $lte: 5}");
    ASSERT_EQ_ELEMMATCH_CE(t, 3.27 /* CE */, 4.0 /* $elemMatch CE */, "a", "{$gte: 2, $lte: 5}");
}

TEST(CEHistogramTest, TestArrayHistogramOnCompositePredicates) {
    constexpr CEType kCollCard{175.0};
    CEHistogramTester t(collName, kCollCard);

    // A scalar histogram with values in the range [1,10], most of which are in the middle bucket.
    t.addHistogram("scalar",
                   getArrayHistogramFromData({
                       {Value(1), 10 /* frequency */},
                       {Value(2), 10 /* frequency */},
                       {Value(3), 20 /* frequency */, 120 /* range frequency */, 5 /* ndv */},
                       {Value(8), 5 /* frequency */, 10 /* range frequency */, 3 /* ndv */},
                   }));

    // An array histogram built on the following arrays with 35 occurrences of each:
    // [{[1, 2, 3]: 35}, {[5, 5, 5, 5, 5]: 35}, {[6]: 35}, {[]: 35}, {[8, 9, 10]: 35}]
    t.addHistogram(
        "array",
        getArrayHistogramFromData(
            {/* No scalar buckets. */},
            {
                // Array unique buckets.
                {Value(2), 35 /* frequency */, 35 /* range frequency */, 2 /* ndv */},
                {Value(5), 35 /* frequency */, 35 /* range frequency */, 2 /* ndv */},
                {Value(6), 35 /* frequency */},
                {Value(10), 35 /* frequency */, 105 /* range frequency */, 3 /* ndv */},
            },
            {
                // Array min buckets.
                {Value(1), 35 /* frequency */},
                {Value(5), 35 /* frequency */},
                {Value(6), 35 /* frequency */},
                {Value(8), 35 /* frequency */},
            },
            {
                // Array max buckets.
                {Value(3), 35 /* frequency */},
                {Value(5), 35 /* frequency */},
                {Value(6), 35 /* frequency */},
                {Value(10), 35 /* frequency */},
            },
            {{sbe::value::TypeTags::NumberInt32, 140}},  // Arrays with ints = 4*35 = 140.
            kCollCard._value,                            // kCollCard arrays total.
            35                                           // 35 empty arrays
            ));

    t.addHistogram(
        "mixed",
        // The mixed histogram has 87 scalars that follow approximately the same distribution as
        // in the pure scalar case, and 88 arrays with the following distribution:
        //  [{[1, 2, 3]: 17}, {[5, 5, 5, 5, 5]: 17}, {[6]: 17}, {[]: 20}, {[8, 9, 10]: 17}]
        getArrayHistogramFromData(
            {
                // Scalar buckets. These are half the number of values from the "scalar" histogram.
                {Value(1), 5 /* frequency */},
                {Value(2), 5 /* frequency */},
                {Value(3), 10 /* frequency */, 60 /* range frequency */, 5 /* ndv */},
                {Value(8), 2 /* frequency */, 5 /* range frequency */, 3 /* ndv */},
            },
            {
                // Array unique buckets.
                {Value(2), 17 /* frequency */, 17 /* range frequency */, 2 /* ndv */},
                {Value(5), 17 /* frequency */, 17 /* range frequency */, 2 /* ndv */},
                {Value(6), 17 /* frequency */},
                {Value(10), 17 /* frequency */, 34 /* range frequency */, 3 /* ndv */},
            },
            {
                // Array min buckets.
                {Value(1), 17 /* frequency */},
                {Value(5), 17 /* frequency */},
                {Value(6), 17 /* frequency */},
                {Value(8), 17 /* frequency */},
            },
            {
                // Array max buckets.
                {Value(3), 17 /* frequency */},
                {Value(5), 17 /* frequency */},
                {Value(6), 17 /* frequency */},
                {Value(10), 17 /* frequency */},
            },
            {{sbe::value::TypeTags::NumberInt32, 68}},  // Arrays with ints = 17*4 = 68.
            88,                                         // kCollCard arrays total.
            20                                          // 20 empty arrays.
            ));

    // Test cardinality of individual predicates.
    ASSERT_EQ_ELEMMATCH_CE(t, 5.0 /* CE */, 0.0 /* $elemMatch CE */, "scalar", "{$eq: 5}");
    ASSERT_EQ_ELEMMATCH_CE(t, 35.0 /* CE */, 35.0 /* $elemMatch CE */, "array", "{$eq: 5}");
    ASSERT_EQ_ELEMMATCH_CE(t, 19.5 /* CE */, 17.0 /* $elemMatch CE */, "mixed", "{$eq: 5}");

    // Test cardinality of predicate combinations; the following tests make sure we correctly track
    // which paths have $elemMatches and which don't. Some notes:
    //  - Whenever we use 'scalar' + $elemMatch, we expect an estimate of 0 because $elemMatch never
    // returns documents on non-array paths.
    //  - Whenever we use 'mixed' + $elemMatch, we expect the estimate to decrease because we omit
    // scalar values in 'mixed' from our estimate.
    //  - We do not expect the estimate on 'array' to be affected by the presence of $elemMatch,
    // since we only have array values for this field.

    // Composite predicate on 'scalar' and 'array' fields.
    ASSERT_MATCH_CE(t, "{scalar: {$eq: 5}, array: {$eq: 5}}", 2.236);
    ASSERT_MATCH_CE(t, "{scalar: {$eq: 5}, array: {$elemMatch: {$eq: 5}}}", 2.236);
    ASSERT_MATCH_CE(t, "{scalar: {$elemMatch: {$eq: 5}}, array: {$eq: 5}}", 0.0);

    // Composite predicate on 'mixed' and 'array' fields.
    ASSERT_MATCH_CE(t, "{mixed: {$eq: 5}, array: {$eq: 5}}", 8.721);
    ASSERT_MATCH_CE(t, "{mixed: {$eq: 5}, array: {$elemMatch: {$eq: 5}}}", 8.721);
    ASSERT_MATCH_CE(t, "{mixed: {$elemMatch: {$eq: 5}}, array: {$eq: 5}}", 7.603);

    // Composite predicate on 'scalar' and 'mixed' fields.
    ASSERT_MATCH_CE(t, "{scalar: {$eq: 5}, mixed: {$eq: 5}}", 1.669);
    ASSERT_MATCH_CE(t, "{scalar: {$eq: 5}, mixed: {$elemMatch: {$eq: 5}}}", 1.559);
    ASSERT_MATCH_CE(t, "{scalar: {$elemMatch: {$eq: 5}}, mixed: {$eq: 5}}", 0.0);

    // Composite predicate on all three fields without '$elemMatch' on 'array'.
    ASSERT_MATCH_CE(t, "{scalar: {$eq: 5}, mixed: {$eq: 5}, array: {$eq: 5}}", 1.116);
    ASSERT_MATCH_CE(t, "{scalar: {$eq: 5}, mixed: {$elemMatch: {$eq: 5}}, array: {$eq: 5}}", 1.042);
    ASSERT_MATCH_CE(t, "{scalar: {$elemMatch: {$eq: 5}}, mixed: {$eq: 5}, array: {$eq: 5}}", 0.0);

    // Composite predicate on all three fields with '$elemMatch' on 'array' (same expected results
    // as above).
    ASSERT_MATCH_CE(t, "{scalar: {$eq: 5}, mixed: {$eq: 5}, array: {$elemMatch: {$eq: 5}}}", 1.116);

    // Test case where the same path has both $match and $elemMatch (same as $elemMatch case).
    ASSERT_MATCH_CE(t, "{scalar: {$elemMatch: {$eq: 5}}, scalar: {$eq: 5}}", 0.0);
    ASSERT_MATCH_CE(t, "{mixed: {$elemMatch: {$eq: 5}}, mixed: {$eq: 5}}", 17.0);
    ASSERT_MATCH_CE(t, "{array: {$elemMatch: {$eq: 5}}, array: {$eq: 5}}", 35.0);

    // Test case with multiple predicates and ranges.
    ASSERT_MATCH_CE(t, "{array: {$elemMatch: {$lt: 5}}, mixed: {$lt: 5}}", 70.2156);
    ASSERT_MATCH_CE(t, "{array: {$elemMatch: {$lt: 5}}, mixed: {$gt: 5}}", 28.4848);

    // Test multiple $elemMatches.
    ASSERT_MATCH_CE(t, "{scalar: {$elemMatch: {$eq: 5}}, array: {$elemMatch: {$eq: 5}}}", 0.0);
    ASSERT_MATCH_CE(t, "{mixed: {$elemMatch: {$eq: 5}}, array: {$elemMatch: {$eq: 5}}}", 7.603);
    ASSERT_MATCH_CE(t, "{scalar: {$elemMatch: {$eq: 5}}, mixed: {$elemMatch: {$eq: 5}}}", 0.0);
    ASSERT_MATCH_CE(
        t, "{scalar: {$elemMatch: {$eq: 5}}, mixed: {$elemMatch: {$eq: 5}}, array: {$eq: 5}}", 0.0);
    ASSERT_MATCH_CE(
        t,
        "{scalar: {$eq: 5}, mixed: {$elemMatch: {$eq: 5}}, array: {$elemMatch: {$eq: 5}}}",
        1.042);
    ASSERT_MATCH_CE(
        t, "{scalar: {$elemMatch: {$eq: 5}}, mixed: {$eq: 5}, array: {$elemMatch: {$eq: 5}}}", 0.0);
    ASSERT_MATCH_CE(t,
                    "{scalar: {$elemMatch: {$eq: 5}}, mixed: {$elemMatch: {$eq: 5}}, array: "
                    "{$elemMatch: {$eq: 5}}}",
                    0.0);
    ASSERT_MATCH_CE(t, "{array: {$elemMatch: {$lt: 5}}, mixed: {$elemMatch: {$lt: 5}}}", 34.1434);
    ASSERT_MATCH_CE(t, "{array: {$elemMatch: {$lt: 5}}, mixed: {$elemMatch: {$gt: 5}}}", 45.5246);

    // Verify that we still return an estimate of 0.0 for any $elemMatch predicate on a scalar
    // field when we have a non-multikey index.
    t.setIndexes({{"aScalarIndex",
                   makeIndexDefinition("scalar", CollationOp::Ascending, /* isMultiKey */ false)}});
    ASSERT_MATCH_CE(t, "{scalar: {$elemMatch: {$eq: 5}}}", 0.0);
    ASSERT_MATCH_CE(t, "{scalar: {$elemMatch: {$gt: 1, $lt: 10}}}", 0.0);

    // Test how we estimate singular PathArr sargable predicate.
    ASSERT_MATCH_CE_NODE(t, "{array: {$elemMatch: {}}}", 175.0, isSargable);
    ASSERT_MATCH_CE_NODE(t, "{mixed: {$elemMatch: {}}}", 88.0, isSargable);

    // Take into account both empty and non-empty arrays.
    auto makePathArrABT = [&](const FieldNameType& fieldName) {
        const ProjectionName scanProjection{"scan_0"};
        auto scanNode = make<ScanNode>(scanProjection, collName);
        auto filterNode =
            make<FilterNode>(make<EvalFilter>(make<PathGet>(std::move(fieldName), make<PathArr>()),
                                              make<Variable>(scanProjection)),
                             std::move(scanNode));
        return make<RootNode>(
            properties::ProjectionRequirement{ProjectionNameVector{scanProjection}},
            std::move(filterNode));
    };

    // There are no arrays in the 'scalar' field.
    ABT scalarABT = makePathArrABT("scalar");
    ASSERT_CE(t, scalarABT, 0.0);

    // About half the values of this field are arrays.
    ABT mixedABT = makePathArrABT("mixed");
    ASSERT_CE(t, mixedABT, 88.0);

    // This field is always an array.
    ABT arrayABT = makePathArrABT("array");
    ASSERT_CE(t, arrayABT, kCollCard);
}

TEST(CEHistogramTest, TestMixedElemMatchAndNonElemMatch) {
    constexpr CEType kCollCard{1.0};
    CEHistogramTester t(collName, kCollCard);

    // A very simple histogram encoding a collection with one document {a: [3, 10]}.
    t.addHistogram("a",
                   getArrayHistogramFromData({/* No scalar buckets. */},
                                             {
                                                 // Array unique buckets.
                                                 {Value(3), 1 /* frequency */},
                                                 {Value(10), 1 /* frequency */},
                                             },
                                             {
                                                 // Array min buckets.
                                                 {Value(3), 1 /* frequency */},
                                             },
                                             {
                                                 // Array max buckets.
                                                 {Value(10), 1 /* frequency */},
                                             },
                                             // We only have one array with ints.
                                             {{sbe::value::TypeTags::NumberInt32, 1}},
                                             1,
                                             0));

    // Tests without indexes.
    ASSERT_MATCH_CE(t, "{a: {$elemMatch: {$gt: 3, $lt: 10}}}", 0.0);
    ASSERT_MATCH_CE(t, "{a: {$gt: 3, $lt: 10}}", 1.0);
    ASSERT_MATCH_CE(t, "{a: {$elemMatch: {$eq: 3}, $gt: 3, $lt: 10}}", 1.0);
    ASSERT_MATCH_CE(t, "{a: {$gt: 3, $lt: 10, $elemMatch: {$eq: 3}}}", 1.0);
    ASSERT_MATCH_CE(t, "{a: {$gt: 3, $lt: 10, $elemMatch: {$gt: 3, $lt: 10}}}", 0.0);
    ASSERT_MATCH_CE(t, "{a: {$elemMatch: {$gt: 3, $lt: 10}, $gt: 3, $lt: 10}}", 0.0);

    // Tests with multikey index (note that the index on "a" must be multikey due to arrays).
    t.setIndexes(
        {{"anIndex", makeIndexDefinition("a", CollationOp::Ascending, /* isMultiKey */ true)}});
    ASSERT_MATCH_CE(t, "{a: {$elemMatch: {$gt: 3, $lt: 10}}}", 0.0);
    ASSERT_MATCH_CE(t, "{a: {$gt: 3, $lt: 10}}", 1.0);
    ASSERT_MATCH_CE(t, "{a: {$elemMatch: {$eq: 3}, $gt: 3, $lt: 10}}", 1.0);
    ASSERT_MATCH_CE(t, "{a: {$gt: 3, $lt: 10, $elemMatch: {$eq: 3}}}", 1.0);
    ASSERT_MATCH_CE(t, "{a: {$gt: 3, $lt: 10, $elemMatch: {$gt: 3, $lt: 10}}}", 0.0);
    ASSERT_MATCH_CE(t, "{a: {$elemMatch: {$gt: 3, $lt: 10}, $gt: 3, $lt: 10}}", 0.0);
}

TEST(CEHistogramTest, TestTypeCounters) {
    constexpr CEType kCollCard{1000.0};
    CEHistogramTester t(collName, kCollCard);

    // This test is designed such that for each document, we have the following fields:
    // 1. scalar: Scalar histogram with no buckets, only type-counted data.
    // 2. array: Array histogram with no buckets, only type-counted data inside of arrays.
    // 3. mixed: Mixed histogram with no buckets, only type-counted data, both scalars and arrays.
    constexpr double kNumObj = 200.0;
    constexpr double kNumNull = 300.0;
    constexpr double kNumFalse = 100.0;
    constexpr double kNumTrue = 400.0;
    constexpr double kNumBool = kNumFalse + kNumTrue;
    t.addHistogram("scalar",
                   getArrayHistogramFromData({/* No histogram data. */},
                                             {{sbe::value::TypeTags::Object, kNumObj},
                                              {sbe::value::TypeTags::Null, kNumNull},
                                              {sbe::value::TypeTags::Boolean, kNumBool}},
                                             kNumTrue,
                                             kNumFalse));
    t.addHistogram("array",
                   getArrayHistogramFromData({/* No scalar buckets. */},
                                             {/* No array unique buckets. */},
                                             {/* No array min buckets. */},
                                             {/* No array max buckets. */},
                                             {{sbe::value::TypeTags::Object, kNumObj},
                                              {sbe::value::TypeTags::Null, kNumNull},
                                              {sbe::value::TypeTags::Boolean, kNumBool}},
                                             kCollCard._value));

    // Count of each type in array type counters for field "mixed".
    constexpr double kNumObjMA = 50.0;
    constexpr double kNumNullMA = 100.0;
    // For the purposes of this test, we have one array of each value of a non-histogrammable type.
    constexpr double kNumBoolMA = 250.0;
    constexpr double kNumArr = kNumObjMA + kNumNullMA + kNumBoolMA;
    const TypeCounts mixedArrayTC{{sbe::value::TypeTags::Object, kNumObjMA},
                                  {sbe::value::TypeTags::Null, kNumNullMA},
                                  {sbe::value::TypeTags::Boolean, kNumBoolMA}};

    // Count of each type in scalar type counters for field "mixed".
    constexpr double kNumObjMS = 150.0;
    constexpr double kNumNullMS = 200.0;
    constexpr double kNumFalseMS = 150.0;
    constexpr double kNumTrueMS = 100.0;
    constexpr double kNumBoolMS = kNumFalseMS + kNumTrueMS;
    const TypeCounts mixedScalarTC{{sbe::value::TypeTags::Object, kNumObjMS},
                                   {sbe::value::TypeTags::Null, kNumNullMS},
                                   {sbe::value::TypeTags::Boolean, kNumBoolMS}};

    // Quick sanity check of test setup for the "mixed" histogram. The idea is that we want a
    // portion of objects inside arrays, and the rest as scalars, but we want the total count of
    // types to be the same.
    ASSERT_EQ(kNumObjMA + kNumObjMS, kNumObj);
    ASSERT_EQ(kNumNullMA + kNumNullMS, kNumNull);
    ASSERT_EQ(kNumBoolMA + kNumBoolMS, kNumBool);

    t.addHistogram("mixed",
                   getArrayHistogramFromData({/* No scalar buckets. */},
                                             {/* No array unique buckets. */},
                                             {/* No array min buckets. */},
                                             {/* No array max buckets. */},
                                             mixedArrayTC,
                                             kNumArr,
                                             0 /* Empty array count. */,
                                             mixedScalarTC,
                                             kNumTrueMS,
                                             kNumFalseMS));

    // Set up indexes.
    t.setIndexes({{"scalarIndex",
                   makeIndexDefinition("scalar", CollationOp::Ascending, /* isMultiKey */ false)}});
    t.setIndexes({{"arrayIndex",
                   makeIndexDefinition("array", CollationOp::Ascending, /* isMultiKey */ true)}});
    t.setIndexes({{"mixedIndex",
                   makeIndexDefinition("mixed", CollationOp::Ascending, /* isMultiKey */ true)}});

    // Tests for scalar type counts only.
    // For object-only intervals in a scalar histogram, we always return object count, no matter
    // what the bounds are. Since we have a scalar histogram for "scalar", we expect all $elemMatch
    // queries to have a cardinality of 0.

    // Test object equality.
    ASSERT_EQ_ELEMMATCH_CE(t, kNumObj, 0.0, "scalar", "{$eq: {}}");
    ASSERT_EQ_ELEMMATCH_CE(t, kNumObj, 0.0, "scalar", "{$eq: {a: 1}}");
    ASSERT_EQ_ELEMMATCH_CE(t, kNumObj, 0.0, "scalar", "{$eq: {b: 2, c: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, kNumObj, 0.0, "scalar", "{$lt: {b: 2, c: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, kNumObj, 0.0, "scalar", "{$gt: {b: 2, c: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, kNumObj, 0.0, "scalar", "{$gte: {b: 2, c: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, kNumObj, 0.0, "scalar", "{$lte: {b: 2, c: 3}}");

    // Test intervals including the empty object. Note that range queries on objects do not generate
    // point equalities, so these fall back onto logic in interval estimation that identifies that
    // the generated intervals are subsets of the object type interval. Note: we don't even generate
    // a SargableNode for the first case. The generated bounds are:
    // [{}, {}) because {} is the "minimum" value for the object type.
    ASSERT_EQ_ELEMMATCH_CE(t, 0.0, 0.0, "scalar", "{$lt: {}}");
    ASSERT_EQ_ELEMMATCH_CE(t, kNumObj, 0.0, "scalar", "{$gt: {}}");
    ASSERT_EQ_ELEMMATCH_CE(t, kNumObj, 0.0, "scalar", "{$gte: {}}");
    ASSERT_EQ_ELEMMATCH_CE(t, kNumObj, 0.0, "scalar", "{$lte: {}}");

    // Rather than combining the intervals together, in the following cases we generate two
    // object-only intervals in the requirements map with the following bounds. Each individual
    // interval is estimated as having a cardinality of 'kNumObj', before we apply conjunctive
    // exponential backoff to combine them.
    constexpr double k2ObjCard = 89.4427;  // == 200/1000 * sqrt(200/1000) * 1000
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, 0.0, "scalar", "{$gt: {}, $lte: {b: 2, c: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, 0.0, "scalar", "{$gte: {}, $lte: {b: 2, c: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, 0.0, "scalar", "{$gte: {}, $lt: {b: 2, c: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, 0.0, "scalar", "{$gt: {}, $lt: {b: 2, c: 3}}");

    // Test intervals including {a: 1}. Similar to the above case, we have two intervals in the
    // requirements map.
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, 0.0, "scalar", "{$gt: {a: 1}, $lte: {b: 2, c: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, 0.0, "scalar", "{$gte: {a: 1}, $lte: {b: 2, c: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, 0.0, "scalar", "{$gte: {a: 1}, $lt: {b: 2, c: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, 0.0, "scalar", "{$gt: {a: 1}, $lt: {b: 2, c: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, 0.0, "scalar", "{$gt: {a: 1}, $lte: {a: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, 0.0, "scalar", "{$gte: {a: 1}, $lte: {a: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, 0.0, "scalar", "{$gte: {a: 1}, $lt: {a: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, 0.0, "scalar", "{$gt: {a: 1}, $lt: {a: 3}}");

    // Test that for null, we always return null count.
    // Note that for ranges including null (e.g. {$lt: null}) we don't generate any SargableNodes.
    ASSERT_EQ_ELEMMATCH_CE(t, kNumNull, 0.0, "scalar", "{$eq: null}");

    // Test boolean count estimate.
    ASSERT_EQ_ELEMMATCH_CE(t, kNumTrue, 0.0, "scalar", "{$eq: true}");
    ASSERT_EQ_ELEMMATCH_CE(t, kNumFalse, 0.0, "scalar", "{$eq: false}");

    // Tests for array type counts only.
    // For object-only intervals in an array histogram, if we're using $elemMatch on an object-only
    // interval, we always return object count. While we have no scalar type counts for "array",
    // non-$elemMatch queries should also match objects embedded in arrays, so we still return
    // object count in that case.

    // Test object equality.
    ASSERT_EQ_ELEMMATCH_CE(t, kNumObj, kNumObj, "array", "{$eq: {}}");
    ASSERT_EQ_ELEMMATCH_CE(t, kNumObj, kNumObj, "array", "{$eq: {a: 1}}");
    ASSERT_EQ_ELEMMATCH_CE(t, kNumObj, kNumObj, "array", "{$eq: {b: 2, c: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, kNumObj, kNumObj, "array", "{$lt: {b: 2, c: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, kNumObj, kNumObj, "array", "{$gt: {b: 2, c: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, kNumObj, kNumObj, "array", "{$gte: {b: 2, c: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, kNumObj, kNumObj, "array", "{$lte: {b: 2, c: 3}}");

    // Test intervals including the empty object.
    // Note: we don't even generate a SargableNode for the first case. The generated bounds are:
    // [{}, {}) because {} is the "minimum" value for the object type.
    ASSERT_EQ_ELEMMATCH_CE(t, 0.0, 0.0, "array", "{$lt: {}}");
    ASSERT_EQ_ELEMMATCH_CE(t, kNumObj, kNumObj, "array", "{$gt: {}}");
    ASSERT_EQ_ELEMMATCH_CE(t, kNumObj, kNumObj, "array", "{$gte: {}}");
    ASSERT_EQ_ELEMMATCH_CE(t, kNumObj, kNumObj, "array", "{$lte: {}}");

    // Similar to above, here we have two object intervals for non-$elemMatch queries. However, for
    // $elemMatch queries, we have the following intervals in the requirements map:
    //  1. [[], BinData(0, )) with CE 1000
    //  2. The actual object interval, e.g. ({}, {b: 2, c: 3}] with CE 200
    constexpr double kArrEMCard = kNumObj;  // == 200/1000 * sqrt(1000/1000) * 1000
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, kArrEMCard, "array", "{$gt: {}, $lte: {b: 2, c: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, kArrEMCard, "array", "{$gte: {}, $lte: {b: 2, c: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, kArrEMCard, "array", "{$gte: {}, $lt: {b: 2, c: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, kArrEMCard, "array", "{$gt: {}, $lt: {b: 2, c: 3}}");

    // Test intervals including {a: 1}; similar to above, we have two object intervals.
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, kArrEMCard, "array", "{$gt: {a: 1}, $lte: {b: 2, c: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, kArrEMCard, "array", "{$gte: {a: 1}, $lte: {b: 2, c: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, kArrEMCard, "array", "{$gte: {a: 1}, $lt: {b: 2, c: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, kArrEMCard, "array", "{$gt: {a: 1}, $lt: {b: 2, c: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, kArrEMCard, "array", "{$gt: {a: 1}, $lte: {a: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, kArrEMCard, "array", "{$gte: {a: 1}, $lte: {a: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, kArrEMCard, "array", "{$gte: {a: 1}, $lt: {a: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, kArrEMCard, "array", "{$gt: {a: 1}, $lt: {a: 3}}");

    // Test that for null, we always return null count.
    // Note that for ranges including null (e.g. {$lt: null}) we don't generate any SargableNodes.
    ASSERT_EQ_ELEMMATCH_CE(t, kNumNull, kNumNull, "array", "{$eq: null}");

    // Test boolean count estimate.
    ASSERT_EQ_ELEMMATCH_CE(t, kNumBool, kNumBool, "array", "{$eq: true}");
    ASSERT_EQ_ELEMMATCH_CE(t, kNumBool, kNumBool, "array", "{$eq: false}");

    // Tests for mixed type counts only. Regular match predicates should be estimated as the sum of
    // the scalar and array counts (e.g. for objects, 'kNumObj'), while elemMatch predicates
    // should be estimated without scalars, returning the array type count (for objects this is
    // 'kNumObjMA').

    // Test object equality.
    ASSERT_EQ_ELEMMATCH_CE(t, kNumObj, kNumObjMA, "mixed", "{$eq: {}}");
    ASSERT_EQ_ELEMMATCH_CE(t, kNumObj, kNumObjMA, "mixed", "{$eq: {a: 1}}");
    ASSERT_EQ_ELEMMATCH_CE(t, kNumObj, kNumObjMA, "mixed", "{$eq: {b: 2, c: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, kNumObj, kNumObjMA, "mixed", "{$lt: {b: 2, c: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, kNumObj, kNumObjMA, "mixed", "{$gt: {b: 2, c: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, kNumObj, kNumObjMA, "mixed", "{$gte: {b: 2, c: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, kNumObj, kNumObjMA, "mixed", "{$lte: {b: 2, c: 3}}");

    // Test intervals including the empty object.
    // Note: we don't even generate a SargableNode for the first case. The generated bounds are:
    // [{}, {}) because {} is the "minimum" value for the object type.
    ASSERT_EQ_ELEMMATCH_CE(t, 0.0, 0.0, "mixed", "{$lt: {}}");
    ASSERT_EQ_ELEMMATCH_CE(t, kNumObj, kNumObjMA, "mixed", "{$gt: {}}");
    ASSERT_EQ_ELEMMATCH_CE(t, kNumObj, kNumObjMA, "mixed", "{$gte: {}}");
    ASSERT_EQ_ELEMMATCH_CE(t, kNumObj, kNumObjMA, "mixed", "{$lte: {}}");

    // Similar to above, here we have two object intervals for non-$elemMatch queries. However, for
    // $elemMatch queries, we have the following intervals in the requirements map:
    //  1. [[], BinData(0, )) with CE 1000
    //  2. The actual object interval, e.g. ({}, {b: 2, c: 3}] with CE 50
    constexpr double kMixEMCard = kNumObjMA;  // == 50/1000 * sqrt(1000/1000) * 1000
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, kMixEMCard, "mixed", "{$gt: {}, $lte: {b: 2, c: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, kMixEMCard, "mixed", "{$gte: {}, $lte: {b: 2, c: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, kMixEMCard, "mixed", "{$gte: {}, $lt: {b: 2, c: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, kMixEMCard, "mixed", "{$gt: {}, $lt: {b: 2, c: 3}}");

    // Test intervals including {a: 1}; similar to above, we have two object intervals.
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, kMixEMCard, "mixed", "{$gt: {a: 1}, $lte: {b: 2, c: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, kMixEMCard, "mixed", "{$gte: {a: 1}, $lte: {b: 2, c: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, kMixEMCard, "mixed", "{$gte: {a: 1}, $lt: {b: 2, c: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, kMixEMCard, "mixed", "{$gt: {a: 1}, $lt: {b: 2, c: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, kMixEMCard, "mixed", "{$gt: {a: 1}, $lte: {a: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, kMixEMCard, "mixed", "{$gte: {a: 1}, $lte: {a: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, kMixEMCard, "mixed", "{$gte: {a: 1}, $lt: {a: 3}}");
    ASSERT_EQ_ELEMMATCH_CE(t, k2ObjCard, kMixEMCard, "mixed", "{$gt: {a: 1}, $lt: {a: 3}}");

    // Test that for null, we always return null count.
    // Note that for ranges including null (e.g. {$lt: null}) we don't generate any SargableNodes.
    ASSERT_EQ_ELEMMATCH_CE(t, kNumNull, kNumNullMA, "mixed", "{$eq: null}");

    // Test boolean count estimate.
    ASSERT_EQ_ELEMMATCH_CE(t, kNumTrueMS + kNumBoolMA, kNumBoolMA, "mixed", "{$eq: true}");
    ASSERT_EQ_ELEMMATCH_CE(t, kNumFalseMS + kNumBoolMA, kNumBoolMA, "mixed", "{$eq: false}");

    // Test combinations of the three fields/ type counters.
    constexpr double k3ObjCard =
        59.814;  // == 200/1000 * sqrt(200/1000) * sqrt(sqrt(200/1000)) * 1000
    constexpr double k4ObjCard = 48.914;
    ASSERT_MATCH_CE_NODE(t,
                         "{scalar: {$eq: {a: 1}}, mixed: {$eq: {b: 1}}, array: {$eq: {c: 1}}}",
                         k3ObjCard,
                         isSargable3);
    ASSERT_MATCH_CE_NODE(
        t,
        "{scalar: {$eq: {}}, mixed: {$lt: {b: 1}}, array: {$gt: {a: 1}, $lte: {a: 2, b: 4, c: 3}}}",
        k4ObjCard,
        isSargable4);

    // Should always get a 0.0 cardinality for an $elemMatch on a scalar predicate.
    ASSERT_MATCH_CE(t,
                    "{scalar: {$elemMatch: {$eq: {a: 1}}}, mixed: {$elemMatch: {$eq: {b: 1}}},"
                    " array: {$elemMatch: {$eq: {c: 1}}}}",
                    0.0);
    ASSERT_MATCH_CE(t,
                    "{scalar: {$elemMatch: {$eq: {}}}, mixed: {$elemMatch: {$lt: {b: 1}}},"
                    " array: {$elemMatch: {$gt: {a: 1}, $lte: {a: 2, b: 4, c: 3}}}}",
                    0.0);

    // The 'array' interval estimate is 50, but the 'mixed' interval estimate is 200.
    constexpr double kArrMixObjEMCard = 22.3607;  // == 50/1000 * sqrt(200/1000) * 1000
    ASSERT_MATCH_CE_NODE(t,
                         "{mixed: {$elemMatch: {$eq: {b: 1}}}, array: {$elemMatch: {$eq: {c: 1}}}}",
                         kArrMixObjEMCard,
                         isSargable4);
    ASSERT_MATCH_CE_NODE(t,
                         "{mixed: {$elemMatch: {$lt: {b: 1}}},"
                         " array: {$elemMatch: {$gt: {a: 1}, $lte: {a: 2, b: 4, c: 3}}}}",
                         kArrMixObjEMCard,
                         isSargable4);
}

TEST(CEHistogramTest, TestNestedArrayTypeCounterPredicates) {
    // This test validates the correct behaviour of both the nested-array type counter as well as
    // combinations of type counters and histogram estimates.
    constexpr CEType kCollCard{1000.0};
    constexpr double kNumArr = 600.0;      // Total number of arrays.
    constexpr double kNumNestArr = 500.0;  // Frequency of nested arrays, e.g. [[1, 2, 3]].
    constexpr double kNumNonNestArr = 100.0;
    constexpr double kNum1 = 2.0;      // Frequency of 1.
    constexpr double kNum2 = 3.0;      // Frequency of 2.
    constexpr double kNum3 = 5.0;      // Frequency of 3.
    constexpr double kNumArr1 = 20.0;  // Frequency of [1].
    constexpr double kNumArr2 = 30.0;  // Frequency of [2].
    constexpr double kNumArr3 = 50.0;  // Frequency of [3].
    constexpr double kNumObj = 390.0;  // Total number of scalar objects.

    // Sanity test numbers.
    ASSERT_EQ(kNumArr1 + kNumArr2, kNumArr3);
    ASSERT_EQ(kNumNonNestArr + kNumNestArr, kNumArr);
    ASSERT_EQ(kNumObj + kNumArr + kNum1 + kNum2 + kNum3, kCollCard._value);

    // Define histogram buckets.
    TestBuckets scalarBuckets{{Value(1), kNum1}, {Value(2), kNum2}, {Value(3), kNum3}};
    TestBuckets arrUniqueBuckets{{Value(1), kNumArr1}, {Value(2), kNumArr2}, {Value(3), kNumArr3}};
    TestBuckets arrMinBuckets{{Value(1), kNumArr1}, {Value(2), kNumArr2}, {Value(3), kNumArr3}};
    TestBuckets arrMaxBuckets{{Value(1), kNumArr1}, {Value(2), kNumArr2}, {Value(3), kNumArr3}};

    // Define type counts.
    TypeCounts arrayTypeCounts{{sbe::value::TypeTags::Array, kNumNestArr},
                               {sbe::value::TypeTags::NumberInt32, kNumNonNestArr}};
    TypeCounts scalarTypeCounts{{sbe::value::TypeTags::Object, kNumObj}};

    CEHistogramTester t(collName, kCollCard);
    t.addHistogram("na",
                   getArrayHistogramFromData(std::move(scalarBuckets),
                                             std::move(arrUniqueBuckets),
                                             std::move(arrMinBuckets),
                                             std::move(arrMaxBuckets),
                                             std::move(arrayTypeCounts),
                                             kNumArr,
                                             0 /* Empty array count. */,
                                             std::move(scalarTypeCounts)));
    t.setIndexes(
        {{"index", makeIndexDefinition("na", CollationOp::Ascending, /* isMultiKey */ true)}});

    // Some equality tests on types that are not present in the type counters should return 0.0.
    ASSERT_EQ_ELEMMATCH_CE(t, 0.0, 0.0, "na", "{$eq: false}");
    ASSERT_EQ_ELEMMATCH_CE(t, 0.0, 0.0, "na", "{$eq: true}");
    ASSERT_EQ_ELEMMATCH_CE(t, 0.0, 0.0, "na", "{$eq: null}");
    // We don't have any objects in arrays, so don't count them.
    ASSERT_EQ_ELEMMATCH_CE(t, kNumObj, 0.0, "na", "{$eq: {a: 1}}");

    // Quick equality test to see if regular array histogram estimation still works as expected.
    ASSERT_EQ_ELEMMATCH_CE(t, kNumArr1 + kNum1, kNumArr1, "na", "{$eq: 1}");
    ASSERT_EQ_ELEMMATCH_CE(t, kNumArr2 + kNum2, kNumArr2, "na", "{$eq: 2}");
    ASSERT_EQ_ELEMMATCH_CE(t, kNumArr3 + kNum3, kNumArr3, "na", "{$eq: 3}");

    // Test a range predicate.
    // - For simple $lt, we correctly return both scalar and array counts that could match.
    // - For $elemMatch + $lt, we have two entries in the requirements map.
    //   - The PathArr interval, estimated correctly as 'kNumArr'.
    //   - The interval {$lt: 3}, estimated as an array histogram range interval.
    // We then combine the estimates for the two using conjunctive exponential backoff.
    constexpr double elemMatchRange = 71.5485;
    ASSERT_EQ_ELEMMATCH_CE(
        t, kNumArr1 + kNum1 + kNumArr2 + kNum2, elemMatchRange, "na", "{$lt: 3}");
    ASSERT_EQ_ELEMMATCH_CE(t, 0.0, 0.0, "na", "{$lt: 1}");

    // Test equality to arrays.
    // - $elemMatch, estimation, as expected, will return the count of nested arrays.
    // - For the case where we see equality to the array, we have a disjunction of intervals in the
    // same entry of the SargableNode requirements map. For the case of {$eq: [1]}, for example, we
    // have: [[1], [1]] U [1, 1]. As a result, we estimate each point interval separately:
    //   - [[1], [1]]: We estimate the nested array interval as 'kNumNestArr'.
    //   - [1, 1]: We estimate the regular point interval as 'kNumArr1' + 'kNum1'.
    // We then combine the results by exponential backoff. Note that we will NOT match {na: 1};
    // however, because of the way the interval is defined, our estimate suggests that we would.
    // TODO: is there a way to know this on the CE side?
    constexpr double kArr1EqCard = 505.531;  // (1 - (1 - 500.0/1000) * sqrt(1 - 22.0/1000)) * 1000
    constexpr double kArr2EqCard = 508.319;  // (1 - (1 - 500.0/1000) * sqrt(1 - 33.0/1000)) * 1000
    constexpr double kArr3EqCard = 513.944;  // (1 - (1 - 500.0/1000) * sqrt(1 - 55.0/1000)) * 1000
    ASSERT_EQ_ELEMMATCH_CE_NODE(t, kArr1EqCard, kNumNestArr, "na", "{$eq: [1]}", isSargable);
    ASSERT_EQ_ELEMMATCH_CE_NODE(t, kArr2EqCard, kNumNestArr, "na", "{$eq: [2]}", isSargable);
    ASSERT_EQ_ELEMMATCH_CE_NODE(t, kArr3EqCard, kNumNestArr, "na", "{$eq: [3]}", isSargable);
    // For the last case, we have the interval [[1, 2, 3], [1, 2, 3]] U [1, 1].
    // TODO: is this interval semantically correct?
    ASSERT_EQ_ELEMMATCH_CE_NODE(t, kArr1EqCard, kNumNestArr, "na", "{$eq: [1, 2, 3]}", isSargable);

    // Now, we test the case of nested arrays.
    // - $elemMatch, once again, returns the number of nested arrays.
    // - Simple equality generates two intervals. We estimate both intervals using the nested array
    // type count. For {$eq: [[1, 2, 3]]}, we get:
    //   - [[1, 2, 3], [1, 2, 3]] U [[[1, 2, 3]]], [[1, 2, 3]]]
    constexpr double kNestedEqCard =
        646.447;  // (1 - (1 - 500.0/1000) * sqrt(1 - 500.0/1000)) * 1000
    ASSERT_EQ_ELEMMATCH_CE_NODE(
        t, kNestedEqCard, kNumNestArr, "na", "{$eq: [[1, 2, 3]]}", isSargable);
    ASSERT_EQ_ELEMMATCH_CE_NODE(t, kNestedEqCard, kNumNestArr, "na", "{$eq: [[1]]}", isSargable);
    ASSERT_EQ_ELEMMATCH_CE_NODE(t, kNestedEqCard, kNumNestArr, "na", "{$eq: [[2]]}", isSargable);
    ASSERT_EQ_ELEMMATCH_CE_NODE(t, kNestedEqCard, kNumNestArr, "na", "{$eq: [[3]]}", isSargable);

    // Note: we can't convert range queries on arrays to SargableNodes yet. If we ever can, we
    // should add some more tests here.
}

TEST(CEHistogramTest, TestFallbackForNonConstIntervals) {
    // This is a sanity test to validate fallback for an interval with non-const bounds.
    IntervalRequirement intervalLowNonConst{
        BoundRequirement(true /*inclusive*/, make<Variable>("v1")),
        BoundRequirement::makePlusInf()};

    IntervalRequirement intervalHighNonConst{
        BoundRequirement::makeMinusInf(),
        BoundRequirement(true /*inclusive*/, make<Variable>("v2"))};

    IntervalRequirement intervalEqNonConst{
        BoundRequirement(true /*inclusive*/, make<Variable>("v3")),
        BoundRequirement(true /*inclusive*/, make<Variable>("v3"))};

    const auto estInterval = [](const auto& interval) {
        const auto ah = ArrayHistogram::make();
        return estimateIntervalCardinality(
            *ah, interval, {100} /* inputCardinality */, true /* includeScalar */);
    };

    ASSERT_EQ(estInterval(intervalLowNonConst)._value, -1.0);
    ASSERT_EQ(estInterval(intervalHighNonConst)._value, -1.0);
    ASSERT_EQ(estInterval(intervalEqNonConst)._value, -1.0);
}

TEST(CEHistogramTest, TestHistogramNeq) {
    constexpr double kCollCard = 10.0;

    CEHistogramTester t("test", {kCollCard});
    {
        std::vector<stats::SBEValue> values;
        for (double v = 0; v < kCollCard; v++) {
            values.push_back(stage_builder::makeValue(Value(v)));
            values.push_back(stage_builder::makeValue(Value(BSON_ARRAY(v))));
        }
        addHistogramFromValues(t, "a", values, kCollCard);
    }

    {
        std::vector<stats::SBEValue> values;
        std::string s = "charA";
        for (double v = 0; v < kCollCard; v++) {
            s[4] += (char)v;
            values.push_back(stage_builder::makeValue(Value(s)));
            values.push_back(stage_builder::makeValue(Value(BSON_ARRAY(s))));
        }
        addHistogramFromValues(t, "b", values, kCollCard);
    }

    // In the scalar case, we generate 10 buckets, with each unique value as a boundary value with
    // cardinality 1. In the array case, we do the same for min/max/unique. Unfortunately, we are
    // not always able to generate sargable nodes, so we generally fall back to heuristic
    // estimation.

    CEType eqCE{2.0};
    CEType eqElemCE{1.0};
    CEType eqHeu{6.83772};
    CEType eqHeuNotNe{3.16228};
    ASSERT_EQ_ELEMMATCH_CE(t, eqCE, eqElemCE, "a", "{$eq: 5}");
    ASSERT_EQ_ELEMMATCH_CE(t, eqHeu, eqHeu, "a", "{$not: {$eq: 5}}");
    ASSERT_EQ_ELEMMATCH_CE(t, eqHeuNotNe, eqElemCE, "a", "{$not: {$ne: 5}}");
    ASSERT_EQ_ELEMMATCH_CE(t, eqHeu, eqHeu, "a", "{$ne: 5}");

    ASSERT_EQ_ELEMMATCH_CE(t, eqCE, eqElemCE, "b", "{$eq: 'charB'}");
    ASSERT_EQ_ELEMMATCH_CE(t, eqHeu, eqHeu, "b", "{$not: {$eq: 'charB'}}");
    ASSERT_EQ_ELEMMATCH_CE(t, eqHeuNotNe, eqElemCE, "b", "{$not: {$ne: 'charB'}}");
    ASSERT_EQ_ELEMMATCH_CE(t, eqHeu, eqHeu, "b", "{$ne: 'charB'}");

    // Test conjunctions where both fields have histograms. Note that when both ops are $ne, we
    // never use histogram estimation because the optimizer only generates filetr nodes (no sargable
    // nodes).
    CEType neNeCE{4.22282};
    CEType neEqCE{0.585786};
    ASSERT_MATCH_CE(t, "{$and: [{a: {$ne: 7}}, {b: {$ne: 'charB'}}]}", neNeCE);
    ASSERT_MATCH_CE(t, "{$and: [{a: {$ne: 7}}, {b: {$eq: 'charB'}}]}", neEqCE);
    ASSERT_MATCH_CE(t, "{$and: [{a: {$ne: 7}}, {b: {$ne: 'charB'}}]}", neNeCE);
    ASSERT_MATCH_CE(t, "{$and: [{a: {$ne: 7}}, {b: {$eq: 'charB'}}]}", neEqCE);

    // Test conjunctions where only one field has a histogram (fallback to heuristics).
    neEqCE = {1.384};
    ASSERT_MATCH_CE(t, "{$and: [{a: {$ne: 7}}, {noHist: {$ne: 'charB'}}]}", neNeCE);
    ASSERT_MATCH_CE(t, "{$and: [{a: {$ne: 7}}, {noHist: {$eq: 'charB'}}]}", neEqCE);
    ASSERT_MATCH_CE(t, "{$and: [{a: {$ne: 7}}, {noHist: {$ne: 'charB'}}]}", neNeCE);
    ASSERT_MATCH_CE(t, "{$and: [{a: {$ne: 7}}, {noHist: {$eq: 'charB'}}]}", neEqCE);
}

TEST(CEHistogramTest, TestHistogramConjTypeCount) {
    constexpr double kCollCard = 40.0;
    CEHistogramTester t("test", {kCollCard});
    {
        std::vector<stats::SBEValue> values;
        for (double v = 0; v < 10.0; v++) {
            values.push_back(stage_builder::makeValue(Value(true)));
            values.push_back(stage_builder::makeValue(Value(false)));
            values.push_back(stage_builder::makeValue(Value(false)));
            // Remaining values in coll for 'tc' are missing.
            values.push_back({value::TypeTags::Nothing, 0});
        }
        addHistogramFromValues(t, "tc", values, kCollCard);
    }

    {
        std::vector<stats::SBEValue> values;
        for (double v = 0; v < 10.0; v++) {
            values.push_back(stage_builder::makeValue(Value(v)));
            // Remaining values in coll for 'i' are missing.
            values.push_back({value::TypeTags::Nothing, 0});
            values.push_back({value::TypeTags::Nothing, 0});
            values.push_back({value::TypeTags::Nothing, 0});
        }
        addHistogramFromValues(t, "i", values, kCollCard);
    }

    // 8.0 values of "i" match (0-7), and each is a bucket boundary.
    ASSERT_MATCH_CE(t, "{i: {$lt: 8}}", 8.0);

    // We estimate this correctly as the number of true values.
    ASSERT_MATCH_CE(t, "{tc: {$eq: true}}", 10.0);

    // We estimate this correctly as the number of false values.
    ASSERT_MATCH_CE(t, "{tc: {$eq: false}}", 20.0);

    // We then apply exponential backoff to combine the estimates of the histogram & type counters.
    // CE = 8/40*sqrt(10/40)*40
    ASSERT_MATCH_CE(t, "{$and: [{i: {$lt: 8}}, {tc: {$eq: true}}]}", 4.0);
    // CE = 8/40*sqrt(20/40)*40
    ASSERT_MATCH_CE(t, "{$and: [{i: {$lt: 8}}, {tc: {$eq: false}}]}", 5.65685);
}
}  // namespace
}  // namespace mongo::optimizer::ce
