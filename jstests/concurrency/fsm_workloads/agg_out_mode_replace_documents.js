'use strict';

/**
 * agg_out_mode_replace_documents.js
 *
 * Tests $out with mode "replaceDocuments" concurrently with moveChunk operations on the output
 * collection.
 *
 * @tags: [requires_sharding, assumes_balancer_off, assumes_autosplit_off,
 * requires_non_retryable_writes]
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');                 // for extendWorkload
load('jstests/concurrency/fsm_workloads/agg_with_chunk_migrations.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {
    // Set the collection to run concurrent moveChunk operations as the output collection.
    $config.data.collWithMigrations = "out_mode_replace_documents";
    $config.data.threadRunCount = 0;

    $config.states.aggregate = function aggregate(db, collName, connCache) {
        // This pipeline will perform an upsert on the first run, and replacement-style update on
        // subsequent runs.
        const res = db[collName].aggregate([
            {$addFields: {_id: this.tid, count: this.threadRunCount}},
            {$out: {to: this.collWithMigrations, mode: "replaceDocuments"}},
        ]);

        // $out should always return 0 documents.
        assert.eq(0, res.itcount());
        // If running with causal consistency, the writes may not have propagated to the secondaries
        // yet.
        assert.soon(() => {
            return db[this.collWithMigrations]
                       .find({_id: this.tid, count: this.threadRunCount})
                       .itcount() == 1;
        });

        this.threadRunCount += 1;
    };

    return $config;
});
