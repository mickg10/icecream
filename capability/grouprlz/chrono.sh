#!/bin/bash
# Chronological gate: is the TRUNCATED PREFIX of the full-corpus grz wire a valid causal
# wire for W(P,100) / W(P,200), and does it beat the Z6-long chronological cap?
#   W(P,N) = full-corpus container cut at the checkpoint covering exactly N TUs
#   Z6(P,N) = zstd -6 --long=31 of concat(first N TUs)
# Each cut is decoded standalone and compared byte-for-byte against concat(first N TUs).
cd ~/grouprlz
OUT=$HOME/grouprlz/chrono.tsv
OPTS="-K 512 -s 5 -t 24 -l 2 -k 1 -b 8 -j 8"
[ -f "$OUT" ] || echo -e "id\tname\tN\tprefix_raw\tW_grz\tZ6\tratio_vs_Z6\tgate\tprefix_exact\twp_z19_full\tW_vs_z19full" > "$OUT"

for c in $CORPORA; do
  M=~/ictmp/$c/manifest.txt
  U=ii/$c.tu
  [ -s "$U" ] || ./grzc tu $M $U >/dev/null
  NM=$(awk -F'\t' -v c=$c 'NR>1 && $1==c && $3=="u8"{print $2}' universal.tsv | head -1)
  ZF=$(awk -F'\t' -v c=$c 'NR>1 && $1==c{print $5}' corpora.tsv)
  # one full-corpus encode, checkpointed every 100 TUs
  ./grzc enc ii/$c.ii /tmp/ch$$.grz $OPTS -u $U -c 100 >/dev/null 2>&1
  ./grzc idx /tmp/ch$$.grz > /tmp/ix$$.tsv
  for N in 100 200; do
    grep -qP "^$c\t[^\t]*\t$N\t" "$OUT" && continue
    CUT=$(awk -F'\t' -v n=$N 'NR>1 && $3==n{print $2}' /tmp/ix$$.tsv)
    [ -n "$CUT" ] || { echo "$c: no checkpoint at TU$N"; continue; }
    head -$N $M | tr '\n' '\0' | xargs -0 cat > /tmp/pre$$.ii
    PRAW=$(stat -c %s /tmp/pre$$.ii)
    zstd -q -f -6 --long=31 -T0 -o /tmp/z6$$.zst /tmp/pre$$.ii
    Z6=$(stat -c %s /tmp/z6$$.zst)
    head -c $CUT /tmp/ch$$.grz > /tmp/cut$$.grz
    ./grzc dec /tmp/cut$$.grz /tmp/dec$$.ii -j 8 >/dev/null 2>&1
    cmp -s /tmp/pre$$.ii /tmp/dec$$.ii && X=YES || X=NO
    awk -v c=$c -v nm="$NM" -v n=$N -v pr=$PRAW -v w=$CUT -v z6=$Z6 -v x=$X -v zf=$ZF 'BEGIN{
      printf "%s\t%s\t%d\t%.0f\t%d\t%d\t%.4f\t%s\t%s\t%d\t%.4f\n",
        c,nm,n,pr,w,z6,w/z6,(w<=z6?"PASS":"FAIL"),x,zf,w/zf}' | tee -a "$OUT"
    rm -f /tmp/pre$$.ii /tmp/z6$$.zst /tmp/cut$$.grz /tmp/dec$$.ii
  done
  rm -f /tmp/ch$$.grz /tmp/ix$$.tsv
done
echo CHRONO_DONE
