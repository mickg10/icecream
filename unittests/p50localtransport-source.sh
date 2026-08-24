#!/bin/sh
# Deletion-sensitive gate for failed-listener pathname ownership.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
transport="$src/cache/p50_local_transport.cpp"

listener_body() {
    sed -n '/^int listen_unix/,/^Connection connect_unix/p' "$1"
}

safe_listener() {
    body=$(listener_body "$1")
    printf '%s\n' "$body" | grep -F 'Status::ListenerNodeLeftForCleanup' >/dev/null
    if printf '%s\n' "$body" | grep -F '::unlink(path.c_str())' >/dev/null; then
        return 1
    fi
}

if ! safe_listener "$transport"; then
    echo 'FAIL: listener failure path may unlink a pathname after bind' >&2
    exit 1
fi
echo 'ok - failed listener leaves post-bind node for identity-safe cleanup'

mutant=$(mktemp "${TMPDIR:-/tmp}/p50localtransport-mutant.XXXXXX")
trap 'rm -f "$mutant"' EXIT HUP INT TERM
sed '/Status::ListenerNodeLeftForCleanup, status);/i\        ::unlink(path.c_str());' \
    "$transport" >"$mutant"
if safe_listener "$mutant"; then
    echo 'FAIL: deletion mutant was accepted by the unlink safety gate' >&2
    exit 1
fi
echo 'ok - unsafe pathname-unlink deletion mutant is rejected'

echo 'PASS: listener failure cleanup remains supervisor-owned and race-safe'
