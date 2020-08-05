/**
 * Tests that that the donor
 * - blocks clusterTime reads that are executed while the migration is in the blocking state but
 *   does not block linearizable reads.
 * - rejects (blocked) clusterTime reads and linearizable reads after the migration commits.
 * - does not reject (blocked) clusterTime reads and linearizable reads after the migration aborts.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern]
 */

(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");

const donorRst = new ReplSetTest(
    {nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}], name: 'donor'});
const recipientRst = new ReplSetTest({nodes: 1, name: 'recipient'});

donorRst.startSet();
donorRst.initiate();
recipientRst.startSet();
recipientRst.initiate();

const kCollName = "testColl";
const kRecipientConnString = recipientRst.getURL();

const kMaxTimeMS = 5 * 1000;
const kConfigDonorsNS = "config.tenantMigrationDonors";

function startMigration(host, dbName, recipientConnString) {
    const primary = new Mongo(host);
    return primary.adminCommand({
        donorStartMigration: 1,
        migrationId: UUID(),
        recipientConnectionString: recipientConnString,
        databasePrefix: dbName,
        readPreference: {mode: "primary"}
    });
}

/**
 * To be used to resume a migration that is paused after entering the blocking state. Waits for the
 * number of blocked reads to reach 'targetBlockedReads' and unpauses the migration.
 */
function resumeMigrationAfterBlockingRead(host, targetBlockedReads) {
    load("jstests/libs/fail_point_util.js");
    const primary = new Mongo(host);

    assert.commandWorked(primary.adminCommand({
        waitForFailPoint: "tenantMigrationBlockRead",
        timesEntered: targetBlockedReads,
        maxTimeMS: kDefaultWaitForFailPointTimeout
    }));

    assert.commandWorked(primary.adminCommand(
        {configureFailPoint: "pauseTenantMigrationAfterBlockingStarts", mode: "off"}));
}

function runCommand(db, cmd, expectedError) {
    const res = db.runCommand(cmd);

    if (expectedError) {
        assert.commandFailedWithCode(res, expectedError);
    } else {
        assert.commandWorked(res);
    }

    if (cmd.lsid) {
        assert.commandWorked(db.runCommand({killSessions: [cmd.lsid]}));
    }
}

/**
 * Tests that the donor rejects causal reads after the migration commits.
 */
function testReadIsRejectedIfSentAfterMigrationHasCommitted(rst, testCase, dbName, collName) {
    let primary = rst.getPrimary();

    assert.commandWorked(primary.adminCommand({
        donorStartMigration: 1,
        migrationId: UUID(),
        recipientConnectionString: kRecipientConnString,
        databasePrefix: dbName,
        readPreference: {mode: "primary"}
    }));

    // Wait for the last oplog entry on the primary to be visible in the committed snapshot view of
    // the oplog on all the secondaries. This is to ensure that the write to put the migration into
    // "committed" is majority-committed and that snapshot reads on the secondaries with unspecified
    // atClusterTime have read timestamp >= commitTimestamp.
    rst.awaitLastOpCommitted();

    const donorDoc = primary.getCollection(kConfigDonorsNS).findOne({databasePrefix: dbName});
    const nodes = testCase.isSupportedOnSecondaries ? rst.nodes : [primary];
    nodes.forEach(node => {
        const db = node.getDB(dbName);
        if (testCase.requiresReadTimestamp) {
            runCommand(db,
                       testCase.command(collName, donorDoc.blockTimestamp),
                       ErrorCodes.TenantMigrationCommitted);
            runCommand(db,
                       testCase.command(collName, donorDoc.commitOrAbortOpTime.ts),
                       ErrorCodes.TenantMigrationCommitted);
        } else {
            runCommand(db, testCase.command(collName), ErrorCodes.TenantMigrationCommitted);
        }
    });
}

/**
 * Tests that the donor does not reject reads after the migration aborts.
 */
function testReadIsAcceptedIfSentAfterMigrationHasAborted(rst, testCase, dbName, collName) {
    const primary = rst.getPrimary();

    let abortFp = configureFailPoint(primary, "abortTenantMigrationAfterBlockingStarts");
    assert.commandFailedWithCode(primary.adminCommand({
        donorStartMigration: 1,
        migrationId: UUID(),
        recipientConnectionString: kRecipientConnString,
        databasePrefix: dbName,
        readPreference: {mode: "primary"}
    }),
                                 ErrorCodes.TenantMigrationAborted);
    abortFp.off();

    // Wait for the last oplog entry on the primary to be visible in the committed snapshot view of
    // the oplog on all the secondaries. This is to ensure that the write to put the migration into
    // "aborted" is majority-committed and that snapshot reads on the secondaries with unspecified
    // atClusterTime have read timestamp >= abortTimestamp.
    rst.awaitLastOpCommitted();

    const donorDoc = primary.getCollection(kConfigDonorsNS).findOne({databasePrefix: dbName});
    const nodes = testCase.isSupportedOnSecondaries ? rst.nodes : [primary];
    nodes.forEach(node => {
        const db = node.getDB(dbName);
        if (testCase.requiresReadTimestamp) {
            runCommand(db, testCase.command(collName, donorDoc.blockTimestamp));
            runCommand(db, testCase.command(collName, donorDoc.commitOrAbortOpTime.ts));
        } else {
            runCommand(db, testCase.command(collName));
        }
    });
}

