/**
 * Tests TTL deletions on a clustered collection delete expired entries of type 'date' only.
 *
 * @tags: [
 *   requires_fcv_51,
 *   does_not_support_stepdowns,
 * ]
 */
(function() {
"use strict";
load("jstests/libs/clustered_indexes_utils.js");
load('jstests/libs/dateutil.js');

// Run TTL monitor constantly to speed up this test.
const conn = MongoRunner.runMongod({setParameter: 'ttlMonitorSleepSecs=1'});

if (areClusteredIndexesEnabled(conn) == false) {
    jsTestLog('Skipping test because the clustered indexes feature flag is disabled');
    MongoRunner.stopMongod(conn);
    return;
}

const dbName = jsTestName();
const testDB = conn.getDB(dbName);
const coll = testDB.getCollection('coll');

// Set expireAfterSeconds to a day to safely test that only expired documents are deleted.
const expireAfterSeconds = 60 * 60 * 24;

const waitForTTL = () => {
    // The 'ttl.passes' metric is incremented when the TTL monitor starts processing the indexes, so
    // we wait for it to be incremented twice to know that the TTL monitor finished processing the
    // indexes at least once.
    const ttlPasses = testDB.serverStatus().metrics.ttl.passes;
    assert.soon(function() {
        return testDB.serverStatus().metrics.ttl.passes > ttlPasses + 1;
    });
};

// Generates an ObjectId with timestamp corresponding to 'date'.
const makeObjectIdFromDate = (date) => {
    try {
        const objIdVal = new ObjectId().valueOf();
        const suffix = objIdVal.substring(objIdVal.length - 16);
        return new ObjectId(Math.floor(date.getTime() / 1000).toString(16) + suffix);
    } catch (e) {
        assert("Invalid date for conversion to Object Id: " + date);
    }
};

const insertAndValidateTTL = (coll, ttlFieldName) => {
    const batchSize = 10;
    const now = new Date();
    let docs = [];

    jsTest.log("Inserting documents with unexpired 'date' types");
    for (let i = 0; i < batchSize; i++) {
        const recentDate = new Date(now - i);
        docs.push({
            [ttlFieldName]: recentDate,
            info: "unexpired",
        });
    }
    assert.commandWorked(coll.insertMany(docs, {ordered: false}));
    assert.eq(coll.find().itcount(), batchSize);

    jsTest.log("Inserting documents that should never expire due to type");
    // Insert documents of type ObjectId with the an expired timestamp. The type of ObjectId sorts
    // lower than Date - this tests that TTL monitor doesn't delete everything from the
    // beginning, only 'Date' types.
    docs = [];
    for (let i = 0; i < batchSize; i++) {
        const tenTimesExpiredMs = 10 * expireAfterSeconds * 1000;
        const pastDate = new Date(now - tenTimesExpiredMs);
        const pastDateOID = makeObjectIdFromDate(pastDate);
        docs.push({
            [ttlFieldName]: pastDateOID,
            info: "unexpired",
            myid: i,
        });
    }
    assert.commandWorked(coll.insertMany(docs, {ordered: false}));

    jsTest.log("Inserting documents with expired 'date' types");
    docs = [];
    for (let i = 0; i < batchSize; i++) {
        const tenTimesExpiredMs = 10 * expireAfterSeconds * 1000;
        const pastDate = new Date(now - tenTimesExpiredMs - i);
        docs.push({
            [ttlFieldName]: pastDate,
            info: "expired",
        });
    }
    assert.commandWorked(coll.insertMany(docs, {ordered: false}));

    waitForTTL();

    // The unexpired documents should still be preserved.
    assert.eq(coll.find().itcount(), batchSize * 2);
    assert.eq(coll.find({info: "expired"}).itcount(), 0);
    assert.eq(coll.find({info: "unexpired"}).itcount(), batchSize * 2);
};

jsTest.log(`Test TTL on cluster key _id`);
assert.commandWorked(testDB.createCollection(
    coll.getName(), {clusteredIndex: {key: {_id: 1}, unique: true}, expireAfterSeconds}));
insertAndValidateTTL(coll, "_id");
coll.drop();

jsTest.log(`Test TTL on secondary index`);
// The collection is clustered, but not TTL on cluster key _id.
assert.commandWorked(
    testDB.createCollection(coll.getName(), {clusteredIndex: {key: {_id: 1}, unique: true}}));
assert.commandWorked(coll.createIndex({ttlField: 1}, {expireAfterSeconds}));
insertAndValidateTTL(coll, "ttlField");
coll.drop();

MongoRunner.stopMongod(conn);
})();
