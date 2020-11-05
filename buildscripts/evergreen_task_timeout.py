#!/usr/bin/env python3
"""Determine the timeout value a task should use in evergreen."""

import argparse
import sys

import yaml

COMMIT_QUEUE_ALIAS = "__commit_queue"

COMMIT_QUEUE_TIMEOUT_SECS = 40 * 60
DEFAULT_REQUIRED_BUILD_TIMEOUT_SECS = 30 * 60
DEFAULT_NON_REQUIRED_BUILD_TIMEOUT_SECS = 2 * 60 * 60
# 2x the longest "run tests" phase for unittests as of c9bf1dbc9cc46e497b2f12b2d6685ef7348b0726,
# which is 5 mins 47 secs, excluding outliers below
UNITTESTS_TIMEOUT_SECS = 12 * 60

SPECIFIC_TASK_OVERRIDES = {
    "linux-64-debug": {"auth": 60 * 60, },

    # unittests outliers
    # repeated execution runs a suite 10 times
    "linux-64-repeated-execution": {"unittests": 10 * UNITTESTS_TIMEOUT_SECS},
    # some of the a/ub/t san variants need a little extra time
    "enterprise-ubuntu2004-debug-tsan": {"unittests": 2 * UNITTESTS_TIMEOUT_SECS},
    "ubuntu1804-asan": {"unittests": 2 * UNITTESTS_TIMEOUT_SECS},
    "ubuntu1804-ubsan": {"unittests": 2 * UNITTESTS_TIMEOUT_SECS},
    "ubuntu1804-debug-asan": {"unittests": 2 * UNITTESTS_TIMEOUT_SECS},
    "ubuntu1804-debug-aubsan-lite": {"unittests": 2 * UNITTESTS_TIMEOUT_SECS},
    "ubuntu1804-debug-ubsan": {"unittests": 2 * UNITTESTS_TIMEOUT_SECS},
}

REQUIRED_BUILD_VARIANTS = {
    "linux-64-debug", "enterprise-windows-64-2k8", "enterprise-rhel-62-64-bit",
    "enterprise-ubuntu1604-clang-3.8-libcxx", "enterprise-rhel-62-64-bit-required-inmem",
    "ubuntu1604-debug-aubsan-lite"
}


def _has_override(variant: str, task_name: str) -> bool:
    return variant in SPECIFIC_TASK_OVERRIDES and task_name in SPECIFIC_TASK_OVERRIDES[variant]


def determine_timeout(task_name: str, variant: str, timeout: int = 0, evg_alias: str = '') -> int:
    """Determine what timeout should be used."""

    if timeout and timeout != 0:
        return timeout

    if task_name == "unittests" and not _has_override(variant, task_name):
        return UNITTESTS_TIMEOUT_SECS

    if evg_alias == COMMIT_QUEUE_ALIAS:
        return COMMIT_QUEUE_TIMEOUT_SECS

    if _has_override(variant, task_name):
        return SPECIFIC_TASK_OVERRIDES[variant][task_name]

    if variant in REQUIRED_BUILD_VARIANTS:
        return DEFAULT_REQUIRED_BUILD_TIMEOUT_SECS
    return DEFAULT_NON_REQUIRED_BUILD_TIMEOUT_SECS


def output_timeout(timeout, options):
    """Output timeout configuration to the specified location."""
    output = {
        "timeout_secs": timeout,
    }

    if options.outfile:
        with open(options.outfile, "w") as outfile:
            yaml.dump(output, stream=outfile, default_flow_style=False)

    yaml.dump(output, stream=sys.stdout, default_flow_style=False)


def main():
    """Determine the timeout value a task should use in evergreen."""
    parser = argparse.ArgumentParser(description=main.__doc__)

    parser.add_argument("--task-name", dest="task", required=True, help="Task being executed.")
    parser.add_argument("--build-variant", dest="variant", required=True,
                        help="Build variant task is being executed on.")
    parser.add_argument("--evg-alias", dest="evg_alias", required=True,
                        help="Evergreen alias used to trigger build.")
    parser.add_argument("--timeout", dest="timeout", type=int, help="Timeout to use.")
    parser.add_argument("--out-file", dest="outfile", help="File to write configuration to.")

    options = parser.parse_args()

    timeout = determine_timeout(options.task, options.variant, options.timeout)
    output_timeout(timeout, options)


if __name__ == "__main__":
    main()
