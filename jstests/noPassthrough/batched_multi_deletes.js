/**
 * Validate basic batched multi-deletion functionality.
 *
 * @tags: [
 * ]
 */

(function() {
"use strict";
load("jstests/libs/analyze_plan.js");

const conn = MongoRunner.runMongod();

const db = conn.getDB("__internalBatchedDeletesTesting");
const coll = db.getCollection('Collection0');
const collName = coll.getName();
const ns = coll.getFullName();

const docsPerBatchDefault = 100;  // BatchedDeleteStageBatchParams::targetBatchDocs
const collCount =
    5017;  // Intentionally not a multiple of BatchedDeleteStageBatchParams::targetBatchDocs.

function validateDeletion(db, coll, docsPerBatch) {
    coll.drop();
    assert.commandWorked(
        coll.insertMany([...Array(collCount).keys()].map(x => ({_id: x, a: "a".repeat(1024)}))));

    const serverStatusBatchesBefore = db.serverStatus()['batchedDeletes']['batches'];
    const serverStatusDocsBefore = db.serverStatus()['batchedDeletes']['docs'];

    assert.eq(collCount, coll.find().itcount());
    assert.commandWorked(coll.deleteMany({_id: {$gte: 0}}));
    assert.eq(0, coll.find().itcount());

    const serverStatusBatchesAfter = db.serverStatus()['batchedDeletes']['batches'];
    const serverStatusDocsAfter = db.serverStatus()['batchedDeletes']['docs'];
    const serverStatusBatchesExpected =
        serverStatusBatchesBefore + Math.ceil(collCount / docsPerBatch);
    const serverStatusDocsExpected = serverStatusDocsBefore + collCount;
    assert.eq(serverStatusBatchesAfter, serverStatusBatchesExpected);
    assert.eq(serverStatusDocsAfter, serverStatusDocsExpected);
}

coll.drop();
assert.commandWorked(
    coll.insertMany([...Array(collCount).keys()].map(x => ({_id: x, a: "a".repeat(1024)}))));

assert.commandWorked(db.adminCommand({setParameter: 1, internalBatchUserMultiDeletesForTest: 1}));

// Batched multi-deletion is only available for multi:true deletes.
assert.commandFailedWithCode(
    db.runCommand({delete: collName, deletes: [{q: {_id: {$gte: 0}}, limit: 1}]}), 6303800);

// Explain plan and executionStats.
{
    const expl = db.runCommand({
        explain: {delete: collName, deletes: [{q: {_id: {$gte: 0}}, limit: 0}]},
        verbosity: "executionStats"
    });
    assert.commandWorked(expl);

    assert(getPlanStage(expl, "BATCHED_DELETE"));
    assert.eq(0, expl.executionStats.nReturned);
    assert.eq(collCount, expl.executionStats.totalDocsExamined);
    assert.eq(collCount, expl.executionStats.totalKeysExamined);
    assert.eq(0, expl.executionStats.executionStages.nReturned);
    assert.eq(1, expl.executionStats.executionStages.isEOF);
}

// Actual deletion.
for (const docsPerBatch of [10, docsPerBatchDefault]) {
    assert.commandWorked(
        db.adminCommand({setParameter: 1, batchedDeletesTargetBatchDocs: docsPerBatch}));
    validateDeletion(db, coll, docsPerBatch);
}

MongoRunner.stopMongod(conn);
})();
