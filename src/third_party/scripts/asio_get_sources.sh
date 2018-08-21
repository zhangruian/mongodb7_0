#!/bin/bash
# This script downloads and imports an unstable revision of ASIO.
# It can be run on Linux or Mac OS X.
# Actual integration into the build system is not done by this script.
#
# Turn on strict error checking, like perl use 'strict'
set -xeuo pipefail
IFS=$'\n\t'

if [ "$#" -ne 0 ]; then
    echo "This script does not take any arguments"
    exit 1
fi

NAME=asio
REVISION=b0926b61b057ce563241d609cae5768ed3a4e1b1
SRC_ROOT=$(mktemp -d /tmp/asio.XXXXXX)
#trap "rm -rf $SRC_ROOT" EXIT
SRC=${SRC_ROOT}/${NAME}_${REVISION}
DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/$NAME-master
PATCH_DIR=$(git rev-parse --show-toplevel)/src/third_party/$NAME-master/patches

if [ ! -d $SRC ]; then
    git clone git@github.com:chriskohlhoff/asio.git $SRC

    pushd $SRC
    git checkout $REVISION
    git am $PATCH_DIR/*.patch

    # Trim files
    rm -r asio/src/examples
    rm -r asio/src/doc
    rm -r asio/src/tests
    
    popd
fi

test -d $DEST_DIR/asio && rm -r $DEST_DIR/asio
mkdir -p $DEST_DIR
mv $SRC/asio $DEST_DIR
