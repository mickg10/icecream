#pragma once

// Sidecar-local reducer for the post-adoption P5CO frame.
//
// This is intentionally only a reducer contract.  The current tree has no
// reviewed typed server-release/client-adopted socket lease, so this layer
// does not accept or manufacture an fd (or an fd-like integer).  A future
// handoff implementation supplies the move-only lease below.

#include "p50_sidecar_supervisor.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <poll.h>

namespace icecc::p50::sidecar {

enum class P5coWriterState : uint8_t {
    Ready = 0,
    Writing,
    FullyFlushed,
    FailedAfterDetach,
};

enum class P5coFailure : uint8_t {
    None = 0,
    InvalidInput,
    Expired,
    ClockDomainMismatch,
    OwnershipLost,
    TerminalEvent,
    SendError,
    EndpointStartExpired,
};

enum class P5coWriteKind : uint8_t {
    Sent = 0,
    WouldBlock,
    Interrupted,
    Error,
};

struct P5coWriteResult {
    P5coWriteKind kind = P5coWriteKind::Error;
    size_t bytes = 0;
};

// These flags are part of the lease contract so the concrete retained socket
// can map them to send(MSG_DONTWAIT | MSG_NOSIGNAL).  The reducer never asks
// the lease for a raw descriptor and never changes shared O_NONBLOCK state.
enum class P5coSendFlag : uint8_t {
    DontWait = 1u << 0,
    NoSignal = 1u << 1,
};

constexpr uint8_t operator|(P5coSendFlag left, P5coSendFlag right) noexcept {
    return static_cast<uint8_t>(left) | static_cast<uint8_t>(right);
}

// The eventual typed handoff implementation owns this object.  Move-only
// ownership is represented by unique_ptr; no raw-fd authority is introduced
// by this standalone reducer.
class P5coAdoptedSocketLease {
public:
    virtual ~P5coAdoptedSocketLease() = default;
    P5coAdoptedSocketLease(const P5coAdoptedSocketLease&) = delete;
    P5coAdoptedSocketLease& operator=(const P5coAdoptedSocketLease&) = delete;

    [[nodiscard]] virtual bool revalidate(
        const local::Identity& operation_identity) const noexcept = 0;
    virtual P5coWriteResult send_nonblocking(
        std::span<const uint8_t> bytes, uint8_t flags) noexcept = 0;
    // Fence/cancel the exact post-detach operation.  This is called once on
    // every terminal writer failure and must not make the relationship
    // reusable.
    virtual void fence() noexcept = 0;

protected:
    P5coAdoptedSocketLease() = default;
};

class AdoptedOutcomeWriter {
public:
    struct Limits {
        // One syscall per advance is deliberate: readiness is advisory and
        // every later syscall gets a fresh deadline/identity check.
        size_t max_bytes_per_advance = 4096;
    };

    AdoptedOutcomeWriter(
        std::unique_ptr<P5coAdoptedSocketLease> lease,
        std::vector<uint8_t> canonical_p5co,
        local::Identity operation_identity,
        AbsoluteMonotonicDeadline deadline) noexcept;
    AdoptedOutcomeWriter(
        std::unique_ptr<P5coAdoptedSocketLease> lease,
        std::vector<uint8_t> canonical_p5co,
        local::Identity operation_identity,
        AbsoluteMonotonicDeadline deadline,
        Limits limits) noexcept;
    ~AdoptedOutcomeWriter() = default;

    AdoptedOutcomeWriter(const AdoptedOutcomeWriter&) = delete;
    AdoptedOutcomeWriter& operator=(const AdoptedOutcomeWriter&) = delete;
    AdoptedOutcomeWriter(AdoptedOutcomeWriter&&) noexcept = default;
    AdoptedOutcomeWriter& operator=(AdoptedOutcomeWriter&&) noexcept = default;

    // At most one bounded send is attempted.  `now_ns` and `clock` must be
    // observations from CLOCK_MONOTONIC in the proved sidecar domain.
    P5coWriterState advance(int64_t now_ns,
                            const MonotonicClockIdentity& clock,
                            short revents) noexcept;

    // Transfers the same retained lease to the endpoint only after the full
    // P5CO frame has flushed and while the original deadline remains valid.
    // A failed/expired writer never returns a lease.
    [[nodiscard]] std::unique_ptr<P5coAdoptedSocketLease>
    take_for_endpoint(int64_t now_ns,
                      const MonotonicClockIdentity& clock) noexcept;

    [[nodiscard]] P5coWriterState state() const noexcept { return state_; }
    [[nodiscard]] P5coFailure failure() const noexcept { return failure_; }
    [[nodiscard]] size_t offset() const noexcept { return offset_; }
    [[nodiscard]] size_t last_bytes() const noexcept { return last_bytes_; }
    [[nodiscard]] size_t last_syscalls() const noexcept { return last_syscalls_; }
    [[nodiscard]] const AbsoluteMonotonicDeadline& deadline() const noexcept {
        return deadline_;
    }
    [[nodiscard]] std::span<const uint8_t> canonical_frame() const noexcept {
        return canonical_p5co_;
    }

private:
    void fail(P5coFailure reason) noexcept;
    bool deadline_valid(int64_t now_ns,
                        const MonotonicClockIdentity& clock) const noexcept;

    std::unique_ptr<P5coAdoptedSocketLease> lease_;
    std::vector<uint8_t> canonical_p5co_;
    local::Identity operation_identity_{};
    AbsoluteMonotonicDeadline deadline_{};
    Limits limits_{};
    P5coWriterState state_ = P5coWriterState::FailedAfterDetach;
    P5coFailure failure_ = P5coFailure::InvalidInput;
    size_t offset_ = 0;
    size_t last_bytes_ = 0;
    size_t last_syscalls_ = 0;
    bool fenced_ = false;
};

} // namespace icecc::p50::sidecar
