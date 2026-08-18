#!/usr/bin/env bash
# Decoder-only replay of retained fixed-112 wires.
#
# This runner is intentionally separate from g2_fixed112.sh: a decoder storage change must
# not pay for or obscure a new encode. It verifies each retained wire against the original
# ledger, measures fresh decoder processes, and performs one byte-for-byte replay per corpus.

set -euo pipefail

SCRIPT_PATH=${BASH_SOURCE[0]-$0}
SCRIPT_DIR=$(cd -- "$(dirname -- "$SCRIPT_PATH")" && pwd)
BIN=${BIN:-"$SCRIPT_DIR/grz2g"}
BASE_RUN_DIR=${BASE_RUN_DIR:-"$HOME/grouprlz/retained/grz2g-fixed16-1777470-20260818T0000Z"}
DATA_DIR=${DATA_DIR:-"$HOME/grouprlz/ii"}
REPS=${REPS:-3}
CORES=${CORES:-0-15}
RUN_DIR=${RUN_DIR:-"$PWD/grz2-fixed16-decoder-replay-$(date -u +%Y%m%dT%H%M%SZ)"}
RESULTS=${RESULTS:-"$RUN_DIR/g2-fixed16-decoder-replay.tsv"}
CORPORA=${CORPORA:-"corpus corpus2 corpus3 corpus4 corpus5 corpus6 corpus7 corpus8 corpus9 corpus10 corpus11 corpus12 corpus13 corpus14 corpus15 corpus16"}
F_FLOOR_BPS=${F_FLOOR_BPS:-500000000}
BASE_RESULTS="$BASE_RUN_DIR/g2-fixed112.tsv"

[[ -x "$BIN" ]] || { printf 'codec is not executable: %s\n' "$BIN" >&2; exit 1; }
[[ -f "$BASE_RESULTS" ]] || { printf 'missing base ledger: %s\n' "$BASE_RESULTS" >&2; exit 1; }
[[ "$REPS" =~ ^[1-9][0-9]*$ ]] || { printf 'REPS must be positive\n' >&2; exit 1; }
mkdir -p "$RUN_DIR"

run_pinned() {
    if command -v taskset >/dev/null 2>&1; then
        taskset -c "$CORES" "$@"
    else
        "$@"
    fi
}

field() {
    local number=$1 file=$2
    awk -F '\t' -v n="$number" 'NF { value=$n } END { print value }' "$file"
}

stderr_rate() {
    local file=$1
    sed -n 's/.* F=\([0-9][0-9]*\) B\/s.*/\1/p' "$file" | tail -n 1
}

min_int() {
    local current=$1 candidate=$2
    if (( candidate < current )); then printf '%s' "$candidate"; else printf '%s' "$current"; fi
}

max_int() {
    local current=$1 candidate=$2
    if (( candidate > current )); then printf '%s' "$candidate"; else printf '%s' "$current"; fi
}

printf '%s\n' \
    "codec=$BIN" \
    "codec_sha256=$(sha256sum "$BIN" | awk '{print $1}')" \
    "base_run_dir=$BASE_RUN_DIR" \
    "base_ledger_sha256=$(sha256sum "$BASE_RESULTS" | awk '{print $1}')" \
    "F_floor_Bps=$F_FLOOR_BPS" \
    "repetitions=$REPS" \
    "cores=$CORES" > "$RUN_DIR/run.meta"

printf '%s\n' \
    $'id\tname\ttus\traw\twire_bytes\twire_sha256\tbase_F_min_Bps\tnew_F_min_Bps\tnew_F_max_Bps\tF_reps_s\tbase_F_RSS_GiB\tnew_F_RSS_GiB\tbase_ring_GiB\tnew_ring_GiB\trate_multiple\trss_saved_GiB\tring_saved_GiB\twire_retained\texact\tF\tPASS' \
    > "$RESULTS"

