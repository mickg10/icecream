#!/bin/sh
# Route transfer-lock serialization under concurrency (K2-0 regression): N
# concurrent same-route source transfers through the production
# transfer_source_on_owner path against a real F, at K=1 (the fix) and at
# K=2 (ICECC_P50_ROUTE_PIPELINE_DEPTH=2, test-unsafe):
# test_k1_concurrent_same_route_transfers_serialize_on_route_lock,
# test_k2_concurrent_same_route_transfers_admit_two_deep.
set -u
build_dir=${ICECC_TEST_BUILDDIR:?}
exec "$build_dir/p50cacheservice" --route-pipeline-concurrency
