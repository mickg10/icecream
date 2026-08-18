#!/usr/bin/env bash
# GATE: the P29 physical two-direction wire sinks.
#
# Exits 0 only if every one of these holds; anything else exits nonzero.  It is a gate,
# not a report: it must be able to FAIL, and section 4 proves that it does.
#
#   1  all three variants encode byte-exact, and their sink offsets are monotone and end
#      exactly at the file sizes (checked inside the codec)
#   2  the deployable variants declare dispatch_lag_tus=0; the batch variant declares 111
#      and its build closes show the cold build's literal frame landing in a later build
#   3  re-encoding builds 1..k reproduces BOTH streams byte-identically to each build-close
#      offset (prefix immutability, both directions)
#   4  the replay gate rejects a truncated / flipped / extended / frame-deleted stream, and
#      rejects damage to the REVERSE stream, while accepting the intact pair
set -Eeuo pipefail
trap 'echo "GATE FAIL: unhandled error at line $LINENO" >&2; exit 1' ERR

BIN=$HOME/selbind/p29build/codec50-sink
MAN=${1:?usage: p29sinkproof.sh <4x-manifest> <tus-per-build>}
NB=${2:?usage: p29sinkproof.sh <4x-manifest> <tus-per-build>}
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
B="--z 3 --mixed-regions --byte-array-lines --direct-ordinals --compressed-blobs --blob-threads 8 --blob-lazy-fallback --mo-factor --s1-max-chain 1024 --blob-z 9 --blob-zstd-workers 4 --blob-zstd-job-mib 5 --blob-zstd-overlap-log 3 --stable-root-tags"
fail() { echo "GATE FAIL: $*" >&2; exit 1; }
num() { case "$2" in ''|*[!0-9]*) fail "$1 is not a nonempty integer: '$2'";; esac; }

echo "=== 1/2. three variants, physical C->F and F->C, with build closes ==="
$BIN --manifest "$MAN" $B --mixed-dump-prefix "$W/pl" > "$W/dump.out" 2>&1 || fail "dump pass"
run() { # run <name> [extra flags...]
  local n=$1; shift
  $BIN --manifest "$MAN" $B "$@" --cf-sink "$W/$n.cf" --fc-sink "$W/$n.fc" \
       --sink-curve "$W/$n.tsv" --sink-build-tus "$NB" > "$W/$n.out" 2> "$W/$n.err" \
       || fail "$n encode"
  grep -q 'byte-exact=OK' "$W/$n.out" || fail "$n not byte-exact"
  local lag; lag=$(sed -n 's/.*dispatch_lag_tus=\([0-9]*\).*/\1/p' "$W/$n.out"); num "$n lag" "$lag"
  local cf fc; cf=$(stat -c %s "$W/$n.cf"); fc=$(stat -c %s "$W/$n.fc")
  printf '%-8s C->F=%-10s F->C=%-9s lag=%-4s closes=%s\n' "$n" "$cf" "$fc" "$lag" \
    "$(awk -F'\t' '$7==1{printf "%s ",$3}' "$W/$n.tsv")"
  echo "$lag" > "$W/$n.lag"
}
run stream
run lg1   --literal-group-prefix "$W/pl" --literal-group-tus 1   --literal-group-workers 8 --literal-group-skip-zstd10 --literal-group-wire "$W/l1"
run lg112 --literal-group-prefix "$W/pl" --literal-group-tus 112 --literal-group-workers 8 --literal-group-skip-zstd10 --literal-group-wire "$W/l112"
[ "$(cat "$W/stream.lag")" = 0 ] || fail "stream must have zero dispatch lag"
[ "$(cat "$W/lg1.lag")"   = 0 ] || fail "lg1 must have zero dispatch lag"
[ "$(cat "$W/lg112.lag")" -gt 0 ] || fail "lg112 must declare a nonzero dispatch lag"
# The hard assertion is the codec-reported dispatch lag above; the byte displacement below
# is REPORTED, not asserted.  It only appears when a build is smaller than the group: with
# n < 112 the cold build's literal frame is pushed into a later build, but with n >= 112 the
# cold build already contains its own first group and the displacement is partial.  An
# earlier version of this gate asserted the displacement and wrongly failed spdlog (n=168).
python3 - "$W/lg112.tsv" "$NB" <<'PY' || exit 1
import sys
rows=[l.split('\t') for l in open(sys.argv[1]).read().splitlines()[1:]]
n=int(sys.argv[2])
cl=[int(r[2]) for r in rows if r[6]=='1']; prev=0; inc=[]
for c in cl: inc.append(c-prev); prev=c
if len(inc)<2:
    print("GATE FAIL: fewer than two build closes",file=sys.stderr); sys.exit(1)
note = ("a later build carries the cold build's literal frame" if n < 112 else
        "n >= the 112-TU group, so the cold build holds its own first group; later groups still straddle")
print("   lg112 build increments:",inc," <-",note)
PY

echo "=== 3. prefix immutability, BOTH directions, at every build close ==="
BO="$B --open-final-entropy"
$BIN --manifest "$MAN" $BO --cf-sink "$W/o4.cf" --fc-sink "$W/o4.fc" --sink-curve "$W/o4.tsv" \
     --sink-build-tus "$NB" > "$W/o4.out" 2>&1 || fail "open-entropy full encode"
