#!/bin/sh
set -eu
build_dir=${ICECC_TEST_BUILDDIR:?}
top_build_dir=${ICECC_TEST_TOP_BUILDDIR:?}
exec "$build_dir/daemoncachedispatch" "$top_build_dir/daemon/iceccd"
