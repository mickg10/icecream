#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PYTHON=${PYTHON:-python3}
ENDPOINT=${P50_ENDPOINT_BIN:-"$SCRIPT_DIR/../../unittests/p50endpoint"}
WORKDIR=$(mktemp -d "${TMPDIR:-/tmp}/p50-live-global-trace.XXXXXX")
trap 'rm -rf "$WORKDIR"' EXIT HUP INT TERM

if [ ! -x "$ENDPOINT" ]; then
    echo "live endpoint binary is not executable: $ENDPOINT" >&2
    exit 1
fi

P50_ENDPOINT_GLOBAL_TRACE_PATH="$WORKDIR/live.jsonl" \
    "$ENDPOINT" >"$WORKDIR/endpoint.log" 2>&1
"$PYTHON" "$SCRIPT_DIR/check_live_global_trace.py" "$WORKDIR/live.jsonl"

"$PYTHON" - "$WORKDIR/live.jsonl" "$WORKDIR" <<'PY'
import copy
import json
import pathlib
import sys

source = pathlib.Path(sys.argv[1])
out = pathlib.Path(sys.argv[2])
rows = [json.loads(line) for line in source.read_text(encoding="utf-8").splitlines()]


def write(name, records):
    (out / f"{name}.jsonl").write_text(
        "".join(json.dumps(record, separators=(",", ":")) + "\n" for record in records),
        encoding="utf-8",
    )


deleted_present = copy.deepcopy(rows)
deleted_present.pop(next(i for i, row in enumerate(deleted_present)
                         if row["action"] == "ARENA_PRESENT"))
write("deleted-present", deleted_present)

changed_digest = copy.deepcopy(rows)
present = next(row for row in changed_digest if row["action"] == "ARENA_PRESENT")
present["content_digest"] = "f" * 32
write("changed-present-digest", changed_digest)

changed_slot = copy.deepcopy(rows)
present = next(row for row in changed_slot if row["action"] == "ARENA_PRESENT")
present["slot"] += 1
write("changed-present-slot", changed_slot)

deleted_release = copy.deepcopy(rows)
deleted_release.pop(next(i for i, row in enumerate(deleted_release)
                         if row["action"] == "ARENA_RELEASED"))
write("deleted-release", deleted_release)

changed_lru = copy.deepcopy(rows)
evicted = next(row for row in changed_lru if row["action"] == "NAMESPACE_EVICTED")
evicted["lru"] += 1
write("changed-eviction-lru", changed_lru)
PY

for mutant in \
    deleted-present changed-present-digest changed-present-slot \
    deleted-release changed-eviction-lru; do
    if "$PYTHON" "$SCRIPT_DIR/check_live_global_trace.py" \
        "$WORKDIR/$mutant.jsonl" >"$WORKDIR/$mutant.log" 2>&1; then
        echo "live global trace control unexpectedly passed: $mutant" >&2
        exit 1
    fi
done

echo "run_live_global_trace_gate.sh: live endpoint trace and 5 controls passed"
