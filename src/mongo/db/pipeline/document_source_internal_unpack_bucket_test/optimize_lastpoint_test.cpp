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

#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/idl/server_parameter_test_util.h"

namespace mongo {
namespace {

using InternalUnpackBucketOptimizeLastpointTest = AggregationContextFixture;

void assertExpectedLastpointOpt(const boost::intrusive_ptr<ExpressionContext> expCtx,
                                const std::vector<std::string>& inputPipelineStrs,
                                const std::vector<std::string>& expectedPipelineStrs,
                                const bool expectedSuccess = true) {
    std::vector<BSONObj> inputPipelineBson;
    for (auto stageStr : inputPipelineStrs) {
        inputPipelineBson.emplace_back(fromjson(stageStr));
    }

    auto pipeline = Pipeline::parse(inputPipelineBson, expCtx);
    auto& container = pipeline->getSources();
    ASSERT_EQ(container.size(), inputPipelineBson.size());

    // Assert that the lastpoint optimization succeeds/fails as expected.
    auto success = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                       ->optimizeLastpoint(container.begin(), &container);
    ASSERT_EQ(success, expectedSuccess);

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(serialized.size(), expectedPipelineStrs.size());

    // Assert the pipeline is unchanged.
    auto serializedItr = serialized.begin();
    for (auto stageStr : expectedPipelineStrs) {
        auto expectedStageBson = fromjson(stageStr);
        ASSERT_BSONOBJ_EQ(*serializedItr, expectedStageBson);
        ++serializedItr;
    }
}

TEST_F(InternalUnpackBucketOptimizeLastpointTest, NonLastpointDoesNotParticipateInOptimization) {
    RAIIServerParameterControllerForTest controller("featureFlagLastPointQuery", true);
    auto assertPipelineUnoptimized = [&](const std::string& unpackStr,
                                         const std::string& sortStr,
                                         const std::string& groupStr) {
        std::vector stageStrs{unpackStr, sortStr, groupStr};
        assertExpectedLastpointOpt(getExpCtx(), stageStrs, stageStrs, /* expectedSuccess */ false);
    };

    // $sort must contain a time field.
    assertPipelineUnoptimized(
        "{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: 'm', "
        "bucketMaxSpanSeconds: 60}}",
        "{$sort: {'m.a': 1}}",
        "{$group: {_id: '$m.a', b: {$first: '$b'}, c: {$first: '$c'}}}");

    // $sort must have the time field as the last field in the sort key pattern.
    assertPipelineUnoptimized(
        "{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: 'm', "
        "bucketMaxSpanSeconds: 60}}",
        "{$sort: {t: -1, 'm.a': 1}}",
        "{$group: {_id: '$m.a', b: {$first: '$b'}, c: {$first: '$c'}}}");

    // $group's _id must be a meta field.
    assertPipelineUnoptimized(
        "{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: 'm', "
        "bucketMaxSpanSeconds: 60}}",
        "{$sort: {'m.a': 1, t: -1}}",
        "{$group: {_id: '$nonMeta', b: {$first: '$b'}, c: {$first: '$c'}}}");

    // $group can only contain $first or $last accumulators.
    assertPipelineUnoptimized(
        "{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: 'm', "
        "bucketMaxSpanSeconds: 60}}",
        "{$sort: {'m.a': 1, t: -1}}",
        "{$group: {_id: '$m.a', b: {$first: '$b'}, c: {$last: '$c'}}}");

    // We disallow the rewrite for firstpoint queries due to rounding behaviour on control.min.time.
    assertPipelineUnoptimized(
        "{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
        "'m', bucketMaxSpanSeconds: 60}}",
        "{$sort: {'m.a': -1, t: 1}}",
        "{$group: {_id: '$m.a', b: {$first: '$b'}, c: {$first: '$c'}}}");

    assertPipelineUnoptimized(
        "{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
        "'m', bucketMaxSpanSeconds: 60}}",
        "{$sort: {'m.a': -1, t: -1}}",
        "{$group: {_id: '$m.a', b: {$last: '$b'}, c: {$last: '$c'}}}");

    // The _id field in $group's must match the meta field in $sort.
    assertPipelineUnoptimized(
        "{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
        "'m', bucketMaxSpanSeconds: 60}}",
        "{$sort: {'m.a': -1, t: -1}}",
        "{$group: {_id: '$m.z', b: {$last: '$b'}, c: {$last: '$c'}}}");
}

TEST_F(InternalUnpackBucketOptimizeLastpointTest,
       LastpointWithMetaSubfieldAscendingTimeDescending) {
    RAIIServerParameterControllerForTest controller("featureFlagLastPointQuery", true);
    assertExpectedLastpointOpt(getExpCtx(),
                               /* inputPipelineStrs */
                               {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
                                "'m', bucketMaxSpanSeconds: 60}}",
                                "{$sort: {'m.a': 1, t: -1}}",
                                "{$group: {_id: '$m.a', b: {$first: '$b'}, c: {$first: '$c'}}}"},
                               /* expectedPipelineStrs */
                               {"{$sort: {'meta.a': 1, 'control.max.t': -1, 'control.min.t': -1}}",
                                "{$group: {_id: '$meta.a', meta: {$first: '$meta'}, control: "
                                "{$first: '$control'}, data: {$first: '$data'}}}",
                                "{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
                                "'m', bucketMaxSpanSeconds: 60}}",
                                "{$sort: {'m.a': 1, t: -1}}",
                                "{$group: {_id: '$m.a', b: {$first: '$b'}, c: {$first: '$c'}}}"});
}

TEST_F(InternalUnpackBucketOptimizeLastpointTest,
       LastpointWithMetaSubfieldDescendingTimeDescending) {
    RAIIServerParameterControllerForTest controller("featureFlagLastPointQuery", true);
    assertExpectedLastpointOpt(getExpCtx(),
                               /* inputPipelineStrs */
                               {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
                                "'m', bucketMaxSpanSeconds: 60}}",
                                "{$sort: {'m.a': -1, t: -1}}",
                                "{$group: {_id: '$m.a', b: {$first: '$b'}, c: {$first: '$c'}}}"},
                               /* expectedPipelineStrs */
                               {"{$sort: {'meta.a': -1, 'control.max.t': -1, 'control.min.t': -1}}",
                                "{$group: {_id: '$meta.a', meta: {$first: '$meta'}, control: "
                                "{$first: '$control'}, data: {$first: '$data'}}}",
                                "{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
                                "'m', bucketMaxSpanSeconds: 60}}",
                                "{$sort: {'m.a': -1, t: -1}}",
                                "{$group: {_id: '$m.a', b: {$first: '$b'}, c: {$first: '$c'}}}"});
}

TEST_F(InternalUnpackBucketOptimizeLastpointTest, LastpointWithMetaSubfieldAscendingTimeAscending) {
    RAIIServerParameterControllerForTest controller("featureFlagLastPointQuery", true);
    assertExpectedLastpointOpt(getExpCtx(),
                               /* inputPipelineStrs */
                               {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
                                "'m', bucketMaxSpanSeconds: 60}}",
                                "{$sort: {'m.a': 1, t: 1}}",
                                "{$group: {_id: '$m.a', b: {$last: '$b'}, c: {$last: '$c'}}}"},
                               /* expectedPipelineStrs */
                               {"{$sort: {'meta.a': -1, 'control.max.t': -1, 'control.min.t': -1}}",
                                "{$group: {_id: '$meta.a', meta: {$first: '$meta'}, control: "
                                "{$first: '$control'}, data: {$first: '$data'}}}",
                                "{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
                                "'m', bucketMaxSpanSeconds: 60}}",
                                "{$sort: {'m.a': 1, t: 1}}",
                                "{$group: {_id: '$m.a', b: {$last: '$b'}, c: {$last: '$c'}}}"});
}

TEST_F(InternalUnpackBucketOptimizeLastpointTest,
       LastpointWithMetaSubfieldDescendingTimeAscending) {
    RAIIServerParameterControllerForTest controller("featureFlagLastPointQuery", true);
    assertExpectedLastpointOpt(getExpCtx(),
                               /* inputPipelineStrs */
                               {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
                                "'m', bucketMaxSpanSeconds: 60}}",
                                "{$sort: {'m.a': -1, t: 1}}",
                                "{$group: {_id: '$m.a', b: {$last: '$b'}, c: {$last: '$c'}}}"},
                               /* expectedPipelineStrs */
                               {"{$sort: {'meta.a': 1, 'control.max.t': -1, 'control.min.t': -1}}",
                                "{$group: {_id: '$meta.a', meta: {$first: '$meta'}, control: "
                                "{$first: '$control'}, data: {$first: '$data'}}}",
                                "{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
                                "'m', bucketMaxSpanSeconds: 60}}",
                                "{$sort: {'m.a': -1, t: 1}}",
                                "{$group: {_id: '$m.a', b: {$last: '$b'}, c: {$last: '$c'}}}"});
}

TEST_F(InternalUnpackBucketOptimizeLastpointTest, LastpointWithComputedMetaProjectionFields) {
    RAIIServerParameterControllerForTest controller("featureFlagLastPointQuery", true);
    // We might get such a case if $_internalUnpackBucket swaps with a $project. Verify that the
    // lastpoint optimization does not break in this scenario. Note that in the full pipeline we
    // would expect $_internalUnpackBucket to be preceded by a stage like $addFields. However,
    // optimizeLastpoint() only gets an interator to the $_internalUnpackBucket stage itself.
    assertExpectedLastpointOpt(
        getExpCtx(),
        /* inputPipelineStrs */
        {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: 'm', "
         "bucketMaxSpanSeconds: 60, computedMetaProjFields: ['abc', 'def']}}",
         "{$sort: {'m.a': -1, t: 1}}",
         "{$group: {_id: '$m.a', b: {$last: '$b'}, c: {$last: '$c'}}}"},
        /* expectedPipelineStrs */
        {"{$sort: {'meta.a': 1, 'control.max.t': -1, 'control.min.t': -1}}",
         "{$group: {_id: '$meta.a', meta: {$first: '$meta'}, control: {$first: '$control'}, data: "
         "{$first: '$data'}, abc: {$first: '$abc'}, def: {$first: '$def'}}}",
         "{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: 'm', "
         "bucketMaxSpanSeconds: 60, computedMetaProjFields: ['abc', 'def']}}",
         "{$sort: {'m.a': -1, t: 1}}",
         "{$group: {_id: '$m.a', b: {$last: '$b'}, c: {$last: '$c'}}}"});

    // Furthermore, validate that we can use the lastpoint optimization in the case where we have a
    // projection included in the final $group.
    assertExpectedLastpointOpt(
        getExpCtx(),
        /* inputPipelineStrs */
        {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: 'm', "
         "bucketMaxSpanSeconds: 60, computedMetaProjFields: ['abc', 'def']}}",
         "{$sort: {'m.a': -1, t: 1}}",
         "{$group: {_id: '$m.a', b: {$last: '$b'}, c: {$last: '$c'}, def: {$last: '$def'}}}"},
        /* expectedPipelineStrs */
        {"{$sort: {'meta.a': 1, 'control.max.t': -1, 'control.min.t': -1}}",
         "{$group: {_id: '$meta.a', meta: {$first: '$meta'}, control: {$first: '$control'}, data: "
         "{$first: '$data'}, abc: {$first: '$abc'}, def: {$first: '$def'}}}",
         "{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: 'm', "
         "bucketMaxSpanSeconds: 60, computedMetaProjFields: ['abc', 'def']}}",
         "{$sort: {'m.a': -1, t: 1}}",
         "{$group: {_id: '$m.a', b: {$last: '$b'}, c: {$last: '$c'}, def: {$last: '$def'}}}"});
}

}  // namespace
}  // namespace mongo
