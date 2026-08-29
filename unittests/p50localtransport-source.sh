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

retry_wait_body() {
    sed -n '/^ConnectRetryWaitResult wait_for_connect_retry/,/^int accept_cloexec/p' "$1"
}

bounded_connector_source() {
    body=$(connector_body "$1")
    printf '%s\n' "$body" | grep -F 'connect_error == EINPROGRESS || connect_error == EAGAIN' >/dev/null &&
        printf '%s\n' "$body" | grep -F 'connect_error == EINTR' >/dev/null &&
        printf '%s\n' "$body" | grep -F 'detail::wait_for_io(fd, POLLOUT, deadline)' >/dev/null &&
        printf '%s\n' "$body" | grep -F 'wait_for_connect_retry(deadline)' >/dev/null &&
        printf '%s\n' "$body" | grep -F 'getsockopt(fd, SOL_SOCKET, SO_ERROR' >/dev/null &&
        printf '%s\n' "$body" | grep -F 'original_flags & ~O_NONBLOCK' >/dev/null &&
        printf '%s\n' "$body" | grep -F 'std::chrono::steady_clock::now() >= deadline' >/dev/null &&
        test "$(printf '%s\n' "$body" | grep -Fc 'connect_once(')" -eq 1 &&
        test "$(printf '%s\n' "$body" | grep -Fc 'private_parent(path)')" -ge 2 &&
        test "$(printf '%s\n' "$body" | grep -Fc 'private_socket_node(path)')" -ge 2 &&
        printf '%s\n' "$body" | grep -F 'kMaxAdmissionAttempts' >/dev/null
}

