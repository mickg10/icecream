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
#include <sys/types.h>

#include "services/digest128.h"

#include "p50_fd_handoff.h"
#include "protocol50.h"

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
    local::Identity lease_identity{};
    FStoreGuid f_store_guid{};
    icecc::Digest128 socket_path_digest{};
    dev_t listener_device = 0;
    ino_t listener_inode = 0;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool current_path_matches() const noexcept;
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

    // Installs immutable lease data only.  No Connection is retained; each
    // decoded CACHE_SESSION creates a fresh one-shot relationship.
    bool set_on_demand_endpoint(OnDemandEndpoint endpoint) noexcept;
    void disable() noexcept;

    [[nodiscard]] bool available() const noexcept;
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
};

} // namespace icecc::p50::daemon
