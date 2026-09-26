#!/bin/sh
set -eu

build_dir=${ICECC_TEST_BUILDDIR:?ICECC_TEST_BUILDDIR is required}
top_build_dir=${ICECC_TEST_TOP_BUILDDIR:?ICECC_TEST_TOP_BUILDDIR is required}
scratch=${ICEFARM_TMPDIR:-}

if [ "$(id -u)" -ne 0 ] || { [ ! -e /.dockerenv ] && [ ! -e /run/.containerenv ]; }; then
    echo "FAIL: P51 capacity W30 requires an isolated root container" >&2
    exit 2
fi
if ! command -v iptables >/dev/null 2>&1 || ! iptables -w -L >/dev/null 2>&1; then
    echo "FAIL: P51 capacity W30 requires usable iptables/NET_ADMIN" >&2
    exit 2
fi
if ! getent passwd icecc >/dev/null 2>&1; then
    echo "FAIL: P51 capacity W30 requires the unprivileged icecc test identity" >&2
    exit 2
fi
if [ -z "$scratch" ] || [ ! -d "$scratch" ] || [ ! -w "$scratch" ]; then
    echo "FAIL: P51 capacity W30 requires writable scratch-backed ICEFARM_TMPDIR" >&2
    exit 2
fi
if command -v runuser >/dev/null 2>&1 &&
   ! runuser -u icecc -- test -x "$scratch" -a -w "$scratch"; then
    echo "FAIL: ICEFARM_TMPDIR is not traversable/writable by the daemon identity" >&2
    exit 2
fi

profile=${ICECC_TEST_P51_CAPACITY_W30_PROFILE:-P29V1}
case "$profile" in
    P29V1|ZSTD_TU|ZSTD_ROUTE) ;;
    *) echo "FAIL: unsupported capacity W30 profile: $profile" >&2; exit 2 ;;
esac
cache_service=${ICECC_TEST_P51_CAPACITY_SERVICE:-$top_build_dir/cache/icecc-cache-service-test}
if [ ! -x "$cache_service" ]; then
    echo "FAIL: dedicated capacity test service is missing: $cache_service" >&2
    exit 2
fi
if ! command -v strings >/dev/null 2>&1 ||
   ! strings "$cache_service" | grep -q 'P51_CAPACITY_TEST_SETTLEMENT_HELD'; then
    echo "FAIL: capacity test service lacks settlement-hook marker" >&2
    exit 2
fi
unset ICECC_TEST_P51_MULTILINK ICECC_TEST_P51_MULTILINK_CAPACITY_OVERLAP \
    ICECC_TEST_P51_VERTICAL ICECC_TEST_P51_VERTICAL_W30 \
    ICECC_TEST_P51_RESTART_F_C1F2 ICECC_TEST_P51_RESTART_C_C2F1 \
    ICECC_TEST_P51_RESTART_CHAIN_F_C_W30 ICECC_TEST_P51_CANCEL_BEFORE_START
export ICECC_TEST_CAPTURE_DAEMON_STDERR=1

echo "P51_CAPACITY_W30_START topology=C1F4 profile=$profile cap=120"
ICECC_TEST_POSITIVE_DAEMON=1 \
ICECC_P51_MODE=on \
ICECC_TEST_P51_MULTILINK=C1F4 \
ICECC_TEST_P51_MULTILINK_CAPACITY_OVERLAP=1 \
ICECC_TEST_P51_PROFILE="$profile" \
    timeout --signal=TERM --kill-after=5s 90s "$build_dir/p50daemonpositive" \
        "$top_build_dir/daemon/iceccd" \
        "$cache_service"
echo "P51_CAPACITY_W30_PASS topology=C1F4 profile=$profile"
