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
PY

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
