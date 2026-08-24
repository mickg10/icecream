#!/bin/sh
# Deletion-sensitive source gate for the pure M3 attachment core.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}

count() {
    expected=$1
    pattern=$2
    file=$3
    label=$4
    actual=$(grep -F -c "$pattern" "$src/$file" || true)
    if [ "$actual" -ne "$expected" ]; then
        echo "FAIL: $label (expected $expected, found $actual)" >&2
        exit 1
    fi
    echo "ok - $label"
}

count 1 'std::unordered_map<InputRecordKey, Lifecycle' cache/p50_input_attachment.h \
    'canonical lifecycle is keyed by cache identity, never ATTEMPT_ID'
count 1 'request.attempt.store_generation' cache/p50_input_attachment.cpp \
    'request identity carries the store generation fence'
count 1 'return pending_ready_count() < max_pending_ready_;' \
    cache/p50_input_attachment.cpp 'pending ready table has a hard admission bound'
count 2 'state.ready_pending = false;' \
    cache/p50_input_attachment.cpp 'ACK retires the one ready event'
count 1 'state.current_attempt = new_owner.attempt_id;' \
    cache/p50_input_attachment.cpp 'replacement changes ownership only'
count 1 'InputCursor cursor = records_.attach(request.key);' \
    cache/p50_input_attachment.cpp 'attachment delegates exact bytes to InputRecordStore'

if grep -E -n 'p50_input_attachment|InputAttachmentCore|InputAttempt' \
        "$src/daemon/compiler_input.cpp" "$src/daemon/compiler_input.h" \
        "$src/cache/p50_endpoint.cpp" "$src/cache/p50_endpoint.h" \
        "$src/services/comm.cpp" "$src/services/comm.h" >/dev/null 2>&1; then
    echo 'FAIL: attachment core leaked into active transport/service seams' >&2
    exit 1
fi
echo 'ok - active endpoint/service/compiler seams remain untouched'

echo 'PASS: p50 attachment core source/deletion gates passed'
