#!/bin/sh
set -eu
src="${ICECC_TEST_TOP_SRCDIR:-$(pwd)/..}/cache/p50_sidecar_lifecycle.cpp"
hdr="${ICECC_TEST_TOP_SRCDIR:-$(pwd)/..}/cache/p50_sidecar_lifecycle.h"
test_src="${ICECC_TEST_TOP_SRCDIR:-$(pwd)/..}/unittests/p50_sidecar_lifecycle_test.cpp"
mutants="${ICECC_TEST_TOP_SRCDIR:-$(pwd)/..}/unittests/p50sidecarlifecycle-mutants.sh"
test -s "$src" && test -s "$hdr"
test -s "$test_src" && test -x "$mutants"
grep -q 'SidecarLifecycle::begin' "$src"
grep -q 'SidecarLifecycle::advance' "$src"
grep -q 'CentralChildReaperRegistry::observe_child_reaped' "$src"
grep -q 'GroupObservation::Gone' "$src"
grep -q 'LifecycleAction::Withdraw' "$src"
grep -q 'FailedClosed' "$src"
grep -q 'teardown_deadline_' "$src"
grep -q 'listener_device' "$src"
grep -q 'kScanQuota' "$src"
grep -q 'kMaximumOwners' "$hdr"
grep -q 'owners.size() >= SharedState::kMaximumOwners' "$src"
grep -q 'ReapMailbox' "$src"
grep -q 'KillDomainLease' "$hdr"
grep -q 'group_domain_' "$src"
grep -q 'RejectingKillDomainVerifier' "$hdr"
grep -q 'kill_domain_verifier_->capture' "$src"
grep -q 'lstat(lease.socket_path' "$src"
grep -Fq 'if (!group_proof_required_)' "$src"
grep -Fq 'observation.group != GroupObservation::Gone' "$src"
grep -Fq 'observation.observed_pgid != process_group_' "$src"
grep -Fq 'group_domain_->matches(child_pid_, process_group_)' "$src"
grep -Fq 'observation.group_domain == *group_domain_' "$src"
grep -q 'PermissiveKillDomainVerifier' "$test_src"
grep -q 'fabricated lease identity' "$test_src"
grep -Fq 'group-proof-required' "$mutants"
grep -Fq 'if (false)' "$mutants"
if grep -Eq 'bool register_owner\([^;]*SidecarLifecycle' "$hdr"; then
    echo 'discarding compatibility registration overload remains' >&2
    exit 1
fi
if grep -Eq 'SidecarLifecycle[[:space:]]*\*' "$src" "$hdr"; then
    echo 'lifecycle registry contains a raw owner pointer' >&2
    exit 1
fi
if grep -Eq '(^|[^[:alnum:]_])(waitpid|poll|select|sleep)[[:space:]]*\(' "$src"; then
    echo 'lifecycle reducer contains a blocking/event-loop primitive' >&2
    exit 1
fi
if grep -q 'launch_and_wait\|reap_blocking' "$src"; then
    echo 'lifecycle reducer contains synchronous supervisor helpers' >&2
    exit 1
fi
