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
# Pre-existing gap, unrelated to the d23d9c5d strict-tail/identity-binding
# work: the S2 cache-handoff retention block Daemon::scheduler_use_cs
# gained in an earlier round ("S2: validate and retain the assignment-
# bound cache-endpoint handoff...") reads msg->assignmentEpoch() a third
# time, to stamp Client::CacheHandoff with the identity a later-present
# cache triple is bound to.  This anchor was never updated to match --
# confirmed present and already failing at d23d9c5d, before any commit in
# this stack -- so it is corrected here rather than left red under a "full
# suite must stay green" requirement it predates.  BigOracle's 5th-gap fix
# (a REAL pre-existing product bug, see p50cacheadvertisement-source.sh)
# dropped this count from 3 to 2: the local-rewrite relay projection no
# longer calls msg->assignmentEpoch() explicitly -- it copies *msg
# wholesale, so that field (and every other) is preserved implicitly by
# the copy constructor rather than named at this call site.  The two
# remaining explicit reads are the remote-worker relay projection and the
# cache-handoff retention read; the local branch's identity preservation
# is proved behaviorally instead, by
# unittests/cachehandoffdaemon.cpp's usecs_matches_except_host_and_cache
# row on Client A, and at the source level by
# p50cacheadvertisement-source.sh's anchor on the copy-construction text
# itself.
require_count 8 'msg->assignmentEpoch()' daemon/main.cpp \
    'submitter daemon preserves assignment identity in relay, cache-handoff, and terminal paths'
require_count 2 'usecs->applyAssignmentTo(&job)' client/remote.cpp \
    'client remote/local-via-daemon paths copy the production UseCS identity'
require_count 1 'crmsg->compileIdentityMatches(job)' client/remote.cpp \
    'client validates assignment and compile identity on the real result path'
require_count 1 'm->assignmentEpoch() != j->assignmentEpoch()' scheduler/scheduler.cpp \
    'scheduler validates assignment identity on the real terminal path'
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
