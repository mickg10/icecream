#!/usr/bin/env bash
# Reproducible binding replay for the frozen bounded-history GRZ2 policy.
#
# The speed ledger excludes --retry-test because that option intentionally parses every
# group twice.  A separate diagnostic encode enables it and must produce the same wire.

set -euo pipefail

SCRIPT_PATH=${BASH_SOURCE[0]-$0}
SCRIPT_DIR=$(cd -- "$(dirname -- "$SCRIPT_PATH")" && pwd)
BIN=${BIN:-"$SCRIPT_DIR/grz2g"}
DATA_DIR=${DATA_DIR:-"$HOME/grouprlz/ii"}
CORPORA_FILE=${CORPORA_FILE:-"$HOME/grouprlz/corpora.tsv"}
CORPORA=${CORPORA:-"corpus2 corpus4"}
REPS=${REPS:-3}
CORES=${CORES:-0-15}
RUN_DIR=${RUN_DIR:-"$PWD/grz2-fixed112-$(date -u +%Y%m%dT%H%M%SZ)"}
RESULTS=${RESULTS:-"$RUN_DIR/g2-fixed112.tsv"}

C_FLOOR_BPS=1000000000
F_FLOOR_BPS=500000000
POLICY=(
    -m g2 -K 256 -s 6 -t 21 -l 4 -k 5 -b 8 -j 8
    --gtu 112 --graw 512 --gadd 128 --hist 1024
)

[[ -x "$BIN" ]] || { printf 'codec is not executable: %s\n' "$BIN" >&2; exit 1; }
[[ -f "$CORPORA_FILE" ]] || { printf 'missing corpus ledger: %s\n' "$CORPORA_FILE" >&2; exit 1; }
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
    local label=$1 file=$2
    sed -n "s/.* ${label}=\([0-9][0-9]*\) B\/s.*/\1/p" "$file" | tail -n 1
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
    'policy=G2,K256,s6,t21,BSC-E0-literals,zstd12-tokens,block8MiB,C-j8,F-j1,gtu112,graw512MiB,gadd128MiB,hist1024MiB' \
    "codec=$BIN" \
    "codec_sha256=$(sha256sum "$BIN" | awk '{print $1}')" \
    "C_floor_Bps=$C_FLOOR_BPS" \
    "F_floor_Bps=$F_FLOOR_BPS" \
    "repetitions=$REPS" > "$RUN_DIR/run.meta"

printf '%s\n' \
    $'id\tname\ttus\traw\twp_z19\tout\tratio\tgroups\tmax_group_bytes\tidx_bytes\tC_min_Bps\tC_max_Bps\tF_min_Bps\tF_max_Bps\tC_reps_s\tF_reps_s\tC_RSS_GiB\tF_RSS_GiB\tF_ring_GiB\twire_sha256\texact\tdeterministic_wire\tretry_replay\tgroup_caps\tsize\tC\tF\tPASS' \
    > "$RESULTS"

