/**
 * Ensure that transaction rollback succeeds after an interrupted index ttl update or upgrade
 * collMod command.
 */
(function() {
    'use strict';

    load("jstests/libs/feature_compatibility_version.js");

    let dbpath = MongoRunner.dataPath + "setFCV_collmod_transaction_rollback";
    resetDbpath(dbpath);

    const latest = "latest";

    let conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest});
    assert.neq(
        null, conn, "mongod was unable to start up with version=" + latest + " and no data files");
    let adminDB = conn.getDB("admin");

    var collName = "collModTest";
    var coll = adminDB.getCollection(collName);
    var ttlBeforeRollback = 50;

    assert.commandWorked(
        coll.createIndex({b: 1}, {"name": "index1", "expireAfterSeconds": ttlBeforeRollback}));

    // The failpoint causes an interrupt in the collMod's WriteUnitOfWork, thus triggers a rollback.
    assert.commandWorked(
        adminDB.adminCommand({configureFailPoint: "assertAfterIndexUpdate", mode: "alwaysOn"}));

    // Test transaction rollback after index ttl update collMod.
    assert.commandFailedWithCode(
        adminDB.runCommand(
            {"collMod": collName, "index": {"name": "index1", "expireAfterSeconds": 100}}),
        50970);

    const index = coll.getIndexes();
    var ttlAfterRollback = index[1].expireAfterSeconds;
    assert.eq(ttlAfterRollback, ttlBeforeRollback);

    // SERVER-37634 should remove this test post 4.2.
    // Test transaction rollback after unique index upgrade collMod.
    assert.commandWorked(adminDB.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));

    assert.commandWorked(coll.createIndex({a: 1}, {unique: true}));
    assert.writeOK(coll.insert({_id: 0, a: 1}));

    assert.commandFailedWithCode(adminDB.adminCommand({setFeatureCompatibilityVersion: latestFCV}),
                                 50971);

    MongoRunner.stopMongod(conn);
})();
