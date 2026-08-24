#pragma once

#include "p50_fd_handoff.h"
#include "p50_input_record.h"
#include "p50_local_transport.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <compare>
#include <functional>
#include <string>
#include <utility>

namespace icecc::p50 {

// This is the only compiler-visible identity in this adapter.  The cache key
// is deliberately exact and independent of compiler ATTEMPT_ID; the local
// transport identity authenticates the current daemon/sidecar relationship.
struct InputFdRequest {
    local::Identity identity{};
    InputRecordKey key{};
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
    // complete memfd creation, FD reply, and its ACK.
    [[nodiscard]] InputFdAttachmentResult serve(
        local::Connection& connection, local::Identity expected_identity,
        const local::CredentialExpectation& expected_peer,
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

}  // namespace icecc::p50
