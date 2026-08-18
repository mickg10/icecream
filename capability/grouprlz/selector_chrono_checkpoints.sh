#!/usr/bin/env bash
# TU100 / TU200 chronological checkpoints: what each codec would have emitted by TU N,
# against the zstd-6 --long=31 cap on the same raw prefix.
set -uo pipefail
C=$1
W=$HOME/selbind/chrono/$C; rm -rf $W; mkdir -p $W
MAN=$HOME/ictmp/$C/manifest.txt
GRZ=$HOME/issue16-selector-v1/tools/grz2g-selector
P29=$HOME/issue16-p29-prefix-state/bin/codec50-stable-root-final-l23
P29C="--z 3 --mixed-regions --byte-array-lines --direct-ordinals --compressed-blobs --blob-threads 8 --blob-lazy-fallback --mo-factor --s1-max-chain 1024 --blob-z 9 --blob-zstd-workers 4 --blob-zstd-job-mib 5 --blob-zstd-overlap-log 3"
TOT=$(wc -l < $MAN)
for N in 100 200; do
  [ $TOT -lt $N ] && { echo -e "$C\t$N\tSKIP_total_tus=$TOT"; continue; }
  head -n $N $MAN > $W/m$N
  tr "\n" "\0" < $W/m$N | xargs -0 cat > $W/p$N.ii
  $GRZ tu $W/m$N $W/p$N.tu > /dev/null
  RAW=$(stat -Lc %s $W/p$N.ii)
  Z6=$(zstd -q -6 --long=31 -T0 -c $W/p$N.ii | wc -c)
  taskset -c 24-31 $GRZ enc $W/p$N.ii $W/g$N.grz -u $W/p$N.tu -m g2 -K 256 -s 6 -t 21 -l 4 -k 5 -b 8 --gtu 112 --graw 512 --gadd 128 --hist 1024 -j 8 > $W/g$N.out 2>/dev/null
  G=$(cut -f2 $W/g$N.out)
  BLIND=""; [ $N -lt $TOT ] && BLIND="--open-final-entropy"
  taskset -c 16-23 $P29 --manifest $W/m$N $P29C --mixed-dump-prefix $W/pl$N > /dev/null 2>&1
  taskset -c 16-23 $P29 --manifest $W/m$N $P29C --literal-group-prefix $W/pl$N --literal-group-tus 112 --stable-root-tags $BLIND --literal-group-workers 8 --literal-group-skip-zstd10 --literal-group-wire $W/pw$N > $W/p$N.out 2>/dev/null
  P=$(grep -o "TOTAL=[0-9]*" $W/p$N.out | head -1 | cut -d= -f2)
  rm -f $W/p$N.ii $W/pl$N.*.raw
  awk -v c=$C -v n=$N -v raw=$RAW -v z=$Z6 -v g=$G -v p=$P "BEGIN{
    m=(g<p)?g:p; printf \"%s\t%d\t%d\t%d\t%s\t%s\t%d\t%.4f\t%s\n\",c,n,raw,z,g,p,m,m/z,(m<=z)?\"PASS\":\"FAIL\"}"
done
