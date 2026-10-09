#!/bin/sh
set -eu

srcdir=${srcdir:-$(dirname "$0")}
PYTHON=${PYTHON:-python3}
workdir="${TMPDIR:-/tmp}/icecream-p50-trace-$$"
trace_file="$workdir/canonical.jsonl"
checker="$srcdir/../cache/formal/check_trace.py"
trap 'rm -rf "$workdir"' EXIT HUP INT TERM
mkdir -p "$workdir"

expect_reject() {
    file=$1
    pattern=$2
    label=$3
    output="$file.out"
    if "$PYTHON" "$checker" "$file" >"$output" 2>&1; then
        echo "p50_trace_check: $label unexpectedly passed" >&2
        exit 1
    fi
    if ! grep -E "$pattern" "$output" >/dev/null 2>&1; then
        echo "p50_trace_check: $label failed through the wrong gate" >&2
        cat "$output" >&2
        exit 1
    fi
}

expect_pass() {
    file=$1
    label=$2
    if ! "$PYTHON" "$checker" "$file" >/dev/null 2>&1; then
        echo "p50_trace_check: $label unexpectedly failed" >&2
        "$PYTHON" "$checker" "$file" >&2 || true
        exit 1
    fi
}

ICECC_P50_ACTION_TRACE="$trace_file" \
ICECC_P50_P29_DIALOGUE_FOCUS=1 \
    ./p50endpoint >/dev/null
"$PYTHON" "$checker" "$trace_file"

"$PYTHON" - "$trace_file" "$workdir" <<'PY'
import copy
import json
import pathlib
import sys

source = pathlib.Path(sys.argv[1])
outdir = pathlib.Path(sys.argv[2])
rows = [json.loads(line) for line in source.read_text(encoding="utf-8").splitlines()]


def write(name, records):
    path = outdir / name
    with path.open("w", encoding="utf-8") as output:
        for record in records:
            output.write(json.dumps(record, separators=(",", ":")) + "\n")


def flip_hex(text):
    if not text:
        return "1"
    return ("0" if text[0] != "0" else "1") + text[1:]


def record(action, actor="F", serial=1, nonce=0, rel=0, tu=0,
           tx="", raw="", state="", **extra):
    value = {
        "action": action,
        "actor": actor,
        "c_store_guid": "c",
        "f_store_guid": "f",
        "session_serial": serial,
        "history_nonce": nonce,
        "rel_seq": rel,
        "tu_seq": tu,
        "transaction_digest": tx,
        "raw_digest": raw,
        "state_digest": state,
        "content_digest": "",
        "key64": None,
        "need_keys": [],
        "remaining_need": 0,
        "duplicate": False,
    }
    value.update(extra)
    return value

need_rows = copy.deepcopy(rows)
need_index = next(i for i, row in enumerate(need_rows)
                  if row["action"] == "NEED_RECORDED")
need_rows[need_index]["remaining_need"] += 1
write("bad-need.jsonl", need_rows)

content_rows = [
    record("SESSION_OPENED", serial=1),
    record("HISTORY_RESET", serial=1, nonce=10, state="s0"),
    record("TX_BEGIN", actor="C", serial=0, nonce=10, rel=0, tu=7,
           tx="tx", raw="raw", state="s0"),
    record("TX_BEGIN", serial=1, nonce=10, rel=0, tu=7,
           tx="tx", raw="raw", state="s0"),
    record("BODY_COMPLETE", serial=1, nonce=10, rel=0, tu=7,
           tx="tx", raw="raw", state="s0"),
    record("NEED_RECORDED", serial=1, nonce=10, rel=0, tu=7,
           tx="tx", raw="raw", state="s0", need_keys=[9],
           remaining_need=1),
    record("OBJECT_APPLIED", serial=1, nonce=10, rel=0, tu=7,
           tx="tx", raw="raw", state="s0", key64=9,
           content_digest="content-a", remaining_need=0),
    record("OBJECT_APPLIED", serial=1, nonce=10, rel=0, tu=7,
           tx="tx", raw="raw", state="s0", key64=9,
           content_digest="content-b", remaining_need=0, duplicate=True),
]
write("changed-object.jsonl", content_rows)

