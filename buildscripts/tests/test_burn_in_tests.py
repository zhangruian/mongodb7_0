"""Unit tests for buildscripts/burn_in_tests.py."""

from __future__ import absolute_import

import collections
import datetime
import os
import sys
import subprocess
import unittest

from math import ceil
from mock import Mock, mock_open, patch, MagicMock

import requests

import buildscripts.burn_in_tests as burn_in
import buildscripts.util.teststats as teststats_utils
import buildscripts.ciconfig.evergreen as evg

# pylint: disable=missing-docstring,protected-access,too-many-lines

BURN_IN = "buildscripts.burn_in_tests"
EVG_CI = "buildscripts.ciconfig.evergreen"
EVG_CLIENT = "buildscripts.client.evergreen"
_DATE = datetime.datetime(2018, 7, 15)
GIT = "buildscripts.git"
RESMOKELIB = "buildscripts.resmokelib"

GENERATE_RESMOKE_TASKS_BASENAME = "this_is_a_gen_task"
GENERATE_RESMOKE_TASKS_NAME = GENERATE_RESMOKE_TASKS_BASENAME + "_gen"
GET_GENERATE_RESMOKE_TASKS_NAME = lambda _: GENERATE_RESMOKE_TASKS_NAME
GENERATE_RESMOKE_TASKS_COMMAND = {
    "func": "generate resmoke tasks",
    "vars": {"suite": "suite3", "resmoke_args": "--shellWriteMode=commands"}
}

GENERATE_RESMOKE_TASKS_COMMAND2 = {
    "func": "generate resmoke tasks", "vars": {"resmoke_args": "--shellWriteMode=commands"}
}

MULTIVERSION_PATH = "/data/multiversion"
GENERATE_RESMOKE_TASKS_MULTIVERSION_COMMAND = {
    "func": "generate resmoke tasks",
    "vars": {"resmoke_args": "--shellWriteMode=commands", "use_multiversion": MULTIVERSION_PATH}
}

MULTIVERSION_COMMAND = {"func": "do multiversion setup"}
RUN_TESTS_MULTIVERSION_COMMAND = {
    "func": "run tests",
    "vars": {"resmoke_args": "--shellWriteMode=commands", "task_path_suffix": MULTIVERSION_PATH}
}


def tasks_mock(  #pylint: disable=too-many-arguments
        tasks, generate_resmoke_tasks_command=None, get_vars_task_name=None, run_tests_command=None,
        multiversion_path=None, multiversion_setup_command=None):
    task_list = Mock()
    task_list.tasks = []
    for idx, task in enumerate(tasks):
        task_list.tasks.append(Mock())
        task_list.tasks[idx].is_generate_resmoke_task = generate_resmoke_tasks_command is not None
        task_list.tasks[idx].is_run_tests_task = run_tests_command is not None
        task_list.tasks[idx].is_multiversion_task = multiversion_path is not None
        task_list.tasks[idx].generate_resmoke_tasks_command = generate_resmoke_tasks_command
        task_list.tasks[idx].run_tests_command = run_tests_command
        task_list.tasks[idx].get_vars_task_name = get_vars_task_name
        task_list.tasks[idx].name = task["name"]
        resmoke_args = task.get("combined_resmoke_args")
        task_list.tasks[idx].combined_resmoke_args = resmoke_args
        task_list.tasks[idx].resmoke_suite = evg.ResmokeArgs.get_arg(
            resmoke_args, "suites") if resmoke_args else None
        task_list.tasks[idx].multiversion_path = multiversion_path
        task_list.tasks[idx].multiversion_setup_command = multiversion_setup_command
        if task["name"].endswith("_gen"):
            task_list.tasks[idx].generated_task_name = task["name"][:-4]

    return task_list


VARIANTS = {
    "variantall":
        tasks_mock([{"name": "task1", "combined_resmoke_args": "--suites=suite1 var1arg1"},
                    {"name": "task2", "combined_resmoke_args": "--suites=suite1 var1arg2"},
                    {"name": "task3", "combined_resmoke_args": "--suites=suite1 var1arg3"}]),
    "variant1":
        tasks_mock([{"name": "task1", "combined_resmoke_args": "--suites=suite1 var1arg1"},
                    {"name": "task2"}]),
    "variant2":
        tasks_mock([{"name": "task2", "combined_resmoke_args": "var2arg1"},
                    {"name": "task3", "combined_resmoke_args": "--suites=suite3 var2arg3"}]),
    "variant3":
        tasks_mock([{"name": "task2", "combined_resmoke_args": "var3arg1"}]),
    "variant4":
        tasks_mock([]),
    "variant_multiversion":
        tasks_mock(
            [{"name": "multiversion_task", "combined_resmoke_args": "--suites=suite3 vararg"}],
            run_tests_command=RUN_TESTS_MULTIVERSION_COMMAND,
            multiversion_setup_command=RUN_TESTS_MULTIVERSION_COMMAND,
            multiversion_path=MULTIVERSION_PATH),
    "variant_generate_tasks":
        tasks_mock([{
            "name": GENERATE_RESMOKE_TASKS_NAME, "combined_resmoke_args": "--suites=suite3 vararg"
        }], generate_resmoke_tasks_command=GENERATE_RESMOKE_TASKS_COMMAND,
                   get_vars_task_name=GET_GENERATE_RESMOKE_TASKS_NAME),
    "variant_generate_tasks_no_suite":
        tasks_mock([{
            "name": GENERATE_RESMOKE_TASKS_NAME, "combined_resmoke_args": "--suites=suite3 vararg"
        }], generate_resmoke_tasks_command=GENERATE_RESMOKE_TASKS_COMMAND2,
                   get_vars_task_name=GET_GENERATE_RESMOKE_TASKS_NAME),
    "variant_generate_tasks_diff_names":
        tasks_mock([{
            "name": "gen_task_name_different_from_vars_task_name",
            "combined_resmoke_args": "--suites=suite3 vararg"
        }], generate_resmoke_tasks_command=GENERATE_RESMOKE_TASKS_COMMAND,
                   get_vars_task_name=GET_GENERATE_RESMOKE_TASKS_NAME),
    "variant_generate_tasks_multiversion":
        tasks_mock([{
            "name": GENERATE_RESMOKE_TASKS_NAME, "combined_resmoke_args": "--suites=suite3 vararg"
        }], generate_resmoke_tasks_command=GENERATE_RESMOKE_TASKS_MULTIVERSION_COMMAND,
                   get_vars_task_name=GET_GENERATE_RESMOKE_TASKS_NAME,
                   multiversion_path=MULTIVERSION_PATH),
}

EVERGREEN_CONF = Mock()
EVERGREEN_CONF.get_variant = VARIANTS.get
EVERGREEN_CONF.variant_names = VARIANTS.keys()


def _mock_parser():
    parser = Mock()
    parser.error = Mock()
    return parser


def _mock_evergreen_api():
    evergreen_api = Mock()
    evergreen_api.test_stats_by_project.return_value = [
        Mock(
            test_file="jstests/test1.js",
            task_name="task1",
            variant="variant1",
            distro="distro1",
            date=_DATE,
            num_pass=1,
            num_fail=0,
            avg_duration_pass=10,
        )
    ]
    return evergreen_api


