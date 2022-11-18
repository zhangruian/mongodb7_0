/**
 * Commits a shard split and abort it due to timeout prior to marking it for garbage collection and
 * checks that we recover the tenant access blockers since the split is aborted but not marked as
 *  garbage collectable. Checks that `abortOpTime` and `blockOpTime` are set.
 * @tags: [requires_fcv_62, serverless]
 */

load("jstests/libs/fail_point_util.js");                         // for "configureFailPoint"
load('jstests/libs/parallel_shell_helpers.js');                  // for "startParallelShell"
load("jstests/noPassthrough/libs/server_parameter_helpers.js");  // for "setParameter"
load("jstests/serverless/libs/shard_split_test.js");
load("jstests/replsets/libs/tenant_migration_test.js");

(function() {
"use strict";

// Skip db hash check because secondary is left with a different config.
TestData.skipCheckDBHashes = true;

const test = new ShardSplitTest({
    quickGarbageCollection: true,
    nodeOptions: {
        setParameter: {
            "failpoint.PrimaryOnlyServiceSkipRebuildingInstances": tojson({mode: "alwaysOn"}),
            "shardSplitTimeoutMS": 1000
        }
    }
});
test.addRecipientNodes();

let donorPrimary = test.donor.getPrimary();

// Pause the shard split before waiting to mark the doc for garbage collection.
let fp = configureFailPoint(donorPrimary.getDB("admin"), "pauseShardSplitAfterBlocking");

const tenantIds = [ObjectId(), ObjectId()];
const operation = test.createSplitOperation(tenantIds);
assert.commandFailed(operation.commit());
fp.wait();

assertMigrationState(donorPrimary, operation.migrationId, "aborted");

test.stop({shouldRestart: true});

test.donor.startSet({restart: true});

donorPrimary = test.donor.getPrimary();
assert(findSplitOperation(donorPrimary, operation.migrationId), "There must be a config document");

test.validateTenantAccessBlockers(
    operation.migrationId, tenantIds, TenantMigrationTest.DonorAccessState.kAborted);

test.stop();
})();
