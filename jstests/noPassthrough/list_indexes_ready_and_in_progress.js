/**
 * Tests that the listIndexes command's default is to only show ready indexes; and that
 * the 'includeIndexBuilds' flag can be set to include indexes that are still building
 * along with the ready indexes.
 */
(function() {
    "use strict";

    load("jstests/noPassthrough/libs/index_build.js");

    const conn = MongoRunner.runMongod();
    assert.neq(null, conn, "mongod was unable to start up");

    const testDB = conn.getDB("test");
    assert.commandWorked(testDB.dropDatabase());

    function assertIndexes(coll, numIndexes, indexes, options) {
        options = options || {};
        let res = coll.runCommand("listIndexes", options);
        assert.eq(numIndexes, res.cursor.firstBatch.length);
        for (var i = 0; i < numIndexes; i++) {
            assert.eq(indexes[i], res.cursor.firstBatch[i].name);
        }
    }

    let coll = testDB.list_indexes_ready_and_in_progress;
    coll.drop();
    assert.commandWorked(testDB.createCollection(coll.getName()));
    assertIndexes(coll, 1, ["_id_"]);
    assert.commandWorked(coll.createIndex({a: 1}));
    assertIndexes(coll, 2, ["_id_", "a_1"]);

    assert.commandWorked(
        testDB.adminCommand({configureFailPoint: 'hangAfterStartingIndexBuild', mode: 'alwaysOn'}));
    const createIdx = startParallelShell(
        "let coll = db.getSiblingDB('test').list_indexes_ready_and_in_progress;" +
            "assert.commandWorked(coll.createIndex({ b: 1 }, { background: true }));",
        conn.port);
    assert.soon(function() {
        return getIndexBuildOpId(testDB) != -1;
    }, "Index build operation not found after starting via parallelShell");

    // Verify there is no third index.
    assertIndexes(coll, 2, ["_id_", "a_1"]);

    // The listIndexes command supports returning all indexes, including ones that are not ready.
    assertIndexes(coll, 3, ["_id_", "a_1", "b_1"], {includeIndexBuilds: true});

    assert.commandWorked(
        testDB.adminCommand({configureFailPoint: 'hangAfterStartingIndexBuild', mode: 'off'}));
    // Wait for the index build to stop.
    assert.soon(function() {
        return getIndexBuildOpId(testDB) == -1;
    });
    const exitCode = createIdx();
    assert.eq(0, exitCode, 'expected shell to exit cleanly');

    assertIndexes(coll, 3, ["_id_", "a_1", "b_1"]);
    MongoRunner.stopMongod(conn);
}());
