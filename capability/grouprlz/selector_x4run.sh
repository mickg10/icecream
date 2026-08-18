#!/usr/bin/env bash
# 4-pass learning curves: run each corpus 4x back-to-back (cold pass 1 -> warm rebuilds 2-4)
# across the four native codecs.  The prediction line is generated separately (Python).
#
# One shared 4x manifest per corpus means every codec sees literally the same TU sequence,
# which is what makes the joined axis assertable.
set -uo pipefail
P29=$HOME/selbind/p29build/codec50-refZ
FUSED=$HOME/selbind/fused/fused-curveB
Z3=$HOME/selbind/zstd3tu
GRZ=$HOME/issue16-selector-v1/tools/grz2g-selector
X=$HOME/selbind/x4; mkdir -p $X
P29C="--z 3 --mixed-regions --byte-array-lines --direct-ordinals --compressed-blobs --blob-threads 8 --blob-lazy-fallback --mo-factor --s1-max-chain 1024 --blob-z 9 --blob-zstd-workers 4 --blob-zstd-job-mib 5 --blob-zstd-overlap-log 3"

for C in "$@"; do
  MAN=$HOME/ictmp/$C/manifest.txt
  [ -f "$MAN" ] || { echo "no manifest $C" >&2; continue; }
  M4=$X/$C.m4.txt
  for i in 1 2 3 4; do cat $MAN; done > $M4
  N=$(wc -l < $MAN); N4=$(wc -l < $M4)

  # --- zstd-3 per TU (no cross-TU memory) ---
  nice -n 5 $Z3 --manifest $M4 --curve $X/$C.zstd3.tsv -j 16 > $X/$C.zstd3.out 2>&1
  z3ok=$(grep -o 'sum_ok=[01]' $X/$C.zstd3.out | cut -d= -f2)

  # --- ultra-fast interner ---
  nice -n 5 taskset -c 0-31 $FUSED --manifest $M4 --level 3 --curve $X/$C.fast.tsv \
    > $X/$C.fast.out 2> $X/$C.fast.err
  fok=$(grep -c 'verify=PASS' $X/$C.fast.err)

  # --- P29+BSC (codec50), batch basis, its own per-TU curve ---
  T=$(mktemp -d)
  nice -n 5 taskset -c 0-31 $P29 --manifest $M4 $P29C --mixed-dump-prefix $T/pl > $T/pl.out 2>&1
  nice -n 5 taskset -c 0-31 $P29 --manifest $M4 $P29C --literal-group-prefix $T/pl \
    --literal-group-tus 112 --stable-root-tags --literal-group-workers 8 \
    --literal-group-skip-zstd10 --literal-group-wire $T/lit \
    --curve-tsv $X/$C.p29.tsv > $X/$C.p29.out 2> $X/$C.p29.err
  ptot=$(grep -o 'TOTAL=[0-9]*' $X/$C.p29.out | head -1 | cut -d= -f2)
  pend=$(tail -1 $X/$C.p29.tsv | cut -f5)
  rm -rf $T

  # --- GRZ2: needs the 4x .ii and its TU map ---
  if [ -f $HOME/grouprlz/ii/$C.ii ]; then
    for i in 1 2 3 4; do cat $HOME/grouprlz/ii/$C.ii; done > $X/$C.ii4
    nice -n 5 $GRZ tu $M4 $X/$C.tu4 > /dev/null 2>&1
    nice -n 5 taskset -c 0-31 $GRZ enc $X/$C.ii4 $X/$C.grz4 -u $X/$C.tu4 \
      -m g2 -K 256 -s 6 -t 21 -l 4 -k 5 -b 8 --gtu 112 --graw 512 --gadd 128 --hist 1024 -j 8 \
      --curve $X/$C.grz2.tsv > $X/$C.grz2.out 2> $X/$C.grz2.err
    gw=$(cut -f2 $X/$C.grz2.out 2>/dev/null)
    rm -f $X/$C.ii4 $X/$C.grz4
  else
    gw=NA
  fi
  echo "$C tus=$N x4=$N4 zstd3_sum_ok=$z3ok fused_verify=$fok p29_total=$ptot p29_curve_end=$pend p29_match=$([ "$ptot" = "$pend" ] && echo YES || echo NO) grz2_wire=$gw" >&2
done
echo "X4 RUN DONE $(date +%T)" >&2
