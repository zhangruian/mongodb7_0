/**
 * Checks that the $_testApiVersion expression used for API versioning testing
 * throws errors as expected.
 *
 * Tests which create views aren't expected to work when collections are implicitly sharded.
 * @tags: [requires_fcv_47, uses_api_parameters, assumes_unsharded_collection, sbe_incompatible]
 */

(function() {
"use strict";

const collName = "api_version_test_expression";
const coll = db[collName];
coll.drop();

for (let i = 0; i < 5; i++) {
    assert.commandWorked(coll.insert({num: i}));
}

// Assert error thrown when command specifies {apiStrict: true} and expression specifies {unstable:
// true}.
let pipeline = [{$project: {v: {$_testApiVersion: {unstable: true}}}}];
assert.commandFailedWithCode(
    db.runCommand(
        {aggregate: collName, pipeline: pipeline, cursor: {}, apiStrict: true, apiVersion: "1"}),
    ErrorCodes.APIStrictError);

// Assert error thrown when command specifies {apiDeprecationErrors: true} and expression specifies
// {deprecated: true}
pipeline = [{$project: {v: {$_testApiVersion: {deprecated: true}}}}];
assert.commandFailedWithCode(db.runCommand({
    aggregate: collName,
    pipeline: pipeline,
    cursor: {},
    apiDeprecationErrors: true,
    apiVersion: "1"
}),
                             ErrorCodes.APIDeprecationError);

// Test that command successfully runs to completion without any API parameters.
pipeline = [{$project: {v: {$_testApiVersion: {unstable: true}}}}];
assert.commandWorked(db.runCommand({aggregate: collName, pipeline: pipeline, cursor: {}}));

// Create a view with {apiStrict: true}.
db.view.drop();
assert.commandWorked(db.runCommand(
    {create: "view", viewOn: collName, pipeline: [], apiStrict: true, apiVersion: "1"}));
// This command will work because API parameters are not inherited from views.
assert.commandWorked(db.runCommand({aggregate: "view", pipeline: pipeline, cursor: {}}));
assert.commandFailedWithCode(
    db.runCommand(
        {aggregate: "view", pipeline: pipeline, cursor: {}, apiVersion: "1", apiStrict: true}),
    ErrorCodes.APIStrictError);

// Create a view with {unstable: true}.
db.unstableView.drop();
assert.commandWorked(db.runCommand({
    create: "unstableView",
    viewOn: collName,
    pipeline: pipeline,
    apiStrict: true,
    apiVersion: "1"
}));
assert.commandWorked(db.runCommand({aggregate: "unstableView", pipeline: [], cursor: {}}));
// This commmand will fail even with the empty pipeline because of the view.
assert.commandFailedWithCode(
    db.runCommand(
        {aggregate: "unstableView", pipeline: [], cursor: {}, apiVersion: "1", apiStrict: true}),
    ErrorCodes.APIStrictError);

// Create a validator containing the unstable test expression.
let validator = {$expr: {$_testApiVersion: {unstable: true}}};
let validatedCollName = collName + "_validated";

// Create the collection with the unstable validator, setting apiStrict: true does not have an
// effect.
db[validatedCollName].drop();
assert.commandWorked(db.runCommand(
    {create: validatedCollName, validator: validator, apiVersion: "1", apiStrict: true}));

// Run an insert command without any API version and verify that it is successful.
assert.commandWorked(
    db[validatedCollName].runCommand({insert: validatedCollName, documents: [{num: 1}]}));

// TODO SERVER-53218: Specifying apiStrict: true results in an error.
assert.commandWorked(db[validatedCollName].runCommand(
    {insert: validatedCollName, documents: [{num: 1}], apiVersion: "1", apiStrict: true}));

// Recreate the validator containing a deprecated test expression.
db[validatedCollName].drop();
validator = {
    $expr: {$_testApiVersion: {deprecated: true}}
};

// Create the collection with the unstable validator, setting apiDeprecationErrors : true does not
// have an effect.
assert.commandWorked(db.runCommand({
    create: validatedCollName,
    validator: validator,
    apiVersion: "1",
    apiDeprecationErrors: true,
}));

// Run an insert command without any API version and verify that it is successful.
assert.commandWorked(
    db[validatedCollName].runCommand({insert: validatedCollName, documents: [{num: 1}]}));

// TODO SERVER-53218: Specifying apiDeprecationErrors: true results in an error.
assert.commandWorked(db[validatedCollName].runCommand({
    insert: validatedCollName,
    documents: [{num: 1}],
    apiVersion: "1",
    apiDeprecationErrors: true
}));
})();
