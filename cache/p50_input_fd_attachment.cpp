#include "p50_input_fd_attachment.h"

#include <algorithm>
#include <cerrno>
#include <span>
#include <stdexcept>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <linux/memfd.h>
#include <sys/syscall.h>
#endif

namespace icecc::p50 {
namespace {

constexpr size_t kRequestBytes = 16 + 8 + 8;

#if defined(__linux__) && defined(SYS_memfd_create) && defined(MFD_CLOEXEC) && \
    defined(MFD_ALLOW_SEALING) && defined(F_ADD_SEALS) && defined(F_GET_SEALS) && \
    defined(F_SEAL_SEAL) && defined(F_SEAL_SHRINK) && defined(F_SEAL_GROW) && \
    defined(F_SEAL_WRITE)
constexpr bool kSealedMemfdBuildSupport = true;
#else
constexpr bool kSealedMemfdBuildSupport = false;
#endif

void put_u64(uint8_t* output, uint64_t value) noexcept {
    for (size_t index = 0; index != sizeof(value); ++index)
        output[index] = static_cast<uint8_t>(value >> (56 - index * 8));
}

uint64_t get_u64(const uint8_t* input) noexcept {
    uint64_t value = 0;
    for (size_t index = 0; index != sizeof(value); ++index)
        value = (value << 8) | input[index];
    return value;
}

std::vector<uint8_t> encode_request(const InputFdRequest& request) {
    std::vector<uint8_t> wire(kRequestBytes);
    std::copy(request.key.c_store_guid.bytes.begin(),
              request.key.c_store_guid.bytes.end(), wire.begin());
    put_u64(wire.data() + 16, request.key.tu_seq.value);
    put_u64(wire.data() + 24, request.request_id);
    return wire;
}

bool decode_request(std::span<const uint8_t> wire, local::Identity identity,
                    InputFdRequest& request) noexcept {
    if (wire.size() != kRequestBytes)
        return false;
    std::copy(wire.begin(), wire.begin() + 16,
              request.key.c_store_guid.bytes.begin());
    request.key.tu_seq.value = get_u64(wire.data() + 16);
    request.request_id = get_u64(wire.data() + 24);
    request.identity = identity;
    return request.key.c_store_guid != CStoreGuid{} && request.request_id != 0;
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

bool write_complete(int fd, std::span<const uint8_t> bytes) noexcept {
    size_t offset = 0;
    while (offset != bytes.size()) {
        const ssize_t count = ::write(fd, bytes.data() + offset,
                                      bytes.size() - offset);
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

int make_sealed_memfd(std::span<const uint8_t> bytes) noexcept {
#if defined(__linux__) && defined(SYS_memfd_create) && defined(MFD_CLOEXEC) && \
    defined(MFD_ALLOW_SEALING) && defined(F_ADD_SEALS) && defined(F_GET_SEALS) && \
    defined(F_SEAL_SEAL) && defined(F_SEAL_SHRINK) && defined(F_SEAL_GROW) && \
    defined(F_SEAL_WRITE)
    const int fd = static_cast<int>(::syscall(
        SYS_memfd_create, "icecc-p50-input", MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (fd < 0)
        return -1;
    if (!write_complete(fd, bytes) ||
        ::lseek(fd, 0, SEEK_SET) != 0 ||
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
    return fd;
#else
    (void)bytes;
    return -1;
#endif
}

int materialize(InputCursor cursor, size_t max_bytes,
                InputFdAttachmentStatus& status) noexcept {
    try {
        const size_t remaining = cursor.remaining();
        if (remaining > max_bytes) {
            status = InputFdAttachmentStatus::MaterializationFailed;
            return -1;
        }
        std::vector<uint8_t> bytes;
        bytes.resize(remaining);
        size_t offset = 0;
        while (offset != bytes.size()) {
            const size_t count =
                cursor.read(std::span<uint8_t>(bytes).subspan(offset));
            if (count == 0 || count > bytes.size() - offset) {
                status = InputFdAttachmentStatus::MaterializationFailed;
                return -1;
            }
            offset += count;
        }
        if (cursor.raw_digest() != icecc::digest128(bytes)) {
            status = InputFdAttachmentStatus::MaterializationFailed;
            return -1;
        }
        const int fd = make_sealed_memfd(bytes);
        if (fd < 0)
            status = kSealedMemfdBuildSupport
                         ? InputFdAttachmentStatus::MaterializationFailed
                         : InputFdAttachmentStatus::UnsupportedPlatform;
        return fd;
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
                               materialization_status);
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
        request.key.c_store_guid == CStoreGuid{} || request.request_id == 0 ||
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
        if (::lseek(fd.get(), 0, SEEK_SET) != 0)
            return rejected(InputFdAttachmentStatus::HandoffFailed);
        result.fd = InputFd(fd.release());
    }
    return result;
}

}  // namespace icecc::p50