for corpus in $CORPORA; do
    name=$(awk -F '\t' -v c="$corpus" '$1==c { print $2; exit }' "$CORPORA_FILE")
    expected_tus=$(awk -F '\t' -v c="$corpus" '$1==c { print $3; exit }' "$CORPORA_FILE")
    expected_raw=$(awk -F '\t' -v c="$corpus" '$1==c { print $4; exit }' "$CORPORA_FILE")
    wp_z19=$(awk -F '\t' -v c="$corpus" '$1==c { print $5; exit }' "$CORPORA_FILE")
    [[ -n "$name" && -n "$expected_tus" && -n "$expected_raw" && -n "$wp_z19" ]] || {
        printf 'incomplete corpus ledger row: %s\n' "$corpus" >&2
        exit 1
    }

    raw_file="$DATA_DIR/$corpus.ii"
    tu_file="$DATA_DIR/$corpus.tu"
    [[ -f "$raw_file" && -f "$tu_file" ]] || {
        printf 'missing input or TU map for %s\n' "$corpus" >&2
        exit 1
    }
    raw=$(stat -Lc %s "$raw_file")
    tu_bytes=$(stat -Lc %s "$tu_file")
    (( tu_bytes % 8 == 0 && tu_bytes >= 16 )) || { printf 'bad TU map: %s\n' "$tu_file" >&2; exit 1; }
    tus=$((tu_bytes / 8 - 1))
    [[ "$raw" == "$expected_raw" && "$tus" == "$expected_tus" ]] || {
        printf 'ledger/input mismatch for %s: raw %s/%s, TUs %s/%s\n' \
            "$corpus" "$raw" "$expected_raw" "$tus" "$expected_tus" >&2
        exit 1
    }
    sha256sum "$raw_file" "$tu_file" > "$RUN_DIR/$corpus.inputs.sha256"

    wire="$RUN_DIR/$corpus.grz"
    first_wire_sha=''
    first_curve=''
    cmin=9223372036854775807
    cmax=0
    crss=0
    ctimes=''
    out=0
    groups=0
    max_group=0
    idx_bytes=0

    for rep in $(seq 1 "$REPS"); do
        enc_out="$RUN_DIR/$corpus.enc.rep$rep.stdout"
        enc_err="$RUN_DIR/$corpus.enc.rep$rep.stderr"
        curve="$RUN_DIR/$corpus.curve.rep$rep.tsv"
        run_pinned "$BIN" enc "$raw_file" "$wire" -u "$tu_file" "${POLICY[@]}" \
            --curve "$curve" > "$enc_out" 2> "$enc_err"
        rate=$(stderr_rate C "$enc_err")
        [[ "$rate" =~ ^[0-9]+$ ]] || { printf 'missing C rate for %s rep %s\n' "$corpus" "$rep" >&2; exit 1; }
        cmin=$(min_int "$cmin" "$rate")
        cmax=$(max_int "$cmax" "$rate")
        rss=$(field 8 "$enc_out")
        crss=$(max_int "$crss" "$rss")
        sec=$(field 6 "$enc_out")
        ctimes+="${ctimes:+,}$sec"
        out=$(field 2 "$enc_out")
        groups=$(field 3 "$enc_out")
        idx_bytes=$(field 7 "$enc_out")
        max_group=$(field 9 "$enc_out")
        [[ $(field 11 "$enc_out") == 0 ]] || { printf 'encoder diagnostic count is nonzero\n' >&2; exit 1; }
        wire_sha=$(sha256sum "$wire" | awk '{print $1}')
        if [[ -z "$first_wire_sha" ]]; then
            first_wire_sha=$wire_sha
            first_curve=$curve
        else
            [[ "$wire_sha" == "$first_wire_sha" ]] || { printf 'wire drift for %s\n' "$corpus" >&2; exit 1; }
            cmp "$first_curve" "$curve"
        fi
    done

    fmin=9223372036854775807
    fmax=0
    frss=0
    ring=0
    ftimes=''
    for rep in $(seq 1 "$REPS"); do
        dec_out="$RUN_DIR/$corpus.dec.rep$rep.stdout"
        dec_err="$RUN_DIR/$corpus.dec.rep$rep.stderr"
        run_pinned "$BIN" dec "$wire" /dev/null -j 1 > "$dec_out" 2> "$dec_err"
        rate=$(stderr_rate F "$dec_err")
        [[ "$rate" =~ ^[0-9]+$ ]] || { printf 'missing F rate for %s rep %s\n' "$corpus" "$rep" >&2; exit 1; }
        fmin=$(min_int "$fmin" "$rate")
        fmax=$(max_int "$fmax" "$rate")
        rss=$(field 6 "$dec_out")
        frss=$(max_int "$frss" "$rss")
        ring=$(field 5 "$dec_out")
        sec=$(field 4 "$dec_out")
        ftimes+="${ftimes:+,}$sec"
    done

    replay="$RUN_DIR/$corpus.replay.ii"
    run_pinned "$BIN" dec "$wire" "$replay" -j 1 \
        > "$RUN_DIR/$corpus.dec.exact.stdout" 2> "$RUN_DIR/$corpus.dec.exact.stderr"
    if cmp "$raw_file" "$replay"; then exact=YES; else exact=NO; fi
    sha256sum "$raw_file" "$replay" > "$RUN_DIR/$corpus.exact.sha256"
    rm -f "$replay"

    # The diagnostic parse must roll back and retry every group from identical state.  It is
    # intentionally excluded from the speed ledger, and its wire must match the normal parse.
    diag_wire="$RUN_DIR/$corpus.retry.grz"
    run_pinned "$BIN" enc "$raw_file" "$diag_wire" -u "$tu_file" "${POLICY[@]}" \
        --retry-test 1 --curve "$RUN_DIR/$corpus.retry.curve.tsv" \
        > "$RUN_DIR/$corpus.retry.stdout" 2> "$RUN_DIR/$corpus.retry.stderr"
    if [[ $(field 11 "$RUN_DIR/$corpus.retry.stdout") == 0 ]] && \
       grep -q 'retry_fail=0' "$RUN_DIR/$corpus.retry.stderr" && cmp "$wire" "$diag_wire"; then
        retry=PASS
    else
        retry=FAIL
    fi

    if [[ $(sha256sum "$wire" | awk '{print $1}') == "$first_wire_sha" ]]; then deterministic=PASS; else deterministic=FAIL; fi
    if awk -v o="$out" -v z="$wp_z19" 'BEGIN { exit !(o <= 1.10*z) }'; then size_gate=PASS; else size_gate=FAIL; fi
    if (( cmin >= C_FLOOR_BPS )); then c_gate=PASS; else c_gate=FAIL; fi
    if (( fmin >= F_FLOOR_BPS )); then f_gate=PASS; else f_gate=FAIL; fi
    if awk -F '\t' 'NR > 1 { if (($3-$2) > 112 || $4 > 536870912 || $5 > 134217728) exit 1 }' "$first_curve"; then caps=PASS; else caps=FAIL; fi
    if [[ "$exact" == YES && "$deterministic" == PASS && "$retry" == PASS && \
          "$caps" == PASS && "$size_gate" == PASS && "$c_gate" == PASS && "$f_gate" == PASS ]]; then
        verdict=PASS
    else
        verdict=FAIL
    fi

    ratio=$(awk -v o="$out" -v z="$wp_z19" 'BEGIN { printf "%.4f", o/z }')
    crss_gib=$(awk -v n="$crss" 'BEGIN { printf "%.2f", n/1073741824 }')
    frss_gib=$(awk -v n="$frss" 'BEGIN { printf "%.2f", n/1073741824 }')
    ring_gib=$(awk -v n="$ring" 'BEGIN { printf "%.2f", n/1073741824 }')
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$corpus" "$name" "$tus" "$raw" "$wp_z19" "$out" "$ratio" "$groups" \
        "$max_group" "$idx_bytes" "$cmin" "$cmax" "$fmin" "$fmax" "$ctimes" "$ftimes" \
        "$crss_gib" "$frss_gib" "$ring_gib" "$first_wire_sha" "$exact" "$deterministic" \
        "$retry" "$caps" "$size_gate" "$c_gate" "$f_gate" "$verdict" | tee -a "$RESULTS"
done

if awk -F '\t' 'NR > 1 && $NF != "PASS" { bad=1 } END { exit bad }' "$RESULTS"; then
    printf 'ALL FIXED-112 ROWS PASSED: %s\n' "$RESULTS"
else
    printf 'ONE OR MORE FIXED-112 ROWS FAILED: %s\n' "$RESULTS" >&2
    exit 1
fi
