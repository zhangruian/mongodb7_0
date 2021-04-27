function activate_venv {
  set -e
  # check if virtualenv is set up
  if [ -d "${workdir}/venv" ]; then
    if [ "Windows_NT" = "$OS" ]; then
      # Need to quote the path on Windows to preserve the separator.
      . "${workdir}/venv/Scripts/activate" 2>/tmp/activate_error.log
    else
      . ${workdir}/venv/bin/activate 2>/tmp/activate_error.log
    fi
    if [ $? -ne 0 ]; then
      echo "Failed to activate virtualenv: $(cat /tmp/activate_error.log)"
    fi
    python=python
  else
    python=${python:-/opt/mongodbtoolchain/v3/bin/python3}
  fi

  if [ "Windows_NT" = "$OS" ]; then
    export PYTHONPATH="$PYTHONPATH;$(cygpath -w ${workdir}/src)"
  else
    export PYTHONPATH="$PYTHONPATH:${workdir}/src"
  fi

  echo "python set to $(which $python)"
  set +e
}
