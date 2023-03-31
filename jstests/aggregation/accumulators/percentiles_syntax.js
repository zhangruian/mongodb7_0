/**
 * Tests for the $percentile accumulator syntax.
 * @tags: [
 *   featureFlagApproxPercentiles,
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");

const coll = db[jsTestName()];
coll.drop();
// These tests don't validate the computed $percentile but we need a result to be produced in
// order to check its format.
coll.insert({x: 42});

/**
 * Tests to check that invalid $percentile specifications are rejected.
 */
function assertInvalidSyntax(percentileSpec, msg) {
    assert.commandFailed(
        coll.runCommand("aggregate",
                        {pipeline: [{$group: {_id: null, p: percentileSpec}}], cursor: {}}),
        msg);
}

assertInvalidSyntax({$percentile: 0.5}, "Should fail if $percentile is not an object");

assertInvalidSyntax({$percentile: {input: "$x", method: "approximate"}},
                    "Should fail if $percentile is missing 'p' field");

assertInvalidSyntax({$percentile: {p: [0.5], method: "approximate"}},
                    "Should fail if $percentile is missing 'input' field");

assertInvalidSyntax({$percentile: {p: [0.5], input: "$x"}},
                    "Should fail if $percentile is missing 'method' field");

assertInvalidSyntax({$percentile: {p: [0.5], input: "$x", method: "approximate", extras: 42}},
                    "Should fail if $percentile contains an unexpected field");

assertInvalidSyntax({$percentile: {p: 0.5, input: "$x", method: "approximate"}},
                    "Should fail if 'p' field in $percentile isn't array");

assertInvalidSyntax({$percentile: {p: [], input: "$x", method: "approximate"}},
                    "Should fail if 'p' field in $percentile is an empty array");

assertInvalidSyntax(
    {$percentile: {p: [0.5, "foo"], input: "$x", method: "approximate"}},
    "Should fail if 'p' field in $percentile is an array with a non-numeric element");

assertInvalidSyntax(
    {$percentile: {p: [0.5, 10], input: "$x", method: "approximate"}},
    "Should fail if 'p' field in $percentile is an array with any value outside of [0, 1] range");

assertInvalidSyntax({$percentile: {p: [0.5, 0.7], input: "$x", method: 42}},
                    "Should fail if 'method' field isn't a string");

assertInvalidSyntax({$percentile: {p: [0.5, 0.7], input: "$x", method: "fancy"}},
                    "Should fail if 'method' isn't one of _predefined_ strings");

/**
 * Tests for $median. $median desugars to $percentile with the field p:[0.5] added, and therefore
 * has similar syntax to $percentile.
 */

assertInvalidSyntax({$median: {p: [0.5], input: "$x", method: "approximate"}},
                    "Should fail if 'p' is defined");

assertInvalidSyntax({$median: {method: "approximate"}},
                    "Should fail if $median is missing 'input' field");

assertInvalidSyntax({$median: {input: "$x"}}, "Should fail if $median is missing 'method' field");

assertInvalidSyntax({$median: {input: "$x", method: "approximate", extras: 42}},
                    "Should fail if $median contains an unexpected field");
/**
 * Test that valid $percentile specifications are accepted. The results, i.e. semantics, are tested
 * elsewhere and would cover all of the cases below, we are providing them here nonetheless for
 * completeness.
 */
function assertValidSyntax(percentileSpec, msg) {
    assert.commandWorked(
        coll.runCommand("aggregate",
                        {pipeline: [{$group: {_id: null, p: percentileSpec}}], cursor: {}}),
        msg);
}

assertValidSyntax(
    {$percentile: {p: [0.0, 0.0001, 0.5, 0.995, 1.0], input: "$x", method: "approximate"}},
    "Should be able to specify an array of percentiles");

assertValidSyntax(
    {$percentile: {p: [0.5, 0.9], input: {$divide: ["$x", 2]}, method: "approximate"}},
    "Should be able to specify 'input' as an expression");

assertValidSyntax({$percentile: {p: [0.5, 0.9], input: "x", method: "approximate"}},
                  "Non-numeric inputs should be gracefully ignored");

/**
 * Tests for $median. $median desugars to $percentile with the field p:[0.5] added.
 */

assertValidSyntax({$median: {input: "$x", method: "approximate"}}, "Simple base case for $median.");
})();
