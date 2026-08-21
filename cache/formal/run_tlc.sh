#!/bin/sh
set -eu

: "${TLA2TOOLS_JAR:?set TLA2TOOLS_JAR to tla2tools.jar}"
JAVA_BIN=${JAVA_BIN:-java}
TLC_WORKERS=${TLC_WORKERS:-1}
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
STATE_ROOT=${TLC_STATE_ROOT:-"${TMPDIR:-/tmp}/icecream-p50-tlc"}

run_pass() {
    name=$1
    config=$2
    module=$3
    state_dir="$STATE_ROOT/$name"
    rm -rf "$state_dir"
    mkdir -p "$state_dir"
    echo "=== PASS expected: $name ==="
    "$JAVA_BIN" -XX:+UseParallelGC -jar "$TLA2TOOLS_JAR" \
        -workers "$TLC_WORKERS" \
        -metadir "$state_dir" \
        -config "$config" \
        "$module"
}

run_mutant() {
    name=$1
    config=$2
    module=$3
    invariant=$4
    state_dir="$STATE_ROOT/$name"
    log="$STATE_ROOT/$name.log"
    rm -rf "$state_dir"
    mkdir -p "$state_dir"
    echo "=== invariant failure expected: $name / $invariant ==="
    set +e
    "$JAVA_BIN" -XX:+UseParallelGC -jar "$TLA2TOOLS_JAR" \
        -workers "$TLC_WORKERS" \
        -metadir "$state_dir" \
        -config "$config" \
        "$module" >"$log" 2>&1
    rc=$?
    set -e
    cat "$log"
    if [ "$rc" -eq 0 ]; then
        echo "$name unexpectedly passed" >&2
        exit 1
    fi
    if ! grep -F "$invariant" "$log" >/dev/null 2>&1 ||
       ! grep -i 'violat' "$log" >/dev/null 2>&1; then
        echo "$name failed, but not through invariant $invariant" >&2
        exit 1
    fi
}

rm -rf "$STATE_ROOT"
mkdir -p "$STATE_ROOT"

run_pass core \
    "$ROOT/cache/formal/Protocol50.cfg" \
    "$ROOT/cache/formal/Protocol50.tla"
run_pass job \
    "$ROOT/cache/formal/Protocol50JobLifecycle.cfg" \
    "$ROOT/cache/formal/Protocol50JobLifecycle.tla"

run_mutant abort-after-commit \
    "$ROOT/cache/formal/Protocol50AbortMutant.cfg" \
    "$ROOT/cache/formal/Protocol50.tla" \
    CommitReconciliationWitness
run_mutant terminal-rel-seq \
    "$ROOT/cache/formal/Protocol50RelSeqMutant.cfg" \
    "$ROOT/cache/formal/Protocol50.tla" \
    ActiveSequenceHasRoom
run_mutant commit-without-input-lease \
    "$ROOT/cache/formal/Protocol50JobLeaseMutant.cfg" \
    "$ROOT/cache/formal/Protocol50JobLifecycle.tla" \
    CommittedInputForOpenJobKeepsLease
