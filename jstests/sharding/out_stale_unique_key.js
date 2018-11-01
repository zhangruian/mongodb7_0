// Tests that an $out stage is able to default the uniqueKey to the correct value - even if one or
// more of the involved nodes has a stale cache of the routing information.
(function() {
    "use strict";

    const st = new ShardingTest({shards: 2, mongos: 2});

    const dbName = "out_stale_unique_key";
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));

    const source = st.s0.getDB(dbName).source;
    const target = st.s0.getDB(dbName).target;

    // Test that an $out through a stale mongos can still use the correct uniqueKey and succeed.
    (function testDefaultUniqueKeyIsRecent() {
        const freshMongos = st.s0;
        const staleMongos = st.s1;

        // Set up two collections for an aggregate with an $out: The source collection will be
        // unsharded and the target collection will be sharded amongst the two shards.
        const staleMongosDB = staleMongos.getDB(dbName);
        st.shardColl(source, {_id: 1}, {_id: 0}, {_id: 1});

        (function setupStaleMongos() {
            // Shard the collection through 'staleMongos', setting up 'staleMongos' to believe the
            // collection is sharded by {sk: 1, _id: 1}.
            assert.commandWorked(staleMongosDB.adminCommand(
                {shardCollection: target.getFullName(), key: {sk: 1, _id: 1}}));
            // Perform a query through that mongos to ensure the cache is populated.
            assert.eq(0, staleMongosDB[target.getName()].find().itcount());

            // Drop the collection from the other mongos - it is no longer sharded but the stale
            // mongos doesn't know that yet.
            target.drop();
        }());

        // At this point 'staleMongos' will believe that the target collection is sharded. This
        // should not prevent it from running an $out without a uniqueKey specified. Specifically,
        // the mongos should force a refresh of its cache before defaulting the uniqueKey.
        assert.commandWorked(source.insert({_id: 'seed'}));

        // If we had used the stale uniqueKey, this aggregation would fail since the documents do
        // not have an 'sk' field.
        assert.doesNotThrow(() => staleMongosDB[source.getName()].aggregate(
                                [{$out: {to: target.getName(), mode: 'insertDocuments'}}]));
        assert.eq(target.find().toArray(), [{_id: 'seed'}]);
        target.drop();
    }());

    // Test that if the collection is dropped and re-sharded during the course of the aggregation
    // that the operation will fail rather than proceed with the old shard key.
    function testEpochChangeDuringAgg({outSpec, failpoint, failpointData}) {
        target.drop();
        if (outSpec.hasOwnProperty("uniqueKey")) {
            assert.commandWorked(target.createIndex(outSpec.uniqueKey, {unique: true}));
            assert.commandWorked(
                st.s.adminCommand({shardCollection: target.getFullName(), key: outSpec.uniqueKey}));
        } else {
            assert.commandWorked(
                st.s.adminCommand({shardCollection: target.getFullName(), key: {sk: 1, _id: 1}}));
        }

        // Use a failpoint to make the query feeding into the aggregate hang while we drop the
        // collection.
        [st.rs0.getPrimary(), st.rs1.getPrimary()].forEach((mongod) => {
            assert.commandWorked(mongod.adminCommand(
                {configureFailPoint: failpoint, mode: "alwaysOn", data: failpointData || {}}));
        });
        let parallelShellJoiner;
        try {
            let parallelCode = `
                const source = db.getSiblingDB("${dbName}").${source.getName()};
                const error = assert.throws(() => source.aggregate([
                    {$addFields: {sk: "$_id"}},
                    {$out: ${tojsononeline(outSpec)}}
                ]));
                assert.eq(error.code, ErrorCodes.StaleEpoch);
            `;

            if (outSpec.hasOwnProperty("uniqueKey")) {
                // If a user specifies their own uniqueKey, we don't need to fail an aggregation if
                // the collection is dropped and recreated or the epoch otherwise changes. We are
                // allowed to fail such an operation should we choose to in the future, but for now
                // we don't expect to because we do not do anything special on mongos to ensure the
                // catalog cache is up to date, so do not want to attach mongos's believed epoch to
                // the command for the shards.
                parallelCode = `
                    const source = db.getSiblingDB("${dbName}").${source.getName()};
                    assert.doesNotThrow(() => source.aggregate([
                        {$addFields: {sk: "$_id"}},
                        {$out: ${tojsononeline(outSpec)}}
                    ]));
                `;
            }

            parallelShellJoiner = startParallelShell(parallelCode, st.s.port);

            // Wait for the merging $out to appear in the currentOp output from the shards. We
            // should see that the $out stage has an 'epoch' field serialized from the mongos.
            const getAggOps = function() {
                return st.s.getDB("admin")
                    .aggregate([
                        {$currentOp: {}},
                        {$match: {"cursor.originatingCommand.pipeline": {$exists: true}}}
                    ])
                    .toArray();
            };
            const hasOutRunning = function() {
                return getAggOps()
                           .filter((op) => {
                               const pipeline = op.cursor.originatingCommand.pipeline;
                               return pipeline.length > 0 &&
                                   pipeline[pipeline.length - 1].hasOwnProperty("$out");
                           })
                           .length >= 1;
            };
            assert.soon(hasOutRunning, () => tojson(getAggOps()));

            // Drop the collection so that the epoch changes.
            target.drop();
        } finally {
            [st.rs0.getPrimary(), st.rs1.getPrimary()].forEach((mongod) => {
                assert.commandWorked(
                    mongod.adminCommand({configureFailPoint: failpoint, mode: "off"}));
            });
        }
        parallelShellJoiner();
    }

    // Insert enough documents to force a yield.
    const bulk = source.initializeUnorderedBulkOp();
    for (let i = 0; i < 1000; ++i) {
        bulk.insert({_id: i});
    }
    assert.commandWorked(bulk.execute());

    testEpochChangeDuringAgg({
        outSpec: {to: target.getName(), mode: "insertDocuments"},
        failpoint: "setYieldAllLocksHang",
        failpointData: {namespace: source.getFullName()}
    });
    testEpochChangeDuringAgg({
        outSpec: {to: target.getName(), mode: "insertDocuments", uniqueKey: {sk: 1}},
        failpoint: "setYieldAllLocksHang",
        failpointData: {namespace: source.getFullName()}
    });
    testEpochChangeDuringAgg({
        outSpec: {to: target.getName(), mode: "replaceDocuments"},
        failpoint: "setYieldAllLocksHang",
        failpointData: {namespace: source.getFullName()}
    });
    testEpochChangeDuringAgg({
        outSpec: {to: target.getName(), mode: "replaceDocuments", uniqueKey: {sk: 1}},
        failpoint: "setYieldAllLocksHang",
        failpointData: {namespace: source.getFullName()}
    });

    // Test with some different failpoints to prove we will detect an epoch change in the middle of
    // the inserts or updates.
    testEpochChangeDuringAgg({
        outSpec: {to: target.getName(), mode: "insertDocuments"},
        failpoint: "hangDuringBatchInsert"
    });
    testEpochChangeDuringAgg({
        outSpec: {to: target.getName(), mode: "replaceDocuments"},
        failpoint: "hangDuringBatchUpdate"
    });

    st.stop();
}());
