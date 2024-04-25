'use strict';

/**
 * - Shard several collections with different (random) configured maxChunkSize
 * - Perform continuous inserts of random amounts of data into the collections
 * - Verify that the balancer fairly redistributes data among available shards
 *
 * @tags: [
 *   requires_sharding,
 *   assumes_balancer_on,
 *   antithesis_incompatible,
 *   resource_intensive,
 *  ]
 */

const isCodeCoverageEnabled = buildInfo().buildEnvironment.ccflags.includes('-ftest-coverage');
const isSanitizerEnabled = buildInfo().buildEnvironment.ccflags.includes('-fsanitize');
const slowTestVariant = isCodeCoverageEnabled || isSanitizerEnabled;

const maxChunkSizeMB = slowTestVariant ? 8 : 10;
const collectionBalancedTimeoutMS =
    slowTestVariant ? 10 * 60 * 1000 /* 10min */ : 5 * 60 * 1000 /* 5min */;

const bigString = 'X'.repeat(1024 * 1024 - 30);  // Almost 1MB, to create documents of exactly 1MB
const dbNames = ['db0', 'db1'];
const collNames = ['collA', 'collB', 'collC'];

/*
 * Get a random db/coll name from the test lists.
 *
 * Using the thread id to introduce more randomness: it has been observed that concurrent calls to
 * Random.randInt(array.length) are returning too often the same number to different threads.
 */
function getRandomDbName(tid) {
    return dbNames[Random.randInt(tid * tid) % dbNames.length];
}
function getRandomCollName(tid) {
    return collNames[Random.randInt(tid * tid) % collNames.length];
}

var $config = (function() {
    let states = {
        /*
         * Insert into a test collection a random amount of documents
         */
        insert: function(db, collName, connCache) {
            const dbName = getRandomDbName(this.tid);
            db = db.getSiblingDB(dbName);
            collName = getRandomCollName(this.tid);
            const coll = db[collName];

            const numDocs = Random.randInt(maxChunkSizeMB - 1) + 1;
            let insertBulkOp = coll.initializeUnorderedBulkOp();
            for (let i = 0; i < numDocs; ++i) {
                insertBulkOp.insert({s: bigString});
            }

            assertAlways.commandWorked(insertBulkOp.execute());
        },
    };

    let defaultBalancerShouldReturnRandomMigrations;

    /*
     * Create sharded collections with random maxChunkSizeMB
     */
    let setup = function(db, collName, cluster) {
        cluster.executeOnConfigNodes((db) => {
            defaultBalancerShouldReturnRandomMigrations =
                assert
                    .commandWorked(db.adminCommand({
                        getParameter: 1,
                        'failpoint.balancerShouldReturnRandomMigrations': 1
                    }))['failpoint.balancerShouldReturnRandomMigrations']
                    .mode;

            // If the failpoint is enabled on this suite, disable it because this test relies on the
            // balancer taking correct decisions.
            if (defaultBalancerShouldReturnRandomMigrations === 1) {
                assert.commandWorked(db.adminCommand(
                    {configureFailPoint: 'balancerShouldReturnRandomMigrations', mode: 'off'}));
            }
        });

        const mongos = cluster.getDB('config').getMongo();
        const shardNames = Object.keys(cluster.getSerializedCluster().shards);
        const numShards = shardNames.length;

        for (let i = 0; i < dbNames.length; i++) {
            // Initialize database
            const dbName = dbNames[i];
            const newDb = db.getSiblingDB(dbName);
            newDb.adminCommand({enablesharding: dbName, primaryShard: shardNames[i % numShards]});

            for (let j = 0; j < collNames.length; j++) {
                // Shard collection
                collName = collNames[j];
                const coll = newDb[collName];
                const ns = coll.getFullName();
                db.adminCommand({shardCollection: ns, key: {_id: 1}});

                // Configure random maxChunkSize
                const randomMaxChunkSizeMB = Random.randInt(maxChunkSizeMB - 1) + 1;
                assert.commandWorked(mongos.adminCommand({
                    configureCollectionBalancing: ns,
                    chunkSize: randomMaxChunkSizeMB,
                }));
            }
        }
    };

    /*
     * Verify that the balancer fairly redistributes data among available shards: the
     * collection size difference between two shards must be at most 2 * maxChunkSize
     */
    let teardown = function(db, collName, cluster) {
        const mongos = cluster.getDB('config').getMongo();
        // Sentinel variable to make sure not all collections have been skipped
        let testedAtLeastOneCollection = false;
        for (let i = 0; i < dbNames.length; i++) {
            const dbName = dbNames[i];
            for (let j = 0; j < collNames.length; j++) {
                collName = collNames[j];
                const ns = dbName + '.' + collName;

                const coll = mongos.getCollection(ns);
                if (coll.countDocuments({}) === 0) {
                    // Skip empty collections
                    continue;
                }
                testedAtLeastOneCollection = true;

                // Wait for collection to be considered balanced
                sh.awaitCollectionBalance(
                    coll, collectionBalancedTimeoutMS, 1000 /* 1s interval */);
                sh.verifyCollectionIsBalanced(coll);
            }

            assert(testedAtLeastOneCollection);
        }

        cluster.executeOnConfigNodes((db) => {
            // Reset the failpoint to its original value.
            if (defaultBalancerShouldReturnRandomMigrations === 1) {
                defaultBalancerShouldReturnRandomMigrations =
                    assert
                        .commandWorked(db.adminCommand({
                            configureFailPoint: 'balancerShouldReturnRandomMigrations',
                            mode: 'alwaysOn'
                        }))
                        .was;
            }
        });
    };

    let transitions = {insert: {insert: 1.0}};

    return {
        threadCount: 5,
        iterations: 8,
        startState: 'insert',
        states: states,
        transitions: transitions,
        data: {},
        setup: setup,
        teardown: teardown,
        passConnectionCache: true
    };
})();
