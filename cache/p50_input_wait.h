#pragma once

#include "p50_source_identity.h"

namespace icecc::p50::daemon {

// C-side ordering gate.  The CacheWire transaction cannot start until F has
// decoded and ACKed the exact arm.  This is deliberately separate from the
// input-record key: an attempt replacement may retain the same input key.
class P50SourceArmGate {
public:
    enum class State { Idle, ArmSent, ArmAcked };

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] const P50SourceArm& arm() const noexcept { return arm_; }
    bool send_arm(const P50SourceArm& arm) noexcept;
    bool receive_arm_ack(const P50SourceArm& acknowledged) noexcept;
    [[nodiscard]] bool cache_transfer_permitted() const noexcept;
    void reset() noexcept;

private:
    State state_ = State::Idle;
    P50SourceArm arm_{};
};

// F-daemon half of the two-phase seam.  WAITP50INPUT owns the arm and its
// deadline.  It cannot expose a compiler-ready descriptor until the later
// P50InputReady matches every arm field and arrives with a sealed FD.
class P50InputWaitState {
public:
    enum class State { Idle, WaitP50Input, Ready, Forked, Closed };

    P50InputWaitState() = default;
    ~P50InputWaitState();
    P50InputWaitState(const P50InputWaitState&) = delete;
    P50InputWaitState& operator=(const P50InputWaitState&) = delete;

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] bool arm_ack_sent() const noexcept {
        return state_ == State::WaitP50Input;
    }
    [[nodiscard]] const P50SourceArm& arm() const noexcept { return arm_; }
    [[nodiscard]] bool can_fork() const noexcept { return state_ == State::Ready; }

    // Installs WAITP50INPUT and returns true only for a complete arm.  The
    // caller sends P50_INPUT_ARMED only after this returns true.
    bool arm_input(const P50SourceArm& arm) noexcept;

    // Takes ownership of sealed_fd only after all exact-ready checks pass.
    // Failure leaves the existing wait state and caller ownership unchanged.
    bool accept_ready(const P50InputReady& ready, int sealed_fd) noexcept;

    // Transfers the one sealed FD to the compiler worker and makes a second
    // fork impossible.  Returns -1 unless an exact ready attachment arrived.
    int take_for_fork() noexcept;

    // Closure is terminal for this attempt.  It closes any staged FD and
    // rejects late CacheWire delivery; it never resurrects WAITP50INPUT.
    void close() noexcept;

private:
    void clear_fd() noexcept;

    State state_ = State::Idle;
    P50SourceArm arm_{};
    P50InputReady ready_{};
    int sealed_fd_ = -1;
};

}  // namespace icecc::p50::daemon
