#!/usr/bin/env bash
# Regression guards for the typed-tag representation (T_current step 2a/2b), covering the
# three points local-oracle raised on 89a882c.  Each guard is paired with the mutation it is
# supposed to catch, because a check that cannot fail is not a check.
#
#   G1  --v1 (no S1) + --stable-root-tags must reconstruct byte-exact.  The V1 branch pushes
#       Regions straight into the Root, so if it pushes a bare id instead of region_tag(),
#       every ODD Region id decodes as a Block.  The corpus must contain both an even and an
#       odd Region id or the test proves nothing -- that is asserted, not assumed.
#   G2  a legacy Root value of NREG + <block count> names a Block that does not exist and
#       must be REJECTED.  fknownBlk is sized boff2.size() but the real count is
#       boff2.size()-1, so passing size() would accept the sentinel slot.
#   G3  the Block half of the id space is guarded against the tag shift, like the Region half.
#
# Usage: BIN=path/to/codec50-sink ./selector_tag_regression.sh <4x-manifest> [1x-manifest]
set -Eeuo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BIN=${BIN:-$HERE/build/codec50-sink}
MAN=${1:?usage: selector_tag_regression.sh <4x-manifest> [1x-manifest]}
[ -x "$BIN" ] || { echo "build codec50-sink first: ./selector_build_codec50_sink.sh" >&2; exit 1; }
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
BASE="--z 3 --mixed-regions --byte-array-lines --direct-ordinals --compressed-blobs --blob-threads 4 --blob-lazy-fallback --mo-factor --s1-max-chain 1024 --blob-z 9 --blob-zstd-workers 2 --blob-zstd-job-mib 5 --blob-zstd-overlap-log 3"
fail() { echo "REGRESSION FAIL: $*" >&2; exit 1; }

echo "=== G1: --v1 + --stable-root-tags round-trips ==="
"$BIN" --manifest "$MAN" $BASE --v1 --stable-root-tags --literal-ondemand \
       --literal-group-skip-zstd10 > "$W/v1.out" 2>&1 || fail "--v1 --stable-root-tags run failed"
grep -q 'byte-exact=OK' "$W/v1.out" || fail "--v1 --stable-root-tags is not byte-exact"
REG=$(sed -n 's/.*regions=\([0-9]*\).*/\1/p' "$W/v1.out" | head -1)
case "$REG" in ''|*[!0-9]*) fail "could not read the Region count" ;; esac
[ "$REG" -ge 2 ] || fail "corpus has $REG Region(s): with fewer than 2 there is no odd id and G1 proves nothing"
echo "   byte-exact with regions=$REG (ids 0..$((REG-1)) include both parities)"

echo "=== G2: tag/bound semantics, including the sentinel slot ==="
"$BIN" --selftest-tags > "$W/st.out" 2> "$W/st.err" || { cat "$W/st.err" >&2; fail "--selftest-tags reported a failure"; }
grep -q 'selftest-tags: PASS' "$W/st.out" || fail "--selftest-tags did not pass"
echo "   both parities round-trip; out-of-range Region and Block refused; legacy NREG+count"
echo "   (the sentinel slot) refused while NREG+count-1 is accepted"

echo "=== G2b: the CALL SITE refuses a Root naming the sentinel Block ==="
# G2 tests the helper.  This injects the sentinel into the Root that F actually decodes, so
# the bound passed at the call site is what is under test -- a well-formed encoder never
# emits this value, which is why a loose bound would otherwise go unnoticed.
for MODE in "--stable-root-tags" ""; do
  L=$([ -n "$MODE" ] && echo stable || echo legacy)
  set +e
  "$BIN" --manifest "$MAN" $BASE $MODE --literal-ondemand --literal-group-skip-zstd10 \
         --selftest-bad-root > "$W/bad.$L.out" 2> "$W/bad.$L.err"
  rc=$?
  set -e
  [ "$rc" -ne 0 ] || fail "$L: a Root naming the sentinel Block was ACCEPTED"
  grep -q 'Root token' "$W/bad.$L.err" || fail "$L: rejected, but not by the Root-token bound: $(tail -1 "$W/bad.$L.err")"
  echo "   $L: rejected (exit $rc) by the Root-token bound"
done

echo "=== G3: the Block-half tag guard is present in the source ==="
grep -q 'too many Blocks for a typed Root tag' "$HERE/codec50-sink.cpp" \
  || fail "no Block-half 2^31 guard in codec50-sink.cpp"
grep -q 'too many Regions for a typed Root tag' "$HERE/codec50-sink.cpp" \
  || fail "no Region-half 2^31 guard in codec50-sink.cpp"
echo "   both halves of the id space are guarded against the tag shift"

echo "TAG REGRESSION PASS"
