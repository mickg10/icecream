#!/usr/bin/env bash
# Foreground, resumable current-GRZ and whole-program-zstd native holdout sweep.

set -euo pipefail

SCRIPT_PATH=${BASH_SOURCE[0]-$0}
SCRIPT_DIR=$(cd -- "$(dirname -- "$SCRIPT_PATH")" && pwd)

: "${RUN_ROOT:?set RUN_ROOT to a new retained native-nine directory}"

MANIFEST_ROOT=${MANIFEST_ROOT:-/home/ttuser/ictmp}
SOURCE_INVENTORY=${SOURCE_INVENTORY:-$SCRIPT_DIR/native9-manifest-sources.tsv}
GRZ_BIN=${GRZ_BIN:-/home/ttuser/issue16-selector-v1/tools/grz2g-selector}
GRZ_SOURCE=${GRZ_SOURCE:-/home/ttuser/issue16-selector-v1/tools/grz2g-selector.cpp}
CORES=${CORES:-0-31}
CODEC_THREADS=${CODEC_THREADS:-16}
ZSTD_THREADS=${ZSTD_THREADS:-16}
EXPECTED_CELLS=${EXPECTED_CELLS:-9}

for path in "$SOURCE_INVENTORY" "$GRZ_BIN" "$GRZ_SOURCE"; do
    [[ -f "$path" ]] || { printf 'missing native-nine input: %s\n' "$path" >&2; exit 1; }
done
[[ -x "$GRZ_BIN" ]] || { printf 'GRZ binary is not executable: %s\n' "$GRZ_BIN" >&2; exit 1; }
inventory_rows=$(awk -F '\t' 'NR > 1 {n++} END {print n+0}' "$SOURCE_INVENTORY")
[[ "$inventory_rows" -eq "$EXPECTED_CELLS" ]] || {
    printf 'expected %s native holdouts, found %s\n' "$EXPECTED_CELLS" "$inventory_rows" >&2
    exit 1
}

generate_ledger() {
    local output=$1
    local manifest actual_tus actual_raw manifest_sha
    printf 'id\tname\tmanifest_path\ttu_count\traw_bytes\tmanifest_sha256\n' > "$output"
    while IFS=$'\t' read -r id name expected_tus expected_raw; do
        [[ "$id" != id ]] || continue
        manifest="$MANIFEST_ROOT/$id/manifest.txt"
        [[ -f "$manifest" ]] || { printf 'missing manifest: %s\n' "$manifest" >&2; return 1; }
        actual_tus=$(wc -l < "$manifest")
        actual_raw=$(python3 - "$manifest" <<'PY'
from pathlib import Path
import sys

paths = [Path(line) for line in Path(sys.argv[1]).read_text().splitlines()]
if not paths or len(paths) != len(set(paths)):
    raise SystemExit("manifest is empty or contains duplicate paths")
total = 0
for path in paths:
    if not path.is_file():
        raise SystemExit(f"missing TU: {path}")
    total += path.stat().st_size
print(total)
PY
        )
        [[ "$actual_tus" -eq "$expected_tus" ]] || {
            printf '%s TU count differs: %s != %s\n' "$id" "$actual_tus" "$expected_tus" >&2
            return 1
        }
        [[ "$actual_raw" -eq "$expected_raw" ]] || {
            printf '%s raw extent differs: %s != %s\n' "$id" "$actual_raw" "$expected_raw" >&2
            return 1
        }
        manifest_sha=$(sha256sum "$manifest" | cut -d ' ' -f 1)
        printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$id" "$name" "$manifest" "$actual_tus" "$actual_raw" "$manifest_sha" >> "$output"
    done < "$SOURCE_INVENTORY"
}

mkdir -p "$RUN_ROOT/cells"
generate_ledger "$RUN_ROOT/native9-ledger.tsv.new"
if [[ -f "$RUN_ROOT/native9-ledger.tsv" ]] && \
   ! cmp -s "$RUN_ROOT/native9-ledger.tsv" "$RUN_ROOT/native9-ledger.tsv.new"; then
    printf 'native-nine source ledger differs from retained run\n' >&2
    diff -u "$RUN_ROOT/native9-ledger.tsv" "$RUN_ROOT/native9-ledger.tsv.new" >&2 || true
    exit 1
fi
mv "$RUN_ROOT/native9-ledger.tsv.new" "$RUN_ROOT/native9-ledger.tsv"