for corpus in $CORPORA; do
    row=$(awk -F '\t' -v c="$corpus" '$1==c { print; exit }' "$BASE_RESULTS")
    [[ -n "$row" ]] || { printf 'missing base row: %s\n' "$corpus" >&2; exit 1; }
    name=$(printf '%s\n' "$row" | awk -F '\t' '{print $2}')
    tus=$(printf '%s\n' "$row" | awk -F '\t' '{print $3}')
    expected_raw=$(printf '%s\n' "$row" | awk -F '\t' '{print $4}')
    expected_wire=$(printf '%s\n' "$row" | awk -F '\t' '{print $6}')
    expected_wire_sha=$(printf '%s\n' "$row" | awk -F '\t' '{print $20}')
    base_fmin=$(printf '%s\n' "$row" | awk -F '\t' '{print $13}')
    base_frss=$(printf '%s\n' "$row" | awk -F '\t' '{print $18}')
    base_ring=$(printf '%s\n' "$row" | awk -F '\t' '{print $19}')

    raw_file="$DATA_DIR/$corpus.ii"
    wire="$BASE_RUN_DIR/$corpus.grz"
    [[ -f "$raw_file" && -f "$wire" ]] || {
        printf 'missing input or retained wire for %s\n' "$corpus" >&2
        exit 1
    }
    raw=$(stat -Lc %s "$raw_file")
    wire_bytes=$(stat -Lc %s "$wire")
    wire_sha=$(sha256sum "$wire" | awk '{print $1}')
    if [[ "$raw" != "$expected_raw" || "$wire_bytes" != "$expected_wire" || \
          "$wire_sha" != "$expected_wire_sha" ]]; then
        printf 'base artifact mismatch for %s\n' "$corpus" >&2
        exit 1
    fi

    fmin=9223372036854775807
    fmax=0
    frss=0
    ring=''
    ftimes=''
    for rep in $(seq 1 "$REPS"); do
        dec_out="$RUN_DIR/$corpus.dec.rep$rep.stdout"
        dec_err="$RUN_DIR/$corpus.dec.rep$rep.stderr"
        run_pinned "$BIN" dec "$wire" /dev/null -j 1 > "$dec_out" 2> "$dec_err"
        rate=$(stderr_rate "$dec_err")
        [[ "$rate" =~ ^[0-9]+$ ]] || { printf 'missing F rate for %s rep %s\n' "$corpus" "$rep" >&2; exit 1; }
        fmin=$(min_int "$fmin" "$rate")
        fmax=$(max_int "$fmax" "$rate")
        rss=$(field 6 "$dec_out")
        frss=$(max_int "$frss" "$rss")
        rep_ring=$(field 5 "$dec_out")
        if [[ -z "$ring" ]]; then ring=$rep_ring; elif [[ "$ring" != "$rep_ring" ]]; then
            printf 'ring size drift for %s\n' "$corpus" >&2
            exit 1
        fi
        sec=$(field 4 "$dec_out")
        ftimes+="${ftimes:+,}$sec"
    done

    replay="$RUN_DIR/$corpus.replay.ii"
    run_pinned "$BIN" dec "$wire" "$replay" -j 1 \
        > "$RUN_DIR/$corpus.dec.exact.stdout" 2> "$RUN_DIR/$corpus.dec.exact.stderr"
    if cmp "$raw_file" "$replay"; then exact=YES; else exact=NO; fi
    sha256sum "$raw_file" "$replay" > "$RUN_DIR/$corpus.exact.sha256"
    rm -f "$replay"

    frss_gib=$(awk -v n="$frss" 'BEGIN { printf "%.3f", n/1073741824 }')
    ring_gib=$(awk -v n="$ring" 'BEGIN { printf "%.3f", n/1073741824 }')
    rate_multiple=$(awk -v n="$fmin" -v o="$base_fmin" 'BEGIN { printf "%.3f", n/o }')
    rss_saved=$(awk -v o="$base_frss" -v n="$frss_gib" 'BEGIN { printf "%.3f", o-n }')
    ring_saved=$(awk -v o="$base_ring" -v n="$ring_gib" 'BEGIN { printf "%.3f", o-n }')
    if (( fmin >= F_FLOOR_BPS )); then f_gate=PASS; else f_gate=FAIL; fi
    if [[ "$exact" == YES && "$f_gate" == PASS ]]; then verdict=PASS; else verdict=FAIL; fi

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$corpus" "$name" "$tus" "$raw" "$wire_bytes" "$wire_sha" "$base_fmin" \
        "$fmin" "$fmax" "$ftimes" "$base_frss" "$frss_gib" "$base_ring" "$ring_gib" \
        "$rate_multiple" "$rss_saved" "$ring_saved" YES "$exact" "$f_gate" "$verdict" | tee -a "$RESULTS"
done

if awk -F '\t' 'NR > 1 && $NF != "PASS" { bad=1 } END { exit bad }' "$RESULTS"; then
    printf 'ALL RETAINED WIRES REPLAYED EXACTLY AND PASSED: %s\n' "$RESULTS"
else
    printf 'ONE OR MORE RETAINED WIRES FAILED: %s\n' "$RESULTS" >&2
    exit 1
fi
