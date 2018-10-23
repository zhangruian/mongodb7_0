/**
 * Test error cases when calling prepare on a committed transaction.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
    "use strict";

    const dbName = "test";
    const collName = "prepare_committed_transaction";
    const testDB = db.getSiblingDB(dbName);
    const testColl = testDB.getCollection(collName);

    testColl.drop({writeConcern: {w: "majority"}});
    assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

    const session = db.getMongo().startSession({causalConsistency: false});
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);

    const doc = {x: 1};

    session.startTransaction();
    assert.commandWorked(sessionColl.insert(doc));
    session.commitTransaction();

    const txnNumber = NumberLong(session.getTxnNumber_forTesting());

    // Call prepare on committed transaction.
    jsTestLog("Test that calling prepare on a committed transaction fails.");
    assert.commandFailedWithCode(sessionDB.adminCommand({
        prepareTransaction: 1,
        coordinatorId: "dummy",
        txnNumber: txnNumber,
        autocommit: false
    }),
                                 ErrorCodes.TransactionCommitted);

    jsTestLog("Test the error precedence when calling prepare on a committed transaction but not " +
              "providing txnNumber to prepareTransaction.");
    assert.commandFailedWithCode(
        sessionDB.adminCommand({prepareTransaction: 1, coordinatorId: "dummy", autocommit: false}),
        ErrorCodes.InvalidOptions);

    jsTestLog("Test the error precedence when calling prepare on a committed transaction but not " +
              "providing autocommit to prepareTransaction.");
    assert.commandFailedWithCode(
        sessionDB.adminCommand(
            {prepareTransaction: 1, coordinatorId: "dummy", txnNumber: txnNumber}),
        ErrorCodes.InvalidOptions);

    jsTestLog("Test the error precedence when calling prepare on a committed transaction and " +
              "providing startTransaction to prepareTransaction.");
    assert.commandFailedWithCode(sessionDB.adminCommand({
        prepareTransaction: 1,
        coordinatorId: "dummy",
        txnNumber: txnNumber,
        autocommit: false,
        startTransaction: true
    }),
                                 ErrorCodes.ConflictingOperationInProgress);

    // Call commit on committed transaction without shell helper.
    jsTestLog("Test that calling commit with invalid fields on a committed transaction fails.");
    assert.commandFailedWithCode(
        sessionDB.adminCommand(
            {commitTransaction: 1, invalidField: 1, txnNumber: txnNumber, autocommit: false}),
        40415 /* IDL unknown field error */);

    // Call abort on committed transaction without shell helper.
    jsTestLog("Test that calling abort on a committed transaction fails.");
    assert.commandFailedWithCode(
        sessionDB.adminCommand({abortTransaction: 1, txnNumber: txnNumber, autocommit: false}),
        ErrorCodes.TransactionCommitted);

    jsTestLog("Test that calling abort with invalid fields on a committed transaction fails.");
    assert.commandFailedWithCode(
        sessionDB.adminCommand(
            {abortTransaction: 1, invalidField: 1, txnNumber: txnNumber, autocommit: false}),
        ErrorCodes.TransactionCommitted);

    session.endSession();
}());
