#!/bin/sh
# G4 count>1 batch-ledger gates (issue #4).  Runs every named case in a fresh
# daemon/session; any failing case fails the suite.  RED at 61e2b73, GREEN with
# the P_batch ledger correction.
dir=$(dirname "$0")
iceccd="$dir/../daemon/iceccd"
rc=0
for c in self-control client-done-filter late-usecs teardown-clean local-capacity batch-nocs; do
    if ! "$dir/batchledger" "$iceccd" "$c"; then
        echo "batchledger case FAILED: $c" >&2
        rc=1
    fi
done
exit $rc