class TestValidateOptions(unittest.TestCase):
    @staticmethod
    def _mock_options():
        options = Mock()
        options.repeat_tests_num = None
        options.repeat_tests_max = None
        options.repeat_tests_min = None
        options.repeat_tests_secs = None
        options.buildvariant = None
        options.run_buildvariant = None
        options.test_list_file = None
        return options

    def test_validate_options_listfile_buildvariant(self):
        mock_parser = _mock_parser()
        options = self._mock_options()
        options.test_list_file = "list_file.json"
        options.buildvariant = "variant1"
        with patch(EVG_CI + ".parse_evergreen_file", return_value=EVERGREEN_CONF):
            burn_in.validate_options(mock_parser, options)
            mock_parser.error.assert_not_called()

    def test_validate_options_nolistfile_buildvariant(self):
        mock_parser = _mock_parser()
        options = self._mock_options()
        options.buildvariant = "variant1"
        with patch(EVG_CI + ".parse_evergreen_file", return_value=EVERGREEN_CONF):
            burn_in.validate_options(mock_parser, options)
            mock_parser.error.assert_not_called()

    def test_validate_options_listfile_nobuildvariant(self):
        mock_parser = _mock_parser()
        options = self._mock_options()
        options.test_list_file = "list_file.json"
        with patch(EVG_CI + ".parse_evergreen_file", return_value=EVERGREEN_CONF):
            burn_in.validate_options(mock_parser, options)
            mock_parser.error.assert_not_called()

    def test_validate_options_no_listfile_no_buildvariant(self):
        mock_parser = _mock_parser()
        options = self._mock_options()
        with patch(EVG_CI + ".parse_evergreen_file", return_value=EVERGREEN_CONF):
            burn_in.validate_options(mock_parser, options)
            mock_parser.error.assert_called()

    def test_validate_options_buildvariant(self):
        mock_parser = _mock_parser()
        options = self._mock_options()
        options.buildvariant = "variant1"
        with patch(EVG_CI + ".parse_evergreen_file", return_value=EVERGREEN_CONF):
            burn_in.validate_options(mock_parser, options)
            mock_parser.error.assert_not_called()

    def test_validate_options_run_buildvariant(self):
        mock_parser = _mock_parser()
        options = self._mock_options()
        options.buildvariant = "variant1"
        options.run_buildvariant = "variant1"
        with patch(EVG_CI + ".parse_evergreen_file", return_value=EVERGREEN_CONF):
            burn_in.validate_options(mock_parser, options)
            mock_parser.error.assert_not_called()

    def test_validate_options_bad_buildvariant(self):
        mock_parser = _mock_parser()
        options = self._mock_options()
        options.buildvariant = "badvariant1"
        with patch(EVG_CI + ".parse_evergreen_file", return_value=EVERGREEN_CONF):
            burn_in.validate_options(mock_parser, options)
            mock_parser.error.assert_called()

    def test_validate_options_bad_run_buildvariant(self):
        mock_parser = _mock_parser()
        options = self._mock_options()
        options.run_buildvariant = "badvariant1"
        with patch(EVG_CI + ".parse_evergreen_file", return_value=EVERGREEN_CONF):
            burn_in.validate_options(mock_parser, options)
            mock_parser.error.assert_called()

    def test_validate_options_tests_max_no_tests_secs(self):
        mock_parser = _mock_parser()
        options = self._mock_options()
        options.repeat_tests_max = 3
        with patch(EVG_CI + ".parse_evergreen_file", return_value=EVERGREEN_CONF):
            burn_in.validate_options(mock_parser, options)
            mock_parser.error.assert_called()

    def test_validate_options_tests_min_no_tests_secs(self):
        mock_parser = _mock_parser()
        options = self._mock_options()
        options.repeat_tests_min = 3
        with patch(EVG_CI + ".parse_evergreen_file", return_value=EVERGREEN_CONF):
            burn_in.validate_options(mock_parser, options)
            mock_parser.error.assert_called()

    def test_validate_options_tests_min_gt_tests_max(self):
        mock_parser = _mock_parser()
        options = self._mock_options()
        options.repeat_tests_min = 3
        options.repeat_tests_max = 2
        with patch(EVG_CI + ".parse_evergreen_file", return_value=EVERGREEN_CONF):
            burn_in.validate_options(mock_parser, options)
            mock_parser.error.assert_called()

    def test_validate_options_tests_secs(self):
        mock_parser = _mock_parser()
        options = self._mock_options()
        options.buildvariant = "variant1"
        options.repeat_tests_min = 2
        options.repeat_tests_max = 1000
        options.repeat_tests_secs = 3
        with patch(EVG_CI + ".parse_evergreen_file", return_value=EVERGREEN_CONF):
            burn_in.validate_options(mock_parser, options)
            mock_parser.error.assert_not_called()

    def test_validate_options_tests_secs_and_tests_num(self):
        mock_parser = _mock_parser()
        options = self._mock_options()
        options.buildvariant = "variant1"
        options.repeat_tests_num = 1
        options.repeat_tests_min = 1
        options.repeat_tests_max = 3
        options.repeat_tests_secs = 3
        with patch(EVG_CI + ".parse_evergreen_file", return_value=EVERGREEN_CONF):
            burn_in.validate_options(mock_parser, options)
            mock_parser.error.assert_called()

    def test_validate_options_tests_secs_no_buildvariant(self):
        mock_parser = _mock_parser()
        options = self._mock_options()
        options.repeat_tests_min = 1
        options.repeat_tests_max = 3
        options.repeat_tests_secs = 3
        with patch(EVG_CI + ".parse_evergreen_file", return_value=EVERGREEN_CONF):
            burn_in.validate_options(mock_parser, options)
            mock_parser.error.assert_called()


class TestGetResmokeRepeatOptions(unittest.TestCase):
    @staticmethod
    def _options_mock():
        options = Mock()
        options.repeat_tests_secs = None
        options.repeat_tests_min = None
        options.repeat_tests_max = None
        options.repeat_tests_num = None
        return options

    def test_get_resmoke_repeat_options_default(self):
        options = self._options_mock()
        repeat_options = burn_in.get_resmoke_repeat_options(options)
        self.assertEqual(repeat_options, "--repeatSuites={}".format(burn_in.REPEAT_SUITES))

    def test_get_resmoke_repeat_options_num(self):
        options = self._options_mock()
        options.repeat_tests_num = 5
        repeat_options = burn_in.get_resmoke_repeat_options(options)
        self.assertEqual(repeat_options, "--repeatSuites={}".format(options.repeat_tests_num))

    def test_get_resmoke_repeat_options_secs(self):
        options = self._options_mock()
        options.repeat_tests_secs = 5
        repeat_options = burn_in.get_resmoke_repeat_options(options)
        self.assertEqual(repeat_options, "--repeatTestsSecs={}".format(options.repeat_tests_secs))

    def test_get_resmoke_repeat_options_secs_min(self):
        options = self._options_mock()
        options.repeat_tests_secs = 5
        options.repeat_tests_min = 2
        repeat_options = burn_in.get_resmoke_repeat_options(options)
        self.assertIn("--repeatTestsSecs={}".format(options.repeat_tests_secs), repeat_options)
        self.assertIn("--repeatTestsMin={}".format(options.repeat_tests_min), repeat_options)
        self.assertNotIn("--repeatTestsMax", repeat_options)
        self.assertNotIn("--repeatSuites", repeat_options)

    def test_get_resmoke_repeat_options_secs_max(self):
        options = self._options_mock()
        options.repeat_tests_secs = 5
        options.repeat_tests_max = 2
        repeat_options = burn_in.get_resmoke_repeat_options(options)
        self.assertIn("--repeatTestsSecs={}".format(options.repeat_tests_secs), repeat_options)
        self.assertIn("--repeatTestsMax={}".format(options.repeat_tests_max), repeat_options)
        self.assertNotIn("--repeatTestsMin", repeat_options)
        self.assertNotIn("--repeatSuites", repeat_options)

    def test_get_resmoke_repeat_options_secs_min_max(self):
        options = self._options_mock()
        options.repeat_tests_secs = 5
        options.repeat_tests_min = 2
        options.repeat_tests_max = 2
        repeat_options = burn_in.get_resmoke_repeat_options(options)
        self.assertIn("--repeatTestsSecs={}".format(options.repeat_tests_secs), repeat_options)
        self.assertIn("--repeatTestsMin={}".format(options.repeat_tests_min), repeat_options)
        self.assertIn("--repeatTestsMax={}".format(options.repeat_tests_max), repeat_options)
        self.assertNotIn("--repeatSuites", repeat_options)

    def test_get_resmoke_repeat_options_min(self):
        options = self._options_mock()
        options.repeat_tests_min = 2
        repeat_options = burn_in.get_resmoke_repeat_options(options)
        self.assertEqual(repeat_options, "--repeatSuites={}".format(burn_in.REPEAT_SUITES))

    def test_get_resmoke_repeat_options_max(self):
        options = self._options_mock()
        options.repeat_tests_max = 2
        repeat_options = burn_in.get_resmoke_repeat_options(options)
        self.assertEqual(repeat_options, "--repeatSuites={}".format(burn_in.REPEAT_SUITES))


