#!/bin/sh
# G4 count>1 batch-ledger complete gate (issue #4).  Every named case runs in a
# fresh daemon/session; any failing case fails the suite.  RED at 61e2b73 (the
# discriminating gates + clean-End over-settlement + eof-0 double-cancel), GREEN
# with the P_batch ledger correction.
dir=$(dirname "$0")
iceccd="$dir/../daemon/iceccd"
rc=0
for c in self-control \
         client-done-filter late-usecs teardown-clean local-capacity batch-nocs \
         teardown-eof-0 teardown-eof-1 teardown-eof-2 teardown-eof-3 teardown-early-1 \
         teardown-local-queued teardown-local-active teardown-normal-local \
         scalar-count0 scalar-count1-remote scalar-count1-local \
         mixed-LRR mixed-RLR mixed-RNR nocs-dedup-excess \
         transition-1 transition-2 transition-3 \
         invalid-jobdone local-lifetime compile-started \
         audit-jobbegin audit-delivery-fail audit-sched-loss; do
    if ! "$dir/batchledger" "$iceccd" "$c"; then
        echo "batchledger case FAILED: $c" >&2
        rc=1
    fi
done
exit $rc
