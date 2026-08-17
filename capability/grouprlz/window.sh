#!/bin/bash
# Bounded-lookahead sweep. Three modes per K, to separate the cost of causal *framing*
# from the cost of *window bounding*:
#   cold  : -c 0  -w 0   full corpus, one segment (not prefix-decodable)
#   frame : -c K  -w 0   checkpointed every K TUs, unbounded history (prefix-decodable)
#   bound : -c K  -w K   checkpointed AND references limited to the last K TUs
cd ~/grouprlz
OUT=${OUT:-$HOME/grouprlz/window.tsv}
CORES=${CORES:-0-15}
OPTS="-K 256 -s 5 -t 24 -l 2 -k 1 -b 25 -j 1"
[ -f "$OUT" ] || echo -e "corpus\tname\tmode\tK\traw\twp_z19\tout\tratio\tnseg\tenc_s\tenc_MBps\tdec_MBps\texact" > "$OUT"

z19_of() { awk -F'\t' -v c=$1 '$1==c{print $5}' corpora.tsv; }
name_of() { awk -F'\t' -v c=$1 '$1==c{print $2}' corpora.tsv | cut -d' ' -f1; }

for c in $CORPORA; do
  F=ii/$c.ii; U=ii/$c.tu
  [ -s "$U" ] || ./grzc tu ~/ictmp/$c/manifest.txt $U >/dev/null
  Z=$(z19_of $c); NM=$(name_of $c); RAW=$(stat -Lc %s $F)
  REF=$(sha256sum < $F | cut -d' ' -f1)
  for spec in $SPECS; do
    mode=${spec%%:*}; K=${spec##*:}
    grep -qP "^$c\t[^\t]*\t$mode\t$K\t" "$OUT" && continue
    case $mode in
      cold)  EX="";;
      frame) EX="-u $U -c $K";;
      bound) EX="-u $U -c $K -w $K";;
    esac
    e=$(taskset -c $CORES ./grzc enc $F /tmp/w$$.grz $OPTS $EX 2>/dev/null) || { echo "FAIL $c $mode $K"; continue; }
    out=$(echo "$e"|cut -f2); tenc=$(echo "$e"|cut -f6); nseg=$(echo "$e"|cut -f7)
    d=$(taskset -c $CORES ./grzc dec /tmp/w$$.grz /tmp/w$$.out 2>/dev/null)
    tdec=$(echo "$d"|cut -f4)
    got=$(sha256sum < /tmp/w$$.out | cut -d' ' -f1)
    [ "$got" = "$REF" ] && X=YES || X=NO
    rm -f /tmp/w$$.out /tmp/w$$.grz
    awk -v c=$c -v nm=$NM -v m=$mode -v k=$K -v raw=$RAW -v z=$Z -v o=$out -v ns=$nseg \
        -v te=$tenc -v td=$tdec -v x=$X 'BEGIN{mb=raw/1048576;
      printf "%s\t%s\t%s\t%s\t%.0f\t%d\t%d\t%.4f\t%d\t%.3f\t%.1f\t%.1f\t%s\n",c,nm,m,k,raw,z,o,o/z,ns,te,mb/te,mb/td,x}' | tee -a "$OUT"
  done
done
echo WINDOW_DONE
