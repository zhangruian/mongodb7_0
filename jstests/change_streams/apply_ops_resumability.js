// Tests that a change stream will correctly unwind applyOps entries generated by a transaction, and
// that we can resume from any point within the transaction.
// @tags: [uses_transactions, requires_snapshot_read, requires_majority_read_concern]

(function() {
"use strict";

load("jstests/libs/auto_retry_transaction_in_sharding.js");  // For withTxnAndAutoRetryOnMongos.
load("jstests/libs/change_stream_util.js");                  // For ChangeStreamTest.
load("jstests/libs/collection_drop_recreate.js");            // For assert[Drop|Create]Collection.
load("jstests/libs/fixture_helpers.js");                     // For FixtureHelpers.isMongos.

const coll = assertDropAndRecreateCollection(db, "change_stream_apply_ops");
const otherCollName = "change_stream_apply_ops_2";
assertDropAndRecreateCollection(db, otherCollName);

const otherDbName = "change_stream_apply_ops_db";
const otherDbCollName = "someColl";
assertDropAndRecreateCollection(db.getSiblingDB(otherDbName), otherDbCollName);

let cst = new ChangeStreamTest(db);
let changeStream = cst.startWatchingChanges(
    {pipeline: [{$changeStream: {}}, {$project: {"lsid.uid": 0}}], collection: coll});

// Record the clusterTime at the outset of the test, before any writes are performed.
const testStartTime = db.hello().$clusterTime.clusterTime;

// Do an insert outside of a transaction.
assert.commandWorked(coll.insert({_id: 0, a: 123}));

// Open a session, and perform two writes within a transaction.
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
    // single-collection change stream.
    assert.commandWorked(sessionOtherColl.insert({_id: 111, a: "Doc on other collection"}));

    // One insert on a collection in a different database. This should be skipped by the single
    // collection and single-db changestreams.
    assert.commandWorked(sessionOtherDbColl.insert({_id: 222, a: "Doc on other DB"}));

    assert.commandWorked(sessionColl.updateOne({_id: 1}, {$inc: {a: 1}}));
}, txnOptions);

// Now insert another document, not part of a transaction.
assert.commandWorked(coll.insert({_id: 3, a: 123}));

// Drop the collection. This will trigger a "drop" event, which in the case of the single-collection
// stream will be followed by an "invalidate".
assert.commandWorked(db.runCommand({drop: coll.getName()}));

// Define the set of all changes expected to be generated by the operations above.
let expectedChanges = [
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
        documentKey: {_id: 111},
        fullDocument: {_id: 111, a: "Doc on other collection"},
        ns: {db: db.getName(), coll: otherCollName},
        operationType: "insert",
        lsid: session.getSessionId(),
        txnNumber: session.getTxnNumber_forTesting(),
    },
    {
        documentKey: {_id: 222},
        fullDocument: {_id: 222, a: "Doc on other DB"},
        ns: {db: otherDbName, coll: otherDbCollName},
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
        documentKey: {_id: 3},
        fullDocument: {_id: 3, a: 123},
        ns: {db: db.getName(), coll: coll.getName()},
        operationType: "insert",
    },
    {operationType: "drop", ns: {db: db.getName(), coll: coll.getName()}}
];

// Validate that we observe all expected changes in the stream, and replace the'expectedChanges'
// list with the changes returned by ChangeStreamTest. These will include the _id resume tokens for
// each change, so subsequent tests will be able to resume from any point.
(function validateExpectedChangesAndPopulateResumeTokens() {
    const wholeClusterCST = new ChangeStreamTest(db.getSiblingDB("admin"));
    const wholeClusterCursor = wholeClusterCST.startWatchingChanges({
        pipeline: [
            {$changeStream: {startAtOperationTime: testStartTime, allChangesForCluster: true}},
            {$project: {"lsid.uid": 0}}
        ],
        collection: 1
    });
    // If we are running in a sharded passthrough, then this may have been a multi-shard txn. Change
    // streams will interleave the txn events from across the shards in (clusterTime, txnOpIndex)
    // order, and so may not reflect the ordering of writes in the test. The ordering of events is
    // important for later tests, so if we are running on mongoS we verify that exactly the expected
    // set of events are observed, and then we adopt the order in which they were returned.
    if (FixtureHelpers.isMongos(db)) {
        expectedChanges = wholeClusterCST.assertNextChangesEqualUnordered(
            {cursor: wholeClusterCursor, expectedChanges: expectedChanges});
    } else {
        expectedChanges = wholeClusterCST.assertNextChangesEqual(
            {cursor: wholeClusterCursor, expectedChanges: expectedChanges});
    }
})();

