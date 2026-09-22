#!/bin/sh
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
build=${ICECC_TEST_BUILDDIR:-${TMPDIR:-/tmp}/icecream-endpoint-cancel-build}
mkdir -p "$build"
stamp=$(date -u +%Y%m%dT%H%M%SZ)
evidence=${ICECC_ENDPOINT_CANCEL_EVIDENCE_DIR:-${TMPDIR:-/tmp}/icecream-endpoint-run-cancel/$stamp}
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

for token in active_socket active_io socket_for_test_cancel active_cancel_fd_ \
    request_cancel_for_test whole_new_attempt ClientRunSettlement AbortedPreDurable; do
    if awk -v token="$token" '
        /^[[:space:]]*#if(n?def)?[[:space:]]+ICECC_P50_ENDPOINT_TEST_HOOKS/ {guard++}
        /^[[:space:]]*#endif/ && guard > 0 {guard--; next}
        guard == 0 && index($0, token) {print FILENAME ":" FNR ":" $0; bad=1}
        END {exit bad ? 0 : 1}
    ' "$src/cache/p50_endpoint.cpp" "$src/cache/p50_endpoint.h" \
      "$src/cache/p50_cache_service.cpp" "$src/cache/p50_cache_service.h" \
      >"$evidence/forbidden.txt"; then
        printf '%s\n' '{"test":"forbidden-symbol-census","status":"fail"}' >>"$evidence/results.jsonl"
        exit 1
    fi
done
printf '%s\n' '{"test":"forbidden-symbol-census","status":"pass"}' >>"$evidence/results.jsonl"

cat >"$build/no-arg-mutant.cpp" <<EOF
#include "$src/cache/p50_endpoint.h"
int main() { icecc::p50::P50ServerEndpoint *endpoint = nullptr; endpoint->request_cancel_for_test(); }
EOF
if "$cxx" $common -c "$build/no-arg-mutant.cpp" -o "$build/no-arg-mutant.o" \
    >"$evidence/no-arg-mutant.log" 2>&1; then
    printf '%s\n' '{"test":"no-arg-cancellation-mutant","status":"red-failure"}' >>"$evidence/results.jsonl"
    exit 1
fi
printf '%s\n' '{"test":"no-arg-cancellation-mutant","status":"pass-red"}' >>"$evidence/results.jsonl"

# A singleton/current-run deletion mutant is compiled without the test hook.
# Restoring a product socket alias must fail because the only cancellation
# authority is the typed registry permit.
sed 's/impl_->endpoint_runs.request_cancel(permit)/impl_->socket_for_test_cancel->close()/g' \
    "$src/cache/p50_endpoint.cpp" >"$build/singleton-mutant.cpp"
if "$cxx" $common -fsyntax-only "$build/singleton-mutant.cpp" \
    >"$evidence/singleton-mutant.log" 2>&1; then
    printf '%s\n' '{"test":"singleton-cancellation-mutant","status":"red-failure"}' >>"$evidence/results.jsonl"
    exit 1
fi
printf '%s\n' '{"test":"singleton-cancellation-mutant","status":"pass-red"}' >>"$evidence/results.jsonl"

# The actual endpoint target carries the production-shaped concurrent and ABA
# rows; record its invocation in the same manifest when the configured build
# supplies it.
test_build=${ICECC_TEST_BUILDDIR:-}
if [ -n "$test_build" ] && [ -x "$test_build/unittests/p50endpoint" ]; then
    "$test_build/unittests/p50endpoint" >"$evidence/endpoint.log" 2>&1
    printf '%s\n' '{"test":"real-endpoint-rows","status":"pass"}' >>"$evidence/results.jsonl"
else
    printf '%s\n' '{"test":"real-endpoint-rows","status":"not-built"}' >>"$evidence/results.jsonl"
fi

# Git metadata is absent from release archives and Docker source snapshots.
# This auxiliary whitespace check must not prevent the runtime checks above
# from being used in those builds, or inspect an unrelated parent repository.
if [ -e "$src/.git" ]; then
    git -C "$src" diff --check
    printf '%s\n' '{"test":"git-diff-check","status":"pass"}' >>"$evidence/results.jsonl"
else
    printf '%s\n' '{"test":"git-diff-check","status":"not-applicable-source-snapshot"}' >>"$evidence/results.jsonl"
fi
printf 'PASS: endpoint run cancellation normal/sanitizer/source census (%s)\n' "$evidence"
