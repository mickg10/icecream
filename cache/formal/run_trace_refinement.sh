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
# red-swap.jsonl, red-wrong-tu.jsonl, red-unknown-action.jsonl, and
# red-duplicate-dict.jsonl (one illegal mutation each, all TLC-rejected or
# generator-fail-closed) and prefix-legal.jsonl (a legal prefix, accepted --
# see trace_to_tla.py's docstring for why a prefix need not reach any
# particular terminal action). Reproduce with e.g.
# `./run_trace_refinement.sh fixtures/green.jsonl` (needs TLA2TOOLS_JAR set).
set -eu

usage() {
    echo "usage: $0 <trace.jsonl> [output-dir]" >&2
    exit 2
}

[ $# -ge 1 ] || usage
TRACE=$1
[ -f "$TRACE" ] || { echo "run_trace_refinement.sh: no such file: $TRACE" >&2; exit 2; }

: "${TLA2TOOLS_JAR:?set TLA2TOOLS_JAR to tla2tools.jar}"
case "$TLA2TOOLS_JAR" in
    /*) ;;
    *) TLA2TOOLS_JAR=$(CDPATH= cd -- "$(dirname -- "$TLA2TOOLS_JAR")" && pwd)/$(basename -- "$TLA2TOOLS_JAR") ;;
esac
PYTHON=${PYTHON:-python3}
TLC_WORKERS=${TLC_WORKERS:-1}
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TLC_MAIN="java -cp $TLA2TOOLS_JAR tlc2.TLC -workers $TLC_WORKERS"

TRACE_ABS=$(CDPATH= cd -- "$(dirname -- "$TRACE")" && pwd)/$(basename -- "$TRACE")
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
