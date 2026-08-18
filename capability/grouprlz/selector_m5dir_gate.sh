#!/usr/bin/env bash
# Identity gate for the direction-split build: same wire, same summary, same pre-existing
# curve columns, and BOTH invariants closing on every row.
set -uo pipefail
C=${1:-corpus13}
W=$HOME/selbind/m5split/gatedir.$C; rm -rf $W; mkdir -p $W
MAN=$HOME/ictmp/$C/manifest.txt
B=$HOME/selbind/m5split/cap_m5_base
S=$HOME/selbind/m5split/cap_m5_dir
ARGS="--manifest $MAN --codec z1 --real-pipes"
for cfg in "--workers 1" "--workers 8" "--workers 30" "--workers 4 --latejoin-at 20"; do
  tag=$(echo $cfg | tr -d " -")
  taskset -c 0-31 $B $ARGS $cfg --curve-out $W/$tag.base.tsv > $W/$tag.base.out 2>&1
  taskset -c 0-31 $S $ARGS $cfg --curve-out $W/$tag.dir.tsv  > $W/$tag.dir.out  2>&1
  norm() { sed -E -e "s/[0-9]+\.[0-9]+//g" -e "s/_ns=[0-9]+//g" -e "s/rss[^ ]*/rss=X/gI" "$1"; }
  OUT=DIFF; cols=DIFF
  diff -q <(norm $W/$tag.base.out) <(norm $W/$tag.dir.out) >/dev/null 2>&1 && OUT=SAME
  diff -q <(cut -f1-5,7-8 $W/$tag.base.tsv) <(cut -f1-5,7-8 $W/$tag.dir.tsv) >/dev/null 2>&1 && cols=SAME
  n=$(( $(wc -l < $W/$tag.dir.tsv) - 1 ))
  bad=$(awk -F"\t" "NR>1 && \$14!=1" $W/$tag.dir.tsv | wc -l)
  dbad=$(awk -F"\t" "NR>1 && \$30!=1" $W/$tag.dir.tsv | wc -l)
  echo -e "$C\t$cfg\tsummary=$OUT\tcurve_cols=$cols\tsplit_ok=$((n-bad))/$n\tdir_ok=$((n-dbad))/$n"
done
