#!/bin/sh
set -eu

trace_file="${TMPDIR:-/tmp}/icecream-p50-action-trace-$$.jsonl"
mutation_file="${TMPDIR:-/tmp}/icecream-p50-action-trace-mutation-$$.jsonl"
trap 'rm -f "$trace_file" "$mutation_file"' EXIT HUP INT TERM

P50_TRACE_PATH="$trace_file" ./p50slice0 >/dev/null
checker="$(dirname "$0")/../cache/formal/check_trace.py"
python3 "$checker" "$trace_file"
python3 - "$trace_file" "$mutation_file" <<'PY'
import json
import sys

source, destination = sys.argv[1:]
rows = [json.loads(line) for line in open(source, encoding="utf-8")]
for index, row in enumerate(rows):
    if row["action"] == "OBJECT_APPLIED":
        changed = dict(row)
        changed["duplicate"] = True
        first = "0" if row["content_digest"][0] != "0" else "1"
        changed["content_digest"] = first + row["content_digest"][1:]
        rows.insert(index + 1, changed)
        break
else:
    raise SystemExit("trace fixture lacks OBJECT_APPLIED")
with open(destination, "w", encoding="utf-8") as output:
    for row in rows:
        output.write(json.dumps(row, separators=(",", ":")) + "\n")
PY
if python3 "$checker" "$mutation_file" >/dev/null 2>&1; then
    echo "check_trace.py accepted changed content for an immutable Key64" >&2
    exit 1
fi
