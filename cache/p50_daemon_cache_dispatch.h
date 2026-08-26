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
#include <vector>
#include <sys/types.h>

#include "services/digest128.h"

#include "p50_fd_handoff.h"
#include "p50_incarnation_identity.h"
#include "p50_phase_open.h"
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
    // True only after FdHandoffReceiver has adopted the descriptor and its
    // exact ACK has been observed by the sender.  This is deliberately
    // separate from `detached`: a release can succeed while the private
    // peer later disconnects before acknowledging adoption.
    bool handoff_acknowledged = false;
    // `release_fd_if_input_empty()` is the ordinary-link trailing-byte
    // barrier.  A phase-open projection may never be emitted without this
    // proof, even if a future caller supplies an arm out of band.
    bool trailing_byte_barrier = false;
};

struct OnDemandEndpoint {
    std::string socket_path;
    local::CredentialExpectation expected_peer;
    local::Identity lease_identity{};
    StoreIdentityRoot store_root{};
    uint64_t store_derivation_version = 0;
    CStoreGuid c_store_guid{};
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

    // Build the exact phase-open envelope only after the one-shot descriptor
    // handoff has been ACKed and the ordinary stream's trailing-byte barrier
    // was proven.  The current CACHE_SESSION wire intentionally carries no
    // P50SourceArm, so callers must supply the complete arm from a later
    // CompileFile/source-arm message; absent that message this remains a
    // fail-closed HOLD rather than synthesizing identity fields.
    std::optional<std::vector<uint8_t>> emit_attachment_phase_open(
        const CacheDispatchOutcome& outcome,
        const icecc::p50::P50SourceArm& source_arm) noexcept;

private:
    static bool valid_identity(local::Identity identity) noexcept;
    CacheDispatchOutcome fail_after_detach(local::FdHandoffStatus status,
                                           local::HandoffRequest request) noexcept;

    local::Identity identity_{};
    uint64_t next_request_id_ = 1;
    std::chrono::milliseconds handoff_timeout_;
    std::optional<OnDemandEndpoint> on_demand_;
    icecc::p50::P50HandoffAuthority phase_authority_;
};

} // namespace icecc::p50::daemon
