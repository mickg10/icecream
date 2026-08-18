#!/usr/bin/env bash
# Cache-domain WIDTH sweep: M = distinct selected F daemons (caches opened), not slots.
# Direction-exact (v2 ledger: the carry is attributed, carry_undirected == 0).
set -uo pipefail
S=$HOME/selbind/m5split/cap_m5_dir2
R=$HOME/selbind/width; mkdir -p $R
OUT=$R/width.tsv
[ -f $OUT ] || printf "corpus\ttus\tM\twire\tcf_total\tfc_total\tcf_root\tcf_fill\tcf_control\tcf_carry\tfc_carry\tcarry_undirected\tmissing\twall_s\tsplit_ok\tdir_ok\n" > $OUT
for C in "$@"; do
  for M in 1 2 4 8 16 30; do
    tag="$C.M$M"
    nice -n 5 taskset -c 0-31 $S --manifest $HOME/ictmp/$C/manifest.txt --codec z1 \
      --real-pipes --workers $M --curve-out $R/$tag.tsv > $R/$tag.out 2> $R/$tag.err
    [ -s $R/$tag.tsv ] || { echo "FAIL $tag" >&2; continue; }
    wall=$(sed -n 's/.*wall=\([0-9.]*\)s.*/\1/p' $R/$tag.out | head -1)
    awk -F'\t' -v c="$C" -v m="$M" -v w="${wall:-NA}" '
      NR>1 { n++; wi+=$5; cf+=$27; fc+=$31; cr+=$23; cfi+=$24; cc+=$25; cy+=$26; fy+=$30;
             un+=$32; ms+=$22; if ($14!=1) b1++; if ($33!=1) b2++ }
      END { printf "%s\t%d\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%s\t%d/%d\t%d/%d\n",
                   c,n,m,wi,cf,fc,cr,cfi,cc,cy,fy,un,ms,w,n-b1,n,n-b2,n }' $R/$tag.tsv >> $OUT
    tail -1 $OUT >&2
  done
done
echo "WIDTH SWEEP DONE $(date +%T)" >&2