sha256sum "$SCRIPT_PATH" "$SOURCE_INVENTORY" "$GRZ_BIN" "$GRZ_SOURCE" \
    > "$RUN_ROOT/tooling.sha256.new"
if [[ -f "$RUN_ROOT/tooling.sha256" ]] && \
   ! cmp -s "$RUN_ROOT/tooling.sha256" "$RUN_ROOT/tooling.sha256.new"; then
    printf 'native-nine tooling differs from retained run\n' >&2
    diff -u "$RUN_ROOT/tooling.sha256" "$RUN_ROOT/tooling.sha256.new" >&2 || true
    exit 1
fi
mv "$RUN_ROOT/tooling.sha256.new" "$RUN_ROOT/tooling.sha256"

{
    printf 'cores=%s\n' "$CORES"
    printf 'codec_threads=%s\n' "$CODEC_THREADS"
    printf 'zstd_threads=%s\n' "$ZSTD_THREADS"
    printf 'expected_cells=%s\n' "$EXPECTED_CELLS"
    printf 'zstd_version=%s\n' "$(zstd --version)"
} > "$RUN_ROOT/config.new"
if [[ -f "$RUN_ROOT/config" ]] && ! cmp -s "$RUN_ROOT/config" "$RUN_ROOT/config.new"; then
    printf 'native-nine configuration differs from retained run\n' >&2
    diff -u "$RUN_ROOT/config" "$RUN_ROOT/config.new" >&2 || true
    exit 1
fi
mv "$RUN_ROOT/config.new" "$RUN_ROOT/config"

