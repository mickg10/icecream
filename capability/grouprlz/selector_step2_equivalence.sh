#!/usr/bin/env bash
# selector_step2_equivalence.sh — a reference build (S1) and the build under test (S2) must
# produce the SAME physical streams on the product path, and the same accounting TOTAL on the
# legacy flat-Root path.
#
# FAIL CLOSED.  The previous version had a worse form of the hole local-oracle found in
# selector_1f_costing.sh: byte-exactness was checked with
#     grep -q byte-exact=OK "$T/$V.out" || echo "$P.$PR variant $V NOT byte-exact"
# which only ECHOED.  It did not `continue`, did not exit, and did not feed the verdict, so
# the identity columns were computed and printed exactly the same whether or not the runs
# were byte-exact -- two identically-broken builds would print stable_ident=YES with the
# warning buried in the same stream as the table.  Exit statuses were never captured, and a
# dropped cell vanished from the table with no count to notice it.
#
# Now: every codec run must exit 0 AND print byte-exact=OK, both streams must compare equal,
# the legacy TOTALs must match and be non-empty, and every requested cell must produce a row.
#
# Usage:  S1=<reference codec50-sink> ./selector_step2_equivalence.sh [project/profile ...]
#   S2=<build under test>  MX=<ii-matrix>  WORK=<scratch>
set -Eeuo pipefail

MX=${MX:-$HOME/ictmp/ii-matrix}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
S1=${S1:?set S1 to the reference codec50-sink build}
S2=${S2:-$HERE/build/codec50-sink}
WORK=${WORK:-/tmp/step2eq}
OUT=${OUT:-$WORK/evidence}
. "$HERE/selector_evidence.sh"

CELL=""; T=""
fail() {
  echo "STEP2 FAIL [${CELL:-<setup>}]: $*" >&2
  if [ -n "$T" ] && [ -d "$T" ]; then echo "  working directory retained: $T" >&2; fi
  exit 1
}
trap 'rc=$?; [ $rc -eq 0 ] || fail "aborted with status $rc at line $LINENO"' ERR

for b in "$S1" "$S2"; do [ -x "$b" ] || fail "not executable: $b"; done
[ -d "$MX" ] || fail "ii-matrix not found: $MX"

BASE=(--z 3 --mixed-regions --byte-array-lines --direct-ordinals --compressed-blobs
      --blob-threads 8 --blob-lazy-fallback --mo-factor --s1-max-chain 1024
      --blob-z 9 --blob-zstd-workers 4 --blob-zstd-job-mib 5 --blob-zstd-overlap-log 3)

