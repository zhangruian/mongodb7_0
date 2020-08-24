/**
 * Tests the TenantMigrationAccessBlocker and donor state document are updated correctly after
 * the donorStartMigration command is run.
 *
 * Tenant migrations are not expected to be run on servers with ephemeralForTest, and in particular
 * this test fails on ephemeralForTest because the donor has to wait for the write to set the
 * migration state to "committed" and "aborted" to be majority committed but it cannot do that on
 * ephemeralForTest.
 *
 * @tags: [requires_fcv_47, incompatible_with_eft]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");

// An object that mirrors the access states for the TenantMigrationAccessBlocker.
const accessState = {
    kAllow: 0,
    kBlockingWrites: 1,
    kBlockingReadsAndWrites: 2,
    kReject: 3
};

const donorRst = new ReplSetTest(
    {nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}], name: 'donor'});
const recipientRst = new ReplSetTest({nodes: 1, name: 'recipient'});

const kDBPrefix = 'testDb';
const kConfigDonorsNS = "config.tenantMigrationDonors";

let donorPrimary;
let recipientPrimary;
let kRecipientConnString;

const setup = () => {
    donorRst.startSet();
    donorRst.initiate();
    recipientRst.startSet();
    recipientRst.initiate();

    donorPrimary = donorRst.getPrimary();
    recipientPrimary = recipientRst.getPrimary();
    kRecipientConnString = recipientRst.getURL();
};
const tearDown = () => {
    donorRst.stopSet();
    recipientRst.stopSet();
};

(() => {
    // Test the case where the migration commits.
    setup();
    const dbName = kDBPrefix + "Commit";

    function startMigration(host, recipientConnString, dbName) {
        const primary = new Mongo(host);
        assert.commandWorked(primary.adminCommand({
            donorStartMigration: 1,
            migrationId: UUID(),
            recipientConnectionString: recipientConnString,
            databasePrefix: dbName,
            readPreference: {mode: "primary"}
        }));
    }

    let migrationThread =
        new Thread(startMigration, donorPrimary.host, kRecipientConnString, dbName);
    let blockingFp = configureFailPoint(donorPrimary, "pauseTenantMigrationAfterBlockingStarts");
    migrationThread.start();

    // Wait for the migration to enter the blocking state.
    blockingFp.wait();

    let mtab = donorPrimary.adminCommand({serverStatus: 1}).tenantMigrationAccessBlocker;
    assert.eq(mtab[dbName].access, accessState.kBlockingReadsAndWrites);
    assert(mtab[dbName].blockTimestamp);

    let donorDoc = donorPrimary.getCollection(kConfigDonorsNS).findOne({databasePrefix: dbName});
    let blockOplogEntry = donorPrimary.getDB("local").oplog.rs.findOne(
        {ns: kConfigDonorsNS, op: "u", "o.databasePrefix": dbName});
    assert.eq(donorDoc.state, "blocking");
    assert.eq(donorDoc.blockTimestamp, blockOplogEntry.ts);

    // Allow the migration to complete.
    blockingFp.off();
    migrationThread.join();

    mtab = donorPrimary.adminCommand({serverStatus: 1}).tenantMigrationAccessBlocker;
    assert.eq(mtab[dbName].access, accessState.kReject);
    assert(mtab[dbName].commitOrAbortOpTime);

    donorDoc = donorPrimary.getCollection(kConfigDonorsNS).findOne({databasePrefix: dbName});
    let commitOplogEntry =
        donorPrimary.getDB("local").oplog.rs.findOne({ns: kConfigDonorsNS, op: "u", o: donorDoc});
    assert.eq(donorDoc.state, "committed");
    assert.eq(donorDoc.commitOrAbortOpTime.ts, commitOplogEntry.ts);

    const donorRecipientMonitorPoolStats =
        donorPrimary.adminCommand({connPoolStats: 1}).replicaSets;
    assert.eq(Object.keys(donorRecipientMonitorPoolStats).length, 0);

    const recipientSyncDataMetrics =
        recipientPrimary.adminCommand({serverStatus: 1}).metrics.commands.recipientSyncData;
    assert.eq(recipientSyncDataMetrics.failed, 0);
    assert.eq(recipientSyncDataMetrics.total, 2);
    tearDown();
})();

(() => {
    // Test the case where the migration aborts.
    setup();
    const dbName = kDBPrefix + "Abort";

    let abortFp = configureFailPoint(donorPrimary, "abortTenantMigrationAfterBlockingStarts");
    assert.commandFailedWithCode(donorPrimary.adminCommand({
        donorStartMigration: 1,
        migrationId: UUID(),
        recipientConnectionString: kRecipientConnString,
        databasePrefix: dbName,
        readPreference: {mode: "primary"}
    }),
                                 ErrorCodes.TenantMigrationAborted);
    abortFp.off();

    const mtab = donorPrimary.adminCommand({serverStatus: 1}).tenantMigrationAccessBlocker;
    assert.eq(mtab[dbName].access, accessState.kAllow);
    assert(!mtab[dbName].commitOrAbortOpTime);

    const donorDoc = donorPrimary.getCollection(kConfigDonorsNS).findOne({databasePrefix: dbName});
    const abortOplogEntry =
        donorPrimary.getDB("local").oplog.rs.findOne({ns: kConfigDonorsNS, op: "u", o: donorDoc});
    assert.eq(donorDoc.state, "aborted");
    assert.eq(donorDoc.commitOrAbortOpTime.ts, abortOplogEntry.ts);

    const donorRecipientMonitorPoolStats =
        donorPrimary.adminCommand({connPoolStats: 1}).replicaSets;
    assert.eq(Object.keys(donorRecipientMonitorPoolStats).length, 0);

    const recipientSyncDataMetrics =
        recipientPrimary.adminCommand({serverStatus: 1}).metrics.commands.recipientSyncData;
    assert.eq(recipientSyncDataMetrics.failed, 0);
    assert.eq(recipientSyncDataMetrics.total, 2);
    tearDown();
})();
})();
