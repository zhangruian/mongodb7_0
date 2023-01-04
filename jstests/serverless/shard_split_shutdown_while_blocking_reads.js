/**
 * Tests that shard split donor can peacefully shut down when there are reads being blocked due
 * to an in-progress split.
 *
 * @tags: [
 *   incompatible_with_eft,
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   requires_fcv_62
 * ]
 */

import {findSplitOperation, ShardSplitTest} from "jstests/serverless/libs/shard_split_test.js";

load("jstests/libs/parallelTester.js");
load("jstests/libs/fail_point_util.js");

const test =
    new ShardSplitTest({recipientTagName: "recipientTag", recipientSetName: "recipientSet"});
test.addRecipientNodes();

const kTenantIds = [ObjectId()];
const kDbName = kTenantIds[0].str + "_testDb";
const kCollName = "testColl";

const donorRst = test.donor;
const donorPrimary = test.donor.getPrimary();
const testDb = donorPrimary.getDB(kDbName);

assert.commandWorked(testDb.runCommand({insert: kCollName, documents: [{_id: 0}]}));

let fp = configureFailPoint(donorPrimary, "pauseShardSplitAfterBlocking");
const operation = test.createSplitOperation(kTenantIds);
const splitThread = operation.commitAsync();

fp.wait();

const donorDoc = findSplitOperation(donorPrimary, operation.migrationId);
assert.neq(null, donorDoc);

let readThread = new Thread((host, dbName, collName, afterClusterTime) => {
    const node = new Mongo(host);
    const db = node.getDB(dbName);
    const res = db.runCommand({
        find: collName,
        readConcern: {afterClusterTime: Timestamp(afterClusterTime.t, afterClusterTime.i)}
    });
    assert.commandFailedWithCode(res, ErrorCodes.InterruptedAtShutdown);
}, donorPrimary.host, kDbName, kCollName, donorDoc.blockOpTime.ts);
readThread.start();

// Shut down the donor after the read starts blocking.
assert.soon(() => ShardSplitTest.getNumBlockedReads(donorPrimary, kTenantIds[0]) == 1);
donorRst.stop(donorPrimary);
readThread.join();

splitThread.join();
// In some cases (ASAN builds) we could end up closing the connection before stopping the worker
// thread. This race condition would result in HostUnreachable instead of
// InterruptedDueToReplStateChange.
const res = splitThread.returnData();
assert(res.code == ErrorCodes.InterruptedDueToReplStateChange ||
           res.code == ErrorCodes.HostUnreachable,
       tojson(res.code));

// Shut down all the other nodes.
test.donor.nodes.filter(node => node.port != donorPrimary.port)
    .forEach(node => donorRst.stop(node));
