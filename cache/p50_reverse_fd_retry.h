#pragma once

// Bounded reverse (F -> C) sealed-input delivery.  This is deliberately a
// transport/reducer seam: it does not create a listener, fork a compiler, or
// call CompileFile.  The endpoint owner stages one immutable master memfd;
// each attempt duplicates that master and passes the duplicate over the wire.

#include "p50_fd_handoff.h"
#include "p50_source_identity.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace icecc::p50 {

using DeliveryId = uint64_t;
using DeliveryToken = uint64_t;

inline constexpr uint32_t kMaxReverseFdRemainingMs = 3'600'000;

struct ReverseFdAttempt {
    local::Identity identity{};
    DeliveryId delivery_id = 0;
    DeliveryToken token = 0;
    uint32_t remaining_ms = 0;
    P50SourceArm arm{};

    [[nodiscard]] bool valid() const noexcept;
    auto operator<=>(const ReverseFdAttempt&) const = default;
};

enum class ReverseFdWireType : uint16_t { Attempt = 1, Ack = 2, Nack = 3 };

enum class ReverseFdDecision : uint8_t {
    Invalid = 0,
    Accepted,
    ExactReplay,
    Stale,
    Conflict,
    WrongArm,
    WrongIdentity,
    Expired,
    Cancelled,
    PhaseViolation,
    Malformed,
    IoError,
};

const char* reverse_fd_decision_name(ReverseFdDecision value) noexcept;

// The attempt codec is host-independent.  It carries remaining_ms only; an
// absolute steady_clock::time_point is intentionally not serializable here.
std::vector<uint8_t> encode_reverse_fd_attempt(const ReverseFdAttempt& attempt);
std::optional<ReverseFdAttempt> decode_reverse_fd_attempt(
    std::span<const uint8_t> wire);

struct ReverseFdMaster {
    ReverseFdMaster() noexcept = default;
    explicit ReverseFdMaster(local::HandoffFd fd) noexcept : fd_(std::move(fd)) {}

    ReverseFdMaster(ReverseFdMaster&&) noexcept = default;
    ReverseFdMaster& operator=(ReverseFdMaster&&) noexcept = default;
    ReverseFdMaster(const ReverseFdMaster&) = delete;
    ReverseFdMaster& operator=(const ReverseFdMaster&) = delete;

    [[nodiscard]] bool valid() const noexcept { return fd_.valid(); }
    [[nodiscard]] bool cloexec() const noexcept { return fd_.cloexec(); }
    [[nodiscard]] int get() const noexcept { return fd_.get(); }
    void reset() noexcept { fd_.reset(); }

private:
    friend class ReverseFdOwner;
    local::HandoffFd fd_;
};

// Owns one staged master and one immutable retry identity/deadline.  Move
// assignment is replacement: the previous master is closed before the new
// owner is installed.
class ReverseFdOwner {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    ReverseFdOwner() noexcept = default;
    ReverseFdOwner(ReverseFdMaster master, ReverseFdAttempt identity,
                   TimePoint original_deadline) noexcept;
    ~ReverseFdOwner() = default;
    ReverseFdOwner(ReverseFdOwner&&) noexcept = default;
    ReverseFdOwner& operator=(ReverseFdOwner&&) noexcept = default;
    ReverseFdOwner(const ReverseFdOwner&) = delete;
    ReverseFdOwner& operator=(const ReverseFdOwner&) = delete;

    // Linux memfd + all required seals.  The supplied deadline is the one
    // deadline retained for every retry; it is never replaced by a retry.
    static std::optional<ReverseFdOwner> stage(
        std::span<const uint8_t> bytes, ReverseFdAttempt identity,
        TimePoint original_deadline) noexcept;

    [[nodiscard]] bool valid() const noexcept { return master_.valid() && identity_.valid(); }
    [[nodiscard]] bool cloexec() const noexcept { return master_.cloexec(); }
    [[nodiscard]] bool expired(TimePoint now) const noexcept { return now >= deadline_; }
    [[nodiscard]] const ReverseFdAttempt& identity() const noexcept { return identity_; }
    [[nodiscard]] TimePoint original_deadline() const noexcept { return deadline_; }
    [[nodiscard]] int master_fd() const noexcept { return master_.get(); }

