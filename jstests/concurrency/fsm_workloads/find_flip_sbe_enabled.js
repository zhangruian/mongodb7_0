'use strict';

/**
 * Sets the internalQueryForceClassicEngine flag to true and false, and
 * asserts that find queries using the plan cache produce the correct results.
 *
 * @tags: [
 *     # Needed as the setParameter for ForceClassicEngine was introduced in 5.1.
 *     requires_fcv_51,
 * ]
 */

load("jstests/libs/sbe_util.js");

var $config = (function() {
    let data = {originalParamValue: false, isSBEEnabled: false};

    function setup(db, coll, cluster) {
        if (!checkSBEEnabled(db)) {
            jsTestLog("Skipping this test because sbe is disabled");
        }

        cluster.executeOnMongodNodes(function(db) {
            db.adminCommand({setParameter: 1, internalQueryForceClassicEngine: true});
        });

        for (let i = 0; i < 10; ++i) {
            assertAlways.commandWorked(
                db.coll.insert({_id: i, x: i.toString(), y: i.toString(), z: i.toString()}));
        }

        assertAlways.commandWorked(db.coll.createIndex({x: 1}));
        assertAlways.commandWorked(db.coll.createIndex({y: 1}));
    }

    let states = (function() {
        function init(db, coll) {
            const originalParamValue =
                db.adminCommand({getParameter: 1, "internalQueryForceClassicEngine": 1});
            assertAlways.commandWorked(originalParamValue);
            this.originalParamValue = originalParamValue.internalQueryForceClassicEngine;

            if (!checkSBEEnabled(db)) {
                return;
            }
            this.isSBEEnabled = true;
        }

        function toggleSBESwitchOn(db, coll) {
            if (!this.isSBEEnabled) {
                return;
            }

            assertAlways.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryForceClassicEngine: false}));
        }

        function toggleSBESwitchOff(db, coll) {
            if (!this.isSBEEnabled) {
                return;
            }

            assertAlways.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryForceClassicEngine: true}));
        }

        function runQueriesAndCheckResults(db, coll) {
            if (!this.isSBEEnabled) {
                return;
            }

            for (let i = 0; i < 10; i++) {
                const res =
                    db.coll.find({x: i.toString(), y: i.toString(), z: i.toString()}).toArray();
                assertAlways.eq(res.length, 1);
                assertAlways.eq(res[0]._id, i);
            }
        }

        function createIndex(db, coll) {
            if (!this.isSBEEnabled) {
                return;
            }

            assertAlways.commandWorked(db.coll.createIndex({z: 1}));
        }

        function dropIndex(db, coll) {
            if (!this.isSBEEnabled) {
                return;
            }

            assertAlways.commandWorked(db.coll.dropIndex({z: 1}));
        }

        return {
            init: init,
            toggleSBESwitchOn: toggleSBESwitchOn,
            toggleSBESwitchOff: toggleSBESwitchOff,
            runQueriesAndCheckResults: runQueriesAndCheckResults,
            createIndex: createIndex,
            dropIndex: dropIndex
        };
    })();

    let transitions = {
        init: {toggleSBESwitchOn: 1},

        toggleSBESwitchOn:
            {toggleSBESwitchOn: 0.1, toggleSBESwitchOff: 0.1, runQueriesAndCheckResults: 0.8},

        toggleSBESwitchOff:
            {toggleSBESwitchOn: 0.1, toggleSBESwitchOff: 0.1, runQueriesAndCheckResults: 0.8},

        runQueriesAndCheckResults: {
            toggleSBESwitchOn: 0.1,
            toggleSBESwitchOff: 0.1,
            runQueriesAndCheckResults: 0.78,
            createIndex: 0.02,
        },

        createIndex: {
            toggleSBESwitchOn: 0.1,
            toggleSBESwitchOff: 0.1,
            runQueriesAndCheckResults: 0.78,
            createIndex: 0.01,
            dropIndex: 0.01
        },

        dropIndex: {
            toggleSBESwitchOn: 0.1,
            toggleSBESwitchOff: 0.1,
            runQueriesAndCheckResults: 0.78,
            createIndex: 0.02,
        }
    };

    function teardown(db, coll, cluster) {
        const setParam = this.originalParamValue;
        cluster.executeOnMongodNodes(function(db) {
            assertAlways.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryForceClassicEngine: setParam}));
        });
    }

    return {
        threadCount: 10,
        iterations: 100,
        startState: 'init',
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown,
        data: data
    };
})();
