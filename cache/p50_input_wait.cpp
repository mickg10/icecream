#include "p50_input_wait.h"

#include <unistd.h>

namespace icecc::p50::daemon {

namespace {

bool same_complete_arm(const P50SourceArmFields& left,
                       const P50SourceArmFields& right) noexcept {
    return left.wire_job_id == right.wire_job_id &&
           left.assignment_epoch == right.assignment_epoch &&
           left.assignment_nonce == right.assignment_nonce &&
           left.selected_f_host == right.selected_f_host &&
           left.selected_f_ordinary_port == right.selected_f_ordinary_port &&
           left.selected_f_cache_port == right.selected_f_cache_port &&
           left.cache_protocol == right.cache_protocol &&
           left.cache_profile == right.cache_profile &&
           left.logical_job == right.logical_job &&
           left.compiler_attempt == right.compiler_attempt &&
           left.c_store_generation == right.c_store_generation &&
           left.c_store_derivation_version == right.c_store_derivation_version &&
           left.c_store_guid == right.c_store_guid &&
           left.source_request_id == right.source_request_id &&
           left.source_mode == right.source_mode &&
           left.c_control_generation == right.c_control_generation &&
           left.c_control_attempt == right.c_control_attempt;
}

bool ready_matches_complete_arm(const P50InputReady& ready,
                                const P50SourceArmFields& arm) noexcept {
    const P50SourceArm& legacy = ready.arm;
    return legacy.wire_job_id == arm.wire_job_id &&
           legacy.assignment_epoch == arm.assignment_epoch &&
           legacy.assignment_nonce == arm.assignment_nonce &&
           legacy.selected_f_host == arm.selected_f_host &&
           legacy.selected_f_ordinary_port == arm.selected_f_ordinary_port &&
           legacy.selected_f_cache_port == arm.selected_f_cache_port &&
           legacy.cache_protocol == arm.cache_protocol &&
           legacy.cache_profile == arm.cache_profile &&
           legacy.logical_job == arm.logical_job &&
           legacy.attempt_id == arm.compiler_attempt &&
           legacy.c_store_generation == arm.c_store_generation &&
           legacy.c_store_guid.bytes == arm.c_store_guid &&
           legacy.source_request_id == arm.source_request_id &&
           legacy.source_mode == arm.source_mode &&
           ready.attachment_request_id == arm.source_request_id;
}

} // namespace

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
    canonical_arm_.reset();
    ready_ = P50InputReady{};
    state_ = State::WaitP50Input;
    return true;
}

bool P50InputWaitState::arm_input(const P50SourceArmFields& arm) noexcept {
    if (state_ != State::Idle ||
        (!arm.valid_for_cache_revision(1) &&
         !arm.valid_for_cache_revision(2)))
        return false;
    canonical_arm_ = arm;
    arm_ = P50SourceArm{};
    ready_ = P50InputReady{};
    state_ = State::WaitP50Input;
    return true;
}

bool P50InputWaitState::accept_ready(const P50InputReady& ready,
                                     int sealed_fd) noexcept {
    if (state_ != State::WaitP50Input || sealed_fd < 0 || !ready.valid() ||
        canonical_arm_.has_value() ||
        !ready.matches_arm(arm_) ||
        ready.attachment_request_id != arm_.source_request_id)
        return false;
    ready_ = ready;
    sealed_fd_ = sealed_fd;
    state_ = State::Ready;
    return true;
}

bool P50InputWaitState::accept_ready(const P50SourceArmFields& arm,
                                     const P50InputReady& ready,
                                     int sealed_fd) noexcept {
    if (state_ != State::WaitP50Input || sealed_fd < 0 || !ready.valid() ||
        !canonical_arm_.has_value() ||
        !same_complete_arm(canonical_arm_.value(), arm) ||
        !ready_matches_complete_arm(ready, arm))
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
