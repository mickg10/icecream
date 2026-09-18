#include "cache/p50_input_fd_attachment.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <poll.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>

using namespace std::chrono_literals;
using namespace icecc::p50;

namespace {

[[noreturn]] void fail(const char* message) {
    std::cerr << "p50inputattachasync: " << message << '\n';
    std::exit(1);
}

void require(bool value, const char* message) {
    if (!value)
        fail(message);
}

void put16(std::vector<uint8_t>& wire, size_t offset, uint16_t value) {
    wire[offset] = static_cast<uint8_t>(value >> 8);
    wire[offset + 1] = static_cast<uint8_t>(value);
}

void put64(std::vector<uint8_t>& wire, size_t offset, uint64_t value) {
    for (unsigned i = 0; i != 8; ++i)
        wire[offset + i] = static_cast<uint8_t>(value >> (56 - i * 8));
}

std::vector<uint8_t> accepted_result(uint64_t request_id) {
    std::vector<uint8_t> wire(16, 0);
    wire[0] = 'P';
    wire[1] = '5';
    wire[2] = 'I';
    wire[3] = 'R';
    put16(wire, 4, 1);
    put16(wire, 6, static_cast<uint16_t>(InputFdAttachmentStatus::Accepted));
    put64(wire, 8, request_id);
    return wire;
}

InputFdRequest request_for(local::Identity identity) {
    return InputFdRequest{identity,
                          InputRecordKey{Id128::from_u64(7), TuSeq{9}},
                          InputLeaseOwner{1, 2, 3}, 1};
}

InputFdAttachmentResult drive(InputFdAttachmentOperation& operation,
                              std::chrono::milliseconds max_wait = 2s) {
    const auto stop = std::chrono::steady_clock::now() + max_wait;
    while (!operation.done()) {
        if (std::chrono::steady_clock::now() >= stop)
            fail("operation did not reach a terminal state");
        const int fd = operation.poll_fd();
        const short events = operation.poll_events();
        if (fd < 0 || events == 0) {
            operation.advance();
            continue;
        }
        pollfd descriptor{fd, events, 0};
        const int ready = ::poll(&descriptor, 1, 20);
        require(ready >= 0, "poll failed");
        operation.advance(ready == 0 ? 0 : descriptor.revents);
    }
    auto result = operation.take_result();
    require(result.has_value(), "terminal operation did not expose a result");
    return std::move(*result);
}

struct Server {
    std::filesystem::path directory;
    std::string path;
    int listener = -1;
    std::thread thread;

    Server() = default;
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&& other) noexcept
        : directory(std::move(other.directory)), path(std::move(other.path)),
          listener(std::exchange(other.listener, -1)),
          thread(std::move(other.thread)) {}
    Server& operator=(Server&&) = delete;

    ~Server() {
        if (thread.joinable())
            thread.join();
        if (listener >= 0)
            ::close(listener);
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
};

Server make_server(const char* name) {
    Server server;
    const char* root = std::getenv("TMPDIR");
    std::string pattern = std::string(root && *root ? root : "/tmp") +
                          "/p50attach-" + name + ".XXXXXX";
    require(::mkdtemp(pattern.data()) != nullptr,
            "private test directory creation failed");
    server.directory = pattern;
    server.path = (server.directory / "control.sock").string();
    local::Status status = local::Status::InvalidArgument;
    server.listener = local::listen_unix(server.path, 2, &status);
    require(server.listener >= 0 && status == local::Status::Ok,
            "test listener creation failed");
    return server;
}

void send_success(local::Connection& connection, local::Identity identity) {
    require(connection.verify_peer_credentials(local::CredentialExpectation{
                static_cast<uint64_t>(::geteuid()), static_cast<uint64_t>(::getegid()),
                static_cast<uint64_t>(::getpid())}) == local::Status::Ok,
            "server peer credentials rejected");
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    local::Frame hello;
    require(connection.receive_until(hello, deadline) == local::Status::Ok,
            "server failed to receive hello");
    require(connection.send_until(local::make_hello_ack(local::PeerRole::Sidecar,
                                                         identity), deadline) ==
                local::Status::Ok,
            "server failed to send hello acknowledgement");
    local::Frame request;
    require(connection.receive_until(request, deadline) == local::Status::Ok,
            "server failed to receive request");
    require(connection.send_until(
                local::Frame{local::kProtocolVersion, local::MessageType::Data,
                             identity, accepted_result(1)},
                deadline) == local::Status::Ok,
            "server failed to send accepted result");

    const char* root = std::getenv("TMPDIR");
    std::string template_path = std::string(root && *root ? root : "/tmp") +
                                "/p50inputattach-fd-XXXXXX";
    const int temporary = ::mkstemp(template_path.data());
    require(temporary >= 0, "temporary handoff file creation failed");
    require(::write(temporary, "async", 5) == 5, "temporary handoff write failed");
    ::close(temporary);
    const int input = ::open(template_path.c_str(), O_RDONLY | O_CLOEXEC);
    ::unlink(template_path.c_str());
    require(input >= 0, "temporary handoff reopen failed");
    local::FdHandoffSender sender{local::HandoffFd(input)};
    const auto handoff = sender.send(
        connection, local::HandoffRequest{identity, 1}, deadline);
    require(handoff.status == local::FdHandoffStatus::Accepted,
            "server handoff failed");
}

}  // namespace

