#!/usr/bin/env python3
"""Render a Protocol-50 canonical JSONL trace as a TLA+ TraceSpec module that
refines Protocol50.tla's Next-relation to EXACTLY the observed sequence.

This is the Level-2 conformance tool: check_trace.py (Level 1) validates the
trace's own ordering rules stated in Python; this tool instead asks TLC to
replay the same trace as a sequence of Protocol50.tla actions, so a legal
trace must additionally be an actual behavior of the audited state machine
(TypeOK, InstalledContentExact, CommitReconciliationWitness, and the rest of
Protocol50.cfg's INVARIANTS all apply during replay).

Constant mapping (the bounded model has exactly 2 F's, 2 TUs, 2 Objects, 2
session tokens per F, and 2 transaction-digest variants -- see
cache/formal/README.md's "Bounded session-token scope" and Protocol50.tla's
CONSTANTS):

  c_store_guid   -- must be the SAME value for every record in the trace.
                    Protocol50.tla has one C-side cursor (cF/cNonce/cRel),
                    matching the product's one-shared-C-namespace contract
                    (README "One shared C namespace"); a second C identity
                    has nothing in the model to refine against.
  f_store_guid   -- first distinct value seen -> F0, second -> F1. A third
                    distinct value has no model constant left.
  session_serial -- scoped per mapped F. The first SESSION_OPENED/
                    SESSION_REPLACED for that F consumes Tok0, the second
                    consumes Tok1 (Protocol50.tla never releases a token:
                    usedTokens[f] only grows). A third session establishment
                    for the same F has no token left. Non-establishing rows
                    (HISTORY_RESET, SESSION_DISCONNECTED, COMMIT_ACCEPTED,
                    LOST_COMMIT_ACCEPTED) use whichever token is currently
                    live for that F at that point in the trace; if none is
                    live yet, Tok0 is used as a placeholder so TLC -- not
                    this generator -- is the one that rejects the step.
  history_nonce  -- scoped per mapped F. Protocol50.tla's nonce is a single
                    bit that flips on every HISTORY_RESET (nextNonce ==
                    1 - nonce[f]), starting at 1 on the first reset. Every
                    other row referencing a given F must carry a
                    history_nonce value that some prior HISTORY_RESET for
                    that F actually introduced; that raw value maps to
                    whichever bit that reset produced.
  rel_seq        -- used verbatim (already an exact position counter per
                    check_trace.py's own cursor rules) and bounds-checked
                    against Protocol50.cfg's MaxRel = 2.
  tu_seq         -- distinguished by the *shape* of its recorded Need, not
                    by order of appearance: TuObjects(T0) = {O0} (exactly
                    one key), TuObjects(T1) = {O0, O1} (exactly two keys,
                    one of which must be the same key T0 uses). At most one
                    distinct tu_seq may show a 1-key Need (-> T0) and at
                    most one may show a 2-key Need (-> T1); anything else
                    (0 or 3+ keys, a second TU of the same shape, a 2-key
                    Need that does not contain T0's key when T0 is also
                    present) cannot be represented. A tu_seq that never
                    gets a NEED_RECORDED (e.g. begun then aborted) is
                    assigned whatever model TU slot remains, since no
                    action in that case reads TuObjects for it.
  key64          -- the key T0's Need names (if T0 is present) is O0;
                    T1's other key is O1. If only T1-shaped TUs appear,
                    its two keys are assigned O0/O1 in first-seen order
                    (no T0 reference exists to force a specific choice).
                    A third distinct key64 has no model Object left.
  (transaction_digest, raw_digest)
                 -- scoped per (mapped F, mapped nonce, rel_seq, mapped TU)
                    cursor. The first pair seen at a cursor is digest
                    variant 0. Protocol50.tla's RealOps includes exactly
                    one second-variant operation, RetryDigestOp =
                    Op(F0, 1, 0, T0, 1); a second distinct pair is only
                    representable at that exact cursor, and a third is
                    never representable.

Everything else is TLC's job: once a trace maps onto concrete constants,
whether the resulting action sequence is actually enabled at each step
(ordering, session fencing, digest matching, ...) is exactly the question
Protocol50.tla's Next-relation answers, so this generator does not
re-implement those rules -- it only refuses to run TLC on a trace it cannot
faithfully translate into the bounded universe. A trace this tool accepts
but the model rejects should fail through run_trace_refinement.sh (TLC
reports a deadlock or invariant violation, not this generator).

FIELD-BINDING AUDIT. Two independent-reviewer HOLDs so far have both been
the same shape: a record-level field that check_trace.py treats as real
per-row identity gets silently discarded by Level 2 because the
corresponding Protocol50.tla action either doesn't expose a parameter for
it (it reads the model's OWN current state instead) or is handed the
model's own state back as if it were the caller's claim. c384cc53 (HOLD #1,
local-oracle) was the (nonce, rel_seq) cursor on TX_BEGIN_C. 0a47a6f5
(HOLD #2, fixed here -- local-oracle and, independently, BigOracle both
found this one) is session_serial on every F-actor row except the two
that establish it.

HOLD #2 specifically is why this audit is now done per FIELD, not per
ACTION SIGNATURE (BigOracle's framing of the same point local-oracle made):
signature-level auditing (HOLD #1's table below) checks "does this arm
pass a full Op(...) and does TLA check that Op for equality" -- which
every op-carrying arm appeared to satisfy, because Op is (f, n, rel, t, d)
and every one of those five components really was checked: whole-Op
equality binds {F, nonce, rel, TU, digest-variant}. But Op contains NO
session token at all -- session_serial lives on a parameter Op-carrying
signatures never had, so a pass that only asks "is the signature's own
parameter bound" cannot see a field the signature never had a slot for.
The fix is to instead walk every field the canonical JSONL schema puts on
a record and place it in one of three buckets:

  (a) WIRE PLACEHOLDER -- present on the row, but check_trace.py itself
      never treats it as this row's own identity for this action (it's a
      fixed, uninformative constant, e.g. always 0), so there is nothing
      for Level 2 to bind:
        session_serial on TX_BEGIN_C/TX_ABORTED/COMMIT_ACCEPTED/
          LOST_COMMIT_ACCEPTED -- always 0. current_f_session, the only
          function that treats session_serial as a real claim, guards on
          row["actor"] == "F" first and so is never even called for these
          four C-actor actions.
        history_nonce/rel_seq/tu_seq on SESSION_OPENED/SESSION_REPLACED/
          SESSION_DISCONNECTED/HISTORY_RESET -- these four are cursor-free
          by construction (see HOLD #1's per-arm table below).

  (b) MODEL-UNREPRESENTED DIAGNOSTIC -- a real check_trace.py identity or
      consistency field with no corresponding Protocol50.tla state
      variable, because the bounded model collapses the dimension that
      field distinguishes down to one fixed value:
        state_digest -- check_trace.py folds it into f["route"]/c["cursor"]
          as a third tuple component (checked on HISTORY_RESET, TX_BEGIN,
          ACTIVE_REPLAYED, INPUT_COMMITTED, COMMIT_ACCEPTED/
          LOST_COMMIT_ACCEPTED). Protocol50.tla's route[f] is a plain
          BOOLEAN (reset-or-not) with no value slot a digest could refine
          against.
        content_digest -- check_trace.py checks it stays immutable per
          key64 once installed. Protocol50.tla's content[f][o] can only
          ever hold NoContent or the FIXED CanonicalContent(o) -- the
          model has no notion of "the wrong content" a digest STRING could
          disagree with.
        need_keys / remaining_need / duplicate -- check_trace.py's own
          redundant bookkeeping, mirroring s.requested/s.missing/s.pinned,
          which Protocol50.tla derives purely from TuObjects(OpTu(op)), a
          FIXED function of the already-bound tu_seq -- not a free value
          these fields could independently disagree with. (An
          inconsistent need_keys/duplicate/etc. across rows for the same
          tu_seq is still generator-fail-closed by TuObjectMapper -- this
          bucket is about what Level 2/TLC cannot ALSO bind, not about
          Level 1 being skipped.)

  (c) BOUND -- checked against live Protocol50.tla state at replay time.
      f/history_nonce/rel_seq/tu_seq/digest-variant via Op equality and
      key64 via OBJECT_APPLIED's o \\in s.requested are covered by HOLD
      #1's table below (still accurate); session_serial is HOLD #2, this
      fix, detailed next.

session_serial, per arm (build_trace_log's new lookup_token/
is_session_bound, and the new StepMatchesRecord conjuncts below).
BigOracle's exact suggested form -- CurrentSession(s, r.f, r.tok), reusing
the model's own helper (Protocol50.tla's CurrentSession(st,f,tok) ==
st.session=f /\\ st.sessionToken=tok, already used by SESSION_DISCONNECTED/
HISTORY_RESET/COMMIT_ACCEPTED/LOST_COMMIT_ACCEPTED and, internally,
DICT_COMPLETE and friends) rather than a bespoke bare equality -- is what
every FIXED arm below now uses, uniformly, whether or not the action
already had a token parameter of its own:
  SESSION_OPENED/SESSION_REPLACED(r.f, r.tok)
      -- ESTABLISHING, unaffected: assign_token gives this row's
         session_serial its Tok0/Tok1 label the first time it's seen (and
         SESSION_REPLACED's own `tok \\in Tokens \\ s.usedTokens[f]` already
         forces the label fresh); nothing upstream to check it against.
  SESSION_DISCONNECTED(r.f, r.tok) / HISTORY_RESET(r.f, r.tok)
      -- FIXED: both already pass tok into a real Protocol50.tla parameter
         that CurrentSession(s, f, tok) checks directly against
         s.sessionToken -- the bug was entry["tok"] being the generator's
         OWN "whichever token is currently live" tracker instead of a
         lookup of THIS row's own session_serial, so a row that lied about
         its session_serial was fed the correct token anyway. Now
         `token_states[mapped_f].known[row["session_serial"]]`, the same
         table SESSION_OPENED/REPLACED populate, looked up via the new
         lookup_token (fails closed, like lookup_nonce, if no earlier
         SESSION_OPENED/SESSION_REPLACED for this F ever established this
         row's session_serial -- see fixtures/red-f-session-unknown.jsonl).
  TX_BEGIN_F / ACTIVE_REPLAYED: new conjunct CurrentSession(s, r.f, r.tok)
      -- FIXED: F_TX_BEGIN(op) takes no token parameter at all -- unlike
         SESSION_DISCONNECTED/HISTORY_RESET there was no existing
         CurrentSession call to feed correctly; this is HOLD #1's
         TX_BEGIN_C shape of bug (a cursor-free signature). The new
         conjunct pins the model's real CURRENT sessionToken to this row's
         mapped token before F_TX_BEGIN/ACTIVE_REPLAYED's own logic (which
         copies sessionToken into pendingToken) runs, so a row that lies
         about its session_serial now has no successor state.
  DICT_COMPLETE / NEED_RECORDED / BODY_COMPLETE / OBJECT_APPLIED /
  INPUT_MATERIALIZED / INPUT_COMMITTED: new conjunct
  CurrentSession(s, r.f, r.tok)
      -- FIXED: each already calls CurrentSession(s, ..., s.pendingToken)
         internally, but against the model's OWN pendingToken, not
         anything the caller supplies (Op carries no token field) -- so
         the check was tautologically true regardless of what this row
         claimed (Protocol50.tla's SessionFence invariant guarantees
         s.pendingToken already equals s.sessionToken whenever pendingOp is
         set, so this was never reachable-but-wrong at the invariant
         level -- it was simply never independently checked against the
         RECORD). The new conjunct makes the record's claim a real
         precondition of this specific step, so each row's own claim is
         verified on its own terms rather than only inherited from
         whatever TX_BEGIN_F/ACTIVE_REPLAYED pinned earlier in the same
         overlay -- see fixtures/red-f-session-stale.jsonl, which is
         mappable (its stale session_serial WAS established, just not by
         the live session) specifically so it reaches this conjunct at TLC
         rather than failing generator mappability like the unknown-serial
         fixture does.
  TX_BEGIN_C / TX_ABORTED / COMMIT_ACCEPTED / LOST_COMMIT_ACCEPTED
      -- no change: session_serial is a wire placeholder on these four
         (bucket (a) above). COMMIT_ACCEPTED/LOST_COMMIT_ACCEPTED already
         pass a real r.tok, but one derived from the generator's own
         live-token tracker rather than from this row's session_serial --
         correctly so, since this row's own field carries no independent
         claim to map.

Two fixtures prove this fix, deliberately hitting the two different ways
Level 2 can independently reject a bad session_serial (see
run_fixture_matrix.sh's layer_independence_proof):
  fixtures/red-f-session-unknown.jsonl (local-oracle's exact discriminator:
    green.jsonl's line 4 F TX_BEGIN session_serial changed 1->2, a serial
    NEVER established anywhere in the trace) -- UNMAPPABLE, so
    lookup_token fails the generator closed before TLC ever runs.
  fixtures/red-f-session-stale.jsonl (BigOracle's addition: SESSION_OPENED
    establishes serial 1/Tok0, SESSION_REPLACED establishes serial 9/Tok1
    as the new live session, then a DICT_COMPLETE row claims the now-STALE
    serial 1) -- MAPPABLE (Tok0 really was established), so the generator
    does not fail closed; TLC reaches the DICT_COMPLETE step and deadlocks
    on CurrentSession(s, r.f, r.tok) specifically, proving that conjunct
    itself catches a bad claim, not just unmappability.

HOLD #1 per-arm table (c384cc53: the original TX_BEGIN_C arm called
C_TX_BEGIN(r.f, r.t, r.d) and silently dropped r.n/r.rel -- C_TX_BEGIN
takes no nonce/rel parameters at all, it reads the model's OWN current
s.cNonce/s.cRel internally, so a record that declared the wrong cursor was
accepted anyway as long as the model's REAL cursor happened to allow some
C_TX_BEGIN transition. Every other arm passes a full
Op(r.f, r.n, r.rel, r.t, r.d) (or, for session/reset actions, has no
cursor to bind at all), and TLA's own precondition then checks that op for
EQUALITY against internal state (s.cActiveOp, s.pendingOp,
s.lastCommitOp[f], ...) -- so those arms were never exposed to this bug;
TX_BEGIN_C was the one arm that took a cursor-free signature and needed an
explicit extra bind; unaffected by HOLD #2, kept here for reference):

  SESSION_OPENED/REPLACED/DISCONNECTED(r.f, r.tok)
      -- no cursor parameters exist on these actions in Protocol50.tla at
         all (session identity is f+tok only); nothing to bind.
  HISTORY_RESET(r.f, r.tok)
      -- likewise cursor-free by construction: it *establishes* the new
         nonce (nextNonce == 1 - nonce[f]) rather than checking one, so
         there is no caller-supplied nonce/rel for it to be bound against.
  TX_BEGIN_C: s.cNonce = r.n /\\ s.cRel = r.rel /\\ C_TX_BEGIN(r.f, r.t, r.d)
      -- the two extra conjuncts pin the model's pre-state cursor to the
         record's declared (nonce, rel_seq) before C_TX_BEGIN's own logic
         runs, so a record that lies about its cursor now has no successor
         state (TLC deadlock) instead of silently reusing whatever cursor
         the model happened to be at.
  TX_BEGIN_F / ACTIVE_REPLAYED: F_TX_BEGIN(Op(r.f, r.n, r.rel, r.t, r.d))
      -- fully bound: F_TX_BEGIN's precondition requires
         OpNonce(op) = s.nonce[f] and OpRel(op) = s.fRel[f], i.e. it
         directly checks the record's declared nonce/rel against state.
  TX_ABORTED(Op(...))
      -- fully bound: precondition requires op = s.cActiveOp exactly, and
         s.cActiveOp's own nonce/rel were pinned by the TX_BEGIN_C arm
         that set it, so this equality is meaningful.
  DICT_COMPLETE / NEED_RECORDED / BODY_COMPLETE / OBJECT_APPLIED /
  INPUT_MATERIALIZED(Op(...))
      -- fully bound: each requires op = s.pendingOp exactly, and
         s.pendingOp's nonce/rel were pinned by the (fully-bound)
         TX_BEGIN_F/ACTIVE_REPLAYED arm that set it.
  INPUT_COMMITTED(Op(...))
      -- fully bound: the single explicit parameter is required to equal
         s.pendingOp (via callbackOp = current, MutantIgnoreTxDigest is
         FALSE in every generated .cfg so the alternate branch is dead).
  COMMIT_ACCEPTED / LOST_COMMIT_ACCEPTED(r.f, r.tok, Op(...))
      -- fully bound: precondition requires op = s.cActiveOp AND
         op = s.lastCommitOp[f] exactly.

Python 3 stdlib only, matching check_trace.py.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

ACTIONS = {
    "SESSION_OPENED",
    "SESSION_REPLACED",
    "SESSION_DISCONNECTED",
    "HISTORY_RESET",
    "TX_BEGIN",
    "TX_ABORTED",
    "DICT_COMPLETE",
    "BODY_COMPLETE",
    "NEED_RECORDED",
    "OBJECT_APPLIED",
    "INPUT_MATERIALIZED",
    "INPUT_COMMITTED",
    "COMMIT_ACCEPTED",
    "ACTIVE_REPLAYED",
    "LOST_COMMIT_ACCEPTED",
}
UINT64_MAX = (1 << 64) - 1
MAX_REL = 2  # must match Protocol50.cfg's MaxRel exactly.

# Every action except TX_BEGIN is inherently one-sided in Protocol50.tla;
# TX_BEGIN is the only name that splits into two model actions (C_TX_BEGIN
# and F_TX_BEGIN) by actor.
EXPECTED_ACTOR = {
    "SESSION_OPENED": "F",
    "SESSION_REPLACED": "F",
    "SESSION_DISCONNECTED": "F",
    "HISTORY_RESET": "F",
    "TX_ABORTED": "C",
    "DICT_COMPLETE": "F",
    "BODY_COMPLETE": "F",
    "NEED_RECORDED": "F",
    "OBJECT_APPLIED": "F",
    "INPUT_MATERIALIZED": "F",
    "INPUT_COMMITTED": "F",
    "COMMIT_ACCEPTED": "C",
    "ACTIVE_REPLAYED": "F",
    "LOST_COMMIT_ACCEPTED": "C",
}


class RefinementError(ValueError):
    """A record cannot be faithfully mapped onto Protocol50.tla's bounded
    constant universe. The only outcome for the CLI is a nonzero exit and a
    'line N: ...' message -- no TLA is ever emitted for a rejected trace."""


def fail(index: int, message: str) -> None:
    raise RefinementError(f"line {index}: {message}")


def load_rows(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for index, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            fail(index, "blank line in a JSONL trace")
        try:
            row = json.loads(line)
        except json.JSONDecodeError as exc:
            fail(index, f"not valid JSON ({exc})")
        if not isinstance(row, dict):
            fail(index, "record is not a JSON object")
        rows.append(row)
    return rows


def require(index: int, row: dict[str, Any], field: str) -> Any:
    if field not in row:
        fail(index, f"missing required field {field!r}")
    return row[field]


def require_u64(index: int, row: dict[str, Any], field: str) -> int:
    value = require(index, row, field)
    if not isinstance(value, int) or isinstance(value, bool) or not (0 <= value <= UINT64_MAX):
        fail(index, f"{field} is outside u64")
    return value


def check_common_shape(index: int, row: dict[str, Any]) -> tuple[str, str]:
    """Validate the fields every canonical record must carry, mirroring
    check_trace.py's own unconditional checks, and return (action, actor)."""
    action = require(index, row, "action")
    if action not in ACTIONS:
        fail(index, f"unknown action {action!r}")
    actor = require(index, row, "actor")
    if actor not in {"C", "F"}:
        fail(index, f"unknown actor {actor!r}")
    for field in ("session_serial", "history_nonce", "rel_seq", "tu_seq"):
        require_u64(index, row, field)
    require(index, row, "c_store_guid")
    require(index, row, "f_store_guid")
    require(index, row, "transaction_digest")
    require(index, row, "raw_digest")
    expected_actor = EXPECTED_ACTOR.get(action)
    if expected_actor is not None and actor != expected_actor:
        fail(index, f"{action} must have actor {expected_actor!r}, found {actor!r}")
    if row["rel_seq"] > MAX_REL:
        fail(index, f"rel_seq {row['rel_seq']} exceeds the bounded model's MaxRel={MAX_REL}")
    return action, actor


