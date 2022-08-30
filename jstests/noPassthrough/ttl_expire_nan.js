/**
 * Tests TTL indexes with NaN for 'expireAfterSeconds'.
 *
 * Existing TTL indexes from older versions of the server may contain a NaN for the duration.
 * Newer server versions (5.0+) normalize the TTL duration to 0.
 *
 * @tags: [
 *     requires_replication,
 * ]
 */
(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load('jstests/noPassthrough/libs/index_build.js');

const rst = new ReplSetTest({
    nodes: [{}, {rsConfig: {votes: 0, priority: 0}}],
    nodeOptions: {setParameter: {ttlMonitorSleepSecs: 5}},
    // Sync from primary only so that we have a well-defined node to check listIndexes behavior.
    settings: {chainingAllowed: false},
});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
const db = primary.getDB('test');
const coll = db.t;

// The test cases here revolve around having a TTL index in the catalog with a NaN
// 'expireAfterSeconds'. The current createIndexes behavior will overwrite NaN with int32::max
// unless we use a fail point.
const fp = configureFailPoint(primary, 'skipTTLIndexNaNExpireAfterSecondsValidation');
try {
    assert.commandWorked(coll.createIndex({t: 1}, {expireAfterSeconds: NaN}));
} finally {
    fp.off();
}

assert.commandWorked(coll.insert({_id: 0, t: ISODate()}));

// Wait for "TTL indexes require the expire field to be numeric, skipping TTL job" log message.
checkLog.containsJson(primary, 22542, {ns: coll.getFullName()});

// TTL index should be replicated to the secondary with a NaN 'expireAfterSeconds'.
const secondary = rst.getSecondary();
checkLog.containsJson(secondary, 20384, {
    namespace: coll.getFullName(),
    properties: (spec) => {
        jsTestLog('TTL index on secondary: ' + tojson(spec));
        return isNaN(spec.expireAfterSeconds);
    }
});

assert.eq(
    coll.countDocuments({}), 1, 'ttl index with NaN duration should not remove any documents.');

// Confirm that TTL index is replicated with a non-zero 'expireAfterSeconds' during initial sync.
const newNode = rst.add({rsConfig: {votes: 0, priority: 0}});
rst.reInitiate();
rst.waitForState(newNode, ReplSetTest.State.SECONDARY);
rst.awaitReplication();
let newNodeTestDB = newNode.getDB(db.getName());
let newNodeColl = newNodeTestDB.getCollection(coll.getName());
const newNodeIndexes = IndexBuildTest.assertIndexes(newNodeColl, 2, ['_id_', 't_1']);
const newNodeSpec = newNodeIndexes.t_1;
jsTestLog('TTL index on initial sync node: ' + tojson(newNodeSpec));
assert(newNodeSpec.hasOwnProperty('expireAfterSeconds'),
       'Index was not replicated as a TTL index during initial sync.');
assert.gt(newNodeSpec.expireAfterSeconds,
          0,
          'NaN expireAferSeconds was replicated as zero during initial sync.');

// Check that listIndexes on the primary logged a "Fixing expire field from TTL index spec" message
// during the NaN 'expireAfterSeconds' conversion.
checkLog.containsJson(primary, 6835900, {namespace: coll.getFullName()});

// Confirm that a node with an existing TTL index with NaN 'expireAfterSeconds' will convert the
// duration on the TTL index from NaN to a large positive value when it becomes the primary node.
// When stepping down the primary, we use 'force' because there's no other electable node.
// Subsequently, we wait for the stepped down node to become primary again.
// To confirm that the TTL index has been fixed, we check the oplog for a collMod operation on the
// TTL index that changes the `expireAfterSeconds` field from NaN to a large positive value.
assert.commandWorked(primary.adminCommand({replSetStepDown: 5, force: true}));
primary = rst.waitForPrimary();
const collModOplogEntries =
    rst.findOplog(primary,
                  {
                      op: 'c',
                      ns: coll.getDB().getCollection('$cmd').getFullName(),
                      'o.collMod': coll.getName(),
                      'o.index.name': 't_1',
                      'o.index.expireAfterSeconds': newNodeSpec.expireAfterSeconds
                  },
                  /*limit=*/1)
        .toArray();
assert.eq(collModOplogEntries.length,
          1,
          'TTL index with NaN expireAfterSeconds was not fixed using collMod during step-up: ' +
              tojson(rst.findOplog(primary, {op: {$ne: 'n'}}, /*limit=*/10).toArray()));

// Confirm that createIndexes will overwrite a NaN 'expireAfterSeconds' in a TTL index before saving
// it to the catalog and replicating it downstream.
const coll2 = db.w;
assert.commandWorked(coll2.createIndex({t: 1}, {expireAfterSeconds: NaN}));
assert.commandWorked(coll2.insert({_id: 0, t: ISODate()}));

// TTL index should be replicated to the secondary with a non-NaN 'expireAfterSeconds'.
checkLog.containsJson(secondary, 20384, {
    namespace: coll2.getFullName(),
    properties: (spec) => {
        jsTestLog('TTL index on secondary (with overwritten NaN expireAfterSeconds): ' +
                  tojson(spec));
        return spec.hasOwnProperty('expireAfterSeconds') && !isNaN(spec.expireAfterSeconds) &&
            spec.expireAfterSeconds === newNodeSpec.expireAfterSeconds;
    }
});

rst.stopSet();
})();
