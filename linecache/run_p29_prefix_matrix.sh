#!/usr/bin/env bash
# Foreground, resumable corrected-P29 replay over one frozen corpus-cell ledger.

set -euo pipefail

SCRIPT_PATH=${BASH_SOURCE[0]-$0}
SCRIPT_DIR=$(cd -- "$(dirname -- "$SCRIPT_PATH")" && pwd)

MATRIX_ROOT=${MATRIX_ROOT:-/home/ttuser/ictmp/ii-matrix}
: "${RUN_ROOT:?set RUN_ROOT to the retained corrected-P29 matrix directory}"
: "${CELL_LEDGER:?set CELL_LEDGER to the exact verified-cell TSV}"

P29_BIN=${P29_BIN:-/home/ttuser/issue16-p29-prefix-state/bin/codec50-stable-root-final-l23}
P29_SOURCE=${P29_SOURCE:-$SCRIPT_DIR/codec50.cpp}
PREPARE_CELL=${PREPARE_CELL:-/home/ttuser/issue16-selector-v1/tools/prepare_selector_cell.py}
LEDGER_VERIFIER=${LEDGER_VERIFIER:-/home/ttuser/issue16-selector-v1/tools/freeze_selector_ledger.py}
IDENTITY_RUNNER=${IDENTITY_RUNNER:-$SCRIPT_DIR/run_p29_prefix_identity.sh}
SUMMARIZER=${SUMMARIZER:-$SCRIPT_DIR/summarize_p29_prefix_matrix.py}
EXPECTED_CELLS=${EXPECTED_CELLS:-44}
CORES=${CORES:-0-15}
P29_WORKERS=${P29_WORKERS:-16}
GROUP_TUS=${GROUP_TUS:-112}
export OMP_NUM_THREADS=${OMP_NUM_THREADS:-1}

for path in "$P29_BIN" "$P29_SOURCE" "$PREPARE_CELL" "$LEDGER_VERIFIER" "$IDENTITY_RUNNER" \
            "$SUMMARIZER" "$CELL_LEDGER"; do
    [[ -f "$path" ]] || { printf 'missing required input: %s\n' "$path" >&2; exit 1; }
done
[[ -x "$P29_BIN" && -x "$IDENTITY_RUNNER" ]] || {
    printf 'P29 binary or identity runner is not executable\n' >&2
    exit 1
}

mkdir -p "$RUN_ROOT/cells"
actual_ledger="$RUN_ROOT/verified-cells.tsv"
python3 "$LEDGER_VERIFIER" --matrix-root "$MATRIX_ROOT" \
    --expected-cells "$EXPECTED_CELLS" --check "$CELL_LEDGER"
cp "$CELL_LEDGER" "$actual_ledger.new"
mv "$actual_ledger.new" "$actual_ledger"

mapfile -t cell_identities < <(awk -F '\t' 'NR > 1 {print $1 "\t" $2}' "$CELL_LEDGER")
[[ ${#cell_identities[@]} -eq "$EXPECTED_CELLS" ]] || {
    printf 'expected %s frozen identities, found %s\n' \
        "$EXPECTED_CELLS" "${#cell_identities[@]}" >&2
    exit 1
}

tooling="$RUN_ROOT/matrix-tooling.sha256"
current_tooling="$RUN_ROOT/matrix-tooling.sha256.new"
sha256sum "$SCRIPT_PATH" "$IDENTITY_RUNNER" "$SUMMARIZER" "$PREPARE_CELL" \
    "$LEDGER_VERIFIER" \
    "$P29_SOURCE" "$P29_BIN" "$CELL_LEDGER" > "$current_tooling"
if [[ -f "$tooling" ]] && ! cmp -s "$tooling" "$current_tooling"; then
    printf 'matrix tooling differs from the existing retained run\n' >&2
    diff -u "$tooling" "$current_tooling" >&2 || true
    exit 1
fi
mv "$current_tooling" "$tooling"

config="$RUN_ROOT/matrix.config"
current_config="$RUN_ROOT/matrix.config.new"
{
    printf 'matrix_root=%s\n' "$MATRIX_ROOT"
    printf 'expected_cells=%s\n' "$EXPECTED_CELLS"
    printf 'cores=%s\n' "$CORES"
    printf 'p29_workers=%s\n' "$P29_WORKERS"
    printf 'group_tus=%s\n' "$GROUP_TUS"
    printf 'omp_num_threads=%s\n' "$OMP_NUM_THREADS"
} > "$current_config"
if [[ -f "$config" ]] && ! cmp -s "$config" "$current_config"; then
    printf 'matrix execution configuration differs from the existing retained run\n' >&2
    diff -u "$config" "$current_config" >&2 || true
    exit 1
fi
mv "$current_config" "$config"

meta="$RUN_ROOT/matrix.meta"
if [[ ! -f "$meta" ]]; then
    {
        printf 'host=%s\n' "$(hostname)"
        printf 'started_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        printf 'matrix_root=%s\n' "$MATRIX_ROOT"
        printf 'expected_cells=%s\n' "$EXPECTED_CELLS"
        printf 'cores=%s\n' "$CORES"
        printf 'p29_workers=%s\n' "$P29_WORKERS"
        printf 'group_tus=%s\n' "$GROUP_TUS"
        printf 'omp_num_threads=%s\n' "$OMP_NUM_THREADS"
    } > "$meta"
fi

ordinal=0
for identity in "${cell_identities[@]}"; do
    ordinal=$((ordinal + 1))
    IFS=$'\t' read -r project profile <<< "$identity"
    cell_dir="$MATRIX_ROOT/$project/$profile"
    cell_run="$RUN_ROOT/cells/$project/$profile"
    if [[ -f "$cell_run/prefix-identity.json" ]]; then
        printf '[%s/%s] already complete: %s/%s\n' \
            "$ordinal" "$EXPECTED_CELLS" "$project" "$profile"
        continue
    fi
    if [[ -e "$cell_run" ]]; then
        printf '[%s/%s] incomplete run must be inspected before resume: %s\n' \
            "$ordinal" "$EXPECTED_CELLS" "$cell_run" >&2
        exit 1
    fi
    printf '[%s/%s] corrected P29: %s/%s\n' \
        "$ordinal" "$EXPECTED_CELLS" "$project" "$profile"
    prefix_tus=$GROUP_TUS
    RUN_DIR="$cell_run" CELL_DIR="$cell_dir" PREPARE_CELL="$PREPARE_CELL" \
        P29_BIN="$P29_BIN" P29_SOURCE="$P29_SOURCE" CORES="$CORES" \
        GROUP_TUS="$GROUP_TUS" PREFIX_TUS="$prefix_tus" \
        P29_WORKERS="$P29_WORKERS" KEEP_INPUTS=0 "$IDENTITY_RUNNER"
done

# Recheck the declared generation after the final cell as well as before the first.  A long
# resumable run must not silently combine cells observed on opposite sides of corpus drift.
python3 "$LEDGER_VERIFIER" --matrix-root "$MATRIX_ROOT" \
    --expected-cells "$EXPECTED_CELLS" --check "$CELL_LEDGER"
python3 "$SUMMARIZER" --run-root "$RUN_ROOT" --expected-cells "$EXPECTED_CELLS" \
    > "$RUN_ROOT/summary.stdout"
printf 'completed_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$meta"
printf 'corrected P29 matrix complete: %s\n' "$RUN_ROOT"
