/**
 * Tests that the periodic job for persisting sampled queries on shardsvr mongods can handle
 * failover.
 *
 * @tags: [requires_fcv_62, featureFlagAnalyzeShardKey]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/sharding/analyze_shard_key/libs/query_sampling_util.js");

function testStepDown(rst) {
    const dbName = "testDb";
    const collName = "testCollStepDown";
    const ns = dbName + "." + collName;

    let primary = rst.getPrimary();
    let primaryDB = primary.getDB(dbName);

    assert.commandWorked(primaryDB.createCollection(collName));
    const collectionUuid = QuerySamplingUtil.getCollectionUuid(primaryDB, collName);

    const localWriteFp = configureFailPoint(primary, "hangQueryAnalysisWriterBeforeWritingLocally");

    const originalCmdObj = {find: collName, filter: {a: 2}, sampleId: UUID()};
    const expectedSampledQueryDocs = [{
        sampleId: originalCmdObj.sampleId,
        cmdName: "find",
        cmdObj: {filter: originalCmdObj.filter}
    }];

    assert.commandWorked(primaryDB.getCollection(collName).runCommand(originalCmdObj));

    localWriteFp.wait();

    assert.commandWorked(
        primary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
    primary = rst.getPrimary();
    primaryDB = primary.getDB(dbName);

    localWriteFp.off();

    // Verify that the sampled query above did not go missing because of the retryable error caused
    // by stepdown.
    QuerySamplingUtil.assertSoonSampledQueryDocuments(
        primary, ns, collectionUuid, expectedSampledQueryDocs);
}

function testStepUp(rst) {
    const dbName = "testDb";
    const collName = "testCollStepUp";
    const ns = dbName + "." + collName;

    let primary = rst.getPrimary();
    const secondary = rst.getSecondary();
    const primaryDB = primary.getDB(dbName);
    const secondaryTestDB = secondary.getDB(dbName);

    assert.commandWorked(primaryDB.createCollection(collName));
    const collectionUuid = QuerySamplingUtil.getCollectionUuid(primaryDB, collName);
    // Wait for the collection to also exist on secondaries.
    rst.awaitReplication();

    const originalCmdObj = {count: collName, query: {a: 2}, sampleId: UUID()};
    const expectedSampledQueryDocs = [{
        sampleId: originalCmdObj.sampleId,
        cmdName: "count",
        cmdObj: {filter: originalCmdObj.query}
    }];

    const remoteWriteFp =
        configureFailPoint(secondary, "hangQueryAnalysisWriterBeforeWritingRemotely");
    assert.commandWorked(secondaryTestDB.getCollection(collName).runCommand(originalCmdObj));

    remoteWriteFp.wait();
    assert.commandWorked(secondary.adminCommand({replSetFreeze: 0}));
    assert.commandWorked(
        primary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
    primary = rst.getPrimary();

    remoteWriteFp.off();

    // Verify that the sampled query above did not go missing because the node stepped up.
    QuerySamplingUtil.assertSoonSampledQueryDocuments(
        primary, ns, collectionUuid, expectedSampledQueryDocs);
}

const st = new ShardingTest({
    shards: 1,
    rs: {
        nodes: 2,
        // Make the periodic job for writing sampled queries have a period of 1 second to speed up
        // the test.
        setParameter: {queryAnalysisWriterIntervalSecs: 1}
    }
});

testStepDown(st.rs0);
testStepUp(st.rs0);

st.stop();
})();
