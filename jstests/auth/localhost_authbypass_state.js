/* Make sure auth bypass is correctly detected across restarts and user add/delete
 * @tags: [requires_replication, requires_persistence]
 */

(function() {
'use strict';

const CREATE_ADMIN = {
    createUser: 'admin',
    pwd: 'pwd',
    roles: ['__system']
};
const CREATE_USER1 = {
    createUser: 'user1',
    pwd: 'pwd',
    roles: []
};
const CREATE_USER2 = {
    createUser: 'user2',
    pwd: 'pwd',
    roles: []
};
const CREATE_USER3 = {
    createUser: 'user3',
    pwd: 'pwd',
    roles: []
};

function runTest(name, conns, restartCallback) {
    jsTest.log('Starting: ' + name);
    assert(conns.primary);
    let admin = conns.primary.getDB('admin');

    // Initial localhost auth bypass in effect.
    assert.commandWorked(admin.runCommand(CREATE_ADMIN));

    // Localhost auth bypass is now closed.
    assert.commandFailed(admin.runCommand(CREATE_USER1));
    if (conns.replset) {
        // Confirm bypass closure has reached secondary.
        conns.replset.awaitSecondaryNodes();
        assert.commandFailed(conns.replset.getSecondary().getDB('admin').runCommand(CREATE_USER1));
    }

    // But it's okay if we actually auth.
    assert(admin.auth('admin', 'pwd'));
    assert.commandWorked(admin.runCommand(CREATE_USER1));
    admin.logout();

    // Shut down server and restart.
    jsTest.log('First restart: ' + name);
    conns = restartCallback();
    assert(conns.primary);
    admin = conns.primary.getDB('admin');

    // Localhost auth bypass is still closed.
    assert.commandFailed(admin.runCommand(CREATE_USER2));
    if (conns.replset) {
        assert.commandFailed(conns.replset.getSecondary().getDB('admin').runCommand(CREATE_USER2));
    }

    // We can happily auth and make another user.
    assert(admin.auth('admin', 'pwd'));
    assert.commandWorked(admin.runCommand(CREATE_USER2));

    // We can even drop the collection and our login session will be invalidated.
    const preDrop =
        assert.commandWorked(admin.runCommand({connectionStatus: 1})).authInfo.authenticatedUsers;
    assert.eq(preDrop.length, 1);
    assert.writeOK(admin.system.users.remove({}));
    const postDrop =
        assert.commandWorked(admin.runCommand({connectionStatus: 1})).authInfo.authenticatedUsers;
    assert.eq(postDrop.length, 0);

    // Can't recreate ourselves because localhost auth bypass is still disabled.
    assert.commandFailed(admin.runCommand(CREATE_ADMIN));
    if (conns.replset) {
        assert.commandFailed(conns.replset.getSecondary().getDB('admin').runCommand(CREATE_ADMIN));
    }

    // Shut down server and restart, we should get bypass back now.
    jsTest.log('Second restart: ' + name);
    conns = restartCallback();
    assert(conns.primary);
    admin = conns.primary.getDB('admin');

    // Localhost auth bypass is back!
    assert.commandWorked(admin.runCommand(CREATE_ADMIN));

    // Aaaaaand, it's gone.
    assert.commandFailed(admin.runCommand(CREATE_USER3));
    if (conns.replset) {
        assert.commandFailed(conns.replset.getSecondary().getDB('admin').runCommand(CREATE_USER3));
    }

    jsTest.log('Finished: ' + name);
}

let standalone = MongoRunner.runMongod({auth: ''});
runTest('Standalone', {primary: standalone}, function() {
    const dbpath = standalone.dbpath;
    MongoRunner.stopMongod(standalone);
    standalone = MongoRunner.runMongod({auth: '', restart: true, cleanData: false, dbpath: dbpath});
    return {primary: standalone};
});
MongoRunner.stopMongod(standalone);

const replset =
    new ReplSetTest({name: 'rs0', nodes: 2, nodeOptions: {auth: ''}, keyFile: 'jstests/libs/key1'});
replset.startSet();
replset.initiate();
replset.awaitSecondaryNodes();
runTest('ReplSet', {primary: replset.getPrimary(), replset: replset}, function() {
    const signalTerm = 15;
    replset.restart([0, 1], undefined, signalTerm, false);
    replset.awaitSecondaryNodes();
    return {primary: replset.getPrimary(), replset: replset};
});
replset.stopSet();
})();
