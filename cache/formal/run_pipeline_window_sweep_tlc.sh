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

ROW_TIMEOUT_SECONDS=${ROW_TIMEOUT_SECONDS:-180}
case "$ROW_TIMEOUT_SECONDS" in
    ''|*[!0-9]*|0) echo 'FAIL: ROW_TIMEOUT_SECONDS must be a positive integer' >&2; exit 2 ;;
esac

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RESULTS="$TLC_STATE_ROOT/pipeline-window-sweep"
mkdir "$RESULTS"

run_row() {
    row=$1
    cfg=$2
    expected=$3
    module=${4:-Protocol50PipelineRecovery.tla}
    trace=${5:-none}
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
        if [ "$rc" -eq 12 ] && grep -Fqx "$diagnostic" "$log"; then
            case "$trace" in
                target-w3)
                    if ! awk '
                        /^State [0-9]+:/ { s = 0; a = 0; in_s = 0; in_a = 0 }
                        index($0, "/\\ S =") == 1 { in_s = 1; in_a = 0 }
                        index($0, "/\\ A =") == 1 { in_a = 1; in_s = 0 }
                        index($0, "/\\") == 1 && index($0, "/\\ S =") != 1 && index($0, "/\\ A =") != 1 { in_s = 0; in_a = 0 }
                        in_s && index($0, "<<C1, F1>> :> 3") { s = 1 }
                        in_a && index($0, "<<C1, F1>> :> 0") { a = 1 }
                        s && a { found = 1 }
                        END { exit !found }
                    ' "$log"; then
                        printf 'FAIL %s missing target S-A=3 trace log=%s\n' "$row" "$log" >&2
                        exit 1
                    fi
                    ;;
                accounting-w4|accounting-w8|accounting-w16|accounting-w30)
                    window=${trace#accounting-w}
                    if ! awk -v w="$window" '
                        /^State [0-9]+:/ { p = 0; a = 0 }
                        index($0, "/\\ A = 0") == 1 { a = 1 }
                        index($0, "/\\ P = ") == 1 && $0 ~ ("= " w "$" ) { p = 1 }
                        p && a { found = 1 }
                        END { exit !found }
                    ' "$log"; then
                        printf 'FAIL %s missing P-A=%s trace log=%s\n' "$row" "$window" "$log" >&2
                        exit 1
                    fi
                    ;;
                none) ;;
                *) printf 'FAIL %s unknown trace check %s\n' "$row" "$trace" >&2; exit 2 ;;
            esac
            printf 'EXPECTED-COUNTEREXAMPLE %s diagnostic=%s exit=%s log=%s\n' \
                "$row" "$expected" "$rc" "$log"
            return
        fi
    fi
    printf 'FAIL %s exit=%s expected=%s log=%s\n' "$row" "$rc" "$expected" "$log" >&2
    tail -40 "$log" >&2
    exit 1
}

printf 'jar_sha256=%s\n' "$ACTUAL_JAR_SHA256"
printf 'workers=2 heap=2g row_timeout_seconds=%s state_root=%s\n' \
    "$ROW_TIMEOUT_SECONDS" "$RESULTS"

for topology in C2F1 C3F1 C4F1 C1F2 C1F3 C1F4; do
    run_row "safety-w3-$topology" "Protocol50PipelineRecovery${topology}W3.cfg" clean
done
for topology in C2F1 C3F1 C4F1 C1F2 C1F3 C1F4; do
    run_row "witness-w3-$topology" "Protocol50PipelineRecovery${topology}W3Witness.cfg" FullWindowWitnessNotReached Protocol50PipelineRecovery.tla target-w3
done

for window in 4 8 16; do
    run_row "accounting-w$window" "Protocol50PipelineWindowAccountingW${window}.cfg" clean Protocol50PipelineWindowAccounting.tla
    run_row "witness-w$window" "Protocol50PipelineWindowWitnessW${window}.cfg" FullWindowNotReached Protocol50PipelineWindowAccounting.tla accounting-w$window
done

run_row accounting-w30 Protocol50PipelineWindowAccountingW30.cfg clean Protocol50PipelineWindowAccounting.tla
run_row witness-w30 Protocol50PipelineWindowWitnessW30.cfg FullWindowNotReached Protocol50PipelineWindowAccounting.tla accounting-w30

printf 'PIPELINE-WINDOW-SWEEP-TLC PASS rows=20\n'
