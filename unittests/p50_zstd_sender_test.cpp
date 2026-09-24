#include "client/p50_zstd_sender.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_future.hpp>

#include <array>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <future>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

using namespace icecc::p50;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

namespace {

void check(bool value, const char* expression) {
    if (!value) throw std::runtime_error(expression);
}

#define CHECK(expression) check((expression), #expression)

struct TemporaryFile {
    std::string path;
    int fd = -1;
};

TemporaryFile create_temporary_file(const char* name) {
    const char* configured = std::getenv("TMPDIR");
    const std::filesystem::path directory =
        configured != nullptr && configured[0] != '\0' ? configured : "/tmp";
    std::string pattern = (directory / (std::string(name) + "-XXXXXX")).string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    const int fd = ::mkstemp(writable.data());
    return {fd < 0 ? std::string{} : std::string(writable.data()), fd};
}

ZstdSourceTransferConfig config() {
    ZstdSourceTransferConfig result;
    result.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    result.endpoint_caps.zstd.max_raw_bytes = 1U << 20;
    result.endpoint_caps.zstd.max_encoded_body_bytes = 1U << 20;
    return result;
}

ZstdSourceTransferConfig route_config() {
    ZstdSourceTransferConfig result = config();
    result.compression_level = 3;
    result.endpoint_caps.profile = ProfileId::ZSTD_ROUTE;
    return result;
}

ClaimAttemptCapability128 sender_r2_capability(uint8_t seed) {
    ClaimAttemptCapability128 result;
    for (size_t index = 0; index != result.bytes.size(); ++index)
        result.bytes[index] = static_cast<uint8_t>(seed + index);
    CHECK(result.valid());
    return result;
}

std::pair<CStoreGuid, FStoreGuid> sender_r2_store_guids() {
    std::array<uint8_t, 16> c{};
    std::array<uint8_t, 16> f{};
    for (size_t index = 0; index != c.size(); ++index) {
        c[index] = static_cast<uint8_t>(7 + index);
        f[index] = static_cast<uint8_t>(71 + index);
    }
    c[kStoreIdentityRoleByte] &= static_cast<uint8_t>(~kStoreIdentityRoleMask);
    f[kStoreIdentityRoleByte] |= kStoreIdentityRoleMask;
    CHECK(store_identity_guid_valid_for_role(c, kStoreIdentityClientRole));
    CHECK(store_identity_guid_valid_for_role(f, kStoreIdentityFileRole));
    CHECK(!store_identity_file_guid_matches_client(c, f));
    return {CStoreGuid{c}, FStoreGuid{f}};
}

P51SourceArmFields sender_r2_arm(CStoreGuid c_guid, uint64_t request_id,
                                 uint32_t wire_job, uint32_t window,
                                 ProfileId profile) {
    P51SourceArmFields arm;
    arm.source.wire_job_id = wire_job;
    arm.source.assignment_epoch = 3;
    arm.source.assignment_nonce = request_id;
    arm.source.selected_f_host = "127.0.0.1";
    arm.source.selected_f_ordinary_port = 42001;
    arm.source.selected_f_cache_port = 42002;
    arm.source.cache_protocol = 2;
    arm.source.cache_profile = profile == ProfileId::P29V1
                                   ? CACHE_PROFILE_P29V1
                               : profile == ProfileId::ZSTD_ROUTE
                                   ? CACHE_PROFILE_ZSTD_ROUTE
                                   : CACHE_PROFILE_ZSTD_TU;
    arm.source.logical_job = 700 + wire_job;
    arm.source.compiler_attempt = 800 + wire_job;
    arm.source.c_store_generation = 11;
    arm.source.c_store_derivation_version = kStoreIdentityDerivationVersion;
    arm.source.c_store_guid = c_guid.bytes;
    arm.source.source_request_id = request_id;
    arm.source.source_mode = profile == ProfileId::P29V1
                                 ? P50_SOURCE_MODE_P29V1
                             : profile == ProfileId::ZSTD_ROUTE
                                 ? P50_SOURCE_MODE_ZSTD_ROUTE
                                 : P50_SOURCE_MODE_ZSTD_TU;
    arm.source.c_control_generation = 12;
    arm.source.c_control_attempt = 13;
    arm.requested_window = window;
    CHECK(arm.valid());
    return arm;
}

P51SourceArmedFields sender_r2_armed(P51SourceArmFields arm, FStoreGuid f_guid,
                                    uint64_t reservation, uint32_t window) {
    P51SourceArmedFields armed;
    armed.arm = std::move(arm);
    armed.f_control_generation = 21;
    armed.f_control_attempt = 22;
    armed.f_store_generation = 23;
    armed.f_store_guid = f_guid.bytes;
    armed.f_store_derivation_version = kStoreIdentityDerivationVersion;
    armed.arm_observation_id = 24;
    armed.source_budget_msec = 9000;
    armed.attempt_capability_1 = sender_r2_capability(31);
    armed.attempt_capability_2 = sender_r2_capability(51);
    armed.reservation_id = Id128::from_u64(reservation).bytes;
    armed.logical_relationship_id = Id128::from_u64(0x5102).bytes;
    armed.relationship_epoch = 25;
    armed.selected_revision = CACHE_WIRE_REVISION_R2;
    armed.selected_window = window;
    CHECK(armed.valid());
    return armed;
}

int connect_fd(tcp::endpoint remote) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    const int descriptor_flags = ::fcntl(fd, F_GETFD);
    if (descriptor_flags < 0 ||
        ::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
        (void)::close(fd);
        return -1;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(remote.port());
    const auto bytes = remote.address().to_v4().to_bytes();
    std::memcpy(&address.sin_addr, bytes.data(), bytes.size());
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                  sizeof(address)) != 0) {
        (void)::close(fd);
        return -1;
    }
    return fd;
}

void test_exact_network_transfer() {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    P50ServerEndpointConfig server_config;
    server_config.input_job_state = [](CStoreGuid, const TxBegin&, const TxCommit&,
                                       std::span<const uint8_t>) {
        return InputJobState::Open;
    };
    P50ServerEndpoint server(Id128::from_u64(7002), {}, nullptr, nullptr, server_config);
    P50ZstdSourceSender sender(Id128::from_u64(7001), PrepareRequestKey{91, 7}, config());
    const std::vector<uint8_t> source{'#', ' ', 'd', 'e', 'f', 'i', 'n', 'e', '\n'};
    std::future<ServerRunResult> server_result = asio::co_spawn(
        context, server.accept_one(acceptor), asio::use_future);
    std::future<ZstdSourceTransferResult> sender_result = asio::co_spawn(
        context, sender.transfer(acceptor.local_endpoint(), source), asio::use_future);
    context.run();
    const ZstdSourceTransferResult result = sender_result.get();
    CHECK(server_result.get().status == ServerRunStatus::Completed);
    CHECK(result.status == ZstdSourceTransferStatus::Committed);
    CHECK(result.committed_input.has_value());
    CHECK(result.committed_input->c_store_guid == Id128::from_u64(7001));
    CHECK(result.committed_input->tu_seq.value == 0);
    CHECK(result.raw_bytes == source.size());
    CHECK(result.raw_digest == icecc::digest128(source));
    CHECK(result.attempts == 1);
    CHECK(result.c_to_f_bytes > source.size());
    CHECK(result.f_to_c_bytes > 0);
    CHECK(!result.system_source_reuse.has_value());
}

