"""A service to proxy requests to resmoke."""
from copy import deepcopy
from typing import List, Dict, Any, NamedTuple, TYPE_CHECKING, Set

import inject
import structlog
import yaml

import buildscripts.resmokelib.parser as _parser
import buildscripts.resmokelib.suitesconfig as _suiteconfig
from buildscripts.task_generation.generated_config import GeneratedFile
from buildscripts.task_generation.task_types.models.resmoke_task_model import ResmokeTask

if TYPE_CHECKING:
    from buildscripts.task_generation.suite_split import GeneratedSuite, SubSuite

LOGGER = structlog.get_logger(__name__)

HEADER_TEMPLATE = """# DO NOT EDIT THIS FILE. All manual edits will be lost.
# This file was generated by {file} from
# {suite_file}.
"""


class ResmokeProxyService:
    """A service to proxy requests to resmoke."""

    @inject.autoparams()
    def __init__(self, run_options="") -> None:
        """Initialize the service."""
        _parser.set_run_options(run_options)
        self._suite_config = _suiteconfig

    def list_tests(self, suite_name: str) -> List[str]:
        """
        List the test files that are part of the suite being split.

        :param suite_name: Name of suite to query.
        :return: List of test names that belong to the suite.
        """
        suite = self._suite_config.get_suite(suite_name)
        test_list = []
        for tests in suite.tests:
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
        return self._suite_config.SuiteFinder.get_config_obj(suite_name)

    def render_suite_files(self, tasks: List[ResmokeTask]) -> List[GeneratedFile]:
        """
        Render the given list of suites.

        This will create a dictionary of all the resmoke config files to create with the
        filename of each file as the key and the contents as the value.

        :param tasks: resmoke tasks to generate config files for.
        :return: Dictionary of rendered resmoke config files.
        """
        suite_configs = []
        # pylint: disable=too-many-arguments
        for resmoke_task in tasks:
            source_config = self._suite_config.SuiteFinder.get_config_obj(
                resmoke_task.resmoke_suite_name)
            suite_configs.append(
                GeneratedFile(
                    file_name=resmoke_task.execution_task_suite_yaml_name,
                    content=generate_resmoke_suite_config(
                        source_config, resmoke_task.resmoke_suite_name,
                        roots=resmoke_task.test_list, excludes=resmoke_task.excludes)))

        LOGGER.debug("Generated files", files=[f.file_name for f in suite_configs])

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
