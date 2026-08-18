#!/usr/bin/env bash
# Per-TU Root/Need/Fill byte split over real corpora.  The split is deterministic
# (byte counts, not clocks), so one rep per configuration is enough; the identity
# gate has already shown the instrumentation does not move the wire.
set -uo pipefail
S=$HOME/selbind/m5split/cap_m5_split
B=$HOME/selbind/m5split/cap_m5_base
R=$HOME/selbind/m5split/runs; mkdir -p $R
for C in "$@"; do
  MAN=$HOME/ictmp/$C/manifest.txt
  [ -f "$MAN" ] || { echo "no manifest $C" >&2; continue; }
  for W in 1 8; do
    tag=$C.w$W
    nice -n 5 taskset -c 0-15 $S --manifest $MAN --codec z1 --real-pipes --workers $W \
      --curve-out $R/$tag.tsv > $R/$tag.out 2> $R/$tag.err
    # identity control on the same cell: unpatched build, same flags
    nice -n 5 taskset -c 0-15 $B --manifest $MAN --codec z1 --real-pipes --workers $W \
      --curve-out $R/$tag.base.tsv > $R/$tag.base.out 2> $R/$tag.base.err
    cols=DIFF
    diff -q <(cut -f1-5,7-8 $R/$tag.base.tsv) <(cut -f1-5,7-8 $R/$tag.tsv) >/dev/null 2>&1 && cols=SAME
    n=$(( $(wc -l < $R/$tag.tsv) - 1 ))
    bad=$(awk -F'\t' 'NR>1 && $14!=1' $R/$tag.tsv | wc -l)
    echo "$tag curve_cols=$cols split_ok=$((n-bad))/$n" >&2
  done
done
echo "SPLIT RUNS DONE $(date +%T)" >&2
