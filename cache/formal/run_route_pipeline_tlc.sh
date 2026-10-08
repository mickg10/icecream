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
run_pass route-pipeline-k1 Protocol50RoutePipeline.tla Protocol50RoutePipeline.cfg
run_pass route-pipeline-k2 Protocol50RoutePipeline.tla Protocol50RoutePipelineK2.cfg
run_pass route-pipeline-k3 Protocol50RoutePipeline.tla Protocol50RoutePipelineK3.cfg
run_pass route-pipeline-progress Protocol50RoutePipeline.tla \
    Protocol50RoutePipelineProgress.cfg progress

run_expected_failure route-pipeline-early-activate-mutant Protocol50RoutePipeline.tla \
    Protocol50RoutePipelineM1.cfg ActiveIsNextExpected
run_expected_failure route-pipeline-skip-successor-mutant Protocol50RoutePipeline.tla \
    Protocol50RoutePipelineM2.cfg ActiveIsNextExpected
run_expected_failure route-pipeline-no-bulk-release-mutant Protocol50RoutePipeline.tla \
    Protocol50RoutePipelineM3.cfg ParkedCurrentEpochOnly
run_expected_failure route-pipeline-state-at-park-mutant Protocol50RoutePipeline.tla \
    Protocol50RoutePipelineM4.cfg StateSentNeverAheadOfCursor
run_expected_failure route-pipeline-retire-any-order-mutant Protocol50RoutePipeline.tla \
    Protocol50RoutePipelineM5.cfg DigestChainConsistency
run_expected_failure route-pipeline-successor-survives-fault-mutant \
    Protocol50RoutePipeline.tla \
    Protocol50RoutePipelineM6.cfg QueueCurrentEpochOnly
