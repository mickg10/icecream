#!/bin/sh
# run_fixture_matrix.sh -- runs run_trace_refinement.sh over every fixture
# in fixtures/ and asserts its exit code against the expected table below,
# so the false-green class local-oracle found on c384cc53 (a trace
# check_trace.py rejects but the old Level-2-only driver silently accepted)
# is CI-runnable evidence, not something only re-derivable by hand. Needs
# TLA2TOOLS_JAR set (same requirement as run_trace_refinement.sh); not
# wired into `make check` for the same reason run_tlc.sh isn't (java +
# the pinned tla2tools.jar are a manual/local-run dependency -- see
# cache/formal/README.md's "Running TLC").
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
FIXTURES_DIR="$SCRIPT_DIR/fixtures"
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

if [ "$fail" -ne 0 ]; then
    echo "FAIL: run_fixture_matrix -- one or more fixtures did not match their expected exit code" >&2
    exit 1
fi
count=$(printf '%s\n' $fixtures | wc -l | tr -d ' ')
echo "PASS: run_fixture_matrix -- all $count fixtures matched their expected exit code"
