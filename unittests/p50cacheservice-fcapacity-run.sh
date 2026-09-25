#!/bin/sh
# Expected to fail (XFAIL_TESTS) at its last check while an overflowed C
# sidecar quarantines its F relationship for good; any other failure is a
# hard error.
set -u
build_dir=${ICECC_TEST_BUILDDIR:?}
status=0
output=$("$build_dir/p50cacheservice" --f-live-capacity 2>&1) || status=$?
printf '%s\n' "$output"
test "$status" -eq 0 && exit 0
case $output in
*'p50cacheservice: second_later.code == local::SourceTransferResultCode::Committed'*)
    exit 1 ;;
esac
exit 99
