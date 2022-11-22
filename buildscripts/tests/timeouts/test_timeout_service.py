"""Unit tests for timeout_service.py."""
import random
import unittest
from unittest.mock import MagicMock, patch

from requests.exceptions import HTTPError

import buildscripts.timeouts.timeout_service as under_test
from buildscripts.resmoke_proxy.resmoke_proxy import ResmokeProxyService
from buildscripts.util.teststats import HistoricTaskData, HistoricTestInfo

# pylint: disable=invalid-name,protected-access

NS = "buildscripts.timeouts.timeout_service"


def ns(relative_name):  # pylint: disable=invalid-name
    """Return a full name from a name relative to the test module"s name space."""
    return NS + "." + relative_name


def build_mock_service(resmoke_proxy=None):
    return under_test.TimeoutService(
        resmoke_proxy=resmoke_proxy if resmoke_proxy else MagicMock(spec_set=ResmokeProxyService))


def tst_stat_mock(file, duration, pass_count):
    return MagicMock(test_name=file, avg_duration_pass=duration, num_pass=pass_count, hooks=[])


def tst_runtime_mock(file, duration, pass_count):
    return MagicMock(test_name=file, avg_duration_pass=duration, num_pass=pass_count)


class TestGetTimeoutEstimate(unittest.TestCase):
    @patch(ns("HistoricTaskData.from_s3"))
    def test_no_stats_should_return_default_timeout(self, from_s3_mock: MagicMock):
        timeout_service = build_mock_service()
        from_s3_mock.return_value = []
        timeout_params = under_test.TimeoutParams(
            evg_project="my project",
            build_variant="bv",
            task_name="my task",
            suite_name="my suite",
            is_asan=False,
        )

        timeout = timeout_service.get_timeout_estimate(timeout_params)

        self.assertFalse(timeout.is_specified())

    @patch(ns("HistoricTaskData.from_s3"))
    def test_a_test_with_missing_history_should_cause_a_default_timeout(
            self, from_s3_mock: MagicMock):
        test_stats = [
            HistoricTestInfo(test_name=f"test_{i}.js", avg_duration=60, num_pass=1, hooks=[])
            for i in range(30)
        ]
        from_s3_mock.return_value = HistoricTaskData(test_stats)
        mock_resmoke_proxy = MagicMock(spec_set=ResmokeProxyService)
        mock_resmoke_proxy.list_tests.return_value = ["test_with_no_stats.js"]
        timeout_service = build_mock_service(resmoke_proxy=mock_resmoke_proxy)
        timeout_params = under_test.TimeoutParams(
            evg_project="my project",
            build_variant="bv",
            task_name="my task",
            suite_name="my suite",
            is_asan=False,
        )

        timeout = timeout_service.get_timeout_estimate(timeout_params)

        self.assertFalse(timeout.is_specified())

    @patch(ns("HistoricTaskData.from_s3"))
    def test_a_test_with_zero_runtime_history_should_cause_a_default_timeout(
            self, from_s3_mock: MagicMock):
        test_stats = [
            HistoricTestInfo(test_name=f"test_{i}.js", avg_duration=60, num_pass=1, hooks=[])
            for i in range(30)
        ]
        test_stats.append(
            HistoricTestInfo(test_name="zero.js", avg_duration=0.0, num_pass=1, hooks=[]))
        from_s3_mock.return_value = HistoricTaskData(test_stats)
        mock_resmoke_proxy = MagicMock(spec_set=ResmokeProxyService)
        mock_resmoke_proxy.list_tests.return_value = [ts.test_name for ts in test_stats]
        timeout_service = build_mock_service(resmoke_proxy=mock_resmoke_proxy)
        timeout_params = under_test.TimeoutParams(
            evg_project="my project",
            build_variant="bv",
            task_name="my task",
            suite_name="my suite",
            is_asan=False,
        )

        timeout = timeout_service.get_timeout_estimate(timeout_params)

        self.assertFalse(timeout.is_specified())

    @patch(ns("HistoricTaskData.from_s3"))
    def test_all_tests_with_runtime_history_should_use_custom_timeout(self,
                                                                      from_s3_mock: MagicMock):
        n_tests = 30
        test_runtime = 600
        test_stats = [
            HistoricTestInfo(test_name=f"test_{i}.js", avg_duration=test_runtime, num_pass=1,
                             hooks=[]) for i in range(n_tests)
        ]
        from_s3_mock.return_value = HistoricTaskData(test_stats)
        mock_resmoke_proxy = MagicMock(spec_set=ResmokeProxyService)
        mock_resmoke_proxy.list_tests.return_value = [ts.test_name for ts in test_stats]
        timeout_service = build_mock_service(resmoke_proxy=mock_resmoke_proxy)
        timeout_params = under_test.TimeoutParams(
            evg_project="my project",
            build_variant="bv",
            task_name="my task",
            suite_name="my suite",
            is_asan=False,
        )

        timeout = timeout_service.get_timeout_estimate(timeout_params)

        self.assertTrue(timeout.is_specified())
        self.assertEqual(1860, timeout.calculate_test_timeout(1))
        self.assertEqual(54180, timeout.calculate_task_timeout(1))


