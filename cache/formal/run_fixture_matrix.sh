#!/bin/sh
# run_fixture_matrix.sh -- runs run_trace_refinement.sh over every fixture
# in fixtures/ and asserts its exit code against the expected table below,
# so the false-green class local-oracle found on c384cc53/0a47a6f5 (a trace
# check_trace.py rejects but a Level-2-only driver silently accepted) is
# CI-runnable evidence, not something only re-derivable by hand. Also runs
# a layer-independence proof (see layer_independence_proof below) for each
# fixture built specifically to reproduce one of those false-greens:
# red-c-cursor.jsonl (c384cc53 -- TX_BEGIN_C's dropped nonce/rel_seq) and
# red-f-session-unknown.jsonl/red-f-stale-token.jsonl (0a47a6f5 -- the
# dropped F-side session_serial, local-oracle's and BigOracle's two
# independent HOLDs on the same defect: an UNMAPPABLE serial that was
# never established anywhere, and a MAPPABLE-but-STALE serial that was
# established earlier but is no longer the live one).
# Needs TLA2TOOLS_JAR set (same requirement as run_trace_refinement.sh);
# not wired into `make check` for the same reason run_tlc.sh isn't (java +
# the pinned tla2tools.jar are a manual/local-run dependency -- see
# cache/formal/README.md's "Running TLC").
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
fixtures="green:0 prefix-legal:0 red-swap:1 red-wrong-tu:1 red-unknown-action:1 red-duplicate-dict:1 red-c-cursor:1 red-f-session-unknown:1 red-f-stale-token:1"

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

# layer_independence_proof NAME L1_MESSAGE L2_MECHANISM
#
# Proves a red fixture built to reproduce one specific false-green is
# killed by Level 1 and Level 2 INDEPENDENTLY of each other -- the fixture
# loop above always runs Level 1 (check_trace.py) first, so by itself it
# only proves Level 1 catches the trace, which was already true before
# whichever fix this fixture targets and is NOT the thing that was broken
# (the false-green was always Level 2 -- trace_to_tla.py/TLC -- silently
# accepting what Level 1 already rejects).
#   NAME        -- fixture stem under fixtures/ (NAME.jsonl)
#   L1_MESSAGE  -- exact substring check_trace.py's ValueError must contain
#   L2_MECHANISM -- "generator" or "tlc": which Level-2 stage this fixture
#                  is expected to be rejected by once Level 1 is bypassed.
#                  Both are legitimate independent Level-2 outcomes (see
#                  trace_to_tla.py's module docstring: an unrepresentable
#                  field fails the generator closed; a representable-but-
#                  wrong one reaches TLC as a deadlock/invariant violation)
#                  -- which one applies is a property of THIS fixture's
#                  specific mutation, not a free choice.
layer_independence_proof() {
    name=$1
    l1_message=$2
    l2_mechanism=$3
    trace="$FIXTURES_DIR/$name.jsonl"
    [ -f "$trace" ] || { echo "FAIL: layer-independence: fixture missing: $trace" >&2; fail=1; return; }

    # (i) Level 1 alone: check_trace.py directly, not through the driver.
    # Invoked via $PYTHON (not executed directly) so this measures
    # check_trace.py's own validation exit code, not the shell's ability
    # to exec the file (it is not marked +x -- run_trace_refinement.sh
    # invokes it the same indirect way).
    l1_out="$WORK_ROOT/layer-independence-$name-l1.log"
    set +e
    "$PYTHON" "$SCRIPT_DIR/check_trace.py" "$trace" >"$l1_out" 2>&1
    l1_rc=$?
    set -e
    if [ "$l1_rc" -ne 1 ]; then
        echo "FAIL: layer-independence (i): check_trace.py exited $l1_rc on $name.jsonl standalone (expected exit 1)" >&2
        tail -20 "$l1_out" >&2
        fail=1
    elif ! grep -qF "$l1_message" "$l1_out"; then
        echo "FAIL: layer-independence (i): check_trace.py exited 1 on $name.jsonl but not for the expected reason ('$l1_message'):" >&2
        tail -20 "$l1_out" >&2
        fail=1
    else
        echo "ok - layer-independence (i): check_trace.py rejects $name.jsonl standalone (exit $l1_rc)"
    fi

    # (ii) Level 2 alone: TRACEREF_SKIP_L1=1 bypasses check_trace.py inside
    # the driver (test-only -- see run_trace_refinement.sh's header) so
    # only trace_to_tla.py/TLC can be why this still fails; if neither
    # independently caught the bad field, this would exit 0. For the "tlc"
    # mechanism this also asserts TLC's own exit code is specifically 11
    # (its deadlock code, distinct from e.g. an invariant violation or a
    # parse error) and greps the raw TLC transcript for its own "Error:
    # Deadlock reached" line, not just the driver's generic wrapper message
    # -- a nonzero exit alone would not distinguish "deadlocked on the
    # bound field" from "rejected for some unrelated reason".
    l2_out="$WORK_ROOT/layer-independence-$name-l2.log"
    set +e
    TRACEREF_SKIP_L1=1 "$SCRIPT_DIR/run_trace_refinement.sh" "$trace" "$WORK_ROOT/layer-independence-$name-l2" >"$l2_out" 2>&1
    l2_rc=$?
    set -e
    case "$l2_mechanism" in
        generator) l2_expect='trace_to_tla.py failed closed on' ;;
        tlc) l2_expect='run_trace_refinement.sh: TLC rejected' ;;
        *) echo "layer_independence_proof: bad L2_MECHANISM $l2_mechanism" >&2; exit 2 ;;
    esac
    if [ "$l2_rc" -eq 0 ]; then
        echo "FAIL: layer-independence (ii): TRACEREF_SKIP_L1=1 run of $name.jsonl exited 0 -- Level 2 did not independently catch it" >&2
        tail -20 "$l2_out" >&2
        fail=1
    elif grep -qF 'check_trace.py (Level 1) rejected' "$l2_out"; then
        echo "FAIL: layer-independence (ii): TRACEREF_SKIP_L1=1 did not actually bypass Level 1 for $name.jsonl" >&2
        fail=1
    elif ! grep -qF "$l2_expect" "$l2_out"; then
        echo "FAIL: layer-independence (ii): $name.jsonl was rejected some other way (not $l2_mechanism) with Level 1 bypassed:" >&2
        tail -20 "$l2_out" >&2
        fail=1
    elif [ "$l2_mechanism" = "tlc" ] && ! grep -qF '(exit 11)' "$l2_out"; then
        echo "FAIL: layer-independence (ii): $name.jsonl was rejected by TLC but not with its deadlock exit code (11):" >&2
        tail -20 "$l2_out" >&2
        fail=1
    elif [ "$l2_mechanism" = "tlc" ] && ! grep -qF 'Error: Deadlock reached' "$l2_out"; then
        echo "FAIL: layer-independence (ii): $name.jsonl's TLC transcript does not show its own deadlock line:" >&2
        tail -20 "$l2_out" >&2
        fail=1
    elif [ "$l2_mechanism" = "generator" ] && grep -qF 'Deadlock' "$l2_out"; then
        echo "FAIL: layer-independence (ii): $name.jsonl reached TLC (deadlock text present) -- expected generator mappability to fail closed BEFORE TLC ever ran:" >&2
        tail -20 "$l2_out" >&2
        fail=1
    else
        echo "ok - layer-independence (ii): $l2_mechanism alone rejects $name.jsonl with Level 1 bypassed (exit $l2_rc)"
    fi
}

