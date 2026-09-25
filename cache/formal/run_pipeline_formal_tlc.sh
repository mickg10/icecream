#!/bin/sh
set -eu

: "${TLA2TOOLS_JAR:?set TLA2TOOLS_JAR to the pinned tla2tools.jar}"
: "${TLC_STATE_ROOT:?set TLC_STATE_ROOT to a unique retained scratch directory}"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
"$SCRIPT_DIR/run_pipeline_recovery_tlc.sh"
"$SCRIPT_DIR/run_pipeline_replacement_tlc.sh"

printf 'PIPELINE-FORMAL-TLC PASS lanes=2\n'
