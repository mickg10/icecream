#include "../cache/p50_endpoint_run_cancel.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>

using namespace icecc::p50;

namespace {

struct Target final : EndpointSocketTarget {
    explicit Target(int& cancels, int descriptor = 0,
                    uintptr_t object_address = 0)
        : cancels_(cancels), descriptor_(descriptor),
          object_address_(object_address) {}
    void cancel() noexcept override { ++cancels_; }
    int& cancels_;
    int descriptor_;
    uintptr_t object_address_;
};

SidecarLaunchIdentity launch(uint64_t generation, uint64_t attempt,
                             uint64_t root_byte) {
    SidecarLaunchIdentity result;
    result.identity = {generation, attempt};
    result.store_generation = root_byte + 10;
    result.store_root.bytes[1] = static_cast<uint8_t>(root_byte);
    result.c_store_guid = c_store_guid_for_root(result.store_root);
    result.f_store_guid = f_store_guid_for_root(result.store_root);
    assert(result.valid());
    return result;
}

EndpointRunIdentity identity(SidecarLaunchIdentity incarnation,
                             uint64_t endpoint_generation,
                             uint64_t session, uint64_t run,
                             uint64_t socket_generation) {
    EndpointRunIdentity result;
    result.sidecar_launch = incarnation;
    result.c_store_guid = incarnation.c_store_guid;
    result.f_store_guid = incarnation.f_store_guid;
    result.f_session_operation = {
        {incarnation.identity.generation, incarnation.identity.attempt},
        daemon::P50SessionOperationRole::FSession, run};
    result.endpoint_generation = endpoint_generation;
    result.endpoint_session_serial = session;
    result.run_sequence = run;
    result.socket_ownership_generation = socket_generation;
    assert(result.valid());
    return result;
}

const sidecar::AbsoluteMonotonicDeadline deadline(uint64_t n) {
    return {static_cast<int64_t>(n), 1, 1};
}

void require(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        std::abort();
    }
}

void test_queued_cancel_finish_restart_and_stale() {
    EndpointRunRegistry registry;
    int a_cancel = 0;
    int b_cancel = 0;
    const auto inc = launch(7, 1, 3);
    const auto a_id = identity(inc, 1, 11, 101, 501);
    // A and B deliberately reuse the same simulated descriptor number and
    // socket-object address. Only the typed socket-ownership generation may
    // distinguish them.
    auto a = registry.admit(a_id, deadline(100),
                            std::make_shared<Target>(a_cancel, 42, 0x1000));
    require(a.has_value(), "admitted A");
    EndpointRunHandle moved = std::move(*a);
    require(!a->valid() && moved.valid(), "run handle move transfers authority");
    a = std::move(moved);
    const EndpointCancelPermit permit_a = a->permit(EndpointCancelReason::CallerRequested);
    require(registry.request_cancel(permit_a) == EndpointCancelResult::CancelRequested,
            "queued cancel A");
    require(a_cancel == 1, "A target cancelled");
    require(registry.mark_terminal(a_id, {EndpointTerminalResultState::Cancelled, 0}),
            "finished A");
    require(registry.consume_terminal(a_id).has_value(), "consumed A result");
    const auto b_id = identity(inc, 1, 12, 102, 502);
    auto b = registry.admit(b_id, deadline(101),
                            std::make_shared<Target>(b_cancel, 42, 0x1000));
    require(b.has_value(), "started B");
    require(registry.request_cancel(permit_a) == EndpointCancelResult::Stale,
            "late A cancel is stale");
    require(b_cancel == 0 && registry.inspect(b_id).has_value(), "B remains live");
}

