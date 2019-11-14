/**
 * Test the backup/restore process:
 * - 3 node replica set
 * - Mongo CRUD client
 * - Mongo FSM client
 * - fsyncLock Secondary
 * - cp DB files
 * - fsyncUnlock Secondary
 * - Start mongod as hidden secondary
 * - Wait until new hidden node becomes secondary
 *
 * Some methods for backup used in this test checkpoint the files in the dbpath. This technique will
 * not work for ephemeral storage engines, as they do not store any data in the dbpath.
 * TODO: (SERVER-44467) Remove two_phase_index_builds_unsupported tag when startup recovery works
 * for two-phase index builds.
 * @tags: [
 *     requires_persistence,
 *     requires_replication,
 *     two_phase_index_builds_unsupported,
 * ]
 */

load("jstests/noPassthrough/libs/backup_restore.js");

(function() {
"use strict";

// Run the fsyncLock test. Will return before testing for any engine that doesn't
// support fsyncLock
new BackupRestoreTest({backup: 'fsyncLock'}).run();
}());