class TestCheckVariant(unittest.TestCase):
    @staticmethod
    def test_check_variant():
        mock_parser = _mock_parser()
        buildvariant = "variant1"
        with patch(EVG_CI + ".parse_evergreen_file", return_value=EVERGREEN_CONF):
            burn_in.check_variant(buildvariant, mock_parser)
            mock_parser.error.assert_not_called()

    @staticmethod
    def test_check_variant_badvariant():
        mock_parser = _mock_parser()
        buildvariant = "badvariant"
        with patch(EVG_CI + ".parse_evergreen_file", return_value=EVERGREEN_CONF):
            burn_in.check_variant(buildvariant, mock_parser)
            mock_parser.error.assert_called()


class TestGetRunBuildvariant(unittest.TestCase):
    def test__get_run_buildvariant_rb(self):
        run_buildvariant = "variant1"
        buildvariant = "variant2"
        options = Mock()
        options.run_buildvariant = run_buildvariant
        options.buildvariant = buildvariant
        self.assertEqual(run_buildvariant, burn_in._get_run_buildvariant(options))

    def test__get_run_buildvariant_bv(self):
        buildvariant = "variant2"
        options = Mock()
        options.run_buildvariant = None
        options.buildvariant = buildvariant
        self.assertEqual(buildvariant, burn_in._get_run_buildvariant(options))


class TestParseAvgTestRuntime(unittest.TestCase):
    def test__parse_avg_test_runtime(self):
        task_avg_test_runtime_stats = [
            teststats_utils.TestRuntime(test_name="dir/test1.js", runtime=30.2),
            teststats_utils.TestRuntime(test_name="dir/test2.js", runtime=455.1)
        ]
        result = burn_in._parse_avg_test_runtime("dir/test2.js", task_avg_test_runtime_stats)
        self.assertEqual(result, 455.1)


class TestCalculateTimeout(unittest.TestCase):
    def test__calculate_timeout(self):
        avg_test_runtime = 455.1
        expected_result = ceil(avg_test_runtime * burn_in.AVG_TEST_TIME_MULTIPLIER)
        self.assertEqual(expected_result, burn_in._calculate_timeout(avg_test_runtime))

    def test__calculate_timeout_avg_is_less_than_min(self):
        avg_test_runtime = 10
        self.assertEqual(burn_in.MIN_AVG_TEST_TIME_SEC,
                         burn_in._calculate_timeout(avg_test_runtime))


class TestCalculateExecTimeout(unittest.TestCase):
    def test__calculate_exec_timeout(self):
        avg_test_runtime = 455.1
        repeat_tests_secs = 600
        options = Mock(repeat_tests_secs=repeat_tests_secs)
        expected_result = repeat_tests_secs + (
            (avg_test_runtime -
             (repeat_tests_secs % avg_test_runtime)) * burn_in.AVG_TEST_TIME_MULTIPLIER)
        self.assertEqual(
            ceil(expected_result), burn_in._calculate_exec_timeout(options, avg_test_runtime))


class TestGenerateTimeouts(unittest.TestCase):
    def test__generate_timeouts(self):
        shrub_commands = []
        task_avg_test_runtime_stats = [
            teststats_utils.TestRuntime(test_name="dir/test2.js", runtime=455.1)
        ]
        options = Mock(repeat_tests_secs=600)
        test_name = "dir/test2.js"
        burn_in._generate_timeouts(options, shrub_commands, test_name, task_avg_test_runtime_stats)

        self.assertEqual(len(shrub_commands), 1)
        command_definition = shrub_commands[0]
        self.assertEqual(command_definition.to_map()['params']['exec_timeout_secs'], 1531)
        self.assertEqual(command_definition.to_map()['params']['timeout_secs'], 1366)

    def test__generate_timeouts_no_results(self):
        shrub_commands = []
        task_avg_test_runtime_stats = []
        options = Mock(repeat_tests_secs=600)
        test_name = "dir/new_test.js"
        burn_in._generate_timeouts(options, shrub_commands, test_name, task_avg_test_runtime_stats)

        self.assertEqual(len(shrub_commands), 0)

    def test__generate_timeouts_avg_runtime_is_zero(self):
        shrub_commands = []
        task_avg_test_runtime_stats = [
            teststats_utils.TestRuntime(test_name="dir/test_with_zero_runtime.js", runtime=0)
        ]
        options = Mock(repeat_tests_secs=600)
        test_name = "dir/test_with_zero_runtime.js"
        burn_in._generate_timeouts(options, shrub_commands, test_name, task_avg_test_runtime_stats)

        self.assertEqual(len(shrub_commands), 0)


class TestGetTaskRuntimeHistory(unittest.TestCase):
    def test__get_task_runtime_history(self):
        evergreen_api = Mock()
        evergreen_api.test_stats_by_project.return_value = [
            Mock(
                test_file="dir/test2.js",
                task_name="task1",
                variant="variant1",
                distro="distro1",
                date=_DATE,
                num_pass=1,
                num_fail=0,
                avg_duration_pass=10.1,
            )
        ]
        analysis_duration = burn_in.AVG_TEST_RUNTIME_ANALYSIS_DAYS
        end_date = datetime.datetime.utcnow().replace(microsecond=0)
        start_date = end_date - datetime.timedelta(days=analysis_duration)

        result = burn_in._get_task_runtime_history(evergreen_api, "project1", "task1", "variant1")
        self.assertEqual(result, [("dir/test2.js", 10.1)])
        evergreen_api.test_stats_by_project.assert_called_with(
            "project1", after_date=start_date.strftime("%Y-%m-%d"),
            before_date=end_date.strftime("%Y-%m-%d"), group_by="test", group_num_days=14,
            tasks=["task1"], variants=["variant1"])

    def test__get_task_runtime_history_evg_degraded_mode_error(self):  # pylint: disable=invalid-name
        response = Mock()
        response.status_code = requests.codes.SERVICE_UNAVAILABLE
        evergreen_api = Mock()
        evergreen_api.test_stats_by_project.side_effect = requests.HTTPError(response=response)

        result = burn_in._get_task_runtime_history(evergreen_api, "project1", "task1", "variant1")
        self.assertEqual(result, [])


