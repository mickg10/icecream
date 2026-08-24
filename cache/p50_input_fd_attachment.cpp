#include "p50_input_fd_attachment.h"
#include "p50_control_operation.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <span>
#include <stdexcept>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#if defined(__linux__)
#include <linux/memfd.h>
#include <sys/syscall.h>
#endif

namespace icecc::p50 {
namespace {

#if defined(__linux__) && defined(SYS_memfd_create) && defined(MFD_CLOEXEC) && \
    defined(MFD_ALLOW_SEALING) && defined(F_ADD_SEALS) && defined(F_GET_SEALS) && \
    defined(F_SEAL_SEAL) && defined(F_SEAL_SHRINK) && defined(F_SEAL_GROW) && \
    defined(F_SEAL_WRITE)
constexpr bool kSealedMemfdBuildSupport = true;
#else
constexpr bool kSealedMemfdBuildSupport = false;
#endif

constexpr size_t kMaterializationChunkBytes = 64u * 1024u;

#if defined(ICECC_P50_INPUT_FD_ATTACHMENT_TEST_HOOKS)
InputMaterializationProgressTestHook materialization_progress_test_hook = nullptr;
#endif

std::vector<uint8_t> encode_request(const InputFdRequest& request) {
    return local::encode_control_operation(local::make_input_fd_attachment_operation(
        request.identity, request.key, request.owner, request.request_id));
}

bool decode_request(std::span<const uint8_t> wire, local::Identity identity,
                    InputFdRequest& request) noexcept {
    local::ControlOperation operation;
    if (!local::decode_control_operation(wire, operation) ||
        operation.kind != local::ControlOperationKind::InputFdAttachment ||
        operation.identity != identity || !operation.input.has_value() ||
        !operation.owner.has_value())
        return false;
    request.key = *operation.input;
    request.owner = *operation.owner;
    request.request_id = operation.request_id;
    request.identity = identity;
    return true;
}

InputFdAttachmentStatus status_for_local(local::Status status) noexcept {
    switch (status) {
    case local::Status::Ok:
        return InputFdAttachmentStatus::Accepted;
    case local::Status::Timeout:
        return InputFdAttachmentStatus::Timeout;
    case local::Status::StaleGeneration:
        return InputFdAttachmentStatus::StaleIdentity;
    case local::Status::IdentityMismatch:
        return InputFdAttachmentStatus::StaleIdentity;
    case local::Status::PeerCredentialUnavailable:
    case local::Status::PeerCredentialMismatch:
    case local::Status::InvalidArgument:
        return InputFdAttachmentStatus::InvalidArgument;
    case local::Status::CleanEof:
    case local::Status::Truncated:
        return InputFdAttachmentStatus::Disconnected;
    default:
        return InputFdAttachmentStatus::HandshakeFailed;
    }
}

InputFdAttachmentStatus status_for_handoff(local::FdHandoffStatus status) noexcept {
    switch (status) {
    case local::FdHandoffStatus::Accepted:
        return InputFdAttachmentStatus::Accepted;
    case local::FdHandoffStatus::Timeout:
        return InputFdAttachmentStatus::Timeout;
    case local::FdHandoffStatus::Disconnected:
        return InputFdAttachmentStatus::Disconnected;
    case local::FdHandoffStatus::StaleGeneration:
    case local::FdHandoffStatus::IdentityMismatch:
    case local::FdHandoffStatus::RequestMismatch:
        return InputFdAttachmentStatus::StaleIdentity;
    case local::FdHandoffStatus::MissingFd:
    case local::FdHandoffStatus::ExtraFd:
    case local::FdHandoffStatus::Malformed:
    case local::FdHandoffStatus::MessageTruncated:
    case local::FdHandoffStatus::ControlTruncated:
    case local::FdHandoffStatus::TrailingData:
    case local::FdHandoffStatus::UnexpectedControl:
        return InputFdAttachmentStatus::MalformedRequest;
    default:
        return InputFdAttachmentStatus::HandoffFailed;
    }
}

bool write_complete(
    int fd, std::span<const uint8_t> bytes,
    std::chrono::steady_clock::time_point deadline,
    InputFdAttachmentStatus& status) noexcept {
    size_t offset = 0;
    while (offset != bytes.size()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            status = InputFdAttachmentStatus::Timeout;
            return false;
        }
        const size_t chunk =
            std::min(kMaterializationChunkBytes, bytes.size() - offset);
        const ssize_t count = ::write(fd, bytes.data() + offset,
                                      chunk);
        if (count > 0) {
            offset += static_cast<size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

int make_sealed_memfd(
    std::span<const uint8_t> bytes,
    std::chrono::steady_clock::time_point deadline,
    InputFdAttachmentStatus& status) noexcept {
#if defined(__linux__) && defined(SYS_memfd_create) && defined(MFD_CLOEXEC) && \
    defined(MFD_ALLOW_SEALING) && defined(F_ADD_SEALS) && defined(F_GET_SEALS) && \
    defined(F_SEAL_SEAL) && defined(F_SEAL_SHRINK) && defined(F_SEAL_GROW) && \
    defined(F_SEAL_WRITE)
    const int fd = static_cast<int>(::syscall(
        SYS_memfd_create, "icecc-p50-input", MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (fd < 0)
        return -1;
    if (!write_complete(fd, bytes, deadline, status)) {
        ::close(fd);
        return -1;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
        status = InputFdAttachmentStatus::Timeout;
        ::close(fd);
        return -1;
    }
    if (::lseek(fd, 0, SEEK_SET) != 0 ||
        ::fcntl(fd, F_ADD_SEALS,
                F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE) < 0) {
        ::close(fd);
        return -1;
    }
    const int seals = ::fcntl(fd, F_GET_SEALS);
    const int required = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
    if (seals < 0 || (seals & required) != required) {
        ::close(fd);
        return -1;
    }
    struct stat info{};
    if (::fstat(fd, &info) != 0 || info.st_size < 0 ||
        static_cast<uintmax_t>(info.st_size) != bytes.size()) {
        ::close(fd);
        return -1;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
        status = InputFdAttachmentStatus::Timeout;
        ::close(fd);
        return -1;
    }
    return fd;
#else
    (void)bytes;
    (void)deadline;
    (void)status;
    return -1;
#endif
}

int reopen_readonly_memfd(
    int writable_fd, size_t exact_size,
    std::chrono::steady_clock::time_point deadline,
    InputFdAttachmentStatus& status) noexcept {
#if defined(__linux__)
    if (writable_fd < 0 || std::chrono::steady_clock::now() >= deadline) {
        if (writable_fd >= 0)
            status = InputFdAttachmentStatus::Timeout;
        return -1;
    }
    char proc_path[64]{};
    const int length = std::snprintf(proc_path, sizeof(proc_path),
                                     "/proc/self/fd/%d", writable_fd);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(proc_path))
        return -1;
    const int readonly_fd = ::open(proc_path, O_RDONLY | O_CLOEXEC);
    if (readonly_fd < 0)
        return -1;
#if defined(ICECC_P50_INPUT_FD_ATTACHMENT_TEST_HOOKS)
    if (materialization_progress_test_hook != nullptr)
        materialization_progress_test_hook();
#endif
    if (std::chrono::steady_clock::now() >= deadline) {
        status = InputFdAttachmentStatus::Timeout;
        (void)::close(readonly_fd);
        return -1;
    }
    const int status_flags = ::fcntl(readonly_fd, F_GETFL);
    const int descriptor_flags = ::fcntl(readonly_fd, F_GETFD);
    struct stat info{};
    if (status_flags < 0 || (status_flags & O_ACCMODE) != O_RDONLY ||
        descriptor_flags < 0 || (descriptor_flags & FD_CLOEXEC) == 0 ||
        ::fstat(readonly_fd, &info) != 0 || info.st_size < 0 ||
        static_cast<uintmax_t>(info.st_size) != exact_size ||
        ::lseek(readonly_fd, 0, SEEK_SET) != 0) {
        (void)::close(readonly_fd);
        return -1;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
        status = InputFdAttachmentStatus::Timeout;
        (void)::close(readonly_fd);
        return -1;
    }
    return readonly_fd;
#else
    (void)writable_fd;
    (void)exact_size;
    (void)deadline;
    (void)status;
    return -1;
#endif
}

int materialize(InputCursor cursor, size_t max_bytes,
                std::chrono::steady_clock::time_point deadline,
                InputFdAttachmentStatus& status) noexcept {
    try {
        if (std::chrono::steady_clock::now() >= deadline) {
            status = InputFdAttachmentStatus::Timeout;
            return -1;
        }
        const size_t remaining = cursor.remaining();
        if (remaining > max_bytes) {
            status = InputFdAttachmentStatus::MaterializationFailed;
            return -1;
        }
        std::vector<uint8_t> bytes;
        bytes.reserve(remaining);
        if (std::chrono::steady_clock::now() >= deadline) {
            status = InputFdAttachmentStatus::Timeout;
            return -1;
        }
        icecc::Digest128Builder digest;
        while (bytes.size() != remaining) {
            if (std::chrono::steady_clock::now() >= deadline) {
                status = InputFdAttachmentStatus::Timeout;
                return -1;
            }
#if defined(ICECC_P50_INPUT_FD_ATTACHMENT_TEST_HOOKS)
            if (materialization_progress_test_hook != nullptr)
                materialization_progress_test_hook();
#endif
            const size_t offset = bytes.size();
            const size_t chunk =
                std::min(kMaterializationChunkBytes, remaining - offset);
            bytes.resize(offset + chunk);
            const size_t count =
                cursor.read(std::span<uint8_t>(bytes).subspan(offset, chunk));
            if (count == 0 || count > chunk) {
                status = InputFdAttachmentStatus::MaterializationFailed;
                return -1;
            }
            bytes.resize(offset + count);
            digest.append(std::span<const uint8_t>(bytes).subspan(offset, count));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            status = InputFdAttachmentStatus::Timeout;
            return -1;
        }
        if (cursor.raw_digest() != digest.finish()) {
            status = InputFdAttachmentStatus::MaterializationFailed;
            return -1;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            status = InputFdAttachmentStatus::Timeout;
            return -1;
        }
        const int writable_fd = make_sealed_memfd(bytes, deadline, status);
        if (writable_fd < 0)
            status = status == InputFdAttachmentStatus::Timeout
                         ? status
                         : kSealedMemfdBuildSupport
                               ? InputFdAttachmentStatus::MaterializationFailed
                               : InputFdAttachmentStatus::UnsupportedPlatform;
        if (writable_fd < 0)
            return -1;
        const int readonly_fd = reopen_readonly_memfd(
            writable_fd, bytes.size(), deadline, status);
        (void)::close(writable_fd);
        if (readonly_fd < 0) {
            if (status != InputFdAttachmentStatus::Timeout)
                status = InputFdAttachmentStatus::MaterializationFailed;
            return -1;
        }
        return readonly_fd;
    } catch (...) {
        status = InputFdAttachmentStatus::MaterializationFailed;
        return -1;
    }
}

bool queued_data(int fd) noexcept {
#if defined(MSG_DONTWAIT)
    struct pollfd descriptor{fd, POLLIN, 0};
    const int ready = ::poll(&descriptor, 1, 0);
    if (ready < 0)
        return true;
    if (ready == 0)
        return false;
    if ((descriptor.revents & (POLLERR | POLLNVAL)) != 0)
        return true;
    if ((descriptor.revents & POLLIN) == 0)
        return false;
    uint8_t byte = 0;
    const ssize_t count = ::recv(fd, &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT);
    return count > 0;
#else
    (void)fd;
    return true;
#endif
}

InputFdAttachmentResult rejected(InputFdAttachmentStatus status) noexcept {
    InputFdAttachmentResult result;
    result.status = status;
    return result;
}

}  // namespace

const char* input_fd_attachment_status_name(
    InputFdAttachmentStatus status) noexcept {
    switch (status) {
    case InputFdAttachmentStatus::Accepted: return "accepted";
    case InputFdAttachmentStatus::InvalidArgument: return "invalid-argument";
    case InputFdAttachmentStatus::UnsupportedPlatform: return "unsupported-platform";
    case InputFdAttachmentStatus::PeerUnauthenticated: return "peer-unauthenticated";
    case InputFdAttachmentStatus::HandshakeFailed: return "handshake-failed";
    case InputFdAttachmentStatus::MalformedRequest: return "malformed-request";
    case InputFdAttachmentStatus::UnknownRecord: return "unknown-record";
    case InputFdAttachmentStatus::StaleIdentity: return "stale-identity";
    case InputFdAttachmentStatus::Timeout: return "timeout";
    case InputFdAttachmentStatus::Disconnected: return "disconnected";
    case InputFdAttachmentStatus::MaterializationFailed: return "materialization-failed";
    case InputFdAttachmentStatus::HandoffFailed: return "handoff-failed";
    }
    return "unknown";
}

InputFd::~InputFd() { reset(); }

void InputFd::reset() noexcept {
    if (fd_ >= 0)
        (void)::close(fd_);
    fd_ = -1;
}

InputFdAttachmentService::InputFdAttachmentService(CursorProvider provider,
                                                   size_t max_materialized_bytes)
    : provider_(std::move(provider)), max_materialized_bytes_(max_materialized_bytes) {
    if (!provider_ || max_materialized_bytes_ == 0)
        throw std::invalid_argument("input FD attachment service configuration");
}

InputFdAttachmentResult InputFdAttachmentService::serve(
    local::Connection& connection, local::Identity expected_identity,
    const local::CredentialExpectation& expected_peer,
    std::chrono::steady_clock::time_point deadline) const noexcept {
    if (!connection.valid() || expected_identity.generation == 0 ||
        expected_identity.attempt == 0 || !expected_peer.specified())
        return rejected(InputFdAttachmentStatus::InvalidArgument);
    const local::Status credential_status =
        connection.verify_peer_credentials(expected_peer);
    if (credential_status != local::Status::Ok)
        return rejected(InputFdAttachmentStatus::PeerUnauthenticated);
    if (std::chrono::steady_clock::now() >= deadline)
        return rejected(InputFdAttachmentStatus::Timeout);

    local::Frame hello;
    local::Status io = connection.receive_until(hello, deadline);
    if (io != local::Status::Ok)
        return rejected(status_for_local(io));
    io = local::validate_handshake(hello, local::MessageType::Hello,
                                   local::PeerRole::Daemon, expected_identity);
    if (io != local::Status::Ok)
        return rejected(status_for_local(io));
    io = connection.send_until(local::make_hello_ack(local::PeerRole::Sidecar,
                                                     expected_identity), deadline);
    if (io != local::Status::Ok)
        return rejected(status_for_local(io));

    local::Frame request_frame;
    io = connection.receive_until(request_frame, deadline);
    if (io != local::Status::Ok)
        return rejected(status_for_local(io));
    if (request_frame.type != local::MessageType::Data)
        return rejected(InputFdAttachmentStatus::MalformedRequest);
    if (local::validate_identity(request_frame, expected_identity) != local::Status::Ok)
        return rejected(InputFdAttachmentStatus::StaleIdentity);
    InputFdRequest request;
    if (!decode_request(request_frame.payload, expected_identity, request))
        return rejected(InputFdAttachmentStatus::MalformedRequest);
    return serve_request(connection, expected_identity, expected_peer, std::move(request),
                         deadline);
}

InputFdAttachmentResult InputFdAttachmentService::serve_request(
    local::Connection& connection, local::Identity expected_identity,
    const local::CredentialExpectation& expected_peer, InputFdRequest request,
    std::chrono::steady_clock::time_point deadline) const noexcept {
    if (!connection.valid() || expected_identity.generation == 0 ||
        expected_identity.attempt == 0 || !expected_peer.specified() ||
        request.identity != expected_identity || request.key.c_store_guid == CStoreGuid{} ||
        !input_lease_owner_valid(request.owner) || request.request_id == 0)
        return rejected(InputFdAttachmentStatus::InvalidArgument);
    if (connection.verify_peer_credentials(expected_peer) != local::Status::Ok)
        return rejected(InputFdAttachmentStatus::PeerUnauthenticated);
    if (queued_data(connection.native_handle()))
        return rejected(InputFdAttachmentStatus::MalformedRequest);
    if (std::chrono::steady_clock::now() >= deadline)
        return rejected(InputFdAttachmentStatus::Timeout);

    InputCursor cursor;
    try {
        cursor = provider_(request.key);
    } catch (...) {
        return rejected(InputFdAttachmentStatus::UnknownRecord);
    }
    if (!cursor)
        return rejected(InputFdAttachmentStatus::UnknownRecord);

    InputFdAttachmentStatus materialization_status =
        InputFdAttachmentStatus::MaterializationFailed;
    const int fd = materialize(std::move(cursor), max_materialized_bytes_,
                               deadline, materialization_status);
    if (fd < 0)
        return rejected(materialization_status);
    if (std::chrono::steady_clock::now() >= deadline) {
        (void)::close(fd);
        return rejected(InputFdAttachmentStatus::Timeout);
    }

    local::FdHandoffSender sender{local::HandoffFd(fd)};
    const local::HandoffRequest handoff_request{
        expected_identity.generation, expected_identity.attempt,
        request.request_id};
    InputFdAttachmentResult result;
    result.handoff = sender.send(connection, handoff_request, deadline);
    result.status = status_for_handoff(result.handoff.status);
    return result;
}

InputFdAttachmentResult InputFdAttachmentClient::attach(
    const std::string& socket_path, InputFdRequest request,
    const local::CredentialExpectation& expected_peer,
    std::chrono::steady_clock::time_point deadline) noexcept {
    if (request.identity.generation == 0 || request.identity.attempt == 0 ||
        request.key.c_store_guid == CStoreGuid{} ||
        !input_lease_owner_valid(request.owner) || request.request_id == 0 ||
        !expected_peer.specified())
        return rejected(InputFdAttachmentStatus::InvalidArgument);
    local::Status status = local::Status::InvalidArgument;
    local::Connection connection =
        local::connect_unix_until(socket_path, deadline, &status);
    if (!connection.valid())
        return rejected(status_for_local(status));
    status = connection.verify_peer_credentials(expected_peer);
    if (status != local::Status::Ok)
        return rejected(InputFdAttachmentStatus::PeerUnauthenticated);
    status = connection.send_until(local::make_hello(local::PeerRole::Daemon,
                                                     request.identity), deadline);
    if (status != local::Status::Ok)
        return rejected(status_for_local(status));
    local::Frame reply;
    status = connection.receive_until(reply, deadline);
    if (status != local::Status::Ok)
        return rejected(status_for_local(status));
    status = local::validate_handshake(reply, local::MessageType::HelloAck,
                                       local::PeerRole::Sidecar,
                                       request.identity);
    if (status != local::Status::Ok)
        return rejected(status_for_local(status));
    local::Frame request_frame{local::kProtocolVersion, local::MessageType::Data,
                               request.identity, encode_request(request)};
    status = connection.send_until(request_frame, deadline);
    if (status != local::Status::Ok)
        return rejected(status_for_local(status));

    local::FdHandoffReceiver receiver;
    const local::HandoffRequest handoff_request{
        request.identity.generation, request.identity.attempt,
        request.request_id};
    InputFdAttachmentResult result;
    result.handoff = receiver.receive_and_ack(connection, handoff_request, deadline);
    result.status = status_for_handoff(result.handoff.status);
    if (result.handoff.status == local::FdHandoffStatus::Accepted) {
        local::HandoffFd fd = receiver.take_adopted_fd();
        if (!fd.valid())
            return rejected(InputFdAttachmentStatus::HandoffFailed);
        const int status_flags = ::fcntl(fd.get(), F_GETFL);
        const int descriptor_flags = ::fcntl(fd.get(), F_GETFD);
        struct stat info{};
        if (status_flags < 0 || (status_flags & O_ACCMODE) != O_RDONLY ||
            descriptor_flags < 0 || (descriptor_flags & FD_CLOEXEC) == 0 ||
            ::fstat(fd.get(), &info) != 0 || !S_ISREG(info.st_mode) ||
            ::lseek(fd.get(), 0, SEEK_SET) != 0)
            return rejected(InputFdAttachmentStatus::HandoffFailed);
        result.fd = InputFd(fd.release());
        result.lease = request;
    }
    return result;
}

#if defined(ICECC_P50_INPUT_FD_ATTACHMENT_TEST_HOOKS)
void test_set_input_materialization_progress_hook(
    InputMaterializationProgressTestHook hook) noexcept {
    materialization_progress_test_hook = hook;
}
#endif

}  // namespace icecc::p50
