#!/usr/bin/env bash
# COLD_1F_STICKY_CODEC: direct isolated C-encode of the 400x-size cold codec.
# In-memory producer -- the clock starts when the corpus bytes are resident, so the
# harness file-load artifact is excluded rather than subtracted.
set -uo pipefail
C=$1; REPS=${REPS:-3}
P29=$HOME/selbind/p29build/codec50-cenc24
GRZ=$HOME/issue16-selector-v1/tools/grz2g-selector
MAN=$HOME/ictmp/$C/manifest.txt; II=$HOME/grouprlz/ii
W=$HOME/selbind/coldc/$C; rm -rf $W; mkdir -p $W
P29C="--z 3 --mixed-regions --byte-array-lines --direct-ordinals --compressed-blobs --blob-threads 8 --blob-lazy-fallback --mo-factor --s1-max-chain 1024 --blob-z 9 --blob-zstd-workers 4 --blob-zstd-job-mib 5 --blob-zstd-overlap-log 3"
Z19=$(awk -F'\t' -v c="$C" '$1==c{print $5;exit}' $HOME/grouprlz/corpora.tsv)
tr '\n' '\0' < $MAN | xargs -0 cat > /dev/null      # warm the producer
taskset -c 0-15 $P29 --manifest $MAN $P29C --mixed-dump-prefix $W/pl > $W/pl.out 2> $W/pl.err
for r in $(seq 1 $REPS); do
  /usr/bin/time -f "%e %U %S %M" -o $W/p29.$r.res taskset -c 0-15 $P29 --manifest $MAN $P29C \
    --literal-group-prefix $W/pl --literal-group-tus 112 --stable-root-tags \
    --literal-group-workers 8 --literal-group-skip-zstd10 --literal-group-wire $W/lit \
    > $W/p29.$r.out 2> $W/p29.$r.err
  cat $II/$C.ii > /dev/null
  /usr/bin/time -f "%e %U %S %M" -o $W/grz.$r.res taskset -c 0-15 $GRZ enc $II/$C.ii $W/g.grz \
    -u $II/$C.tu -m g2 -K 256 -s 6 -t 21 -l 4 -k 5 -b 8 --gtu 112 --graw 512 --gadd 128 --hist 1024 -j 8 \
    > $W/grz.$r.out 2> $W/grz.$r.err
done
rm -f $W/pl.*.raw
PB=$(grep -o 'TOTAL=[0-9]*' $W/p29.1.out | head -1 | cut -d= -f2)
GB=$(cut -f2 $W/grz.1.out)
for r in $(seq 1 $REPS); do
  awk -v c="$C" -v r="$r" -v pb="$PB" -v gb="$GB" -v z="$Z19" -v res="$(cat $W/p29.$r.res)" '
    /^CENCODE/ { for(i=1;i<=NF;i++){split($i,a,"=");v[a[1]]=a[2]}
      split(res,R," ")
      printf "%s\t%s\t%s\t%s\t%.4f\t%.4f\t%.4f\t%.4f\t%s\t%.4f\t%s\t%s\n",
        c,r,v["raw"],pb,v["C_input_ready_s"],v["C_encode_ready_s"],v["C_total_s"],
        v["encode_GBps"],z,pb/z,R[4],gb }' $W/p29.$r.err
done
