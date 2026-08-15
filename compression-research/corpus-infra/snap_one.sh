#!/bin/bash
# One transfer-snapshot job.  Invoked by the parallel pool in snapshot_corpuses.sh.
#
#   snap_one.sh ii  <corpusN>
#   snap_one.sh src <checkout-dir>
#
# Fast settings on purpose: these are TRANSFER archives (shipped to quietbox2), not the
# metrics measurement.  zstd -3 --long=27 -T0.
#
# The archive is written to NAME.tar.zst.tmp and mv'd into place only on success, so a
# concurrent rsync watching this directory can never pick up a half-written file.
set -uo pipefail
ICT=/tanksmall/scratch/ictmp
OUT=$ICT/corpus-snapshots
ZARGS=(-3 --long=27 -T0)

kind="$1"; arg="$2"
mkdir -p "$OUT"

case "$kind" in
ii)
  c="$arg"; d="$ICT/$c"; out="$OUT/$c.ii.tar.zst"
  [ -s "$out" ] && { echo "[$c.ii] present -- skip"; exit 0; }
  [ -s "$d/manifest.txt" ] || { echo "[$c.ii] NO MANIFEST"; exit 1; }
  members="$OUT/.$c.members"
  sed "s#^$d/##" "$d/manifest.txt" > "$members"
  for extra in manifest.txt METADATA.json NAME; do
    [ -f "$d/$extra" ] && echo "$extra" >> "$members"
  done
  tar -C "$d" --files-from="$members" -cf - | zstd "${ZARGS[@]}" -c > "$out.tmp"
  st=("${PIPESTATUS[@]}")
  ;;
src)
  # arg is the checkout dir; tar it by name from its parent so paths stay relative.
  d="$arg"; name=$(basename "$d"); parent=$(dirname "$d")
  out="$OUT/src-$name.tar.zst"
  [ -s "$out" ] && { echo "[src-$name] present -- skip"; exit 0; }
  [ -d "$d" ] || { echo "[src-$name] MISSING CHECKOUT $d"; exit 1; }
  # SOURCE-ONLY.  Beyond the required .git/build/CMakeFiles/*.ii exclusions, compiled
  # output has to go too: Godot builds IN-TREE with SCons, so its checkout carries
  # ~2957 .o files and a 230 MB linked editor under bin/ -- 1.9 GiB that is not source
  # and has no business in a training root.  The other 13 checkouts are clean; these
  # patterns are no-ops there.
  tar -C "$parent" \
      --exclude='*/.git' --exclude='.git' \
      --exclude='*/build' --exclude='build' \
      --exclude='*/CMakeFiles' --exclude='CMakeFiles' \
      --exclude='*.ii' \
      --exclude='*.o' --exclude='*.os' --exclude='*.obj' \
      --exclude='*.a' --exclude='*.lib' \
      --exclude='*.so' --exclude='*.so.*' --exclude='*.dylib' --exclude='*.dll' \
      --exclude='*.pyc' --exclude='*.pyo' --exclude='__pycache__' \
      --exclude='.sconsign.dblite' --exclude="$name/bin" \
      -cf - "$name" | zstd "${ZARGS[@]}" -c > "$out.tmp"
  st=("${PIPESTATUS[@]}")
  ;;
*) echo "usage: snap_one.sh {ii|src} <arg>"; exit 2 ;;
esac

if [ "${st[0]}" != "0" ] || [ "${st[1]}" != "0" ]; then
  echo "[$(basename "$out")] FAILED (tar=${st[0]} zstd=${st[1]})"
  rm -f "$out.tmp"; exit 1
fi
mv "$out.tmp" "$out"          # atomic publish
[ "$kind" = ii ] && rm -f "$OUT/.$arg.members"
echo "[$(basename "$out")] $(stat -c%s "$out") bytes"
