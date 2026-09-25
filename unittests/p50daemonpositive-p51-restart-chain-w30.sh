#!/bin/sh
set -eu

build_dir=${ICECC_TEST_BUILDDIR:?ICECC_TEST_BUILDDIR is required}
top_build_dir=${ICECC_TEST_TOP_BUILDDIR:?ICECC_TEST_TOP_BUILDDIR is required}

if [ "$(id -u)" -ne 0 ] || { [ ! -e /.dockerenv ] && [ ! -e /run/.containerenv ]; }; then
    echo "FAIL: ordered P51 restart chain requires an isolated root container" >&2
    exit 2
fi
if ! command -v iptables >/dev/null 2>&1 || ! iptables -w -L >/dev/null 2>&1; then
    echo "FAIL: ordered P51 restart chain requires usable NET_ADMIN" >&2
    exit 2
fi
if ! getent passwd icecc >/dev/null 2>&1; then
    echo "FAIL: isolated image has no unprivileged icecc identity" >&2
    exit 2
fi
if [ -z "${ICEFARM_TMPDIR:-}" ] || [ ! -d "$ICEFARM_TMPDIR" ] ||
   [ ! -w "$ICEFARM_TMPDIR" ]; then
    echo "FAIL: ordered P51 restart chain needs writable ICEFARM_TMPDIR" >&2
    exit 2
fi

short_tmp="$ICEFARM_TMPDIR/p51r"
mkdir -p "$short_tmp"
chmod 1777 "$short_tmp"
export TMPDIR="$short_tmp"

unset ICECC_TEST_P51_RESTART_F_C1F2 ICECC_TEST_P51_RESTART_C_C2F1 \
    ICECC_TEST_P51_CANCEL_BEFORE_START \
    ICECC_TEST_P51_RESTART_W30_F_C1F2 ICECC_TEST_P51_RESTART_W30_C_C2F1 \
    ICECC_TEST_P51_RESTART_W30_TOPOLOGY ICECC_TEST_P51_MULTILINK \
    ICECC_TEST_P51_VERTICAL_W30 ICECC_TEST_P51_VERTICAL \
    ICECC_TEST_P51_CANCEL_REPLACEMENT ICECC_TEST_PENDING_DISCONNECT \
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
    log="$ICEFARM_TMPDIR/p51-restart-chain-f-c-w30-$profile.log"
    echo "P51_RESTART_CHAIN_START=F_then_C/$profile"
    set +e
    timeout --signal=TERM --kill-after=5s 300s env \
        -u ICECC_TEST_P51_RESTART_F_C1F2 \
        -u ICECC_TEST_P51_RESTART_C_C2F1 \
        -u ICECC_TEST_P51_RESTART_W30_F_C1F2 \
        -u ICECC_TEST_P51_RESTART_W30_C_C2F1 \
        -u ICECC_TEST_P51_RESTART_W30_TOPOLOGY \
        ICECC_TEST_POSITIVE_DAEMON=1 ICECC_P51_MODE=on \
        ICECC_TEST_P51_PROFILE="$profile" \
        ICECC_TEST_P51_RESTART_CHAIN_F_C_W30=1 \
        "$build_dir/p50daemonpositive" \
        "$top_build_dir/daemon/iceccd" \
        "$top_build_dir/cache/icecc-cache-service" >"$log" 2>&1
    status=$?
    set -e
    cat "$log"
    if [ "$status" -ne 0 ]; then
        echo "FAIL: ordered F-to-C restart chain $profile exited $status; log=$log" >&2
        exit "$status"
    fi
    grep -F "P51_PROCESS_RESTART_CHAIN_F_C profile=$profile_mask f_old_assignment_rejected=1 c_parent_stopped=1 healthy_c2_f2=1 c_f_commits=30" \
        "$log" >/dev/null &&
    grep -F "c_old_attach=30" "$log" >/dev/null &&
    grep -F "post_c=30/30" "$log" >/dev/null &&
    grep -F "c_rotated_f_preserved=1" \
        "$log" >/dev/null || {
        echo "FAIL: exact ordered-chain completion marker missing; log=$log" >&2
        exit 1
    }
    echo "P51_RESTART_CHAIN_PASS=F_then_C/$profile log=$log"
done

echo "PASS: ordered F-then-C restart chain qualified for P29V1, ZSTD_TU, and ZSTD_ROUTE"
