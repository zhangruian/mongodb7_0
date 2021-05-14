#!/usr/bin/env python3
"""Generate multiple powercycle tasks to run in evergreen."""
from collections import namedtuple
from typing import Any, List, Tuple, Set

import click
from shrub.v2 import BuildVariant, FunctionCall, ShrubProject, Task, TaskDependency
from shrub.v2.command import BuiltInCommand

from buildscripts.util.fileops import write_file
from buildscripts.util.read_config import read_config_file
from buildscripts.util.taskname import name_generated_task

Config = namedtuple("config", [
    "task_names",
    "num_tasks",
    "timeout_params",
    "remote_credentials_vars",
    "set_up_ec2_instance_vars",
    "run_powercycle_vars",
    "build_variant",
    "distro",
])


def make_config(expansions_file: Any) -> Config:
    """Group expansions into config."""
    expansions = read_config_file(expansions_file)
    task_names = expansions.get("task_names", "powercycle_smoke_skip_compile")
    # Avoid duplicated task names
    task_names = {task_name for task_name in task_names.split(" ")}
    num_tasks = int(expansions.get("num_tasks", 10))
    timeout_params = {
        "exec_timeout_secs": int(expansions.get("exec_timeout_secs", 7200)),
        "timeout_secs": int(expansions.get("timeout_secs", 1800)),
    }
    remote_credentials_vars = {
        "private_key_file": "src/powercycle.pem",
        "private_key_remote": "${__project_aws_ssh_key_value}",
    }
    set_up_ec2_instance_vars = {
        "set_up_retry_count": int(expansions.get("set_up_retry_count", 2)),
    }
    run_powercycle_vars = {
        "run_powercycle_args": expansions.get("run_powercycle_args"),
    }
    build_variant = expansions.get("build_variant")
    distro = expansions.get("distro_id")

    return Config(task_names, num_tasks, timeout_params, remote_credentials_vars,
                  set_up_ec2_instance_vars, run_powercycle_vars, build_variant, distro)


def get_setup_commands() -> Tuple[List[FunctionCall], Set[TaskDependency]]:
    """Return setup commands."""
    return [
        FunctionCall("do setup"),
    ], {TaskDependency("archive_dist_test_debug")}


def get_skip_compile_setup_commands() -> Tuple[List[FunctionCall], set]:
    """Return skip compile setup commands."""
    return [
        FunctionCall("set task expansion macros"),
        FunctionCall("set up venv"),
        FunctionCall("upload pip requirements"),
        FunctionCall("f_expansions_write"),
        FunctionCall("configure evergreen api credentials"),
        FunctionCall("get compiled binaries"),
    ], set()


@click.command()
@click.argument("expansions_file", type=str, default="expansions.yml")
@click.argument("output_file", type=str, default="powercycle_tasks.json")
def main(expansions_file: str = "expansions.yml",
         output_file: str = "powercycle_tasks.json") -> None:
    """Generate multiple powercycle tasks to run in evergreen."""

    config = make_config(expansions_file)
    build_variant = BuildVariant(config.build_variant)
    for task_name in config.task_names:
        if "skip_compile" in task_name:
            commands, task_dependency = get_skip_compile_setup_commands()
        else:
            commands, task_dependency = get_setup_commands()

        commands.extend([
            FunctionCall("set up remote credentials", config.remote_credentials_vars),
            BuiltInCommand("timeout.update", config.timeout_params),
            FunctionCall("set up EC2 instance", config.set_up_ec2_instance_vars),
            FunctionCall("run powercycle test", config.run_powercycle_vars),
        ])

        build_variant.display_task(
            task_name, {
                Task(
                    name_generated_task(task_name, index, config.num_tasks, config.build_variant),
                    commands, task_dependency)
                for index in range(config.num_tasks)
            }, distros=[config.distro])

    shrub_project = ShrubProject.empty()
    shrub_project.add_build_variant(build_variant)

    write_file(output_file, shrub_project.json())


if __name__ == '__main__':
    main()
