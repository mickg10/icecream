#!/bin/sh
set -eu

sim=${P50SIM_BINARY:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/.p50sim.bin}
if [ ! -x "$sim" ]; then
    echo "p50sim_batch_test: product batch binary is not built; skipping" >&2
    exit 77
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/p50sim-batch-test.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
printf '%s\n' 'alpha alpha alpha' > "$work/a.ii"
printf '%s\n' 'beta beta beta beta' > "$work/b.ii"
printf '%s\n%s\n' "$work/a.ii" "$work/b.ii" > "$work/manifest"
printf '%s\n%s\n%s\n' cardinality=1 0 0 > "$work/map1"

ICECC_P50_PROFILE=ZSTD_ROUTE "$sim" --batch-manifest "$work/manifest" \
    --batch-assignment-map "$work/map1" --batch-output "$work/route.jsonl"
ICECC_P50_PROFILE=ZSTD_TU "$sim" --batch-manifest "$work/manifest" \
    --batch-assignment-map "$work/map1" --batch-output "$work/tu.jsonl"

python3 - "$work/route.jsonl" "$work/tu.jsonl" <<'PY'
import json, sys
route = [json.loads(line) for line in open(sys.argv[1])]
tu = [json.loads(line) for line in open(sys.argv[2])]
assert len(route) == len(tu) == 2
assert all(type(row.get("prepare_ns")) is int and row["prepare_ns"] >= 0
           for rows in (route, tu) for row in rows)
assert route[0]["relationship_id"] == "c1f1-r00"
assert route[1]["state_before_digest"] == route[0]["state_digest"]
assert route[1]["transaction_digest"] != route[0]["transaction_digest"]
assert route[1]["c_store_guid"] == route[0]["c_store_guid"]
assert route[1]["f_store_guid"] == route[0]["f_store_guid"]
assert [row["rel_seq"] for row in route] == [0, 1]
assert route[1]["c_to_f_bytes"] != tu[1]["c_to_f_bytes"] or route[1]["state_digest"] != tu[1]["state_digest"]
PY

printf '%s\n%s\n%s\n%s\n' "$work/a.ii" "$work/b.ii" \
    "$work/a.ii" "$work/b.ii" > "$work/repeated.manifest"
printf '%s\n%s\n%s\n%s\n%s\n' cardinality=1 0 0 0 0 > "$work/repeated.map"
if ICECC_P50_PROFILE=ZSTD_TU "$sim" \
       --batch-manifest "$work/repeated.manifest" \
       --batch-assignment-map "$work/repeated.map" \
       --batch-output "$work/repeated-rejected.jsonl" 2>"$work/repeated.err"; then
    echo "p50sim_batch_test: repeated paths accepted without explicit occurrence mode" >&2
    exit 1
fi
grep -q 'batch TU manifest contains a duplicate path' "$work/repeated.err"
ICECC_P50_PROFILE=ZSTD_TU "$sim" \
    --batch-manifest "$work/repeated.manifest" \
    --batch-assignment-map "$work/repeated.map" \
    --batch-output "$work/repeated.jsonl" \
    --batch-allow-repeated-inputs 1
python3 - "$work/repeated.jsonl" <<'PY'
import json, sys
rows = [json.loads(line) for line in open(sys.argv[1])]
assert len(rows) == 4
assert [row["tu_index"] for row in rows] == [0, 1, 2, 3]
assert [row["tu_seq"] for row in rows] == [0, 1, 2, 3]
assert rows[0]["raw_digest"] == rows[2]["raw_digest"]
assert rows[0]["transaction_digest"] != rows[2]["transaction_digest"]
assert [row["rel_seq"] for row in rows] == [0, 1, 2, 3]
PY

for n in $(seq 0 19); do
    printf 'unit-%s\n' "$n" > "$work/$n.ii"
    printf '%s\n' "$work/$n.ii" >> "$work/manifest20"
done
{
    printf '%s\n' cardinality=20
    seq 0 19
} > "$work/map20"
ICECC_P50_PROFILE=ZSTD_TU "$sim" --batch-manifest "$work/manifest20" \
    --batch-assignment-map "$work/map20" --batch-output "$work/route20.jsonl"
python3 - "$work/route20.jsonl" <<'PY'
import json, sys
rows = [json.loads(line) for line in open(sys.argv[1])]
assert len(rows) == 20
assert {row["relationship_id"] for row in rows} == {f"c1f20-r{i:02d}" for i in range(20)}
assert len({row["f_store_guid"] for row in rows}) == 20
assert len({row["c_store_guid"] for row in rows}) == 1
assert [row["tu_seq"] for row in rows] == list(range(20))
assert [row["rel_seq"] for row in rows] == [0] * 20
assert len({row["state_digest"] for row in rows}) == 20
PY

