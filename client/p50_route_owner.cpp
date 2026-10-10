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
    result.reopen = owner_config.reopen;
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

    // The shared authority is pinned to the one real base C-store GUID
    // (seeded by SidecarRuntime, or lazily by the first transfer, before any
    // lane is ever used). A ROUTE_LANES lane-derived GUID must never reach
    // ensure_authority() -- only the registered-lane membership check below
    // sees it. With no lanes registered (default), this check is exactly
    // the pre-lanes equality test: byte-identical default behavior.
    if (authority_) {
        if (relationship.c_store_guid != authority_->c_store_guid() &&
            !lane_guids_.contains(relationship.c_store_guid))
            throw std::invalid_argument("route belongs to another C store");
    } else {
        ensure_authority(relationship.c_store_guid);
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

void P50CRouteOwner::ensure_authority(CStoreGuid c_store_guid) {
    if (!authority_) {
        authority_ = std::make_shared<P50PreparationAuthority>(
            c_store_guid, config_.endpoint_caps.zstd,
            config_.authority_limits, config_.compression_level,
            config_.endpoint_caps.profile, TuSeq{},
            config_.p29_interner_fault_injection);
    } else if (authority_->c_store_guid() != c_store_guid) {
        throw std::invalid_argument("route belongs to another C store");
    }
}

std::shared_ptr<P50PreparationAuthority> P50CRouteOwner::authority(
    CStoreGuid c_store_guid) {
    ensure_authority(c_store_guid);
    return authority_;
}

void P50CRouteOwner::register_lane(CStoreGuid lane_guid) {
    lane_guids_.insert(lane_guid);
}

PreparationRouteKey P50CRouteOwner::route_key(
    const P50RouteRelationship& relationship) const noexcept {
    // relationship.c_store_guid carries the lane GUID (or the base GUID at
    // lane 0), so each lane gets its own preparation-route entry -- its own
    // "one uncommitted successor" slot -- inside the one shared authority.
    return {relationship.c_store_guid, relationship.f_store_guid,
            relationship.f_store_generation, relationship.profile};
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
    if (result.replacement_required && !result.route_local_failure)
        replacement_required_ = true;
    co_return result;
}

boost::asio::awaitable<ZstdSourceTransferResult> P50CRouteOwner::transfer(
    P50RouteRelationship relationship, PrepareRequestKey request,
    ConnectedFdFactory connection,
    std::chrono::steady_clock::time_point deadline,
    std::span<const uint8_t> source) {
    co_return co_await transfer(
        relationship, request, std::move(connection), deadline,
        p50::P50SourceView::own(std::vector<uint8_t>(source.begin(), source.end())));
}

boost::asio::awaitable<ZstdSourceTransferResult> P50CRouteOwner::transfer(
    P50RouteRelationship relationship, PrepareRequestKey request,
    ConnectedFdFactory connection,
    std::chrono::steady_clock::time_point deadline,
    std::shared_ptr<const p50::P50SourceView> source,
    std::optional<Digest128> raw_digest,
    std::shared_ptr<std::atomic<ClientStallStage>> stall_stage,
    std::function<void()> write_complete) {
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
        std::move(connection), request, deadline, source, raw_digest,
        std::move(stall_stage), std::move(write_complete));
    if (result.replacement_required && !result.route_local_failure)
        replacement_required_ = true;
    co_return result;
}

// --- Design note: safe force-discard of a superseded F incarnation --------
//
// Problem: every non-Committed exit of P50ZstdSourceSender::transfer_bytes
// (RetryExhausted, TerminalError, DeadlineExceeded) leaves its
// PreparedTuHandle un-released. The orphaned Entry makes
// P50PreparationAuthority::reset_route(key) return false forever, so once
// SidecarRuntime::bind_route_endpoint_identity (cache/p50_cache_service.cpp)
// observes a new (guid, generation) at the same host:port,
// reset_f_store_exact() below never clears the old relationship and the
// whole C sidecar gets replaced -- for every later route, not just the one
// that actually failed.
//
// Safety argument (why no old-incarnation history can reach the new one):
// PreparationRouteKey embeds f_store_generation, so the predecessor's route
// key and the successor's route key are structurally different map keys in
// impl_->routes / impl_->entries (cache/p50_endpoint.cpp). Each
// P50RouteRelationship maps 1:1 to exactly one P50ZstdSourceSender (owners_
// below) and exactly one PreparationRouteKey (route_key()). Only that one
// sender could ever have created an Entry under the old key, so
// force-erasing every Entry still filed under the old key cannot touch the
// new incarnation's entries, any other relationship's entries, or any other
// route's RouteState. discard_route() is scoped to a single route_key and is
// therefore safe to call once that key's F incarnation is known dead.
//
// In-flight argument (why the whole reset is gated on transfer_in_flight,
// checked before either reset_route or discard_route is even attempted):
// erasing owners_'s entry for a relationship destroys its Sender (and the
// Impl any of its coroutine frames access via `this`). That is only safe
// once nothing can resume into it. An Entry can be missing or already
// released for two different reasons: (1) the sender's transfer already
// reached a terminal result (Committed and released, or non-Committed and
// orphaned) -- in both cases the Sender is quiescent and safe to erase --
// or (2) some transfer_bytes() coroutine of that sender is still running
// or suspended and may not have created, or may already have released, its
// Entry at this exact instant, racing this reset. transfer_in_flight()
// distinguishes the two regardless of Entry state, so case (2) is always
// refused up front, before reset_route()/discard_route() are consulted.
// With route pipelining (K>1, P50_ROUTE_PIPELINING.md step 4c) a sender
// can have TWO transfers overlapped (head + successor on the same route),
// so transfer_in_flight() is a nonzero-COUNT test ("any transfer in
// flight"), not a single bool; see the InFlightGuard counter in
// client/p50_zstd_sender.cpp. Case (2) should be rare to impossible here:
// per the class comment above, "the owner is single-threaded ... callers
// serialize operations for one relationship on their compiler-wrapper
// owner" (lanes=1), and a pipelined route is forced to one lane; and
// bind_route_endpoint_identity() only calls reset_f_store_exact()
// between assignments, never nested inside one of its own transfer()
// calls. When in-flight is nonetheless observed, this function keeps
// today's behavior: return false and let the caller set
// replacement_required_.
//
// Unchanged: reset_route() (same-incarnation path; protects lost-commit
// reconcile) and P50CRouteOwner::reset() (full teardown). discard_route()
// touches the uncommitted_route_entries successor deque the same uniform
// way release() and the pipelining abandon path already do (clearing the
// whole outstanding-successor chain, with at most one CRoute::
// abandon_active() call per discard_route() invocation); the perf-pipe
// deque conversion is exactly this one field in cache/p50_endpoint.cpp.
// ---------------------------------------------------------------------------
bool P50CRouteOwner::reset_f_store_exact(
    FStoreGuid old_f_store_guid,
    uint64_t old_f_store_generation) noexcept {
    if (old_f_store_guid == FStoreGuid{} || old_f_store_generation == 0)
        return false;
    bool reset = true;
    for (auto position = owners_.begin(); position != owners_.end();) {
        if (position->first.f_store_guid != old_f_store_guid ||
            position->first.f_store_generation != old_f_store_generation) {
            ++position;
            continue;
        }
        if (position->second && position->second->transfer_in_flight()) {
            reset = false;
            ++position;
            continue;
        }
        const PreparationRouteKey key = route_key(position->first);
        if (!authority_ || authority_->reset_route(key) ||
            authority_->discard_route(key))
            position = owners_.erase(position);
        else {
            reset = false;
            ++position;
        }
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
