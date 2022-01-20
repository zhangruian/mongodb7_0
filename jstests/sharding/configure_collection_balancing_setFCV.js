/**
 * Test setFCV interactions with per-collection balancing settings
 *
 * @tags: [
 *  requires_fcv_53,
 *  featureFlagPerCollBalancingSettings,
 * ]
 */
// TODO SERVER-62693 get rid of this file once 6.0 branches out

'use strict';

const st = new ShardingTest({mongos: 1, shards: 1, other: {enableBalancer: false}});

const database = st.getDB('test');
assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));
const collName = 'coll';
const coll = database[collName];
const fullNs = coll.getFullName();

assert.commandWorked(st.s.adminCommand({shardCollection: fullNs, key: {x: 1}}));

// TODO SERVER-62584: remove lastContinuousFCV check once 5.3 branches out
for (const downgradeVersion of [lastLTSFCV, lastContinuousFCV]) {
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    // Test that downgrade is not allowed if a collection is undergoing defragmentation
    {
        // Set collection under defragmentation to block downgrade
        assert.commandWorked(
            st.s.adminCommand({configureCollectionBalancing: fullNs, defragmentCollection: true}));

        var setFCVCmdResult = st.s.adminCommand({setFeatureCompatibilityVersion: downgradeVersion});
        assert.commandFailedWithCode(setFCVCmdResult, ErrorCodes.CannotDowngrade);

        // Rollback the change to allow downgrade
        assert.commandWorked(st.config.collections.updateOne(
            {defragmentCollection: {$exists: true}}, {$unset: {defragmentCollection: 1}}));
    }

    // Check that per-collection balancing fields are removed upon setFCV < 5.3
    {
        assert.commandWorked(st.s.adminCommand(
            {configureCollectionBalancing: fullNs, enableAutoSplitter: false, chunkSize: 10}));

        var configEntryBeforeSetFCV =
            st.config.getSiblingDB('config').collections.findOne({_id: fullNs});
        var shardEntryBeforeSetFCV =
            st.shard0.getDB('config').cache.collections.findOne({_id: fullNs});
        assert.eq(10 * 1024 * 1024, configEntryBeforeSetFCV.maxChunkSizeBytes);
        assert(configEntryBeforeSetFCV.noAutoSplit);
        assert.eq(10 * 1024 * 1024, shardEntryBeforeSetFCV.maxChunkSizeBytes);
        assert(!shardEntryBeforeSetFCV.allowAutoSplit);

        assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: downgradeVersion}));

        var configEntryAfterSetFCV =
            st.config.getSiblingDB('config').collections.findOne({_id: fullNs});
        var shardEntryAfterSetFCV =
            st.shard0.getDB('config').cache.collections.findOne({_id: fullNs});
        assert.isnull(configEntryAfterSetFCV.maxChunkSizeBytes);
        assert.isnull(configEntryAfterSetFCV.noAutoSplit);
        assert.isnull(shardEntryAfterSetFCV.maxChunkSizeBytes);
        assert.isnull(shardEntryAfterSetFCV.allowAutoSplit);
    }
}

st.stop();
