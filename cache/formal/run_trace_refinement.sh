#!/bin/sh
# Level-2 Protocol-50 conformance driver: render a canonical JSONL trace as
# a TLA+ TraceSpec (trace_to_tla.py) that refines Protocol50.tla to exactly
# the observed sequence, then ask TLC to replay it. Exits 0 iff TLC accepts
# the behavior (a legal prefix of the model, ending anywhere the trace
# ends -- see trace_to_tla.py's module docstring). Any record trace_to_tla.py
# cannot map onto the bounded model's constants is a fail-closed nonzero
# exit before TLC ever runs; any mapped-but-illegal step is a TLC deadlock
# or invariant violation. Command shape mirrors run_tlc.sh.
#
# fixtures/ holds the two-sided evidence this tool was verified against:
# green.jsonl (the simplest complete legal scenario the model admits) plus
# red-swap.jsonl, red-wrong-tu.jsonl, red-unknown-action.jsonl,
# red-duplicate-dict.jsonl, and red-c-cursor.jsonl (one illegal mutation
# each, all TLC-rejected or generator-fail-closed) and prefix-legal.jsonl
# (a legal prefix, accepted -- see trace_to_tla.py's docstring for why a
# prefix need not reach any particular terminal action). Reproduce with
# e.g. `./run_trace_refinement.sh fixtures/green.jsonl` (needs
# TLA2TOOLS_JAR set).
#
# MANDATORY LEVEL-1 GATE (plan v11; local-oracle HOLD on c384cc53): every
# run first replays the trace through check_trace.py's own ordering rules
# and fails closed on any Level-1 rejection, before trace_to_tla.py or TLC
# ever run. This is not optional and has no bypass flag -- Level-2 (TLC
# replay) checks that the trace is an actual Protocol50.tla behavior once
# mapped onto the bounded model's constants, but per-record cursor fields
# that a Protocol50.tla action does not take as an explicit parameter (the
# clearest example being C_TX_BEGIN's nonce/rel_seq, which the model reads
# from its own current state rather than checking against the caller) can
# only be caught by re-deriving what check_trace.py already tracks. See
# trace_to_tla.py's module docstring for the full per-arm field-binding
# audit of which cursor fields Level-2 now binds explicitly and which
# arms don't need to (because their signature already forces the
# equality check).
set -eu

usage() {
    echo "usage: $0 <trace.jsonl> [output-dir]" >&2
    exit 2
}

[ $# -ge 1 ] || usage
TRACE=$1
[ -f "$TRACE" ] || { echo "run_trace_refinement.sh: no such file: $TRACE" >&2; exit 2; }

PYTHON=${PYTHON:-python3}
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TRACE_ABS_EARLY=$(CDPATH= cd -- "$(dirname -- "$TRACE")" && pwd)/$(basename -- "$TRACE")

echo "== check_trace.py (Level 1): $TRACE_ABS_EARLY =="
if ! "$PYTHON" "$SCRIPT_DIR/check_trace.py" "$TRACE_ABS_EARLY"; then
    echo "run_trace_refinement.sh: check_trace.py (Level 1) rejected $TRACE_ABS_EARLY -- Level 2 (trace_to_tla.py/TLC) did not run" >&2
    exit 1
fi

: "${TLA2TOOLS_JAR:?set TLA2TOOLS_JAR to tla2tools.jar}"
case "$TLA2TOOLS_JAR" in
    /*) ;;
    *) TLA2TOOLS_JAR=$(CDPATH= cd -- "$(dirname -- "$TLA2TOOLS_JAR")" && pwd)/$(basename -- "$TLA2TOOLS_JAR") ;;
esac
TLC_WORKERS=${TLC_WORKERS:-1}
TLC_MAIN="java -cp $TLA2TOOLS_JAR tlc2.TLC -workers $TLC_WORKERS"

TRACE_ABS=$TRACE_ABS_EARLY
TRACE_BASE=$(basename -- "$TRACE_ABS")
TRACE_STEM=${TRACE_BASE%.*}

if [ $# -ge 2 ]; then
    OUT_DIR=$2
else
    OUT_DIR="${TMPDIR:-/tmp}/icecream-p50-trace-refinement-$$"
fi
STATE_DIR="$OUT_DIR/tlc-state"
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR" "$STATE_DIR"

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

echo "== trace_to_tla: $TRACE_ABS =="
if ! "$PYTHON" "$SCRIPT_DIR/trace_to_tla.py" "$TRACE_ABS" --out "$OUT_DIR"; then
    echo "run_trace_refinement.sh: trace_to_tla.py failed closed on $TRACE_ABS" >&2
    exit 1
fi

MODULE="TraceSpec_$(printf '%s' "$TRACE_STEM" | sed 's/[^A-Za-z0-9_]/_/g')"
case "$MODULE" in
    TraceSpec_[A-Za-z]*) ;;
    *) MODULE="TraceSpec_T_${MODULE#TraceSpec_}" ;;
esac
TLA_MODULE="$OUT_DIR/$MODULE.tla"
CFG_FILE="$OUT_DIR/$MODULE.cfg"
[ -f "$TLA_MODULE" ] && [ -f "$CFG_FILE" ] || {
    echo "run_trace_refinement.sh: trace_to_tla.py did not produce $MODULE.tla/.cfg" >&2
    exit 1
}
cp "$SCRIPT_DIR/Protocol50.tla" "$OUT_DIR/Protocol50.tla"

echo "== TLC: $MODULE =="
echo "identity:"
sha256_file "$TLA2TOOLS_JAR"
sha256_file "$SCRIPT_DIR/Protocol50.tla"
sha256_file "$TLA_MODULE"
sha256_file "$CFG_FILE"
java -version 2>&1 | sed 's/^/java: /'
echo "command: $TLC_MAIN -metadir $STATE_DIR -config $MODULE.cfg $MODULE.tla"

LOG="$OUT_DIR/tlc.log"
set +e
(cd "$OUT_DIR" && $TLC_MAIN -metadir "$STATE_DIR" -config "$MODULE.cfg" "$MODULE.tla") >"$LOG" 2>&1
rc=$?
set -e
cat "$LOG"
sha256_file "$LOG"

if [ "$rc" -ne 0 ]; then
    echo "run_trace_refinement.sh: TLC rejected $TRACE_ABS (exit $rc)" >&2
    exit 1
fi
grep -F "Model checking completed. No error" "$LOG" >/dev/null || {
    echo "run_trace_refinement.sh: TLC returned zero without its no-error completion marker" >&2
    exit 1
}

echo "run_trace_refinement.sh: TLC accepted $TRACE_ABS as a Protocol50.tla behavior"
