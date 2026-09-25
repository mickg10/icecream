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
MODULE=Protocol50ReplacementReplay.tla
RESULTS="$TLC_STATE_ROOT/pipeline-replacement"
mkdir "$RESULTS"

run_row() {
    row=$1
    cfg=$2
    expected=$3
    log="$RESULTS/$row.log"
    state="$RESULTS/$row-states"
    printf 'ROW %s\n' "$row"
    printf 'config_sha256=%s\n' "$(sha256sum "$SCRIPT_DIR/$cfg" | awk '{print $1}')"
    printf 'module_sha256=%s\n' "$(sha256sum "$SCRIPT_DIR/$MODULE" | awk '{print $1}')"
    set +e
    timeout --signal=TERM --kill-after=5s "$ROW_TIMEOUT_SECONDS" \
        java -Xmx2g -XX:+UseParallelGC -cp "$TLA2TOOLS_JAR" tlc2.TLC \
        -workers 2 -metadir "$state" -config "$SCRIPT_DIR/$cfg" \
        "$SCRIPT_DIR/$MODULE" >"$log" 2>&1
    rc=$?
    set -e
    if [ "$expected" = clean ]; then
        if [ "$rc" -eq 0 ] && grep -Fqx 'Model checking completed. No error has been found.' "$log"; then
            printf 'PASS %s exit=%s log=%s\n' "$row" "$rc" "$log"
            return
        fi
    elif [ "$expected" = witness ]; then
        diagnostic='Error: Invariant ReplacementUseWitnessNotReached is violated.'
        if [ "$rc" -ne 0 ] && [ "$rc" -ne 124 ] && [ "$rc" -ne 137 ] && \
           grep -Fqx "$diagnostic" "$log" && \
           grep -Fq 'ReplacementUseWitnessNotReached' "$log"; then
            printf 'REACHABILITY-WITNESS %s exit=%s log=%s\n' "$row" "$rc" "$log"
            return
        fi
    else
        diagnostic="Error: Invariant $expected is violated."
        if [ "$rc" -ne 0 ] && [ "$rc" -ne 124 ] && [ "$rc" -ne 137 ] && \
           grep -Fqx "$diagnostic" "$log"; then
            printf 'EXPECTED-COUNTEREXAMPLE %s invariant=%s exit=%s log=%s\n' \
                "$row" "$expected" "$rc" "$log"
            return
        fi
    fi
    printf 'FAIL %s exit=%s expected=%s log=%s\n' "$row" "$rc" "$expected" "$log" >&2
    tail -40 "$log" >&2
    exit 1
}

printf 'jar_sha256=%s module_sha256=%s timeout_seconds=%s\n' \
    "$ACTUAL_JAR_SHA256" "$(sha256sum "$SCRIPT_DIR/$MODULE" | awk '{print $1}')" \
    "$ROW_TIMEOUT_SECONDS"

for topology in C2F1 C3F1 C4F1 C1F2 C1F3 C1F4; do
    run_row "safety-$topology" "Protocol50Replacement$topology.cfg" clean
    run_row "witness-$topology" "Protocol50Replacement$topology"Witness.cfg witness
done

run_row mutant-reset-replay Protocol50ReplacementResetReplayMutant.cfg ResetReplayResultExact
run_row mutant-old-relationship Protocol50ReplacementOldOfferMutant.cfg OldRelationshipOfferRejected
run_row mutant-same-f-store-generation Protocol50ReplacementStoreGuidMutant.cfg SameFReplacementPreservesStore

printf 'PIPELINE-REPLACEMENT-TLC PASS safety=6 witness=6 mutants=3\n'
