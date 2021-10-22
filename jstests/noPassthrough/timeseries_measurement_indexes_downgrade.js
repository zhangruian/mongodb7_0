/**
 * Tests that the cluster cannot be downgraded when there are secondary indexes on time-series
 * measurements present. Additionally, this verifies that only indexes that are incompatible for
 * downgrade have the "originalSpec" field present on the buckets index definition.
 *
 * TODO SERVER-60576: Re-enable this test. Downgrading FCV does not remove the
 * 'timeseriesBucketsMayHaveMixedSchemaData' catalog entry flag. When upgrading, an invariant will
 * be triggered as the catalog entry flag is expected to be removed on downgrade.
 *
 * @tags: [__TEMPORARILY_DISABLED__]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");

if (!TimeseriesTest.timeseriesMetricIndexesEnabled(db.getMongo())) {
    jsTestLog(
        "Skipped test as the featureFlagTimeseriesMetricIndexes feature flag is not enabled.");
    MongoRunner.stopMongod(conn);
    return;
}

const collName = "timeseries_measurement_indexes_downgrade";
const coll = db.getCollection(collName);
const bucketsColl = db.getCollection("system.buckets." + collName);

const timeFieldName = "tm";
const metaFieldName = "mm";

assert.commandWorked(db.createCollection("regular"));
assert.commandWorked(db.createCollection("system.buckets.abc"));

assert.commandWorked(db.createCollection(
    coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

function checkIndexForDowngrade(isCompatible, createdOnBucketsCollection) {
    const index = bucketsColl.getIndexes()[0];

    if (isCompatible) {
        assert(!index.hasOwnProperty("originalSpec"));
    } else {
        if (createdOnBucketsCollection) {
            // Indexes created directly on the buckets collection do not have the original user
            // index definition.
            assert(!index.hasOwnProperty("originalSpec"));
        } else {
            assert(index.hasOwnProperty("originalSpec"));
        }

        assert.commandFailedWithCode(db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}),
                                     ErrorCodes.CannotDowngrade);
        assert.commandWorked(coll.dropIndexes("*"));
    }

    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    assert.commandWorked(coll.dropIndexes("*"));
}

assert.commandWorked(coll.createIndex({[timeFieldName]: 1}));
checkIndexForDowngrade(true, false);

assert.commandWorked(coll.createIndex({[metaFieldName]: 1}));
checkIndexForDowngrade(true, false);

assert.commandWorked(coll.createIndex({[metaFieldName]: 1, a: 1}));
checkIndexForDowngrade(false, false);

assert.commandWorked(coll.createIndex({b: 1}));
checkIndexForDowngrade(false, false);

assert.commandWorked(bucketsColl.createIndex({"control.min.c.d": 1, "control.max.c.d": 1}));
checkIndexForDowngrade(false, true);

assert.commandWorked(bucketsColl.createIndex({"control.min.e": 1, "control.min.f": 1}));
checkIndexForDowngrade(false, true);

assert.commandWorked(coll.createIndex({g: "2dsphere"}));
checkIndexForDowngrade(false, false);

assert.commandWorked(coll.createIndex({[metaFieldName]: "2d"}));
checkIndexForDowngrade(true, false);

assert.commandWorked(coll.createIndex({[metaFieldName]: "2dsphere"}));
checkIndexForDowngrade(true, false);

MongoRunner.stopMongod(conn);
}());
