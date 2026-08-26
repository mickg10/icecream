#!/bin/sh
set -eu

if test "${ICECC_TEST_SOURCE_ARM_LIVE:-}" != 1; then
    echo 'SKIP: set ICECC_TEST_SOURCE_ARM_LIVE=1 in an isolated root container' >&2
    exit 77
fi

build=${ICECC_TEST_BUILDDIR:?}
test -x "$build/../daemon/iceccd"
test -x "$build/../cache/icecc-cache-service"
exec "$build/p50sourcearm-live" "$build/../daemon/iceccd" \
    "$build/../cache/icecc-cache-service"
