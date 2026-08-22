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
    kind=${4:-safety}
    state_dir="$TLC_STATE_ROOT/$name"
    log="$TLC_STATE_ROOT/$name.log"
    rm -rf "$state_dir"
    mkdir -p "$state_dir"
    echo "== $name =="

    set +e
    (cd "$SCRIPT_DIR" &&
        $TLC_MAIN -metadir "$state_dir" -config "$config" "$module") \
        >"$log" 2>&1
    rc=$?
    set -e
    cat "$log"

    if [ "$rc" -ne 0 ]; then
        echo "$name failed with TLC exit status $rc" >&2
        exit 1
    fi
    grep -F "Model checking completed. No error" "$log" >/dev/null || {
        echo "$name returned zero without TLC's no-error completion marker" >&2
        exit 1
    }
    if [ "$kind" = progress ] &&
       grep -F "Temporal properties were violated" "$log" >/dev/null; then
        echo "$name reported a temporal-property violation" >&2
        exit 1
    fi
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
run_pass reconnect Protocol50Reconnect.tla Protocol50Reconnect.cfg
run_pass job-restart-progress Protocol50JobLifecycle.tla \
    Protocol50JobRestartProgress.cfg progress
run_pass incarnation Protocol50IncarnationBridge.tla Protocol50IncarnationBridge.cfg
run_pass incarnation-progress Protocol50IncarnationBridge.tla \
    Protocol50IncarnationProgress.cfg progress

run_expected_failure abort-mutant Protocol50.tla Protocol50AbortMutant.cfg \
    CommitReconciliationWitness
run_expected_failure relseq-mutant Protocol50.tla Protocol50RelSeqMutant.cfg \
    ActiveSequenceHasRoom
run_expected_failure operation-digest-mutant Protocol50.tla \
    Protocol50OperationDigestMutant.cfg CommitOnlyAfterExactMaterialization
run_expected_failure job-lease-mutant Protocol50JobLifecycle.tla \
    Protocol50JobLeaseMutant.cfg CommittedInputForOpenJobKeepsLease
run_expected_failure job-cancel-lease-mutant Protocol50JobLifecycle.tla \
    Protocol50JobCancelLeaseMutant.cfg CommittedInputForOpenJobKeepsLease
run_expected_failure job-ownership-mutant Protocol50JobLifecycle.tla \
    Protocol50JobOwnershipMutant.cfg AuthorizedAttemptOwnsIndependentInput
run_expected_failure reconnect-same-guid-mutant Protocol50Reconnect.tla \
    Protocol50ReconnectSameGuidMutant.cfg ColdRetirementHasProof
run_expected_failure reconnect-active-reset-mutant Protocol50Reconnect.tla \
    Protocol50ReconnectActiveResetMutant.cfg UnresolvedActiveNotDiscarded
run_expected_failure reconnect-repeat-reset-mutant Protocol50Reconnect.tla \
    Protocol50ReconnectRepeatResetMutant.cfg AtMostOneResetPerSession
run_expected_failure incarnation-mutant Protocol50IncarnationBridge.tla \
    Protocol50IncarnationMutant.cfg ReplacementPreservesRetryIdentity
run_expected_failure incarnation-ownership-mutant Protocol50IncarnationBridge.tla \
    Protocol50IncarnationOwnershipMutant.cfg \
    AuthorizedCompilerOwnsIndependentInput
