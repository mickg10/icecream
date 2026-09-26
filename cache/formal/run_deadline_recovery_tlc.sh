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
MODULE=Protocol50DeadlineRecovery.tla
MODULE_PATH="$SCRIPT_DIR/$MODULE"
RESULTS="$TLC_STATE_ROOT/deadline-recovery"
mkdir "$RESULTS"

run_row() {
    row=$1 cfg=$2 expected_rc=$3 diagnostic=$4
    log="$RESULTS/$row.log" state="$RESULTS/$row-states"
    printf 'ROW %s\nconfig_sha256=%s\nmodule_sha256=%s\n' \
        "$row" "$(sha256sum "$SCRIPT_DIR/$cfg" | awk '{print $1}')" \
        "$(sha256sum "$MODULE_PATH" | awk '{print $1}')"
    printf 'command=timeout --signal=TERM --kill-after=5s %ss java -Xmx1g -XX:+UseParallelGC -cp %s tlc2.TLC -workers 2 -metadir %s -config %s %s\n' \
        "$ROW_TIMEOUT_SECONDS" "$TLA2TOOLS_JAR" "$state" \
        "$SCRIPT_DIR/$cfg" "$MODULE_PATH"
    set +e
    timeout --signal=TERM --kill-after=5s "$ROW_TIMEOUT_SECONDS" \
        java -Xmx1g -XX:+UseParallelGC -cp "$TLA2TOOLS_JAR" tlc2.TLC \
        -workers 2 -metadir "$state" -config "$SCRIPT_DIR/$cfg" "$MODULE_PATH" \
        >"$log" 2>&1
    rc=$?
    set -e
    if [ "$rc" -eq "$expected_rc" ] && grep -Fq "$diagnostic" "$log"; then
        printf 'PASS %s exit=%s log=%s\n' "$row" "$rc" "$log"
        return
    fi
    printf 'FAIL %s exit=%s expected=%s diagnostic=%s log=%s\n' \
        "$row" "$rc" "$expected_rc" "$diagnostic" "$log" >&2
    tail -40 "$log" >&2
    exit 1
}

run_row clean-c1f2-c-expiry Protocol50DeadlineRecoveryC1F2.cfg 0 \
    'Model checking completed. No error has been found.'
run_row clean-c1f3-c-expiry Protocol50DeadlineRecoveryC1F3.cfg 0 \
    'Model checking completed. No error has been found.'
run_row clean-c1f4-c-expiry Protocol50DeadlineRecoveryC1F4.cfg 0 \
    'Model checking completed. No error has been found.'
run_row clean-c2f1-c-expiry Protocol50DeadlineRecoveryC2F1.cfg 0 \
    'Model checking completed. No error has been found.'
run_row clean-c3f1-c-expiry Protocol50DeadlineRecoveryC3F1.cfg 0 \
    'Model checking completed. No error has been found.'
run_row clean-c4f1-c-expiry Protocol50DeadlineRecoveryC4F1.cfg 0 \
    'Model checking completed. No error has been found.'
run_row clean-c1f2-f-expiry Protocol50DeadlineRecoveryC1F2_Fexpiry.cfg 0 \
    'Model checking completed. No error has been found.'
run_row clean-c1f3-f-expiry Protocol50DeadlineRecoveryC1F3_Fexpiry.cfg 0 \
    'Model checking completed. No error has been found.'
run_row clean-c1f4-f-expiry Protocol50DeadlineRecoveryC1F4_Fexpiry.cfg 0 \
    'Model checking completed. No error has been found.'
run_row clean-c2f1-f-expiry Protocol50DeadlineRecoveryC2F1_Fexpiry.cfg 0 \
    'Model checking completed. No error has been found.'
run_row clean-c3f1-f-expiry Protocol50DeadlineRecoveryC3F1_Fexpiry.cfg 0 \
    'Model checking completed. No error has been found.'
run_row clean-c4f1-f-expiry Protocol50DeadlineRecoveryC4F1_Fexpiry.cfg 0 \
    'Model checking completed. No error has been found.'

run_row urgent-caller-expiry-admission Protocol50DeadlineRecoveryBoundedCaller.cfg 0 \
    'Model checking completed. No error has been found.'
run_row mutant-drop-trigger Protocol50DeadlineRecoveryDropNotification.cfg 13 \
    'Error: Temporal properties were violated.'
run_row mutant-disable-retirement Protocol50DeadlineRecoveryRetirementDisabled.cfg 13 \
    'Error: Temporal properties were violated.'
run_row mutant-renew-cleanup Protocol50DeadlineRecoveryRenewCleanupMutant.cfg 12 \
    'Error: Invariant CleanupNotRenewed is violated.'
run_row mutant-replay-expired Protocol50DeadlineRecoveryReplayExpiredMutant.cfg 12 \
    'Error: Invariant NoExpiredReplay is violated.'
run_row mutant-write-after-expiry Protocol50DeadlineRecoveryWriteAfterExpiryMutant.cfg 12 \
    'Error: Invariant NoExpiredReplay is violated.'
run_row mutant-gate-sibling Protocol50DeadlineRecoveryGateSiblingMutant.cfg 12 \
    'Error: Invariant SiblingProgressEnabled is violated.'

printf 'DEADLINE-RECOVERY-TLC PASS rows=19 jar_sha256=%s module_sha256=%s results=%s\n' \
    "$ACTUAL_JAR_SHA256" "$(sha256sum "$MODULE_PATH" | awk '{print $1}')" "$RESULTS"
