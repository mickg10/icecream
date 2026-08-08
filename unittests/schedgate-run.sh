#!/bin/sh
# Scheduler-level gate for the deferrable transport (see schedgate.cpp).
# The clog is manufactured with an LD_PRELOAD shim that shrinks the
# scheduler's socket buffers; that mechanism is Linux-specific, so other
# platforms skip (the product code under test is portable; this gate's
# instrument is not).
dir=$(dirname "$0")
[ "$(uname -s)" = Linux ] || exit 77
[ -f "$dir/sndbuf_shim.so" ] || exit 77
exec "$dir/schedgate" "$dir/../scheduler/icecc-scheduler" "$dir/sndbuf_shim.so"
