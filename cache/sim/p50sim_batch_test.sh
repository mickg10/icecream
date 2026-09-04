#!/bin/sh
set -eu

sim=${P50SIM_BINARY:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/.p50sim.bin}
sim_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source_root=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$sim_dir/../.." && pwd)}
if [ ! -x "$sim" ]; then
    echo "p50sim_batch_test: product batch binary is not built; skipping" >&2
    exit 77
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/p50sim-batch-test.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir "$work/fingerprint-cache"
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
assert all(type(row.get("f_apply_materialize_ns")) is int and
           row["f_apply_materialize_ns"] >= 0
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

ICECC_P50_PROFILE=P29V1 "$sim" --batch-manifest "$work/manifest" \
    --batch-assignment-map "$work/map1" --batch-output "$work/p29v1.jsonl" \
    --p29-fingerprint-cache-directory "$work/fingerprint-cache" \
    --p29-inner-cf-output "$work/p29v1.cf" \
    --p29-inner-fc-output "$work/p29v1.fc"
python3 - "$work/p29v1.jsonl" "$work/p29v1.cf" "$work/p29v1.fc" <<'PY'
import json, sys
rows = [json.loads(line) for line in open(sys.argv[1])]
assert len(rows) == 2
assert [row["profile"] for row in rows] == ["P29V1", "P29V1"]
assert [row["tu_seq"] for row in rows] == [0, 1]
assert [row["rel_seq"] for row in rows] == [0, 1]
assert rows[1]["state_before_digest"] == rows[0]["state_digest"]
assert all(row["p29v1_interner_reserved_bytes"] > 0 and
           row["p29v1_interner_committed_bytes"] > 0
           for row in rows)
assert all(row["negotiated_profile_mask"] == 1 and
           row["system_source_reuse"] is True for row in rows)
def frames(path):
    data = open(path, "rb").read()
    result = []
    at = 0
    while at < len(data):
        assert len(data) - at >= 5
        kind = data[at]
        length = int.from_bytes(data[at + 1:at + 5], "little")
        at += 5
        assert length <= len(data) - at
        result.append((kind, data[at:at + length]))
        at += length
    return result
cf = frames(sys.argv[2])
fc = frames(sys.argv[3])
assert sum(kind == 0xfe for kind, _ in cf) == 2
assert sum(kind == 0xfe for kind, _ in fc) == 2
PY

# Permanent transport regression control: without TCP_NODELAY the interactive
# dialogue falls onto Linux's roughly 40 ms delayed-ACK timer.  The median
# across 200 tiny TUs is deliberately robust to process startup and outliers.
python3 - "$work/tiny.ii" "$work/tiny.manifest" "$work/tiny.map" <<'PY'
import sys
open(sys.argv[1], "wb").write(b"x" * 1024)
open(sys.argv[2], "w").write((sys.argv[1] + "\n") * 200)
open(sys.argv[3], "w").write("cardinality=1\n" + "0\n" * 200)
PY
ICECC_P50_PROFILE=P29V1 "$sim" --batch-manifest "$work/tiny.manifest" \
    --batch-assignment-map "$work/tiny.map" --batch-output "$work/tiny.jsonl" \
    --batch-allow-repeated-inputs 1 \
    --p29-fingerprint-cache-directory "$work/fingerprint-cache"
python3 - "$work/tiny.jsonl" <<'PY'
import json, statistics, sys
rows = [json.loads(line) for line in open(sys.argv[1])]
assert len(rows) == 200
median = statistics.median(row["simulator_execution_ns"] for row in rows)
assert median < 2_000_000, median
PY

# Product-path M1 witness.  The standalone codec test intentionally closes
# its entropy streams; a live route does not.  Capture the product endpoint's
# exact inner messages for the retained 11-input corpus and bind them to the
# open-final-entropy research hashes without duplicating a 3 MiB golden.
phase0_inputs="$source_root/unittests/codec_golden/inputs"
: > "$work/phase0.manifest"
for index in 00 01 02 03 04 05 06 07; do
    printf '%s\n' "$phase0_inputs/real-$index.ii" >> "$work/phase0.manifest"
done
for name in synthetic-repeat synthetic-unique synthetic-blank-heavy; do
    printf '%s\n' "$phase0_inputs/$name.ii" >> "$work/phase0.manifest"
done
{
    printf '%s\n' cardinality=1
    for ignored in 00 01 02 03 04 05 06 07 repeat unique blank; do
        printf '%s\n' 0
    done
} > "$work/phase0.map"
ICECC_P50_PROFILE=P29V1 "$sim" \
    --batch-manifest "$work/phase0.manifest" \
    --batch-assignment-map "$work/phase0.map" \
    --batch-output "$work/phase0.jsonl" \
    --p29-fingerprint-cache-directory "$work/fingerprint-cache" \
    --p29-inner-cf-output "$work/phase0.cf" \
    --p29-inner-fc-output "$work/phase0.fc"
test "$(wc -l < "$work/phase0.jsonl")" -eq 11
test "$(sha256sum "$work/phase0.cf" | awk '{print $1}')" = \
    ebaa60acf1abfd2afbbf7892927479680df1ada2fada9edeea5613fba16a828b
test "$(sha256sum "$work/phase0.fc" | awk '{print $1}')" = \
    83616f1cf7f130ebdc6b6303d2dd3ceb6bfc20ed0f5d1dd513f2c41f8ddf8960
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
