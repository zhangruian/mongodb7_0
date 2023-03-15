#!/bin/bash
# This script downloads and imports grpc

set -euo pipefail
IFS=$'\n\t'

set -vx

NAME=grpc
REVISION="v1.46.6"
VERSION="1.46.6"

DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/grpc
if [[ -d $DEST_DIR/dist ]]; then
    echo "You must remove '$DEST_DIR/dist' before running $0" >&2
    exit 1
fi

git clone --branch $REVISION git@github.com:mongodb-forks/grpc.git $DEST_DIR/dist
pushd $DEST_DIR/dist
find . -mindepth 1 -maxdepth 1 -name ".*" -exec rm -rf {} \;
rm -rf bazel
rm -rf cmake
rm -rf doc
rm -rf summerofcode
rm -rf templates
rm -rf test
rm -rf tools
rm -rf examples/android
rm -rf examples/csharp
rm -rf examples/node
rm -rf examples/objective-c
rm -rf examples/php
rm -rf examples/python
rm -rf examples/ruby
find . -type f -name "*.BUILD" -exec rm -rf {} \;
find . -type f -name "*.podspec" -exec rm -rf {} \;
find . -type f -name "*.gemspec" -exec rm -rf {} \;
popd
