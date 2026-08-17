#!/bin/bash
# G0/G1/G2 binding runner.
# Gate (decimal bytes/sec, as specified): C >= 1,000,000,000 B/s AND single-thread F >= 500,000,000 B/s.
# F is ALWAYS run with -j1. All reps retained. Peak RSS from getrusage on each side.
cd ~/grouprlz
OUT=${OUT:-$HOME/grouprlz/g012.tsv}
REPS=${REPS:-3}
CORES=${CORES:-0-15}
OPTS=${OPTS:-"-K 256 -s 5 -l 2 -k 1 -b 8 -j 8"}
[ -f "$OUT" ] || echo -e "id\tname\tmode\tgtu\tgraw_MB\tgadd_MB\thist_MB\traw\twp_z19\tout\tratio\tgroups\tidx_bytes\tC_Bps\tC_MiBps\tF_Bps\tF_MiBps\tC_reps\tF_reps\tC_RSS_GiB\tF_RSS_GiB\tF_ring_GiB\texact\tgate_size\tgate_C\tgate_F\tPASS" > "$OUT"

for c in $CORPORA; do
  Z=$(awk -F'\t' -v c=$c 'NR>1 && $1==c{print $5}' corpora.tsv)
  NM=$(awk -F'\t' -v c=$c 'NR>1 && $1==c && $3=="u8"{print $2}' universal.tsv | head -1)
  F=ii/$c.ii; U=ii/$c.tu
  [ -s "$U" ] || ./grz2g tu ~/ictmp/$c/manifest.txt $U >/dev/null
  RAW=$(stat -Lc %s $F)
  REF=$(sha256sum < $F | cut -d' ' -f1)
  for m in $MODES; do
    grep -qP "^$c\t[^\t]*\t$m\t$GTU\t" "$OUT" && continue
    EX=""; [ "$m" != "g0" ] && EX="-u $U --gtu $GTU --graw $GRAW --gadd $GADD --hist $HIST"
    [ "$m" = "g0" ] && EX="-u $U"
    creps=""; cbest=999999; out=0; grp=0; idx=0; crss=0
    for i in $(seq $REPS); do
      e=$(taskset -c $CORES ./grz2g enc $F /tmp/gg$$.grz -m $m $OPTS $EX --curve $HOME/grouprlz/curve.$c.$m.tsv 2>/dev/null) || { echo "ENCFAIL $c $m"; break; }
      t=$(echo "$e"|cut -f6); out=$(echo "$e"|cut -f2); grp=$(echo "$e"|cut -f3); idx=$(echo "$e"|cut -f7); crss=$(echo "$e"|cut -f8)
      creps="$creps$t,"; awk -v a=$t -v b=$cbest 'BEGIN{exit !(a<b)}' && cbest=$t
    done
    [ "$out" = "0" ] && continue
    freps=""; fbest=999999; frss=0; ring=0
    for i in $(seq $REPS); do
      d=$(taskset -c $CORES ./grz2g dec /tmp/gg$$.grz /dev/null -j 1 2>/dev/null) || { echo "DECFAIL $c $m"; break; }
      t=$(echo "$d"|cut -f4); ring=$(echo "$d"|cut -f5); frss=$(echo "$d"|cut -f6)
      freps="$freps$t,"; awk -v a=$t -v b=$fbest 'BEGIN{exit !(a<b)}' && fbest=$t
    done
    if taskset -c $CORES ./grz2g dec /tmp/gg$$.grz /tmp/gg$$.out -j 1 >/dev/null 2>&1; then
      got=$(sha256sum < /tmp/gg$$.out | cut -d' ' -f1)
    else
      got=DECODE_FAILED
    fi
    [ "$got" = "$REF" ] && X=YES || X=NO
    rm -f /tmp/gg$$.out /tmp/gg$$.grz
    awk -v c=$c -v nm="$NM" -v m=$m -v gtu=$GTU -v graw=$GRAW -v gadd=$GADD -v hist=$HIST \
        -v raw=$RAW -v z=$Z -v o=$out -v g=$grp -v idx=$idx -v ce=$cbest -v fe=$fbest \
        -v cr="$creps" -v fr="$freps" -v crss=$crss -v frss=$frss -v ring=$ring -v x=$X 'BEGIN{
      cb=raw/ce; fb=raw/fe; r=o/z;
      gs=(r<=1.10)?"PASS":"FAIL"; gc=(cb>=1e9)?"PASS":"FAIL"; gf=(fb>=5e8)?"PASS":"FAIL";
      printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%.0f\t%d\t%d\t%.4f\t%d\t%d\t%.0f\t%.1f\t%.0f\t%.1f\t%s\t%s\t%.2f\t%.2f\t%.2f\t%s\t%s\t%s\t%s\t%s\n",
        c,nm,m,gtu,graw,gadd,hist,raw,z,o,r,g,idx,cb,cb/1048576,fb,fb/1048576,cr,fr,
        crss/1073741824,frss/1073741824,ring/1073741824,x,gs,gc,gf,
        (gs=="PASS"&&gc=="PASS"&&gf=="PASS"&&x=="YES")?"PASS":"FAIL"}' | tee -a "$OUT"
  done
done
echo G012_DONE