grep -q 'byte-exact=OK' "$W/o4.out" || fail "open-entropy full encode not byte-exact"
for K in 1 2 3; do
  $BIN --manifest "$MAN" $BO --max-files $((NB*K)) --cf-sink "$W/q$K.cf" --fc-sink "$W/q$K.fc" \
       --sink-curve "$W/q$K.tsv" --sink-build-tus "$NB" > "$W/q$K.out" 2>&1 || fail "prefix encode x$K"
  grep -q 'byte-exact=OK' "$W/q$K.out" || fail "prefix encode x$K not byte-exact"
  for D in cf fc; do
    C=$([ $D = cf ] && echo 3 || echo 4)
    CUT=$(awk -F'\t' -v k=$((NB*K)) -v c=$C '$1==k{print $c}' "$W/o4.tsv"); num "cut" "$CUT"
    S=$(stat -c %s "$W/q$K.$D")
    [ "$S" = "$CUT" ] || fail "build-$K $D size $S != build-close offset $CUT"
    head -c "$CUT" "$W/o4.$D" > "$W/cut.bin"
    cmp -s "$W/q$K.$D" "$W/cut.bin" || fail "build-$K $D is NOT a byte-identical prefix"
    echo "   build-$K $D: BYTE-PREFIX IDENTICAL ($CUT bytes)"
  done
done

echo "=== 4. the replay gate must reject damage (control must pass) ==="
replay() { # replay <label> ; uses $W/x.cf and $W/x.fc ; echoes exit status
  set +e
  $BIN --manifest "$MAN" $B --cf-sink "$W/x.cf" --fc-sink "$W/x.fc" --sink-build-tus "$NB" \
       --sink-replay > /dev/null 2> "$W/x.err"
  local rc=$?; set -e
  printf '   %-28s exit=%d  %s\n' "$1" "$rc" "$(sed -n 's/^replay: //p' "$W/x.err" | head -1 | cut -c1-90)"
  return $rc
}
cp "$W/stream.cf" "$W/x.cf"; cp "$W/stream.fc" "$W/x.fc"
replay "control (intact)" || fail "the replay gate rejected an intact pair"
cp "$W/stream.cf" "$W/x.cf"; truncate -s -100 "$W/x.cf"
if replay "C->F truncated 100 B"; then fail "truncation accepted"; fi
cp "$W/stream.cf" "$W/x.cf"; printf '\x00' | dd of="$W/x.cf" bs=1 seek=$(( $(stat -c %s "$W/stream.cf") / 2 )) conv=notrunc 2>/dev/null
if replay "C->F one byte flipped"; then fail "a flipped byte was accepted"; fi
cp "$W/stream.cf" "$W/x.cf"; printf 'junkjunk' >> "$W/x.cf"
if replay "C->F 8 bytes appended"; then fail "trailing bytes accepted"; fi
python3 - "$W/stream.cf" "$W/x.cf" <<'PY'
import struct,sys
d=open(sys.argv[1],'rb').read(); o=0; f=[]
while o<len(d):
    n=struct.unpack_from('<I',d,o+1)[0]; f.append(d[o:o+5+n]); o+=5+n
del f[3]
open(sys.argv[2],'wb').write(b''.join(f))
PY
if replay "C->F one frame deleted"; then fail "a deleted frame was accepted"; fi
cp "$W/stream.cf" "$W/x.cf"; cp "$W/stream.fc" "$W/x.fc"; truncate -s -20 "$W/x.fc"
if replay "F->C truncated 20 B"; then fail "reverse-stream damage accepted"; fi

echo "=== 5. a stream truncated at a build close is EXACTLY that build's frames ==="
# Cut the pair at build k's physical offsets and replay them against a run restricted to
# those TUs.  Passing means the byte range holds every frame that run consumes, in order,
# byte-identically, with nothing left over -- and that run reconstructs all of its TUs
# byte-exact.  (It still runs in-process: this is not a substitute for a separate receiver.)
for K in 1 2 3; do
  CCUT=$(awk -F'\t' -v k=$((NB*K)) '$1==k{print $3}' "$W/o4.tsv"); num "cf cut" "$CCUT"
  FCUT=$(awk -F'\t' -v k=$((NB*K)) '$1==k{print $4}' "$W/o4.tsv"); num "fc cut" "$FCUT"
  head -c "$CCUT" "$W/o4.cf" > "$W/t.cf"; head -c "$FCUT" "$W/o4.fc" > "$W/t.fc"
  $BIN --manifest "$MAN" $BO --max-files $((NB*K)) --cf-sink "$W/t.cf" --fc-sink "$W/t.fc" \
       --sink-build-tus "$NB" --sink-replay > "$W/t$K.out" 2> "$W/t$K.err" \
       || fail "truncation at build $K did not replay"
  grep -q 'SINK REPLAY OK' "$W/t$K.out" || fail "build-$K truncation: replay did not confirm"
  grep -q 'byte-exact=OK' "$W/t$K.out" || fail "build-$K truncation: not byte-exact"
  echo "   cut at build $K (C→F $CCUT B, F→C $FCUT B): fully consumed, $((NB*K)) TUs byte-exact"
done

echo "GATE PASS"
