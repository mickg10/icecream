#!/usr/bin/env bash
# Prove that the focused whole-TU retry gate observes each independently-owned state plane.
# One source-mutated executable makes the selected abort/ordering operation conditional; each
# run removes exactly one operation and must stop at the full route-state equality check.
set -Eeuo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
MAN=${1:?usage: selector_transaction_source_mutations.sh MANIFEST}
BIN=${BIN:-$HERE/build/codec50-sink}
WORK=${WORK:-$(mktemp -d /tmp/p29-txn-source-mut.XXXXXX)}
TX_TU=${TX_TU:-20}
MO_MANIFEST=${MO_MANIFEST:-}
MO_TX_TU=${MO_TX_TU:-525}
LIBBSC_DIR=${LIBBSC_DIR:-$HOME/libbsc}
LIBBSC_A=${LIBBSC_A:-$HOME/grouprlz/libbsc.a}
LIBZSTD_A=${LIBZSTD_A:-/usr/lib/x86_64-linux-gnu/libzstd.a}

fail(){ echo "TRANSACTION SOURCE MUTATION FAIL: $*" >&2; exit 1; }
trap 'rc=$?; [ "$rc" -eq 0 ] || echo "  evidence retained: $WORK" >&2' EXIT

[ -x "$BIN" ] || fail "codec binary not executable: $BIN"
[ -s "$MAN" ] || fail "manifest is empty or missing: $MAN"
for f in "$LIBBSC_DIR/libbsc/libbsc.h" "$LIBBSC_A" "$LIBZSTD_A"; do
  [ -e "$f" ] || fail "missing build dependency: $f"
done
mkdir -p "$WORK"
head -n "$((TX_TU+1))" "$MAN" >"$WORK/manifest"
[ "$(wc -l <"$WORK/manifest")" -gt "$TX_TU" ] || fail "manifest has no zero-based TU $TX_TU"
head -1 "$MAN" >"$WORK/manifest.source"

src=$WORK/codec50-sink.transaction-mut.cpp
exe=$WORK/codec50-sink.transaction-mut
cp "$HERE/codec50-sink.cpp" "$src"

# This sequence plus its following equality check occurs only in the deliberate receiver-reject branch.  The normal
# reconstruction-failure cleanup and every product-path operation remain untouched.
needle='            fTuJournal.abort(Fpaths,FmixedRegionData,FmixedRegions,FmixedPublic,Freg_stream,Froot_child,Froot_off);
            Fblocks.abort_transaction();Fmo.abort_transaction();mixedFSource.abort_transaction();routeS1->abort();
            if(routeLocalStateDigest()!=selftestPreState)'
[ "$(grep -Fxc '            if(routeLocalStateDigest()!=selftestPreState){fprintf(stderr,"transaction selftest: reject did not restore full route-local state\n");return 2;}' "$src")" -eq 1 ] ||
  fail "receiver-reject mutation anchor is not unique"
NEEDLE=$needle perl -0pi -e '
  my $n=$ENV{"NEEDLE"};
  my $r=q{            if(!getenv("P29_MUT_F_ACTIVE"))fTuJournal.abort(Fpaths,FmixedRegionData,FmixedRegions,FmixedPublic,Freg_stream,Froot_child,Froot_off);
            if(!getenv("P29_MUT_F_BLOCK"))Fblocks.abort_transaction();
            if(!getenv("P29_MUT_F_MO"))Fmo.abort_transaction();
            if(!getenv("P29_MUT_F_SOURCE"))mixedFSource.abort_transaction();
            if(!getenv("P29_MUT_ROUTE"))routeS1->abort();
            if(routeLocalStateDigest()!=selftestPreState)};
  die "receiver-reject sequence not found\n" unless s/\Q$n\E/$r/;
' "$src"

