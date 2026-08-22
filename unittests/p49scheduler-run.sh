#!/bin/sh
set -eu

build_dir=${ICECC_TEST_BUILDDIR:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)}
top_build_dir=${ICECC_TEST_TOP_BUILDDIR:-$build_dir/..}

exec "$build_dir/p49scheduler" "$top_build_dir/scheduler/icecc-scheduler"
