// @tags: [requires_non_retryable_writes, requires_fcv_50, requires_fcv_51]

load("jstests/libs/analyze_plan.js");
(function() {
"use strict";
const coll = db.partialFilterExpression_with_geoWithin;
coll.drop();

const isFeatureEnabled = db.adminCommand({getParameter: 1, featureFlagTimeseriesMetricIndexes: 1})
                             .featureFlagTimeseriesMetricIndexes.value;

if (isFeatureEnabled) {
    // The first collection ensures our changes work with a variety of types (polygon, point,
    // linestring) and guarantees some shapes are not inside our index (bigPoly20).
    var bigPoly20 = {
        type: "Polygon",
        coordinates: [[[10.0, 10.0], [-10.0, 10.0], [-10.0, -10.0], [10.0, -10.0], [10.0, 10.0]]],
    };

    var bigPoly20Comp = {
        type: "Polygon",
        coordinates: [[[10.0, 10.0], [10.0, -10.0], [-10.0, -10.0], [-10.0, 10.0], [10.0, 10.0]]],
    };

    var poly10 = {
        type: "Polygon",
        coordinates: [[[5.0, 5.0], [5.0, -5.0], [-5.0, -5.0], [-5.0, 5.0], [5.0, 5.0]]]
    };

    var line10 = {
        type: "LineString",
        coordinates: [[5.0, 5.0], [5.0, -5.0], [-5.0, -5.0], [-5.0, 5.0], [5.0, 5.0]]
    };

    var centerPoint = {type: "Point", coordinates: [0, 0]};

    var polarPoint = {type: "Point", coordinates: [85, 85]};

    var lineEquator = {type: "LineString", coordinates: [[-20, 0], [20, 0]]};

    assert.commandWorked(coll.insert({loc: poly10, a: 1, x: 5}));
    assert.commandWorked(coll.insert({loc: poly10, a: 2, x: 5}));
    assert.commandWorked(coll.insert({loc: line10, a: 0, x: 6}));
    assert.commandWorked(coll.insert({loc: line10, a: 2, x: 6}));
    assert.commandWorked(coll.insert({loc: centerPoint, a: 2, x: 4}));
    assert.commandWorked(coll.insert({loc: polarPoint, a: 5, x: 3}));
    assert.commandWorked(coll.insert({loc: polarPoint, a: 2, x: 3}));
    assert.commandWorked(coll.insert({loc: lineEquator, a: -1, x: 2}));
    assert.commandWorked(coll.insert({loc: lineEquator, a: 2, x: 2}));

    assert.commandWorked(coll.createIndex(
        {a: 1}, {partialFilterExpression: {loc: {$geoWithin: {$geometry: bigPoly20Comp}}}}));

    var explainResults =
        coll.find({a: 2, loc: {$geoWithin: {$geometry: bigPoly20Comp}}}).explain("queryPlanner");
    var winningPlan = getWinningPlan(explainResults.queryPlanner);
    assert(isIxscan(db, winningPlan));
    coll.drop();

    assert.commandWorked(coll.insert(
        {a: 1, name: "Dallas", loc: {type: "Point", coordinates: [-96.808891, 32.779]}}));
    assert.commandWorked(coll.insert(
        {a: 1, name: "Paris TX", loc: {type: "Point", coordinates: [-95.555513, 33.6609389]}}));
    assert.commandWorked(coll.insert(
        {a: 1, name: "San Antonio", loc: {type: "Point", coordinates: [-98.4936282, 29.4241219]}}));
    assert.commandWorked(coll.insert(
        {a: 2, name: "Houston", loc: {type: "Point", coordinates: [-95.3632715, 29.7632836]}}));
    assert.commandWorked(coll.insert(
        {a: 3, name: "LA", loc: {type: "Point", coordinates: [-118.2436849, 34.0522342]}}));
    assert.commandWorked(coll.insert(
        {a: 3, name: "Berkeley", loc: {type: "Point", coordinates: [-122.272747, 37.8715926]}}));
    assert.commandWorked(coll.insert(
        {a: 1, name: "NYC", loc: {type: "Point", coordinates: [-74.0059729, 40.7142691]}}));

    var texasPolygon = {
        type: "Polygon",
        coordinates: [[
            [-97.516473, 26.02054],
            [-106.528371, 31.895644],
            [-103.034724, 31.932947],
            [-102.979798, 36.456096],
            [-100.051947, 36.482601],
            [-99.975048, 34.506004],
            [-94.240190, 33.412542],
            [-94.075400, 29.725640],
            [-97.516473, 26.02054]
        ]],
    };
    var southWestUSPolygon = {
        type: "Polygon",
        coordinates: [[
            [-97.516473, 26.02054],
            [-106.528371, 31.895644],
            [
                -118.646927,
                33.748207,
            ],
            [-119.591751, 34.348991],
            [-103.068314, 36.426696],
            [-100.080033, 36.497382],
            [-99.975048, 34.506004],
            [-94.240190, 33.412542],
            [-94.075400, 29.725640],
            [-97.516473, 26.02054]
        ]]
    };

    // Create index to cover Texas and Southern California.
    assert.commandWorked(coll.createIndex(
        {loc: "2dsphere"},
        {partialFilterExpression: {loc: {$geoWithin: {$geometry: southWestUSPolygon}}}}));

    // Search for points only located in a smaller region within our larger index, in this case
    // Texas.
    var command = coll.find({
        $and: [
            {loc: {$geoWithin: {$geometry: southWestUSPolygon}}},
            {loc: {$geoWithin: {$geometry: texasPolygon}}},
        ]
    });
    explainResults = command.explain("queryPlanner");
    winningPlan = getWinningPlan(explainResults.queryPlanner);
    assert(isIxscan(db, winningPlan));
    var results = command.toArray();
    assert.eq(results.length, 4);

    command = coll.find({
        a: 1,
        $and: [
            {loc: {$geoWithin: {$geometry: southWestUSPolygon}}},
            {loc: {$geoWithin: {$geometry: texasPolygon}}},
        ]
    });
    explainResults = command.explain("queryPlanner");
    winningPlan = getWinningPlan(explainResults.queryPlanner);
    assert(isIxscan(db, winningPlan));
    results = command.toArray();
    assert.eq(results.length, 3);

    // Test index maintenace to make sure a doc is removed from index when it is no longer in the
    // $geoWithin bounds.
    assert.commandWorked(coll.updateMany(
        {loc: {type: "Point", coordinates: [-95.555513, 33.6609389]}},
        {$set: {name: "Paris France", loc: {type: "Point", coordinates: [2.360791, 48.885033]}}}));

    command = coll.find({
        $and: [
            {loc: {$geoWithin: {$geometry: southWestUSPolygon}}},
            {loc: {$geoWithin: {$geometry: texasPolygon}}},
        ]
    });
    explainResults = command.explain("queryPlanner");
    winningPlan = getWinningPlan(explainResults.queryPlanner);
    assert(isIxscan(db, winningPlan));
    results = command.toArray();
    assert.eq(results.length, 3);
}
})();