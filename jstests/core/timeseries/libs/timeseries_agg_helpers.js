load("jstests/core/timeseries/libs/timeseries.js");

/**
 * Helper class for aggregate tests with time-series collection.
 */
var TimeseriesAggTests = class {
    /**
     * Gets a test db object based on the test suite name.
     */
    static getTestDb() {
        return db.getSiblingDB(jsTestName());
    }

    /**
     * Prepares two input collections with exactly the same data set. One is a time-series
     * collection and the other one is a non time-series collection.
     *
     * @param numHosts Number of different hosts to generate.
     * @param numIterations Number of measurement generation loop iterations.
     * @returns An array of a time-series collection and a non time-series collection,
     *     respectively in this order.
     */
    static prepareInputCollections(numHosts, numIterations) {
        const timeseriesCollOption = {timeseries: {timeField: "time", metaField: "tags"}};

        Random.setRandomSeed();
        const hosts = TimeseriesTest.generateHosts(numHosts);

        const testDB = TimeseriesAggTests.getTestDb();

        // Creates a time-series input collection.
        const inColl = testDB.getCollection("in");
        inColl.drop();
        assert.commandWorked(testDB.createCollection(inColl.getName(), timeseriesCollOption));

        // Creates a non time-series observer input collection.
        const observerInColl = testDB.getCollection("observer_in");
        observerInColl.drop();
        assert.commandWorked(testDB.createCollection(observerInColl.getName()));
        const currTime = new Date();

        // Inserts exactly the same random measurement to both inColl and observerInColl.
        for (let i = 0; i < numIterations; i++) {
            for (let host of hosts) {
                const userUsage = TimeseriesTest.getRandomUsage();
                let newMeasurement = {
                    tags: host.tags,
                    time: new Date(currTime + i),
                    usage_guest: TimeseriesTest.getRandomUsage(),
                    usage_guest_nice: TimeseriesTest.getRandomUsage(),
                    usage_idle: TimeseriesTest.getRandomUsage(),
                    usage_iowait: TimeseriesTest.getRandomUsage(),
                    usage_irq: TimeseriesTest.getRandomUsage(),
                    usage_nice: TimeseriesTest.getRandomUsage(),
                    usage_softirq: TimeseriesTest.getRandomUsage(),
                    usage_steal: TimeseriesTest.getRandomUsage(),
                    usage_system: TimeseriesTest.getRandomUsage(),
                    usage_user: userUsage
                };
                assert.commandWorked(inColl.insert(newMeasurement));
                assert.commandWorked(observerInColl.insert(newMeasurement));

                if (i % 2) {
                    let idleMeasurement = {
                        tags: host.tags,
                        time: new Date(currTime + i),
                        idle_user: 100 - userUsage
                    };
                    assert.commandWorked(inColl.insert(idleMeasurement));
                    assert.commandWorked(observerInColl.insert(idleMeasurement));
                }
            }
        }

        return [inColl, observerInColl];
    }

    /**
     * Gets an output collection object with the name 'outCollname'.
     */
    static getOutputCollection(outCollName) {
        const testDB = TimeseriesAggTests.getTestDb();

        let outColl = testDB.getCollection(outCollName);
        outColl.drop();

        return outColl;
    }

    /**
     * Gets the aggregation results from the 'outColl' which the last stage of the 'pipeline' on the
     * 'inColl' will output the results to.
     *
     * Assumes that the last stage of the 'pipeline' is either $out or $merge.
     *
     * Executes 'prepareAction' before executing 'pipeline'. 'prepareAction' takes a collection
     * parameter and returns nothing.
     *
     * Returns sorted data by "time" field. The sorted result data will help simplify comparison
     * logic.
     */
    static getOutputAggregateResults(inColl, pipeline, prepareAction = null) {
        // Figures out the output collection name from the last pipeline stage.
        var outCollName = "out";
        if (pipeline[pipeline.length - 1]["$out"] != undefined) {
            // If the last stage is "$out", gets the output collection name from it.
            outCollName = pipeline[pipeline.length - 1]["$out"];
        } else if (pipeline[pipeline.length - 1]["$merge"] != undefined) {
            // If the last stage is "$merge", gets the output collection name from it.
            outCollName = pipeline[pipeline.length - 1]["$merge"].into;
        }

        let outColl = TimeseriesAggTests.getOutputCollection(outCollName);
        if (prepareAction != null) {
            prepareAction(outColl);
        }

        // Assumes that the pipeline's last stage outputs result into 'outColl'.
        assert.doesNotThrow(() => inColl.aggregate(pipeline));

        return outColl.find({}, {"_id": 0, "time": 1, "hostid": 1, "cpu": 1, "idle": 1})
            .sort({"time": 1})
            .toArray();
    }
};
