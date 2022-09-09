/**
 * Util functions used by cluster server parameter tests.
 *
 * When adding new cluster server parameter, do the following:
 *   If it's test-only, add its definition to kTestOnlyClusterParameters.
 *   Otherwise, add to kNonTestOnlyClusterParameters.
 * The keyname will be the name of the cluster-wide server parameter,
 * it's value will be an object with at least three keys named:
 *   * 'default': Properties to expect on an unset CWSP
 *   * 'insert': Values to set on the CWSP on first write
 *   * 'update': Values to set on the CWSP on second write
 * A fourth property 'featureFlag' may also be set if the
 *   parameter depends on a featureFlag.
 * Use the name of the featureFlag if it is required in
 *   order to consider the parameter.
 * Prefix the name with a bang '!' if it is only considered
 *   when the featureFlag is disabled.
 */

load("jstests/libs/feature_flag_util.js");

const kNonTestOnlyClusterParameters = {
    changeStreamOptions: {
        default: {preAndPostImages: {expireAfterSeconds: 'off'}},
        insert: {preAndPostImages: {expireAfterSeconds: 30}},
        update: {preAndPostImages: {expireAfterSeconds: 'off'}},
        featureFlag: '!ServerlessChangeStreams',
    },
    changeStreams: {
        default: {expireAfterSeconds: NumberLong(3600)},
        insert: {expireAfterSeconds: 30},
        update: {expireAfterSeconds: 10},
        featureFlag: 'ServerlessChangeStreams',
    },
};

const kTestOnlyClusterParameters = {
    cwspTestNeedsFeatureFlagClusterWideToaster: {
        default: {intData: 16},
        insert: {intData: 17},
        update: {intData: 18},
        featureFlag: 'ClusterWideToaster',
    },
    testStrClusterParameter: {
        default: {strData: 'off'},
        insert: {strData: 'on'},
        update: {strData: 'sleep'},
    },
    testIntClusterParameter: {
        default: {intData: 16},
        insert: {intData: 17},
        update: {intData: 18},
    },
    testBoolClusterParameter: {
        default: {boolData: false},
        insert: {boolData: true},
        update: {boolData: false},
    },
};

const kAllClusterParameters =
    Object.assign({}, kNonTestOnlyClusterParameters, kTestOnlyClusterParameters);
const kAllClusterParameterNames = Object.keys(kAllClusterParameters);
const kAllClusterParameterDefaults = kAllClusterParameterNames.map(
    (name) => Object.assign({_id: name}, kAllClusterParameters[name].default));
const kAllClusterParameterInserts = kAllClusterParameterNames.map(
    (name) => Object.assign({_id: name}, kAllClusterParameters[name].insert));
const kAllClusterParameterUpdates = kAllClusterParameterNames.map(
    (name) => Object.assign({_id: name}, kAllClusterParameters[name].update));

const kNonTestOnlyClusterParameterDefaults =
    Object.keys(kNonTestOnlyClusterParameters)
        .map((name) => Object.assign({_id: name}, kAllClusterParameters[name].default));

