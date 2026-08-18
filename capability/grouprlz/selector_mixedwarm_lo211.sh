#!/usr/bin/env bash
# Mixed warmth recut on local-oracle 211bd585 as the canonical base.
# C->F is read NATIVELY from its per-TU c_to_f (col 9); categories from c_root/c_fill (13/14).
set -uo pipefail
S=$HOME/selbind/lo211/cap_m5_lo211
R=$HOME/selbind/mixedwarm211; mkdir -p $R
OUT=$R/mixedwarm211.tsv
[ -f $OUT ] || printf "corpus\ttus\tk\tp1_cf\tp2_cf\tp2_duplex\tp2_warm_cf\tp2_cold_cf\tp2_warm_tus\tp2_cold_tus\tp2_c_root\tp2_c_fill\twall_s\tdirection_ok\tcategory_ok\n" > $OUT
for C in "$@"; do
  MAN=$HOME/ictmp/$C/manifest.txt
  N=$(wc -l < $MAN)
  for K in 1 2 4 8 30; do
    tag="$C.lo.k$K"
    nice -n 5 taskset -c 0-31 $S --manifest $MAN --codec z1 --real-pipes \
      --repetitions 2 --workers $K --latejoin-at $N --curve-out $R/$tag.tsv \
      > $R/$tag.out 2> $R/$tag.err
    [ -s $R/$tag.tsv ] || { echo "FAIL $tag" >&2; continue; }
    wall=$(sed -n 's/.*wall=\([0-9.]*\)s.*/\1/p' $R/$tag.out | head -1)
    awk -F'\t' -v c="$C" -v k="$K" -v n="$N" -v w="${wall:-NA}" '
      NR>1 { rows++
             if ($1 < n) { p1+=$9 }
             else { p2+=$9; dx+=$5; r+=$13; f+=$14
                    if ($3 == 0) { wcf+=$9; wt++ } else { ccf+=$9; ct++ } }
             if ($18!=1) b1++; if ($19!=1) b2++ }
      END { printf "%s\t%d\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%s\t%d/%d\t%d/%d\n",
                   c, rows, k, p1, p2, dx, wcf, ccf, wt, ct, r, f, w,
                   rows-b1, rows, rows-b2, rows }' $R/$tag.tsv >> $OUT
    tail -1 $OUT >&2
  done
done
echo "MIXED WARMTH (211bd585) DONE $(date +%T)" >&2
