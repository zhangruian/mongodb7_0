var defragmentationUtil = (function() {
    load("jstests/sharding/libs/find_chunks_util.js");

    // This function creates a randomized, fragmented collection. It does not necessarily make a
    // collection with exactly numChunks chunks nor exactly numZones zones.
    let createFragmentedCollection = function(
        mongos, ns, numChunks, maxChunkFillMB, numZones, docSizeBytes, chunkSpacing) {
        assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {key: 1}}));

        createAndDistributeChunks(mongos, ns, numChunks, chunkSpacing);
        createRandomZones(mongos, ns, numZones, chunkSpacing);
        fillChunksToRandomSize(mongos, ns, docSizeBytes, maxChunkFillMB);
    };

    let createAndDistributeChunks = function(mongos, ns, numChunks, chunkSpacing) {
        const shards = mongos.getCollection('config.shards').find().toArray();
        for (let i = -Math.floor(numChunks / 2); i <= Math.floor(numChunks / 2); i++) {
            assert.commandWorked(mongos.adminCommand({split: ns, middle: {key: i * chunkSpacing}}));
            assert.soon(() => {
                let toShard = Random.randInt(shards.length);
                let res = mongos.adminCommand(
                    {moveChunk: ns, find: {key: i * chunkSpacing}, to: shards[toShard]._id});
                return res.ok;
            });
        }
    };

    let createRandomZones = function(mongos, ns, numZones, chunkSpacing) {
        for (let i = -Math.floor(numZones / 2); i < Math.ceil(numZones / 2); i++) {
            let zoneName = "Zone" + i;
            let shardForZone =
                findChunksUtil
                    .findOneChunkByNs(mongos.getDB('config'), ns, {min: {key: i * chunkSpacing}})
                    .shard;
            assert.commandWorked(
                mongos.adminCommand({addShardToZone: shardForZone, zone: zoneName}));
            assert.commandWorked(mongos.adminCommand({
                updateZoneKeyRange: ns,
                min: {key: i * chunkSpacing},
                max: {key: i * chunkSpacing + chunkSpacing},
                zone: zoneName
            }));
        }
    };

    let fillChunksToRandomSize = function(mongos, ns, docSizeBytes, maxChunkFillMB) {
        const chunks = findChunksUtil.findChunksByNs(mongos.getDB('config'), ns).toArray();
        const bigString = "X".repeat(docSizeBytes);
        const coll = mongos.getCollection(ns);
        let bulk = coll.initializeUnorderedBulkOp();
        chunks.forEach((chunk) => {
            let chunkSize = Random.randInt(maxChunkFillMB);
            let docsPerChunk = (chunkSize * 1024 * 1024) / docSizeBytes;
            if (docsPerChunk === 0) {
                return;
            }
            let minKey = chunk["min"]["key"];
            if (minKey === MinKey)
                minKey = Number.MIN_SAFE_INTEGER;
            let maxKey = chunk["max"]["key"];
            if (maxKey === MaxKey)
                maxKey = Number.MAX_SAFE_INTEGER;
            let currKey = minKey;
            const gap = ((maxKey - minKey) / docsPerChunk);
            for (let i = 0; i < docsPerChunk; i++) {
                bulk.insert({key: currKey, longString: bigString});
                currKey += gap;
            }
        });
        assert.commandWorked(bulk.execute());
    };

    let checkPostDefragmentationState = function(mongos, ns, maxChunkSizeMB, shardKey) {
        const oversizedChunkThreshold = maxChunkSizeMB * 1024 * 1024 * 4 / 3;
        const chunks =
            findChunksUtil.findChunksByNs(mongos.getDB('config'), ns).sort({shardKey: 1}).toArray();
        const coll = mongos.getCollection(ns);
        const collStats = assert.commandWorked(coll.getDB().runCommand({collStats: ns}));
        const avgObjSize = collStats.avgObjSize;
        let checkForOversizedChunk = function(
            coll, chunk, shardKey, avgObjSize, oversizedChunkThreshold) {
            let chunkSize =
                coll.countDocuments({key: {$gte: chunk.min[shardKey], $lt: chunk.max[shardKey]}}) *
                avgObjSize;
            assert.lte(
                chunkSize,
                oversizedChunkThreshold,
                `Chunk ${tojson(chunk)} has size ${
                    chunkSize} which is greater than max chunk size of ${oversizedChunkThreshold}`);
        };
        for (let i = 1; i < chunks.length; i++) {
            let chunk1 = chunks[i - 1];
            let chunk2 = chunks[i];
            // Check for mergeable chunks with combined size less than maxChunkSize
            if (chunk1["shard"] === chunk2["shard"] && chunk1["max"] === chunk2["min"]) {
                let chunk1Zone = getZoneForRange(mongos, ns, chunk1.min, chunk1.max);
                let chunk2Zone = getZoneForRange(mongos, ns, chunk2.min, chunk2.max);
                if (chunk1Zone === chunk2Zone) {
                    let combinedDataSize = coll.countDocuments({
                        shardKey: {$gte: chunk1.min[shardKey], $lt: chunk2.max[shardKey]}
                    }) * avgObjSize;
                    assert.lte(
                        combinedDataSize,
                        oversizedChunkThreshold,
                        `Chunks ${tojson(chunk1)} and ${
                            tojson(chunk2)} are mergeable with combined size ${combinedDataSize}`);
                }
            }
            // Check for oversized chunks
            checkForOversizedChunk(coll, chunk1, shardKey, avgObjSize, oversizedChunkThreshold);
        }
        const lastChunk = chunks[chunks.length - 1];
        checkForOversizedChunk(coll, lastChunk, shardKey, avgObjSize, oversizedChunkThreshold);
    };

    let getZoneForRange = function(mongos, ns, minKey, maxKey) {
        const tags = mongos.getDB('config')
                         .tags.find({ns: ns, min: {$lte: minKey}, max: {$gte: maxKey}})
                         .toArray();
        assert.leq(tags.length, 1);
        if (tags.length === 1) {
            return tags[0].tag;
        }
        return null;
    };

    let waitForEndOfDefragmentation = function(mongos, ns) {
        jsTest.log("Waiting end of defragmentation for " + ns);
        assert.soon(function() {
            let balancerStatus =
                assert.commandWorked(mongos.adminCommand({balancerCollectionStatus: ns}));
            return balancerStatus.balancerCompliant ||
                balancerStatus.firstComplianceViolation !== 'defragmentingChunks';
        });
        jsTest.log("Defragmentation completed for " + ns);
    };

    return {
        createFragmentedCollection,
        checkPostDefragmentationState,
        waitForEndOfDefragmentation,
    };
})();