ICECC_P50_PROFILE=P29 "$sim" --batch-manifest "$work/manifest" \
    --batch-assignment-map "$work/map1" --batch-output "$work/p29.jsonl"
test "$(wc -l < "$work/p29.jsonl")" -eq 2
if ICECC_P50_PROFILE=GRZ_RESIDUAL "$sim" --batch-manifest "$work/manifest" \
       --batch-assignment-map "$work/map1" --batch-output "$work/grz.jsonl" 2>"$work/grz.err"; then
    test "$(wc -l < "$work/grz.jsonl")" -eq 2
    grz_available=1
    ICECC_P50_PROFILE=GRZ_RESIDUAL "$sim" --batch-manifest "$work/manifest20" \
        --batch-assignment-map "$work/map20" --batch-output "$work/grz20.jsonl"
    python3 - "$work/grz20.jsonl" <<'PY'
import json, sys
rows = [json.loads(line) for line in open(sys.argv[1])]
assert len(rows) == 20
assert [row["tu_seq"] for row in rows] == list(range(20))
assert [row["rel_seq"] for row in rows] == [0] * 20
PY
else
    grz_available=0
    grep -q 'requires a simulator built with --with-libbsc' "$work/grz.err"
fi

ICECC_P50_PROFILE=ZSTD_ROUTE "$sim" --batch-manifest "$work/manifest" \
    --batch-assignment-map "$work/map1" --batch-manifest-2 "$work/manifest" \
    --batch-assignment-map-2 "$work/map1" --batch-output "$work/pair.jsonl"
python3 - "$work/pair.jsonl" <<'PY'
import json, sys
rows = [json.loads(line) for line in open(sys.argv[1])]
assert [row["segment"] for row in rows] == ["full-1", "full-1", "full-2", "full-2"]
assert [row["tu_seq"] for row in rows] == [0, 1, 2, 3]
assert [row["rel_seq"] for row in rows] == [0, 1, 2, 3]
assert rows[2]["tu_seq"] == 2 and rows[2]["state_before_digest"] == rows[1]["state_digest"]
PY

# Interleaved relationships retain independent REL_SEQ while the C authority
# continues one global TU_SEQ stream.
printf '%s\n%s\n%s\n%s\n' "$work/a.ii" "$work/b.ii" "$work/a.ii" "$work/b.ii" > "$work/interleaved.manifest"
printf '%s\n%s\n%s\n%s\n%s\n' cardinality=20 0 1 0 1 > "$work/interleaved.map"
ICECC_P50_PROFILE=ZSTD_ROUTE "$sim" --batch-manifest "$work/interleaved.manifest" \
    --batch-assignment-map "$work/interleaved.map" --batch-allow-repeated-inputs 1 \
    --batch-output "$work/interleaved.jsonl"
python3 - "$work/interleaved.jsonl" <<'PY'
import json, sys
rows = [json.loads(line) for line in open(sys.argv[1])]
assert [row["tu_seq"] for row in rows] == [0, 1, 2, 3]
assert [row["rel_seq"] for row in rows] == [0, 0, 1, 1]
by_route = {}
for row in rows:
    by_route.setdefault(row["relationship_id"], []).append(row["rel_seq"])
assert sorted(by_route.values()) == [[0, 1], [0, 1]]
PY

# A batch larger than the F InputRecordStore record bound must remain
# successful: each committed compiler-visible record is closed and collected
# before the next TU, while the relationship state continues across rows.
: > "$work/retention.manifest"
: > "$work/retention.map"
printf '%s\n' cardinality=1 >> "$work/retention.map"
for n in $(seq 0 4096); do
    printf 'retention-%s\n' "$n" > "$work/retention-$n.ii"
    printf '%s\n' "$work/retention-$n.ii" >> "$work/retention.manifest"
    printf '0\n' >> "$work/retention.map"
done
ICECC_P50_PROFILE=ZSTD_TU "$sim" --batch-manifest "$work/retention.manifest" \
    --batch-assignment-map "$work/retention.map" --batch-output "$work/retention.jsonl"
test "$(wc -l < "$work/retention.jsonl")" -eq 4097

ICECC_P50_PROFILE=P29 "$sim" --batch-manifest "$work/manifest" \
    --batch-assignment-map "$work/map1" --batch-manifest-2 "$work/manifest" \
    --batch-assignment-map-2 "$work/map1" --batch-output "$work/p29-pair.jsonl"
python3 - "$work/p29-pair.jsonl" <<'PY'
import json, sys
rows = [json.loads(line) for line in open(sys.argv[1])]
assert [row["segment"] for row in rows] == ["full-1", "full-1", "full-2", "full-2"]
assert rows[2]["tu_seq"] == 2
assert rows[2]["state_before_digest"] == rows[1]["state_digest"]
assert rows[3]["state_before_digest"] == rows[2]["state_digest"]
PY