void test_owned_fd_and_fail_closed_validation() {
    const TemporaryFile temporary = create_temporary_file("p50-zstd-sender-test");
    const int write_fd = temporary.fd;
    CHECK(write_fd >= 0);
    CHECK(::fchmod(write_fd, 0600) == 0);
    const std::string source = "#define owned_fd 1\n";
    CHECK(::write(write_fd, source.data(), source.size()) ==
          static_cast<ssize_t>(source.size()));
    CHECK(::close(write_fd) == 0);
    const int read_fd = ::open(temporary.path.c_str(), O_RDONLY | O_CLOEXEC);
    CHECK(read_fd >= 0);

    asio::io_context context;
    P50ZstdSourceSender sender(Id128::from_u64(7003), PrepareRequestKey{92, 8}, config());
    auto result = asio::co_spawn(
        context,
        sender.transfer({asio::ip::address_v4::loopback(), 0}, OwnedSourceFd(read_fd)),
        asio::use_future);
    context.run();
    CHECK(result.get().status == ZstdSourceTransferStatus::InvalidRequest);
    CHECK(::unlink(temporary.path.c_str()) == 0);
}

void test_owned_fd_release_transfers_single_ownership() {
    const TemporaryFile temporary = create_temporary_file("p50-owned-source-release");
    const int fd = temporary.fd;
    CHECK(fd >= 0);
    CHECK(::unlink(temporary.path.c_str()) == 0);
    {
        OwnedSourceFd source(fd);
        CHECK(source.get() == fd);
        CHECK(source.release() == fd);
        CHECK(!source);
    }
    CHECK(::fcntl(fd, F_GETFD) >= 0);
    CHECK(::close(fd) == 0);
}

void test_adopted_fd_factory_exact_transfer() {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    P50ServerEndpointConfig server_config;
    server_config.input_job_state = [](CStoreGuid, const TxBegin&, const TxCommit&,
                                       std::span<const uint8_t>) {
        return InputJobState::Open;
    };
    P50ServerEndpoint server(Id128::from_u64(7012), {}, nullptr, nullptr,
                             server_config);
    P50ZstdSourceSender sender(Id128::from_u64(7011), PrepareRequestKey{96, 12},
                               config());
    const std::vector<uint8_t> source{'a', 'd', 'o', 'p', 't', 'e', 'd'};
    unsigned calls = 0;
    ConnectedFdFactory factory = [remote = acceptor.local_endpoint(), &calls](auto deadline) {
        ++calls;
        if (std::chrono::steady_clock::now() >= deadline)
            return -1;
        return connect_fd(remote);
    };
    std::future<ServerRunResult> server_result = asio::co_spawn(
        context, server.accept_one(acceptor), asio::use_future);
    std::future<ZstdSourceTransferResult> sender_result = asio::co_spawn(
        context, sender.transfer(std::move(factory), source), asio::use_future);
    context.run();
    const ZstdSourceTransferResult result = sender_result.get();
    CHECK(server_result.get().status == ServerRunStatus::Completed);
    CHECK(result.status == ZstdSourceTransferStatus::Committed);
    CHECK(result.committed_input.has_value());
    CHECK(result.committed_input->c_store_guid == Id128::from_u64(7011));
    CHECK(result.raw_digest == icecc::digest128(source));
    CHECK(result.attempts == 1);
    CHECK(result.c_to_f_bytes > source.size());
    CHECK(result.f_to_c_bytes > 0);
    CHECK(!result.system_source_reuse.has_value());
    CHECK(calls == 1);
}

void test_async_fd_factory_exact_transfer() {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    EndpointCaps caps;
    caps.profile = ProfileId::ZSTD_ROUTE;
    caps.zstd.max_raw_bytes = 1U << 20;
    caps.zstd.max_encoded_body_bytes = 1U << 20;
    P50ServerEndpointConfig server_config;
    server_config.input_job_state = [](CStoreGuid, const TxBegin&,
                                       const TxCommit&,
                                       std::span<const uint8_t>) {
        return InputJobState::Open;
    };
    P50ServerEndpoint server(Id128::from_u64(7032), caps, nullptr, nullptr,
                             server_config);
    P50ZstdSourceSender sender(Id128::from_u64(7031),
                               PrepareRequestKey{99, 1}, route_config());
    const std::vector<uint8_t> source{'a', 's', 'y', 'n', 'c'};
    unsigned calls = 0;
    int duplicate_fd = -1;
    AsyncConnectedFdFactory factory =
        [remote = acceptor.local_endpoint(), &calls,
         &duplicate_fd](auto deadline, auto completion) {
            ++calls;
            const int fd = std::chrono::steady_clock::now() < deadline
                               ? connect_fd(remote)
                               : -1;
            completion(fd);
            duplicate_fd = connect_fd(remote);
            completion(duplicate_fd);
        };
    auto server_result = asio::co_spawn(context, server.accept_one(acceptor),
                                         asio::use_future);
    auto sender_result = asio::co_spawn(
        context,
        sender.transfer_route(std::move(factory), PrepareRequestKey{99, 1},
                              std::chrono::steady_clock::now() +
                                  std::chrono::seconds(3),
                              source),
        asio::use_future);
    context.run();
    const auto result = sender_result.get();
    CHECK(server_result.get().status == ServerRunStatus::Completed);
    CHECK(result.status == ZstdSourceTransferStatus::Committed);
    CHECK(result.raw_digest == icecc::digest128(source));
    CHECK(result.attempts == 1);
    CHECK(calls == 1);
    errno = 0;
    CHECK(::fcntl(duplicate_fd, F_GETFD) == -1 && errno == EBADF);
}

