/**
 * Multiversion initial sync test. Tests that initial sync succeeds when a 'last-stable' version
 * secondary syncs from a 'latest' version replica set.
 */
// @tags: [fix_for_fcv_46]

'use strict';

load("./jstests/multiVersion/libs/initial_sync.js");

var testName = "multiversion_initial_sync_last_stable_from_latest";
let replSetVersion = "latest";
let newSecondaryVersion = "last-stable";

multversionInitialSyncTest(testName, replSetVersion, newSecondaryVersion, {}, lastStableFCV);
