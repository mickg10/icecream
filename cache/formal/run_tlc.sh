#!/bin/sh
set -eu

: "${TLA2TOOLS_JAR:?set TLA2TOOLS_JAR to tla2tools.jar}"
TLC_WORKERS=${TLC_WORKERS:-1}
TLC_STATE_ROOT=${TLC_STATE_ROOT:-/tmp/icecream-p50-tlc}
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TLC_MAIN="java -cp $TLA2TOOLS_JAR tlc2.TLC -workers $TLC_WORKERS"

run_pass() {
    name=$1
    module=$2
    config=$3
    state_dir="$TLC_STATE_ROOT/$name"
    log="$TLC_STATE_ROOT/$name.log"
    rm -rf "$state_dir"
    mkdir -p "$state_dir"
    echo "== $name =="
    (cd "$SCRIPT_DIR" &&
        $TLC_MAIN -metadir "$state_dir" -config "$config" "$module") \
        2>&1 | tee "$log"
}

run_expected_failure() {
    name=$1
    module=$2
    config=$3
    invariant=$4
    state_dir="$TLC_STATE_ROOT/$name"
    log="$TLC_STATE_ROOT/$name.log"
    rm -rf "$state_dir"
    mkdir -p "$state_dir"
    echo "== $name (expected invariant failure: $invariant) =="
    set +e
    (cd "$SCRIPT_DIR" &&
        $TLC_MAIN -metadir "$state_dir" -config "$config" "$module") \
        >"$log" 2>&1
    rc=$?
    set -e
    cat "$log"
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
run_pass cache Protocol50.tla Protocol50.cfg
run_pass job Protocol50JobLifecycle.tla Protocol50JobLifecycle.cfg
run_pass incarnation Protocol50IncarnationBridge.tla Protocol50IncarnationBridge.cfg
run_pass incarnation-progress Protocol50IncarnationBridge.tla Protocol50IncarnationProgress.cfg
run_expected_failure abort-mutant Protocol50.tla Protocol50AbortMutant.cfg \
    CommitReconciliationWitness
run_expected_failure relseq-mutant Protocol50.tla Protocol50RelSeqMutant.cfg \
    ActiveSequenceHasRoom
run_expected_failure operation-digest-mutant Protocol50.tla \
    Protocol50OperationDigestMutant.cfg CommitOnlyAfterExactMaterialization
run_expected_failure job-lease-mutant Protocol50JobLifecycle.tla \
    Protocol50JobLeaseMutant.cfg CommittedInputForOpenJobKeepsLease
run_expected_failure incarnation-mutant Protocol50IncarnationBridge.tla \
    Protocol50IncarnationMutant.cfg ReplacementPreservesRetryIdentity
