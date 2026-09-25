#pragma once

#include "p50_zstd_sender.h"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <vector>

namespace icecc::p50 {

// The complete identity of one C-cache to F-cache source relationship.  F's
// generation is part of the key so a restarted F cannot inherit route history
// from its predecessor, even when it reuses the same store GUID.
struct P50RouteRelationship {
    CStoreGuid c_store_guid{};
    FStoreGuid f_store_guid{};
    uint64_t f_store_generation = 0;
    ProfileId profile = ProfileId::ZSTD_TU;

    auto operator<=>(const P50RouteRelationship&) const = default;

    [[nodiscard]] bool valid() const noexcept;
};

struct P50RouteOwnerConfig {
    EndpointCaps endpoint_caps{};
    PreparationAuthorityLimits authority_limits{};
    // Both dimensions are process-lifetime bounds.  Reaching either one is
    // an explicit request to replace the supervised C sidecar; it must never
    // silently turn later assignments into legacy work.
    size_t max_completed_requests = 4096;
    size_t max_relationships = 256;
    std::chrono::steady_clock::duration maximum_duration =
        std::chrono::seconds(300);
    int compression_level = 3;
    P29InternerFaultInjection p29_interner_fault_injection =
        P29InternerFaultInjection::Disabled;
    // Invoked from detached R2 pump completion. Production installs a
    // lifetime-fenced callback which posts route reaping onto the owner
    // executor; tests may leave it empty.
    std::function<void()> post_retired_reap;
    // Narrow observation seams for proving that a pump-triggered idle reap
    // retires exact route state without a subsequent transfer.
    std::function<bool()> hold_r2_receipt_reader_for_test;
    std::function<bool()> hold_r2_ack_pump_for_test;
    std::function<void()> after_r2_rebind_wait_for_test;
    std::function<void()> after_retired_route_reaped_for_test;
    std::function<bool(uint64_t)> disconnect_r2_after_bundle_for_test;
    // Observes each complete R2 source bundle written by the sender. This is
    // test-only evidence of C-side wire occupancy, not proof that F parsed it.
    std::function<void(uint64_t)> after_r2_bundle_sent_for_test;
    // Request-scoped observation of the caller that enters shared R2 recovery.
    // Test-only; it does not participate in admission or recovery decisions.
    std::function<void(PrepareRequestKey)> before_r2_recovery_for_test;
    // Observes the assignment selected as the shared recovery coordinator.
    std::function<void(PrepareRequestKey)>
        before_r2_recovery_attempt_for_test;
    std::function<void(PrepareRequestKey, uint64_t)>
        after_r2_recovery_receipt_settled_for_test;
    std::function<bool(uint64_t, size_t, bool, bool)>
        disconnect_r2_before_replay_bundle_for_test;
    // Forwarded only to the sender's deterministic route-poison unit seam.
    // Production callers always leave this empty.
    std::function<void()> before_prepare_for_route_for_test;
};

// Long-lived C-side ownership for route source transfers.  One sender is
// retained for each exact C/F relationship, F generation, and profile.  The
// owner is single-threaded like P50PreparationAuthority; callers serialize
// operations for one relationship on their compiler-wrapper owner.
class P50CRouteOwner {
public:
    explicit P50CRouteOwner(P50RouteOwnerConfig config = {});
    ~P50CRouteOwner();
    P50CRouteOwner(const P50CRouteOwner&) = delete;
    P50CRouteOwner& operator=(const P50CRouteOwner&) = delete;

    boost::asio::awaitable<ZstdSourceTransferResult> transfer(
        P50RouteRelationship relationship, PrepareRequestKey request,
        boost::asio::ip::tcp::endpoint remote,
        std::chrono::steady_clock::time_point deadline,
        std::span<const uint8_t> source);

    boost::asio::awaitable<ZstdSourceTransferResult> transfer(
        P50RouteRelationship relationship, PrepareRequestKey request,
        ConnectedFdFactory connection,
        std::chrono::steady_clock::time_point deadline,
        std::span<const uint8_t> source);

    boost::asio::awaitable<ZstdSourceTransferResult> transfer(
        P50RouteRelationship relationship, PrepareRequestKey request,
        AsyncConnectedFdFactory connection,
        std::chrono::steady_clock::time_point deadline,
        std::span<const uint8_t> source);