# Actions whose rows carry a real (history_nonce, rel_seq, tu_seq,
# transaction_digest, raw_digest) operation identity that must round-trip
# through the F0/F1 x nonce x rel x T0/T1 x digest-variant mapping.
# SESSION_OPENED/SESSION_REPLACED/SESSION_DISCONNECTED and HISTORY_RESET
# itself carry only placeholder values in those fields (see the real
# canonical trace: SESSION_OPENED/HISTORY_RESET always show tu_seq=0,
# TX_BEGIN(C)/COMMIT_ACCEPTED always show session_serial=0) and must not
# be fed into the cursor/Need/digest bookkeeping below.
OP_CARRYING_ACTIONS = {
    "TX_BEGIN",
    "ACTIVE_REPLAYED",
    "TX_ABORTED",
    "DICT_COMPLETE",
    "NEED_RECORDED",
    "BODY_COMPLETE",
    "OBJECT_APPLIED",
    "INPUT_MATERIALIZED",
    "INPUT_COMMITTED",
    "COMMIT_ACCEPTED",
    "LOST_COMMIT_ACCEPTED",
}


def record_kind(action: str, actor: str) -> str:
    """The TraceLog 'kind' tag. TX_BEGIN is the only canonical action that
    is two different Protocol50.tla actions (C_TX_BEGIN vs F_TX_BEGIN)
    depending on actor; every other name is already one-sided."""
    if action == "TX_BEGIN":
        return "TX_BEGIN_C" if actor == "C" else "TX_BEGIN_F"
    return action


