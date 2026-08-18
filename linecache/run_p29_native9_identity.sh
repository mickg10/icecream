#!/usr/bin/env bash
# Foreground, resumable corrected-P29 sweep over the nine untouched native holdouts.

set -euo pipefail

SCRIPT_PATH=${BASH_SOURCE[0]-$0}
SCRIPT_DIR=$(cd -- "$(dirname -- "$SCRIPT_PATH")" && pwd)

: "${RUN_ROOT:?set RUN_ROOT to a new retained native-nine directory}"

SOURCE_INVENTORY=${SOURCE_INVENTORY:-$SCRIPT_DIR/native9-manifest-sources.tsv}
MATRIX_RUNNER=${MATRIX_RUNNER:-$SCRIPT_DIR/run_p29_fixed16_identity.sh}
SUMMARIZER=${SUMMARIZER:-$SCRIPT_DIR/summarize_p29_native9_identity.py}
EXPECTED_CELLS=${EXPECTED_CELLS:-9}

for path in "$SOURCE_INVENTORY" "$MATRIX_RUNNER" "$SUMMARIZER"; do
    [[ -f "$path" ]] || { printf 'missing native-nine input: %s\n' "$path" >&2; exit 1; }
done
[[ -x "$MATRIX_RUNNER" ]] || { printf 'matrix runner is not executable: %s\n' "$MATRIX_RUNNER" >&2; exit 1; }
[[ ! -e "$RUN_ROOT/native9-wrapper.sha256" ]] || {
    printf 'native-nine wrapper provenance already exists: %s\n' "$RUN_ROOT" >&2
    exit 1
}

RUN_ROOT="$RUN_ROOT" SOURCE_INVENTORY="$SOURCE_INVENTORY" EXPECTED_CELLS="$EXPECTED_CELLS" \
    SUMMARIZER="$SUMMARIZER" "$MATRIX_RUNNER"

sha256sum "$SCRIPT_PATH" "$SOURCE_INVENTORY" "$MATRIX_RUNNER" "$SUMMARIZER" \
    > "$RUN_ROOT/native9-wrapper.sha256"
printf 'corrected P29 native-nine sweep complete: %s\n' "$RUN_ROOT"
