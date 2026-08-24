#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PYTHON=${PYTHON:-python3}
CHECKER="$SCRIPT_DIR/check_global_trace.py"
CANONICAL="$SCRIPT_DIR/global-trace.jsonl"
FRESH_CYCLE="$SCRIPT_DIR/global-fresh-cycle.jsonl"
TERMINAL_FIXTURE="$SCRIPT_DIR/global-admit-at-terminal-generation.jsonl"
WORKDIR=$(mktemp -d "${TMPDIR:-/tmp}/p50-global-trace.XXXXXX")
trap 'rm -rf "$WORKDIR"' EXIT HUP INT TERM

"$PYTHON" "$CHECKER" "$CANONICAL"
"$PYTHON" "$CHECKER" "$FRESH_CYCLE"
set +e
"$PYTHON" "$CHECKER" "$TERMINAL_FIXTURE" >"$WORKDIR/committed-terminal.out" 2>&1
rc=$?
set -e
if [ "$rc" -eq 0 ]; then
    echo "committed terminal-admission fixture unexpectedly passed" >&2
    exit 1
fi
grep -F "admission at terminal generation" "$WORKDIR/committed-terminal.out" >/dev/null || {
    echo "committed terminal-admission fixture failed through the wrong gate" >&2
    cat "$WORKDIR/committed-terminal.out" >&2
    exit 1
}

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

staged = copy.deepcopy(rows)
installing = next(row for row in staged
                 if row["action"] == "ARENA_INSTALLING" and
                 row["namespace"] == "n1")
installing["content_digest"] = "content0"
write("changed-staged-content.jsonl", staged)

retry_staged = copy.deepcopy(rows)
retry_installing = next(row for row in retry_staged
                        if row["action"] == "ARENA_RETRY_INSTALLING")
retry_installing["content_digest"] = "content1"
write("changed-retry-staged-content.jsonl", retry_staged)

cap = copy.deepcopy(rows)
present_index = next(i for i, row in enumerate(cap)
                     if row["action"] == "TU_FINISHED" and
                     row["namespace"] == "n0")
cap[present_index:present_index] = [
    {"action": "ARENA_INSTALLING", "namespace": "n0", "key": "k1",
     "slot": "slot0", "content_digest": "content1",
     "guid": "guid0", "generation": 0},
    {"action": "ARENA_PRESENT", "namespace": "n0", "key": "k1",
     "slot": "slot0", "content_digest": "content1",
     "guid": "guid0", "generation": 0},
]
write("aggregate-cap.jsonl", cap)

lru = copy.deepcopy(rows)
evict_index = next(i for i, row in enumerate(lru)
                   if row["action"] == "NAMESPACE_EVICTED")
lru[evict_index]["namespace"] = "n1"
write("wrong-lru.jsonl", lru)

admission = copy.deepcopy(rows)
stop_index = next(i for i, row in enumerate(admission)
                  if row["action"] == "GENERATION_WRAP_STOPPED")
admission.insert(stop_index + 1,
                 {"action": "NAMESPACE_ADMITTED", "namespace": "n0",
                  "guid": "guid0", "generation": 0})
write("admit-after-wrap.jsonl", admission)

terminal_admission = copy.deepcopy(rows)
advance_index = next(i for i, row in enumerate(terminal_admission)
                     if row["action"] == "GENERATION_ADVANCED")
terminal_admission.insert(advance_index + 1,
                           {"action": "NAMESPACE_ADMITTED", "namespace": "n0",
                           "guid": "guid0", "generation": 1})
write("admit-at-terminal-generation.jsonl", terminal_admission)

crash = copy.deepcopy(rows)
crash_row = next(row for row in crash if row["action"] == "INSTALL_CRASHED")
crash_row["slot"] = "slot1"
write("stale-crash-slot.jsonl", crash)

conflict = copy.deepcopy(rows)
conflict_row = next(row for row in conflict
                    if row["action"] == "CONTENT_CONFLICT_FATAL")
conflict_row["content_digest"] = "content1"
write("same-content-not-conflict.jsonl", conflict)

alias = copy.deepcopy(rows)
flip = next(row for row in alias if row["action"] == "C_GUID_FLIPPED")
flip["guid"] = "guid2"
write("cross-namespace-guid-alias.jsonl", alias)

required = {
    "NAMESPACE_ADMITTED": ("guid", "generation"),
    "NAMESPACE_TOUCHED": ("lru",),
    "ARENA_INSTALLING": ("key", "slot", "content_digest", "guid", "generation"),
    "ARENA_RETRY_INSTALLING": ("key", "slot", "content_digest", "guid", "generation"),
    "ARENA_PRESENT": ("key", "slot", "content_digest", "guid", "generation"),
    "ARENA_PINNED": ("key",),
    "ARENA_UNPINNED": ("key",),
    "INSTALL_CRASHED": ("key", "slot"),
    "CONTENT_CONFLICT_FATAL": ("key", "content_digest"),
    "GENERATION_ADVANCED": ("generation",),
    "GENERATION_WRAP_STOPPED": ("generation",),
    "C_GUID_FLIPPED": ("guid",),
}
field_mutants = 0
for index, row in enumerate(rows):
    for field in required.get(row["action"], ()):
        mutated = copy.deepcopy(rows)
        mutated[index].pop(field, None)
        write(f"missing-field-{field_mutants}.jsonl", mutated)
        field_mutants += 1
(outdir / "missing-field-count").write_text(str(field_mutants), encoding="utf-8")
PY

field_mutants=$(cat "$WORKDIR/missing-field-count")
for i in $(seq 0 $((field_mutants - 1))); do
    set +e
    "$PYTHON" "$CHECKER" "$WORKDIR/missing-field-$i.jsonl" \
        >"$WORKDIR/missing-field-$i.out" 2>&1
    rc=$?
    set -e
    if [ "$rc" -eq 0 ]; then
        echo "global trace missing-field mutant unexpectedly passed: $i" >&2
        exit 1
    fi
    grep -F "missing global field" "$WORKDIR/missing-field-$i.out" >/dev/null || {
        echo "global trace missing-field mutant failed through the wrong gate: $i" >&2
        cat "$WORKDIR/missing-field-$i.out" >&2
        exit 1
    }
done

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
expect_reject changed-content "staged content changed before PRESENT"
expect_reject changed-staged-content "staged content changed before PRESENT"
expect_reject changed-retry-staged-content "staged content changed before PRESENT"
expect_reject aggregate-cap "aggregate byte cap exceeded"
expect_reject wrong-lru "eviction victim is not whole-namespace LRU"
expect_reject admit-after-wrap "admission after generation wrap stop"
expect_reject admit-at-terminal-generation "admission at terminal generation"
expect_reject stale-crash-slot "crash does not own staging slot"
expect_reject same-content-not-conflict "conflict is not same-key/different-content"
expect_reject cross-namespace-guid-alias "GUID aliases another namespace"

echo "run_global_trace_gate.sh: canonical trace and deletion/mutation controls passed"
