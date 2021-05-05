/**
 * Test that:
 * 1. Text indexes properly validate the index spec used to create them.
 * 2. Text indexes properly enforce a schema on the language_override field.
 * 3. Collections may have at most one text index.
 * 4. Text indexes properly handle large documents.
 * 5. Bad weights test cases.
 *
 * @tags: [
 *   # Cannot implicitly shard accessed collections because of collection existing when none
 *   # expected.
 *   assumes_no_implicit_collection_creation_after_drop,
 *   # Has operations which may never complete in stepdown/kill/terminate transaction tests.
 *   operations_longer_than_stepdown_interval_in_txns,
 *   # Uses index building in background
 *   requires_background_index,
 *   requires_fcv_49,
 * ]
 */

var coll = db.fts_index;
var indexName = "textIndex";
coll.drop();
coll.getDB().createCollection(coll.getName());

//
// 1. Text indexes properly validate the index spec used to create them.
//

// Spec passes text-specific index validation.
assert.commandWorked(coll.createIndex({a: "text"}, {name: indexName, default_language: "spanish"}));
assert.eq(1,
          coll.getIndexes()
              .filter(function(z) {
                  return z.name == indexName;
              })
              .length);
coll.dropIndexes();

// Spec fails text-specific index validation ("spanglish" unrecognized).
assert.commandFailed(
    coll.createIndex({a: "text"}, {name: indexName, default_language: "spanglish"}));
assert.eq(0,
          coll.getIndexes()
              .filter(function(z) {
                  return z.name == indexName;
              })
              .length);
coll.dropIndexes();

// Spec passes general index validation.
assert.commandWorked(coll.createIndex({"$**": "text"}, {name: indexName}));
assert.eq(1,
          coll.getIndexes()
              .filter(function(z) {
                  return z.name == indexName;
              })
              .length);
coll.dropIndexes();

// Spec fails general index validation ("a.$**" invalid field name for key).
assert.commandFailed(coll.createIndex({"a.$**": "text"}, {name: indexName}));
assert.eq(0,
          coll.getIndexes()
              .filter(function(z) {
                  return z.name == indexName;
              })
              .length);
coll.dropIndexes();

// SERVER-19519 Spec fails if '_fts' is specified on a non-text index.
assert.commandFailed(coll.createIndex({_fts: 1}, {name: indexName}));
assert.eq(0,
          coll.getIndexes()
              .filter(function(z) {
                  return z.name == indexName;
              })
              .length);
coll.dropIndexes();
assert.commandFailed(coll.createIndex({_fts: "text"}, {name: indexName}));
assert.eq(0,
          coll.getIndexes()
              .filter(function(z) {
                  return z.name == indexName;
              })
              .length);
coll.dropIndexes();

//
// 2. Text indexes properly enforce a schema on the language_override field.
//

// Can create a text index on a collection where no documents have invalid language_override.
coll.insert({a: ""});
coll.insert({a: "", language: "spanish"});
assert.commandWorked(coll.createIndex({a: "text"}));
coll.drop();

// Can't create a text index on a collection containing document with an invalid language_override.
coll.insert({a: "", language: "spanglish"});
assert.commandFailed(coll.createIndex({a: "text"}));
coll.drop();

// Can insert documents with valid language_override into text-indexed collection.
assert.commandWorked(coll.createIndex({a: "text"}));
coll.insert({a: ""});
assert.commandWorked(coll.insert({a: "", language: "spanish"}));
coll.drop();

// Can't insert documents with invalid language_override into text-indexed collection.
assert.commandWorked(coll.createIndex({a: "text"}));
assert.writeError(coll.insert({a: "", language: "spanglish"}));
coll.drop();

//
// 3. Collections may have at most one text index.
//

// createIndex() becomes a no-op on an equivalent index spec.
assert.commandWorked(coll.getDB().createCollection(coll.getName()));
assert.eq(1, coll.getIndexes().length);
assert.commandWorked(coll.createIndex({a: 1, b: "text", c: 1}));
assert.eq(2, coll.getIndexes().length);
assert.commandWorked(coll.createIndex({a: 1, b: "text", c: 1}));
assert.eq(2, coll.getIndexes().length);
assert.commandWorked(coll.createIndex({a: 1, b: "text", c: 1}, {background: true}));
assert.eq(2, coll.getIndexes().length);
assert.commandFailedWithCode(coll.createIndex({a: 1, b: 1, c: "text"}),
                             ErrorCodes.CannotCreateIndex);
assert.commandFailedWithCode(
    coll.createIndex({a: 1, _fts: "text", _ftsx: 1, c: 1}, {weights: {b: 1}}),
    ErrorCodes.IndexOptionsConflict);
assert.eq(2, coll.getIndexes().length);
assert.commandWorked(coll.createIndex({a: 1, b: "text", c: 1}, {default_language: "english"}));
assert.eq(2, coll.getIndexes().length);
assert.commandWorked(coll.createIndex({a: 1, b: "text", c: 1}, {textIndexVersion: 2}));
assert.eq(2, coll.getIndexes().length);
assert.commandWorked(coll.createIndex({a: 1, b: "text", c: 1}, {language_override: "language"}));
assert.eq(2, coll.getIndexes().length);
coll.drop();

