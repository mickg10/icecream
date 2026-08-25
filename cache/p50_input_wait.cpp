#include "p50_input_wait.h"

#include <unistd.h>

namespace icecc::p50::daemon {

bool P50SourceArmGate::send_arm(const P50SourceArm& arm) noexcept {
    if (state_ != State::Idle || !arm.valid())
        return false;
    arm_ = arm;
    state_ = State::ArmSent;
    return true;
}

bool P50SourceArmGate::receive_arm_ack(const P50SourceArm& acknowledged) noexcept {
    if (state_ != State::ArmSent || acknowledged != arm_)
        return false;
    state_ = State::ArmAcked;
    return true;
}

bool P50SourceArmGate::cache_transfer_permitted() const noexcept {
    return state_ == State::ArmAcked;
}

void P50SourceArmGate::reset() noexcept {
    state_ = State::Idle;
    arm_ = P50SourceArm{};
}

P50InputWaitState::~P50InputWaitState() { clear_fd(); }

bool P50InputWaitState::arm_input(const P50SourceArm& arm) noexcept {
    if (state_ != State::Idle || !arm.valid())
        return false;
    arm_ = arm;
    ready_ = P50InputReady{};
    state_ = State::WaitP50Input;
    return true;
}

bool P50InputWaitState::accept_ready(const P50InputReady& ready,
                                     int sealed_fd) noexcept {
    if (state_ != State::WaitP50Input || sealed_fd < 0 || !ready.valid() ||
        !ready.matches_arm(arm_) ||
        ready.attachment_request_id != arm_.source_request_id)
        return false;
    ready_ = ready;
    sealed_fd_ = sealed_fd;
    state_ = State::Ready;
    return true;
}

int P50InputWaitState::take_for_fork() noexcept {
    if (state_ != State::Ready || sealed_fd_ < 0)
        return -1;
    const int result = sealed_fd_;
    sealed_fd_ = -1;
    state_ = State::Forked;
    return result;
}

void P50InputWaitState::close() noexcept {
    clear_fd();
    if (state_ != State::Forked)
        state_ = State::Closed;
}

void P50InputWaitState::clear_fd() noexcept {
    if (sealed_fd_ >= 0) {
        (void)::close(sealed_fd_);
        sealed_fd_ = -1;
    }
}

}  // namespace icecc::p50::daemon
