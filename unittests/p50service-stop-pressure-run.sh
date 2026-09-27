#!/bin/sh
# Opt-in thorough service-stop/reconnect regression; intentionally not in TESTS.
set -eu

build_dir=${ICECC_TEST_BUILDDIR:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)}
binary=${ICECC_TEST_P50CACHESERVICE_BIN:-$build_dir/p50cacheservice}
cache_service=${ICECC_TEST_CACHE_SERVICE:-$(dirname -- "$build_dir")/cache/icecc-cache-service}
ready_shim=${ICECC_TEST_READY_CLOSE_SHIM:-$build_dir/readyclose_shim.so}
test -x "$binary"
test -x "$cache_service"
test -r "$ready_shim"

tmp_root=${ICEFARM_TMPDIR:-${TMPDIR:-/tmp}}
run_dir=$(mktemp -d "$tmp_root/p50service-stop-pressure.XXXXXX")
cleanup() {
    status=$?
    trap - EXIT
    if [ "$status" -ne 0 ]; then
        printf 'P51_SERVICE_STOP_PRESSURE_FAIL logs=%s\n' "$run_dir" >&2
        [ ! -f "$run_dir/d16.log" ] || cat "$run_dir/d16.log" >&2
        [ ! -f "$run_dir/d17.log" ] || cat "$run_dir/d17.log" >&2
        return "$status"
    fi
    rm -rf -- "$run_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

export ICECC_TEST_CACHE_SERVICE=$cache_service
export ICECC_TEST_READY_CLOSE_SHIM=$ready_shim
timeout --signal=TERM --kill-after=10s 240s \
    "$binary" --d16-service-stop-pressure >"$run_dir/d16.log" 2>&1
timeout --signal=TERM --kill-after=10s 360s \
    "$binary" --d17-repeated-window-cancel >"$run_dir/d17.log" 2>&1

for profile in P29V1 ZSTD_TU ZSTD_ROUTE; do
    d16_count=$(awk -v profile="$profile" \
        '$1 == "P51_D16_SERVICE_STOP_PRESSURE" && $2 == "profile=" profile && $NF == "PASS" { n++ } END { print n+0 }' \
        "$run_dir/d16.log")
    d17_count=$(awk -v profile="$profile" \
        '$1 == "P51_D17" && $2 == "cycle" && $3 == "profile=" profile { n++ } END { print n+0 }' \
        "$run_dir/d17.log")
    test "$d16_count" -eq 1
    test "$d17_count" -eq 3
done

cat "$run_dir/d16.log"
cat "$run_dir/d17.log"
printf 'P51_SERVICE_STOP_PRESSURE_PASS profiles=3 d16=3 d17=9\n'