void test_async_fd_factory_late_and_throwing_completion_close_fds() {
    {
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        auto sender_config = route_config();
        sender_config.deadline = std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds(150);
        P50ZstdSourceSender sender(Id128::from_u64(7041),
                                   PrepareRequestKey{100, 1}, sender_config);
        const std::vector<uint8_t> source{'l', 'a', 't', 'e'};
        std::function<void(int)> late_completion;
        unsigned calls = 0;
        AsyncConnectedFdFactory factory =
            [&late_completion, &calls](auto, auto completion) {
                ++calls;
                if (calls == 1)
                    completion(-1);
                else
                    late_completion = std::move(completion);
            };
        auto result = asio::co_spawn(
            context,
            sender.transfer_route(std::move(factory), PrepareRequestKey{100, 1},
                                  sender_config.deadline, source),
            asio::use_future);
        context.run();
        CHECK(result.get().status == ZstdSourceTransferStatus::DeadlineExceeded);
        CHECK(calls == 2);
        CHECK(static_cast<bool>(late_completion));
        const int fd = connect_fd(acceptor.local_endpoint());
        CHECK(fd >= 0);
        late_completion(fd);
        errno = 0;
        CHECK(::fcntl(fd, F_GETFD) == -1 && errno == EBADF);
    }
    {
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        auto sender_config = route_config();
        sender_config.deadline = std::chrono::steady_clock::now() +
                                 std::chrono::seconds(3);
        P50ZstdSourceSender sender(Id128::from_u64(7051),
                                   PrepareRequestKey{101, 1}, sender_config);
        const std::vector<uint8_t> source{'t', 'h', 'r', 'o', 'w'};
        std::vector<int> descriptors;
        unsigned calls = 0;
        AsyncConnectedFdFactory factory =
            [&descriptors, &calls, remote = acceptor.local_endpoint()](
                auto, auto completion) {
                ++calls;
                const int fd = connect_fd(remote);
                descriptors.push_back(fd);
                completion(fd);
                throw std::runtime_error("factory threw after completion");
            };
        auto result = asio::co_spawn(
            context,
            sender.transfer_route(std::move(factory), PrepareRequestKey{101, 1},
                                  sender_config.deadline, source),
            asio::use_future);
        context.run();
        CHECK(result.get().status == ZstdSourceTransferStatus::RetryExhausted);
        CHECK(calls == 2);
        CHECK(descriptors.size() == 2);
        for (const int fd : descriptors) {
            errno = 0;
            CHECK(::fcntl(fd, F_GETFD) == -1 && errno == EBADF);
        }
    }
}

void test_route_sender_reuses_relationship_for_two_transfers() {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    EndpointCaps caps;
    caps.profile = ProfileId::ZSTD_ROUTE;
    caps.zstd.max_raw_bytes = 1U << 20;
    caps.zstd.max_encoded_body_bytes = 1U << 20;
    P50ServerEndpointConfig server_config;
    server_config.input_job_state = [](CStoreGuid, const TxBegin&, const TxCommit&,
                                       std::span<const uint8_t>) {
        return InputJobState::Open;
    };
    P50ServerEndpoint server(Id128::from_u64(7022), caps, nullptr, nullptr,
                             server_config);
    P50ZstdSourceSender sender(Id128::from_u64(7021), PrepareRequestKey{98, 1},
                               route_config());
    const std::vector<uint8_t> first{'r', 'o', 'u', 't', 'e', '-', '1'};
    const std::vector<uint8_t> second{'r', 'o', 'u', 't', 'e', '-', '2'};

    auto first_server = asio::co_spawn(context, server.accept_one(acceptor),
                                        asio::use_future);
    auto first_transfer = asio::co_spawn(
        context, sender.transfer(acceptor.local_endpoint(), first), asio::use_future);
    context.run();
    const auto first_result = first_transfer.get();
    CHECK(first_server.get().status == ServerRunStatus::Completed);
    CHECK(first_result.status == ZstdSourceTransferStatus::Committed);
    CHECK(first_result.committed_input.has_value());
    CHECK(first_result.committed_input->c_store_guid == Id128::from_u64(7021));
    CHECK(first_result.committed_input->tu_seq.value == 0);

    context.restart();
    auto second_server = asio::co_spawn(context, server.accept_one(acceptor),
                                         asio::use_future);
    auto second_transfer = asio::co_spawn(
        context, sender.transfer(acceptor.local_endpoint(), second), asio::use_future);
    context.run();
    const auto second_result = second_transfer.get();
    CHECK(second_server.get().status == ServerRunStatus::Completed);
    CHECK(second_result.status == ZstdSourceTransferStatus::Committed);
    CHECK(second_result.committed_input.has_value());
    CHECK(second_result.committed_input->c_store_guid == Id128::from_u64(7021));
    CHECK(second_result.committed_input->tu_seq.value == 1);

    server.reset_store(Id128::from_u64(7023));
    P50ZstdSourceSender reset_sender(Id128::from_u64(7024),
                                     PrepareRequestKey{99, 1}, route_config());
    const std::vector<uint8_t> after_reset{'r', 'o', 'u', 't', 'e', '-', 'r'};
    context.restart();
    auto reset_server = asio::co_spawn(context, server.accept_one(acceptor),
                                        asio::use_future);
    auto reset_transfer = asio::co_spawn(
        context, reset_sender.transfer(acceptor.local_endpoint(), after_reset),
        asio::use_future);
    context.run();
    const auto reset_result = reset_transfer.get();
    CHECK(reset_server.get().status == ServerRunStatus::Completed);
    CHECK(reset_result.status == ZstdSourceTransferStatus::Committed);
    CHECK(reset_result.committed_input.has_value());
    CHECK(reset_result.committed_input->c_store_guid == Id128::from_u64(7024));
    CHECK(reset_result.committed_input->tu_seq.value == 0);
}