class TestGetTaskName(unittest.TestCase):
    def test__get_task_name(self):
        name = "mytask"
        task = Mock()
        task.is_generate_resmoke_task = False
        task.name = name
        self.assertEqual(name, burn_in._get_task_name(task))

    def test__get_task_name_generate_resmoke_task(self):
        task_name = "mytask"
        task = Mock(is_generate_resmoke_task=True, generated_task_name=task_name)
        self.assertEqual(task_name, burn_in._get_task_name(task))


class TestSetResmokeArgs(unittest.TestCase):
    def test__set_resmoke_args(self):
        resmoke_args = "--suites=suite1 test1.js"
        task = Mock()
        task.combined_resmoke_args = resmoke_args
        task.is_generate_resmoke_task = False
        self.assertEqual(resmoke_args, burn_in._set_resmoke_args(task))

    def test__set_resmoke_args_gen_resmoke_task(self):
        resmoke_args = "--suites=suite1 test1.js"
        new_suite = "suite2"
        new_resmoke_args = "--suites={} test1.js".format(new_suite)
        task = Mock()
        task.combined_resmoke_args = resmoke_args
        task.is_generate_resmoke_task = True
        task.get_vars_suite_name = lambda cmd_vars: cmd_vars["suite"]
        task.generate_resmoke_tasks_command = {"vars": {"suite": new_suite}}
        self.assertEqual(new_resmoke_args, burn_in._set_resmoke_args(task))

    def test__set_resmoke_args_gen_resmoke_task_no_suite(self):
        suite = "suite1"
        resmoke_args = "--suites={} test1.js".format(suite)
        task = Mock()
        task.combined_resmoke_args = resmoke_args
        task.is_generate_resmoke_task = True
        task.get_vars_suite_name = lambda cmd_vars: cmd_vars["task"]
        task.generate_resmoke_tasks_command = {"vars": {"task": suite}}
        self.assertEqual(resmoke_args, burn_in._set_resmoke_args(task))


class TestSetResmokeCmd(unittest.TestCase):
    def test__set_resmoke_cmd_no_opts_no_args(self):
        with patch(BURN_IN + ".get_resmoke_repeat_options", return_value=""):
            self.assertListEqual([sys.executable, "buildscripts/resmoke.py"],
                                 burn_in._set_resmoke_cmd(None, None))

    def test__set_resmoke_cmd_no_opts(self):
        args = ["arg1", "arg2"]
        with patch(BURN_IN + ".get_resmoke_repeat_options", return_value=""):
            self.assertListEqual(args, burn_in._set_resmoke_cmd(None, args))

    def test__set_resmoke_cmd(self):
        opts = "myopt1 myopt2"
        args = ["arg1", "arg2"]
        new_cmd = args + opts.split()
        with patch(BURN_IN + ".get_resmoke_repeat_options", return_value=opts):
            self.assertListEqual(new_cmd, burn_in._set_resmoke_cmd(opts, args))


class TestSubTaskName(unittest.TestCase):
    def test__sub_task_name(self):
        options = MagicMock(buildvariant="myvar", run_buildvariant=None)
        task = "mytask"
        task_num = 0
        self.assertEqual("burn_in:myvar_mytask_0", burn_in._sub_task_name(options, task, task_num))

    def test__sub_task_name_with_run_bv(self):
        options = MagicMock(buildvariant="myvar", run_buildvariant="run_var")
        task = "mytask"
        task_num = 0
        self.assertEqual("burn_in:run_var_mytask_0", burn_in._sub_task_name(
            options, task, task_num))


TESTS_BY_TASK = {
    "task1": {
        "resmoke_args": "--suites=suite1",
        "tests": ["jstests/test1.js", "jstests/test2.js"]},
    "task2": {
        "resmoke_args": "--suites=suite1",
        "tests": ["jstests/test1.js", "jstests/test3.js"]},
    "task3": {
        "resmoke_args": "--suites=suite3",
        "tests": ["jstests/test4.js", "jstests/test5.js"]},
    "task4": {
        "resmoke_args": "--suites=suite4", "tests": []},
    "taskmulti": {
        "resmoke_args": "--suites=suite4",
        "tests": ["jstests/multi1.js"],
        "use_multiversion": "/data/multi"},
} # yapf: disable


