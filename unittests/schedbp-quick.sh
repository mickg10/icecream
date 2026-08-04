#!/bin/sh
# Fast deterministic subset of the scheduler backpressure integration test
# for `make check`: 400 jobs, 5s transient clog (~15s total).  The full
# 4000-job transient and stall modes remain manual (see schedbp.cpp).
dir=$(dirname "$0")
exec "$dir/schedbp" "$dir/../scheduler/icecc-scheduler" "$dir/sndbuf_shim.so" 400 5
