#!/bin/sh
# Deletion-sensitive production wiring and bounded-state gate.  The focused
# binary supplies the behavioral mutants; this gate keeps the real iceccd path
# from regressing to a test-only facade.
set -eu
src=${ICECC_TEST_TOP_SRCDIR:?}
impl="$src/cache/p50_daemon_cache_dispatch.cpp"
header="$src/cache/p50_daemon_cache_dispatch.h"
daemon="$src/daemon/main.cpp"
makefile="$src/daemon/Makefile.am"
test="$src/unittests/p50_daemon_cache_dispatch_test.cpp"
real_test="$src/unittests/daemoncachedispatch.cpp"

grep -F 'release_fd_if_input_empty' "$impl" >/dev/null
grep -F 'decoded_type != kCacheSession' "$impl" >/dev/null
grep -F 'peer_credentials_verified' "$impl" >/dev/null
grep -F 'validate_handshake' "$impl" >/dev/null
grep -F 'connection.send_until' "$impl" >/dev/null
grep -F 'connection.receive_until' "$impl" >/dev/null
grep -F 'const auto deadline' "$impl" >/dev/null
grep -F 'identity != identity_' "$impl" >/dev/null
grep -F 'next_request_id_++' "$impl" >/dev/null
grep -F 'disable();' "$impl" >/dev/null
grep -F 'handle_cache_session' "$daemon" >/dev/null
grep -F 'cache_dispatcher->dispatch' "$daemon" >/dev/null
grep -F 'p50_daemon_cache_dispatch.cpp' "$makefile" >/dev/null
grep -F 'p50daemoncachedispatch' "$src/unittests/Makefile.am" >/dev/null
grep -F 'p50daemoncachedispatch-sanitize.sh' "$src/unittests/Makefile.am" >/dev/null
grep -F '../cache/p50_daemon_cache_dispatch.cpp' "$makefile" >/dev/null
grep -F '../cache/p50_daemon_cache_dispatch.cpp' "$src/unittests/Makefile.am" >/dev/null
test -f "$impl"
test -f "$header"
test -x "$src/unittests/p50daemoncachedispatch-sanitize.sh"
grep -F 'real public iceccd listener' "$real_test" >/dev/null
grep -F 'CACHE_SESSION and failed closed boundedly' "$real_test" >/dev/null

# Mutant witnesses: all must remain present in the production source and
# focused matrix (CACHE_SESSION check, read-ahead release, identity binding,
# duplicate/timeout/disconnect fail-close, normal/P49 discrimination, and fd
# ownership teardown).
for needle in \
    'ReleaseRefused' 'SidecarUnavailable' 'HandoffFailed' 'detached' \
    'request_id == 1' 'buffered byte blocks handoff' \
    'wrong/stale generation/attempt' 'duplicate request' \
    'sidecar disconnect' 'handoff timeout' 'P49 discriminator' \
    'normal-job misclassification' 'leaked fd/process'; do
    grep -F "$needle" "$test" >/dev/null
done

bounded_hello_source() {
    grep -F 'connection.send_until' "$1" >/dev/null &&
        grep -F 'connection.receive_until' "$1" >/dev/null &&
        grep -F 'validate_handshake' "$1" >/dev/null
}
if ! bounded_hello_source "$impl"; then
    echo 'FAIL: production HELLO path is not bounded/authenticated' >&2
    exit 1
fi

# A handshake bypass/deletion mutant must not satisfy the source gate.  The
# runtime matrix separately proves the saturated non-reading peer timeout.
mutant=$(mktemp "${TMPDIR:-/tmp}/p50daemoncachedispatch-hello-mutant.XXXXXX")
trap 'rm -f "$mutant"' EXIT HUP INT TERM
sed 's/connection\.send_until(/connection.send(/' "$impl" >"$mutant"
if bounded_hello_source "$mutant"; then
    echo 'FAIL: HELLO bounded-send deletion/bypass mutant was accepted' >&2
    exit 1
fi
echo 'ok - HELLO bounded-send deletion/bypass mutant is rejected'

# This lane must not silently acquire endpoint or cache-service ownership.  The
# archive gate is intentionally independent of Git: exact source archives do
# not have a repository metadata directory, and a failing `git diff` pipeline
# there used to produce noisy false evidence.  Scan every production wiring
# file directly for forbidden ownership references instead.
source_boundary_clean() {
    for file in "$impl" "$header" "$daemon" "$makefile" "$test"; do
        if grep -nE 'p50_(cache_service|endpoint)([.]cpp|[.]h|[[:space:]])' \
            "$file" >/dev/null; then
            return 1
        fi
    done
}
if ! source_boundary_clean; then
    echo 'FAIL: daemon cache-dispatch lane edited service/endpoint sources' >&2
    exit 1
fi
echo 'ok - daemon cache-dispatch source boundary holds without Git metadata'

grep -F 'detail::wait_for_io' "$src/cache/p50_local_transport.cpp" >/dev/null
grep -F 'detail::wait_for_io' "$src/cache/p50_fd_handoff.cpp" >/dev/null
grep -F '(POLLERR | POLLHUP | POLLNVAL)' "$src/cache/p50_local_transport.h" >/dev/null
echo 'ok - transport terminal poll conditions are explicit'
echo 'ok - daemon cache-dispatch source and mutant gates hold'