class TestCreateGenerateTasksFile(unittest.TestCase):
    @staticmethod
    def _options_mock():
        options = Mock()
        options.buildvariant = None
        options.run_buildvariant = None
        options.repeat_tests_secs = 600
        options.distro = None
        options.branch = "master"
        return options

    @staticmethod
    def _get_tests(tests_by_task):
        tests = []
        for task in tests_by_task:
            tests.extend(tests_by_task[task]["tests"])
        return tests

    def test_create_generate_tasks_file_tasks(self):
        evergreen_api = Mock()
        evergreen_api.test_stats_by_project.return_value = [
            Mock(
                test_file="jstests/test1.js",
                task_name="task1",
                variant="variant1",
                distro="distro1",
                date=_DATE,
                num_pass=1,
                num_fail=0,
                avg_duration_pass=10,
            ),
            Mock(
                test_file="jstests/test2.js",
                task_name="task1",
                variant="variant1",
                distro="distro1",
                date=_DATE,
                num_pass=1,
                num_fail=0,
                avg_duration_pass=10,
            ),
            Mock(
                test_file="jstests/multi1.js",
                task_name="task1",
                variant="variant1",
                distro="distro1",
                date=_DATE,
                num_pass=1,
                num_fail=0,
                avg_duration_pass=10,
            )
        ]
        options = self._options_mock()
        options.buildvariant = "myvariant"
        tests_by_task = TESTS_BY_TASK
        test_tasks = self._get_tests(tests_by_task)
        with patch(BURN_IN + "._write_json_file") as mock_write_json:
            burn_in.create_generate_tasks_file(evergreen_api, options, tests_by_task)
            evg_config = mock_write_json.call_args_list[0][0][0]
            evg_tasks = evg_config["tasks"]
            self.assertEqual(len(evg_tasks), len(test_tasks))
            # Check task1 - test1.js
            task = evg_tasks[0]
            self.assertEqual(task["name"], "burn_in:myvariant_task1_0")
            self.assertEqual(len(task["depends_on"]), 1)
            self.assertEqual(task["depends_on"][0]["name"], "compile")
            self.assertEqual(len(task["commands"]), 3)
            self.assertEqual(task["commands"][1]["func"], "do setup")
            self.assertEqual(task["commands"][2]["func"], "run tests")
            resmoke_args = task["commands"][2]["vars"]["resmoke_args"]
            self.assertIn("--suites=suite1", resmoke_args)
            self.assertIn("jstests/test1.js", resmoke_args)
            # Check task1 - test2.js
            task = evg_tasks[1]
            self.assertEqual(task["name"], "burn_in:myvariant_task1_1")
            self.assertEqual(len(task["depends_on"]), 1)
            self.assertEqual(task["depends_on"][0]["name"], "compile")
            self.assertEqual(len(task["commands"]), 3)
            self.assertEqual(task["commands"][1]["func"], "do setup")
            self.assertEqual(task["commands"][2]["func"], "run tests")
            resmoke_args = task["commands"][2]["vars"]["resmoke_args"]
            self.assertIn("--suites=suite1", resmoke_args)
            self.assertIn("jstests/test2.js", resmoke_args)
            # task[2] - task[5] are similar to task[0] & task[1]
            # Check taskmulti - multi1.js
            taskmulti = evg_tasks[6]
            self.assertEqual(taskmulti["name"], "burn_in:myvariant_taskmulti_0")
            self.assertEqual(len(taskmulti["depends_on"]), 1)
            self.assertEqual(taskmulti["depends_on"][0]["name"], "compile")
            self.assertEqual(len(taskmulti["commands"]), 4)
            self.assertEqual(taskmulti["commands"][1]["func"], "do setup")
            self.assertEqual(taskmulti["commands"][2]["func"], "do multiversion setup")
            self.assertEqual(taskmulti["commands"][3]["func"], "run tests")
            resmoke_args = taskmulti["commands"][3]["vars"]["resmoke_args"]
            self.assertIn("--suites=suite4", resmoke_args)
            self.assertIn("jstests/multi1.js", resmoke_args)
            self.assertEqual(taskmulti["commands"][3]["vars"]["task_path_suffix"], "/data/multi")

    def test_create_generate_tasks_file_variants(self):
        evergreen_api = _mock_evergreen_api()
        options = self._options_mock()
        options.buildvariant = "myvariant"
        tests_by_task = TESTS_BY_TASK
        with patch(BURN_IN + "._write_json_file") as mock_write_json:
            burn_in.create_generate_tasks_file(evergreen_api, options, tests_by_task)
            evg_config = mock_write_json.call_args_list[0][0][0]
            self.assertEqual(len(evg_config["buildvariants"]), 1)
            self.assertEqual(evg_config["buildvariants"][0]["name"], "myvariant")
            self.assertEqual(len(evg_config["buildvariants"][0]["tasks"]), 7)
            self.assertEqual(len(evg_config["buildvariants"][0]["display_tasks"]), 1)
            display_task = evg_config["buildvariants"][0]["display_tasks"][0]
            self.assertEqual(display_task["name"], burn_in.BURN_IN_TESTS_TASK)
            execution_tasks = display_task["execution_tasks"]
            self.assertEqual(len(execution_tasks), 8)
            self.assertEqual(execution_tasks[0], burn_in.BURN_IN_TESTS_GEN_TASK)
            self.assertEqual(execution_tasks[1], "burn_in:myvariant_task1_0")
            self.assertEqual(execution_tasks[2], "burn_in:myvariant_task1_1")
            self.assertEqual(execution_tasks[3], "burn_in:myvariant_task2_0")
            self.assertEqual(execution_tasks[4], "burn_in:myvariant_task2_1")
            self.assertEqual(execution_tasks[5], "burn_in:myvariant_task3_0")
            self.assertEqual(execution_tasks[6], "burn_in:myvariant_task3_1")
            self.assertEqual(execution_tasks[7], "burn_in:myvariant_taskmulti_0")

    def test_create_generate_tasks_file_run_variants(self):
        evergreen_api = _mock_evergreen_api()
        options = self._options_mock()
        options.buildvariant = "myvariant"
        options.run_buildvariant = "run_variant"
        tests_by_task = TESTS_BY_TASK
        with patch(BURN_IN + "._write_json_file") as mock_write_json:
            burn_in.create_generate_tasks_file(evergreen_api, options, tests_by_task)
            evg_config = mock_write_json.call_args_list[0][0][0]
            self.assertEqual(len(evg_config["buildvariants"]), 1)
            self.assertEqual(evg_config["buildvariants"][0]["name"], "run_variant")
            self.assertEqual(len(evg_config["buildvariants"][0]["tasks"]), 7)
            self.assertEqual(len(evg_config["buildvariants"][0]["display_tasks"]), 1)
            display_task = evg_config["buildvariants"][0]["display_tasks"][0]
            self.assertEqual(display_task["name"], burn_in.BURN_IN_TESTS_TASK)
            execution_tasks = display_task["execution_tasks"]
            self.assertEqual(len(execution_tasks), 8)
            self.assertEqual(execution_tasks[0], burn_in.BURN_IN_TESTS_GEN_TASK)
            self.assertEqual(execution_tasks[1], "burn_in:run_variant_task1_0")
            self.assertEqual(execution_tasks[2], "burn_in:run_variant_task1_1")
            self.assertEqual(execution_tasks[3], "burn_in:run_variant_task2_0")
            self.assertEqual(execution_tasks[4], "burn_in:run_variant_task2_1")
            self.assertEqual(execution_tasks[5], "burn_in:run_variant_task3_0")
            self.assertEqual(execution_tasks[6], "burn_in:run_variant_task3_1")
            self.assertEqual(execution_tasks[7], "burn_in:run_variant_taskmulti_0")

    def test_create_generate_tasks_file_distro(self):
        evergreen_api = _mock_evergreen_api()
        options = self._options_mock()
        options.buildvariant = "myvariant"
        options.distro = "mydistro"
        tests_by_task = TESTS_BY_TASK
        test_tasks = self._get_tests(tests_by_task)
        with patch(BURN_IN + "._write_json_file") as mock_write_json:
            burn_in.create_generate_tasks_file(evergreen_api, options, tests_by_task)
            evg_config = mock_write_json.call_args_list[0][0][0]
            self.assertEqual(len(evg_config["tasks"]), len(test_tasks))
            self.assertEqual(len(evg_config["buildvariants"]), 1)
            for variant in evg_config["buildvariants"]:
                for task in variant.get("tasks", []):
                    self.assertEqual(len(task["distros"]), 1)
                    self.assertEqual(task["distros"][0], options.distro)

    def test_create_generate_tasks_file_no_tasks(self):
        evergreen_api = _mock_evergreen_api()
        variant = "myvariant"
        options = self._options_mock()
        options.buildvariant = variant
        tests_by_task = {}
        with patch(BURN_IN + "._write_json_file") as mock_write_json:
            burn_in.create_generate_tasks_file(evergreen_api, options, tests_by_task)
            evg_config = mock_write_json.call_args_list[0][0][0]
            self.assertEqual(len(evg_config), 1)
            self.assertEqual(len(evg_config["buildvariants"]), 1)
            self.assertEqual(evg_config["buildvariants"][0]["name"], variant)
            display_tasks = evg_config["buildvariants"][0]["display_tasks"]
            self.assertEqual(len(display_tasks), 1)
            self.assertEqual(display_tasks[0]["name"], burn_in.BURN_IN_TESTS_TASK)
            execution_tasks = display_tasks[0]["execution_tasks"]
            self.assertEqual(len(execution_tasks), 1)
            self.assertEqual(execution_tasks[0], burn_in.BURN_IN_TESTS_GEN_TASK)


class UpdateReportDataTests(unittest.TestCase):
    def test_update_report_data_nofile(self):
        data = {}
        task = ""
        pathname = "file_exists"
        with patch("os.path.isfile", return_value=False) as mock_isfile,\
             patch("json.load", return_value=data) as mock_json:
            burn_in._update_report_data(data, pathname, task)
            self.assertEqual(mock_isfile.call_count, 1)
            self.assertEqual(mock_json.call_count, 0)

    def test_update_report_data(self):
        task1 = "task1"
        task2 = "task2"
        data = {
            "failures": 1,
            "results": [
                {"test_file": "test1:" + task1},
                {"test_file": "test2:" + task1}]
        } # yapf: disable
        new_data = {
            "failures": 1,
            "results": [
                {"test_file": "test3"},
                {"test_file": "test4"}]
        } # yapf: disable

        pathname = "file_exists"
        with patch("os.path.isfile", return_value=True),\
             patch("builtins.open", mock_open()),\
             patch("json.load", return_value=new_data):
            burn_in._update_report_data(data, pathname, task2)
            self.assertEqual(len(data["results"]), 4)
            self.assertEqual(data["failures"], 2)
            self.assertIn({"test_file": "test1:" + task1}, data["results"])
            self.assertIn({"test_file": "test3:" + task2}, data["results"])


