/**
 * Tests that retryable findAndModify statements that are executed with image collection enabled
 * inside internal transactions that start and commit the donor(s) during resharding are retryable
 * on the recipient after resharding.
 *
 * @tags: [requires_fcv_53, featureFlagInternalTransactions]
 */
(function() {
"use strict";

load("jstests/sharding/libs/internal_transaction_resharding_test.js");

const storeFindAndModifyImagesInSideCollection = true;
const abortOnInitialTry = false;

{
    const transactionTest = new InternalTransactionReshardingTest(
        {reshardInPlace: false, storeFindAndModifyImagesInSideCollection});
    transactionTest.runTestForFindAndModifyDuringResharding(
        transactionTest.InternalTxnType.kRetryable, abortOnInitialTry);
    transactionTest.stop();
}

{
    const transactionTest = new InternalTransactionReshardingTest(
        {reshardInPlace: true, storeFindAndModifyImagesInSideCollection});
    transactionTest.runTestForFindAndModifyDuringResharding(
        transactionTest.InternalTxnType.kRetryable, abortOnInitialTry);
    transactionTest.stop();
}
})();
