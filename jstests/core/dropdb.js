// Test that a db does not exist after it is dropped.
// Disabled in the small oplog suite because the secondary may create a primary db
// with the same name as the dropped db when requesting a clone.

m = db.getMongo();
baseName = "jstests_dropdb";
ddb = db.getSiblingDB(baseName);

print("initial dbs: " + tojson(m.getDBNames()));

function check(shouldExist) {
    var dbs = m.getDBNames();
    assert.eq(Array.contains(dbs, baseName),
              shouldExist,
              "DB " + baseName + " should " + (shouldExist ? "" : "not ") + "exist." +
                  " dbs: " + tojson(dbs) + "\n" + tojson(m.getDBs()));
}

ddb.c.save({});
check(true);

assert.commandWorked(ddb.dropDatabase());
check(false);

assert.commandWorked(ddb.dropDatabase());
check(false);
