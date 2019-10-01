/**
 * Tests that the 'allowPartialResults' option to find is respected, and that aggregation does not
 * accept the 'allowPartialResults' option.
 */

// This test shuts down a shard.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
"use strict";
const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

const st = new ShardingTest({shards: 2});

jsTest.log("Insert some data.");
const nDocs = 100;
const coll = st.s0.getDB(dbName)[collName];
let bulk = coll.initializeUnorderedBulkOp();
for (let i = -50; i < 50; i++) {
    bulk.insert({_id: i});
}
assert.commandWorked(bulk.execute());

jsTest.log("Create a sharded collection with one chunk on each of the two shards.");
st.ensurePrimaryShard(dbName, st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName}));

let findRes;

jsTest.log("Without 'allowPartialResults', if all shards are up, find returns all docs.");
findRes = coll.runCommand({find: collName});
assert.commandWorked(findRes);
assert.eq(nDocs, findRes.cursor.firstBatch.length);

jsTest.log("With 'allowPartialResults: false', if all shards are up, find returns all docs.");
findRes = coll.runCommand({find: collName, allowPartialResults: false});
assert.commandWorked(findRes);
assert.eq(nDocs, findRes.cursor.firstBatch.length);

jsTest.log("With 'allowPartialResults: true', if all shards are up, find returns all docs.");
findRes = coll.runCommand({find: collName, allowPartialResults: true});
assert.commandWorked(findRes);
assert.eq(nDocs, findRes.cursor.firstBatch.length);

jsTest.log("Stopping " + st.shard0.shardName);
st.rs0.stopSet();

jsTest.log("Without 'allowPartialResults', if some shard down, find fails.");
assert.commandFailed(coll.runCommand({find: collName}));

jsTest.log("With 'allowPartialResults: false', if some shard down, find fails.");
assert.commandFailed(coll.runCommand({find: collName, allowPartialResults: false}));

if (jsTestOptions().mongosBinVersion == "last-stable") {
    // In v4.4, mongos was updated to swallow FailedToSatisfyReadPreference errors if
    // allowPartialResults is true, allowing allowPartialResults to work with replica set shards
    // (see SERVER-33597 for details). So when the mongos version is v4.2, the command should
    // fail.
    jsTest.log(
        "With 'allowPartialResults: true', if some shard down and mongos version is v4.2, find fails");
    assert.commandFailedWithCode(coll.runCommand({find: collName, allowPartialResults: true}),
                                 ErrorCodes.FailedToSatisfyReadPreference);
} else {
    jsTest.log(
        "With 'allowPartialResults: true', if some shard down, find succeeds with partial results");
    findRes = coll.runCommand({find: collName, allowPartialResults: true});
    assert.commandWorked(findRes);
    assert.eq(nDocs / 2, findRes.cursor.firstBatch.length);
}

jsTest.log("The allowPartialResults option does not currently apply to aggregation.");
assert.commandFailedWithCode(coll.runCommand({
    aggregate: collName,
    pipeline: [{$project: {_id: 1}}],
    cursor: {},
    allowPartialResults: true
}),
                             ErrorCodes.FailedToParse);

st.stop();
}());
