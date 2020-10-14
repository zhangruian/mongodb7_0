"""Test hook that runs tenant migrations continuously."""

import random
import threading
import time
import uuid

import bson
import pymongo.errors

from buildscripts.resmokelib import errors
from buildscripts.resmokelib import utils
from buildscripts.resmokelib.testing.fixtures import interface as fixture_interface
from buildscripts.resmokelib.testing.fixtures import tenant_migration
from buildscripts.resmokelib.testing.hooks import interface


class ContinuousTenantMigration(interface.Hook):  # pylint: disable=too-many-instance-attributes
    """Starts a tenant migration thread at the beginning of each test."""

    DESCRIPTION = ("Continuous tenant migrations")

    def __init__(self, hook_logger, fixture, shell_options):
        """Initialize the ContinuousTenantMigration.

        Args:
            hook_logger: the logger instance for this hook.
            fixture: the target TenantMigrationFixture containing two replica sets.
            shell_options: contains the global_vars which contains TestData.tenantId to be used for
                           tenant migrations.

        """
        interface.Hook.__init__(self, hook_logger, fixture, ContinuousTenantMigration.DESCRIPTION)

        self._tenant_id = shell_options["global_vars"]["TestData"]["tenantId"]
        if not isinstance(fixture, tenant_migration.TenantMigrationFixture) or \
                fixture.get_num_replica_sets() != 2:
            raise ValueError(
                "The ContinuousTenantMigration hook requires a TenantMigrationFixture with two replica sets"
            )
        self._tenant_migration_fixture = fixture

        self._tenant_migration_thread = None

    def before_suite(self, test_report):
        """Before suite."""
        # TODO (SERVER-50496): Make the hook start the migration thread once here instead of inside
        # before_test and make it run migrations continuously back and forth between the two replica
        # sets.
        if not self._tenant_migration_fixture:
            raise ValueError("No replica set pair to run migrations on")

    def after_suite(self, test_report):
        """After suite."""
        return

    def before_test(self, test, test_report):
        """Before test."""
        self.logger.info("Starting the migration thread.")
        self._tenant_migration_thread = _TenantMigrationThread(
            self.logger, self._tenant_migration_fixture, self._tenant_id)
        self._tenant_migration_thread.start()

    def after_test(self, test, test_report):
        """After test."""
        self.logger.info("Stopping the migration thread.")
        self._tenant_migration_thread.stop()
        self.logger.info("migration thread stopped.")


