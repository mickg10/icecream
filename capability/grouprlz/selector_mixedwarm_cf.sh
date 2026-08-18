#!/usr/bin/env bash
# Mixed warmth, recut on the C->F-only wire (direction-exact ledger).
# Duplex totals retained alongside so elapsed-time reasoning still has them.
set -uo pipefail
S=$HOME/selbind/m5split/cap_m5_dir2
R=$HOME/selbind/mixedwarm; mkdir -p $R
OUT=$R/mixedwarm_cf.tsv
[ -f $OUT ] || printf "corpus\ttus\tk\tp1_cf\tp2_cf\tp2_duplex\tp2_warm_cf\tp2_cold_cf\tp2_warm_duplex\tp2_cold_duplex\tp2_warm_tus\tp2_cold_tus\tp2_cf_root\tp2_cf_fill\twall_s\tsplit_ok\tdir_ok\tundirected\n" > $OUT
for C in "$@"; do
  MAN=$HOME/ictmp/$C/manifest.txt
  N=$(wc -l < $MAN)
  for K in 1 2 4 8 30; do
    tag="$C.mwcf.k$K"
    nice -n 5 taskset -c 0-31 $S --manifest $MAN --codec z1 --real-pipes \
      --repetitions 2 --workers $K --latejoin-at $N --curve-out $R/$tag.tsv \
      > $R/$tag.out 2> $R/$tag.err
    [ -s $R/$tag.tsv ] || { echo "FAIL $tag" >&2; continue; }
    wall=$(sed -n 's/.*wall=\([0-9.]*\)s.*/\1/p' $R/$tag.out | head -1)
    awk -F'\t' -v c="$C" -v k="$K" -v n="$N" -v w="${wall:-NA}" '
      NR>1 { rows++
             if ($1 < n) { p1+=$27 }
             else { p2+=$27; dx+=$5; r+=$23; f+=$24
                    if ($3 == 0) { wcf+=$27; wdx+=$5; wt++ }
                    else         { ccf+=$27; cdx+=$5; ct++ } }
             un+=$32; if ($14!=1) b1++; if ($33!=1) b2++ }
      END { printf "%s\t%d\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%s\t%d/%d\t%d/%d\t%d\n",
                   c, rows, k, p1, p2, dx, wcf, ccf, wdx, cdx, wt, ct, r, f, w,
                   rows-b1, rows, rows-b2, rows, un }' $R/$tag.tsv >> $OUT
    tail -1 $OUT >&2
  done
done
echo "MIXED WARMTH CF DONE $(date +%T)" >&2
