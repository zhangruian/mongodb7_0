/**
 * Test that dbcheck will be ignored during initial sync.
 *
 * @tags: [
 *   featureFlagSecondaryIndexChecksInDbCheck
 * ]
 */

load("jstests/libs/fail_point_util.js");
import {
    checkHealthLog,
    insertDocsWithMissingIndexKeys,
    logQueries,
    runDbCheck
} from "jstests/replsets/libs/dbcheck_utils.js";

// Skipping data consistency checks because data is inserted into primary and secondary separately.
TestData.skipCollectionAndIndexValidation = true;
TestData.skipCheckDBHashes = true;

const dbName = "ignore_dbcheck_in_intial_sync";
const collName = "ignore_dbcheck_in_intial_sync-collection";

const doc1 = {
    a: 1
};

const replSet = new ReplSetTest({
    name: jsTestName(),
    nodes: [
        {},
        {
            rsConfig:
                // disallow elections on secondary
                {priority: 0}
        }
    ],
    nodeOptions: {setParameter: {dbCheckHealthLogEveryNBatches: 1}}
});
replSet.startSet();
replSet.initiateWithHighElectionTimeout();

let primary = replSet.getPrimary();
const secondary = replSet.getSecondary();
const primaryHealthlog = primary.getDB("local").system.healthlog;
const primaryDb = primary.getDB(dbName);
const maxDocsPerBatch = 100;

jsTestLog("Testing that dbcheck command will be ignored during initial sync.");

const nDocs = 10;
insertDocsWithMissingIndexKeys(replSet, dbName, collName, doc1, nDocs);

replSet.awaitReplication();

const initialSyncNode = replSet.add({
    rsConfig: {priority: 0},
    setParameter:
        {'failpoint.initialSyncHangBeforeSplittingControlFlow': tojson({mode: 'alwaysOn'})}
});

const initialSyncHangBeforeSplittingControlFlowFailPoint =
    configureFailPoint(initialSyncNode, "initialSyncHangBeforeSplittingControlFlow");

replSet.reInitiate();

initialSyncHangBeforeSplittingControlFlowFailPoint.wait();

const primaryDbCheckFailPoint = configureFailPoint(primary, "sleepAfterExtraIndexKeysHashing");

runDbCheck(replSet, primaryDb, collName, {
    maxDocsPerBatch: maxDocsPerBatch  // , validateMode: "dataConsistencyAndMissingIndexKeysCheck"
});

primaryDbCheckFailPoint.wait({timesEntered: 1});

assert.commandWorked(initialSyncNode.adminCommand(
    {configureFailPoint: 'initialSyncHangBeforeSplittingControlFlow', mode: 'off'}));

replSet.waitForState(initialSyncNode, ReplSetTest.State.SECONDARY);

// Check that the primary logged an error health log entry for each document with missing index key.
// checkHealthLog(primaryHealthlog, logQueries.missingIndexKeysQuery, nDocs);
// Check that the primary does not have other error/warning entries.
// checkHealthLog(primaryHealthlog, logQueries.allErrorsOrWarningsQuery, nDocs);

// Check that the start, batch, and stop entries are warning logs since the node skips dbcheck
// during initial sync.
const initialSyncNodeHealthLog = initialSyncNode.getDB("local").system.healthlog;
// checkHealthLog(initialSyncNodeHealthLog, logQueries.duringInitialSyncQuery, 3);
checkHealthLog(initialSyncNodeHealthLog, logQueries.duringInitialSyncStart, 1);
checkHealthLog(initialSyncNodeHealthLog, logQueries.duringInitialSyncBatch, 1);
// Check that the initial sync node does not have other info or error/warning entries.
// checkHealthLog(initialSyncNodeHealthLog, logQueries.infoBatchQuery, 0);
// checkHealthLog(initialSyncNodeHealthLog, logQueries.allErrorsOrWarningsQuery, 3);

replSet.stopSet();
