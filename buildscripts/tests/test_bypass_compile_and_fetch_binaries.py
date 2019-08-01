"""Unit tests for buildscripts/bypass_compile_and_fetch_binaries.py."""

import unittest

from mock import mock_open, patch, MagicMock

import buildscripts.bypass_compile_and_fetch_binaries as under_test

# pylint: disable=missing-docstring,protected-access,too-many-lines

NS = "buildscripts.bypass_compile_and_fetch_binaries"


def ns(relative_name):  # pylint: disable=invalid-name
    """Return a full name from a name relative to the test module"s name space."""
    return NS + "." + relative_name


class TestFileInGroup(unittest.TestCase):
    def test_file_is_in_group(self):
        target_file = "file 1"
        group = {
            "files": {target_file},
        }  # yapf: disable

        self.assertTrue(under_test._file_in_group(target_file, group))

    def test_file_is_in_directory(self):
        directory = "this/is/a/directory"
        target_file = directory + "/file 1"
        group = {
            "files": {},
            "directories": {directory}
        }  # yapf: disable

        self.assertTrue(under_test._file_in_group(target_file, group))

    def test_file_is_not_in_directory(self):
        directory = "this/is/a/directory"
        target_file = "some/other/dir/file 1"
        group = {
            "files": {},
            "directories": {directory}
        }  # yapf: disable

        self.assertFalse(under_test._file_in_group(target_file, group))

    def test_no_files_in_group_throws(self):
        group = {
            "directories": {}
        }  # yapf: disable

        with self.assertRaises(TypeError):
            under_test._file_in_group("file", group)

    def test_no_dirs_in_group_throws(self):
        group = {
            "files": {},
        }  # yapf: disable

        with self.assertRaises(TypeError):
            under_test._file_in_group("file", group)


class TestShouldBypassCompile(unittest.TestCase):
    @patch("builtins.open", mock_open(read_data=""))
    def test_nothing_in_patch_file(self):
        build_variant = "build_variant"
        self.assertTrue(under_test.should_bypass_compile("", build_variant))

    def test_change_to_blacklist_file(self):
        build_variant = "build_variant"
        git_changes = """
buildscripts/burn_in_tests.py
buildscripts/scons.py
buildscripts/yaml_key_value.py
        """.strip()

        with patch("builtins.open") as open_mock:
            open_mock.return_value.__enter__.return_value = git_changes.splitlines()
            self.assertFalse(under_test.should_bypass_compile(git_changes, build_variant))

    def test_change_to_blacklist_directory(self):
        build_variant = "build_variant"
        git_changes = """
buildscripts/burn_in_tests.py
buildscripts/idl/file.py
buildscripts/yaml_key_value.py
        """.strip()

        with patch("builtins.open") as open_mock:
            open_mock.return_value.__enter__.return_value = git_changes.splitlines()
            self.assertFalse(under_test.should_bypass_compile(git_changes, build_variant))

    def test_change_to_only_whitelist(self):
        build_variant = "build_variant"
        git_changes = """
buildscripts/burn_in_tests.py
buildscripts/yaml_key_value.py
jstests/test1.js
pytests/test2.py
        """.strip()

        with patch("builtins.open") as open_mock:
            open_mock.return_value.__enter__.return_value = git_changes.splitlines()
            self.assertTrue(under_test.should_bypass_compile(git_changes, build_variant))

    @staticmethod
    def variant_mock(evg_mock):
        return evg_mock.return_value.get_variant.return_value

    @patch(ns('parse_evergreen_file'))
    @patch(ns('_get_original_etc_evergreen'))
    def test_change_to_etc_evergreen_that_bypasses(self, get_original_mock, parse_evg_mock):
        build_variant = "build_variant"
        git_changes = """
buildscripts/burn_in_tests.py
etc/evergreen.yml
jstests/test1.js
pytests/test2.py
        """.strip()

        with patch("builtins.open") as open_mock:
            self.variant_mock(get_original_mock).expansion.return_value = "expansion 1"
            self.variant_mock(parse_evg_mock).expansion.return_value = "expansion 1"

            open_mock.return_value.__enter__.return_value = git_changes.splitlines()
            self.assertTrue(under_test.should_bypass_compile(git_changes, build_variant))

    @patch(ns('parse_evergreen_file'))
    @patch(ns('_get_original_etc_evergreen'))
    def test_change_to_etc_evergreen_that_compiles(self, get_original_mock, parse_evg_mock):
        build_variant = "build_variant"
        git_changes = """
buildscripts/burn_in_tests.py
etc/evergreen.yml
jstests/test1.js
pytests/test2.py
        """.strip()

        with patch("builtins.open") as open_mock:
            self.variant_mock(get_original_mock).expansion.return_value = "expansion 1"
            self.variant_mock(parse_evg_mock).expansion.return_value = "expansion 2"

            open_mock.return_value.__enter__.return_value = git_changes.splitlines()
            self.assertFalse(under_test.should_bypass_compile(git_changes, build_variant))


class TestFindBuildForPreviousCompileTask(unittest.TestCase):
    def test_find_build(self):
        revision = "a22"
        project = "project"
        build_variant = "variant"
        expected_build_id = "project_variant_patch_a22_date"
        evergreen_api = MagicMock()
        version_response = MagicMock()
        evergreen_api.version_by_id.return_value = version_response
        version_response.build_by_variant.return_value = MagicMock(id=expected_build_id)

        build_id = under_test.find_build_for_previous_compile_task(evergreen_api, revision, project,
                                                                   build_variant)
        self.assertEqual(build_id, expected_build_id)


class TestFindPreviousCompileTask(unittest.TestCase):
    def test_find_task(self):
        revision = "a22"
        build_id = "project_variant_patch_a22_date"
        evergreen_api = MagicMock()
        task_response = MagicMock(status="success")
        evergreen_api.task_by_id.return_value = task_response

        task = under_test.find_previous_compile_task(evergreen_api, build_id, revision)
        self.assertEqual(task, task_response)
