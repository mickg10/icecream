#!/bin/sh
set -eu

: "${TLA2TOOLS_JAR:?set TLA2TOOLS_JAR to the pinned tla2tools.jar}"
: "${TLC_STATE_ROOT:?set TLC_STATE_ROOT to a unique retained scratch directory}"

[ "${TLC_STATE_ROOT#/}" != "$TLC_STATE_ROOT" ] || {
    echo 'FAIL: TLC_STATE_ROOT must be absolute' >&2; exit 2;
}
[ -d "$TLC_STATE_ROOT" ] && [ ! -L "$TLC_STATE_ROOT" ] && [ -w "$TLC_STATE_ROOT" ] || {
    echo 'FAIL: TLC_STATE_ROOT must be an existing writable non-symlink directory' >&2; exit 2;
}
[ -f "$TLA2TOOLS_JAR" ] || { echo 'FAIL: pinned TLC jar is absent' >&2; exit 2; }

PINNED_JAR_SHA256=936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88
ACTUAL_JAR_SHA256=$(sha256sum "$TLA2TOOLS_JAR" | awk '{print $1}')
[ "$ACTUAL_JAR_SHA256" = "$PINNED_JAR_SHA256" ] || {
    echo "FAIL: TLC jar digest mismatch: $ACTUAL_JAR_SHA256" >&2; exit 2;
}

ROW_TIMEOUT_SECONDS=${ROW_TIMEOUT_SECONDS:-120}
case "$ROW_TIMEOUT_SECONDS" in
    ''|*[!0-9]*|0) echo 'FAIL: ROW_TIMEOUT_SECONDS must be a positive integer' >&2; exit 2 ;;
esac

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
MODULE=Protocol50ConsumedProof.tla
MODULE_PATH="$SCRIPT_DIR/$MODULE"
RESULTS="$TLC_STATE_ROOT/consumed-proof"
mkdir "$RESULTS"

run_row() {
    row=$1
    cfg=$2
    expected=$3
    log="$RESULTS/$row.log"
    state="$RESULTS/$row-states"
    printf 'ROW %s\n' "$row"
    printf 'config_sha256=%s\n' "$(sha256sum "$SCRIPT_DIR/$cfg" | awk '{print $1}')"
    printf 'module_sha256=%s\n' "$(sha256sum "$MODULE_PATH" | awk '{print $1}')"
    set +e
    timeout --signal=TERM --kill-after=5s "$ROW_TIMEOUT_SECONDS" \
        java -Xmx2g -XX:+UseParallelGC -cp "$TLA2TOOLS_JAR" tlc2.TLC \
        -workers 2 -metadir "$state" -config "$SCRIPT_DIR/$cfg" "$MODULE_PATH" \
        >"$log" 2>&1
    rc=$?
    set -e
    if [ "$expected" = clean ]; then
        if [ "$rc" -eq 0 ] && grep -Fqx 'Model checking completed. No error has been found.' "$log"; then
            printf 'PASS %s exit=%s log=%s\n' "$row" "$rc" "$log"
            return
        fi
    else
        diagnostic="Error: Invariant $expected is violated."
        if [ "$rc" -ne 0 ] && [ "$rc" -ne 124 ] && [ "$rc" -ne 137 ] && \
           grep -Fqx "$diagnostic" "$log"; then
            printf 'EXPECTED-COUNTEREXAMPLE %s diagnostic=%s exit=%s log=%s\n' \
                "$row" "$expected" "$rc" "$log"
            return
        fi
    fi
    printf 'FAIL %s exit=%s expected=%s log=%s\n' "$row" "$rc" "$expected" "$log" >&2
    tail -40 "$log" >&2
    exit 1
}

run_row safety-c2f1 Protocol50ConsumedProofC2F1.cfg clean
run_row safety-c3f1 Protocol50ConsumedProofC3F1.cfg clean
run_row safety-c4f1 Protocol50ConsumedProofC4F1.cfg clean
run_row safety-c1f2 Protocol50ConsumedProofC1F2.cfg clean
run_row safety-c1f3 Protocol50ConsumedProofC1F3.cfg clean
run_row safety-c1f4 Protocol50ConsumedProofC1F4.cfg clean

run_row witness-c2f1 Protocol50ConsumedProofC2F1Witness.cfg ConsumedProofResetWitnessNotReached
run_row witness-c3f1 Protocol50ConsumedProofC3F1Witness.cfg ConsumedProofResetWitnessNotReached
run_row witness-c4f1 Protocol50ConsumedProofC4F1Witness.cfg ConsumedProofResetWitnessNotReached
run_row witness-c1f2 Protocol50ConsumedProofC1F2Witness.cfg ConsumedProofResetWitnessNotReached
run_row witness-c1f3 Protocol50ConsumedProofC1F3Witness.cfg ConsumedProofResetWitnessNotReached
run_row witness-c1f4 Protocol50ConsumedProofC1F4Witness.cfg ConsumedProofResetWitnessNotReached

run_row mutant-clear-proof Protocol50ConsumedProofClearMutantC2F1.cfg EveryConsumedReservationRetainsItsProof
run_row mutant-cross-c-on-one-f Protocol50ConsumedProofCrossCMutantC2F1.cfg OtherRelationshipUntouchedByTargetSettlement
run_row mutant-reset-retry-double-credit Protocol50ConsumedProofDoubleCreditMutantC1F2.cfg ResetCreditRearmedAtMostOnce

printf 'CONSUMED-PROOF-TLC PASS rows=15\n'
