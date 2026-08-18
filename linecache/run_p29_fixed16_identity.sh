#!/usr/bin/env bash
# Foreground, resumable corrected-P29 identity pass over the fixed-16 manifests.

set -euo pipefail

SCRIPT_PATH=${BASH_SOURCE[0]-$0}
SCRIPT_DIR=$(cd -- "$(dirname -- "$SCRIPT_PATH")" && pwd)

: "${RUN_ROOT:?set RUN_ROOT to a new retained fixed-16 directory}"
MANIFEST_ROOT=${MANIFEST_ROOT:-/home/ttuser/ictmp}
SOURCE_INVENTORY=${SOURCE_INVENTORY:-$SCRIPT_DIR/fixed16-manifest-sources.tsv}
IDENTITY_RUNNER=${IDENTITY_RUNNER:-$SCRIPT_DIR/run_p29_prefix_identity.sh}
SUMMARIZER=${SUMMARIZER:-$SCRIPT_DIR/summarize_p29_fixed16_identity.py}
P29_BIN=${P29_BIN:-/home/ttuser/issue16-p29-prefix-state/bin/codec50-stable-root-final-l23}
P29_SOURCE=${P29_SOURCE:-$SCRIPT_DIR/codec50.cpp}
EXPECTED_CELLS=${EXPECTED_CELLS:-16}
CORES=${CORES:-0-31}
P29_WORKERS=${P29_WORKERS:-16}
GROUP_TUS=${GROUP_TUS:-112}
export OMP_NUM_THREADS=${OMP_NUM_THREADS:-1}
export GROUP_TUS

for path in "$SOURCE_INVENTORY" "$IDENTITY_RUNNER" "$SUMMARIZER" "$P29_BIN" "$P29_SOURCE"; do
    [[ -f "$path" ]] || { printf 'missing required input: %s\n' "$path" >&2; exit 1; }
done
[[ -x "$IDENTITY_RUNNER" && -x "$P29_BIN" ]] || {
    printf 'identity runner or P29 binary is not executable\n' >&2
    exit 1
}

inventory_rows=$(awk -F '\t' 'NR > 1 {n++} END {print n+0}' "$SOURCE_INVENTORY")
[[ "$inventory_rows" -eq "$EXPECTED_CELLS" ]] || {
    printf 'expected %s source rows, found %s\n' "$EXPECTED_CELLS" "$inventory_rows" >&2
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

manifest = Path(sys.argv[1])
paths = [Path(line) for line in manifest.read_text().splitlines()]
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
ledger="$RUN_ROOT/fixed16-ledger.tsv"
generate_ledger "$RUN_ROOT/fixed16-ledger.tsv.new"
if [[ -f "$ledger" ]] && ! cmp -s "$ledger" "$RUN_ROOT/fixed16-ledger.tsv.new"; then
    printf 'fixed-16 source ledger differs from retained run\n' >&2
    diff -u "$ledger" "$RUN_ROOT/fixed16-ledger.tsv.new" >&2 || true
    exit 1
fi
mv "$RUN_ROOT/fixed16-ledger.tsv.new" "$ledger"

tooling="$RUN_ROOT/matrix-tooling.sha256"
sha256sum "$SCRIPT_PATH" "$IDENTITY_RUNNER" "$SUMMARIZER" "$SOURCE_INVENTORY" \
    "$P29_SOURCE" "$P29_BIN" > "$RUN_ROOT/matrix-tooling.sha256.new"
if [[ -f "$tooling" ]] && ! cmp -s "$tooling" "$RUN_ROOT/matrix-tooling.sha256.new"; then
    printf 'fixed-16 tooling differs from retained run\n' >&2
    diff -u "$tooling" "$RUN_ROOT/matrix-tooling.sha256.new" >&2 || true
    exit 1
fi
mv "$RUN_ROOT/matrix-tooling.sha256.new" "$tooling"

config="$RUN_ROOT/matrix.config"
{
    printf 'manifest_root=%s\n' "$MANIFEST_ROOT"
    printf 'expected_cells=%s\n' "$EXPECTED_CELLS"
    printf 'cores=%s\n' "$CORES"
    printf 'p29_workers=%s\n' "$P29_WORKERS"
    printf 'group_tus=%s\n' "$GROUP_TUS"
    printf 'omp_num_threads=%s\n' "$OMP_NUM_THREADS"
} > "$RUN_ROOT/matrix.config.new"
if [[ -f "$config" ]] && ! cmp -s "$config" "$RUN_ROOT/matrix.config.new"; then
    printf 'fixed-16 execution configuration differs from retained run\n' >&2
    diff -u "$config" "$RUN_ROOT/matrix.config.new" >&2 || true
    exit 1
fi
mv "$RUN_ROOT/matrix.config.new" "$config"

meta="$RUN_ROOT/matrix.meta"
if [[ ! -f "$meta" ]]; then
    {
        printf 'host=%s\n' "$(hostname)"
        printf 'started_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } > "$meta"
fi

ordinal=0
while IFS=$'\t' read -r id name manifest_path tu_count raw_bytes manifest_sha; do
    [[ "$id" != id ]] || continue
    ordinal=$((ordinal + 1))
    cell_run="$RUN_ROOT/cells/$id"
    if [[ -f "$cell_run/prefix-identity.json" ]]; then
        printf '[%s/%s] already complete: %s (%s)\n' "$ordinal" "$EXPECTED_CELLS" "$name" "$id"
        continue
    fi
    if [[ -e "$cell_run" ]]; then
        printf 'incomplete fixed-16 cell must be inspected: %s\n' "$cell_run" >&2
        exit 1
    fi
    printf '[%s/%s] corrected P29: %s (%s), %s TUs / %s raw bytes\n' \
        "$ordinal" "$EXPECTED_CELLS" "$name" "$id" "$tu_count" "$raw_bytes"
    RUN_DIR="$cell_run" SOURCE_MANIFEST="$manifest_path" P29_BIN="$P29_BIN" \
        P29_SOURCE="$P29_SOURCE" CORES="$CORES" \
        PREFIX_TUS="$GROUP_TUS" P29_WORKERS="$P29_WORKERS" KEEP_INPUTS=1 \
        "$IDENTITY_RUNNER"
done < "$ledger"

generate_ledger "$RUN_ROOT/fixed16-ledger.final.tsv"
cmp "$ledger" "$RUN_ROOT/fixed16-ledger.final.tsv"
python3 "$SUMMARIZER" --run-root "$RUN_ROOT" --expected-cells "$EXPECTED_CELLS" \
    > "$RUN_ROOT/summary.stdout"
printf 'completed_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$meta"
printf 'corrected P29 fixed-16 matrix complete: %s\n' "$RUN_ROOT"
