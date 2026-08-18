#!/usr/bin/env bash
# A real GATE over the GRZ2 proof table -- selector_grzproof.sh only COLLECTS evidence and
# exits success even after a DIFFER/FAIL/skipped cell, which is not a gate.
#
# Fails nonzero unless, for the DECLARED cell list:
#   * every declared cell is present exactly once (no missing, no duplicate, no stale rows)
#   * every cell has four strictly-increasing build-close offsets
#   * container size > last offset, and the terminator is charged to the final build
#   * prefix_1in4 and prefix_2in4 are IDENTICAL
#   * decode_k1/k2/k4 are EXACT
#   * build_close_rows is exactly 4/4
set -Eeuo pipefail
T=${1:?proof.tsv}; shift
declare -a WANT=("$@")
[ "${#WANT[@]}" -gt 0 ] || { echo "GATE FAIL: no declared cells"; exit 2; }
[ -s "$T" ] || { echo "GATE FAIL: $T missing/empty"; exit 2; }

fails=0
note() { echo "  GATE FAIL: $*"; fails=$((fails+1)); }

hdr=$(head -1 "$T"); rows=$(( $(wc -l < "$T") - 1 ))
declare -A seen
while IFS=$'\t' read -r cell n o1 o2 o3 o4 cont term b1 b2 b3 b4 p1 p2 d1 d2 d4 closes rest; do
  [ "$cell" = "cell" ] && continue
  seen["$cell"]=$(( ${seen["$cell"]:-0} + 1 ))
done < "$T"

for c in "${WANT[@]}"; do
  k=${seen["$c"]:-0}
  [ "$k" = 1 ] || note "$c appears $k times (want exactly 1)"
done
for c in "${!seen[@]}"; do
  found=0; for w in "${WANT[@]}"; do [ "$w" = "$c" ] && found=1; done
  [ "$found" = 1 ] || note "undeclared/stale row present: $c"
done

while IFS=$'\t' read -r cell n o1 o2 o3 o4 cont term b1 b2 b3 b4 p1 p2 d1 d2 d4 closes rest; do
  [ "$cell" = "cell" ] && continue
  for v in "$o1" "$o2" "$o3" "$o4" "$cont" "$term"; do
    case "$v" in ''|*[!0-9]*) note "$cell: non-numeric field '$v'";; esac
  done
  [ "$o1" -lt "$o2" ] && [ "$o2" -lt "$o3" ] && [ "$o3" -lt "$o4" ] \
    || note "$cell: offsets not strictly increasing ($o1 $o2 $o3 $o4)"
  [ "$cont" -gt "$o4" ] || note "$cell: container $cont not greater than last offset $o4"
  [ "$term" = "$((cont-o4))" ] || note "$cell: terminator $term != container-lastoffset"
  [ "$b4" = "$(( (o4-o3) + term ))" ] || note "$cell: build4 $b4 does not carry the terminator"
  [ "$p1" = IDENTICAL ] || note "$cell: prefix_1in4=$p1"
  [ "$p2" = IDENTICAL ] || note "$cell: prefix_2in4=$p2"
  [ "$d1" = EXACT ] || note "$cell: decode_k1=$d1"
  [ "$d2" = EXACT ] || note "$cell: decode_k2=$d2"
  [ "$d4" = EXACT ] || note "$cell: decode_k4=$d4"
  [ "$closes" = "4/4" ] || note "$cell: build_close_rows=$closes"
done < "$T"

if [ "$fails" = 0 ]; then
  echo "GATE PASS: ${#WANT[@]} declared cells, $rows rows, all offsets/prefixes/decodes/closes verified"
  exit 0
fi
echo "GATE FAILED with $fails problem(s)"; exit 1
