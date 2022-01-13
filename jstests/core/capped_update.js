/**
 * Tests various update scenarios on capped collections:
 *  -- SERVER-20529: Ensure capped document sizes do not change
 *  -- SERVER-11983: Don't create _id field on capped updates
 * @tags: [
 *   requires_capped,
 *   uses_testing_only_commands,
 *   # godinsert and can't run under replication
 *   assumes_standalone_mongod,
 *   # capped collections connot be sharded
 *   assumes_unsharded_collection,
 * ]
 */

(function() {
'use strict';

const localDB = db.getSiblingDB("local");
const t = localDB.capped_update;
t.drop();

assert.commandWorked(
    localDB.createCollection(t.getName(), {capped: true, size: 1024, autoIndexId: false}));
assert.sameMembers([], t.getIndexes(), "the capped collection has indexes");

let docs = [];
for (let j = 1; j <= 10; j++) {
    docs.push({_id: j, s: "Hello, World!"});
}
assert.commandWorked(t.insert(docs));

assert.commandWorked(t.update({_id: 3}, {s: "Hello, Mongo!"}));  // Mongo is same length as World
assert.writeError(t.update({_id: 3}, {$set: {s: "Hello!"}}));
assert.writeError(t.update({_id: 10}, {}));
assert.writeError(t.update({_id: 10}, {s: "Hello, World!!!"}));

assert.commandWorked(localDB.runCommand({godinsert: t.getName(), obj: {a: 2}}));
let doc = t.findOne({a: 2});
assert(!doc.hasOwnProperty("_id"), "now has _id after godinsert: " + tojson(doc));
assert.commandWorked(t.update({a: 2}, {$inc: {a: 1}}));
doc = t.findOne({a: 3});
assert(!doc.hasOwnProperty("_id"), "now has _id after update: " + tojson(doc));
})();
