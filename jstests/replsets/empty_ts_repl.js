/**
 * Tests how replication handles inserts and updates with "Timestamp(0,0)" values.
 *
 * @tags: [
 *  backport_required_multiversion,
 * ]
 */

const dbName = "test";
const collName = "coll";

const rst = new ReplSetTest({
    name: jsTestName(),
    nodes: 2,
});
rst.startSet();
rst.initiate();

const primaryColl = rst.getPrimary().getDB(dbName).getCollection(collName);
const secondaryColl = rst.getSecondary().getDB(dbName).getCollection(collName);
const emptyTs = Timestamp(0, 0);

// Insert several documents. For the first document inserted (_id=1), we expect that mongod
// will set field "a" to the current timestamp.
assert.commandWorked(
    primaryColl.insert([{_id: 1, a: emptyTs}, {_id: 2, a: 0}, {_id: 4, a: 0}, {_id: 5, a: 0}]));

// Wait for all the inserted documents to replicate to the secondaries.
rst.awaitReplication();

// Use a replacement-style update to update _id=2. We expect that mongod will set field "a" to
// the current timestamp.
assert.commandWorked(primaryColl.update({_id: 2}, {a: emptyTs}));

// Do a replacement-style upsert to add a new document with _id=3. We expect that mongod will set
// field "a" to the current timestamp.
assert.commandWorked(primaryColl.update({_id: 3}, {a: emptyTs}, {upsert: true}));

// For the rest of the commands below, we expect empty timestamp values in field "a" will be
// preserved as-is.

// Use a pipeline-style update to update _id=4.
assert.commandWorked(primaryColl.update({_id: 4}, {$set: {a: emptyTs}}));

// Use $internalApplyOplogUpdate to update _id=5.
assert.commandWorked(primaryColl.update(
    {_id: 5}, [{$_internalApplyOplogUpdate: {oplogUpdate: {$v: 2, diff: {i: {a: emptyTs}}}}}]));

// Do a pipeline-style upsert to add a new document with _id=6.
assert.commandWorked(primaryColl.update({_id: 6}, {$set: {a: emptyTs}}, {upsert: true}));

// Do an upsert that uses $internalApplyOplogUpdate to add a new document _id=7.
assert.commandWorked(primaryColl.update(
    {_id: 7},
    [{$_internalApplyOplogUpdate: {oplogUpdate: {$v: 2, diff: {i: {a: emptyTs}}}}}],
    {upsert: true}));

rst.awaitReplication();

// Verify that all inserts, updates, and upserts behaved the way we expect and that they
// all were replicated correctly to the secondaries.
for (let i = 1; i <= 7; ++i) {
    const result = primaryColl.findOne({_id: i});
    const secondaryResult = secondaryColl.findOne({_id: i});

    assert.eq(tojson(result), tojson(secondaryResult), () => "_id=" + i);

    if (i >= 4) {
        assert.eq(tojson(result.a), tojson(emptyTs), "_id=" + i);
    } else {
        assert.neq(tojson(result.a), tojson(emptyTs), "_id=" + i);
    }
}

rst.stopSet();
