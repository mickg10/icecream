#!/bin/sh
set -eu

build_dir=${ICECC_TEST_BUILDDIR:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)}
top_build_dir=${ICECC_TEST_TOP_BUILDDIR:-$build_dir/..}

"$build_dir/p49scheduler" "$top_build_dir/scheduler/icecc-scheduler"
ICECC_TEST_PRE_EXPOSURE_LOSS_ONLY=1 ICECC_TEST_P49_OCCUPY_RESERVED_PORT=1 \
    exec "$build_dir/p49scheduler" "$top_build_dir/scheduler/icecc-scheduler"
