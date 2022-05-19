'use strict';

/**
 * - Shard several collections with different (random) configured maxChunkSize
 * - Perform continuous inserts of random amounts of data into the collections
 * - Verify that the balancer fairly redistributes data among available shards
 *
 * @tags: [
 *   requires_sharding,
 *   assumes_balancer_on,
 *   featureFlagBalanceAccordingToDataSize,
 *   does_not_support_stepdowns,
 *   requires_fcv_61,
 *  ]
 */

const bigString = 'X'.repeat(1024 * 1024 - 30);  // Almost 1MB, to create documents of exactly 1MB
const minChunkSizeMB = 1;
const maxChunkSizeMB = 10;
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
         * Insert into a test collection a random amount of documents (up to 10MB per iteration)
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

    /*
     * Create sharded collections with random maxChunkSizeMB (betwen 1MB and 10MB)
     */
    let setup = function(db, collName, cluster) {
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
                assert.soon(
                    function() {
                        return assert
                            .commandWorked(mongos.adminCommand({balancerCollectionStatus: ns}))
                            .balancerCompliant;
                    },
                    'Timed out waiting for collections to be balanced',
                    60000 * 5 /* timeout (5 minutes) */,
                    1000 /* interval */);

                const statsPipeline = [
                    {'$collStats': {'storageStats': {}}},
                    {
                        '$project': {
                            'shard': true,
                            'storageStats': {
                                'count': true,
                                'size': true,
                                'avgObjSize': true,
                                'numOrphanDocs': true
                            }
                        }
                    }
                ];

                // Get stats for the collection from each shard
                const storageStats = coll.aggregate(statsPipeline).toArray();
                let minSizeOnShardForCollection = Number.MAX_VALUE;
                let maxSizeOnShardForCollection = Number.MIN_VALUE;

                storageStats.forEach(function(shardStats) {
                    const orphansSize = shardStats['storageStats']['numOrphanDocs'] *
                        shardStats['storageStats']['avgObjSize'];
                    const size = shardStats['storageStats']['size'] - orphansSize;
                    if (size > maxSizeOnShardForCollection) {
                        maxSizeOnShardForCollection = size;
                    }
                    if (size < minSizeOnShardForCollection) {
                        minSizeOnShardForCollection = size;
                    }
                });

                // Check that there is no imbalance
                const collEntry = cluster.getDB('config').collections.findOne({'_id': ns});
                const errMsg = "ns=" + ns + ' , collEntry=' + JSON.stringify(collEntry) +
                    ', storageStats=' + JSON.stringify(storageStats);
                assert.lte(maxSizeOnShardForCollection - minSizeOnShardForCollection,
                           2 * collEntry.maxChunkSizeBytes,
                           errMsg);
            }

            assert(testedAtLeastOneCollection);
        }
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
