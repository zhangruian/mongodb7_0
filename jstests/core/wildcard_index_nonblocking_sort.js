(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For arrayEq().
    load("jstests/libs/analyze_plan.js");         // For getPlanStages().
    load("jstests/libs/fixture_helpers.js");      // For numberOfShardsForCollection().

    const coll = db.wildcard_nonblocking_sort;

    assert.commandWorked(coll.createIndex({"$**": 1}, {wildcardProjection: {"excludedField": 0}}));

    for (let i = 0; i < 50; i++) {
        assert.commandWorked(coll.insert({a: i, b: -i, x: [123], excludedField: i}));
    }

    function checkQueryHasSameResultsWhenUsingIdIndex(query, sort) {
        const l = coll.find(query).sort(sort).toArray();
        const r = coll.find(query).sort(sort).hint({$natural: 1}).toArray();
        assert(arrayEq(l, r));
    }

    function checkQueryUsesSortType(query, sort, isBlocking) {
        const explain = assert.commandWorked(coll.find(query).sort(sort).explain());
        const plan = explain.queryPlanner.winningPlan;

        const ixScans = getPlanStages(plan, "IXSCAN");
        const sorts = getPlanStages(plan, "SORT");

        if (isBlocking) {
            assert.eq(sorts.length, FixtureHelpers.numberOfShardsForCollection(coll));
            assert.eq(sorts[0].sortPattern, sort);

            // A blocking sort may or may not use the index, so we don't check the length of
            // 'ixScans'.
        } else {
            assert.eq(sorts.length, 0);
            assert.eq(ixScans.length, FixtureHelpers.numberOfShardsForCollection(coll));

            const sortKey = Object.keys(sort)[0];
            assert.docEq(ixScans[0].keyPattern, {$_path: 1, [sortKey]: 1});
        }
    }

    function checkQueryUsesNonBlockingSortAndGetsCorrectResults(query, sort) {
        checkQueryUsesSortType(query, sort, false);
        checkQueryHasSameResultsWhenUsingIdIndex(query, sort);
    }

    function checkQueryUsesBlockingSortAndGetsCorrectResults(query, sort) {
        checkQueryUsesSortType(query, sort, true);
        checkQueryHasSameResultsWhenUsingIdIndex(query, sort);
    }

    checkQueryUsesNonBlockingSortAndGetsCorrectResults({a: {$gte: 0}}, {a: 1});
    checkQueryUsesNonBlockingSortAndGetsCorrectResults({a: {$gte: 0}, x: 123}, {a: 1});

    checkQueryUsesBlockingSortAndGetsCorrectResults({x: {$elemMatch: {$eq: 123}}}, {x: 1});
    checkQueryUsesBlockingSortAndGetsCorrectResults({x: {$elemMatch: {$eq: 123}}}, {a: 1});
    checkQueryUsesBlockingSortAndGetsCorrectResults({a: {$gte: 0}}, {a: 1, b: 1});
    checkQueryUsesBlockingSortAndGetsCorrectResults({a: {$exists: true}}, {a: 1});
    checkQueryUsesBlockingSortAndGetsCorrectResults({}, {a: 1});
    checkQueryUsesBlockingSortAndGetsCorrectResults({x: 123}, {a: 1});
    checkQueryUsesBlockingSortAndGetsCorrectResults({excludedField: {$gte: 0}}, {excludedField: 1});
})();
