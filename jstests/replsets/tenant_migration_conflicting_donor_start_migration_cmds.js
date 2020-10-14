/**
 * Test that tenant migration donors correctly join retried donorStartMigration commands and reject
 * conflicting donorStartMigration commands.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft]
 */
(function() {
'use strict';

load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_util.js");

/**
 * Asserts that the number of recipientDataSync commands executed on the given recipient primary is
 * equal to the given number.
 */
function checkNumRecipientSyncDataCmdExecuted(recipientPrimary, expectedNumExecuted) {
    const recipientSyncDataMetrics =
        recipientPrimary.adminCommand({serverStatus: 1}).metrics.commands.recipientSyncData;
    assert.eq(0, recipientSyncDataMetrics.failed);
    assert.eq(expectedNumExecuted, recipientSyncDataMetrics.total);
}

const chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789';
let charIndex = 0;

/**
 * Returns a tenantId that will not match any existing prefix.
 */
function generateUniqueTenantId() {
    assert.lt(charIndex, chars.length);
    return chars[charIndex++];
}

const rst0 = new ReplSetTest(
    {nodes: 1, name: 'rst0', nodeOptions: {setParameter: {enableTenantMigrations: true}}});
const rst1 = new ReplSetTest(
    {nodes: 1, name: 'rst1', nodeOptions: {setParameter: {enableTenantMigrations: true}}});
const rst2 = new ReplSetTest(
    {nodes: 1, name: 'rst2', nodeOptions: {setParameter: {enableTenantMigrations: true}}});

rst0.startSet();
rst0.initiate();

rst1.startSet();
rst1.initiate();

rst2.startSet();
rst2.initiate();

const rst0Primary = rst0.getPrimary();
const rst1Primary = rst1.getPrimary();

const kConfigDonorsNS = "config.tenantMigrationDonors";

let numRecipientSyncDataCmdSent = 0;

// Test that a retry of a donorStartMigration command joins the existing migration that has
// completed but has not been garbage-collected.
(() => {
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        recipientConnString: rst1.getURL(),
        tenantId: generateUniqueTenantId() + "RetryAfterMigrationCompletes",
        readPreference: {mode: "primary"}
    };

    assert.commandWorked(TenantMigrationUtil.startMigration(rst0Primary.host, migrationOpts));
    assert.commandWorked(TenantMigrationUtil.startMigration(rst0Primary.host, migrationOpts));

    // If the second donorStartMigration had started a duplicate migration, the recipient would have
    // received four recipientSyncData commands instead of two.
    numRecipientSyncDataCmdSent += 2;
    checkNumRecipientSyncDataCmdExecuted(rst1Primary, numRecipientSyncDataCmdSent);
})();

// Test that a retry of a donorStartMigration command joins the ongoing migration.
(() => {
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        recipientConnString: rst1.getURL(),
        tenantId: generateUniqueTenantId() + "RetryBeforeMigrationCompletes",
        readPreference: {mode: "primary"}
    };

    let migrationThread0 =
        new Thread(TenantMigrationUtil.startMigration, rst0Primary.host, migrationOpts);
    let migrationThread1 =
        new Thread(TenantMigrationUtil.startMigration, rst0Primary.host, migrationOpts);

    migrationThread0.start();
    migrationThread1.start();
    migrationThread0.join();
    migrationThread1.join();

    assert.commandWorked(migrationThread0.returnData());
    assert.commandWorked(migrationThread1.returnData());

    // If the second donorStartMigration had started a duplicate migration, the recipient would have
    // received four recipientSyncData commands instead of two.
    numRecipientSyncDataCmdSent += 2;
    checkNumRecipientSyncDataCmdExecuted(rst1Primary, numRecipientSyncDataCmdSent);
})();

/**
 * Tests that the donor throws a ConflictingOperationInProgress error if the client runs a
 * donorStartMigration command to start a migration that conflicts with an existing migration that
 * has committed but not garbage-collected (i.e. the donor has not received donorForgetMigration).
 */
function testStartingConflictingMigrationAfterInitialMigrationCommitted(
    donorPrimary, migrationOpts0, migrationOpts1) {
    assert.commandWorked(TenantMigrationUtil.startMigration(donorPrimary.host, migrationOpts0));
    assert.commandFailedWithCode(
        TenantMigrationUtil.startMigration(donorPrimary.host, migrationOpts1),
        ErrorCodes.ConflictingOperationInProgress);

    // If the second donorStartMigration had started a duplicate migration, there would be two donor
    // state docs.
    let configDonorsColl = donorPrimary.getCollection(kConfigDonorsNS);
    assert.eq(1, configDonorsColl.count({tenantId: migrationOpts0.tenantId}));
}

/**
 * Tests that if the client runs multiple donorStartMigration commands that would start conflicting
 * migrations, only one of the migrations will start and succeed.
 */
