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
grep -F 'verify_peer_credentials' "$impl" >/dev/null
grep -F 'validate_handshake' "$impl" >/dev/null
grep -F 'relationship.send_until' "$impl" >/dev/null
grep -F 'relationship.receive_until' "$impl" >/dev/null
grep -F 'connect_unix_until' "$impl" >/dev/null
grep -F 'expected_peer.pid' "$test" >/dev/null
grep -F 'on_demand_' "$impl" >/dev/null
grep -F 'const auto deadline' "$impl" >/dev/null
grep -F 'identity != identity_' "$impl" >/dev/null
grep -F 'store_identity_guid_valid_for_role(c_store_guid.bytes' "$impl" >/dev/null
grep -F 'kStoreIdentityClientRole' "$impl" >/dev/null
grep -F 'store_identity_guid_valid_for_role(f_store_guid.bytes' "$impl" >/dev/null
grep -F 'kStoreIdentityFileRole' "$impl" >/dev/null
grep -F 'c_store_guid != f_store_guid' "$impl" >/dev/null
grep -F 'next_request_id_++' "$impl" >/dev/null
grep -F 'disable();' "$impl" >/dev/null
grep -Fx '    if (!on_demand_->current_path_matches())' "$impl" >/dev/null

# The dispatcher owns only an immutable on-demand lease.  A cached control
# relationship or the removed attach API would bypass fresh accept,
# credentials, HELLO/ACK, and one-shot handoff ownership.
for file in "$impl" "$header" "$test"; do
    if grep -nE 'attach_authenticated|sidecar_' "$file" >/dev/null; then
        echo "FAIL: cached relationship/attach API remains in $file" >&2
        exit 1
    fi
done
for needle in \
    'OnDemandEndpoint' 'set_on_demand_endpoint' 'fresh endpoint relationship' \
    'fresh accepted relationship' 'endpoint lease' 'path replacement' \
    'wrong endpoint path digest' 'wrong endpoint F_STORE_GUID' \
    'zero endpoint C_STORE_GUID' \
    'wrong endpoint listener device' 'wrong endpoint listener inode' \
    'post-HELLO endpoint replacement' \
    'post-release peer disconnect' 'post-release handoff timeout' \
    'request_id == 1' 'request_id == 2'; do
    grep -F "$needle" "$test" >/dev/null
done
grep -F 'handle_cache_session' "$daemon" >/dev/null
grep -F 'cache_advertisement_snapshot().present()' "$daemon" >/dev/null
grep -F 'cache_adapter->dispatcher()->dispatch(' "$daemon" >/dev/null
grep -F 'poll_cache_adapter();' "$daemon" >/dev/null
grep -F 'exact_public_tcp_listener' "$daemon" >/dev/null
grep -F 'SO_ACCEPTCONN' "$daemon" >/dev/null
grep -F 'scheduler_cache_snapshot_valid = false' "$daemon" >/dev/null
grep -F 'cache_adapter->outer_request_shutdown(&update)' "$daemon" >/dev/null
grep -F 'connection_leases.revalidate' "$daemon" >/dev/null
grep -F 'handoff_acknowledged' "$impl" "$header" >/dev/null
grep -F 'trailing_byte_barrier' "$impl" "$header" >/dev/null
grep -F 'emit_attachment_phase_open' "$impl" "$header" "$test" >/dev/null
grep -F 'p50_phase_open.cpp' "$makefile" >/dev/null
grep -F 'p50_reverse_fd_retry.cpp' "$makefile" >/dev/null
grep -F 'P50_PROTOCOL.md' "$src/cache/Makefile.am" >/dev/null
grep -F 'Additional tested components' "$src/cache/P50_PROTOCOL.md" >/dev/null
grep -F 'p50_daemon_cache_dispatch.cpp' "$makefile" >/dev/null
grep -F 'libp50daemonsidecaradapter.a' "$makefile" >/dev/null
grep -F 'libp50readyadvertisement.a' "$makefile" >/dev/null
grep -F 'libp50sidecarsupervisor.a' "$makefile" >/dev/null
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
    'buffered ordinary byte' 'stale or wrong ACK' 'post-release' \
    'P49 discriminator' 'normal ordinary job' 'fresh relationship'; do
    grep -F "$needle" "$test" >/dev/null
done

bounded_hello_source() {
    grep -F 'relationship.send_until' "$1" >/dev/null &&
        grep -F 'relationship.receive_until' "$1" >/dev/null &&
        grep -F 'validate_handshake' "$1" >/dev/null
}
if ! bounded_hello_source "$impl"; then
    echo 'FAIL: production HELLO path is not bounded/authenticated' >&2
    exit 1
fi

# A handshake bypass/deletion mutant must not satisfy the source gate.  The
# runtime matrix separately proves the saturated non-reading peer timeout.
mutant=$(mktemp "${TMPDIR:-/tmp}/p50daemoncachedispatch-hello-mutant.XXXXXX")
mutant_identity=$(mktemp "${TMPDIR:-/tmp}/p50daemoncachedispatch-identity-mutant.XXXXXX")
trap 'rm -f "$mutant" "$mutant_identity"' EXIT HUP INT TERM
sed 's/relationship\.send_until(/relationship.send(/' "$impl" >"$mutant"
if bounded_hello_source "$mutant"; then
    echo 'FAIL: HELLO bounded-send deletion/bypass mutant was accepted' >&2
    exit 1
fi
echo 'ok - HELLO bounded-send deletion/bypass mutant is rejected'

# Deleting the post-connect listener identity check must be observable. The
# runtime row replaces the pathname after HELLO and before release; this source
# witness prevents a build configuration from silently omitting that guard.
post_connect_identity_guard() {
    grep -Fx '    if (!on_demand_->current_path_matches())' "$1" >/dev/null
}
sed '/^    if (!on_demand_->current_path_matches())$/,+2d' "$impl" >"$mutant_identity"
if post_connect_identity_guard "$mutant_identity"; then
    echo 'FAIL: post-connect endpoint identity deletion mutant was accepted' >&2
    exit 1
fi
echo 'ok - post-connect endpoint identity deletion mutant is rejected'

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
grep -F '(POLLERR | POLLNVAL)' "$src/cache/p50_local_transport.h" >/dev/null
grep -F 'if ((descriptor.revents & POLLHUP)' "$src/cache/p50_local_transport.h" >/dev/null
echo 'ok - transport terminal poll conditions are explicit'
echo 'ok - daemon cache-dispatch source and mutant gates hold'
