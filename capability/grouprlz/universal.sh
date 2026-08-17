#!/bin/bash
# Universal-cold test: ONE fixed config across every corpus (no per-corpus tuning).
# Gate is two-sided: ratio <= 1.10x wp_z19 AND compress >= 1 GB/s AND decompress >= 1 GB/s.
# usage: env CORPORA=".." CFG=u8|u1 universal.sh
cd ~/grouprlz
OUT=${OUT:-$HOME/grouprlz/universal.tsv}
CORES=${CORES:-0-15}
CFG=${CFG:-u8}
case $CFG in
  u8) OPTS="-K 512 -s 5 -l 2 -k 1 -b 8 -j 8"; DJ=8;;   # 8 threads on the literal blocks
  u1) OPTS="-K 256 -s 5 -l 4 -k 5 -b 8 -j 1"; DJ=1;;   # strict single thread
esac
[ -f "$OUT" ] || echo -e "corpus\tname\tcfg\ttus\traw\twp_z19\tp29_bsc\tout\tratio\tp29_ratio\tenc_s\tC_MBps\tdec_s\tF_MBps\texact\tgate\tC\tF\tPASS" > "$OUT"

p29_of() { awk -F'\t' -v n="$1" 'NR>1 && $1==n{w=$6} END{print (w==""?0:w)}' ~/capability/rbase-p29-fixed16-per-tu.tsv; }

for c in $CORPORA; do
  grep -qP "^$c\t[^\t]*\t$CFG\t" "$OUT" && continue
  row=$(awk -F'\t' -v c=$c '$1==c' corpora.tsv)
  [ -n "$row" ] || { echo "no baseline for $c"; continue; }
  NM=$(echo "$row"|cut -f2|cut -d' ' -f1); TUS=$(echo "$row"|cut -f3); Z=$(echo "$row"|cut -f5)
  case $NM in corpus) NM=llvm;; corpus2) NM=rocksdb;; corpus3) NM=duckdb;; corpus4) NM=abseil;;
              corpus5) NM=opencv;; corpus6) NM=godot;; corpus12) NM=eigen;; esac
  P29=$(p29_of $NM)
  F=ii/$c.ii
  RAW=$(stat -Lc %s $F)
  REF=$(sha256sum < $F | cut -d' ' -f1)
  e=$(taskset -c $CORES ./grzc enc $F /tmp/u$$.grz $OPTS 2>/dev/null) || { echo "ENCFAIL $c"; continue; }
  out=$(echo "$e"|cut -f2); tenc=$(echo "$e"|cut -f6)
  bd=99999
  for i in 1 2; do
    d=$(taskset -c $CORES ./grzc dec /tmp/u$$.grz /dev/null -j $DJ 2>/dev/null)
    t=$(echo "$d"|cut -f4); awk -v a=$t -v b=$bd 'BEGIN{exit !(a<b)}' && bd=$t
  done
  taskset -c $CORES ./grzc dec /tmp/u$$.grz /tmp/u$$.out -j $DJ >/dev/null 2>&1
  got=$(sha256sum < /tmp/u$$.out | cut -d' ' -f1)
  [ "$got" = "$REF" ] && X=YES || X=NO
  rm -f /tmp/u$$.out /tmp/u$$.grz
  awk -v c=$c -v nm=$NM -v cf=$CFG -v t=$TUS -v raw=$RAW -v z=$Z -v p=$P29 -v o=$out -v te=$tenc -v td=$bd -v x=$X \
    'BEGIN{mb=raw/1048576; cv=mb/te; fv=mb/td; r=o/z;
     g=(r<=1.10)?"PASS":"FAIL"; cc=(cv>=1024)?"PASS":"FAIL"; ff=(fv>=1024)?"PASS":"FAIL";
     printf "%s\t%s\t%s\t%d\t%.0f\t%d\t%d\t%d\t%.4f\t%.4f\t%.3f\t%.1f\t%.3f\t%.1f\t%s\t%s\t%s\t%s\t%s\n",
       c,nm,cf,t,raw,z,p,o,r,(p>0?p/z:0),te,cv,td,fv,x,g,cc,ff,
       (g=="PASS"&&cc=="PASS"&&ff=="PASS"&&x=="YES")?"PASS":"FAIL"}' | tee -a "$OUT"
done
echo UNIVERSAL_DONE
