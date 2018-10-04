/**
 * Tests that an idle cursor will appear in the $currentOp output if the idleCursors option is
 * set to true.
 *
 * @tags: [assumes_read_concern_unchanged, requires_capped]
 */

(function() {
    "use strict";
    const coll = db.jstests_currentop;
    // Avoiding using the shell helper to avoid the implicit collection recreation.
    db.runCommand({drop: coll.getName()});
    assert.commandWorked(db.createCollection(coll.getName(), {capped: true, size: 1000}));
    for (let i = 0; i < 5; ++i) {
        assert.commandWorked(coll.insert({"val": i}));
    }
    /**
     * runTest creates a new collection called jstests_currentop and then runs the provided find
     * query. It calls $currentOp and does some basic assertions to make sure idleCursors is
     * behaving as intended in each case.
     * findFunc: A function that runs a find query. Is expected to return a cursorID.
     *  Arbitrary code can be run in findFunc as long as it returns a cursorID.
     * assertFunc: A function that runs assertions against the results of the $currentOp.
     * Takes the following arguments
     *  'findOut': The cursorID returned from findFunc.
     *  'result': The results from running $currenpOp as an array of JSON objects.
     * Arbitrary code can be run in assertFunc, and there is no return value needed.
     */
    function runTest({findFunc, assertFunc}) {
        const adminDB = db.getSiblingDB("admin");
        const findOut = findFunc();
        const result =
            adminDB
                .aggregate([
                    {$currentOp: {localOps: true, allUsers: false, idleCursors: true}},
                    {$match: {$and: [{type: "idleCursor"}, {"cursor.cursorId": findOut}]}}
                ])
                .toArray();
        assert.eq(result[0].cursor.ns, coll.getFullName(), result);
        assert.eq(result[0].cursor.originatingCommand.find, coll.getName(), result);
        assertFunc(findOut, result);
        const noIdle =
            adminDB
                .aggregate([
                    {$currentOp: {allUsers: false, idleCursors: false}},
                    {$match: {$and: [{type: "idleCursor"}, {"cursor.cursorId": findOut}]}}
                ])
                .toArray();
        assert.eq(noIdle.length, 0, tojson(noIdle));
        const noFlag =
            adminDB.aggregate([{$currentOp: {allUsers: false}}, {$match: {type: "idleCursor"}}])
                .toArray();

        assert.eq(noIdle.length, 0, tojson(noFlag));
    }

    runTest({
        findFunc: function() {
            return assert.commandWorked(db.runCommand({find: "jstests_currentop", batchSize: 2}))
                .cursor.id;
        },
        assertFunc: function(cursorId, result) {
            assert.eq(result.length, 1, result);
            const idleCursor = result[0].cursor;
            assert.eq(idleCursor.nDocsReturned, 2, result);
            assert.eq(idleCursor.nBatchesReturned, 1, result);
            assert.eq(idleCursor.tailable, false, result);
            assert.eq(idleCursor.awaitData, false, result);
            assert.eq(idleCursor.noCursorTimeout, false, result);
            assert.eq(idleCursor.originatingCommand.batchSize, 2, result);
            assert.lte(idleCursor.createdDate, idleCursor.lastAccessDate, result);
        }
    });
    runTest({
        findFunc: function() {
            return assert
                .commandWorked(db.runCommand({
                    find: "jstests_currentop",
                    batchSize: 2,
                    tailable: true,
                    awaitData: true,
                    noCursorTimeout: true
                }))
                .cursor.id;
        },
        assertFunc: function(cursorId, result) {

            assert.eq(result.length, 1, result);
            const idleCursor = result[0].cursor;
            assert.eq(idleCursor.tailable, true, result);
            assert.eq(idleCursor.awaitData, true, result);
            assert.eq(idleCursor.noCursorTimeout, true, result);
            assert.eq(idleCursor.originatingCommand.batchSize, 2, result);
        }
    });
    runTest({
        findFunc: function() {
            return assert.commandWorked(db.runCommand({find: "jstests_currentop", batchSize: 2}))
                .cursor.id;
        },
        assertFunc: function(cursorId, result) {
            const secondCursor =
                assert.commandWorked(db.runCommand({find: "jstests_currentop", batchSize: 2}));
            const adminDB = db.getSiblingDB("admin");
            const secondResult =
                adminDB
                    .aggregate([
                        {$currentOp: {localOps: true, allUsers: false, idleCursors: true}},
                        {
                          $match: {
                              $and: [
                                  {type: "idleCursor"},
                                  {"cursor.cursorId": secondCursor.cursor.id}
                              ]
                          }
                        }
                    ])
                    .toArray();
            assert.lt(result[0].cursor.createdDate, secondResult[0].cursor.createdDate, function() {
                return tojson(result) + tojson(secondResult);
            });

        }
    });

    runTest({
        findFunc: function() {
            return assert
                .commandWorked(
                    db.runCommand({find: "jstests_currentop", batchSize: 4, noCursorTimeout: true}))
                .cursor.id;
        },
        assertFunc: function(cursorId, result) {
            const idleCursor = result[0].cursor;
            assert.eq(result.length, 1, result);
            assert.eq(idleCursor.nDocsReturned, 4, result);
            assert.eq(idleCursor.nBatchesReturned, 1, result);
            assert.eq(idleCursor.noCursorTimeout, true, result);
            assert.eq(idleCursor.originatingCommand.batchSize, 4, result);
        }
    });

    runTest({
        findFunc: function() {
            return assert.commandWorked(db.runCommand({find: "jstests_currentop", batchSize: 2}))
                .cursor.id;
        },
        assertFunc: function(cursorId, result) {
            const adminDB = db.getSiblingDB("admin");
            const originalAccess = result[0].cursor.lastAccessDate;
            assert.commandWorked(
                db.runCommand({getMore: cursorId, collection: "jstests_currentop", batchSize: 2}));
            result =
                adminDB
                    .aggregate([
                        {$currentOp: {localOps: true, allUsers: false, idleCursors: true}},
                        {$match: {$and: [{type: "idleCursor"}, {"cursor.cursorId": cursorId}]}}
                    ])
                    .toArray();
            const idleCursor = result[0].cursor;
            assert.eq(idleCursor.nDocsReturned, 4, result);
            assert.eq(idleCursor.nBatchesReturned, 2, result);
            assert.eq(idleCursor.originatingCommand.batchSize, 2, result);
            assert.lt(idleCursor.createdDate, idleCursor.lastAccessDate, result);
            assert.lt(originalAccess, idleCursor.lastAccessDate, result);
        }
    });

})();
