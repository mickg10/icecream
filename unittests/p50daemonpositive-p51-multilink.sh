#!/bin/sh
set -eu
unset ICECC_TEST_P51_RESTART_W30_TOPOLOGY

build_dir=${ICECC_TEST_BUILDDIR:?ICECC_TEST_BUILDDIR is required}
top_build_dir=${ICECC_TEST_TOP_BUILDDIR:?ICECC_TEST_TOP_BUILDDIR is required}

if [ "$(id -u)" -ne 0 ] || { [ ! -e /.dockerenv ] && [ ! -e /run/.containerenv ]; }; then
    echo "FAIL: P51 multi-link W30 requires an isolated root container" >&2
    exit 2
fi
if ! command -v iptables >/dev/null 2>&1 || ! iptables -w -L >/dev/null 2>&1; then
    echo "FAIL: P51 multi-link W30 requires usable iptables/NET_ADMIN" >&2
    exit 2
fi
if ! getent passwd icecc >/dev/null 2>&1; then
    echo "FAIL: P51 multi-link W30 requires the unprivileged icecc test identity" >&2
    exit 2
fi
if [ -z "${ICEFARM_TMPDIR:-}" ] || [ ! -d "$ICEFARM_TMPDIR" ] ||
   [ ! -w "$ICEFARM_TMPDIR" ]; then
    echo "FAIL: P51 multi-link W30 requires writable scratch-backed ICEFARM_TMPDIR" >&2
    exit 2
fi
if command -v runuser >/dev/null 2>&1 &&
   ! runuser -u icecc -- test -x "$ICEFARM_TMPDIR" -a -w "$ICEFARM_TMPDIR"; then
    echo "FAIL: ICEFARM_TMPDIR is not traversable/writable by the daemon identity" >&2
    exit 2
fi

for topology in C1F2 C1F3 C1F4 C2F1 C3F1 C4F1; do
    for profile in P29V1 ZSTD_TU ZSTD_ROUTE; do
        echo "P51_MULTILINK_START=$topology/$profile"
        unset ICECC_TEST_P51_VERTICAL ICECC_TEST_P51_VERTICAL_W30 \
            ICECC_TEST_P51_RESTART_F_C1F2 ICECC_TEST_P51_RESTART_C_C2F1 \
            ICECC_TEST_P51_CANCEL_REPLACEMENT ICECC_TEST_P51_MULTILINK \
            ICECC_TEST_PENDING_DISCONNECT ICECC_TEST_R1_PUBLISH_ONLY \
            ICECC_TEST_SCHEDULER_BACKPRESSURE \
            ICECC_TEST_SCHEDULER_BACKPRESSURE_EXPIRE \
            ICECC_TEST_SCHEDULER_RCVBUF ICECC_TEST_SCHEDULER_SNDBUF \
            ICECC_TEST_SCHEDULER_SENDBUF ICECC_TEST_SEND_EAGAIN_MARKER \
            ICECC_TEST_BACKPRESSURE_SCHED_PORT ICECC_TEST_SNDBUF_SHIM \
            ICECC_TEST_SOCKET ICECC_TEST_CACHE_SESSION_CLOSE \
            ICECC_TEST_P50_SOURCE_CANCEL_FAIL \
            ICECC_TEST_P51_SOURCE_RESERVATION_FAIL \
            ICECC_TEST_P51_EXPIRED_ARM_WIRE \
            ICECC_TEST_P51_PAUSE_AFTER_GOODBYE_REQUEST \
            ICECC_TEST_P50_SOURCE_BUDGET_MSEC \
            ICECC_TEST_P51_RESTART_CHAIN_F_C_W30
        if [ "${ICECC_TEST_P51_CAPTURE_DAEMON_STDERR:-}" = 1 ]; then
            ICECC_TEST_CAPTURE_DAEMON_STDERR=1
            export ICECC_TEST_CAPTURE_DAEMON_STDERR
        else
            unset ICECC_TEST_CAPTURE_DAEMON_STDERR
        fi
        ICECC_TEST_POSITIVE_DAEMON=1 \
        ICECC_P51_MODE=on \
        ICECC_TEST_P51_MULTILINK="$topology" \
        ICECC_TEST_P51_PROFILE="$profile" \
            timeout --signal=TERM --kill-after=5s 240s "$build_dir/p50daemonpositive" \
                "$top_build_dir/daemon/iceccd" \
                "$top_build_dir/cache/icecc-cache-service"
        echo "P51_MULTILINK_PASS=$topology/$profile"
    done
done
