#!/bin/bash
# Final verified GROUP-RLZ measurement: 3 timed reps (best), then a full byte-exact decode.
# usage: final2.sh <corpus> <K> <sbits> <litbe> <tokbe> <blkMB> <threads> <tag>
cd ~/grouprlz
NM=$1; K=$2; S=$3; LB=$4; TK=$5; BLK=$6; THR=${7:-1}; TAG=${8:-}
CORES=${CORES:-0-15}
OUT=${OUT:-$HOME/grouprlz/final.tsv}
[ -f "$OUT" ] || echo -e "corpus\ttag\tK\tsbits\tlitbe\ttokbe\tblkMB\tthr\traw\tout\tratio_z19\tvs_gate\tenc_s\tenc_MBps\tdec_s\tdec_MBps\tmatch_s\tmatch_MBps\tent_s\tlitraw\tlitpct\tmatches\tc_ll\tc_lit\tc_sd\tc_ml\tsha_exact\tgate_pass\tspeed_pass\tVERDICT\tlrz_recov_pct" > "$OUT"
z19_of() { case $1 in fmt) echo 735266;; abseil) echo 4020529;; rocksdb) echo 6199621;; esac; }
lrz_of() { case $1 in fmt) echo 0;; abseil) echo 3299454;; rocksdb) echo 5477082;; esac; }

BE=99999; BEST=""
for i in 1 2 3; do
  e=$(taskset -c $CORES ./grz enc $NM.ii /tmp/fin$$.grz $K $S 0 $LB $TK 0 15 72 $BLK $THR 2>/dev/null)
  t=$(echo "$e"|cut -f8); awk -v a=$t -v b=$BE 'BEGIN{exit !(a<b)}' && { BE=$t; BEST=$e; }
done
BD=99999
for i in 1 2 3; do
  d=$(taskset -c $CORES ./grz dec /tmp/fin$$.grz /dev/null 2>/dev/null)
  t=$(echo "$d"|cut -f4); awk -v a=$t -v b=$BD 'BEGIN{exit !(a<b)}' && BD=$t
done
taskset -c $CORES ./grz dec /tmp/fin$$.grz /tmp/fin$$.out >/dev/null 2>&1
A=$(sha256sum < $NM.ii | cut -d' ' -f1); B=$(sha256sum < /tmp/fin$$.out | cut -d' ' -f1)
[ "$A" = "$B" ] && EX=YES || EX=NO
rm -f /tmp/fin$$.out /tmp/fin$$.grz

awk -v nm=$NM -v tag="$TAG" -v k=$K -v s=$S -v lb=$LB -v tk=$TK -v blk=$BLK -v thr=$THR \
    -v z=$(z19_of $NM) -v l=$(lrz_of $NM) -v enc=$BE -v dec=$BD -v ex=$EX \
    -v raw=$(echo "$BEST"|cut -f1) -v out=$(echo "$BEST"|cut -f2) -v nmt=$(echo "$BEST"|cut -f3) \
    -v lit=$(echo "$BEST"|cut -f4) -v tmat=$(echo "$BEST"|cut -f6) -v tent=$(echo "$BEST"|cut -f7) \
    -v cll=$(echo "$BEST"|cut -f9) -v clit=$(echo "$BEST"|cut -f10) -v csd=$(echo "$BEST"|cut -f11) \
    -v cml=$(echo "$BEST"|cut -f12) 'BEGIN{
  mb=raw/1048576; r=out/z; ev=mb/enc; dv=mb/dec;
  g=(r<=1.10)?"PASS":"FAIL"; sp=(ev>=1024)?"PASS":"FAIL";
  v=(g=="PASS"&&sp=="PASS"&&ex=="YES")?"PASS":"FAIL";
  rec=(l>0)?100*(z-out)/(z-l):0;
  printf "%s\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%.0f\t%d\t%.4f\t%.4f\t%.3f\t%.1f\t%.3f\t%.1f\t%.3f\t%.0f\t%.3f\t%d\t%.3f\t%d\t%d\t%d\t%d\t%d\t%s\t%s\t%s\t%s\t%.1f\n",
    nm,tag,k,s,lb,tk,blk,thr,raw,out,r,out/(1.10*z),enc,ev,dec,dv,tmat,mb/tmat,tent,lit,100*lit/raw,nmt,cll,clit,csd,cml,ex,g,sp,v,rec;
}' | tee -a "$OUT"