    boost::asio::awaitable<ZstdSourceTransferResult> transfer_p51(
        P50RouteRelationship relationship, P51SourceArmedFields armed,
        AsyncConnectedFdFactory connection, PrepareRequestKey request,
        std::chrono::steady_clock::time_point deadline,
        std::span<const uint8_t> source);

    // Drops every profile view for one exact retired F incarnation.  False
    // means at least one route still owns an uncommitted preparation; callers
    // must replace the whole C sidecar and must not admit the successor.
    [[nodiscard]] bool reset_f_store_exact(
        FStoreGuid old_f_store_guid,
        uint64_t old_f_store_generation) noexcept;
    // R2-only F incarnation retirement. Active senders are fenced and moved
    // to the bounded retired table; their route preparation is discarded only
    // after every caller/pump releases its shared sender reference. R1 reset
    // semantics above remain unchanged.
    [[nodiscard]] bool retire_f_store_exact_p51(
        FStoreGuid old_f_store_guid,
        uint64_t old_f_store_generation) noexcept;
    // Retires only the exact R2 logical relationship/epoch on the specified
    // physical link. This is used for a typed ReservationMissing response on
    // an otherwise unchanged F store incarnation.
    [[nodiscard]] bool retire_relationship_exact_p51(
        const P50RouteRelationship& relationship,
        const P50ZstdSourceSender* expected_sender, Id128 relationship_id,
        uint64_t relationship_epoch,
        uint64_t physical_link_generation) noexcept;
    // Stop/fail the C runtime's current R2 physical links without touching
    // retained route maps from another thread. Must run on the route owner's
    // executor; active senders close their exact socket and wake their waits.
    void cancel_active_p51_transfers() noexcept;
    // Owner-affine callback target for post-pump retirement notifications.
    void reap_retired_p51() noexcept { reap_retired_senders(); }
    void reset() noexcept;

    [[nodiscard]] size_t owner_count() const noexcept { return owners_.size(); }
    [[nodiscard]] bool owns(const P50RouteRelationship& relationship) const noexcept;
#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
    // Seeds one fully constructed relationship owner without opening its F
    // connection.  SidecarRuntime capacity tests use this only while no
    // transfer is active.
    [[nodiscard]] bool seed_relationship_for_test(
        const P50RouteRelationship& relationship) noexcept;
#endif

private:
    using Sender = std::shared_ptr<P50ZstdSourceSender>;
    struct RetiredSender {
        Sender sender;
        std::optional<PreparationRouteKey> abandon_route_when_quiescent;
    };
    struct P51LinkIdentity {
        Id128 relationship_id{};
        uint64_t relationship_epoch = 0;
    };

    [[nodiscard]] ZstdSourceTransferResult invalid() const noexcept;
    [[nodiscard]] ZstdSourceTransferResult replacement() const noexcept;
    void require_replacement(ReplacementTrigger trigger) noexcept;
    [[nodiscard]] PreparationRouteKey route_key(
        const P50RouteRelationship& relationship) const noexcept;
    Sender& get_or_create(const P50RouteRelationship& relationship,
                          PrepareRequestKey request,
                          std::chrono::steady_clock::time_point deadline);
    void reap_retired_senders() noexcept;
    boost::asio::awaitable<bool> wait_for_retired_route_quiescence(
        PreparationRouteKey route,
        std::chrono::steady_clock::time_point deadline);
    boost::asio::awaitable<bool> wait_for_sender_rebind_quiescence(
        const Sender& sender,
        std::chrono::steady_clock::time_point deadline);

    P50RouteOwnerConfig config_{};
    std::shared_ptr<P50PreparationAuthority> authority_;
    std::map<P50RouteRelationship, Sender> owners_;
    std::vector<RetiredSender> retired_senders_;
    std::map<P50RouteRelationship, uint64_t> physical_generations_;
    std::map<P50RouteRelationship, P51LinkIdentity> p51_link_identities_;
    std::map<std::tuple<CStoreGuid, FStoreGuid, uint64_t>, ProfileId>
        p51_incarnation_profiles_;
    uint64_t next_physical_generation_ = 1;
    // The supervisor replaces this whole C sidecar, not one relationship.
    // Once any sender reports ambiguous state, no other relationship may
    // open F even if the wrapper that observed the first failure disappears.
    bool replacement_required_ = false;
    ReplacementTrigger replacement_trigger_ =
        ReplacementTrigger::Unattributed;
};

}  // namespace icecc::p50
