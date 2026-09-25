#!/bin/sh
set -eu

build_dir=${ICECC_TEST_BUILDDIR:?ICECC_TEST_BUILDDIR is required}
top_build_dir=${ICECC_TEST_TOP_BUILDDIR:?ICECC_TEST_TOP_BUILDDIR is required}

if [ "$(id -u)" -ne 0 ] || { [ ! -e /.dockerenv ] && [ ! -e /run/.containerenv ]; }; then
    echo "FAIL: P51 process-restart gate requires an isolated root container" >&2
    exit 2
fi
if ! command -v iptables >/dev/null 2>&1 || ! iptables -w -L >/dev/null 2>&1; then
    echo "FAIL: P51 process-restart gate requires usable NET_ADMIN" >&2
    exit 2
fi
if ! getent passwd icecc >/dev/null 2>&1; then
    echo "FAIL: isolated image has no unprivileged icecc identity" >&2
    exit 2
fi
if [ -z "${ICEFARM_TMPDIR:-}" ] || [ ! -d "$ICEFARM_TMPDIR" ] ||
   [ ! -w "$ICEFARM_TMPDIR" ]; then
    echo "FAIL: P51 process-restart gate needs writable ICEFARM_TMPDIR" >&2
    exit 2
fi

# The C++ fixture creates Unix-domain sockets below TMPDIR; keep the path short
# enough for sun_path while still placing artifacts on the selected scratch.
short_tmp="$ICEFARM_TMPDIR/p51r"
mkdir -p "$short_tmp"
chmod 1777 "$short_tmp"
export TMPDIR="$short_tmp"

unset ICECC_TEST_P51_RESTART_F_C1F2 ICECC_TEST_P51_RESTART_C_C2F1 \
    ICECC_TEST_P51_RESTART_W30_F_C1F2 ICECC_TEST_P51_RESTART_W30_C_C2F1 \
    ICECC_TEST_P51_MULTILINK \
    ICECC_TEST_P51_VERTICAL_W30 ICECC_TEST_P51_VERTICAL \
    ICECC_TEST_P51_CANCEL_REPLACEMENT ICECC_TEST_P51_PROFILE \
    ICECC_TEST_SCHEDULER_BACKPRESSURE ICECC_TEST_SCHEDULER_BACKPRESSURE_EXPIRE \
    ICECC_TEST_PENDING_DISCONNECT ICECC_TEST_R1_PUBLISH_ONLY \
    ICECC_TEST_SOCKET ICECC_TEST_SEND_EAGAIN_MARKER \
    ICECC_TEST_BACKPRESSURE_SCHED_PORT ICECC_TEST_SNDBUF_SHIM \
    ICECC_TEST_SCHEDULER_RCVBUF

timeout --signal=TERM --kill-after=5s 120s env \
    -u ICECC_TEST_P51_RESTART_C_C2F1 \
    ICECC_TEST_POSITIVE_DAEMON=1 ICECC_P51_MODE=on \
    ICECC_TEST_P51_PROFILE=ZSTD_TU ICECC_TEST_P51_RESTART_F_C1F2=1 \
    "$build_dir/p50daemonpositive" \
    "$top_build_dir/daemon/iceccd" "$top_build_dir/cache/icecc-cache-service"

timeout --signal=TERM --kill-after=5s 120s env \
    -u ICECC_TEST_P51_RESTART_F_C1F2 \
    ICECC_TEST_POSITIVE_DAEMON=1 ICECC_P51_MODE=on \
    ICECC_TEST_P51_PROFILE=ZSTD_TU ICECC_TEST_P51_RESTART_C_C2F1=1 \
    "$build_dir/p50daemonpositive" \
    "$top_build_dir/daemon/iceccd" "$top_build_dir/cache/icecc-cache-service"
