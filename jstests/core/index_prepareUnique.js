/**
 * Tests that the createIndex command accepts a prepareUnique field and works accordingly
 * then.
 *
 * @tags: [requires_fcv_53]
 */
(function() {
"use strict";

const coll = db.index_prepareUnique;
coll.drop();

const collModIndexUniqueEnabled = assert
                                      .commandWorked(db.getMongo().adminCommand(
                                          {getParameter: 1, featureFlagCollModIndexUnique: 1}))
                                      .featureFlagCollModIndexUnique.value;

if (!collModIndexUniqueEnabled) {
    jsTestLog('Skipping test because the collMod unique index feature flag is disabled.');
    return;
}

assert.commandWorked(coll.insert({_id: 0, a: 1}));

// Starts rejecting new duplicate keys.
assert.commandWorked(coll.createIndex({a: 1}, {prepareUnique: true}));

// Disallows creating another index on the same key with a different option.
assert.commandFailedWithCode(coll.createIndex({a: 1}, {prepareUnique: false}),
                             ErrorCodes.IndexOptionsConflict);

// Checks the index is rejecting duplicates but accepting other keys.
assert.commandFailedWithCode(coll.insert({_id: 1, a: 1}), ErrorCodes.DuplicateKey);
assert.commandWorked(coll.insert({_id: 2, a: 2}));

// Checks that the prepareUnique field exists when getIndexes is called.
let indexesWithComments = coll.getIndexes().filter(function(doc) {
    return friendlyEqual(doc.prepareUnique, true);
});
assert.eq(1, indexesWithComments.length);

// Removes the field and checks the index works as a regular index.
assert.commandWorked(coll.runCommand(
    {collMod: "index_prepareUnique", index: {keyPattern: {a: 1}, prepareUnique: false}}));
assert.commandWorked(coll.insert({_id: 1, a: 1}));

indexesWithComments = coll.getIndexes().filter(function(doc) {
    return friendlyEqual(doc.prepareUnique, true);
});
assert.eq(0, indexesWithComments.length);
})();