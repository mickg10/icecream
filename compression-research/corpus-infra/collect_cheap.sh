#!/bin/bash
# Phase 1a: cheap metrics (counts / logical bytes / LOC) for every corpus.
# Writes per-corpus file lists into corpus-infra/lists/ so the (expensive)
# compression pass can reuse exactly the same file sets.
set -uo pipefail
source /tanksmall/scratch/ictmp/corpus-infra/lib_corpora.sh
mkdir -p "$INFRA/lists" "$INFRA/cheap"

for c in "${CORPORA[@]}"; do
  echo "=== $c ($(proj_of "$c")) $(date +%T) ==="
  man="$ICT/$c/manifest.txt"
  ii_list="$INFRA/lists/$c.ii.list"
  src_list="$INFRA/lists/$c.src.list"

  # --- .ii side (manifest is one absolute path per line, no spaces in practice) ---
  cp "$man" "$ii_list"
  TU=$(wc -l < "$ii_list")
  bytes_ii=$(tr '\n' '\0' < "$ii_list" | xargs -0 -n 500 stat -c%s | awk '{s+=$1} END{print s+0}')
  loc_ii=$(tr '\n' '\0' < "$ii_list" | xargs -0 -n 500 cat | wc -l)

  # --- raw source side ---
  src_files_0 "$c" | sort -z > "$src_list.0"
  tr '\0' '\n' < "$src_list.0" > "$src_list"
  nfiles_src=$(tr -cd '\0' < "$src_list.0" | wc -c)
  if [ "$nfiles_src" -gt 0 ]; then
    bytes_src=$(xargs -0 -n 500 stat -c%s < "$src_list.0" | awk '{s+=$1} END{print s+0}')
    loc_src=$(xargs -0 -n 500 cat < "$src_list.0" | wc -l)
  else
    bytes_src=0; loc_src=0
  fi

  printf 'TU\t%s\nbytes_ii\t%s\nloc_ii\t%s\nnfiles_src\t%s\nbytes_src\t%s\nloc_src\t%s\n' \
    "$TU" "$bytes_ii" "$loc_ii" "$nfiles_src" "$bytes_src" "$loc_src" > "$INFRA/cheap/$c.tsv"
  cat "$INFRA/cheap/$c.tsv" | tr '\n' ' '; echo
done
echo "CHEAP DONE $(date)"
