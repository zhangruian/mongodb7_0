/**
 * Util functions used by cluster server parameter tests.
 *
 * When adding new cluster server parameters, do the following:
 * 1. Add its name to clusterParameterNames.
 * 2. Add the clusterParameter document that's expected as default to clusterParametersDefault.
 * 3. Add the clusterParameter document that setClusterParameter is expected to insert after its
 *    first invocation to clusterParametersInsert.
 * 4. Add the clusterParameter document that setClusterParameter is expected to update to after its
 *    second invocation to clusterParametersUpdate.
 *
 */

const clusterParameterNames =
    ["testStrClusterParameter", "testIntClusterParameter", "testBoolClusterParameter"];
const clusterParametersDefault = [
    {
        _id: "testStrClusterParameter",
        strData: "off",
    },
    {
        _id: "testIntClusterParameter",
        intData: 16,
    },
    {
        _id: "testBoolClusterParameter",
        boolData: false,
    }
];

const clusterParametersInsert = [
    {
        _id: "testStrClusterParameter",
        strData: "on",
    },
    {
        _id: "testIntClusterParameter",
        intData: 17,
    },
    {
        _id: "testBoolClusterParameter",
        boolData: true,
    }
];

const clusterParametersUpdate = [
    {
        _id: "testStrClusterParameter",
        strData: "sleep",
    },
    {
        _id: "testIntClusterParameter",
        intData: 18,
    },
    {
        _id: "testBoolClusterParameter",
        boolData: false,
    }
];

// Set the log level for get/setClusterParameter logging to appear.
function setupNode(conn) {
    const adminDB = conn.getDB('admin');
    adminDB.setLogLevel(2);
}

function setupReplicaSet(rst) {
    setupNode(rst.getPrimary());

    rst.getSecondaries().forEach(function(secondary) {
        setupNode(secondary);
    });
}

function setupSharded(st) {
    setupNode(st.s0);

    const shards = [st.rs0, st.rs1, st.rs2];
    shards.forEach(function(shard) {
        setupReplicaSet(shard);
    });
}

// Upserts config.clusterParameters document with w:majority via setClusterParameter.
function runSetClusterParameter(conn, update) {
    const paramName = update._id;
    let updateCopy = Object.assign({}, update);
    delete updateCopy._id;
    delete updateCopy.clusterParameterTime;
    const setClusterParameterDoc = {
        [paramName]: updateCopy,
    };

    const adminDB = conn.getDB('admin');
    assert.commandWorked(adminDB.runCommand({setClusterParameter: setClusterParameterDoc}));
}

// Runs getClusterParameter on a specific mongod or mongos node and returns true/false depending
// on whether the expected values were returned.
function runGetClusterParameterNode(conn, getClusterParameterArgs, expectedClusterParameters) {
    const adminDB = conn.getDB('admin');
    const actualClusterParameters =
        assert.commandWorked(adminDB.runCommand({getClusterParameter: getClusterParameterArgs}))
            .clusterParameters;

    // Sort the returned clusterParameters and the expected clusterParameters by _id.
    actualClusterParameters.sort((a, b) => a._id.localeCompare(b._id));
    expectedClusterParameters.sort((a, b) => a._id.localeCompare(b._id));
    for (let i = 0; i < expectedClusterParameters.length; i++) {
        const expectedClusterParameter = expectedClusterParameters[i];
        const actualClusterParameter = actualClusterParameters[i];

        // Sort both expectedClusterParameter and actualClusterParameter into alphabetical order
        // by key.
        const sortedExpectedClusterParameter =
            Object.keys(expectedClusterParameter).sort().reduce(function(sorted, key) {
                sorted[key] = expectedClusterParameter[key];
                return sorted;
            }, {});
        const sortedActualClusterParameter =
            Object.keys(actualClusterParameter).sort().reduce(function(sorted, key) {
                if (key !== 'clusterParameterTime') {
                    sorted[key] = actualClusterParameter[key];
                }
                return sorted;
            }, {});
        if (bsonWoCompare(sortedExpectedClusterParameter, sortedActualClusterParameter) !== 0) {
            print('expected: ' + tojson(sortedExpectedClusterParameter) +
                  '\nactual: ' + tojson(sortedActualClusterParameter));
            return false;
        }
    }

    return true;
}