class RunTests(unittest.TestCase):
    class SysExit(Exception):
        pass

    def _test_run_tests(self, no_exec, tests_by_task, resmoke_cmd):
        with patch("subprocess.check_call", return_value=None) as mock_subproc,\
             patch(BURN_IN + "._update_report_data", return_value=None),\
             patch(BURN_IN + "._write_json_file", return_value=None):
            burn_in.run_tests(no_exec, tests_by_task, resmoke_cmd, None)
            self.assertEqual(mock_subproc.call_count, len(tests_by_task.keys()))
            for idx, task in enumerate(sorted(tests_by_task)):
                for task_test in tests_by_task[task].get("tests", []):
                    self.assertIn(task_test, mock_subproc.call_args_list[idx][0][0])

    def test_run_tests_noexec(self):
        no_exec = True
        resmoke_cmd = None
        with patch("subprocess.check_call", return_value=None) as mock_subproc,\
             patch(BURN_IN + "._write_json_file", return_value=None) as mock_write_json:
            burn_in.run_tests(no_exec, TESTS_BY_TASK, resmoke_cmd, None)
            self.assertEqual(mock_subproc.call_count, 0)
            self.assertEqual(mock_write_json.call_count, 0)

    def test_run_tests_notests(self):
        no_exec = False
        tests_by_task = {}
        resmoke_cmd = ["python", "buildscripts/resmoke.py", "--continueOnFailure"]
        self._test_run_tests(no_exec, tests_by_task, resmoke_cmd)

    def test_run_tests_tests(self):
        no_exec = False
        resmoke_cmd = ["python", "buildscripts/resmoke.py", "--continueOnFailure"]
        self._test_run_tests(no_exec, TESTS_BY_TASK, resmoke_cmd)

    def test_run_tests_tests_resmoke_failure(self):
        no_exec = False
        resmoke_cmd = ["python", "buildscripts/resmoke.py", "--continueOnFailure"]
        error_code = -1
        with patch("subprocess.check_call", return_value=None) as mock_subproc,\
             patch("sys.exit", return_value=error_code) as mock_exit,\
             patch(BURN_IN + "._update_report_data", return_value=None),\
             patch(BURN_IN + "._write_json_file", return_value=None):
            mock_subproc.side_effect = subprocess.CalledProcessError(error_code, "err1")
            mock_exit.side_effect = self.SysExit(error_code)
            with self.assertRaises(self.SysExit):
                burn_in.run_tests(no_exec, TESTS_BY_TASK, resmoke_cmd, None)


class FindLastActivated(unittest.TestCase):
    def test_find_last_activated_task_first_rev(self):
        rev_list = ["rev1", "rev2", "rev3"]
        variant = "build_variant_0"
        branch = "master"
        evg_api = MagicMock()

        revision = burn_in.find_last_activated_task(rev_list, variant, branch, evg_api)
        self.assertEqual(revision, rev_list[0])

    def test_find_last_activated_task_last_rev(self):
        rev_list = ["rev1", "rev2", "rev3"]
        variant = "build_variant_0"
        branch = "master"
        evg_api = MagicMock()
        evg_api.version_by_id.return_value.build_by_variant.side_effect = [
            MagicMock(activated=False),
            MagicMock(activated=False),
            MagicMock(activated=True),
        ]

        revision = burn_in.find_last_activated_task(rev_list, variant, branch, evg_api)
        self.assertEqual(revision, rev_list[2])

    def test_find_last_activated_task_no_rev(self):
        rev_list = ["rev1", "rev2", "rev3"]
        variant = "build_variant_0"
        branch = "master"
        evg_api = MagicMock()
        evg_api.version_by_id.return_value.build_by_variant.return_value.activated = False

        revision = burn_in.find_last_activated_task(rev_list, variant, branch, evg_api)
        self.assertIsNone(revision)

    def test_find_last_activated_norevisions(self):
        rev_list = []
        variant = "build_variant_0"
        branch = "master"
        evg_api = MagicMock()

        revision = burn_in.find_last_activated_task(rev_list, variant, branch, evg_api)
        self.assertIsNone(revision)


MEMBERS_MAP = {
    "test1.js": ["suite1", "suite2"], "test2.js": ["suite1", "suite3"], "test3.js": [],
    "test4.js": ["suite1", "suite2", "suite3"], "test5.js": ["suite2"]
}

SUITE1 = Mock()
SUITE1.tests = ["test1.js", "test2.js", "test4.js"]
SUITE2 = Mock()
SUITE2.tests = ["test1.js"]
SUITE3 = Mock()
SUITE3.tests = ["test2.js", "test4.js"]


def _create_executor_list(suites, exclude_suites):
    with patch(RESMOKELIB + ".suitesconfig.create_test_membership_map", return_value=MEMBERS_MAP):
        return burn_in.create_executor_list(suites, exclude_suites)


class CreateExecutorList(unittest.TestCase):
    def test_create_executor_list_no_excludes(self):
        suites = [SUITE1, SUITE2]
        exclude_suites = []
        executor_list = _create_executor_list(suites, exclude_suites)
        self.assertEqual(executor_list["suite1"], SUITE1.tests)
        self.assertEqual(executor_list["suite2"], ["test1.js", "test4.js"])
        self.assertEqual(executor_list["suite3"], ["test2.js", "test4.js"])

    def test_create_executor_list_excludes(self):
        suites = [SUITE1, SUITE2]
        exclude_suites = ["suite3"]
        executor_list = _create_executor_list(suites, exclude_suites)
        self.assertEqual(executor_list["suite1"], SUITE1.tests)
        self.assertEqual(executor_list["suite2"], ["test1.js", "test4.js"])
        self.assertEqual(executor_list["suite3"], [])

    def test_create_executor_list_nosuites(self):
        executor_list = _create_executor_list([], [])
        self.assertEqual(executor_list, collections.defaultdict(list))

    @patch(RESMOKELIB + ".testing.suite.Suite")
    @patch(RESMOKELIB + ".suitesconfig.get_named_suites")
    def test_create_executor_list_runs_core_suite(self, mock_get_named_suites, mock_suite_class):
        mock_get_named_suites.return_value = ["core"]

        burn_in.create_executor_list([], [])
        self.assertEqual(mock_suite_class.call_count, 1)

    @patch(RESMOKELIB + ".testing.suite.Suite")
    @patch(RESMOKELIB + ".suitesconfig.get_named_suites")
    def test_create_executor_list_ignores_dbtest_suite(self, mock_get_named_suites,
                                                       mock_suite_class):
        mock_get_named_suites.return_value = ["dbtest"]

        burn_in.create_executor_list([], [])
        self.assertEqual(mock_suite_class.call_count, 0)


