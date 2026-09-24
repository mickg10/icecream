#!/bin/sh
set -eu

: "${TLA2TOOLS_JAR:?set TLA2TOOLS_JAR to the pinned tla2tools.jar}"
: "${TLC_STATE_ROOT:?set TLC_STATE_ROOT to a unique retained scratch directory}"

[ "${TLC_STATE_ROOT#/}" != "$TLC_STATE_ROOT" ] || {
    echo 'FAIL: TLC_STATE_ROOT must be an absolute path' >&2; exit 2;
}
[ -d "$TLC_STATE_ROOT" ] || { echo 'FAIL: TLC_STATE_ROOT must already exist' >&2; exit 2; }
[ ! -L "$TLC_STATE_ROOT" ] || { echo 'FAIL: TLC_STATE_ROOT must not be a symlink' >&2; exit 2; }
[ -w "$TLC_STATE_ROOT" ] || { echo 'FAIL: TLC_STATE_ROOT must be writable' >&2; exit 2; }
[ -f "$TLA2TOOLS_JAR" ] || { echo 'FAIL: pinned TLC jar is absent' >&2; exit 2; }

PINNED_JAR_SHA256=936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88
ACTUAL_JAR_SHA256=$(sha256sum "$TLA2TOOLS_JAR" | awk '{print $1}')
[ "$ACTUAL_JAR_SHA256" = "$PINNED_JAR_SHA256" ] || {
    echo "FAIL: TLC jar digest mismatch: $ACTUAL_JAR_SHA256" >&2
    exit 2
}

ROW_TIMEOUT_SECONDS=${ROW_TIMEOUT_SECONDS:-120}
case "$ROW_TIMEOUT_SECONDS" in
    ''|*[!0-9]*|0) echo 'FAIL: ROW_TIMEOUT_SECONDS must be a positive integer' >&2; exit 2 ;;
esac

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
MODULE=Protocol50PipelineRecovery.tla
MODULE_PATH="$SCRIPT_DIR/$MODULE"
RESULTS="$TLC_STATE_ROOT/pipeline-recovery"
mkdir "$RESULTS"

run_row() {
    row=$1
    cfg=$2
    expected=$3
    module=${4:-$MODULE}
    log="$RESULTS/$row.log"
    state="$RESULTS/$row-states"
    module_path="$SCRIPT_DIR/$module"
    printf 'ROW %s\n' "$row"
    printf 'config_sha256=%s\n' "$(sha256sum "$SCRIPT_DIR/$cfg" | awk '{print $1}')"
    printf 'module_sha256=%s\n' "$(sha256sum "$module_path" | awk '{print $1}')"
    printf 'command=timeout --signal=TERM --kill-after=5s %ss java -Xmx2g -XX:+UseParallelGC -cp %s tlc2.TLC -workers 2 -metadir %s -config %s %s\n' \
        "$ROW_TIMEOUT_SECONDS" "$TLA2TOOLS_JAR" "$state" "$SCRIPT_DIR/$cfg" "$module_path"
    set +e
    timeout --signal=TERM --kill-after=5s "$ROW_TIMEOUT_SECONDS" \
        java -Xmx2g -XX:+UseParallelGC -cp "$TLA2TOOLS_JAR" tlc2.TLC \
        -workers 2 -metadir "$state" -config "$SCRIPT_DIR/$cfg" "$module_path" \
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

printf 'TLC version is recorded in each row log.\n'
printf 'jar_sha256=%s\nmodule_sha256=%s\n' "$ACTUAL_JAR_SHA256" \
    "$(sha256sum "$MODULE_PATH" | awk '{print $1}')"
printf 'workers=2 heap=2g row_timeout_seconds=%s state_root=%s\n' \
    "$ROW_TIMEOUT_SECONDS" "$RESULTS"

run_row topology-c2f1 Protocol50PipelineRecoveryC2F1.cfg clean
run_row topology-c3f1 Protocol50PipelineRecoveryC3F1.cfg clean
run_row topology-c4f1 Protocol50PipelineRecoveryC4F1.cfg clean
run_row topology-c1f2 Protocol50PipelineRecoveryC1F2.cfg clean
run_row topology-c1f3 Protocol50PipelineRecoveryC1F3.cfg clean
run_row topology-c1f4 Protocol50PipelineRecoveryC1F4.cfg clean
run_row recovery-loss-reset-fencing Protocol50PipelineRecoveryFaults.cfg clean
run_row witness-w2-third-job-refill Protocol50PipelineThirdJobRefillWitness.cfg ThirdJobRefillNotReached

run_row witness-w2-full-window Protocol50PipelineWindowWitnessC2F1.cfg FullWindowWitnessNotReached
run_row witness-w2-independent-c2f1 Protocol50PipelineIndependentLinksC2F1.cfg NoIndependentProgressWhileTargetFull
run_row witness-w2-independent-c1f2 Protocol50PipelineIndependentLinksC1F2.cfg NoIndependentProgressWhileTargetFull

run_row mutant-last-receipt Protocol50PipelineLastReceiptMutant.cfg RecoveryResponseContainsAllReceipts
run_row mutant-wrong-job-receipt Protocol50PipelineWrongJobReceiptMutant.cfg RecoveryResponseIdentityExact
run_row mutant-ack-beyond-k Protocol50PipelineAckBeyondKMutant.cfg CursorAndWindowBounds
run_row mutant-stale-worker Protocol50PipelineStaleWorkerMutant.cfg StaleWorkerCannotPublish
run_row mutant-reset-retry Protocol50NonIdempotentResetMutant.cfg ResetOperationIdempotent
run_row mutant-cancel-hole Protocol50PipelineCancelHoleMutant.cfg NoOrdinalHoleThroughPrepared

run_row accounting-w30 Protocol50PipelineWindowAccountingW30.cfg clean Protocol50PipelineWindowAccounting.tla
run_row witness-w30-full-window Protocol50PipelineWindowWitnessW30.cfg FullWindowNotReached Protocol50PipelineWindowAccounting.tla

printf 'PIPELINE-RECOVERY-TLC PASS rows=19\n'
