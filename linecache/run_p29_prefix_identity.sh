#!/usr/bin/env bash
# Reproduce P29 first-group identity on one verified ii-matrix cell.

set -euo pipefail

SCRIPT_PATH=${BASH_SOURCE[0]-$0}
SCRIPT_DIR=$(cd -- "$(dirname -- "$SCRIPT_PATH")" && pwd)

: "${RUN_DIR:?set RUN_DIR to a new retained-result directory}"

CELL_DIR=${CELL_DIR:-}
SOURCE_MANIFEST=${SOURCE_MANIFEST:-}
PREPARE_CELL=${PREPARE_CELL:-}

P29_BIN=${P29_BIN:-/home/ttuser/issue16-p29-prefix-state/bin/codec50-prefix}
P29_SOURCE=${P29_SOURCE:-}
P29_COMMIT=${P29_COMMIT:-56c1744d1ef8c3ee6ce0c785e80b2ea4123bb720}
LIBBSC_COMMIT=${LIBBSC_COMMIT:-baffa62c70b6ebbecc9af14ce550e965ea247680}
CORES=${CORES:-16-31}
GROUP_TUS=${GROUP_TUS:-112}
PREFIX_TUS=${PREFIX_TUS:-$GROUP_TUS}
P29_WORKERS=${P29_WORKERS:-16}
KEEP_INPUTS=${KEEP_INPUTS:-0}
export OMP_NUM_THREADS=${OMP_NUM_THREADS:-1}

[[ -x "$P29_BIN" ]] || { printf 'P29 binary is not executable: %s\n' "$P29_BIN" >&2; exit 1; }
if [[ -n "$CELL_DIR" && -n "$SOURCE_MANIFEST" ]] || [[ -z "$CELL_DIR" && -z "$SOURCE_MANIFEST" ]]; then
    printf 'set exactly one of CELL_DIR or SOURCE_MANIFEST\n' >&2
    exit 1
fi
if [[ -n "$CELL_DIR" ]]; then
    [[ -n "$PREPARE_CELL" ]] || { printf 'CELL_DIR mode requires PREPARE_CELL\n' >&2; exit 1; }
    [[ -f "$CELL_DIR/corpus.json" && -f "$CELL_DIR/manifest.tsv" ]] || {
        printf 'incomplete corpus cell: %s\n' "$CELL_DIR" >&2
        exit 1
    }
else
    [[ -f "$SOURCE_MANIFEST" ]] || { printf 'missing source manifest: %s\n' "$SOURCE_MANIFEST" >&2; exit 1; }
fi
[[ ! -e "$RUN_DIR" ]] || { printf 'run directory already exists: %s\n' "$RUN_DIR" >&2; exit 1; }
mkdir -p "$RUN_DIR/extracted" "$RUN_DIR/input" "$RUN_DIR/full" "$RUN_DIR/prefix"

exec > >(tee "$RUN_DIR/runner.stdout") 2> >(tee "$RUN_DIR/runner.stderr" >&2)

manifest="$RUN_DIR/input/manifest.txt"
if [[ -n "$CELL_DIR" ]]; then
    archive=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["payload"]["path"])' "$CELL_DIR/corpus.json")
    printf 'extracting %s\n' "$CELL_DIR/$archive"
    zstd -d --long=31 -q -c "$CELL_DIR/$archive" | tar -xf - -C "$RUN_DIR/extracted"
    python3 "$PREPARE_CELL" \
        --cell-dir "$CELL_DIR" \
        --extracted-dir "$RUN_DIR/extracted" \
        --output-dir "$RUN_DIR/input" \
        > "$RUN_DIR/input/prepare.stdout"
else
    cp -- "$SOURCE_MANIFEST" "$manifest"
fi

total_tus=$(wc -l < "$manifest")
prefix_tus=$(( total_tus < PREFIX_TUS ? total_tus : PREFIX_TUS ))
if (( prefix_tus != total_tus && prefix_tus % GROUP_TUS != 0 )); then
    printf 'prefix TU count must end at a complete group boundary: prefix=%s group=%s\n' \
        "$prefix_tus" "$GROUP_TUS" >&2
    exit 1
fi
if (( prefix_tus < total_tus )); then
    prefix_mode=suffix-blind
    prefix_entropy_args=(--open-final-entropy)
else
    prefix_mode=complete-program
    prefix_entropy_args=()
fi
head -n "$prefix_tus" "$manifest" > "$RUN_DIR/input/manifest.prefix.txt"

P29_COMMON=(
    --z 3 --mixed-regions --byte-array-lines --direct-ordinals
    --compressed-blobs --blob-threads 8 --blob-lazy-fallback --mo-factor
    --s1-max-chain 1024 --blob-z 9 --blob-zstd-workers 4
    --blob-zstd-job-mib 5 --blob-zstd-overlap-log 3
)

printf 'forming complete raw-plane plan (%s TUs)\n' "$total_tus"
/usr/bin/time -v -o "$RUN_DIR/full/plan.time" \
    taskset -c "$CORES" "$P29_BIN" --manifest "$manifest" "${P29_COMMON[@]}" \
    --mixed-dump-prefix "$RUN_DIR/full/plan" \
    > "$RUN_DIR/full/plan.stdout" 2> "$RUN_DIR/full/plan.stderr"

