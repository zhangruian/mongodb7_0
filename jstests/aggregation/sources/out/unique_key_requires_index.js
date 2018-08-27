// Tests that the $out stage enforces that the uniqueKey argument can be used to uniquely identify
// documents by checking that there is a supporting unique, non-partial, collator-compatible index
// in the index catalog.
(function() {
    "use strict";

    const testDB = db.getSiblingDB("unique_key_requires_index");
    const source = testDB.source;
    source.drop();
    assert.commandWorked(source.insert([{_id: 0, a: 0}, {_id: 1, a: 1}]));

    function withEachOutMode(callback) {
        callback("replaceCollection");
        callback("insertDocuments");
        callback("replaceDocuments");
    }

    // Test that using {_id: 1} or not providing a unique key does not require any special indexes.
    (function simpleIdUniqueKeyOrDefaultShouldNotRequireIndexes() {
        function assertDefaultUniqueKeySuceeds({setupCallback, collName}) {
            // Legacy style $out - "replaceCollection".
            setupCallback();
            assert.doesNotThrow(() => source.aggregate([{$out: collName}]));

            withEachOutMode((mode) => {
                setupCallback();
                assert.doesNotThrow(() => source.aggregate([{$out: {to: collName, mode: mode}}]));
                setupCallback();
                assert.doesNotThrow(() => source.aggregate(
                                        [{$out: {to: collName, uniqueKey: {_id: 1}, mode: mode}}]));
            });
        }

        // Test that using {_id: 1} or not specifying a uniqueKey works for a collection which does
        // not exist.
        const non_existent = testDB.non_existent;
        assertDefaultUniqueKeySuceeds(
            {setupCallback: () => non_existent.drop(), collName: non_existent.getName()});

        const unindexed = testDB.unindexed;
        assertDefaultUniqueKeySuceeds({
            setupCallback: () => {
                unindexed.drop();
                assert.commandWorked(testDB.runCommand({create: unindexed.getName()}));
            },
            collName: unindexed.getName()
        });
    }());

    function assertUniqueKeyIsInvalid({uniqueKey, targetColl, options}) {
        let cmd = {
            aggregate: source.getName(),
            pipeline: [{$out: {mode: "replaceDocuments", uniqueKey: uniqueKey, to: targetColl}}],
            cursor: {}
        };
        withEachOutMode((mode) => {
            cmd.pipeline[0].$out.mode = mode;
            assert.commandFailedWithCode(testDB.runCommand(Object.merge(cmd, options)), 50938);
        });
    }

    // Test that a unique index on the unique key can be used to satisfy the requirement.
    (function basicUniqueIndexWorks() {
        const target = testDB.regular_unique;
        target.drop();
        assertUniqueKeyIsInvalid({uniqueKey: {_id: 1, a: 1}, targetColl: target.getName()});

        assert.commandWorked(target.createIndex({a: 1, _id: 1}, {unique: true}));
        assert.doesNotThrow(() => source.aggregate([
            {$out: {to: target.getName(), mode: "replaceDocuments", uniqueKey: {_id: 1, a: 1}}}
        ]));
        assert.doesNotThrow(() => source.aggregate([
            {$out: {to: target.getName(), mode: "replaceDocuments", uniqueKey: {a: 1, _id: 1}}}
        ]));

        assertUniqueKeyIsInvalid({uniqueKey: {_id: 1, a: 1, b: 1}, targetColl: target.getName()});
        assertUniqueKeyIsInvalid({uniqueKey: {a: 1, b: 1}, targetColl: target.getName()});
        assertUniqueKeyIsInvalid({uniqueKey: {b: 1}, targetColl: target.getName()});
        assertUniqueKeyIsInvalid({uniqueKey: {a: 1}, targetColl: target.getName()});
        assertUniqueKeyIsInvalid({uniqueKey: {}, targetColl: target.getName()});

        assert.commandWorked(target.dropIndex({a: 1, _id: 1}));
        assert.commandWorked(target.createIndex({a: 1}, {unique: true}));
        assert.doesNotThrow(
            () => source.aggregate(
                [{$out: {to: target.getName(), mode: "replaceDocuments", uniqueKey: {a: 1}}}]));

        // Create a non-unique index and make sure that doesn't work.
        assert.commandWorked(target.dropIndex({a: 1}));
        assert.commandWorked(target.createIndex({a: 1}));
        assertUniqueKeyIsInvalid({uniqueKey: {a: 1}, targetColl: target.getName()});
        assertUniqueKeyIsInvalid({uniqueKey: {a: 1, _id: 1}, targetColl: target.getName()});
    }());

    // Test that a unique index on the unique key cannot be used to satisfy the requirement if it is
    // a partial index.
    (function uniqueButPartialShouldNotWork() {
        const target = testDB.unique_but_partial_indexes;
        target.drop();
        assertUniqueKeyIsInvalid({uniqueKey: {a: 1}, targetColl: target.getName()});

        assert.commandWorked(
            target.createIndex({a: 1}, {unique: true, partialFilterExpression: {a: {$gte: 2}}}));
        assertUniqueKeyIsInvalid({uniqueKey: {a: 1}, targetColl: target.getName()});
        assertUniqueKeyIsInvalid({uniqueKey: {a: 1, _id: 1}, targetColl: target.getName()});
    }());

    // Test that a unique index on the unique key cannot be used to satisfy the requirement if it
    // has a different collation.
    (function indexMustMatchCollationOfOperation() {
        const target = testDB.collation_indexes;
        target.drop();
        assertUniqueKeyIsInvalid({uniqueKey: {a: 1}, targetColl: target.getName()});

        assert.commandWorked(
            target.createIndex({a: 1}, {unique: true, collation: {locale: "en_US"}}));
        assertUniqueKeyIsInvalid({uniqueKey: {a: 1}, targetColl: target.getName()});
        assertUniqueKeyIsInvalid({
            uniqueKey: {a: 1},
            targetColl: target.getName(),
            options: {collation: {locale: "en"}}
        });
        assertUniqueKeyIsInvalid({
            uniqueKey: {a: 1},
            targetColl: target.getName(),
            options: {collation: {locale: "simple"}}
        });
        assertUniqueKeyIsInvalid({
            uniqueKey: {a: 1},
            targetColl: target.getName(),
            options: {collation: {locale: "en_US", strength: 1}}
        });
        assert.doesNotThrow(
            () => source.aggregate(
                [{$out: {to: target.getName(), mode: "replaceDocuments", uniqueKey: {a: 1}}}],
                {collation: {locale: "en_US"}}));

        // Test that a non-unique index with the same collation cannot be used.
        assert.commandWorked(target.dropIndex({a: 1}));
        assert.commandWorked(target.createIndex({a: 1}, {collation: {locale: "en_US"}}));
        assertUniqueKeyIsInvalid({
            uniqueKey: {a: 1},
            targetColl: target.getName(),
            options: {collation: {locale: "en_US"}}
        });

        // Test that a collection-default collation will be applied to the index, but not the $out's
        // update or insert into that collection. The pipeline will inherit a collection-default
        // collation, but from the source collection, not the $out's target collection.
        target.drop();
        assert.commandWorked(
            testDB.runCommand({create: target.getName(), collation: {locale: "en_US"}}));
        assert.commandWorked(target.createIndex({a: 1}, {unique: true}));
        assertUniqueKeyIsInvalid({
            uniqueKey: {a: 1},
            targetColl: target.getName(),
        });
        assert.doesNotThrow(
            () => source.aggregate(
                [{$out: {to: target.getName(), mode: "replaceDocuments", uniqueKey: {a: 1}}}],
                {collation: {locale: "en_US"}}));

        // Test that when the source collection and foreign collection have the same default
        // collation, a unique index on the foreign collection can be used.
        const newSourceColl = testDB.new_source;
        newSourceColl.drop();
        assert.commandWorked(
            testDB.runCommand({create: newSourceColl.getName(), collation: {locale: "en_US"}}));
        assert.commandWorked(newSourceColl.insert([{_id: 1, a: 1}, {_id: 2, a: 2}]));
        // This aggregate does not specify a collation, but it should inherit the default collation
        // from 'newSourceColl', and therefor the index on 'target' should be eligible for use since
        // it has the same collation.
        assert.doesNotThrow(
            () => newSourceColl.aggregate(
                [{$out: {to: target.getName(), mode: "replaceDocuments", uniqueKey: {a: 1}}}]));

        // Test that an explicit "simple" collation can be used with an index without a collation.
        target.drop();
        assert.commandWorked(target.createIndex({a: 1}, {unique: true}));
        assert.doesNotThrow(
            () => source.aggregate(
                [{$out: {to: target.getName(), mode: "replaceDocuments", uniqueKey: {a: 1}}}],
                {collation: {locale: "simple"}}));
        assertUniqueKeyIsInvalid({
            uniqueKey: {a: 1},
            targetColl: target.getName(),
            options: {collation: {locale: "en_US"}}
        });
    }());

    // Test that a unique index which is not simply ascending/descending fields cannot be used for
    // the uniqueKey
    (function testSpecialIndexTypes() {
        const target = testDB.special_index_types;
        target.drop();

        assert.commandWorked(target.createIndex({a: 1, text: "text"}, {unique: true}));
        assertUniqueKeyIsInvalid({uniqueKey: {a: 1}, targetColl: target.getName()});
        assertUniqueKeyIsInvalid({uniqueKey: {a: 1, text: 1}, targetColl: target.getName()});
        assertUniqueKeyIsInvalid({uniqueKey: {text: 1}, targetColl: target.getName()});

        target.drop();
        assert.commandWorked(target.createIndex({a: 1, geo: "2dsphere"}, {unique: true}));
        assertUniqueKeyIsInvalid({uniqueKey: {a: 1}, targetColl: target.getName()});
        assertUniqueKeyIsInvalid({uniqueKey: {a: 1, geo: 1}, targetColl: target.getName()});
        assertUniqueKeyIsInvalid({uniqueKey: {geo: 1, a: 1}, targetColl: target.getName()});

        target.drop();
        assert.commandWorked(target.createIndex({geo: "2d"}, {unique: true}));
        assertUniqueKeyIsInvalid({uniqueKey: {a: 1, geo: 1}, targetColl: target.getName()});
        assertUniqueKeyIsInvalid({uniqueKey: {geo: 1}, targetColl: target.getName()});

        target.drop();
        assert.commandWorked(
            target.createIndex({geo: "geoHaystack", a: 1}, {unique: true, bucketSize: 5}));
        assertUniqueKeyIsInvalid({uniqueKey: {a: 1, geo: 1}, targetColl: target.getName()});
        assertUniqueKeyIsInvalid({uniqueKey: {geo: 1, a: 1}, targetColl: target.getName()});

        target.drop();
        // MongoDB does not support unique hashed indexes.
        assert.commandFailedWithCode(target.createIndex({a: "hashed"}, {unique: true}), 16764);
        assert.commandWorked(target.createIndex({a: "hashed"}));
        assertUniqueKeyIsInvalid({uniqueKey: {a: 1}, targetColl: target.getName()});
    }());

    // Test that a unique index with dotted field names can be used.
    (function testDottedFieldNames() {
        const target = testDB.dotted_field_paths;
        target.drop();

        assert.commandWorked(target.createIndex({a: 1, "b.c.d": -1}, {unique: true}));
        assertUniqueKeyIsInvalid({uniqueKey: {a: 1}, targetColl: target.getName()});
        assert.doesNotThrow(() => source.aggregate([
            {$project: {_id: 1, a: 1, b: {c: {d: "x"}}}},
            {
              $out: {
                  to: target.getName(),
                  mode: "replaceDocuments",
                  uniqueKey: {a: 1, "b.c.d": 1}
              }
            }
        ]));

        target.drop();
        assert.commandWorked(target.createIndex({"id.x": 1, "id.y": -1}, {unique: true}));
        assert.doesNotThrow(() => source.aggregate([
            {$group: {_id: {x: "$_id", y: "$a"}}},
            {$project: {id: "$_id"}},
            {
              $out: {
                  to: target.getName(),
                  mode: "replaceDocuments",
                  uniqueKey: {"id.x": 1, "id.y": 1}
              }
            }
        ]));
        assert.doesNotThrow(() => source.aggregate([
            {$group: {_id: {x: "$_id", y: "$a"}}},
            {$project: {id: "$_id"}},
            {
              $out: {
                  to: target.getName(),
                  mode: "replaceDocuments",
                  uniqueKey: {"id.y": 1, "id.x": 1}
              }
            }
        ]));

        // Test that we cannot use arrays with a dotted path within an $out.
        target.drop();
        assert.commandWorked(target.createIndex({"b.c": 1}, {unique: true}));
        withEachOutMode((mode) => {
            assert.commandFailedWithCode(testDB.runCommand({
                aggregate: source.getName(),
                pipeline: [
                    {$replaceRoot: {newRoot: {b: [{c: 1}, {c: 2}]}}},
                    {$out: {to: target.getName(), mode: mode, uniqueKey: {"b.c": 1}}}
                ],
                cursor: {}
            }),
                                         50905);
        });
    }());

    // Test that a unique index that is multikey can still be used.
    (function testMultikeyIndex() {
        const target = testDB.multikey_index;
        target.drop();

        assert.commandWorked(target.createIndex({"a.b": 1}, {unique: true}));
        assert.doesNotThrow(() => source.aggregate([
            {$project: {_id: 1, "a.b": "$a"}},
            {$out: {to: target.getName(), mode: "replaceDocuments", uniqueKey: {"a.b": 1}}}
        ]));
        assert.commandWorked(target.insert({_id: "TARGET", a: [{b: "hi"}, {b: "hello"}]}));
        assert.commandWorked(source.insert({a: "hi", proofOfUpdate: "PROOF"}));
        assert.doesNotThrow(() => source.aggregate([
            {$project: {_id: 0, proofOfUpdate: "PROOF", "a.b": "$a"}},
            {$out: {to: target.getName(), mode: "replaceDocuments", uniqueKey: {"a.b": 1}}}
        ]));
        assert.docEq(target.findOne({"a.b": "hi", proofOfUpdate: "PROOF"}),
                     {_id: "TARGET", a: {b: "hi"}, proofOfUpdate: "PROOF"});
    }());

    // Test that a unique index that is sparse can still be used.
    (function testSparseIndex() {
        const target = testDB.multikey_index;
        target.drop();

        assert.commandWorked(target.createIndex({a: 1}, {unique: true, sparse: true}));
        assert.doesNotThrow(
            () => source.aggregate(
                [{$out: {to: target.getName(), mode: "replaceDocuments", uniqueKey: {a: 1}}}]));
        assert.commandWorked(target.insert([{b: 1, c: 1}, {a: null}, {d: 4}]));
        assert.doesNotThrow(
            () => source.aggregate(
                [{$out: {to: target.getName(), mode: "replaceDocuments", uniqueKey: {a: 1}}}]));
    }());
}());
