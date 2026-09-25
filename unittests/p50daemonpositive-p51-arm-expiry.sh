#!/bin/sh
set -eu

build_dir=${ICECC_TEST_BUILDDIR:?ICECC_TEST_BUILDDIR is required}
top_build_dir=${ICECC_TEST_TOP_BUILDDIR:?ICECC_TEST_TOP_BUILDDIR is required}

if [ "$(id -u)" -ne 0 ] || { [ ! -e /.dockerenv ] && [ ! -e /run/.containerenv ]; }; then
    echo "FAIL: P51 ARM-expiry wire gate requires an isolated root container" >&2
    exit 2
fi
if ! getent passwd icecc >/dev/null 2>&1; then
    echo "FAIL: P51 ARM-expiry wire gate requires the unprivileged icecc test identity" >&2
    exit 2
fi
if [ -z "${ICEFARM_TMPDIR:-}" ] || [ ! -d "$ICEFARM_TMPDIR" ] ||
   [ ! -w "$ICEFARM_TMPDIR" ]; then
    echo "FAIL: P51 ARM-expiry wire gate requires writable scratch-backed ICEFARM_TMPDIR" >&2
    exit 2
fi
run_dir=$(mktemp -d "$ICEFARM_TMPDIR/p51-arm-expiry.XXXXXX")

for profile in P29V1 ZSTD_TU ZSTD_ROUTE; do
    echo "P51_ARM_EXPIRY_WIRE_START=$profile"
    unset ICECC_TEST_P51_VERTICAL ICECC_TEST_P51_VERTICAL_W30 \
        ICECC_TEST_P51_CANCEL_REPLACEMENT ICECC_TEST_P51_MULTILINK \
        ICECC_TEST_P51_RESTART_F_C1F2 ICECC_TEST_P51_RESTART_C_C2F1 \
        ICECC_TEST_P51_RESTART_W30_F_C1F2 ICECC_TEST_P51_RESTART_W30_C_C2F1 \
        ICECC_TEST_P51_RESTART_W30_TOPOLOGY ICECC_TEST_P51_LOST_RECEIPTS \
        ICECC_TEST_P51_SYNTH_SCHEDULER_W30 \
        ICECC_TEST_P51_RESTART_CHAIN_F_C_W30 \
        ICECC_TEST_P51_PAUSE_AFTER_GOODBYE_REQUEST \
        ICECC_TEST_P50_SOURCE_BUDGET_MSEC
    log="$run_dir/$profile.log"
    set +e
    timeout --signal=TERM --kill-after=10s 45s env \
        ICECC_TEST_POSITIVE_DAEMON=1 \
        ICECC_P51_MODE=on \
        ICECC_TEST_P51_EXPIRED_ARM_WIRE=1 \
        ICECC_TEST_P51_PROFILE="$profile" \
            "$build_dir/p50daemonpositive" \
                "$top_build_dir/daemon/iceccd" \
                "$top_build_dir/cache/icecc-cache-service" >"$log" 2>&1
    status=$?
    set -e
    printf '%s\n' "$status" >"$run_dir/$profile.exit"
    cat "$log"
    if [ "$status" -ne 0 ]; then
        echo "FAIL: P51 ARM-expiry wire gate $profile exited $status; log=$log" >&2
        exit "$status"
    fi
    echo "P51_ARM_EXPIRY_WIRE_PASS=$profile"
done
