/**
 * Test that stepdown during collection cloning and oplog fetching does not interrupt initial sync.
 */
(function() {
    "use strict";

    load("jstests/libs/check_log.js");
    load("jstests/libs/curop_helpers.js");  // for waitForCurOpByFailPoint().

    const testName = "initialSyncDuringStepDown";
    const dbName = testName;
    const collName = "testcoll";

    // Start a 3 node replica set to avoid primary step down after secondary restart.
    const rst = new ReplSetTest({
        nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
        settings: {chainingAllowed: false}
    });
    rst.startSet();
    rst.initiate();

    var primary = rst.getPrimary();
    var primaryDB = primary.getDB(dbName);
    var primaryAdmin = primary.getDB("admin");
    var primaryColl = primaryDB[collName];
    var secondary = rst.getSecondary();
    var secondaryDB = secondary.getDB(dbName);
    var secondaryColl = secondaryDB[collName];
    var dbNss = primaryDB.getName();
    var collNss = primaryColl.getFullName();

    function setupTest({
        failPoint,
        nss: nss = '',
        nssSuffix: nssSuffix = '',
        secondaryStartupParams: secondaryStartupParams = {}
    }) {
        jsTestLog("Writing data to collection.");
        assert.writeOK(primaryColl.insert([{_id: 1}, {_id: 2}]));

        jsTestLog("Stopping secondary.");
        rst.stop(secondary);

        jsTestLog("Enabling failpoint '" + failPoint + "' on primary (sync source).");
        assert.commandWorked(primary.adminCommand({
            configureFailPoint: failPoint,
            data: {nss: nss + nssSuffix, shouldCheckForInterrupt: true, shouldNotdropLock: true},
            mode: "alwaysOn"
        }));

        jsTestLog("Starting secondary.");
        secondaryStartupParams['numInitialSyncAttempts'] = 1;
        rst.start(secondary, {startClean: true, setParameter: secondaryStartupParams});

        // Wait until secondary reaches RS_STARTUP2 state.
        rst.waitForState(secondary, ReplSetTest.State.STARTUP_2);
    }

    function finishTest(
        {failPoint, nss: nss = '', DocsCopiedByOplogFetcher: DocsCopiedByOplogFetcher = 0}) {
        jsTestLog("Waiting for primary to reach failPoint '" + failPoint + "'.");
        waitForCurOpByFailPoint(primaryAdmin, new RegExp('^' + nss), failPoint);

        jsTestLog("Making primary step down");
        const joinStepDownThread = startParallelShell(() => {
            assert.commandWorked(db.adminCommand({"replSetStepDown": 30 * 60, "force": true}));
        }, primary.port);

        // Wait until the step down has started to kill user operations.
        checkLog.contains(primary, "Starting to kill user operations");

        jsTestLog("Allowing initial sync to continue.");
        assert.commandWorked(
            primaryAdmin.adminCommand({configureFailPoint: failPoint, mode: 'off'}));

        jsTestLog("Waiting for initial sync to complete.");
        rst.waitForState(secondary, ReplSetTest.State.SECONDARY);

        // Wait until the primary transitioned to SECONDARY state.
        joinStepDownThread();
        rst.waitForState(primary, ReplSetTest.State.SECONDARY);

        jsTestLog("Validating initial sync data.");
        let res =
            assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1, initialSync: 1}));
        assert.eq(0, res.initialSyncStatus.failedInitialSyncAttempts);
        assert.eq(2 + DocsCopiedByOplogFetcher, secondaryColl.find().itcount());

        // As checkReplicatedDataHashes requires primary to validate the cloned data, we need to
        // unfreeze the old primary and make it re-elected.
        assert.commandWorked(primaryAdmin.adminCommand({replSetFreeze: 0}));
        rst.getPrimary();
        rst.checkReplicatedDataHashes();

        jsTestLog("Dropping collection '" + collName + "'.");
        assert(primaryColl.drop());
    }

    function runStepDownTest(params) {
        setupTest(params);
        finishTest(params);
    }

    jsTestLog("Testing stepdown while 'databases' cloner lists databases.");
    runStepDownTest({failPoint: "hangBeforeListDatabases"});

    jsTestLog("Testing stepdown while 'database' cloner lists collections.");
    runStepDownTest(
        {failPoint: "hangBeforeListCollections", nss: dbNss, nssSuffix: ".$cmd.listCollections"});

    jsTestLog("Testing stepdown while 'collection' cloner performs collection count.");
    runStepDownTest({failPoint: "hangBeforeCollectionCount", nss: collNss});

    jsTestLog("Testing stepdown while 'collection' cloner list indexes for a collection.");
    runStepDownTest({failPoint: "hangBeforeListIndexes", nss: collNss});

    jsTestLog("Testing stepdown while 'collection' cloner clones collection data.");
    runStepDownTest({failPoint: "waitInFindBeforeMakingBatch", nss: collNss});

    jsTestLog("Testing stepdown between collection data batches.");
    runStepDownTest({
        failPoint: "waitAfterPinningCursorBeforeGetMoreBatch",
        nss: collNss,
        secondaryStartupParams: {collectionClonerBatchSize: 1}
    });

    // Restart secondary with "oplogFetcherInitialSyncMaxFetcherRestarts"
    // set to zero to avoid masking the oplog fetcher error.
    jsTestLog("Testing stepdown during oplog fetching");
    setupTest({
        failPoint: "waitAfterPinningCursorBeforeGetMoreBatch",
        nss: "local.oplog.rs",
        secondaryStartupParams: {
            initialSyncOplogFetcherBatchSize: 1,
            oplogFetcherInitialSyncMaxFetcherRestarts: 0,
            "failpoint.initialSyncHangAfterDataCloning": tojson({mode: 'alwaysOn'})
        }
    });

    jsTestLog("Waiting for collection cloning to complete.");
    checkLog.contains(secondary, "initialSyncHangAfterDataCloning fail point enabled");

    // Insert more data so that these are replicated to secondary node via oplog fetcher.
    jsTestLog("Inserting more data on primary.");
    assert.writeOK(primaryColl.insert([{_id: 3}, {_id: 4}]));

    // Disable failpoint on secondary to allow initial sync to continue.
    assert.commandWorked(secondary.adminCommand(
        {configureFailPoint: "initialSyncHangAfterDataCloning", mode: "off"}));

    finishTest({
        failPoint: "waitAfterPinningCursorBeforeGetMoreBatch",
        nss: "local.oplog.rs",
        DocsCopiedByOplogFetcher: 2
    });

    rst.stopSet();
})();
