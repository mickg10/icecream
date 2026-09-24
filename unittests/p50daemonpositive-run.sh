#!/bin/sh
set -eu
build_dir=${ICECC_TEST_BUILDDIR:?}
top_build_dir=${ICECC_TEST_TOP_BUILDDIR:?}
"$build_dir/p50daemonpositive" \
    "$top_build_dir/daemon/iceccd" \
    "$top_build_dir/cache/icecc-cache-service"
ICECC_TEST_PENDING_DISCONNECT=1 "$build_dir/p50daemonpositive" \
    "$top_build_dir/daemon/iceccd" \
    "$top_build_dir/cache/icecc-cache-service"
ICECC_P51_MODE=on ICECC_TEST_P51_CANCEL_REPLACEMENT=1 \
    "$build_dir/p50daemonpositive" \
    "$top_build_dir/daemon/iceccd" \
    "$top_build_dir/cache/icecc-cache-service"
ICECC_P51_MODE=on ICECC_TEST_P51_VERTICAL=1 \
    "$build_dir/p50daemonpositive" \
    "$top_build_dir/daemon/iceccd" \
    "$top_build_dir/cache/icecc-cache-service"
