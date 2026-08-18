#!/usr/bin/env bash
# One (corpus x docker-env) cell: extract, build a 4x manifest, run all codecs, per-TU.
#
# Payload is resolved from corpus.json -> payload.path and its sha256 VERIFIED.
# Never glob *.ii.tar.zst: a cell can hold a superseded second archive carrying its own
# internal manifest, which fails silently with the wrong TU set.
set -uo pipefail
P29=$HOME/selbind/p29build/codec50-refZ
FUSED=$HOME/selbind/fused/fused-curveB
Z3=$HOME/selbind/zstd3tu
GRZ=$HOME/issue16-selector-v1/tools/grz2g-selector
MX=$HOME/ictmp/ii-matrix
W=$HOME/selbind/envx; mkdir -p $W
P29C="--z 3 --mixed-regions --byte-array-lines --direct-ordinals --compressed-blobs --blob-threads 8 --blob-lazy-fallback --mo-factor --s1-max-chain 1024 --blob-z 9 --blob-zstd-workers 4 --blob-zstd-job-mib 5 --blob-zstd-overlap-log 3"

PROJ=$1; PROF=$2
CELL=$MX/$PROJ/$PROF
J=$CELL/corpus.json
[ -f "$J" ] || { echo "SKIP $PROJ/$PROF: no corpus.json" >&2; exit 1; }
read -r PATHREL SHA TUS < <(python3 -c "
import json;d=json.load(open('$J'))
print(d['payload']['path'], d['payload']['sha256'], d['tu_count'])")
ARCH=$CELL/$PATHREL
[ -f "$ARCH" ] || { echo "SKIP $PROJ/$PROF: payload $PATHREL missing" >&2; exit 1; }
GOT=$(sha256sum "$ARCH" | cut -d" " -f1)
[ "$GOT" = "$SHA" ] || { echo "SKIP $PROJ/$PROF: sha mismatch" >&2; exit 1; }

TAG=$PROJ.$PROF
D=$W/$TAG; rm -rf $D; mkdir -p $D/ii
# payload is "zstd-3 --long=31" per corpus.json, so the long window must be passed
zstd -d --long=31 -c "$ARCH" 2>/dev/null | tar -xf - -C $D/ii 2>/dev/null \
  || { echo "SKIP $TAG: untar failed" >&2; exit 1; }
find $D/ii -type f -name '*.ii' | sort > $D/man.txt
N=$(wc -l < $D/man.txt)
[ "$N" = "$TUS" ] || echo "WARN $TAG: manifest $N vs corpus.json tu_count $TUS" >&2
for i in 1 2 3 4; do cat $D/man.txt; done > $D/man4.txt

nice -n 5 $Z3 --manifest $D/man4.txt --curve $W/$TAG.zstd3.tsv -j 16 > $W/$TAG.zstd3.out 2>&1
z3ok=$(grep -o 'sum_ok=[01]' $W/$TAG.zstd3.out | cut -d= -f2)

nice -n 5 taskset -c 0-31 $FUSED --manifest $D/man4.txt --level 3 --curve $W/$TAG.fast.tsv \
  > $W/$TAG.fast.out 2> $W/$TAG.fast.err
fok=$(grep -c 'verify=PASS' $W/$TAG.fast.err)

T=$(mktemp -d)
nice -n 5 taskset -c 0-31 $P29 --manifest $D/man4.txt $P29C --mixed-dump-prefix $T/pl > $T/pl.out 2>&1
nice -n 5 taskset -c 0-31 $P29 --manifest $D/man4.txt $P29C --literal-group-prefix $T/pl \
  --literal-group-tus 112 --stable-root-tags --literal-group-workers 8 \
  --literal-group-skip-zstd10 --literal-group-wire $T/lit \
  --curve-tsv $W/$TAG.p29.tsv > $W/$TAG.p29.out 2> $W/$TAG.p29.err
ptot=$(grep -o 'TOTAL=[0-9]*' $W/$TAG.p29.out | head -1 | cut -d= -f2)
pend=$(tail -1 $W/$TAG.p29.tsv 2>/dev/null | cut -f5)
rm -rf $T

tr '\n' '\0' < $D/man4.txt | xargs -0 cat > $D/ii4 2>/dev/null
nice -n 5 $GRZ tu $D/man4.txt $D/tu4 > /dev/null 2>&1
nice -n 5 taskset -c 0-31 $GRZ enc $D/ii4 $D/g.grz -u $D/tu4 \
  -m g2 -K 256 -s 6 -t 21 -l 4 -k 5 -b 8 --gtu 112 --graw 512 --gadd 128 --hist 1024 -j 8 \
  --curve $W/$TAG.grz2.tsv > $W/$TAG.grz2.out 2> $W/$TAG.grz2.err
gw=$(cut -f2 $W/$TAG.grz2.out 2>/dev/null)
rm -f $D/ii4 $D/g.grz

echo "$TAG tus=$N x4=$((N*4)) zstd3_sum_ok=$z3ok fused_verify=$fok p29_total=$ptot p29_end=$pend p29_match=$([ "$ptot" = "$pend" ] && echo YES || echo NO) grz2=$gw" >&2
