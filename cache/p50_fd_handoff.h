#pragma once

// Bounded daemon -> sidecar accepted-descriptor handoff over an already
// authenticated AF_UNIX control connection.  This is deliberately a generic
// ownership primitive: it does not know the ordinary-link protocol or cache
// session codec, and it never creates a listener or advertises an endpoint.

#include <chrono>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <poll.h>

#include "p50_local_transport.h"

namespace icecc::p50::local {

class HandoffFd {
public:
    HandoffFd() noexcept = default;
    explicit HandoffFd(int fd) noexcept;
    ~HandoffFd();

    HandoffFd(HandoffFd&& other) noexcept;
    HandoffFd& operator=(HandoffFd&& other) noexcept;
    HandoffFd(const HandoffFd&) = delete;
    HandoffFd& operator=(const HandoffFd&) = delete;

    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool cloexec() const noexcept;
    [[nodiscard]] int release() noexcept;
    void reset() noexcept;

private:
    int fd_ = -1;
};

struct HandoffRequest {
    Identity identity{};
    uint64_t request_id = 0;
    auto operator<=>(const HandoffRequest&) const = default;
};

enum class FdHandoffStatus {
    Accepted = 0,
    Nack,
    InvalidArgument,
    NotAuthenticated,
    AlreadySent,
    AlreadyConsumed,
    Timeout,
    Disconnected,
    IoError,
    Malformed,
    Truncated,
    MessageTruncated,
    ControlTruncated,
    TrailingData,
    MissingFd,
    ExtraFd,
    UnexpectedControl,
    StaleGeneration,
    IdentityMismatch,
    RequestMismatch,
    DuplicateRequest,
    AckMismatch,
    AdoptionFailed,
};

const char* fd_handoff_status_name(FdHandoffStatus status) noexcept;

enum class FdHandoffSenderState {
    Prepared = 0,
    Sent,
    Acked,
    Nacked,
    TimedOut,
    Disconnected,
    Rejected,
};

struct FdHandoffResult {
    FdHandoffStatus status = FdHandoffStatus::InvalidArgument;
    FdHandoffSenderState sender_state = FdHandoffSenderState::Rejected;
};

// The sender owns the accepted descriptor until the handoff reaches a
// terminal state.  It closes the local copy only after ACK or a fail-closed
// terminal result; it never retries a sent request with the same descriptor.
class FdHandoffSender {
public:
    explicit FdHandoffSender(HandoffFd fd) noexcept;

    FdHandoffSender(FdHandoffSender&&) noexcept = default;
    FdHandoffSender& operator=(FdHandoffSender&&) noexcept = default;
    FdHandoffSender(const FdHandoffSender&) = delete;
    FdHandoffSender& operator=(const FdHandoffSender&) = delete;

    [[nodiscard]] FdHandoffSenderState state() const noexcept { return state_; }
    [[nodiscard]] bool owns_fd() const noexcept { return fd_.valid(); }

    // deadline is absolute and covers both send and response receipt.
    FdHandoffResult send(Connection& connection, const HandoffRequest& request,
                         std::chrono::steady_clock::time_point deadline) noexcept;

private:
    HandoffFd fd_;
    FdHandoffSenderState state_ = FdHandoffSenderState::Prepared;
};

// The receiver admits at most one request.  Rejected requests are still
// consumed and any attached descriptors are closed before a NACK is sent.
class FdHandoffReceiver {
public:
    FdHandoffReceiver() noexcept = default;

    FdHandoffReceiver(FdHandoffReceiver&&) noexcept = default;
    FdHandoffReceiver& operator=(FdHandoffReceiver&&) noexcept = default;
    FdHandoffReceiver(const FdHandoffReceiver&) = delete;
    FdHandoffReceiver& operator=(const FdHandoffReceiver&) = delete;

    [[nodiscard]] bool consumed() const noexcept { return consumed_; }
    [[nodiscard]] bool adopted() const noexcept { return adopted_.valid(); }

    // expected identifies the one request this receiver is prepared to
    // adopt.  The ACK is emitted only after received_fd has been moved into
    // adopted_ and CLOEXEC has been proven.
    FdHandoffResult receive_and_ack(
        Connection& connection, const HandoffRequest& expected,
        std::chrono::steady_clock::time_point deadline) noexcept;

    // Transfers the adopted descriptor to its eventual ordinary-link owner.
    // Calling this before Accepted returns an invalid owner.
    HandoffFd take_adopted_fd() noexcept;

private:
    bool consumed_ = false;
    std::optional<HandoffRequest> seen_request_;
    HandoffFd adopted_;
};

// Event-loop form of FdHandoffReceiver.  The caller owns the authenticated
// Connection and invokes advance() only after poll(2) reports the events from
// poll_events().  advance() never waits and performs a bounded amount of
// nonblocking I/O.  The absolute deadline is fixed at start().
class AsyncFdHandoffReceiver {
public:
    AsyncFdHandoffReceiver() noexcept = default;
    ~AsyncFdHandoffReceiver() noexcept;
    AsyncFdHandoffReceiver(const AsyncFdHandoffReceiver&) = delete;
    AsyncFdHandoffReceiver& operator=(const AsyncFdHandoffReceiver&) = delete;

    // The Connection is borrowed and remains owned by the caller on every
    // terminal result; the caller must close/recycle it after completion.
    bool start(Connection& connection, const HandoffRequest& expected,
               std::chrono::steady_clock::time_point deadline) noexcept;
    void advance(short revents = POLLIN) noexcept;
    [[nodiscard]] int poll_fd() const noexcept { return connection_fd_; }
    [[nodiscard]] short poll_events() const noexcept;
    [[nodiscard]] bool done() const noexcept { return done_; }
    [[nodiscard]] FdHandoffResult result() const noexcept { return result_; }
    [[nodiscard]] HandoffFd take_fd() noexcept;
    void cancel(FdHandoffStatus status = FdHandoffStatus::Disconnected) noexcept;

private:
    enum class State : uint8_t { Idle, Receiving, SendingAck, Done };
    void fail(FdHandoffStatus status) noexcept;
    void receive_once() noexcept;
    void finish_receive() noexcept;
    void send_ack_once() noexcept;

    int connection_fd_ = -1;
    HandoffRequest expected_{};
    std::chrono::steady_clock::time_point deadline_{};
    State state_ = State::Idle;
    bool done_ = false;
    bool consumed_ = false;
    std::array<uint8_t, 40> wire_{};
    size_t offset_ = 0;
    int received_fd_ = -1;
    size_t fd_count_ = 0;
    bool has_rights_ = false;
    std::array<uint8_t, 40> ack_wire_{};
    size_t ack_offset_ = 0;
    HandoffFd adopted_{};
    FdHandoffStatus ack_status_ = FdHandoffStatus::Accepted;
    FdHandoffResult result_{};
};

#if defined(ICECC_P50_FD_HANDOFF_TEST_HOOKS)
// Compile-time-only deterministic short-write seam.  The installed library
// never contains this symbol or an environment-controlled equivalent.
void fd_handoff_test_set_max_send_chunk(size_t bytes) noexcept;
#endif

} // namespace icecc::p50::local