void test_route_completed_ledger_releases_live_entry() {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    EndpointCaps caps;
    caps.profile = ProfileId::ZSTD_ROUTE;
    caps.zstd.max_raw_bytes = 1U << 20;
    caps.zstd.max_encoded_body_bytes = 1U << 20;
    P50ServerEndpointConfig server_config;
    server_config.input_job_state = [](CStoreGuid, const TxBegin&, const TxCommit&,
                                       std::span<const uint8_t>) {
        return InputJobState::Open;
    };
    P50ServerEndpoint server(Id128::from_u64(7062), caps, nullptr, nullptr,
                             server_config);
    ZstdSourceTransferConfig bounded = route_config();
    bounded.authority_limits.max_live_entries = 1;
    bounded.max_completed_requests = 2;
    P50ZstdSourceSender sender(Id128::from_u64(7061), PrepareRequestKey{106, 1},
                               bounded);
    const PrepareRequestKey first_request{9001, 1};
    const PrepareRequestKey second_request{9001, 2};
    const std::vector<uint8_t> first{'l', 'e', 'd', 'g', 'e', 'r', '-', '1'};
    const std::vector<uint8_t> second{'l', 'e', 'd', 'g', 'e', 'r', '-', '2'};
    auto first_server = asio::co_spawn(context, server.accept_one(acceptor),
                                        asio::use_future);
    auto first_transfer = asio::co_spawn(
        context, sender.transfer_route(acceptor.local_endpoint(), first_request,
                                       std::chrono::steady_clock::now() +
                                           std::chrono::seconds(10), first),
        asio::use_future);
    context.run();
    CHECK(first_server.get().status == ServerRunStatus::Completed);
    const auto first_result = first_transfer.get();
    CHECK(first_result.status == ZstdSourceTransferStatus::Committed);
    CHECK(first_result.committed_input->tu_seq.value == 0);

    context.restart();
    auto second_server = asio::co_spawn(context, server.accept_one(acceptor),
                                         asio::use_future);
    auto second_transfer = asio::co_spawn(
        context, sender.transfer_route(acceptor.local_endpoint(), second_request,
                                       std::chrono::steady_clock::now() +
                                           std::chrono::seconds(10), second),
        asio::use_future);
    context.run();
    CHECK(second_server.get().status == ServerRunStatus::Completed);
    const auto second_result = second_transfer.get();
    CHECK(second_result.status == ZstdSourceTransferStatus::Committed);
    CHECK(second_result.committed_input->tu_seq.value == 1);

    // The completed ledger makes this a local replay: no acceptor is armed
    // and the connection factory must not be invoked.
    unsigned replay_connections = 0;
    context.restart();
    auto replay = asio::co_spawn(
        context,
        sender.transfer_route(
            ConnectedFdFactory{[&replay_connections](auto) {
                ++replay_connections;
                return -1;
            }}, first_request,
            std::chrono::steady_clock::now() + std::chrono::seconds(10), first),
        asio::use_future);
    context.run();
    const auto replay_result = replay.get();
    CHECK(replay_result.status == ZstdSourceTransferStatus::Committed);
    CHECK(replay_result.committed_input == first_result.committed_input);
    CHECK(replay_connections == 0);

    const std::vector<uint8_t> different{'d', 'i', 'f', 'f'};
    context.restart();
    auto conflicting = asio::co_spawn(
        context,
        sender.transfer_route(
            ConnectedFdFactory{[&replay_connections](auto) {
                ++replay_connections;
                return -1;
            }}, first_request,
            std::chrono::steady_clock::now() + std::chrono::seconds(10), different),
        asio::use_future);
    context.run();
    CHECK(conflicting.get().status == ZstdSourceTransferStatus::InvalidRequest);
    CHECK(replay_connections == 0);

    // A distinct third identity cannot exceed the bounded replay ledger.  It
    // requests a cold sidecar replacement before opening F, while a completed
    // exact replay remains authoritative and connection-free after the cap.
    unsigned cap_connections = 0;
    context.restart();
    auto capped = asio::co_spawn(
        context,
        sender.transfer_route(
            ConnectedFdFactory{[&cap_connections](auto) {
                ++cap_connections;
                return -1;
            }}, PrepareRequestKey{9001, 3},
            std::chrono::steady_clock::now() + std::chrono::seconds(10), second),
        asio::use_future);
    context.run();
    const auto capped_result = capped.get();
    CHECK(capped_result.status == ZstdSourceTransferStatus::Unavailable);
    CHECK(capped_result.replacement_required);
    CHECK(capped_result.attempts == 0);
    CHECK(cap_connections == 0);

    context.restart();
    auto replay_after_cap = asio::co_spawn(
        context,
        sender.transfer_route(
            ConnectedFdFactory{[&cap_connections](auto) {
                ++cap_connections;
                return -1;
            }}, first_request,
            std::chrono::steady_clock::now() + std::chrono::seconds(10), first),
        asio::use_future);
    context.run();
    CHECK(replay_after_cap.get().status == ZstdSourceTransferStatus::Committed);
    CHECK(cap_connections == 0);
}

asio::awaitable<ServerRunResult> sender_r2_accept(
    tcp::acceptor& acceptor, P50ServerEndpoint& endpoint) {
    tcp::socket socket(co_await asio::this_coro::executor);
    co_await acceptor.async_accept(socket, asio::use_awaitable);
    socket.set_option(tcp::socket::receive_buffer_size(4096));
    co_return co_await endpoint.run_adopted_r2(std::move(socket));
}

