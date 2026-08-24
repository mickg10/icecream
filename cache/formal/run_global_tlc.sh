#!/bin/sh
set -eu

: "${TLA2TOOLS_JAR:?set TLA2TOOLS_JAR to a pinned tla2tools.jar}"
case "$TLA2TOOLS_JAR" in
    /*) ;;
    *) TLA2TOOLS_JAR=$(CDPATH= cd -- "$(dirname -- "$TLA2TOOLS_JAR")" && pwd)/$(basename -- "$TLA2TOOLS_JAR") ;;
esac
[ -f "$TLA2TOOLS_JAR" ] || {
    echo "TLA2TOOLS_JAR is not a regular file: $TLA2TOOLS_JAR" >&2
    exit 2
}
command -v java >/dev/null 2>&1 || {
    echo "java is required for TLC" >&2
    exit 2
}

TLC_WORKERS=${TLC_WORKERS:-1}
TLC_STATE_ROOT=${TLC_STATE_ROOT:-/tmp/icecream-p50-global-tlc}
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
MODULE=Protocol50Global.tla
TLC_MAIN="java -cp $TLA2TOOLS_JAR tlc2.TLC -workers $TLC_WORKERS"

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1"
    else
        shasum -a 256 "$1"
    fi
}

run_pass() {
    name=$1
    config=$2
    state_dir="$TLC_STATE_ROOT/$name"
    log="$TLC_STATE_ROOT/$name.log"
    rm -rf "$state_dir"
    mkdir -p "$state_dir"
    echo "== global $name =="
    sha256_file "$TLA2TOOLS_JAR"
    sha256_file "$SCRIPT_DIR/$MODULE"
    sha256_file "$SCRIPT_DIR/$config"
    echo "command: $TLC_MAIN -metadir $state_dir -config $config $MODULE"
    set +e
    (cd "$SCRIPT_DIR" &&
        $TLC_MAIN -metadir "$state_dir" -config "$config" "$MODULE") \
        >"$log" 2>&1
    rc=$?
    set -e
    cat "$log"
    sha256_file "$log"
    if [ "$rc" -ne 0 ]; then
        echo "global $name failed with TLC exit status $rc" >&2
        exit 1
    fi
    grep -F "Model checking completed. No error" "$log" >/dev/null || {
        echo "global $name returned zero without TLC no-error marker" >&2
        exit 1
    }
}

run_expected_failure() {
    name=$1
    config=$2
    invariant=$3
    state_dir="$TLC_STATE_ROOT/$name"
    log="$TLC_STATE_ROOT/$name.log"
    rm -rf "$state_dir"
    mkdir -p "$state_dir"
    echo "== global $name (expected $invariant) =="
    set +e
    (cd "$SCRIPT_DIR" &&
        $TLC_MAIN -metadir "$state_dir" -config "$config" "$MODULE") \
        >"$log" 2>&1
    rc=$?
    set -e
    cat "$log"
    sha256_file "$log"
    if [ "$rc" -eq 0 ]; then
        echo "global $name unexpectedly passed" >&2
        exit 1
    fi
    grep -F "Invariant $invariant is violated" "$log" >/dev/null || {
        echo "global $name failed through the wrong invariant" >&2
        exit 1
    }
}

mkdir -p "$TLC_STATE_ROOT"
run_pass safety Protocol50Global.cfg
run_expected_failure aggregate Protocol50GlobalAggregateCapMutant.cfg NoMutantFaults
run_expected_failure namespace-cap Protocol50GlobalNamespaceCapMutant.cfg NoMutantFaults
run_expected_failure slot Protocol50GlobalSlotMutant.cfg InstallingOwnsExactlyOneSlot
run_expected_failure lru Protocol50GlobalLruMutant.cfg NoMutantFaults
run_expected_failure generation Protocol50GlobalGenerationWrapMutant.cfg NoMutantFaults
run_expected_failure admission Protocol50GlobalAdmissionMutant.cfg NoMutantFaults
run_expected_failure conflict Protocol50GlobalConflictMutant.cfg ConflictIsFatal
run_expected_failure crash Protocol50GlobalCrashMutant.cfg NoMutantFaults
run_expected_failure reverse-slot Protocol50GlobalReverseSlotMutant.cfg \
    EveryOwnedSlotHasInstallingArena
run_expected_failure content Protocol50GlobalContentMutant.cfg ImmutableArenaContent
run_expected_failure stall Protocol50GlobalStallMutant.cfg WatchdogNoStall

echo "run_global_tlc.sh: bounded global model and caught mutants passed"
