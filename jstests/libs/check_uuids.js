// Contains helpers for checking UUIDs on collections.

/**
 * Verifies that all collections on all databases on the server with admin database adminDB have
 * UUIDs.
 */
function checkCollectionUUIDs(adminDB) {
    let res = adminDB.runCommand({"listDatabases": 1});
    assert.commandWorked(res);
    let databaseList = res.databases;

    databaseList.forEach(function(database) {
        let currentDatabase = adminDB.getSiblingDB(database.name);
        let collectionInfos = currentDatabase.getCollectionInfos();
        for (let i = 0; i < collectionInfos.length; i++) {
            // Always skip system.indexes due to SERVER-30500.
            if (collectionInfos[i].name == "system.indexes") {
                continue;
            }
            assert(collectionInfos[i].info.uuid,
                   "Expect uuid for collection: " + tojson(collectionInfos[i]));
        }
    });
}