void test_p51_sender_w30_concurrent_callers_refill_and_duplicate() {
    for (const ProfileId profile : {ProfileId::ZSTD_TU, ProfileId::P29V1,
                                    ProfileId::ZSTD_ROUTE}) {
    const char* const profile_name = profile == ProfileId::P29V1 ? "P29V1"
                                    : profile == ProfileId::ZSTD_ROUTE ? "ZSTD_ROUTE"
                                    : "ZSTD_TU";
    constexpr size_t kJobs = 31;
    constexpr size_t kWindow = 30;
    const auto [c_guid, f_guid] = sender_r2_store_guids();
    const Id128 relationship_id = Id128::from_u64(0x5102);
    asio::io_context f_context;
    tcp::acceptor acceptor(f_context, {asio::ip::address_v4::loopback(), 0});

    std::vector<P51SourceArmedFields> armed;
    std::vector<std::vector<uint8_t>> input;
    armed.reserve(kJobs);
    input.reserve(kJobs);
    for (size_t index = 0; index != kJobs; ++index) {
        P51SourceArmFields arm = sender_r2_arm(
            c_guid, 100 + index, static_cast<uint32_t>(700 + index), kWindow,
            profile);
        armed.push_back(sender_r2_armed(std::move(arm), f_guid,
                                        0x520000 + index, kWindow));
        std::vector<uint8_t> bytes(16U << 10);
        uint32_t state = static_cast<uint32_t>(index + 1);
        for (uint8_t& byte : bytes) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            byte = static_cast<uint8_t>(state);
        }
        input.push_back(std::move(bytes));
    }
    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto deadline = sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
        std::chrono::steady_clock::now() + std::chrono::seconds(30),
        clock.clock_domain_id, clock.time_namespace_id);

    std::mutex progress_mutex;
    std::condition_variable progress_cv;
    std::atomic<bool> hold_receipt_reader{true};
    std::mutex ack_mutex;
    std::condition_variable ack_cv;
    std::atomic<unsigned> bundles_sent{0};
    std::atomic<unsigned> committed{0};
    std::atomic<unsigned> acknowledged{0};
    std::atomic<unsigned> exact_input_mismatches{0};
    std::vector<bool> reservation_consumed(kJobs, false);
    std::mutex consumed_mutex;
    P50ServerEndpointConfig server_config;
    EndpointCaps server_caps;
    server_caps.profile = profile;
    server_caps.supported_profiles = profile_bit(profile);
    server_config.input_job_state = [&, profile](CStoreGuid, const TxBegin& begin,
                                                  const TxCommit& commit,
                                                  std::span<const uint8_t> bytes) {
        if (begin.profile != profile || begin.tu_seq.value >= input.size() ||
            bytes.size() != input[begin.tu_seq.value].size() ||
            !std::equal(bytes.begin(), bytes.end(),
                        input[begin.tu_seq.value].begin()) ||
            commit.raw_digest != icecc::digest128(input[begin.tu_seq.value]))
            exact_input_mismatches.fetch_add(1, std::memory_order_relaxed);
        return InputJobState::Open;
    };
    server_config.lookup_p51_link_reservation =
        [&, deadline](const LinkHello& hello)
            -> std::optional<P51SourceLinkLease> {
        if (hello.profile != profile || hello.window != kWindow ||
            hello.reservation_id != Id128{armed.front().reservation_id} ||
            hello.relationship_id != relationship_id ||
            hello.relationship_epoch != armed.front().relationship_epoch ||
            hello.c_store_guid != c_guid || hello.f_store_guid != f_guid ||
            hello.physical_link_generation != 27)
            return std::nullopt;
        P51SourceLinkLease lease;
        lease.initial_armed = armed.front();
        lease.absolute_deadline = deadline;
        return lease;
    };
    server_config.consume_p51_job_reservation =
        [&, deadline](const LinkHello& hello, const JobBind& binding)
            -> std::optional<P51SourceJobLease> {
        if (hello.relationship_id != relationship_id ||
            binding.physical_link_generation != 27 ||
            binding.profile != profile || binding.tu_seq.value >= kJobs)
            return std::nullopt;
        size_t index = kJobs;
        for (size_t i = 0; i != kJobs; ++i) {
            if (binding.reservation_id == Id128{armed[i].reservation_id}) {
                index = i;
                break;
            }
        }
        if (index == kJobs || binding.raw_bytes != input[index].size() ||
            binding.raw_digest != icecc::digest128(input[index]) ||
            binding.wire_job_id != armed[index].arm.source.wire_job_id ||
            binding.assignment_nonce != armed[index].arm.source.assignment_nonce ||
            binding.source_request_id != armed[index].arm.source.source_request_id)
            return std::nullopt;
        {
            std::lock_guard lock(consumed_mutex);
            if (reservation_consumed[index]) return std::nullopt;
            reservation_consumed[index] = true;
        }
        P51SourceJobLease lease;
        lease.armed = armed[index];
        lease.absolute_deadline = deadline;
        lease.binding = binding;
        lease.binding_digest = compute_r2_binding_digest(binding);
        lease.input_key = InputRecordKey{c_guid, binding.tu_seq};
        return lease;
    };
    server_config.record_p51_job_commit =
        [&](const LinkHello& hello, const JobBind& binding, const R2TxCommit&) {
            if (hello.relationship_id != relationship_id || binding.tu_seq.value >= kJobs)
                return false;
            committed.fetch_add(1, std::memory_order_release);
            progress_cv.notify_all();
            return true;
        };
    server_config.acknowledge_p51_receipt =
        [&](const LinkHello& hello, const CommitAck& ack) {
            if (hello.relationship_id != relationship_id ||
                ack.relationship_id != relationship_id ||
                ack.physical_link_generation != 27)
                return false;
            acknowledged.store(static_cast<unsigned>(ack.contiguous_verified_ordinal),
                               std::memory_order_release);
            ack_cv.notify_all();
            return true;
        };
    P50ServerEndpoint server(f_guid, server_caps, nullptr, nullptr,
                             std::move(server_config));
    auto server_future = asio::co_spawn(f_context,
        sender_r2_accept(acceptor, server), asio::use_future);
    std::thread f_thread([&] { f_context.run(); });

    PreparationAuthorityLimits limits;
    limits.max_speculative_tus = kWindow;
    limits.max_speculative_raw_bytes = 2U << 20;
    limits.max_live_entries = kJobs + 4;
    EndpointCaps caps;
    caps.profile = profile;
    caps.supported_profiles = profile_bit(profile);
    caps.zstd.max_raw_bytes = 1U << 20;
    caps.zstd.max_encoded_body_bytes = 1U << 20;
    auto authority = std::make_shared<P50PreparationAuthority>(
        c_guid, caps.zstd, limits, 1, profile);
    const PreparationRouteKey route{f_guid, 23, profile};
    ZstdSourceTransferConfig sender_config = config();
    sender_config.endpoint_caps = caps;
    sender_config.authority_limits = limits;
    if (profile == ProfileId::ZSTD_ROUTE)
        sender_config.compression_level = 3;
    sender_config.after_r2_bundle_sent_for_test = [&](uint64_t) {
        bundles_sent.fetch_add(1, std::memory_order_release);
        progress_cv.notify_all();
    };
    sender_config.hold_r2_receipt_reader_for_test = [&] {
        return hold_receipt_reader.load(std::memory_order_acquire);
    };
    auto sender = std::make_shared<P50ZstdSourceSender>(
        authority, route, PrepareRequestKey{3, 100}, sender_config);

    asio::io_context c_context;
    const tcp::endpoint remote = acceptor.local_endpoint();
    std::atomic<unsigned> connector_calls{0};
    AsyncConnectedFdFactory connector = [&](auto, auto completion) {
        connector_calls.fetch_add(1, std::memory_order_relaxed);
        const int fd = connect_fd(remote);
        if (fd >= 0) {
            const int tiny_send_buffer = 4096;
            (void)::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &tiny_send_buffer,
                               sizeof(tiny_send_buffer));
        }
        completion(fd);
    };
    std::vector<std::future<ZstdSourceTransferResult>> results;
    results.reserve(kJobs + 2);
    results.push_back(asio::co_spawn(c_context,
        sender->transfer_p51_route(armed[0], 27, connector,
            PrepareRequestKey{3, 100}, deadline.as_steady_time_point(), input[0]),
        asio::use_future));
    std::thread c_thread([&] { c_context.run(); });
    struct ThreadCleanup {
        asio::io_context& client_context;
        asio::io_context& server_context;
        std::mutex& mutex;
        std::condition_variable& cv;
        std::atomic<bool>& release;
        std::thread& client_thread;
        std::thread& server_thread;
        ~ThreadCleanup() {
            release.store(false, std::memory_order_release);
            cv.notify_all();
            client_context.stop();
            server_context.stop();
            if (client_thread.joinable()) client_thread.join();
            if (server_thread.joinable()) server_thread.join();
        }
    } cleanup{c_context, f_context, progress_mutex, progress_cv,
              hold_receipt_reader, c_thread, f_thread};

    {
        std::unique_lock lock(progress_mutex);
        CHECK(progress_cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return bundles_sent.load(std::memory_order_acquire) >= 1;
        }));
    }
    // Exercise duplicate identities while the first exact transaction is
    // known to be in flight. Exact replay may join or reject locally; a
    // conflicting digest must reject without touching the original job.
    results.push_back(asio::co_spawn(c_context,
        sender->transfer_p51_route(armed[0], 27, connector,
            PrepareRequestKey{3, 100}, deadline.as_steady_time_point(), input[0]),
        asio::use_future));
    std::vector<uint8_t> conflicting = input[0];
    conflicting.front() ^= 0x80;
    results.push_back(asio::co_spawn(c_context,
        sender->transfer_p51_route(armed[0], 27, connector,
            PrepareRequestKey{3, 100}, deadline.as_steady_time_point(), conflicting),
        asio::use_future));
    for (size_t index = 1; index != kJobs; ++index) {
        results.push_back(asio::co_spawn(c_context,
            sender->transfer_p51_route(armed[index], 27, connector,
                PrepareRequestKey{3, 100 + index}, deadline.as_steady_time_point(),
                input[index]), asio::use_future));
    }
    {
        std::unique_lock lock(progress_mutex);
        const auto progress_start = std::chrono::steady_clock::now();
        const auto progress_deadline = deadline.as_steady_time_point();
        const auto reached_window = [&] {
            return bundles_sent.load(std::memory_order_acquire) >= kWindow &&
                   committed.load(std::memory_order_acquire) >= kWindow;
        };
        while (!reached_window() &&
               std::chrono::steady_clock::now() < progress_deadline) {
            const auto next_sample = std::min(
                progress_deadline,
                std::chrono::steady_clock::now() + std::chrono::seconds(1));
            (void)progress_cv.wait_until(lock, next_sample, reached_window);
            std::cerr << "P51 sender W30 sample elapsed_ms="
                      << std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - progress_start)
                             .count()
                      << " bundles_sent="
                      << bundles_sent.load(std::memory_order_acquire)
                      << " committed="
                      << committed.load(std::memory_order_acquire)
                      << " acknowledged="
                      << acknowledged.load(std::memory_order_acquire) << '\n';
        }
        const bool complete = reached_window();
        if (!complete)
            std::cerr << "P51 sender W30 progress: bundles_sent="
                      << bundles_sent.load(std::memory_order_acquire)
                      << " committed="
                      << committed.load(std::memory_order_acquire)
                      << " acknowledged="
                      << acknowledged.load(std::memory_order_acquire)
                      << " consumed=" << std::count(reservation_consumed.begin(),
                                                     reservation_consumed.end(), true)
                      << " ready_results="
                      << std::count_if(results.begin(), results.end(), [](auto& result) {
                             return result.wait_for(std::chrono::seconds(0)) ==
                                    std::future_status::ready;
                         }) << '\n';
        CHECK(complete);
    }
    CHECK(acknowledged.load(std::memory_order_acquire) == 0);
    hold_receipt_reader.store(false, std::memory_order_release);
    progress_cv.notify_all();

    const auto first_result = results.front().get();
    CHECK(first_result.status == ZstdSourceTransferStatus::Committed);
    const auto duplicate_result = results[1].get();
    CHECK(duplicate_result.status == ZstdSourceTransferStatus::Committed ||
          duplicate_result.status == ZstdSourceTransferStatus::InvalidRequest);
    CHECK(results[2].get().status == ZstdSourceTransferStatus::InvalidRequest);
    for (size_t index = 3; index != results.size(); ++index)
        CHECK(results[index].get().status == ZstdSourceTransferStatus::Committed);
    CHECK(committed.load(std::memory_order_acquire) == kJobs);
    CHECK(exact_input_mismatches.load(std::memory_order_relaxed) == 0);
    {
        std::unique_lock lock(ack_mutex);
        CHECK(ack_cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return acknowledged.load(std::memory_order_acquire) == kJobs;
        }));
    }
    CHECK(acknowledged.load(std::memory_order_acquire) == kJobs);
    CHECK(connector_calls.load(std::memory_order_relaxed) == 1);
    sender->retire_for_replacement();
    c_context.stop();
    c_thread.join();
    const ServerRunResult server_result = server_future.get();
    CHECK(server_result.status == ServerRunStatus::Disconnected);
    f_context.stop();
    f_thread.join();
    std::cerr << "P51_SENDER_W30_PROFILE=" << profile_name << " PASS\n";
    }
}

