#!/bin/sh
# G4 scheduler-session LOGIN_ATTEMPT lifecycle regression (issue #4).
#
# Drives a real iceccd against a fake scheduler that accepts Login but
# withholds ConfCS, then a replacement scheduler that activates.  Asserts the
# committed cea7c5f behavior: a GetCS arriving before ConfCS is held (never
# forwarded on the pending channel), schedulerless local work survives the
# attempt, the first ConfCS commits exactly one generation, active-session
# loss cleans up exactly once, and bounded shutdown exits cleanly.
#
# RED at a7eb908 (GetCS observed before ConfCS); GREEN at cea7c5f.
src_dir=$(dirname "$0")
build_dir=${ICECC_TEST_BUILDDIR:-$src_dir}
top_build_dir=${ICECC_TEST_TOP_BUILDDIR:-$build_dir/..}
exec "$build_dir/daemonlogin" "$top_build_dir/daemon/iceccd"