pending_rows = copy.deepcopy(rows)
c_begin = next(row for row in pending_rows
               if row["action"] == "TX_BEGIN" and row["actor"] == "C")
f_begin_index = next(i for i, row in enumerate(pending_rows)
                     if row["action"] == "TX_BEGIN" and row["actor"] == "F")
pending_abort = dict(c_begin)
pending_abort["action"] = "TX_ABORTED"
pending_abort["actor"] = "C"
pending_rows.insert(f_begin_index + 1, pending_abort)
write("pending-abort.jsonl", pending_rows)

commit_rows = copy.deepcopy(rows)
commit_index = next(i for i, row in enumerate(commit_rows)
                    if row["action"] == "INPUT_COMMITTED")
commit_abort = dict(commit_rows[commit_index])
commit_abort["action"] = "TX_ABORTED"
commit_abort["actor"] = "C"
commit_rows.insert(commit_index + 1, commit_abort)
write("commit-abort.jsonl", commit_rows)

digest_rows = copy.deepcopy(rows)
digest_index = next(i for i, row in enumerate(digest_rows)
                    if row["action"] == "BODY_COMPLETE")
digest_rows[digest_index]["transaction_digest"] = flip_hex(
    digest_rows[digest_index]["transaction_digest"])
write("wrong-operation-digest.jsonl", digest_rows)

cursor_rows = copy.deepcopy(rows)
old_begin = next(row for row in cursor_rows
                 if row["action"] == "TX_BEGIN" and row["actor"] == "C")
reused_begin = dict(old_begin)
reused_begin["tu_seq"] += 1
reused_begin["transaction_digest"] = flip_hex(old_begin["transaction_digest"])
reused_begin["raw_digest"] = flip_hex(old_begin["raw_digest"])
cursor_rows.append(reused_begin)
write("cursor-reuse.jsonl", cursor_rows)

relseq_rows = copy.deepcopy(rows)
relseq_index = next(i for i, row in enumerate(relseq_rows)
                    if row["action"] == "TX_BEGIN" and row["actor"] == "C")
relseq_rows[relseq_index]["rel_seq"] = (1 << 64) - 1
write("terminal-relseq.jsonl", relseq_rows)

write("stale-session.jsonl", [
    record("SESSION_OPENED", serial=1),
    record("HISTORY_RESET", serial=1, nonce=10, state="s0"),
    record("TX_BEGIN", actor="C", serial=0, nonce=10, rel=0, tu=7,
           tx="tx", raw="raw", state="s0"),
    record("SESSION_REPLACED", serial=2, nonce=10, state="s0"),
    record("TX_BEGIN", serial=1, nonce=10, rel=0, tu=7,
           tx="tx", raw="raw", state="s0"),
])

write("session-serial-reuse.jsonl", [
    record("SESSION_OPENED", serial=2),
    record("SESSION_DISCONNECTED", serial=2),
    record("SESSION_OPENED", serial=2),
])

write("nonce-decrease.jsonl", [
    record("SESSION_OPENED", serial=1),
    record("HISTORY_RESET", serial=1, nonce=9, state="s9"),
    record("SESSION_DISCONNECTED", serial=1, nonce=9, state="s9"),
    record("SESSION_OPENED", serial=2, nonce=9, state="s9"),
    record("HISTORY_RESET", serial=2, nonce=8, state="s8"),
])

write("second-reset.jsonl", [
    record("SESSION_OPENED", serial=1),
    record("HISTORY_RESET", serial=1, nonce=1, state="s1"),
    record("HISTORY_RESET", serial=1, nonce=2, state="s2"),
])