class CreateTaskList(unittest.TestCase):
    def test_create_task_list(self):
        variant = "variantall"
        suites = [SUITE1, SUITE2, SUITE3]
        exclude_suites = []
        suite_list = _create_executor_list(suites, exclude_suites)
        task_list = burn_in.create_task_list(EVERGREEN_CONF, variant, suite_list, exclude_suites)
        self.assertEqual(len(task_list), len(VARIANTS["variantall"].tasks))
        self.assertIn("task1", task_list)
        self.assertEqual(task_list["task1"]["resmoke_args"], "--suites=suite1 var1arg1")
        self.assertEqual(task_list["task1"]["tests"], SUITE1.tests)
        self.assertIsNone(task_list["task1"]["use_multiversion"])
        self.assertIn("task2", task_list)
        self.assertEqual(task_list["task2"]["resmoke_args"], "--suites=suite1 var1arg2")
        self.assertEqual(task_list["task2"]["tests"], SUITE1.tests)
        self.assertIsNone(task_list["task2"]["use_multiversion"])
        self.assertIn("task3", task_list)
        self.assertEqual(task_list["task3"]["resmoke_args"], "--suites=suite1 var1arg3")
        self.assertEqual(task_list["task3"]["tests"], SUITE1.tests)
        self.assertIsNone(task_list["task3"]["use_multiversion"])

    def test_create_task_list_multiversion(self):
        variant = "variant_multiversion"
        suites = [SUITE1, SUITE2, SUITE3]
        exclude_suites = []
        suite_list = _create_executor_list(suites, exclude_suites)
        task_list = burn_in.create_task_list(EVERGREEN_CONF, variant, suite_list, exclude_suites)
        self.assertEqual(len(task_list), len(VARIANTS["variant_multiversion"].tasks))
        self.assertEqual(task_list["multiversion_task"]["use_multiversion"], MULTIVERSION_PATH)

    def test_create_task_list_gen_tasks(self):
        variant = "variant_generate_tasks"
        suites = [SUITE3]
        exclude_suites = []
        suite_list = _create_executor_list(suites, exclude_suites)
        task_list = burn_in.create_task_list(EVERGREEN_CONF, variant, suite_list, exclude_suites)
        self.assertEqual(len(task_list), len(VARIANTS["variant_generate_tasks"].tasks))
        self.assertIn(GENERATE_RESMOKE_TASKS_BASENAME, task_list)
        self.assertEqual(task_list[GENERATE_RESMOKE_TASKS_BASENAME]["tests"], SUITE3.tests)
        self.assertIsNone(task_list[GENERATE_RESMOKE_TASKS_BASENAME]["use_multiversion"])

    def test_create_task_list_gen_tasks_multiversion(self):
        variant = "variant_generate_tasks_multiversion"
        suites = [SUITE3]
        exclude_suites = []
        suite_list = _create_executor_list(suites, exclude_suites)
        task_list = burn_in.create_task_list(EVERGREEN_CONF, variant, suite_list, exclude_suites)
        self.assertEqual(len(task_list), len(VARIANTS["variant_generate_tasks_multiversion"].tasks))
        self.assertEqual(task_list[GENERATE_RESMOKE_TASKS_BASENAME]["use_multiversion"],
                         MULTIVERSION_PATH)

    def test_create_task_list_gen_tasks_no_suite(self):
        variant = "variant_generate_tasks_no_suite"
        suites = [SUITE3]
        exclude_suites = []
        suite_list = _create_executor_list(suites, exclude_suites)
        task_list = burn_in.create_task_list(EVERGREEN_CONF, variant, suite_list, exclude_suites)
        self.assertEqual(len(task_list), len(VARIANTS["variant_generate_tasks_no_suite"].tasks))
        self.assertIn(GENERATE_RESMOKE_TASKS_BASENAME, task_list)
        self.assertEqual(task_list[GENERATE_RESMOKE_TASKS_BASENAME]["tests"], SUITE3.tests)

    def test_create_task_list_no_excludes(self):
        variant = "variant1"
        suites = [SUITE1, SUITE2]
        exclude_suites = []
        suite_list = _create_executor_list(suites, exclude_suites)
        task_list = burn_in.create_task_list(EVERGREEN_CONF, variant, suite_list, exclude_suites)
        self.assertEqual(len(task_list), 1)
        self.assertIn("task1", task_list)
        self.assertEqual(task_list["task1"]["resmoke_args"], "--suites=suite1 var1arg1")
        self.assertEqual(task_list["task1"]["tests"], SUITE1.tests)
        self.assertNotIn("task2", task_list)
        self.assertNotIn("task3", task_list)

    def test_create_task_list_with_excludes(self):
        variant = "variant2"
        suites = [SUITE1, SUITE2, SUITE3]
        suite_list = _create_executor_list(suites, [])
        exclude_suites = ["suite2"]
        task_list = burn_in.create_task_list(EVERGREEN_CONF, variant, suite_list, exclude_suites)
        self.assertEqual(len(task_list), 1)
        self.assertIn("task3", task_list)
        self.assertEqual(task_list["task3"]["resmoke_args"], "--suites=suite3 var2arg3")
        self.assertEqual(task_list["task3"]["tests"], SUITE3.tests)
        self.assertNotIn("task1", task_list)
        self.assertNotIn("task2", task_list)

    def test_create_task_list_no_suites(self):
        variant = "variant2"
        suite_list = {}
        exclude_suites = ["suite2"]
        task_list = burn_in.create_task_list(EVERGREEN_CONF, variant, suite_list, exclude_suites)
        self.assertEqual(len(task_list), 0)
        self.assertEqual(task_list, {})

    def test_create_task_list_novariant(self):
        class BadVariant(Exception):
            pass

        def _raise_bad_variant(code=0):
            raise BadVariant("Bad variant {}".format(code))

        variant = "novariant"
        suites = [SUITE1, SUITE2, SUITE3]
        suite_list = _create_executor_list(suites, [])
        with patch("sys.exit", _raise_bad_variant):
            with self.assertRaises(BadVariant):
                burn_in.create_task_list(EVERGREEN_CONF, variant, suite_list, [])


