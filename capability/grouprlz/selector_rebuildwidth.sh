#!/usr/bin/env bash
# Rebuild at the BINDING cache-domain widths, pinning vs not, on the direction-exact
# C->F basis.  M in {1,4,8,30} are M_min(30) for c_f in {30,8,4,1}.
#
# --repetitions 2: TUs [0,N) are the cold build, [N,2N) the rebuild.
#   sticky     = each file hashes to a fixed daemon, so the rebuild returns to it.
#   roundrobin = indexes the concatenated sequence, so N mod M shifts files onto other
#                daemons -- a non-pinning scheduler.
set -uo pipefail
S=$HOME/selbind/m5split/cap_m5_dir2
R=$HOME/selbind/rebuildwidth; mkdir -p $R
OUT=$R/rebuildwidth.tsv
[ -f $OUT ] || printf "corpus\ttus\tM\tassignment\tp1_cf\tp2_cf\tp2_over_p1\tp2_cf_root\tp2_cf_fill\tp2_fc\tp2_missing\twall_s\tsplit_ok\tdir_ok\tundirected\n" > $OUT
for C in "$@"; do
  MAN=$HOME/ictmp/$C/manifest.txt
  N=$(wc -l < $MAN)
  for M in 1 4 8 30; do
    for A in sticky roundrobin; do
      tag="$C.r2.M$M.$A"
      nice -n 5 taskset -c 0-31 $S --manifest $MAN --codec z1 --real-pipes \
        --repetitions 2 --workers $M --assignment $A --curve-out $R/$tag.tsv \
        > $R/$tag.out 2> $R/$tag.err
      [ -s $R/$tag.tsv ] || { echo "FAIL $tag" >&2; continue; }
      wall=$(sed -n 's/.*wall=\([0-9.]*\)s.*/\1/p' $R/$tag.out | head -1)
      awk -F'\t' -v c="$C" -v m="$M" -v a="$A" -v n="$N" -v w="${wall:-NA}" '
        NR>1 { rows++
               if ($1 < n) { p1+=$27 }
               else { p2+=$27; r+=$23; f+=$24; fc+=$31; ms+=$22 }
               un+=$32; if ($14!=1) b1++; if ($33!=1) b2++ }
        END { printf "%s\t%d\t%s\t%s\t%d\t%d\t%.4f\t%d\t%d\t%d\t%d\t%s\t%d/%d\t%d/%d\t%d\n",
                     c, rows, m, a, p1, p2, p1?p2/p1:0, r, f, fc, ms, w,
                     rows-b1, rows, rows-b2, rows, un }' $R/$tag.tsv >> $OUT
      tail -1 $OUT >&2
    done
  done
done
echo "REBUILD WIDTH DONE $(date +%T)" >&2