retry_wait_source() {
    body=$(retry_wait_body "$1")
    printf '%s\n' "$body" | grep -F 'remaining >= std::chrono::milliseconds(1)' >/dev/null &&
        printf '%s\n' "$body" | grep -F 'poll(nullptr, 0, timeout_ms)' >/dev/null &&
        printf '%s\n' "$body" | grep -F 'ConnectRetryWaitResult::Timeout' >/dev/null
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

if ! retry_wait_source "$transport"; then
    echo 'FAIL: fresh-admission wait does not preserve floor-based absolute deadline' >&2
    exit 1
fi
echo 'ok - fresh-admission wait preserves floor-based absolute deadline'

retry_wait_mutant=$(mktemp "${TMPDIR:-/tmp}/p50localtransport-retry-wait-mutant.XXXXXX")
trap 'rm -f "$send_mutant" "$receive_mutant" "$connector_mutant" "$retry_wait_mutant"' EXIT HUP INT TERM
sed 's/remaining >= std::chrono::milliseconds(1) ? 1 : 0/1/' \
    "$transport" >"$retry_wait_mutant"
if retry_wait_source "$retry_wait_mutant"; then
    echo 'FAIL: fresh-admission wait rounding deletion mutant was accepted' >&2
    exit 1
fi
echo 'ok - fresh-admission wait rounding deletion mutant is rejected'

pending_retry_body() {
    connector_body "$1" | sed -n '/connect_error == EAGAIN/,/continue;/p'
}

if ! pending_retry_body "$transport" | grep -F '::close(fd);' >/dev/null ||
   ! pending_retry_body "$transport" | grep -F 'wait_for_connect_retry(deadline)' >/dev/null; then
    echo 'FAIL: EAGAIN/EINTR path is missing immediate close or bounded fresh retry wait' >&2
    exit 1
fi
echo 'ok - EAGAIN/EINTR path closes before fresh admission retry'

sed '/::close(fd);/d' "$transport" >"$connector_mutant"
if pending_retry_body "$connector_mutant" | grep -F '::close(fd);' >/dev/null; then
    echo 'FAIL: EAGAIN fresh-close deletion mutant was accepted' >&2
    exit 1
fi
echo 'ok - EAGAIN fresh-close deletion mutant is rejected'

sed 's/!private_parent(path) || !private_socket_node(path)/true/g' "$transport" >"$connector_mutant"
if bounded_connector_source "$connector_mutant"; then
    echo 'FAIL: fresh-attempt endpoint revalidation deletion mutant was accepted' >&2
    exit 1
fi
echo 'ok - fresh-attempt endpoint revalidation deletion mutant is rejected'

sed '/const int connect_result = connect_once(/a\        const int forbidden_retry = connect_once(fd, reinterpret_cast<const sockaddr*>(&address), address_length);' \
    "$transport" >"$connector_mutant"
if bounded_connector_source "$connector_mutant"; then
    echo 'FAIL: same-descriptor connect retry mutant was accepted' >&2
    exit 1
fi
echo 'ok - same-descriptor connect retry mutant is rejected'

# Compile and execute behavioral mutants.  These are intentionally stronger
# than nearby-string predicates: the test-only EINTR hook must still reach a
# fresh successful attempt, and the real backlog must still wait for EAGAIN
# admission rather than fail immediately.  The production object never sees
# this hook because it is compiled only with ICECC_P50_LOCAL_TRANSPORT_TEST_HOOKS.
cxx=${ICECC_TEST_CXX:-g++}
run_expected_mutant_failure() {
    mutant_source=$1
    label=$2
    mutant_binary=$(mktemp "${TMPDIR:-/tmp}/p50localtransport-${label}.XXXXXX")
    mutant_log=$(mktemp "${TMPDIR:-/tmp}/p50localtransport-${label}-log.XXXXXX")
    trap 'rm -f "$mutant_binary" "$mutant_log"' HUP INT TERM
    if ! "$cxx" -std=c++20 -Wall -Wextra -Werror -pthread -I"$src" \
        -DICECC_P50_LOCAL_TRANSPORT_TEST_HOOKS -I"$src/cache" \
        "$src/unittests/p50_local_transport_test.cpp" -x c++ "$mutant_source" \
        -o "$mutant_binary" >"$mutant_log" 2>&1; then
        echo "FAIL: $label mutant did not compile" >&2
        cat "$mutant_log" >&2
        return 1
    fi
    if "$mutant_binary" >"$mutant_log" 2>&1; then
        echo "FAIL: $label mutant passed focused runtime" >&2
        cat "$mutant_log" >&2
        return 1
    fi
    rm -f "$mutant_binary" "$mutant_log"
    return 0
}

eintr_mutant=$(mktemp "${TMPDIR:-/tmp}/p50localtransport-eintr-mutant.XXXXXX")
sed 's/ || connect_error == EINTR//g' "$transport" >"$eintr_mutant"
if ! run_expected_mutant_failure "$eintr_mutant" eintr; then
    exit 1
fi
rm -f "$eintr_mutant"
echo 'ok - executable EINTR deletion mutant is rejected'

eagain_mutant=$(mktemp "${TMPDIR:-/tmp}/p50localtransport-eagain-mutant.XXXXXX")
sed -e 's/connect_error == EAGAIN/false/g' \
    -e 's/connect_error == EWOULDBLOCK/false/g' \
    "$transport" >"$eagain_mutant"
if ! run_expected_mutant_failure "$eagain_mutant" eagain; then
    exit 1
fi
rm -f "$eagain_mutant"
echo 'ok - executable EAGAIN/EWOULDBLOCK deletion mutant is rejected'

# A bounded read/write operation must use per-call MSG_DONTWAIT and never
# toggle the shared open-file-description status flags.  The connector above
# is the sole exception: it owns a newly-created descriptor until return and
# explicitly restores that descriptor before handing it to Connection.  The
# local transport and FD handoff both use the one shared absolute-deadline poll
# helper in the public transport header.  Readable bytes win over a concurrent
# stream hangup; hard poll errors and write-side hangup remain terminal.
grep -F 'MSG_DONTWAIT' "$transport" >/dev/null
grep -F 'detail::wait_for_io' "$transport" >/dev/null
grep -F 'DeadlinePollResult' "$poll_helper" >/dev/null
grep -F '(POLLERR | POLLNVAL)' "$poll_helper" >/dev/null
grep -F '(events & POLLIN)' "$poll_helper" >/dev/null
grep -F '(descriptor.revents & POLLHUP)' "$poll_helper" >/dev/null
grep -F '(descriptor.revents & events)' "$poll_helper" >/dev/null
if grep -F '(us + 999)' "$transport" "$poll_helper" >/dev/null; then
    echo 'FAIL: shared deadline helper still rounds sub-millisecond waits up' >&2
    exit 1
fi
echo 'ok - bounded transport preserves shared flags and drains readable EOF state'

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
    if nm -C "$1" | grep -E 'listen_unix_with_test_hook|ListenPostBindTestHook|connect_unix_until_with_test_hook|ConnectAttemptTestHook|connect_attempt_test_hook|getenv|usleep' >/dev/null; then
        return 1
    fi
    if strings "$1" | grep -E 'ICECC_P50_LOCAL_TRANSPORT_TEST_HOOKS|ICECC_TEST_LOCAL_TRANSPORT|listen_unix_with_test_hook|connect_unix_until_with_test_hook|ConnectAttemptTestHook|connect_attempt_test_hook' >/dev/null; then
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
