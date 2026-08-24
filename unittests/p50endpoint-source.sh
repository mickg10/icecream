#!/bin/sh
# Supplemental deletion-sensitive gate for the adopted endpoint seam.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
endpoint="$src/cache/p50_endpoint.cpp"

adopted_body() {
    awk '
        /P50ServerEndpoint::run_adopted\(/ { inside = 1 }
        inside {
            print
            opens = gsub(/\{/, "{")
            closes = gsub(/\}/, "}")
            if (opens != 0 || closes != 0) depth += opens - closes
            if (depth == 0 && (opens != 0 || closes != 0)) exit
        }
    ' "$1"
}

body=$(adopted_body "$endpoint")
printf '%s\n' "$body" | grep -F 'co_await run_connected(std::move(socket), session.serial' >/dev/null
if printf '%s\n' "$body" | grep -E 'listen|async_accept|async_read_frame|decode_as|materialize_and_commit' >/dev/null; then
    echo 'FAIL: adopted endpoint contains a reducer bypass or a second accept' >&2
    exit 1
fi
echo 'ok - adopted endpoint delegates to the shared connected reducer'
echo 'ok - adopted endpoint has no accept/read/reducer duplicate'

mutant=$(mktemp "${TMPDIR:-/tmp}/p50endpoint-mutant.XXXXXX")
trap 'rm -f "$mutant"' EXIT HUP INT TERM
sed 's/co_return co_await run_connected(std::move(socket), session.serial, std::move(control), nullptr);/co_return ServerRunResult{};/' \
    "$endpoint" >"$mutant"
if adopted_body "$mutant" | grep -F 'co_await run_connected(std::move(socket), session.serial' >/dev/null; then
    echo 'FAIL: reducer-deletion mutant was accepted' >&2
    exit 1
fi
echo 'ok - deleting adopted delegation is rejected'

grep -F 'adopt_connected_fd' "$src/cache/p50_endpoint.h" >/dev/null
grep -F 'CLOEXEC' "$src/cache/P50_ENDPOINT.md" >/dev/null
grep -F '`0/0/0`' "$src/cache/P50_ENDPOINT.md" >/dev/null
echo 'PASS: adopted endpoint source gate preserves ownership and mechanism-only scope'
