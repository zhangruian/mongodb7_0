/**
 * Tests that a change stream will correctly unwind applyOps entries generated by a transaction.
 * @tags: [
 *   uses_transactions,
 *   requires_fcv_61, # Pre-6.1 builds do not emit change stream events for atomic applyOps.
 *   requires_majority_read_concern,
 *   requires_snapshot_read,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/auto_retry_transaction_in_sharding.js");  // For withTxnAndAutoRetryOnMongos.
load("jstests/libs/change_stream_util.js");                  // For ChangeStreamTest.
load("jstests/libs/collection_drop_recreate.js");            // For assert[Drop|Create]Collection.
load("jstests/libs/fixture_helpers.js");                     // For FixtureHelpers.isMongos.

const otherCollName = "change_stream_apply_ops_2";
const coll = assertDropAndRecreateCollection(db, "change_stream_apply_ops");
assertDropAndRecreateCollection(db, otherCollName);

const otherDbName = "change_stream_apply_ops_db";
const otherDbCollName = "someColl";
assertDropAndRecreateCollection(db.getSiblingDB(otherDbName), otherDbCollName);

// Insert a document that gets deleted as part of the transaction.
const kDeletedDocumentId = 0;
const insertRes = assert.commandWorked(coll.runCommand("insert", {
    documents: [{_id: kDeletedDocumentId, a: "I was here before the transaction"}],
    writeConcern: {w: "majority"}
}));

// Record the clusterTime of the insert, and increment it to give the test start time.
const testStartTime = insertRes.$clusterTime.clusterTime;
testStartTime.i++;

let cst = new ChangeStreamTest(db);
let changeStream = cst.startWatchingChanges({
    pipeline: [{$changeStream: {}}, {$project: {"lsid.uid": 0}}],
    collection: coll,
    doNotModifyInPassthroughs:
        true  // A collection drop only invalidates single-collection change streams.
});

const sessionOptions = {
    causalConsistency: false
};
const txnOptions = {
    readConcern: {level: "snapshot"},
    writeConcern: {w: "majority"}
};

const session = db.getMongo().startSession(sessionOptions);

// Create these variables before starting the transaction. In sharded passthroughs, accessing
// db[collname] may attempt to implicitly shard the collection, which is not allowed in a txn.
const sessionDb = session.getDatabase(db.getName());
const sessionColl = sessionDb[coll.getName()];
const sessionOtherColl = sessionDb[otherCollName];
const sessionOtherDbColl = session.getDatabase(otherDbName)[otherDbCollName];

withTxnAndAutoRetryOnMongos(session, () => {
    // Two inserts on the main test collection.
    assert.commandWorked(sessionColl.insert({_id: 1, a: 0}));
    assert.commandWorked(sessionColl.insert({_id: 2, a: 0}));

    // One insert on a collection that we're not watching. This should be skipped by the
    // single-collection changestream.
    assert.commandWorked(sessionOtherColl.insert({_id: 111, a: "Doc on other collection"}));

    // One insert on a collection in a different database. This should be skipped by the single
    // collection and single-db changestreams.
    assert.commandWorked(sessionOtherDbColl.insert({_id: 222, a: "Doc on other DB"}));

    assert.commandWorked(sessionColl.updateOne({_id: 1}, {$inc: {a: 1}}));

    assert.commandWorked(sessionColl.deleteOne({_id: kDeletedDocumentId}));
}, txnOptions);

// Do applyOps on the collection that we care about. This is an "external" applyOps, though (not run
// as part of a transaction). This checks that although this applyOps doesn't have an 'lsid' and
// 'txnNumber', the field gets unwound and a change stream event is emitted. Skip if running in a
// sharded passthrough, since the applyOps command does not exist on mongoS.
if (!FixtureHelpers.isMongos(db)) {
    assert.commandWorked(db.runCommand({
        applyOps: [
            {op: "i", ns: coll.getFullName(), o: {_id: 3, a: "insert from atomic applyOps"}},
        ]
    }));
}

// Drop the collection. This will trigger an "invalidate" event at the end of the stream.
assert.commandWorked(db.runCommand({drop: coll.getName()}));

// Define the set of changes expected for the single-collection case per the operations above.
let expectedChanges = [
    {
        documentKey: {_id: 1},
        fullDocument: {_id: 1, a: 0},
        ns: {db: db.getName(), coll: coll.getName()},
        operationType: "insert",
        lsid: session.getSessionId(),
        txnNumber: session.getTxnNumber_forTesting(),
    },
    {
        documentKey: {_id: 2},
        fullDocument: {_id: 2, a: 0},
        ns: {db: db.getName(), coll: coll.getName()},
        operationType: "insert",
        lsid: session.getSessionId(),
        txnNumber: session.getTxnNumber_forTesting(),
    },
    {
        documentKey: {_id: 1},
        ns: {db: db.getName(), coll: coll.getName()},
        operationType: "update",
        updateDescription: {removedFields: [], updatedFields: {a: 1}, truncatedArrays: []},
        lsid: session.getSessionId(),
        txnNumber: session.getTxnNumber_forTesting(),
    },
    {
        documentKey: {_id: kDeletedDocumentId},
        ns: {db: db.getName(), coll: coll.getName()},
        operationType: "delete",
        lsid: session.getSessionId(),
        txnNumber: session.getTxnNumber_forTesting(),
    }
];

if (!FixtureHelpers.isMongos(db)) {
    expectedChanges.push({
        documentKey: {_id: 3},
        fullDocument: {_id: 3, a: "insert from atomic applyOps"},
        ns: {db: db.getName(), coll: coll.getName()},
        operationType: "insert",
    });
}
expectedChanges.push({
    operationType: "drop",
    ns: {db: db.getName(), coll: coll.getName()},
});

// If we are running in a sharded passthrough, then this may have been a multi-shard transaction.
// Change streams will interleave the txn events from across the shards in (clusterTime, txnOpIndex)
// order, and so may not reflect the ordering of writes in the test. We thus verify that exactly the
// expected set of events are observed, but we relax the ordering requirements.
function assertNextChangesEqual({cursor, expectedChanges, expectInvalidate}) {
    const assertEqualFunc = FixtureHelpers.isMongos(db) ? cst.assertNextChangesEqualUnordered
                                                        : cst.assertNextChangesEqual;
    return assertEqualFunc(
        {cursor: cursor, expectedChanges: expectedChanges, expectInvalidate: expectInvalidate});
}

//
// Test behavior of single-collection change streams with apply ops.
//

// Verify that the stream returns the expected sequence of changes.
const changes = assertNextChangesEqual({cursor: changeStream, expectedChanges: expectedChanges});

// Single collection change stream should also be invalidated by the drop.
assertNextChangesEqual({
    cursor: changeStream,
    expectedChanges: [{operationType: "invalidate"}],
    expectInvalidate: true
});

//
// Test behavior of whole-db change streams with apply ops.
//

// In a sharded cluster, whole-db-or-cluster streams will see a collection drop from each shard.
for (let i = 1; i < FixtureHelpers.numberOfShardsForCollection(coll); ++i) {
    expectedChanges.push({operationType: "drop", ns: {db: db.getName(), coll: coll.getName()}});
}

// Add an entry for the insert on db.otherColl into expectedChanges.
expectedChanges.splice(2, 0, {
    documentKey: {_id: 111},
    fullDocument: {_id: 111, a: "Doc on other collection"},
    ns: {db: db.getName(), coll: otherCollName},
    operationType: "insert",
    lsid: session.getSessionId(),
    txnNumber: session.getTxnNumber_forTesting(),
});

// Verify that a whole-db stream returns the expected sequence of changes, including the insert
// on the other collection but NOT the changes on the other DB or the manual applyOps.
changeStream = cst.startWatchingChanges({
    pipeline: [{$changeStream: {startAtOperationTime: testStartTime}}, {$project: {"lsid.uid": 0}}],
    collection: 1
});
assertNextChangesEqual({cursor: changeStream, expectedChanges: expectedChanges});

//
// Test behavior of whole-cluster change streams with apply ops.
//

// Add an entry for the insert on otherDb.otherDbColl into expectedChanges.
expectedChanges.splice(3, 0, {
    documentKey: {_id: 222},
    fullDocument: {_id: 222, a: "Doc on other DB"},
    ns: {db: otherDbName, coll: otherDbCollName},
    operationType: "insert",
    lsid: session.getSessionId(),
    txnNumber: session.getTxnNumber_forTesting(),
});

// Verify that a whole-cluster stream returns the expected sequence of changes, including the
// inserts on the other collection and the other database, but NOT the manual applyOps.
cst = new ChangeStreamTest(db.getSiblingDB("admin"));
changeStream = cst.startWatchingChanges({
    pipeline: [
        {$changeStream: {startAtOperationTime: testStartTime, allChangesForCluster: true}},
        {$project: {"lsid.uid": 0}}
    ],
    collection: 1
});
assertNextChangesEqual({cursor: changeStream, expectedChanges: expectedChanges});

cst.cleanUp();
}());
