/**
 * Multiversion rollback test. Checks that rollback succeeds between a
 * 'latest' version rollback node and a 'last-lts' version sync source.
 */

(function() {
"use strict";
load("jstests/multiVersion/libs/multiversion_rollback.js");

var testName = "multiversion_rollback_latest_to_last_lts";
testMultiversionRollback(testName, "latest", "last-lts");
})();