#include "client/p50_zstd_sender.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/write.hpp>

#include <array>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <future>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <string_view>
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

asio::awaitable<LinkHello> sender_r2_accept_and_reject(
    tcp::socket socket, LinkRejectReason reason) {
    std::array<uint8_t, 4> header_bytes{};
    co_await asio::async_read(socket, asio::buffer(header_bytes),
                              asio::use_awaitable);
    const FrameHeader header = decode_frame_header(header_bytes);
    if (header.type != MessageType::LINK_HELLO)
        throw std::runtime_error("sender did not begin with R2 LINK_HELLO");
    std::vector<uint8_t> payload(header.payload_bytes);
    co_await asio::async_read(socket, asio::buffer(payload), asio::use_awaitable);
    const Message decoded = decode_payload(header.type, payload);
    const LinkHello offered = std::get<LinkHello>(decoded);
    LinkRejectMessage reject;
    reject.reason = reason;
    reject.offered_hello_digest = compute_r2_link_offer_digest(offered);
    const std::vector<uint8_t> frame = encode_frame(Message{reject});
    co_await asio::async_write(socket, asio::buffer(frame), asio::use_awaitable);
    co_return offered;
}

asio::awaitable<LinkHello> sender_r2_accept_and_reject(
    tcp::acceptor& acceptor, LinkRejectReason reason) {
    tcp::socket socket(co_await asio::this_coro::executor);
    co_await acceptor.async_accept(socket, asio::use_awaitable);
    co_return co_await sender_r2_accept_and_reject(std::move(socket), reason);
}

enum class RejectEchoMutation { WrongDigest, StalePhysicalOffer };

asio::awaitable<LinkHello> sender_r2_read_hello(tcp::socket& socket);

asio::awaitable<void> sender_r2_accept_and_reject_with_bad_echo(
    tcp::acceptor& acceptor, RejectEchoMutation mutation) {
    tcp::socket socket(co_await asio::this_coro::executor);
    co_await acceptor.async_accept(socket, asio::use_awaitable);
    const LinkHello offered = co_await sender_r2_read_hello(socket);
    LinkHello digest_offer = offered;
    if (mutation == RejectEchoMutation::StalePhysicalOffer) {
        if (digest_offer.physical_link_generation < 2)
            throw std::runtime_error("fixture cannot construct stale physical offer");
        --digest_offer.physical_link_generation;
    }
    LinkRejectMessage reject;
    reject.reason = LinkRejectReason::ReservationMissing;
    reject.offered_hello_digest = compute_r2_link_offer_digest(digest_offer);
    if (mutation == RejectEchoMutation::WrongDigest)
        reject.offered_hello_digest.bytes[0] ^= 0x80;
    const std::vector<uint8_t> frame = encode_frame(Message{reject});
    co_await asio::async_write(socket, asio::buffer(frame), asio::use_awaitable);
}

asio::awaitable<void> sender_r2_open_expect_bad_reject_echo(
    P50ClientEndpoint& client, LinkHello hello, tcp::endpoint remote,
    std::chrono::steady_clock::time_point deadline,
    std::atomic<bool>& protocol_error, std::atomic<bool>& typed_rejection) {
    const int fd = connect_fd(remote);
    if (fd < 0)
        throw std::runtime_error("fixture connector failed");
    boost::system::error_code error;
    auto socket = P50ClientEndpoint::adopt_connected_fd(
        co_await asio::this_coro::executor, fd, error);
    if (!socket)
        throw boost::system::system_error(error);
    try {
        (void)co_await client.open_r2_link(*socket, hello, deadline);
    } catch (const R2LinkRejected&) {
        typed_rejection.store(true, std::memory_order_release);
    } catch (const std::invalid_argument&) {
        protocol_error.store(true, std::memory_order_release);
    }
}

void test_p51_client_reject_echo_must_match_current_offer() {
    for (const RejectEchoMutation mutation : {
             RejectEchoMutation::WrongDigest,
             RejectEchoMutation::StalePhysicalOffer}) {
        asio::io_context c_context;
        asio::io_context f_context;
        tcp::acceptor acceptor(f_context,
            {asio::ip::address_v4::loopback(), 0});
        const auto [c_guid, f_guid] = sender_r2_store_guids();
        const ProfileId profile = ProfileId::ZSTD_TU;
        P51SourceArmFields arm = sender_r2_arm(c_guid, 1201, 1801, 1,
                                               profile);
        P51SourceArmedFields armed = sender_r2_armed(
            std::move(arm), f_guid, 0x1201, 1);
        EndpointCaps caps;
        caps.profile = profile;
        caps.supported_profiles = profile_bit(profile);
        caps.zstd.max_raw_bytes = 1U << 20;
        caps.zstd.max_encoded_body_bytes = 1U << 20;
        PreparationAuthorityLimits limits;
        limits.max_speculative_tus = 1;
        limits.max_speculative_raw_bytes = 1U << 20;
        limits.max_live_entries = 4;
        auto authority = std::make_shared<P50PreparationAuthority>(
            c_guid, caps.zstd, limits, 1, profile);
        P50ClientEndpoint client(authority, caps);
        LinkHello hello;
        hello.profile = profile;
        hello.window = armed.selected_window;
        hello.max_frame_payload = caps.wire.max_frame_payload;
        hello.max_raw_bytes = caps.zstd.max_raw_bytes;
        hello.max_encoded_bytes = caps.zstd.max_encoded_body_bytes;
        hello.max_output_bytes = caps.zstd.max_raw_bytes;
        hello.reservation_id = Id128{armed.reservation_id};
        hello.relationship_id = Id128{armed.logical_relationship_id};
        hello.relationship_epoch = armed.relationship_epoch;
        hello.physical_link_generation = 27;
        hello.c_store_guid = c_guid;
        hello.c_store_generation = armed.arm.source.c_store_generation;
        hello.f_store_guid = f_guid;
        hello.f_store_generation = armed.f_store_generation;
        hello.c_control_generation = armed.arm.source.c_control_generation;
        hello.c_control_attempt = armed.arm.source.c_control_attempt;
        hello.history_nonce = HistoryNonce{1};

        auto server_future = asio::co_spawn(
            f_context,
            sender_r2_accept_and_reject_with_bad_echo(acceptor, mutation),
            asio::use_future);
        std::thread c_thread;
        std::thread f_thread([&] { f_context.run(); });
        struct EchoThreadCleanup {
            asio::io_context& c_context;
            asio::io_context& f_context;
            std::thread& c_thread;
            std::thread& f_thread;
            ~EchoThreadCleanup() {
                c_context.stop();
                f_context.stop();
                if (c_thread.joinable()) c_thread.join();
                if (f_thread.joinable()) f_thread.join();
            }
        } cleanup{c_context, f_context, c_thread, f_thread};
        std::atomic<bool> protocol_error{false};
        std::atomic<bool> typed_rejection{false};
        auto client_future = asio::co_spawn(c_context,
            sender_r2_open_expect_bad_reject_echo(
                client, hello, acceptor.local_endpoint(),
                std::chrono::steady_clock::now() + std::chrono::seconds(3),
                protocol_error, typed_rejection),
            asio::use_future);
        c_thread = std::thread([&] { c_context.run(); });
        client_future.get();
        server_future.get();
        c_thread.join();
        f_thread.join();
        CHECK(protocol_error.load(std::memory_order_acquire));
        CHECK(!typed_rejection.load(std::memory_order_acquire));
    }
}

asio::awaitable<LinkHello> sender_r2_read_hello(tcp::socket& socket) {
    std::array<uint8_t, 4> header_bytes{};
    co_await asio::async_read(socket, asio::buffer(header_bytes),
                              asio::use_awaitable);
    const FrameHeader header = decode_frame_header(header_bytes);
    if (header.type != MessageType::LINK_HELLO)
        throw std::runtime_error("sender did not begin with R2 LINK_HELLO");
    std::vector<uint8_t> payload(header.payload_bytes);
    co_await asio::async_read(socket, asio::buffer(payload), asio::use_awaitable);
    const Message decoded = decode_payload(header.type, payload);
    co_return std::get<LinkHello>(decoded);
}

void test_p51_sender_typed_link_rejection_is_terminal_and_exact() {
    for (const LinkRejectReason reason : {LinkRejectReason::StoreReplaced,
                                          LinkRejectReason::ReservationMissing}) {
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        const auto [c_guid, f_guid] = sender_r2_store_guids();
        const ProfileId profile = ProfileId::ZSTD_TU;
        P51SourceArmFields arm = sender_r2_arm(c_guid, 991, 71, 1, profile);
        const P51SourceArmedFields armed =
            sender_r2_armed(std::move(arm), f_guid, 992, 1);
        EndpointCaps caps;
        caps.profile = profile;
        caps.supported_profiles = profile_bit(profile);
        caps.zstd.max_raw_bytes = 1U << 20;
        caps.zstd.max_encoded_body_bytes = 1U << 20;
        PreparationAuthorityLimits limits;
        limits.max_speculative_tus = 1;
        limits.max_speculative_raw_bytes = 1U << 20;
        limits.max_live_entries = 4;
        auto authority = std::make_shared<P50PreparationAuthority>(
            c_guid, caps.zstd, limits, 1, profile);
        const PreparationRouteKey route{f_guid, 23, profile};
        ZstdSourceTransferConfig sender_config = config();
        sender_config.endpoint_caps = caps;
        sender_config.authority_limits = limits;
        P50ZstdSourceSender sender(authority, route,
                                   PrepareRequestKey{3, 991}, sender_config);
        const std::vector<uint8_t> source{'t', 'y', 'p', 'e', 'd'};
        unsigned connector_calls = 0;
        const tcp::endpoint remote = acceptor.local_endpoint();
        AsyncConnectedFdFactory connector = [&](auto, auto completion) {
            ++connector_calls;
            completion(connect_fd(remote));
        };
        auto reject_future = asio::co_spawn(
            context, sender_r2_accept_and_reject(acceptor, reason),
            asio::use_future);
        auto transfer_future = asio::co_spawn(
            context,
            sender.transfer_p51_route(
                armed, 27, connector, PrepareRequestKey{3, 991},
                std::chrono::steady_clock::now() + std::chrono::seconds(3),
                source),
            asio::use_future);
        context.run();
        const LinkHello offered = reject_future.get();
        const ZstdSourceTransferResult result = transfer_future.get();
        CHECK(connector_calls == 1);
        CHECK(result.status == ZstdSourceTransferStatus::TerminalError);
        CHECK(result.route_local_failure);
        CHECK(!result.replacement_required);
        CHECK(!result.committed_input.has_value());
        CHECK(result.raw_bytes == 0);
        CHECK(result.r2_link_rejection.has_value());
        CHECK(result.r2_link_rejection->reason == reason);
        CHECK(result.r2_link_rejection->offered == offered);
        CHECK(result.r2_link_rejection->offered.physical_link_generation == 27);
        CHECK(result.r2_link_rejection->offered.relationship_id ==
              Id128{armed.logical_relationship_id});
    }
}

