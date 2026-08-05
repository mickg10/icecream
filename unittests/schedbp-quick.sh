#!/bin/sh
# Fast deterministic scheduler-integration test for `make check` (~20s):
# 600 jobs, one submitter stops reading for 8s while a second stays healthy.
# Asserts the BP-1 admission invariant (dispatch to the non-reading submitter
# stops at its credit, the healthy one keeps being served, the scheduler
# stays responsive).  The long transient/stall/deadline modes remain manual
# (see schedbp.cpp).
dir=$(dirname "$0")
exec "$dir/schedbp" "$dir/../scheduler/icecc-scheduler" "$dir/sndbuf_shim.so" 600 8 gate
