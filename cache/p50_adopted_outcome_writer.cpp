#include "p50_adopted_outcome_writer.h"

#include <algorithm>
#include <limits>
#include <time.h>

namespace icecc::p50::sidecar {

std::optional<MonotonicObservation>
SystemMonotonicObservationSource::observe() noexcept {
    timespec sample{};
    if (::clock_gettime(CLOCK_MONOTONIC, &sample) != 0 || sample.tv_sec < 0 ||
        sample.tv_nsec < 0 || sample.tv_nsec >= 1000000000L)
        return std::nullopt;
    constexpr int64_t kNanosPerSecond = 1000000000;
    if (static_cast<uint64_t>(sample.tv_sec) >
        static_cast<uint64_t>((std::numeric_limits<int64_t>::max() -
                               kNanosPerSecond + 1) /
                              kNanosPerSecond))
        return std::nullopt;
    const int64_t seconds = static_cast<int64_t>(sample.tv_sec);
    const int64_t now_ns = seconds * kNanosPerSecond + sample.tv_nsec;
    const MonotonicClockIdentity clock = process_monotonic_clock_identity();
    MonotonicObservation observation{now_ns, clock};
    return observation.valid() ? std::optional<MonotonicObservation>(observation)
                               : std::nullopt;
}

AdoptedOutcomeWriter::AdoptedOutcomeWriter(
    std::unique_ptr<P5coAdoptedSocketLease> lease,
    daemon::P50CacheSessionOutcome outcome,
    std::unique_ptr<MonotonicObservationSource> observations,
    AbsoluteMonotonicDeadline deadline) noexcept
    : AdoptedOutcomeWriter(std::move(lease), std::move(outcome),
                           std::move(observations), deadline, Limits{}) {}

AdoptedOutcomeWriter::AdoptedOutcomeWriter(
    std::unique_ptr<P5coAdoptedSocketLease> lease,
    daemon::P50CacheSessionOutcome outcome,
    std::unique_ptr<MonotonicObservationSource> observations,
    AbsoluteMonotonicDeadline deadline,
    Limits limits) noexcept
    : lease_(std::move(lease)), outcome_(std::move(outcome)),
      observations_(std::move(observations)), deadline_(deadline), limits_(limits) {
    // P5CO bytes are generated only from the exact typed ADOPTED DTO.  A
    // caller cannot inject a different canonical byte vector or a bare local
    // control identity into this reducer.
    if (lease_ && outcome_.kind == daemon::P50CacheSessionOutcomeKind::Adopted &&
        outcome_.valid() && observations_ && deadline_.valid() &&
        limits_.max_bytes_per_advance != 0) {
        canonical_p5co_ = daemon::encode_cache_session_outcome(outcome_);
        if (!canonical_p5co_.empty()) {
            state_ = P5coWriterState::Ready;
            failure_ = P5coFailure::None;
        }
    }
}

AdoptedOutcomeWriter::~AdoptedOutcomeWriter() noexcept { fence_owned(); }

AdoptedOutcomeWriter::AdoptedOutcomeWriter(AdoptedOutcomeWriter&& other) noexcept
    : lease_(std::move(other.lease_)), outcome_(std::move(other.outcome_)),
      canonical_p5co_(std::move(other.canonical_p5co_)),
      observations_(std::move(other.observations_)), deadline_(other.deadline_),
      limits_(other.limits_), state_(other.state_), failure_(other.failure_),
      offset_(other.offset_), last_bytes_(other.last_bytes_),
      last_syscalls_(other.last_syscalls_), fenced_(other.fenced_) {
    other.state_ = P5coWriterState::FailedAfterDetach;
    other.failure_ = P5coFailure::InvalidInput;
    other.offset_ = 0;
    other.last_bytes_ = 0;
    other.last_syscalls_ = 0;
    other.fenced_ = false;
}

AdoptedOutcomeWriter& AdoptedOutcomeWriter::operator=(
    AdoptedOutcomeWriter&& other) noexcept {
    if (this == &other)
        return *this;
    // A move assignment is an ownership transition.  Explicitly fence this
    // destination before replacing it; silently dropping a live lease would
    // create an unaccounted post-detach socket.
    fence_owned();
    lease_ = std::move(other.lease_);
    outcome_ = std::move(other.outcome_);
    canonical_p5co_ = std::move(other.canonical_p5co_);
    observations_ = std::move(other.observations_);
    deadline_ = other.deadline_;
    limits_ = other.limits_;
    state_ = other.state_;
    failure_ = other.failure_;
    offset_ = other.offset_;
    last_bytes_ = other.last_bytes_;
    last_syscalls_ = other.last_syscalls_;
    fenced_ = other.fenced_;
    other.state_ = P5coWriterState::FailedAfterDetach;
    other.failure_ = P5coFailure::InvalidInput;
    other.offset_ = 0;
    other.last_bytes_ = 0;
    other.last_syscalls_ = 0;
    other.fenced_ = false;
    return *this;
}

bool AdoptedOutcomeWriter::deadline_valid(
    const MonotonicObservation& observation) const noexcept {
    return observation.valid() && deadline_.matches_clock(observation.clock) &&
           observation.now_ns < deadline_.expires_at_ns;
}

void AdoptedOutcomeWriter::fence_owned() noexcept {
    if (lease_ && !fenced_) {
        lease_->fence();
        fenced_ = true;
    }
}

void AdoptedOutcomeWriter::fail(P5coFailure reason) noexcept {
    if (state_ == P5coWriterState::FailedAfterDetach)
        return;
    state_ = P5coWriterState::FailedAfterDetach;
    failure_ = reason;
    fence_owned();
}

P5coWriterState AdoptedOutcomeWriter::advance(short revents) noexcept {
    last_bytes_ = 0;
    last_syscalls_ = 0;
    if (state_ == P5coWriterState::FailedAfterDetach ||
        state_ == P5coWriterState::FullyFlushed)
        return state_;
    if (!observations_) {
        fail(P5coFailure::InvalidInput);
        return state_;
    }

    const std::optional<MonotonicObservation> initial = observations_->observe();
    if (!initial.has_value() || !initial->valid()) {
        fail(P5coFailure::ObservationFailed);
        return state_;
    }

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
    if (!deadline_.matches_clock(initial->clock)) {
        fail(P5coFailure::ClockDomainMismatch);
        return state_;
    }
    if (initial->now_ns >= deadline_.expires_at_ns) {
        fail(P5coFailure::Expired);
        return state_;
    }
    if (!lease_ || !lease_->revalidate(outcome_, deadline_)) {
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

    // Linearization point: take a second fresh observation and revalidate the
    // exact outcome/deadline immediately before the bounded send syscall.
    const std::optional<MonotonicObservation> final = observations_->observe();
    if (!final.has_value() || !final->valid()) {
        fail(P5coFailure::ObservationFailed);
        return state_;
    }
    if (!deadline_valid(*final)) {
        fail(!deadline_.matches_clock(final->clock)
                 ? P5coFailure::ClockDomainMismatch
                 : P5coFailure::Expired);
        return state_;
    }
    if (!lease_->revalidate(outcome_, deadline_)) {
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
    if ((result.kind == P5coWriteKind::WouldBlock ||
         result.kind == P5coWriteKind::Interrupted) && result.bytes == 0)
        return state_ = P5coWriterState::Writing;

    fail(P5coFailure::SendError);
    return state_;
}

std::unique_ptr<P5coAdoptedSocketLease>
AdoptedOutcomeWriter::take_for_endpoint() noexcept {
    if (state_ != P5coWriterState::FullyFlushed || !lease_)
        return {};

    // Endpoint transfer is another irreversible boundary: bind the exact
    // outcome/deadline first, then sample a fresh observation after it.
    if (!lease_->revalidate(outcome_, deadline_)) {
        fail(P5coFailure::OwnershipLost);
        return {};
    }
    if (!observations_) {
        fail(P5coFailure::InvalidInput);
        return {};
    }
    const std::optional<MonotonicObservation> observation = observations_->observe();
    if (!observation.has_value() || !observation->valid()) {
        fail(P5coFailure::ObservationFailed);
        return {};
    }
    if (!deadline_valid(*observation)) {
        fail(!deadline_.matches_clock(observation->clock)
                 ? P5coFailure::ClockDomainMismatch
                 : P5coFailure::EndpointStartExpired);
        return {};
    }
    return std::move(lease_);
}

} // namespace icecc::p50::sidecar
