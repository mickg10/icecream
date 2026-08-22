#!/bin/sh
# Connectivity regression under the failure-injection preload.
src_dir=$(dirname "$0")
build_dir=${ICECC_TEST_BUILDDIR:-$src_dir}
LD_PRELOAD="$build_dir/conn_shim.so" "$build_dir/connectivity"
