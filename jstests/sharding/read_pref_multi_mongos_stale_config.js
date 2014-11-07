//
// Tests that a mongos will correctly retry a stale shard version when read preference is used
//

var st = new ShardingTest({shards : {rs0 : {quiet : ''}, rs1 : {quiet : ''}},
                           mongos : 2,
                           other : {mongosOptions : {verbose : 2}}});

var testDB1 = st.s0.getDB('test');
var testDB2 = st.s1.getDB('test');

// Trigger a query on mongos 1 so it will have a view of test.user as being unsharded.
testDB1.user.findOne();

testDB2.adminCommand({ enableSharding: 'test' });
testDB2.adminCommand({ shardCollection: 'test.user', key: { x: 1 }});

testDB2.adminCommand({ split: 'test.user', middle: { x: 100 }});

var configDB2 = st.s1.getDB('config');
var chunkToMove = configDB2.chunks.find().sort({ min: 1 }).next();
var toShard = configDB2.shards.findOne({ _id: { $ne: chunkToMove.shard }})._id;
testDB2.adminCommand({ moveChunk: 'test.user', to: toShard, find: { x: 50 }});

for (var x = 0; x < 200; x++) {
    testDB2.user.insert({ x: x });
}

// The testDB1 mongos does not know the chunk has been moved, and will retry
var cursor = testDB1.user.find({ x: 30 }).readPref('primary');
assert(cursor.hasNext());
assert.eq(30, cursor.next().x);

cursor = testDB1.user.find({ x: 130 }).readPref('primary');
assert(cursor.hasNext());
assert.eq(130, cursor.next().x);

st.stop();