if [[ ! -f "$RUN_ROOT/run.meta" ]]; then
    {
        printf 'host=%s\n' "$(hostname)"
        printf 'started_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } > "$RUN_ROOT/run.meta"
fi

policy=(-m g2 -K 256 -s 6 -t 21 -l 4 -k 5 -b 8 -j "$CODEC_THREADS"
        --gtu 112 --graw 512 --gadd 128 --hist 1024)

ordinal=0
while IFS=$'\t' read -r id name manifest_path total_tus total_raw manifest_sha; do
    [[ "$id" != id ]] || continue
    ordinal=$((ordinal + 1))
    cell="$RUN_ROOT/cells/$id"
    if [[ -f "$cell/measurement.tsv" ]]; then
        printf '[%s/%s] already complete: %s (%s)\n' "$ordinal" "$EXPECTED_CELLS" "$name" "$id"
        continue
    fi
    [[ ! -e "$cell" ]] || { printf 'incomplete native-nine cell must be inspected: %s\n' "$cell" >&2; exit 1; }
    mkdir -p "$cell"
    cp "$manifest_path" "$cell/manifest.txt"
    printf '[%s/%s] GRZ/zstd holdout: %s (%s), %s TUs / %s raw bytes\n' \
        "$ordinal" "$EXPECTED_CELLS" "$name" "$id" "$total_tus" "$total_raw"

    # The positional parameters are intentionally expanded by the inner shell.
    # shellcheck disable=SC2016
    /usr/bin/time -v -o "$cell/concatenate.time" \
        bash -c 'tr "\n" "\0" < "$1" | xargs -0 cat > "$2"' \
        native9-concatenate "$cell/manifest.txt" "$cell/cell.ii"
    [[ "$(stat -Lc %s "$cell/cell.ii")" -eq "$total_raw" ]] || {
        printf 'concatenated raw extent differs: %s\n' "$id" >&2
        exit 1
    }
    taskset -c "$CORES" "$GRZ_BIN" tu "$cell/manifest.txt" "$cell/cell.tu" \
        > "$cell/tu.stdout" 2> "$cell/tu.stderr"

    /usr/bin/time -v -o "$cell/grz-encode.time" \
        taskset -c "$CORES" "$GRZ_BIN" enc "$cell/cell.ii" "$cell/cell.grz" \
        -u "$cell/cell.tu" "${policy[@]}" --curve "$cell/grz-curve.tsv" \
        > "$cell/grz-encode.stdout" 2> "$cell/grz-encode.stderr"
    /usr/bin/time -v -o "$cell/grz-decode.time" \
        taskset -c "$CORES" "$GRZ_BIN" dec "$cell/cell.grz" "$cell/replay.ii" -j 1 \
        > "$cell/grz-decode.stdout" 2> "$cell/grz-decode.stderr"
    cmp "$cell/cell.ii" "$cell/replay.ii"

    /usr/bin/time -v -o "$cell/z19.time" \
        taskset -c "$CORES" zstd -19 --long=31 -T"$ZSTD_THREADS" -q -f \
        "$cell/cell.ii" -o "$cell/cell.z19.zst"
    zstd -d --long=31 -q -c "$cell/cell.z19.zst" | cmp -s "$cell/cell.ii" -
    /usr/bin/time -v -o "$cell/z6.time" \
        taskset -c "$CORES" zstd -6 --long=31 -T"$ZSTD_THREADS" -q -f \
        "$cell/cell.ii" -o "$cell/cell.z6.zst"
    zstd -d --long=31 -q -c "$cell/cell.z6.zst" | cmp -s "$cell/cell.ii" -

    tu100_raw=$(head -n 100 "$cell/manifest.txt" | tr '\n' '\0' | \
        xargs -0 stat -Lc %s | awk '{sum += $1} END {printf "%.0f", sum}')
    tu200_raw=$(head -n 200 "$cell/manifest.txt" | tr '\n' '\0' | \
        xargs -0 stat -Lc %s | awk '{sum += $1} END {printf "%.0f", sum}')
    head -c "$tu100_raw" "$cell/cell.ii" | \
        taskset -c "$CORES" zstd -6 --long=31 -T"$ZSTD_THREADS" -q -f \
        -o "$cell/tu100.z6.zst"
    zstd -d --long=31 -q -c "$cell/tu100.z6.zst" | \
        cmp -n "$tu100_raw" "$cell/cell.ii" -
    head -c "$tu200_raw" "$cell/cell.ii" | \
        taskset -c "$CORES" zstd -6 --long=31 -T"$ZSTD_THREADS" -q -f \
        -o "$cell/tu200.z6.zst"
    zstd -d --long=31 -q -c "$cell/tu200.z6.zst" | \
        cmp -n "$tu200_raw" "$cell/cell.ii" -

    grz_wire=$(stat -Lc %s "$cell/cell.grz")
    z19_wire=$(stat -Lc %s "$cell/cell.z19.zst")
    z6_wire=$(stat -Lc %s "$cell/cell.z6.zst")
    tu100_z6_wire=$(stat -Lc %s "$cell/tu100.z6.zst")
    tu200_z6_wire=$(stat -Lc %s "$cell/tu200.z6.zst")
    curve_tus=$(awk -F '\t' 'NR > 1 {last=$3} END {print last+0}' "$cell/grz-curve.tsv")
    [[ "$curve_tus" -eq "$total_tus" ]] || {
        printf 'GRZ curve extent differs: %s\n' "$id" >&2
        exit 1
    }
    sha256sum "$cell/cell.ii" "$cell/replay.ii" "$cell/cell.tu" "$cell/cell.grz" \
        "$cell/cell.z19.zst" "$cell/cell.z6.zst" "$cell/tu100.z6.zst" \
        "$cell/tu200.z6.zst" "$cell/grz-curve.tsv" \
        > "$cell/exact.sha256"
    {
        printf 'id\tname\ttus\traw_bytes\tmanifest_sha256\tgrz_wire_bytes\tz19_long_bytes\tz6_long_bytes\ttu100_raw_bytes\ttu100_z6_long_bytes\ttu200_raw_bytes\ttu200_z6_long_bytes\tgrz_exact\tz19_exact\tz6_exact\n'
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\ttrue\ttrue\ttrue\n' \
            "$id" "$name" "$total_tus" "$total_raw" "$manifest_sha" \
            "$grz_wire" "$z19_wire" "$z6_wire" "$tu100_raw" "$tu100_z6_wire" \
            "$tu200_raw" "$tu200_z6_wire"
    } > "$cell/measurement.tsv"
    rm -f -- "$cell/cell.ii" "$cell/replay.ii"
done < "$RUN_ROOT/native9-ledger.tsv"

generate_ledger "$RUN_ROOT/native9-ledger.final.tsv"
cmp "$RUN_ROOT/native9-ledger.tsv" "$RUN_ROOT/native9-ledger.final.tsv"
printf 'completed_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$RUN_ROOT/run.meta"
printf 'current-GRZ native-nine holdout complete: %s\n' "$RUN_ROOT"
