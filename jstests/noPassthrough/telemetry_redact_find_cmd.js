/**
 * Test that $telemetry properly redacts find commands, on mongod and mongos.
 */
(function() {
"use strict";

function runTest(conn) {
    const db = conn.getDB("test");
    const admin = conn.getDB("admin");

    db.test.drop();
    db.test.insert({v: 1});

    db.test.find({v: 1}).toArray();

    const getTelemetryRedacted = (conn) => {
        const result = conn.adminCommand({
            aggregate: 1,
            pipeline: [
                {$telemetry: {redactFieldNames: true}},
                // Filter out agg queries, including $telemetry.
                {$match: {"key.find": {$exists: true}}},
                // Sort on telemetry key so entries are in a deterministic order.
                {$sort: {key: 1}},
            ],
            cursor: {}
        });
        return result.cursor.firstBatch;
    };

    let telemetry = getTelemetryRedacted(admin);

    assert.eq(1, telemetry.length);
    assert.eq("n4bQgYhMfWWa", telemetry[0].key.find);
    assert.eq({"TJRIXgwhrmxB": {$eq: "?"}}, telemetry[0].key.filter);

    db.test.insert({v: 2});

    const cursor = db.test.find({v: {$gt: 0, $lt: 3}}).batchSize(1);
    telemetry = getTelemetryRedacted(admin);
    // Cursor isn't exhausted, so there shouldn't be another entry yet.
    assert.eq(1, telemetry.length);

    assert.commandWorked(
        db.runCommand({getMore: cursor.getId(), collection: db.test.getName(), batchSize: 2}));

    telemetry = getTelemetryRedacted(admin);
    assert.eq(2, telemetry.length);
    assert.eq("n4bQgYhMfWWa", telemetry[1].key.find);
    assert.eq({"$and": [{"TJRIXgwhrmxB": {"$gt": "?"}}, {"TJRIXgwhrmxB": {"$lt": "?"}}]},
              telemetry[1].key.filter);
}

const conn = MongoRunner.runMongod({
    setParameter: {
        internalQueryConfigureTelemetrySamplingRate: 2147483647,
        featureFlagTelemetry: true,
    }
});
runTest(conn);
MongoRunner.stopMongod(conn);

const st = new ShardingTest({
    mongos: 1,
    shards: 1,
    config: 1,
    rs: {nodes: 1},
    mongosOptions: {
        setParameter: {
            internalQueryConfigureTelemetrySamplingRate: 2147483647,
            featureFlagTelemetry: true,
        }
    },
});
runTest(st.s);
st.stop();
}());