// Runs getClusterParameter on each replica set node and asserts that the response matches the
// expected parameter objects on at least a majority of nodes.
function runGetClusterParameterReplicaSet(rst, getClusterParameterArgs, expectedClusterParameters) {
    let numMatches = 0;
    const numTotalNodes = rst.getSecondaries().length + 1;
    if (runGetClusterParameterNode(
            rst.getPrimary(), getClusterParameterArgs, expectedClusterParameters)) {
        numMatches++;
    }

    rst.getSecondaries().forEach(function(secondary) {
        if (runGetClusterParameterNode(
                secondary, getClusterParameterArgs, expectedClusterParameters)) {
            numMatches++;
        }
    });

    assert((numMatches / numTotalNodes) > 0.5);
}

// Runs getClusterParameter on mongos, each mongod in each shard replica set, and each mongod in
// the config server replica set.
function runGetClusterParameterSharded(st, getClusterParameterArgs, expectedClusterParameters) {
    runGetClusterParameterNode(st.s0, getClusterParameterArgs, expectedClusterParameters);

    runGetClusterParameterReplicaSet(
        st.configRS, getClusterParameterArgs, expectedClusterParameters);
    const shards = [st.rs0, st.rs1, st.rs2];
    shards.forEach(function(shard) {
        runGetClusterParameterReplicaSet(shard, getClusterParameterArgs, expectedClusterParameters);
    });
}

// Tests valid usages of getClusterParameter and verifies that the expected values are returned.
function testValidClusterParameterCommands(conn) {
    if (conn instanceof ReplSetTest) {
        // Run getClusterParameter in list format and '*' and ensure it returns all default values
        // on all nodes in the replica set.
        runGetClusterParameterReplicaSet(conn, clusterParameterNames, clusterParametersDefault);
        runGetClusterParameterReplicaSet(conn, '*', clusterParametersDefault);

        // For each parameter, run setClusterParameter and verify that getClusterParameter
        // returns the updated value on all nodes in the replica set.
        for (let i = 0; i < clusterParameterNames.length; i++) {
            runSetClusterParameter(conn.getPrimary(), clusterParametersInsert[i]);
            runGetClusterParameterReplicaSet(
                conn, clusterParameterNames[i], [clusterParametersInsert[i]]);
        }

        // Do the above again to verify that document updates are also handled properly.
        for (let i = 0; i < clusterParameterNames.length; i++) {
            runSetClusterParameter(conn.getPrimary(), clusterParametersUpdate[i]);
            runGetClusterParameterReplicaSet(
                conn, clusterParameterNames[i], [clusterParametersUpdate[i]]);
        }

        // Finally, run getClusterParameter in list format and '*' and ensure that they now all
        // return updated values.
        runGetClusterParameterReplicaSet(conn, clusterParameterNames, clusterParametersUpdate);
        runGetClusterParameterReplicaSet(conn, '*', clusterParametersUpdate);
    } else {
        // Run getClusterParameter in list format and '*' and ensure it returns all default values
        // on all nodes in the sharded cluster.
        runGetClusterParameterSharded(conn, clusterParameterNames, clusterParametersDefault);
        runGetClusterParameterSharded(conn, '*', clusterParametersDefault);

        // For each parameter, simulate setClusterParameter and verify that getClusterParameter
        // returns the updated value on all nodes in the sharded cluster.
        for (let i = 0; i < clusterParameterNames.length; i++) {
            runSetClusterParameter(conn.s0, clusterParametersInsert[i]);
            runGetClusterParameterSharded(
                conn, clusterParameterNames[i], [clusterParametersInsert[i]]);
        }

        // Do the above again to verify that document updates are also handled properly.
        for (let i = 0; i < clusterParameterNames.length; i++) {
            runSetClusterParameter(conn.s0, clusterParametersUpdate[i]);
            runGetClusterParameterSharded(
                conn, clusterParameterNames[i], [clusterParametersUpdate[i]]);
        }

        // Finally, run getClusterParameter in list format and '*' and ensure that they now all
        // return updated values.
        runGetClusterParameterSharded(conn, clusterParameterNames, clusterParametersUpdate);
        runGetClusterParameterSharded(conn, '*', clusterParametersUpdate);
    }
}

// Tests that invalid uses of getClusterParameter fails on a given node.
function testInvalidGetClusterParameter(conn) {
    const adminDB = conn.getDB('admin');
    // Assert that specifying a nonexistent parameter returns an error.
    assert.commandFailedWithCode(adminDB.runCommand({getClusterParameter: "nonexistentParam"}),
                                 ErrorCodes.NoSuchKey);
    assert.commandFailedWithCode(adminDB.runCommand({getClusterParameter: ["nonexistentParam"]}),
                                 ErrorCodes.NoSuchKey);
    assert.commandFailedWithCode(
        adminDB.runCommand({getClusterParameter: ["testIntClusterParameter", "nonexistentParam"]}),
        ErrorCodes.NoSuchKey);
}

