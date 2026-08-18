#!/bin/bash
# Run local-oracle selector cells over packaged corpora: sweep.sh <cellname>...
set -uo pipefail
T=/home/ttuser/issue16-selector-v1/tools
for c in "$@"; do
  d=/home/ttuser/runway/cells/$c
  r=/home/ttuser/runway/runs/$c
  [ -f "$r/measurement.json" ] && { echo "skip $c"; continue; }
  rm -rf "$r"
  ( cd "$T" && CELL_DIR="$d" RUN_DIR="$r" P29_BIN=/home/ttuser/gdict/bin/codec50-bigcap-mt \
      ./run_selector_cell.sh > /home/ttuser/runway/runs/$c.log 2>&1 )
  if [ -f "$r/measurement.json" ]; then
    python3 -c "
import json,sys
d=json.load(open(\"$r/measurement.json\"))
print(\"%-10s tus=%-6d raw=%-12d grz=%-10d p29=%-10d winner=%s\" % (
  d[\"project\"], d[\"tus\"] if \"tus\" in d else -1, d[\"raw_bytes\"],
  d[\"grz\"][\"wire_bytes\"], d[\"p29\"][\"wire_bytes\"], d[\"winner\"]))"
  else echo "$c FAILED (see runs/$c.log)"; fi
done
