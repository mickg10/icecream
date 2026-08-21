#!/bin/sh
set -eu

trace_file="${TMPDIR:-/tmp}/icecream-p50-action-trace-$$.jsonl"
trap 'rm -f "$trace_file"' EXIT HUP INT TERM

P50_TRACE_PATH="$trace_file" ./p50slice0 >/dev/null
checker="$(dirname "$0")/../cache/formal/check_trace.py"
python3 "$checker" "$trace_file"
python3 - "$checker" "$trace_file" <<'PY'
import json
import subprocess
import sys
import tempfile

checker, source = sys.argv[1:]
rows = [json.loads(line) for line in open(source, encoding="utf-8")]


def checker_accepts(records):
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8") as output:
        for record in records:
            output.write(json.dumps(record, separators=(",", ":")) + "\n")
        output.flush()
        return subprocess.run(
            [sys.executable, checker, output.name],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        ).returncode == 0


def require_rejected(label, records):
    if checker_accepts(records):
        raise SystemExit(f"check_trace.py accepted {label}")


for index, row in enumerate(rows):
    if row["action"] == "OBJECT_APPLIED":
        changed = dict(row)
        changed["duplicate"] = True
        first = "0" if row["content_digest"][0] != "0" else "1"
        changed["content_digest"] = first + row["content_digest"][1:]
        content_rows = list(rows)
        content_rows.insert(index + 1, changed)
        require_rejected("changed content for an immutable Key64", content_rows)
        break
else:
    raise SystemExit("trace fixture lacks OBJECT_APPLIED")

c_begin = next(row for row in rows
               if row["actor"] == "C" and row["action"] == "TX_BEGIN")
f_begin_index = next(index for index, row in enumerate(rows)
                     if row["actor"] == "F" and row["action"] == "TX_BEGIN")
f_commit_index = next(index for index, row in enumerate(rows)
                      if row["actor"] == "F" and row["action"] == "INPUT_COMMITTED")
abort = dict(c_begin)
abort["action"] = "TX_ABORTED"
require_rejected("abort while F had a pending transaction",
                 rows[:f_begin_index + 1] + [abort])
require_rejected("abort after F commit but before C acceptance",
                 rows[:f_commit_index + 1] + [abort])

f_actions = (
    "HISTORY_RESET", "TX_BEGIN", "DICT_COMPLETE", "BODY_COMPLETE",
    "NEED_RECORDED", "OBJECT_APPLIED", "INPUT_MATERIALIZED", "INPUT_COMMITTED",
)
for action in f_actions:
    changed = [dict(row) for row in rows]
    record = next(row for row in changed
                  if row["actor"] == "F" and row["action"] == action)
    record["session_serial"] += 1
    require_rejected(f"stale-session {action}", changed)

f_begin_index = next(index for index, row in enumerate(rows)
                     if row["actor"] == "F" and row["action"] == "TX_BEGIN")
replay = [dict(row) for row in rows[:f_begin_index + 1]]
replay[-1]["action"] = "ACTIVE_REPLAYED"
if not checker_accepts(replay):
    raise SystemExit("synthetic ACTIVE_REPLAYED fixture is invalid")
replay[-1]["session_serial"] += 1
require_rejected("stale-session ACTIVE_REPLAYED", replay)
PY
