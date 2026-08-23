#!/bin/sh
# run_fixture_matrix.sh -- runs run_trace_refinement.sh over every fixture
# in fixtures/ and asserts its exit code against the expected table below,
# so the false-green class local-oracle found on c384cc53 (a trace
# check_trace.py rejects but the old Level-2-only driver silently accepted)
# is CI-runnable evidence, not something only re-derivable by hand. Also
# runs a layer-independence proof for red-c-cursor.jsonl (the fixture that
# reproduces that exact false-green): with TRACEREF_SKIP_L1=1 bypassing
# check_trace.py, TLC must still reject it on the (now-fixed) cursor-bound
# C_TX_BEGIN arm alone, so the fix is proven real and not just shadowed by
# the Level-1 gate. Needs TLA2TOOLS_JAR set (same requirement as
# run_trace_refinement.sh); not wired into `make check` for the same
# reason run_tlc.sh isn't (java + the pinned tla2tools.jar are a
# manual/local-run dependency -- see cache/formal/README.md's "Running
# TLC").
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
FIXTURES_DIR="$SCRIPT_DIR/fixtures"
PYTHON=${PYTHON:-python3}
WORK_ROOT=${1:-${TMPDIR:-/tmp}/icecream-p50-fixture-matrix-$$}
rm -rf "$WORK_ROOT"
mkdir -p "$WORK_ROOT"

# fixture -> expected exit code. Every red fixture is expected exit 1
# regardless of WHICH layer catches it -- the matrix asserts the overall
# accept/reject verdict; per-fixture stage attribution (Level 1 vs Level 2)
# is recorded in the delivery report, not asserted here, since which layer
# catches a given red fixture is allowed to shift as Level-1/Level-2 both
# get stricter over time.
fixtures="green:0 prefix-legal:0 red-swap:1 red-wrong-tu:1 red-unknown-action:1 red-duplicate-dict:1 red-c-cursor:1"

fail=0
for entry in $fixtures; do
    name=${entry%%:*}
    expected=${entry##*:}
    trace="$FIXTURES_DIR/$name.jsonl"
    [ -f "$trace" ] || { echo "FAIL: fixture missing: $trace" >&2; fail=1; continue; }
    out="$WORK_ROOT/$name.log"
    set +e
    "$SCRIPT_DIR/run_trace_refinement.sh" "$trace" "$WORK_ROOT/$name" >"$out" 2>&1
    actual=$?
    set -e
    if [ "$actual" -eq "$expected" ]; then
        echo "ok - $name: exit $actual (expected $expected)"
    else
        echo "FAIL: $name: exit $actual, expected $expected" >&2
        tail -20 "$out" >&2
        fail=1
    fi
done

# Layer-independence proof for red-c-cursor.jsonl (local-oracle HOLD
# requirement on c384cc53): the fixture loop above always runs Level 1
# (check_trace.py) first, so by itself it only proves Level 1 catches this
# trace -- which was already true before the fix and is NOT the thing that
# was broken. The actual false-green was Level 2 (trace_to_tla.py + TLC)
# silently accepting it. So prove both layers reject it ON THEIR OWN:
CURSOR_TRACE="$FIXTURES_DIR/red-c-cursor.jsonl"
[ -f "$CURSOR_TRACE" ] || { echo "FAIL: layer-independence: fixture missing: $CURSOR_TRACE" >&2; fail=1; }

if [ -f "$CURSOR_TRACE" ]; then
    # (i) Level 1 alone: check_trace.py directly, not through the driver.
    # Invoked via $PYTHON (not executed directly) so this measures
    # check_trace.py's own validation exit code, not the shell's ability
    # to exec the file (it is not marked +x -- run_trace_refinement.sh
    # invokes it the same indirect way).
    l1_out="$WORK_ROOT/layer-independence-l1.log"
    set +e
    "$PYTHON" "$SCRIPT_DIR/check_trace.py" "$CURSOR_TRACE" >"$l1_out" 2>&1
    l1_rc=$?
    set -e
    if [ "$l1_rc" -ne 1 ]; then
        echo "FAIL: layer-independence (i): check_trace.py exited $l1_rc on red-c-cursor.jsonl standalone (expected exit 1)" >&2
        tail -20 "$l1_out" >&2
        fail=1
    elif ! grep -qF 'C TX_BEGIN missed its cursor' "$l1_out"; then
        echo "FAIL: layer-independence (i): check_trace.py exited 1 but not for the expected cursor reason:" >&2
        tail -20 "$l1_out" >&2
        fail=1
    else
        echo "ok - layer-independence (i): check_trace.py rejects red-c-cursor.jsonl standalone (exit $l1_rc, cursor mismatch)"
    fi

    # (ii) Level 2 alone: TRACEREF_SKIP_L1=1 bypasses check_trace.py inside
    # the driver (test-only -- see run_trace_refinement.sh's header) so
    # only trace_to_tla.py's cursor-bound C_TX_BEGIN arm and TLC's replay
    # of it can be why this fails; if they didn't independently catch the
    # bad cursor, this would exit 0.
    l2_out="$WORK_ROOT/layer-independence-l2.log"
    set +e
    TRACEREF_SKIP_L1=1 "$SCRIPT_DIR/run_trace_refinement.sh" "$CURSOR_TRACE" "$WORK_ROOT/layer-independence-l2" >"$l2_out" 2>&1
    l2_rc=$?
    set -e
    if [ "$l2_rc" -eq 0 ]; then
        echo "FAIL: layer-independence (ii): TRACEREF_SKIP_L1=1 run of red-c-cursor.jsonl exited 0 -- Level 2's cursor binding did not independently catch it" >&2
        tail -20 "$l2_out" >&2
        fail=1
    elif grep -qF 'check_trace.py (Level 1) rejected' "$l2_out"; then
        echo "FAIL: layer-independence (ii): TRACEREF_SKIP_L1=1 did not actually bypass Level 1" >&2
        fail=1
    elif ! grep -qF 'run_trace_refinement.sh: TLC rejected' "$l2_out"; then
        echo "FAIL: layer-independence (ii): red-c-cursor.jsonl was rejected some other way (not TLC) with Level 1 bypassed:" >&2
        tail -20 "$l2_out" >&2
        fail=1
    else
        echo "ok - layer-independence (ii): TLC alone rejects red-c-cursor.jsonl with Level 1 bypassed (exit $l2_rc)"
    fi
fi

if [ "$fail" -ne 0 ]; then
    echo "FAIL: run_fixture_matrix -- one or more fixtures/proofs did not match their expected outcome" >&2
    exit 1
fi
count=$(printf '%s\n' $fixtures | wc -l | tr -d ' ')
echo "PASS: run_fixture_matrix -- all $count fixtures matched their expected exit code, and red-c-cursor.jsonl is proven killed by Level 1 and Level 2 independently"
