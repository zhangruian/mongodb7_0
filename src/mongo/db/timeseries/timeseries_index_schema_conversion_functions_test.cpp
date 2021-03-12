/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

const std::string kTimeseriesTimeFieldName("tm");
const std::string kTimeseriesMetaFieldName("mm");
const std::string kSubField1Name(".subfield1");
const std::string kSubField2Name(".subfield2");
const std::string kControlMinTimeFieldName(
    DocumentSourceInternalUnpackBucket::kControlMinFieldNamePrefix + kTimeseriesTimeFieldName);
const std::string kControlMaxTimeFieldName(
    DocumentSourceInternalUnpackBucket::kControlMaxFieldNamePrefix + kTimeseriesTimeFieldName);
const std::string kTimeseriesSomeDataFieldName("somedatafield");
const std::string kBucketsSomeDataFieldName(BucketUnpacker::kBucketDataFieldName + "." +
                                            kTimeseriesSomeDataFieldName);

/**
 * Constructs a TimeseriesOptions object for testing.
 */
TimeseriesOptions makeTimeseriesOptions() {
    TimeseriesOptions options(kTimeseriesTimeFieldName);
    options.setMetaField(StringData(kTimeseriesMetaFieldName));

    return options;
}

/**
 * Uses 'timeseriesOptions' to convert 'timeseriesIndexSpec' to 'bucketsIndexSpec' and vice versa.
 * If 'testShouldSucceed' is false, pivots to testing that conversion attempts fail.
 */
void testBothWaysIndexSpecConversion(const TimeseriesOptions& timeseriesOptions,
                                     const BSONObj& timeseriesIndexSpec,
                                     const BSONObj& bucketsIndexSpec,
                                     bool testShouldSucceed = true) {
    // Test time-series => buckets schema conversion.

    auto swBucketsIndexSpecs = timeseries::convertTimeseriesIndexSpecToBucketsIndexSpec(
        timeseriesOptions, timeseriesIndexSpec);

    if (testShouldSucceed) {
        ASSERT_OK(swBucketsIndexSpecs);
        ASSERT_BSONOBJ_EQ(bucketsIndexSpec, swBucketsIndexSpecs.getValue());
    } else {
        ASSERT_NOT_OK(swBucketsIndexSpecs);
    }

    // Test buckets => time-series schema conversion.

    auto timeseriesIndexSpecResult = timeseries::convertBucketsIndexSpecToTimeseriesIndexSpec(
        timeseriesOptions, bucketsIndexSpec);

    if (testShouldSucceed) {
        ASSERT_BSONOBJ_EQ(timeseriesIndexSpec, timeseriesIndexSpecResult);
    } else {
        // A buckets collection index spec that does not conform to the supported time-series index
        // spec schema should be converted to an empty time-series index spec result.
        ASSERT(timeseriesIndexSpecResult.isEmpty());
    }
}

// {} <=> {}
TEST(TimeseriesIndexSchemaConversionTest, EmptyTimeseriesIndexSpecDoesNothing) {
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj emptyIndexSpec = {};

    testBothWaysIndexSpecConversion(timeseriesOptions, emptyIndexSpec, emptyIndexSpec);
}

// {tm: 1} <=> {control.min.tm: 1, control.max.tm: 1}
TEST(TimeseriesIndexSchemaConversionTest, AscendingTimeIndexSpecConversion) {
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec = BSON(kTimeseriesTimeFieldName << 1);
    BSONObj bucketsIndexSpec = BSON(kControlMinTimeFieldName << 1 << kControlMaxTimeFieldName << 1);

    testBothWaysIndexSpecConversion(timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec);
}

// {tm: -1} <=> {control.max.tm: -1, control.min.tm: -1}
TEST(TimeseriesIndexSchemaConversionTest, DescendingTimeIndexSpecConversion) {
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec = BSON(kTimeseriesTimeFieldName << -1);
    BSONObj bucketsIndexSpec =
        BSON(kControlMaxTimeFieldName << -1 << kControlMinTimeFieldName << -1);

    testBothWaysIndexSpecConversion(timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec);
}

// {tm.subfield1: 1} <=> {tm.subfield1: 1}
TEST(TimeseriesIndexSchemaConversionTest, TimeSubFieldIndexSpecConversionFails) {
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec = BSON(kTimeseriesTimeFieldName + kSubField1Name << 1);
    BSONObj bucketsIndexSpec = BSON(kTimeseriesTimeFieldName + kSubField1Name << 1);

    testBothWaysIndexSpecConversion(
        timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec, false /* testShouldSucceed */);
}

// {mm: 1} <=> {meta: 1}
TEST(TimeseriesIndexSchemaConversionTest, MetadataIndexSpecConversion) {
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec = BSON(kTimeseriesMetaFieldName << 1);
    BSONObj bucketsIndexSpec = BSON(BucketUnpacker::kBucketMetaFieldName << 1);

    testBothWaysIndexSpecConversion(timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec);
}

// {mm.subfield1: 1, mm.subfield2: 1} <=> {meta.subfield1: 1, mm.subfield2: 1}
TEST(TimeseriesIndexSchemaConversionTest, MetadataCompoundIndexSpecConversion) {
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec = BSON(kTimeseriesMetaFieldName + kSubField1Name
                                       << 1 << kTimeseriesMetaFieldName + kSubField2Name << 1);
    BSONObj bucketsIndexSpec =
        BSON(BucketUnpacker::kBucketMetaFieldName + kSubField1Name
             << 1 << BucketUnpacker::kBucketMetaFieldName + kSubField2Name << 1);

    testBothWaysIndexSpecConversion(timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec);
}

