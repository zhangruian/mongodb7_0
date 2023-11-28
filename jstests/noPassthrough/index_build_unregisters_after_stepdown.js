/**
 * Tests that index builds are unregistered when we stepdown after an index build was registered but
 * not replicated to secondaries.
 */
load("jstests/libs/fail_point_util.js");            // For configureFailPoint
load('jstests/noPassthrough/libs/index_build.js');  // For IndexBuildTest

const rst = new ReplSetTest({nodes: 3});
rst.startSet();
rst.initiate();

const dbName = 'test';
const collName = 'coll';
const primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB.getCollection(collName);

assert.commandWorked(primaryColl.insert({a: 1}));

rst.awaitReplication();

const secondary = rst.getSecondary();
const secondaryDB = secondary.getDB(dbName);
const secondaryColl = secondaryDB.getCollection(collName);

const hangFp = configureFailPoint(primary, 'hangAfterRegisteringIndexBuild');

let awaitIndexBuild = IndexBuildTest.startIndexBuild(
    primary, primaryColl.getFullName(), {a: 1}, null, [ErrorCodes.InterruptedDueToReplStateChange]);

// Wait for the primary to register the index build. Nothing is replicated to the secondaries at
// this point yet.
hangFp.wait();

assert.commandWorked(secondary.adminCommand({replSetStepUp: 1}));
rst.waitForState(secondary, ReplSetTest.State.PRIMARY);

awaitIndexBuild();

hangFp.off();

// Build the index on the secondary again.
awaitIndexBuild = IndexBuildTest.startIndexBuild(secondary, secondaryColl.getFullName(), {a: 1});
assert.eq(0, awaitIndexBuild(), 'expected shell to exit successfully');

rst.stopSet();