# ------------------------------------------------------------------
# Route pipelining (P50_ROUTE_PIPELINING.md step 7): synthetic K=2
# fixtures.  These are fully hand-synthesized (no binary needed) and
# exercise the generalized checker rules: C's ACTIVE_TX queue
# admission/retire order, F's unacknowledged-commit FIFO, the
# "ahead by at most K" reconciliation witness, and the
# universal-invalidation (poison -> cold reset) shape.  The
# queue_position/queue_depth fields are the additive C-side-only
# fields emitted by CRoute::record at K>1; their absence everywhere
# is what makes the canonical trace above (and every mutation of it)
# a K=1 trace that must keep passing byte-identically.
def pipelined_record(action, actor="F", serial=1, nonce=10, rel=0, tu=7,
                     tx="", raw="", state="", position=None, depth=None,
                     **extra):
    row = record(action, actor=actor, serial=serial, nonce=nonce, rel=rel,
                tu=tu, tx=tx, raw=raw, state=state, **extra)
    if position is not None or depth is not None:
        row["queue_position"] = position
        row["queue_depth"] = depth
    return row

def f_dialogue(rows, serial, rel, tu, tx, raw, state, key):
    rows.append(record("TX_BEGIN", serial=serial, nonce=10, rel=rel, tu=tu,
                       tx=tx, raw=raw, state=state))
    rows.append(record("BODY_COMPLETE", serial=serial, nonce=10, rel=rel, tu=tu,
                       tx=tx, raw=raw, state=state))
    rows.append(record("NEED_RECORDED", serial=serial, nonce=10, rel=rel,
                       tu=tu, tx=tx, raw=raw, state=state,
                       need_keys=[key], remaining_need=1))
    rows.append(record("OBJECT_APPLIED", serial=serial, nonce=10, rel=rel,
                       tu=tu, tx=tx, raw=raw, state=state, key64=key,
                       content_digest="c%d" % key, remaining_need=0))
    rows.append(record("INPUT_MATERIALIZED", serial=serial, nonce=10,
                       rel=rel, tu=tu, tx=tx, raw=raw, state=state))

# K=2 accepted-overlap chain: the successor is admitted (and released by
# F, running its whole dialogue) while the predecessor's commit is still
# durable-but-unaccepted by C; C then accepts both commits strictly
# head-first.  This is the valid pipelined shape.
k2_overlap = [
    record("SESSION_OPENED", serial=1),
    record("HISTORY_RESET", serial=1, nonce=10, state="s0"),
    pipelined_record("TX_BEGIN", actor="C", serial=0, rel=0, tu=7,
                     tx="tx0", raw="r0", state="s0", position=0, depth=2),
]
f_dialogue(k2_overlap, 1, 0, 7, "tx0", "r0", "s0", 9)
k2_overlap.append(record("INPUT_COMMITTED", serial=1, nonce=10, rel=0, tu=7,
                         tx="tx0", raw="r0", state="s1"))
k2_overlap.append(pipelined_record("TX_BEGIN", actor="C", serial=0, rel=1,
                                   tu=8, tx="tx1", raw="r1", state="s1",
                                   position=1, depth=2))
k2_overlap.append(record("SESSION_REPLACED", serial=2, nonce=10, rel=1,
                        state="s1"))
f_dialogue(k2_overlap, 2, 1, 8, "tx1", "r1", "s1", 11)
k2_overlap.append(record("INPUT_COMMITTED", serial=2, nonce=10, rel=1, tu=8,
                         tx="tx1", raw="r1", state="s2"))
k2_overlap.append(pipelined_record("COMMIT_ACCEPTED", actor="C", serial=0,
                                   rel=0, tu=7, tx="tx0", raw="r0",
                                   state="s1", position=0, depth=2))
k2_overlap.append(pipelined_record("COMMIT_ACCEPTED", actor="C", serial=0,
                                   rel=1, tu=8, tx="tx1", raw="r1",
                                   state="s2", position=0, depth=2))
k2_overlap.append(record("SESSION_DISCONNECTED", serial=2, nonce=10, rel=1,
                         tu=8, tx="tx1", raw="r1", state="s2"))
write("pipeline-k2-overlap.jsonl", k2_overlap)

