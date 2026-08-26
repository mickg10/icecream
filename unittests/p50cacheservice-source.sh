#!/bin/sh
# Deletion-sensitive StoreIdentity/legacy READY source gate.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
impl="$src/cache/p50_cache_service.cpp"
header="$src/cache/p50_cache_service.h"
control="$src/cache/p50_control_operation.h"
identity="$src/cache/p50_incarnation_identity.h"
wire="$src/services/p50_store_identity_wire.h"
doc="$src/cache/P50_CACHE_SERVICE.md"
test_file="$src/unittests/p50cacheservice.cpp"

require() { grep -F "$2" "$1" >/dev/null; }
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
    "$impl|store_identity_root_from_f_guid" \
    "$impl|c_guid != c_store_guid_for_root(root)" \
    "$impl|guid != f_store_guid_for_root(root)" \
    "$impl|fresh_store_identity_root(legacy_root)" \
    "$impl|runtime_config.f_store_guid = structured_launch.active" \
    "$impl|ICECC_CACHE_SERVICE_EXPECTED_DERIVATION_VERSION" \
    "$header|RuntimeConfig" \
    "$test_file|legacy_store_identity_launches" \
    "$test_file|test_runtime_store_identity_is_explicit_and_role_tagged" \
    "$test_file|READY v2 generation=91 attempt=7 DERIVATION_VERSION=1" \
    "$doc|CSPRNG"; do
    file=${pair%%|*}; pattern=${pair#*|}
    require "$file" "$pattern"
done

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
    "$impl|cancel_active_socket()" \
    "$control|kControlOperationVersionV3" \
    "$control|ControlCancelTargetRole::CSource" \
    "$control|ControlCancelTargetRole::FSession" \
    "$control|ControlOperationRole::Sidecar" \
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
