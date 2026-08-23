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
#
# Also anchors HOLD fixes from three independent reviews, all the same
# false-green shape (a record field check_trace.py treats as real per-row
# identity gets silently discarded by Level 2): c384cc53/local-oracle (the
# mandatory Level-1 gate in the driver, and the cursor-binding predicate on
# TX_BEGIN_C, the one arm whose Protocol50.tla action signature has no
# cursor parameters of its own to check against) and 0a47a6f5/local-oracle
# +BigOracle (the driver accepted a trace whose F TX_BEGIN declared a
# session_serial belonging to no established session, because
# F_TX_BEGIN/DICT_COMPLETE/etc. either take no token parameter at all or
# check the model's OWN pendingToken instead of the caller's claim -- the
# CurrentSession(s, r.f, r.tok)-binding predicate on all eight affected
# arms, and the lookup_token fail-closed branch, must stay present).
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
require_count 1 'was never established by a "' "$GEN" \
    'a session_serial never established by SESSION_OPENED/REPLACED fails closed'
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

# local-oracle HOLD fix anchors (c384cc53 false-green): the mandatory
# Level-1 gate the driver runs before generation/TLC, and the cursor-bind
# predicate on the one CASE arm (TX_BEGIN_C) whose Protocol50.tla action
# signature has no cursor parameters of its own to check against.
require_count 1 '"$PYTHON" "$SCRIPT_DIR/check_trace.py" "$TRACE_ABS_EARLY"' "$DRIVER" \
    'the driver runs check_trace.py (Level 1) before trace_to_tla.py/TLC (Level 2)'
require_count 1 '[] r.kind = "TX_BEGIN_C"           -> s.cNonce = r.n /\\ s.cRel = r.rel /\\ C_TX_BEGIN(r.f, r.t, r.d)' "$GEN" \
    'TX_BEGIN_C binds the record'"'"'s declared cursor before calling C_TX_BEGIN'

# local-oracle + BigOracle HOLD #2 fix anchors (0a47a6f5 false-green, two
# independent reviews of the same defect): every arm whose Protocol50.tla
# action either takes no token parameter at all (TX_BEGIN_F/ACTIVE_REPLAYED)
# or checks the model's OWN pendingToken instead of the caller's claim (the
# six op-carrying arms below) must conjoin CurrentSession(s, r.f, r.tok)
# (BigOracle's exact suggested form -- reuses the model's own helper rather
# than a bespoke bare equality) before calling the action.
require_count 1 '[] r.kind = "TX_BEGIN_F"           -> CurrentSession(s, r.f, r.tok) /\\ F_TX_BEGIN(Op(r.f, r.n, r.rel, r.t, r.d))' "$GEN" \
    'TX_BEGIN_F binds the record'"'"'s declared session token before calling F_TX_BEGIN'
require_count 1 '[] r.kind = "ACTIVE_REPLAYED"      -> CurrentSession(s, r.f, r.tok) /\\ ACTIVE_REPLAYED(Op(r.f, r.n, r.rel, r.t, r.d))' "$GEN" \
    'ACTIVE_REPLAYED binds the record'"'"'s declared session token before calling ACTIVE_REPLAYED'
require_count 1 '[] r.kind = "DICT_COMPLETE"        -> CurrentSession(s, r.f, r.tok) /\\ DICT_COMPLETE(Op(r.f, r.n, r.rel, r.t, r.d))' "$GEN" \
    'DICT_COMPLETE binds the record'"'"'s declared session token before calling DICT_COMPLETE'
require_count 1 '[] r.kind = "NEED_RECORDED"        -> CurrentSession(s, r.f, r.tok) /\\ NEED_RECORDED(Op(r.f, r.n, r.rel, r.t, r.d))' "$GEN" \
    'NEED_RECORDED binds the record'"'"'s declared session token before calling NEED_RECORDED'
require_count 1 '[] r.kind = "BODY_COMPLETE"        -> CurrentSession(s, r.f, r.tok) /\\ BODY_COMPLETE(Op(r.f, r.n, r.rel, r.t, r.d))' "$GEN" \
    'BODY_COMPLETE binds the record'"'"'s declared session token before calling BODY_COMPLETE'
require_count 1 '[] r.kind = "OBJECT_APPLIED"       -> CurrentSession(s, r.f, r.tok) /\\ OBJECT_APPLIED(Op(r.f, r.n, r.rel, r.t, r.d), r.o)' "$GEN" \
    'OBJECT_APPLIED binds the record'"'"'s declared session token before calling OBJECT_APPLIED'
require_count 1 '[] r.kind = "INPUT_MATERIALIZED"   -> CurrentSession(s, r.f, r.tok) /\\ INPUT_MATERIALIZED(Op(r.f, r.n, r.rel, r.t, r.d))' "$GEN" \
    'INPUT_MATERIALIZED binds the record'"'"'s declared session token before calling INPUT_MATERIALIZED'
require_count 1 '[] r.kind = "INPUT_COMMITTED"      -> CurrentSession(s, r.f, r.tok) /\\ INPUT_COMMITTED(Op(r.f, r.n, r.rel, r.t, r.d))' "$GEN" \
    'INPUT_COMMITTED binds the record'"'"'s declared session token before calling INPUT_COMMITTED'

echo 'PASS: trace-refinement generator keeps its fail-closed branches and deadlock gate'
