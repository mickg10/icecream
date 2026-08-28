#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PYTHON=${PYTHON:-python3}

"$SCRIPT_DIR/run_global_trace_gate.sh"
"$PYTHON" -m py_compile "$SCRIPT_DIR/check_global_trace.py"

for symbol in \
    NAMESPACE_ADMITTED NAMESPACE_TOUCHED TU_STARTED TU_FINISHED \
    ARENA_INSTALLING ARENA_RETRY_INSTALLING ARENA_PRESENT \
    ARENA_PINNED ARENA_UNPINNED INSTALL_CRASHED CONTENT_CONFLICT_FATAL \
    NAMESPACE_EVICTED GENERATION_ADVANCED GENERATION_WRAP_STOPPED \
    C_GUID_FLIPPED; do
    grep -F "$symbol" "$SCRIPT_DIR/check_global_trace.py" >/dev/null || {
        echo "missing Level-1 action binding: $symbol" >&2
        exit 1
    }
done

for invariant in \
    AggregateByteCap NamespaceByteCaps InstallingOwnsExactlyOneSlot \
    EveryOwnedSlotHasInstallingArena ArenaStateMachine WholeNamespaceEviction GenerationAdmissionStop \
    CrashMidInstallIsIdempotent ConflictIsFatal LruOrderingWitness \
    FreshAdmissionReady CrossNamespaceGuidIsolation \
    CrossNamespaceGuidHistoryIsolation StagingByteCap \
    TotalSimultaneousByteCap; do
    grep -F "$invariant" "$SCRIPT_DIR/Protocol50Global.tla" >/dev/null || {
        echo "missing global invariant: $invariant" >&2
        exit 1
    }
done

for symbol in WatchdogLimit WriterWorkEnabled WATCHDOG_TICK WatchdogNoStall; do
    grep -F "$symbol" "$SCRIPT_DIR/Protocol50Global.tla" >/dev/null || {
        echo "missing executable watchdog binding: $symbol" >&2
        exit 1
    }
done

# The TLA/JSONL lane is a model witness only unless the production resource
# owner and its trace emitter are present in the candidate source.  Keep this
# census next to the existing action/invariant census so a model-only change
# cannot be reported as an S3 exit.
for symbol in GlobalResourceModel GlobalResourceTrace global_action_name \
    write_global_trace begin_install publish conflict evict_oldest free_staging_slots; do
    grep -F "$symbol" "$SCRIPT_DIR/../p50_slice0.h" "$SCRIPT_DIR/../p50_slice0.cpp" >/dev/null || {
        echo "missing production global-resource binding: $symbol" >&2
        exit 1
    }
done
grep -F "ignore_slot_ownership" "$SCRIPT_DIR/../p50_slice0.h" \
    "$SCRIPT_DIR/../../unittests/p50_slice0_test.cpp" >/dev/null || {
    echo "missing known-caught global slot-ownership control" >&2
    exit 1
}

if [ -n "${TLA2TOOLS_JAR:-}" ]; then
    "$SCRIPT_DIR/run_global_tlc.sh"
else
    echo "TLC: SKIP — exact dependency missing: TLA2TOOLS_JAR (pinned tla2tools.jar)"
fi

echo "run_global_formal_gate.sh: static/formal gate passed"

