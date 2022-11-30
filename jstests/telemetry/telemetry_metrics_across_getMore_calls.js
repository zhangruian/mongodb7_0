/**
 * Test that the telemetry metrics are updated correctly across getMores.
 */
load("jstests/libs/profiler.js");  // For getLatestProfilerEntry.

(function() {
"use strict";

if (!FeatureFlagUtil.isEnabled(db, "Telemetry")) {
    return;
}

// Turn on the collecting of telemetry metrics.
let options = {
    setParameter: {internalQueryConfigureTelemetrySamplingRate: 2147483647},
};

const conn = MongoRunner.runMongod(options);
const testDB = conn.getDB('test');
var coll = testDB[jsTestName()];
var collTwo = db[jsTestName() + 'Two'];
coll.drop();

// Make it easier to extract correct telemetry store entry for purposes of this test.
assert.commandWorked(testDB.adminCommand(
    {setParameter: 1, internalQueryConfigureTelemetryFieldNameRedactionStrategy: "none"}));

function verifyMetrics(batch) {
    batch.forEach(element => {
        assert(element.metrics.docsScanned.sum > element.metrics.docsScanned.min);
        assert(element.metrics.docsScanned.sum >= element.metrics.docsScanned.max);
        assert(element.metrics.docsScanned.min <= element.metrics.docsScanned.max);

        // Ensure execution count does not increase with subsequent getMore() calls.
        assert.eq(element.metrics.execCount.sum,
                  element.metrics.execCount.min,
                  element.metrics.execCount.max);

        if (element.metrics.execCount === 1) {
            // Ensure planning time is > 0 after first batch and does not change with subsequent
            // getMore() calls.
            assert(queryOptMicros.min > 0);
            assert.eq(queryOptMicros.sum, queryOptMicros.min, queryOptMicros.max);
        }
        // Confirm that execution time increases with getMore() calls
        assert(element.metrics.queryExecMicros.sum > element.metrics.queryExecMicros.min);
        assert(element.metrics.queryExecMicros.sum > element.metrics.queryExecMicros.max);
        assert(element.metrics.queryExecMicros.min <= element.metrics.queryExecMicros.max);
    });
}

for (var i = 0; i < 200; i++) {
    coll.insert({foo: 0, bar: Math.floor(Math.random() * 3)});
    coll.insert({foo: 1, bar: Math.floor(Math.random() * -2)});
    collTwo.insert({foo: Math.floor(Math.random() * 2), bar: Math.floor(Math.random() * 2)});
}

// Assert that two queries with identical structures are represented by the same key
coll.aggregate([{$match: {foo: 1}}], {cursor: {batchSize: 2}});
coll.aggregate([{$match: {foo: 0}}], {cursor: {batchSize: 2}});
// This command will return all telemetry store entires.
let telStore = testDB.adminCommand({aggregate: 1, pipeline: [{$telemetry: {}}], cursor: {}});
assert.eq(telStore.cursor.firstBatch.length, 1);
// Assert we update execution count for identically shaped queries.
assert.eq(telStore.cursor.firstBatch[0].metrics.execCount, 2);
verifyMetrics(telStore.cursor.firstBatch);

// Assert that options such as limit/sort create different keys
coll.find({foo: {$eq: 0}}).batchSize(2).toArray();
coll.find({foo: {$eq: 1}}).limit(50).batchSize(2).toArray();
coll.find().sort({"foo": 1}).batchSize(2).toArray();
// This filters telemetry entires to just the ones entered when running above find queries.
telStore = testDB.adminCommand({
    aggregate: 1,
    pipeline: [{$telemetry: {}}, {$match: {"key.find.find": {$eq: "###"}}}],
    cursor: {}
});
assert.eq(telStore.cursor.firstBatch.length, 3);
verifyMetrics(telStore.cursor.firstBatch);

// Ensure that for queries using an index, keys scanned is nonzero.
assert.commandWorked(coll.createIndex({bar: 1}));
coll.aggregate([{$match: {$or: [{bar: 1, foo: 1}]}}], {cursor: {batchSize: 2}});
// This filters telemetry entries to just the one entered for the above agg command.
telStore = testDB.adminCommand({
    aggregate: 1,
    pipeline: [
        {$telemetry: {}},
        {$match: {"key.pipeline.$match.$or": {$eq: [{'bar': '###', 'foo': '###'}]}}}
    ],
    cursor: {}
});
assert(telStore.cursor.firstBatch[0].metrics.keysScanned.sum > 0);

MongoRunner.stopMongod(conn);
}());
