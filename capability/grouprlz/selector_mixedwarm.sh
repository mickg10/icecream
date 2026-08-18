#!/usr/bin/env bash
# MIXED WARMTH: a build arrives at a farm where ONE daemon already has this project's
# history and the rest are cold.  That is the scheduler's real decision -- neither the
# pure-cold nor the pure-rebuild arm.
#
#   --repetitions 2 --latejoin-at N   (N = TUs in one pass)
#     pass 1 runs entirely on worker 0, so it ends fully warm;
#     workers 1..k-1 spawn at the boundary COLD and share pass 2.
#   pass-2 wire is therefore "what this build costs on a farm with 1 warm + (k-1) cold".
#   k=1 is the dense-to-warm baseline: use only the daemon that already knows the project.
set -uo pipefail
S=$HOME/selbind/m5split/cap_m5_dir
R=$HOME/selbind/mixedwarm; mkdir -p $R
OUT=$R/mixedwarm.tsv
[ -f $OUT ] || printf "corpus\ttus\tk\twarm\tcold\tpass1_wire\tpass2_wire\tp2_cf\tp2_fc\tp2_root\tp2_fill\tp2_warm_wire\tp2_cold_wire\tp2_warm_tus\tp2_cold_tus\twall_s\tsplit_ok\tdir_ok\n" > $OUT
for C in "$@"; do
  MAN=$HOME/ictmp/$C/manifest.txt
  N=$(wc -l < $MAN)
  for K in 1 2 4 8 30; do
    tag="$C.mw.k$K"
    nice -n 5 taskset -c 0-31 $S --manifest $MAN --codec z1 --real-pipes \
      --repetitions 2 --workers $K --latejoin-at $N --curve-out $R/$tag.tsv \
      > $R/$tag.out 2> $R/$tag.err
    [ -s $R/$tag.tsv ] || { echo "FAIL $tag" >&2; continue; }
    wall=$(sed -n 's/.*wall=\([0-9.]*\)s.*/\1/p' $R/$tag.out | head -1)
    awk -F'\t' -v c="$C" -v k="$K" -v n="$N" -v w="${wall:-NA}" '
      NR>1 { rows++
             if ($1 < n) { p1+=$5 }
             else { p2+=$5; cf+=$26; fc+=$29; r+=$9; f+=$11
                    if ($3 == 0) { ww+=$5; wt++ } else { cw+=$5; ct++ } }
             if ($14!=1) b1++; if ($30!=1) b2++ }
      END { printf "%s\t%d\t%s\t1\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%s\t%d/%d\t%d/%d\n",
                   c, rows, k, k-1, p1, p2, cf, fc, r, f, ww, cw, wt, ct, w,
                   rows-b1, rows, rows-b2, rows }' $R/$tag.tsv >> $OUT
    tail -1 $OUT >&2
  done
done
echo "MIXED WARMTH DONE $(date +%T)" >&2
