#!/usr/bin/env bash
# One (corpus x env) cell with PASS-BOUNDARY CLOSURE and enforced gates.
#
# Why this differs from the first harness: a single 4x stream with --gtu 112 lets a GRZ2
# group span the cold build into the first warm rebuild, so pass 1 is not independently
# closed or decodable.  The owner's cold+warm totals require each build to be complete at
# its OWN last TU.
#
# Method: encode the 1x, 2x, 3x and 4x PREFIXES separately.  Each is a complete stream that
# ends exactly on a build boundary and is independently decoded and digest-verified; the
# warm state carried between builds is exactly the earlier passes present in the prefix.
# Per-build wire = successive differences.  zstd-3 and the fast interner need no such
# treatment: both are per-TU closed by construction (independent frame / per-TU byte-exact
# reconstruction), which this script still verifies.
#
# EVERY gate is enforced: the script exits nonzero and writes FAIL into the status file, so
# the join can refuse the cell rather than publish an unchecked number.
set -Eeuo pipefail
# any unhandled failure marks the cell FAILED rather than leaving a half-written status
trap 'echo "FAIL ${TAG:-cell}: unhandled error at line $LINENO" >> "${S:-/dev/stderr}"; exit 1' ERR

# every scalar a gate depends on must be present AND numeric -- an empty TOTAL compared
# against an empty endpoint used to pass as equal, which is no check at all
num() {  # num <name> <value>
  case "$2" in
    ''|*[!0-9]*) die "$1 is not a nonempty integer: '$2'" ;;
  esac
}
P29=$HOME/selbind/p29build/codec50-refZ
FUSED=$HOME/selbind/fused/fused-curveB
Z3=$HOME/selbind/zstd3tu
GRZ=$HOME/issue16-selector-v1/tools/grz2g-selector
MX=$HOME/ictmp/ii-matrix
W=$HOME/selbind/passx; mkdir -p $W
P29C="--z 3 --mixed-regions --byte-array-lines --direct-ordinals --compressed-blobs --blob-threads 8 --blob-lazy-fallback --mo-factor --s1-max-chain 1024 --blob-z 9 --blob-zstd-workers 4 --blob-zstd-job-mib 5 --blob-zstd-overlap-log 3"
GRZP="-m g2 -K 256 -s 6 -t 21 -l 4 -k 5 -b 8 --gtu 112 --graw 512 --gadd 128 --hist 1024 -j 8"

PROJ=$1; PROF=${2:-native}
TAG=$PROJ.$PROF
S=$W/$TAG.status
die() { echo "FAIL $TAG: $*" | tee -a $S >&2; exit 1; }
: > $S

# ---- resolve + verify the payload, or take a native manifest -------------------------
D=$W/$TAG; rm -rf $D; mkdir -p $D
if [ "$PROF" = "native" ]; then
  SRC=$HOME/ictmp/$PROJ/manifest.txt
  [ -f "$SRC" ] || die "no native manifest"
  cp "$SRC" $D/man.txt
  echo "payload=native-manifest sha=n/a" >> $S
