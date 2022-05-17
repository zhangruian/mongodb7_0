"""Unit tests for multiversion_service.py."""
from unittest import TestCase

from packaging.version import Version

import buildscripts.resmokelib.multiversion.multiversion_service as under_test

# pylint: disable=missing-docstring,invalid-name


class TestTagStr(TestCase):
    def test_require_fcv_tag_should_be_returned(self):
        self.assertEqual(under_test.tag_str(Version("6.0")), "requires_fcv_60")
        self.assertEqual(under_test.tag_str(Version("5.3")), "requires_fcv_53")
        self.assertEqual(under_test.tag_str(Version("31.41")), "requires_fcv_3141")


class TestGetVersion(TestCase):
    def test_version_should_be_extracted(self):
        mongo_version = under_test.MongoVersion(mongo_version="6.0.0-rc5-18-gbcdfaa9035b")

        self.assertEqual(mongo_version.get_version(), Version("6.0"))

    def test_incompatible_version_should_raise_an_exception(self):
        mongo_version = under_test.MongoVersion(mongo_version="not_a_version")

        with self.assertRaises(ValueError):
            mongo_version.get_version()


class TestCalculateFcvConstants(TestCase):
    def test_fcv_constants_should_be_accurate_for_lts_testing(self):
        mongo_version = under_test.MongoVersion(mongo_version="6.0")
        mongo_releases = under_test.MongoReleases(
            **{
                "featureCompatibilityVersions": [
                    "4.0", "4.2", "4.4", "4.7", "4.8", "4.9", "5.0", "5.1", "5.2", "5.3", "6.0",
                    "100.0"
                ],
                "longTermSupportReleases": ["4.0", "4.2", "4.4", "5.0"],
            })

        multiversion_service = under_test.MultiversionService(
            mongo_version=mongo_version,
            mongo_releases=mongo_releases,
        )

        fcv_constants = multiversion_service.calculate_fcv_constants()

        self.assertEqual(fcv_constants.latest, Version("6.0"))
        self.assertEqual(fcv_constants.last_continuous, Version("5.3"))
        self.assertEqual(fcv_constants.last_lts, Version("5.0"))
        self.assertEqual(fcv_constants.requires_fcv_tag_list,
                         [Version(v) for v in ["5.1", "5.2", "5.3", "6.0"]])
        self.assertEqual(fcv_constants.requires_fcv_tag_list_continuous, [Version("6.0")])
        self.assertEqual(fcv_constants.fcvs_less_than_latest, [
            Version(v)
            for v in ["4.0", "4.2", "4.4", "4.7", "4.8", "4.9", "5.0", "5.1", "5.2", "5.3"]
        ])
