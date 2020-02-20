// If a donor aborts a migration to a recipient, the recipient does not realize the migration has
// been aborted, and the donor moves on to a new migration, the original recipient will then fail to
// retrieve transferMods from the donor's xfermods log.
//
// Note: don't use coll1 in this test after a coll1 migration is interrupted -- the distlock isn't
// released promptly when interrupted.

load('./jstests/libs/chunk_manipulation_util.js');

(function() {
"use strict";

var staticMongod = MongoRunner.runMongod({});  // For startParallelOps.

var st = new ShardingTest({shards: 3, rs: {nodes: 2}});

var mongos = st.s0, admin = mongos.getDB('admin'), dbName = "testDB", ns1 = dbName + ".foo",
    ns2 = dbName + ".bar", coll1 = mongos.getCollection(ns1), coll2 = mongos.getCollection(ns2),
    shard0 = st.rs0.getPrimary(), shard1 = st.rs1.getPrimary(), shard2 = st.rs2.getPrimary(),
    shard0Coll1 = shard0.getCollection(ns1), shard1Coll1 = shard1.getCollection(ns1),
    shard2Coll1 = shard2.getCollection(ns1), shard0Coll2 = shard0.getCollection(ns2),
    shard1Coll2 = shard1.getCollection(ns2), shard2Coll2 = shard2.getCollection(ns2);

assert.commandWorked(admin.runCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);

assert.commandWorked(admin.runCommand({shardCollection: ns1, key: {a: 1}}));
assert.commandWorked(coll1.insert({a: 0}));
assert.eq(1, shard0Coll1.find().itcount());
assert.eq(0, shard1Coll1.find().itcount());
assert.eq(0, shard2Coll1.find().itcount());
assert.eq(1, coll1.find().itcount());

assert.commandWorked(admin.runCommand({shardCollection: ns2, key: {a: 1}}));
assert.commandWorked(coll2.insert({a: 0}));
assert.eq(1, shard0Coll2.find().itcount());
assert.eq(0, shard1Coll2.find().itcount());
assert.eq(0, shard2Coll2.find().itcount());
assert.eq(1, coll2.find().itcount());

// Shard0:
//      coll1:     [-inf, +inf)
//      coll2:     [-inf, +inf)
// Shard1:
// Shard2:

jsTest.log("Set up complete, now proceeding to test that migration interruption fails.");

// Start coll1 migration to shard1: pause recipient after cloning, donor before interrupt check
pauseMigrateAtStep(shard1, migrateStepNames.cloned);
pauseMoveChunkAtStep(shard0, moveChunkStepNames.startedMoveChunk);
const joinMoveChunk = moveChunkParallel(
    staticMongod, st.s0.host, {a: 0}, null, coll1.getFullName(), st.shard1.shardName);
waitForMigrateStep(shard1, migrateStepNames.cloned);

// Abort migration on donor side, recipient is unaware
killRunningMoveChunk(admin);

unpauseMoveChunkAtStep(shard0, moveChunkStepNames.startedMoveChunk);

if (jsTestOptions().mongosBinVersion == "last-stable") {
    assert.throws(function() {
        joinMoveChunk();
    });
} else {
    jsTestLog("Waiting for donor to write an abort decision.");
    // In FCV 4.4, check the migration coordinator document, because the moveChunk command itself
    // will hang on trying to bump the txn number on the recipient until the recipient has completed
    // and checked the session back in.
    assert.soon(() => {
        return st.rs0.getPrimary().getDB("config").getCollection("migrationCoordinators").findOne({
            nss: ns1,
            decision: "aborted",
        }) != null;
    });

    // This is necessary to allow the following moveChunk to succeed, since the original primary
    // will stay blocked trying to advance the transaction number on the recipient.
    jsTestLog("Electing a new primary for the donor shard.");
    let newPrimary = st.rs0.getSecondary();
    st.rs0.stepUpNoAwaitReplication(newPrimary);
    // This is needed because stepUpNoAwaitReplication does not wait for step-up to complete before
    // returning - only for a new primary to be decided.
    st.rs0.awaitReplication();
    // This is necessary to avoid NotMaster errors on the subsequent moveChunk request, which goes
    // through the config server.
    awaitRSClientHosts(st.configRS.getPrimary(), st.rs0.getPrimary(), {ok: true, ismaster: true});
    // This is necessary to avoid NotMaster errors on the subsequent CRUD ops, which go
    // through the router.
    awaitRSClientHosts(st.s, st.rs0.getPrimary(), {ok: true, ismaster: true});
    jsTestLog("Finished electing a new primary for the donor shard.");

    shard0 = newPrimary;
    shard0Coll1 = shard0.getCollection(ns1);
    shard0Coll2 = shard0.getCollection(ns2);
}

// Start coll2 migration to shard2, pause recipient after cloning step.
pauseMigrateAtStep(shard2, migrateStepNames.cloned);
const joinMoveChunk2 = moveChunkParallel(
    staticMongod, st.s0.host, {a: 0}, null, coll2.getFullName(), st.shard2.shardName);
waitForMigrateStep(shard2, migrateStepNames.cloned);

// Populate donor (shard0) xfermods log.
assert.commandWorked(coll2.insert({a: 1}));
assert.commandWorked(coll2.insert({a: 2}));
assert.eq(3, coll2.find().itcount(), "Failed to insert documents into coll2.");
assert.eq(3, shard0Coll2.find().itcount());

jsTest.log('Releasing coll1 migration recipient, whose transferMods command should fail....');
unpauseMigrateAtStep(shard1, migrateStepNames.cloned);
assert.soon(function() {
    // Wait for the destination shard to report that it is not in an active migration.
    var res = shard1.adminCommand({'_recvChunkStatus': 1});
    return (res.active == false);
}, "coll1 migration recipient didn't abort migration in catchup phase.", 2 * 60 * 1000);
assert.eq(1, shard0Coll1.find().itcount(), "donor shard0 completed a migration that it aborted.");

if (jsTestOptions().mongosBinVersion != "last-stable") {
    assert.throws(function() {
        joinMoveChunk();
    });
}

jsTest.log('Finishing coll2 migration, which should succeed....');
unpauseMigrateAtStep(shard2, migrateStepNames.cloned);
assert.doesNotThrow(function() {
    joinMoveChunk2();
});
assert.eq(0,
          shard0Coll2.find().itcount(),
          "donor shard0 failed to complete a migration after aborting a prior migration.");
assert.eq(3, shard2Coll2.find().itcount(), "shard2 failed to complete migration.");

st.stop();
MongoRunner.stopMongod(staticMongod);
})();
