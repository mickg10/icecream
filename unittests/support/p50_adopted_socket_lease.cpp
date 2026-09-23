#include "unittests/support/p50_adopted_socket_lease.h"

#include <cerrno>

#include <sys/socket.h>

namespace icecc::p50::sidecar {

P5coRetainedSocketLease::P5coRetainedSocketLease(
    local::HandoffFd adopted,
    daemon::P50CacheSessionOutcome exact_outcome,
    AbsoluteMonotonicDeadline exact_deadline) noexcept
    : adopted_(std::move(adopted)), exact_outcome_(std::move(exact_outcome)),
      exact_deadline_(exact_deadline) {}

bool P5coRetainedSocketLease::revalidate(
    const daemon::P50CacheSessionOutcome& exact_outcome,
    const AbsoluteMonotonicDeadline& exact_deadline) const noexcept {
    return !fenced_ && !released_ && adopted_.valid() && adopted_.cloexec() &&
           exact_outcome_.kind ==
               daemon::P50CacheSessionOutcomeKind::Adopted &&
           exact_outcome_.valid() && exact_deadline_.valid() &&
           exact_outcome == exact_outcome_ &&
           exact_deadline == exact_deadline_;
}

P5coWriteResult P5coRetainedSocketLease::send_nonblocking(
    std::span<const uint8_t> bytes, uint8_t flags) noexcept {
    const uint8_t required =
        P5coSendFlag::DontWait | P5coSendFlag::NoSignal;
    if (!revalidate(exact_outcome_, exact_deadline_) || bytes.empty() ||
        flags != required)
        return {P5coWriteKind::Error, 0};

#if defined(MSG_DONTWAIT) && defined(MSG_NOSIGNAL)
    const ssize_t result =
        ::send(adopted_.get(), bytes.data(), bytes.size(),
               MSG_DONTWAIT | MSG_NOSIGNAL);
#else
    // This Linux-first product must fail closed on a platform which cannot
    // suppress blocking and SIGPIPE per call without changing shared state.
    (void)bytes;
    errno = ENOTSUP;
    const ssize_t result = -1;
#endif
    if (result > 0)
        return {P5coWriteKind::Sent, static_cast<size_t>(result)};
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return {P5coWriteKind::WouldBlock, 0};
    if (result < 0 && errno == EINTR)
        return {P5coWriteKind::Interrupted, 0};
    return {P5coWriteKind::Error, 0};
}

void P5coRetainedSocketLease::fence() noexcept {
    if (fenced_)
        return;
    if (!released_)
        adopted_.reset();
    fenced_ = true;
}

int P5coRetainedSocketLease::release_native_fd_for_endpoint(
    const daemon::P50CacheSessionOutcome& exact_outcome,
    const AbsoluteMonotonicDeadline& exact_deadline) noexcept {
    if (!revalidate(exact_outcome, exact_deadline))
        return -1;
    released_ = true;
    return adopted_.release();
}

} // namespace icecc::p50::sidecar
