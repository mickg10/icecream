#!/bin/sh
# Deletion-sensitive gate for failed-listener pathname ownership.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
transport="$src/cache/p50_local_transport.cpp"
poll_helper="$src/cache/p50_local_transport.h"

# The bounded writer is a production invariant: the complete encoded frame
# must use one deadline, not the legacy blocking writer.  Keep a deletion
# witness here so the focused runtime test cannot be bypassed by removing the
# new call site.
bounded_writer_source() {
    grep -F 'Status write_all_until' "$1" >/dev/null &&
        grep -F 'Status Connection::send_until' "$1" >/dev/null &&
        grep -F 'write_all_until(fd_, encoded, deadline)' "$1" >/dev/null &&
        grep -F 'Status::Timeout' "$1" >/dev/null &&
        grep -F 'MSG_DONTWAIT' "$1" >/dev/null
}
if ! bounded_writer_source "$transport"; then
    echo 'FAIL: production bounded writer is missing' >&2
    exit 1
fi
send_mutant=$(mktemp "${TMPDIR:-/tmp}/p50localtransport-send-mutant.XXXXXX")
trap 'rm -f "$send_mutant"' EXIT HUP INT TERM
sed 's/write_all_until(fd_, encoded, deadline)/write_all(fd_, encoded)/' \
    "$transport" >"$send_mutant"
if bounded_writer_source "$send_mutant"; then
    echo 'FAIL: bounded writer deletion/bypass mutant was accepted' >&2
    exit 1
fi
echo 'ok - bounded writer deletion/bypass mutant is rejected'

bounded_reader_source() {
    grep -F 'Status Connection::receive_until' "$1" >/dev/null &&
        grep -F 'read_frame_until(fd_, frame, deadline)' "$1" >/dev/null &&
        grep -F 'Status::Timeout' "$1" >/dev/null
}
if ! bounded_reader_source "$transport"; then
    echo 'FAIL: production bounded reader is missing' >&2
    exit 1
fi
receive_mutant=$(mktemp "${TMPDIR:-/tmp}/p50localtransport-receive-mutant.XXXXXX")
trap 'rm -f "$send_mutant" "$receive_mutant"' EXIT HUP INT TERM
sed 's/read_frame_until(fd_, frame, deadline)/read_frame(fd_, frame)/' \
    "$transport" >"$receive_mutant"
if bounded_reader_source "$receive_mutant"; then
    echo 'FAIL: bounded reader deletion/bypass mutant was accepted' >&2
    exit 1
fi
echo 'ok - bounded reader deletion/bypass mutant is rejected'

connector_body() {
    sed -n '/^Connection connect_unix_until/,/^Connection accept_unix/p' "$1"
}

bounded_connector_source() {
    body=$(connector_body "$1")
    printf '%s\n' "$body" | grep -F 'connect_error == EINPROGRESS' >/dev/null &&
        printf '%s\n' "$body" | grep -F 'detail::wait_for_io(fd, POLLOUT, deadline)' >/dev/null &&
        printf '%s\n' "$body" | grep -F 'getsockopt(fd, SOL_SOCKET, SO_ERROR' >/dev/null &&
        printf '%s\n' "$body" | grep -F 'original_flags & ~O_NONBLOCK' >/dev/null &&
        printf '%s\n' "$body" | grep -F 'std::chrono::steady_clock::now() >= deadline' >/dev/null
}

if ! bounded_connector_source "$transport"; then
    echo 'FAIL: bounded AF_UNIX connector is missing a required guard' >&2
    exit 1
fi
echo 'ok - bounded AF_UNIX connector has deadline, wait, SO_ERROR, and restore guards'

connector_mutant=$(mktemp "${TMPDIR:-/tmp}/p50localtransport-connect-mutant.XXXXXX")
trap 'rm -f "$send_mutant" "$receive_mutant" "$connector_mutant"' EXIT HUP INT TERM
sed 's/detail::wait_for_io(fd, POLLOUT, deadline)/detail::wait_for_io(fd, POLLIN, deadline)/' \
    "$transport" >"$connector_mutant"
if bounded_connector_source "$connector_mutant"; then
    echo 'FAIL: connector readiness deletion mutant was accepted' >&2
    exit 1
fi
echo 'ok - connector readiness deletion mutant is rejected'

sed '/getsockopt(fd, SOL_SOCKET, SO_ERROR/d' "$transport" >"$connector_mutant"
if bounded_connector_source "$connector_mutant"; then
    echo 'FAIL: connector SO_ERROR deletion mutant was accepted' >&2
    exit 1
fi
echo 'ok - connector SO_ERROR deletion mutant is rejected'

sed '/::fcntl(fd, F_SETFL, original_flags & ~O_NONBLOCK)/d' "$transport" >"$connector_mutant"
if bounded_connector_source "$connector_mutant"; then
    echo 'FAIL: connector blocking-restore deletion mutant was accepted' >&2
    exit 1
