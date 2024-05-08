"""
Test hook for verifying $queryStats collects expected metrics and can redact query shapes.

This runs in the background as other tests are ongoing.
"""

from buildscripts.resmokelib.testing.hooks.interface import Hook
from bson import binary
import pymongo.errors

QUERY_STATS_NOT_ENABLED_CODES = [224, 7373500, 6579000]


class RunQueryStats(Hook):
    """Runs $queryStats after every test, and clears the query stats store before every test."""

    IS_BACKGROUND = False

    def __init__(self, hook_logger, fixture, allow_feature_not_supported=False):
        """Initialize the RunQueryStats hook.

        Args:
            hook_logger: the logger instance for this hook.
            fixture: the target fixture (replica sets or a sharded cluster).
            allow_feature_not_supported: absorb 'QueryFeatureNotAllowed' errors when calling
                $queryStats. This is to support fuzzer suites that may manipulate the FCV.
        """
        description = "Read query stats data after each test."
        super().__init__(hook_logger, fixture, description)
        self.client = self.fixture.mongo_client()
        self.hmac_key = binary.Binary(("0" * 32).encode('utf-8'), 8)
        self.allow_feature_not_supported = allow_feature_not_supported

    def verify_query_stats(self, querystats_spec):
        """Verify a $queryStats call has all the right properties."""
        try:
            with self.client.admin.aggregate([{"$queryStats": querystats_spec}]) as cursor:
                nreturned = 0
                for operation in cursor:
                    assert "key" in operation
                    assert "metrics" in operation
                    assert "asOf" in operation
                    nreturned += 1
                self.logger.info("Found %d query stats entries.", nreturned)
        except pymongo.errors.OperationFailure as err:
            if self.allow_feature_not_supported and err.code in QUERY_STATS_NOT_ENABLED_CODES:
                self.logger.info("Encountered an error while running $queryStats. "
                                 "$queryStats will not be run for this test.")
            else:
                raise err

    def after_test(self, test, test_report):
        self.verify_query_stats({})
        self.verify_query_stats(
            {"transformIdentifiers": {"algorithm": "hmac-sha-256", "hmacKey": self.hmac_key}})

        # Log the number of evictions we encountered.
        server_status = self.client.admin.command({"serverStatus": 1})
        num_evicted_entries = server_status["metrics"]["queryStats"]["numEvicted"]
        if num_evicted_entries > 0:
            self.logger.info("Evicted %d query stats entries during test execution.",
                             num_evicted_entries)

    def before_test(self, test, test_report):
        try:
            # Clear out all existing entries, then reset the size cap.
            self.client.admin.command("setParameter", 1, internalQueryStatsCacheSize="0%")
            self.client.admin.command("setParameter", 1, internalQueryStatsCacheSize="1%")
        except pymongo.errors.OperationFailure as err:
            if self.allow_feature_not_supported and err.code in QUERY_STATS_NOT_ENABLED_CODES:
                self.logger.info("Encountered an error while configuring the query stats store. "
                                 "Query stats will not be collected for this test.")
            else:
                raise err