printf 'running complete stable-Root P29+BSC\n'
/usr/bin/time -v -o "$RUN_DIR/full/grouped.time" \
    taskset -c "$CORES" "$P29_BIN" --manifest "$manifest" "${P29_COMMON[@]}" \
    --literal-group-prefix "$RUN_DIR/full/plan" --literal-group-tus "$GROUP_TUS" \
    --stable-root-tags \
    --literal-group-workers "$P29_WORKERS" --literal-group-skip-zstd10 \
    --literal-group-wire "$RUN_DIR/full/literal.wire" \
    --curve-tsv "$RUN_DIR/full/curve.tsv" \
    --component-curve-tsv "$RUN_DIR/full/components.tsv" \
    > "$RUN_DIR/full/grouped.stdout" 2> "$RUN_DIR/full/grouped.stderr"

printf 'forming %s raw-plane plan (%s TUs)\n' "$prefix_mode" "$prefix_tus"
/usr/bin/time -v -o "$RUN_DIR/prefix/plan.time" \
    taskset -c "$CORES" "$P29_BIN" --manifest "$RUN_DIR/input/manifest.prefix.txt" \
    "${P29_COMMON[@]}" --mixed-dump-prefix "$RUN_DIR/prefix/plan" \
    > "$RUN_DIR/prefix/plan.stdout" 2> "$RUN_DIR/prefix/plan.stderr"

printf 'running %s prefix P29+BSC\n' "$prefix_mode"
/usr/bin/time -v -o "$RUN_DIR/prefix/grouped.time" \
    taskset -c "$CORES" "$P29_BIN" --manifest "$RUN_DIR/input/manifest.prefix.txt" \
    "${P29_COMMON[@]}" \
    --literal-group-prefix "$RUN_DIR/prefix/plan" --literal-group-tus "$GROUP_TUS" \
    --stable-root-tags "${prefix_entropy_args[@]}" \
    --literal-group-workers "$P29_WORKERS" --literal-group-skip-zstd10 \
    --literal-group-wire "$RUN_DIR/prefix/literal.wire" \
    --curve-tsv "$RUN_DIR/prefix/curve.tsv" \
    --component-curve-tsv "$RUN_DIR/prefix/components.tsv" \
    > "$RUN_DIR/prefix/grouped.stdout" 2> "$RUN_DIR/prefix/grouped.stderr"

python3 "$SCRIPT_DIR/verify_p29_prefix_identity.py" \
    --full-curve "$RUN_DIR/full/curve.tsv" \
    --prefix-curve "$RUN_DIR/prefix/curve.tsv" \
    --full-components "$RUN_DIR/full/components.tsv" \
    --prefix-components "$RUN_DIR/prefix/components.tsv" \
    --full-literal-wire "$RUN_DIR/full/literal.wire" \
    --prefix-literal-wire "$RUN_DIR/prefix/literal.wire" \
    --full-plan-prefix "$RUN_DIR/full/plan" \
    --prefix-plan-prefix "$RUN_DIR/prefix/plan" \
    --prefix-tus "$prefix_tus" \
    --group-tus "$GROUP_TUS" \
    --output "$RUN_DIR/prefix-identity.json" \
    > "$RUN_DIR/verify.stdout"

tooling=("$P29_BIN" "$SCRIPT_PATH" "$SCRIPT_DIR/verify_p29_prefix_identity.py")
if [[ -n "$PREPARE_CELL" ]]; then tooling+=("$PREPARE_CELL"); fi
if [[ -n "$P29_SOURCE" ]]; then
    [[ -f "$P29_SOURCE" ]] || { printf 'missing P29 source: %s\n' "$P29_SOURCE" >&2; exit 1; }
    tooling+=("$P29_SOURCE")
fi
sha256sum "${tooling[@]}" > "$RUN_DIR/tooling.sha256"
{
    printf 'host=%s\n' "$(hostname)"
    printf 'cell_dir=%s\n' "$CELL_DIR"
    printf 'source_manifest=%s\n' "$SOURCE_MANIFEST"
    printf 'total_tus=%s\n' "$total_tus"
    printf 'prefix_tus=%s\n' "$prefix_tus"
    printf 'prefix_mode=%s\n' "$prefix_mode"
    printf 'group_tus=%s\n' "$GROUP_TUS"
    printf 'cores=%s\n' "$CORES"
    printf 'omp_num_threads=%s\n' "$OMP_NUM_THREADS"
    printf 'p29_commit=%s\n' "$P29_COMMIT"
    printf 'libbsc_commit=%s\n' "$LIBBSC_COMMIT"
    printf 'zstd_version=%s\n' "$(zstd --version)"
    printf 'completed_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$RUN_DIR/run.meta"

if [[ "$KEEP_INPUTS" == 0 && -n "$CELL_DIR" ]]; then
    rm -f -- "$RUN_DIR/input/cell.ii"
    find "$RUN_DIR/extracted" -depth -delete
fi
printf 'prefix identity PASS: %s\n' "$RUN_DIR"
