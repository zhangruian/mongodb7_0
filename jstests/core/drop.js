// Cannot implicitly shard accessed collections because of collection existing when none
// expected.
// @tags: [assumes_no_implicit_collection_creation_after_drop]

var coll = db.jstests_drop;

coll.drop();

res = coll.runCommand("drop");
assert(!res.ok, tojson(res));

assert.eq(0, coll.getIndexes().length, "A");
coll.save({});
assert.eq(1, coll.getIndexes().length, "B");
coll.createIndex({a: 1});
assert.eq(2, coll.getIndexes().length, "C");
assert.commandWorked(db.runCommand({drop: coll.getName()}));
assert.eq(0, coll.getIndexes().length, "D");

coll.createIndex({a: 1});
assert.eq(2, coll.getIndexes().length, "E");
assert.commandWorked(db.runCommand({dropIndexes: coll.getName(), index: "*"}), "drop indexes A");
assert.eq(1, coll.getIndexes().length, "G");

// make sure we can still use it
coll.save({});
assert.eq(1, coll.find().hint("_id_").toArray().length, "H");
