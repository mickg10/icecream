#!/bin/sh
# F session capacity: two C sidecars overflow one F's live-session bound, and
# a reopen meets BUSY (test_f_live_session_overflow_from_two_c_runtimes,
# test_reopen_busy_waits_only_for_the_same_f_store).
set -u
build_dir=${ICECC_TEST_BUILDDIR:?}
exec "$build_dir/p50cacheservice" --f-live-capacity
