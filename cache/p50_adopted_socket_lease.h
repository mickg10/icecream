#pragma once

#include "p50_adopted_outcome_writer.h"
#include "p50_fd_handoff.h"

namespace icecc::p50::sidecar {

// Concrete post-SCM_RIGHTS socket authority used by the F sidecar.  It owns
// one CLOEXEC descriptor and its exact P5CO outcome/deadline binding.  The
// descriptor is never publicly observable or detachable: only
// P50ServerEndpoint can consume it through the private virtual boundary on
// P5coAdoptedSocketLease.
class P5coRetainedSocketLease final : public P5coAdoptedSocketLease {
public:
    P5coRetainedSocketLease(
        local::HandoffFd adopted,
        daemon::P50CacheSessionOutcome exact_outcome,
        AbsoluteMonotonicDeadline exact_deadline) noexcept;
    ~P5coRetainedSocketLease() override = default;

    P5coRetainedSocketLease(const P5coRetainedSocketLease&) = delete;
    P5coRetainedSocketLease& operator=(const P5coRetainedSocketLease&) = delete;
    P5coRetainedSocketLease(P5coRetainedSocketLease&&) = delete;
    P5coRetainedSocketLease& operator=(P5coRetainedSocketLease&&) = delete;

    [[nodiscard]] bool revalidate(
        const daemon::P50CacheSessionOutcome& exact_outcome,
        const AbsoluteMonotonicDeadline& exact_deadline) const noexcept override;
    P5coWriteResult send_nonblocking(
        std::span<const uint8_t> bytes, uint8_t flags) noexcept override;
    void fence() noexcept override;

#if defined(ICECC_P50_ADOPTED_SOCKET_LEASE_TEST_HOOKS)
    [[nodiscard]] int release_native_fd_for_test(
        const daemon::P50CacheSessionOutcome& exact_outcome,
        const AbsoluteMonotonicDeadline& exact_deadline) noexcept {
        return release_native_fd_for_endpoint(exact_outcome, exact_deadline);
    }
    [[nodiscard]] bool fenced_for_test() const noexcept { return fenced_; }
#endif

private:
    [[nodiscard]] int release_native_fd_for_endpoint(
        const daemon::P50CacheSessionOutcome& exact_outcome,
        const AbsoluteMonotonicDeadline& exact_deadline) noexcept override;

    local::HandoffFd adopted_;
    daemon::P50CacheSessionOutcome exact_outcome_;
    AbsoluteMonotonicDeadline exact_deadline_{};
    bool fenced_ = false;
    bool released_ = false;
};

} // namespace icecc::p50::sidecar
