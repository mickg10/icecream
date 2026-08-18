#!/usr/bin/env bash
# Exact forced-boundary GRZ controls for the native-nine TU100/TU200 gates.
#
# A full GRZ stream closes ordinary groups at 112 TUs.  Reading its curve at
# TU100 or TU200 would therefore count buffered work as zero and would not be
# a usable checkpoint comparison.  This runner encodes each prefix as its own
# complete stream, decodes it, byte-compares it, and compares the retained
# bytes with the independently decoded zstd-6-long prefix control.

set -euo pipefail

SCRIPT_PATH=${BASH_SOURCE[0]-$0}
SCRIPT_DIR=$(cd -- "$(dirname -- "$SCRIPT_PATH")" && pwd)

: "${FULL_ROOT:?set FULL_ROOT to the completed native9-holdout-v2 root}"
: "${CHECK_ROOT:?set CHECK_ROOT to a new retained checkpoint directory}"

GRZ_BIN=${GRZ_BIN:-/home/ttuser/issue16-selector-v1/tools/grz2g-selector}
GRZ_SOURCE=${GRZ_SOURCE:-/home/ttuser/issue16-selector-v1/tools/grz2g-selector.cpp}
CORES=${CORES:-0-31}
CODEC_THREADS=${CODEC_THREADS:-16}
EXPECTED_CELLS=${EXPECTED_CELLS:-9}

for path in "$FULL_ROOT/native9-ledger.tsv" \
            "$FULL_ROOT/native9-ledger.final.tsv" \
            "$FULL_ROOT/tooling.sha256" "$GRZ_BIN" "$GRZ_SOURCE"; do
    [[ -f "$path" ]] || { printf 'missing checkpoint input: %s\n' "$path" >&2; exit 1; }
done
cmp "$FULL_ROOT/native9-ledger.tsv" "$FULL_ROOT/native9-ledger.final.tsv"
[[ -x "$GRZ_BIN" ]] || { printf 'GRZ binary is not executable: %s\n' "$GRZ_BIN" >&2; exit 1; }

rows=$(awk -F '\t' 'NR > 1 {n++} END {print n+0}' "$FULL_ROOT/native9-ledger.tsv")
[[ "$rows" -eq "$EXPECTED_CELLS" ]] || {
    printf 'expected %s native holdouts, found %s\n' "$EXPECTED_CELLS" "$rows" >&2
    exit 1
}

mkdir -p "$CHECK_ROOT/cells"
cp "$FULL_ROOT/native9-ledger.tsv" "$CHECK_ROOT/native9-ledger.tsv.new"
if [[ -f "$CHECK_ROOT/native9-ledger.tsv" ]] &&
   ! cmp -s "$CHECK_ROOT/native9-ledger.tsv" "$CHECK_ROOT/native9-ledger.tsv.new"; then
    printf 'checkpoint ledger differs from retained run\n' >&2
    exit 1
fi
mv "$CHECK_ROOT/native9-ledger.tsv.new" "$CHECK_ROOT/native9-ledger.tsv"

sha256sum "$SCRIPT_PATH" "$GRZ_BIN" "$GRZ_SOURCE" \
    "$FULL_ROOT/native9-ledger.tsv" "$FULL_ROOT/tooling.sha256" \
    > "$CHECK_ROOT/tooling.sha256.new"
if [[ -f "$CHECK_ROOT/tooling.sha256" ]] &&
   ! cmp -s "$CHECK_ROOT/tooling.sha256" "$CHECK_ROOT/tooling.sha256.new"; then
    printf 'checkpoint tooling differs from retained run\n' >&2
    exit 1
fi
mv "$CHECK_ROOT/tooling.sha256.new" "$CHECK_ROOT/tooling.sha256"

if [[ ! -f "$CHECK_ROOT/run.meta" ]]; then
    {
        printf 'host=%s\n' "$(hostname)"
        printf 'started_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        printf 'full_root=%s\n' "$FULL_ROOT"
        printf 'cores=%s\n' "$CORES"
        printf 'codec_threads=%s\n' "$CODEC_THREADS"
    } > "$CHECK_ROOT/run.meta"
fi

policy=(-m g2 -K 256 -s 6 -t 21 -l 4 -k 5 -b 8 -j "$CODEC_THREADS"
        --gtu 112 --graw 512 --gadd 128 --hist 1024)