# K=2 poison + cold reset: C aborts the whole chain (drain batch 0,1)
# after F made T0 durable, then a fresh-session HISTORY_RESET discards
# the abandoned-but-durable commit.
k2_poison = [
    record("SESSION_OPENED", serial=1),
    record("HISTORY_RESET", serial=1, nonce=10, state="s0"),
    pipelined_record("TX_BEGIN", actor="C", serial=0, rel=0, tu=7,
                     tx="tx0", raw="r0", state="s0", position=0, depth=2),
]
f_dialogue(k2_poison, 1, 0, 7, "tx0", "r0", "s0", 9)
k2_poison.append(record("INPUT_COMMITTED", serial=1, nonce=10, rel=0, tu=7,
                        tx="tx0", raw="r0", state="s1"))
k2_poison.append(pipelined_record("TX_BEGIN", actor="C", serial=0, rel=1,
                                  tu=8, tx="tx1", raw="r1", state="s1",
                                  position=1, depth=2))
k2_poison.append(pipelined_record("TX_ABORTED", actor="C", serial=0, rel=0,
                                  tu=7, tx="tx0", raw="r0", state="s0",
                                  position=0, depth=2))
k2_poison.append(pipelined_record("TX_ABORTED", actor="C", serial=0, rel=1,
                                  tu=8, tx="tx1", raw="r1", state="s1",
                                  position=1, depth=2))
k2_poison.append(record("SESSION_DISCONNECTED", serial=1, nonce=11,
                        state="s0x"))
k2_poison.append(record("SESSION_OPENED", serial=2, nonce=11, state="s0x"))
k2_poison.append(record("HISTORY_RESET", serial=2, nonce=11, state="s0x"))
write("pipeline-k2-poison-reset.jsonl", k2_poison)

# Negative: successor retiring before its head (accept T1 first).
k2_retire_bad = [dict(row) for row in k2_overlap]
accepts = [i for i, row in enumerate(k2_retire_bad)
           if row["action"] == "COMMIT_ACCEPTED"]
first, second = accepts[0], accepts[1]
k2_retire_bad[first], k2_retire_bad[second] = (
    k2_retire_bad[second], k2_retire_bad[first])
write("pipeline-k2-retire-before-head.jsonl", k2_retire_bad)

# Negative: C ahead of F by more than K -- a third admission (position 2)
# while the route's depth is 2.
k2_depth_bad = [dict(row) for row in k2_overlap]
begin_t1 = next(i for i, row in enumerate(k2_depth_bad)
                if row["action"] == "TX_BEGIN" and row["actor"] == "C"
                and row["rel_seq"] == 1)
third = dict(k2_depth_bad[begin_t1])
third["rel_seq"] = 2
third["tu_seq"] = 9
third["transaction_digest"] = "tx2"
third["raw_digest"] = "r2"
third["queue_position"] = 2
k2_depth_bad.insert(begin_t1 + 1, third)
write("pipeline-k2-ahead-more-than-k.jsonl", k2_depth_bad)

# Negative: acceptance names a non-head queue slot.
k2_nonhead = [dict(row) for row in k2_overlap]
first_accept = next(i for i, row in enumerate(k2_nonhead)
                    if row["action"] == "COMMIT_ACCEPTED")
k2_nonhead[first_accept]["queue_position"] = 1
write("pipeline-k2-accept-non-head.jsonl", k2_nonhead)

# Negative: admission skips a queue slot (position 0 while slot 0 is
# occupied).
k2_hole = [dict(row) for row in k2_overlap]
begin_t1 = next(i for i, row in enumerate(k2_hole)
                if row["action"] == "TX_BEGIN" and row["actor"] == "C"
                and row["rel_seq"] == 1)
k2_hole[begin_t1]["queue_position"] = 0
write("pipeline-k2-position-hole.jsonl", k2_hole)

# Negative: abort drain out of order (second record claims position 0).
k2_abort_bad = [dict(row) for row in k2_poison]
abort_t1 = next(i for i, row in enumerate(k2_abort_bad)
                if row["action"] == "TX_ABORTED" and row["rel_seq"] == 1)
k2_abort_bad[abort_t1]["queue_position"] = 0
write("pipeline-k2-abort-batch-order.jsonl", k2_abort_bad)

