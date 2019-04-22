'use strict';

var {withTxnAndAutoRetry} = (function() {

    /**
     * Calls 'func' with the print() function overridden to be a no-op.
     *
     * This function is useful for silencing JavaScript backtraces that would otherwise be logged
     * from doassert() being called, even when the JavaScript exception is ultimately caught and
     * handled.
     */
    function quietly(func) {
        const printOriginal = print;
        try {
            print = Function.prototype;
            func();
        } finally {
            print = printOriginal;
        }
    }

    // Returns true if the transaction can be retried with a higher transaction number after the
    // given error.
    function shouldRetryEntireTxnOnError(e, hasCommitTxnError, retryOnKilledSession) {
        if ((e.hasOwnProperty('errorLabels') &&
             e.errorLabels.includes('TransientTransactionError'))) {
            return true;
        }

        if (isNetworkError(e) && !hasCommitTxnError) {
            // A network error before commit is considered a transient txn error. Network
            // errors during commit should be handled at the same level as retries of
            // retryable writes.
            return true;
        }

        // TODO SERVER-40186: Only retry the entire transaction on killed session errors from before
        // commit.
        if ((retryOnKilledSession &&
             (e.code === ErrorCodes.Interrupted || e.code === ErrorCodes.CursorKilled ||
              e.code == ErrorCodes.CursorNotFound))) {
            return true;
        }

        return false;
    }

    // Use a "signature" value that won't typically match a value assigned in normal use. This way
    // the wtimeout set by this override is distinguishable in the server logs.
    const kDefaultWtimeout = 5 * 60 * 1000 + 789;

    /**
     * Runs 'func' inside of a transaction started with 'txnOptions', and automatically retries
     * until it either succeeds or the server returns a non-TransientTransactionError error
     * response. If retryOnKilledSession is true, the transaction will be automatically retried on
     * error codes that may come from a killed session as well. There is a probability of
     * 'prepareProbability' that the transaction is prepared before committing.
     *
     * The caller should take care to ensure 'func' doesn't modify any captured variables in a
     * speculative fashion where calling it multiple times would lead to unintended behavior. The
     * transaction started by the withTxnAndAutoRetry() function is only known to have committed
     * after the withTxnAndAutoRetry() function returns.
     */
    function withTxnAndAutoRetry(session, func, {
        txnOptions: txnOptions = {
            readConcern: {level: TestData.defaultTransactionReadConcernLevel || 'snapshot'},
            writeConcern: TestData.hasOwnProperty("defaultTransactionWriteConcernW")
                ? {w: TestData.defaultTransactionWriteConcernW, wtimeout: kDefaultWtimeout}
                : undefined
        },
        retryOnKilledSession: retryOnKilledSession = false,
        prepareProbability: prepareProbability = 0.0
    } = {}) {
        let hasTransientError;

        do {
            session.startTransaction_forTesting(txnOptions, {ignoreActiveTxn: true});
            let hasCommitTxnError = false;
            hasTransientError = false;

            try {
                func();

                try {
                    const rand = Random.rand();
                    if (rand < prepareProbability) {
                        const prepareTimestamp = PrepareHelpers.prepareTransaction(session);
                        PrepareHelpers.commitTransaction(session, prepareTimestamp);
                    } else {
                        // commitTransaction() calls assert.commandWorked(), which may fail with a
                        // WriteConflict error response, which is ignored.
                        quietly(() => session.commitTransaction());
                    }
                } catch (e) {
                    hasCommitTxnError = true;
                    throw e;
                }

            } catch (e) {
                if (!hasCommitTxnError) {
                    // Use the version of abortTransaction() that ignores errors. We ignore the
                    // error from abortTransaction because the transaction may have implicitly
                    // been aborted by the server already and will therefore return a
                    // NoSuchTransaction error response.
                    // We need to call abortTransaction() in order to update the mongo shell's
                    // state such that it agrees no transaction is currently in progress on this
                    // session.
                    session.abortTransaction();
                }

                if (shouldRetryEntireTxnOnError(e, hasCommitTxnError, retryOnKilledSession)) {
                    hasTransientError = true;
                    continue;
                }

                throw e;
            }
        } while (hasTransientError);
    }

    return {withTxnAndAutoRetry};
})();