void test_p51_sender_typed_rejection_is_shared_route_local() {
    constexpr size_t kCallers = 30;
    asio::io_context c_context;
    asio::io_context f_context;
    tcp::acceptor acceptor(f_context,
        {asio::ip::address_v4::loopback(), 0});
    const auto [c_guid, f_guid] = sender_r2_store_guids();
    const ProfileId profile = ProfileId::ZSTD_TU;
    std::vector<P51SourceArmedFields> armed;
    std::vector<std::vector<uint8_t>> sources;
    armed.reserve(kCallers);
    sources.reserve(kCallers);
    for (size_t i = 0; i != kCallers; ++i) {
        const uint64_t request_id = 995 + i;
        armed.push_back(sender_r2_armed(
            sender_r2_arm(c_guid, static_cast<uint32_t>(request_id),
                          static_cast<uint32_t>(75 + i), 1, profile),
            f_guid, request_id, 1));
        sources.push_back({'j', static_cast<uint8_t>(i)});
    }
    EndpointCaps caps;
    caps.profile = profile;
    caps.supported_profiles = profile_bit(profile);
    caps.zstd.max_raw_bytes = 1U << 20;
    caps.zstd.max_encoded_body_bytes = 1U << 20;
    PreparationAuthorityLimits limits;
    limits.max_speculative_tus = kCallers;
    limits.max_speculative_raw_bytes = 1U << 20;
    limits.max_live_entries = kCallers + 4;
    auto authority = std::make_shared<P50PreparationAuthority>(
        c_guid, caps.zstd, limits, 1, profile);
    const PreparationRouteKey route{f_guid, 23, profile};
    ZstdSourceTransferConfig sender_config = config();
    sender_config.endpoint_caps = caps;
    sender_config.authority_limits = limits;
    auto sender = std::make_shared<P50ZstdSourceSender>(
        authority, route, PrepareRequestKey{3, 995}, sender_config);
    const tcp::endpoint remote = acceptor.local_endpoint();
    std::atomic<unsigned> connector_calls{0};
    AsyncConnectedFdFactory connector = [&](auto, auto completion) {
        connector_calls.fetch_add(1, std::memory_order_relaxed);
        completion(connect_fd(remote));
    };
    auto reject_future = asio::co_spawn(
        f_context,
        sender_r2_accept_and_reject(acceptor,
                                    LinkRejectReason::ReservationMissing),
        asio::use_future);
    std::thread f_thread([&] { f_context.run(); });
    struct FThreadCleanup {
        asio::io_context& context;
        std::thread& thread;
        ~FThreadCleanup() {
            context.stop();
            if (thread.joinable())
                thread.join();
        }
    } f_thread_cleanup{f_context, f_thread};
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    std::vector<std::future<ZstdSourceTransferResult>> results;
    results.reserve(kCallers);
    for (size_t i = 0; i != kCallers; ++i) {
        results.push_back(asio::co_spawn(
            c_context,
            sender->transfer_p51_route(
                armed[i], 27, connector,
                PrepareRequestKey{3, 995 + i}, deadline, sources[i]),
            asio::use_future));
    }
    c_context.run();
    const LinkHello offered = reject_future.get();
    CHECK(connector_calls.load(std::memory_order_acquire) == 1);
    for (auto& future : results) {
        const ZstdSourceTransferResult result = future.get();
        CHECK(result.status == ZstdSourceTransferStatus::TerminalError);
        CHECK(result.route_local_failure);
        CHECK(!result.replacement_required);
        CHECK(!result.committed_input);
        CHECK(result.r2_link_rejection.has_value());
        CHECK(result.r2_link_rejection->reason ==
              LinkRejectReason::ReservationMissing);
        CHECK(result.r2_link_rejection->offered == offered);
    }
    f_context.stop();
    f_thread.join();
    std::cerr << "P51_SENDER_SHARED_TYPED_REJECTION callers="
              << kCallers << " connectors=1 PASS\n";
}

asio::awaitable<std::vector<ServerRunResult>>
sender_r2_accept_recovery_with_lost_reset_ack(
    tcp::acceptor& acceptor, P50ServerEndpoint& endpoint,
    size_t connection_count, size_t interrupted_materializations,
    bool close_initial_commit = false) {
    std::vector<ServerRunResult> results(connection_count);
    for (size_t index = 0; index != results.size(); ++index) {
        tcp::socket socket(co_await asio::this_coro::executor);
        co_await acceptor.async_accept(socket, asio::use_awaitable);
        socket.set_option(tcp::socket::receive_buffer_size(4096));
        EndpointIoControl control;
        if (index == 0 && close_initial_commit)
            control.close_before_write = MessageType::R2_TX_COMMIT;
        if (index == 1)
            control.close_before_write = MessageType::RESET_ACK;
        if (index == 0 || index == 2) {
            const bool interrupt_this_materialization = index == 0
                ? interrupted_materializations != 0
                : interrupted_materializations > 1;
            if (interrupt_this_materialization) {
                control.before_materialize_on_worker = [] {
                    throw std::runtime_error(
                        "test interrupts retained R2 materialization");
                };
            }
        }
        results[index] = co_await endpoint.run_adopted_r2(
            std::move(socket), std::move(control));
        std::fprintf(stderr, "R2 shared-failure server[%zu] status=%u\n", index,
                     static_cast<unsigned>(results[index].status));
        if (results[index].terminal_error)
            std::fprintf(stderr, "R2 recovery server[%zu]: %s\n", index,
                         results[index].terminal_error->detail.c_str());
    }
    co_return results;
}

asio::awaitable<std::vector<ServerRunResult>>
sender_r2_accept_shared_failure(
    tcp::acceptor& acceptor, P50ServerEndpoint& endpoint,
    EndpointCaps replacement_caps,
    std::atomic<unsigned>& bundles_sent,
    size_t bundles_required, std::mutex& gate_mutex,
    std::condition_variable& gate_cv, bool repeat_recovery_loss,
    bool retire_after_positive_receipt,
    bool expire_after_positive_receipt = false,
    bool reject_stale_reconnect = false,
    std::atomic<bool>* stop_reconnect_listener = nullptr,
    std::atomic<unsigned>* rejected_reconnects = nullptr,
    bool close_reconnect_after_hello = false,
    bool reject_after_positive_receipt = false) {
    const size_t connection_count =
        (reject_stale_reconnect || close_reconnect_after_hello ||
         reject_after_positive_receipt) ? 1
        : repeat_recovery_loss ? 3
        : (retire_after_positive_receipt || expire_after_positive_receipt) ? 1
                                                                           : 2;
    std::vector<ServerRunResult> results(connection_count);
    for (size_t index = 0; index != results.size(); ++index) {
        tcp::socket socket(co_await acceptor.async_accept(asio::use_awaitable));
        socket.set_option(tcp::socket::receive_buffer_size(4096));
        EndpointIoControl control;
        if (index == 0 && reject_after_positive_receipt) {
            control.close_after_write = MessageType::R2_TX_COMMIT;
            control.before_materialize_on_worker = [&] {
                std::unique_lock lock(gate_mutex);
                if (!gate_cv.wait_for(lock, std::chrono::seconds(15), [&] {
                        return bundles_sent.load(std::memory_order_acquire) >=
                               bundles_required;
                    }))
                    throw std::runtime_error(
                        "second caller did not send before positive receipt");
            };
        } else if (index == 0 && !retire_after_positive_receipt &&
            !expire_after_positive_receipt) {
            control.close_before_write = MessageType::R2_TX_COMMIT;
            control.before_materialize_on_worker = [&] {
                std::unique_lock lock(gate_mutex);
                if (!gate_cv.wait_for(lock, std::chrono::seconds(15), [&] {
                        return bundles_sent.load(std::memory_order_acquire) >=
                               bundles_required;
                    }))
                    throw std::runtime_error(
                        "second caller did not send before first receipt loss");
            };
        } else if (index == 1 && repeat_recovery_loss) {
            control.close_before_write = MessageType::RESET_ACK;
        }
        results[index] = co_await endpoint.run_adopted_r2(
            std::move(socket), std::move(control));
        if (results[index].terminal_error)
            std::fprintf(stderr, "R2 shared-failure server[%zu]: %s\n", index,
                         results[index].terminal_error->detail.c_str());
        if (reject_after_positive_receipt) {
            (void)co_await sender_r2_accept_and_reject(
                acceptor, LinkRejectReason::ReservationMissing);
            if (rejected_reconnects != nullptr)
                rejected_reconnects->fetch_add(1, std::memory_order_release);
            co_return results;
        }
        if (close_reconnect_after_hello) {
            boost::system::error_code nonblocking_error;
            acceptor.non_blocking(true, nonblocking_error);
            if (nonblocking_error)
                throw boost::system::system_error(nonblocking_error);
            while (stop_reconnect_listener == nullptr ||
                   !stop_reconnect_listener->load(std::memory_order_acquire)) {
                tcp::socket retry_socket(co_await asio::this_coro::executor);
                boost::system::error_code accept_error;
                acceptor.accept(retry_socket, accept_error);
                if (accept_error == asio::error::would_block ||
                    accept_error == asio::error::try_again) {
                    asio::steady_timer poll(co_await asio::this_coro::executor);
                    poll.expires_after(std::chrono::milliseconds(2));
                    co_await poll.async_wait(asio::use_awaitable);
                    continue;
                }
                if (accept_error)
                    throw boost::system::system_error(accept_error);
                (void)co_await sender_r2_read_hello(retry_socket);
                boost::system::error_code ignored;
                retry_socket.close(ignored);
                if (rejected_reconnects != nullptr)
                    rejected_reconnects->fetch_add(1, std::memory_order_release);
            }
            co_return results;
        }
        if (reject_stale_reconnect) {
            P50ServerEndpointConfig replacement_config;
            P50ServerEndpoint replacement(
                Id128::from_u64(0xf005), replacement_caps, nullptr, nullptr,
                std::move(replacement_config));
            boost::system::error_code nonblocking_error;
            acceptor.non_blocking(true, nonblocking_error);
            if (nonblocking_error)
                throw boost::system::system_error(nonblocking_error);
            while (stop_reconnect_listener == nullptr ||
                   !stop_reconnect_listener->load(std::memory_order_acquire)) {
                tcp::socket retry_socket(co_await asio::this_coro::executor);
                boost::system::error_code accept_error;
                acceptor.accept(retry_socket, accept_error);
                if (accept_error == asio::error::would_block ||
                    accept_error == asio::error::try_again) {
                    asio::steady_timer poll(co_await asio::this_coro::executor);
                    poll.expires_after(std::chrono::milliseconds(2));
                    co_await poll.async_wait(asio::use_awaitable);
                    continue;
                }
                if (accept_error)
                    throw boost::system::system_error(accept_error);
                const ServerRunResult rejected = co_await
                    replacement.run_adopted_r2(std::move(retry_socket));
                if (rejected.status != ServerRunStatus::Disconnected)
                    throw std::runtime_error(
                        "replacement F did not send a typed stale-store rejection");
                if (rejected_reconnects != nullptr)
                    rejected_reconnects->fetch_add(1, std::memory_order_release);
            }
            co_return results;
        }
    }
    co_return results;
}

