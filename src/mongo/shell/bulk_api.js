//
// Scope for the function
//
var _bulk_api_module = (function() {
    // Batch types
    var NONE = 0;
    var INSERT = 1;
    var UPDATE = 2;
    var REMOVE = 3;

    // Error codes
    var UNKNOWN_ERROR = 8;
    var WRITE_CONCERN_FAILED = 64;
    var UNKNOWN_REPL_WRITE_CONCERN = 79;
    var NOT_MASTER = 10107;

    /**
     * Helper function to define properties
     */
    var defineReadOnlyProperty = function(self, name, value) {
        Object.defineProperty(self, name, {
            enumerable: true,
            get: function() {
                return value;
            }
        });
    };

    /**
     * Shell representation of WriteConcern, possibly includes:
     *  j: write waits for journal
     *  w: write waits until replicated to number of servers (including primary), or mode (string)
     *  wtimeout: how long to wait for "w" replication
     *  fsync: waits for data flush (either journal, nor database files depending on server conf)
     *
     * Accepts { w : x, j : x, wtimeout : x, fsync: x } or w, wtimeout, j
     */
    var WriteConcern = function(wValue, wTimeout, jValue) {
        if (!(this instanceof WriteConcern)) {
            var writeConcern = Object.create(WriteConcern.prototype);
            WriteConcern.apply(writeConcern, arguments);
            return writeConcern;
        }

        var opts = {};
        if (typeof wValue == 'object') {
            if (arguments.length == 1)
                opts = Object.merge(wValue);
            else
                throw Error("If the first arg is an Object then no additional args are allowed!");
        } else {
            if (typeof wValue != 'undefined')
                opts.w = wValue;
            if (typeof wTimeout != 'undefined')
                opts.wtimeout = wTimeout;
            if (typeof jValue != 'undefined')
                opts.j = jValue;
        }

        // Do basic validation.
        if (typeof opts.w != 'undefined' && typeof opts.w != 'number' && typeof opts.w != 'string')
            throw Error("w value must be a number or string but was found to be a " +
                        typeof opts.w);
        if (typeof opts.w == 'number' && NumberInt(opts.w).toNumber() < 0)
            throw Error("Numeric w value must be equal to or larger than 0, not " + opts.w);

        if (typeof opts.wtimeout != 'undefined') {
            if (typeof opts.wtimeout != 'number')
                throw Error("wtimeout must be a number, not " + opts.wtimeout);
            if (NumberInt(opts.wtimeout).toNumber() < 0)
                throw Error("wtimeout must be a number greater than or equal to 0, not " +
                            opts.wtimeout);
        }

        if (typeof opts.j != 'undefined' && typeof opts.j != 'boolean')
            throw Error("j value must be true or false if defined, not " + opts.j);

        this._wc = opts;

        this.toJSON = function() {
            return Object.merge({}, this._wc);
        };

        /**
         * @return {string}
         */
        this.tojson = function(indent, nolint) {
            return tojson(this.toJSON(), indent, nolint);
        };

        this.toString = function() {
            return "WriteConcern(" + this.tojson() + ")";
        };

        this.shellPrint = function() {
            return this.toString();
        };
    };

    /**
     * Wraps the result for write commands and presents a convenient api for accessing
     * single results & errors (returns the last one if there are multiple).
     * singleBatchType is passed in on bulk operations consisting of a single batch and
     * are used to filter the WriteResult to only include relevant result fields.
     */
    var WriteResult = function(bulkResult, singleBatchType, writeConcern) {
        if (!(this instanceof WriteResult))
            return new WriteResult(bulkResult, singleBatchType, writeConcern);

        // Define properties
        defineReadOnlyProperty(this, "ok", bulkResult.ok);
        defineReadOnlyProperty(this, "nInserted", bulkResult.nInserted);
        defineReadOnlyProperty(this, "nUpserted", bulkResult.nUpserted);
        defineReadOnlyProperty(this, "nMatched", bulkResult.nMatched);
        defineReadOnlyProperty(this, "nModified", bulkResult.nModified);
        defineReadOnlyProperty(this, "nRemoved", bulkResult.nRemoved);
        if (bulkResult.upserted.length > 0) {
            defineReadOnlyProperty(
                this, "_id", bulkResult.upserted[bulkResult.upserted.length - 1]._id);
        }

        //
        // Define access methods
        this.getUpsertedId = function() {
            if (bulkResult.upserted.length == 0) {
                return null;
            }

            return bulkResult.upserted[bulkResult.upserted.length - 1];
        };

        this.getRawResponse = function() {
            return bulkResult;
        };

        this.getWriteError = function() {
            if (bulkResult.writeErrors.length == 0) {
                return null;
            } else {
                return bulkResult.writeErrors[bulkResult.writeErrors.length - 1];
            }
        };

        this.hasWriteError = function() {
            return this.getWriteError() != null;
        };

        this.getWriteConcernError = function() {
            if (bulkResult.writeConcernErrors.length == 0) {
                return null;
            } else {
                return bulkResult.writeConcernErrors[0];
            }
        };

        this.hasWriteConcernError = function() {
            return this.getWriteConcernError() != null;
        };

        /**
         * @return {string}
         */
        this.tojson = function(indent, nolint) {
            var result = {};

            if (singleBatchType == INSERT) {
                result.nInserted = this.nInserted;
            }

            if (singleBatchType == UPDATE) {
                result.nMatched = this.nMatched;
                result.nUpserted = this.nUpserted;

                if (this.nModified != undefined)
                    result.nModified = this.nModified;

                if (Array.isArray(bulkResult.upserted) && bulkResult.upserted.length == 1) {
                    result._id = bulkResult.upserted[0]._id;
                }
            }

            if (singleBatchType == REMOVE) {
                result.nRemoved = bulkResult.nRemoved;
            }

            if (this.getWriteError() != null) {
                result.writeError = {};
                result.writeError.code = this.getWriteError().code;
                result.writeError.errmsg = this.getWriteError().errmsg;
                let errInfo = this.getWriteError().errInfo;
                if (errInfo) {
                    result.writeError.errInfo = errInfo;
                }
            }

            if (this.getWriteConcernError() != null) {
                result.writeConcernError = this.getWriteConcernError();
            }

            return tojson(result, indent, nolint);
        };

        this.toString = function() {
            // Suppress all output for the write concern w:0, since the client doesn't care.
            if (writeConcern && writeConcern.w == 0) {
                return "WriteResult(" + tojson({}) + ")";
            }
            return "WriteResult(" + this.tojson() + ")";
        };

        this.shellPrint = function() {
            return this.toString();
        };
    };

    /**
     * Wraps the result for the commands
     */
    var BulkWriteResult = function(bulkResult, singleBatchType, writeConcern) {
        if (!(this instanceof BulkWriteResult) && !(this instanceof BulkWriteError))
            return new BulkWriteResult(bulkResult, singleBatchType, writeConcern);

        // Define properties
        defineReadOnlyProperty(this, "ok", bulkResult.ok);
        defineReadOnlyProperty(this, "nInserted", bulkResult.nInserted);
        defineReadOnlyProperty(this, "nUpserted", bulkResult.nUpserted);
        defineReadOnlyProperty(this, "nMatched", bulkResult.nMatched);
        defineReadOnlyProperty(this, "nModified", bulkResult.nModified);
        defineReadOnlyProperty(this, "nRemoved", bulkResult.nRemoved);

        //
        // Define access methods
        this.getUpsertedIds = function() {
            return bulkResult.upserted;
        };

        this.getUpsertedIdAt = function(index) {
            return bulkResult.upserted[index];
        };

        this.getRawResponse = function() {
            return bulkResult;
        };

        this.hasWriteErrors = function() {
            return bulkResult.writeErrors.length > 0;
        };

        this.getWriteErrorCount = function() {
            return bulkResult.writeErrors.length;
        };

        this.getWriteErrorAt = function(index) {
            if (index < bulkResult.writeErrors.length) {
                return bulkResult.writeErrors[index];
            }
            return null;
        };

        //
        // Get all errors
        this.getWriteErrors = function() {
            return bulkResult.writeErrors;
        };

        this.hasWriteConcernError = function() {
            return bulkResult.writeConcernErrors.length > 0;
        };

        this.getWriteConcernError = function() {
            if (bulkResult.writeConcernErrors.length == 0) {
                return null;
            } else if (bulkResult.writeConcernErrors.length == 1) {
                // Return the error
                return bulkResult.writeConcernErrors[0];
            } else {
                // Combine the errors
                var errmsg = "";
                for (var i = 0; i < bulkResult.writeConcernErrors.length; i++) {
                    var err = bulkResult.writeConcernErrors[i];
                    errmsg = errmsg + err.errmsg;
                    // TODO: Something better
                    if (i != bulkResult.writeConcernErrors.length - 1) {
                        errmsg = errmsg + " and ";
                    }
                }

                return new WriteConcernError({errmsg: errmsg, code: WRITE_CONCERN_FAILED});
            }
        };

        /**
         * @return {string}
         */
        this.tojson = function(indent, nolint) {
            return tojson(bulkResult, indent, nolint);
        };

        this.toString = function() {
            // Suppress all output for the write concern w:0, since the client doesn't care.
            if (writeConcern && writeConcern.w == 0) {
                return "BulkWriteResult(" + tojson({}) + ")";
            }
            return "BulkWriteResult(" + this.tojson() + ")";
        };

        this.shellPrint = function() {
            return this.toString();
        };

        this.hasErrors = function() {
            return this.hasWriteErrors() || this.hasWriteConcernError();
        };

        this.toError = function() {
            if (this.hasErrors()) {
                // Create a combined error message
                var message = "";
                var numWriteErrors = this.getWriteErrorCount();
                if (numWriteErrors == 1) {
                    message += "write error at item " + this.getWriteErrors()[0].index;
                } else if (numWriteErrors > 1) {
                    message += numWriteErrors + " write errors";
                }

                var hasWCError = this.hasWriteConcernError();
                if (numWriteErrors > 0 && hasWCError) {
                    message += " and ";
                }

                if (hasWCError) {
                    message += "problem enforcing write concern";
                }
                message += " in bulk operation";

                return new BulkWriteError(bulkResult, singleBatchType, writeConcern, message);
            } else {
                throw Error("batch was successful, cannot create BulkWriteError");
            }
        };

        /**
         * @return {WriteResult} the simplified results condensed into one.
         */
        this.toSingleResult = function() {
            if (singleBatchType == null)
                throw Error("Cannot output single WriteResult from multiple batch result");
            return new WriteResult(bulkResult, singleBatchType, writeConcern);
        };
    };

    /**
     * Represents a bulk write error, identical to a BulkWriteResult but thrown
     */
    var BulkWriteError = function(bulkResult, singleBatchType, writeConcern, message) {
        if (!(this instanceof BulkWriteError))
            return new BulkWriteError(bulkResult, singleBatchType, writeConcern, message);

        this.name = 'BulkWriteError';
        this.message = message || 'unknown bulk write error';

        // Bulk errors are basically bulk results with additional error information
        BulkWriteResult.apply(this, arguments);

        // Override some particular methods
        delete this.toError;

        this.toString = function() {
            return "BulkWriteError(" + this.tojson() + ")";
        };
        this.stack = this.toString() + "\n" + (new Error().stack);

        this.toResult = function() {
            return new BulkWriteResult(bulkResult, singleBatchType, writeConcern);
        };
    };

    BulkWriteError.prototype = Object.create(Error.prototype);
    BulkWriteError.prototype.constructor = BulkWriteError;

    var getEmptyBulkResult = function() {
        return {
            writeErrors: [],
            writeConcernErrors: [],
            nInserted: 0,
            nUpserted: 0,
            nMatched: 0,
            nModified: 0,
            nRemoved: 0,
            upserted: []
        };
    };

    /**
     * Wraps a command error
     */
    var WriteCommandError = function(commandError) {
        if (!(this instanceof WriteCommandError))
            return new WriteCommandError(commandError);

        // Define properties
        defineReadOnlyProperty(this, "code", commandError.code);
        defineReadOnlyProperty(this, "errmsg", commandError.errmsg);
        if (commandError.hasOwnProperty("errorLabels")) {
            defineReadOnlyProperty(this, "errorLabels", commandError.errorLabels);
        }

        this.name = 'WriteCommandError';
        this.message = this.errmsg;

        /**
         * @return {string}
         */
        this.tojson = function(indent, nolint) {
            return tojson(commandError, indent, nolint);
        };

        this.toString = function() {
            return "WriteCommandError(" + this.tojson() + ")";
        };
        this.stack = this.toString() + "\n" + (new Error().stack);

        this.shellPrint = function() {
            return this.toString();
        };
    };

    WriteCommandError.prototype = Object.create(Error.prototype);
    WriteCommandError.prototype.constructor = WriteCommandError;

    /**
     * Wraps an error for a single write
     */
    var WriteError = function(err) {
        if (!(this instanceof WriteError))
            return new WriteError(err);

        this.name = 'WriteError';
        this.message = err.errmsg || 'unknown write error';

        // Define properties
        defineReadOnlyProperty(this, "code", err.code);
        defineReadOnlyProperty(this, "index", err.index);
        defineReadOnlyProperty(this, "errmsg", err.errmsg);
        // errInfo field is optional.
        if (err.hasOwnProperty("errInfo"))
            defineReadOnlyProperty(this, "errInfo", err.errInfo);

        //
        // Define access methods
        this.getOperation = function() {
            return err.op;
        };

        /**
         * @return {string}
         */
        this.tojson = function(indent, nolint) {
            return tojson(err, indent, nolint);
        };

        this.toString = function() {
            return "WriteError(" + tojson(err) + ")";
        };
        this.stack = this.toString() + "\n" + (new Error().stack);

        this.shellPrint = function() {
            return this.toString();
        };
    };

    WriteError.prototype = Object.create(Error.prototype);
    WriteError.prototype.constructor = WriteError;

    /**
     * Wraps a write concern error
     */
    var WriteConcernError = function(err) {
        if (!(this instanceof WriteConcernError))
            return new WriteConcernError(err);

        this.name = 'WriteConcernError';
        this.message = err.errmsg || 'unknown write concern error';

        // Define properties
        defineReadOnlyProperty(this, "code", err.code);
        defineReadOnlyProperty(this, "errInfo", err.errInfo);
        defineReadOnlyProperty(this, "errmsg", err.errmsg);

        /**
         * @return {string}
         */
        this.tojson = function(indent, nolint) {
            return tojson(err, indent, nolint);
        };

        this.toString = function() {
            return "WriteConcernError(" + tojson(err) + ")";
        };
        this.stack = this.toString() + "\n" + (new Error().stack);

        this.shellPrint = function() {
            return this.toString();
        };
    };

    WriteConcernError.prototype = Object.create(Error.prototype);
    WriteConcernError.prototype.constructor = WriteConcernError;

    /**
     * Keeps the state of an unordered batch so we can rewrite the results
     * correctly after command execution
     */
    var Batch = function(batchType, originalZeroIndex) {
        this.originalZeroIndex = originalZeroIndex;
        this.batchType = batchType;
        this.operations = [];
    };

    /***********************************************************
     * Wraps the operations done for the batch
     ***********************************************************/
    var Bulk = function(collection, ordered) {
        var self = this;
        var coll = collection;
        var executed = false;

        // Set max byte size
        var maxBatchSizeBytes = 1024 * 1024 * 16;
        var maxNumberOfDocsInBatch = (TestData && TestData.disableBatchWrites) ? 1 : 1000;
        var idFieldOverhead = Object.bsonsize({_id: ObjectId()}) - Object.bsonsize({});
        var writeConcern = null;
        var letParams = null;
        var currentOp;

        // Final results
        var bulkResult = getEmptyBulkResult();

        // Current batch
        var currentBatch = null;
        var currentIndex = 0;
        var currentBatchSize = 0;
        var currentBatchSizeBytes = 0;
        var batches = [];

        var defineBatchTypeCounter = function(self, name, type) {
            Object.defineProperty(self, name, {
                enumerable: true,
                get: function() {
                    var counter = 0;

                    for (var i = 0; i < batches.length; i++) {
                        if (batches[i].batchType == type) {
                            counter += batches[i].operations.length;
                        }
                    }

                    if (currentBatch && currentBatch.batchType == type) {
                        counter += currentBatch.operations.length;
                    }

                    return counter;
                }
            });
        };

        defineBatchTypeCounter(this, "nInsertOps", INSERT);
        defineBatchTypeCounter(this, "nUpdateOps", UPDATE);
        defineBatchTypeCounter(this, "nRemoveOps", REMOVE);

        // Convert bulk into string
        this.toString = function() {
            return this.tojson();
        };

        this.tojson = function() {
            return tojson({
                nInsertOps: this.nInsertOps,
                nUpdateOps: this.nUpdateOps,
                nRemoveOps: this.nRemoveOps,
                nBatches: batches.length + (currentBatch == null ? 0 : 1)
            });
        };

        this.getOperations = function() {
            return batches;
        };

        var finalizeBatch = function(newDocType) {
            // Save the batch to the execution stack
            batches.push(currentBatch);

            // Create a new batch
            currentBatch = new Batch(newDocType, currentIndex);

            // Reset the current size trackers
            currentBatchSize = 0;
            currentBatchSizeBytes = 0;
        };

        // Add to internal list of documents
        var addToOperationsList = function(docType, document) {
            if (Array.isArray(document))
                throw Error("operation passed in cannot be an Array");

            // Get the bsonSize
            var bsonSize = Object.bsonsize(document);

            // If an _id will be added to the insert, adjust the bsonSize
            if (docType === INSERT && documentNeedsId(document)) {
                bsonSize += idFieldOverhead;
            }

            // Create a new batch object if we don't have a current one
            if (currentBatch == null)
                currentBatch = new Batch(docType, currentIndex);

            // Finalize and create a new batch if this op would take us over the
            // limits *or* if this op is of a different type
            if (currentBatchSize + 1 > maxNumberOfDocsInBatch ||
                (currentBatchSize > 0 && currentBatchSizeBytes + bsonSize >= maxBatchSizeBytes) ||
                currentBatch.batchType != docType) {
                finalizeBatch(docType);
            }

            currentBatch.operations.push(document);
            currentIndex = currentIndex + 1;
            // Update current batch size
            currentBatchSize = currentBatchSize + 1;
            currentBatchSizeBytes = currentBatchSizeBytes + bsonSize;
        };

        /**
         *
         * @param obj {Object} the document to check if an _id is present
         * @returns true if the document needs an _id and false otherwise
         */
        var documentNeedsId = function(obj) {
            return typeof (obj._id) == "undefined" && !Array.isArray(obj);
        };

        /**
         * @return {Object} a new document with an _id: ObjectId if _id is not present.
         *     Otherwise, returns the same object passed.
         */
        var addIdIfNeeded = function(obj) {
            if (documentNeedsId(obj)) {
                var tmp = obj;  // don't want to modify input
                obj = {_id: new ObjectId()};
                for (var key in tmp) {
                    obj[key] = tmp[key];
                }
            }

            return obj;
        };

        /**
         * Add the insert document.
         *
         * @param document {Object} the document to insert.
         */
        this.insert = function(document) {
            return addToOperationsList(INSERT, document);
        };

        //
        // Find based operations
        var findOperations = {
            update: function(updateDocument) {
                // Set the top value for the update 0 = multi true, 1 = multi false
                var upsert = typeof currentOp.upsert == 'boolean' ? currentOp.upsert : false;
                // Establish the update command
                var document =
                    {q: currentOp.selector, u: updateDocument, multi: true, upsert: upsert};

                // Copy over the hint, if we have one.
                if (currentOp.hasOwnProperty('hint')) {
                    document.hint = currentOp.hint;
                }

                // Copy over the collation, if we have one.
                if (currentOp.hasOwnProperty('collation')) {
                    document.collation = currentOp.collation;
                }

                // Copy over the arrayFilters, if we have them.
                if (currentOp.hasOwnProperty('arrayFilters')) {
                    document.arrayFilters = currentOp.arrayFilters;
                }

                // Clear out current Op
                currentOp = null;
                // Add the update document to the list
                return addToOperationsList(UPDATE, document);
            },

            updateOne: function(updateDocument) {
                // Set the top value for the update 0 = multi true, 1 = multi false
                var upsert = typeof currentOp.upsert == 'boolean' ? currentOp.upsert : false;
                // Establish the update command
                var document =
                    {q: currentOp.selector, u: updateDocument, multi: false, upsert: upsert};

                // Copy over the hint, if we have one.
                if (currentOp.hasOwnProperty('hint')) {
                    document.hint = currentOp.hint;
                }

                // Copy over the collation, if we have one.
                if (currentOp.hasOwnProperty('collation')) {
                    document.collation = currentOp.collation;
                }

                // Copy over the arrayFilters, if we have them.
                if (currentOp.hasOwnProperty('arrayFilters')) {
                    document.arrayFilters = currentOp.arrayFilters;
                }

                // Clear out current Op
                currentOp = null;
                // Add the update document to the list
                return addToOperationsList(UPDATE, document);
            },

            replaceOne: function(updateDocument) {
                // Cannot use pipeline-style updates in a replacement operation.
                if (Array.isArray(updateDocument)) {
                    throw new Error('Cannot use pipeline-style updates in a replacement operation');
                }
                findOperations.updateOne(updateDocument);
            },

            upsert: function() {
                currentOp.upsert = true;
                // Return the findOperations
                return findOperations;
            },

            hint: function(hint) {
                currentOp.hint = hint;
                return findOperations;
            },

            removeOne: function() {
                // Establish the removeOne command
                var document = {q: currentOp.selector, limit: 1};

                // Copy over the collation, if we have one.
                if (currentOp.hasOwnProperty('collation')) {
                    document.collation = currentOp.collation;
                }

                // Clear out current Op
                currentOp = null;
                // Add the remove document to the list
                return addToOperationsList(REMOVE, document);
            },

            remove: function() {
                // Establish the remove command
                var document = {q: currentOp.selector, limit: 0};

                // Copy over the collation, if we have one.
                if (currentOp.hasOwnProperty('collation')) {
                    document.collation = currentOp.collation;
                }

                // Clear out current Op
                currentOp = null;
                // Add the remove document to the list
                return addToOperationsList(REMOVE, document);
            },

            collation: function(collationSpec) {
                currentOp.collation = collationSpec;
                return findOperations;
            },

            arrayFilters: function(filters) {
                currentOp.arrayFilters = filters;
                return findOperations;
            },
        };

        //
        // Start of update and remove operations
        this.find = function(selector) {
            if (selector == undefined)
                throw Error("find() requires query criteria");
            // Save a current selector
            currentOp = {selector: selector};

            // Return the find Operations
            return findOperations;
        };

        this.setLetParams = function(userLet) {
            letParams = userLet;
        };

        //
        // Merge write command result into aggregated results object
        var mergeBatchResults = function(batch, bulkResult, result) {
            // If we have an insert Batch type
            if (batch.batchType == INSERT) {
                bulkResult.nInserted = bulkResult.nInserted + result.n;
            }

            // If we have a remove batch type
            if (batch.batchType == REMOVE) {
                bulkResult.nRemoved = bulkResult.nRemoved + result.n;
            }

            var nUpserted = 0;

            // We have an array of upserted values, we need to rewrite the indexes
            if (Array.isArray(result.upserted)) {
                nUpserted = result.upserted.length;

                for (var i = 0; i < result.upserted.length; i++) {
                    bulkResult.upserted.push({
                        index: result.upserted[i].index + batch.originalZeroIndex,
                        _id: result.upserted[i]._id
                    });
                }
            } else if (result.upserted) {
                nUpserted = 1;

                bulkResult.upserted.push({index: batch.originalZeroIndex, _id: result.upserted});
            }

            // If we have an update Batch type
            if (batch.batchType == UPDATE) {
                bulkResult.nUpserted = bulkResult.nUpserted + nUpserted;
                bulkResult.nMatched = bulkResult.nMatched + (result.n - nUpserted);
                if (result.nModified == undefined) {
                    bulkResult.nModified = undefined;
                } else if (bulkResult.nModified != undefined) {
                    bulkResult.nModified = bulkResult.nModified + result.nModified;
                }
            }

            if (Array.isArray(result.writeErrors)) {
                for (var i = 0; i < result.writeErrors.length; i++) {
                    var writeError = {
                        index: batch.originalZeroIndex + result.writeErrors[i].index,
                        code: result.writeErrors[i].code,
                        errmsg: result.writeErrors[i].errmsg,
                        op: batch.operations[result.writeErrors[i].index]
                    };
                    var errInfo = result.writeErrors[i].errInfo;
                    if (errInfo) {
                        writeError['errInfo'] = errInfo;
                    }

                    bulkResult.writeErrors.push(new WriteError(writeError));
                }
            }

            if (result.writeConcernError) {
                bulkResult.writeConcernErrors.push(new WriteConcernError(result.writeConcernError));
            }
        };

        //
        // Constructs the write batch command.
        var buildBatchCmd = function(batch) {
            var cmd = null;

            // Generate the right update
            if (batch.batchType == UPDATE) {
                cmd = {update: coll.getName(), updates: batch.operations, ordered: ordered};
            } else if (batch.batchType == INSERT) {
                var transformedInserts = [];
                batch.operations.forEach(function(insertDoc) {
                    transformedInserts.push(addIdIfNeeded(insertDoc));
                });
                batch.operations = transformedInserts;

                cmd = {insert: coll.getName(), documents: batch.operations, ordered: ordered};
            } else if (batch.batchType == REMOVE) {
                cmd = {delete: coll.getName(), deletes: batch.operations, ordered: ordered};
            }

            // If we have a write concern
            if (writeConcern) {
                cmd.writeConcern = writeConcern;
            }

            // If we have let parameters, add them to the command.
            if (letParams) {
                cmd.let = letParams;
            }

            {
                const kWireVersionSupportingRetryableWrites = 6;
                const serverSupportsRetryableWrites =
                    coll.getMongo().getMinWireVersion() <= kWireVersionSupportingRetryableWrites &&
                    kWireVersionSupportingRetryableWrites <= coll.getMongo().getMaxWireVersion();

                const session = collection.getDB().getSession();
                if (serverSupportsRetryableWrites && session.getOptions().shouldRetryWrites() &&
                    _ServerSession.canRetryWrites(cmd) && !session._serverSession.isTxnActive()) {
                    cmd = session._serverSession.assignTransactionNumber(cmd);
                }
            }

            return cmd;
        };

        //
        // Execute the batch
        var executeBatch = function(batch) {
            var result = null;
            var cmd = buildBatchCmd(batch);

            // Run the command (may throw)
            result = collection.runCommand(cmd);

            if (result.ok == 0) {
                throw new WriteCommandError(result);
            }

            // Merge the results
            mergeBatchResults(batch, bulkResult, result);
        };

        // Execute the batch
        this.execute = function(_writeConcern) {
            if (executed)
                throw Error("A bulk operation cannot be re-executed");

            // If writeConcern set, use it, else get from collection (which will inherit from
            // db/mongo)
            writeConcern = _writeConcern ? _writeConcern : coll.getWriteConcern();
            if (writeConcern instanceof WriteConcern)
                writeConcern = writeConcern.toJSON();

            // If we have current batch
            if (currentBatch)
                batches.push(currentBatch);

            // Total number of batches to execute
            var totalNumberToExecute = batches.length;

            // Execute all the batches
            for (var i = 0; i < batches.length; i++) {
                // Execute the batch
                executeBatch(batches[i]);

                // If we are ordered and have errors and they are
                // not all replication errors terminate the operation
                if (bulkResult.writeErrors.length > 0 && ordered) {
                    // Ordered batches can't enforce full-batch write concern if they fail - they
                    // fail-fast
                    bulkResult.writeConcernErrors = [];
                    break;
                }
            }

            // Set as executed
            executed = true;

            // Create final result object
            typedResult = new BulkWriteResult(
                bulkResult, batches.length == 1 ? batches[0].batchType : null, writeConcern);
            // Throw on error
            if (typedResult.hasErrors()) {
                throw typedResult.toError();
            }

            return typedResult;
        };

        // Generate an explain command for the bulk operation. Currently we only support single
        // batches
        // of size 1, which must be either delete or update.
        this.convertToExplainCmd = function(verbosity) {
            // If we have current batch
            if (currentBatch) {
                batches.push(currentBatch);
            }

            // We can only explain singleton batches.
            if (batches.length !== 1) {
                throw Error("Explained bulk operations must consist of exactly 1 batch");
            }

            var explainBatch = batches[0];
            var writeCmd = buildBatchCmd(explainBatch);
            return {"explain": writeCmd, "verbosity": verbosity};
        };
    };

    //
    // Exports
    //

    module = {};
    module.WriteConcern = WriteConcern;
    module.WriteResult = WriteResult;
    module.BulkWriteResult = BulkWriteResult;
    module.BulkWriteError = BulkWriteError;
    module.WriteCommandError = WriteCommandError;
    module.WriteError = WriteError;
    module.initializeUnorderedBulkOp = function() {
        return new Bulk(this, false);
    };
    module.initializeOrderedBulkOp = function() {
        return new Bulk(this, true);
    };

    return module;
})();

// Globals
WriteConcern = _bulk_api_module.WriteConcern;
WriteResult = _bulk_api_module.WriteResult;
BulkWriteResult = _bulk_api_module.BulkWriteResult;
BulkWriteError = _bulk_api_module.BulkWriteError;
WriteCommandError = _bulk_api_module.WriteCommandError;
WriteError = _bulk_api_module.WriteError;

/***********************************************************
 * Adds the initializers of bulk operations to the db collection
 ***********************************************************/
DBCollection.prototype.initializeUnorderedBulkOp = _bulk_api_module.initializeUnorderedBulkOp;
DBCollection.prototype.initializeOrderedBulkOp = _bulk_api_module.initializeOrderedBulkOp;
