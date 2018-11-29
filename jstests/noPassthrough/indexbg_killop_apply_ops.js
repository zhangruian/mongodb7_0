/**
 * Confirms that background index builds started through applyOps on a primary cannot be aborted
 * using killop.
 * @tags: [requires_replication]
 */
(function() {
    "use strict";

    load('jstests/noPassthrough/libs/index_build.js');

    const rst = new ReplSetTest({
        nodes: [
            {},
            {
              // Disallow elections on secondary.
              rsConfig: {
                  priority: 0,
                  votes: 0,
              },
            },
        ]
    });
    const nodes = rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const testDB = primary.getDB('test');
    const coll = testDB.getCollection('test');

    assert.writeOK(coll.insert({a: 1}));

    IndexBuildTest.pauseIndexBuilds(primary);

    const applyOpsCmd = {
        applyOps: [
            {
              op: 'c',
              ns: testDB.getCollection('$cmd').getFullName(),
              o: {
                  createIndexes: coll.getName(),
                  v: 2,
                  name: 'a_1',
                  key: {a: 1},
                  background: true,
              },
            },
        ]
    };
    const createIdx =
        startParallelShell('db.adminCommand(' + tojson(applyOpsCmd) + ')', primary.port);

    // When the index build starts, find its op id.
    const opId = IndexBuildTest.waitForIndexBuildToStart(testDB);

    // Kill the index build.
    assert.commandWorked(testDB.killOp(opId));

    // Wait for the index build to stop.
    try {
        IndexBuildTest.waitForIndexBuildToStop(testDB);
    } finally {
        IndexBuildTest.resumeIndexBuilds(primary);
    }

    // Expect successful createIndex command invocation in parallel shell because applyOps returns
    // immediately after starting the background index build in a separate thread.
    createIdx();

    // Check that no new index has been created.  This verifies that the index build was aborted
    // rather than successfully completed.
    IndexBuildTest.assertIndexes(coll, 1, ['_id_']);

    // Check that a new index has been created on the secondary.
    // This is due to the createIndexes command being replicated to the secondary before the primary
    // has completed the index build in a background job.
    rst.awaitReplication();
    const secondary = rst.getSecondary();
    const secondaryDB = secondary.getDB(testDB.getName());
    const secondaryColl = secondaryDB.getCollection(coll.getName());
    IndexBuildTest.assertIndexes(secondaryColl, 2, ['_id_', 'a_1']);

    // Index 'a_1' was aborted on the primary, resulting in a different set of indexes on the
    // secondary compared to the primary. Therefore, we skip the dbhash checking while tearing down
    // the replica set test fixture.
    TestData.skipCheckDBHashes = true;
    rst.stopSet();
})();