class FNonceState:
    """Per-F history_nonce bookkeeping. Protocol50.tla's nonce is a single
    bit that flips (nextNonce == 1 - nonce[f]) on every HISTORY_RESET,
    starting from 0 in Init, so the k-th reset for a given F always maps to
    1 if k is odd, 0 if k is even -- independent of the trace's own raw
    nonce magnitudes, which only need to be strictly informative enough to
    name distinct epochs."""

    def __init__(self) -> None:
        self.current = 0
        self.known: dict[Any, int] = {}


def map_reset_nonce(index: int, raw_nonce: Any, state: FNonceState) -> int:
    tla_nonce = 1 - state.current
    state.known[raw_nonce] = tla_nonce
    state.current = tla_nonce
    return tla_nonce


def lookup_nonce(index: int, raw_nonce: Any, state: FNonceState) -> int:
    if raw_nonce not in state.known:
        fail(index, f"history_nonce {raw_nonce!r} was never introduced by a "
                    f"HISTORY_RESET for this F, so it cannot be mapped onto the "
                    f"bounded model's alternating nonce bit")
    return state.known[raw_nonce]


class FTokenState:
    """Per-F session_serial -> Tok0/Tok1 bookkeeping. Protocol50.tla never
    releases a used token (usedTokens[f] only grows), matching the README's
    'one original session and one replacement' bound: a third distinct
    session_serial for the same F has no token left."""

    def __init__(self) -> None:
        self.known: dict[Any, str] = {}
        self.order: list[Any] = []


