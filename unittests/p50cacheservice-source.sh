#!/bin/sh
# Deletion-sensitive StoreIdentity/legacy READY source gate.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
impl="$src/cache/p50_cache_service.cpp"
header="$src/cache/p50_cache_service.h"
control="$src/cache/p50_control_operation.h"
control_cpp="$src/cache/p50_control_operation.cpp"
identity="$src/cache/p50_incarnation_identity.h"
wire="$src/services/p50_store_identity_wire.h"
doc="$src/cache/P50_PROTOCOL.md"
test_file="$src/unittests/p50cacheservice.cpp"
test_makefile="$src/unittests/Makefile.am"
product_makefile="$src/cache/Makefile.am"

require() { grep -F -- "$2" "$1" >/dev/null; }
for pair in \
    "$identity|StoreIdentityEntropyProvider" \
    "$wire|kStoreIdentityRoleMask" \
    "$wire|kStoreIdentityClientRole" \
    "$wire|kStoreIdentityFileRole" \
    "$identity|fresh_store_identity_root_with_provider" \
    "$identity|result > 0" \
    "$identity|root = {}" \
    "$impl|read_structured_launch" \
    "$impl|present != names.size()" \
    "$impl|format != \"2\"" \
    "$impl|have_c_store_guid" \
    "$impl|have_f_store_guid" \
    "$impl|structured_launch.c_store_guid" \
    "$impl|structured_launch.f_store_guid" \
    "$impl|kMaxControlWorkers = 64" \
    "$impl|SidecarRuntime::acquire_source_address" \
    "$impl|SidecarRuntime::acquire_source_incarnations" \
    "$impl|SidecarRuntime::acquire_source_credit" \
    "$impl|SidecarRuntime::release_source_admission" \
    "$impl|owner_preflight_source_endpoint" \
    "$impl|source_fd_size(" \
    "$impl|static_cast<uint64_t>(before.st_size) != reserved_size" \
    "$impl|std::make_shared<std::vector<uint8_t>>(" \
    "$impl|source_admission_changed_.wait_until" \
    "$impl|source_admission_changed_.notify_all()" \
    "$impl|open_arm_start + open_arm_timeout" \
    "$impl|kSourceConnectAttemptBudget" \
    "$impl|Service::createChannelRetryUntil(" \
    "$impl|open_arm_budget_ms=%lld" \
    "$impl|source_read_ns=%llu" \
    "$impl|#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS" \
    "$impl|endpoint_->request_cancel_for_test()" \
    "$impl|structured_launch.f_store_generation" \
    "$impl|store_identity_root_from_f_guid" \
    "$impl|c_guid != c_store_guid_for_root(root)" \
    "$impl|guid != f_store_guid_for_root(root)" \
    "$impl|fresh_store_identity_root(legacy_root)" \
    "$impl|runtime_config.f_store_guid = structured_launch.active" \
    "$impl|wait_p29_system_source_fingerprint_for(" \
    "$impl|cancel_p29_system_source_fingerprint();" \
    "$impl|kP29FingerprintReadyBudget" \
    "$impl|ICECC_CACHE_SERVICE_EXPECTED_DERIVATION_VERSION" \
    "$header|RuntimeConfig" \
    "$header|source_open_arm_timeout{5000}" \
    "$header|max_active_source_transfers = 4" \
    "$header|max_aggregate_source_raw_bytes = uint64_t{2} * 1024 * 1024 * 1024" \
    "$header|active_source_addresses_" \
    "$header|active_source_incarnations_" \
    "$header|active_source_count_" \
    "$header|active_source_raw_bytes_" \
    "$header|seed_route_endpoint_identity_for_test" \
    "$header|seed_route_relationship_for_test" \
    "$test_file|legacy_store_identity_launches" \
    "$test_file|test_runtime_store_identity_is_explicit_and_role_tagged" \
    "$test_file|test_source_open_arm_timeout_bounds" \
    "$test_file|test_route_endpoint_cap_refuses_before_f_open" \
    "$test_file|test_known_endpoint_relationship_cap_refuses_before_f_open" \
    "$test_file|test_source_connect_protocol_slice_retries_before_arm" \
    "$test_file|observation.first_connection_bytes == protocol_only" \
    "$test_file|test_source_connect_protocol_slices_share_one_outer_budget" \
    "$test_file|attempts.size() == 2" \
    "$test_file|test_stalled_f_arm_is_bounded_before_healthy_transfer" \
    "$test_file|healthy_result.code == local::SourceTransferResultCode::Committed" \
    "$test_file|test_parallel_distinct_f_matrix" \
    "$test_file|serve_source_transfers_on_persistent_f" \
    "$test_file|committed_rel_seq == wave" \
    "$test_file|committed_history_nonce ==" \
    "$test_file|--a05-global-gate-negative-control" \
    "$test_file|test_same_link_serialization_matrix" \
    "$test_file|test_same_link_serialization(CACHE_PROFILE_P29V1" \
    "$test_file|second_connected_while_first_held" \
    "$test_file|first_value.tu_seq == 0 && second_value.tu_seq == 1" \
    "$test_file|A03 ZSTD_ROUTE" \
    "$test_file|A04 ROUTE->P29V1" \
    "$test_file|test_expired_alias_cannot_release_held_incarnation" \
    "$test_file|final_still_waiting_after_expiry" \
    "$test_file|alias_connected_before_predecessor_release" \
    "$test_file|A07/A12 ZSTD_TU" \
    "$test_file|test_incarnation_change_waits_for_old_operation" \
    "$test_file|replacement_connected_while_old_held" \
    "$test_file|A08 ZSTD_TU->ZSTD_ROUTE" \
    "$test_file|test_source_active_count_cap_waits_then_releases" \
    "$test_file|test_source_raw_byte_cap_waits_then_releases" \
    "$test_file|test_source_admission_releases_on_open_read_error_and_expiry" \
    "$test_file|test_alias_waiter_releases_active_source_credit_for_independent_f" \
    "$test_file|accepted_before_release == 0" \
    "$test_file|A09: active-source count pressure" \
    "$test_file|A10: aggregate raw-byte pressure" \
    "$test_file|A11: success/open/read/expiry" \
    "$test_file|test_runtime_interner_poison_preserves_active_commit" \
    "$test_file|test_runtime_active_source_stop_fail_stops_bounded" \
    "$test_file|--p50-runtime-active-source-stop-child" \
    "$test_file|run_active_source_stop_fail_stop_child" \
    "$test_file|test_runtime_stop_bounds_opening_source_arm" \
    "$test_file|seed_route_endpoint_identity_for_test(" \
    "$test_file|release_retry.wait_for" \
    "$test_file|test_held_retry_does_not_block_healthy_link" \
    "$test_file|healthy_finished_before_retry_release" \
    "$test_file|A06 %s" \
    "$test_file|test_parallel_distinct_c_matrix" \
    "$test_file|codec_gate.wait_for_arrivals(held_workers" \
    "$test_file|all_namespaces_live_while_workers_held" \
    "$test_file|arm_barrier.arrived() == f_count" \
    "$test_file|commit_barrier.arrived() == f_count" \
    "$test_file|healthy_finished_before_release" \
    "$test_file|test_route_poison_latches_before_successor_f_open" \
    "$test_file|first_observation.eof_without_cachewire" \
    "$test_file|authenticated_control_farm_accepts_twenty_and_stops" \
    "$test_file|kConnectionCount = 20" \
    "$test_file|receive_until(acknowledgement, deadline)" \
    "$test_file|wait_for_exit_bounded(child.pid, 1000, status)" \
    "$test_makefile|p50cacheservice_LDADD = ../cache/libp50endpointtesthooks.a" \
    "$test_makefile|../cache/libp50endpoint.a" \
    "$test_makefile|p50cacheservice_DEPENDENCIES = ../cache/icecc-cache-service" \
    "$test_makefile|-DICECC_P50_CACHE_SERVICE_NO_MAIN -DICECC_P50_ENDPOINT_TEST_HOOKS" \
    "$test_file|READY v2 generation=91 attempt=7 F_STORE_GENERATION=191 DERIVATION_VERSION=1" \
    "$doc|Sidecar process and lease ownership"; do
    file=${pair%%|*}; pattern=${pair#*|}
    require "$file" "$pattern"
done

fingerprint_start_line=$(grep -n -F \
    'start_p29_system_source_fingerprint(' "$impl" | tail -n 1 | cut -d: -f1)
fingerprint_wait_line=$(grep -n -F \
    'wait_p29_system_source_fingerprint_for(' \
    "$impl" | tail -n 1 | cut -d: -f1)
ready_line=$(grep -n -F \
    'structured_ready ? write_ready_lease' "$impl" | tail -n 1 | cut -d: -f1)
if test -z "$fingerprint_start_line" || test -z "$fingerprint_wait_line" || \
        test -z "$ready_line" || \
        test "$fingerprint_start_line" -ge "$fingerprint_wait_line" || \
        test "$fingerprint_wait_line" -ge "$ready_line" || \
        ! grep -F 'P29FingerprintOutcome::TimedOut' "$src/cache/p50_slice0.cpp" >/dev/null || \
        ! grep -F 'P29FingerprintOutcome::Cancelled' "$src/cache/p50_slice0.cpp" >/dev/null; then
    echo 'FAIL: P29 fingerprint must complete before cache-service READY' >&2
    exit 1
fi
echo 'ok - P29 fingerprint completes before cache-service READY'

owner_preflight_line=$(grep -n -F \
    'if (!owner_preflight_source_endpoint(endpoint_key,' "$impl" | head -n 1 | cut -d: -f1)
source_size_line=$(grep -n -F 'const auto source_size = source_fd_size(' \
    "$impl" | head -n 1 | cut -d: -f1)
source_read_line=$(grep -n -F 'const auto source_bytes = read_source_fd(' \
    "$impl" | head -n 1 | cut -d: -f1)
f_open_line=$(grep -n -F 'const int first_fd = open_armed(' \
    "$impl" | head -n 1 | cut -d: -f1)
credit_line=$(grep -n -F 'if (!acquire_source_credit(reserved_raw_bytes,' \
    "$impl" | head -n 1 | cut -d: -f1)
if test -z "$owner_preflight_line" || test -z "$source_size_line" || \
        test -z "$source_read_line" || test -z "$f_open_line" || \
        test -z "$credit_line" || test "$owner_preflight_line" -ge "$f_open_line" || \
        test "$owner_preflight_line" -ge "$source_size_line" || \
        test "$source_size_line" -ge "$credit_line" || \
        test "$credit_line" -ge "$f_open_line" || \
        test "$f_open_line" -ge "$source_read_line"; then
    echo 'FAIL: preflight, source sizing and credits must precede F open and source read' >&2
    exit 1
fi
echo 'ok - endpoint preflight and raw-byte reservation precede F open/read'

relationship_cap_line=$(grep -n -F \
    'route_owner_->owner_count() +' \
    "$impl" | head -n 1 | cut -d: -f1)
if test -z "$relationship_cap_line" || \
        ! grep -F 'pending_route_relationships_.size() >=' "$impl" >/dev/null || \
        test "$owner_preflight_line" -ge "$f_open_line"; then
    echo 'FAIL: known relationship capacity must refuse before F open' >&2
    exit 1
fi
echo 'ok - known relationship capacity is checked by pre-open owner preflight'

if sed -n '/^icecc_cache_service_CPPFLAGS =/,/^icecc_cache_service_CXXFLAGS =/p' \
    "$product_makefile" | grep -Fq 'ICECC_P50_ENDPOINT_TEST_HOOKS'; then
    echo 'FAIL: installed cache service enables endpoint test hooks' >&2
    exit 1
fi

# Structured GUIDs must not have an identity-derived fallback, and the legacy
# launch must remain a separate, fresh-root path.
if grep -E 'f_store_guid_for_incarnation|c_store_guid_for_incarnation|monotonic_msec|local::Identity.*store' \
    "$impl" "$header" "$identity" >/dev/null; then
    echo 'FAIL: identity-derived StoreIdentity path remains' >&2
    exit 1
fi

# Source deletion mutants: each authentication/entropy anchor is required.
mutant_dir=$(mktemp -d "${TMPDIR:-/tmp}/p50cacheservice-source.XXXXXX")
trap 'rmdir "$mutant_dir" 2>/dev/null || true' EXIT HUP INT TERM
for pattern in \
    'read_structured_launch' \
    'have_c_store_guid' \
    'have_f_store_guid' \
    'fresh_store_identity_root(legacy_root)' \
    'structured_launch.c_store_guid'; do
    mutant="$mutant_dir/mutant"
    sed "/$(printf '%s' "$pattern" | sed 's/[.[\*^$\\/]/\\&/g')/d" "$impl" >"$mutant"
    if grep -F "$pattern" "$mutant" >/dev/null; then
        echo "deletion mutant survived: $pattern" >&2
        exit 1
    fi
done

# Operation-control must remain owned through endpoint settlement, use one
# cumulative deadline, and fail-stop rather than returning across a possibly
# live owner coroutine.
for pair in \
    "$impl|SidecarRuntime::run_endpoint_on_owner" \
    "$impl|run_endpoint_on_owner(dispatch_fd" \
    "$impl|context_.run()" \
    "$impl|if (endpoint_owner_failed_.load(std::memory_order_acquire)) {" \
    "$impl|owner_failure_after_live" \
    "$impl|live_sessions_.store(1, std::memory_order_release)" \
    "$impl|SidecarRuntime::run_one" \
    "$impl|make_completion_pipe" \
    "$impl|CompletionWake" \
    "$impl|ControlOperationKind::OperationCancel" \
    "$impl|RuntimeCancellationReason::ControlEof" \
    "$impl|RuntimeCancellationReason::Malformed" \
    "$impl|quiescence_deadline" \
    "$impl|cancellation_trigger_deadline" \
    "$impl|std::min(" \
    "$impl|std::_Exit(125)" \
    "$impl|completion_pipe" \
    "$impl|~ActiveControlGuard()" \
    "$impl|close_active_control" \
    "$impl|FdHandoffReceiver receiver" \
    "$impl|receiver.receive_and_ack" \
    "$impl|receiver.take_adopted_fd" \
    "$impl|send_cache_session_ready(adopted.get(), deadline)" \
    "$impl|endpoint_->run_adopted" \
    "$impl|busy_.test_and_set" \
    "$impl|source_admission_mutex_" \
    "$impl|raw_bytes <= config_.max_aggregate_source_raw_bytes -" \
    "$impl|active_source_count_ <" \
    "$impl|config_.max_active_source_transfers" \
    "$impl|cancel_endpoint_run()" \
    "$control|kControlOperationVersionV3" \
    "$control|ControlCancelTargetRole::CSource" \
    "$control|ControlCancelTargetRole::FSession" \
    "$control|ControlOperationRole::Sidecar" \
    "$control_cpp|encode_control_operation(" \
    "$control_cpp|decode_control_operation(" \
    "$control_cpp|wire.begin() + 40" \
    "$test_file|test_operation_cancel_prebyte_mid_dialogue_eof_deadline" \
    "$test_file|test_runtime_cancel_fail_stop_subprocess" \
    "$test_file|test_runtime_owner_failure_fail_stop_subprocess" \
    "$test_file|test_runtime_live_owner_failure_fail_stop_subprocess" \
    "$test_file|test_operation_cancel_commit_race_preserves_witness" \
    "$test_file|WEXITSTATUS(status) == 125" \
    "$test_file|ClientCancellationDisposition::None" \
    "$test_file|wrong_binding[0]" \
    "$header|exact nonzero session binding" \
    "$header|daemon OP_CANCEL/C_SOURCE emission remain HOLD"; do
    file=${pair%%|*}; pattern=${pair#*|}
    require "$file" "$pattern"
done

if grep -F 'release_active_control' "$impl" "$header" >/dev/null; then
    echo 'FAIL: stale release_active_control path survived' >&2
    exit 1
fi
if [ "$(grep -c 'fail_stop();' "$impl")" -lt 2 ]; then
    echo 'FAIL: both owner-failure and cumulative-deadline fail-stops are required' >&2
    exit 1
fi

for pattern in \
    'if (endpoint_owner_failed_.load(std::memory_order_acquire)) {' \
    'owner_failure_after_live' \
    '~ActiveControlGuard()' \
    'cancellation_trigger_deadline' \
    'std::_Exit(125)'; do
    mutant="$mutant_dir/opcancel-mutant"
    awk -v needle="$pattern" 'index($0, needle) == 0' "$impl" >"$mutant"
    if grep -F "$pattern" "$mutant" >/dev/null; then
        echo "operation-control deletion mutant survived: $pattern" >&2
        exit 1
    fi
done

mutant="$mutant_dir/deadline-mutant"
sed 's/std::min(/std::chrono::steady_clock::now() + /' "$impl" >"$mutant"
if grep -F 'std::min(' "$mutant" >/dev/null; then
    echo 'cumulative-deadline grace-extension mutant survived' >&2
    exit 1
fi

echo 'ok - cache service StoreIdentity/operation-control source gates hold'
