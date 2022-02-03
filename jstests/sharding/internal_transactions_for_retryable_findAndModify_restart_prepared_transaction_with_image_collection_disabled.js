/*
 * Tests that prepared retryable internal transactions for findAndModify on a shard with image
 * collection disabled are retryable across restart.
 *
 * @tags: [requires_fcv_53, featureFlagInternalTransactions]
 */
(function() {
'use strict';

load('jstests/sharding/libs/retryable_internal_transaction_test.js');

const transactionTest = new RetryableInternalTransactionTest();
transactionTest.runTestsForAllPreparedRetryableInternalTransactionTypes(
    transactionTest.runFindAndModifyTestsDisableImageCollection, transactionTest.TestMode.kRestart);
transactionTest.stop();
})();
