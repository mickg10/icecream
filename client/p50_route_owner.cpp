#include "p50_route_owner.h"

#include <stdexcept>
#include <utility>

namespace icecc::p50 {
namespace {

bool supported_profile(ProfileId profile) noexcept {
    if (profile == ProfileId::P29 || profile == ProfileId::ZSTD_TU ||
        profile == ProfileId::Z3_LONG)
        return true;
#if defined(ICECC_P50_WITH_LIBBSC)
    if (profile == ProfileId::GRZ)
        return true;
#endif
    return false;
}

ZstdSourceTransferConfig sender_config(const P50RouteOwnerConfig& owner_config,
                                       ProfileId profile,
                                       std::chrono::steady_clock::time_point deadline) {
    ZstdSourceTransferConfig result;
    result.endpoint_caps = owner_config.endpoint_caps;
    result.endpoint_caps.profile = profile;
    result.authority_limits = owner_config.authority_limits;
    result.deadline = deadline;
    result.maximum_duration = owner_config.maximum_duration;
    result.compression_level = owner_config.compression_level;
    return result;
}

}  // namespace

bool P50RouteRelationship::valid() const noexcept {
    return c_store_guid != CStoreGuid{} && f_store_guid != FStoreGuid{} &&
           c_store_guid != f_store_guid && f_store_generation != 0 &&
           supported_profile(profile);
}

P50CRouteOwner::P50CRouteOwner(P50RouteOwnerConfig config)
    : config_(std::move(config)) {}

P50CRouteOwner::~P50CRouteOwner() = default;

ZstdSourceTransferResult P50CRouteOwner::invalid() const noexcept {
    ZstdSourceTransferResult result;
    result.status = ZstdSourceTransferStatus::InvalidRequest;
    return result;
}

P50CRouteOwner::Sender& P50CRouteOwner::get_or_create(
    const P50RouteRelationship& relationship, PrepareRequestKey request,
    std::chrono::steady_clock::time_point deadline) {
    const auto position = owners_.find(relationship);
    if (position != owners_.end())
        return position->second;

    if (!authority_) {
        authority_ = std::make_shared<P50PreparationAuthority>(
            relationship.c_store_guid, config_.endpoint_caps.zstd,
            config_.authority_limits, config_.compression_level,
            config_.endpoint_caps.profile);
    } else if (authority_->c_store_guid() != relationship.c_store_guid) {
        throw std::invalid_argument("route belongs to another C store");
    }
    ZstdSourceTransferConfig config =
        sender_config(config_, relationship.profile, deadline);
    auto sender = std::make_unique<P50ZstdSourceSender>(
        authority_, route_key(relationship), request, std::move(config));
    const auto [inserted, ignored] =
        owners_.emplace(relationship, std::move(sender));
    (void)ignored;
    return inserted->second;
}

PreparationRouteKey P50CRouteOwner::route_key(
    const P50RouteRelationship& relationship) const noexcept {
    return {relationship.f_store_guid, relationship.f_store_generation,
            relationship.profile};
}

boost::asio::awaitable<ZstdSourceTransferResult> P50CRouteOwner::transfer(
    P50RouteRelationship relationship, PrepareRequestKey request,
    boost::asio::ip::tcp::endpoint remote,
    std::chrono::steady_clock::time_point deadline,
    std::span<const uint8_t> source) {
    if (!relationship.valid() ||
        request.producer_session == 0 ||
        request.request_token == 0 || remote.port() == 0 ||
        remote.address().is_unspecified())
        co_return invalid();

    P50ZstdSourceSender* sender = nullptr;
    try {
        sender = get_or_create(relationship, request, deadline).get();
    } catch (const std::invalid_argument&) {
        co_return invalid();
    }
    // The sender retains only this relationship's route view and retry
    // ledger; TU identity is allocated by the shared C authority.
    co_return co_await sender->transfer_route(remote, request, deadline, source);
}

boost::asio::awaitable<ZstdSourceTransferResult> P50CRouteOwner::transfer(
    P50RouteRelationship relationship, PrepareRequestKey request,
    ConnectedFdFactory connection,
    std::chrono::steady_clock::time_point deadline,
    std::span<const uint8_t> source) {
    if (!relationship.valid() ||
        request.producer_session == 0 ||
        request.request_token == 0 || !connection)
        co_return invalid();

    P50ZstdSourceSender* sender = nullptr;
    try {
        sender = get_or_create(relationship, request, deadline).get();
    } catch (const std::invalid_argument&) {
        co_return invalid();
    }
    co_return co_await sender->transfer_route(
        std::move(connection), request, deadline, source);
}

void P50CRouteOwner::reset_f_store(FStoreGuid f_store_guid,
                                   uint64_t new_f_store_generation) noexcept {
    if (f_store_guid == FStoreGuid{} || new_f_store_generation == 0)
        return;
    for (auto position = owners_.begin(); position != owners_.end();) {
        if (position->first.f_store_guid == f_store_guid) {
            const PreparationRouteKey key = route_key(position->first);
            if (!authority_ || authority_->reset_route(key))
                position = owners_.erase(position);
            else
                ++position;
        } else
            ++position;
    }
}

void P50CRouteOwner::reset() noexcept {
    for (auto position = owners_.begin(); position != owners_.end();) {
        const PreparationRouteKey key = route_key(position->first);
        if (!authority_ || authority_->reset_route(key))
            position = owners_.erase(position);
        else
            ++position;
    }
}

bool P50CRouteOwner::owns(
    const P50RouteRelationship& relationship) const noexcept {
    return owners_.find(relationship) != owners_.end();
}

}  // namespace icecc::p50
