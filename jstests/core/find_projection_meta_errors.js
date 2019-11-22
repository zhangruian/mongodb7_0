// Basic tests for errors when parsing the $meta projection.
// @tags: [requires_fcv_44]
(function() {
"use strict";

const coll = db.find_projection_meta_errors;
coll.drop();

assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.insert({a: 2}));

assert.commandFailedWithCode(
    db.runCommand({find: coll.getName(), projection: {score: {$meta: "some garbage"}}}), 17308);
}());
