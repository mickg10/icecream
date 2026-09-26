#include "p50_route_owner.h"
#include "services/comm.h"

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace icecc::p50 {
namespace {

bool supported_profile(ProfileId profile) noexcept {
    return profile == ProfileId::P29V1 || profile == ProfileId::ZSTD_TU ||
           profile == ProfileId::ZSTD_ROUTE;
}

uint32_t cache_profile_mask(ProfileId profile) noexcept {
    switch (profile) {
    case ProfileId::P29V1: return CACHE_PROFILE_P29V1;
    case ProfileId::ZSTD_TU: return CACHE_PROFILE_ZSTD_TU;
    case ProfileId::ZSTD_ROUTE: return CACHE_PROFILE_ZSTD_ROUTE;
    }
    return 0;
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
    result.r2_interval_observer = owner_config.r2_interval_observer;
    result.before_prepare_for_route_for_test =
        owner_config.before_prepare_for_route_for_test;
    result.on_r2_background_quiescent = owner_config.post_retired_reap;
    result.hold_r2_receipt_reader_for_test =
        owner_config.hold_r2_receipt_reader_for_test;
    result.hold_r2_ack_pump_for_test = owner_config.hold_r2_ack_pump_for_test;
    result.disconnect_r2_after_bundle_for_test =
        owner_config.disconnect_r2_after_bundle_for_test;
    result.after_r2_bundle_sent_for_test =
        owner_config.after_r2_bundle_sent_for_test;
    result.before_r2_recovery_for_test =
        owner_config.before_r2_recovery_for_test;
    result.before_r2_recovery_attempt_for_test =
        owner_config.before_r2_recovery_attempt_for_test;
    result.after_r2_recovery_receipt_settled_for_test =
        owner_config.after_r2_recovery_receipt_settled_for_test;
    result.disconnect_r2_before_replay_bundle_for_test =
        owner_config.disconnect_r2_before_replay_bundle_for_test;
#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
    result.before_r2_first_bundle_write_for_test =
        owner_config.before_r2_first_bundle_write_for_test;
    result.r2_prewrite_cancelled_for_test =
        owner_config.r2_prewrite_cancelled_for_test;
#endif
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
    retired_senders_.reserve(config_.max_relationships);
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
    result.replacement_trigger = replacement_trigger_;
    return result;
}

void P50CRouteOwner::require_replacement(
    ReplacementTrigger trigger) noexcept {
    if (!replacement_required_)
        replacement_trigger_ = trigger;
    replacement_required_ = true;
}

P50CRouteOwner::Sender& P50CRouteOwner::get_or_create(
    const P50RouteRelationship& relationship, PrepareRequestKey request,
    std::chrono::steady_clock::time_point deadline) {
    reap_retired_senders();
    const auto position = owners_.find(relationship);
    if (position != owners_.end())
        return position->second;
    const PreparationRouteKey requested_route = route_key(relationship);
    for (const auto& retired : retired_senders_) {
        if (retired.abandon_route_when_quiescent &&
            *retired.abandon_route_when_quiescent == requested_route)
            throw std::length_error("R2 route retirement is not quiescent");
    }
    if (owners_.size() + retired_senders_.size() >= config_.max_relationships)
        throw std::length_error("route relationship table is full");

    if (!authority_) {
        PreparationAuthorityLimits authority_limits = config_.authority_limits;
        // R2 is capped at W30 on the physical link. Raising the preparation
        // slot count does not pipeline R1: its endpoint remains W1 and keeps
        // its accepted-commit advancement semantics. The existing raw-byte cap
        // remains the aggregate bound across retained speculative inputs.
        authority_limits.max_speculative_tus = std::max<uint32_t>(
            authority_limits.max_speculative_tus, 30);
        authority_ = std::make_shared<P50PreparationAuthority>(
            relationship.c_store_guid, config_.endpoint_caps.zstd,
            authority_limits, config_.compression_level,
            config_.endpoint_caps.profile, TuSeq{},
            config_.p29_interner_fault_injection);
    } else if (authority_->c_store_guid() != relationship.c_store_guid) {
        throw std::invalid_argument("route belongs to another C store");
    }
    ZstdSourceTransferConfig config =
        sender_config(config_, relationship.profile, deadline);
    auto sender = std::make_shared<P50ZstdSourceSender>(
        authority_, route_key(relationship), request, std::move(config));
    const auto [inserted, ignored] =
        owners_.emplace(relationship, std::move(sender));
    (void)ignored;
    return inserted->second;
}

void P50CRouteOwner::reap_retired_senders() noexcept {
    for (auto position = retired_senders_.begin();
         position != retired_senders_.end();) {
        if (!position->sender || position->sender.use_count() != 1) {
            ++position;
            continue;
        }
        if (position->abandon_route_when_quiescent &&
            (!authority_ || !authority_->abandon_retired_route(
                                *position->abandon_route_when_quiescent))) {
            ++position;
            continue;
        }
        position = retired_senders_.erase(position);
        if (config_.after_retired_route_reaped_for_test) {
            try {
                config_.after_retired_route_reaped_for_test();
            } catch (...) {
                // A test observer must not change bounded owner cleanup.
            }
        }
    }
}

#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ == 13
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
boost::asio::awaitable<bool>
P50CRouteOwner::wait_for_retired_route_quiescence(
    PreparationRouteKey route,
    std::chrono::steady_clock::time_point deadline) {
    const auto executor = co_await boost::asio::this_coro::executor;
    const auto pending = [&] {
        return std::any_of(
            retired_senders_.begin(), retired_senders_.end(),
            [&](const RetiredSender& retired) {
                return retired.abandon_route_when_quiescent &&
                       *retired.abandon_route_when_quiescent == route;
            });
    };
    while (pending()) {
        reap_retired_senders();
        if (!pending())
            co_return true;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            co_return false;
        boost::asio::steady_timer timer(executor);
        timer.expires_at(std::min(
            deadline, now + std::chrono::milliseconds(2)));
        co_await timer.async_wait(boost::asio::use_awaitable);
    }
    co_return true;
}

boost::asio::awaitable<bool>
P50CRouteOwner::wait_for_sender_rebind_quiescence(
    const Sender& sender,
    std::chrono::steady_clock::time_point deadline) {
    const auto executor = co_await boost::asio::this_coro::executor;
    bool observed_wait = false;
    while (sender && !sender->can_rebind_r2_relationship()) {
        // Only a fully settled link whose remaining work is ACK completion (or
        // the just-finished caller unwinding) may be waited out here. A live
        // bundle, receipt, recovery, or unrelated writer waiter is a real
        // overlap and must leave the old relationship untouched.
        if (!sender->r2_rebind_waitable())
            co_return false;
        if (!observed_wait && config_.after_r2_rebind_wait_for_test) {
            observed_wait = true;
            try {
                config_.after_r2_rebind_wait_for_test();
            } catch (...) {
                // A fixture observer cannot affect route transition policy.
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            co_return false;
        boost::asio::steady_timer timer(executor);
        timer.expires_at(std::min(
            deadline, now + std::chrono::milliseconds(2)));
        co_await timer.async_wait(boost::asio::use_awaitable);
    }
    co_return sender && sender->can_rebind_r2_relationship();
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

    Sender sender;
    try {
        sender = get_or_create(relationship, request, deadline);
    } catch (const std::invalid_argument&) {
        co_return invalid();
    } catch (const std::length_error&) {
        require_replacement(ReplacementTrigger::Unattributed);
        co_return replacement();
    }
    // The sender retains only this relationship's route view and retry
    // ledger; TU identity is allocated by the shared C authority.
    ZstdSourceTransferResult result =
        co_await sender->transfer_route(remote, request, deadline, source);
    if (result.replacement_required && !result.route_local_failure) {
        require_replacement(result.replacement_trigger);
        result.replacement_trigger = replacement_trigger_;
    }
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

    Sender sender;
    try {
        sender = get_or_create(relationship, request, deadline);
    } catch (const std::invalid_argument&) {
        co_return invalid();
    } catch (const std::length_error&) {
        require_replacement(ReplacementTrigger::Unattributed);
        co_return replacement();
    }
    ZstdSourceTransferResult result = co_await sender->transfer_route(
        std::move(connection), request, deadline, source);
    if (result.replacement_required && !result.route_local_failure) {
        require_replacement(result.replacement_trigger);
        result.replacement_trigger = replacement_trigger_;
    }
    co_return result;
}

boost::asio::awaitable<ZstdSourceTransferResult> P50CRouteOwner::transfer(
    P50RouteRelationship relationship, PrepareRequestKey request,
    AsyncConnectedFdFactory connection,
    std::chrono::steady_clock::time_point deadline,
    std::span<const uint8_t> source) {
    if (!relationship.valid() || request.producer_session == 0 ||
        request.request_token == 0 || !connection ||
        source.size() > config_.endpoint_caps.zstd.max_raw_bytes)
        co_return invalid();
    if (replacement_required_)
        co_return replacement();

    Sender sender;
    try {
        sender = get_or_create(relationship, request, deadline);
    } catch (const std::invalid_argument&) {
        co_return invalid();
    } catch (const std::length_error&) {
        require_replacement(ReplacementTrigger::Unattributed);
        co_return replacement();
    }
    ZstdSourceTransferResult result = co_await sender->transfer_route(
        std::move(connection), request, deadline, source);
    if (result.replacement_required && !result.route_local_failure) {
        require_replacement(result.replacement_trigger);
        result.replacement_trigger = replacement_trigger_;
    }
    co_return result;
}

boost::asio::awaitable<ZstdSourceTransferResult> P50CRouteOwner::transfer_p51(
    P50RouteRelationship relationship, P51SourceArmedFields armed,
    AsyncConnectedFdFactory connection, PrepareRequestKey request,
    std::chrono::steady_clock::time_point deadline,
    std::span<const uint8_t> source,
    std::shared_ptr<P51RequestCancellation> cancellation) {
    if (!relationship.valid() || !armed.valid() || !connection ||
        request.producer_session != armed.arm.source.assignment_epoch ||
        request.request_token != armed.arm.source.assignment_nonce ||
        relationship.c_store_guid.bytes != armed.arm.source.c_store_guid ||
        relationship.f_store_guid.bytes != armed.f_store_guid ||
        relationship.f_store_generation != armed.f_store_generation ||
        (relationship.profile != ProfileId::P29V1 &&
         relationship.profile != ProfileId::ZSTD_TU &&
         relationship.profile != ProfileId::ZSTD_ROUTE))
        co_return invalid();
    if (armed.arm.source.cache_profile != cache_profile_mask(relationship.profile) ||
        source.size() > config_.endpoint_caps.zstd.max_raw_bytes)
        co_return invalid();
    if (replacement_required_)
        co_return replacement();
    const auto incarnation = std::make_tuple(
        relationship.c_store_guid, relationship.f_store_guid,
        relationship.f_store_generation);
    const Id128 logical_relationship_id{armed.logical_relationship_id};
    if (logical_relationship_id == Id128{} || armed.relationship_epoch == 0)
        co_return invalid();
    const PreparationRouteKey preparation_route = route_key(relationship);
    if (!co_await wait_for_retired_route_quiescence(preparation_route,
                                                    deadline)) {
        ZstdSourceTransferResult unavailable;
        unavailable.status = ZstdSourceTransferStatus::Unavailable;
        unavailable.profile = relationship.profile;
        unavailable.route_local_failure = true;
        co_return unavailable;
    }
    auto logical_position = p51_link_identities_.find(relationship);
    if (logical_position != p51_link_identities_.end() &&
        logical_position->second.relationship_id != logical_relationship_id) {
        // A newly authenticated ARM for the same exact C/F/profile route may
        // replace an idle relationship only at a strictly newer epoch. Fence
        // its sender before codec authority is discarded; the old callers and
        // detached reader/ACK pumps retain shared ownership until they unwind.
        if (armed.relationship_epoch <=
            logical_position->second.relationship_epoch)
            co_return invalid();
        const P51LinkIdentity prior_identity = logical_position->second;
        auto old_sender = owners_.find(relationship);
        auto generation_position = physical_generations_.find(relationship);
        if (old_sender == owners_.end() || !old_sender->second ||
            generation_position == physical_generations_.end() ||
            retired_senders_.size() >= config_.max_relationships)
            co_return invalid();
        Sender old_sender_keepalive = old_sender->second;
        if (!old_sender_keepalive->can_rebind_r2_relationship() &&
            !co_await wait_for_sender_rebind_quiescence(old_sender_keepalive,
                                                         deadline))
            co_return invalid();

        // The await above permits another request on this owner to make
        // progress. Revalidate every route-local assumption before fencing
        // the prior sender; stale waiters may not retire a successor.
        logical_position = p51_link_identities_.find(relationship);
        old_sender = owners_.find(relationship);
        generation_position = physical_generations_.find(relationship);
        if (logical_position != p51_link_identities_.end() &&
            logical_position->second.relationship_id == logical_relationship_id &&
            logical_position->second.relationship_epoch >=
                armed.relationship_epoch) {
            // Another waiter installed this exact authenticated successor
            // while we were suspended on ACK-only quiescence. Release our old
            // sender reference before waiting for its route cleanup so it
            // cannot prevent the first waiter from reaping that route.
            old_sender_keepalive.reset();
            if (!co_await wait_for_retired_route_quiescence(preparation_route,
                                                            deadline)) {
                ZstdSourceTransferResult unavailable;
                unavailable.status = ZstdSourceTransferStatus::Unavailable;
                unavailable.profile = relationship.profile;
                unavailable.route_local_failure = true;
                co_return unavailable;
            }
            logical_position = p51_link_identities_.find(relationship);
            if (logical_position == p51_link_identities_.end() ||
                logical_position->second.relationship_id !=
                    logical_relationship_id ||
                logical_position->second.relationship_epoch <
                    armed.relationship_epoch)
                co_return invalid();
        } else {
            if (logical_position == p51_link_identities_.end() ||
                logical_position->second.relationship_id !=
                    prior_identity.relationship_id ||
                logical_position->second.relationship_epoch !=
                    prior_identity.relationship_epoch ||
                old_sender == owners_.end() ||
                old_sender->second != old_sender_keepalive ||
                generation_position == physical_generations_.end() ||
                !old_sender_keepalive->can_rebind_r2_relationship() ||
                retired_senders_.size() >= config_.max_relationships)
                co_return invalid();
            const uint64_t prior_generation =
                old_sender_keepalive->current_r2_physical_generation();
            if (prior_generation == UINT64_MAX)
                co_return invalid();
            const uint64_t successor_generation = std::max(
                next_physical_generation_, prior_generation + 1);
            if (successor_generation == 0 ||
                successor_generation == UINT64_MAX)
                co_return invalid();

            old_sender_keepalive->retire_for_replacement();
            retired_senders_.push_back(
                {std::move(old_sender_keepalive), preparation_route});
            owners_.erase(old_sender);
            generation_position->second = successor_generation;
            next_physical_generation_ = successor_generation + 1;
            logical_position->second =
                P51LinkIdentity{logical_relationship_id,
                                armed.relationship_epoch};
            if (!co_await wait_for_retired_route_quiescence(preparation_route,
                                                            deadline)) {
                ZstdSourceTransferResult unavailable;
                unavailable.status = ZstdSourceTransferStatus::Unavailable;
                unavailable.profile = relationship.profile;
                unavailable.route_local_failure = true;
                co_return unavailable;
            }
            logical_position = p51_link_identities_.find(relationship);
            if (logical_position == p51_link_identities_.end() ||
                logical_position->second.relationship_id !=
                    logical_relationship_id ||
                logical_position->second.relationship_epoch <
                    armed.relationship_epoch)
                co_return invalid();
        }
    }
    if (logical_position != p51_link_identities_.end() &&
        logical_position->second.relationship_id != logical_relationship_id)
        co_return invalid();
    const auto profile_position = p51_incarnation_profiles_.find(incarnation);
    if (profile_position != p51_incarnation_profiles_.end() &&
        profile_position->second != relationship.profile)
        co_return invalid();
    Sender sender;
    uint64_t physical_generation = 0;
    try {
        if (profile_position == p51_incarnation_profiles_.end())
            p51_incarnation_profiles_.emplace(incarnation,
                                               relationship.profile);
        sender = get_or_create(relationship, request, deadline);
        auto position = physical_generations_.find(relationship);
        if (position == physical_generations_.end()) {
            if (next_physical_generation_ == 0 ||
                next_physical_generation_ == UINT64_MAX)
                throw std::overflow_error("physical R2 link generation exhausted");
            position = physical_generations_.emplace(
                relationship, next_physical_generation_++).first;
        }
        physical_generation = position->second;
        const uint64_t sender_generation =
            sender->current_r2_physical_generation();
        if (sender_generation == UINT64_MAX)
            throw std::overflow_error("physical R2 link generation exhausted");
        if (sender_generation != 0) {
            physical_generation = std::max(physical_generation,
                                           sender_generation);
            position->second = physical_generation;
            if (next_physical_generation_ <= sender_generation)
                next_physical_generation_ = sender_generation + 1;
        }
        if (logical_position == p51_link_identities_.end())
            p51_link_identities_.emplace(
                relationship,
                P51LinkIdentity{logical_relationship_id,
                                armed.relationship_epoch});
        else
            logical_position->second.relationship_epoch = std::max(
                logical_position->second.relationship_epoch,
                armed.relationship_epoch);
    } catch (const std::invalid_argument&) {
        if (owners_.find(relationship) == owners_.end())
            p51_incarnation_profiles_.erase(incarnation);
        co_return invalid();
    } catch (const std::length_error&) {
        if (owners_.find(relationship) == owners_.end())
            p51_incarnation_profiles_.erase(incarnation);
        ZstdSourceTransferResult unavailable;
        unavailable.status = ZstdSourceTransferStatus::Unavailable;
        unavailable.profile = relationship.profile;
        unavailable.route_local_failure = true;
        co_return unavailable;
    } catch (const std::overflow_error&) {
        if (owners_.find(relationship) == owners_.end())
            p51_incarnation_profiles_.erase(incarnation);
        ZstdSourceTransferResult unavailable;
        unavailable.status = ZstdSourceTransferStatus::Unavailable;
        unavailable.profile = relationship.profile;
        unavailable.route_local_failure = true;
        co_return unavailable;
    } catch (...) {
        if (owners_.find(relationship) == owners_.end())
            p51_incarnation_profiles_.erase(incarnation);
        if (owners_.find(relationship) == owners_.end())
            p51_link_identities_.erase(relationship);
        require_replacement(ReplacementTrigger::RouteOwnerAdmissionException);
        co_return replacement();
    }
    ZstdSourceTransferResult result = co_await sender->transfer_p51_route(
        std::move(armed), physical_generation, std::move(connection), request,
        deadline, source, std::move(cancellation));
    const auto current = owners_.find(relationship);
    bool still_current = current != owners_.end() &&
                         current->second == sender;
    if (result.r2_link_rejection) {
        const LinkHello& offered = result.r2_link_rejection->offered;
        bool retired = false;
        if (result.r2_link_rejection->reason ==
            LinkRejectReason::StoreReplaced) {
            // The offered F identity is the old one; a valid typed response
            // proves the server is now a different incarnation.
            if (offered.f_store_guid == relationship.f_store_guid &&
                offered.f_store_generation == relationship.f_store_generation)
                retired = retire_f_store_exact_p51(
                    relationship.f_store_guid,
                    relationship.f_store_generation);
        } else if (result.r2_link_rejection->reason ==
                   LinkRejectReason::ReservationMissing) {
            if (offered.c_store_guid == relationship.c_store_guid &&
                offered.f_store_guid == relationship.f_store_guid &&
                offered.f_store_generation == relationship.f_store_generation)
                retired = retire_relationship_exact_p51(
                    relationship, sender.get(), offered.relationship_id,
                    offered.relationship_epoch,
                    offered.physical_link_generation);
        }
        if (!retired) {
            // Do not re-open the rejected sender. This is local to its exact
            // R2 route; unrelated C/F relationships remain usable.
            result.route_local_failure = true;
            result.replacement_required = false;
        } else {
            still_current = false;
        }
    }
    if (!still_current && result.status != ZstdSourceTransferStatus::Committed) {
        // A verified old receipt remains positive evidence, but a failed
        // operation on a retired F incarnation is route-local. It must not
        // poison the C process or its unrelated F relationships.
        result.replacement_required = false;
        result.route_local_failure = true;
    }
    if (result.replacement_required && !result.route_local_failure) {
        require_replacement(result.replacement_trigger);
        result.replacement_trigger = replacement_trigger_;
    }
    sender.reset();
    reap_retired_senders();
    co_return result;
}
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ == 13
#pragma GCC diagnostic pop
#endif

bool P50CRouteOwner::reset_f_store_exact(
    FStoreGuid old_f_store_guid,
    uint64_t old_f_store_generation) noexcept {
    reap_retired_senders();
    if (old_f_store_guid == FStoreGuid{} || old_f_store_generation == 0)
        return false;
    bool reset = true;
    for (auto position = owners_.begin(); position != owners_.end();) {
        if (position->first.f_store_guid == old_f_store_guid &&
            position->first.f_store_generation == old_f_store_generation) {
            const PreparationRouteKey key = route_key(position->first);
            const bool route_reset = !authority_ || authority_->reset_route(key);
            if (route_reset) {
                p51_incarnation_profiles_.erase(std::make_tuple(
                    position->first.c_store_guid,
                    position->first.f_store_guid,
                    position->first.f_store_generation));
                physical_generations_.erase(position->first);
                p51_link_identities_.erase(position->first);
                if (position->second.use_count() > 1) {
                    position->second->retire_for_replacement();
                    retired_senders_.push_back(
                        {std::move(position->second), std::nullopt});
                }
                position = owners_.erase(position);
            } else {
                size_t live_entries = 0;
                size_t route_history_entries = 0;
                try {
                    if (authority_) {
                        live_entries = authority_->live_entry_count();
                        route_history_entries =
                            authority_->route_history_entries(key);
                    }
                } catch (...) {
                }
                std::fprintf(stderr,
                    "P51_ROUTE_RESET_BLOCKED profile=%u c=%02x%02x f=%02x%02x/%llu live_entries=%zu route_history=%zu\n",
                    static_cast<unsigned>(position->first.profile),
                    static_cast<unsigned>(position->first.c_store_guid.bytes[0]),
                    static_cast<unsigned>(position->first.c_store_guid.bytes[1]),
                    static_cast<unsigned>(position->first.f_store_guid.bytes[0]),
                    static_cast<unsigned>(position->first.f_store_guid.bytes[1]),
                    static_cast<unsigned long long>(
                        position->first.f_store_generation),
                    live_entries, route_history_entries);
                std::fflush(stderr);
                reset = false;
                ++position;
            }
        } else
            ++position;
    }
    if (!reset)
        require_replacement(ReplacementTrigger::Unattributed);
    return reset;
}

bool P50CRouteOwner::retire_f_store_exact_p51(
    FStoreGuid old_f_store_guid,
    uint64_t old_f_store_generation) noexcept {
    reap_retired_senders();
    if (old_f_store_guid == FStoreGuid{} || old_f_store_generation == 0)
        return false;

    // Do not let an R2 replacement silently retire a legacy R1 route which
    // happens to use the same F store identity.
    for (const auto& [relationship, sender] : owners_) {
        (void)sender;
        if (relationship.f_store_guid == old_f_store_guid &&
            relationship.f_store_generation == old_f_store_generation &&
            !p51_incarnation_profiles_.contains(std::make_tuple(
                relationship.c_store_guid, relationship.f_store_guid,
                relationship.f_store_generation)))
            return false;
    }

    for (auto position = owners_.begin(); position != owners_.end();) {
        if (position->first.f_store_guid != old_f_store_guid ||
            position->first.f_store_generation != old_f_store_generation) {
            ++position;
            continue;
        }
        if (retired_senders_.size() >= config_.max_relationships)
            return false;

        const P50RouteRelationship relationship = position->first;
        const PreparationRouteKey key = route_key(relationship);
        Sender sender = position->second;
        if (!sender)
            return false;
        sender->retire_for_replacement();
        // Detach the exact sender before attempting codec-state cleanup.
        // Cleanup may need a later owner turn if a pump/caller still owns the
        // sender; a typed F replacement must not leave that fenced sender in
        // the active map or globally poison unrelated relationships.
        retired_senders_.push_back({std::move(position->second), key});
        position = owners_.erase(position);
        sender.reset();
        reap_retired_senders();
        physical_generations_.erase(relationship);
        p51_link_identities_.erase(relationship);
        p51_incarnation_profiles_.erase(std::make_tuple(
            relationship.c_store_guid, relationship.f_store_guid,
            relationship.f_store_generation));
    }
    // A repeated typed rejection after the predecessor was already retired is
    // idempotent; the new incarnation has no matching owner to erase.
    return true;
}

bool P50CRouteOwner::retire_relationship_exact_p51(
    const P50RouteRelationship& relationship,
    const P50ZstdSourceSender* expected_sender, Id128 relationship_id,
    uint64_t relationship_epoch,
    uint64_t physical_link_generation) noexcept {
    reap_retired_senders();
    if (!relationship.valid() || !expected_sender ||
        relationship_id == Id128{} ||
        relationship_epoch == 0 || physical_link_generation == 0)
        return false;
    const auto sender_position = owners_.find(relationship);
    const auto identity_position = p51_link_identities_.find(relationship);
    const auto generation_position = physical_generations_.find(relationship);
    if (sender_position == owners_.end() ||
        identity_position == p51_link_identities_.end() ||
        generation_position == physical_generations_.end() ||
        sender_position->second.get() != expected_sender ||
        identity_position->second.relationship_id != relationship_id ||
        !p51_incarnation_profiles_.contains(std::make_tuple(
            relationship.c_store_guid, relationship.f_store_guid,
            relationship.f_store_generation)))
        return false;
    if (retired_senders_.size() >= config_.max_relationships)
        return false;

    // Recovery increments the physical generation inside the sender, so the
    // route-owner's next-allocation counter can lag the exact offer which was
    // just rejected. Keep same-F relationship reuse strictly above that
    // validated attempted generation; UINT64_MAX is a permanent local
    // exhaustion marker and is never wrapped.
    if (physical_link_generation >= next_physical_generation_) {
        next_physical_generation_ =
            physical_link_generation == UINT64_MAX
                ? UINT64_MAX
                : physical_link_generation + 1;
    }

    const PreparationRouteKey key = route_key(relationship);
    Sender sender = sender_position->second;
    if (!sender)
        return false;
    sender->retire_for_replacement();
    // As above, owner-map retirement is immediate even when exact authority
    // cleanup must wait for the final pump/caller reference to drain.
    retired_senders_.push_back(
        {std::move(sender_position->second), key});
    owners_.erase(sender_position);
    sender.reset();
    reap_retired_senders();
    physical_generations_.erase(relationship);
    p51_link_identities_.erase(relationship);
    p51_incarnation_profiles_.erase(std::make_tuple(
        relationship.c_store_guid, relationship.f_store_guid,
        relationship.f_store_generation));
    return true;
}

void P50CRouteOwner::reset() noexcept {
    reap_retired_senders();
    for (auto position = owners_.begin(); position != owners_.end();) {
        const PreparationRouteKey key = route_key(position->first);
        if (!authority_ || authority_->reset_route(key))
        {
            p51_incarnation_profiles_.erase(std::make_tuple(
                position->first.c_store_guid, position->first.f_store_guid,
                position->first.f_store_generation));
            physical_generations_.erase(position->first);
            p51_link_identities_.erase(position->first);
            if (position->second.use_count() > 1) {
                position->second->retire_for_replacement();
                retired_senders_.push_back(
                    {std::move(position->second), std::nullopt});
            }
            position = owners_.erase(position);
        }
        else
            ++position;
    }
}

void P50CRouteOwner::cancel_active_p51_transfers() noexcept {
    for (const auto& [relationship, sender] : owners_) {
        (void)relationship;
        if (sender)
            sender->retire_for_replacement();
    }
    for (const auto& retired : retired_senders_) {
        if (retired.sender)
            retired.sender->retire_for_replacement();
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
