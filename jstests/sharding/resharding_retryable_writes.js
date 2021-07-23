/**
 * Verify that the cloning phase of a resharding operation takes at least
 * reshardingMinimumOperationDurationMillis to complete. This will also indirectly verify that the
 * txnCloners were not started until after waiting for reshardingMinimumOperationDurationMillis to
 * elapse.
 *
 * @tags: [uses_atclustertime]
 */

(function() {
"use strict";

load("jstests/sharding/libs/resharding_test_fixture.js");
load("jstests/libs/retryable_writes_util.js");

function runTest(minimumOperationDurationMS, shouldReshardInPlace) {
    jsTest.log(`Running test for minimumReshardingDuration = ${
        minimumOperationDurationMS} and reshardInPlace = ${shouldReshardInPlace}`);

    const reshardingTest = new ReshardingTest({
        numDonors: 2,
        numRecipients: 2,
        reshardInPlace: shouldReshardInPlace,
        minimumOperationDurationMS: minimumOperationDurationMS
    });
    reshardingTest.setup();

    const donorShardNames = reshardingTest.donorShardNames;
    const sourceCollection = reshardingTest.createShardedCollection({
        ns: "reshardingDb.coll",
        shardKeyPattern: {oldKey: 1},
        chunks: [
            {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
            {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[1]},
        ],
    });

    assert.commandWorked(sourceCollection.insert([
        {_id: "stays on shard0", oldKey: -10, newKey: -10, counter: 0},
        {_id: "moves to shard0", oldKey: 10, newKey: -10, counter: 0},
    ]));

    const mongos = sourceCollection.getMongo();
    const session = mongos.startSession({causalConsistency: false, retryWrites: false});
    const sessionCollection = session.getDatabase(sourceCollection.getDB().getName())
                                  .getCollection(sourceCollection.getName());
    const updateCommand = {
        update: sourceCollection.getName(),
        updates: [
            {q: {_id: "stays on shard0"}, u: {$inc: {counter: 1}}},
            {q: {_id: "moves to shard0"}, u: {$inc: {counter: 1}}},
        ],
        txnNumber: NumberLong(1)
    };

    function runRetryableWrite(phase, expectedErrorCode = ErrorCodes.OK) {
        RetryableWritesUtil.runRetryableWrite(sessionCollection, updateCommand, expectedErrorCode);

        const docs = sourceCollection.find().toArray();
        assert.eq(2, docs.length, {docs});

        for (const doc of docs) {
            assert.eq(
                1,
                doc.counter,
                {message: `retryable write executed more than once ${phase}`, id: doc._id, docs});
        }
    }

    runRetryableWrite("before resharding");

    const recipientShardNames = reshardingTest.recipientShardNames;
    reshardingTest.withReshardingInBackground(
        {
            newShardKeyPattern: {newKey: 1},
            newChunks: [
                {min: {newKey: MinKey}, max: {newKey: 0}, shard: recipientShardNames[0]},
                {min: {newKey: 0}, max: {newKey: MaxKey}, shard: recipientShardNames[1]},
            ],
        },
        () => {
            runRetryableWrite("during resharding");

            assert.soon(() => {
                const coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne({
                    ns: sourceCollection.getFullName()
                });

                return coordinatorDoc !== null && coordinatorDoc.cloneTimestamp !== undefined;
            });

            runRetryableWrite("during resharding after cloneTimestamp was chosen");

            assert.soon(() => {
                const coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne({
                    ns: sourceCollection.getFullName()
                });

                return coordinatorDoc !== null && coordinatorDoc.state === "cloning";
            });

            runRetryableWrite("during resharding when in coordinator in cloning state");

            let startTime = Date.now();

            assert.soon(() => {
                const coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne({
                    ns: sourceCollection.getFullName()
                });

                return coordinatorDoc !== null && coordinatorDoc.state === "applying";
            });

            const epsilon = 5000;
            const elapsed = Date.now() - startTime;
            assert.gt(elapsed, minimumOperationDurationMS - epsilon);
            runRetryableWrite("during resharding after collection cloning had finished",
                              ErrorCodes.IncompleteTransactionHistory);
        });

    runRetryableWrite("after resharding", ErrorCodes.IncompleteTransactionHistory);

    reshardingTest.teardown();
}
const minimumOperationDurationMS = 30000;
runTest(minimumOperationDurationMS, true);
runTest(minimumOperationDurationMS, false);
})();
