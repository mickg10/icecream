#!/usr/bin/env bash
# Identity gate for the observation-only per-TU split: the instrumented build must
# emit the same wire, the same summary and the same pre-existing curve columns.
set -uo pipefail
C=${1:-corpus13}
W=$HOME/selbind/m5split/gate.$C; rm -rf $W; mkdir -p $W
MAN=$HOME/ictmp/$C/manifest.txt
B=$HOME/selbind/m5split/cap_m5_base
S=$HOME/selbind/m5split/cap_m5_split
ARGS="--manifest $MAN --codec z1 --real-pipes"
for cfg in "--workers 1" "--workers 8" "--workers 4 --latejoin-at 20"; do
  tag=$(echo $cfg | tr -d ' -')
  taskset -c 0-15 $B $ARGS $cfg --curve-out $W/$tag.base.tsv > $W/$tag.base.out 2>&1
  taskset -c 0-15 $S $ARGS $cfg --curve-out $W/$tag.split.tsv > $W/$tag.split.out 2>&1
  # strip every timing/rate field; keep byte counts, ledgers, exactness flags
  norm() { sed -E -e 's/[0-9]+\.[0-9]+//g' -e 's/(ns|seconds|elapsed|wall|rate|MB_s|GB_s|us)=[0-9]+//g' \
                  -e 's/_ns=[0-9]+//g' -e 's/rss[^ ]*/rss=X/gI' "$1"; }
  OUT=DIFF; cols=DIFF
  diff -q <(norm $W/$tag.base.out) <(norm $W/$tag.split.out) >/dev/null 2>&1 && OUT=SAME
  # pre-existing curve columns 1-5,7,8 (drop latency_ns, col 6)
  diff -q <(cut -f1-5,7-8 $W/$tag.base.tsv) <(cut -f1-5,7-8 $W/$tag.split.tsv) >/dev/null 2>&1 && cols=SAME
  # every split row must close against its own wire
  bad=$(awk -F'\t' 'NR>1 && $14!=1' $W/$tag.split.tsv | wc -l)
  n=$(( $(wc -l < $W/$tag.split.tsv) - 1 ))
  echo -e "$C\t$cfg\tsummary=$OUT\tcurve_cols=$cols\tsplit_ok=$((n-bad))/$n"
done
