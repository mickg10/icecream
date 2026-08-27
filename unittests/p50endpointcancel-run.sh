#!/bin/sh
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
build=${ICECC_TEST_BUILDDIR:-${TMPDIR:-/tmp}/icecream-endpoint-cancel-build}
mkdir -p "$build"
stamp=$(date -u +%Y%m%dT%H%M%SZ)
evidence=${ICECC_ENDPOINT_CANCEL_EVIDENCE_DIR:-/tanksmall/scratch/ictmp/experiments/icecream/p50-endpoint-run-cancel/$stamp}
mkdir -p "$evidence"
printf '%s\n' '{"test":"p50endpointcancel","source":"typed-registry","status":"started"}' >"$evidence/manifest.jsonl"

cxx=${CXX:-c++}
common="-std=c++20 -O2 -pthread -I$src -I$src/cache -I$src/services"
"$cxx" $common "$src/cache/p50_endpoint_run_cancel.cpp" \
    "$src/unittests/p50_endpoint_run_cancel_test.cpp" -o "$build/p50endpointcancel"
"$build/p50endpointcancel" >"$evidence/normal.log" 2>&1
printf '%s\n' '{"test":"normal","status":"pass"}' >>"$evidence/results.jsonl"

"$cxx" $common -fsanitize=address,undefined -fno-omit-frame-pointer \
    "$src/cache/p50_endpoint_run_cancel.cpp" \
    "$src/unittests/p50_endpoint_run_cancel_test.cpp" -o "$build/p50endpointcancel-sanitize"
ASAN_OPTIONS=detect_leaks=1 "$build/p50endpointcancel-sanitize" >"$evidence/sanitize.log" 2>&1
printf '%s\n' '{"test":"sanitizer","status":"pass"}' >>"$evidence/results.jsonl"

forbidden_socket='active_'; forbidden_socket="${forbidden_socket}socket"
forbidden_cancel='cancel_'; forbidden_cancel="${forbidden_cancel}active_io"
forbidden_fd='active_'; forbidden_fd="${forbidden_fd}cancel_fd_"
forbidden_attempt='whole_'; forbidden_attempt="${forbidden_attempt}new_attempt"
if rg -n "${forbidden_socket}|${forbidden_cancel}|${forbidden_fd}|${forbidden_attempt}" \
    "$src/cache" "$src/client" "$src/daemon" "$src/unittests" --glob '*.{cpp,h,sh,md}' >"$evidence/forbidden.txt"; then
    printf '%s\n' '{"test":"forbidden-symbol-census","status":"fail"}' >>"$evidence/results.jsonl"
    exit 1
fi
printf '%s\n' '{"test":"forbidden-symbol-census","status":"pass"}' >>"$evidence/results.jsonl"

cat >"$build/no-arg-mutant.cpp" <<EOF
#include "$src/cache/p50_endpoint.h"
int main() { icecc::p50::P50ServerEndpoint *endpoint = nullptr; endpoint->${forbidden_cancel}(); }
EOF
if "$cxx" $common -c "$build/no-arg-mutant.cpp" -o "$build/no-arg-mutant.o" \
    >"$evidence/no-arg-mutant.log" 2>&1; then
    printf '%s\n' '{"test":"no-arg-cancellation-mutant","status":"red-failure"}' >>"$evidence/results.jsonl"
    exit 1
fi
printf '%s\n' '{"test":"no-arg-cancellation-mutant","status":"pass-red"}' >>"$evidence/results.jsonl"

git -C "$src" diff --check
printf '%s\n' '{"test":"git-diff-check","status":"pass"}' >>"$evidence/results.jsonl"
printf 'PASS: endpoint run cancellation normal/sanitizer/source census (%s)\n' "$evidence"
