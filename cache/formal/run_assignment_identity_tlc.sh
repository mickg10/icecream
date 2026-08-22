#!/bin/sh
set -eu

: "${TLA2TOOLS_JAR:?set TLA2TOOLS_JAR to tla2tools.jar}"
case "$TLA2TOOLS_JAR" in
    /*) ;;
    *) TLA2TOOLS_JAR=$(CDPATH='' cd -- "$(dirname -- "$TLA2TOOLS_JAR")" && pwd)/$(basename -- "$TLA2TOOLS_JAR") ;;
esac
TLC_WORKERS=${TLC_WORKERS:-1}
TLC_STATE_ROOT=${TLC_STATE_ROOT:-/tmp/icecream-p50-assignment-identity-tlc}
SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
MODULE=Protocol50AssignmentIdentity.tla
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
    sha256_file "$SCRIPT_DIR/$MODULE"
    sha256_file "$SCRIPT_DIR/$config"
    java -version
    echo "command: $TLC_MAIN -metadir $state_dir -config $config $MODULE"
    set +e
    (cd "$SCRIPT_DIR" &&
        $TLC_MAIN -metadir "$state_dir" -config "$config" "$MODULE") \
        >"$log" 2>&1
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
    sha256_file "$TLA2TOOLS_JAR"
    sha256_file "$SCRIPT_DIR/$MODULE"
    sha256_file "$SCRIPT_DIR/$config"
    java -version
    echo "command: $TLC_MAIN -metadir $state_dir -config $config $MODULE"
    set +e
    (cd "$SCRIPT_DIR" &&
        $TLC_MAIN -metadir "$state_dir" -config "$config" "$MODULE") \
        >"$log" 2>&1
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

run_pass identity Protocol50AssignmentIdentity.cfg

run_expected_failure cell-43-43-43 \
    Protocol50AssignmentIdentityCell43_43_43.cfg NoExpectedCellOutcome
run_expected_failure cell-43-43-50 \
    Protocol50AssignmentIdentityCell43_43_50.cfg NoExpectedCellOutcome
run_expected_failure cell-43-50-43 \
    Protocol50AssignmentIdentityCell43_50_43.cfg NoExpectedCellOutcome
run_expected_failure cell-43-50-50 \
    Protocol50AssignmentIdentityCell43_50_50.cfg NoExpectedCellOutcome
run_expected_failure cell-50-43-43 \
    Protocol50AssignmentIdentityCell50_43_43.cfg NoExpectedCellOutcome
run_expected_failure cell-50-43-50 \
    Protocol50AssignmentIdentityCell50_43_50.cfg NoExpectedCellOutcome
run_expected_failure cell-50-50-43 \
    Protocol50AssignmentIdentityCell50_50_43.cfg NoExpectedCellOutcome
run_expected_failure cell-50-50-50 \
    Protocol50AssignmentIdentityCell50_50_50.cfg NoExpectedCellOutcome

run_expected_failure strict-exact-witness \
    Protocol50AssignmentIdentityStrictWitness.cfg \
    NoStrictExactClaimWitness
run_expected_failure strict-mixed-refusal-witness \
    Protocol50AssignmentIdentityStrictMixedWitness.cfg \
    NoStrictMixedRefusalWitness
run_expected_failure local-exemption-witness \
    Protocol50AssignmentIdentityLocalWitness.cfg \
    NoLocalExemptionWitness
run_expected_failure reconnect-exact-witness \
    Protocol50AssignmentIdentityReconnectWitness.cfg \
    NoReconnectExactClaimWitness
run_expected_failure revoked-release-witness \
    Protocol50AssignmentIdentityRevokedWitness.cfg \
    NoRevokedReleaseWitness
run_expected_failure claimed-settlement-witness \
    Protocol50AssignmentIdentitySettlementWitness.cfg \
    NoClaimedOrdinarySettlementWitness

run_expected_failure stale-epoch-mutant \
    Protocol50AssignmentIdentityStaleEpochMutant.cfg \
    StaleIdentityRejected
run_expected_failure stale-wire-mutant \
    Protocol50AssignmentIdentityStaleWireMutant.cfg \
    StaleIdentityRejected
run_expected_failure stale-nonce-mutant \
    Protocol50AssignmentIdentityStaleNonceMutant.cfg \
    StaleIdentityRejected
run_expected_failure epoch-only-mutant \
    Protocol50AssignmentIdentityEpochOnlyMutant.cfg \
    PartialIdentityRejected
run_expected_failure nonce-only-mutant \
    Protocol50AssignmentIdentityNonceOnlyMutant.cfg \
    PartialIdentityRejected
run_expected_failure dropped-p50-identity-mutant \
    Protocol50AssignmentIdentityDropMutant.cfg \
    P50PathPreservesIdentity
run_expected_failure strict-absent-mutant \
    Protocol50AssignmentIdentityStrictAbsentMutant.cfg \
    StrictClaimsExact
run_expected_failure reconnect-drop-mutant \
    Protocol50AssignmentIdentityReconnectDropMutant.cfg \
    ReconnectPreservesIdentity
run_expected_failure release-claimed-mutant \
    Protocol50AssignmentIdentityReleaseClaimedMutant.cfg \
    ClaimedResultRetains
