#!/bin/sh
set -eu

: "${TLA2TOOLS_JAR:?set TLA2TOOLS_JAR to tla2tools.jar}"
case "$TLA2TOOLS_JAR" in
    /*) ;;
    *) TLA2TOOLS_JAR=$(CDPATH= cd -- "$(dirname -- "$TLA2TOOLS_JAR")" && pwd)/$(basename -- "$TLA2TOOLS_JAR") ;;
esac
TLC_WORKERS=${TLC_WORKERS:-1}
TLC_STATE_ROOT=${TLC_STATE_ROOT:-/tmp/icecream-p50-tlc}
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

print_identity() {
    name=$1
    module=$2
    config=$3
    state_dir=$4
    echo "identity[$name]:"
    sha256_file "$TLA2TOOLS_JAR"
    sha256_file "$SCRIPT_DIR/$module"
    sha256_file "$SCRIPT_DIR/$config"
    java -version 2>&1 | sed 's/^/java: /'
    echo "command: $TLC_MAIN -metadir $state_dir -config $config $module"
}

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
    print_identity "$name" "$module" "$config" "$state_dir"

    set +e
    (cd "$SCRIPT_DIR" &&
        $TLC_MAIN -metadir "$state_dir" -config "$config" "$module") \
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
    print_identity "$name" "$module" "$config" "$state_dir"
    set +e
    (cd "$SCRIPT_DIR" &&
        $TLC_MAIN -metadir "$state_dir" -config "$config" "$module") \
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
run_pass cache Protocol50.tla Protocol50.cfg
run_pass job Protocol50JobLifecycle.tla Protocol50JobLifecycle.cfg
run_pass reconnect Protocol50Reconnect.tla Protocol50Reconnect.cfg
run_pass multiroute Protocol50MultiRoute.tla Protocol50MultiRoute.cfg
run_pass job-restart-progress Protocol50JobLifecycle.tla \
    Protocol50JobRestartProgress.cfg progress
run_pass incarnation Protocol50IncarnationBridge.tla Protocol50IncarnationBridge.cfg
run_pass incarnation-progress Protocol50IncarnationBridge.tla \
    Protocol50IncarnationProgress.cfg progress
run_pass assignment Protocol50Assignment.tla Protocol50Assignment.cfg
run_pass assignment-restart-progress Protocol50Assignment.tla \
    Protocol50AssignmentRestart.cfg progress
run_pass assignment-ordering Protocol50AssignmentOrdering.tla \
    Protocol50AssignmentOrdering.cfg

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
run_expected_failure multiroute-move-mutant Protocol50MultiRoute.tla \
    Protocol50MultiRouteMoveMutant.cfg CrossRouteForkPreservesSource
run_expected_failure multiroute-result-mutant Protocol50MultiRoute.tla \
    Protocol50MultiRouteResultMutant.cfg ResultDoesNotRetireCache
run_expected_failure incarnation-mutant Protocol50IncarnationBridge.tla \
    Protocol50IncarnationMutant.cfg ReplacementPreservesRetryIdentity
run_expected_failure incarnation-ownership-mutant Protocol50IncarnationBridge.tla \
    Protocol50IncarnationOwnershipMutant.cfg \
    AuthorizedCompilerOwnsIndependentInput
run_expected_failure assignment-mixed-compat Protocol50Assignment.tla \
    Protocol50AssignmentMixedCompat.cfg EnforcingCompatClaimsExact
run_expected_failure assignment-strict-legacy-mutant Protocol50Assignment.tla \
    Protocol50AssignmentStrictLegacyMutant.cfg StrictClaimsExact
run_expected_failure assignment-ready-mutant Protocol50Assignment.tla \
    Protocol50AssignmentReadyMutant.cfg ReadyGate
run_expected_failure assignment-cancelled-publish-mutant \
    Protocol50Assignment.tla \
    Protocol50AssignmentCancelledPublishMutant.cfg ReadyGate
run_expected_failure assignment-release-mutant Protocol50Assignment.tla \
    Protocol50AssignmentReleaseMutant.cfg ReleaseHasRevocationProof
run_expected_failure assignment-tombstone-mutant Protocol50Assignment.tla \
    Protocol50AssignmentTombstoneMutant.cfg TombstoneIsNotLive
run_expected_failure assignment-epoch-reuse-mutant Protocol50Assignment.tla \
    Protocol50AssignmentEpochReuseMutant.cfg SameEpochRevocationSafety
run_expected_failure assignment-ordering-early-cancel-witness \
    Protocol50AssignmentOrdering.tla \
    Protocol50AssignmentOrderingEarlyCancelWitness.cfg \
    NoEarlyCancelWitness
run_expected_failure assignment-ordering-advisory-witness \
    Protocol50AssignmentOrdering.tla \
    Protocol50AssignmentOrderingAdvisoryWitness.cfg \
    NoAdvisoryClaimBeforePrepareWitness
run_expected_failure assignment-ordering-prepare-mutant \
    Protocol50AssignmentOrdering.tla \
    Protocol50AssignmentOrderingPrepareMutant.cfg \
    ClaimNeverRegresses
run_expected_failure assignment-ordering-ready-mutant \
    Protocol50AssignmentOrdering.tla \
    Protocol50AssignmentOrderingReadyMutant.cfg \
    CancelCutsPublication
run_expected_failure assignment-ordering-revoke-mutant \
    Protocol50AssignmentOrdering.tla \
    Protocol50AssignmentOrderingRevokeMutant.cfg \
    ClaimWinsRevoke

# S3 is intentionally a separate global/cross-relationship model.  Keep its
# bounded rows in the canonical runner once the per-relationship suite has
# completed, so a formal pass cannot accidentally omit the model-first gate.
sh "$SCRIPT_DIR/run_global_tlc.sh"