function testConcurrentConflictingMigrations(donorPrimary, migrationOpts0, migrationOpts1) {
    let migrationThread0 =
        new Thread(TenantMigrationUtil.startMigration, rst0Primary.host, migrationOpts0);
    let migrationThread1 =
        new Thread(TenantMigrationUtil.startMigration, rst0Primary.host, migrationOpts1);

    migrationThread0.start();
    migrationThread1.start();
    migrationThread0.join();
    migrationThread1.join();

    const res0 = migrationThread0.returnData();
    const res1 = migrationThread1.returnData();
    let configDonorsColl = donorPrimary.getCollection(kConfigDonorsNS);

    // Verify that only one migration succeeded.
    assert(res0.ok || res1.ok);
    assert(!res0.ok || !res1.ok);

    if (res0.ok) {
        assert.commandFailedWithCode(res1, ErrorCodes.ConflictingOperationInProgress);
        assert.eq(1, configDonorsColl.count({tenantId: migrationOpts0.tenantId}));
        if (migrationOpts0.tenantId != migrationOpts1.tenantId) {
            assert.eq(0, configDonorsColl.count({tenantId: migrationOpts1.tenantId}));
        }
    } else {
        assert.commandFailedWithCode(res0, ErrorCodes.ConflictingOperationInProgress);
        assert.eq(1, configDonorsColl.count({tenantId: migrationOpts1.tenantId}));
        if (migrationOpts0.tenantId != migrationOpts1.tenantId) {
            assert.eq(0, configDonorsColl.count({tenantId: migrationOpts0.tenantId}));
        }
    }
}

// Test migrations with different migrationIds but identical settings.
(() => {
    let makeMigrationOpts = () => {
        const migrationOpts0 = {
            migrationIdString: extractUUIDFromObject(UUID()),
            recipientConnString: rst1.getURL(),
            tenantId: generateUniqueTenantId() + "DiffMigrationId",
            readPreference: {mode: "primary"}
        };
        const migrationOpts1 = Object.extend({}, migrationOpts0, true);
        migrationOpts1.migrationIdString = extractUUIDFromObject(UUID());
        return [migrationOpts0, migrationOpts1];
    };

    testStartingConflictingMigrationAfterInitialMigrationCommitted(rst0Primary,
                                                                   ...makeMigrationOpts());
    testConcurrentConflictingMigrations(rst0Primary, ...makeMigrationOpts());
})();

// Test reusing a migrationId for different migration settings.

// Test different tenantIds.
(() => {
    let makeMigrationOpts = () => {
        const migrationOpts0 = {
            migrationIdString: extractUUIDFromObject(UUID()),
            recipientConnString: rst1.getURL(),
            tenantId: generateUniqueTenantId() + "DiffTenantId",
            readPreference: {mode: "primary"}
        };
        const migrationOpts1 = Object.extend({}, migrationOpts0, true);
        migrationOpts1.tenantId = generateUniqueTenantId() + "DiffTenantId";
        return [migrationOpts0, migrationOpts1];
    };

    testStartingConflictingMigrationAfterInitialMigrationCommitted(rst0Primary,
                                                                   ...makeMigrationOpts());
    testConcurrentConflictingMigrations(rst0Primary, ...makeMigrationOpts());
})();

// Test different recipient connection strings.
(() => {
    let makeMigrationOpts = () => {
        const migrationOpts0 = {
            migrationIdString: extractUUIDFromObject(UUID()),
            recipientConnString: rst1.getURL(),
            tenantId: generateUniqueTenantId() + "DiffRecipientConnString",
            readPreference: {mode: "primary"}
        };
        const migrationOpts1 = Object.extend({}, migrationOpts0, true);
        migrationOpts1.recipientConnString = rst2.getURL();
        return [migrationOpts0, migrationOpts1];
    };

    testStartingConflictingMigrationAfterInitialMigrationCommitted(rst0Primary,
                                                                   ...makeMigrationOpts());
    testConcurrentConflictingMigrations(rst0Primary, ...makeMigrationOpts());
})();

// Test different cloning read preference.
(() => {
    let makeMigrationOpts = () => {
        const migrationOpts0 = {
            migrationIdString: extractUUIDFromObject(UUID()),
            recipientConnString: rst1.getURL(),
            tenantId: generateUniqueTenantId() + "DiffReadPref",
            readPreference: {mode: "primary"}
        };
        const migrationOpts1 = Object.extend({}, migrationOpts0, true);
        migrationOpts1.readPreference = {mode: "secondary"};
        return [migrationOpts0, migrationOpts1];
    };

    testStartingConflictingMigrationAfterInitialMigrationCommitted(rst0Primary,
                                                                   ...makeMigrationOpts());
    testConcurrentConflictingMigrations(rst0Primary, ...makeMigrationOpts());
})();

rst0.stopSet();
rst1.stopSet();
rst2.stopSet();
})();
