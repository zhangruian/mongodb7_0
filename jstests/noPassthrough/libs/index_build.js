// Helper functions for testing index builds.

class IndexBuildTest {
    /**
     * Starts an index build in a separate mongo shell process with given options.
     */
    static startIndexBuild(conn, ns, keyPattern, options) {
        options = options || {};
        return startParallelShell('const coll = db.getMongo().getCollection("' + ns + '");' +
                                      'assert.commandWorked(coll.createIndex(' +
                                      tojson(keyPattern) + ', ' + tojson(options) + '));',
                                  conn.port);
    }

    /**
     *  Returns the op id for the running index build, or -1 if there is no current index build.
     */
    static getIndexBuildOpId(database) {
        const result = database.currentOp();
        assert.commandWorked(result);
        let indexBuildOpId = -1;

        result.inprog.forEach(function(op) {
            if (op.op == 'command' && 'createIndexes' in op.command) {
                indexBuildOpId = op.opid;
            }
        });
        return indexBuildOpId;
    }

    /**
     * Wait for index build to start and return its op id.
     */
    static waitForIndexBuildToStart(database) {
        let opId;
        assert.soon(function() {
            return (opId = IndexBuildTest.getIndexBuildOpId(database)) !== -1;
        }, "Index build operation not found after starting via parallelShell");
        return opId;
    }

    /**
     * Wait for all index builds to stop and return its op id.
     */
    static waitForIndexBuildToStop(database) {
        assert.soon(function() {
            return IndexBuildTest.getIndexBuildOpId(database) === -1;
        }, "Index build operations still running after unblocking or killOp");
    }
    /**
     * Runs listIndexes command on collection.
     * If 'options' is provided, these will be sent along with the command request.
     * Asserts that all the indexes on this collection fit within the first batch of results.
     */
    static assertIndexes(coll, numIndexes, readyIndexes, notReadyIndexes, options) {
        notReadyIndexes = notReadyIndexes || [];
        options = options || {};

        let res = coll.runCommand("listIndexes", options);
        assert.eq(numIndexes,
                  res.cursor.firstBatch.length,
                  'unexpected number of indexes in collection: ' + tojson(res));

        // First batch contains all the indexes in the collection.
        assert.eq(0, res.cursor.id);

        // A map of index specs keyed by index name.
        const indexMap = res.cursor.firstBatch.reduce(
            (m, spec) => {
                m[spec.name] = spec;
                return m;
            },
            {});

        // Check ready indexes.
        for (let name of readyIndexes) {
            assert(indexMap.hasOwnProperty(name),
                   'ready index ' + name + ' missing from listIndexes result: ' + tojson(res));
            const spec = indexMap[name];
            assert(!spec.hasOwnProperty('buildUUID'),
                   'unexpected buildUUID field in ' + name + ' index spec: ' + tojson(spec));
        }

        // Check indexes that are not ready.
        for (let name of notReadyIndexes) {
            assert(indexMap.hasOwnProperty(name),
                   'not-ready index ' + name + ' missing from listIndexes result: ' + tojson(res));
            const spec = indexMap[name];
            assert(spec.hasOwnProperty('buildUUID'),
                   'expected buildUUID field in ' + name + ' index spec: ' + tojson(spec));
        }
    }

    /**
     * Prevent subsequent index builds from running to completion.
     */
    static pauseIndexBuilds(conn) {
        assert.commandWorked(conn.adminCommand(
            {configureFailPoint: 'hangAfterStartingIndexBuild', mode: 'alwaysOn'}));
    }

    /**
     * Unblock current and subsequent index builds.
     */
    static resumeIndexBuilds(conn) {
        assert.commandWorked(
            conn.adminCommand({configureFailPoint: 'hangAfterStartingIndexBuild', mode: 'off'}));
    }
}