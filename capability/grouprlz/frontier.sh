#!/bin/bash
# Clean serial size/speed frontier for GROUP-RLZ. Encode timed end-to-end (single process,
# single thread); decode timed to /dev/null so the number is reconstruct-in-RAM, not disk.
cd ~/grouprlz
OUT=${OUT:-$HOME/grouprlz/frontier.tsv}
CORES=${CORES:-0-7}
LB=${LB:-3}; TK=${TK:-1}; BLK=${BLK:-25}; THR=${THR:-1}
[ -f "$OUT" ] || echo -e "corpus\tK\tsbits\tlitbe\tblkMB\tthr\traw\tout\tratio\tlitraw\tlitpct\tmatch_s\tmatch_MBps\tent_s\tenc_s\tenc_MBps\tdec_s\tdec_MBps\tgate\tspeed" > "$OUT"
z19_of() { case $1 in fmt) echo 735266;; abseil) echo 4020529;; rocksdb) echo 6199621;; esac; }

for nm in $CORPORA; do
  z=$(z19_of $nm)
  for K in $KS; do for S in $SBITS; do
    grep -qP "^$nm\t$K\t$S\t$LB\t$BLK\t$THR\t" "$OUT" && continue
    best=""; bt=99999
    for i in 1 2; do
      e=$(taskset -c $CORES ./grz enc $nm.ii /tmp/fr$$.grz $K $S 0 $LB $TK 0 15 72 $BLK $THR 2>/dev/null)
      t=$(echo "$e"|cut -f8); awk -v a=$t -v b=$bt 'BEGIN{exit !(a<b)}' && { bt=$t; best="$e"; best=$e; }
    done
    bd=99999
    for i in 1 2; do
      d=$(taskset -c $CORES ./grz dec /tmp/fr$$.grz /dev/null 2>/dev/null)
      t=$(echo "$d"|cut -f4); awk -v a=$t -v b=$bd 'BEGIN{exit !(a<b)}' && bd=$t
    done
    rm -f /tmp/fr$$.grz
    awk -v nm=$nm -v k=$K -v s=$S -v lb=$LB -v blk=$BLK -v thr=$THR -v z=$z -v enc=$bt -v dec=$bd \
        -v raw=$(echo "$best"|cut -f1) -v out=$(echo "$best"|cut -f2) -v lit=$(echo "$best"|cut -f4) \
        -v tmat=$(echo "$best"|cut -f6) -v tent=$(echo "$best"|cut -f7) 'BEGIN{
      mb=raw/1048576; ev=mb/enc; dv=mb/dec;
      printf "%s\t%d\t%d\t%d\t%d\t%d\t%.0f\t%d\t%.4f\t%d\t%.3f\t%.3f\t%.0f\t%.3f\t%.3f\t%.1f\t%.3f\t%.1f\t%s\t%s\n",
        nm,k,s,lb,blk,thr,raw,out,out/z,lit,100*lit/raw,tmat,mb/tmat,tent,enc,ev,dec,dv,
        (out<=1.10*z)?"PASS":"FAIL",(ev>=1024)?"PASS":"FAIL";
    }' | tee -a "$OUT"
  done; done
done
echo FRONTIER_DONE
