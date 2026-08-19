#!/usr/bin/env bash
# selector_step1_equivalence.sh — step 1 (on-demand one-TU literals) must produce the SAME
# physical C->F and F->C streams as the planning path it replaced.
#
# FAIL CLOSED, for the same reason selector_1f_costing.sh had to be: the previous version
# ran under `set +e`, never captured a codec exit status, and printed a table row per cell
# with no requirement that every cell produced one.  A cell that hit a SHA mismatch or a
# non-byte-exact run simply `continue`d and VANISHED from the table -- so "10/10 identical"
# was a count of the rows that happened to appear, not a checked property of ten cells.
# Nothing here re-derives a verdict from the thing it checks: cf_identical/fc_identical come
# from cmp against a separately produced stream.  What was missing was completeness and the
# process status, and both are now required.
#
# Usage:  ./selector_step1_equivalence.sh [project/profile ...]
#   BIN=<codec50-sink>  MX=<ii-matrix>  WORK=<scratch>
set -Eeuo pipefail

MX=${MX:-$HOME/ictmp/ii-matrix}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BIN=${BIN:-$HERE/build/codec50-sink}
WORK=${WORK:-/tmp/step1eq}

CELL=""; T=""
fail() {
  echo "STEP1 FAIL [${CELL:-<setup>}]: $*" >&2
  if [ -n "$T" ] && [ -d "$T" ]; then echo "  working directory retained: $T" >&2; fi
  exit 1
}
trap 'rc=$?; [ $rc -eq 0 ] || fail "aborted with status $rc at line $LINENO"' ERR

[ -x "$BIN" ] || fail "build codec50-sink first: ./selector_build_codec50_sink.sh"
[ -d "$MX" ] || fail "ii-matrix not found: $MX"

BASE=(--z 3 --mixed-regions --byte-array-lines --direct-ordinals --compressed-blobs
      --blob-threads 8 --blob-lazy-fallback --mo-factor --s1-max-chain 1024
      --blob-z 9 --blob-zstd-workers 4 --blob-zstd-job-mib 5 --blob-zstd-overlap-log 3
      --stable-root-tags)

CELLS=("$@")
[ ${#CELLS[@]} -gt 0 ] || CELLS=(re2/debian-gcc fmt/debian-gcc cereal/debian-gcc leveldb/debian-gcc
                                 nlohmann-json/debian-gcc spdlog/debian-gcc re2/fedora-clang-libcxx
                                 fmt/linuxbrew cereal/conan-gcc leveldb/fedora-clang-libcxx)

# run <tag> <logdir> -- <argv...>: the exit status is captured and required, and stdout and
# stderr are kept apart so the byte-exact grep cannot be confused by interleaved stderr.
run() {
  local tag=$1 dir=$2; shift 3
  local rc=0
  nice -n 8 "$BIN" "$@" >"$dir/$tag.out" 2>"$dir/$tag.err" || rc=$?
  [ "$rc" -eq 0 ] || fail "$tag: codec exited $rc"
  grep -qF 'byte-exact=OK' "$dir/$tag.out" || fail "$tag: byte-exact is not OK"
}

printf 'cell\tn\tplan_cf\tod_cf\tcf_identical\tfc_identical\tplan_wall\tod_wall\n'
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

  S1=$(date +%s.%N)
  # The dump run feeds the planning run; it was previously sent to /dev/null unchecked.
  run dump "$T" -- --manifest "$T/man4" "${BASE[@]}" --mixed-dump-prefix "$T/pl"
  run plan "$T" -- --manifest "$T/man4" "${BASE[@]}" --literal-group-prefix "$T/pl" \
      --literal-group-tus 1 --literal-group-workers 8 --literal-group-skip-zstd10 \
      --cf-sink "$T/plan.cf" --fc-sink "$T/plan.fc" --sink-build-tus "$N"
  S2=$(date +%s.%N)
  run od "$T" -- --manifest "$T/man4" "${BASE[@]}" --literal-ondemand --literal-group-skip-zstd10 \
      --cf-sink "$T/od.cf" --fc-sink "$T/od.fc" --sink-build-tus "$N"
  S3=$(date +%s.%N)

  for f in plan.cf plan.fc od.cf od.fc; do
    [ -s "$T/$f" ] || fail "$f is missing or empty"
  done
  cmp -s "$T/plan.cf" "$T/od.cf" || fail "C->F streams differ"
  cmp -s "$T/plan.fc" "$T/od.fc" || fail "F->C streams differ"

  printf '%s.%s\t%s\t%s\t%s\tYES\tYES\t%.2f\t%.2f\n' "$P" "$PR" "$N" \
      "$(stat -c %s "$T/plan.cf")" "$(stat -c %s "$T/od.cf")" \
      "$(awk -v a="$S1" -v b="$S2" 'BEGIN{printf "%.2f", b-a}')" \
      "$(awk -v a="$S2" -v b="$S3" 'BEGIN{printf "%.2f", b-a}')"
  done_cells=$((done_cells + 1))
  rm -rf "$T"; T=""
done

CELL=""
# Completeness: the table must have a row for every cell asked for.  Without this a dropped
# cell is invisible, and the headline is a row count rather than a checked property.
[ "$done_cells" = "${#CELLS[@]}" ] || fail "produced $done_cells rows for ${#CELLS[@]} cells"
echo "# $done_cells/${#CELLS[@]} cells: both streams byte-identical, every codec run exited 0 and byte-exact"
