#!/bin/sh
# Fast deterministic scheduler-integration tests for `make check` (~35s).
#
# Run 1 -- big farm: 600 jobs, one submitter stops reading for 8s while a
# second stays healthy.  Asserts the BP-1 admission invariant with direct
# per-submitter attribution from the scheduler's own log: dispatch to the
# non-reading submitter stops at its credit while the healthy one keeps
# receiving assignments and replies.
#
# Runs 2 and 3 -- small farms (8 and 16 slots): same freeze, but the fake
# compile server advertises only that many slots, so the scheduler must
# clamp the credit to slots-1 -- the case where a full credit of 32 could
# otherwise reserve the entire farm before a second submitter is served.
#
# The long transient/stall/deadline modes remain manual (see schedbp.cpp).
dir=$(dirname "$0")
"$dir/schedbp" "$dir/../scheduler/icecc-scheduler" "$dir/sndbuf_shim.so" 600 8 gate || exit 1
"$dir/schedbp" "$dir/../scheduler/icecc-scheduler" "$dir/sndbuf_shim.so" 300 6 gate 8 || exit 1
exec "$dir/schedbp" "$dir/../scheduler/icecc-scheduler" "$dir/sndbuf_shim.so" 300 6 gate 16