class FindChangedTests(unittest.TestCase):

    NUM_COMMITS = 10
    MOD_FILES = [os.path.normpath("jstests/test1.js"), os.path.normpath("jstests/test2.js")]
    REV_DIFF = dict(zip([str(x) for x in range(NUM_COMMITS)],
                        [MOD_FILES] * NUM_COMMITS))  #type: ignore
    NO_REV_DIFF = dict(
        zip([str(x) for x in range(NUM_COMMITS)], [None for _ in range(NUM_COMMITS)]))

    UNTRACKED_FILES = [
        os.path.normpath("jstests/untracked1.js"),
        os.path.normpath("jstests/untracked2.js")
    ]

    @staticmethod
    def _copy_rev_diff(rev_diff):
        """Use this method instead of copy.deepcopy().

        Note - it was discovered during testing that after using copy.deepcopy() that
        updating one key would update all of them, i.e.,
            rev_diff = {"1": ["abc"], 2": ["abc"]}
            copy_rev_diff = copy.deepcopy(rev_diff)
            copy_rev_diff["2"] += "xyz"
            print(rev_diff)
                Result: {"1": ["abc"], 2": ["abc"]}
            print(copy_rev_diff)
                Result: {"1": ["abc", "xyz"], 2": ["abc", "xyz"]}
        At this point no identifiable issue could be found related to this problem.
        """
        copy_rev_diff = {}
        for key in rev_diff:
            copy_rev_diff[key] = []
            for file_name in rev_diff[key]:
                copy_rev_diff[key].append(file_name)
        return copy_rev_diff

    @staticmethod
    def _get_rev_list(range1, range2):
        return [str(num) for num in range(range1, range2 + 1)]

    def _mock_git_repository(self, directory):
        return MockGitRepository(directory, FindChangedTests._get_rev_list(self.rev1, self.rev2),
                                 self.rev_diff, self.untracked_files)

    def _test_find_changed_tests(  #pylint: disable=too-many-arguments
            self, commit, max_revisions, variant, check_evg, rev1, rev2, rev_diff, untracked_files,
            last_activated_task=None):
        branch = "master"
        project = "project"
        # pylint: disable=attribute-defined-outside-init
        self.rev1 = rev1
        self.rev2 = rev2
        self.rev_diff = rev_diff
        self.untracked_files = untracked_files
        self.expected_changed_tests = []
        if commit is None and rev_diff:
            self.expected_changed_tests += rev_diff[str(self.NUM_COMMITS - 1)]
        elif rev_diff.get(commit, []):
            self.expected_changed_tests += rev_diff.get(commit, [])
        self.expected_changed_tests += untracked_files
        # pylint: enable=attribute-defined-outside-init
        evg_api = MagicMock()
        with patch(GIT + ".Repository", self._mock_git_repository),\
             patch("os.path.isfile", return_value=True),\
             patch(BURN_IN + ".find_last_activated_task", return_value=last_activated_task):
            return burn_in.find_changed_tests(branch, commit, max_revisions, variant, project,
                                              check_evg, evg_api)

    def test_find_changed_tests(self):
        commit = "3"
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", False, 0, 3,
                                                      self.REV_DIFF, self.UNTRACKED_FILES)
        self.assertEqual(changed_tests, self.expected_changed_tests)
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", True, 0, 3,
                                                      self.REV_DIFF, self.UNTRACKED_FILES)
        self.assertEqual(changed_tests, self.expected_changed_tests)

    def test_find_changed_tests_no_changes(self):
        commit = "3"
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", False, 0, 3,
                                                      self.NO_REV_DIFF, [])
        self.assertEqual(changed_tests, [])
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", True, 0, 3,
                                                      self.NO_REV_DIFF, [], "1")
        self.assertEqual(changed_tests, [])

    def test_find_changed_tests_check_evergreen(self):
        commit = "1"
        rev_diff = self._copy_rev_diff(self.REV_DIFF)
        rev_diff["2"] += [os.path.normpath("jstests/test.js")]
        expected_changed_tests = self.REV_DIFF[commit] + self.UNTRACKED_FILES
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", False, 0, 3, rev_diff,
                                                      self.UNTRACKED_FILES)
        self.assertEqual(changed_tests, expected_changed_tests)
        rev_diff = self._copy_rev_diff(self.REV_DIFF)
        rev_diff["3"] += [os.path.normpath("jstests/test.js")]
        expected_changed_tests = rev_diff["3"] + self.UNTRACKED_FILES
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", True, 0, 3, rev_diff,
                                                      self.UNTRACKED_FILES, "1")
        self.assertEqual(changed_tests, expected_changed_tests)

    def test_find_changed_tests_no_diff(self):
        commit = "3"
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", False, 0, 3,
                                                      self.NO_REV_DIFF, self.UNTRACKED_FILES)
        self.assertEqual(changed_tests, self.UNTRACKED_FILES)
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", True, 0, 3,
                                                      self.NO_REV_DIFF, self.UNTRACKED_FILES)
        self.assertEqual(changed_tests, self.UNTRACKED_FILES)

    def test_find_changed_tests_no_untracked(self):
        commit = "3"
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", False, 0, 3,
                                                      self.REV_DIFF, [])
        self.assertEqual(changed_tests, self.REV_DIFF[commit])
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", True, 0, 3,
                                                      self.REV_DIFF, [])
        self.assertEqual(changed_tests, self.REV_DIFF[commit])

    def test_find_changed_tests_no_base_commit(self):
        changed_tests = self._test_find_changed_tests(None, 5, "myvariant", False, 0, 3,
                                                      self.REV_DIFF, self.UNTRACKED_FILES)
        self.assertEqual(changed_tests, self.expected_changed_tests)
        changed_tests = self._test_find_changed_tests(None, 5, "myvariant", True, 0, 3,
                                                      self.REV_DIFF, self.UNTRACKED_FILES)
        self.assertEqual(changed_tests, self.expected_changed_tests)

    def test_find_changed_tests_non_js(self):
        commit = "3"
        rev_diff = self._copy_rev_diff(self.REV_DIFF)
        rev_diff[commit] += [os.path.normpath("jstests/test.yml")]
        untracked_files = self.UNTRACKED_FILES + [os.path.normpath("jstests/untracked.yml")]
        expected_changed_tests = self.REV_DIFF[commit] + self.UNTRACKED_FILES
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", False, 0, 3, rev_diff,
                                                      untracked_files)
        self.assertEqual(changed_tests, expected_changed_tests)
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", True, 0, 3, rev_diff,
                                                      untracked_files)
        self.assertEqual(changed_tests, expected_changed_tests)

    def test_find_changed_tests_not_in_jstests(self):
        commit = "3"
        rev_diff = self._copy_rev_diff(self.REV_DIFF)
        rev_diff[commit] += [os.path.normpath("other/test.js")]
        untracked_files = self.UNTRACKED_FILES + [os.path.normpath("other/untracked.js")]
        expected_changed_tests = self.REV_DIFF[commit] + self.UNTRACKED_FILES
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", False, 0, 3, rev_diff,
                                                      untracked_files)
        self.assertEqual(changed_tests, expected_changed_tests)
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", True, 0, 3, rev_diff,
                                                      untracked_files)
        self.assertEqual(changed_tests, expected_changed_tests)

    def test_find_changed_tests_no_revisions(self):
        commit = "3"
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", False, 0, 0,
                                                      self.REV_DIFF, self.UNTRACKED_FILES)
        self.assertEqual(changed_tests, self.expected_changed_tests)
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", True, 0, 0,
                                                      self.REV_DIFF, self.UNTRACKED_FILES)
        self.assertEqual(changed_tests, self.expected_changed_tests)

    def test_find_changed_tests_too_many_revisions(self):
        commit = "3"
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", False, 0, 9,
                                                      self.REV_DIFF, self.UNTRACKED_FILES)
        self.assertEqual(changed_tests, [])
        changed_tests = self._test_find_changed_tests(commit, 5, "myvariant", True, 0, 9,
                                                      self.REV_DIFF, self.UNTRACKED_FILES)
        self.assertEqual(changed_tests, [])


class MockGitRepository(object):
    def __init__(self, _, rev_list, rev_diff, untracked_files):
        self.rev_list = rev_list
        self.rev_diff = rev_diff
        self.untracked_files = untracked_files

    def _get_revs(self, rev_range):
        revs = rev_range.split("...")
        if not revs:
            return revs
        elif len(revs) == 1:
            revs.append("HEAD")
        if revs[1] == "HEAD" and self.rev_list:
            revs[1] = self.rev_list[-1]
        return revs

    def __get_rev_range(self, rev_range):
        commits = []
        if len(self.rev_list) < 2:
            return commits
        revs = self._get_revs(rev_range)
        latest_commit_found = False
        for commit in self.rev_list:
            latest_commit_found = latest_commit_found or revs[0] == commit
            if revs[1] == commit:
                break
            if latest_commit_found:
                commits.append(commit)
        return commits

    def get_merge_base(self, _):
        return self.rev_list[-1]

    def git_rev_list(self, args):
        return "\n".join(self.__get_rev_range(args[0])[::-1])

    def git_diff(self, args):
        revs = self._get_revs(args[1])
        if revs:
            diff_list = self.rev_diff.get(revs[-1], [])
            if diff_list:
                return "\n".join(diff_list)
        return ""

    def git_status(self, args):
        revs = self._get_revs(args[0])
        modified_files = [""]
        if revs:
            diff_list = self.rev_diff.get(revs[-1], [])
            if diff_list:
                modified_files = [" M {}".format(untracked) for untracked in diff_list]
        untracked_files = ["?? {}".format(untracked) for untracked in self.untracked_files]
        return "\n".join(modified_files + untracked_files)
