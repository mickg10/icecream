#!/bin/bash
# Rate pass. Waits for the box to go quiet (another agent's LLVM build saturates all 32
# cores), then measures C/F throughput with 3 reps each, best kept. Sizes come from
# universal.tsv and are unaffected by load; only these numbers need a quiet box.
cd ~/grouprlz
OUT=$HOME/grouprlz/rates.tsv
OPTS="-K 512 -s 5 -l 2 -k 1 -b 8 -j 8"
DJ=8
[ -f "$OUT" ] || echo -e "id\tname\traw\tout\tratio\tenc_s\tC_MBps\tdec_s\tF_MBps\texact\tC\tF\tsize_gate\tPASS\tload_at_run" > "$OUT"

while :; do
  n=$(pgrep -c cc1plus 2>/dev/null || echo 0)
  l=$(awk '{print int($1)}' /proc/loadavg)
  [ "${n:-0}" -eq 0 ] && [ "${l:-99}" -lt 4 ] && break
  sleep 60
done
sleep 30

for c in $CORPORA; do
  grep -qP "^$c\t" "$OUT" && continue
  row=$(awk -F'\t' -v c=$c 'NR>1 && $1==c' corpora.tsv)
  [ -n "$row" ] || continue
  Z=$(echo "$row"|cut -f5)
  NM=$(awk -F'\t' -v c=$c 'NR>1 && $1==c && $3=="u8"{print $2}' universal.tsv | head -1)
  F=ii/$c.ii
  RAW=$(stat -Lc %s $F)
  REF=$(sha256sum < $F | cut -d' ' -f1)
  be=99999; out=0
  for i in 1 2 3; do
    e=$(taskset -c 0-15 ./grzc enc $F /tmp/r$$.grz $OPTS 2>/dev/null)
    t=$(echo "$e"|cut -f6); out=$(echo "$e"|cut -f2)
    awk -v a=$t -v b=$be 'BEGIN{exit !(a<b)}' && be=$t
  done
  bd=99999
  for i in 1 2 3; do
    d=$(taskset -c 0-15 ./grzc dec /tmp/r$$.grz /dev/null -j $DJ 2>/dev/null)
    t=$(echo "$d"|cut -f4); awk -v a=$t -v b=$bd 'BEGIN{exit !(a<b)}' && bd=$t
  done
  taskset -c 0-15 ./grzc dec /tmp/r$$.grz /tmp/r$$.out -j $DJ >/dev/null 2>&1
  got=$(sha256sum < /tmp/r$$.out | cut -d' ' -f1)
  [ "$got" = "$REF" ] && X=YES || X=NO
  rm -f /tmp/r$$.out /tmp/r$$.grz
  LD=$(cut -d' ' -f1 /proc/loadavg)
  awk -v c=$c -v nm="$NM" -v raw=$RAW -v o=$out -v z=$Z -v te=$be -v td=$bd -v x=$X -v ld=$LD 'BEGIN{
    mb=raw/1048576; cv=mb/te; fv=mb/td; r=o/z;
    cc=(cv>=1024)?"PASS":"FAIL"; ff=(fv>=1024)?"PASS":"FAIL"; g=(r<=1.10)?"PASS":"FAIL";
    printf "%s\t%s\t%.0f\t%d\t%.4f\t%.3f\t%.1f\t%.3f\t%.1f\t%s\t%s\t%s\t%s\t%s\t%s\n",
      c,nm,raw,o,r,te,cv,td,fv,x,cc,ff,g,(g=="PASS"&&cc=="PASS"&&ff=="PASS"&&x=="YES")?"PASS":"FAIL",ld}' >> "$OUT"
done
echo RATES_DONE
