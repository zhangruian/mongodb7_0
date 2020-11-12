// end-to-end tests on index bounds for numerical values
// should handle numerical extremes
// such as Number.MAX_VALUE and Infinity

t = db.indexboundsnumberedgecases;

t.drop();

t.ensureIndex({a: 1});

t.save({a: -Infinity});
t.save({a: -Number.MAX_VALUE});
t.save({a: 1});
t.save({a: Number.MAX_VALUE});
t.save({a: Infinity});

// index bounds generated by query planner are
// validated in unit tests

// lte

assert.eq(1, t.find({a: {$lte: -Infinity}}).itcount());
assert.eq(2, t.find({a: {$lte: -Number.MAX_VALUE}}).itcount());
assert.eq(3, t.find({a: {$lte: 1}}).itcount());
assert.eq(4, t.find({a: {$lte: Number.MAX_VALUE}}).itcount());
assert.eq(5, t.find({a: {$lte: Infinity}}).itcount());

// lt

assert.eq(0, t.find({a: {$lt: -Infinity}}).itcount());
assert.eq(1, t.find({a: {$lt: -Number.MAX_VALUE}}).itcount());
assert.eq(2, t.find({a: {$lt: 1}}).itcount());
assert.eq(3, t.find({a: {$lt: Number.MAX_VALUE}}).itcount());
assert.eq(4, t.find({a: {$lt: Infinity}}).itcount());

// gt

assert.eq(0, t.find({a: {$gt: Infinity}}).itcount());
assert.eq(1, t.find({a: {$gt: Number.MAX_VALUE}}).itcount());
assert.eq(2, t.find({a: {$gt: 1}}).itcount());
assert.eq(3, t.find({a: {$gt: -Number.MAX_VALUE}}).itcount());
assert.eq(4, t.find({a: {$gt: -Infinity}}).itcount());

// gte

assert.eq(1, t.find({a: {$gte: Infinity}}).itcount());
assert.eq(2, t.find({a: {$gte: Number.MAX_VALUE}}).itcount());
assert.eq(3, t.find({a: {$gte: 1}}).itcount());
assert.eq(4, t.find({a: {$gte: -Number.MAX_VALUE}}).itcount());
assert.eq(5, t.find({a: {$gte: -Infinity}}).itcount());
