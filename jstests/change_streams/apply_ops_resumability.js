// Tests that a change stream will correctly unwind applyOps entries generated by a transaction.
// @tags: [uses_transactions]

(function() {
"use strict";

load("jstests/libs/change_stream_util.js");        // For ChangeStreamTest.
load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.

const coll = assertDropAndRecreateCollection(db, "change_stream_apply_ops");
const otherCollName = "change_stream_apply_ops_2";
assertDropAndRecreateCollection(db, otherCollName);

const otherDbName = "change_stream_apply_ops_db";
const otherDbCollName = "someColl";
assertDropAndRecreateCollection(db.getSiblingDB(otherDbName), otherDbCollName);

let cst = new ChangeStreamTest(db);
let changeStream = cst.startWatchingChanges(
    {pipeline: [{$changeStream: {}}, {$project: {"lsid.uid": 0}}], collection: coll});

// Do an insert outside of a transaction.
assert.commandWorked(coll.insert({_id: 0, a: 123}));

// Open a session, and perform two writes within a transaction.
const sessionOptions = {
    causalConsistency: false
};
const session = db.getMongo().startSession(sessionOptions);
const sessionDb = session.getDatabase(db.getName());
const sessionColl = sessionDb[coll.getName()];

session.startTransaction({readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}});
assert.commandWorked(sessionColl.insert({_id: 1, a: 0}));
assert.commandWorked(sessionColl.insert({_id: 2, a: 0}));

// One insert on a collection that we're not watching. This should be skipped by the
// single-collection change stream.
assert.commandWorked(sessionDb[otherCollName].insert({_id: 111, a: "Doc on other collection"}));

// One insert on a collection in a different database. This should be skipped by the single
// collection and single-db changestreams.
assert.commandWorked(
    session.getDatabase(otherDbName)[otherDbCollName].insert({_id: 222, a: "Doc on other DB"}));

assert.commandWorked(sessionColl.updateOne({_id: 1}, {$inc: {a: 1}}));

assert.commandWorked(session.commitTransaction_forTesting());

// Now insert another document, not part of a transaction.
assert.commandWorked(coll.insert({_id: 3, a: 123}));

// Define the set of changes expected for the single-collection case per the operations above.
const expectedChanges = [
    {
        documentKey: {_id: 0},
        fullDocument: {_id: 0, a: 123},
        ns: {db: db.getName(), coll: coll.getName()},
        operationType: "insert",
    },
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
        updateDescription: {removedFields: [], updatedFields: {a: 1}},
        lsid: session.getSessionId(),
        txnNumber: session.getTxnNumber_forTesting(),
    },
    {
        documentKey: {_id: 3},
        fullDocument: {_id: 3, a: 123},
        ns: {db: db.getName(), coll: coll.getName()},
        operationType: "insert",
    },
];

//
// Test behavior of single-collection change streams with apply ops.
//

// Verify that the stream returns the expected sequence of changes.
const changes =
    cst.assertNextChangesEqual({cursor: changeStream, expectedChanges: expectedChanges});

// Record the first (non-transaction) change and the first in-transaction change.
const nonTxnChange = changes[0], firstTxnChange = changes[1], secondTxnChange = changes[2];

// Resume after the first non-transaction change. Be sure we see the documents from the
// transaction again.
changeStream = cst.startWatchingChanges({
    pipeline: [{$changeStream: {resumeAfter: nonTxnChange._id}}, {$project: {"lsid.uid": 0}}],
    collection: coll
});
cst.assertNextChangesEqual({cursor: changeStream, expectedChanges: expectedChanges.slice(1)});

// Resume after the first transaction change. Be sure we see the second change again.
changeStream = cst.startWatchingChanges({
    pipeline: [{$changeStream: {resumeAfter: firstTxnChange._id}}, {$project: {"lsid.uid": 0}}],
    collection: coll
});
cst.assertNextChangesEqual({cursor: changeStream, expectedChanges: expectedChanges.slice(2)});

// Try starting another change stream from the _last_ change caused by the transaction. Verify
// that we can see the insert performed after the transaction was committed.
let otherCursor = cst.startWatchingChanges({
    pipeline: [{$changeStream: {resumeAfter: secondTxnChange._id}}, {$project: {"lsid.uid": 0}}],
    collection: coll,
    doNotModifyInPassthroughs: true  // A collection drop only invalidates single-collection
                                     // change streams.
});
cst.assertNextChangesEqual({cursor: otherCursor, expectedChanges: expectedChanges.slice(3)});

// Drop the collection. This will trigger a "drop" followed by an "invalidate" for the single
// collection change stream.
assert.commandWorked(db.runCommand({drop: coll.getName()}));
let change = cst.getOneChange(otherCursor);
assert.eq(change.operationType, "drop");
assert.eq(change.ns, {db: db.getName(), coll: coll.getName()});
change = cst.getOneChange(otherCursor, true);
assert.eq(change.operationType, "invalidate");

//
// Test behavior of whole-db change streams with apply ops.
//

// For a whole-db or whole-cluster change stream, the collection drop should return a single
// "drop" entry and not invalidate the stream.
expectedChanges.push({operationType: "drop", ns: {db: db.getName(), coll: coll.getName()}});

// Add an entry for the insert on db.otherColl into expectedChanges.
expectedChanges.splice(3, 0, {
    documentKey: {_id: 111},
    fullDocument: {_id: 111, a: "Doc on other collection"},
    ns: {db: db.getName(), coll: otherCollName},
    operationType: "insert",
    lsid: session.getSessionId(),
    txnNumber: session.getTxnNumber_forTesting(),
});

// Verify that a whole-db stream can be resumed from the middle of the transaction, and that it
// will see all subsequent changes including the insert on the other collection but NOT the
// changes on the other DB.
changeStream = cst.startWatchingChanges({
    pipeline: [{$changeStream: {resumeAfter: secondTxnChange._id}}, {$project: {"lsid.uid": 0}}],
    collection: 1,
});
cst.assertNextChangesEqual({cursor: changeStream, expectedChanges: expectedChanges.slice(3)});

// Add an entry for the insert on otherDb.otherDbColl into expectedChanges.
expectedChanges.splice(4, 0, {
    documentKey: {_id: 222},
    fullDocument: {_id: 222, a: "Doc on other DB"},
    ns: {db: otherDbName, coll: otherDbCollName},
    operationType: "insert",
    lsid: session.getSessionId(),
    txnNumber: session.getTxnNumber_forTesting(),
});

// Verify that a whole-cluster stream can be resumed from the middle of the transaction, and
// that it will see all subsequent changes including the insert on the other collection and the
// changes on the other DB.
cst = new ChangeStreamTest(db.getSiblingDB("admin"));
changeStream = cst.startWatchingChanges({
    pipeline: [
        {$changeStream: {resumeAfter: secondTxnChange._id, allChangesForCluster: true}},
        {$project: {"lsid.uid": 0}}
    ],
    collection: 1
});
cst.assertNextChangesEqual({cursor: changeStream, expectedChanges: expectedChanges.slice(3)});

cst.cleanUp();
}());
