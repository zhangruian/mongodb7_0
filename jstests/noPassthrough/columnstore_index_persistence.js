/**
 * Tests that a columnstore index can be persisted and found in listIndexes after a server restart.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 *   # column store indexes are still under a feature flag and require full sbe
 *   uses_column_store_index,
 *   featureFlagColumnstoreIndexes,
 *   featureFlagSbeFull,
 *   # TODO SERVER-69884: featureFlag guarded tests shouldn't require explicit 'no_selinux' tag.
 *   no_selinux,
 * ]
 */

(function() {
'use strict';

load('jstests/libs/index_catalog_helpers.js');

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();

const collName = 'columnstore_index_persistence';
let db_primary = primary.getDB('test');
let coll_primary = db_primary.getCollection(collName);

assert.commandWorked(coll_primary.createIndex({"$**": "columnstore"}));

// Restarts the primary and checks the index spec is persisted.
rst.restart(primary);
rst.waitForPrimary();
const indexList = rst.getPrimary().getDB('test').getCollection(collName).getIndexes();
assert.neq(null, IndexCatalogHelpers.findByKeyPattern(indexList, {"$**": "columnstore"}));

rst.stopSet();
})();
