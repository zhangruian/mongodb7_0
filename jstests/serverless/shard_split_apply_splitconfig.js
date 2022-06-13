load("jstests/libs/fail_point_util.js");         // for "configureFailPoint"
load('jstests/libs/parallel_shell_helpers.js');  // for "startParallelShell"
load("jstests/serverless/libs/basic_serverless_test.js");

function populateRecipientMembers(splitConfig) {
    return splitConfig.members.filter(m => m._id >= 3).map((member, idx) => {
        member._id = idx;
        member.votes = 1;
        member.priority = 1;
        member.hidden = false;
        return member;
    });
}

function runReconfigToSplitConfig() {
    "use strict";

    const kRecipientSetName = "receiveSet";

    jsTestLog("Starting serverless");
    const test =
        new BasicServerlessTest({recipientTagName: "recipientNode", recipientSetName: "recipient"});

    jsTestLog("Adding recipient nodes");
    test.addRecipientNodes();

    test.donor.awaitSecondaryNodes();

    jsTestLog("Reconfigure the donor to apply a `splitConfig`");
    const config = test.donor.getReplSetConfigFromNode();
    const splitConfig = Object.extend({}, config, /* deepCopy */ true);
    splitConfig._id = kRecipientSetName;
    splitConfig.version++;
    splitConfig.members = populateRecipientMembers(splitConfig);

    // TODO: possible future validation in replSetReconfig command?
    delete splitConfig.settings.replicaSetId;

    const configWithSplitConfig = Object.extend({}, config, /* deepCopy */ true);
    configWithSplitConfig.version++;
    configWithSplitConfig.recipientConfig = splitConfig;
    configWithSplitConfig.members = configWithSplitConfig.members.filter(m => m._id < 3);

    jsTestLog("Applying the split config, and waiting for it to propagate to recipient");
    const admin = test.donor.getPrimary().getDB("admin");
    assert.commandWorked(admin.runCommand({replSetReconfig: configWithSplitConfig}));
    assert.soon(() => {
        const recipientNode = test.recipientNodes[0];
        const status =
            assert.commandWorked(recipientNode.getDB('admin').runCommand({replSetGetStatus: 1}));
        return status.set === kRecipientSetName;
    }, "waiting for split config to take", 30000, 2000);

    jsTestLog("Confirming we can write to recipient");
    let recipientPrimary = undefined;
    assert.soon(function() {
        recipientPrimary = test.recipientNodes.find(node => {
            const n = node.adminCommand('hello');
            return n.isWritablePrimary || n.ismaster;
        });
        return recipientPrimary != undefined;
    }, "waiting for primary to be available", 30000, 1000);

    assert(recipientPrimary);
    assert.commandWorked(recipientPrimary.getDB('foo').bar.insert({fake: 'document'}));

    test.stop();
}

runReconfigToSplitConfig();