void test_p51_sender_w30_concurrent_callers_refill_and_duplicate() {
    for (const ProfileId profile : {ProfileId::ZSTD_TU, ProfileId::P29V1,
                                    ProfileId::ZSTD_ROUTE}) {
    const char* const profile_name = profile == ProfileId::P29V1 ? "P29V1"
                                    : profile == ProfileId::ZSTD_ROUTE ? "ZSTD_ROUTE"
                                    : "ZSTD_TU";
    std::cerr << "P51_SENDER_W30_PROFILE_START=" << profile_name << "\n";
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
        lease.relationship_epoch = hello.relationship_epoch;
        lease.history_nonce = hello.history_nonce;
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
    auto c_work = asio::make_work_guard(c_context);
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
        const bool first_bundle_seen = progress_cv.wait_for(
            lock, std::chrono::seconds(5), [&] {
            return bundles_sent.load(std::memory_order_acquire) >= 1;
        });
        if (!first_bundle_seen) {
            std::cerr << "P51_SENDER_W30_FIRST_BUNDLE_TIMEOUT profile="
                      << profile_name << " connector=" << connector_calls.load()
                      << " sent=" << bundles_sent.load()
                      << " committed=" << committed.load() << "\n";
            if (results.front().wait_for(std::chrono::milliseconds(0)) ==
                std::future_status::ready) {
                const auto first = results.front().get();
                std::cerr << "P51_SENDER_W30_FIRST_RESULT status="
                          << static_cast<unsigned>(first.status)
                          << " terminal="
                          << (first.terminal_error
                                  ? first.terminal_error->detail : "none")
                          << "\n";
            }
            if (server_future.wait_for(std::chrono::milliseconds(0)) ==
                std::future_status::ready) {
                const auto server = server_future.get();
                std::cerr << "P51_SENDER_W30_SERVER_RESULT status="
                          << static_cast<unsigned>(server.status)
                          << " terminal="
                          << (server.terminal_error
                                  ? server.terminal_error->detail : "none")
                          << "\n";
            }
            CHECK(first_bundle_seen);
        }
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
        const auto progress_deadline = deadline.as_steady_time_point();
        const auto reached_window = [&] {
            return bundles_sent.load(std::memory_order_acquire) >= kWindow &&
                   committed.load(std::memory_order_acquire) >= kWindow;
        };
        while (!reached_window() &&
               std::chrono::steady_clock::now() < progress_deadline)
            progress_cv.wait_until(lock, std::min(
                progress_deadline,
                std::chrono::steady_clock::now() + std::chrono::seconds(1)));
        CHECK(reached_window());
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
    std::promise<void> retirement_posted;
    auto retirement_done = retirement_posted.get_future();
    asio::post(c_context, [&sender, &retirement_posted] {
        sender->retire_for_replacement();
        retirement_posted.set_value();
    });
    CHECK(retirement_done.wait_for(std::chrono::seconds(5)) ==
          std::future_status::ready);
    CHECK(server_future.wait_for(std::chrono::seconds(5)) ==
          std::future_status::ready);
    c_work.reset();
    c_context.stop();
    c_thread.join();
    const ServerRunResult server_result = server_future.get();
    CHECK(server_result.status == ServerRunStatus::Disconnected);
    f_context.stop();
    f_thread.join();
    std::cerr << "P51_SENDER_W30_PROFILE=" << profile_name << " PASS\n";
    }
}

