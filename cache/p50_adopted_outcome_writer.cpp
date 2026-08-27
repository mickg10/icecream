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

P5coEndpointHandoff::P5coEndpointHandoff(
    std::unique_ptr<P5coAdoptedSocketLease> lease,
    daemon::P50CacheSessionOutcome outcome,
    AbsoluteMonotonicDeadline deadline) noexcept
    : lease_(std::move(lease)), outcome_(std::move(outcome)),
      deadline_(deadline) {}

P5coEndpointHandoff::~P5coEndpointHandoff() noexcept { fence_owned(); }

P5coEndpointHandoff::P5coEndpointHandoff(P5coEndpointHandoff&& other) noexcept
    : lease_(std::move(other.lease_)), outcome_(std::move(other.outcome_)),
      deadline_(other.deadline_), fenced_(other.fenced_) {
    other.fenced_ = false;
}

P5coEndpointHandoff& P5coEndpointHandoff::operator=(
    P5coEndpointHandoff&& other) noexcept {
    if (this == &other)
        return *this;

    // Replacing an unconsumed handoff must account for its retained lease.
    // The old unique_ptr is then destroyed, but its fence has already made
    // the ownership loss explicit.
    fence_owned();
    lease_ = std::move(other.lease_);
    outcome_ = std::move(other.outcome_);
    deadline_ = other.deadline_;
    fenced_ = other.fenced_;
    other.fenced_ = false;
    return *this;
}

void P5coEndpointHandoff::fence_owned() noexcept {
    if (lease_ && !fenced_) {
        lease_->fence();
        fenced_ = true;
    }
}

std::unique_ptr<P5coAdoptedSocketLease>
P5coEndpointHandoff::take_lease_for_endpoint() noexcept {
    return std::move(lease_);
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
    // caller cannot inject another canonical byte vector or a bare local
    // control identity into this reducer.
    if (lease_ && outcome_.kind == daemon::P50CacheSessionOutcomeKind::Adopted &&
        outcome_.valid() && observations_ && deadline_.valid() &&
        limits_.max_bytes_per_advance != 0) {
        canonical_p5co_ = daemon::encode_cache_session_outcome(outcome_);
        if (!canonical_p5co_.empty()) {
            state_ = P5coWriterState::Ready;
            failure_ = P5coFailure::None;
            return;
        }
    }

    // Construction is itself an ownership boundary.  Do not leave an
    // adopted lease live merely because the object will eventually destruct.
    state_ = P5coWriterState::FailedAfterDetach;
    failure_ = P5coFailure::InvalidInput;
    fence_owned(); // INVALID_CONSTRUCTION_FENCE
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

    // Terminal readiness is authoritative.  It is intentionally checked
    // before state/clock/ownership work: a closed endpoint must be fenced
    // even when the observation source is absent, stale, or broken.
    const short terminal_events =
        static_cast<short>(POLLERR | POLLHUP | POLLNVAL
#ifdef POLLRDHUP
                           | POLLRDHUP
#endif
        );
    if (state_ != P5coWriterState::FailedAfterDetach &&
        (revents & terminal_events) != 0) {
        fail(P5coFailure::TerminalEvent);
        return state_;
    }

    if (state_ == P5coWriterState::FailedAfterDetach ||
        state_ == P5coWriterState::FullyFlushed)
        return state_;
    if (!observations_) {
        fail(P5coFailure::InvalidInput);
        return state_;
    }

    // This is the first fresh sample for this turn.  The caller cannot pass
    // or renew a time value; the source owns the CLOCK_MONOTONIC observation.
    const std::optional<MonotonicObservation> initial = observations_->observe();
    if (!initial.has_value() || !initial->valid()) {
        fail(P5coFailure::ObservationFailed);
        return state_;
    }
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

    // Linearization point: this second fresh observation and the following
    // exact identity check are immediately before the bounded send syscall.
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

    // EAGAIN/EWOULDBLOCK and EINTR are advisory-turn outcomes.  They do not
    // advance the exact prefix and, critically, do not mutate Ready into
    // Writing; the state remains exactly what it was on entry.
    if ((result.kind == P5coWriteKind::WouldBlock ||
         result.kind == P5coWriteKind::Interrupted) && result.bytes == 0)
        return state_;

    fail(P5coFailure::SendError);
    return state_;
}

std::optional<P5coEndpointHandoff>
AdoptedOutcomeWriter::take_for_endpoint() noexcept {
    if (state_ != P5coWriterState::FullyFlushed || !lease_)
        return {};
    if (!observations_) {
        fail(P5coFailure::InvalidInput);
        return {};
    }

    // Endpoint transfer has one irreversible order: fresh observation,
    // validation against the original immutable deadline/domain, exact owner
    // revalidation, and only then move of the complete authority bundle.
    const std::optional<MonotonicObservation> observation = observations_->observe();
    if (!observation.has_value() || !observation->valid()) {
        fail(P5coFailure::ObservationFailed);
        return {};
    }
    if (!deadline_.valid()) {
        fail(P5coFailure::InvalidInput);
        return {};
    }
    if (!deadline_.matches_clock(observation->clock)) {
        fail(P5coFailure::ClockDomainMismatch);
        return {};
    }
    if (observation->now_ns >= deadline_.expires_at_ns) {
        fail(P5coFailure::EndpointStartExpired);
        return {};
    }
    if (!lease_->revalidate(outcome_, deadline_)) {
        fail(P5coFailure::OwnershipLost);
        return {};
    }

    P5coEndpointHandoff handoff(std::move(lease_), std::move(outcome_), deadline_);
    return std::optional<P5coEndpointHandoff>(std::move(handoff));
}

} // namespace icecc::p50::sidecar