class _TenantMigrationThread(threading.Thread):  # pylint: disable=too-many-instance-attributes
    MIN_START_MIGRATION_DELAY_SECS = 0.1
    MAX_START_MIGRATION_DELAY_SECS = 0.25
    MIN_BLOCK_TIME_SECS = 1
    MAX_BLOCK_TIME_SECS = 2.5
    DONOR_START_MIGRATION_POLL_INTERVAL_SECS = 0.1

    def __init__(self, logger, tenant_migration_fixture, tenant_id):
        """Initialize _TenantMigrationThread."""
        threading.Thread.__init__(self, name="TenantMigrationThread")
        self.daemon = True
        self.logger = logger
        self._tenant_migration_fixture = tenant_migration_fixture
        self._tenant_id = tenant_id

        self._last_exec = time.time()

    def run(self):
        """Execute the thread."""
        if not self._tenant_migration_fixture:
            self.logger.warning("No replica set pair to run migrations on.")
            return

        try:
            now = time.time()
            self.logger.info("Starting a tenant migration for tenantId '%s'", self._tenant_id)
            self._run_migration(self._tenant_migration_fixture)
            self._last_exec = time.time()
            self.logger.info("Completed a tenant migration in %0d ms",
                             (self._last_exec - now) * 1000)
        except Exception:  # pylint: disable=W0703
            # Proactively log the exception when it happens so it will be
            # flushed immediately.
            self.logger.exception("Migration Thread threw exception")

    def stop(self):
        """Stop the thread."""
        self.join()

    def _enable_abort(self, donor_primary_client, donor_primary_port, donor_primary_rs_name):
        # Configure the failpoint to make the migration abort after the migration has been
        # blocking reads and writes for a randomly generated number of milliseconds
        # (< MAX_BLOCK_TIME_MILLISECS). Must be called with _disable_abort at the start and
        # end of each test so that each test uses its own randomly generated block time.
        try:
            donor_primary_client.admin.command(
                bson.SON(
                    [("configureFailPoint", "abortTenantMigrationAfterBlockingStarts"),
                     ("mode", "alwaysOn"),
                     ("data",
                      bson.SON(
                          [("blockTimeMS",
                            1000 * random.uniform(_TenantMigrationThread.MIN_BLOCK_TIME_SECS,
                                                  _TenantMigrationThread.MAX_BLOCK_TIME_SECS))]))]))
        except pymongo.errors.OperationFailure as err:
            self.logger.exception(
                "Unable to enable the failpoint to make migrations abort on donor primary on port "
                + "%d of replica set '%s'.", donor_primary_port, donor_primary_rs_name)
            raise errors.ServerFailure(
                "Unable to enable the failpoint to make migrations abort on donor primary on port "
                + "{} of replica set '{}': {}".format(donor_primary_port, donor_primary_rs_name,
                                                      err.args[0]))

    def _disable_abort(self, donor_primary_client, donor_primary_port, donor_primary_rs_name):
        try:
            donor_primary_client.admin.command(
                bson.SON([("configureFailPoint", "abortTenantMigrationAfterBlockingStarts"),
                          ("mode", "off")]))
        except pymongo.errors.OperationFailure as err:
            self.logger.exception(
                "Unable to disable the failpoint to make migrations abort on donor primary on port "
                + "%d of replica set '%s'.", donor_primary_port, donor_primary_rs_name)
            raise errors.ServerFailure(
                "Unable to disable the failpoint to make migrations abort on donor primary on port "
                + "{} of replica set '{}': {}".format(donor_primary_port, donor_primary_rs_name,
                                                      err.args[0]))

    def _run_migration(self, tenant_migration_fixture):
        donor_rs = tenant_migration_fixture.get_replset(0)
        recipient_rs = tenant_migration_fixture.get_replset(1)

        donor_primary = donor_rs.get_primary()
        donor_primary_client = donor_primary.mongo_client()

        time.sleep(
            random.uniform(_TenantMigrationThread.MIN_START_MIGRATION_DELAY_SECS,
                           _TenantMigrationThread.MAX_START_MIGRATION_DELAY_SECS))

        self.logger.info(
            "Starting a tenant migration with donor primary on port %d of replica set '%s'.",
            donor_primary.port, donor_rs.replset_name)

        cmd_obj = {
            "donorStartMigration": 1, "migrationId": bson.Binary(uuid.uuid4().bytes, 4),
            "recipientConnectionString": recipient_rs.get_driver_connection_url(),
            "tenantId": self._tenant_id, "readPreference": {"mode": "primary"}
        }

        try:
            self._enable_abort(donor_primary_client, donor_primary.port, donor_rs.replset_name)

            while True:
                # Keep polling the migration state until the migration completes, otherwise we might
                # end up disabling 'abortTenantMigrationAfterBlockingStarts' before the migration
                # enters the blocking state and aborts.
                res = donor_primary_client.admin.command(
                    cmd_obj,
                    bson.codec_options.CodecOptions(uuid_representation=bson.binary.UUID_SUBTYPE))
                if (not res["ok"] or res["state"] == "committed" or res["state"] == "aborted"):
                    break
                time.sleep(_TenantMigrationThread.DONOR_START_MIGRATION_POLL_INTERVAL_SECS)
        except pymongo.errors.PyMongoError:
            self.logger.exception(
                "Error running tenant migration with donor primary on port %d of replica set '%s'.",
                donor_primary.port, donor_rs.replset_name)
            raise
        finally:
            self._disable_abort(donor_primary_client, donor_primary.port, donor_rs.replset_name)
