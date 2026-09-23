#!/bin/sh
# Deletion-sensitive source gate for the isolated reverse FD seam.
set -eu
src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}

count() {
    expected=$1; pattern=$2; file=$3; label=$4
    actual=$(grep -F -c "$pattern" "$src/$file" || true)
    if [ "$actual" -ne "$expected" ]; then
        echo "FAIL: $label (expected $expected, found $actual)" >&2
        exit 1
    fi
    echo "ok - $label"
}

at_least() {
    minimum=$1; pattern=$2; file=$3; label=$4
    actual=$(grep -F -c "$pattern" "$src/$file" || true)
    if [ "$actual" -lt "$minimum" ]; then
        echo "FAIL: $label (expected at least $minimum, found $actual)" >&2
        exit 1
    fi
    echo "ok - $label"
}

count 1 'kMaxReverseFdRemainingMs' unittests/support/p50_reverse_fd_retry.h 'bounded duration constant'
count 1 'DeliveryId delivery_id = 0' unittests/support/p50_reverse_fd_retry.h 'explicit DeliveryId field'
count 1 'DeliveryToken token = 0' unittests/support/p50_reverse_fd_retry.h 'explicit token field'
at_least 1 'original_deadline' unittests/support/p50_reverse_fd_retry.h 'original absolute deadline API'
at_least 1 'F_DUPFD_CLOEXEC' unittests/support/p50_reverse_fd_retry.cpp 'fresh CLOEXEC duplication'
at_least 1 'F_SEAL_WRITE' unittests/support/p50_reverse_fd_retry.cpp 'sealed master write protection'
at_least 1 'remaining_ms' unittests/support/p50_reverse_fd_retry.cpp 'wire carries remaining duration'
at_least 1 'high_water_' unittests/support/p50_reverse_fd_retry.h 'service high-water ledger'
at_least 1 'exact_accepted' unittests/support/p50_reverse_fd_retry.cpp 'exact acceptance fingerprint'
at_least 1 'ReverseFdReceiverState::ToCompile' unittests/support/p50_reverse_fd_retry.cpp 'pre-ACK TOCOMPILE transition'
at_least 1 'ReverseFdDecision::ExactReplay' unittests/support/p50_reverse_fd_retry.cpp 'replay ACK decision'
at_least 1 'transition_count_' unittests/support/p50_reverse_fd_retry.h 'single transition witness'
at_least 1 'fork_count_' unittests/support/p50_reverse_fd_retry.h 'single fork witness'
count 1 'void ReverseFdOwner::cancel' unittests/support/p50_reverse_fd_retry.cpp 'owner cancellation closure'
count 1 'void ReverseFdReceiverLedger::cancel' unittests/support/p50_reverse_fd_retry.cpp 'receiver cancellation closure'
count 1 'P50_PROTOCOL.md' cache/Makefile.am 'documentation distributed'
for registration in p50reversefdretry-mutants.sh p50_reverse_fd_retry_sanitize.sh; do
    count 2 "$registration" unittests/Makefile.am \
        "$registration appears once in TESTS and once in EXTRA_DIST"
done

if grep -E -n 'p50_reverse_fd_retry|ReverseFdOwner|ReverseFdReceiverLedger' \
        "$src/daemon" "$src/client" "$src/services" >/dev/null 2>&1; then
    echo 'FAIL: reverse FD seam leaked into daemon/client/services wiring' >&2
    exit 1
fi
echo 'ok - daemon/client/services remain untouched'
echo 'PASS: reverse sealed-FD source/deletion gates passed'