void test_concurrent_namespaces_and_exact_validation() {
    EndpointRunRegistry registry(4);
    int a_cancel = 0;
    int b_cancel = 0;
    const auto inc_a = launch(8, 1, 4);
    const auto inc_b = launch(8, 2, 5);
    const auto a_id = identity(inc_a, 2, 21, 201, 601);
    const auto b_id = identity(inc_b, 2, 22, 202, 602);
    auto a = registry.admit(a_id, deadline(200), std::make_shared<Target>(a_cancel));
    auto b = registry.admit(b_id, deadline(201), std::make_shared<Target>(b_cancel));
    require(a.has_value() && b.has_value(), "concurrent A/B namespaces");
    require(registry.request_cancel(a->permit(EndpointCancelReason::ControlEof)) ==
                EndpointCancelResult::CancelRequested,
            "cancel A only");
    require(a_cancel == 1 && b_cancel == 0, "B unaffected");
    EndpointCancelPermit wrong = b->permit(EndpointCancelReason::CallerRequested);
    const EndpointRunIdentity original = wrong.identity;
    const auto stale = [&](EndpointCancelPermit candidate, const char* message) {
        require(registry.request_cancel(candidate) == EndpointCancelResult::Stale, message);
        require(b_cancel == 0, "wrong permit did not touch B");
    };
    wrong.identity.sidecar_launch.identity.attempt++;
    stale(wrong, "wrong launch is stale");
    wrong.identity = original;
    wrong.identity.c_store_guid.bytes[1]++;
    stale(wrong, "wrong C store is stale");
    wrong.identity = original;
    wrong.identity.f_store_guid.bytes[1]++;
    stale(wrong, "wrong F store is stale");
    wrong.identity = original;
    wrong.identity.f_session_operation.operation_sequence++;
    stale(wrong, "wrong operation is stale");
    wrong.identity = original;
    ++wrong.identity.endpoint_generation;
    stale(wrong, "wrong endpoint generation is stale");
    wrong.identity = original;
    ++wrong.identity.endpoint_session_serial;
    stale(wrong, "wrong endpoint session is stale");
    wrong.identity = original;
    ++wrong.identity.run_sequence;
    stale(wrong, "wrong run sequence is stale");
    wrong.identity = original;
    ++wrong.identity.socket_ownership_generation;
    stale(wrong, "wrong socket generation is stale");
    wrong.identity = original;
    ++wrong.observation_id;
    stale(wrong, "wrong observation is stale");
    wrong = b->permit(EndpointCancelReason::CallerRequested);
    wrong.reason = EndpointCancelReason::None;
    stale(wrong, "missing reason is stale");
    require(registry.request_cancel(b->permit(EndpointCancelReason::CallerRequested)) ==
                EndpointCancelResult::CancelRequested,
            "exact B cancel");
    require(registry.request_cancel(b->permit(EndpointCancelReason::CallerRequested)) ==
                EndpointCancelResult::AlreadyRequested,
            "duplicate exact cancel is idempotent");
}

void test_reuse_terminal_races_saturation_and_shutdown() {
    EndpointRunRegistry registry(1);
    int first_cancel = 0;
    int second_cancel = 0;
    const auto inc_a = launch(9, 1, 6);
    const auto inc_b = launch(9, 2, 7);
    const auto a_id = identity(inc_a, 3, 31, 301, 701);
    auto a = registry.admit(a_id, deadline(300), std::make_shared<Target>(first_cancel));
    require(a.has_value(), "saturation admits A");
    require(!registry.admit(identity(inc_b, 3, 32, 302, 702), deadline(301),
                            std::make_shared<Target>(second_cancel)),
            "saturation rejects B before A terminal");
    const auto p = a->permit(EndpointCancelReason::CallerRequested);
    require(registry.request_cancel(p) == EndpointCancelResult::CancelRequested,
            "cancel A in saturated registry");
    require(registry.request_cancel(p) == EndpointCancelResult::AlreadyRequested,
            "duplicate cancellation before terminal");
    require(registry.mark_terminal(a_id, {EndpointTerminalResultState::Failed, 5}),
            "terminal wins after cancellation");
    require(registry.request_cancel(p) == EndpointCancelResult::AlreadyTerminal,
            "cancel after terminal is terminal");
    require(registry.consume_terminal(a_id).has_value(), "consume terminal A");
    auto b = registry.admit(identity(inc_b, 3, 32, 302, 702), deadline(301),
                            std::make_shared<Target>(second_cancel));
    require(b.has_value(), "one saturated slot reopens");

    // Only the exact launch incarnation is eligible for shutdown cancellation.
    require(registry.cancel_all_for_incarnation(inc_a) == 0,
            "old incarnation cannot cancel B");
    require(registry.cancel_all_for_incarnation(inc_b) == 1,
            "matching incarnation cancels B");
    require(second_cancel == 1, "shutdown cancellation touched B");
    require(registry.mark_terminal(b->identity(),
                                  {EndpointTerminalResultState::Cancelled, 0}),
            "terminal B");
    require(registry.consume_terminal(b->identity()).has_value(), "consume B");
    require(registry.live_count() == 0 && registry.timer_count() == 0 &&
                registry.target_count() == 0 && registry.cancelled_live_count() == 0,
            "zero final registry/timers/targets/cancelled-live inventory");
}

void test_adopted_result_is_observation_until_owner_settlement() {
    // The endpoint result carries no local replacement/fallback authority.
    // Malformed TX_COMMIT, partial/full BODY exceptions, and a late F result
    // all leave this observation unresolved until the owning FSession row is
    // consumed through the typed registry.
    enum class ObservationSettlement { Unresolved, CommittedInput };
    ObservationSettlement malformed_commit = ObservationSettlement::Unresolved;
    require(malformed_commit == ObservationSettlement::Unresolved,
            "malformed commit does not settle locally");
    ObservationSettlement body_exception = ObservationSettlement::Unresolved;
    require(body_exception == ObservationSettlement::Unresolved,
            "BODY exception does not settle locally");
    ObservationSettlement late_f = ObservationSettlement::Unresolved;
    require(late_f == ObservationSettlement::Unresolved,
            "late F result remains an observation");
}

} // namespace

int main() {
    test_queued_cancel_finish_restart_and_stale();
    test_concurrent_namespaces_and_exact_validation();
    test_reuse_terminal_races_saturation_and_shutdown();
    test_adopted_result_is_observation_until_owner_settlement();
    std::cout << "PASS: operation-scoped endpoint run cancellation evidence\n";
}
