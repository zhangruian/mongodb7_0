/**
 * Tests that a time-series bucket's control.min and control.max accurately reflect the minimum and
 * maximum values inserted into the bucket.
 *
 * @tags: [
 *     assumes_no_implicit_collection_creation_after_drop,
 *     does_not_support_stepdowns,
 *     requires_fcv_49,
 *     requires_find_command,
 *     requires_getmore,
 *     sbe_incompatible,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

if (!TimeseriesTest.timeseriesCollectionsEnabled(db.getMongo())) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    return;
}

const collNamePrefix = 'timeseries_min_max_';

const timeFieldName = 'time';
const metaFieldName = 'meta';

let collCount = 0;
let coll;
let bucketsColl;

const clearColl = function() {
    coll = db.getCollection(collNamePrefix + collCount++);
    bucketsColl = db.getCollection('system.buckets.' + coll.getName());

    coll.drop();

    const timeFieldName = 'time';
    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
    assert.contains(bucketsColl.getName(), db.getCollectionNames());
};
clearColl();

const insert = function(doc, expectedMin, expectedMax) {
    doc[timeFieldName] = ISODate();
    assert.commandWorked(coll.insert(doc, {ordered: false}));

    const bucketDocs = bucketsColl
                           .find({}, {
                               'control.min._id': 0,
                               'control.max._id': 0,
                               ['control.min.' + timeFieldName]: 0,
                               ['control.max.' + timeFieldName]: 0
                           })
                           .toArray();
    assert.eq(1, bucketDocs.length, bucketDocs);
    const bucketDoc = bucketDocs[0];
    jsTestLog('Bucket collection document: ' + tojson(bucketDoc));

    assert.docEq(expectedMin, bucketDoc.control.min, 'invalid min in bucket: ' + tojson(bucketDoc));
    assert.docEq(expectedMax, bucketDoc.control.max, 'invalid max in bucket: ' + tojson(bucketDoc));
};

// Empty objects are considered.
insert({a: {}}, {a: {}}, {a: {}});
insert({a: {x: {}}}, {a: {x: {}}}, {a: {x: {}}});
insert({a: {x: {y: 1}}}, {a: {x: {y: 1}}}, {a: {x: {y: 1}}});
insert({a: {x: {}}}, {a: {x: {y: 1}}}, {a: {x: {y: 1}}});
clearColl();

// The metadata field is not considered.
insert({meta: 1}, {}, {});
clearColl();

// Objects and arrays are updated element-wise.
insert({a: {x: 1, y: 2}, b: [1, 2]}, {a: {x: 1, y: 2}, b: [1, 2]}, {a: {x: 1, y: 2}, b: [1, 2]});
insert({a: {x: 2, y: 1}, b: [2, 1]}, {a: {x: 1, y: 1}, b: [1, 1]}, {a: {x: 2, y: 2}, b: [2, 2]});

// Multiple levels of nesting are also updated element-wise.
insert({a: {x: {z: [3, 4]}}, b: [{x: 3, y: 4}]},
       {a: {x: 1, y: 1}, b: [1, 1]},
       {a: {x: {z: [3, 4]}, y: 2}, b: [{x: 3, y: 4}, 2]});
insert({a: {x: {z: [4, 3]}}, b: [{x: 4, y: 3}, 3, 1]},
       {a: {x: 1, y: 1}, b: [1, 1, 1]},
       {a: {x: {z: [4, 4]}, y: 2}, b: [{x: 4, y: 4}, 3, 1]});
clearColl();

// If the two types being compared are not both objects or both arrays, a woCompare is used.
insert({a: 1}, {a: 1}, {a: 1});
insert({a: {b: 1}}, {a: 1}, {a: {b: 1}});
insert({a: []}, {a: 1}, {a: []});
clearColl();

// Sparse measurements only affect the min/max for the fields present.
insert({a: 1, c: 1}, {a: 1, c: 1}, {a: 1, c: 1});
insert({b: 2}, {a: 1, b: 2, c: 1}, {a: 1, b: 2, c: 1});
insert({c: 3, d: 3}, {a: 1, b: 2, c: 1, d: 3}, {a: 1, b: 2, c: 3, d: 3});
})();
