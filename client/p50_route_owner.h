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
    std::chrono::steady_clock::duration maximum_duration =
        std::chrono::seconds(300);
    int compression_level = 3;
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

    // Drops every sender for this F relationship.  The generation argument is
    // the newly observed nonzero F generation and makes reset callers prove
    // they are not clearing state for an unbound F identity.
    void reset_f_store(FStoreGuid f_store_guid,
                       uint64_t new_f_store_generation) noexcept;
    void reset() noexcept;

    [[nodiscard]] size_t owner_count() const noexcept { return owners_.size(); }
    [[nodiscard]] bool owns(const P50RouteRelationship& relationship) const noexcept;

private:
    using Sender = std::unique_ptr<P50ZstdSourceSender>;

    [[nodiscard]] ZstdSourceTransferResult invalid() const noexcept;
    Sender& get_or_create(const P50RouteRelationship& relationship,
                          PrepareRequestKey request,
                          std::chrono::steady_clock::time_point deadline);

    P50RouteOwnerConfig config_{};
    std::map<P50RouteRelationship, Sender> owners_;
};

}  // namespace icecc::p50
