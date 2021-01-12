// Test that commands other than retryable writes may not use txnNumber outside transactions.
// @tags: [
//   uses_map_reduce_with_temp_collections,
// ]
(function() {
"use strict";

const isMongos = assert.commandWorked(db.runCommand("hello")).msg === "isdbgrid";

const session = db.getMongo().startSession();
const sessionDb = session.getDatabase("admin");

const nonRetryableWriteCommands = [
    // Commands that are allowed in transactions.
    {aggregate: 1, pipeline: [], cursor: {}},
    {commitTransaction: 1},
    {distinct: "c"},
    {find: "c"},
    {getMore: NumberLong(1), collection: "c"},
    {killCursors: 'system.users', cursors: []},
    // A selection of commands that are not allowed in transactions.
    {count: 1},
    {explain: {find: "c"}},
    {filemd5: 1},
    {isMaster: 1},
    {buildInfo: 1},
    {ping: 1},
    {listCommands: 1},
    {create: "c"},
    {drop: "c"},
    {createIndexes: "c", indexes: []},
    {mapReduce: "c"}
];

const nonRetryableWriteCommandsMongodOnly = [
    // Commands that are allowed in transactions.
    {coordinateCommitTransaction: 1, participants: []},
    {prepareTransaction: 1},
    // A selection of commands that are not allowed in transactions.
    {applyOps: 1}
];

nonRetryableWriteCommands.forEach(function(command) {
    jsTest.log("Testing command: " + tojson(command));
    assert.commandFailedWithCode(
        sessionDb.runCommand(Object.assign({}, command, {txnNumber: NumberLong(0)})),
        [50768, 50889]);
});

if (!isMongos) {
    nonRetryableWriteCommandsMongodOnly.forEach(function(command) {
        jsTest.log("Testing command: " + tojson(command));
        assert.commandFailedWithCode(
            sessionDb.runCommand(Object.assign({}, command, {txnNumber: NumberLong(0)})),
            [50768, 50889]);
    });
}

session.endSession();
}());
