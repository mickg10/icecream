#!/bin/sh
# Deletion-sensitive source gate for explicit scheduler profile selection.
set -eu
src=${ICECC_TEST_TOP_SRCDIR:?}
comm="$src/services/comm.h"
scheduler="$src/scheduler/scheduler.cpp"
test="$src/unittests/p50_compile_binding_test.cpp"

grep -F 'ICECC_P50_PROFILE' "$comm" >/dev/null
grep -F 'P50CacheProfileRequest::ZSTD_TU' "$comm" >/dev/null
grep -F 'P50CacheProfileRequest::ZSTD_ROUTE' "$comm" >/dev/null
grep -F 'P50CacheProfileRequest::Unsupported' "$comm" >/dev/null
grep -F 'p50_select_cache_profile' "$comm" "$scheduler" "$test" >/dev/null
grep -F 'p50_cache_profile_request_from_env' "$scheduler" "$test" >/dev/null
grep -F 'unavailable requests remain absent' "$scheduler" >/dev/null
grep -F 'UNSUPPORTED_PROFILE' "$test" >/dev/null

# Selection is a single exact assignment-tail value.  The source must retain
# the capability mask as the input and must not introduce cohort terminology.
grep -F 'const uint32_t selected_mask' "$scheduler" >/dev/null
grep -F 'cache_advertisement_is_valid_present(port, protocol, selected_mask)' \
    "$scheduler" >/dev/null
if grep -nE 'ZSTD_COHORT|CACHE_PROFILE_COHORT' "$comm" "$scheduler" "$test" >/dev/null 2>&1; then
    echo 'FAIL: cohort terminology entered the source-profile selector' >&2
    exit 1
fi

echo 'PASS: explicit TU/ROUTE profile selection source gates hold'
