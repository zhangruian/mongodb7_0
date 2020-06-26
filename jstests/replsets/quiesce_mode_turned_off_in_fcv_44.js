/*
 * Tests that a node does not enter quiesce mode during shutdown if we are in FCV 4.4.
 *
 * TODO SERVER-49138: Remove this test once we branch for 4.8.
 *
 * @tags: [multiversion_incompatible]
 */
(function() {
'use strict';

load('jstests/replsets/rslib.js');

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiateWithHighElectionTimeout();

const primary = rst.getPrimary();

// Set FCV to 4.4 to test that quiesce mode is ignored.
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
assert.commandWorked(primary.adminCommand({clearLog: "global"}));

// Shutdown the node.
rst.stop(primary);
// Restart the node so we can check its logs.
rst.restart(primary);

jsTestLog("Check for the absence of quiesce mode logs");
assert(!checkLog.checkContainsOnce(primary, "Entering quiesce mode for shutdown"));

rst.stopSet();
}());