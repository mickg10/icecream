#!/bin/sh
set -eu

test -n "${ICECC_TEST_TOP_SRCDIR:-}" || exit 2
src="$ICECC_TEST_TOP_SRCDIR"
sender="$ICECC_TEST_TOP_SRCDIR/client/p50_zstd_sender.cpp"
header="$ICECC_TEST_TOP_SRCDIR/client/p50_zstd_sender.h"
test -f "$sender" -a -f "$header"
grep -F '#include "services/p50_cache_profile_mask.h"' "$sender" >/dev/null

# The sender must remain a client-only seam.  These forbidden dependencies
# would silently widen the lane into daemon/service or FileChunk behavior.
if grep -nE 'client/remote\.cpp|services/(job|comm)\.|daemon/|FileChunk|p50_cache_service' \
    "$sender" "$header"; then
    echo "forbidden sender dependency" >&2
    exit 1
fi

# A deletion mutant of the exact-retry branch must be observable in source.
grep -q 'attempt <= 2' "$sender"
grep -q 'RetryExhausted' "$sender"
grep -q 'PreparedTuHandle prepared' "$sender"
grep -q 'completed_for(request' "$sender"

# Compile and run a deletion mutant which removes the second attempt.  The
# focused test's attempts==2 assertion must redden it, proving the retry gate
# is behavioral rather than a source-only grep.
work=$(mktemp -d "${TMPDIR:-/tmp}/p50-zstd-sender-mutant.XXXXXX")
keep_work=1
cleanup() {
    status=$?
    if test "$status" -eq 0 && test "$keep_work" -eq 0; then
        rm -rf "$work"
    else
        echo "sender source-gate artifacts retained: $work" >&2
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
top_build=${ICECC_TEST_TOP_BUILDDIR:-$(CDPATH= cd -- "$src/.." && pwd)}
baseline="$top_build/unittests/p50zstdsender"
test -x "$baseline"
timeout 35s "$baseline" --disconnected-retry >"$work/retry-baseline.log" 2>&1
grep -F 'P50_SENDER_DISCONNECTED_RETRY_SELECTOR PASS' \
    "$work/retry-baseline.log" >/dev/null
timeout 35s "$baseline" --route-ledger-replay >"$work/ledger-baseline.log" 2>&1
grep -F 'P51_SENDER_ROUTE_LEDGER_REPLAY_SELECTOR PASS' \
    "$work/ledger-baseline.log" >/dev/null
mutant="$work/p50_zstd_sender.cpp"
sed 's/attempt <= 2/attempt <= 1/' "$sender" >"$mutant"
test "$(grep -F -c 'attempt <= 2' "$sender")" -eq 1
test "$(grep -F -c 'attempt <= 1' "$mutant")" -eq 1
cxx=${ICECC_TEST_CXX:-${CXX:-c++}}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:--std=c++23}
"$cxx" "$standard" -O1 -g -pthread \
    ${ICECC_TEST_CPPFLAGS:-} ${ICECC_TEST_BOOST_CPPFLAGS:-} \
    -I"$src" -I"$src/client" -I"$src/cache" -I"$src/services" \
    "$src/unittests/p50_zstd_sender_test.cpp" "$mutant" \
    "$top_build/cache/libp50endpoint.a" \
    "$top_build/cache/libp50adoptedoutcomewriter.a" \
    "$top_build/cache/libprotocol50.a" \
    "$top_build/services/.libs/libicecc.a" \
    ${ICECC_TEST_LDFLAGS:-} ${ICECC_TEST_LIBZSTD_LIBS:--lzstd} \
    ${ICECC_TEST_XXHASH_LIBS:--lxxhash} ${ICECC_TEST_LIBCAP_NG_LIBS:-} \
    -llzo2 -ldl -o "$work/mutant"
if timeout 35s "$work/mutant" --disconnected-retry >"$work/retry-mutant.log" 2>&1; then
    echo 'FAIL: retry-deletion mutant survived' >&2
    exit 1
else
    mutant_status=$?
fi
grep -F 'transfer.attempts == 2' "$work/retry-mutant.log" >/dev/null
case "$mutant_status" in
    124|137|143)
        echo "FAIL: retry mutant timed out (status $mutant_status)" >&2
        exit 1
        ;;
esac
echo 'ok - deleting the exact retry reddens the focused test'

# Removing the completed-request lookup must make the no-connection replay
# control fail: the mutant attempts the factory again and returns a retry
# failure instead of the cached committed witness.
ledger_mutant="$work/p50_zstd_sender_ledger.cpp"
sed 's/impl_->completed_for(request, \*source, raw_digest)/std::optional<ZstdSourceTransferResult>{}/' \
    "$sender" >"$ledger_mutant"
"$cxx" "$standard" -O1 -g -pthread \
    ${ICECC_TEST_CPPFLAGS:-} ${ICECC_TEST_BOOST_CPPFLAGS:-} \
    -I"$src" -I"$src/client" -I"$src/cache" -I"$src/services" \
    "$src/unittests/p50_zstd_sender_test.cpp" "$ledger_mutant" \
    "$top_build/cache/libp50endpoint.a" \
    "$top_build/cache/libp50adoptedoutcomewriter.a" \
    "$top_build/cache/libprotocol50.a" \
    "$top_build/services/.libs/libicecc.a" \
    ${ICECC_TEST_LDFLAGS:-} ${ICECC_TEST_LIBZSTD_LIBS:--lzstd} \
    ${ICECC_TEST_XXHASH_LIBS:--lxxhash} ${ICECC_TEST_LIBCAP_NG_LIBS:-} \
    -llzo2 -ldl -o "$work/ledger-mutant"
if timeout 35s "$work/ledger-mutant" --route-ledger-replay \
    >"$work/ledger-mutant.log" 2>&1; then
    echo 'FAIL: completed-request-ledger deletion mutant survived' >&2
    exit 1
else
    ledger_status=$?
fi
grep -F 'replay_result.status == ZstdSourceTransferStatus::Committed' \
    "$work/ledger-mutant.log" >/dev/null
case "$ledger_status" in
    124|137|143)
        echo "FAIL: ledger mutant timed out (status $ledger_status)" >&2
        exit 1
        ;;
esac
echo 'ok - deleting the completed-request lookup reddens the replay control'

keep_work=0
