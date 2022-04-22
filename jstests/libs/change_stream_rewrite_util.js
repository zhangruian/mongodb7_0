/**
 * Helper functions that are used in change streams rewrite test cases.
 */

load("jstests/libs/collection_drop_recreate.js");  // For assertDropAndRecreateCollection.
load("jstests/libs/fixture_helpers.js");           // For isMongos.

// Function which generates a write workload on the specified collection, including all events that
// a change stream may consume. Assumes that the specified collection does not already exist.
function generateChangeStreamWriteWorkload(db, collName, numDocs, includInvalidatingEvents = true) {
    // If this is a sharded passthrough, make sure we shard on something other than _id so that a
    // non-id field appears in the documentKey. This will generate 'create' and 'shardCollection'.
    if (FixtureHelpers.isMongos(db)) {
        assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));
        assert.commandWorked(db.adminCommand(
            {shardCollection: `${db.getName()}.${collName}`, key: {shardKey: "hashed"}}));
    }

    // If the collection hasn't already been created, do so here.
    let testColl = assertCreateCollection(db, collName);

    // Build an index, collMod it, then drop it.
    assert.commandWorked(testColl.createIndex({a: 1}));
    assert.commandWorked(db.runCommand({
        collMod: testColl.getName(),
        index: {keyPattern: {a: 1}, hidden: true, expireAfterSeconds: 500}
    }));
    assert.commandWorked(testColl.dropIndex({a: 1}));

    // Insert some documents.
    for (let i = 0; i < numDocs; ++i) {
        assert.commandWorked(testColl.insert(
            {_id: i, shardKey: i, a: [1, [2], {b: 3}], f1: {subField: true}, f2: false}));
    }

    // Update half of them. We generate these updates individually so that they generate different
    // values for the 'updatedFields', 'removedFields' and 'truncatedArrays' subfields.
    const updateSpecs = [
        [{$set: {f2: true}}],                                // only populates 'updatedFields'
        [{$unset: ["f1"]}],                                  // only populates 'removedFields'
        [{$set: {a: [1, [2]]}}],                             // only populates 'truncatedArrays'
        [{$set: {a: [1, [2]], f2: true}}, {$unset: ["f1"]}]  // populates all fields
    ];
    for (let i = 0; i < numDocs / 2; ++i) {
        assert.commandWorked(
            testColl.update({_id: i, shardKey: i}, updateSpecs[(i % updateSpecs.length)]));
    }

    // Replace the other half.
    for (let i = numDocs / 2; i < numDocs; ++i) {
        assert.commandWorked(testColl.replaceOne({_id: i, shardKey: i}, {_id: i, shardKey: i}));
    }

    // Delete half of the updated documents.
    for (let i = 0; i < numDocs / 4; ++i) {
        assert.commandWorked(testColl.remove({_id: i, shardKey: i}));
    }

    // If the caller is prepared to handle potential invalidations, include the following events.
    if (includInvalidatingEvents) {
        // Rename the collection.
        const collNameAfterRename = `${testColl.getName()}_renamed`;
        assert.commandWorked(testColl.renameCollection(collNameAfterRename));
        testColl = db[collNameAfterRename];

        // Rename it back.
        assert.commandWorked(testColl.renameCollection(collName));
        testColl = db[collName];

        // Drop the collection.
        assert(testColl.drop());

        // Drop the database.
        assert.commandWorked(db.dropDatabase());
    }
    return testColl;
}

// Helper function to fully exhaust a change stream from the specified point and return all events.
// Assumes that all relevant events can fit into a single 16MB batch.
function getAllChangeStreamEvents(db, extraPipelineStages = [], csOptions = {}, resumeToken) {
    // Open a whole-cluster stream based on the supplied arguments.
    const csCursor = db.getMongo().watch(
        extraPipelineStages,
        Object.assign({resumeAfter: resumeToken, maxAwaitTimeMS: 1}, csOptions));

    // Run getMore until the post-batch resume token advances. In a sharded passthrough, this will
    // guarantee that all shards have returned results, and we expect all results to fit into a
    // single batch, so we know we have exhausted the stream.
    while (bsonWoCompare(csCursor._postBatchResumeToken, resumeToken) == 0) {
        csCursor.hasNext();  // runs a getMore
    }

    // Close the cursor since we have already retrieved all results.
    csCursor.close();

    // Extract all events from the streams. Since the cursor is closed, it will not attempt to
    // retrieve any more batches from the server.
    return csCursor.toArray();
}

// Helper function to check whether this value is a plain old javascript object.
function isPlainObject(value) {
    return (value && typeof (value) == "object" && value.constructor === Object);
}

// Verifies the number of change streams events returned from a particular shard.
function assertNumChangeStreamDocsReturnedFromShard(stats, shardName, expectedTotalReturned) {
    assert(stats.shards.hasOwnProperty(shardName), stats);
    const stages = stats.shards[shardName].stages;
    const lastStage = stages[stages.length - 1];
    assert.eq(lastStage.nReturned, expectedTotalReturned, stages);
}

// Verifies the number of oplog events read by a particular shard.
function assertNumMatchingOplogEventsForShard(stats, shardName, expectedTotalReturned) {
    assert(stats.shards.hasOwnProperty(shardName), stats);
    assert.eq(Object.keys(stats.shards[shardName].stages[0])[0], "$cursor", stats);
    const executionStats = stats.shards[shardName].stages[0].$cursor.executionStats;
    assert.eq(executionStats.nReturned, expectedTotalReturned, executionStats);
}

// Returns a newly created sharded collection sharded by caller provided shard key.
function createShardedCollection(shardingTest, shardKey, dbName, collName, splitAt) {
    const db = shardingTest.s.getDB(dbName);
    assertDropAndRecreateCollection(db, collName);

    const coll = db.getCollection(collName);
    assert.commandWorked(coll.createIndex({[shardKey]: 1}));

    shardingTest.ensurePrimaryShard(dbName, shardingTest.shard0.shardName);

    // Shard the test collection and split it into two chunks: one that contains all {shardKey: <lt
    // splitAt>} documents and one that contains all {shardKey: <gte splitAt>} documents.
    shardingTest.shardColl(
        collName,
        {[shardKey]: 1} /* shard key */,
        {[shardKey]: splitAt} /* split at */,
        {[shardKey]: splitAt} /* move the chunk containing {shardKey: splitAt} to its own shard */,
        dbName,
        true);
    return coll;
}