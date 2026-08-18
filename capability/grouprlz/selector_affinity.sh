#!/usr/bin/env bash
# DENSE_AFFINITY: what does routing a build to FEWER F daemons avoid?
#
# The wire is the primary measurement and is exact and deterministic; makespan is
# recorded too but the box has 32 cores, so k=32 is oversubscribed (C side + 32 real
# compiler-pipe consumers) and its wall time is indicative only.
set -uo pipefail
S=$HOME/selbind/m5split/cap_m5_dir
R=$HOME/selbind/affinity; mkdir -p $R
OUT=$R/affinity.tsv
[ -f $OUT ] || printf "corpus\ttus\tworkers\tassignment\traw\twire\tb_root\tb_need\tb_fill\tb_control\tb_carry\twall_s\tsplit_ok\tdistinct_workers\n" > $OUT
run() {   # corpus workers assignment
  local C=$1 K=$2 A=$3 tag="$1.k$2.$3"
  local MAN=$HOME/ictmp/$C/manifest.txt
  /usr/bin/time -f "%e %U %S %M" -o $R/$tag.res \
    nice -n 5 taskset -c 0-31 $S --manifest $MAN --codec z1 --real-pipes \
    --workers $K --assignment $A --curve-out $R/$tag.tsv > $R/$tag.out 2> $R/$tag.err
  [ -s $R/$tag.tsv ] || { echo "FAIL $tag" >&2; return; }
  local wall
  wall=$(sed -n 's/.*wall=\([0-9.]*\)s.*/\1/p' $R/$tag.out | head -1)
  awk -F'\t' -v c="$C" -v k="$K" -v a="$A" -v w="${wall:-NA}" '
    NR>1 { raw+=$4; wire+=$5; r+=$9; n+=$10; f+=$11; ct+=$12; cy+=$13;
           if ($14!=1) bad++; seen[$3]=1; rows++ }
    END { d=0; for (x in seen) d++
          printf "%s\t%d\t%s\t%s\t%.0f\t%d\t%d\t%d\t%d\t%d\t%d\t%s\t%d/%d\t%d\n",
                 c, rows, k, a, raw, wire, r, n, f, ct, cy, w, rows-bad, rows, d }' \
    $R/$tag.tsv >> $OUT
  tail -1 $OUT >&2
}
for C in "$@"; do
  for K in 1 2 4 8 16 32; do run $C $K roundrobin; done
  for A in sticky random; do run $C 8 $A; done
done
echo "AFFINITY DONE $(date +%T)" >&2