void test_route_failure_requires_cold_replacement() {
    ZstdSourceTransferConfig bounded = route_config();
    bounded.authority_limits.max_live_entries = 1;
    P50ZstdSourceSender sender(Id128::from_u64(7071), PrepareRequestKey{107, 1},
                               bounded);
    const PrepareRequestKey request{9101, 1};
    const std::vector<uint8_t> source{'f', 'a', 'i', 'l'};
    unsigned failed_connections = 0;
    asio::io_context failed_context;
    auto failed = asio::co_spawn(
        failed_context,
        sender.transfer_route(
            ConnectedFdFactory{[&failed_connections](auto) {
                ++failed_connections;
                return -1;
            }}, request,
            std::chrono::steady_clock::now() + std::chrono::seconds(10), source),
        asio::use_future);
    failed_context.run();
    const auto failed_result = failed.get();
    CHECK(failed_result.status == ZstdSourceTransferStatus::RetryExhausted);
    CHECK(failed_result.replacement_required);
    CHECK(failed_connections == 2);

    // The ambiguous preparation is sticky.  A later wrapper cannot reuse the
    // old sender and must not open another F connection.
    unsigned poisoned_connections = 0;
    asio::io_context poisoned_context;
    auto poisoned = asio::co_spawn(
        poisoned_context,
        sender.transfer_route(
            ConnectedFdFactory{[&poisoned_connections](auto) {
                ++poisoned_connections;
                return -1;
            }}, request,
            std::chrono::steady_clock::now() + std::chrono::seconds(10), source),
        asio::use_future);
    poisoned_context.run();
    const auto poisoned_result = poisoned.get();
    CHECK(poisoned_result.status == ZstdSourceTransferStatus::Unavailable);
    CHECK(poisoned_result.replacement_required);
    CHECK(poisoned_result.attempts == 0);
    CHECK(poisoned_connections == 0);

    // Whole-sidecar replacement creates a new C store and authority.  That
    // cold owner can admit the assignment and begins at TU0.
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    EndpointCaps caps;
    caps.profile = ProfileId::ZSTD_ROUTE;
    caps.zstd.max_raw_bytes = 1U << 20;
    caps.zstd.max_encoded_body_bytes = 1U << 20;
    P50ServerEndpointConfig server_config;
    server_config.input_job_state = [](CStoreGuid, const TxBegin&, const TxCommit&,
                                       std::span<const uint8_t>) {
        return InputJobState::Open;
    };
    P50ServerEndpoint server(Id128::from_u64(7072), caps, nullptr, nullptr,
                             server_config);
    P50ZstdSourceSender replacement(
        Id128::from_u64(7073), PrepareRequestKey{108, 1}, bounded);
    auto server_run = asio::co_spawn(context, server.accept_one(acceptor),
                                      asio::use_future);
    auto retry = asio::co_spawn(
        context,
        replacement.transfer_route(
            acceptor.local_endpoint(), request,
            std::chrono::steady_clock::now() + std::chrono::seconds(10), source),
        asio::use_future);
    context.run();
    CHECK(server_run.get().status == ServerRunStatus::Completed);
    const auto retry_result = retry.get();
    CHECK(retry_result.status == ZstdSourceTransferStatus::Committed);
    CHECK(retry_result.committed_input->tu_seq.value == 0);
}

