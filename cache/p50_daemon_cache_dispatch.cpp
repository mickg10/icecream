#include "p50_daemon_cache_dispatch.h"

#include <cstdio>
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
constexpr uint32_t kP51CacheLinkSession = 0x51f00012u;

} // namespace

bool OnDemandEndpoint::valid() const noexcept {
    return !socket_path.empty() && lease_identity.generation != 0 &&
           lease_identity.attempt != 0 &&
           f_store_generation != 0 &&
           store_root.valid() &&
           store_derivation_version == kStoreIdentityDerivationVersion &&
           store_identity_guid_valid_for_role(c_store_guid.bytes,
                                              kStoreIdentityClientRole) &&
           store_identity_guid_valid_for_role(f_store_guid.bytes,
                                              kStoreIdentityFileRole) &&
           c_store_guid != f_store_guid &&
           c_store_guid == c_store_guid_for_root(store_root) &&
           f_store_guid == f_store_guid_for_root(store_root) &&
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
    const bool r1_session = decoded_type == kCacheSession &&
                            protocol_supports_p50_r1_bridge(
                                negotiated_protocol);
    const bool r2_link = decoded_type == kP51CacheLinkSession &&
                         protocol_supports_cache_r2(negotiated_protocol) &&
                         p50_cache_revision_from_environment(
                             PROTOCOL_VERSION) == CACHE_WIRE_REVISION_R2;
    if (!r1_session && !r2_link)
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
    const auto started_at = std::chrono::steady_clock::now();
    const auto deadline = started_at + handoff_timeout_;
    const auto diagnose_r2 = [&](const char *stage, local::Status status) {
        if (!r2_link)
            return;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at).count();
        std::fprintf(stderr,
            "P51_CACHE_LINK_DISPATCH_FAIL request=%llu stage=%s status=%u "
            "elapsed_ms=%lld budget_ms=%lld\n",
            static_cast<unsigned long long>(request.request_id), stage,
            static_cast<unsigned>(status), static_cast<long long>(elapsed),
            static_cast<long long>(handoff_timeout_.count()));
        std::fflush(stderr);
    };

    // Product dispatch establishes no relationship before the discriminator
    // is decoded.  Connect, exact peer credentials, HELLO and HELLO_ACK all
    // happen before release_fd_if_input_empty(), under one absolute deadline.
    // Connection's destructor closes this one-shot relationship on every
    // pre-release and post-release terminal path.
    local::Status connect_status = local::Status::Ok;
    local::Connection relationship = local::connect_unix_until(
        on_demand_->socket_path, deadline, &connect_status);
    if (!relationship.valid()) {
        diagnose_r2("connect", connect_status);
        return CacheDispatchOutcome{CacheDispatchResult::SidecarUnavailable,
                                    local::FdHandoffStatus::Disconnected, request, false};
    }
    // The supervisor pre-binds the listener before forking the sidecar.  On
    // Linux SO_PEERCRED consequently reports the daemon that created the
    // socket, rather than the child (or worker) that accepts it.  Keep the
    // READY child PID binding, but also accept that one exact creator PID;
    // UID/GID and the immutable pathname lease remain mandatory in either
    // case.  A service-created listener still takes the direct child-PID
    // path, while a pre-bound listener takes the creator-PID path.
    bool peer_authenticated =
        relationship.verify_peer_credentials(on_demand_->expected_peer) ==
        local::Status::Ok;
    if (!peer_authenticated) {
        // Re-run through the transport verifier so the Connection records
        // authentication for the subsequent SCM_RIGHTS sender as well.
        // Merely querying credentials here would leave that state unset and
        // make an otherwise valid handoff fail as NotAuthenticated.
        local::CredentialExpectation creator_expectation = on_demand_->expected_peer;
        creator_expectation.pid = static_cast<uint64_t>(::getpid());
        peer_authenticated =
            relationship.verify_peer_credentials(creator_expectation) ==
            local::Status::Ok;
    }
    if (!peer_authenticated) {
        diagnose_r2("peer-credentials", local::Status::PeerCredentialMismatch);
        return CacheDispatchOutcome{CacheDispatchResult::SidecarUnavailable,
                                    local::FdHandoffStatus::NotAuthenticated, request, false};
    }
    const local::Status hello_send_status = relationship.send_until(
        local::make_hello(local::PeerRole::Daemon, identity_), deadline);
    if (hello_send_status != local::Status::Ok) {
        diagnose_r2("hello-send", hello_send_status);
        return CacheDispatchOutcome{CacheDispatchResult::SidecarUnavailable,
                                    local::FdHandoffStatus::Disconnected, request, false};
    }
    local::Frame acknowledgement;
    const local::Status hello_receive_status =
        relationship.receive_until(acknowledgement, deadline);
    if (hello_receive_status != local::Status::Ok) {
        diagnose_r2("hello-ack-receive", hello_receive_status);
        return CacheDispatchOutcome{CacheDispatchResult::SidecarUnavailable,
                                    local::FdHandoffStatus::Disconnected, request, false};
    }
    const local::Status hello_validate_status = local::validate_handshake(
        acknowledgement, local::MessageType::HelloAck,
        local::PeerRole::Sidecar, identity_);
    if (hello_validate_status != local::Status::Ok) {
        diagnose_r2("hello-ack-validate", hello_validate_status);
        return CacheDispatchOutcome{CacheDispatchResult::SidecarUnavailable,
                                    local::FdHandoffStatus::Disconnected, request, false};
    }

    // The real sidecar dispatches by an explicit semantic operation, not by
    // guessing from the following SCM_RIGHTS frame.  Announce the exact
    // request before touching the ordinary descriptor; every pre-release
    // failure therefore leaves MsgChannel as its sole owner.
    const local::ControlOperation control_operation = r2_link
        ? local::make_cache_link_session_operation(identity_, request.request_id)
        : local::make_cache_session_operation(identity_, request.request_id);
    const local::Frame operation{
        local::kProtocolVersion, local::MessageType::Data, identity_,
        local::encode_control_operation(control_operation)};
    const local::Status operation_status = operation.payload.empty()
        ? local::Status::Malformed
        : relationship.send_until(operation, deadline);
    if (operation.payload.empty() || operation_status != local::Status::Ok) {
        diagnose_r2("operation-send", operation_status);
        return CacheDispatchOutcome{CacheDispatchResult::SidecarUnavailable,
                                    local::FdHandoffStatus::Disconnected, request, false};
    }

    // Authentication proves which process owns the connected peer, but the
    // immutable READY lease also names the listener node that made this
    // incarnation dispatchable. Revalidate that node after the potentially
    // blocking connect/HELLO exchange and immediately before relinquishing
    // the ordinary descriptor. A retired listener can keep an accepted
    // connection alive after its pathname has already been replaced.
    if (!on_demand_->current_path_matches()) {
        diagnose_r2("listener-path", local::Status::IdentityMismatch);
        return CacheDispatchOutcome{CacheDispatchResult::SidecarUnavailable,
                                    local::FdHandoffStatus::NotAuthenticated, request, false};
    }

    const int released_fd = r2_link
        ? channel.send_p51_cache_link_session_ready_and_release(deadline)
        : channel.release_fd_if_input_empty();
    if (released_fd < 0) {
        diagnose_r2("ordinary-release", local::Status::Malformed);
        // The operation has been announced but the ordinary stream did not
        // prove a clean release boundary.  Drop the relationship so the
        // sidecar cannot wait on or reinterpret a half-announced stream.
        return CacheDispatchOutcome{CacheDispatchResult::ReleaseRefused,
                                    local::FdHandoffStatus::AlreadyConsumed, request, false};
    }

    local::FdHandoffSender sender{local::HandoffFd(released_fd)};
    const local::FdHandoffResult result = sender.send(relationship, request, deadline);
    if (result.status != local::FdHandoffStatus::Accepted) {
        diagnose_r2("descriptor-handoff", local::Status::IoError);
        if (r2_link) {
            std::fprintf(stderr,
                "P51_CACHE_LINK_HANDOFF_FAIL request=%llu status=%u\n",
                static_cast<unsigned long long>(request.request_id),
                static_cast<unsigned>(result.status));
            std::fflush(stderr);
        }
        return fail_after_detach(result.status, request);
    }

    // `relationship` closes here. Retain the immutable endpoint lease so TU2
    // opens a different authenticated relationship to the same incarnation.
    return CacheDispatchOutcome{CacheDispatchResult::Accepted, result.status, request, true,
                                true, true};
}

} // namespace icecc::p50::daemon