// Two index specs are also considered equivalent if they differ only in 'textIndexVersion', and
// createIndex() becomes a no-op on repeated requests that only differ in this way.
assert.commandWorked(coll.getDB().createCollection(coll.getName()));
assert.eq(1, coll.getIndexes().length);
assert.commandWorked(coll.createIndex({a: "text"}, {textIndexVersion: 1}));
assert.eq(2, coll.getIndexes().length);
assert.commandWorked(coll.createIndex({a: "text"}, {textIndexVersion: 2}));
assert.eq(2, coll.getIndexes().length);
assert.commandWorked(coll.createIndex({a: "text"}, {textIndexVersion: 3}));
assert.eq(2, coll.getIndexes().length);
assert.commandWorked(coll.createIndex({a: "text"}));
assert.eq(2, coll.getIndexes().length);
coll.drop();

assert.commandWorked(coll.getDB().createCollection(coll.getName()));
assert.eq(1, coll.getIndexes().length);
assert.commandWorked(coll.createIndex({a: "text"}, {textIndexVersion: 3}));
assert.eq(2, coll.getIndexes().length);
assert.commandWorked(coll.createIndex({a: "text"}, {textIndexVersion: 2}));
assert.eq(2, coll.getIndexes().length);
assert.commandWorked(coll.createIndex({a: "text"}, {textIndexVersion: 1}));
assert.eq(2, coll.getIndexes().length);
assert.commandWorked(coll.createIndex({a: "text"}));
assert.eq(2, coll.getIndexes().length);
coll.drop();

// createIndex() fails if a second text index would be built.
assert.commandWorked(coll.getDB().createCollection(coll.getName()));
assert.eq(1, coll.getIndexes().length);
assert.commandWorked(coll.createIndex({a: 1, b: "text", c: 1}));
assert.eq(2, coll.getIndexes().length);
assert.commandFailed(coll.createIndex({a: 1, _fts: "text", _ftsx: 1, c: 1}, {weights: {d: 1}}));
assert.commandFailed(coll.createIndex({a: 1, b: "text", c: 1}, {default_language: "none"}));
assert.commandFailed(coll.createIndex({a: 1, b: "text", c: 1}, {language_override: "idioma"}));
assert.commandFailed(coll.createIndex({a: 1, b: "text", c: 1}, {weights: {d: 1}}));
assert.commandFailed(coll.createIndex({a: 1, b: "text", d: 1}));
assert.commandFailed(coll.createIndex({a: 1, d: "text", c: 1}));
assert.commandFailed(coll.createIndex({b: "text"}));
assert.commandFailed(coll.createIndex({b: "text", c: 1}));
assert.commandFailed(coll.createIndex({a: 1, b: "text"}));

coll.dropIndexes();

//
// 4. Text indexes properly handle large keys.
//

assert.commandWorked(coll.createIndex({a: "text"}));

var longstring = "";
var longstring2 = "";
for (var i = 0; i < 1024 * 1024; ++i) {
    longstring = longstring + "a";
    longstring2 = longstring2 + "b";
}
coll.insert({a: longstring});
coll.insert({a: longstring2});
assert.eq(1, coll.find({$text: {$search: longstring}}).itcount(), "long string not found in index");
coll.dropIndexes();

//
// 5. Bad weights test cases.
//
assert.commandFailed(coll.createIndex({a: 1, _fts: "text", _ftsx: 1, c: 1}, {weights: {}}));
assert.commandFailed(coll.createIndex({a: 1, _fts: "text", _ftsx: 1, c: 1}));

// The 'weights' parameter should only be allowed when the index is a text index.
coll.drop();
assert.commandFailedWithCode(coll.createIndex({a: 1, c: 1}, {weights: {d: 1}}),
                             ErrorCodes.CannotCreateIndex);
coll.getIndexes();
coll.drop();
assert.commandFailedWithCode(coll.createIndex({a: 1, c: 1}, {weights: "$**"}),
                             ErrorCodes.CannotCreateIndex);
coll.getIndexes();
coll.drop();
assert.commandFailedWithCode(coll.createIndex({a: 1, c: 1}, {weights: {}}),
                             ErrorCodes.CannotCreateIndex);
coll.getIndexes();
coll.drop();
assert.commandFailedWithCode(coll.createIndex({a: 1, c: 1}, {weights: "$foo"}),
                             ErrorCodes.CannotCreateIndex);
coll.getIndexes();

coll.drop();

//
// 6. Bad direction value for non-text key in compound index.
//
assert.commandFailedWithCode(coll.createIndex({a: "text", b: Number.MAX_VALUE}),
                             ErrorCodes.CannotCreateIndex);
assert.commandFailedWithCode(coll.createIndex({a: "text", b: -Number.MAX_VALUE}),
                             ErrorCodes.CannotCreateIndex);
coll.getIndexes();

coll.drop();
