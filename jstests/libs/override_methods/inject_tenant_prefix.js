/**
 * Overrides the database name of each accessed database ("config", "admin", "local" excluded) to
 * have the prefix TestData.tenantId so that the accessed data will be migrated by the background
 * tenant migrations run by the ContinuousTenantMigration hook.
 */
(function() {
'use strict';

load("jstests/libs/override_methods/override_helpers.js");  // For 'OverrideHelpers'.
load("jstests/libs/transactions_util.js");

// Save references to the original methods in the IIFE's scope.
// This scoping allows the original methods to be called by the overrides below.
// Override this method to make the accessed database have the prefix TestData.tenantId.
let originalRunCommand = Mongo.prototype.runCommand;

const blacklistedDbNames = ["config", "admin", "local"];

function isBlacklistedDb(dbName) {
    return blacklistedDbNames.includes(dbName);
}

/**
 * If the database with the given name can be migrated, prepends TestData.tenantId to the name if
 * it does not already start with the prefix.
 */
function prependTenantIdToDbNameIfApplicable(dbName) {
    if (dbName.length === 0) {
        // There are input validation tests that use invalid database names, those should be
        // ignored.
        return dbName;
    }
    return isBlacklistedDb(dbName) ? dbName : TestData.tenantId + "_" + dbName;
}

/**
 * If the database for the given namespace can be migrated, prepends TestData.tenantId to the
 * namespace if it does not already start with the prefix.
 */
function prependTenantIdToNsIfApplicable(ns) {
    if (ns.length === 0 || !ns.includes(".")) {
        // There are input validation tests that use invalid namespaces, those should be ignored.
        return ns;
    }
    let splitNs = ns.split(".");
    splitNs[0] = prependTenantIdToDbNameIfApplicable(splitNs[0]);
    return splitNs.join(".");
}

/**
 * If the given database name starts TestData.tenantId, removes the prefix.
 */
function extractOriginalDbName(dbName) {
    return dbName.replace(TestData.tenantId + "_", "");
}

/**
 * If the database name for the given namespace starts TestData.tenantId, removes the prefix.
 */
function extractOriginalNs(ns) {
    let splitNs = ns.split(".");
    splitNs[0] = extractOriginalDbName(splitNs[0]);
    return splitNs.join(".");
}

/**
 * Removes all occurrences of TestDatabase.tenantId in the string.
 */
function removeTenantIdFromString(string) {
    return string.replace(new RegExp(TestData.tenantId + "_", "g"), "");
}

/**
 * Prepends TestDatabase.tenantId to all the database name and namespace fields inside the given
 * object.
 */
function prependTenantId(obj) {
    for (let k of Object.keys(obj)) {
        let v = obj[k];
        if (typeof v === "string") {
            if (k === "dbName" || k == "db") {
                obj[k] = prependTenantIdToDbNameIfApplicable(v);
            } else if (k === "namespace" || k === "ns") {
                obj[k] = prependTenantIdToNsIfApplicable(v);
            }
        } else if (Array.isArray(v)) {
            obj[k] = v.map((item) => {
                return (typeof item === "object" && item !== null) ? prependTenantId(item) : item;
            });
        } else if (typeof v === "object" && v !== null && Object.keys(v).length > 0) {
            obj[k] = prependTenantId(v);
        }
    }
    return obj;
}

/**
 * Removes TestDatabase.tenantId from all the database name and namespace fields inside the given
 * object.
 */
function removeTenantId(obj) {
    for (let k of Object.keys(obj)) {
        let v = obj[k];
        let originalK = removeTenantIdFromString(k);
        if (typeof v === "string") {
            if (k === "dbName" || k == "db" || k == "dropped") {
                obj[originalK] = extractOriginalDbName(v);
            } else if (k === "namespace" || k === "ns") {
                obj[originalK] = extractOriginalNs(v);
            } else if (k === "errmsg" || k == "name") {
                obj[originalK] = removeTenantIdFromString(v);
            }
        } else if (Array.isArray(v)) {
            obj[originalK] = v.map((item) => {
                return (typeof item === "object" && item !== null) ? removeTenantId(item) : item;
            });
        } else if (typeof v === "object" && v !== null && Object.keys(v).length > 0) {
            obj[originalK] = removeTenantId(v);
        }
    }
    return obj;
}

const kCmdsWithNsAsFirstField =
    new Set(["renameCollection", "checkShardingIndex", "dataSize", "datasize", "splitVector"]);

/**
 * Returns a new cmdObj with TestData.tenantId prepended to all database name and namespace fields.
 */
function createCmdObjWithTenantId(cmdObj) {
    const cmdName = Object.keys(cmdObj)[0];
    let cmdObjWithTenantId = TransactionsUtil.deepCopyObject({}, cmdObj);

    // Handle commands with special database and namespace field names.
    if (kCmdsWithNsAsFirstField.has(cmdName)) {
        cmdObjWithTenantId[cmdName] = prependTenantIdToNsIfApplicable(cmdObjWithTenantId[cmdName]);
    }

    switch (cmdName) {
        case "renameCollection":
            cmdObjWithTenantId.to = prependTenantIdToNsIfApplicable(cmdObjWithTenantId.to);
            break;
        case "internalRenameIfOptionsAndIndexesMatch":
            cmdObjWithTenantId.from = prependTenantIdToNsIfApplicable(cmdObjWithTenantId.from);
            cmdObjWithTenantId.to = prependTenantIdToNsIfApplicable(cmdObjWithTenantId.to);
            break;
        case "configureFailPoint":
            if (cmdObjWithTenantId.data) {
                if (cmdObjWithTenantId.data.namespace) {
                    cmdObjWithTenantId.data.namespace =
                        prependTenantIdToNsIfApplicable(cmdObjWithTenantId.data.namespace);
                } else if (cmdObjWithTenantId.data.ns) {
                    cmdObjWithTenantId.data.ns =
                        prependTenantIdToNsIfApplicable(cmdObjWithTenantId.data.ns);
                }
            }
            break;
        case "applyOps":
            for (let op of cmdObjWithTenantId.applyOps) {
                if (typeof op.ns === "string" && op.ns.endsWith("system.views") && op.o._id &&
                    typeof op.o._id === "string") {
                    // For views, op.ns and op.o._id must be equal.
                    op.o._id = prependTenantIdToNsIfApplicable(op.o._id);
                }
            }
            break;
        default:
            break;
    }

    // Recursively override the database name and namespace fields. Exclude 'configureFailPoint'
    // since data.errorExtraInfo.namespace or data.errorExtraInfo.ns can sometimes refer to
    // collection name instead of namespace.
    if (cmdName != "configureFailPoint") {
        prependTenantId(cmdObjWithTenantId);
    }

    return cmdObjWithTenantId;
}

/**
 * If the given response object contains a TenantMigrationAborted error, returns the error object.
 * Otherwise, returns null.
 */
function extractTenantMigrationAbortedError(resObj) {
    if (resObj.code == ErrorCodes.TenantMigrationAborted) {
        // Commands, like createIndex and dropIndex, have TenantMigrationAborted error in the top
        // level
        return resObj;
    }
    if (resObj.writeErrors) {
        for (let writeError of resObj.writeErrors) {
            if (writeError.code == ErrorCodes.TenantMigrationAborted) {
                return writeError;
            }
        }
    }
    return null;
}
/**
 * If the command was a batch command where some of the operations failed, modifies the command
 * object so that only failed operations are retried.
 */
function modifyCmdObjForRetry(cmdObj, resObj) {
    if (cmdObj.insert) {
        let retryOps = [];
        if (cmdObj.ordered) {
            retryOps = cmdObj.documents.slice(resObj.writeErrors[0].index);
        } else {
            for (let writeError of resObj.writeErrors) {
                if (writeError.code == ErrorCodes.TenantMigrationAborted) {
                    retryOps.push(cmdObj.documents[writeError.index]);
                }
            }
        }
        cmdObj.documents = retryOps;
    }

    // findAndModify may also have an update field, but is not a batched command.
    if (cmdObj.update && !cmdObj.findAndModify && !cmdObj.findandmodify) {
        let retryOps = [];
        if (cmdObj.ordered) {
            retryOps = cmdObj.updates.slice(resObj.writeErrors[0].index);
        } else {
            for (let writeError of resObj.writeErrors) {
                if (writeError.code == ErrorCodes.TenantMigrationAborted) {
                    retryOps.push(cmdObj.updates[writeError.index]);
                }
            }
        }
        cmdObj.updates = retryOps;
    }

    if (cmdObj.delete) {
        let retryOps = [];
        if (cmdObj.ordered) {
            retryOps = cmdObj.deletes.slice(resObj.writeErrors[0].index);
        } else {
            for (let writeError of resObj.writeErrors) {
                if (writeError.code == ErrorCodes.TenantMigrationAborted) {
                    retryOps.push(cmdObj.deletes[writeError.index]);
                }
            }
        }
        cmdObj.deletes = retryOps;
    }
}

/**
 * Sets the keys of the given index map to consecutive non-negative integers starting from 0.
 */
function resetIndices(indexMap) {
    let newIndexMap = {};
    Object.keys(indexMap).map((key, index) => {
        newIndexMap[index] = indexMap[key];
    });
    return newIndexMap;
}

function toIndexSet(indexedDocs) {
    let set = new Set();
    if (indexedDocs) {
        for (let doc of indexedDocs) {
            set.add(doc.index);
        }
    }
    return set;
}

/**
 * Remove the indices for non-upsert writes that succeeded.
 */
function removeSuccessfulOpIndexesExceptForUpserted(resObj, indexMap) {
    // Optimization to only look through the indices in a set rather than in an array.
    let indexSetForUpserted = toIndexSet(resObj.upserted);
    let indexSetForWriteErrors = toIndexSet(resObj.writeErrors);

    for (let index in Object.keys(indexMap)) {
        if ((!indexSetForUpserted.has(parseInt(index)) &&
             !indexSetForWriteErrors.has(parseInt(index)))) {
            delete indexMap[index];
        }
    }
    return indexMap;
}

Mongo.prototype.runCommand = function(dbName, cmdObj, options) {
    // Create another cmdObj from this command with TestData.tenantId prepended to all the
    // applicable database names and namespaces.
    const cmdObjWithTenantId = createCmdObjWithTenantId(cmdObj);

    let numAttempts = 0;

    // Keep track of the write operations that were applied.
    let n = 0;
    let nModified = 0;
    let upserted = [];
    let nonRetryableWriteErrors = [];

    // 'indexMap' is a mapping from a write's index in the current cmdObj to its index in the
    // original cmdObj.
    let indexMap = {};
    if (cmdObjWithTenantId.documents) {
        for (let i = 0; i < cmdObjWithTenantId.documents.length; i++) {
            indexMap[i] = i;
        }
    }
    if (cmdObjWithTenantId.updates) {
        for (let i = 0; i < cmdObjWithTenantId.updates.length; i++) {
            indexMap[i] = i;
        }
    }
    if (cmdObjWithTenantId.deletes) {
        for (let i = 0; i < cmdObjWithTenantId.deletes.length; i++) {
            indexMap[i] = i;
        }
    }

    while (true) {
        numAttempts++;
        let resObj = originalRunCommand.apply(
            this, [prependTenantIdToDbNameIfApplicable(dbName), cmdObjWithTenantId, options]);

        // Remove TestData.tenantId from all database names and namespaces in the resObj since tests
        // assume the command was run against the original database.
        removeTenantId(resObj);

        // If the write didn't encounter a TenantMigrationAborted error at all, return the result
        // directly.
        let tenantMigrationAbortedErr = extractTenantMigrationAbortedError(resObj);
        if (numAttempts == 1 && !tenantMigrationAbortedErr) {
            return resObj;
        }

        // Add/modify the shells's n, nModified, upserted, and writeErrors.
        if (resObj.n) {
            n += resObj.n;
        }
        if (resObj.nModified) {
            nModified += resObj.nModified;
        }
        if (resObj.upserted || resObj.writeErrors) {
            // This is an optimization to make later lookups into 'indexMap' faster, since it
            // removes any key that is not pertinent in the current cmdObj execution.
            indexMap = removeSuccessfulOpIndexesExceptForUpserted(resObj, indexMap);

            if (resObj.upserted) {
                for (let upsert of resObj.upserted) {
                    // Set the entry's index to the write's index in the original cmdObj.
                    upsert.index = indexMap[upsert.index];

                    // Track that this write resulted in an upsert.
                    upserted.push(upsert);

                    // This write will not need to be retried, so remove it from 'indexMap'.
                    delete indexMap[upsert.index];
                }
            }
            if (resObj.writeErrors) {
                for (let writeError of resObj.writeErrors) {
                    // If we encounter a TenantMigrationAborted error, the rest of the batch must
                    // have failed with the same code.
                    if (writeError.code === ErrorCodes.TenantMigrationAborted) {
                        break;
                    }

                    // Set the entry's index to the write's index in the original cmdObj.
                    writeError.index = indexMap[writeError.index];

                    // Track that this write resulted in a non-retryable error.
                    nonRetryableWriteErrors.push(writeError);

                    // This write will not need to be retried, so remove it from 'indexMap'.
                    delete indexMap[writeError.index];
                }
            }
        }

        if (tenantMigrationAbortedErr) {
            modifyCmdObjForRetry(cmdObjWithTenantId, resObj);

            // Build a new indexMap where the keys are the index that each write that needs to be
            // retried will have in the next attempt's cmdObj.
            indexMap = resetIndices(indexMap);

            jsTest.log(
                `Got TenantMigrationAborted for command against database ` +
                `"${dbName}" with response ${tojson(resObj)} after trying ${numAttempts} times, ` +
                `retrying the command`);
        } else {
            // Modify the resObj before returning the result.
            if (resObj.n) {
                resObj.n = n;
            }
            if (resObj.nModified) {
                resObj.nModified = nModified;
            }
            if (upserted.length > 0) {
                resObj.upserted = upserted;
            }
            if (nonRetryableWriteErrors.length > 0) {
                resObj.writeErrors = nonRetryableWriteErrors;
            }
            return resObj;
        }
    }
};

Mongo.prototype.runCommandWithMetadata = function(dbName, metadata, commandArgs) {
    // Create another cmdObj from this command with TestData.tenantId prepended to all the
    // applicable database names and namespaces.
    const cmdObjWithTenantId = createCmdObjWithTenantId(cmdObj);

    let numAttempts = 0;

    // Keep track of the write operations that were applied.
    let n = 0;
    let nModified = 0;
    let upserted = [];
    let nonRetryableWriteErrors = [];

    // 'indexMap' is a mapping from a write's index in the current cmdObj to its index in the
    // original cmdObj.
    let indexMap = {};
    if (cmdObjWithTenantId.documents) {
        for (let i = 0; i < cmdObjWithTenantId.documents.length; i++) {
            indexMap[i] = i;
        }
    }
    if (cmdObjWithTenantId.updates) {
        for (let i = 0; i < cmdObjWithTenantId.updates.length; i++) {
            indexMap[i] = i;
        }
    }
    if (cmdObjWithTenantId.deletes) {
        for (let i = 0; i < cmdObjWithTenantId.deletes.length; i++) {
            indexMap[i] = i;
        }
    }

    while (true) {
        numAttempts++;
        let resObj = originalRunCommand.apply(
            this, [prependTenantIdToDbNameIfApplicable(dbName), metadata, commandArgsWithTenantId]);

        // Remove TestData.tenantId from all database names and namespaces in the resObj since tests
        // assume the command was run against the original database.
        removeTenantId(resObj);

        // If the write didn't encounter a TenantMigrationAborted error at all, return the result
        // directly.
        let tenantMigrationAbortedErr = extractTenantMigrationAbortedError(resObj);
        if (numAttempts == 1 && !tenantMigrationAbortedErr) {
            return resObj;
        }

        // Add/modify the shells's n, nModified, upserted, and writeErrors.
        if (resObj.n) {
            n += resObj.n;
        }
        if (resObj.nModified) {
            nModified += resObj.nModified;
        }
        if (resObj.upserted || resObj.writeErrors) {
            // This is an optimization to make later lookups into 'indexMap' faster, since it
            // removes any key that is not pertinent in the current cmdObj execution.
            indexMap = removeSuccessfulOpIndexesExceptForUpserted(resObj, indexMap);

            if (resObj.upserted) {
                for (let upsert of resObj.upserted) {
                    // Set the entry's index to the write's index in the original cmdObj.
                    upsert.index = indexMap[upsert.index];

                    // Track that this write resulted in an upsert.
                    upserted.push(upsert);

                    // This write will not need to be retried, so remove it from 'indexMap'.
                    delete indexMap[upsert.index];
                }
            }
            if (resObj.writeErrors) {
                for (let writeError of resObj.writeErrors) {
                    // If we encounter a TenantMigrationAborted error, the rest of the batch must
                    // have failed with the same code.
                    if (writeError.code === ErrorCodes.TenantMigrationAborted) {
                        break;
                    }

                    // Set the entry's index to the write's index in the original cmdObj.
                    writeError.index = indexMap[writeError.index];

                    // Track that this write resulted in a non-retryable error.
                    nonRetryableWriteErrors.push(writeError);

                    // This write will not need to be retried, so remove it from 'indexMap'.
                    delete indexMap[writeError.index];
                }
            }
        }

        if (tenantMigrationAbortedErr) {
            modifyCmdObjForRetry(cmdObjWithTenantId, resObj);

            // Build a new indexMap where the keys are the index that each write that needs to be
            // retried will have in the next attempt's cmdObj.
            indexMap = resetIndices(indexMap);

            jsTest.log(
                `Got TenantMigrationAborted for command against database ` +
                `"${dbName}" with response ${tojson(resObj)} after trying ${numAttempts} times, ` +
                `retrying the command`);
        } else {
            // Modify the resObj before returning the result.
            if (resObj.n) {
                resObj.n = n;
            }
            if (resObj.nModified) {
                resObj.nModified = nModified;
            }
            if (upserted.length > 0) {
                resObj.upserted = upserted;
            }
            if (nonRetryableWriteErrors.length > 0) {
                resObj.writeErrors = nonRetryableWriteErrors;
            }
            return resObj;
        }
    }
};

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/inject_tenant_prefix.js");
}());
