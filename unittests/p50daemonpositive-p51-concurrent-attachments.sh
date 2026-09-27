#!/bin/sh
set -eu
unset ICECC_TEST_P51_VERTICAL ICECC_TEST_P51_VERTICAL_W30 \
    ICECC_TEST_P51_MULTILINK ICECC_TEST_P51_RESTART_W30_TOPOLOGY \
    ICECC_TEST_P51_EXPIRED_ARM_WIRE ICECC_TEST_P51_CANCEL_BEFORE_START \
    ICECC_TEST_P51_PAUSE_AFTER_GOODBYE_REQUEST \
    ICECC_TEST_P50_SOURCE_BUDGET_MSEC ICECC_TEST_P51_RESTART_CHAIN_F_C_W30

build_dir=${ICECC_TEST_BUILDDIR:?ICECC_TEST_BUILDDIR is required}
top_build_dir=${ICECC_TEST_TOP_BUILDDIR:?ICECC_TEST_TOP_BUILDDIR is required}

if [ "$(id -u)" -ne 0 ]; then
    echo "FAIL: concurrent P51 attachment check requires an isolated root container" >&2
    exit 2
fi
if [ ! -e /.dockerenv ] && [ ! -e /run/.containerenv ]; then
    echo "FAIL: concurrent P51 attachment check must run in a disposable container" >&2
    exit 2
fi
if [ -z "${ICEFARM_TMPDIR:-}" ] || [ ! -d "$ICEFARM_TMPDIR" ] ||
   [ ! -w "$ICEFARM_TMPDIR" ]; then
    echo "FAIL: concurrent P51 attachment check requires writable ICEFARM_TMPDIR" >&2
    exit 2
fi

for profile in P29V1 ZSTD_TU ZSTD_ROUTE; do
    echo "P51_CONCURRENT_ATTACHMENTS_PROFILE_START=$profile"
    timeout 180s env ICECC_TEST_POSITIVE_DAEMON=1 \
    ICECC_P51_MODE=on \
    ICECC_TEST_P51_CONCURRENT_ATTACHMENTS=1 \
    ICECC_TEST_P51_PROFILE="$profile" \
        "$build_dir/p50daemonpositive" \
        "$top_build_dir/daemon/iceccd" \
        "$top_build_dir/cache/icecc-cache-service"
    echo "P51_CONCURRENT_ATTACHMENTS_PROFILE_PASS=$profile"
done
