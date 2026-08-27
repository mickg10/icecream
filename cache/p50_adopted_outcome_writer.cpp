#include "p50_adopted_outcome_writer.h"

#include <algorithm>

namespace icecc::p50::sidecar {

AdoptedOutcomeWriter::AdoptedOutcomeWriter(
    std::unique_ptr<P5coAdoptedSocketLease> lease,
    std::vector<uint8_t> canonical_p5co,
    local::Identity operation_identity,
    AbsoluteMonotonicDeadline deadline) noexcept
    : AdoptedOutcomeWriter(std::move(lease), std::move(canonical_p5co),
                           operation_identity, deadline, Limits{}) {}

AdoptedOutcomeWriter::AdoptedOutcomeWriter(
    std::unique_ptr<P5coAdoptedSocketLease> lease,
    std::vector<uint8_t> canonical_p5co,
    local::Identity operation_identity,
    AbsoluteMonotonicDeadline deadline,
    Limits limits) noexcept
    : lease_(std::move(lease)), canonical_p5co_(std::move(canonical_p5co)),
      operation_identity_(operation_identity), deadline_(deadline), limits_(limits) {
    if (lease_ && !canonical_p5co_.empty() && operation_identity_.generation != 0 &&
        operation_identity_.attempt != 0 &&
        deadline_.valid() && limits_.max_bytes_per_advance != 0) {
        state_ = P5coWriterState::Ready;
        failure_ = P5coFailure::None;
    }
}

bool AdoptedOutcomeWriter::deadline_valid(
    int64_t now_ns, const MonotonicClockIdentity& clock) const noexcept {
    return deadline_.matches_clock(clock) && now_ns < deadline_.expires_at_ns;
}

void AdoptedOutcomeWriter::fail(P5coFailure reason) noexcept {
    if (state_ == P5coWriterState::FailedAfterDetach)
        return;
    state_ = P5coWriterState::FailedAfterDetach;
    failure_ = reason;
    if (!fenced_ && lease_) {
        lease_->fence();
        fenced_ = true;
    }
}

P5coWriterState AdoptedOutcomeWriter::advance(
    int64_t now_ns, const MonotonicClockIdentity& clock, short revents) noexcept {
    last_bytes_ = 0;
    last_syscalls_ = 0;
    if (state_ == P5coWriterState::FailedAfterDetach ||
        state_ == P5coWriterState::FullyFlushed)
        return state_;

    // Terminal readiness wins over POLLOUT; a stale/closed relationship never
    // receives even a prefix of the outcome.
    if ((revents & (POLLERR | POLLHUP | POLLNVAL
#ifdef POLLRDHUP
                    | POLLRDHUP
#endif
                    )) != 0) {
        fail(P5coFailure::TerminalEvent);
        return state_;
    }

    // This check is intentionally made on every turn, including turns with
    // no writable event, so an expired reducer cannot later be revived.
    if (!deadline_.valid()) {
        fail(P5coFailure::InvalidInput);
        return state_;
    }
    if (!deadline_.matches_clock(clock)) {
        fail(P5coFailure::ClockDomainMismatch);
        return state_;
    }
    if (now_ns >= deadline_.expires_at_ns) {
        fail(P5coFailure::Expired);
        return state_;
    }
    if (!lease_ || !lease_->revalidate(operation_identity_)) {
        fail(P5coFailure::OwnershipLost);
        return state_;
    }
    if ((revents & POLLOUT) == 0)
        return state_;

    const size_t remaining = canonical_p5co_.size() - offset_;
    const size_t amount = std::min(remaining, limits_.max_bytes_per_advance);
    if (amount == 0) {
        state_ = P5coWriterState::FullyFlushed;
        failure_ = P5coFailure::None;
        return state_;
    }

    // Linearization point: readiness is rechecked immediately before the
    // bounded syscall.  Never move this below send_nonblocking().
    if (!deadline_valid(now_ns, clock)) {
        fail(!deadline_.matches_clock(clock) ? P5coFailure::ClockDomainMismatch
                                             : P5coFailure::Expired);
        return state_;
    }
    if (!lease_->revalidate(operation_identity_)) {
        fail(P5coFailure::OwnershipLost);
        return state_;
    }
    const P5coWriteResult result = lease_->send_nonblocking(
        std::span<const uint8_t>(canonical_p5co_.data() + offset_, amount),
        P5coSendFlag::DontWait | P5coSendFlag::NoSignal);
    last_syscalls_ = 1;
    if (result.kind == P5coWriteKind::Sent && result.bytes != 0 &&
        result.bytes <= amount) {
        offset_ += result.bytes;
        last_bytes_ = result.bytes;
        state_ = offset_ == canonical_p5co_.size()
                     ? P5coWriterState::FullyFlushed
                     : P5coWriterState::Writing;
        return state_;
    }
    if (result.kind == P5coWriteKind::WouldBlock ||
        result.kind == P5coWriteKind::Interrupted)
        return state_ = P5coWriterState::Writing;

    fail(P5coFailure::SendError);
    return state_;
}

std::unique_ptr<P5coAdoptedSocketLease> AdoptedOutcomeWriter::take_for_endpoint(
    int64_t now_ns, const MonotonicClockIdentity& clock) noexcept {
    if (state_ != P5coWriterState::FullyFlushed || !lease_)
        return {};
    if (!deadline_valid(now_ns, clock)) {
        fail(!deadline_.matches_clock(clock) ? P5coFailure::ClockDomainMismatch
                                             : P5coFailure::EndpointStartExpired);
        return {};
    }
    if (!lease_->revalidate(operation_identity_)) {
        fail(P5coFailure::OwnershipLost);
        return {};
    }
    return std::move(lease_);
}

} // namespace icecc::p50::sidecar
