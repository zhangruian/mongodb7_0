// The test runs commands that are not allowed with security token: createUser, logout,
// setProfilingLevel.
// @tags: [
//   not_allowed_with_security_token,
//   assumes_superuser_permissions,
//   creates_and_authenticates_user,
//   requires_profiling,
//   requires_auth,
//   # This test depends on hardcoded database name equality.
//   tenant_migration_incompatible,
// ]
// special db so that it can be run in parallel tests
var stddb = db;
var db = db.getSiblingDB("profile3");

db.dropAllUsers();
t = db.profile3;
t.drop();

profileCursor = function(query) {
    print("----");
    query = query || {};
    Object.extend(query, {user: username + "@" + db.getName()});
    return db.system.profile.find(query);
};

try {
    username = "jstests_profile3_user";
    db.createUser({user: username, pwd: "password", roles: jsTest.basicUserRoles});

    db.logout();
    db.auth(username, "password");

    db.setProfilingLevel(0);

    db.system.profile.drop();
    assert.eq(0, profileCursor().count());

    db.setProfilingLevel(2);

    db.createCollection(t.getName());
    t.insert({x: 1});
    t.findOne({x: 1});
    t.find({x: 1}).count();
    t.update({x: 1}, {$inc: {a: 1}, $set: {big: Array(128).toString()}});
    t.update({x: 1}, {$inc: {a: 1}});
    t.update({x: 0}, {$inc: {a: 1}});

    profileCursor().forEach(printjson);

    db.setProfilingLevel(0);

    assert.eq(profileCursor({nMatched: {$exists: 1}}).count(), 3);
    assert.eq(profileCursor({nMatched: 1}).count(), 2);
    assert.eq(profileCursor({nMatched: 0}).count(), 1);
    db.system.profile.drop();

} finally {
    db.setProfilingLevel(0);
    db.logout();
    db = stddb;
}