CELLS=("$@")
[ ${#CELLS[@]} -gt 0 ] || CELLS=(re2/debian-gcc fmt/debian-gcc cereal/debian-gcc leveldb/debian-gcc
                                 nlohmann-json/debian-gcc spdlog/debian-gcc re2/fedora-clang-libcxx
                                 fmt/linuxbrew cereal/conan-gcc leveldb/fedora-clang-libcxx)

# run <binary> <tag> <dir> -- <argv...>: status captured and required; byte-exactness is a
# hard failure, not a note printed next to a verdict that ignores it.
run() {
  local bin=$1 tag=$2 dir=$3; shift 4
  local rc=0
  nice -n 8 "$bin" "$@" >"$dir/$tag.out" 2>"$dir/$tag.err" || rc=$?
  [ "$rc" -eq 0 ] || fail "$tag: codec exited $rc"
  grep -qF 'byte-exact=OK' "$dir/$tag.out" || fail "$tag: byte-exact is not OK"
}

selector_evidence_init "$OUT" "$S1" "$S2"
printf 'cell\tn\tstable_cf\tstable_ident\tlegacy_total\tlegacy_ident\n'
done_cells=0
for cell in "${CELLS[@]}"; do
  CELL=$cell
  P=${cell%%/*}; PR=${cell##*/}
  T=$WORK/$P.$PR; rm -rf "$T"; mkdir -p "$T/ii"

  J=$MX/$P/$PR/corpus.json
  [ -f "$J" ] || fail "no corpus.json at $J"
  meta=$(python3 -c "
import json,sys
d=json.load(open(sys.argv[1]))
print(d['payload']['path'], d['payload']['sha256'], d['tu_count'])" "$J")
  read -r REL SHA N <<<"$meta"
  have=$(sha256sum "$MX/$P/$PR/$REL" | cut -d' ' -f1)
  [ "$have" = "$SHA" ] || fail "payload sha256 mismatch: $have != $SHA"

  zstd -d --long=31 -c "$MX/$P/$PR/$REL" 2>/dev/null | tar -xf - -C "$T/ii"
  find "$T/ii" -name '*.ii' | sort >"$T/man"
  M=$(wc -l <"$T/man"); [ "$M" = "$N" ] || fail "extracted $M .ii files, corpus.json says $N"
  : >"$T/man4"; for i in 1 2 3 4; do cat "$T/man" >>"$T/man4"; done

  # A) product path: stable tags + on-demand literals, compare the physical streams.
  run "$S1" a "$T" -- --manifest "$T/man4" "${BASE[@]}" --stable-root-tags --literal-ondemand \
      --literal-group-skip-zstd10 --cf-sink "$T/a.cf" --fc-sink "$T/a.fc" --sink-build-tus "$N"
  run "$S2" b "$T" -- --manifest "$T/man4" "${BASE[@]}" --stable-root-tags --literal-ondemand \
      --literal-group-skip-zstd10 --cf-sink "$T/b.cf" --fc-sink "$T/b.fc" --sink-build-tus "$N"
  for f in a.cf a.fc b.cf b.fc; do [ -s "$T/$f" ] || fail "$f is missing or empty"; done
  cmp -s "$T/a.cf" "$T/b.cf" || fail "C->F streams differ between the reference and the build under test"
  cmp -s "$T/a.fc" "$T/b.fc" || fail "F->C streams differ between the reference and the build under test"

  # B) legacy flat Root namespace: the accounting TOTAL must be unchanged.
  run "$S1" c "$T" -- --manifest "$T/man4" "${BASE[@]}" --literal-ondemand --literal-group-skip-zstd10
  run "$S2" d "$T" -- --manifest "$T/man4" "${BASE[@]}" --literal-ondemand --literal-group-skip-zstd10
  LA=$(grep -o 'TOTAL=[0-9]*' "$T/c.out" | head -1)
  LB=$(grep -o 'TOTAL=[0-9]*' "$T/d.out" | head -1)
  [ -n "$LA" ] && [ -n "$LB" ] || fail "no accounting TOTAL on the legacy path"
  [ "$LA" = "$LB" ] || fail "legacy accounting TOTAL differs: $LA vs $LB"

  selector_evidence_cell "$OUT" "$P.$PR" \
      "S1=$S1 S2=$S2 --manifest <man x4> ${BASE[*]} [--stable-root-tags] --literal-ondemand --literal-group-skip-zstd10 [--cf-sink --fc-sink --sink-build-tus $N]" \
      "$T/a.out" "$T/a.err" "$T/b.out" "$T/b.err" "$T/c.out" "$T/c.err" "$T/d.out" "$T/d.err" \
      "$T/a.cf" "$T/a.fc" "$T/b.cf" "$T/b.fc"
  printf '%s.%s\t%s\t%s\tYES\t%s\tYES\n' "$P" "$PR" "$N" "$(stat -c %s "$T/b.cf")" "${LB#TOTAL=}"
  done_cells=$((done_cells + 1))
  rm -rf "$T"; T=""
done

CELL=""
[ "$done_cells" = "${#CELLS[@]}" ] || fail "produced $done_cells rows for ${#CELLS[@]} cells"
echo "# $done_cells/${#CELLS[@]} cells: streams byte-identical and legacy TOTAL unchanged, every run exited 0 and byte-exact"
