#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PYTHON=${PYTHON:-python3}

"$SCRIPT_DIR/run_global_trace_gate.sh"
"$PYTHON" -m py_compile "$SCRIPT_DIR/check_global_trace.py"

for symbol in \
    NAMESPACE_ADMITTED NAMESPACE_EVICTED ARENA_INSTALLING \
    ARENA_PRESENT ARENA_PINNED INSTALL_CRASHED CONTENT_CONFLICT_FATAL \
    GENERATION_WRAP_STOPPED C_GUID_FLIPPED; do
    grep -F "$symbol" "$SCRIPT_DIR/check_global_trace.py" >/dev/null || {
        echo "missing Level-1 action binding: $symbol" >&2
        exit 1
    }
done

for invariant in \
    AggregateByteCap NamespaceByteCaps InstallingOwnsExactlyOneSlot \
    EveryOwnedSlotHasInstallingArena ArenaStateMachine WholeNamespaceEviction GenerationAdmissionStop \
    CrashMidInstallIsIdempotent ConflictIsFatal; do
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

if [ -n "${TLA2TOOLS_JAR:-}" ]; then
    "$SCRIPT_DIR/run_global_tlc.sh"
else
    echo "TLC: SKIP — exact dependency missing: TLA2TOOLS_JAR (pinned tla2tools.jar)"
fi

echo "run_global_formal_gate.sh: static/formal gate passed"