# c384cc53: TX_BEGIN_C's dropped (nonce, rel_seq) -- representable values,
# just wrong ones, so Level 2 alone reaches TLC and deadlocks there.
layer_independence_proof red-c-cursor 'C TX_BEGIN missed its cursor' tlc

# 0a47a6f5, two independent HOLDs on the same dropped F-side session_serial,
# each needing its own fixture because they are rejected by DIFFERENT
# Level-2 mechanisms (local-oracle's and BigOracle's point: mappability and
# the CurrentSession conjunct are two separate things to prove killed):
#
# red-f-session-unknown.jsonl (local-oracle's exact discriminator: green
# line 4's session_serial 1->2) -- serial 2 was never established by any
# SESSION_OPENED/REPLACED anywhere in the trace, so it is UNMAPPABLE, not
# merely wrong; Level 2 alone rejects it via the generator's fail-closed
# lookup_token, before TLC ever runs.
layer_independence_proof red-f-session-unknown 'F TX_BEGIN at the wrong boundary' generator

# red-f-stale-token.jsonl (BigOracle's addition) -- SESSION_OPENED
# establishes serial 1 (Tok0), SESSION_REPLACED establishes serial 5
# (Tok1, now the live session), then a DICT_COMPLETE row claims the
# stale serial 1. Tok0 WAS established (mappable, so the generator does
# not fail closed) but is no longer live, so this is the row that proves
# the new CurrentSession(s, r.f, r.tok) conjunct itself, not just
# mappability: Level 2 alone reaches TLC and deadlocks there (TLC's own
# exit 11 and "Error: Deadlock reached" line, asserted above -- this is
# the ONLY fixture in this matrix that reaches the new conjunct at TLC
# rather than dying in the generator's mappability guard first).
layer_independence_proof red-f-stale-token 'DICT does not match F pending/current session' tlc

if [ "$fail" -ne 0 ]; then
    echo "FAIL: run_fixture_matrix -- one or more fixtures/proofs did not match their expected outcome" >&2
    exit 1
fi
count=$(printf '%s\n' $fixtures | wc -l | tr -d ' ')
echo "PASS: run_fixture_matrix -- all $count fixtures matched their expected exit code, and red-c-cursor.jsonl/red-f-session-unknown.jsonl/red-f-stale-token.jsonl are each proven killed by Level 1 and Level 2 independently"
