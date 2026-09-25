#!/bin/sh
set -eu
unset ICECC_TEST_P51_EXPIRED_ARM_WIRE \
    ICECC_TEST_P51_PAUSE_AFTER_GOODBYE_REQUEST \
    ICECC_TEST_P50_SOURCE_BUDGET_MSEC \
    ICECC_TEST_P51_RESTART_CHAIN_F_C_W30

build_dir=${ICECC_TEST_BUILDDIR:?ICECC_TEST_BUILDDIR is required}
top_build_dir=${ICECC_TEST_TOP_BUILDDIR:?ICECC_TEST_TOP_BUILDDIR is required}

if [ "$(id -u)" -ne 0 ] || { [ ! -e /.dockerenv ] && [ ! -e /run/.containerenv ]; }; then
    echo "FAIL: P51 W30 process-restart gate requires an isolated root container" >&2
    exit 2
fi
if ! command -v iptables >/dev/null 2>&1 || ! iptables -w -L >/dev/null 2>&1; then
    echo "FAIL: P51 W30 process-restart gate requires usable NET_ADMIN" >&2
    exit 2
fi
if ! getent passwd icecc >/dev/null 2>&1; then
    echo "FAIL: isolated image has no unprivileged icecc identity" >&2
    exit 2
fi
if [ -z "${ICEFARM_TMPDIR:-}" ] || [ ! -d "$ICEFARM_TMPDIR" ] ||
   [ ! -w "$ICEFARM_TMPDIR" ]; then
    echo "FAIL: P51 W30 process-restart gate needs writable ICEFARM_TMPDIR" >&2
    exit 2
fi
if command -v runuser >/dev/null 2>&1 &&
   ! runuser -u icecc -- test -x "$ICEFARM_TMPDIR" -a -w "$ICEFARM_TMPDIR"; then
    echo "FAIL: ICEFARM_TMPDIR is not traversable/writable by the daemon identity" >&2
    exit 2
fi

# Keep Unix-domain socket paths short; the caller should mount its scratch at
# a short in-container path such as /work/tmp.
short_tmp="$ICEFARM_TMPDIR/p51r"
mkdir -p "$short_tmp"
chmod 1777 "$short_tmp"
export TMPDIR="$short_tmp"

unset ICECC_TEST_P51_RESTART_F_C1F2 ICECC_TEST_P51_RESTART_C_C2F1 \
    ICECC_TEST_P51_RESTART_W30_F_C1F2 ICECC_TEST_P51_RESTART_W30_C_C2F1 \
    ICECC_TEST_P51_RESTART_W30_TOPOLOGY ICECC_TEST_P51_RESTART_CHAIN_F_C_W30 \
    ICECC_TEST_P51_MULTILINK ICECC_TEST_P51_VERTICAL_W30 \
    ICECC_TEST_P51_VERTICAL ICECC_TEST_P51_CANCEL_REPLACEMENT \
    ICECC_TEST_PENDING_DISCONNECT ICECC_TEST_R1_PUBLISH_ONLY \
    ICECC_TEST_SCHEDULER_BACKPRESSURE ICECC_TEST_SCHEDULER_BACKPRESSURE_EXPIRE \
    ICECC_TEST_SCHEDULER_RCVBUF ICECC_TEST_SCHEDULER_SNDBUF \
    ICECC_TEST_SCHEDULER_SENDBUF ICECC_TEST_SEND_EAGAIN_MARKER \
    ICECC_TEST_BACKPRESSURE_SCHED_PORT ICECC_TEST_SNDBUF_SHIM \
    ICECC_TEST_SOCKET ICECC_TEST_CACHE_SESSION_CLOSE

for topology in C1F2 C1F3 C1F4 C2F1 C3F1 C4F1; do
    for profile in P29V1 ZSTD_TU ZSTD_ROUTE; do
        case "$profile" in
            P29V1) profile_mask=1 ;;
            ZSTD_TU) profile_mask=2 ;;
            ZSTD_ROUTE) profile_mask=4 ;;
        esac
        case "$topology" in
            C1F2|C2F1) healthy_siblings=1 ;;
            C1F3|C3F1) healthy_siblings=2 ;;
            C1F4|C4F1) healthy_siblings=3 ;;
        esac
        log="$ICEFARM_TMPDIR/p51-restart-w30-$topology-$profile.log"
        echo "P51_RESTART_W30_START=$topology/$profile"
        set +e
        timeout --signal=TERM --kill-after=5s 180s env \
            -u ICECC_TEST_P51_RESTART_F_C1F2 \
            -u ICECC_TEST_P51_RESTART_C_C2F1 \
            -u ICECC_TEST_P51_RESTART_W30_F_C1F2 \
            -u ICECC_TEST_P51_RESTART_W30_C_C2F1 \
            ICECC_TEST_POSITIVE_DAEMON=1 ICECC_P51_MODE=on \
            ICECC_TEST_P51_PROFILE="$profile" \
            ICECC_TEST_P51_RESTART_W30_TOPOLOGY="$topology" \
            "$build_dir/p50daemonpositive" \
            "$top_build_dir/daemon/iceccd" \
            "$top_build_dir/cache/icecc-cache-service" >"$log" 2>&1
        status=$?
        set -e
        cat "$log"
        if [ "$status" -ne 0 ]; then
            echo "FAIL: W30 process restart $topology/$profile exited $status; log=$log" >&2
            exit "$status"
        fi
        marker="P51_PROCESS_RESTART_W30 topology=$topology affected="
        if ! grep -F "$marker" "$log" | \
             grep -F "profile=$profile_mask jobs=30 fresh_attached=1 healthy_attached=1 healthy_siblings=$healthy_siblings/$healthy_siblings target_parent_stopped=1" >/dev/null; then
            echo "FAIL: expected W30 restart completion marker missing; log=$log" >&2
            exit 1
        fi
        echo "P51_RESTART_W30_PASS=$topology/$profile log=$log"
    done
done
