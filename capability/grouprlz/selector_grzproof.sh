#!/usr/bin/env bash
# Multi-cell GRZ2 in-stream closure proof.  Per cell, all six points, each verified rather
# than asserted: one continuing stream with --build-tus, offsets at every close, each
# k-build stream decoded against builds 1..k, and cmp proving the k-build stream is a
# byte-prefix of the 4-build stream.
set -Eeuo pipefail
G=$HOME/selbind/grzflush/grz2g-flush
GSEL=$HOME/issue16-selector-v1/tools/grz2g-selector
MX=$HOME/ictmp/ii-matrix
W=$HOME/selbind/grzproof; mkdir -p $W
POL="-m g2 -K 256 -s 6 -t 21 -l 4 -k 5 -b 8 --gtu 112 --graw 512 --gadd 128 --hist 1024 -j 8"
OUT=$W/proof.tsv
[ -f $OUT ] || printf "cell\ttus_per_build\toff1\toff2\toff3\toff4\tcontainer4\tterm_bytes\tb1\tb2\tb3\tb4_with_term\tprefix_1in4\tprefix_2in4\tdecode_k1\tdecode_k2\tdecode_k4\tbuild_close_rows\tenc_GBps\tdec_GBps\n" > $OUT

cell() {
  local PROJ=$1 PROF=$2 TAG=$1.$2
  local D=$W/$TAG; rm -rf $D; mkdir -p $D
  if [ "$PROF" = native ]; then
    cp $HOME/ictmp/$PROJ/manifest.txt $D/man.txt
  else
    local J=$MX/$PROJ/$PROF/corpus.json
    [ -f "$J" ] || { echo "$TAG SKIP no corpus.json" >&2; return 0; }
    local REL SHA TUS
    read -r REL SHA TUS < <(python3 -c "
import json;d=json.load(open('$J'));print(d['payload']['path'],d['payload']['sha256'],d['tu_count'])")
    [ "$(sha256sum $MX/$PROJ/$PROF/$REL | cut -d' ' -f1)" = "$SHA" ] || { echo "$TAG SKIP sha" >&2; return 0; }
    mkdir -p $D/ii
    zstd -d --long=31 -c $MX/$PROJ/$PROF/$REL 2>/dev/null | tar -xf - -C $D/ii
    find $D/ii -type f -name '*.ii' | sort > $D/man.txt
    [ "$(wc -l < $D/man.txt)" = "$TUS" ] || { echo "$TAG SKIP tu_count" >&2; return 0; }
  fi
  local N; N=$(wc -l < $D/man.txt)
  $GSEL tu $D/man.txt $D/tu1 >/dev/null 2>&1
  for k in 1 2 4; do
    : > $D/m$k; for i in $(seq 1 $k); do cat $D/man.txt >> $D/m$k; done
    tr '\n' '\0' < $D/m$k | xargs -0 cat > $D/ii$k
    python3 $HOME/selbind/tu4.py $D/tu1 $D/tu$k.map $k >/dev/null
    $G enc $D/ii$k $D/g$k.grz -u $D/tu$k.map $POL --build-tus $N --curve $D/c$k.tsv > $D/e$k.log 2>&1
  done
  local o1 o2 o3 o4
  o1=$(awk -F'\t' -v n=$N        '$3==n{print $11}'   $D/c4.tsv)
  o2=$(awk -F'\t' -v n=$((2*N))  '$3==n{print $11}'   $D/c4.tsv)
  o3=$(awk -F'\t' -v n=$((3*N))  '$3==n{print $11}'   $D/c4.tsv)
  o4=$(awk -F'\t' -v n=$((4*N))  '$3==n{print $11}'   $D/c4.tsv)
  local p1 p2 d1 d2 d4 allb
  cmp -n "$o1" $D/g1.grz $D/g4.grz >/dev/null 2>&1 && p1=IDENTICAL || p1=DIFFER
  cmp -n "$o2" $D/g2.grz $D/g4.grz >/dev/null 2>&1 && p2=IDENTICAL || p2=DIFFER
  for k in 1 2 4; do
    if $G dec $D/g$k.grz $D/d$k > $D/d$k.log 2>&1 && cmp -s $D/d$k $D/ii$k; then eval "d$k=EXACT"; else eval "d$k=FAIL"; fi
    rm -f $D/d$k
  done
  # exactly four build-close rows, at exactly N,2N,3N,4N
  nclose=$(awk -F'\t' -v n=$N 'NR>1 && $10=="build" && $3%n==0 {c++} END{print c+0}' $D/c4.tsv)
  allb=$([ "$nclose" = 4 ] && echo "4/4" || echo "$nclose/4_BAD")
  cont=$(stat -c %s $D/g4.grz); term=$((cont-o4))
  local egb dgb
  egb=$(grep -oE "C=[0-9]+ B/s" $D/e4.log 2>/dev/null | head -1 | grep -oE "[0-9]+" | awk '{printf "%.3f", $1/1e9}')
  dgb=$(grep -oE "F=[0-9]+ B/s" $D/d4.log 2>/dev/null | head -1 | grep -oE "[0-9]+" | awk '{printf "%.3f", $1/1e9}')
  printf "%s\t%d\t%s\t%s\t%s\t%s\t%s\t%s\t%d\t%d\t%d\t%d\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" \
    "$TAG" "$N" "$o1" "$o2" "$o3" "$o4" "$cont" "$term" \
    "$o1" "$((o2-o1))" "$((o3-o2))" "$(((o4-o3)+term))" \
    "$p1" "$p2" "$d1" "$d2" "$d4" "$allb" "${egb:-NA}" "${dgb:-NA}" >> $OUT
  tail -1 $OUT >&2
  rm -rf $D/ii $D/ii1 $D/ii2 $D/ii4
}

for spec in "$@"; do cell ${spec%%:*} ${spec##*:}; done
echo "GRZ PROOF DONE $(date +%T)" >&2
