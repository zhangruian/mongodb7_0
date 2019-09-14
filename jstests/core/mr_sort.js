// Cannot implicitly shard accessed collections because the "limit" option to the "mapReduce"
// command cannot be used on a sharded collection.
// @tags: [
//   assumes_unsharded_collection,
//   # mapReduce does not support afterClusterTime.
//   does_not_support_causal_consistency,
//   does_not_support_stepdowns,
//   uses_map_reduce_with_temp_collections,
// ]

load("jstests/libs/fixture_helpers.js");  // For FixtureHelpers.

// Do not execute new path on the passthrough suites.
if (!FixtureHelpers.isMongos(db)) {
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryUseAggMapReduce: true}));
}

t = db.mr_sort;
t.drop();

t.ensureIndex({x: 1});

t.insert({x: 1});
t.insert({x: 10});
t.insert({x: 2});
t.insert({x: 9});
t.insert({x: 3});
t.insert({x: 8});
t.insert({x: 4});
t.insert({x: 7});
t.insert({x: 5});
t.insert({x: 6});

m = function() {
    emit("a", this.x);
};

r = function(k, v) {
    return Array.sum(v);
};

res = t.mapReduce(m, r, "mr_sort_out ");
x = res.convertToSingleObject();
res.drop();
assert.eq({"a": 55}, x, "A1");

res = t.mapReduce(m, r, {out: "mr_sort_out", query: {x: {$lt: 3}}});
x = res.convertToSingleObject();
res.drop();
assert.eq({"a": 3}, x, "A2");

res = t.mapReduce(m, r, {out: "mr_sort_out", sort: {x: 1}, limit: 2});
x = res.convertToSingleObject();
res.drop();
assert.eq({"a": 3}, x, "A3");

assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryUseAggMapReduce: false}));