class TestGetTaskHookOverhead(unittest.TestCase):
    def test_no_stats_should_return_zero(self):
        timeout_service = build_mock_service()

        overhead = timeout_service.get_task_hook_overhead("suite", is_asan=False, test_count=30,
                                                          historic_stats=None)

        self.assertEqual(0.0, overhead)

    def test_stats_with_no_clean_every_n_should_return_zero(self):
        timeout_service = build_mock_service()
        test_stats = HistoricTaskData.from_stats_list(
            [tst_stat_mock(f"test_{i}.js", 60, 1) for i in range(30)])

        overhead = timeout_service.get_task_hook_overhead("suite", is_asan=False, test_count=30,
                                                          historic_stats=test_stats)

        self.assertEqual(0.0, overhead)

    def test_stats_with_clean_every_n_should_return_overhead(self):
        test_count = 30
        runtime = 25
        timeout_service = build_mock_service()
        test_stat_list = [tst_stat_mock(f"test_{i}.js", 60, 1) for i in range(test_count)]
        test_stat_list.extend([
            tst_stat_mock(f"test_{i}:{under_test.CLEAN_EVERY_N_HOOK}", runtime, 1)
            for i in range(10)
        ])
        random.shuffle(test_stat_list)
        test_stats = HistoricTaskData.from_stats_list(test_stat_list)

        overhead = timeout_service.get_task_hook_overhead(
            "suite", is_asan=True, test_count=test_count, historic_stats=test_stats)

        self.assertEqual(runtime * test_count, overhead)


class TestLookupHistoricStats(unittest.TestCase):
    @patch(ns("HistoricTaskData.from_s3"))
    def test_no_stats_from_evergreen_should_return_none(self, from_s3_mock: MagicMock):
        from_s3_mock.return_value = None
        timeout_service = build_mock_service()
        timeout_params = under_test.TimeoutParams(
            evg_project="my project",
            build_variant="bv",
            task_name="my task",
            suite_name="my suite",
            is_asan=False,
        )

        stats = timeout_service.lookup_historic_stats(timeout_params)

        self.assertIsNone(stats)

    @patch(ns("HistoricTaskData.from_s3"))
    def test_errors_from_evergreen_should_return_none(self, from_s3_mock: MagicMock):
        from_s3_mock.side_effect = HTTPError("failed to connect")
        timeout_service = build_mock_service()
        timeout_params = under_test.TimeoutParams(
            evg_project="my project",
            build_variant="bv",
            task_name="my task",
            suite_name="my suite",
            is_asan=False,
        )

        stats = timeout_service.lookup_historic_stats(timeout_params)

        self.assertIsNone(stats)

    @patch(ns("HistoricTaskData.from_s3"))
    def test_stats_from_evergreen_should_return_the_stats(self, from_s3_mock: MagicMock):
        test_stats = [tst_stat_mock(f"test_{i}.js", 60, 1) for i in range(100)]
        from_s3_mock.return_value = HistoricTaskData(test_stats)
        timeout_service = build_mock_service()
        timeout_params = under_test.TimeoutParams(
            evg_project="my project",
            build_variant="bv",
            task_name="my task",
            suite_name="my suite",
            is_asan=False,
        )

        stats = timeout_service.lookup_historic_stats(timeout_params)

        self.assertIsNotNone(stats)
        self.assertEqual(len(test_stats), len(stats.historic_test_results))


class TestGetCleanEveryNCadence(unittest.TestCase):
    def test_clean_every_n_cadence_on_asan(self):
        timeout_service = build_mock_service()

        cadence = timeout_service._get_clean_every_n_cadence("suite", True)

        self.assertEqual(1, cadence)

    def test_clean_every_n_cadence_from_hook_config(self):
        expected_n = 42
        mock_resmoke_proxy = MagicMock()
        mock_resmoke_proxy.read_suite_config.return_value = {
            "executor": {
                "hooks": [{
                    "class": "hook1",
                }, {
                    "class": under_test.CLEAN_EVERY_N_HOOK,
                    "n": expected_n,
                }]
            }
        }
        timeout_service = build_mock_service(resmoke_proxy=mock_resmoke_proxy)

        cadence = timeout_service._get_clean_every_n_cadence("suite", False)

        self.assertEqual(expected_n, cadence)

    def test_clean_every_n_cadence_no_n_in_hook_config(self):
        mock_resmoke_proxy = MagicMock()
        mock_resmoke_proxy.read_suite_config.return_value = {
            "executor": {
                "hooks": [{
                    "class": "hook1",
                }, {
                    "class": under_test.CLEAN_EVERY_N_HOOK,
                }]
            }
        }
        timeout_service = build_mock_service(resmoke_proxy=mock_resmoke_proxy)

        cadence = timeout_service._get_clean_every_n_cadence("suite", False)

        self.assertEqual(1, cadence)

    def test_clean_every_n_cadence_no_hook_config(self):
        mock_resmoke_proxy = MagicMock()
        mock_resmoke_proxy.read_suite_config.return_value = {
            "executor": {"hooks": [{
                "class": "hook1",
            }, ]}
        }
        timeout_service = build_mock_service(resmoke_proxy=mock_resmoke_proxy)

        cadence = timeout_service._get_clean_every_n_cadence("suite", False)

        self.assertEqual(1, cadence)
