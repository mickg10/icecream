#pragma once

#include "p50_fd_handoff.h"
#include "p50_input_lifecycle.h"
#include "p50_input_record.h"
#include "p50_local_transport.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <compare>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace icecc::p50 {

// This is the only compiler-visible identity in this adapter.  The cache key
// is deliberately exact and independent of compiler ATTEMPT_ID; the local
// transport identity authenticates the current daemon/sidecar relationship.
struct InputFdRequest {
    local::Identity identity{};
    InputRecordKey key{};
    InputLeaseOwner owner{};
    uint64_t request_id = 0;
    auto operator<=>(const InputFdRequest&) const = default;
};

enum class InputFdAttachmentStatus : uint8_t {
    Accepted = 0,
    InvalidArgument,
    UnsupportedPlatform,
    PeerUnauthenticated,
    HandshakeFailed,
    MalformedRequest,
    UnknownRecord,
    StaleIdentity,
    Timeout,
    Disconnected,
    MaterializationFailed,
    HandoffFailed,
};

const char* input_fd_attachment_status_name(
    InputFdAttachmentStatus status) noexcept;

// A compiler-consumer-owned descriptor.  The descriptor names a complete,
// sealed snapshot, not the store's mutable backing.  It is independent for
// every request and starts at byte zero.
class InputFd {
public:
    InputFd() noexcept = default;
    explicit InputFd(int fd) noexcept : fd_(fd) {}
    ~InputFd();

    InputFd(InputFd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    InputFd& operator=(InputFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }
    InputFd(const InputFd&) = delete;
    InputFd& operator=(const InputFd&) = delete;

    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] int release() noexcept {
        const int result = fd_;
        fd_ = -1;
        return result;
    }
    void reset() noexcept;

private:
    int fd_ = -1;
};

struct InputFdAttachmentResult {
    InputFdAttachmentStatus status = InputFdAttachmentStatus::InvalidArgument;
    local::FdHandoffResult handoff{};
    InputFd fd{};
    // The exact sidecar-incarnation/key/request tuple against which an
    // accepted compiler cursor was authorized.  The daemon retains this
    // observation independently of the transferred descriptor so teardown
    // can close the logical InputRecord lease on the same owner.
    std::optional<InputFdRequest> lease;
};

// A service instance is used on the P50 endpoint owner thread.  The callback
// is synchronous by design: a request cannot race the InputRecordStore owner.
// for_endpoint() is the production-shaped constructor and calls
// P50ServerEndpoint::attach_input() on that owner thread.
class InputFdAttachmentService {
public:
    using CursorProvider = std::function<InputCursor(InputRecordKey)>;

    explicit InputFdAttachmentService(CursorProvider provider,
                                      size_t max_materialized_bytes = size_t{64} << 20);

    template <class Endpoint>
    static InputFdAttachmentService for_endpoint(
        Endpoint& endpoint, size_t max_materialized_bytes = size_t{64} << 20) {
        return InputFdAttachmentService(
            [&endpoint](InputRecordKey key) { return endpoint.attach_input(key); },
            max_materialized_bytes);
    }

    // Handles exactly one authenticated request.  deadline is absolute and
    // covers credential verification, both handshake frames, the request,
    // the authenticated typed result, complete memfd creation, FD reply, and
    // its ACK. A non-Accepted result terminates before SCM_RIGHTS handoff.
    [[nodiscard]] InputFdAttachmentResult serve(
        local::Connection& connection, local::Identity expected_identity,
        const local::CredentialExpectation& expected_peer,
        std::chrono::steady_clock::time_point deadline) const noexcept;

    // Completes one request after the caller has already authenticated HELLO
    // and decoded the exact InputFdAttachment Data operation.  This seam is
    // used by the sidecar demux so cache-session and compiler-input controls
    // share one bounded relationship without repeating a handshake.
    [[nodiscard]] InputFdAttachmentResult serve_request(
        local::Connection& connection, local::Identity expected_identity,
        const local::CredentialExpectation& expected_peer, InputFdRequest request,
        std::chrono::steady_clock::time_point deadline) const noexcept;

private:
    CursorProvider provider_;
    size_t max_materialized_bytes_ = 0;
};

class InputFdAttachmentClient {
public:
    // Any failure returns an invalid descriptor and closes all transport-owned
    // handles; callers must treat this as a single fail-closed mode.
    [[nodiscard]] static InputFdAttachmentResult attach(
        const std::string& socket_path, InputFdRequest request,
        const local::CredentialExpectation& expected_peer,
        std::chrono::steady_clock::time_point deadline) noexcept;
};

// Nonblocking client-side equivalent of InputFdAttachmentClient::attach().
// The caller owns scheduling: advance() performs one bounded progress step
// and never waits internally.  The absolute deadline covers every phase.
class InputFdAttachmentOperation {
public:
    InputFdAttachmentOperation(
        std::string socket_path, InputFdRequest request,
        local::CredentialExpectation expected_peer,
        std::chrono::steady_clock::time_point deadline) noexcept;
    ~InputFdAttachmentOperation() noexcept;
    InputFdAttachmentOperation(const InputFdAttachmentOperation&) = delete;
    InputFdAttachmentOperation& operator=(const InputFdAttachmentOperation&) = delete;

    void advance(short revents = 0) noexcept;
    void cancel() noexcept;
    [[nodiscard]] bool done() const noexcept { return done_; }
    [[nodiscard]] InputFdAttachmentStatus status() const noexcept { return result_.status; }
    [[nodiscard]] int poll_fd() const noexcept;
    [[nodiscard]] short poll_events() const noexcept;
    [[nodiscard]] std::chrono::steady_clock::time_point next_wakeup() const noexcept;
    [[nodiscard]] std::optional<InputFdAttachmentResult> take_result() noexcept;
    [[nodiscard]] const InputFdRequest& request() const noexcept { return request_; }

private:
    enum class Phase : uint8_t { Connect, Verify, SendHello, ReceiveHelloAck,
                                 SendRequest, ReceiveResult, Handoff, Done };
    void fail(InputFdAttachmentStatus status) noexcept;
    void finish_result(InputFdAttachmentResult result) noexcept;
    void advance_frame() noexcept;

    std::string socket_path_;
    InputFdRequest request_{};
    local::CredentialExpectation expected_peer_{};
    std::chrono::steady_clock::time_point deadline_{};
    Phase phase_ = Phase::Connect;
    bool done_ = false;
    bool result_taken_ = false;
    InputFdAttachmentResult result_{};
    local::UnixConnectOperation connect_;
    std::optional<local::Connection> connection_;
    std::unique_ptr<local::FrameOperation> frame_;
    std::unique_ptr<local::AsyncFdHandoffReceiver> handoff_;
};

#if defined(ICECC_P50_INPUT_FD_ATTACHMENT_TEST_HOOKS)
// Test-only progress seam used to prove the absolute deadline is checked
// during, not merely around, chunked materialization. Production objects do
// not declare or emit this symbol.
using InputMaterializationProgressTestHook = void (*)() noexcept;
void test_set_input_materialization_progress_hook(
    InputMaterializationProgressTestHook hook) noexcept;
#endif

}  // namespace icecc::p50