function considerParameter(paramName, conn) {
    // { featureFlag: 'name' } indicates that the CWSP should only be considered with the FF
    // enabled. { featureFlag: '!name' } indicates that the CWSP should only be considered with the
    // FF disabled.
    const cp = kAllClusterParameters[paramName] || {};
    if (cp.featureFlag) {
        const considerWhenFFEnabled = cp.featureFlag[0] !== '!';
        const ff = cp.featureFlag.substr(considerWhenFFEnabled ? 0 : 1);
        return FeatureFlagUtil.isEnabled(conn, ff) === considerWhenFFEnabled;
    }
    return true;
}

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
    if (!considerParameter(paramName, conn)) {
        return;
    }
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

    // Filter out parameters that we don't care about.
    if (Array.isArray(getClusterParameterArgs)) {
        getClusterParameterArgs =
            getClusterParameterArgs.filter((name) => considerParameter(name, conn));
    } else if ((typeof getClusterParameterArgs === 'string') &&
               !considerParameter(getClusterParameterArgs, conn)) {
        return true;
    }

    const actualClusterParameters =
        assert.commandWorked(adminDB.runCommand({getClusterParameter: getClusterParameterArgs}))
            .clusterParameters;

    // Reindex actual based on name, and remove irrelevant field.
    let actual = {};
    actualClusterParameters.forEach(function(acp) {
        actual[acp._id] = acp;
        delete actual[acp._id].clusterParameterTime;
    });

    for (let i = 0; i < expectedClusterParameters.length; i++) {
        if (!considerParameter(expectedClusterParameters[i]._id, conn)) {
            continue;
        }

        const id = expectedClusterParameters[i]._id;
        if (actual[id] === undefined) {
            jsTest.log('Expected to retreive ' + id + ' but it was not returned');
            return false;
        }

        if (bsonWoCompare(expectedClusterParameters[i], actual[id]) !== 0) {
            jsTest.log('Server parameter mismatch on node: ' + conn.host + '\n' +
                       'Expected: ' + tojson(expectedClusterParameters[i]) + '\n' +
                       'Actual: ' + tojson(actual[id]));
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
        runGetClusterParameterReplicaSet(
            conn, kAllClusterParameterNames, kAllClusterParameterDefaults);
        runGetClusterParameterReplicaSet(conn, '*', kAllClusterParameterDefaults);

        // For each parameter, run setClusterParameter and verify that getClusterParameter
        // returns the updated value on all nodes in the replica set.
        for (let i = 0; i < kAllClusterParameterNames.length; i++) {
            runSetClusterParameter(conn.getPrimary(), kAllClusterParameterInserts[i]);
            runGetClusterParameterReplicaSet(
                conn, kAllClusterParameterNames[i], [kAllClusterParameterInserts[i]]);
        }

        // Do the above again to verify that document updates are also handled properly.
        for (let i = 0; i < kAllClusterParameterNames.length; i++) {
            runSetClusterParameter(conn.getPrimary(), kAllClusterParameterUpdates[i]);
            runGetClusterParameterReplicaSet(
                conn, kAllClusterParameterNames[i], [kAllClusterParameterUpdates[i]]);
        }

        // Finally, run getClusterParameter in list format and '*' and ensure that they now all
        // return updated values.
        runGetClusterParameterReplicaSet(
            conn, kAllClusterParameterNames, kAllClusterParameterUpdates);
        runGetClusterParameterReplicaSet(conn, '*', kAllClusterParameterUpdates);
    } else {
        // Run getClusterParameter in list format and '*' and ensure it returns all default values
        // on all nodes in the sharded cluster.
        runGetClusterParameterSharded(
            conn, kAllClusterParameterNames, kAllClusterParameterDefaults);
        runGetClusterParameterSharded(conn, '*', kAllClusterParameterDefaults);

        // For each parameter, simulate setClusterParameter and verify that getClusterParameter
        // returns the updated value on all nodes in the sharded cluster.
        for (let i = 0; i < kAllClusterParameterNames.length; i++) {
            runSetClusterParameter(conn.s0, kAllClusterParameterInserts[i]);
            runGetClusterParameterSharded(
                conn, kAllClusterParameterNames[i], [kAllClusterParameterInserts[i]]);
        }

        // Do the above again to verify that document updates are also handled properly.
        for (let i = 0; i < kAllClusterParameterNames.length; i++) {
            runSetClusterParameter(conn.s0, kAllClusterParameterUpdates[i]);
            runGetClusterParameterSharded(
                conn, kAllClusterParameterNames[i], [kAllClusterParameterUpdates[i]]);
        }

        // Finally, run getClusterParameter in list format and '*' and ensure that they now all
        // return updated values.
        runGetClusterParameterSharded(conn, kAllClusterParameterNames, kAllClusterParameterUpdates);
        runGetClusterParameterSharded(conn, '*', kAllClusterParameterUpdates);
    }
}

