/**
 * Encapsulates testing that verifies the behavior of createIndexes with the 'clustered' option. The
 * 'clustered' option may not be used to create a clustered collection impicitly via createIndexes.
 */
const CreateIndexesClusteredTest = (function() {
    "use strict";

    /**
     * Tests that createIndex with the 'clustered' option fails when a collection exists and is not
     * clustered.
     */
    const runNonClusteredCollectionTest = function(testDB, collName) {
        assertDropCollection(testDB, collName);
        const testColl = testDB[collName];
        assert.commandWorked(testDB.createCollection(collName));

        // Start with the collection empty.
        assert.commandFailedWithCode(
            testColl.createIndex({_id: 1}, {clustered: true, unique: true}), 6100905);
        assert.commandFailedWithCode(testColl.createIndex({a: 1}, {clustered: true, unique: true}),
                                     6100905);

        // Insert some docs. Sometimes empty collections are treated as special when it comes to
        // index builds.
        const batchSize = 100;
        const bulk = testColl.initializeUnorderedBulkOp();
        for (let i = 0; i < batchSize; i++) {
            bulk.insert({_id: i, a: -i});
        }
        assert.commandWorked(bulk.execute());
        assert.commandFailedWithCode(
            testColl.createIndex({_id: 1}, {clustered: true, unique: true}), 6100905);
        assert.commandFailedWithCode(testColl.createIndex({a: 1}, {clustered: true, unique: true}),
                                     6100905);
    };

    /**
     * Tests running createIndex on a clustered collection.
     */
    const runClusteredCollectionTest = function(testDB, collName) {
        assertDropCollection(testDB, collName);

        // The createIndex 'clustered' option is disallowed for implicit collection creation.
        assert.commandFailedWithCode(testDB.runCommand({
            createIndexes: collName,
            indexes: [{key: {_id: 1}, name: "_id_", clustered: true, unique: true}],
        }),
                                     6100900);

        // Create the clustered collection.
        const createOptions = {
            clusteredIndex: {key: {_id: 1}, name: "theClusterKeyName", unique: true}
        };
        assert.commandWorked(testDB.createCollection(collName, createOptions));

        // Confirm we start out with a valid clustered collection.
        const fullCreateOptions = ClusteredCollectionUtil.constructFullCreateOptions(createOptions);
        ClusteredCollectionUtil.validateListCollections(testDB, collName, fullCreateOptions);
        ClusteredCollectionUtil.validateListIndexes(testDB, collName, fullCreateOptions);

        const testColl = testDB[collName];

        // createIndex on the cluster key is a no-op.
        assert.commandWorked(testColl.createIndex({_id: 1}));
        ClusteredCollectionUtil.validateListIndexes(testDB, collName, fullCreateOptions);

        // createIndex on the cluster key with the 'clustered' option is a no-op.
        assert.commandWorked(testColl.createIndex({_id: 1}, {clustered: true, unique: true}));

        // 'clustered' is not a valid option for an index not on the cluster key.
        assert.commandFailedWithCode(
            testColl.createIndex({notMyIndex: 1}, {clustered: true, unique: true}), 6243700);

        // Insert some docs. Empty collections are treated as special (single phase) when
        // it comes to index builds.
        const batchSize = 100;
        const bulk = testColl.initializeUnorderedBulkOp();
        for (let i = 0; i < batchSize; i++) {
            bulk.insert({_id: i, a: -i});
        }
        assert.commandWorked(bulk.execute());

        assert.commandWorked(testColl.createIndex({_id: 1}));
        assert.commandWorked(testColl.createIndex({_id: 1}, {clustered: true, unique: true}));

        // 'clustered' is still not a valid option for an index not on the cluster key.
        assert.commandFailedWithCode(testColl.createIndex({a: 1}, {clustered: true, unique: true}),
                                     6243700);

        assert.commandFailedWithCode(testDB.runCommand({
            createIndexes: collName,
            indexes: [
                {key: {_id: 1}, name: "_id_", clustered: true, unique: true},
                {key: {a: 1}, name: "a_1", clustered: true, unique: true}
            ],
        }),
                                     6243700);

        // Note: this a quirk of how we handle the 'name' field for indexes of {_id: 1}. The
        // createIndex is still a no-op, and the specified name is discarded.
        //
        // Only in implicit collection creation on a non-existent collection can createIndex create
        // a clusteredIndex with a custom name.
        assert.commandWorked(testColl.createIndex({_id: 1}, {name: "notTheClusterKeyName"}));

        ClusteredCollectionUtil.validateListIndexes(testDB, collName, fullCreateOptions);
    };

    /**
     * Runs test cases that are agnostic to whether the database is replicated or not.
     */
    const runBaseTests = function(testDB, collName) {
        runNonClusteredCollectionTest(testDB, collName);
        runClusteredCollectionTest(testDB, collName);
    };

    return {
        runBaseTests: runBaseTests,
    };
})();
