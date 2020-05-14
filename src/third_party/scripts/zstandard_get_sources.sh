#!/bin/bash
# This script downloads and imports a revision of zstandard.
# It can be run on Linux, Mac OS X or Windows WSL.
# Actual integration into the build system is not done by this script.
#
# Turn on strict error checking, like perl use 'strict'
set -xeuo pipefail
IFS=$'\n\t'

if [ "$#" -ne 0 ]; then
    echo "This script does not take any arguments"
    exit 1
fi

GIT_EXE=git
if grep -q Microsoft /proc/version; then
    GIT_EXE=git.exe
fi

NAME=zstandard
REVISION=1.4.4
if grep -q Microsoft /proc/version; then
    SRC_ROOT=$(wslpath -u $(powershell.exe -Command "Get-ChildItem Env:TEMP | Get-Content | Write-Host"))
    SRC_ROOT+="$(mktemp -u /zstandard.XXXXXX)"
    mkdir -p $SRC_ROOT
else
    SRC_ROOT=$(mktemp -d /tmp/zstandard.XXXXXX)
fi

SRC=${SRC_ROOT}/${NAME}_${REVISION}
CLONE_DEST=$SRC
if grep -q Microsoft /proc/version; then
    CLONE_DEST=$(wslpath -m $SRC)
fi
DEST_DIR=$($GIT_EXE rev-parse --show-toplevel)/src/third_party/$NAME-$REVISION
PATCH_DIR=$($GIT_EXE rev-parse --show-toplevel)/src/third_party/$NAME-$REVISION/patches
if grep -q Microsoft /proc/version; then
    DEST_DIR=$(wslpath -u "$DEST_DIR")
    PATCH_DIR=$(wslpath -w $(wslpath -u "$PATCH_DIR"))
fi

echo "dest: $DEST_DIR"
echo "patch: $PATCH_DIR"

if [ ! -d $SRC ]; then
    $GIT_EXE clone https://github.com/facebook/zstd.git $CLONE_DEST

    pushd $SRC
    $GIT_EXE checkout v$REVISION
    
    popd
fi

test -d $DEST_DIR/zstd && rm -r $DEST_DIR/zstd
mkdir -p $DEST_DIR/zstd
mv $SRC/* $DEST_DIR/zstd

# Generate the SConscript
( cat > ${DEST_DIR}/SConscript ) << ___EOF___
# -*- mode: python; -*-
# NOTE: This file is auto-generated by "$(basename $0)" - DO NOT EDIT

Import("env")

env = env.Clone()

env.Prepend(CPPPATH=[
    'zstd/lib/common',
])

env.Library(
    target="zstd",
    source=[
        'zstd/lib/common/entropy_common.c',
        'zstd/lib/common/error_private.c',
        'zstd/lib/common/fse_decompress.c',
        'zstd/lib/common/pool.c',
        'zstd/lib/common/threading.c',
        'zstd/lib/common/xxhash.c',
        'zstd/lib/common/zstd_common.c',
        'zstd/lib/compress/fse_compress.c',
        'zstd/lib/compress/hist.c',
        'zstd/lib/compress/huf_compress.c',
        'zstd/lib/compress/zstd_compress.c',
        'zstd/lib/compress/zstd_compress_literals.c',
        'zstd/lib/compress/zstd_compress_sequences.c',
        'zstd/lib/compress/zstd_double_fast.c',
        'zstd/lib/compress/zstd_fast.c',
        'zstd/lib/compress/zstd_lazy.c',
        'zstd/lib/compress/zstd_ldm.c',
        'zstd/lib/compress/zstd_opt.c',
        'zstd/lib/compress/zstdmt_compress.c',
        'zstd/lib/decompress/huf_decompress.c',
        'zstd/lib/decompress/zstd_ddict.c',
        'zstd/lib/decompress/zstd_decompress.c',
        'zstd/lib/decompress/zstd_decompress_block.c',
        'zstd/lib/deprecated/zbuff_common.c',
        'zstd/lib/deprecated/zbuff_compress.c',
        'zstd/lib/deprecated/zbuff_decompress.c',
        'zstd/lib/dictBuilder/cover.c',
        'zstd/lib/dictBuilder/divsufsort.c',
        'zstd/lib/dictBuilder/fastcover.c',
        'zstd/lib/dictBuilder/zdict.c',
        
    ],
    LIBDEPS_TAGS=[
        'init-no-global-side-effects',
    ],
)
___EOF___

echo "Done"