else
  J=$MX/$PROJ/$PROF/corpus.json
  [ -f "$J" ] || die "no corpus.json"
  read -r REL SHA TUS < <(python3 -c "
import json;d=json.load(open('$J'))
print(d['payload']['path'], d['payload']['sha256'], d['tu_count'])") || die "corpus.json unreadable"
  A=$MX/$PROJ/$PROF/$REL
  [ -f "$A" ] || die "payload $REL missing"
  [ "$(sha256sum "$A" | cut -d' ' -f1)" = "$SHA" ] || die "payload sha mismatch"
  mkdir -p $D/ii
  zstd -d --long=31 -c "$A" 2>/dev/null | tar -xf - -C $D/ii || die "extract failed"
  find $D/ii -type f -name '*.ii' | sort > $D/man.txt
  N0=$(wc -l < $D/man.txt)
  [ "$N0" = "$TUS" ] || die "manifest $N0 != corpus.json tu_count $TUS"
  echo "payload=$REL sha=OK tu_count=$TUS" >> $S
fi
N=$(wc -l < $D/man.txt); num "manifest TU count" "$N"
[ "$N" -gt 0 ] || die "empty manifest"

for k in 1 2 3 4; do : > $D/man$k.txt; for i in $(seq 1 $k); do cat $D/man.txt >> $D/man$k.txt; done; done

# ---- per-TU codecs on the full 4x (closed per TU by construction, still verified) -----
$Z3 --manifest $D/man4.txt --curve $W/$TAG.zstd3.tsv -j 16 > $D/z3.out 2>&1 || die "zstd3 failed"
grep -q 'sum_ok=1' $D/z3.out || die "zstd3 sum_ok!=1"
echo "zstd3=OK" >> $S

taskset -c 0-31 $FUSED --manifest $D/man4.txt --level 3 --curve $W/$TAG.fast.tsv \
  > $D/fast.out 2> $D/fast.err || die "fused failed"
[ "$(grep -c 'verify=PASS' $D/fast.err)" -ge 1 ] || die "fused verify not PASS"
echo "fused=OK verify=PASS" >> $S

# ---- GRZ2 and P29 per PREFIX, each closed and independently decoded ------------------
GW=(); PW=()
# one 1x TU map from the manifest; the k-rep maps are pure arithmetic on its offsets
$GRZ tu $D/man.txt $D/tu1 >/dev/null 2>&1 || die "grz2 tu map build failed"
for k in 1 2 3 4; do
  python3 $HOME/selbind/tu4.py $D/tu1 $D/tu$k.map $k >/dev/null || die "tu map x$k failed"
done
for k in 1 2 3 4; do
  tr '\n' '\0' < $D/man$k.txt | xargs -0 cat > $D/ii$k || die "concat x$k failed"
  taskset -c 0-31 $GRZ enc $D/ii$k $D/g$k.grz -u $D/tu$k.map $GRZP \
    --curve $W/$TAG.grz2.p$k.tsv > $D/g$k.out 2> $D/g$k.err || die "GRZ2 enc x$k failed"
  gw=$(cut -f2 $D/g$k.out || true); num "GRZ2 x$k wire" "$gw"; GW+=("$gw")
  # independent decode of the complete stream + byte comparison against the input
  $GRZ dec $D/g$k.grz $D/g$k.dec -j 8 > $D/g$k.dec.out 2>&1 || die "GRZ2 x$k did not decode"
  cmp -s $D/g$k.dec $D/ii$k || die "GRZ2 x$k decode differs from input"
  rm -f $D/g$k.dec $D/g$k.grz
  echo "grz2_pass$k=OK wire=${GW[-1]} decode=EXACT" >> $S

  T=$(mktemp -d)
  taskset -c 0-31 $P29 --manifest $D/man$k.txt $P29C --mixed-dump-prefix $T/pl > $T/pl.out 2>&1 \
    || { rm -rf $T; die "P29 prefix pass x$k failed"; }
  taskset -c 0-31 $P29 --manifest $D/man$k.txt $P29C --literal-group-prefix $T/pl \
    --literal-group-tus 112 --stable-root-tags --literal-group-workers 8 \
    --literal-group-skip-zstd10 --literal-group-wire $T/lit \
    --curve-tsv $W/$TAG.p29.p$k.tsv > $D/p$k.out 2> $D/p$k.err || { rm -rf $T; die "P29 x$k failed"; }
  rm -rf $T
  tot=$(grep -o 'TOTAL=[0-9]*' $D/p$k.out | head -1 | cut -d= -f2 || true)
  end=$(tail -1 $W/$TAG.p29.p$k.tsv | cut -f5 || true)
  num "P29 x$k TOTAL" "$tot"
  num "P29 x$k endpoint" "$end"
  [ "$tot" = "$end" ] || die "P29 x$k endpoint $end != TOTAL $tot"
  grep -q 'byte-exact=OK' $D/p$k.out || die "P29 x$k not byte-exact"
  PW+=("$tot")
  echo "p29_pass$k=OK wire=$tot endpoint=MATCH byte_exact=OK" >> $S
  rm -f $D/ii$k
done

echo "PASS $TAG tus_per_pass=$N grz2=[${GW[*]}] p29=[${PW[*]}]" >> $S
echo "PASS $TAG tus_per_pass=$N grz2=[${GW[*]}] p29=[${PW[*]}]" >&2
