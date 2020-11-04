// @tags: [requires_fcv_49]
load("jstests/replsets/rslib.js");

var replTest = new ReplSetTest({nodes: 3, useBridge: true});
var nodes = replTest.startSet();
// If featureFlagUseSecondaryDelaySecs is enabled, we must use the 'secondaryDelaySecs' field
// name in our config. Otherwise, we use 'slaveDelay'.
const delayFieldName = selectDelayFieldName(replTest);

var config = replTest.getReplSetConfig();
// ensure member 0 is primary
config.members[0].priority = 2;
config.members[1].priority = 0;
config.members[1][delayFieldName] = 5;
config.members[2].priority = 0;

replTest.initiate(config);
var primary = replTest.getPrimary().getDB(jsTestName());

var secondaryConns = replTest.getSecondaries();
var secondaries = [];
for (var i in secondaryConns) {
    var d = secondaryConns[i].getDB(jsTestName());
    d.getMongo().setSecondaryOk();
    secondaries.push(d);
}

waitForAllMembers(primary);

nodes[0].disconnect(nodes[2]);

primary.foo.insert({x: 1});

syncFrom(nodes[1], nodes[0], replTest);

// make sure the record still appears in the remote secondary
assert.soon(function() {
    return secondaries[1].foo.findOne() != null;
});

replTest.stopSet();