fi
echo 'ok - connector blocking-restore deletion mutant is rejected'

sed 's/std::chrono::steady_clock::now() >= deadline/false/g' "$transport" >"$connector_mutant"
if bounded_connector_source "$connector_mutant"; then
    echo 'FAIL: connector deadline deletion mutant was accepted' >&2
    exit 1
fi
echo 'ok - connector deadline deletion mutant is rejected'

# A bounded read/write operation must use per-call MSG_DONTWAIT and never
# toggle the shared open-file-description status flags.  The connector above
# is the sole exception: it owns a newly-created descriptor until return and
# explicitly restores that descriptor before handing it to Connection.  The
# local transport and FD handoff both use the one shared absolute-deadline poll
# helper in the public transport header; keep its terminal checks explicit so
# a POLLERR/POLLHUP/POLLNVAL deletion mutant is visible in review.
grep -F 'MSG_DONTWAIT' "$transport" >/dev/null
grep -F 'detail::wait_for_io' "$transport" >/dev/null
grep -F 'DeadlinePollResult' "$poll_helper" >/dev/null
grep -F '(POLLERR | POLLHUP | POLLNVAL)' "$poll_helper" >/dev/null
grep -F '(descriptor.revents & events)' "$poll_helper" >/dev/null
if grep -F '(us + 999)' "$transport" "$poll_helper" >/dev/null; then
    echo 'FAIL: shared deadline helper still rounds sub-millisecond waits up' >&2
    exit 1
fi
echo 'ok - bounded transport preserves shared flags and rejects terminal poll state'

listener_body() {
    sed -n '/^static int listen_unix_impl/,/^Connection connect_unix/p' "$1"
}

safe_listener() {
    body=$(listener_body "$1")
    printf '%s\n' "$body" | grep -F 'Status::ListenerNodeLeftForCleanup' >/dev/null
    if printf '%s\n' "$body" | grep -E '::unlink\(path\.c_str\(\)\)|getenv|usleep|ICECC_TEST_LOCAL_TRANSPORT' >/dev/null; then
        return 1
    fi
}

if ! safe_listener "$transport"; then
    echo 'FAIL: listener failure path may unlink a pathname after bind' >&2
    exit 1
fi
echo 'ok - failed listener leaves post-bind node for identity-safe cleanup'

mutant=$(mktemp "${TMPDIR:-/tmp}/p50localtransport-mutant.XXXXXX")
trap 'rm -f "$send_mutant" "$mutant"' EXIT HUP INT TERM
sed '/Status::ListenerNodeLeftForCleanup, status);/i\        ::unlink(path.c_str());' \
    "$transport" >"$mutant"
if safe_listener "$mutant"; then
    echo 'FAIL: deletion mutant was accepted by the unlink safety gate' >&2
    exit 1
fi
echo 'ok - unsafe pathname-unlink deletion mutant is rejected'

# The production object must not contain the compile-time test seam.  Build a
# macro-free object, inspect its symbols/strings, then build the deliberate
# macro-reintroduction mutant and prove the same production predicate rejects
# it.  This catches accidentally shipping a runtime hook branch or symbol.
cxx=${ICECC_TEST_CXX:-g++}
production_object=$(mktemp "${TMPDIR:-/tmp}/p50localtransport-production.XXXXXX.o")
hook_mutant_object=$(mktemp "${TMPDIR:-/tmp}/p50localtransport-hook-mutant.XXXXXX.o")
trap 'rm -f "$send_mutant" "$mutant" "$production_object" "$hook_mutant_object"' EXIT HUP INT TERM

"$cxx" -std=c++20 -Wall -Wextra -Werror -pthread -I"$src" \
    -UICECC_P50_LOCAL_TRANSPORT_TEST_HOOKS -c "$transport" -o "$production_object"

production_safe() {
    if nm -C "$1" | grep -E 'listen_unix_with_test_hook|ListenPostBindTestHook|getenv|usleep' >/dev/null; then
        return 1
    fi
    if strings "$1" | grep -E 'ICECC_P50_LOCAL_TRANSPORT_TEST_HOOKS|ICECC_TEST_LOCAL_TRANSPORT|listen_unix_with_test_hook' >/dev/null; then
        return 1
    fi
}

if ! production_safe "$production_object"; then
    echo 'FAIL: production transport object contains test-hook seam' >&2
    exit 1
fi
echo 'ok - production object contains no test-hook symbol, branch, or string'

"$cxx" -std=c++20 -Wall -Wextra -Werror -pthread -I"$src" \
    -DICECC_P50_LOCAL_TRANSPORT_TEST_HOOKS -c "$transport" -o "$hook_mutant_object"
if production_safe "$hook_mutant_object"; then
    echo 'FAIL: macro-reintroduction test-hook mutant was accepted' >&2
    exit 1
fi
echo 'ok - macro-reintroduction test-hook mutant is rejected'

echo 'PASS: listener failure cleanup remains supervisor-owned and race-safe'