// Tests that invalid uses of set/getClusterParameter fail with the appropriate errors.
function testInvalidClusterParameterCommands(conn) {
    if (conn instanceof ReplSetTest) {
        const adminDB = conn.getPrimary().getDB('admin');

        // Assert that invalid uses of getClusterParameter fail on the primary.
        testInvalidGetClusterParameter(conn.getPrimary());

        // Assert that setting a nonexistent parameter on the primary returns an error.
        assert.commandFailedWithCode(
            adminDB.runCommand({setClusterParameter: {nonexistentParam: {intData: 5}}}),
            ErrorCodes.IllegalOperation);

        // Assert that running setClusterParameter with a scalar value fails.
        assert.commandFailedWithCode(
            adminDB.runCommand({setClusterParameter: {testIntClusterParameter: 5}}),
            ErrorCodes.IllegalOperation);

        conn.getSecondaries().forEach(function(secondary) {
            // Assert that setClusterParameter cannot be run on a secondary.
            const secondaryAdminDB = secondary.getDB('admin');
            assert.commandFailedWithCode(
                secondaryAdminDB.runCommand(
                    {setClusterParameter: {testIntClusterParameter: {intData: 5}}}),
                ErrorCodes.NotWritablePrimary);
            // Assert that invalid uses of getClusterParameter fail on secondaries.
            testInvalidGetClusterParameter(secondary);
        });
    } else {
        const adminDB = conn.s0.getDB('admin');

        // Assert that invalid uses of getClusterParameter fail on mongos.
        testInvalidGetClusterParameter(conn.s0);

        // Assert that setting a nonexistent parameter on the mongos returns an error.
        assert.commandFailedWithCode(
            adminDB.runCommand({setClusterParameter: {nonexistentParam: {intData: 5}}}),
            ErrorCodes.IllegalOperation);

        // Assert that running setClusterParameter with a scalar value fails.
        assert.commandFailedWithCode(
            adminDB.runCommand({setClusterParameter: {testIntClusterParameter: 5}}),
            ErrorCodes.IllegalOperation);

        const shards = [conn.rs0, conn.rs1, conn.rs2];
        shards.forEach(function(shard) {
            // Assert that setClusterParameter cannot be run directly on a shard primary.
            const shardPrimaryAdmin = shard.getPrimary().getDB('admin');
            assert.commandFailedWithCode(
                shardPrimaryAdmin.runCommand(
                    {setClusterParameter: {testIntClusterParameter: {intData: 5}}}),
                ErrorCodes.NotImplemented);
            // Assert that invalid forms of getClusterParameter fail on the shard primary.
            testInvalidGetClusterParameter(shard.getPrimary());
            shard.getSecondaries().forEach(function(secondary) {
                // Assert that setClusterParameter cannot be run on a shard secondary.
                const shardSecondaryAdmin = secondary.getDB('admin');
                assert.commandFailedWithCode(
                    shardSecondaryAdmin.runCommand(
                        {setClusterParameter: {testIntClusterParameter: {intData: 5}}}),
                    ErrorCodes.NotWritablePrimary);
                // Assert that invalid forms of getClusterParameter fail on shard secondaries.
                testInvalidGetClusterParameter(secondary);
            });
        });

        // Assert that setClusterParameter cannot be run directly on the configsvr primary.
        const configRS = conn.configRS;
        const configPrimaryAdmin = configRS.getPrimary().getDB('admin');
        assert.commandFailedWithCode(
            configPrimaryAdmin.runCommand(
                {setClusterParameter: {testIntClusterParameter: {intData: 5}}}),
            ErrorCodes.NotImplemented);
        // Assert that invalid forms of getClusterParameter fail on the configsvr primary.
        testInvalidGetClusterParameter(configRS.getPrimary());
        configRS.getSecondaries().forEach(function(secondary) {
            // Assert that setClusterParameter cannot be run on a configsvr secondary.
            const configSecondaryAdmin = secondary.getDB('admin');
            assert.commandFailedWithCode(
                configSecondaryAdmin.runCommand(
                    {setClusterParameter: {testIntClusterParameter: {intData: 5}}}),
                ErrorCodes.NotWritablePrimary);
            // Assert that invalid forms of getClusterParameter fail on configsvr secondaries.
            testInvalidGetClusterParameter(secondary);
        });
    }
}
