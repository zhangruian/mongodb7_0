/**
 * Tests that the lifetime of the config.transactions and config.image_collection entries for
 * child sessions is tied to the lifetime of the config.system.sessions entry for their parent
 * sessions.
 *
 * @tags: [requires_fcv_51, featureFlagInternalTransactions]
 */

(function() {
"use strict";

// This test makes assertions about the number of sessions, which are not compatible with
// implicit sessions.
TestData.disableImplicitSessions = true;

const st = new ShardingTest({
    shards: 1,
    shardOptions: {
        setParameter: {
            maxSessions: 1,
            TransactionRecordMinimumLifetimeMinutes: 0,
            storeFindAndModifyImagesInSideCollection: true
        }
    }
});

const shard0Rst = st.rs0;
const shard0Primary = shard0Rst.getPrimary();

const kDbName = "testDb";
const kCollName = "testColl";

const kConfigSessionsNs = "config.system.sessions";
const kConfigTxnsNs = "config.transactions";
const kImageCollNs = "config.image_collection";

let sessionsCollOnPrimary = shard0Primary.getCollection(kConfigSessionsNs);
let transactionsCollOnPrimary = shard0Primary.getCollection(kConfigTxnsNs);
let imageCollOnPrimary = shard0Primary.getCollection(kImageCollNs);
let testDB = shard0Primary.getDB(kDbName);

assert.commandWorked(testDB.createCollection(kCollName));
assert.commandWorked(shard0Primary.adminCommand({refreshLogicalSessionCacheNow: 1}));

const sessionUUID = UUID();
const parentLsid = {
    id: sessionUUID
};

const kInternalTxnNumber = NumberLong(0);

let numTransactionsCollEntries = 0;
let numImageCollEntries = 0;

assert.commandWorked(
    testDB.runCommand({insert: kCollName, documents: [{_id: 0}], lsid: parentLsid}));

const childLsid0 = {
    id: sessionUUID,
    txnUUID: UUID()
};

assert.commandWorked(testDB.runCommand({
    update: kCollName,
    updates: [{q: {_id: 0}, u: {$set: {a: 0}}}],
    lsid: childLsid0,
    txnNumber: kInternalTxnNumber,
    startTransaction: true,
    autocommit: false
}));
assert.commandWorked(testDB.adminCommand(
    {commitTransaction: 1, lsid: childLsid0, txnNumber: kInternalTxnNumber, autocommit: false}));
numTransactionsCollEntries++;
assert.eq(numTransactionsCollEntries, transactionsCollOnPrimary.find().itcount());

jsTest.log("Verify that the config.transactions entry for the internal transaction for " +
           "the non-retryable update did not get reaped after command returned");
assert.eq(numTransactionsCollEntries, transactionsCollOnPrimary.find().itcount());

const parentTxnNumber1 = NumberLong(1);
assert.commandWorked(testDB.runCommand({
    update: kCollName,
    updates: [{q: {_id: 0}, u: {$set: {b: 0}}}],
    lsid: parentLsid,
    txnNumber: parentTxnNumber1,
}));
numTransactionsCollEntries++;

const childLsid1 = {
    id: sessionUUID,
    txnNumber: parentTxnNumber1,
    txnUUID: UUID()
};

assert.commandWorked(testDB.runCommand({
    update: kCollName,
    updates: [{q: {_id: 0}, u: {$set: {c: 0}}}],
    lsid: childLsid1,
    txnNumber: kInternalTxnNumber,
    startTransaction: true,
    autocommit: false
}));
assert.commandWorked(testDB.adminCommand(
    {commitTransaction: 1, lsid: childLsid1, txnNumber: kInternalTxnNumber, autocommit: false}));
numTransactionsCollEntries++;

const parentTxnNumber2 = NumberLong(2);
assert.commandWorked(testDB.runCommand({
    findAndModify: kCollName,
    query: {_id: 0},
    update: {$set: {d: 0}},
    lsid: parentLsid,
    txnNumber: parentTxnNumber2
}));
numImageCollEntries++;

jsTest.log("Verify that the config.transactions entry for the retryable internal transaction for " +
           "the update did not get reaped although there is already a new retryable write");
assert.eq(numTransactionsCollEntries, transactionsCollOnPrimary.find().itcount());

const childLsid2 = {
    id: sessionUUID,
    txnNumber: parentTxnNumber2,
    txnUUID: UUID()
};

assert.commandWorked(testDB.runCommand({
    findAndModify: kCollName,
    query: {_id: 0},
    update: {$set: {e: 0}},
    lsid: childLsid2,
    txnNumber: kInternalTxnNumber,
    startTransaction: true,
    autocommit: false
}));
assert.commandWorked(testDB.adminCommand(
    {commitTransaction: 1, lsid: childLsid2, txnNumber: kInternalTxnNumber, autocommit: false}));
numTransactionsCollEntries++;
numImageCollEntries++;

const parentTxnNumber3 = NumberLong(3);
assert.commandWorked(testDB.runCommand(
    {insert: kCollName, documents: [{_id: 1}], lsid: parentLsid, txnNumber: parentTxnNumber3}));

jsTest.log("Verify that the config.transactions entry for the retryable internal transaction for " +
           "the findAndModify did not get reaped although there is already a new retryable write");
assert.eq(numTransactionsCollEntries, transactionsCollOnPrimary.find().itcount());
assert.eq(numImageCollEntries, imageCollOnPrimary.find().itcount());

assert.eq({_id: 0, a: 0, b: 0, c: 0, d: 0, e: 0},
          testDB.getCollection(kCollName).findOne({_id: 0}));
assert.eq({_id: 1}, testDB.getCollection(kCollName).findOne({_id: 1}));

assert.commandWorked(shard0Primary.adminCommand({refreshLogicalSessionCacheNow: 1}));

assert.eq(1, sessionsCollOnPrimary.find({"_id.id": sessionUUID}).itcount());
assert.eq(numTransactionsCollEntries, transactionsCollOnPrimary.find().itcount());
assert.eq(numImageCollEntries, imageCollOnPrimary.find().itcount());

assert.commandWorked(shard0Primary.adminCommand({reapLogicalSessionCacheNow: 1}));

jsTest.log("Verify that the config.transactions entries for internal transactions did not get " +
           "reaped although they are expired since the config.system.sessions entry for the " +
           "parent session still has not been deleted");

assert.eq(1, sessionsCollOnPrimary.find({"_id.id": sessionUUID}).itcount());
assert.eq(numTransactionsCollEntries,
          transactionsCollOnPrimary.find().itcount(),
          tojson(transactionsCollOnPrimary.find().toArray()));
assert.eq(numImageCollEntries, imageCollOnPrimary.find().itcount());

// Remove the session doc so the parent session gets reaped when reapLogicalSessionCacheNow is run.
assert.commandWorked(sessionsCollOnPrimary.remove({}));
assert.commandWorked(shard0Primary.adminCommand({reapLogicalSessionCacheNow: 1}));

jsTest.log("Verify that the config.transactions entries got reaped since the " +
           "config.system.sessions entry for the parent session had already been deleted");
assert.eq(0, sessionsCollOnPrimary.find().itcount());
assert.eq(0,
          transactionsCollOnPrimary.find().itcount(),
          tojson(transactionsCollOnPrimary.find().toArray()));
assert.eq(0, imageCollOnPrimary.find().itcount());

st.stop();
})();