void run_p51_sender_recovery_case(bool repeat_interrupted_materialization,
                                  bool retire_during_recovery = false,
                                  bool expire_during_recovery = false) {
    const auto [c_guid, f_guid] = sender_r2_store_guids();
    const Id128 relationship_id = Id128::from_u64(0x5102);
    P51SourceArmFields source_arm = sender_r2_arm(
        c_guid, 901, 1901, 2, ProfileId::ZSTD_TU);
    P51SourceArmedFields armed = sender_r2_armed(
        std::move(source_arm), f_guid, 0x529001, 2);
    std::vector<uint8_t> source(4096);
    uint32_t random = 0x9e3779b9;
    for (uint8_t& byte : source) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        byte = static_cast<uint8_t>(random);
    }
    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto deadline = sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
        std::chrono::steady_clock::now() +
            (expire_during_recovery ? std::chrono::seconds(3)
                                    : std::chrono::seconds(25)),
        clock.clock_domain_id, clock.time_namespace_id);

    std::mutex commit_mutex;
    std::condition_variable commit_cv;
    std::optional<R2TxCommit> retained_commit;
    std::atomic<unsigned> materialized{0};
    std::atomic<unsigned> consumed{0};
    std::atomic<unsigned> resets{0};
    std::atomic<unsigned> input_selectors{0};
    std::mutex recovery_gate_mutex;
    std::condition_variable recovery_gate_cv;
    bool recovery_waiting = false;
    bool release_recovery = false;
    std::optional<ResetRequest> retained_reset;
    P50ServerEndpointConfig server_config;
    EndpointCaps server_caps;
    server_caps.profile = ProfileId::ZSTD_TU;
    server_caps.supported_profiles = profile_bit(ProfileId::ZSTD_TU);
    server_caps.zstd.max_raw_bytes = 1U << 20;
    server_caps.zstd.max_encoded_body_bytes = 1U << 20;
    server_config.input_job_state = [&](CStoreGuid, const TxBegin&,
                                       const TxCommit&,
                                       std::span<const uint8_t>) {
        ++input_selectors;
        return InputJobState::Open;
    };
    server_config.lookup_p51_link_reservation =
        [&, deadline](const LinkHello& hello)
            -> std::optional<P51SourceLinkLease> {
        if (hello.profile != ProfileId::ZSTD_TU || hello.window != 2 ||
            hello.relationship_id != relationship_id ||
            hello.reservation_id != Id128{armed.reservation_id} ||
            hello.c_store_guid != c_guid || hello.f_store_guid != f_guid ||
            (hello.relationship_epoch != armed.relationship_epoch &&
             (!retained_reset || hello.relationship_epoch !=
                                     retained_reset->new_relationship_epoch)))
            return std::nullopt;
        P51SourceLinkLease lease;
        lease.initial_armed = armed;
        lease.absolute_deadline = deadline;
        lease.reconnect = hello.start_mode == LinkStartMode::Reconnect;
        lease.relationship_epoch = retained_reset
            ? retained_reset->new_relationship_epoch
            : hello.relationship_epoch;
        lease.history_nonce = retained_reset
            ? retained_reset->new_history_nonce
            : hello.history_nonce;
        if (lease.reconnect) {
            lease.committed_prefix_k = repeat_interrupted_materialization ? 0 : 1;
            lease.acknowledged_prefix_q = 0;
        }
        return lease;
    };
    server_config.consume_p51_job_reservation =
        [&, deadline](const LinkHello& hello, const JobBind& binding)
            -> std::optional<P51SourceJobLease> {
        if ((hello.start_mode != LinkStartMode::Initial &&
             hello.start_mode != LinkStartMode::Reconnect) ||
            binding.reservation_id != Id128{armed.reservation_id} ||
            binding.relationship_ordinal != 1 || binding.tu_seq.value != 0 ||
            binding.wire_job_id != armed.arm.source.wire_job_id ||
            binding.raw_bytes != source.size() ||
            binding.raw_digest != icecc::digest128(source))
            return std::nullopt;
        ++consumed;
        P51SourceJobLease lease;
        lease.armed = armed;
        lease.armed.relationship_epoch = hello.relationship_epoch;
        lease.absolute_deadline = deadline;
        lease.binding = binding;
        lease.binding_digest = compute_r2_binding_digest(binding);
        lease.input_key = InputRecordKey{c_guid, binding.tu_seq};
        return lease;
    };
    server_config.record_p51_job_commit =
        [&](const LinkHello& hello, const JobBind& binding,
            const R2TxCommit& commit) {
        if ((hello.start_mode != LinkStartMode::Initial &&
             hello.start_mode != LinkStartMode::Reconnect) ||
            binding.relationship_ordinal != 1 ||
            commit.relationship_ordinal != 1 ||
            commit.inner.tu_seq != binding.tu_seq)
            return false;
        {
            std::lock_guard lock(commit_mutex);
            retained_commit = commit;
        }
        ++materialized;
        commit_cv.notify_all();
        return true;
    };
    server_config.acknowledge_p51_receipt =
        [relationship_id](const LinkHello& hello, const CommitAck& ack) {
        return hello.relationship_id == relationship_id &&
               ack.relationship_id == relationship_id &&
               ack.relationship_epoch == hello.relationship_epoch &&
               ack.physical_link_generation ==
                   hello.physical_link_generation &&
               ack.contiguous_verified_ordinal == 1;
    };
    server_config.settle_p51_interrupted_job =
        [](const LinkHello& hello) {
        return hello.start_mode == LinkStartMode::Reconnect;
    };
    server_config.p51_source_reservation_terminal =
        [](const JobBind&) { return false; };
    server_config.recover_p51_receipts =
        [&](const LinkHello& hello, const RecoverBegin& begin,
            std::span<const RecoverWitness> witnesses, const RecoverEnd& end)
            -> std::optional<P51RecoveryReceiptInterval> {
        std::optional<R2TxCommit> commit;
        {
            std::lock_guard lock(commit_mutex);
            commit = retained_commit;
        }
        const bool committed_before_recovery =
            !repeat_interrupted_materialization;
        if (hello.start_mode != LinkStartMode::Reconnect ||
            begin.relationship_id != relationship_id || begin.witness_count != 1 ||
            begin.verified_floor_a != 0 || begin.prepared_prefix_p != 1 ||
            witnesses.size() != 1 || end.witness_count != 1 ||
            witnesses.front().relationship_ordinal != 1 ||
            witnesses.front().inner.tu_seq.value != 0 ||
            witnesses.front().inner.raw_digest != icecc::digest128(source) ||
            (committed_before_recovery && !commit) ||
            (!committed_before_recovery && commit.has_value()))
            return std::nullopt;
        if (retire_during_recovery) {
            std::unique_lock lock(recovery_gate_mutex);
            recovery_waiting = true;
            recovery_gate_cv.notify_all();
            recovery_gate_cv.wait(lock, [&] { return release_recovery; });
        }
        P51RecoveryReceiptInterval interval;
        const uint64_t committed_prefix = committed_before_recovery ? 1 : 0;
        if (commit) {
            if (commit->binding_digest != witnesses.front().binding_digest ||
                commit->transaction_digest != witnesses.front().transaction_digest ||
                commit->inner.history_nonce != witnesses.front().inner.history_nonce ||
                commit->inner.rel_seq != witnesses.front().inner.rel_seq ||
                commit->inner.tu_seq != witnesses.front().inner.tu_seq ||
                commit->inner.raw_digest != witnesses.front().inner.raw_digest ||
                commit->inner.transaction_digest !=
                    witnesses.front().inner.transaction_digest)
                return std::nullopt;
            interval.rows.push_back(ReceiptRow{
                begin.relationship_id, begin.relationship_epoch,
                begin.physical_link_generation, begin.operation_id, *commit});
        }
        interval.end = ReceiptsEnd{
            begin.relationship_id, begin.relationship_epoch,
            begin.physical_link_generation, begin.operation_id,
            0, committed_prefix, 0, static_cast<uint32_t>(interval.rows.size())};
        return interval;
    };
    server_config.validate_p51_reset =
        [&, c_guid](const LinkHello& hello, const ResetRequest& request)
            -> std::optional<ResetAck> {
        if (request.settled_prefix_k !=
                (repeat_interrupted_materialization ? 0 : 1) ||
            request.old_relationship_epoch != hello.relationship_epoch ||
            request.new_relationship_epoch !=
                request.old_relationship_epoch + 1)
            return std::nullopt;
        if (retained_reset) {
            const bool exact_replay =
                retained_reset->operation_id == request.operation_id &&
                retained_reset->old_relationship_epoch ==
                    request.old_relationship_epoch &&
                retained_reset->new_relationship_epoch ==
                    request.new_relationship_epoch &&
                retained_reset->new_history_nonce == request.new_history_nonce &&
                retained_reset->settled_prefix_k == request.settled_prefix_k;
            const bool next_reset =
                retained_reset->new_relationship_epoch ==
                    request.old_relationship_epoch;
            if (!exact_replay && !next_reset) return std::nullopt;
        } else if (request.old_relationship_epoch != hello.relationship_epoch) {
            return std::nullopt;
        }
        return ResetAck{request,
                        initial_route_digest(c_guid, request.new_history_nonce),
                        RelSeq{}};
    };
    server_config.commit_p51_reset =
        [&](const LinkHello&, const ResetRequest& request, const ResetAck&) {
        retained_reset = request;
        ++resets;
        return true;
    };
    server_config.confirm_p51_reset =
        [repeat_interrupted_materialization](const LinkHello& hello,
                                              const ResetConfirm& confirm) {
        return hello.start_mode == LinkStartMode::Reconnect &&
               confirm.relationship_id == Id128{hello.relationship_id} &&
               confirm.settled_prefix_k ==
                   (repeat_interrupted_materialization ? 0 : 1) &&
               confirm.new_relationship_epoch ==
                   hello.relationship_epoch + 1;
    };

    P50ServerEndpoint server(f_guid, server_caps, nullptr, nullptr,
                             std::move(server_config));
    asio::io_context f_context;
    tcp::acceptor acceptor(f_context, {asio::ip::address_v4::loopback(), 0});
    auto server_future = asio::co_spawn(
        f_context,
        sender_r2_accept_recovery_with_lost_reset_ack(
            acceptor, server,
            expire_during_recovery ? 1 : retire_during_recovery ? 2 :
                (repeat_interrupted_materialization ? 4 : 3),
            repeat_interrupted_materialization ? 2 : 0,
            expire_during_recovery),
        asio::use_future);
    std::thread f_thread([&] { f_context.run(); });
    struct RecoveryThreadCleanup {
        asio::io_context& c_context;
        asio::io_context& f_context;
        std::thread& c_thread;
        std::thread& f_thread;
        ~RecoveryThreadCleanup() {
            c_context.stop();
            f_context.stop();
            if (c_thread.joinable()) c_thread.join();
            if (f_thread.joinable()) f_thread.join();
        }
    };

    PreparationAuthorityLimits limits;
    limits.max_speculative_tus = 2;
    limits.max_speculative_raw_bytes = 1U << 20;
    limits.max_live_entries = 8;
    EndpointCaps caps;
    caps.profile = ProfileId::ZSTD_TU;
    caps.supported_profiles = profile_bit(ProfileId::ZSTD_TU);
    caps.zstd.max_raw_bytes = 1U << 20;
    caps.zstd.max_encoded_body_bytes = 1U << 20;
    auto authority = std::make_shared<P50PreparationAuthority>(
        c_guid, caps.zstd, limits, 1, ProfileId::ZSTD_TU);
    const PreparationRouteKey route{f_guid, 23, ProfileId::ZSTD_TU};
    ZstdSourceTransferConfig sender_config = config();
    sender_config.deadline = deadline.as_steady_time_point();
    sender_config.endpoint_caps = caps;
    sender_config.authority_limits = limits;
    sender_config.disconnect_r2_after_bundle_for_test =
        [&](uint64_t ordinal) {
        if (repeat_interrupted_materialization) return false;
        if (ordinal != 1) return false;
        std::unique_lock lock(commit_mutex);
        const bool seen = commit_cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return retained_commit.has_value();
        });
        CHECK(seen);
        return true;
    };
    auto sender = std::make_shared<P50ZstdSourceSender>(
        authority, route, PrepareRequestKey{3, 901}, sender_config);

    asio::io_context c_context;
    auto c_work = asio::make_work_guard(c_context);
    const tcp::endpoint remote = acceptor.local_endpoint();
    std::atomic<unsigned> connector_calls{0};
    std::thread deadline_connector_thread;
    struct DeadlineConnectorCleanup {
        std::thread& connector_thread;
        ~DeadlineConnectorCleanup() {
            if (connector_thread.joinable()) connector_thread.join();
        }
    } connector_cleanup{deadline_connector_thread};
    std::mutex deadline_connector_mutex;
    std::condition_variable deadline_connector_cv;
    bool deadline_connector_waiting = false;
    AsyncConnectedFdFactory connector = [&](auto deadline_at, auto completion) {
        const unsigned call = connector_calls.fetch_add(1,
            std::memory_order_relaxed) + 1;
        if (expire_during_recovery && call == 2) {
            {
                std::lock_guard lock(deadline_connector_mutex);
                deadline_connector_waiting = true;
            }
            deadline_connector_cv.notify_all();
            deadline_connector_thread = std::thread(
                [deadline_at, completion = std::move(completion)]() mutable {
                    std::this_thread::sleep_until(deadline_at);
                    completion(-1);
                });
            return;
        }
        if (std::chrono::steady_clock::now() >= deadline_at || call == 2) {
            completion(-1);  // Failed connector cannot consume retained suffix.
            return;
        }
        completion(connect_fd(remote));
    };
    std::thread c_thread([&] { c_context.run(); });
    RecoveryThreadCleanup thread_cleanup{
        c_context, f_context, c_thread, f_thread};
    const auto source_deadline = sender_config.deadline;
    const auto invoke = [&] {
        return asio::co_spawn(c_context,
            sender->transfer_p51_route(armed, 41, connector,
                PrepareRequestKey{3, 901}, source_deadline, source),
            asio::use_future);
    };
    std::future<ZstdSourceTransferResult> transfer = invoke();
    if (expire_during_recovery) {
        {
            std::unique_lock lock(deadline_connector_mutex);
            CHECK(deadline_connector_cv.wait_for(
                lock, std::chrono::seconds(5), [&] {
                    return deadline_connector_waiting;
                }));
        }
        CHECK(transfer.wait_for(std::chrono::seconds(8)) ==
              std::future_status::ready);
        const auto expired = transfer.get();
        if (deadline_connector_thread.joinable())
            deadline_connector_thread.join();
        CHECK(expired.status == ZstdSourceTransferStatus::DeadlineExceeded);
        CHECK(std::chrono::steady_clock::now() >= deadline.as_steady_time_point());
        CHECK(std::chrono::steady_clock::now() - deadline.as_steady_time_point() <
              std::chrono::seconds(2));
        CHECK(connector_calls.load() == 2);
        CHECK(resets.load() == 0);
        CHECK(consumed.load() == 1);
        CHECK(materialized.load() == 1);
        CHECK(input_selectors.load() == 1);
        {
            std::lock_guard lock(commit_mutex);
            CHECK(retained_commit.has_value());
            CHECK(retained_commit->inner.raw_digest ==
                  icecc::digest128(source));
        }
        std::promise<void> retirement_posted;
        auto retirement_done = retirement_posted.get_future();
        asio::post(c_context, [&sender, &retirement_posted] {
            sender->retire_for_replacement();
            retirement_posted.set_value();
        });
        CHECK(retirement_done.wait_for(std::chrono::seconds(5)) ==
              std::future_status::ready);
        CHECK(server_future.wait_for(std::chrono::seconds(5)) ==
              std::future_status::ready);
        c_work.reset();
        c_context.stop();
        c_thread.join();
        f_context.stop();
        f_thread.join();
        const auto server_runs = server_future.get();
        CHECK(server_runs.size() == 1);
        CHECK(server_runs[0].status == ServerRunStatus::Disconnected);
        std::cerr << "P51_SENDER_RECOVERY_DEADLINE expiry_during_connector PASS\n";
        return;
    }
    if (retire_during_recovery) {
        {
            std::unique_lock lock(recovery_gate_mutex);
            CHECK(recovery_gate_cv.wait_for(lock, std::chrono::seconds(10), [&] {
                return recovery_waiting;
            }));
        }
        std::promise<void> retirement_posted;
        auto retirement_done = retirement_posted.get_future();
        asio::post(c_context, [&sender, &retirement_posted] {
            sender->retire_for_replacement();
            retirement_posted.set_value();
        });
        CHECK(retirement_done.wait_for(std::chrono::seconds(5)) ==
              std::future_status::ready);
        {
            std::lock_guard lock(recovery_gate_mutex);
            release_recovery = true;
        }
        recovery_gate_cv.notify_all();
    }
    std::cerr << "P51_RECOVERY_TRANSFER_WAIT repeated_abort="
              << repeat_interrupted_materialization
              << " retire=" << retire_during_recovery << "\n";
    const ZstdSourceTransferResult recovered = transfer.get();
    std::cerr << "P51_RECOVERY_TRANSFER_DONE status="
              << static_cast<unsigned>(recovered.status)
              << " connectors=" << connector_calls.load()
              << " repeated_abort=" << repeat_interrupted_materialization
              << " retire=" << retire_during_recovery << "\n";
    if (retire_during_recovery) {
        CHECK(recovered.status == ZstdSourceTransferStatus::Unavailable);
        CHECK(connector_calls.load() == 3);
        CHECK(resets.load() <= 1);
        CHECK(server_future.wait_for(std::chrono::seconds(5)) ==
              std::future_status::ready);
        c_work.reset();
        c_context.stop();
        c_thread.join();
        f_context.stop();
        f_thread.join();
        const auto server_runs = server_future.get();
        CHECK(server_runs.size() == 2);
        CHECK(server_runs[0].status == ServerRunStatus::Disconnected);
        CHECK(server_runs[1].status == ServerRunStatus::Disconnected);
        std::cerr << "P51_SENDER_RETIRE_DURING_RECOVERY PASS\n";
        return;
    }
    if (recovered.status != ZstdSourceTransferStatus::Committed)
        throw std::runtime_error(std::string("recovery status=") +
            std::to_string(static_cast<unsigned>(recovered.status)) +
            ", repeated_abort=" +
                (repeat_interrupted_materialization ? "true" : "false") +
            ", connectors=" + std::to_string(connector_calls.load()) +
            ", consumed=" + std::to_string(consumed.load()) +
            ", committed=" + std::to_string(materialized.load()) +
            ", selectors=" + std::to_string(input_selectors.load()) +
            ", resets=" + std::to_string(resets.load()));
    CHECK(recovered.raw_digest == icecc::digest128(source));
    CHECK((recovered.committed_input == InputRecordKey{c_guid, TuSeq{0}}));
    const unsigned expected_connector_calls =
        repeat_interrupted_materialization ? 5U : 4U;
    if (connector_calls.load() != expected_connector_calls)
        throw std::runtime_error("recovery connector calls=" +
                                 std::to_string(connector_calls.load()));
    CHECK(materialized.load() == 1);
    CHECK(consumed.load() ==
          (repeat_interrupted_materialization ? 3U : 1U));
    CHECK(resets.load() == (repeat_interrupted_materialization ? 2U : 1U));

    std::promise<void> retirement_posted;
    auto retirement_done = retirement_posted.get_future();
    asio::post(c_context, [&sender, &retirement_posted] {
        sender->retire_for_replacement();
        retirement_posted.set_value();
    });
    CHECK(retirement_done.wait_for(std::chrono::seconds(5)) ==
          std::future_status::ready);
    std::cerr << "P51_RECOVERY_SERVER_WAIT repeated_abort="
              << repeat_interrupted_materialization << "\n";
    CHECK(server_future.wait_for(std::chrono::seconds(5)) ==
          std::future_status::ready);
    c_work.reset();
    c_context.stop();
    c_thread.join();
    f_context.stop();
    f_thread.join();
    const auto server_runs = server_future.get();
    CHECK(server_runs.back().status == ServerRunStatus::Disconnected);
    std::cerr << "P51_SENDER_RECOVERY repeated_abort="
              << (repeat_interrupted_materialization ? "true" : "false")
              << " PASS\n";
}

