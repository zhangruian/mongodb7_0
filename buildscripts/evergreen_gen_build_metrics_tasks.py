#!/usr/bin/env python3
import os
import sys
import platform

from shrub.v2 import ShrubProject, Task, BuildVariant, FunctionCall, TaskGroup
from shrub.v2.command import BuiltInCommand


def main():

    tasks = {
        'windows_tasks': [],
        'linux_x86_64_tasks': [],
        'linux_arm64_tasks': [],
        'macos_tasks': [],
    }

    def create_build_metric_task_steps(task_build_flags, task_targets):

        evg_flags = f"--debug=time,count,memory VARIANT_DIR=metrics BUILD_METRICS_EVG_TASK_ID={os.environ['task_id']} BUILD_METRICS_EVG_BUILD_VARIANT={os.environ['build_variant']}"
        cache_flags = "--cache-dir=$PWD/scons-cache --cache-signature-mode=validate"

        scons_task_steps = [
            f"{evg_flags} --build-metrics=build_metrics.json",
            f"{evg_flags} {cache_flags} --cache-populate --build-metrics=populate_cache.json",
            f"{evg_flags} --clean",
            f"{evg_flags} {cache_flags} --build-metrics=pull_cache.json",
        ]

        task_steps = [
            FunctionCall(
                "scons compile", {
                    "task_compile_flags": f"{task_build_flags} {step_flags}",
                    "targets": task_targets,
                    "compiling_for_test": "true",
                }) for step_flags in scons_task_steps
        ]
        task_steps.append(FunctionCall("attach build metrics"))
        task_steps.append(FunctionCall("print top N metrics"))
        return task_steps

    #############################
    if sys.platform == 'win32':
        targets = "install-all-meta-but-not-unittests"
        build_flags = '--cache=nolinked'

        tasks['windows_tasks'].append(
            Task("build_metrics_msvc", create_build_metric_task_steps(build_flags, targets)))

    ##############################
    elif sys.platform == 'darwin':

        for link_model in ['dynamic', 'static']:
            if link_model == 'dynamic':
                targets = "install-all-meta generate-libdeps-graph"
            else:
                targets = "install-all-meta-but-not-unittests"

            build_flags = f"--link-model={link_model} --force-macos-dynamic-link" + (
                ' --cache=nolinked' if link_model == 'static' else "")

            tasks['macos_tasks'].append(
                Task(f"build_metrics_xcode_{link_model}",
                     create_build_metric_task_steps(build_flags, targets)))

    ##############################
    else:
        for toolchain in ['v4']:
            # possibly we want to add clang to the mix here, so leaving as an easy drop in
            for compiler in ['gcc']:
                for link_model in ['dynamic', 'static']:

                    if link_model == 'dynamic':
                        targets = "install-all-meta generate-libdeps-graph"
                    else:
                        targets = "install-all-meta-but-not-unittests"

                    build_flags = f"BUILD_METRICS_BLOATY=/opt/mongodbtoolchain/v4/bin/bloaty --variables-files=etc/scons/mongodbtoolchain_{toolchain}_{compiler}.vars --link-model={link_model}" + (
                        ' --cache=nolinked' if link_model == 'static' else "")

                    tasks['linux_x86_64_tasks'].append(
                        Task(f"build_metrics_x86_64_{toolchain}_{compiler}_{link_model}",
                             create_build_metric_task_steps(build_flags, targets)))
                    tasks['linux_arm64_tasks'].append(
                        Task(f"build_metrics_arm64_{toolchain}_{compiler}_{link_model}",
                             create_build_metric_task_steps(build_flags, targets)))

    def create_task_group(target_platform, tasks):
        task_group = TaskGroup(
            name=f'build_metrics_{target_platform}_task_group_gen',
            tasks=tasks,
            max_hosts=len(tasks),
            setup_group=[
                BuiltInCommand("manifest.load", {}),
                FunctionCall("git get project and add git tag"),
                FunctionCall("set task expansion macros"),
                FunctionCall("f_expansions_write"),
                FunctionCall("kill processes"),
                FunctionCall("cleanup environment"),
                FunctionCall("set up venv"),
                FunctionCall("upload pip requirements"),
                FunctionCall("get all modified patch files"),
                FunctionCall("f_expansions_write"),
                FunctionCall("configure evergreen api credentials"),
                FunctionCall("get buildnumber"),
                FunctionCall("f_expansions_write"),
                FunctionCall("generate compile expansions"),
                FunctionCall("f_expansions_write"),
            ],
            setup_task=[
                FunctionCall("f_expansions_write"),
                FunctionCall("apply compile expansions"),
                FunctionCall("set task expansion macros"),
                FunctionCall("f_expansions_write"),
            ],
            teardown_group=[
                FunctionCall("f_expansions_write"),
                FunctionCall("cleanup environment"),
            ],
            teardown_task=[
                FunctionCall("f_expansions_write"),
                FunctionCall("attach scons logs"),
                FunctionCall("kill processes"),
                FunctionCall("save disk statistics"),
                FunctionCall("save system resource information"),
                FunctionCall("remove files",
                             {'files': ' '.join(['src/build', 'src/scons-cache', '*.tgz'])}),
            ],
            setup_group_can_fail_task=True,
        )
        return task_group

    if sys.platform == 'win32':
        variant = BuildVariant(
            name="enterprise-windows-build-metrics",
            activate=True,
        )
        variant.add_task_group(
            create_task_group('windows', tasks['windows_tasks']), ['windows-vsCurrent-large'])
    elif sys.platform == 'darwin':
        variant = BuildVariant(
            name="macos-enterprise-build-metrics",
            activate=True,
        )
        variant.add_task_group(create_task_group('macos', tasks['macos_tasks']), ['macos-1100'])
    else:
        if platform.machine() == 'x86_64':
            variant = BuildVariant(
                name="enterprise-rhel-80-64-bit-build-metrics",
                activate=True,
            )
            variant.add_task_group(
                create_task_group('linux_X86_64', tasks['linux_x86_64_tasks']), ['rhel80-xlarge'])
        else:
            variant = BuildVariant(
                name="enterprise-rhel-80-aarch64-build-metrics",
                activate=True,
            )
            variant.add_task_group(
                create_task_group('linux_arm64', tasks['linux_arm64_tasks']),
                ['amazon2022-arm64-large'])

    project = ShrubProject({variant})
    with open('build_metrics_task_gen.json', 'w') as fout:
        fout.write(project.json())


if __name__ == "__main__":
    main()
