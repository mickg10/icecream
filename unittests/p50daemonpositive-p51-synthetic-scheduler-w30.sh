#!/bin/sh
set -eu
unset ICECC_TEST_P51_RESTART_W30_TOPOLOGY \
    ICECC_TEST_P51_EXPIRED_ARM_WIRE \
    ICECC_TEST_P51_CANCEL_BEFORE_START \
    ICECC_TEST_P51_PAUSE_AFTER_GOODBYE_REQUEST \
    ICECC_TEST_P50_SOURCE_BUDGET_MSEC \
    ICECC_TEST_P51_RESTART_CHAIN_F_C_W30

build_dir=${ICECC_TEST_BUILDDIR:?ICECC_TEST_BUILDDIR is required}
top_build_dir=${ICECC_TEST_TOP_BUILDDIR:?ICECC_TEST_TOP_BUILDDIR is required}

if [ "$(id -u)" -ne 0 ] || { [ ! -e /.dockerenv ] && [ ! -e /run/.containerenv ]; }; then
    echo "FAIL: synthetic P51 scheduler-epoch W30 gate requires a disposable root container" >&2
    exit 2
fi
if ! command -v iptables >/dev/null 2>&1 || ! iptables -w -L >/dev/null 2>&1; then
    echo "FAIL: synthetic P51 scheduler-epoch W30 gate requires usable NET_ADMIN" >&2
    exit 2
fi
if ! getent passwd icecc >/dev/null 2>&1; then
    echo "FAIL: isolated image has no unprivileged icecc identity" >&2
    exit 2
fi
if [ -z "${ICEFARM_TMPDIR:-}" ] || [ ! -d "$ICEFARM_TMPDIR" ] ||
   [ ! -w "$ICEFARM_TMPDIR" ]; then
    echo "FAIL: synthetic P51 scheduler-epoch W30 gate needs writable ICEFARM_TMPDIR" >&2
    exit 2
fi
if command -v runuser >/dev/null 2>&1 &&
   ! runuser -u icecc -- test -x "$ICEFARM_TMPDIR" -a -w "$ICEFARM_TMPDIR"; then
    echo "FAIL: ICEFARM_TMPDIR is not traversable/writable by the daemon identity" >&2
    exit 2
fi

# Keep daemon Unix-socket paths short; mount scratch at a short path like /work/tmp.
short_tmp="$ICEFARM_TMPDIR/p51ss"
mkdir -p "$short_tmp"
chmod 1777 "$short_tmp"
export TMPDIR="$short_tmp"

unset ICECC_TEST_P51_RESTART_F_C1F2 ICECC_TEST_P51_RESTART_C_C2F1 \
    ICECC_TEST_P51_RESTART_W30_F_C1F2 ICECC_TEST_P51_RESTART_W30_C_C2F1 \
    ICECC_TEST_P51_MULTILINK ICECC_TEST_P51_VERTICAL_W30 \
    ICECC_TEST_P51_VERTICAL ICECC_TEST_P51_CANCEL_REPLACEMENT \
    ICECC_TEST_P51_CANCEL_BEFORE_START \
    ICECC_TEST_P51_SYNTH_SCHEDULER_W30 ICECC_TEST_PENDING_DISCONNECT \
    ICECC_TEST_R1_PUBLISH_ONLY ICECC_TEST_SCHEDULER_BACKPRESSURE \
    ICECC_TEST_SCHEDULER_BACKPRESSURE_EXPIRE ICECC_TEST_SCHEDULER_RCVBUF \
    ICECC_TEST_SCHEDULER_SNDBUF ICECC_TEST_SCHEDULER_SENDBUF \
    ICECC_TEST_SEND_EAGAIN_MARKER ICECC_TEST_BACKPRESSURE_SCHED_PORT \
    ICECC_TEST_SNDBUF_SHIM ICECC_TEST_SOCKET ICECC_TEST_CACHE_SESSION_CLOSE

for profile in P29V1 ZSTD_TU ZSTD_ROUTE; do
    case "$profile" in
        P29V1) profile_mask=1 ;;
        ZSTD_TU) profile_mask=2 ;;
        ZSTD_ROUTE) profile_mask=4 ;;
    esac
    log="$ICEFARM_TMPDIR/p51-synth-sched-w30-$profile.log"
    echo "P51_SYNTH_SCHEDULER_W30_START=$profile (synthetic scheduler-session epoch replacement)"
    set +e
    timeout --signal=TERM --kill-after=5s 180s env \
        ICECC_TEST_POSITIVE_DAEMON=1 ICECC_P51_MODE=on \
        ICECC_TEST_P51_PROFILE="$profile" \
        ICECC_TEST_P51_SYNTH_SCHEDULER_W30=1 \
        "$build_dir/p50daemonpositive" \
        "$top_build_dir/daemon/iceccd" \
        "$top_build_dir/cache/icecc-cache-service" >"$log" 2>&1
    status=$?
    set -e
    cat "$log"
    if [ "$status" -ne 0 ]; then
        echo "FAIL: synthetic scheduler-epoch W30 $profile exited $status; log=$log" >&2
        exit "$status"
    fi
    marker="P51_SYNTH_SCHEDULER_EPOCH_W30_PASS profile=$profile_mask old=30/exact-results fresh=30/committed/30/attached healthy_sibling=1 daemon_identity_stable=1"
    if ! grep -F "$marker" "$log" >/dev/null; then
        echo "FAIL: expected synthetic scheduler-epoch W30 marker missing; log=$log" >&2
        exit 1
    fi
    echo "P51_SYNTH_SCHEDULER_W30_PASS=$profile log=$log"
done
