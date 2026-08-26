#pragma once

// Bounded daemon-side bridge from an exactly decoded Protocol-50
// CACHE_SESSION to the already-authenticated sidecar control relationship.
// This class owns neither a listener nor cache bytes: the ordinary link must
// first prove the MsgChannel release boundary, then ownership moves exactly
// once into FdHandoffSender.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include "services/digest128.h"

#include "p50_fd_handoff.h"

class MsgChannel;

namespace icecc::p50::daemon {

enum class CacheDispatchResult : uint8_t {
    Accepted = 0,
    NotCacheSession,
    SidecarUnavailable,
    InvalidIdentity,
    ReleaseRefused,
    HandoffFailed,
};

struct CacheDispatchOutcome {
    CacheDispatchResult result = CacheDispatchResult::NotCacheSession;
    local::FdHandoffStatus handoff_status = local::FdHandoffStatus::InvalidArgument;
    local::HandoffRequest request{};
    // True once MsgChannel relinquished its descriptor.  Every terminal
    // failure after this point has already closed or transferred that fd.
    bool detached = false;
};

struct OnDemandEndpoint {
    std::string socket_path;
    local::CredentialExpectation expected_peer;
    std::optional<local::Identity> lease_identity;
    std::optional<icecc::Digest128> socket_path_digest;

    [[nodiscard]] bool valid() const noexcept {
        return !socket_path.empty() && expected_peer.uid.has_value() &&
               expected_peer.gid.has_value() && expected_peer.pid.has_value();
    }
};

class CacheSessionDispatcher {
public:
    explicit CacheSessionDispatcher(
        local::Identity identity,
        std::chrono::milliseconds handoff_timeout = std::chrono::milliseconds(250)) noexcept;
    CacheSessionDispatcher(local::Identity identity, OnDemandEndpoint endpoint,
                           std::chrono::milliseconds handoff_timeout =
                               std::chrono::milliseconds(250)) noexcept;
    ~CacheSessionDispatcher();

    CacheSessionDispatcher(const CacheSessionDispatcher&) = delete;
    CacheSessionDispatcher& operator=(const CacheSessionDispatcher&) = delete;
    CacheSessionDispatcher(CacheSessionDispatcher&&) = delete;
    CacheSessionDispatcher& operator=(CacheSessionDispatcher&&) = delete;

    // Takes ownership only of an OS-credential-authenticated private
    // relationship whose HELLO_ACK proves the constructor-bound generation
    // and attempt.  An unauthenticated, stale, or malformed connection is
    // closed and not retained.
    bool attach_authenticated(local::Connection connection,
                              local::Identity identity) noexcept;
    // Installs immutable lease data only.  No Connection is retained; each
    // decoded CACHE_SESSION creates a fresh one-shot relationship.
    bool set_on_demand_endpoint(OnDemandEndpoint endpoint) noexcept;
    void disable() noexcept;

    [[nodiscard]] bool available() const noexcept {
        return (on_demand_.has_value() ? on_demand_->valid()
                                       : sidecar_.has_value() && sidecar_->valid() &&
                                             sidecar_->peer_credentials_verified()) &&
               valid_identity(identity_);
    }
    [[nodiscard]] local::Identity identity() const noexcept { return identity_; }
    [[nodiscard]] uint64_t next_request_id() const noexcept { return next_request_id_; }

    // `decoded_type` must be the result of the immediately preceding
    // MsgChannel::get_msg().  The release call is made only for the exact
    // Protocol-50 empty CACHE_SESSION discriminator; no cache byte is read.
    CacheDispatchOutcome dispatch(MsgChannel& channel, int negotiated_protocol,
                                  uint32_t decoded_type) noexcept;

private:
    static bool valid_identity(local::Identity identity) noexcept;
    CacheDispatchOutcome fail_after_detach(local::FdHandoffStatus status,
                                           local::HandoffRequest request) noexcept;

    local::Identity identity_{};
    uint64_t next_request_id_ = 1;
    std::chrono::milliseconds handoff_timeout_;
    std::optional<OnDemandEndpoint> on_demand_;
    // Kept only for old checkpoint tests.  The configured product path never
    // consults or retains this relationship.
    std::optional<local::Connection> sidecar_;
};

} // namespace icecc::p50::daemon