def assign_token(index: int, raw_serial: Any, state: FTokenState) -> str:
    label = state.known.get(raw_serial)
    if label is not None:
        return label
    if len(state.order) >= 2:
        fail(index, f"a third session establishment (session_serial={raw_serial!r}) "
                    f"for this F has no session token left (only Tok0/Tok1 exist)")
    label = f"Tok{len(state.order)}"
    state.known[raw_serial] = label
    state.order.append(raw_serial)
    return label


def lookup_token(index: int, raw_serial: Any, state: FTokenState) -> str:
    if raw_serial not in state.known:
        fail(index, f"session_serial {raw_serial!r} was never established by a "
                    f"SESSION_OPENED/SESSION_REPLACED for this F, so it cannot be "
                    f"mapped onto the bounded model's Tok0/Tok1")
    return state.known[raw_serial]


def is_session_bound(action: str, actor: str) -> bool:
    """True iff this row's session_serial is a real, per-row F-session
    identity that check_trace.py itself validates -- i.e. current_f_session
    (check_trace.py) checks row["session_serial"] == f["session"] for this
    action. That function's first guard is row["actor"] == "F", so TX_BEGIN
    only counts here on its F half (TX_BEGIN_F); the C half is a wholly
    different Protocol50.tla action (C_TX_BEGIN) with no session concept.
    SESSION_OPENED/SESSION_REPLACED are the two ESTABLISHING actions:
    current_f_session is never called on them (they SET f["session"], they
    don't check it against anything), so they are handled separately by
    assign_token, not by lookup_token."""
    return actor == "F" and action not in ("SESSION_OPENED", "SESSION_REPLACED")