void test_p51_sender_recovers_lost_commit_reply_after_connector_failure() {
    run_p51_sender_recovery_case(false);
    run_p51_sender_recovery_case(true);
}

void run_p51_sender_shared_failure_case(size_t kJobs, ProfileId profile,
                                        bool repeat_recovery_loss = false,
                                        bool retire_after_positive_receipt = false,
                                        bool expire_after_positive_receipt = false,
                                        bool reject_stale_reconnect = false,
                                        bool retire_during_retry_wait = false,
                                        bool close_reconnect_after_hello = false,
                                        bool reject_after_positive_receipt = false) {
    CHECK(kJobs >= 1 && kJobs <= 30);
    const uint32_t kWindow = static_cast<uint32_t>(kJobs);
    const char* const profile_name = profile == ProfileId::P29V1 ? "P29V1"
                                    : profile == ProfileId::ZSTD_ROUTE ? "ZSTD_ROUTE"
                                    : "ZSTD_TU";
    const auto [c_guid, f_guid] = sender_r2_store_guids();
    const Id128 relationship_id = Id128::from_u64(0x5102);
    std::vector<P51SourceArmedFields> armed(kJobs);
    std::vector<std::vector<uint8_t>> input(kJobs);
    for (size_t index = 0; index != kJobs; ++index) {
        auto arm = sender_r2_arm(c_guid, 921 + index,
                                 static_cast<uint32_t>(1921 + index),
                                 kWindow, profile);
        armed[index] = sender_r2_armed(std::move(arm), f_guid,
                                       0x529100 + index, kWindow);
        const std::string file = "/tmp/p51-shared-" +
            std::string(profile_name) + "-" + std::to_string(index) + ".cc";
        const std::string text = "# 1 \"" + file + "\"\n" +
            "int p51_value_" + std::to_string(index) + " = " +
            std::to_string(index + 17) + ";\n" +
            "extern int shared_preprocessed_line;\n" +
            "extern int shared_preprocessed_line;\n" +
            "extern int shared_preprocessed_line;\n" +
            "extern int shared_preprocessed_line;\n" +
            "int p51_tail_" + std::to_string(index) + ";\n";
        input[index].assign(text.begin(), text.end());
    }
    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto deadline = sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
        std::chrono::steady_clock::now() +
            (reject_after_positive_receipt
                 ? std::chrono::seconds(8)
             : reject_stale_reconnect || close_reconnect_after_hello
                 ? (retire_during_retry_wait ? std::chrono::seconds(5)
                                             : std::chrono::milliseconds(1400))
             : expire_after_positive_receipt ? std::chrono::seconds(3)
                                           : std::chrono::seconds(30)),
        clock.clock_domain_id, clock.time_namespace_id);

    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    std::mutex ack_mutex;
    std::condition_variable ack_cv;
    std::mutex receipt_hook_mutex;
    std::condition_variable receipt_hook_cv;
    std::atomic<bool> receipt_hook_entered{false};
    std::atomic<bool> receipt_hook_before_deadline{false};
    std::atomic<unsigned> bundles_sent{0};
    std::atomic<unsigned> connector_calls{0};
    std::atomic<bool> stop_reconnect_listener{false};
    std::atomic<unsigned> rejected_reconnects{0};
    std::atomic<bool> retry_wait_registered{false};
    std::atomic<int64_t> retry_wait_registered_ns{0};
    std::atomic<unsigned> retry_wait_count{0};
    std::atomic<int64_t> retry_wait_remaining_ns{0};
    std::atomic<unsigned> input_selections{0};
    std::atomic<unsigned> input_mismatches{0};
    std::atomic<unsigned> materialized{0};
    std::atomic<unsigned> acknowledged{0};
    std::vector<std::atomic<unsigned>> binds(kJobs);
    std::vector<std::atomic<unsigned>> commits(kJobs);
    for (size_t index = 0; index != kJobs; ++index) {
        binds[index].store(0, std::memory_order_relaxed);
        commits[index].store(0, std::memory_order_relaxed);
    }
    std::mutex commit_mutex;
    std::vector<std::optional<R2TxCommit>> retained_commits(kJobs);
    std::optional<ResetRequest> retained_reset;
    std::optional<HistoryNonce> initial_history_nonce;

    P50ServerEndpointConfig server_config;
    EndpointCaps server_caps;
    server_caps.profile = profile;
    server_caps.supported_profiles = profile_bit(profile);
    server_caps.zstd.max_raw_bytes = 1U << 20;
    server_caps.zstd.max_encoded_body_bytes = 1U << 20;
    server_config.input_job_state = [&](CStoreGuid, const TxBegin& begin,
                                        const TxCommit& commit,
                                        std::span<const uint8_t> bytes) {
        const size_t index = static_cast<size_t>(begin.tu_seq.value);
        if (index >= kJobs || begin.profile != profile ||
            (index < kJobs && commit.raw_digest != icecc::digest128(input[index])) ||
            (index < kJobs && bytes.size() != input[index].size()) ||
            (index < kJobs &&
             !std::equal(bytes.begin(), bytes.end(), input[index].begin()))) {
            input_mismatches.fetch_add(1, std::memory_order_relaxed);
        } else if (index == 0) {
            if (!initial_history_nonce || begin.history_nonce != *initial_history_nonce)
                input_mismatches.fetch_add(1, std::memory_order_relaxed);
        } else if (retained_reset &&
                   begin.history_nonce != retained_reset->new_history_nonce) {
            // Replayed suffix TUs must be rebuilt under confirmed fresh
            // history while preserving the original input identity.
            input_mismatches.fetch_add(1, std::memory_order_relaxed);
        }
        input_selections.fetch_add(1, std::memory_order_relaxed);
        return InputJobState::Open;
    };
    server_config.lookup_p51_link_reservation =
        [&, deadline](const LinkHello& hello)
            -> std::optional<P51SourceLinkLease> {
        if (hello.profile != profile || hello.window != kWindow ||
            hello.relationship_id != relationship_id ||
            hello.c_store_guid != c_guid || hello.f_store_guid != f_guid ||
            hello.reservation_id != Id128{armed[0].reservation_id})
            return std::nullopt;
        if (hello.start_mode == LinkStartMode::Initial) {
            if (hello.relationship_epoch != armed[0].relationship_epoch ||
                initial_history_nonce)
                return std::nullopt;
            initial_history_nonce = hello.history_nonce;
        } else if (hello.start_mode == LinkStartMode::Reconnect) {
            if (!initial_history_nonce ||
                hello.relationship_epoch != armed[0].relationship_epoch ||
                hello.history_nonce != *initial_history_nonce)
                return std::nullopt;
        } else {
            return std::nullopt;
        }
        P51SourceLinkLease lease;
        lease.initial_armed = armed[0];
        lease.absolute_deadline = deadline;
        lease.reconnect = hello.start_mode == LinkStartMode::Reconnect;
        // A lost RESET_ACK leaves the next physical HELLO at the original
        // epoch/nonce, while F's active codec history already reflects RESET.
        // Return that authoritative active state for the exact replay.
        lease.relationship_epoch = retained_reset
            ? retained_reset->new_relationship_epoch
            : hello.relationship_epoch;
        lease.initial_armed.relationship_epoch = lease.relationship_epoch;
        lease.history_nonce = retained_reset
            ? retained_reset->new_history_nonce
            : hello.history_nonce;
        if (lease.reconnect) {
            lease.committed_prefix_k = 1;
            lease.acknowledged_prefix_q = 0;
        }
        return lease;
    };
    server_config.consume_p51_job_reservation =
        [&, deadline](const LinkHello& hello, const JobBind& binding)
            -> std::optional<P51SourceJobLease> {
        if (binding.relationship_ordinal == 0 ||
            binding.relationship_ordinal > kJobs ||
            binding.physical_link_generation != hello.physical_link_generation ||
            binding.profile != profile)
            return std::nullopt;
        const size_t index = static_cast<size_t>(binding.relationship_ordinal - 1);
        if (binding.reservation_id != Id128{armed[index].reservation_id} ||
            binding.wire_job_id != armed[index].arm.source.wire_job_id ||
            binding.source_request_id != armed[index].arm.source.source_request_id ||
            binding.assignment_nonce != armed[index].arm.source.assignment_nonce ||
            binding.assignment_epoch != armed[index].arm.source.assignment_epoch ||
            binding.logical_job != armed[index].arm.source.logical_job ||
            binding.compiler_attempt != armed[index].arm.source.compiler_attempt ||
            binding.tu_seq.value != index ||
            binding.raw_bytes != input[index].size() ||
            binding.raw_digest != icecc::digest128(input[index]))
            return std::nullopt;
        if (hello.start_mode == LinkStartMode::Initial) {
            if (hello.relationship_epoch != armed[index].relationship_epoch)
                return std::nullopt;
        } else if (!retained_reset ||
                   hello.relationship_epoch !=
                       retained_reset->new_relationship_epoch ||
                   hello.history_nonce != retained_reset->new_history_nonce) {
            return std::nullopt;
        }
        const unsigned prior = binds[index].fetch_add(1, std::memory_order_relaxed);
        if (prior != 0 &&
            (hello.start_mode != LinkStartMode::Reconnect || index != 1))
            return std::nullopt;
        P51SourceJobLease lease;
        lease.armed = armed[index];
        lease.armed.relationship_epoch = hello.relationship_epoch;
        lease.absolute_deadline = deadline;
        lease.binding = binding;
        lease.binding_digest = compute_r2_binding_digest(binding);
        lease.input_key = InputRecordKey{c_guid, binding.tu_seq};
        return lease;
    };
    server_config.record_p51_job_commit =
        [&](const LinkHello& hello, const JobBind& binding,
            const R2TxCommit& commit) {
        if (hello.relationship_id != relationship_id ||
            binding.relationship_ordinal == 0 ||
            binding.relationship_ordinal > kJobs ||
            commit.relationship_ordinal != binding.relationship_ordinal ||
            commit.inner.raw_digest != binding.raw_digest ||
            commit.inner.tu_seq != binding.tu_seq)
            return false;
        const size_t index = static_cast<size_t>(binding.relationship_ordinal - 1);
        {
            std::lock_guard lock(commit_mutex);
            retained_commits[index] = commit;
        }
        commits[index].fetch_add(1, std::memory_order_relaxed);
        materialized.fetch_add(1, std::memory_order_release);
        return true;
    };
    server_config.acknowledge_p51_receipt =
        [&](const LinkHello& hello, const CommitAck& ack) {
        if (hello.relationship_id != relationship_id ||
            ack.relationship_id != relationship_id ||
            ack.relationship_epoch != hello.relationship_epoch ||
            ack.physical_link_generation != hello.physical_link_generation ||
            ack.contiguous_verified_ordinal > kJobs)
            return false;
        acknowledged.store(static_cast<unsigned>(ack.contiguous_verified_ordinal),
                           std::memory_order_release);
        ack_cv.notify_all();
        return true;
    };
    server_config.settle_p51_interrupted_job =
        [](const LinkHello& hello) {
        return hello.start_mode == LinkStartMode::Reconnect;
    };
    server_config.p51_source_reservation_terminal =
        [](const JobBind&) { return false; };
    server_config.recover_p51_receipts =
        [&](const LinkHello& hello, const RecoverBegin& begin,
            std::span<const RecoverWitness> witnesses, const RecoverEnd& end)
            -> std::optional<P51RecoveryReceiptInterval> {
        std::optional<R2TxCommit> commit;
        {
            std::lock_guard lock(commit_mutex);
            commit = retained_commits[0];
        }
        if (hello.start_mode != LinkStartMode::Reconnect ||
            !initial_history_nonce ||
            hello.history_nonce != *initial_history_nonce ||
            begin.relationship_id != relationship_id ||
            begin.verified_floor_a != 0 || begin.prepared_prefix_p != kJobs ||
            begin.witness_count != kJobs || witnesses.size() != kJobs ||
            end.witness_count != kJobs || !commit ||
            witnesses[0].relationship_ordinal != 1 ||
            witnesses[1].relationship_ordinal != 2 ||
            commit->binding_digest != witnesses[0].binding_digest ||
            commit->transaction_digest != witnesses[0].transaction_digest ||
            commit->inner.tu_seq != witnesses[0].inner.tu_seq ||
            commit->inner.raw_digest != witnesses[0].inner.raw_digest)
            return std::nullopt;
        for (size_t index = 0; index != kJobs; ++index) {
            if (witnesses[index].relationship_ordinal != index + 1 ||
                witnesses[index].inner.tu_seq.value != index ||
                witnesses[index].inner.history_nonce != *initial_history_nonce ||
                witnesses[index].inner.profile != profile ||
                witnesses[index].inner.raw_bytes != input[index].size() ||
                witnesses[index].inner.raw_digest != icecc::digest128(input[index]))
                return std::nullopt;
        }
        P51RecoveryReceiptInterval interval;
        interval.rows.push_back(ReceiptRow{
            begin.relationship_id, begin.relationship_epoch,
            begin.physical_link_generation, begin.operation_id, *commit});
        interval.end = ReceiptsEnd{
            begin.relationship_id, begin.relationship_epoch,
            begin.physical_link_generation, begin.operation_id,
            0, 1, 0, 1};
        return interval;
    };
    server_config.validate_p51_reset =
        [&](const LinkHello& hello, const ResetRequest& request)
            -> std::optional<ResetAck> {
        if (request.settled_prefix_k != 1 ||
            request.old_relationship_epoch != hello.relationship_epoch ||
            request.new_relationship_epoch != request.old_relationship_epoch + 1 ||
            !initial_history_nonce ||
            request.old_history_nonce != *initial_history_nonce ||
            request.new_history_nonce == request.old_history_nonce)
            return std::nullopt;
        if (retained_reset &&
            (retained_reset->operation_id != request.operation_id ||
             retained_reset->old_relationship_epoch !=
                 request.old_relationship_epoch ||
             retained_reset->new_relationship_epoch !=
                 request.new_relationship_epoch ||
             retained_reset->new_history_nonce != request.new_history_nonce ||
             retained_reset->settled_prefix_k != request.settled_prefix_k))
            return std::nullopt;
        return ResetAck{request,
                        initial_route_digest(c_guid, request.new_history_nonce),
                        RelSeq{}};
    };
    server_config.commit_p51_reset =
        [&](const LinkHello&, const ResetRequest& request, const ResetAck&) {
        retained_reset = request;
        return true;
    };
    server_config.confirm_p51_reset =
        [](const LinkHello& hello, const ResetConfirm& confirm) {
        return hello.start_mode == LinkStartMode::Reconnect &&
               confirm.relationship_id == Id128{hello.relationship_id} &&
               confirm.settled_prefix_k == 1 &&
               confirm.new_relationship_epoch == hello.relationship_epoch + 1;
    };

    P50ServerEndpoint server(f_guid, server_caps, nullptr, nullptr,
                             std::move(server_config));
    std::shared_ptr<P50ZstdSourceSender> sender;
    asio::io_context f_context;
    tcp::acceptor acceptor(f_context, {asio::ip::address_v4::loopback(), 0});
    asio::io_context c_context;
    std::thread c_thread;
    std::thread f_thread;
    struct ThreadCleanup {
        asio::io_context& c_context;
        asio::io_context& f_context;
        std::thread& c_thread;
        std::thread& f_thread;
        ~ThreadCleanup() {
            c_context.stop();
            f_context.stop();
            if (c_thread.joinable()) c_thread.join();
            if (f_thread.joinable()) f_thread.join();
        }
    } cleanup{c_context, f_context, c_thread, f_thread};
    auto server_future = asio::co_spawn(
        f_context, sender_r2_accept_shared_failure(
            acceptor, server, server_caps, bundles_sent, kJobs,
            gate_mutex, gate_cv,
            repeat_recovery_loss, retire_after_positive_receipt,
            expire_after_positive_receipt, reject_stale_reconnect,
            &stop_reconnect_listener, &rejected_reconnects,
            close_reconnect_after_hello, reject_after_positive_receipt),
        asio::use_future);
    f_thread = std::thread([&] { f_context.run(); });

    PreparationAuthorityLimits limits;
    limits.max_speculative_tus = kWindow;
    limits.max_speculative_raw_bytes = 1U << 20;
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
    sender_config.deadline = deadline.as_steady_time_point();
    sender_config.endpoint_caps = caps;
    sender_config.authority_limits = limits;
    if (profile == ProfileId::ZSTD_ROUTE)
        sender_config.compression_level = 3;
    sender_config.after_r2_bundle_sent_for_test = [&](uint64_t) {
        bundles_sent.fetch_add(1, std::memory_order_release);
        gate_cv.notify_all();
    };
    if (retire_after_positive_receipt) {
        sender_config.after_r2_receipt_validated_for_test =
            [&sender](uint64_t ordinal) {
                if (ordinal == 1)
                    sender->retire_for_replacement();
            };
    } else if (expire_after_positive_receipt) {
        sender_config.after_r2_receipt_validated_for_test =
            [&, deadline](uint64_t ordinal) {
                if (ordinal != 1) return;
                receipt_hook_before_deadline.store(
                    std::chrono::steady_clock::now() <
                        deadline.as_steady_time_point(),
                    std::memory_order_release);
                receipt_hook_entered.store(true, std::memory_order_release);
                receipt_hook_cv.notify_all();
                std::unique_lock lock(receipt_hook_mutex);
                while (std::chrono::steady_clock::now() <
                       deadline.as_steady_time_point())
                    receipt_hook_cv.wait_until(
                        lock, deadline.as_steady_time_point());
            };
    } else if (retire_during_retry_wait) {
        sender_config.after_r2_recovery_waiter_registered_for_test =
            [&](std::chrono::steady_clock::duration retry_delay) {
            retry_wait_count.fetch_add(1, std::memory_order_acq_rel);
            // Retire only when the actual registered shared wait has reached
            // the 500ms cap, proving this interrupts a long retry timer.
            if (retry_delay < std::chrono::milliseconds(450))
                return;
            retry_wait_remaining_ns.store(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    retry_delay).count(), std::memory_order_release);
            retry_wait_registered_ns.store(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count(),
                std::memory_order_release);
            retry_wait_registered.store(true, std::memory_order_release);
            sender->retire_for_replacement();
        };
    }
    sender = std::make_shared<P50ZstdSourceSender>(
        authority, route, PrepareRequestKey{3, 921}, sender_config);

    const tcp::endpoint remote = acceptor.local_endpoint();
    AsyncConnectedFdFactory connector = [&](auto, auto completion) {
        connector_calls.fetch_add(1, std::memory_order_relaxed);
        completion(connect_fd(remote));
    };
    std::vector<std::future<ZstdSourceTransferResult>> results(kJobs);
    for (size_t index = 0; index != kJobs; ++index) {
        results[index] = asio::co_spawn(c_context,
            sender->transfer_p51_route(
                armed[index], 41, connector,
                PrepareRequestKey{3, 921 + index}, deadline.as_steady_time_point(),
                input[index]), asio::use_future);
    }
    auto c_work = asio::make_work_guard(c_context);
    const auto transfer_started = std::chrono::steady_clock::now();
    c_thread = std::thread([&] { c_context.run(); });

    std::vector<ZstdSourceTransferResult> outcomes(kJobs);
    for (size_t index = 0; index != kJobs; ++index) outcomes[index] = results[index].get();
    if (reject_after_positive_receipt) {
        CHECK(kJobs == 2);
        CHECK(outcomes[0].status == ZstdSourceTransferStatus::Committed);
        CHECK(outcomes[0].committed_input.has_value());
        CHECK((outcomes[0].committed_input ==
               InputRecordKey{c_guid, TuSeq{0}}));
        CHECK(outcomes[0].raw_digest == icecc::digest128(input[0]));
        CHECK(outcomes[1].status == ZstdSourceTransferStatus::TerminalError);
        CHECK(outcomes[1].route_local_failure);
        CHECK(!outcomes[1].replacement_required);
        CHECK(outcomes[1].r2_link_rejection.has_value());
        CHECK(outcomes[1].r2_link_rejection->reason ==
              LinkRejectReason::ReservationMissing);
        CHECK(server_future.wait_for(std::chrono::seconds(5)) ==
              std::future_status::ready);
        const auto server_runs = server_future.get();
        CHECK(server_runs.size() == 1);
        CHECK(server_runs[0].status == ServerRunStatus::Disconnected);
        CHECK(rejected_reconnects.load(std::memory_order_acquire) == 1);
        // The validated first positive remains in the exact-result cache after
        // the later route-local rejection fences this link.
        const ZstdSourceTransferResult replay = asio::co_spawn(c_context,
            sender->transfer_p51_route(
                armed[0], 41, connector, PrepareRequestKey{3, 921},
                deadline.as_steady_time_point(), input[0]),
            asio::use_future).get();
        CHECK(replay.status == ZstdSourceTransferStatus::Committed);
        CHECK(replay.committed_input == outcomes[0].committed_input);
        CHECK(replay.raw_digest == outcomes[0].raw_digest);
        CHECK(connector_calls.load(std::memory_order_acquire) == 2);
        c_work.reset();
        c_context.stop();
        f_context.stop();
        c_thread.join();
        f_thread.join();
        std::cerr << "P51_SENDER_POSITIVE_SURVIVES_LATER_TYPED_REJECTION PASS\n";
        return;
    }
    if (retire_during_retry_wait) {
        CHECK(kJobs == 1);
        CHECK(retry_wait_registered.load(std::memory_order_acquire));
        CHECK(retry_wait_count.load(std::memory_order_acquire) > 0);
        CHECK(outcomes[0].status == ZstdSourceTransferStatus::Unavailable);
        CHECK(outcomes[0].route_local_failure);
        CHECK(!outcomes[0].committed_input);
        CHECK(bundles_sent.load(std::memory_order_acquire) == 1);
        CHECK(connector_calls.load(std::memory_order_acquire) ==
              rejected_reconnects.load(std::memory_order_acquire) + 1);
        CHECK(rejected_reconnects.load(std::memory_order_acquire) > 0);
        const auto registered = std::chrono::steady_clock::time_point(
            std::chrono::nanoseconds(retry_wait_registered_ns.load(
                std::memory_order_acquire)));
        const auto retirement_wake_elapsed =
            std::chrono::steady_clock::now() - registered;
        CHECK(retry_wait_remaining_ns.load(std::memory_order_acquire) >=
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::milliseconds(450)).count());
        CHECK(retirement_wake_elapsed < std::chrono::milliseconds(200));
        stop_reconnect_listener.store(true, std::memory_order_release);
        CHECK(server_future.wait_for(std::chrono::seconds(5)) ==
              std::future_status::ready);
        const auto server_runs = server_future.get();
        CHECK(server_runs.size() == 1);
        CHECK(server_runs[0].status == ServerRunStatus::Disconnected);
        c_work.reset();
        c_context.stop();
        f_context.stop();
        c_thread.join();
        f_thread.join();
        std::cerr << "P51_SENDER_RETRY_WAIT_RETIRE jobs=1 connectors="
                  << connector_calls.load() << " wait_count="
                  << retry_wait_count.load() << " remaining_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::nanoseconds(retry_wait_remaining_ns.load(
                             std::memory_order_acquire))).count()
                  << " retire_wake_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         retirement_wake_elapsed).count()
                  << " PASS\n";
        return;
    }
    if (reject_stale_reconnect && !retire_during_retry_wait) {
        CHECK(kJobs == 1);
        CHECK(bundles_sent.load(std::memory_order_acquire) == 1);
        const auto& outcome = outcomes[0];
        CHECK(outcome.status == ZstdSourceTransferStatus::TerminalError);
        CHECK(outcome.route_local_failure);
        CHECK(!outcome.committed_input);
        CHECK(outcome.r2_link_rejection.has_value());
        CHECK(outcome.r2_link_rejection->reason ==
              LinkRejectReason::StoreReplaced);
        CHECK(outcome.r2_link_rejection->offered.relationship_id ==
              relationship_id);
        CHECK(outcome.r2_link_rejection->offered.physical_link_generation != 41);
        CHECK(connector_calls.load() == 2);
        CHECK(input_mismatches.load() == 0);
        stop_reconnect_listener.store(true, std::memory_order_release);
        CHECK(server_future.wait_for(std::chrono::seconds(5)) ==
              std::future_status::ready);
        const auto server_runs = server_future.get();
        CHECK(server_runs.size() == 1);
        CHECK(server_runs[0].status == ServerRunStatus::Disconnected);
        CHECK(rejected_reconnects.load(std::memory_order_acquire) == 1);
        std::promise<void> retirement_posted;
        auto retirement_done = retirement_posted.get_future();
        asio::post(c_context, [&sender, &retirement_posted] {
            sender->retire_for_replacement();
            retirement_posted.set_value();
        });
        CHECK(retirement_done.wait_for(std::chrono::seconds(5)) ==
              std::future_status::ready);
        c_work.reset();
        c_context.stop();
        f_context.stop();
        c_thread.join();
        f_thread.join();
        std::cerr << "P51_SENDER_TYPED_RECONNECT_REJECTION connectors=2 PASS\n";
        return;
    }
    if (close_reconnect_after_hello && !retire_during_retry_wait) {
        CHECK(bundles_sent.load(std::memory_order_acquire) == kJobs);
        for (const auto& outcome : outcomes) {
            CHECK(outcome.status == ZstdSourceTransferStatus::DeadlineExceeded);
            CHECK(outcome.route_local_failure);
            CHECK(!outcome.committed_input);
        }
        const unsigned reconnect_attempts = connector_calls.load();
        if (reconnect_attempts > 10U)
            throw std::runtime_error(
                "R2 shared reconnect gate exceeded aggregate attempt budget: " +
                std::to_string(reconnect_attempts));
        CHECK(input_mismatches.load() == 0);
        CHECK(rejected_reconnects.load(std::memory_order_acquire) > 0);
        stop_reconnect_listener.store(true, std::memory_order_release);
        CHECK(server_future.wait_for(std::chrono::seconds(5)) ==
              std::future_status::ready);
        const auto server_runs = server_future.get();
        CHECK(server_runs.size() == 1);
        CHECK(server_runs[0].status == ServerRunStatus::Disconnected);
        std::promise<void> retirement_posted;
        auto retirement_done = retirement_posted.get_future();
        asio::post(c_context, [&sender, &retirement_posted] {
            sender->retire_for_replacement();
            retirement_posted.set_value();
        });
        CHECK(retirement_done.wait_for(std::chrono::seconds(5)) ==
              std::future_status::ready);
        c_work.reset();
        c_context.stop();
        f_context.stop();
        c_thread.join();
        f_thread.join();
        std::cerr << "P51_SENDER_RECONNECT_BACKOFF jobs=" << kJobs
                  << " initial_bundles=" << bundles_sent.load()
                  << " aggregate_connects=" << reconnect_attempts
                  << " elapsed_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - transfer_started)
                         .count()
                  << " PASS\n";
        return;
    }
    for (size_t index = 0; index != kJobs; ++index) {
        if (outcomes[index].status != ZstdSourceTransferStatus::Committed)
            throw std::runtime_error(
                "shared failure result=" +
                std::to_string(static_cast<unsigned>(outcomes[index].status)) +
                " caller=" + std::to_string(index) +
                " sent=" + std::to_string(bundles_sent.load()) +
                " commits=" + std::to_string(materialized.load()) +
                " connectors=" + std::to_string(connector_calls.load()));
        CHECK(outcomes[index].committed_input.has_value());
        CHECK((outcomes[index].committed_input ==
               InputRecordKey{c_guid, TuSeq{index}}));
        CHECK(outcomes[index].raw_digest == icecc::digest128(input[index]));
    }
    if (expire_after_positive_receipt) {
        CHECK(kJobs == 1);
        CHECK(receipt_hook_entered.load(std::memory_order_acquire));
        CHECK(receipt_hook_before_deadline.load(std::memory_order_acquire));
        CHECK(std::chrono::steady_clock::now() >=
              deadline.as_steady_time_point());
        // A positive exact receipt wins even though the original caller's
        // deadline expires before it can acquire the ACK writer.
        CHECK(outcomes[0].status == ZstdSourceTransferStatus::Committed);
    }
    if (retire_after_positive_receipt) {
        auto duplicate = asio::co_spawn(c_context,
            sender->transfer_p51_route(
                armed[0], 41, connector, PrepareRequestKey{3, 921},
                deadline.as_steady_time_point(), input[0]),
            asio::use_future).get();
        CHECK(duplicate.status == ZstdSourceTransferStatus::Committed);
        CHECK(duplicate.raw_digest == icecc::digest128(input[0]));
        std::vector<uint8_t> conflicting = input[0];
        conflicting.push_back(0x7f);
        auto rejected = asio::co_spawn(c_context,
            sender->transfer_p51_route(
                armed[0], 41, connector, PrepareRequestKey{3, 921},
                deadline.as_steady_time_point(), std::move(conflicting)),
            asio::use_future).get();
        CHECK(rejected.status == ZstdSourceTransferStatus::InvalidRequest);
        CHECK(connector_calls.load() == 1);
    }
    CHECK(bundles_sent.load() == kJobs); // all original callers were in flight
    std::cerr << "P51_SENDER_SHARED_FAILURE_COUNTS sent=" << bundles_sent.load()
              << " selected=" << input_selections.load()
              << " mismatches=" << input_mismatches.load()
              << " materialized=" << materialized.load()
              << " binds=" << binds[0].load() << ".."
              << binds[kJobs - 1].load()
              << " commits=" << commits[0].load() << ".."
              << commits[kJobs - 1].load()
              << " ack=" << acknowledged.load()
              << " connectors=" << connector_calls.load()
              << " repeated_loss=" << repeat_recovery_loss << "\n";
    CHECK(input_selections.load() == kJobs);
    CHECK(input_mismatches.load() == 0);
    CHECK(materialized.load() == kJobs);
    CHECK(commits[0].load() == 1);
    for (size_t index = 1; index != kJobs; ++index)
        CHECK(commits[index].load() == 1);
    {
        std::unique_lock lock(ack_mutex);
        while (acknowledged.load(std::memory_order_acquire) < kJobs &&
               std::chrono::steady_clock::now() < deadline.as_steady_time_point())
            ack_cv.wait_until(lock, deadline.as_steady_time_point());
    }
    if (expire_after_positive_receipt)
        CHECK(acknowledged.load() <= kJobs);
    else
        CHECK(acknowledged.load() == (retire_after_positive_receipt ? 0U
                                                                     : kJobs));
    CHECK(connector_calls.load() ==
          ((retire_after_positive_receipt || expire_after_positive_receipt)
               ? 1U : repeat_recovery_loss ? 3U : 2U));
    CHECK(binds[0].load() == 1);
    for (size_t index = 1; index != kJobs; ++index)
        CHECK(binds[index].load() == 1); // uncommitted suffix binds after reset

    std::promise<void> retirement_posted;
    auto retirement_done = retirement_posted.get_future();
    asio::post(c_context, [&sender, &retirement_posted] {
        sender->retire_for_replacement();
        retirement_posted.set_value();
    });
    CHECK(retirement_done.wait_for(std::chrono::seconds(5)) ==
          std::future_status::ready);
    if (server_future.wait_for(std::chrono::seconds(5)) !=
        std::future_status::ready) {
        throw std::runtime_error(
            "shared-failure F endpoint did not observe retired-link EOF");
    }
    const auto server_runs = server_future.get();
    c_work.reset();
    c_context.stop();
    f_context.stop();
    c_thread.join();
    f_thread.join();
    CHECK(server_runs.size() == (repeat_recovery_loss ? 3U
        : (retire_after_positive_receipt || expire_after_positive_receipt)
            ? 1U : 2U));
    CHECK(server_runs[0].status == ServerRunStatus::Disconnected);
    if (server_runs.size() > 1)
        CHECK(server_runs[1].status == ServerRunStatus::Disconnected);
    if (server_runs.size() > 2)
        CHECK(server_runs[2].status == ServerRunStatus::Disconnected);
    std::cerr << "P51_SENDER_SHARED_FAILURE profile=" << profile_name
              << " callers=" << kJobs
              << " initial_sent=" << kJobs
              << " replayed_suffix=" << (kJobs - 1)
              << " repeated_loss=" << repeat_recovery_loss
              << " recovered_prefix=1 PASS\n";
}