/**
 * Tests that the donor blocks clusterTime reads in the blocking state with readTimestamp >=
 * blockingTimestamp but does not block linearizable reads.
 */
function testReadBlocksIfMigrationIsInBlocking(rst, testCase, dbName, collName) {
    const primary = rst.getPrimary();

    let blockingFp = configureFailPoint(primary, "pauseTenantMigrationAfterBlockingStarts");
    let migrationThread = new Thread(startMigration, primary.host, dbName, kRecipientConnString);

    // Wait for the migration to enter the blocking state.
    migrationThread.start();
    blockingFp.wait();

    // Wait for the last oplog entry on the primary to be visible in the committed snapshot view of
    // the oplog on all secondaries to ensure that snapshot reads on the secondaries with
    // unspecified atClusterTime have read timestamp >= blockTimestamp.
    rst.awaitLastOpCommitted();

    const donorDoc = primary.getCollection(kConfigDonorsNS).findOne({databasePrefix: dbName});
    const command = testCase.requiresReadTimestamp
        ? testCase.command(collName, donorDoc.blockTimestamp)
        : testCase.command(collName);
    command.maxTimeMS = kMaxTimeMS;

    const nodes = testCase.isSupportedOnSecondaries ? rst.nodes : [primary];
    nodes.forEach(node => {
        const db = node.getDB(dbName);
        runCommand(db, command, testCase.isLinearizableRead ? null : ErrorCodes.MaxTimeMSExpired);
    });

    blockingFp.off();
    migrationThread.join();
    assert.commandWorked(migrationThread.returnData());
}

/**
 * Tests that the donor blocks clusterTime reads in the blocking state with readTimestamp >=
 * blockingTimestamp, and unblocks the reads once the migration aborts.
 */
function testBlockedReadGetsUnblockedAndRejectedIfMigrationCommits(
    rst, testCase, dbName, collName) {
    if (testCase.isLinearizableRead) {
        // Linearizable reads are not blocked.
        return;
    }

    const primary = rst.getPrimary();

    let blockingFp = configureFailPoint(primary, "pauseTenantMigrationAfterBlockingStarts");
    const targetBlockedReads =
        assert
            .commandWorked(primary.adminCommand(
                {configureFailPoint: "tenantMigrationBlockRead", mode: "alwaysOn"}))
            .count +
        1;

    let migrationThread = new Thread(startMigration, primary.host, dbName, kRecipientConnString);
    let resumeMigrationThread =
        new Thread(resumeMigrationAfterBlockingRead, primary.host, targetBlockedReads);

    // Run the commands after the migration enters the blocking state.
    resumeMigrationThread.start();
    migrationThread.start();
    blockingFp.wait();

    // Wait for the last oplog entry on the primary to be visible in the committed snapshot view of
    // the oplog on all secondaries to ensure that snapshot reads on the secondaries with
    // unspecified atClusterTime have read timestamp >= blockTimestamp.
    rst.awaitLastOpCommitted();

    const donorDoc = primary.getCollection(kConfigDonorsNS).findOne({databasePrefix: dbName});
    const command = testCase.requiresReadTimestamp
        ? testCase.command(collName, donorDoc.blockTimestamp)
        : testCase.command(collName);
    const nodes = testCase.isSupportedOnSecondaries ? rst.nodes : [primary];

    // The migration should unpause and commit after the read is blocked. Verify that the read
    // is rejected.
    nodes.forEach(node => {
        const db = node.getDB(dbName);
        runCommand(db, command, ErrorCodes.TenantMigrationCommitted);
    });

    // Verify that the migration succeeded.
    resumeMigrationThread.join();
    migrationThread.join();
    assert.commandWorked(migrationThread.returnData());
}

/**
 * Tests that the donor blocks clusterTime reads in the blocking state with readTimestamp >=
 * blockingTimestamp, and rejects the reads once the migration commits.
 */
