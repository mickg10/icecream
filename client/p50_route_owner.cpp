#include "p50_route_owner.h"

#include <stdexcept>
#include <utility>

namespace icecc::p50 {
namespace {

bool supported_profile(ProfileId profile) noexcept {
    return profile == ProfileId::P29V1 || profile == ProfileId::ZSTD_TU ||
           profile == ProfileId::ZSTD_ROUTE;
}

ZstdSourceTransferConfig sender_config(const P50RouteOwnerConfig& owner_config,
                                       ProfileId profile,
                                       std::chrono::steady_clock::time_point deadline) {
    ZstdSourceTransferConfig result;
    result.endpoint_caps = owner_config.endpoint_caps;
    result.endpoint_caps.profile = profile;
    result.authority_limits = owner_config.authority_limits;
    result.max_completed_requests = owner_config.max_completed_requests;
    result.deadline = deadline;
    result.maximum_duration = owner_config.maximum_duration;
    result.compression_level = owner_config.compression_level;
    result.before_prepare_for_route_for_test =
        owner_config.before_prepare_for_route_for_test;
    return result;
}

}  // namespace

bool P50RouteRelationship::valid() const noexcept {
    return c_store_guid != CStoreGuid{} && f_store_guid != FStoreGuid{} &&
           c_store_guid != f_store_guid && f_store_generation != 0 &&
           supported_profile(profile);
}

P50CRouteOwner::P50CRouteOwner(P50RouteOwnerConfig config)
    : config_(std::move(config)) {
    if (config_.max_completed_requests == 0)
        throw std::invalid_argument("route completed-request limit is zero");
    if (config_.max_relationships == 0)
        throw std::invalid_argument("route relationship limit is zero");
}

P50CRouteOwner::~P50CRouteOwner() = default;

ZstdSourceTransferResult P50CRouteOwner::invalid() const noexcept {
    ZstdSourceTransferResult result;
    result.status = ZstdSourceTransferStatus::InvalidRequest;
    return result;
}

ZstdSourceTransferResult P50CRouteOwner::replacement() const noexcept {
    ZstdSourceTransferResult result;
    result.status = ZstdSourceTransferStatus::Unavailable;
    result.replacement_required = true;
    return result;
}

P50CRouteOwner::Sender& P50CRouteOwner::get_or_create(
    const P50RouteRelationship& relationship, PrepareRequestKey request,
    std::chrono::steady_clock::time_point deadline) {
    const auto position = owners_.find(relationship);
    if (position != owners_.end())
        return position->second;
    if (owners_.size() >= config_.max_relationships)
        throw std::length_error("route relationship table is full");

    if (!authority_) {
        authority_ = std::make_shared<P50PreparationAuthority>(
            relationship.c_store_guid, config_.endpoint_caps.zstd,
            config_.authority_limits, config_.compression_level,
            config_.endpoint_caps.profile, TuSeq{},
            config_.p29_interner_fault_injection);
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
    if (replacement_required_)
        co_return replacement();

    P50ZstdSourceSender* sender = nullptr;
    try {
        sender = get_or_create(relationship, request, deadline).get();
    } catch (const std::invalid_argument&) {
        co_return invalid();
    } catch (const std::length_error&) {
        replacement_required_ = true;
        co_return replacement();
    }
    // The sender retains only this relationship's route view and retry
    // ledger; TU identity is allocated by the shared C authority.
    ZstdSourceTransferResult result =
        co_await sender->transfer_route(remote, request, deadline, source);
    if (result.replacement_required)
        replacement_required_ = true;
    co_return result;
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
    if (replacement_required_)
        co_return replacement();

    P50ZstdSourceSender* sender = nullptr;
    try {
        sender = get_or_create(relationship, request, deadline).get();
    } catch (const std::invalid_argument&) {
        co_return invalid();
    } catch (const std::length_error&) {
        replacement_required_ = true;
        co_return replacement();
    }
    ZstdSourceTransferResult result = co_await sender->transfer_route(
        std::move(connection), request, deadline, source);
    if (result.replacement_required)
        replacement_required_ = true;
    co_return result;
}

bool P50CRouteOwner::reset_f_store_exact(
    FStoreGuid old_f_store_guid,
    uint64_t old_f_store_generation) noexcept {
    if (old_f_store_guid == FStoreGuid{} || old_f_store_generation == 0)
        return false;
    bool reset = true;
    for (auto position = owners_.begin(); position != owners_.end();) {
        if (position->first.f_store_guid == old_f_store_guid &&
            position->first.f_store_generation == old_f_store_generation) {
            const PreparationRouteKey key = route_key(position->first);
            if (!authority_ || authority_->reset_route(key))
                position = owners_.erase(position);
            else {
                reset = false;
                ++position;
            }
        } else
            ++position;
    }
    if (!reset)
        replacement_required_ = true;
    return reset;
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

#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
bool P50CRouteOwner::seed_relationship_for_test(
    const P50RouteRelationship& relationship) noexcept {
    if (!relationship.valid() || replacement_required_)
        return false;
    if (owns(relationship))
        return true;
    try {
        (void)get_or_create(
            relationship, PrepareRequestKey{1, 1},
            std::chrono::steady_clock::now() + std::chrono::seconds(1));
        return owns(relationship);
    } catch (...) {
        return false;
    }
}
#endif

}  // namespace icecc::p50
