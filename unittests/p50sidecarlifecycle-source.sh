#!/bin/sh
set -eu
src="${ICECC_TEST_TOP_SRCDIR:-$(pwd)/..}/cache/p50_sidecar_lifecycle.cpp"
hdr="${ICECC_TEST_TOP_SRCDIR:-$(pwd)/..}/cache/p50_sidecar_lifecycle.h"
test -s "$src" && test -s "$hdr"
grep -q 'SidecarLifecycle::begin' "$src"
grep -q 'SidecarLifecycle::advance' "$src"
grep -q 'CentralChildReaperRegistry::observe_child_reaped' "$src"
grep -q 'GroupObservation::Gone' "$src"
grep -q 'LifecycleAction::Withdraw' "$src"
grep -q 'FailedClosed' "$src"
grep -q 'teardown_deadline_' "$src"
grep -q 'listener_device' "$src"
grep -q 'kScanQuota' "$src"
grep -q 'ReapMailbox' "$src"
grep -q 'KillDomainLease' "$hdr"
grep -q 'group_domain_' "$src"
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