void test_explicit_route_operations_bind_request_and_deadline() {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    EndpointCaps caps;
    caps.profile = ProfileId::ZSTD_ROUTE;
    caps.zstd.max_raw_bytes = 1U << 20;
    caps.zstd.max_encoded_body_bytes = 1U << 20;
    P50ServerEndpointConfig server_config;
    server_config.input_job_state = [](CStoreGuid, const TxBegin&, const TxCommit&,
                                       std::span<const uint8_t>) {
        return InputJobState::Open;
    };
    P50ServerEndpoint server(Id128::from_u64(7032), caps, nullptr, nullptr,
                             server_config);

    // A persistent owner outlives the constructor operation.  Its later
    // transfers must use their own current deadline rather than this stale
    // compatibility deadline.
    ZstdSourceTransferConfig persistent_config = route_config();
    persistent_config.deadline =
        std::chrono::steady_clock::now() - std::chrono::seconds(1);
    P50ZstdSourceSender sender(Id128::from_u64(7031),
                               PrepareRequestKey{100, 1}, persistent_config);
    const PrepareRequestKey first_request{7001, 9001};
    const PrepareRequestKey second_request{7002, 99001};
    const std::vector<uint8_t> first{'e', 'x', 'a', 'c', 't', '-', '1'};
    const std::vector<uint8_t> second{'e', 'x', 'a', 'c', 't', '-', '2'};

    auto first_server = asio::co_spawn(context, server.accept_one(acceptor),
                                        asio::use_future);
    auto first_transfer = asio::co_spawn(
        context,
        sender.transfer_route(acceptor.local_endpoint(), first_request,
                              std::chrono::steady_clock::now() +
                                  std::chrono::seconds(10),
                              first),
        asio::use_future);
    context.run();
    const auto first_result = first_transfer.get();
    CHECK(first_server.get().status == ServerRunStatus::Completed);
    CHECK(first_result.status == ZstdSourceTransferStatus::Committed);
    CHECK(first_result.committed_input.has_value());
    CHECK(first_result.committed_input->tu_seq.value == 0);

    // Reusing an exact producer request for different bytes is rejected
    // before another connection is opened or route history can advance.
    context.restart();
    const std::vector<uint8_t> wrong{'w', 'r', 'o', 'n', 'g'};
    auto wrong_transfer = asio::co_spawn(
        context,
        sender.transfer_route(acceptor.local_endpoint(), first_request,
                              std::chrono::steady_clock::now() +
                                  std::chrono::seconds(10),
                              wrong),
        asio::use_future);
    context.run();
    CHECK(wrong_transfer.get().status == ZstdSourceTransferStatus::InvalidRequest);

    context.restart();
    auto second_server = asio::co_spawn(context, server.accept_one(acceptor),
                                         asio::use_future);
    auto second_transfer = asio::co_spawn(
        context,
        sender.transfer_route(acceptor.local_endpoint(), second_request,
                              std::chrono::steady_clock::now() +
                                  std::chrono::seconds(10),
                              second),
        asio::use_future);
    context.run();
    const auto second_result = second_transfer.get();
    CHECK(second_server.get().status == ServerRunStatus::Completed);
    CHECK(second_result.status == ZstdSourceTransferStatus::Committed);
    CHECK(second_result.committed_input.has_value());
    CHECK(second_result.committed_input->tu_seq.value == 1);

    // ZSTD_TU keeps sequence ownership across wrappers while each transfer
    // still uses the independent per-TU codec path.
    P50ZstdSourceSender tu_sender(Id128::from_u64(7033),
                                  PrepareRequestKey{7033, 1}, config());
    P50ServerEndpoint tu_server(Id128::from_u64(7034), {}, nullptr, nullptr,
                                server_config);
    context.restart();
    auto tu_first_server = asio::co_spawn(context, tu_server.accept_one(acceptor),
                                           asio::use_future);
    auto tu_first = asio::co_spawn(
        context,
        tu_sender.transfer_route(
            acceptor.local_endpoint(), PrepareRequestKey{7033, 2},
            std::chrono::steady_clock::now() + std::chrono::seconds(10), first),
        asio::use_future);
    context.run();
    CHECK(tu_first_server.get().status == ServerRunStatus::Completed);
    const auto tu_first_result = tu_first.get();
    CHECK(tu_first_result.status == ZstdSourceTransferStatus::Committed);
    CHECK(tu_first_result.committed_input->tu_seq.value == 0);

    context.restart();
    auto tu_second_server = asio::co_spawn(context, tu_server.accept_one(acceptor),
                                            asio::use_future);
    auto tu_second = asio::co_spawn(
        context,
        tu_sender.transfer_route(
            acceptor.local_endpoint(), PrepareRequestKey{7033, 3},
            std::chrono::steady_clock::now() + std::chrono::seconds(10), second),
        asio::use_future);
    context.run();
    CHECK(tu_second_server.get().status == ServerRunStatus::Completed);
    const auto tu_second_result = tu_second.get();
    CHECK(tu_second_result.status == ZstdSourceTransferStatus::Committed);
    CHECK(tu_second_result.committed_input->tu_seq.value == 1);
}

