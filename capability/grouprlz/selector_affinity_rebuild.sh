#!/usr/bin/env bash
# DENSE_AFFINITY, rebuild half: what does the SECOND build cost as a function of how many
# F daemons it is routed to, and whether routing is sticky (same file -> same daemon)?
#
# --repetitions 2 replays the corpus, so TUs [0,N) are the cold build and [N,2N) the
# rebuild.  The per-TU curve lets the two be separated exactly.
set -uo pipefail
S=$HOME/selbind/m5split/cap_m5_split
R=$HOME/selbind/affinity; mkdir -p $R
OUT=$R/rebuild.tsv
[ -f $OUT ] || printf "corpus\ttus\tworkers\tassignment\tpass1_wire\tpass2_wire\tpass2_over_pass1\tp2_root\tp2_need\tp2_fill\twall_s\tsplit_ok\n" > $OUT
for C in "$@"; do
  MAN=$HOME/ictmp/$C/manifest.txt
  N=$(wc -l < $MAN)
  for K in 1 4 8 32; do
    for A in sticky roundrobin; do
      tag="$C.r2.k$K.$A"
      nice -n 5 taskset -c 0-31 $S --manifest $MAN --codec z1 --real-pipes \
        --repetitions 2 --workers $K --assignment $A --curve-out $R/$tag.tsv \
        > $R/$tag.out 2> $R/$tag.err
      [ -s $R/$tag.tsv ] || { echo "FAIL $tag" >&2; continue; }
      wall=$(sed -n 's/.*wall=\([0-9.]*\)s.*/\1/p' $R/$tag.out | head -1)
      awk -F'\t' -v c="$C" -v k="$K" -v a="$A" -v n="$N" -v w="${wall:-NA}" '
        NR>1 { rows++
               if ($1 < n) { p1+=$5 } else { p2+=$5; r+=$9; nd+=$10; f+=$11 }
               if ($14!=1) bad++ }
        END { printf "%s\t%d\t%s\t%s\t%d\t%d\t%.4f\t%d\t%d\t%d\t%s\t%d/%d\n",
                     c, rows, k, a, p1, p2, p1?p2/p1:0, r, nd, f, w, rows-bad, rows }' \
        $R/$tag.tsv >> $OUT
      tail -1 $OUT >&2
    done
  done
done
echo "REBUILD DONE $(date +%T)" >&2
