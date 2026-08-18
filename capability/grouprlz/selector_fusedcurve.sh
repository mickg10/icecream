#!/usr/bin/env bash
# Ultra-fast interner (PRODUCTION_FUSED) per-TU cumulative wire, level 3.
# Identity control: the unpatched build must report the same body/comp/miss/occ totals.
set -uo pipefail
D=$HOME/selbind/fused; W=$HOME/selbind/curves
for C in "$@"; do
  MAN=$HOME/ictmp/$C/manifest.txt
  nice -n 5 taskset -c 0-15 $D/fused-curve --manifest $MAN --level 3 \
    --curve $W/$C.fusedcurve.tsv > $W/$C.fused.out 2> $W/$C.fused.err
  nice -n 5 taskset -c 0-15 $D/fused-base --manifest $MAN --level 3 \
    > $W/$C.fusedbase.out 2> $W/$C.fusedbase.err
  norm() { grep -E "^   body=|^FUSED" "$1"; }
  ID=FAIL; diff -q <(norm $W/$C.fused.err) <(norm $W/$C.fusedbase.err) >/dev/null 2>&1 && ID=PASS
  V=$(grep -c "verify=PASS" $W/$C.fused.err)
  CUM=$(tail -1 $W/$C.fusedcurve.tsv | cut -f5)
  echo "$C identity=$ID verify=$V/2 cum_wire=$CUM rows=$(( $(wc -l < $W/$C.fusedcurve.tsv) - 1 ))" >&2
done
echo "FUSED CURVES DONE $(date +%T)" >&2