int main() {
    const local::Identity identity{11, 13};
    const InputFdRequest request = request_for(identity);
    const local::CredentialExpectation peer{static_cast<uint64_t>(::geteuid()),
                                            std::nullopt, std::nullopt};

    {
        Server server = make_server("success");
        server.thread = std::thread([&] {
            local::Status status = local::Status::InvalidArgument;
            auto connection = local::accept_unix(server.listener, &status);
            require(status == local::Status::Ok && connection.valid(),
                    "success server accept failed");
            send_success(connection, identity);
        });
        InputFdAttachmentOperation operation(
            server.path, request, peer, std::chrono::steady_clock::now() + 2s);
        auto result = drive(operation);
        require(result.status == InputFdAttachmentStatus::Accepted && result.fd.valid(),
                "async success did not accept descriptor");
        char bytes[5]{};
        require(::read(result.fd.get(), bytes, sizeof(bytes)) == 5 &&
                    std::string(bytes, sizeof(bytes)) == "async",
                "async descriptor was not positioned at byte zero");
    }

    {
        Server server = make_server("wrong-identity");
        server.thread = std::thread([&] {
            local::Status status = local::Status::InvalidArgument;
            auto connection = local::accept_unix(server.listener, &status);
            require(status == local::Status::Ok && connection.valid(),
                    "wrong-identity server accept failed");
            const auto deadline = std::chrono::steady_clock::now() + 2s;
            local::Frame hello;
            require(connection.receive_until(hello, deadline) == local::Status::Ok,
                    "wrong-identity server failed to receive hello");
            require(connection.send_until(
                        local::make_hello_ack(local::PeerRole::Sidecar,
                                              local::Identity{11, 14}),
                        deadline) == local::Status::Ok,
                    "wrong-identity acknowledgement failed");
        });
        InputFdAttachmentOperation operation(
            server.path, request, peer, std::chrono::steady_clock::now() + 2s);
        auto result = drive(operation);
        require(result.status == InputFdAttachmentStatus::HandshakeFailed,
                "wrong identity was not rejected");
    }

    {
        Server server = make_server("auth");
        server.thread = std::thread([&] {
            local::Status status = local::Status::InvalidArgument;
            auto connection = local::accept_unix(server.listener, &status);
            require(status == local::Status::Ok && connection.valid(),
                    "auth server accept failed");
            std::this_thread::sleep_for(100ms);
        });
        const local::CredentialExpectation impossible{static_cast<uint64_t>(::geteuid()),
                                                       std::nullopt,
                                                       static_cast<uint64_t>(::getpid() + 1)};
        InputFdAttachmentOperation operation(
            server.path, request, impossible, std::chrono::steady_clock::now() + 2s);
        auto result = drive(operation);
        require(result.status == InputFdAttachmentStatus::PeerUnauthenticated,
                "peer credential mismatch was not rejected");
    }

    {
        Server server = make_server("deadline");
        server.thread = std::thread([&] {
            local::Status status = local::Status::InvalidArgument;
            auto connection = local::accept_unix(server.listener, &status);
            require(status == local::Status::Ok && connection.valid(),
                    "deadline server accept failed");
            std::this_thread::sleep_for(150ms);
        });
        InputFdAttachmentOperation operation(
            server.path, request, peer, std::chrono::steady_clock::now() + 25ms);
        auto result = drive(operation, 1s);
        require(result.status == InputFdAttachmentStatus::Timeout,
                "absolute deadline was not enforced");
    }

    {
        Server server = make_server("cancel");
        server.thread = std::thread([&] {
            local::Status status = local::Status::InvalidArgument;
            auto connection = local::accept_unix(server.listener, &status);
            require(status == local::Status::Ok && connection.valid(),
                    "cancel server accept failed");
            std::this_thread::sleep_for(100ms);
        });
        InputFdAttachmentOperation operation(
            server.path, request, peer, std::chrono::steady_clock::now() + 2s);
        for (unsigned i = 0; i != 100 && operation.poll_fd() < 0; ++i)
            operation.advance();
        operation.cancel();
        auto result = operation.take_result();
        require(result.has_value() && result->status == InputFdAttachmentStatus::Disconnected,
                "cancel did not produce a terminal disconnected result");
        require(operation.poll_fd() < 0 && operation.poll_events() == 0,
                "cancel did not close transport state");
    }
}