ICECC_P50_PROFILE=ZSTD_ROUTE "$sim" --batch-manifest "$work/manifest" \
    --batch-assignment-map "$work/map1" --batch-manifest-2 "$work/manifest" \
    --batch-assignment-map-2 "$work/map1" --batch-manifest-3 "$work/manifest" \
    --batch-assignment-map-3 "$work/map1" --batch-output "$work/warm-pair.jsonl"
python3 - "$work/warm-pair.jsonl" <<'PY'
import json, sys
rows = [json.loads(line) for line in open(sys.argv[1])]
assert [row["segment"] for row in rows] == [
    "prewarm", "prewarm", "full-1", "full-1", "full-2", "full-2"]
assert rows[2]["state_before_digest"] == rows[1]["state_digest"]
assert rows[4]["state_before_digest"] == rows[3]["state_digest"]
PY

# Real-input GRZ_RESIDUAL long control (finding F12).  The C1F1 Full-1 GRZ
# arm at e58ed450 failed in the allocator while committing row 339 of 2,498
# although every short control stayed silent, so a bounded real-input replay
# is retained here as an opt-in gate.  Supply an authenticated C1F1 full-1
# manifest.json (P50SIM_GRZ_REAL_MANIFEST); the first N selected inputs
# (P50SIM_GRZ_REAL_ROWS, default 341 so the control passes the failing row)
# are replayed through GRZ_RESIDUAL on one relationship and must produce a
# clean exit with exactly N rows.  P50SIM_GRZ_REAL_EXPECTED names retained
# rows (native-output.jsonl) that every replayed row must match field by
# field; P50SIM_GRZ_REAL_DETERMINISM=1 replays the manifest a second time and
# requires the two row streams to be identical in every content field.
if [ -n "${P50SIM_GRZ_REAL_MANIFEST:-}" ] && [ "$grz_available" = 1 ]; then
    real_rows=${P50SIM_GRZ_REAL_ROWS:-341}
    python3 - "$P50SIM_GRZ_REAL_MANIFEST" "$real_rows" "$work/real.manifest" "$work/real.map" <<'PY'
import json, os, sys
manifest = json.load(open(sys.argv[1]))
authority = manifest["input_authority"]
root = authority.get("corpus_root", "")
rows = int(sys.argv[2])
selected = authority["selected_inputs"][:rows]
if len(selected) != rows:
    raise SystemExit("manifest has only %d selected inputs" % len(selected))
paths = [os.path.join(root, entry["source_relative"]) for entry in selected]
missing = [path for path in paths if not os.path.isfile(path)]
if missing:
    raise SystemExit("missing input " + missing[0])
open(sys.argv[3], "w").write("".join(path + "\n" for path in paths))
open(sys.argv[4], "w").write("cardinality=1\n" + "".join("0\n" for _ in paths))
PY
    ICECC_P50_PROFILE=GRZ_RESIDUAL "$sim" --batch-manifest "$work/real.manifest" \
        --batch-assignment-map "$work/real.map" --batch-allow-repeated-inputs 1 \
        --batch-output "$work/real-grz.jsonl"
    test "$(wc -l < "$work/real-grz.jsonl")" -eq "$real_rows"
    real_compare() {
        python3 - "$1" "$2" <<'PY'
import json, sys
expected = [json.loads(line) for line in open(sys.argv[1])]
got = [json.loads(line) for line in open(sys.argv[2])]
fields = ("tu_seq", "rel_seq", "raw_bytes", "raw_digest", "encoded_source_bytes",
          "c_to_f_bytes", "f_to_c_bytes", "state_before_digest", "state_digest",
          "transaction_digest")
for index, (want, have) in enumerate(zip(expected, got)):
    for field in fields:
        assert have.get(field) == want.get(field), (index, field, want.get(field), have.get(field))
PY
    }
    if [ -n "${P50SIM_GRZ_REAL_EXPECTED:-}" ]; then
        real_compare "$P50SIM_GRZ_REAL_EXPECTED" "$work/real-grz.jsonl"
    fi
    if [ "${P50SIM_GRZ_REAL_DETERMINISM:-0}" = 1 ]; then
        ICECC_P50_PROFILE=GRZ_RESIDUAL "$sim" --batch-manifest "$work/real.manifest" \
            --batch-assignment-map "$work/real.map" --batch-allow-repeated-inputs 1 \
            --batch-output "$work/real-grz-2.jsonl"
        test "$(wc -l < "$work/real-grz-2.jsonl")" -eq "$real_rows"
        real_compare "$work/real-grz.jsonl" "$work/real-grz-2.jsonl"
    fi
fi