# A C mirror/state delta made visible before Ack must also be detected by the same equality
# boundary.  This mutation applies the already-frozen redo immediately before F decoding.
anchor='          if(cTuJournal.active()){fprintf(stderr,"C active-TU journal remained active after freeze\n");return 2;}'
[ "$(grep -Fc "$anchor" "$src")" -eq 1 ] || fail "early-C mutation anchor is not unique"
sed -i '/C active-TU journal remained active after freeze/a\          if(transactionSelftest&&getenv("P29_MUT_C_EARLY")&&!pendingF.prepared_c_state.apply(pathid,paths,mixedCLine,nextMixedPublic,fknownReg,fknownBlk)){fprintf(stderr,"mutation could not apply early C state\\n");return 2;}' "$src"

cmp -s "$HERE/codec50-sink.cpp" "$src" && fail "source mutation changed nothing"
g++ -O2 -std=c++17 -fopenmp -Wformat=2 -Werror=format -DWITH_BSC_GROUPS \
    -I"$HERE" -I"$LIBBSC_DIR/libbsc" "$src" -o "$exe" \
    "$LIBBSC_A" "$LIBZSTD_A" -lz -lpthread >"$WORK/build.out" 2>"$WORK/build.err" ||
  fail "mutated transaction source did not build"

BASE=(--z 3 --mixed-regions --byte-array-lines --direct-ordinals --compressed-blobs
      --blob-threads 8 --blob-lazy-fallback --mo-factor --s1-max-chain 1024
      --blob-z 9 --blob-zstd-workers 4 --blob-zstd-job-mib 5 --blob-zstd-overlap-log 3
      --stable-root-tags --literal-ondemand --literal-group-skip-zstd10 --route-s1 1
      --transactional-tu)

run_mutation(){ # run_mutation <name> <environment-variable> <target-TU> <manifest>
  local name=$1 variable=$2 target=$3 manifest=$4 rc=0
  env "$variable=1" "$exe" --manifest "$manifest" "${BASE[@]}" \
      --selftest-transaction-reject-once "$target" \
      --cf-sink "$WORK/$name.cf" --fc-sink "$WORK/$name.fc" \
      >"$WORK/$name.out" 2>"$WORK/$name.err" || rc=$?
  [ "$rc" -ne 0 ] || fail "$name mutation was accepted"
  grep -qF 'transaction selftest: reject did not restore full route-local state' "$WORK/$name.err" ||
    fail "$name stopped, but not at the full-state equality boundary"
  printf 'caught %-10s exit=%d\n' "$name" "$rc"
}

run_mutation f_active P29_MUT_F_ACTIVE "$TX_TU" "$WORK/manifest"
run_mutation f_block P29_MUT_F_BLOCK "$TX_TU" "$WORK/manifest"
# TU 0 is intentionally used here: it is where this fixture first populates the F source-text
# cache.  A later target can legitimately reuse an already-committed entry and touch no cache.
run_mutation f_source P29_MUT_F_SOURCE 0 "$WORK/manifest.source"
run_mutation route P29_MUT_ROUTE "$TX_TU" "$WORK/manifest"
run_mutation c_early P29_MUT_C_EARLY "$TX_TU" "$WORK/manifest"

# Optional large branch-coverage run.  The ruled Godot fixture selects MO with real pending
# definitions at TU 525; ordinary acceptance keeps this explicit because it is a 1.6-GB input.
if [ -n "$MO_MANIFEST" ]; then
  [ -s "$MO_MANIFEST" ] || fail "MO_MANIFEST is empty or missing: $MO_MANIFEST"
  head -n "$((MO_TX_TU+1))" "$MO_MANIFEST" >"$WORK/manifest.mo"
  [ "$(wc -l <"$WORK/manifest.mo")" -gt "$MO_TX_TU" ] || fail "MO manifest has no TU $MO_TX_TU"
  run_mutation f_mo P29_MUT_F_MO "$MO_TX_TU" "$WORK/manifest.mo"
fi

echo "all 5 whole-TU source mutations reached the full-state equality boundary"
