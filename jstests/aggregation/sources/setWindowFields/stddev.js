/**
 * Test that standard deviation works as a window function.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/window_function_helpers.js");

const featureEnabled =
    assert.commandWorked(db.adminCommand({getParameter: 1, featureFlagWindowFunctions: 1}))
        .featureFlagWindowFunctions.value;
if (!featureEnabled) {
    jsTestLog("Skipping test because the window function feature flag is disabled");
    return;
}

const coll = db[jsTestName()];
coll.drop();

// Create a collection of tickers and prices.
const nDocsPerTicker = 10;
seedWithTickerData(coll, nDocsPerTicker);

// Run the suite of partition and bounds tests against the $stdDevPop function.
testAccumAgainstGroup(coll, "$stdDevPop", nDocsPerTicker);
// Run the suite of partition and bounds tests against the $stdDevSamp function.
testAccumAgainstGroup(coll, "$stdDevSamp", nDocsPerTicker);

// Test that $stdDevPop and $stdDevSamp return null for windows which do not contain numeric values.
let results =
    coll.aggregate([
            {$addFields: {str: "hiya"}},
            {
                $setWindowFields: {
                    sortBy: {ts: 1},
                    output: {
                        stdDevPop:
                            {$stdDevPop: {input: "$str", documents: ["unbounded", "current"]}},
                        stdDevSamp:
                            {$stdDevSamp: {input: "$str", documents: ["unbounded", "current"]}},
                    }
                }
            }
        ])
        .toArray();
for (let index = 0; index < results.length; index++) {
    assert.eq(null, results[index].stdDevPop);
    assert.eq(null, results[index].stdDevSamp);
}
})();
