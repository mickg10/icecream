#!/bin/sh
# Deletion-sensitive production-leg audit. Behavioral tests exercise each
# result; these anchors ensure they remain attached to production call sites.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}

require_count() {
    expected=$1
    pattern=$2
    file=$3
    label=$4
    actual=$(grep -F -c "$pattern" "$src/$file" || true)
    if [ "$actual" -ne "$expected" ]; then
        echo "FAIL: $label (expected $expected production anchor(s), found $actual)" >&2
        exit 1
    fi
    echo "ok - $label"
}

require_count 1 'job->dispatchMatchedJobId(), job->assignmentEpoch(),' \
    scheduler/scheduler.cpp 'scheduler stamps authorized identity into UseCS'
require_count 2 'msg->assignmentEpoch()' daemon/main.cpp \
    'submitter daemon preserves identity in both scalar relay projections'
require_count 2 'usecs->applyAssignmentTo(&job)' client/remote.cpp \
    'client remote/local-via-daemon paths copy the production UseCS identity'
require_count 1 'record.key.epoch == job.assignmentEpoch()' daemon/main.cpp \
    'fulfillment admission compares the complete epoch'
require_count 1 'record.key.nonce == job.assignmentNonce()' daemon/main.cpp \
    'fulfillment admission compares the complete nonce'
require_count 1 'if (wire_id == 0)' daemon/main.cpp \
    'remote claim authority rejects the absent wire identity before admission'
require_count 1 'export ICECC_TEST_ASSIGNMENT_FENCE_MODE=strict-nonce' \
    unittests/p50assignment-remote.sh \
    'strict production client row remains selected independently'

echo 'PASS: all assignment-identity production anchors remain connected'