void test_p51_sender_shared_failure_recovers_pending_callers() {
    for (const ProfileId profile : {ProfileId::ZSTD_TU, ProfileId::P29V1,
                                    ProfileId::ZSTD_ROUTE}) {
        run_p51_sender_shared_failure_case(2, profile);
        run_p51_sender_shared_failure_case(30, profile);
    }
}

void test_p51_sender_repeated_shared_failure_recovers_pending_callers() {
    for (const ProfileId profile : {ProfileId::ZSTD_TU, ProfileId::P29V1,
                                    ProfileId::ZSTD_ROUTE}) {
        run_p51_sender_shared_failure_case(2, profile, true);
        run_p51_sender_shared_failure_case(30, profile, true);
    }
}

void test_p51_sender_positive_commit_survives_later_typed_rejection() {
    run_p51_sender_shared_failure_case(
        2, ProfileId::ZSTD_TU, false, false, false, false, false, false, true);
}

void test_p51_sender_reconnect_backoff_bounds_shared_eof() {
    run_p51_sender_shared_failure_case(
        1, ProfileId::ZSTD_TU, false, false, false, false, false, true);
    run_p51_sender_shared_failure_case(
        30, ProfileId::ZSTD_TU, false, false, false, false, false, true);
    run_p51_sender_shared_failure_case(
        1, ProfileId::ZSTD_TU, false, false, false, true);
}

