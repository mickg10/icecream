#!/bin/sh
set -eu

test -n "${ICECC_TEST_TOP_SRCDIR:-}" || exit 2
src="$ICECC_TEST_TOP_SRCDIR"
sender="$ICECC_TEST_TOP_SRCDIR/client/p50_zstd_sender.cpp"
header="$ICECC_TEST_TOP_SRCDIR/client/p50_zstd_sender.h"
test -f "$sender" -a -f "$header"

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

# Compile and run a deletion mutant which removes the second attempt.  The
# focused test's attempts==2 assertion must redden it, proving the retry gate
# is behavioral rather than a source-only grep.
work=$(mktemp -d "${TMPDIR:-/tmp}/p50-zstd-sender-mutant.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
mutant="$work/p50_zstd_sender.cpp"
sed 's/attempt <= 2/attempt <= 1/' "$sender" >"$mutant"
test "$(grep -F -c 'attempt <= 2' "$sender")" -eq 1
test "$(grep -F -c 'attempt <= 1' "$mutant")" -eq 1
cxx=${ICECC_TEST_CXX:-${CXX:-c++}}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:--std=c++23}
top_build=${ICECC_TEST_TOP_BUILDDIR:-$(CDPATH= cd -- "$src/.." && pwd)}
"$cxx" "$standard" -O1 -g -pthread \
    ${ICECC_TEST_CPPFLAGS:-} ${ICECC_TEST_BOOST_CPPFLAGS:-} \
    -I"$src" -I"$src/client" -I"$src/cache" -I"$src/services" \
    "$src/unittests/p50_zstd_sender_test.cpp" "$mutant" \
    "$top_build/cache/libp50endpoint.a" \
    "$top_build/cache/libp50adoptedoutcomewriter.a" \
    "$top_build/cache/libprotocol50.a" \
    "$top_build/services/.libs/libicecc.a" \
    ${ICECC_TEST_LDFLAGS:-} ${ICECC_TEST_LIBZSTD_LIBS:--lzstd} \
    ${ICECC_TEST_XXHASH_LIBS:--lxxhash} -llzo2 -ldl -o "$work/mutant"
if "$work/mutant" >/dev/null 2>&1; then
    echo 'FAIL: retry-deletion mutant survived' >&2
    exit 1
fi
echo 'ok - deleting the exact retry reddens the focused test'