void test_explicit_route_retry_is_bounded_then_replaced() {
    ZstdSourceTransferConfig persistent_config = route_config();
    persistent_config.deadline =
        std::chrono::steady_clock::now() - std::chrono::seconds(1);
    P50ZstdSourceSender sender(Id128::from_u64(7041),
                               PrepareRequestKey{7041, 1}, persistent_config);
    const PrepareRequestKey request{8001, 177};
    const std::vector<uint8_t> source{'r', 'e', 't', 'a', 'i', 'n'};

    unsigned failed_connections = 0;
    asio::io_context failed_context;
    auto failed = asio::co_spawn(
        failed_context,
        sender.transfer_route(
            ConnectedFdFactory{[&failed_connections](auto) {
                ++failed_connections;
                return -1;
            }},
            request,
            std::chrono::steady_clock::now() + std::chrono::seconds(10), source),
        asio::use_future);
    failed_context.run();
    const auto failed_result = failed.get();
    CHECK(failed_result.status == ZstdSourceTransferStatus::RetryExhausted);
    CHECK(failed_result.replacement_required);
    CHECK(failed_result.attempts == 2);
    CHECK(failed_connections == 2);

    // Once both same-operation attempts fail, neither the exact request nor a
    // successor may touch F through this retained sender.
    unsigned poisoned_connections = 0;
    asio::io_context poisoned_context;
    auto poisoned = asio::co_spawn(
        poisoned_context,
        sender.transfer_route(
            ConnectedFdFactory{[&poisoned_connections](auto) {
                ++poisoned_connections;
                return -1;
            }},
            PrepareRequestKey{8002, 178},
            std::chrono::steady_clock::now() + std::chrono::seconds(10), source),
        asio::use_future);
    poisoned_context.run();
    const auto poisoned_result = poisoned.get();
    CHECK(poisoned_result.status == ZstdSourceTransferStatus::Unavailable);
    CHECK(poisoned_result.replacement_required);
    CHECK(poisoned_connections == 0);

    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    EndpointCaps caps;
    caps.profile = ProfileId::ZSTD_ROUTE;
    caps.zstd.max_raw_bytes = 1U << 20;
    caps.zstd.max_encoded_body_bytes = 1U << 20;
    P50ServerEndpointConfig server_config;
    server_config.input_job_state = [](CStoreGuid, const TxBegin&, const TxCommit&,
                                       std::span<const uint8_t>) {
        return InputJobState::Open;
    };
    P50ServerEndpoint server(Id128::from_u64(7042), caps, nullptr, nullptr,
                             server_config);
    P50ZstdSourceSender replacement(
        Id128::from_u64(7043), PrepareRequestKey{7043, 1}, persistent_config);
    auto retry_server = asio::co_spawn(context, server.accept_one(acceptor),
                                        asio::use_future);
    auto retry = asio::co_spawn(
        context,
        replacement.transfer_route(
            acceptor.local_endpoint(), request,
            std::chrono::steady_clock::now() + std::chrono::seconds(10), source),
        asio::use_future);
    context.run();
    const auto retry_result = retry.get();
    CHECK(retry_server.get().status == ServerRunStatus::Completed);
    CHECK(retry_result.status == ZstdSourceTransferStatus::Committed);
    CHECK(retry_result.committed_input.has_value());
    CHECK(retry_result.committed_input->tu_seq.value == 0);

    // The replacement owner retains ordinary route history after its first
    // exact commit, so the successor advances to TU1.
    context.restart();
    const std::vector<uint8_t> successor{'s', 'u', 'c', 'c', 'e', 's', 's'};
    auto successor_server = asio::co_spawn(context, server.accept_one(acceptor),
                                            asio::use_future);
    auto successor_transfer = asio::co_spawn(
        context,
        replacement.transfer_route(
            acceptor.local_endpoint(), PrepareRequestKey{8002, 178},
            std::chrono::steady_clock::now() + std::chrono::seconds(10), successor),
        asio::use_future);
    context.run();
    const auto successor_result = successor_transfer.get();
    CHECK(successor_server.get().status == ServerRunStatus::Completed);
    CHECK(successor_result.status == ZstdSourceTransferStatus::Committed);
    CHECK(successor_result.committed_input.has_value());
    CHECK(successor_result.committed_input->tu_seq.value == 1);
}

void test_absolute_deadline_is_required() {
    ZstdSourceTransferConfig expired = config();
    expired.deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    P50ZstdSourceSender sender(Id128::from_u64(7004), PrepareRequestKey{93, 9}, expired);
    asio::io_context context;
    const std::vector<uint8_t> source{'x'};
    auto result = asio::co_spawn(
        context,
        sender.transfer({asio::ip::address_v4::loopback(), 1}, source),
        asio::use_future);
    context.run();
    CHECK(result.get().status == ZstdSourceTransferStatus::DeadlineExceeded);
}

void test_disconnected_retry_is_bounded_and_exactly_once() {
    P50ZstdSourceSender sender(Id128::from_u64(7005), PrepareRequestKey{94, 10}, config());
    asio::io_context context;
    const std::vector<uint8_t> source{'r', 'e', 't', 'r', 'y'};
    auto result = asio::co_spawn(
        context,
        sender.transfer({asio::ip::address_v4::loopback(), 1}, source),
        asio::use_future);
    context.run();
    const ZstdSourceTransferResult transfer = result.get();
    CHECK(transfer.status == ZstdSourceTransferStatus::RetryExhausted);
    CHECK(transfer.attempts == 2);
    CHECK(!transfer.committed_input.has_value());

    unsigned factory_calls = 0;
    P50ZstdSourceSender factory_sender(
        Id128::from_u64(7006), PrepareRequestKey{95, 11}, config());
    asio::io_context factory_context;
    const std::vector<uint8_t> factory_source{'f', 'r', 'e', 's', 'h'};
    auto factory_result = asio::co_spawn(
        factory_context,
        factory_sender.transfer(
            ConnectedFdFactory{[&factory_calls](auto) {
                ++factory_calls;
                return -1;
            }},
            factory_source),
        asio::use_future);
    factory_context.run();
    CHECK(factory_result.get().status == ZstdSourceTransferStatus::RetryExhausted);
    CHECK(factory_calls == 2);
}

void test_factory_cannot_extend_absolute_deadline() {
    ZstdSourceTransferConfig short_config = config();
    short_config.deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
    P50ZstdSourceSender sender(Id128::from_u64(7007), PrepareRequestKey{97, 13},
                               short_config);
    asio::io_context context;
    const std::vector<uint8_t> source{'t', 'i', 'm', 'e'};
    auto result = asio::co_spawn(
        context,
        sender.transfer(
            ConnectedFdFactory{[](auto) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                return -1;
            }},
            source),
        asio::use_future);
    context.run();
    const ZstdSourceTransferResult transfer = result.get();
    CHECK(transfer.status == ZstdSourceTransferStatus::DeadlineExceeded);
    CHECK(transfer.attempts == 1);
}

}  // namespace

int main() {
    test_exact_network_transfer();
    test_route_sender_reuses_relationship_for_two_transfers();
    test_route_completed_ledger_releases_live_entry();
    test_p51_sender_w30_concurrent_callers_refill_and_duplicate();
    test_route_failure_requires_cold_replacement();
    test_explicit_route_operations_bind_request_and_deadline();
    test_explicit_route_retry_is_bounded_then_replaced();
    test_owned_fd_and_fail_closed_validation();
    test_owned_fd_release_transfers_single_ownership();
    test_adopted_fd_factory_exact_transfer();
    test_async_fd_factory_exact_transfer();
    test_async_fd_factory_late_and_throwing_completion_close_fds();
    test_absolute_deadline_is_required();
    test_disconnected_retry_is_bounded_and_exactly_once();
    test_factory_cannot_extend_absolute_deadline();
    std::cerr << "P50_SENDER_W30_ALL_PROFILES=PASS\n";
}
