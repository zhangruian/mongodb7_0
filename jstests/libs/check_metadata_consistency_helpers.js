'use strict';

load('jstests/libs/feature_flag_util.js');  // For FeatureFlagUtil.

const MetadataConsistencyChecker = (function() {
    const run = (mongos) => {
        const adminDB = mongos.getDB('admin');

        // TODO (SERVER-70396): Remove once 7.0 becomes last LTS.
        try {
            if (!FeatureFlagUtil.isEnabled(adminDB, 'CheckMetadataConsistency')) {
                jsTest.log('Skipped metadata consistency check: feature disabled');
                return;
            }
        } catch (err) {
            jsTest.log(`Skipped metadata consistency check: ${err}`);
            return;
        }

        const checkMetadataConsistency = function() {
            jsTest.log('Started metadata consistency check');

            const inconsistencies = adminDB.checkMetadataConsistency().toArray();
            assert.eq(0,
                      inconsistencies.length,
                      `Found metadata inconsistencies: ${tojson(inconsistencies)}`);

            jsTest.log('Completed metadata consistency check');
        };

        try {
            checkMetadataConsistency();
        } catch (e) {
            if (ErrorCodes.isRetriableError(e.code) || ErrorCodes.isInterruption(e.code)) {
                jsTest.log(`Aborted metadata consistency check due to retriable error: ${e}`);
            } else {
                throw e;
            }
        }
    };

    return {
        run: run,
    };
})();