ordinal=0
while IFS=$'\t' read -r id name manifest total_tus total_raw manifest_sha; do
    [[ "$id" != id ]] || continue
    ordinal=$((ordinal + 1))
    cell="$CHECK_ROOT/cells/$id"
    full_measurement="$FULL_ROOT/cells/$id/measurement.tsv"
    if [[ -f "$cell/measurement.tsv" ]]; then
        printf '[%s/%s] checkpoint already complete: %s (%s)\n' \
            "$ordinal" "$EXPECTED_CELLS" "$name" "$id"
        continue
    fi
    [[ -f "$full_measurement" ]] || {
        printf 'missing completed full measurement: %s\n' "$full_measurement" >&2
        exit 1
    }
    [[ ! -e "$cell" ]] || {
        printf 'incomplete checkpoint cell must be inspected: %s\n' "$cell" >&2
        exit 1
    }
    mkdir -p "$cell"
    printf '[%s/%s] forced GRZ TU100/TU200: %s (%s)\n' \
        "$ordinal" "$EXPECTED_CELLS" "$name" "$id"

    full_row=$(tail -n 1 "$full_measurement")
    IFS=$'\t' read -r _ _ _ _ _ _ _ _ expected100_raw expected100_z6 \
        expected200_raw expected200_z6 _ _ _ <<< "$full_row"

    values=()
    for checkpoint in 100 200; do
        prefix="$cell/tu$checkpoint"
        head -n "$checkpoint" "$manifest" > "$prefix.manifest.txt"
        actual_tus=$(wc -l < "$prefix.manifest.txt")
        [[ "$actual_tus" -eq "$checkpoint" ]] || {
            printf '%s has fewer than %s TUs\n' "$id" "$checkpoint" >&2
            exit 1
        }
        /usr/bin/time -v -o "$prefix.concatenate.time" \
            bash -c 'tr "\n" "\0" < "$1" | xargs -0 cat > "$2"' \
            native9-prefix "$prefix.manifest.txt" "$prefix.ii"
        raw_bytes=$(stat -Lc %s "$prefix.ii")
        if [[ "$checkpoint" -eq 100 ]]; then
            [[ "$raw_bytes" -eq "$expected100_raw" ]] || exit 1
            z6_bytes=$expected100_z6
        else
            [[ "$raw_bytes" -eq "$expected200_raw" ]] || exit 1
            z6_bytes=$expected200_z6
        fi

        taskset -c "$CORES" "$GRZ_BIN" tu "$prefix.manifest.txt" "$prefix.tu" \
            > "$prefix.tu.stdout" 2> "$prefix.tu.stderr"
        /usr/bin/time -v -o "$prefix.encode.time" \
            taskset -c "$CORES" "$GRZ_BIN" enc "$prefix.ii" "$prefix.grz" \
            -u "$prefix.tu" "${policy[@]}" --curve "$prefix.curve.tsv" \
            > "$prefix.encode.stdout" 2> "$prefix.encode.stderr"
        /usr/bin/time -v -o "$prefix.decode.time" \
            taskset -c "$CORES" "$GRZ_BIN" dec "$prefix.grz" \
            "$prefix.replay.ii" -j 1 \
            > "$prefix.decode.stdout" 2> "$prefix.decode.stderr"
        cmp "$prefix.ii" "$prefix.replay.ii"
        curve_tus=$(awk -F '\t' 'NR > 1 {last=$3} END {print last+0}' \
            "$prefix.curve.tsv")
        [[ "$curve_tus" -eq "$checkpoint" ]] || exit 1
        wire_bytes=$(stat -Lc %s "$prefix.grz")
        sha256sum "$prefix.ii" "$prefix.replay.ii" "$prefix.tu" \
            "$prefix.grz" "$prefix.curve.tsv" > "$prefix.exact.sha256"
        values+=("$raw_bytes" "$wire_bytes" "$z6_bytes")
        rm -f -- "$prefix.ii" "$prefix.replay.ii"
    done

    pass100=false
    pass200=false
    [[ "${values[1]}" -le "${values[2]}" ]] && pass100=true
    [[ "${values[4]}" -le "${values[5]}" ]] && pass200=true
    {
        printf 'id\tname\ttus\traw_bytes\tmanifest_sha256\t'
        printf 'tu100_raw_bytes\ttu100_grz_bytes\ttu100_z6_long_bytes\ttu100_pass\t'
        printf 'tu200_raw_bytes\ttu200_grz_bytes\ttu200_z6_long_bytes\ttu200_pass\texact\n'
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\ttrue\n' \
            "$id" "$name" "$total_tus" "$total_raw" "$manifest_sha" \
            "${values[0]}" "${values[1]}" "${values[2]}" "$pass100" \
            "${values[3]}" "${values[4]}" "${values[5]}" "$pass200"
    } > "$cell/measurement.tsv"
done < "$CHECK_ROOT/native9-ledger.tsv"

first=true
: > "$CHECK_ROOT/native9-prefix-checkpoints.tsv.new"
while IFS= read -r measurement; do
    if $first; then
        cat "$measurement" >> "$CHECK_ROOT/native9-prefix-checkpoints.tsv.new"
        first=false
    else
        tail -n 1 "$measurement" >> "$CHECK_ROOT/native9-prefix-checkpoints.tsv.new"
    fi
done < <(find "$CHECK_ROOT/cells" -mindepth 2 -maxdepth 2 \
             -name measurement.tsv -print | sort)
measured=$(awk -F '\t' 'NR > 1 {n++} END {print n+0}' \
    "$CHECK_ROOT/native9-prefix-checkpoints.tsv.new")
[[ "$measured" -eq "$EXPECTED_CELLS" ]] || exit 1
mv "$CHECK_ROOT/native9-prefix-checkpoints.tsv.new" \
   "$CHECK_ROOT/native9-prefix-checkpoints.tsv"
printf 'completed_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$CHECK_ROOT/run.meta"
find "$CHECK_ROOT" -type f ! -name SHA256SUMS -print0 | sort -z | \
    xargs -0 sha256sum > "$CHECK_ROOT/SHA256SUMS"
printf 'native-nine forced prefix checkpoints complete: %s\n' "$CHECK_ROOT"
