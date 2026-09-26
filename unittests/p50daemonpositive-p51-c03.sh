#!/bin/sh
set -eu

build_dir=${ICECC_TEST_BUILDDIR:?ICECC_TEST_BUILDDIR is required}
top_build_dir=${ICECC_TEST_TOP_BUILDDIR:?ICECC_TEST_TOP_BUILDDIR is required}

if [ "$(id -u)" -ne 0 ] || { [ ! -e /.dockerenv ] && [ ! -e /run/.containerenv ]; }; then
    echo "FAIL: P51 C03 wire gate requires an isolated root container" >&2
    exit 2
fi
if ! getent passwd icecc >/dev/null 2>&1; then
    echo "FAIL: P51 C03 wire gate requires the unprivileged icecc test identity" >&2
    exit 2
fi
if [ -z "${ICEFARM_TMPDIR:-}" ] || [ ! -d "$ICEFARM_TMPDIR" ] ||
   [ ! -w "$ICEFARM_TMPDIR" ]; then
    echo "FAIL: P51 C03 wire gate requires writable scratch-backed ICEFARM_TMPDIR" >&2
    exit 2
fi
run_dir=$(mktemp -d "$ICEFARM_TMPDIR/p51-c03.XXXXXX")

run_case() {
    name=$1
    mode=$2
    profile=$3
    shift 3
    log="$run_dir/$name-$profile.log"
    echo "P51_C03_START=$name profile=$profile"
    set +e
    timeout --signal=TERM --kill-after=10s 45s env \
        ICECC_TEST_POSITIVE_DAEMON=1 ICECC_P51_MODE=on \
        ICECC_TEST_P51_PROFILE="$profile" "$@" \
        "$build_dir/p50daemonpositive" \
        "$top_build_dir/daemon/iceccd" \
        "$top_build_dir/cache/icecc-cache-service" >"$log" 2>&1
    status=$?
    set -e
    cat "$log"
    if [ "$status" -ne 0 ]; then
        echo "FAIL: P51 C03 $name $profile exited $status; log=$log" >&2
        exit "$status"
    fi
    case "$mode" in
        client|sidecar)
            scenario=$mode
            [ "$mode" = sidecar ] && scenario=sidecar_death
            [ "$mode" = client ] && scenario=client_eof
            grep -q "C03_OBSERVATION scenario=$scenario" "$log"
            grep -q "C03_FRESH scenario=$scenario .*success=1" "$log"
            ;;
        expiry)
            grep -q "expired exact R2 reservation produced End then EOF" "$log"
            grep -q "fresh exact ARM succeeds" "$log"
            ;;
        vertical)
            grep -q "P51_CACHE_LINK_READY request=" "$log"
            ;;
        *) echo "FAIL: internal unknown C03 mode $mode" >&2; exit 2 ;;
    esac
    echo "P51_C03_PASS=$name profile=$profile"
}

for profile in P29V1 ZSTD_TU ZSTD_ROUTE; do
    run_case client client "$profile" ICECC_TEST_P51_C03_CLIENT_EOF=1
done
for profile in P29V1 ZSTD_TU ZSTD_ROUTE; do
    run_case sidecar sidecar "$profile" ICECC_TEST_P51_C03_SIDECAR_DEATH=1
done
for profile in P29V1 ZSTD_TU ZSTD_ROUTE; do
    run_case expiry expiry "$profile" ICECC_TEST_P51_EXPIRED_ARM_WIRE=1
done
run_case healthy vertical P29V1 ICECC_TEST_P51_VERTICAL=1
