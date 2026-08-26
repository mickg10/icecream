#!/bin/sh
set -eu

src=${ICECC_TEST_TOP_SRCDIR:?}
daemon="$src/daemon/main.cpp"
comm_h="$src/services/comm.h"
comm_cpp="$src/services/comm.cpp"
makefile="$src/unittests/Makefile.am"
runtime="$src/unittests/p50_source_arm_live_wait_test.cpp"
lease_header="$src/daemon/p50_source_arm_wait_lease.h"

for file in "$daemon" "$comm_h" "$comm_cpp" "$makefile" "$runtime" "$lease_header"; do
    test -f "$file"
done

check_contract() {
    candidate=$1
    runtime_candidate=${2:-$runtime}
    for needle in \
        'std::optional<P50SourceArmFields> p50_source_arm_fields' \
        'std::optional<icecc::p50::sidecar::ReadyLease> p50_source_f_lease' \
        'std::optional<P50SourceArmedMsg> p50_source_armed_ack' \
        'uint64_t p50_source_f_store_generation' \
        'ConnectionProvenance p50_source_arm_provenance' \
        'p50_source_deadline_msec' \
        'p50_source_compile_pending' \
        'authorize_source_arm_claim' \
        'bind_source_assignment_for_settlement' \
        'take_invalid_p50_source_arm_identity' \
        'P50SourceArmedFields::MaxSourceBudgetMsec' \
        'arm.selected_f_host != remote_name' \
        'arm.selected_f_ordinary_port != static_cast<uint32_t>(daemon_port)' \
        'arm.selected_f_cache_port != snapshot.endpoint_port' \
        '(snapshot.profile_mask & arm.cache_profile) == 0' \
        'client->status != Client::UNKNOWN' \
        'client->set_status(Client::UNKNOWN, cancel ? "finish_transfer_env: canceled" : "finish_transfer_env: done")' \
        'cache_adapter->supervisor()->current_lease()' \
        'p50_ready_lease_observation_equal' \
        '? (POLLIN | POLLHUP | POLLERR)' \
        'next_p50_source_deadline_msec()' \
        'const uint64_t source_deadline_msec = next_p50_source_deadline_msec();' \
        'if (expire_p50_source_waiters()) {' \
        'source_budget_msec' \
        'f_store_generation' \
        'source_budget_msec <= MaxSourceBudgetMsec' \
        'store_identity_file_guid_matches_client' \
        'p50_source_compile_pending' \
        'retained_source_owner' \
        'client->status == Client::WAITP50INPUT && *msg == Msg::CACHE_SESSION'; do
        grep -F -- "$needle" "$candidate" "$comm_h" "$comm_cpp" >/dev/null || return 1
    done

    # The replacement predicate must be the production invalidation seam, not
    # merely a helper exercised by a unit test.
    grep -F 'p50_wait_owner_replaced' "$candidate" >/dev/null || return 1

    grep -F 'wait_for_sidecar_child' "$runtime_candidate" >/dev/null || return 1
    grep -F 'SIGKILL' "$runtime_candidate" >/dev/null || return 1
    grep -F 'replacement_followup_armed->f_store_guid != old_f_guid' \
        "$runtime_candidate" >/dev/null || return 1
    grep -F 'store_identity_root_from_f_guid' "$runtime_candidate" >/dev/null || return 1
    grep -F 'p50sourcearm-live-run.sh' "$makefile" >/dev/null || return 1
    if grep -E '^TESTS .*p50sourcearm-live([[:space:]]|$)' "$makefile" >/dev/null; then
        return 1
    fi

    # WAIT must not be put back on the ignored-channel path.
    grep -F 'current_status == Client::WAITFORCHILD ||' "$candidate" >/dev/null || return 1
    grep -F 'current_status == Client::WAITINSTALL;' "$candidate" >/dev/null || return 1
    if grep -F 'current_status == Client::WAITP50INPUT;' "$candidate" >/dev/null; then
        return 1
    fi

    # The source-arm handler is ordinary-link admission only.  The positive
    # CACHE_SESSION/private/SCM_RIGHTS/InputReady path must not be smuggled
    # into this successor.
    handler=$(sed -n '/bool Daemon::handle_p50_source_arm/,/^bool Daemon::handle_activity/p' "$candidate")
    if printf '%s\n' "$handler" | grep -E 'attach_input|CACHE_SESSION|SCM_RIGHTS|P50InputReady|accept_ready|cacheHandoff|cache_eligible|release_fd' >/dev/null; then
        return 1
    fi
    return 0
}

check_contract "$daemon"

# Each deletion mutant must fail the same contract check, rather than merely
# proving that a string substitution happened.  This keeps the gate sensitive
# to the old ignored-fd and no-deadline implementations.
mutant=$(mktemp "${TMPDIR:-/tmp}/p50sourcearm-live-mutant.XXXXXX")
trap 'rm -f "$mutant" "${live_root_mutant-}"' EXIT HUP INT TERM

sed 's/? (POLLIN | POLLHUP | POLLERR)/? POLLIN/' "$daemon" >"$mutant"
if check_contract "$mutant"; then
    echo 'FAIL: WAIT POLLHUP/POLLERR deletion mutant survived' >&2
    exit 1
fi

# A live replacement that reuses the old F GUID/root is not a fresh sidecar
# incarnation.  Keep this deletion mutant tied to the actual SIGKILL witness,
# rather than merely checking that a comparison string exists in isolation.
live_root_mutant=$(mktemp "${TMPDIR:-/tmp}/p50sourcearm-live-root-mutant.XXXXXX")
sed 's/replacement_followup_armed->f_store_guid != old_f_guid/true/' \
    "$runtime" >"$live_root_mutant"
if check_contract "$daemon" "$live_root_mutant"; then
    echo 'FAIL: frozen live replacement F-root mutant survived' >&2
    exit 1
fi

sed 's/const uint64_t source_deadline_msec = next_p50_source_deadline_msec();/const uint64_t source_deadline_msec = 0;/' "$daemon" >"$mutant"
if check_contract "$mutant"; then
    echo 'FAIL: source poll-deadline cap deletion mutant survived' >&2
    exit 1
fi

sed 's/if (expire_p50_source_waiters()) {/if (false) {/g' "$daemon" >"$mutant"
if check_contract "$mutant"; then
    echo 'FAIL: silent-expiry sweep deletion mutant survived' >&2
    exit 1
fi

echo 'ok - live source-arm WAIT polling/deadline/lease/settlement deletion contract holds'
