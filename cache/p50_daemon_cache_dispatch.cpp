#include "p50_daemon_cache_dispatch.h"

#include <limits>
#include "services/digest128.h"

#include "comm.h"
#include "p50_control_operation.h"

namespace icecc::p50::daemon {
namespace {

constexpr uint32_t kCacheSession = 0x50f00000u;
constexpr int kHandshakeTimeoutMilliseconds = 250;

} // namespace

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
    if (!on_demand_->valid() ||
        (on_demand_->lease_identity.has_value() && *on_demand_->lease_identity != identity_) ||
        (on_demand_->socket_path_digest.has_value() &&
         *on_demand_->socket_path_digest != icecc::digest128(on_demand_->socket_path)))
        on_demand_.reset();
}

CacheSessionDispatcher::~CacheSessionDispatcher() { disable(); }

bool CacheSessionDispatcher::valid_identity(local::Identity identity) noexcept {
    return identity.generation != 0 && identity.attempt != 0;
}

bool CacheSessionDispatcher::attach_authenticated(local::Connection connection,
                                                   local::Identity identity) noexcept {
    if (!connection.valid() || !connection.peer_credentials_verified() ||
        !valid_identity(identity) || identity != identity_) {
        return false;
    }

    // Credential authentication alone does not bind a socket to the current
    // supervised sidecar incarnation.  Complete the private HELLO exchange on
    // this very connection before it can become the descriptor-handoff owner.
    // The caller cannot replace identity_ with a stale/non-current value.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kHandshakeTimeoutMilliseconds);
    if (connection.send_until(local::make_hello(local::PeerRole::Daemon, identity_), deadline) !=
        local::Status::Ok) {
        return false;
    }
    local::Frame acknowledgement;
    if (connection.receive_until(acknowledgement, deadline) !=
            local::Status::Ok ||
        local::validate_handshake(acknowledgement,
                                  local::MessageType::HelloAck,
                                  local::PeerRole::Sidecar,
                                  identity_) != local::Status::Ok) {
        return false;
    }
    sidecar_.reset();
    on_demand_.reset();
    sidecar_.emplace(std::move(connection));
    return true;
}

bool CacheSessionDispatcher::set_on_demand_endpoint(OnDemandEndpoint endpoint) noexcept {
    if (!endpoint.valid() || !valid_identity(identity_) ||
        (endpoint.lease_identity.has_value() && *endpoint.lease_identity != identity_) ||
        (endpoint.socket_path_digest.has_value() &&
         *endpoint.socket_path_digest != icecc::digest128(endpoint.socket_path)))
        return false;
    sidecar_.reset();
    on_demand_ = std::move(endpoint);
    return true;
}

void CacheSessionDispatcher::disable() noexcept {
    sidecar_.reset();
    on_demand_.reset();
}

CacheDispatchOutcome CacheSessionDispatcher::fail_after_detach(
    local::FdHandoffStatus status, local::HandoffRequest request) noexcept {
    // FdHandoffSender has already closed the descriptor on every terminal
    // result.  Dropping the relationship prevents a reconnect/reuse of a
    // partially consumed stream or a stale sidecar generation.
    disable();
    return CacheDispatchOutcome{CacheDispatchResult::HandoffFailed, status, request, true};
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
    std::optional<local::Connection> fresh;
    if (on_demand_.has_value()) {
        local::Status connect_status = local::Status::Ok;
        local::Connection connection = local::connect_unix_until(
            on_demand_->socket_path, deadline, &connect_status);
        if (!connection.valid())
            return CacheDispatchOutcome{CacheDispatchResult::SidecarUnavailable,
                                        local::FdHandoffStatus::Disconnected, request, false};
        if (connection.verify_peer_credentials(on_demand_->expected_peer) != local::Status::Ok)
            return CacheDispatchOutcome{CacheDispatchResult::SidecarUnavailable,
                                        local::FdHandoffStatus::NotAuthenticated, request, false};
        if (connection.send_until(local::make_hello(local::PeerRole::Daemon, identity_),
                                  deadline) != local::Status::Ok)
            return CacheDispatchOutcome{CacheDispatchResult::SidecarUnavailable,
                                        local::FdHandoffStatus::Disconnected, request, false};
        local::Frame acknowledgement;
        if (connection.receive_until(acknowledgement, deadline) != local::Status::Ok ||
            local::validate_handshake(acknowledgement, local::MessageType::HelloAck,
                                      local::PeerRole::Sidecar, identity_) != local::Status::Ok)
            return CacheDispatchOutcome{CacheDispatchResult::SidecarUnavailable,
                                        local::FdHandoffStatus::Disconnected, request, false};
        fresh.emplace(std::move(connection));
    }

    local::Connection* relationship =
        fresh.has_value() ? &*fresh : &*sidecar_;

    // Establish the semantic operation before touching the ordinary link.
    // If this frame cannot be sent, the ordinary descriptor remains owned by
    // MsgChannel and the caller can tear it down without a mixed stream.  A
    // fresh relationship closes locally; a retained legacy checkpoint
    // relationship must also be retired because it consumed an operation.
    const local::Frame operation{
        local::kProtocolVersion, local::MessageType::Data, identity_,
        local::encode_control_operation(
            local::make_cache_session_operation(identity_, request.request_id))};
    if (operation.payload.empty() ||
        relationship->send_until(operation, deadline) != local::Status::Ok) {
        if (!fresh.has_value())
            disable();
        return CacheDispatchOutcome{CacheDispatchResult::SidecarUnavailable,
                                    local::FdHandoffStatus::Disconnected, request, false};
    }

    const int released_fd = channel.release_fd_if_input_empty();
    if (released_fd < 0) {
        // The operation has been announced but the ordinary stream did not
        // prove a clean release boundary.  Drop the relationship so the
        // sidecar cannot wait on or reinterpret a half-announced stream.
        if (!fresh.has_value())
            disable();
        return CacheDispatchOutcome{CacheDispatchResult::ReleaseRefused,
                                    local::FdHandoffStatus::AlreadyConsumed, request, false};
    }

    local::FdHandoffSender sender{local::HandoffFd(released_fd)};
    const local::FdHandoffResult result = sender.send(*relationship, request, deadline);
    if (result.status != local::FdHandoffStatus::Accepted)
        return fail_after_detach(result.status, request);

    // The sidecar receiver is deliberately one-shot.  A later CACHE_SESSION
    // must arrive with a newly authenticated relationship and cannot reuse a
    // control stream after a successful handoff.
    disable();
    return CacheDispatchOutcome{CacheDispatchResult::Accepted, result.status, request, true};
}

} // namespace icecc::p50::daemon
