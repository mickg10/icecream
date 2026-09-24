#!/bin/sh
# Supplemental deletion-sensitive gate for the adopted endpoint seam.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
endpoint="$src/cache/p50_endpoint.cpp"

function_body_containing() {
    needle=$1
    file=$2
    awk -v needle="$needle" '
        /P50ServerEndpoint::(run_adopted|run_connected)\(/ {
            inside = 1
            capture = ""
            target = 0
            depth = 0
            saw_open = 0
        }
        inside {
            capture = capture $0 ORS
            if (index($0, needle) != 0) target = 1
            opens = gsub(/\{/, "{")
            closes = gsub(/\}/, "}")
            if (opens != 0) saw_open = 1
            depth += opens - closes
            if (saw_open && depth == 0) {
                if (target) printf "%s", capture
                inside = 0
            }
        }
    ' "$file"
}

body=$(function_body_containing 'sidecar::P5coEndpointHandoff' "$endpoint")
printf '%s\n' "$body" | grep -F 'co_return co_await run_connected(' >/dev/null
printf '%s\n' "$body" | \
    grep -F 'std::move(*socket), std::move(registration), std::move(control)' >/dev/null
printf '%s\n' "$body" | grep -F 'SessionRegistration registration(*impl_, session)' >/dev/null
if printf '%s\n' "$body" | grep -E 'listen|async_accept|async_read_frame|decode_as|materialize_and_commit' >/dev/null; then
    echo 'FAIL: adopted endpoint contains a reducer bypass or a second accept' >&2
    exit 1
fi
echo 'ok - adopted endpoint delegates to the shared connected reducer'
echo 'ok - adopted endpoint has no accept/read/reducer duplicate'

mutant=$(mktemp "${TMPDIR:-/tmp}/p50endpoint-mutant.XXXXXX")
trap 'rm -f "$mutant"' EXIT HUP INT TERM
perl -0pe \
    's/co_return co_await run_connected\(\n        std::move\(\*socket\)/co_return ServerRunResult{}; \/\* deletion mutant *\/\n    ignored_connected_call(\n        std::move(*socket)/' \
    "$endpoint" >"$mutant"
if function_body_containing 'sidecar::P5coEndpointHandoff' "$mutant" | \
        grep -F 'co_return co_await run_connected(' >/dev/null; then
    echo 'FAIL: reducer-deletion mutant was accepted' >&2
    exit 1
fi
echo 'ok - deleting adopted delegation is rejected'

typed_body=$body
connected_body=$(function_body_containing 'std::optional<CStoreGuid> expected_c_store_guid' \
    "$endpoint")
test -n "$typed_body"
test -n "$connected_body"
for law in \
    'decode_cache_session_wire_claim' \
    'expected_c_store_guid' \
    'take_lease_for_endpoint' \
    'release_native_fd_for_endpoint' \
    'lease->fence();' \
    'allocate_session(exact_outcome.operation, exact_deadline)' \
    'nullptr, exact_deadline'
do
    printf '%s\n' "$typed_body" | grep -F "$law" >/dev/null
done
grep -F 'timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)' \
    "$endpoint" >/dev/null
grep -F 'timerfd_settime(timer_fd, TFD_TIMER_ABSTIME' "$endpoint" >/dev/null
grep -F '::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)' "$endpoint" >/dev/null
grep -F '::fcntl(owner_notification.get(), F_DUPFD_CLOEXEC, 0)' \
    "$endpoint" >/dev/null
grep -F 'signal_notification_fd(state->worker_notification.get())' \
    "$endpoint" >/dev/null
grep -F 'static constexpr size_t kMaxOutstanding = 8' "$endpoint" >/dev/null
if grep -F 'asio::post(owner_executor' "$endpoint" >/dev/null; then
    echo 'FAIL: codec worker restored an allocating owner-executor post' >&2
    exit 1
fi
for law in \
    'io->deadline_timer.async_wait' \
    'require_operation();' \
    'begin_materialization' \
    'co_await async_materialize' \
    'finish_materialization' \
    'select_materialized_job_state' \
    'commit_materialized'
do
    printf '%s\n' "$connected_body" | grep -F "$law" >/dev/null
done
if printf '%s\n' "$connected_body" | grep -F 'materialize_and_commit' >/dev/null; then
    echo 'FAIL: connected endpoint restored synchronous materialize-and-commit' >&2
    exit 1
fi
echo 'ok - typed P5CO endpoint preserves claim, operation, descriptor, and deadline authority'
echo 'ok - owner timer and bounded codec-worker completion precede commit'

typed_mutant=$(mktemp "${TMPDIR:-/tmp}/p50endpoint-typed-mutant.XXXXXX")
trap 'rm -f "$mutant" "$typed_mutant"' EXIT HUP INT TERM
sed 's/impl_->allocate_session(exact_outcome.operation, exact_deadline)/impl_->allocate_session()/' \
    "$endpoint" >"$typed_mutant"
if function_body_containing 'sidecar::P5coEndpointHandoff' "$typed_mutant" | \
        grep -F 'allocate_session(exact_outcome.operation, exact_deadline)' >/dev/null; then
    echo 'FAIL: P5CO operation-binding deletion mutant was accepted' >&2
    exit 1
fi
echo 'ok - deleting the exact P5CO operation binding is rejected'

grep -F 'adopt_connected_fd' "$src/cache/p50_endpoint.h" >/dev/null
grep -F 'EndpointCancelResult request_cancel(const EndpointCancelPermit& permit)' \
    "$src/cache/p50_endpoint.h" >/dev/null
grep -F 'EndpointRunRegistry endpoint_runs' "$endpoint" >/dev/null
grep -F 'impl_->endpoint_runs.request_cancel(permit)' "$endpoint" >/dev/null
grep -F 'ClientRunObservation::ExactCommitObserved' "$endpoint" >/dev/null
grep -F 'ClientRunObservation::WrongAdoptedPeer' "$endpoint" >/dev/null
grep -F 'ClientCancellationDisposition::ReconcileRequired' "$endpoint" >/dev/null
if rg -n 'whole_new_attempt|ClientRunSettlement|\.settlement|active_remote_transmission_may_have_begun|AbortedPreDurable' \
    "$src/cache/p50_endpoint.cpp" "$src/cache/p50_endpoint.h"; then
    echo 'FAIL: local endpoint retained a settlement or pre-durable authority' >&2
    exit 1
fi
for token in active_socket active_io socket_for_test_cancel active_cancel_fd_ request_cancel_for_test; do
    if awk -v token="$token" '
        /^[[:space:]]*#if(n?def)?[[:space:]]+ICECC_P50_ENDPOINT_TEST_HOOKS/ {guard++}
        /^[[:space:]]*#endif/ && guard > 0 {guard--; next}
        guard == 0 && index($0, token) {print FNR ":" $0; bad=1}
        END {exit bad ? 0 : 1}
    ' "$src/cache/p50_endpoint.cpp" "$src/cache/p50_endpoint.h" \
      "$src/cache/p50_cache_service.cpp" "$src/cache/p50_cache_service.h"; then
        echo "FAIL: macro-free product contains forbidden endpoint alias $token" >&2
        exit 1
    fi
done
grep -F 'raw_cancel_client_after_hello' "$src/unittests/p50_endpoint_test.cpp" >/dev/null
grep -F 'test_complete_p5co_endpoint_handoff' \
    "$src/unittests/p50_endpoint_test.cpp" >/dev/null
grep -F 'test_p5co_worker_completion_is_stale_after_deadline_or_cancel' \
    "$src/unittests/p50_endpoint_test.cpp" >/dev/null
grep -F 'test_p5co_codec_queue_is_bounded' \
    "$src/unittests/p50_endpoint_test.cpp" >/dev/null
grep -F 'test_server_completion_rechecks_after_live_callback' \
    "$src/unittests/p50_endpoint_test.cpp" >/dev/null
grep -F 'test_p5co_deadline_wins_after_owner_job_selector' \
    "$src/unittests/p50_endpoint_test.cpp" >/dev/null
grep -F 'test_p5co_endpoint_fences_post_transfer_failures' \
    "$src/unittests/p50_endpoint_test.cpp" >/dev/null
grep -F 'test_client_completion_deadline_is_fresh_before_first_write' \
    "$src/unittests/p50_endpoint_test.cpp" >/dev/null
grep -F 'endpoint_elapsed < std::chrono::milliseconds(300)' \
    "$src/unittests/p50_endpoint_test.cpp" >/dev/null
grep -F 'post-deadline codec completion published durable input' \
    "$src/unittests/p50_endpoint_test.cpp" >/dev/null
grep -F 'codec pool did not enforce its running-plus-queued job bound' \
    "$src/unittests/p50_endpoint_test.cpp" >/dev/null
grep -F 'codec worker completion leaked an admission slot' \
    "$src/unittests/p50_endpoint_test.cpp" >/dev/null
grep -F 'deadline-abandoned worker leaked eventfd/timerfd authority' \
    "$src/unittests/p50_endpoint_test.cpp" >/dev/null
grep -F 'server completion callback crossed operation authority' \
    "$src/unittests/p50_endpoint_test.cpp" >/dev/null
grep -F 'post-release session-allocation failure was not fenced' \
    "$src/unittests/p50_endpoint_test.cpp" >/dev/null
grep -F 'post_release_fence_records_without_closing_transferred_fd' \
    "$src/unittests/p50_adopted_socket_lease_test.cpp" >/dev/null
grep -F 'ICECC_P50_ADOPTED_SOCKET_LEASE_TEST_HOOKS' \
    "$src/unittests/p50_adopted_socket_lease-run.sh" >/dev/null
grep -F 'post-deadline client completion attempted the first CacheWire write' \
    "$src/unittests/p50_endpoint_test.cpp" >/dev/null
grep -F 'stale P5CO operation completion advanced the endpoint' \
    "$src/unittests/p50_endpoint_test.cpp" >/dev/null
grep -F 'records_.reserve(max_records_)' \
    "$src/cache/p50_input_record.cpp" >/dev/null
grep -F 'InputRecordStore::prepare_verified_publish' \
    "$src/cache/p50_input_record.cpp" >/dev/null
grep -F 'records_.insert(std::move(prepared.state_->node))' \
    "$src/cache/p50_input_record.cpp" >/dev/null
grep -F 'prepared InputRecord commit allocated at linearization' \
    "$src/unittests/p50_input_record_test.cpp" >/dev/null
grep -F 'p50endpoint-mutants.sh p50profile-digest-mutants.sh p50inputrecord-mutants.sh' \
    "$src/unittests/Makefile.am" >/dev/null
grep -F 'post-hello C cancellation silently authorized an abort' \
    "$src/unittests/p50_endpoint_test.cpp" >/dev/null
grep -F 'CLOEXEC' "$src/cache/P50_PROTOCOL.md" >/dev/null
grep -F 'Endpoint execution and cancellation' "$src/cache/P50_PROTOCOL.md" >/dev/null
grep -F '(0, 0, 0)' "$src/cache/P50_PROTOCOL.md" >/dev/null
# A FILL sent before NEED is still checked against that NEED.
grep -F 'impl_->preparation->confirm_p29v1_need(' "$endpoint" >/dev/null
echo 'PASS: adopted endpoint and operation-scoped cancellation source gates hold'
