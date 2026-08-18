#!/usr/bin/env bash
# Native family beyond fixed-16: complete GRZ2 (frozen fixed-112) + corrected complete
# P29+BSC (56c1744, stable Root tags) + the TU112 probe census, per corpus.
set -uo pipefail
C=$1
W=$HOME/selbind/n25/$C; rm -rf $W; mkdir -p $W/p29
MAN=$HOME/ictmp/$C/manifest.txt; II=$HOME/grouprlz/ii
GRZ=$HOME/issue16-selector-v1/tools/grz2g-selector
P29=$HOME/issue16-p29-prefix-state/bin/codec50-stable-root-final-l23
P29C="--z 3 --mixed-regions --byte-array-lines --direct-ordinals --compressed-blobs --blob-threads 8 --blob-lazy-fallback --mo-factor --s1-max-chain 1024 --blob-z 9 --blob-zstd-workers 4 --blob-zstd-job-mib 5 --blob-zstd-overlap-log 3"
TUS=$(wc -l < $MAN); PROBE=$(( TUS<112 ? TUS : 112 ))
head -n $PROBE $MAN > $W/man.probe
tr "\n" "\0" < $W/man.probe | xargs -0 cat > $W/probe.ii
$GRZ tu $W/man.probe $W/probe.tu > /dev/null
PRAW=$(stat -Lc %s $W/probe.ii)
# complete GRZ2, frozen fixed-112
taskset -c 24-31 $GRZ enc $II/$C.ii $W/full.grz -u $II/$C.tu -m g2 -K 256 -s 6 -t 21 -l 4 -k 5 -b 8 --gtu 112 --graw 512 --gadd 128 --hist 1024 -j 8 > $W/grz.full.out 2> $W/grz.full.err
# complete corrected P29+BSC
taskset -c 16-23 $P29 --manifest $MAN $P29C --mixed-dump-prefix $W/p29/fplan > $W/p29/fplan.out 2> $W/p29/fplan.err
taskset -c 16-23 $P29 --manifest $MAN $P29C --literal-group-prefix $W/p29/fplan --literal-group-tus 112 --stable-root-tags --literal-group-workers 8 --literal-group-skip-zstd10 --literal-group-wire $W/p29/full.lit > $W/p29/full.out 2> $W/p29/full.err
rm -f $W/p29/fplan.*.raw
# TU112 probe census, both codecs
BLIND=""; [ $PROBE -lt $TUS ] && BLIND="--open-final-entropy"
taskset -c 16-23 $P29 --manifest $W/man.probe $P29C --mixed-dump-prefix $W/p29/pplan > $W/p29/pplan.out 2> $W/p29/pplan.err
taskset -c 16-23 $P29 --manifest $W/man.probe $P29C --literal-group-prefix $W/p29/pplan --literal-group-tus 112 --stable-root-tags $BLIND --literal-group-workers 8 --literal-group-skip-zstd10 --literal-group-wire $W/p29/probe.lit > $W/p29/probe.out 2> $W/p29/probe.err
taskset -c 24-31 $GRZ enc $W/probe.ii $W/probe.grz -u $W/probe.tu -m g2 -K 256 -s 6 -t 21 -l 4 -k 5 -b 8 --gtu 112 --graw 512 --gadd 128 --hist 1024 -j 8 --curve $W/probe.curve > $W/grz.probe.out 2> $W/grz.probe.err
rm -f $W/p29/pplan.*.raw $W/probe.ii
g(){ grep -o "$1=[0-9]*" "$2" | head -1 | cut -d= -f2; }
{ printf "id\t%s\ntotal_tus\t%s\nprobe_tus\t%s\nprobe_raw\t%s\n" "$C" "$TUS" "$PROBE" "$PRAW"
  printf "grz_complete\t%s\np29_complete\t%s\np29_complete_exact\t%s\n" "$(cut -f2 $W/grz.full.out)" "$(g TOTAL $W/p29/full.out)" "$(grep -o "byte-exact=[A-Z]*" $W/p29/full.out|head -1|cut -d= -f2)"
  printf "p29_probe_bytes\t%s\ngrz_probe_bytes\t%s\n" "$(g TOTAL $W/p29/probe.out)" "$(cut -f2 $W/grz.probe.out)"
  printf "p29_regions\t%s\np29_distinct_lines\t%s\n" "$(g regions $W/p29/probe.out)" "$(g distinct_lines $W/p29/probe.out)"
  printf "p29_region_occ\t%s\n" "$(sed -n "s/.*region_occ=\([0-9]*\).*/\1/p" $W/p29/probe.err|head -1)"
  printf "p29_raw_literal\t%s\n" "$(sed -n "s/.*raw_literal=\([0-9]*\).*/\1/p" $W/p29/probe.out|head -1)"
  for op in literal ref; do printf "p29_op_%s\t%s\n" $op "$(sed -n "s/.*ops=\[.*$op=\([0-9]*\).*/\1/p" $W/p29/probe.out|head -1)"; done
  awk -F"\t" "NR==2{printf \"grz_g1_out_bytes\t%s\ngrz_g1_add_bytes\t%s\ngrz_g1_closed_by\t%s\n\",\$4,\$5,\$NF}" $W/probe.curve
} > $W/feat.tsv
echo "N25_DONE $C tus=$TUS probe=$PROBE grz=$(cut -f2 $W/grz.full.out) p29=$(g TOTAL $W/p29/full.out)"
