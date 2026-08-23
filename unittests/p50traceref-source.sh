#!/bin/sh
# Deletion-sensitive scope gate for the TLA Level-2 trace-refinement
# generator (trace_to_tla.py) and its driver (run_trace_refinement.sh).
#
# This does not run TLC (that needs java + tla2tools.jar and is exercised
# manually, like the rest of cache/formal/run_tlc.sh's callers -- see
# cache/formal/README.md's "Running TLC"). It instead anchors the
# fail-closed branches trace_to_tla.py must keep: unknown action names,
# missing identity fields, and every "cannot be mapped onto the bounded
# model's constant universe" cardinality overflow (a third F, C, session
# token, TU, or transaction-digest variant) must stay reachable, and the
# generated .cfg must keep CHECK_DEADLOCK TRUE -- without it, a trace that
# gets stuck partway through (the exact failure mode the RED tests exist to
# catch) would silently report success instead of a TLC deadlock.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}

require_count() {
    expected=$1
    pattern=$2
    file=$3
    label=$4
    actual=$(grep -F -c "$pattern" "$src/$file" || true)
    if [ "$actual" -ne "$expected" ]; then
        echo "FAIL: $label (expected $expected anchor(s), found $actual)" >&2
        exit 1
    fi
    echo "ok - $label"
}

require_absent() {
    pattern=$1
    shift
    label=$1
    shift
    if grep -E -n "$pattern" "$@" >/dev/null 2>&1; then
        echo "FAIL: $label" >&2
        grep -E -n "$pattern" "$@" >&2 || true
        exit 1
    fi
    echo "ok - $label"
}

GEN=cache/formal/trace_to_tla.py
DRIVER=cache/formal/run_trace_refinement.sh

# Unknown action / missing field -- the two most basic fail-closed gates.
require_count 1 'f"unknown action {action!r}"' "$GEN" \
    'an unrecognized action name fails closed'
require_count 1 'f"missing required field {field!r}"' "$GEN" \
    'an absent required identity field fails closed'
require_count 1 'f"{action} must have actor {expected_actor!r}, found {actor!r}"' "$GEN" \
    'an action on the wrong actor side fails closed'
require_count 1 "exceeds the bounded model's MaxRel={MAX_REL}" "$GEN" \
    'rel_seq beyond Protocol50.cfg'"'"'s MaxRel fails closed'

# Cardinality-overflow fail-closed branches: no constant is left to
# represent a 3rd F, a 2nd C, a 3rd session token per F, a mis-shaped or
# 3rd distinct TU, or a 3rd transaction-digest variant at one cursor.
require_count 1 'f"a third distinct f_store_guid ({guid!r}) has no model F left "' "$GEN" \
    'a third distinct f_store_guid fails closed (only F0/F1 exist)'
require_count 1 'f"a second distinct c_store_guid ({guid!r}) has no model C "' "$GEN" \
    'a second distinct c_store_guid fails closed (one shared C namespace)'
require_count 1 'f"a third session establishment (session_serial={raw_serial!r}) "' "$GEN" \
    'a third session establishment for one F fails closed (only Tok0/Tok1 exist)'
require_count 1 'was never introduced by a "' "$GEN" \
    'a history_nonce never established by HISTORY_RESET fails closed'
require_count 1 'f"NEED_RECORDED for tu_seq {raw_tu!r} names {size} key(s); "' "$GEN" \
    'a Need whose size is not 1 or 2 fails closed'
require_count 1 'already claimed the bounded model'"'"'s only {size}-key TU")' "$GEN" \
    'a second TU of the same Need-shape fails closed (only one T0, one T1)'
require_count 1 'f"tu_seq {two!r}'"'"'s 2-key Need {sorted(keys2)!r} does not "' "$GEN" \
    'a 2-key Need not containing the 1-key TU'"'"'s key fails closed (T1 superset of T0)'
require_count 1 'slot is left (T0 and T1 are both already assigned)")' "$GEN" \
    'a third distinct tu_seq fails closed once T0 and T1 are both assigned'
require_count 1 'was never part of any recorded Need, "' "$GEN" \
    'an OBJECT_APPLIED key64 outside every recorded Need fails closed'
require_count 1 'the bounded model'"'"'s only second-variant "' "$GEN" \
    'a second transaction digest off RetryDigestOp'"'"'s one fixed cursor fails closed'
require_count 1 'f"a third distinct transaction digest at cursor {cursor!r} exceeds "' "$GEN" \
    'a third transaction digest at one cursor fails closed'

# The deadlock/acceptance signal the whole driver depends on: without
# CHECK_DEADLOCK TRUE, a trace that gets stuck before its own last record
# would look identical to one that legitimately reached the end.
require_count 1 'SPECIFICATION RefinementSpec' "$GEN" \
    'the generated .cfg drives the trace-constrained spec, not the base model'
require_count 1 'CHECK_DEADLOCK TRUE' "$GEN" \
    'the generated .cfg checks deadlock (a stuck trace must be a TLC error)'
require_absent 'CHECK_DEADLOCK[[:space:]]+FALSE' "no relaxed .cfg deadlock setting" "$src/$GEN"

# No bypass: nothing lets a record be accepted without going through the
# mapping/validation path above.
require_absent '\-\-force|\-\-skip|ignore.?error|best.?effort' \
    'no flag weakens the fail-closed contract' "$src/$GEN" "$src/$DRIVER"

# The driver actually renders via trace_to_tla.py and then asks real TLC,
# and only accepts TLC's own no-error completion line (mirrors run_tlc.sh).
require_count 1 'trace_to_tla.py" "$TRACE_ABS" --out "$OUT_DIR"' "$DRIVER" \
    'the driver renders the trace through trace_to_tla.py before TLC runs'
require_count 1 'tlc2.TLC' "$DRIVER" \
    'the driver invokes the real TLC main class'
require_count 1 'Model checking completed. No error' "$DRIVER" \
    'the driver only accepts TLC'"'"'s own no-error completion marker'

echo 'PASS: trace-refinement generator keeps its fail-closed branches and deadlock gate'
