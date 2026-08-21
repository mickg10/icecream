#!/bin/sh
set -eu

: "${TLA2TOOLS_JAR:?set TLA2TOOLS_JAR to tla2tools.jar}"
JAVA_BIN=${JAVA_BIN:-java}
TLC_WORKERS=${TLC_WORKERS:-1}
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
STATE_ROOT=${TLC_STATE_ROOT:-"${TMPDIR:-/tmp}/icecream-p50-tlc"}

rm -rf "$STATE_ROOT/core" "$STATE_ROOT/job"
mkdir -p "$STATE_ROOT/core" "$STATE_ROOT/job"

"$JAVA_BIN" -XX:+UseParallelGC -jar "$TLA2TOOLS_JAR" \
    -workers "$TLC_WORKERS" \
    -metadir "$STATE_ROOT/core" \
    -config "$ROOT/cache/formal/Protocol50.cfg" \
    "$ROOT/cache/formal/Protocol50.tla"

"$JAVA_BIN" -XX:+UseParallelGC -jar "$TLA2TOOLS_JAR" \
    -workers "$TLC_WORKERS" \
    -metadir "$STATE_ROOT/job" \
    -config "$ROOT/cache/formal/Protocol50JobLifecycle.cfg" \
    "$ROOT/cache/formal/Protocol50JobLifecycle.tla"
