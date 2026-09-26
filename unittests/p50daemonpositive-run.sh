#!/bin/sh
set -eu
unset ICECC_TEST_P51_RESTART_W30_TOPOLOGY \
    ICECC_TEST_P51_EXPIRED_ARM_WIRE \
    ICECC_TEST_P51_PAUSE_AFTER_GOODBYE_REQUEST \
    ICECC_TEST_P50_SOURCE_BUDGET_MSEC \
    ICECC_TEST_P51_RESTART_CHAIN_F_C_W30 \
    ICECC_TEST_P51_CANCEL_BEFORE_START \
    ICECC_TEST_P51_CANCEL_AFTER_DEADLINE \
    ICECC_TEST_P51_CANCEL_C_EXPIRED_F_LIVE \
    ICECC_TEST_P51_CANCEL_RETAINED_COMMITTED
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
for profile in P29V1 ZSTD_TU ZSTD_ROUTE; do
    ICECC_TEST_POSITIVE_DAEMON=1 ICECC_P51_MODE=on \
        ICECC_TEST_P51_CANCEL_BEFORE_START=1 \
        ICECC_TEST_P51_PROFILE="$profile" \
        "$build_dir/p50daemonpositive" \
        "$top_build_dir/daemon/iceccd" \
        "$top_build_dir/cache/icecc-cache-service"
done
for profile in P29V1 ZSTD_TU ZSTD_ROUTE; do
    ICECC_TEST_POSITIVE_DAEMON=1 ICECC_P51_MODE=on \
        ICECC_TEST_P51_CANCEL_AFTER_DEADLINE=1 \
        ICECC_TEST_P51_PROFILE="$profile" \
        "$build_dir/p50daemonpositive" \
        "$top_build_dir/daemon/iceccd" \
        "$top_build_dir/cache/icecc-cache-service"
done
for profile in P29V1 ZSTD_TU ZSTD_ROUTE; do
    ICECC_TEST_POSITIVE_DAEMON=1 ICECC_P51_MODE=on \
        ICECC_TEST_P51_CANCEL_C_EXPIRED_F_LIVE=1 \
        ICECC_TEST_P51_PROFILE="$profile" \
        "$build_dir/p50daemonpositive" \
        "$top_build_dir/daemon/iceccd" \
        "$top_build_dir/cache/icecc-cache-service-test"
done
for profile in P29V1 ZSTD_TU ZSTD_ROUTE; do
    ICECC_TEST_POSITIVE_DAEMON=1 ICECC_P51_MODE=on \
        ICECC_TEST_P51_CANCEL_RETAINED_COMMITTED=1 \
        ICECC_TEST_P51_PROFILE="$profile" \
        "$build_dir/p50daemonpositive" \
        "$top_build_dir/daemon/iceccd" \
        "$top_build_dir/cache/icecc-cache-service"
done
