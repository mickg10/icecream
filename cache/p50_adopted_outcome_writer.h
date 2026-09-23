#pragma once

// Sidecar-local reducer for the post-adoption P5CO frame.
//
// The reducer itself never accepts or manufactures an fd (or an fd-like
// integer).  The concrete retained-socket implementation supplies the
// move-only lease below, and only P50ServerEndpoint may perform its final
// descriptor release while consuming the complete authority bundle.

#include "p50_sidecar_identity.h"
#include "../services/p50_cache_session_wire.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <poll.h>

namespace icecc::p50 {
class P50ServerEndpoint;
}

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
    ObservationFailed,
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

struct MonotonicObservation {
    int64_t now_ns = 0;
    MonotonicClockIdentity clock{};

    [[nodiscard]] bool valid() const noexcept {
        return now_ns >= 0 && clock.valid();
    }
};

// The source is move-only by ownership.  A production instance samples
// CLOCK_MONOTONIC; tests inject a deterministic source with one fresh sample
// per observation call.
class MonotonicObservationSource {
public:
    virtual ~MonotonicObservationSource() = default;
    MonotonicObservationSource(const MonotonicObservationSource&) = delete;
    MonotonicObservationSource& operator=(const MonotonicObservationSource&) = delete;

    [[nodiscard]] virtual std::optional<MonotonicObservation>
    observe() noexcept = 0;

protected:
    MonotonicObservationSource() = default;
};

class SystemMonotonicObservationSource final : public MonotonicObservationSource {
public:
    [[nodiscard]] std::optional<MonotonicObservation> observe() noexcept override;
};

// The eventual typed handoff implementation owns this object.  Move-only
// ownership is represented by unique_ptr; no raw-fd authority is introduced
// by this standalone reducer.
class P5coAdoptedSocketLease {
public:
    virtual ~P5coAdoptedSocketLease() = default;
    P5coAdoptedSocketLease(const P5coAdoptedSocketLease&) = delete;
    P5coAdoptedSocketLease& operator=(const P5coAdoptedSocketLease&) = delete;

    // Revalidation binds the exact complete P5CO value and its original
    // absolute deadline.  A lease for another claim/launch/store/operation
    // must not be able to emit this writer's bytes.
    [[nodiscard]] virtual bool revalidate(
        const daemon::P50CacheSessionOutcome& exact_outcome,
        const AbsoluteMonotonicDeadline& exact_deadline) const noexcept = 0;
    virtual P5coWriteResult send_nonblocking(
        std::span<const uint8_t> bytes, uint8_t flags) noexcept = 0;
    // Fence/cancel the exact post-detach operation.  This is called once on
    // every terminal writer failure and when an owned writer is destroyed.
    virtual void fence() noexcept = 0;

protected:
    P5coAdoptedSocketLease() = default;

private:
    friend class ::icecc::p50::P50ServerEndpoint;

    // The native descriptor may cross this final boundary only while the
    // endpoint consumes the complete P5coEndpointHandoff.  This deliberately
    // has no public wrapper: callers cannot strip socket authority from the
    // exact outcome and original deadline.  Standalone reducer fakes retain
    // the fail-closed default.
    [[nodiscard]] virtual int release_native_fd_for_endpoint(
        const daemon::P50CacheSessionOutcome&,
        const AbsoluteMonotonicDeadline&) noexcept {
        return -1;
    }
};

// The endpoint must receive the complete authority bundle, not only a socket
// pointer.  It is move-only and fences an unconsumed retained lease on
// destruction.  Only P50ServerEndpoint may perform the one-shot internal
// lease transfer; callers cannot separate socket authority from the exact
// outcome and original deadline.
class P5coEndpointHandoff {
public:
    P5coEndpointHandoff(const P5coEndpointHandoff&) = delete;
    P5coEndpointHandoff& operator=(const P5coEndpointHandoff&) = delete;
    P5coEndpointHandoff(P5coEndpointHandoff&& other) noexcept;
    P5coEndpointHandoff& operator=(P5coEndpointHandoff&& other) noexcept;
    ~P5coEndpointHandoff() noexcept;

