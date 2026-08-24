#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PYTHON=${PYTHON:-python3}
CHECKER="$SCRIPT_DIR/check_global_trace.py"
CANONICAL="$SCRIPT_DIR/global-trace.jsonl"
WORKDIR=$(mktemp -d "${TMPDIR:-/tmp}/p50-global-trace.XXXXXX")
trap 'rm -rf "$WORKDIR"' EXIT HUP INT TERM

"$PYTHON" "$CHECKER" "$CANONICAL"

"$PYTHON" - "$CANONICAL" "$WORKDIR" <<'PY'
import copy
import json
import pathlib
import sys

source = pathlib.Path(sys.argv[1])
outdir = pathlib.Path(sys.argv[2])
rows = [json.loads(line) for line in source.read_text(encoding="utf-8").splitlines()]


def write(name, records):
    path = outdir / name
    path.write_text(
        "".join(json.dumps(record, separators=(",", ":")) + "\n"
                for record in records),
        encoding="utf-8",
    )


retry = copy.deepcopy(rows)
retry.pop(next(i for i, row in enumerate(retry)
              if row["action"] == "ARENA_RETRY_INSTALLING"))
write("deleted-retry.jsonl", retry)

content = copy.deepcopy(rows)
present = next(row for row in content if row["action"] == "ARENA_PRESENT")
present["content_digest"] = "content1"
write("changed-content.jsonl", content)

cap = copy.deepcopy(rows)
present_index = next(i for i, row in enumerate(cap)
                     if row["action"] == "CONTENT_CONFLICT_FATAL")
cap[present_index:present_index] = [
    {"action": "ARENA_INSTALLING", "namespace": "n0", "key": "k1",
     "slot": "slot0", "content_digest": "content1"},
    {"action": "ARENA_PRESENT", "namespace": "n0", "key": "k1",
     "slot": "slot0", "content_digest": "content1"},
]
write("aggregate-cap.jsonl", cap)

lru = copy.deepcopy(rows)
evict_index = next(i for i, row in enumerate(lru)
                   if row["action"] == "NAMESPACE_EVICTED")
lru[evict_index]["namespace"] = "n0"
write("wrong-lru.jsonl", lru)

admission = copy.deepcopy(rows)
stop_index = next(i for i, row in enumerate(admission)
                  if row["action"] == "GENERATION_WRAP_STOPPED")
admission.insert(stop_index + 1,
                 {"action": "NAMESPACE_ADMITTED", "namespace": "n0"})
write("admit-after-wrap.jsonl", admission)

crash = copy.deepcopy(rows)
crash_row = next(row for row in crash if row["action"] == "INSTALL_CRASHED")
crash_row["slot"] = "slot1"
write("stale-crash-slot.jsonl", crash)

conflict = copy.deepcopy(rows)
conflict_row = next(row for row in conflict
                    if row["action"] == "CONTENT_CONFLICT_FATAL")
conflict_row["content_digest"] = "content0"
write("same-content-not-conflict.jsonl", conflict)
PY

expect_reject() {
    name=$1
    pattern=$2
    set +e
    "$PYTHON" "$CHECKER" "$WORKDIR/$name.jsonl" >"$WORKDIR/$name.out" 2>&1
    rc=$?
    set -e
    if [ "$rc" -eq 0 ]; then
        echo "global trace mutant unexpectedly passed: $name" >&2
        exit 1
    fi
    grep -F "$pattern" "$WORKDIR/$name.out" >/dev/null || {
        echo "global trace mutant failed through the wrong gate: $name" >&2
        cat "$WORKDIR/$name.out" >&2
        exit 1
    }
}

expect_reject deleted-retry "PRESENT without INSTALLING"
expect_reject changed-content "immutable arena content changed"
expect_reject aggregate-cap "aggregate byte cap exceeded"
expect_reject wrong-lru "eviction victim is not whole-namespace LRU"
expect_reject admit-after-wrap "admission after generation wrap stop"
expect_reject stale-crash-slot "crash does not own staging slot"
expect_reject same-content-not-conflict "conflict is not same-key/different-content"

echo "run_global_trace_gate.sh: canonical trace and deletion/mutation controls passed"
