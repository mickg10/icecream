#include "p50_daemon_cache_dispatch.h"

#include <limits>
#include <sys/stat.h>
#include <unistd.h>
#include "services/digest128.h"

#include "comm.h"
#include "p50_control_operation.h"
#include "p50_incarnation_identity.h"

namespace icecc::p50::daemon {
namespace {

constexpr uint32_t kCacheSession = 0x50f00000u;

} // namespace

bool OnDemandEndpoint::valid() const noexcept {
    return !socket_path.empty() && lease_identity.generation != 0 &&
           lease_identity.attempt != 0 && f_store_guid != FStoreGuid{} &&
           f_store_guid == f_store_guid_for_incarnation(lease_identity) &&
           socket_path_digest != icecc::Digest128{} && listener_device != 0 &&
           listener_inode != 0 && expected_peer.uid.has_value() &&
           expected_peer.gid.has_value() && expected_peer.pid.has_value() &&
           *expected_peer.pid > 1;
}

bool OnDemandEndpoint::current_path_matches() const noexcept {
    if (!valid() || icecc::digest128(socket_path) != socket_path_digest)
        return false;
    struct stat info{};
    return ::lstat(socket_path.c_str(), &info) == 0 && S_ISSOCK(info.st_mode) &&
           info.st_dev == listener_device && info.st_ino == listener_inode &&
           info.st_uid == ::geteuid() && (info.st_mode & 07777) == 0600;
}

CacheSessionDispatcher::CacheSessionDispatcher(local::Identity identity,
                                               std::chrono::milliseconds handoff_timeout) noexcept
    : identity_(identity), handoff_timeout_(handoff_timeout) {
    if (handoff_timeout_.count() < 0)
        handoff_timeout_ = std::chrono::milliseconds(0);
}

CacheSessionDispatcher::CacheSessionDispatcher(local::Identity identity,
                                               OnDemandEndpoint endpoint,
                                               std::chrono::milliseconds handoff_timeout) noexcept
    : identity_(identity), handoff_timeout_(handoff_timeout), on_demand_(std::move(endpoint)) {
    if (handoff_timeout_.count() < 0)
        handoff_timeout_ = std::chrono::milliseconds(0);
    if (!on_demand_->current_path_matches() ||
        on_demand_->lease_identity != identity_)
        on_demand_.reset();
}

CacheSessionDispatcher::~CacheSessionDispatcher() { disable(); }

bool CacheSessionDispatcher::valid_identity(local::Identity identity) noexcept {
    return identity.generation != 0 && identity.attempt != 0;
}

bool CacheSessionDispatcher::set_on_demand_endpoint(OnDemandEndpoint endpoint) noexcept {
    if (!endpoint.current_path_matches() || !valid_identity(identity_) ||
        endpoint.lease_identity != identity_)
        return false;
    on_demand_ = std::move(endpoint);
    return true;
}

void CacheSessionDispatcher::disable() noexcept {
    on_demand_.reset();
}

bool CacheSessionDispatcher::available() const noexcept {
    if (!valid_identity(identity_))
        return false;
    return on_demand_.has_value() && on_demand_->lease_identity == identity_ &&
           on_demand_->current_path_matches();
}

CacheDispatchOutcome CacheSessionDispatcher::fail_after_detach(
    local::FdHandoffStatus status, local::HandoffRequest request) noexcept {
    // FdHandoffSender has already closed the descriptor on every terminal
    // result.  Dropping the relationship prevents a reconnect/reuse of a
    // partially consumed stream or a stale sidecar generation.
    // The fresh relationship is one-shot and is destroyed by its local owner.
    // The immutable READY lease remains usable for a later TU unless the
    // supervisor withdraws/replaces it.
    return CacheDispatchOutcome{CacheDispatchResult::HandoffFailed, status, request, true,
                                false, true};
}

CacheDispatchOutcome CacheSessionDispatcher::dispatch(MsgChannel& channel,
                                                      int negotiated_protocol,
                                                      uint32_t decoded_type) noexcept {
    if (decoded_type != kCacheSession)
        return CacheDispatchOutcome{CacheDispatchResult::NotCacheSession};
    if (negotiated_protocol != 50)
        return CacheDispatchOutcome{CacheDispatchResult::NotCacheSession};

    // A missing/restarting sidecar fails closed before touching the ordinary
    // link.  The parser retains ownership and any following bytes intact;
    // the daemon's normal teardown closes the ordinary channel.
    if (!available())
        return CacheDispatchOutcome{CacheDispatchResult::SidecarUnavailable};

    if (next_request_id_ == 0 || next_request_id_ == std::numeric_limits<uint64_t>::max()) {
        disable();
        return CacheDispatchOutcome{CacheDispatchResult::InvalidIdentity};
    }

    const local::HandoffRequest request{identity_, next_request_id_++};
    const auto deadline = std::chrono::steady_clock::now() + handoff_timeout_;

    // Product dispatch establishes no relationship before the discriminator
    // is decoded.  Connect, exact peer credentials, HELLO and HELLO_ACK all
    // happen before release_fd_if_input_empty(), under one absolute deadline.
    // Connection's destructor closes this one-shot relationship on every
    // pre-release and post-release terminal path.
    local::Status connect_status = local::Status::Ok;
    local::Connection relationship = local::connect_unix_until(
        on_demand_->socket_path, deadline, &connect_status);
    if (!relationship.valid())
        return CacheDispatchOutcome{CacheDispatchResult::SidecarUnavailable,
                                    local::FdHandoffStatus::Disconnected, request, false};
    if (relationship.verify_peer_credentials(on_demand_->expected_peer) != local::Status::Ok)
        return CacheDispatchOutcome{CacheDispatchResult::SidecarUnavailable,
                                    local::FdHandoffStatus::NotAuthenticated, request, false};
    if (relationship.send_until(local::make_hello(local::PeerRole::Daemon, identity_),
                                deadline) != local::Status::Ok)
        return CacheDispatchOutcome{CacheDispatchResult::SidecarUnavailable,
                                    local::FdHandoffStatus::Disconnected, request, false};
    local::Frame acknowledgement;
    if (relationship.receive_until(acknowledgement, deadline) != local::Status::Ok ||
        local::validate_handshake(acknowledgement, local::MessageType::HelloAck,
                                  local::PeerRole::Sidecar, identity_) != local::Status::Ok)
        return CacheDispatchOutcome{CacheDispatchResult::SidecarUnavailable,
                                    local::FdHandoffStatus::Disconnected, request, false};

    // The real sidecar dispatches by an explicit semantic operation, not by
    // guessing from the following SCM_RIGHTS frame.  Announce the exact
    // request before touching the ordinary descriptor; every pre-release
    // failure therefore leaves MsgChannel as its sole owner.
    const local::Frame operation{
        local::kProtocolVersion, local::MessageType::Data, identity_,
        local::encode_control_operation(
            local::make_cache_session_operation(identity_, request.request_id))};
    if (operation.payload.empty() ||
        relationship.send_until(operation, deadline) != local::Status::Ok)
        return CacheDispatchOutcome{CacheDispatchResult::SidecarUnavailable,
                                    local::FdHandoffStatus::Disconnected, request, false};

    // Authentication proves which process owns the connected peer, but the
    // immutable READY lease also names the listener node that made this
    // incarnation dispatchable. Revalidate that node after the potentially
    // blocking connect/HELLO exchange and immediately before relinquishing
    // the ordinary descriptor. A retired listener can keep an accepted
    // connection alive after its pathname has already been replaced.
    if (!on_demand_->current_path_matches())
        return CacheDispatchOutcome{CacheDispatchResult::SidecarUnavailable,
                                    local::FdHandoffStatus::NotAuthenticated, request, false};

    const int released_fd = channel.release_fd_if_input_empty();
    if (released_fd < 0) {
        // The operation has been announced but the ordinary stream did not
        // prove a clean release boundary.  Drop the relationship so the
        // sidecar cannot wait on or reinterpret a half-announced stream.
        return CacheDispatchOutcome{CacheDispatchResult::ReleaseRefused,
                                    local::FdHandoffStatus::AlreadyConsumed, request, false};
    }

    local::FdHandoffSender sender{local::HandoffFd(released_fd)};
    const local::FdHandoffResult result = sender.send(relationship, request, deadline);
    if (result.status != local::FdHandoffStatus::Accepted)
        return fail_after_detach(result.status, request);

    // `relationship` closes here. Retain the immutable endpoint lease so TU2
    // opens a different authenticated relationship to the same incarnation.
    return CacheDispatchOutcome{CacheDispatchResult::Accepted, result.status, request, true,
                                true, true};
}

std::optional<std::vector<uint8_t>> CacheSessionDispatcher::emit_attachment_phase_open(
    const CacheDispatchOutcome& outcome,
    const icecc::p50::P50SourceArm& source_arm) noexcept {
    // The empty CACHE_SESSION discriminator cannot carry a source arm.  Do
    // not derive one from the handoff identity: C_STORE_GUID, logical job,
    // attempt, and source mode are independent fields and are not present on
    // this wire.  The caller must provide the exact later arm, and this gate
    // still requires both ownership proofs from this very dispatch result.
    if (outcome.result != CacheDispatchResult::Accepted ||
        !outcome.detached || !outcome.handoff_acknowledged ||
        !outcome.trailing_byte_barrier || !source_arm.valid() ||
        source_arm.source_request_id != outcome.request.request_id)
        return std::nullopt;

    const icecc::p50::HandoffOffer offer{
        outcome.request.request_id, source_arm.cache_profile, source_arm};
    const auto offer_decision = phase_authority_.offer(offer);
    if (offer_decision != icecc::p50::OfferDecision::Accepted &&
        offer_decision != icecc::p50::OfferDecision::ExactReplay)
        return std::nullopt;
    const icecc::p50::AttachmentPhaseOpen open{
        outcome.request.request_id, source_arm};
    const auto open_decision = phase_authority_.phase_open(open);
    if (open_decision != icecc::p50::OfferDecision::Accepted &&
        open_decision != icecc::p50::OfferDecision::ExactReplay)
        return std::nullopt;
    const auto wire = icecc::p50::encode_attachment_phase_open(open);
    if (wire.empty())
        return std::nullopt;
    return wire;
}

} // namespace icecc::p50::daemon
