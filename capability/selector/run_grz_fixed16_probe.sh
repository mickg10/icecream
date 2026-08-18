#!/usr/bin/env bash
# Foreground, resumable current-GRZ probe pass over a fixed-16 P29 manifest ledger.

set -euo pipefail

SCRIPT_PATH=${BASH_SOURCE[0]-$0}
: "${RUN_ROOT:?set RUN_ROOT to a new retained GRZ probe directory}"
: "${FIXED16_LEDGER:?set FIXED16_LEDGER to the corrected P29 fixed-16 ledger}"

GRZ_BIN=${GRZ_BIN:-/home/ttuser/issue16-selector-v1/tools/grz2g-selector}
GRZ_SOURCE=${GRZ_SOURCE:-/home/ttuser/issue16-selector-v1/tools/grz2g-selector.cpp}
CORES=${CORES:-0-15}
EXPECTED_CELLS=${EXPECTED_CELLS:-16}
GROUP_TUS=${GROUP_TUS:-112}

for path in "$FIXED16_LEDGER" "$GRZ_BIN" "$GRZ_SOURCE"; do
    [[ -f "$path" ]] || { printf 'missing required input: %s\n' "$path" >&2; exit 1; }
done
[[ -x "$GRZ_BIN" ]] || { printf 'GRZ binary is not executable: %s\n' "$GRZ_BIN" >&2; exit 1; }
rows=$(awk -F '\t' 'NR > 1 {n++} END {print n+0}' "$FIXED16_LEDGER")
[[ "$rows" -eq "$EXPECTED_CELLS" ]] || {
    printf 'expected %s ledger rows, found %s\n' "$EXPECTED_CELLS" "$rows" >&2
    exit 1
}

mkdir -p "$RUN_ROOT/cells"
cp "$FIXED16_LEDGER" "$RUN_ROOT/fixed16-ledger.tsv.new"
if [[ -f "$RUN_ROOT/fixed16-ledger.tsv" ]] && \
   ! cmp -s "$RUN_ROOT/fixed16-ledger.tsv" "$RUN_ROOT/fixed16-ledger.tsv.new"; then
    printf 'fixed-16 ledger differs from retained GRZ run\n' >&2
    exit 1
fi
mv "$RUN_ROOT/fixed16-ledger.tsv.new" "$RUN_ROOT/fixed16-ledger.tsv"

sha256sum "$SCRIPT_PATH" "$GRZ_BIN" "$GRZ_SOURCE" "$FIXED16_LEDGER" \
    > "$RUN_ROOT/tooling.sha256.new"
if [[ -f "$RUN_ROOT/tooling.sha256" ]] && \
   ! cmp -s "$RUN_ROOT/tooling.sha256" "$RUN_ROOT/tooling.sha256.new"; then
    printf 'GRZ probe tooling differs from retained run\n' >&2
    exit 1
fi
mv "$RUN_ROOT/tooling.sha256.new" "$RUN_ROOT/tooling.sha256"

{
    printf 'cores=%s\n' "$CORES"
    printf 'expected_cells=%s\n' "$EXPECTED_CELLS"
    printf 'group_tus=%s\n' "$GROUP_TUS"
} > "$RUN_ROOT/config.new"
if [[ -f "$RUN_ROOT/config" ]] && ! cmp -s "$RUN_ROOT/config" "$RUN_ROOT/config.new"; then
    printf 'GRZ probe configuration differs from retained run\n' >&2
    exit 1
fi
mv "$RUN_ROOT/config.new" "$RUN_ROOT/config"

if [[ ! -f "$RUN_ROOT/run.meta" ]]; then
    {
        printf 'host=%s\n' "$(hostname)"
        printf 'started_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } > "$RUN_ROOT/run.meta"
fi

policy=(-m g2 -K 256 -s 6 -t 21 -l 4 -k 5 -b 8 -j 8
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
    [[ ! -e "$cell" ]] || { printf 'incomplete GRZ cell must be inspected: %s\n' "$cell" >&2; exit 1; }
    mkdir -p "$cell"
    probe_tus=$((total_tus < GROUP_TUS ? total_tus : GROUP_TUS))
    head -n "$probe_tus" "$manifest_path" > "$cell/manifest.probe.txt"
    printf '[%s/%s] current GRZ probe: %s (%s), %s TUs\n' \
        "$ordinal" "$EXPECTED_CELLS" "$name" "$id" "$probe_tus"

    tr '\n' '\0' < "$cell/manifest.probe.txt" | xargs -0 cat > "$cell/probe.ii"
    taskset -c "$CORES" "$GRZ_BIN" tu "$cell/manifest.probe.txt" "$cell/probe.tu" \
        > "$cell/tu.stdout" 2> "$cell/tu.stderr"
    /usr/bin/time -v -o "$cell/encode.time" \
        taskset -c "$CORES" "$GRZ_BIN" enc "$cell/probe.ii" "$cell/probe.grz" \
        -u "$cell/probe.tu" "${policy[@]}" --curve "$cell/curve.tsv" \
        > "$cell/encode.stdout" 2> "$cell/encode.stderr"
    /usr/bin/time -v -o "$cell/decode.time" \
        taskset -c "$CORES" "$GRZ_BIN" dec "$cell/probe.grz" "$cell/replay.ii" -j 1 \
        > "$cell/decode.stdout" 2> "$cell/decode.stderr"
    cmp "$cell/probe.ii" "$cell/replay.ii"
    probe_raw=$(stat -Lc %s "$cell/probe.ii")
    probe_wire=$(stat -Lc %s "$cell/probe.grz")
    curve_tus=$(awk -F '\t' 'NR > 1 {last=$3} END {print last+0}' "$cell/curve.tsv")
    [[ "$curve_tus" -eq "$probe_tus" ]] || {
        printf 'GRZ curve extent differs for %s: %s != %s\n' "$id" "$curve_tus" "$probe_tus" >&2
        exit 1
    }
    sha256sum "$cell/probe.ii" "$cell/replay.ii" "$cell/probe.tu" \
        "$cell/probe.grz" "$cell/curve.tsv" > "$cell/exact.sha256"
    {
        printf 'id\tname\ttotal_tus\ttotal_raw_bytes\tprobe_tus\tprobe_raw_bytes\tprobe_wire_bytes\tmanifest_sha256\texact\n'
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\ttrue\n' \
            "$id" "$name" "$total_tus" "$total_raw" "$probe_tus" "$probe_raw" \
            "$probe_wire" "$manifest_sha"
    } > "$cell/measurement.tsv"
    rm -f -- "$cell/probe.ii" "$cell/replay.ii"
done < "$RUN_ROOT/fixed16-ledger.tsv"

cp "$FIXED16_LEDGER" "$RUN_ROOT/fixed16-ledger.final.tsv"
cmp "$RUN_ROOT/fixed16-ledger.tsv" "$RUN_ROOT/fixed16-ledger.final.tsv"
printf 'completed_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$RUN_ROOT/run.meta"
printf 'current GRZ fixed-16 probes complete: %s\n' "$RUN_ROOT"
