"""A service to proxy requests to resmoke."""
import os
from copy import deepcopy
from typing import List, Dict, Any, NamedTuple

import inject
import yaml

import buildscripts.resmokelib.parser as _parser
import buildscripts.resmokelib.suitesconfig as suitesconfig
from buildscripts.task_generation.generated_config import GeneratedFile
from buildscripts.util.fileops import read_yaml_file

HEADER_TEMPLATE = """# DO NOT EDIT THIS FILE. All manual edits will be lost.
# This file was generated by {file} from
# {suite_file}.
"""


class ResmokeProxyConfig(NamedTuple):
    """
    Configuration for resmoke proxy.

    resmoke_suite_dir: Directory that contains resmoke suite configurations.
    """

    resmoke_suite_dir: str


class ResmokeProxyService:
    """A service to proxy requests to resmoke."""

    @inject.autoparams()
    def __init__(self, proxy_config: ResmokeProxyConfig) -> None:
        """
        Initialize the service.

        :param proxy_config: Configuration for the proxy.
        """
        _parser.set_run_options()
        self.suitesconfig = suitesconfig
        self.resmoke_suite_dir = proxy_config.resmoke_suite_dir

    def list_tests(self, suite_name: str) -> List[str]:
        """
        List the test files that are part of the suite being split.

        :param suite_name: Name of suite to query.
        :return: List of test names that belong to the suite.
        """
        suite_config = self.suitesconfig.get_suite(suite_name)
        test_list = []
        for tests in suite_config.tests:
            # `tests` could return individual tests or lists of tests, we need to handle both.
            if isinstance(tests, list):
                test_list.extend(tests)
            else:
                test_list.append(tests)

        return test_list

    def read_suite_config(self, suite_name: str) -> Dict[str, Any]:
        """
        Read the given resmoke suite configuration.

        :param suite_name: Name of suite to read.
        :return: Configuration of specified suite.
        """
        return read_yaml_file(os.path.join(self.resmoke_suite_dir, f"{suite_name}.yml"))

    def render_suite_files(self, suites: List, suite_name: str, generated_suite_filename: str,
                           test_list: List[str], create_misc_suite: bool,
                           build_variant: str) -> List[GeneratedFile]:
        """
        Render the given list of suites.

        This will create a dictionary of all the resmoke config files to create with the
        filename of each file as the key and the contents as the value.

        :param suites: List of suites to render.
        :param suite_name: Base name of suites.
        :param generated_suite_filename: The name to use as the file name for generated suite file.
        :param test_list: List of tests used in suites.
        :param create_misc_suite: Whether or not a _misc suite file should be created.
        :param build_variant: Build variant suite file is being rendered for.
        :return: Dictionary of rendered resmoke config files.
        """
        # pylint: disable=too-many-arguments
        source_config = self.read_suite_config(suite_name)
        suite_configs = [
            GeneratedFile(
                file_name=f"{os.path.basename(suite.name(len(suites)))}_{build_variant}.yml",
                content=suite.generate_resmoke_config(source_config)) for suite in suites
        ]
        if create_misc_suite:
            suite_configs.append(
                GeneratedFile(
                    file_name=f"{generated_suite_filename}_misc_{build_variant}.yml",
                    content=generate_resmoke_suite_config(source_config, generated_suite_filename,
                                                          excludes=test_list)))
        return suite_configs


def update_suite_config(suite_config, roots=None, excludes=None):
    """
    Update suite config based on the roots and excludes passed in.

    :param suite_config: suite_config to update.
    :param roots: new roots to run, or None if roots should not be updated.
    :param excludes: excludes to add, or None if excludes should not be include.
    :return: updated suite_config
    """
    if roots:
        suite_config["selector"]["roots"] = roots

    if excludes:
        # This must be a misc file, if the exclude_files section exists, extend it, otherwise,
        # create it.
        if "exclude_files" in suite_config["selector"] and \
                suite_config["selector"]["exclude_files"]:
            suite_config["selector"]["exclude_files"] += excludes
        else:
            suite_config["selector"]["exclude_files"] = excludes
    else:
        # if excludes was not specified this must not a misc file, so don"t exclude anything.
        if "exclude_files" in suite_config["selector"]:
            del suite_config["selector"]["exclude_files"]

    return suite_config


def generate_resmoke_suite_config(source_config, source_file, roots=None, excludes=None):
    """
    Read and evaluate the yaml suite file.

    Override selector.roots and selector.excludes with the provided values. Write the results to
    target_suite_name.

    :param source_config: Config of suite to base generated config on.
    :param source_file: Filename of source suite.
    :param roots: Roots used to select tests for split suite.
    :param excludes: Tests that should be excluded from split suite.
    """
    suite_config = update_suite_config(deepcopy(source_config), roots, excludes)

    contents = HEADER_TEMPLATE.format(file=__file__, suite_file=source_file)
    contents += yaml.safe_dump(suite_config, default_flow_style=False)
    return contents
