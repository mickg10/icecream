#!/bin/bash
# Binding chronology test: encode P, then encode P||S, and require that every byte emitted
# through P's last COMPLETE group is identical. This catches both failure modes at once --
# a parser that reads past its own group end, and a header carrying future-derived totals.
# S is several more TUs of the same project, so the tail of P is a long exact match that
# would happily extend into S if the group bound were not enforced.
#
# P's final group is closed by end-of-input, not by policy, so with a suffix present that
# same group legitimately grows. The invariant is about COMPLETE groups: the comparison
# stops before it.
set -e
cd ~/grouprlz
C=${C:-corpus2}
NP=${NP:-224}
NS=${NS:-200}
GTU=${GTU:-112}
OPTS="-K 256 -s 5 -l 4 -k 1 -b 8 -j 8 --gtu $GTU --graw 512 --gadd 128 --hist 2048"
M=~/ictmp/$C/manifest.txt
W=$(mktemp -d /tmp/sfx.XXXXXX)

head -$NP $M > $W/mP.txt
head -$((NP+NS)) $M > $W/mPS.txt
tr '\n' '\0' < $W/mP.txt  | xargs -0 cat > $W/P.ii
tr '\n' '\0' < $W/mPS.txt | xargs -0 cat > $W/PS.ii
./grz2g tu $W/mP.txt  $W/P.tu  >/dev/null
./grz2g tu $W/mPS.txt $W/PS.tu >/dev/null

for m in g1 g2; do
  ./grz2g enc $W/P.ii  $W/P.grz  -m $m -u $W/P.tu  $OPTS --curve $W/cP.tsv  >/dev/null 2>&1
  ./grz2g enc $W/PS.ii $W/PS.grz -m $m -u $W/PS.tu $OPTS --curve $W/cPS.tsv >/dev/null 2>&1
  A=$(stat -c %s $W/P.grz)
  LASTC=$(tail -1 $W/cP.tsv | cut -f6)
  NG=$(( $(wc -l < $W/cP.tsv) - 1 ))
  KEEP=$(( NG - 1 ))
  TUEND=$(awk -F'\t' -v k=$KEEP 'NR>1 && $1==k-1 {print $3}' $W/cP.tsv)
  CUT=$(( A - 36 - LASTC ))
  head -c $CUT $W/P.grz  > $W/a.bin
  head -c $CUT $W/PS.grz > $W/b.bin
  cmp -s $W/a.bin $W/b.bin && R=IDENTICAL || R=DIFFER
  echo -e "$C\t$m\tP=${NP}TU\tPS=$((NP+NS))TU\tcomplete_groups=$KEEP\tthrough_TU=$TUEND\tbytes=$CUT\temitted_prefix=$R"
  head -$TUEND $M | tr '\n' '\0' | xargs -0 cat > $W/expect.ii
  ./grz2g decprefix $W/PS.grz $W/pre.ii -g $KEEP -j 1 >/dev/null 2>&1
  cmp -s $W/expect.ii $W/pre.ii && E="PREFIX_DECODES_TO_TU$TUEND" || E=PREFIX_MISMATCH
  echo -e "$C\t$m\t\t\t\t\t\t$E"
  rm -f $W/a.bin $W/b.bin $W/pre.ii $W/expect.ii
done
rm -rf $W
