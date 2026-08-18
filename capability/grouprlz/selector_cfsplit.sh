#!/usr/bin/env bash
# Per-TU split on the C->F-only wire, under the owner's binding 30-slot cold-shard
# constraint, with the dense k=1 reference alongside.
set -uo pipefail
S=$HOME/selbind/m5split/cap_m5_dir
R=$HOME/selbind/cfsplit; mkdir -p $R
OUT=$R/cfsplit.tsv
[ -f $OUT ] || printf "corpus\ttus\tshards\twire\tcf_total\tfc_total\tcarry\tcf_root\tcf_fill\tcf_control\tfc_need\tfc_control\tmissing\tsplit_ok\tdir_ok\n" > $OUT
for C in "$@"; do
  for K in 1 30; do
    tag="$C.s$K"
    nice -n 5 taskset -c 0-31 $S --manifest $HOME/ictmp/$C/manifest.txt --codec z1 \
      --real-pipes --workers $K --curve-out $R/$tag.tsv > $R/$tag.out 2> $R/$tag.err
    [ -s $R/$tag.tsv ] || { echo "FAIL $tag" >&2; continue; }
    awk -F'\t' -v c="$C" -v k="$K" '
      NR>1 { n++; w+=$5; cf+=$26; fc+=$29; cy+=$13;
             cfr+=$23; cff+=$24; cfc+=$25; fcn+=$27; fcc+=$28; ms+=$22;
             if ($14!=1) b1++; if ($30!=1) b2++ }
      END { printf "%s\t%d\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d/%d\t%d/%d\n",
                   c,n,k,w,cf,fc,cy,cfr,cff,cfc,fcn,fcc,ms,n-b1,n,n-b2,n }' $R/$tag.tsv >> $OUT
    tail -1 $OUT >&2
  done
done
echo "CF SPLIT DONE $(date +%T)" >&2
