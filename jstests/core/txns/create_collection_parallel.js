/* Tests parallel transactions with createCollections.
 *
 * @tags: [uses_transactions,
 *         # Creating collections inside multi-document transactions is supported only in v4.4
 *         # onwards.
 *         requires_fcv_44]
 */
(function() {
"use strict";

load("jstests/libs/create_collection_txn_helpers.js");

function runParallelCollectionCreateTest(explicitCreate) {
    const dbName = "test";
    const collName = "create_new_collection";
    const distinctCollName = collName + "_second";
    const session = db.getMongo().getDB(dbName).getMongo().startSession({causalConsistency: false});
    const secondSession =
        db.getMongo().getDB(dbName).getMongo().startSession({causalConsistency: false});

    let sessionDB = session.getDatabase("test");
    let secondSessionDB = secondSession.getDatabase("test");
    let sessionColl = sessionDB[collName];
    sessionColl.drop({writeConcern: {w: "majority"}});
    let distinctSessionColl = sessionDB[distinctCollName];
    distinctSessionColl.drop({writeConcern: {w: "majority"}});

    jsTest.log("Testing duplicate createCollections, second createCollection fails");

    session.startTransaction({writeConcern: {w: "majority"}});        // txn 1
    secondSession.startTransaction({writeConcern: {w: "majority"}});  // txn 2

    createCollAndCRUDInTxn(sessionDB, collName, explicitCreate);
    jsTest.log("Committing transaction 1");
    session.commitTransaction();
    assert.eq(sessionColl.find({}).itcount(), 1);

    assert.commandFailedWithCode(secondSessionDB.runCommand({create: collName}),
                                 ErrorCodes.NamespaceExists);

    assert.commandFailedWithCode(secondSession.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    sessionColl.drop({writeConcern: {w: "majority"}});

    jsTest.log(
        "Testing duplicate createCollections in parallel, both attempt to commit, second to commit fails");

    secondSession.startTransaction({writeConcern: {w: "majority"}});  // txn 2
    createCollAndCRUDInTxn(secondSession.getDatabase("test"), collName, explicitCreate);

    session.startTransaction({writeConcern: {w: "majority"}});  // txn 1
    createCollAndCRUDInTxn(sessionDB, collName, explicitCreate);

    jsTest.log("Committing transaction 2");
    secondSession.commitTransaction();
    jsTest.log("Committing transaction 1 (SHOULD FAIL)");
    assert.commandFailedWithCode(session.commitTransaction_forTesting(), ErrorCodes.WriteConflict);
    assert.eq(sessionColl.find({}).itcount(), 1);

    sessionColl.drop({writeConcern: {w: "majority"}});
    distinctSessionColl.drop({writeConcern: {w: "majority"}});

    jsTest.log("Testing distinct createCollections in parallel, both successfully commit.");
    session.startTransaction({writeConcern: {w: "majority"}});  // txn 1
    createCollAndCRUDInTxn(sessionDB, collName, explicitCreate);

    secondSession.startTransaction({writeConcern: {w: "majority"}});  // txn 2
    createCollAndCRUDInTxn(secondSessionDB, distinctCollName, explicitCreate);

    session.commitTransaction();
    secondSession.commitTransaction();

    secondSession.endSession();
    session.endSession();
}
runParallelCollectionCreateTest(true /*explicitCreate*/);
runParallelCollectionCreateTest(false /*explicitCreate*/);
}());