    [[nodiscard]] bool valid() const noexcept {
        return lease_ && outcome_.valid() && deadline_.valid();
    }
    [[nodiscard]] const daemon::P50CacheSessionOutcome& outcome() const noexcept {
        return outcome_;
    }
    [[nodiscard]] const AbsoluteMonotonicDeadline& deadline() const noexcept {
        return deadline_;
    }
private:
    friend class AdoptedOutcomeWriter;
    friend class ::icecc::p50::P50ServerEndpoint;
    P5coEndpointHandoff(std::unique_ptr<P5coAdoptedSocketLease> lease,
                        daemon::P50CacheSessionOutcome outcome,
                        AbsoluteMonotonicDeadline deadline) noexcept;
    [[nodiscard]] std::unique_ptr<P5coAdoptedSocketLease>
    take_lease_for_endpoint() noexcept;
    void fence_owned() noexcept;

    std::unique_ptr<P5coAdoptedSocketLease> lease_;
    daemon::P50CacheSessionOutcome outcome_;
    AbsoluteMonotonicDeadline deadline_{};
    bool fenced_ = false;
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
        daemon::P50CacheSessionOutcome outcome,
        std::unique_ptr<MonotonicObservationSource> observations,
        AbsoluteMonotonicDeadline deadline) noexcept;
    AdoptedOutcomeWriter(
        std::unique_ptr<P5coAdoptedSocketLease> lease,
        daemon::P50CacheSessionOutcome outcome,
        std::unique_ptr<MonotonicObservationSource> observations,
        AbsoluteMonotonicDeadline deadline,
        Limits limits) noexcept;
    ~AdoptedOutcomeWriter() noexcept;

    AdoptedOutcomeWriter(const AdoptedOutcomeWriter&) = delete;
    AdoptedOutcomeWriter& operator=(const AdoptedOutcomeWriter&) = delete;
    AdoptedOutcomeWriter(AdoptedOutcomeWriter&& other) noexcept;
    AdoptedOutcomeWriter& operator=(AdoptedOutcomeWriter&& other) noexcept;

    // At most one bounded send is attempted.  Each call samples the owned
    // CLOCK_MONOTONIC observation source; callers cannot supply or renew time.
    P5coWriterState advance(short revents) noexcept;

    // Transfers the complete authority bundle only after the full P5CO frame
    // has flushed and while the original deadline remains valid.
    [[nodiscard]] std::optional<P5coEndpointHandoff>
    take_for_endpoint() noexcept;

    [[nodiscard]] P5coWriterState state() const noexcept { return state_; }
    [[nodiscard]] P5coFailure failure() const noexcept { return failure_; }
    [[nodiscard]] size_t offset() const noexcept { return offset_; }
    [[nodiscard]] size_t last_bytes() const noexcept { return last_bytes_; }
    [[nodiscard]] size_t last_syscalls() const noexcept { return last_syscalls_; }
    [[nodiscard]] const AbsoluteMonotonicDeadline& deadline() const noexcept {
        return deadline_;
    }
    [[nodiscard]] const daemon::P50CacheSessionOutcome& outcome() const noexcept {
        return outcome_;
    }
    [[nodiscard]] std::span<const uint8_t> canonical_frame() const noexcept {
        return canonical_p5co_;
    }

private:
    void fence_owned() noexcept;
    void fail(P5coFailure reason) noexcept;
    bool deadline_valid(const MonotonicObservation& observation) const noexcept;

    std::unique_ptr<P5coAdoptedSocketLease> lease_;
    daemon::P50CacheSessionOutcome outcome_;
    std::vector<uint8_t> canonical_p5co_;
    std::unique_ptr<MonotonicObservationSource> observations_;
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
