#!/usr/bin/env bash
set -euo pipefail

: "${II_COMPILE_COMMANDS:?set II_COMPILE_COMMANDS to compile_commands.json}"
: "${LOOSE_ROOT:?}"
: "${CELL_ROOT:?}"
jobs=${JOBS:-8}
allow_failures=${II_ALLOW_FAILURES:-0}
limit=${II_TU_LIMIT:-0}

python3 /harness/scripts/preprocess_compile_commands.py \
    --cc-json "$II_COMPILE_COMMANDS" --out "$LOOSE_ROOT" --jobs "$jobs" \
    --allow-failures "$allow_failures" --limit "$limit" \
    --raw-manifest "$CELL_ROOT/raw-manifest.txt" \
    --commands-out "$CELL_ROOT/commands.jsonl" \
    --log "$CELL_ROOT/logs/preprocess.log"
