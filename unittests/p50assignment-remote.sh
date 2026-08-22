#!/bin/sh
# Separate all-P50 STRICT_NONCE row over the capability-aware real remote
# compile harness. The legacy row remains remoteice-quick.sh's default.
set -eu
export ICECC_TEST_ASSIGNMENT_FENCE_MODE=strict-nonce
exec "$(dirname -- "$0")/remoteice-quick.sh" "$@"
