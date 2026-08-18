#!/usr/bin/env bash
# Is the GRZ2 small-cell sub-1GB/s a removable fixed cost or a real throughput floor?
# Encode-only, input already in page cache (production holds the stream in memory).
set -uo pipefail
GRZ=$HOME/issue16-selector-v1/tools/grz2g-selector
W=$HOME/selbind/grzsmall; mkdir -p $W
FROZEN="-m g2 -K 256 -s 6 -t 21 -l 4 -k 5 -b 8 --gtu 112 --graw 512 --gadd 128 --hist 1024 -j 8"
SMALL="-m g2 -K 256 -s 8 -t 16 -l 4 -k 5 -b 1 --gtu 112 --graw 512 --gadd 128 --hist 32 -j 8"
printf "project\tprofile\traw\tfrozen_s\tfrozen_gbps\tfrozen_bytes\tsmall_s\tsmall_gbps\tsmall_bytes\tsize_cost_pct\texact\n" > $W/small.tsv
for cell in "$@"; do
  p=${cell%/*}; pr=${cell#*/}
  C=$HOME/ictmp/ii-matrix/$p/$pr
  rm -rf $W/ex; mkdir -p $W/ex
  PAY=$(python3 -c "import json;print(json.load(open(\"$C/corpus.json\"))[\"payload\"][\"path\"])")
  zstd -d --long=31 -q -c $C/$PAY | tar -xf - -C $W/ex
  awk -F"\t" -v d="$W/ex/" "NR>1{print d \$2}" $C/manifest.tsv > $W/man.txt
  tr "\n" "\0" < $W/man.txt | xargs -0 cat > $W/cell.ii
  $GRZ tu $W/man.txt $W/cell.tu > /dev/null
  RAW=$(stat -Lc %s $W/cell.ii); cat $W/cell.ii > /dev/null
  best_f=99999; best_s=99999
  for i in 1 2 3; do
    /usr/bin/time -f %e -o $W/f.t taskset -c 24-31 $GRZ enc $W/cell.ii $W/f.grz -u $W/cell.tu $FROZEN > $W/f.out 2>/dev/null
    t=$(cat $W/f.t); awk -v a=$t -v b=$best_f "BEGIN{exit !(a<b)}" && best_f=$t
    /usr/bin/time -f %e -o $W/s.t taskset -c 24-31 $GRZ enc $W/cell.ii $W/s.grz -u $W/cell.tu $SMALL > $W/s.out 2>/dev/null
    t=$(cat $W/s.t); awk -v a=$t -v b=$best_s "BEGIN{exit !(a<b)}" && best_s=$t
  done
  FB=$(cut -f2 $W/f.out); SB=$(cut -f2 $W/s.out)
  taskset -c 24-31 $GRZ dec $W/s.grz $W/r.ii -j 1 >/dev/null 2>&1
  cmp -s $W/cell.ii $W/r.ii && X=YES || X=NO; rm -f $W/r.ii
  awk -v p=$p -v pr=$pr -v raw=$RAW -v f=$best_f -v s=$best_s -v fb=$FB -v sb=$SB -v x=$X "BEGIN{
    printf \"%s\t%s\t%d\t%s\t%.4f\t%d\t%s\t%.4f\t%d\t%.2f\t%s\n\",p,pr,raw,f,raw/1e9/f,fb,s,raw/1e9/s,sb,100*(sb-fb)/fb,x}" >> $W/small.tsv
  rm -rf $W/ex $W/cell.ii
done
column -t $W/small.tsv
