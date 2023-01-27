// This test is related to mixed_mode_repl_nossl.js in
// the sslSpecial test set. This test must be run with --use-ssl

load("jstests/ssl/libs/ssl_helpers.js");

// Limit the amount of time we'll wait on a failure.
// Apply equally to success tests as well so that
// a failure to complete replication is more likely to
// give us a false negative, than a false positive.
ReplSetTest.kDefaultTimeoutMS = 3 * 60 * 1000;

// Verify that requireSSL allows ssl connections
print("=== Testing requireSSL/requireSSL cluster ===");
replShouldSucceed("require-require", requireSSL, requireSSL);

// Test mixed sslMode allowSSL/preferSSL
print("=== Testing allowSSL/preferSSL cluster ===");
replShouldSucceed("allow-prefer", allowSSL, preferSSL);

// Test mixed sslMode preferSSL/requireSSL
print("=== Testing preferSSL/requireSSL cluster ===");
replShouldSucceed("prefer-require", preferSSL, requireSSL);

// Test mixed sslMode disabled/preferSSL - should fail
print("=== Testing allowSSL/requireSSL cluster - SHOULD FAIL ===");
replShouldFail("allow-require", allowSSL, requireSSL);
