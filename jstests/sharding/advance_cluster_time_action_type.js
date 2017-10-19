/**
 * Test a role with an advanceClusterTime action type.
 */

(function() {
    "use strict";

    let st = new ShardingTest(
        {mongos: 1, config: 1, shards: 1, keyFile: 'jstests/libs/key1', mongosWaitsForKeys: true});
    let adminDB = st.s.getDB('admin');

    assert.commandWorked(adminDB.runCommand({createUser: "admin", pwd: "admin", roles: ["root"]}));
    assert.eq(1, adminDB.auth("admin", "admin"));

    assert.commandWorked(adminDB.runCommand({
        createRole: "advanceClusterTimeRole",
        privileges: [{resource: {cluster: true}, actions: ["advanceClusterTime"]}],
        roles: []
    }));

    let testDB = adminDB.getSiblingDB("testDB");

    assert.commandWorked(
        testDB.runCommand({createUser: 'NotTrusted', pwd: 'pwd', roles: ['readWrite']}));
    assert.commandWorked(testDB.runCommand({
        createUser: 'Trusted',
        pwd: 'pwd',
        roles: [{role: 'advanceClusterTimeRole', db: 'admin'}, 'readWrite']
    }));
    assert.eq(1, testDB.auth("NotTrusted", "pwd"));

    let res = testDB.runCommand({insert: "foo", documents: [{_id: 0}]});
    assert.commandWorked(res);

    let clusterTime = res.$clusterTime;
    let clusterTimeTS = new Timestamp(clusterTime.clusterTime.getTime() + 1000, 0);
    clusterTime.clusterTime = clusterTimeTS;

    const cmdObj = {find: "foo", limit: 1, singleBatch: true, $clusterTime: clusterTime};
    jsTestLog("running NonTrusted. command: " + tojson(cmdObj));
    res = testDB.runCommand(cmdObj);
    assert.commandFailed(res, "Command request was: " + tojsononeline(cmdObj));

    assert.eq(1, testDB.auth("Trusted", "pwd"));
    jsTestLog("running Trusted. command: " + tojson(cmdObj));
    res = testDB.runCommand(cmdObj);
    assert.commandWorked(res, "Command request was: " + tojsononeline(cmdObj));

    testDB.logout();

    st.stop();
})();
