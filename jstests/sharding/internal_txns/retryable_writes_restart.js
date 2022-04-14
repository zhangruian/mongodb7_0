/*
 * Tests that retryable internal transactions for insert, update and delete are retryable across
 * restart.
 *
 * @tags: [requires_fcv_60, uses_transactions, requires_persistence]
 */
(function() {
'use strict';

load("jstests/sharding/internal_txns/libs/retryable_internal_transaction_test.js");

const transactionTest = new RetryableInternalTransactionTest();
transactionTest.runTestsForAllRetryableInternalTransactionTypes(
    transactionTest.runInsertUpdateDeleteTests, transactionTest.TestMode.kRestart);
transactionTest.stop();
})();
