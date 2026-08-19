#!/usr/bin/env bash
# Physical-prefix companion to the strict unavailable-suffix state gate.
#
# Run the live selector over a complete supplied manifest and over independently truncated
# manifests.  With final entropy tails deliberately left open, every directional frame byte,
# sink-curve row and selector row through K must match.  This proves physical prefix
# invariance for the current codec.  It does NOT claim its loader is one-phase: every one of
# these invocations still preloads the manifest it was given.  p29_unavailable_suffix.sh owns
# the stronger staged-input state/API check.
set -Eeuo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BIN=${BIN:-$HERE/build/codec50-sink}
WORK=${WORK:-$(mktemp -d /tmp/selector-prefix.XXXXXX)}
MANIFEST=${1:-}

fail() {
  echo "PREFIX-INVARIANCE FAIL: $*" >&2
  echo "  evidence retained: $WORK" >&2
  exit 1
}
on_error() {
  local status=$?
  echo "PREFIX-INVARIANCE FAIL: aborted with status $status" >&2
  echo "  evidence retained: $WORK" >&2
  exit "$status"
}
trap on_error ERR

[ -x "$BIN" ] || fail "codec binary is not executable: $BIN"
if [ -z "$MANIFEST" ] || [ ! -f "$MANIFEST" ]; then fail "usage: $0 MANIFEST"; fi
N=$(awk 'NF{n++} END{print n+0}' "$MANIFEST")
[ "$N" -ge 2 ] || fail "manifest needs at least two TUs"
mkdir -p "$WORK"

BASE=(--z 3 --mixed-regions --byte-array-lines --direct-ordinals --compressed-blobs
      --blob-threads 8 --blob-lazy-fallback --mo-factor --s1-max-chain 1024
      --blob-z 9 --blob-zstd-workers 4 --blob-zstd-job-mib 5 --blob-zstd-overlap-log 3
      --stable-root-tags --open-final-entropy --literal-ondemand
      --literal-group-skip-zstd10 --route-s1 1 --transactional-tu --live-selector)

run_codec() { # run_codec <tag> <manifest> <expected rows>
  local tag=$1 manifest=$2 expected=$3 dir=$WORK/$1 rc=0 rows curve_rows
  mkdir -p "$dir"
  "$BIN" --manifest "$manifest" "${BASE[@]}" \
    --selector-tsv "$dir/selector.tsv" --sink-curve "$dir/curve.tsv" \
    --cf-sink "$dir/cf.bin" --fc-sink "$dir/fc.bin" \
    >"$dir/stdout.log" 2>"$dir/stderr.log" || rc=$?
  [ "$rc" -eq 0 ] || fail "$tag: codec exited $rc"
  grep -qF 'byte-exact=OK' "$dir/stdout.log" || fail "$tag: reconstruction was not exact"
  grep -qF 'SELECTOR boundary:' "$dir/stdout.log" || fail "$tag: loader boundary is not declared"
  rows=$(( $(wc -l <"$dir/selector.tsv") - 1 ))
  curve_rows=$(( $(wc -l <"$dir/curve.tsv") - 1 ))
  [ "$rows" -eq "$expected" ] || fail "$tag: selector has $rows rows, expected $expected"
  [ "$curve_rows" -eq "$expected" ] || fail "$tag: curve has $curve_rows rows, expected $expected"
  if [ ! -s "$dir/cf.bin" ] || [ ! -s "$dir/fc.bin" ]; then
    fail "$tag: a directional stream is empty"
  fi
}

run_codec full "$MANIFEST" "$N"

checkpoints=()
for candidate in 1 2 5 10 20; do
  if [ "$candidate" -lt "$N" ]; then checkpoints+=("$candidate"); fi
done
[ "${#checkpoints[@]}" -gt 0 ] || fail "no proper prefix checkpoint exists"

for k in "${checkpoints[@]}"; do
  prefix=$WORK/prefix-$k.manifest
  awk -v wanted="$k" 'NF {print; seen++; if (seen == wanted) exit}' \
    "$MANIFEST" >"$prefix"
  run_codec "prefix-$k" "$prefix" "$k"
  read -r cf_offset fc_offset < <(
    awk -F'\t' -v wanted="$k" 'NR>1 && $1==wanted {print $3, $4}' "$WORK/full/curve.tsv"
  )
  if [ -z "${cf_offset:-}" ] || [ -z "${fc_offset:-}" ]; then
    fail "K=$k: full curve lacks the checkpoint"
  fi
  [ "$(stat -c %s "$WORK/prefix-$k/cf.bin")" = "$cf_offset" ] ||
    fail "K=$k: C-to-F prefix size differs from the full-run offset"
  [ "$(stat -c %s "$WORK/prefix-$k/fc.bin")" = "$fc_offset" ] ||
    fail "K=$k: F-to-C prefix size differs from the full-run offset"
  cmp -n "$cf_offset" "$WORK/prefix-$k/cf.bin" "$WORK/full/cf.bin" ||
    fail "K=$k: C-to-F frame prefix changed with the supplied suffix"
  cmp -n "$fc_offset" "$WORK/prefix-$k/fc.bin" "$WORK/full/fc.bin" ||
    fail "K=$k: F-to-C frame prefix changed with the supplied suffix"
  head -n "$((k + 1))" "$WORK/full/curve.tsv" >"$WORK/expected-$k.curve.tsv"
  head -n "$((k + 1))" "$WORK/full/selector.tsv" >"$WORK/expected-$k.selector.tsv"
  cmp "$WORK/expected-$k.curve.tsv" "$WORK/prefix-$k/curve.tsv" >/dev/null ||
    fail "K=$k: sink-curve rows changed with the supplied suffix"
  cmp "$WORK/expected-$k.selector.tsv" "$WORK/prefix-$k/selector.tsv" >/dev/null ||
    fail "K=$k: selector rows changed with the supplied suffix"
  printf '  K=%-3s C-to-F=%-10s F-to-C=%-10s identical\n' "$k" "$cf_offset" "$fc_offset"
done

echo "physical prefix invariance: PASS (${#checkpoints[@]} checkpoints over $N supplied TUs)"
echo "boundary: each codec process still preloaded only the manifest supplied to that run"
echo "evidence: $WORK"
