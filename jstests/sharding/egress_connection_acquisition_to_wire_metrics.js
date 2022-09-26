/**
 * Tests that we are able to log the metrics corresponding to the time it takes from egress
 * connection acquisition to writing to the wire.
 *
 * @tags: [requires_fcv_62, featureFlagConnHealthMetrics]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/log.js");
load("jstests/libs/parallel_shell_helpers.js");

function getConnAcquiredToWireMicros(conn) {
    return conn.adminCommand({serverStatus: 1})
        .metrics.network.totalTimeForEgressConnectionAcquiredToWireMicros;
}

// Set it so that we log the intended metrics only on the mongos.
const paramsDoc = {
    mongosOptions: {setParameter: {connectionAcquisitionToWireLoggingRate: 1.0}},
    shardOptions: {setParameter: {connectionAcquisitionToWireLoggingRate: 0.0}},
    configOptions: {setParameter: {connectionAcquisitionToWireLoggingRate: 0.0}},
};
const st = new ShardingTest({shards: 1, mongos: 1, other: paramsDoc});
let initialConnAcquiredToWireTime = getConnAcquiredToWireMicros(st.s);
jsTestLog(`Initial metric value for mongos totalTimeForEgressConnectionAcquiredToWireMicros: ${
    tojson(initialConnAcquiredToWireTime)}`);
assert.commandWorked(st.s.adminCommand({clearLog: 'global'}));

// The RSM will periodically acquire egress connections to ping the shard and config server nodes,
// but we do an insert to speed up the wait and to be more explicit.
assert.commandWorked(st.s.getDB(jsTestName())["test"].insert({x: 1}));
checkLog.containsJson(st.s, 6496702);
let afterConnAcquiredToWireTime = getConnAcquiredToWireMicros(st.s);
jsTestLog(`End metric value for mongos totalTimeForEgressConnectionAcquiredToWireMicros: ${
    tojson(afterConnAcquiredToWireTime)}`);
assert.gt(afterConnAcquiredToWireTime,
          initialConnAcquiredToWireTime,
          st.s.adminCommand({serverStatus: 1}));

// Test that setting the logging rate to 0 results in silencing of the logs.
st.s.adminCommand({setParameter: 1, connectionAcquisitionToWireLoggingRate: 0.0});
assert.commandWorked(st.s.adminCommand({clearLog: 'global'}));
assert.commandWorked(st.s.getDB(jsTestName())["test"].insert({x: 2}));
try {
    checkLog.containsJson(st.s, 6496702, null, 5 * 1000);
    assert(false);
} catch (e) {
    jsTestLog("Waited long enough to believe logs were correctly silenced.");
}

// Test with mirrored reads to execute the 'fireAndForget' path and verify logs are still correctly
// printed.
const shardPrimary = st.rs0.getPrimary();
assert.commandWorked(
    shardPrimary.adminCommand({setParameter: 1, connectionAcquisitionToWireLoggingRate: 1.0}));
assert.commandWorked(shardPrimary.adminCommand({clearLog: 'global'}));
initialConnAcquiredToWireTime = getConnAcquiredToWireMicros(shardPrimary);
jsTestLog(`Initial metric value for mongod totalTimeForEgressConnectionAcquiredToWireMicros: ${
    tojson(initialConnAcquiredToWireTime)}`);
assert.commandWorked(
    shardPrimary.adminCommand({setParameter: 1, mirrorReads: {samplingRate: 1.0}}));
shardPrimary.getDB(jsTestName()).runCommand({find: "test", filter: {}});
checkLog.containsJson(shardPrimary, 6496702);
afterConnAcquiredToWireTime = getConnAcquiredToWireMicros(shardPrimary);
jsTestLog(`End metric value for mongod totalTimeForEgressConnectionAcquiredToWireMicros: ${
    tojson(afterConnAcquiredToWireTime)}`);
assert.gt(afterConnAcquiredToWireTime,
          initialConnAcquiredToWireTime,
          shardPrimary.adminCommand({serverStatus: 1}));
st.stop();
})();
