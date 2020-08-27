/**
 * Tests that resumable index builds complete properly after being interrupted for rollback during
 * the collection scan phase.
 *
 * TODO (SERVER-49075): Move this test to the replica_sets suite once it is enabled on the resumable
 * index builds variant.
 *
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load('jstests/replsets/libs/rollback_resumable_index_build.js');

const dbName = "test";
const rollbackStartFailPointName = "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion";
const insertsToBeRolledBack = [{a: 6}, {a: 7}];

const rollbackTest = new RollbackTest(jsTestName());
const coll = rollbackTest.getPrimary().getDB(dbName).getCollection(jsTestName());

assert.commandWorked(coll.insert([{a: 1}, {a: 2}, {a: 3}, {a: 4}, {a: 5}]));

// TODO (SERVER-49774): Enable these test cases once resumable index builds are resilient to the
// node going into rollback during the collection scan phase.
if (true) {
    rollbackTest.stop();
    return;
}

// Rollback to earlier in the collection scan phase.
RollbackResumableIndexBuildTest.run(rollbackTest,
                                    dbName,
                                    coll.getName(),
                                    {a: 1},
                                    rollbackStartFailPointName,
                                    {fieldsToMatch: {a: 4}},
                                    "hangIndexBuildDuringCollectionScanPhaseAfterInsertion",
                                    {fieldsToMatch: {a: 2}},
                                    insertsToBeRolledBack);

// Rollback to before the index begins to be built.
RollbackResumableIndexBuildTest.run(rollbackTest,
                                    dbName,
                                    coll.getName(),
                                    {a: 1},
                                    rollbackStartFailPointName,
                                    {fieldsToMatch: {a: 2}},
                                    "hangAfterSettingUpIndexBuildUnlocked",
                                    {});

rollbackTest.stop();
})();
