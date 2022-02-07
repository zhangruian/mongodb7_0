/**
 * Tests the collectionUUID parameter of the reshardCollection command.
 *
 * @tags: [
 *   featureFlagCommandsAcceptCollectionUUID,
 * ]
 */
(function() {
'use strict';

const st = new ShardingTest({shards: 1});
const mongos = st.s0;
const db = mongos.getDB(jsTestName());
const coll = db['coll'];
assert.commandWorked(mongos.adminCommand({enableSharding: db.getName()}));

const oldKeyDoc = {
    a: 1
};
const newKeyDoc = {
    b: 1
};

const resetColl = function(shardedColl) {
    shardedColl.drop();
    assert.commandWorked(shardedColl.insert({a: 1, b: 2}));
    assert.commandWorked(mongos.getCollection(shardedColl.getFullName()).createIndex(oldKeyDoc));
    assert.commandWorked(mongos.getCollection(shardedColl.getFullName()).createIndex(newKeyDoc));
    assert.commandWorked(
        mongos.adminCommand({shardCollection: shardedColl.getFullName(), key: oldKeyDoc}));
};

const uuid = function() {
    return assert.commandWorked(db.runCommand({listCollections: 1}))
        .cursor.firstBatch.find(c => c.name === coll.getName())
        .info.uuid;
};

resetColl(coll);

// The command succeeds when provided with the correct collection UUID.
assert.commandWorked(mongos.adminCommand(
    {reshardCollection: coll.getFullName(), key: newKeyDoc, collectionUUID: uuid()}));

// The command fails when provided with a UUID with no corresponding collection.
resetColl(coll);
const nonexistentUUID = UUID();
let res = assert.commandFailedWithCode(mongos.adminCommand({
    reshardCollection: coll.getFullName(),
    key: newKeyDoc,
    collectionUUID: nonexistentUUID,
}),
                                       ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.collectionUUID, nonexistentUUID);
assert.eq(res.actualNamespace, "");

// The command fails when provided with a different collection's UUID.
const coll2 = db['coll_2'];
resetColl(coll2);
res = assert.commandFailedWithCode(mongos.adminCommand({
    reshardCollection: coll2.getFullName(),
    key: newKeyDoc,
    collectionUUID: uuid(),
}),
                                   ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.collectionUUID, uuid());
assert.eq(res.actualNamespace, coll.getFullName());

// The command fails when the provided UUID corresponds to a different collection, even if the
// provided namespace is a view.
const view = db['view'];
assert.commandWorked(db.createView(view.getName(), coll.getName(), []));
res = assert.commandFailedWithCode(mongos.adminCommand({
    reshardCollection: view.getFullName(),
    key: newKeyDoc,
    collectionUUID: uuid(),
}),
                                   ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.collectionUUID, uuid());
assert.eq(res.actualNamespace, coll.getFullName());

st.stop();
})();