# Negative: F TX_BEGIN names a TX C never admitted (foreign queue member).
k2_foreign = [dict(row) for row in k2_overlap]
f_begin_t1 = next(i for i, row in enumerate(k2_foreign)
                  if row["action"] == "TX_BEGIN" and row["actor"] == "F"
                  and row["rel_seq"] == 1)
k2_foreign[f_begin_t1]["tu_seq"] = 99
write("pipeline-k2-f-begin-foreign.jsonl", k2_foreign)

# Negative: a K=1-scalar record (depth 1) carrying position 1 -- the
# additive fields must not smuggle a second slot past the scalar rule.
k1_scalar_bad = [dict(row) for row in k2_overlap]
begin_t1 = next(i for i, row in enumerate(k1_scalar_bad)
                if row["action"] == "TX_BEGIN" and row["actor"] == "C"
                and row["rel_seq"] == 1)
k1_scalar_bad[begin_t1]["queue_position"] = 1
k1_scalar_bad[begin_t1]["queue_depth"] = 1
write("pipeline-k1-scalar-position.jsonl", k1_scalar_bad)
PY

expect_pass "$workdir/pipeline-k2-overlap.jsonl" 'K=2 pipelined overlap chain'
expect_pass "$workdir/pipeline-k2-poison-reset.jsonl" \
    'K=2 poison -> cold reset (universal invalidation)'
expect_reject "$workdir/pipeline-k2-retire-before-head.jsonl" \
    'C acceptance lacks matching F commit' 'successor retired before its head'
expect_reject "$workdir/pipeline-k2-ahead-more-than-k.jsonl" \
    'C TX_BEGIN missed its queue slot' 'C ahead of F by more than K'
expect_reject "$workdir/pipeline-k2-accept-non-head.jsonl" \
    'C acceptance does not name the retiring queue head' \
    'acceptance named a non-head queue slot'
expect_reject "$workdir/pipeline-k2-position-hole.jsonl" \
    'C TX_BEGIN missed its queue slot' 'admission skipped a queue slot'
expect_reject "$workdir/pipeline-k2-abort-batch-order.jsonl" \
    'pipelined abort does not match its queue slot' \
    'abort drain out of batch order'
expect_reject "$workdir/pipeline-k2-f-begin-foreign.jsonl" \
    'F TX_BEGIN does not match C active' 'F began a foreign transaction'
expect_reject "$workdir/pipeline-k1-scalar-position.jsonl" \
    'C TX_BEGIN missed its queue slot' 'K=1 record claims a second slot'

expect_reject "$workdir/bad-need.jsonl" \
    'Need precedes exact BODY' 'inexact Need mutation'
expect_reject "$workdir/changed-object.jsonl" \
    'Key64 changed immutable content' 'immutable-content mutation'
expect_reject "$workdir/pending-abort.jsonl" \
    'C abort while F still owns pending overlay' 'abort with F pending'
expect_reject "$workdir/commit-abort.jsonl" \
    'C abort after durable F commit before acceptance' 'abort after durable commit'
expect_reject "$workdir/wrong-operation-digest.jsonl" \
    'BODY does not match F pending/current session' 'same-cursor digest ABA'
expect_reject "$workdir/cursor-reuse.jsonl" \
    '(C TX_BEGIN missed its cursor|second C active transaction)' \
    'route-cursor reuse'
expect_reject "$workdir/terminal-relseq.jsonl" \
    'C REL_SEQ exhausted before TX_BEGIN' 'terminal REL_SEQ begin'
expect_reject "$workdir/stale-session.jsonl" \
    'F TX_BEGIN at the wrong boundary' 'stale-session mutation'
expect_reject "$workdir/session-serial-reuse.jsonl" \
    'F session serial was reused or did not increase' 'session-serial reuse'
expect_reject "$workdir/nonce-decrease.jsonl" \
    'HISTORY_NONCE was reused or did not increase' 'history-nonce decrease'
expect_reject "$workdir/second-reset.jsonl" \
    'second HISTORY_RESET in one F session' 'second reset in one session'

echo "p50_trace_check: canonical trace and fail-closed mutations passed"
