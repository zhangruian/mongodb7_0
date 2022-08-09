# Copyright 2020 MongoDB Inc.
#
# Permission is hereby granted, free of charge, to any person obtaining
# a copy of this software and associated documentation files (the
# "Software"), to deal in the Software without restriction, including
# without limitation the rights to use, copy, modify, merge, publish,
# distribute, sublicense, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so, subject to
# the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
# KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
# WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
# LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
# OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
# WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#
"""Configure the build to track build performance."""

import atexit
import json
import os
import sys
import time
from timeit import default_timer as timer

from jsonschema import validate
import psutil

from .util import add_meta_data, get_build_metric_dict, CaptureAtexits
from .memory import MemoryMonitor
from .per_action_metrics import PerActionMetrics
from .artifacts import CollectArtifacts
from .scons import SConsStats
from .cache_dir import CacheDirCollector, CacheDirValidateWithMetrics
from .libdeps import LibdepsCollector

_SEC_TO_NANOSEC_FACTOR = 1000000000.0
_METRICS_COLLECTORS = []


def finalize_build_metrics(env):
    metrics = get_build_metric_dict()
    metrics['end_time'] = time.time_ns()
    for m in _METRICS_COLLECTORS:
        start_time = timer()
        sys.stdout.write(f"Processing {m.get_name()}...")
        sys.stdout.flush()
        key, value = m.finalize()
        sys.stdout.write(f" {timer() - start_time}s\n")
        metrics[key] = value

    with open(os.path.join(os.path.dirname(__file__), "build_metrics_format.schema")) as f:
        validate(metrics, json.load(f))

    build_metrics_file = env.GetOption('build-metrics')
    if build_metrics_file == '-':
        json.dump(metrics, sys.stdout, indent=4, sort_keys=True)
    else:
        with open(build_metrics_file, 'w') as f:
            json.dump(metrics, f, indent=4, sort_keys=True)


def generate(env, **kwargs):
    global _METRICS_COLLECTORS

    # This will force our at exit to the of the stack ensuring
    # that it is the last thing called when exiting.
    c = CaptureAtexits()
    atexit.unregister(c)
    for func in c.captured:
        atexit.unregister(func)
    atexit.register(finalize_build_metrics, env)
    for func in c.captured:
        atexit.register(func)

    env.AddMethod(get_build_metric_dict, "GetBuildMetricDictionary")
    env.AddMethod(add_meta_data, "AddBuildMetricsMetaData")

    metrics = get_build_metric_dict()
    p = psutil.Process(os.getpid())

    metrics['start_time'] = int(p.create_time() * _SEC_TO_NANOSEC_FACTOR)
    metrics['scons_command'] = " ".join([sys.executable] + sys.argv)

    _METRICS_COLLECTORS = [
        MemoryMonitor(psutil.Process().memory_info().vms),
        PerActionMetrics(),
        CollectArtifacts(env),
        SConsStats(),
        CacheDirCollector(),
        LibdepsCollector(env)
    ]

    env['CACHEDIR_CLASS'] = CacheDirValidateWithMetrics


def exists(env):
    return True


def options(opts):
    """
    Add command line Variables for build metrics tool.
    """
    opts.AddVariables(
        ("BUILD_METRICS_ARTIFACTS_DIR", "Path to scan for artifacts after the build has stopped."),
        ("BUILD_METRICS_BLOATY", "Path to the bloaty bin"),
    )
