#!/usr/bin/env bash
# Cross-check against local-oracle 211bd585, which carries NATIVE per-TU category fields.
# Their columns:  9 c_to_f  10 f_to_c  13 c_root 14 c_fill 15 c_control 16 f_need
#                 17 f_control  18 direction_ok  19 category_ok
# Mine (v2):     23 cf_root 24 cf_fill 25 cf_control 26 cf_carry 27 cf_total
#                28 fc_need 29 fc_control 30 fc_carry 31 fc_total 32 carry_undirected 33 dir_ok
set -uo pipefail
LO=$HOME/selbind/lo211/cap_m5_lo211
MINE=$HOME/selbind/m5split/cap_m5_dir2
W=$HOME/selbind/xcheck211; rm -rf $W; mkdir -p $W
C=${1:-corpus11}
for M in 1 30; do
  nice -n 5 taskset -c 0-31 $LO   --manifest $HOME/ictmp/$C/manifest.txt --codec z1 --real-pipes \
    --workers $M --curve-out $W/lo.M$M.tsv   > $W/lo.M$M.out 2>&1
  nice -n 5 taskset -c 0-31 $MINE --manifest $HOME/ictmp/$C/manifest.txt --codec z1 --real-pipes \
    --workers $M --curve-out $W/mine.M$M.tsv > $W/mine.M$M.out 2>&1
done
tot() { awk -F'\t' -v c="$2" 'NR>1{s+=$c}END{print s+0}' "$1"; }
echo "CROSS-CHECK vs local-oracle 211bd585 -- corpus $C"
printf "%-26s %14s %14s %s\n" quantity local-oracle selector match
for M in 1 30; do
  echo "--- M=$M ---"
  for pair in "C->F:9:27" "F->C:10:31"; do
    n=${pair%%:*}; rest=${pair#*:}; a=${rest%%:*}; b=${rest##*:}
    L=$(tot $W/lo.M$M.tsv $a); S=$(tot $W/mine.M$M.tsv $b)
    printf "%-26s %14s %14s %s\n" "$n" "$L" "$S" "$([ "$L" = "$S" ] && echo IDENTICAL || echo "DIFF $((L-S))")"
  done
  # category split: their c_control folds the carry, so compare mine + carry
  for pair in "c_root:13:23:0" "c_fill:14:24:0" "c_control:15:25:26" "f_need:16:28:0" "f_control:17:29:30"; do
    IFS=: read -r n a b cc <<< "$pair"
    L=$(tot $W/lo.M$M.tsv $a); S=$(tot $W/mine.M$M.tsv $b)
    [ "$cc" != "0" ] && S=$(( S + $(tot $W/mine.M$M.tsv $cc) ))
    printf "%-26s %14s %14s %s\n" "$n" "$L" "$S" "$([ "$L" = "$S" ] && echo IDENTICAL || echo "DIFF $((L-S))")"
  done
  # per-row agreement on the five categories
  d=$(paste <(cut -f13-17 $W/lo.M$M.tsv | tail -n +2) \
            <(awk -F'\t' 'NR>1{printf "%d\t%d\t%d\t%d\t%d\n",$23,$24,$25+$26,$28,$29+$30}' $W/mine.M$M.tsv) \
      | awk '$1!=$6||$2!=$7||$3!=$8||$4!=$9||$5!=$10' | wc -l)
  rows=$(( $(wc -l < $W/lo.M$M.tsv) - 1 ))
  printf "%-26s %14s / %s rows\n" "per-row category diffs" "$d" "$rows"
  printf "%-26s lo direction_ok=%s category_ok=%s | mine dir_ok=%s\n" "gates (failures)" \
    "$(awk -F'\t' 'NR>1 && $18!=1' $W/lo.M$M.tsv | wc -l)" \
    "$(awk -F'\t' 'NR>1 && $19!=1' $W/lo.M$M.tsv | wc -l)" \
    "$(awk -F'\t' 'NR>1 && $33!=1' $W/mine.M$M.tsv | wc -l)"
done
