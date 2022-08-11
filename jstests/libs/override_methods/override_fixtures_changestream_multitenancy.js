// Overrides the JS test exiting 'RepliSetTest' fixture to enable reading the change stream in the
// multitenancy and serverless environment.
(function() {
"use strict";

// Make a copy of the original 'ReplSetTest' fixture.
const originalReplSet = ReplSetTest;

ReplSetTest = function(opts) {
    // Call the constructor with the original 'ReplSetTest' to populate 'this' with required fields.
    // TODO SERVER-67267 add {serverless:true} to the 'opts'.
    originalReplSet.apply(this, [opts]);

    // Make a copy of the original 'startSetAsync' function and then override it to include the
    // required parameters.
    this._originalStartSetAsync = this.startSetAsync;
    this.startSetAsync = function(options, restart) {
        const newOptions = Object.assign({}, options || {});

        const fpAssertChangeStreamNssColl =
            tojson({mode: "alwaysOn", data: {collectionName: "system.change_collection"}});

        // The 'setParameter' that should be merged with 'newOpts' for the sharded-cluster and the
        // replica-set.
        const setParameters = {
            "sharded-cluster": {
                // TODO SERVER-67267 check if 'forceEnableChangeCollectionsMode' can be removed.
                "failpoint.forceEnableChangeCollectionsMode": tojson({mode: "alwaysOn"}),
                "failpoint.assertChangeStreamNssCollection": fpAssertChangeStreamNssColl
            },
            "replica-set": {
                featureFlagServerlessChangeStreams: true,
                "failpoint.assertChangeStreamNssCollection": fpAssertChangeStreamNssColl
            }
        };

        // TODO SERVER-67634 avoid using change collection in the config server.
        const clusterTopology =
            newOptions.hasOwnProperty("configsvr") || newOptions.hasOwnProperty("shardsvr")
            ? "sharded-cluster"
            : "replica-set";

        const setParameter = setParameters[clusterTopology];
        newOptions.setParameter = Object.assign({}, newOptions.setParameter, setParameter);
        return this._originalStartSetAsync(newOptions, restart);
    };

    // Make a copy of the original 'initiate' function and then override it to issue
    // 'setChangeStreamState' command.
    this._originalInitiate = this.initiate;
    this.initiate = function(cfg, initCmd) {
        this._originalInitiate(cfg, initCmd, {doNotWaitForPrimaryOnlyServices: false});

        // Enable the change stream and verify that it is enabled.
        const adminDb = this.getPrimary().getDB("admin");
        assert.commandWorked(adminDb.runCommand({setChangeStreamState: 1, enabled: true}));
        assert.eq(assert.commandWorked(adminDb.runCommand({getChangeStreamState: 1})).enabled,
                  true);
    };
};

// Extend the new 'ReplSetTest' fixture with the properties of the original one.
Object.extend(ReplSetTest, originalReplSet);

// Make a copy of the original 'ShardingTest' fixture.
const originalShardingTest = ShardingTest;

ShardingTest = function(params) {
    // Helper to enable change stream on a particular shard.
    function enableChangeStream(shard) {
        const adminDb = shard.getPrimary().getDB("admin");
        assert.commandWorked(adminDb.runCommand({setChangeStreamState: 1, enabled: true}));
        assert.eq(assert.commandWorked(adminDb.runCommand({getChangeStreamState: 1})).enabled,
                  true);
    }

    // Call the original 'ShardingTest' fixture.
    const retShardingTest = originalShardingTest.apply(this, [params]);

    // For each shard, enable the change stream.
    this._rs.forEach((shardSvr) => {
        enableChangeStream(shardSvr.test);
    });

    // TODO SERVER-67634 Avoid enabling change collection in the config server.
    enableChangeStream(this.configRS);

    return retShardingTest;
};

// Extend the new 'ShardingTest' fixture with the properties of the original one.
Object.extend(ShardingTest, originalShardingTest);
})();