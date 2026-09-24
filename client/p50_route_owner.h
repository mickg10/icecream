#pragma once

#include "p50_zstd_sender.h"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
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
    using Sender = std::unique_ptr<P50ZstdSourceSender>;

    [[nodiscard]] ZstdSourceTransferResult invalid() const noexcept;
    [[nodiscard]] ZstdSourceTransferResult replacement() const noexcept;
    [[nodiscard]] PreparationRouteKey route_key(
        const P50RouteRelationship& relationship) const noexcept;
    Sender& get_or_create(const P50RouteRelationship& relationship,
                          PrepareRequestKey request,
                          std::chrono::steady_clock::time_point deadline);

    P50RouteOwnerConfig config_{};
    std::shared_ptr<P50PreparationAuthority> authority_;
    std::map<P50RouteRelationship, Sender> owners_;
    std::map<P50RouteRelationship, uint64_t> physical_generations_;
    std::map<std::tuple<CStoreGuid, FStoreGuid, uint64_t>, ProfileId>
        p51_incarnation_profiles_;
    uint64_t next_physical_generation_ = 1;
    // The supervisor replaces this whole C sidecar, not one relationship.
    // Once any sender reports ambiguous state, no other relationship may
    // open F even if the wrapper that observed the first failure disappears.
    bool replacement_required_ = false;
};

}  // namespace icecc::p50
