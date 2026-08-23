#!/bin/sh
# S2 daemon-relay gate (BigOracle steer): a real iceccd must relay the
# validated cache-handoff triple to its local C client on the
# self-selected-F (127.0.0.1 rewrite) branch, not silently drop it.
src_dir=$(dirname "$0")
build_dir=${ICECC_TEST_BUILDDIR:-$src_dir}
top_build_dir=${ICECC_TEST_TOP_BUILDDIR:-$build_dir/..}
exec "$build_dir/cachehandoffdaemon" "$top_build_dir/daemon/iceccd"
