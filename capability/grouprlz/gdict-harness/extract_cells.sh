#!/bin/bash
# Extract docker-matrix cells: extract_cells.sh <project> [profiles...]
set -euo pipefail
M=/home/ttuser/ictmp/ii-matrix
C=/home/ttuser/gdict/cells
p="$1"; shift
for f in "$@"; do
  t="$M/$p/$f/$p-$f.ii.tar.zst"
  [ -f "$t" ] || { echo "MISSING $p/$f"; continue; }
  ( cd "$M/$p/$f" && sha256sum -c "$(basename "$t").sha256" >/dev/null ) || { echo "SHA FAIL $p/$f"; exit 1; }
  out="$C/${p}__${f}"; rm -rf "$out"; mkdir -p "$out"
  zstd -dc --long=31 "$t" | tar -xf - -C "$out"
  # absolute-path manifest in canonical ordinal order
  awk -F"\t" -v d="$out" "NR>1{print d\"/\"\$2}" "$out/manifest.tsv" > "$C/${p}__${f}.man"
  echo "$p/$f tus=$(wc -l < "$C/${p}__${f}.man") raw=$(awk -F\"\t\" \"NR>1{s+=\\\$3}END{print s}\" "$out/manifest.tsv")"
done
