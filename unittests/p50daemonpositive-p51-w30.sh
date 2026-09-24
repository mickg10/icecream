#!/bin/sh
set -eu

build_dir=${ICECC_TEST_BUILDDIR:?ICECC_TEST_BUILDDIR is required}
top_build_dir=${ICECC_TEST_TOP_BUILDDIR:?ICECC_TEST_TOP_BUILDDIR is required}

if [ "$(id -u)" -ne 0 ]; then
    echo "FAIL: P51 W30 requires an isolated root container with NET_ADMIN" >&2
    exit 2
fi
if [ ! -e /.dockerenv ] && [ ! -e /run/.containerenv ]; then
    echo "FAIL: P51 W30 must run inside a disposable container namespace" >&2
    exit 2
fi
if ! command -v iptables >/dev/null 2>&1; then
    echo "FAIL: P51 W30 requires iptables in the isolated test container" >&2
    exit 2
fi
if ! iptables -w -L >/dev/null 2>&1; then
    echo "FAIL: P51 W30 requires usable NET_ADMIN in the isolated container" >&2
    exit 2
fi
if ! getent passwd icecc >/dev/null 2>&1; then
    echo "FAIL: P51 W30 requires the unprivileged icecc test identity" >&2
    exit 2
fi
if [ -z "${ICEFARM_TMPDIR:-}" ] || [ ! -d "$ICEFARM_TMPDIR" ] ||
   [ ! -w "$ICEFARM_TMPDIR" ]; then
    echo "FAIL: P51 W30 requires a writable scratch-backed ICEFARM_TMPDIR" >&2
    exit 2
fi

for profile in P29V1 ZSTD_TU ZSTD_ROUTE; do
    echo "P51_W30_PROFILE_START=$profile"
    unset ICECC_TEST_P51_VERTICAL
    ICECC_TEST_POSITIVE_DAEMON=1 \
    ICECC_P51_MODE=on \
    ICECC_TEST_P51_VERTICAL_W30=1 \
    ICECC_TEST_P51_PROFILE="$profile" \
        "$build_dir/p50daemonpositive" \
        "$top_build_dir/daemon/iceccd" \
        "$top_build_dir/cache/icecc-cache-service"
    echo "P51_W30_PROFILE_PASS=$profile"
done
