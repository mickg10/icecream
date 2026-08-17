#!/usr/bin/env bash
set -euo pipefail

: "${SOURCE_ROOT:?}"
: "${BUILD_ROOT:?}"
: "${LOOSE_ROOT:?}"
: "${CELL_ROOT:?}"

cmake -S "$SOURCE_ROOT" -B "$BUILD_ROOT" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DFMT_DOC=OFF \
    -DFMT_INSTALL=OFF \
    -DFMT_TEST=ON \
    -DFMT_FUZZ=OFF \
    -DFMT_CUDA_TEST=OFF \
    -DFMT_MODULE=OFF

II_COMPILE_COMMANDS="$BUILD_ROOT/compile_commands.json" \
    bash /harness/adapters/from-compile-commands.sh
