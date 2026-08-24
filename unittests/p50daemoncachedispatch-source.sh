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
grep -F 'next_request_id_++' "$impl" >/dev/null
grep -F 'disable();' "$impl" >/dev/null
grep -F 'handle_cache_session' "$daemon" >/dev/null
grep -F 'cache_dispatcher->dispatch' "$daemon" >/dev/null
grep -F 'p50_daemon_cache_dispatch.cpp' "$makefile" >/dev/null
grep -F 'p50daemoncachedispatch' "$src/unittests/Makefile.am" >/dev/null
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

# This lane must not silently acquire endpoint or cache-service ownership.
if git -C "$src" diff --name-only | grep -E 'p50_(cache_service|endpoint)'; then
    echo 'FAIL: daemon cache-dispatch lane edited service/endpoint sources' >&2
    exit 1
fi
echo 'ok - daemon cache-dispatch source and mutant gates hold'
