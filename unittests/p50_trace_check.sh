#!/bin/sh
set -eu

trace_file="${TMPDIR:-/tmp}/icecream-p50-action-trace-$$.jsonl"
content_mutation="${TMPDIR:-/tmp}/icecream-p50-action-content-$$.jsonl"
pending_abort_mutation="${TMPDIR:-/tmp}/icecream-p50-action-pending-abort-$$.jsonl"
commit_abort_mutation="${TMPDIR:-/tmp}/icecream-p50-action-commit-abort-$$.jsonl"
trap 'rm -f "$trace_file" "$content_mutation" "$pending_abort_mutation" "$commit_abort_mutation"' EXIT HUP INT TERM

P50_TRACE_PATH="$trace_file" ./p50slice0 >/dev/null
checker="$(dirname "$0")/../cache/formal/check_trace.py"
python3 "$checker" "$trace_file"
python3 - "$trace_file" "$content_mutation" "$pending_abort_mutation" "$commit_abort_mutation" <<'PY'
import copy
import json
import sys

source, content_path, pending_path, commit_path = sys.argv[1:]
rows = [json.loads(line) for line in open(source, encoding="utf-8")]

content_rows = copy.deepcopy(rows)
for index, row in enumerate(content_rows):
    if row["action"] == "OBJECT_APPLIED":
        changed = dict(row)
        changed["duplicate"] = True
        first = "0" if row["content_digest"][0] != "0" else "1"
        changed["content_digest"] = first + row["content_digest"][1:]
        content_rows.insert(index + 1, changed)
        break
else:
    raise SystemExit("trace fixture lacks OBJECT_APPLIED")

pending_rows = copy.deepcopy(rows)
c_begin = next((row for row in pending_rows
                if row["action"] == "TX_BEGIN" and row["actor"] == "C"), None)
f_begin_index = next((index for index, row in enumerate(pending_rows)
                      if row["action"] == "TX_BEGIN" and row["actor"] == "F"), None)
if c_begin is None or f_begin_index is None:
    raise SystemExit("trace fixture lacks C/F TX_BEGIN")
pending_abort = dict(c_begin)
pending_abort["action"] = "TX_ABORTED"
pending_abort["actor"] = "C"
pending_rows.insert(f_begin_index + 1, pending_abort)

commit_rows = copy.deepcopy(rows)
commit_index = next((index for index, row in enumerate(commit_rows)
                     if row["action"] == "INPUT_COMMITTED"), None)
if commit_index is None:
    raise SystemExit("trace fixture lacks INPUT_COMMITTED")
commit_abort = dict(commit_rows[commit_index])
commit_abort["action"] = "TX_ABORTED"
commit_abort["actor"] = "C"
commit_rows.insert(commit_index + 1, commit_abort)

for path, output_rows in ((content_path, content_rows),
                          (pending_path, pending_rows),
                          (commit_path, commit_rows)):
    with open(path, "w", encoding="utf-8") as output:
        for row in output_rows:
            output.write(json.dumps(row, separators=(",", ":")) + "\n")
PY

if python3 "$checker" "$content_mutation" >/dev/null 2>&1; then
    echo "check_trace.py accepted changed content for an immutable Key64" >&2
    exit 1
fi
if python3 "$checker" "$pending_abort_mutation" >/dev/null 2>&1; then
    echo "check_trace.py accepted TX_ABORTED while F had a pending overlay" >&2
    exit 1
fi
if python3 "$checker" "$commit_abort_mutation" >/dev/null 2>&1; then
    echo "check_trace.py accepted TX_ABORTED after durable F commit" >&2
    exit 1
fi