class TuObjectMapper:
    """Assigns raw tu_seq values to T0/T1 by NEED_RECORDED shape (not by
    order of appearance -- see the module docstring), and raw key64 values
    to O0/O1. observe_tu/observe_need are called while scanning the trace
    in order (giving precise per-line error messages); finalize() runs once
    the whole trace has been scanned, since which raw tu_seq is T0 vs T1,
    and which key is O0 vs O1, can depend on a NEED_RECORDED that has not
    been seen yet at any single row."""

    def __init__(self) -> None:
        self.tu_map: dict[Any, str] = {}
        self.tu_key_tuple: dict[Any, tuple[int, ...]] = {}
        self.size_owner: dict[int, Any] = {}
        self.tu_seen_order: list[Any] = []

    def observe_tu(self, raw_tu: Any) -> None:
        if raw_tu not in self.tu_seen_order:
            self.tu_seen_order.append(raw_tu)

    def observe_need(self, index: int, raw_tu: Any, keys: tuple[int, ...]) -> None:
        size = len(keys)
        if size not in (1, 2):
            fail(index, f"NEED_RECORDED for tu_seq {raw_tu!r} names {size} key(s); "
                        f"the bounded model's TUs need exactly 1 (T0) or 2 (T1) objects")
        if raw_tu in self.tu_key_tuple:
            if frozenset(self.tu_key_tuple[raw_tu]) != frozenset(keys):
                fail(index, f"tu_seq {raw_tu!r} previously recorded Need "
                            f"{sorted(self.tu_key_tuple[raw_tu])!r}, now {sorted(keys)!r}; "
                            f"the bounded model's TuObjects is a fixed function of the TU")
            return
        owner = self.size_owner.get(size)
        if owner is not None and owner != raw_tu:
            fail(index, f"tu_seq {raw_tu!r} records a {size}-key Need; tu_seq {owner!r} "
                        f"already claimed the bounded model's only {size}-key TU")
        self.tu_key_tuple[raw_tu] = keys
        self.size_owner[size] = raw_tu
        self.tu_map[raw_tu] = "T0" if size == 1 else "T1"

    def finalize(self, index: int) -> dict[Any, str]:
        key_map: dict[Any, str] = {}
        one = self.size_owner.get(1)
        two = self.size_owner.get(2)
        o0_key = None
        if one is not None:
            o0_key = self.tu_key_tuple[one][0]
            key_map[o0_key] = "O0"
        if two is not None:
            keys2 = self.tu_key_tuple[two]
            if o0_key is not None:
                if o0_key not in keys2:
                    fail(index, f"tu_seq {two!r}'s 2-key Need {sorted(keys2)!r} does not "
                                f"contain tu_seq {one!r}'s key {o0_key!r}; Protocol50.tla "
                                f"requires T1's Need to be exactly {{O0, O1}}, a superset "
                                f"of T0's {{O0}}")
                other_key = keys2[0] if keys2[1] == o0_key else keys2[1]
            else:
                key_map[keys2[0]] = "O0"
                other_key = keys2[1]
            key_map[other_key] = "O1"

        remaining = [t for t in ("T0", "T1") if t not in self.tu_map.values()]
        for raw_tu in self.tu_seen_order:
            if raw_tu in self.tu_map:
                continue
            if not remaining:
                fail(index, f"tu_seq {raw_tu!r} has no recorded Need but no model TU "
                            f"slot is left (T0 and T1 are both already assigned)")
            self.tu_map[raw_tu] = remaining.pop(0)
        return key_map

    def map_tu(self, index: int, raw_tu: Any) -> str:
        label = self.tu_map.get(raw_tu)
        if label is None:
            fail(index, f"tu_seq {raw_tu!r} was never assigned a model TU")
        return label