void test_p51_sender_retirement_wakes_shared_retry_waiter() {
    run_p51_sender_shared_failure_case(
        1, ProfileId::ZSTD_TU, false, false, false, false, true, true);
}

void test_p51_sender_retirement_during_recovery() {
    run_p51_sender_recovery_case(false, true);
    run_p51_sender_shared_failure_case(
        1, ProfileId::ZSTD_TU, false, true);
}

void test_p51_sender_deadline_during_recovery_and_ack() {
    run_p51_sender_recovery_case(false, false, true);
    run_p51_sender_shared_failure_case(
        1, ProfileId::ZSTD_TU, false, false, true);
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

int main(int argc, char** argv) {
    if (argc == 2 &&
        std::string_view(argv[1]) == "--recovery-lost-commit") {
        test_p51_sender_recovers_lost_commit_reply_after_connector_failure();
        std::cerr << "P51_SENDER_RECOVERY_SELECTOR PASS\n";
        return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--shared-failure-recovery") {
        test_p51_sender_shared_failure_recovers_pending_callers();
        std::cerr << "P51_SENDER_SHARED_FAILURE_SELECTOR PASS\n";
        return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--shared-failure-route30") {
        run_p51_sender_shared_failure_case(30, ProfileId::ZSTD_ROUTE);
        std::cerr << "P51_SENDER_SHARED_FAILURE_ROUTE30_SELECTOR PASS\n";
        return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--repeated-shared-failure") {
        test_p51_sender_repeated_shared_failure_recovers_pending_callers();
        std::cerr << "P51_SENDER_REPEATED_SHARED_FAILURE_SELECTOR PASS\n";
        return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--reconnect-backoff") {
        test_p51_sender_reconnect_backoff_bounds_shared_eof();
        std::cerr << "P51_SENDER_RECONNECT_BACKOFF_SELECTOR PASS\n";
        return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--typed-link-rejection") {
        test_p51_sender_typed_link_rejection_is_terminal_and_exact();
        test_p51_sender_typed_rejection_is_shared_route_local();
        test_p51_client_reject_echo_must_match_current_offer();
        std::cerr << "P51_SENDER_TYPED_LINK_REJECTION_SELECTOR PASS\n";
        return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--positive-after-rejection") {
        test_p51_sender_positive_commit_survives_later_typed_rejection();
        std::cerr << "P51_SENDER_POSITIVE_AFTER_REJECTION_SELECTOR PASS\n";
        return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--retry-wait-retire") {
        test_p51_sender_retirement_wakes_shared_retry_waiter();
        std::cerr << "P51_SENDER_RETRY_WAIT_RETIRE_SELECTOR PASS\n";
        return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--retire-during-recovery") {
        test_p51_sender_retirement_during_recovery();
        std::cerr << "P51_SENDER_RETIRE_DURING_RECOVERY_SELECTOR PASS\n";
        return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--deadline-recovery-ack") {
        test_p51_sender_deadline_during_recovery_and_ack();
        std::cerr << "P51_SENDER_DEADLINE_RECOVERY_ACK_SELECTOR PASS\n";
        return 0;
    }
    const auto run = [](const char* name, auto test) {
        std::cerr << "P51_TEST_BEGIN " << name << "\n";
        test();
        std::cerr << "P51_TEST_END " << name << "\n";
    };
    run("exact_network", test_exact_network_transfer);
    run("route_reuse", test_route_sender_reuses_relationship_for_two_transfers);
    run("route_ledger", test_route_completed_ledger_releases_live_entry);
    run("w30_profiles", test_p51_sender_w30_concurrent_callers_refill_and_duplicate);
    run("typed_link_rejection", test_p51_sender_typed_link_rejection_is_terminal_and_exact);
    run("shared_typed_rejection", test_p51_sender_typed_rejection_is_shared_route_local);
    run("bad_link_reject_echo", test_p51_client_reject_echo_must_match_current_offer);
    run("positive_after_rejection", test_p51_sender_positive_commit_survives_later_typed_rejection);
    run("lost_commit_recovery", test_p51_sender_recovers_lost_commit_reply_after_connector_failure);
    run("shared_failure", test_p51_sender_shared_failure_recovers_pending_callers);
    run("repeated_shared_failure", test_p51_sender_repeated_shared_failure_recovers_pending_callers);
    run("reconnect_backoff", test_p51_sender_reconnect_backoff_bounds_shared_eof);
    run("retry_wait_retire", test_p51_sender_retirement_wakes_shared_retry_waiter);
    run("retirement", test_p51_sender_retirement_during_recovery);
    run("deadline_recovery_ack", test_p51_sender_deadline_during_recovery_and_ack);
    run("cold_replacement", test_route_failure_requires_cold_replacement);
    run("explicit_route", test_explicit_route_operations_bind_request_and_deadline);
    run("explicit_retry", test_explicit_route_retry_is_bounded_then_replaced);
    run("owned_fd", test_owned_fd_and_fail_closed_validation);
    run("owned_fd_release", test_owned_fd_release_transfers_single_ownership);
    run("adopted_fd", test_adopted_fd_factory_exact_transfer);
    run("async_fd", test_async_fd_factory_exact_transfer);
    run("async_fd_late", test_async_fd_factory_late_and_throwing_completion_close_fds);
    run("deadline_required", test_absolute_deadline_is_required);
    run("disconnected_retry", test_disconnected_retry_is_bounded_and_exactly_once);
    run("deadline_not_extended", test_factory_cannot_extend_absolute_deadline);
}