// Assert that explicitly getting a disabled cluster server parameter fails on a node.
function testExplicitDisabledGetClusterParameter(conn) {
    const adminDB = conn.getDB('admin');
    assert.commandFailedWithCode(
        adminDB.runCommand({getClusterParameter: "testIntClusterParameter"}), ErrorCodes.BadValue);
    assert.commandFailedWithCode(
        adminDB.runCommand(
            {getClusterParameter: ["changeStreamOptions", "testIntClusterParameter"]}),
        ErrorCodes.BadValue);
}

// Tests that disabled cluster server parameters return errors or are filtered out as appropriate
// by get/setClusterParameter.
function testDisabledClusterParameters(conn) {
    if (conn instanceof ReplSetTest) {
        // Assert that explicitly setting a disabled cluster server parameter fails.
        const adminDB = conn.getPrimary().getDB('admin');
        assert.commandFailedWithCode(
            adminDB.runCommand({setClusterParameter: {testIntClusterParameter: {intData: 5}}}),
            ErrorCodes.BadValue);

        // Assert that explicitly getting a disabled cluster server parameter fails on the primary.
        testExplicitDisabledGetClusterParameter(conn.getPrimary());

        // Assert that explicitly getting a disabled cluster server parameter fails on secondaries.
        conn.getSecondaries().forEach(function(secondary) {
            testExplicitDisabledGetClusterParameter(secondary);
        });

        // Assert that getClusterParameter: '*' succeeds but only returns enabled cluster
        // parameters.
        runGetClusterParameterReplicaSet(conn, '*', kNonTestOnlyClusterParameterDefaults);
    } else {
        // Assert that explicitly setting a disabled cluster server parameter fails.
        const adminDB = conn.s0.getDB('admin');
        assert.commandFailedWithCode(
            adminDB.runCommand({setClusterParameter: {testIntClusterParameter: {intData: 5}}}),
            ErrorCodes.IllegalOperation);

        // Assert that explicitly getting a disabled cluster server parameter fails on mongos.
        testExplicitDisabledGetClusterParameter(conn.s0);

        // Assert that explicitly getting a disabled cluster server parameter on each shard replica
        // set and the config replica set fails.
        const shards = [conn.rs0, conn.rs1, conn.rs2];
        const configRS = conn.configRS;
        shards.forEach(function(shard) {
            testExplicitDisabledGetClusterParameter(shard.getPrimary());
            shard.getSecondaries().forEach(function(secondary) {
                testExplicitDisabledGetClusterParameter(secondary);
            });
        });

        testExplicitDisabledGetClusterParameter(configRS.getPrimary());
        configRS.getSecondaries().forEach(function(secondary) {
            testExplicitDisabledGetClusterParameter(secondary);
        });

        // Assert that getClusterParameter: '*' succeeds but only returns enabled cluster
        // parameters.
        runGetClusterParameterSharded(conn, '*', kNonTestOnlyClusterParameterDefaults);
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
    assert.commandFailedWithCode(adminDB.runCommand({getClusterParameter: []}),
                                 ErrorCodes.BadValue);
}

// Tests that invalid uses of set/getClusterParameter fail with the appropriate errors.
function testInvalidClusterParameterCommands(conn) {
    if (conn instanceof ReplSetTest) {
        const adminDB = conn.getPrimary().getDB('admin');

        // Assert that invalid uses of getClusterParameter fail on the primary.
        testInvalidGetClusterParameter(conn.getPrimary());

        // Assert that setting a nonexistent parameter on the primary returns an error.
        assert.commandFailed(
            adminDB.runCommand({setClusterParameter: {nonexistentParam: {intData: 5}}}));

        // Assert that running setClusterParameter with a scalar value fails.
        assert.commandFailed(
            adminDB.runCommand({setClusterParameter: {testIntClusterParameter: 5}}));

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
        assert.commandFailed(
            adminDB.runCommand({setClusterParameter: {nonexistentParam: {intData: 5}}}));

        // Assert that running setClusterParameter with a scalar value fails.
        assert.commandFailed(
            adminDB.runCommand({setClusterParameter: {testIntClusterParameter: 5}}));

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