// Helper function to find the first non-transaction event and the first two transaction events in
// the given list of change stream events.
function findMilestoneEvents(eventList) {
    const nonTxnIdx = eventList.findIndex(event => !event.lsid),
          firstTxnIdx = eventList.findIndex(event => event.lsid),
          secondTxnIdx = eventList.findIndex((event, idx) => (idx > firstTxnIdx && event.lsid));
    // Return the array indices of each event, and the events themselves.
    return [
        nonTxnIdx,
        firstTxnIdx,
        secondTxnIdx,
        eventList[nonTxnIdx],
        eventList[firstTxnIdx],
        eventList[secondTxnIdx]
    ];
}

//
// Test behavior of single-collection change streams with apply ops.
//

// Filter out any events that aren't on the main test collection namespace.
const expectedSingleCollChanges = expectedChanges.filter(
    event => (event.ns.db === db.getName() && event.ns.coll === coll.getName()));

// Verify that the stream returns the expected sequence of changes.
cst.assertNextChangesEqual({cursor: changeStream, expectedChanges: expectedSingleCollChanges});

// Obtain the first non-transaction change and the first two in-transaction changes.
let [nonTxnIdx, firstTxnIdx, secondTxnIdx, nonTxnChange, firstTxnChange, secondTxnChange] =
    findMilestoneEvents(expectedSingleCollChanges);

// Resume after the first non-transaction change. Be sure we see the documents from the
// transaction again.
changeStream = cst.startWatchingChanges({
    pipeline: [{$changeStream: {resumeAfter: nonTxnChange._id}}, {$project: {"lsid.uid": 0}}],
    collection: coll
});
cst.assertNextChangesEqual(
    {cursor: changeStream, expectedChanges: expectedSingleCollChanges.slice(nonTxnIdx + 1)});

// Resume after the first transaction change. Be sure we see the second change again.
changeStream = cst.startWatchingChanges({
    pipeline: [{$changeStream: {resumeAfter: firstTxnChange._id}}, {$project: {"lsid.uid": 0}}],
    collection: coll
});
cst.assertNextChangesEqual(
    {cursor: changeStream, expectedChanges: expectedSingleCollChanges.slice(firstTxnIdx + 1)});

// Try starting another change stream from the second change caused by the transaction. Verify
// that we can see the insert performed after the transaction was committed.
let otherCursor = cst.startWatchingChanges({
    pipeline: [{$changeStream: {resumeAfter: secondTxnChange._id}}, {$project: {"lsid.uid": 0}}],
    collection: coll,
    doNotModifyInPassthroughs: true  // A collection drop only invalidates single-collection
                                     // change streams.
});
cst.assertNextChangesEqual(
    {cursor: otherCursor, expectedChanges: expectedSingleCollChanges.slice(secondTxnIdx + 1)});

// Verify that the next event observed by the stream is an invalidate following the collection drop.
const invalidateEvent = cst.getOneChange(otherCursor, true);
assert.eq(invalidateEvent.operationType, "invalidate");

//
// Test behavior of whole-db change streams with apply ops.
//

// In a sharded cluster, whole-db-or-cluster streams will see a collection drop from each shard.
for (let i = 1; i < FixtureHelpers.numberOfShardsForCollection(coll); ++i) {
    expectedChanges.push({operationType: "drop", ns: {db: db.getName(), coll: coll.getName()}});
}

// Filter out any events that aren't on the main test database.
const expectedSingleDBChanges = expectedChanges.filter(event => (event.ns.db === db.getName()));

// Obtain the first non-transaction change and the first two in-transaction changes.
[nonTxnIdx, firstTxnIdx, secondTxnIdx, nonTxnChange, firstTxnChange, secondTxnChange] =
    findMilestoneEvents(expectedSingleDBChanges);

// Verify that a whole-db stream can be resumed from the middle of the transaction, and that it
// will see all subsequent changes including the insert on the other collection but NOT the
// changes on the other DB.
changeStream = cst.startWatchingChanges({
    pipeline: [{$changeStream: {resumeAfter: secondTxnChange._id}}, {$project: {"lsid.uid": 0}}],
    collection: 1,
});
cst.assertNextChangesEqual(
    {cursor: changeStream, expectedChanges: expectedSingleDBChanges.slice(secondTxnIdx + 1)});

//
// Test behavior of whole-cluster change streams with apply ops.
//

// Obtain the first non-transaction change and the first two in-transaction changes.
[nonTxnIdx, firstTxnIdx, secondTxnIdx, nonTxnChange, firstTxnChange, secondTxnChange] =
    findMilestoneEvents(expectedChanges);

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
cst.assertNextChangesEqual(
    {cursor: changeStream, expectedChanges: expectedChanges.slice(secondTxnIdx + 1)});

cst.cleanUp();
}());
