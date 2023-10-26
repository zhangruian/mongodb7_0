import subprocess
import os
import sys


def run_command(command):  # noqa: D406
    """
    Execute a shell command and return its standard output (`stdout`).

    Args:
        command (str): The shell command to be executed.

    Returns:
        str: The standard output of the executed command.

    Raises:
        subprocess.CalledProcessError: If the command execution fails.

    """
    try:
        return subprocess.run(command, shell=True, check=True, text=True,
                              capture_output=True).stdout
    except subprocess.CalledProcessError as err:
        print(f"Error while executing: '{command}'.\n{err}\nStandard Error: {err.stderr}")
        raise


def create_mongodb_bot_gitconfig():
    """Create the mongodb-bot.gitconfig file with the desired content."""

    content = """
    [user]
        name = MongoDB Bot
        email = mongo-bot@mongodb.com
    """

    gitconfig_path = os.path.expanduser("~/mongodb-bot.gitconfig")

    with open(gitconfig_path, 'w') as file:
        file.write(content)

    print("mongodb-bot.gitconfig file created.")


def main():
    # Check if the copybara directory already exists
    if os.path.exists('copybara'):
        print("Copybara directory already exists.")
    else:
        run_command("git clone https://github.com/10gen/copybara.git")

    # Navigate to the Copybara directory and build the Copybara Docker image
    run_command("cd copybara && docker build --rm -t copybara .")

    # Create the mongodb-bot.gitconfig file as necessary.
    create_mongodb_bot_gitconfig()

    # Set up the Docker command and execute it
    current_dir = os.getcwd()

    docker_cmd = [
        "docker run", "-v ~/.ssh:/root/.ssh", "-v ~/mongodb-bot.gitconfig:/root/.gitconfig",
        f'-v "{current_dir}/copybara.staging.sky":/usr/src/app/copy.bara.sky',
        "-e COPYBARA_CONFIG='copy.bara.sky'", "-e COPYBARA_SUBCOMMAND='migrate'",
        "-e COPYBARA_OPTIONS='-v'", "copybara copybara"
    ]

    try:
        run_command(" ".join(docker_cmd))
    except subprocess.CalledProcessError as err:
        error_message = str(err.stderr)
        acceptable_error_messages = [
            # Indicates the two repositories are identical
            "No new changes to import for resolved ref",
            # Indicates differences exist but no changes affect the destination, for example: exclusion rules
            "Iterative workflow produced no changes in the destination for resolved ref",
        ]

        if any(acceptable_message in error_message
               for acceptable_message in acceptable_error_messages):
            return

        raise


if __name__ == "__main__":
    main()