// {tm: 1, mm.subfield1: 1} <=> {control.min.tm: 1, control.max.tm: 1, meta.subfield1: 1}
TEST(TimeseriesIndexSchemaConversionTest, TimeAndMetadataCompoundIndexSpecConversion) {
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec =
        BSON(kTimeseriesTimeFieldName << 1 << kTimeseriesMetaFieldName + kSubField1Name << 1);
    BSONObj bucketsIndexSpec = BSON(kControlMinTimeFieldName
                                    << 1 << kControlMaxTimeFieldName << 1
                                    << BucketUnpacker::kBucketMetaFieldName + kSubField1Name << 1);

    testBothWaysIndexSpecConversion(timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec);
}

// {mm.subfield1: 1, tm: 1} <=> {meta.subfield1: 1, control.min.tm: 1, control.max.tm: 1}
TEST(TimeseriesIndexSchemaConversionTest, MetadataAndTimeCompoundIndexSpecConversion) {
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec =
        BSON(kTimeseriesMetaFieldName + kSubField1Name << 1 << kTimeseriesTimeFieldName << 1);
    BSONObj bucketsIndexSpec =
        BSON(BucketUnpacker::kBucketMetaFieldName + kSubField1Name
             << 1 << kControlMinTimeFieldName << 1 << kControlMaxTimeFieldName << 1);

    testBothWaysIndexSpecConversion(timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec);
}

// {somedatafield: 1} <=> {data.somedatafield: 1}
TEST(TimeseriesIndexSchemaConversionTest, DataIndexSpecConversionFails) {
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec = BSON(kTimeseriesSomeDataFieldName << 1);
    BSONObj bucketsIndexSpec = BSON(kBucketsSomeDataFieldName << 1);

    testBothWaysIndexSpecConversion(
        timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec, false /* testShouldSucceed */);
}

// {tm: 1, somedatafield: 1} <=> {control.min.tm: 1, control.max.tm: 1, data.somedatafield: 1}
TEST(TimeseriesIndexSchemaConversionTest, TimeAndDataCompoundIndexSpecConversionFails) {
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec =
        BSON(kTimeseriesTimeFieldName << 1 << kTimeseriesSomeDataFieldName << 1);
    BSONObj bucketsIndexSpec = BSON(kControlMinTimeFieldName << 1 << kControlMaxTimeFieldName << 1
                                                             << kBucketsSomeDataFieldName << 1);

    testBothWaysIndexSpecConversion(
        timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec, false /* testShouldSucceed */);
}

// {somedatafield: 1, tm: 1} <=> {data.somedatafield: 1, control.min.tm: 1, control.max.tm: 1}
TEST(TimeseriesIndexSchemaConversionTest, DataAndTimeCompoundIndexSpecConversionFails) {
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec =
        BSON(kTimeseriesSomeDataFieldName << 1 << kTimeseriesTimeFieldName << 1);
    BSONObj bucketsIndexSpec = BSON(kBucketsSomeDataFieldName << 1 << kControlMinTimeFieldName << 1
                                                              << kControlMaxTimeFieldName << 1);

    testBothWaysIndexSpecConversion(
        timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec, false /* testShouldSucceed */);
}

// {mm: 1, somedatafield: 1} <=> {meta: 1, data.somedatafield: 1}
TEST(TimeseriesIndexSchemaConversionTest, MetadataAndDataCompoundIndexSpecConversionFails) {
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec =
        BSON(kTimeseriesMetaFieldName << 1 << kTimeseriesSomeDataFieldName << 1);
    BSONObj bucketsIndexSpec =
        BSON(BucketUnpacker::kBucketMetaFieldName << 1 << kBucketsSomeDataFieldName << 1);

    testBothWaysIndexSpecConversion(
        timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec, false /* testShouldSucceed */);
}

// {somedatafield: 1, mm: 1} <=> {data.somedatafield: 1, meta: 1}
TEST(TimeseriesIndexSchemaConversionTest, DataAndMetadataCompoundIndexSpecConversionFails) {
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec =
        BSON(kTimeseriesSomeDataFieldName << 1 << kTimeseriesMetaFieldName << 1);
    BSONObj bucketsIndexSpec =
        BSON(kBucketsSomeDataFieldName << 1 << BucketUnpacker::kBucketMetaFieldName << 1);

    testBothWaysIndexSpecConversion(
        timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec, false /* testShouldSucceed */);
}

// {mm.subfield1: 1, mm.subfield2: 1, mm.foo:1, mm.bar: 1, mm.baz: 1, tm: 1} <=>
// {meta.subfield1: 1, meta.subfield2: 1, meta.foo: 1, meta.bar: 1, meta.baz: 1, control.min.tm: 1,
// control.max.tm: 1}
TEST(TimeseriesIndexSchemaConversionTest, ManyFieldCompoundIndexSpecConversion) {
    const auto kMetaFieldName = BucketUnpacker::kBucketMetaFieldName;

    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec =
        BSON(kTimeseriesMetaFieldName + kSubField1Name
             << 1 << kTimeseriesMetaFieldName + kSubField2Name << 1
             << kTimeseriesMetaFieldName + ".foo" << 1 << kTimeseriesMetaFieldName + ".bar" << 1
             << kTimeseriesMetaFieldName + ".baz" << 1 << kTimeseriesTimeFieldName << 1);
    BSONObj bucketsIndexSpec =
        BSON(kMetaFieldName + kSubField1Name
             << 1 << kMetaFieldName + kSubField2Name << 1 << kMetaFieldName + ".foo" << 1
             << kMetaFieldName + ".bar" << 1 << kMetaFieldName + ".baz" << 1
             << kControlMinTimeFieldName << 1 << kControlMaxTimeFieldName << 1);

    testBothWaysIndexSpecConversion(timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec);
}

}  // namespace
}  // namespace mongo
