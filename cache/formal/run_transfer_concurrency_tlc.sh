#!/bin/sh
set -eu

: "${TLA2TOOLS_JAR:?set TLA2TOOLS_JAR to the pinned tla2tools.jar}"
: "${TLC_STATE_ROOT:?set TLC_STATE_ROOT to a unique scratch directory}"
case "$TLA2TOOLS_JAR" in /*) ;; *) echo "TLA2TOOLS_JAR must be absolute" >&2; exit 2 ;; esac
case "$TLC_STATE_ROOT" in /*) ;; *) echo "TLC_STATE_ROOT must be absolute" >&2; exit 2 ;; esac
case "$TLC_STATE_ROOT" in /tmp|/tmp/*) echo "TLC_STATE_ROOT must use the configured scratch root" >&2; exit 2 ;; esac
[ -d "$TLC_STATE_ROOT" ] || { echo "TLC_STATE_ROOT must already exist" >&2; exit 2; }
[ ! -L "$TLC_STATE_ROOT" ] || { echo "TLC_STATE_ROOT must not be a symlink" >&2; exit 2; }

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
MODULE=Protocol50TransferConcurrency.tla
ROW_TIMEOUT_SECONDS=${ROW_TIMEOUT_SECONDS:-120}
case "$ROW_TIMEOUT_SECONDS" in
    ''|*[!0-9]*) echo "ROW_TIMEOUT_SECONDS must be an integer in 1..300" >&2; exit 2 ;;
esac
[ "$ROW_TIMEOUT_SECONDS" -ge 1 ] && [ "$ROW_TIMEOUT_SECONDS" -le 300 ] || {
    echo "ROW_TIMEOUT_SECONDS must be an integer in 1..300" >&2
    exit 2
}
[ "$(sha256sum "$TLA2TOOLS_JAR" | awk '{print $1}')" = \
  "936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88" ] || {
    echo "TLA2TOOLS_JAR does not match the pinned tool SHA-256" >&2
    exit 2
}
TLC="$TLC_STATE_ROOT/transfer-concurrency"
mkdir -p "$TLC"

# This explicit selected-input list is also consumed by aggregate preflight.
SELECTED_CONFIGS="Protocol50TransferConcurrencyC2F1.cfg Protocol50TransferConcurrencyC3F1.cfg Protocol50TransferConcurrencyC4F1.cfg Protocol50TransferConcurrencyC1F2.cfg Protocol50TransferConcurrencyC1F3.cfg Protocol50TransferConcurrencyC1F4.cfg Protocol50TransferConcurrencyC2F1Overlap.cfg Protocol50TransferConcurrencyC3F1Overlap.cfg Protocol50TransferConcurrencyC4F1Overlap.cfg Protocol50TransferConcurrencyC1F2Overlap.cfg Protocol50TransferConcurrencyC1F3Overlap.cfg Protocol50TransferConcurrencyC1F4Overlap.cfg Protocol50TransferConcurrencyC1F2Progress.cfg Protocol50TransferConcurrencyByteCap.cfg Protocol50TransferConcurrencyGenerationReplacement.cfg Protocol50TransferConcurrencyGlobalGateMutant.cfg Protocol50TransferConcurrencyProfileKeyMutant.cfg Protocol50TransferConcurrencyStalePublishMutant.cfg Protocol50TransferConcurrencyWrongCreditMutant.cfg Protocol50TransferConcurrencyDuplicateReleaseMutant.cfg Protocol50TransferConcurrencyLostAckMutant.cfg"

print_identity() {
    config=$1
    state=$2
    echo "tla2tools.sha256: $(sha256sum "$TLA2TOOLS_JAR" | awk '{print $1}')"
    echo "module.sha256: $(sha256sum "$SCRIPT_DIR/$MODULE" | awk '{print $1}')"
    echo "config.sha256: $(sha256sum "$SCRIPT_DIR/$config" | awk '{print $1}')"
    java -version 2>&1 | sed 's/^/java: /'
    echo "command: (cd $SCRIPT_DIR && timeout --signal=TERM --kill-after=5s $ROW_TIMEOUT_SECONDS java -Xmx2g -XX:+UseParallelGC -cp $TLA2TOOLS_JAR tlc2.TLC -workers 2 -metadir $state -config $config $MODULE)"
}

print_summary() {
    log=$1
    grep -E "Model checking completed|states generated|distinct states found|Finished in" "$log" | tail -4 || true
}

run_clean() {
    name=$1
    config=$2
    property=${3:-}
    state="$TLC/$name"
    log="$TLC/$name.log"
    [ ! -e "$state" ] && [ ! -e "$log" ] || {
        echo "refusing to reuse formal row state: $name" >&2
        exit 2
    }
    mkdir -p "$state"
    echo "== transfer-concurrency: $name =="
    print_identity "$config" "$state"
    started=$(date +%s)
    set +e
    (cd "$SCRIPT_DIR" && timeout --signal=TERM --kill-after=5s "$ROW_TIMEOUT_SECONDS" \
        java -Xmx2g -XX:+UseParallelGC -cp "$TLA2TOOLS_JAR" tlc2.TLC \
        -workers 2 -metadir "$state" -config "$config" "$MODULE") >"$log" 2>&1
    rc=$?
    set -e
    elapsed=$(($(date +%s) - started))
    if [ "$rc" -ne 0 ] || ! grep -F "Model checking completed. No error has been found." "$log" >/dev/null; then
        cat "$log"
        echo "$name failed clean completion (exit $rc)" >&2
        exit 1
    fi
    if grep -F "Error:" "$log" >/dev/null; then
        echo "$name emitted an error despite its completion marker" >&2
        exit 1
    fi
    if [ -n "$property" ] && ! grep -F "Finished checking temporal properties" "$log" >/dev/null; then
        echo "$name did not complete temporal-property checking" >&2
        exit 1
    fi
    print_summary "$log"
    echo "row-result: PASS exit=$rc elapsed_seconds=$elapsed"
}

run_expected_invariant() {
    name=$1
    config=$2
    invariant=$3
    state="$TLC/$name"
    log="$TLC/$name.log"
    [ ! -e "$state" ] && [ ! -e "$log" ] || {
        echo "refusing to reuse formal row state: $name" >&2
        exit 2
    }
    mkdir -p "$state"
    echo "== transfer-concurrency: $name (expected invariant $invariant) =="
    print_identity "$config" "$state"
    started=$(date +%s)
    set +e
    (cd "$SCRIPT_DIR" && timeout --signal=TERM --kill-after=5s "$ROW_TIMEOUT_SECONDS" \
        java -Xmx2g -XX:+UseParallelGC -cp "$TLA2TOOLS_JAR" tlc2.TLC \
        -workers 2 -metadir "$state" -config "$config" "$MODULE") >"$log" 2>&1
    rc=$?
    set -e
    elapsed=$(($(date +%s) - started))
    if [ "$rc" -eq 0 ] || [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ] ||
       ! grep -F "Invariant $invariant is violated." "$log" >/dev/null ||
       grep -E "Semantic errors:|Parsing or semantic analysis failed|TLC threw an unexpected exception|Temporal properties were violated" "$log" >/dev/null; then
        echo "$name did not fail through the expected invariant $invariant (exit $rc)" >&2
        exit 1
    fi
    grep -F "Invariant $invariant is violated." "$log"
    print_summary "$log"
    echo "row-result: PASS expected_invariant=$invariant exit=$rc elapsed_seconds=$elapsed"
}

run_expected_temporal() {
    name=$1
    config=$2
    property=$3
    state="$TLC/$name"
    log="$TLC/$name.log"
    [ ! -e "$state" ] && [ ! -e "$log" ] || {
        echo "refusing to reuse formal row state: $name" >&2
        exit 2
    }
    grep -Fx "    $property" "$SCRIPT_DIR/$config" >/dev/null || {
        echo "$config does not select the exact expected temporal property $property" >&2
        exit 2
    }
    mkdir -p "$state"
    echo "== transfer-concurrency: $name (expected temporal violation $property) =="
    print_identity "$config" "$state"
    started=$(date +%s)
    set +e
    (cd "$SCRIPT_DIR" && timeout --signal=TERM --kill-after=5s "$ROW_TIMEOUT_SECONDS" \
        java -Xmx2g -XX:+UseParallelGC -cp "$TLA2TOOLS_JAR" tlc2.TLC \
        -workers 2 -metadir "$state" -config "$config" "$MODULE") >"$log" 2>&1
    rc=$?
    set -e
    elapsed=$(($(date +%s) - started))
    if [ "$rc" -eq 0 ] || [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ] ||
       ! grep -F "Temporal properties were violated." "$log" >/dev/null ||
       grep -E "Invariant .* is violated|Semantic errors:|Parsing or semantic analysis failed|TLC threw an unexpected exception" "$log" >/dev/null; then
        echo "$name did not fail through the selected temporal property $property (exit $rc)" >&2
        exit 1
    fi
    grep -F "Temporal properties were violated." "$log"
    print_summary "$log"
    echo "row-result: PASS expected_temporal=$property exit=$rc elapsed_seconds=$elapsed"
}

for topology in C2F1 C3F1 C4F1 C1F2 C1F3 C1F4; do
    run_clean "safety-$topology" "Protocol50TransferConcurrency$topology.cfg"
done
for topology in C2F1 C3F1 C4F1 C1F2 C1F3 C1F4; do
    run_expected_invariant "overlap-$topology" \
        "Protocol50TransferConcurrency${topology}Overlap.cfg" NoAllLinksRunning
done

run_clean blocked-progress Protocol50TransferConcurrencyC1F2Progress.cfg temporal
run_clean byte-cap-pressure Protocol50TransferConcurrencyByteCap.cfg
run_clean generation-replacement Protocol50TransferConcurrencyGenerationReplacement.cfg
run_expected_temporal global-serialization-mutant \
    Protocol50TransferConcurrencyGlobalGateMutant.cfg BlockedLinkDoesNotBlockHealthyProgress
run_expected_invariant profile-in-key-mutant \
    Protocol50TransferConcurrencyProfileKeyMutant.cfg OneOwnerPerExactRelationship
run_expected_invariant stale-publication-mutant \
    Protocol50TransferConcurrencyStalePublishMutant.cfg OwnerPublicationMatchesGeneration
run_expected_invariant stale-credit-release-mutant \
    Protocol50TransferConcurrencyWrongCreditMutant.cfg ReservationCountsMatchOwnedOperations
run_expected_invariant duplicate-release-mutant \
    Protocol50TransferConcurrencyDuplicateReleaseMutant.cfg ReservationReleasedAtMostOnce
run_expected_invariant lost-ack-early-release-mutant \
    Protocol50TransferConcurrencyLostAckMutant.cfg ReservationCountsMatchOwnedOperations
