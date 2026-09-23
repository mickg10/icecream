#!/bin/sh
# Deletion-sensitive ownership/lifecycle source gate.
set -eu

root=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
lifecycle_h=$root/cache/p50_input_lifecycle.h
lifecycle_cpp=$root/cache/p50_input_lifecycle.cpp
control=$root/cache/p50_control_operation.h
control_cpp=$root/cache/p50_control_operation.cpp
service=$root/cache/p50_cache_service.cpp
adapter=$root/cache/p50_daemon_sidecar_adapter.cpp
daemon=$root/daemon/main.cpp
test_file=$root/unittests/p50_input_lifecycle_test.cpp
doc=$root/cache/P50_PROTOCOL.md

gate() {
    grep -Fq 'InputLeaseOwner' "$lifecycle_h" &&
        grep -Fq 'CancelAttempt' "$lifecycle_h" &&
        grep -Fq 'CloseAcceptedJob' "$lifecycle_h" &&
        grep -Fq 'CancelJob' "$lifecycle_h" &&
        grep -Fq 'prepare_route_commit' "$lifecycle_cpp" &&
        grep -Fq 'ObservePrepared' "$lifecycle_cpp" &&
        grep -Fq 'CancelOrExpire' "$lifecycle_cpp" &&
        grep -Fq 'SelectCommit' "$lifecycle_cpp" &&
        grep -Fq 'CommitDurable' "$lifecycle_cpp" &&
        grep -Fq 'SuppressDeliveryAfterCommit' "$lifecycle_cpp" &&
        grep -Fq 'CancelledNoDurability' "$lifecycle_cpp" &&
        grep -Fq 'CommitWon' "$lifecycle_cpp" &&
        grep -Fq 'retired_owners' "$lifecycle_cpp" &&
        grep -Fq 'ConflictingReplay' "$lifecycle_cpp" &&
        grep -Fq 'lease.attempt_cancelled' "$lifecycle_cpp" &&
        grep -Fq 'lease.job_closed' "$lifecycle_cpp"
}

gate
grep -Fq 'kControlOperationVersionV1 = 1' "$control"
grep -Fq 'kControlOperationVersionV2 = 2' "$control"
grep -Fq 'kLegacyInputFdAttachmentOperationBytes = 56' "$control"
grep -Fq 'kInputLifecycleOperationBytes = 88' "$control"
grep -Fq 'wire.begin() + 84' "$control_cpp"
grep -Fq 'input_lifecycle_.prepare_route_commit' "$service"
grep -Fq 'owner_limits.max_retained_input_records' "$service"
grep -Fq 'input_lifecycle_.observe_route_commit' "$service"
grep -Fq 'input_lifecycle_.finish_attachment' "$service"
grep -Fq 'input_lifecycle_.finish_apply' "$service"
grep -Fq 'pending_input_lifecycle_' "$adapter"
grep -Fq 'position->identity.attempt < attempt_' "$adapter"
grep -Fq 'retire_input_lifecycle_relationship' "$adapter"
grep -Fq 'outer_request_shutdown' "$adapter"
grep -Fq 'pending_advertisement_update_' "$adapter"
grep -Fq 'InputLifecycleOperationExhausted' "$adapter"
grep -Fq 'adapter.advertisement_snapshot().absent()' \
    "$root/unittests/p50_daemon_sidecar_adapter_test.cpp"
grep -Fq 'input.attempt_id == job->assignmentNonce()' "$daemon"
grep -Fq 'input.request_id == job->assignmentNonce()' "$daemon"
grep -Fq 'InputLifecycleAction::CancelAttempt' "$daemon"
grep -Fq 'p50_input_lease_state' "$daemon"
grep -Fq 'close-before-commit' "$test_file"
grep -Fq 'retired assignment owner was resurrected' "$test_file"
grep -Fq 'owner capacity was not enforced before publication' "$test_file"
grep -Fq 'cache key remains exactly' "$doc"

if grep -E -n 'struct InputRecordKey[^}]*(attempt|assignment)' \
    "$lifecycle_h" "$control" >/dev/null; then
    echo 'FAIL: assignment identity leaked into cache InputRecordKey' >&2
    exit 1
fi

mutant_dir=$(mktemp -d "${TMPDIR:-/tmp}/p50inputlifecycle-source.XXXXXX")
trap 'rm -rf "$mutant_dir"' EXIT HUP INT TERM
for needle in \
    'retired_owners' \
    'ConflictingReplay' \
    'lease.attempt_cancelled' \
    'lease.job_closed' \
    'ObservePrepared' \
    'CancelOrExpire' \
    'SelectCommit' \
    'CommitDurable' \
    'SuppressDeliveryAfterCommit' \
    'CancelledNoDurability' \
    'CommitWon' \
    'prepare_route_commit'; do
    mutant=$mutant_dir/lifecycle.cpp
    awk -v pattern="$needle" 'index($0, pattern) == 0' "$lifecycle_cpp" >"$mutant"
    if grep -Fq 'InputLeaseOwner' "$lifecycle_h" &&
       grep -Fq 'CancelAttempt' "$lifecycle_h" &&
       grep -Fq 'CloseAcceptedJob' "$lifecycle_h" &&
       grep -Fq 'CancelJob' "$lifecycle_h" &&
       grep -Fq 'prepare_route_commit' "$mutant" &&
       grep -Fq 'retired_owners' "$mutant" &&
       grep -Fq 'ConflictingReplay' "$mutant" &&
       grep -Fq 'lease.attempt_cancelled' "$mutant" &&
       grep -Fq 'lease.job_closed' "$mutant" &&
       grep -Fq 'ObservePrepared' "$mutant" &&
       grep -Fq 'CancelOrExpire' "$mutant" &&
       grep -Fq 'SelectCommit' "$mutant" &&
       grep -Fq 'CommitDurable' "$mutant" &&
       grep -Fq 'SuppressDeliveryAfterCommit' "$mutant" &&
       grep -Fq 'CancelledNoDurability' "$mutant" &&
       grep -Fq 'CommitWon' "$mutant"; then
        echo "FAIL: lifecycle deletion mutant survived: $needle" >&2
        exit 1
    fi
done

echo 'PASS: p50 input lifecycle source/deletion gates'
