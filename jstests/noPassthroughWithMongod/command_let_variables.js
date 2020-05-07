// Tests that commands like find, aggregate and update accepts a 'let' parameter which defines
// variables for use in expressions within the command.
// TODO SERVER-46707: move this back to core after let params work in sharded aggreggate.
// @tags: [assumes_against_mongod_not_mongos, requires_fcv46]

(function() {
"use strict";

const coll = db.update_let_variables;
coll.drop();

assert.commandWorked(coll.insert([
    {
        Species: "Blackbird (Turdus merula)",
        population_trends: [
            {term: {start: 1970, end: 2014}, pct_change: -16, annual: -0.38, trend: "no change"},
            {term: {start: 2009, end: 2014}, pct_change: -2, annual: -0.36, trend: "no change"}
        ]
    },
    {
        Species: "Bullfinch (Pyrrhula pyrrhula)",
        population_trends: [
            {term: {start: 1970, end: 2014}, pct_change: -39, annual: -1.13, trend: "no change"},
            {term: {start: 2009, end: 2014}, pct_change: 12, annual: 2.38, trend: "weak increase"}
        ]
    },
    {
        Species: "Chaffinch (Fringilla coelebs)",
        population_trends: [
            {term: {start: 1970, end: 2014}, pct_change: 27, annual: 0.55, trend: "no change"},
            {term: {start: 2009, end: 2014}, pct_change: -7, annual: -1.49, trend: "weak decline"}
        ]
    },
    {
        Species: "Song Thrush (Turdus philomelos)",
        population_trends: [
            {term: {start: 1970, end: 2014}, pct_change: -53, annual: -1.7, trend: "weak decline"},
            {term: {start: 2009, end: 2014}, pct_change: -4, annual: -0.88, trend: "no change"}
        ]
    }
]));

// Aggregate tests
const pipeline = [
    {$project: {_id: 0}},
    {$unwind: "$population_trends"},
    {$match: {$expr: {$eq: ["$population_trends.trend", "$$target_trend"]}}},
    {$sort: {Species: 1}}
];
let expectedResults = [{
    Species: "Bullfinch (Pyrrhula pyrrhula)",
    population_trends:
        {term: {start: 2009, end: 2014}, pct_change: 12, annual: 2.38, trend: "weak increase"}
}];
assert.eq(coll.aggregate(pipeline, {let : {target_trend: "weak increase"}}).toArray(),
          expectedResults);

expectedResults = [
    {
        Species: "Chaffinch (Fringilla coelebs)",
        population_trends:
            {term: {start: 2009, end: 2014}, pct_change: -7, annual: -1.49, trend: "weak decline"}
    },
    {
        Species: "Song Thrush (Turdus philomelos)",
        population_trends:
            {term: {start: 1970, end: 2014}, pct_change: -53, annual: -1.7, trend: "weak decline"}
    }
];
assert.eq(coll.aggregate(pipeline, {let : {target_trend: "weak decline"}}).toArray(),
          expectedResults);

// Test that if runtimeConstants and let are both specified, both will coexist.
let constants = {
    localNow: new Date(),
    clusterTime: new Timestamp(0, 0),
};

assert.eq(
    coll.aggregate(pipeline, {runtimeConstants: constants, let : {target_trend: "weak decline"}})
        .toArray(),
    expectedResults);

// Test that undefined let params in the pipeline fail gracefully.
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: pipeline,
    runtimeConstants: constants,
    cursor: {},
    let : {cat: "not_a_bird"}
}),
                             17276);

// Test null and empty let parameters
const pipeline_no_lets = [
    {$project: {_id: 0}},
    {$unwind: "$population_trends"},
    {$match: {$expr: {$eq: ["$population_trends.trend", "weak decline"]}}},
    {$sort: {Species: 1}}
];
assert.eq(coll.aggregate(pipeline_no_lets, {runtimeConstants: constants, let : {}}).toArray(),
          expectedResults);

assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: pipeline_no_lets,
    runtimeConstants: constants,
    cursor: {},
    let : null
}),
                             ErrorCodes.TypeMismatch);

// findAndModify
assert.commandWorked(coll.insert({Species: "spy_bird"}));
let result = db.runCommand({
    findAndModify: coll.getName(),
    let : {target_species: "spy_bird"},
    query: {$expr: {$eq: ["$Species", "$$target_species"]}},
    update: {Species: "questionable_bird"},
    fields: {_id: 0},
    new: true
});
assert.eq(result.value, {Species: "questionable_bird"}, result);

result = db.runCommand({
    findAndModify: coll.getName(),
    let : {species_name: "not_a_bird", realSpecies: "dino"},
    query: {$expr: {$eq: ["$Species", "questionable_bird"]}},
    update: [{$project: {Species: "$$species_name"}}, {$addFields: {suspect: "$$realSpecies"}}],
    fields: {_id: 0},
    new: true
});
assert.eq(result.value, {Species: "not_a_bird", suspect: "dino"}, result);

// Delete
result = assert.commandWorked(db.runCommand({
    delete: coll.getName(),
    let : {target_species: "not_a_bird"},
    deletes: [{q: {$expr: {$eq: ["$Species", "$$target_species"]}}, limit: 0}]
}));

// Update
assert.commandWorked(db.runCommand({
    update: coll.getName(),
    let : {target_species: "Song Thrush (Turdus philomelos)", new_name: "Song Thrush"},
    updates: [
        {q: {$expr: {$eq: ["$Species", "$$target_species"]}}, u: [{$set: {Species: "$$new_name"}}]}
    ]
}));

assert.commandWorked(db.runCommand({
    update: coll.getName(),
    let : {target_species: "Song Thrush (Turdus philomelos)"},
    updates: [{
        q: {$expr: {$eq: ["$Species", "$$target_species"]}},
        u: [{$set: {Location: "$$place"}}],
        c: {place: "North America"}
    }]
}));
}());
