/**
 * Tests that the migration still proceeds successfully after a state transition write aborts.
 *
 * @tags: [requires_fcv_47, incompatible_with_eft, requires_majority_read_concern]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const kTenantIdPrefix = "testTenantId";
const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    return;
}

/**
 * Starts a migration and forces the write to insert the donor's state doc to abort on the first few
 * tries. Asserts that the migration still completes successfully.
 */
function testAbortInitialState(donorRst) {
    const donorPrimary = donorRst.getPrimary();

    // Force the storage transaction for the insert to abort prior to inserting the WiredTiger
    // record.
    let writeConflictFp = configureFailPoint(donorPrimary, "WTWriteConflictException");

    const tenantId = `${kTenantIdPrefix}-initial`;
    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId,
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
    };

    const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);

    // Run the migration in its own thread, since the initial 'donorStartMigration' command will
    // hang due to the failpoint.
    let migrationThread =
        new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);
    migrationThread.start();
    writeConflictFp.wait();

    // Next, force the storage transaction for the insert to abort after inserting the WiredTiger
    // record and initializing the in-memory migration state.
    let opObserverFp = configureFailPoint(donorPrimary, "donorOpObserverFailAfterOnInsert");
    writeConflictFp.off();
    opObserverFp.wait();
    opObserverFp.off();

    // Verify that the migration completes successfully.
    assert.commandWorked(migrationThread.returnData());
    tenantMigrationTest.waitForNodesToReachState(
        donorRst.nodes, migrationId, tenantId, TenantMigrationTest.State.kCommitted);
}

/**
 * Starts a migration after enabling 'pauseFailPoint' (must pause the migration) and
 * 'setUpFailPoints' on the donor's primary. Forces the write to transition to 'nextState' after
 * reaching 'pauseFailPoint' to abort on the first few tries. Asserts that the migration still
 * completes successfully.
 */
function testAbortStateTransition(donorRst, pauseFailPoint, setUpFailPoints, nextState) {
    jsTest.log(`Test aborting the write to transition to state "${
        nextState}" after reaching failpoint "${pauseFailPoint}"`);

    const donorPrimary = donorRst.getPrimary();
    const tenantId = `${kTenantIdPrefix}-${nextState}`;

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId,
    };

    setUpFailPoints.forEach(failPoint => configureFailPoint(donorPrimary, failPoint));
    let pauseFp = configureFailPoint(donorPrimary, pauseFailPoint);

    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
    pauseFp.wait();

    // Force the storage transaction for the write to transition to the next state to abort prior to
    // updating the WiredTiger record.
    let writeConflictFp = configureFailPoint(donorPrimary, "WTWriteConflictException");
    pauseFp.off();
    writeConflictFp.wait();

    // Next, force the storage transaction for the write to abort after updating the WiredTiger
    // record and registering the change.
    let opObserverFp = configureFailPoint(donorPrimary, "donorOpObserverFailAfterOnUpdate");
    writeConflictFp.off();
    opObserverFp.wait();
    opObserverFp.off();

    // Verify that the migration completes successfully.
    assert.commandWorked(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
    if (nextState === TenantMigrationTest.State.kAborted) {
        tenantMigrationTest.waitForNodesToReachState(
            donorRst.nodes, migrationId, tenantId, TenantMigrationTest.State.kAborted);
    } else {
        tenantMigrationTest.waitForNodesToReachState(
            donorRst.nodes, migrationId, tenantId, TenantMigrationTest.State.kCommitted);
    }
}

const donorRst = tenantMigrationTest.getDonorRst();
jsTest.log("Test aborting donor's state doc insert");
testAbortInitialState(donorRst);

jsTest.log("Test aborting donor's state doc update");
[{
    pauseFailPoint: "pauseTenantMigrationAfterDataSync",
    nextState: TenantMigrationTest.State.kBlocking
},
 {
     pauseFailPoint: "pauseTenantMigrationAfterBlockingStarts",
     nextState: TenantMigrationTest.State.kCommitted
 },
 {
     pauseFailPoint: "pauseTenantMigrationAfterBlockingStarts",
     setUpFailPoints: ["abortTenantMigrationAfterBlockingStarts"],
     nextState: TenantMigrationTest.State.kAborted
 }].forEach(({pauseFailPoint, setUpFailPoints = [], nextState}) => {
    testAbortStateTransition(donorRst, pauseFailPoint, setUpFailPoints, nextState);
});

tenantMigrationTest.stop();
}());
