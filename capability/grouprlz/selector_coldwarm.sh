#!/usr/bin/env bash
# Cold-to-warm transition matrix, rows 2-4 of the requested measurement.
#
#   row 4 controls   : --workers 1            (current 1-F)
#                      --workers 8            (independent 8-F)
#   row 2 transition : --workers 4 --latejoin-at {50,100,200}   (1 -> 4 after TU N)
#   row 3 transition : --workers 8 --latejoin-at {50,100,200}   (1 -> 8 after TU N)
#
# `--latejoin-at N` is verified to start with ONE consumer and join the rest at TU N
# (cap_m5_main.cpp:1184 `initial = latejoin_at==UINT32_MAX ? workers : 1`, and confirmed
# empirically: 1 distinct worker below the boundary, N above it).
#
# Retained per run: exact reconstruction, both ledger closures, the Root/Need/Fill frame
# split, the per-TU curve (physical bytes and latency at any TU), wall + user + sys +
# peak RSS, and the run's binary/corpus hashes.
set -uo pipefail

CORPUS=${1:?corpus id}
REPS=${REPS:-3}
M5=${M5:-$HOME/issue16-m5-f-compiler-overlap-dc5e0ab5/rate-suite-v2/cap_m5}
ROOT=${ROOT:-$HOME/selbind/coldwarm}
MAN=$HOME/ictmp/$CORPUS/manifest.txt
W=$ROOT/$CORPUS
CODEC=${CODEC:-z1}

[[ -x $M5 && -f $MAN ]] || { echo "missing binary or manifest for $CORPUS" >&2; exit 1; }
mkdir -p "$W"
TUS=$(wc -l < "$MAN")

run() {   # $1=label  $2..=extra args
    local label=$1; shift
    local rep
    for rep in $(seq 1 "$REPS"); do
        local tag="$label.rep$rep"
        /usr/bin/time -f '%e %U %S %M' -o "$W/$tag.res" \
            taskset -c 0-15 "$M5" --manifest "$MAN" --codec "$CODEC" \
            --curve-out "$W/$tag.curve.tsv" --real-pipes "$@" \
            > "$W/$tag.out" 2> "$W/$tag.err" || { echo "RUNFAIL $CORPUS $tag" >&2; continue; }
        local exact closure txn ledger
        exact=$(sed -n 's/.*exact=\([A-Z]*\).*/\1/p' "$W/$tag.out" | head -1)
        ledger=$(grep -m1 '^FRAME_LEDGER' "$W/$tag.out")
        closure=$(sed -n 's/.*total=[0-9]* closure=\([A-Z]*\).*/\1/p' <<<"$ledger")
        txn=$(sed -n 's/.*aborted=[0-9]* closure=\([A-Z]*\).*/\1/p' "$W/$tag.out" | head -1)
        # per-TU curve -> physical bytes and wall clock at the checkpoints
        awk -F'\t' -v c="$CORPUS" -v l="$label" -v r="$rep" -v tus="$TUS" \
            -v res="$(cat "$W/$tag.res")" -v ex="$exact" -v cl="$closure" -v tx="$txn" \
            -v led="$(sed 's/^FRAME_LEDGER //' <<<"$ledger")" '
            NR==2 { first=$6 }
            NR>1 { cw[$1]=$8; ct[$1]=$6; last=$1; lastw=$8; lastt=$6 }
            END {
              split(res,R," ")
              printf "%s\t%s\t%s\t%s\t%.3f\t%s\t%s\t%s\t%s\t%.3f\t%.3f\t%.3f\t%s\t%s\t%s\t%s\t%s\n",
                c,l,r,tus,first/1e6,
                (49 in cw?cw[49]:"NA"),(99 in cw?cw[99]:"NA"),(199 in cw?cw[199]:"NA"),lastw,
                (49 in ct?ct[49]/1e9:0),(99 in ct?ct[99]/1e9:0),(199 in ct?ct[199]/1e9:0),
                R[1],R[2],R[3],R[4],ex"/"cl"/"tx
            }' "$W/$tag.curve.tsv"
    done
}

run ctl-1F   --workers 1
run ctl-8F   --workers 8
for n in 50 100 200; do
    (( TUS > n )) || continue
    run "t1to4-at$n" --workers 4 --latejoin-at "$n"
    run "t1to8-at$n" --workers 8 --latejoin-at "$n"
done