class DigestVariantMapper:
    """Assigns each (mapped F, mapped nonce, rel_seq, mapped TU) cursor's
    (transaction_digest, raw_digest) pairs to variant 0/1, mirroring
    Protocol50.tla's RealOps: every cursor gets one primary (variant 0)
    operation; only the fixed cursor (F0, nonce=1, rel_seq=0, T0)
    additionally has RetryDigestOp (variant 1)."""

    def __init__(self) -> None:
        self.seen: dict[tuple[Any, int, int, Any], dict[tuple[Any, Any], int]] = {}

    def variant(self, index: int, cursor: tuple[Any, int, int, Any],
                digest_pair: tuple[Any, Any]) -> int:
        table = self.seen.setdefault(cursor, {})
        if digest_pair in table:
            return table[digest_pair]
        n = len(table)
        if n == 0:
            table[digest_pair] = 0
            return 0
        if n == 1:
            if cursor != ("F0", 1, 0, "T0"):
                f, nonce, rel, tu = cursor
                fail(index, f"a second transaction digest at cursor "
                            f"(f={f}, nonce={nonce}, rel_seq={rel}, tu={tu}) cannot be "
                            f"represented; the bounded model's only second-variant "
                            f"operation is RetryDigestOp = Op(F0, nonce=1, rel_seq=0, T0)")
            table[digest_pair] = 1
            return 1
        fail(index, f"a third distinct transaction digest at cursor {cursor!r} exceeds "
                    f"the bounded model's two digest variants")
        raise AssertionError("unreachable")


def map_f(index: int, row: dict[str, Any], f_map: dict[Any, str], f_order: list[Any]) -> str:
    guid = row["f_store_guid"]
    label = f_map.get(guid)
    if label is not None:
        return label
    if len(f_order) >= 2:
        fail(index, f"a third distinct f_store_guid ({guid!r}) has no model F left "
                    f"(only F0/F1 exist)")
    label = f"F{len(f_order)}"
    f_map[guid] = label
    f_order.append(guid)
    return label


