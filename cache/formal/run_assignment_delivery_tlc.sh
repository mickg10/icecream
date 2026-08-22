#!/bin/sh
set -eu

: "${TLA2TOOLS_JAR:?set TLA2TOOLS_JAR to tla2tools.jar}"
case "$TLA2TOOLS_JAR" in
    /*) ;;
    *) TLA2TOOLS_JAR=$(CDPATH= cd -- "$(dirname -- "$TLA2TOOLS_JAR")" && pwd)/$(basename -- "$TLA2TOOLS_JAR") ;;
esac
TLC_WORKERS=${TLC_WORKERS:-1}
TLC_STATE_ROOT=${TLC_STATE_ROOT:-/tmp/icecream-p50-assignment-delivery-tlc}
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TLC_MAIN="java -cp $TLA2TOOLS_JAR tlc2.TLC -workers $TLC_WORKERS"

sha256_file() {
    file=$1
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$file"
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$file"
    else
        echo "SHA256-UNAVAILABLE  $file"
    fi
}

run_pass() {
    name=$1
    config=$2
    state_dir="$TLC_STATE_ROOT/$name"
    log="$TLC_STATE_ROOT/$name.log"
    rm -rf "$state_dir"
    mkdir -p "$state_dir"
    echo "== $name =="
    sha256_file "$TLA2TOOLS_JAR"
    sha256_file "$SCRIPT_DIR/Protocol50AssignmentDelivery.tla"
    sha256_file "$SCRIPT_DIR/$config"
    set +e
    (cd "$SCRIPT_DIR" &&
        $TLC_MAIN -metadir "$state_dir" -config "$config" \
            Protocol50AssignmentDelivery.tla) >"$log" 2>&1
    rc=$?
    set -e
    cat "$log"
    sha256_file "$log"
    if [ "$rc" -ne 0 ]; then
        echo "$name failed with TLC exit status $rc" >&2
        exit 1
    fi
    grep -F "Model checking completed. No error" "$log" >/dev/null || {
        echo "$name returned zero without TLC's no-error marker" >&2
        exit 1
    }
}

run_expected_failure() {
    name=$1
    config=$2
    invariant=$3
    state_dir="$TLC_STATE_ROOT/$name"
    log="$TLC_STATE_ROOT/$name.log"
    rm -rf "$state_dir"
    mkdir -p "$state_dir"
    echo "== $name (expected invariant failure: $invariant) =="
    sha256_file "$SCRIPT_DIR/$config"
    set +e
    (cd "$SCRIPT_DIR" &&
        $TLC_MAIN -metadir "$state_dir" -config "$config" \
            Protocol50AssignmentDelivery.tla) >"$log" 2>&1
    rc=$?
    set -e
    cat "$log"
    sha256_file "$log"
    if [ "$rc" -eq 0 ]; then
        echo "$name unexpectedly passed" >&2
        exit 1
    fi
    grep -F "Invariant $invariant is violated" "$log" >/dev/null || {
        echo "$name failed, but not through $invariant" >&2
        exit 1
    }
}

mkdir -p "$TLC_STATE_ROOT"
run_pass delivery Protocol50AssignmentDelivery.cfg
run_expected_failure ready-wire-mutant \
    Protocol50AssignmentReadyWireMutant.cfg PublishedHasMatchingPrepare
run_expected_failure revoke-request-wire-mutant \
    Protocol50AssignmentRevokeRequestWireMutant.cfg \
    RevocationTargetsSentAssignment
run_expected_failure revoke-result-wire-mutant \
    Protocol50AssignmentRevokeResultWireMutant.cfg \
    ReleaseHasMatchingWorkerProof
run_expected_failure late-prepare-mutant \
    Protocol50AssignmentLatePrepareMutant.cfg ClaimedPhaseNeverRegresses