function testBlockedReadGetsUnblockedAndSucceedsIfMigrationAborts(rst, testCase, dbName, collName) {
    if (testCase.isLinearizableRead) {
        // Linearizable reads are not blocked.
        return;
    }

    const primary = rst.getPrimary();

    let blockingFp = configureFailPoint(primary, "pauseTenantMigrationAfterBlockingStarts");
    let abortFp = configureFailPoint(primary, "abortTenantMigrationAfterBlockingStarts");
    const targetBlockedReads =
        assert
            .commandWorked(primary.adminCommand(
                {configureFailPoint: "tenantMigrationBlockRead", mode: "alwaysOn"}))
            .count +
        1;

    let migrationThread = new Thread(startMigration, primary.host, dbName, kRecipientConnString);
    let resumeMigrationThread =
        new Thread(resumeMigrationAfterBlockingRead, primary.host, targetBlockedReads);

    // Run the commands after the migration enters the blocking state.
    resumeMigrationThread.start();
    migrationThread.start();
    blockingFp.wait();

    // Wait for the last oplog entry on the primary to be visible in the committed snapshot view of
    // the oplog on all secondaries to ensure that snapshot reads on the secondaries with
    // unspecified atClusterTime have read timestamp >= blockTimestamp.
    rst.awaitLastOpCommitted();

    const donorDoc = primary.getCollection(kConfigDonorsNS).findOne({databasePrefix: dbName});
    const command = testCase.requiresReadTimestamp
        ? testCase.command(collName, donorDoc.blockTimestamp)
        : testCase.command(collName);
    const nodes = testCase.isSupportedOnSecondaries ? rst.nodes : [primary];

    // The migration should unpause and abort after the read is blocked. Verify that the read
    // unblocks.
    nodes.forEach(node => {
        const db = node.getDB(dbName);
        runCommand(db, command);
    });

    // Verify that the migration failed due to the simulated error.
    resumeMigrationThread.join();
    migrationThread.join();
    abortFp.off();
    assert.commandFailedWithCode(migrationThread.returnData(), ErrorCodes.TenantMigrationAborted);
}

const testCases = {
    snapshotReadWithAtClusterTime: {
        isSupportedOnSecondaries: true,
        requiresReadTimestamp: true,
        command: function(collName, readTimestamp) {
            return {
                find: collName,
                readConcern: {
                    level: "snapshot",
                    atClusterTime: readTimestamp,
                }
            };
        },
    },
    snapshotReadWithoutAtClusterTime: {
        isSupportedOnSecondaries: true,
        command: function(collName) {
            return {
                find: collName,
                readConcern: {
                    level: "snapshot",
                }
            };
        },
    },
    snapshotReadWithAtClusterTimeInTxn: {
        isSupportedOnSecondaries: false,
        requiresReadTimestamp: true,
        command: function(collName, readTimestamp) {
            return {
                find: collName,
                lsid: {id: UUID()},
                txnNumber: NumberLong(0),
                startTransaction: true,
                autocommit: false,
                readConcern: {level: "snapshot", atClusterTime: readTimestamp}
            };
        }
    },
    snapshotReadWithoutAtClusterTimeInTxn: {
        isSupportedOnSecondaries: false,
        command: function(collName) {
            return {
                find: collName,
                lsid: {id: UUID()},
                txnNumber: NumberLong(0),
                startTransaction: true,
                autocommit: false,
                readConcern: {level: "snapshot"}
            };
        }
    },
    readWithAfterClusterTime: {
        isSupportedOnSecondaries: true,
        requiresReadTimestamp: true,
        command: function(collName, readTimestamp) {
            return {
                find: collName,
                readConcern: {
                    afterClusterTime: readTimestamp,
                }
            };
        },
    },
    linearizableRead: {
        isSupportedOnSecondaries: false,
        isLinearizableRead: true,
        command: function(collName) {
            return {
                find: collName,
                readConcern: {level: "linearizable"},
            };
        }
    }
};

// Run test cases.
const testFuncs = {
    inCommitted: testReadIsRejectedIfSentAfterMigrationHasCommitted,
    inAborted: testReadIsAcceptedIfSentAfterMigrationHasAborted,
    inBlocking: testReadBlocksIfMigrationIsInBlocking,
    inBlockingThenCommitted: testBlockedReadGetsUnblockedAndRejectedIfMigrationCommits,
    inBlockingThenAborted: testBlockedReadGetsUnblockedAndSucceedsIfMigrationAborts,
};

for (const [testName, testFunc] of Object.entries(testFuncs)) {
    for (const [commandName, testCase] of Object.entries(testCases)) {
        let dbName = commandName + "-" + testName + "0";
        testFunc(donorRst, testCase, dbName, kCollName);
    }
}

donorRst.stopSet();
recipientRst.stopSet();
})();