def build_trace_log(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Map a validated list of canonical JSONL records onto a list of
    TraceLog entries (dicts with kind/f/tok/n/rel/t/d/o, all naming
    Protocol50.tla constants or plain integers) ready for TLA rendering.
    Raises RefinementError with a precise message on the first record that
    cannot be represented in the bounded model; never silently drops one."""
    c_guid: Any = None
    f_map: dict[Any, str] = {}
    f_order: list[Any] = []
    nonce_states: dict[str, FNonceState] = {}
    token_states: dict[str, FTokenState] = {}
    tu_mapper = TuObjectMapper()

    parsed: list[tuple[int, str, str, dict[str, Any], str]] = []

    # Pass 1: validate shape, resolve F/nonce/token labels, and collect
    # every NEED_RECORDED shape (T0/T1, O0/O1 need a full-trace view before
    # finalize() can assign them -- see TuObjectMapper's docstring).
    for index, row in enumerate(rows, 1):
        action, actor = check_common_shape(index, row)

        guid = row["c_store_guid"]
        if c_guid is None:
            c_guid = guid
        elif guid != c_guid:
            fail(index, f"a second distinct c_store_guid ({guid!r}) has no model C "
                        f"left; Protocol50.tla has one shared C-side cursor (see "
                        f"README's 'One shared C namespace')")

        mapped_f = map_f(index, row, f_map, f_order)
        nonce_state = nonce_states.setdefault(mapped_f, FNonceState())
        token_state = token_states.setdefault(mapped_f, FTokenState())

        if action == "HISTORY_RESET":
            map_reset_nonce(index, row["history_nonce"], nonce_state)
        elif action in OP_CARRYING_ACTIONS:
            lookup_nonce(index, row["history_nonce"], nonce_state)

        if action in ("SESSION_OPENED", "SESSION_REPLACED"):
            assign_token(index, row["session_serial"], token_state)
        elif is_session_bound(action, actor):
            lookup_token(index, row["session_serial"], token_state)

        if action in OP_CARRYING_ACTIONS:
            tu_mapper.observe_tu(row["tu_seq"])
            if action == "NEED_RECORDED":
                keys = require(index, row, "need_keys")
                if not isinstance(keys, list) or not keys:
                    fail(index, "need_keys must be a non-empty list")
                key_tuple = tuple(dict.fromkeys(keys))
                for key in key_tuple:
                    if not isinstance(key, int) or isinstance(key, bool) or not (0 <= key <= UINT64_MAX):
                        fail(index, f"need_keys entry {key!r} is outside u64")
                tu_mapper.observe_need(index, row["tu_seq"], key_tuple)

        parsed.append((index, action, actor, row, mapped_f))

    key_map = tu_mapper.finalize(parsed[-1][0] if parsed else 1)

    # Pass 2: render each row now that every mapping table is complete.
    # Token "current"-ness is inherently sequential (it depends on which
    # SESSION_OPENED/REPLACED/DISCONNECTED came before this row), so it is
    # tracked here rather than in pass 1.
    entries: list[dict[str, Any]] = []
    current_token: dict[str, str | None] = {f: None for f in f_order}
    digest_mapper = DigestVariantMapper()

    for index, action, actor, row, mapped_f in parsed:
        kind = record_kind(action, actor)
        entry: dict[str, Any] = {
            "kind": kind, "f": mapped_f, "tok": "NoToken",
            "n": 0, "rel": row["rel_seq"], "t": "NoTU", "d": 0, "o": "O0",
        }

        if action in ("SESSION_OPENED", "SESSION_REPLACED"):
            tok = token_states[mapped_f].known[row["session_serial"]]
            entry["tok"] = tok
            current_token[mapped_f] = tok
        elif is_session_bound(action, actor):
            # FIXED (local-oracle HOLD #2 on 0a47a6f5): this used to be
            # `current_token[mapped_f] or "Tok0"` for SESSION_DISCONNECTED/
            # HISTORY_RESET (the generator's OWN "whichever token is
            # currently live" tracker) and simply absent (defaulting to the
            # template's "NoToken", unused by these arms) for every other
            # session-bound action -- in both cases the record's OWN
            # session_serial field was discarded rather than mapped. Now
            # every session-bound row maps ITS OWN declared session_serial
            # through the same table SESSION_OPENED/REPLACED populate, so a
            # row that lies about which session it belongs to no longer
            # gets a free pass by falling back to whatever is actually
            # live. lookup_token already proved in pass 1 that this raw
            # value was established by an earlier SESSION_OPENED/REPLACED
            # for this F, so the lookup below cannot KeyError.
            entry["tok"] = token_states[mapped_f].known[row["session_serial"]]
            if action == "SESSION_DISCONNECTED":
                current_token[mapped_f] = None
        elif action in ("COMMIT_ACCEPTED", "LOST_COMMIT_ACCEPTED"):
            # session_serial is a wire placeholder on these two (always 0;
            # see is_session_bound's docstring and the module docstring's
            # field-binding audit) -- current_f_session never checks it, so
            # there is no per-row claim to map. Protocol50.tla's
            # COMMIT_ACCEPTED/LOST_COMMIT_ACCEPTED still take a real tok
            # parameter, so the generator supplies its own independently
            # tracked "whichever token is currently live for this F" here,
            # same as before this fix.
            entry["tok"] = current_token[mapped_f] or "Tok0"

        if action in OP_CARRYING_ACTIONS:
            n = nonce_states[mapped_f].known[row["history_nonce"]]
            t = tu_mapper.map_tu(index, row["tu_seq"])
            cursor = (mapped_f, n, row["rel_seq"], t)
            digest_pair = (row["transaction_digest"], row["raw_digest"])
            d = digest_mapper.variant(index, cursor, digest_pair)
            entry["n"] = n
            entry["t"] = t
            entry["d"] = d

        if action == "OBJECT_APPLIED":
            key = require(index, row, "key64")
            if not isinstance(key, int) or isinstance(key, bool) or not (0 <= key <= UINT64_MAX):
                fail(index, "key64 is outside u64")
            label = key_map.get(key)
            if label is None:
                fail(index, f"key64 {key!r} was never part of any recorded Need, "
                            f"cannot map onto O0/O1")
            entry["o"] = label

        entries.append(entry)

    return entries


def render_entry(entry: dict[str, Any]) -> str:
    return (
        f'[kind |-> "{entry["kind"]}", f |-> {entry["f"]}, tok |-> {entry["tok"]}, '
        f'n |-> {entry["n"]}, rel |-> {entry["rel"]}, t |-> {entry["t"]}, '
        f'd |-> {entry["d"]}, o |-> {entry["o"]}]'
    )


TLA_TEMPLATE = """\
------------------------------ MODULE {module_name} ------------------------------
(* GENERATED by trace_to_tla.py from {source_name} -- do not hand-edit.
   Refines Protocol50.tla: TraceNext restricts Protocol50's Next-relation to
   exactly the {step_count} step(s) below, in order; once the trace is
   exhausted the only further step is stuttering. See trace_to_tla.py's
   module docstring for the constant-mapping rules used to build TraceLog. *)
EXTENDS Protocol50, Sequences, Naturals, TLC

VARIABLE i

TraceLog == {trace_log}

allVars == <<s, i>>

TraceInit == Init /\\ i = 1

StepMatchesRecord(k) ==
    LET r == TraceLog[k]
    IN CASE r.kind = "SESSION_OPENED"       -> SESSION_OPENED(r.f, r.tok)
         [] r.kind = "SESSION_REPLACED"     -> SESSION_REPLACED(r.f, r.tok)
         [] r.kind = "SESSION_DISCONNECTED" -> SESSION_DISCONNECTED(r.f, r.tok)
         [] r.kind = "HISTORY_RESET"        -> HISTORY_RESET(r.f, r.tok)
         [] r.kind = "TX_BEGIN_C"           -> s.cNonce = r.n /\\ s.cRel = r.rel /\\ C_TX_BEGIN(r.f, r.t, r.d)
         [] r.kind = "TX_BEGIN_F"           -> CurrentSession(s, r.f, r.tok) /\\ F_TX_BEGIN(Op(r.f, r.n, r.rel, r.t, r.d))
         [] r.kind = "ACTIVE_REPLAYED"      -> CurrentSession(s, r.f, r.tok) /\\ ACTIVE_REPLAYED(Op(r.f, r.n, r.rel, r.t, r.d))
         [] r.kind = "TX_ABORTED"           -> TX_ABORTED(Op(r.f, r.n, r.rel, r.t, r.d))
         [] r.kind = "DICT_COMPLETE"        -> CurrentSession(s, r.f, r.tok) /\\ DICT_COMPLETE(Op(r.f, r.n, r.rel, r.t, r.d))
         [] r.kind = "NEED_RECORDED"        -> CurrentSession(s, r.f, r.tok) /\\ NEED_RECORDED(Op(r.f, r.n, r.rel, r.t, r.d))
         [] r.kind = "BODY_COMPLETE"        -> CurrentSession(s, r.f, r.tok) /\\ BODY_COMPLETE(Op(r.f, r.n, r.rel, r.t, r.d))
         [] r.kind = "OBJECT_APPLIED"       -> CurrentSession(s, r.f, r.tok) /\\ OBJECT_APPLIED(Op(r.f, r.n, r.rel, r.t, r.d), r.o)
         [] r.kind = "INPUT_MATERIALIZED"   -> CurrentSession(s, r.f, r.tok) /\\ INPUT_MATERIALIZED(Op(r.f, r.n, r.rel, r.t, r.d))
         [] r.kind = "INPUT_COMMITTED"      -> CurrentSession(s, r.f, r.tok) /\\ INPUT_COMMITTED(Op(r.f, r.n, r.rel, r.t, r.d))
         [] r.kind = "COMMIT_ACCEPTED"      -> COMMIT_ACCEPTED(r.f, r.tok, Op(r.f, r.n, r.rel, r.t, r.d))
         [] r.kind = "LOST_COMMIT_ACCEPTED" -> LOST_COMMIT_ACCEPTED(r.f, r.tok, Op(r.f, r.n, r.rel, r.t, r.d))

TraceNext ==
    \\/ /\\ i <= Len(TraceLog)
       /\\ StepMatchesRecord(i)
       /\\ i' = i + 1
    \\/ /\\ i > Len(TraceLog)
       /\\ UNCHANGED allVars

RefinementSpec == TraceInit /\\ [][TraceNext]_allVars

TraceComplete == i > Len(TraceLog)

=============================================================================
"""

CFG_TEMPLATE = """\
SPECIFICATION RefinementSpec

CONSTANTS
    F0 = f0
    F1 = f1
    T0 = t0
    T1 = t1
    O0 = o0
    O1 = o1
    V0 = v0
    V1 = v1
    Tok0 = tok0
    Tok1 = tok1
    NoF = no_f
    NoTU = no_tu
    NoToken = no_token
    NoContent = no_content
    MaxRel = {max_rel}
    MutantAbortAfterCommit = FALSE
    MutantBeginAtMaxRel = FALSE
    MutantIgnoreTxDigest = FALSE

INVARIANTS
    TypeOK
    InstalledContentExact
    OneActive
    SessionFence
    ActiveOperationMatchesCursor
    PendingOperationMatchesRoute
    NeedIsExact
    PinnedObjectsPresent
    CommitOnlyAfterExactMaterialization
    AtMostOneAhead
    CommitReconciliationWitness
    ActiveSequenceHasRoom
    LastCommitOperationWellFormed

CHECK_DEADLOCK TRUE
"""


def module_name_for(trace_path: Path) -> str:
    stem = trace_path.stem
    sanitized = "".join(ch if ch.isalnum() else "_" for ch in stem)
    if not sanitized or not sanitized[0].isalpha():
        sanitized = f"T_{sanitized}"
    return f"TraceSpec_{sanitized}"


def render_tla(module_name: str, source_name: str, trace: list[dict[str, Any]]) -> str:
    if trace:
        body = ",\n        ".join(render_entry(e) for e in trace)
        trace_log = f"<<\n        {body}\n    >>"
    else:
        trace_log = "<< >>"
    return TLA_TEMPLATE.format(
        module_name=module_name, source_name=source_name,
        step_count=len(trace), trace_log=trace_log,
    )


def render_cfg() -> str:
    return CFG_TEMPLATE.format(max_rel=MAX_REL)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path, help="canonical Protocol-50 JSONL trace")
    parser.add_argument("--out", type=Path, required=True,
                         help="output directory for the generated .tla/.cfg")
    args = parser.parse_args()

    try:
        rows = load_rows(args.trace)
        trace_log = build_trace_log(rows)
    except RefinementError as exc:
        print(f"trace_to_tla.py: {exc}", file=sys.stderr)
        return 1

    module_name = module_name_for(args.trace)
    args.out.mkdir(parents=True, exist_ok=True)
    tla_path = args.out / f"{module_name}.tla"
    cfg_path = args.out / f"{module_name}.cfg"
    tla_path.write_text(render_tla(module_name, args.trace.name, trace_log), encoding="utf-8")
    cfg_path.write_text(render_cfg(), encoding="utf-8")

    print(f"trace_to_tla.py: wrote {tla_path} and {cfg_path} ({len(trace_log)} step(s))")
    print(f"trace_to_tla.py: module {module_name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