    // A fresh FD_CLOEXEC duplicate is sent on every invocation.  remaining_ms
    // is computed from the original deadline and is never accepted from a
    // caller, so a reconnect cannot renew the local budget.
    ReverseFdDecision send_attempt(local::Connection& connection) noexcept;

    // Cancel/expiry close the master.  Any duplicate created by a completed
    // send is already closed by that send; move replacement closes the old
    // master through ReverseFdMaster's RAII ownership.
    void cancel() noexcept;
    void expire(TimePoint now) noexcept;

private:
    [[nodiscard]] int duplicate_master() const noexcept;
    ReverseFdMaster master_;
    ReverseFdAttempt identity_{};
    TimePoint deadline_{};
    bool cancelled_ = false;
};

enum class ReverseFdReceiverState : uint8_t {
    Idle = 0,
    WaitP50Input,
    ToCompile,
    Expired,
    Cancelled,
    Closed,
};

// Service-incarnation reducer.  It is shared by every receiver connection;
// reconnecting must not reset high_water_ or the accepted replay fingerprint.
class ReverseFdReceiverLedger {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit ReverseFdReceiverLedger(uint64_t service_incarnation) noexcept
        : service_incarnation_(service_incarnation) {}

    [[nodiscard]] ReverseFdReceiverState state() const noexcept { return state_; }
    [[nodiscard]] uint64_t service_incarnation() const noexcept { return service_incarnation_; }
    [[nodiscard]] DeliveryId high_water() const noexcept { return high_water_; }
    [[nodiscard]] size_t transition_count() const noexcept { return transition_count_; }
    [[nodiscard]] size_t fork_count() const noexcept { return fork_count_; }
    [[nodiscard]] bool has_compiler_fd() const noexcept { return compiler_fd_.valid(); }
    [[nodiscard]] const std::optional<ReverseFdAttempt>& accepted() const noexcept {
        return accepted_;
    }

    // Installs the exact arm and an absolute local deadline.  The deadline
    // is set once by the local owner; wire remaining_ms never resets it.
    ReverseFdDecision arm_input(const P50SourceArm& arm, TimePoint now,
                                TimePoint deadline) noexcept;

    // Accepts one already-received descriptor.  This method always consumes
    // fd by value.  Replays/conflicts therefore close their duplicate before
    // returning; only Accepted retains a compiler-ready descriptor.
    ReverseFdDecision accept(ReverseFdAttempt attempt, local::HandoffFd fd,
                             TimePoint now) noexcept;

    // ACK may be emitted after this returns: Accepted has already transitioned
    // WAITP50INPUT -> TOCOMPILE.  ExactReplay is also ACKable and never
    // increments transition/fork counters.
    [[nodiscard]] bool ackable(ReverseFdDecision result) const noexcept;
    local::HandoffFd take_for_compile() noexcept;
    void cancel() noexcept;
    void close() noexcept;

private:
    bool exact_accepted(const ReverseFdAttempt& attempt) const noexcept;
    void retire_fd() noexcept;

    uint64_t service_incarnation_ = 0;
    DeliveryId high_water_ = 0;
    std::optional<ReverseFdAttempt> offered_arm_;
    std::optional<ReverseFdAttempt> accepted_;
    local::HandoffFd compiler_fd_;
    TimePoint deadline_{};
    ReverseFdReceiverState state_ = ReverseFdReceiverState::Idle;
    size_t transition_count_ = 0;
    size_t fork_count_ = 0;
};

// One attempt per authenticated Connection.  The receiver ledger remains
// service-global and must be reused after a lost ACK/reconnect.
ReverseFdDecision receive_reverse_fd_attempt(
    local::Connection& connection, ReverseFdReceiverLedger& ledger,
    std::chrono::steady_clock::time_point io_deadline) noexcept;

}  // namespace icecc::p50
