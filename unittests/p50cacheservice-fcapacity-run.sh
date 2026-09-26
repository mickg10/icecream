#!/bin/sh
# Two C sidecars overflow one F's live-session bound (see
# test_f_live_session_overflow_from_two_c_runtimes).
set -u
build_dir=${ICECC_TEST_BUILDDIR:?}
exec "$build_dir/p50cacheservice" --f-live-capacity
