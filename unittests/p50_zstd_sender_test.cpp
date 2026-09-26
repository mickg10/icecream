#include "client/p50_zstd_sender.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
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
#include <limits>
#if defined(__linux__)
#include <linux/sockios.h>
#include <poll.h>
#include <sys/ioctl.h>
#endif
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

class ScopedP50Diagnostics {
public:
    ScopedP50Diagnostics() {
        if (const char* value = std::getenv("ICECC_P50_DIAGNOSTICS")) {
            was_set_ = true;
            previous_ = value;
        }
        if (::setenv("ICECC_P50_DIAGNOSTICS", "1", 1) != 0)
            throw std::runtime_error("could not enable P50 diagnostics for test");
    }
    ScopedP50Diagnostics(const ScopedP50Diagnostics&) = delete;
    ScopedP50Diagnostics& operator=(const ScopedP50Diagnostics&) = delete;
    ~ScopedP50Diagnostics() {
        if (was_set_)
            (void)::setenv("ICECC_P50_DIAGNOSTICS", previous_.c_str(), 1);
        else
            (void)::unsetenv("ICECC_P50_DIAGNOSTICS");
    }

private:
    bool was_set_ = false;
    std::string previous_;
};

bool p50_diagnostics_enabled_for_test() {
    const char* value = std::getenv("ICECC_P50_DIAGNOSTICS");
    return value != nullptr && std::strcmp(value, "1") == 0;
}

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
    tcp::acceptor& acceptor, P50ServerEndpoint& endpoint,
    EndpointIoControl control = {}, bool small_receive_buffer = true);

void test_p51_completed_ledger_reserves_live_capacity() {
    constexpr size_t kJobs = 2;
    constexpr uint32_t kWindow = 2;
    constexpr uint64_t kPhysicalGeneration = 27;
    const ProfileId profile = ProfileId::ZSTD_TU;
    const auto [c_guid, f_guid] = sender_r2_store_guids();
    const Id128 relationship_id = Id128::from_u64(0x5102);
    std::vector<P51SourceArmedFields> armed;
    std::vector<std::vector<uint8_t>> inputs;
    for (size_t index = 0; index != kJobs; ++index) {
        armed.push_back(sender_r2_armed(
            sender_r2_arm(c_guid, 18001 + index,
                          static_cast<uint32_t>(18101 + index), kWindow, profile),
            f_guid, 0x518001 + index, kWindow));
        const std::string text = "int ledger_cap_" + std::to_string(index) +
            " = " + std::to_string(index + 1) + ";\n";
        inputs.emplace_back(text.begin(), text.end());
    }
    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto deadline = sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
        std::chrono::steady_clock::now() + std::chrono::seconds(12),
        clock.clock_domain_id, clock.time_namespace_id);
    const auto deadline_tp = deadline.as_steady_time_point();

    std::mutex progress_mutex;
    std::condition_variable progress_cv;
    std::mutex ack_mutex;
    std::condition_variable ack_cv;
    std::atomic<bool> hold_receipt_reader{true};
    std::atomic<unsigned> bundles_sent{0};
    std::atomic<unsigned> committed{0};
    std::atomic<unsigned> acknowledged{0};
    std::atomic<bool> capacity_waiter_registered{false};
    std::atomic<unsigned> input_mismatches{0};
    std::array<std::atomic<unsigned>, kJobs> binds{};
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
            bytes.size() != inputs[index].size() ||
            !std::equal(bytes.begin(), bytes.end(), inputs[index].begin()) ||
            commit.raw_digest != icecc::digest128(inputs[index]))
            input_mismatches.fetch_add(1, std::memory_order_relaxed);
        return InputJobState::Open;
    };
    server_config.lookup_p51_link_reservation =
        [&, deadline](const LinkHello& hello)
            -> std::optional<P51SourceLinkLease> {
        if (hello.start_mode != LinkStartMode::Initial ||
            hello.profile != profile || hello.window != kWindow ||
            hello.relationship_id != relationship_id ||
            hello.c_store_guid != c_guid || hello.f_store_guid != f_guid ||
            hello.reservation_id != Id128{armed.front().reservation_id} ||
            hello.relationship_epoch != armed.front().relationship_epoch ||
            hello.physical_link_generation != kPhysicalGeneration)
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
            hello.physical_link_generation != kPhysicalGeneration ||
            binding.profile != profile || binding.tu_seq.value >= kJobs ||
            binding.relationship_ordinal != binding.tu_seq.value + 1)
            return std::nullopt;
        const size_t index = static_cast<size_t>(binding.tu_seq.value);
        if (binding.reservation_id != Id128{armed[index].reservation_id} ||
            binding.wire_job_id != armed[index].arm.source.wire_job_id ||
            binding.assignment_nonce !=
                armed[index].arm.source.assignment_nonce ||
            binding.source_request_id !=
                armed[index].arm.source.source_request_id ||
            binding.raw_bytes != inputs[index].size() ||
            binding.raw_digest != icecc::digest128(inputs[index]))
            return std::nullopt;
        if (binds[index].fetch_add(1, std::memory_order_relaxed) != 0)
            return std::nullopt;
        P51SourceJobLease lease;
        lease.armed = armed[index];
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
        committed.fetch_add(1, std::memory_order_release);
        progress_cv.notify_all();
        return true;
    };
    server_config.acknowledge_p51_receipt =
        [&](const LinkHello& hello, const CommitAck& ack) {
        if (hello.relationship_id != relationship_id ||
            ack.relationship_id != relationship_id ||
            ack.physical_link_generation != kPhysicalGeneration)
            return false;
        acknowledged.store(
            static_cast<unsigned>(ack.contiguous_verified_ordinal),
            std::memory_order_release);
        ack_cv.notify_all();
        return true;
    };
    server_config.settle_p51_interrupted_job =
        [](const LinkHello&) { return false; };
    server_config.p51_source_reservation_terminal =
        [](const JobBind&) { return false; };
    P50ServerEndpoint server(f_guid, server_caps, nullptr, nullptr,
                             std::move(server_config));
    asio::io_context f_context;
    tcp::acceptor acceptor(f_context, {asio::ip::address_v4::loopback(), 0});
    auto server_future = asio::co_spawn(
        f_context, sender_r2_accept(acceptor, server), asio::use_future);
    std::thread f_thread([&] { f_context.run(); });

    PreparationAuthorityLimits limits;
    limits.max_speculative_tus = kWindow;
    limits.max_speculative_raw_bytes = 1U << 20;
    limits.max_live_entries = 4;
    EndpointCaps caps;
    caps.profile = profile;
    caps.supported_profiles = profile_bit(profile);
    caps.zstd.max_raw_bytes = 1U << 20;
    caps.zstd.max_encoded_body_bytes = 1U << 20;
    auto authority = std::make_shared<P50PreparationAuthority>(
        c_guid, caps.zstd, limits, 1, profile);
    ZstdSourceTransferConfig sender_config = config();
    sender_config.endpoint_caps = caps;
    sender_config.authority_limits = limits;
    sender_config.max_completed_requests = 1;
    sender_config.hold_r2_receipt_reader_for_test = [&] {
        return hold_receipt_reader.load(std::memory_order_acquire);
    };
    sender_config.after_r2_completed_capacity_waiter_registered_for_test =
        [&](PrepareRequestKey key) {
        if (key != PrepareRequestKey{3, 18002})
            input_mismatches.fetch_add(1, std::memory_order_relaxed);
        capacity_waiter_registered.store(true, std::memory_order_release);
        progress_cv.notify_all();
    };
    sender_config.after_r2_bundle_sent_for_test = [&](uint64_t) {
        bundles_sent.fetch_add(1, std::memory_order_release);
        progress_cv.notify_all();
    };
    auto sender = std::make_shared<P50ZstdSourceSender>(
        authority, PreparationRouteKey{f_guid, 23, profile},
        PrepareRequestKey{3, 18001}, sender_config);

    asio::io_context c_context;
    auto c_work = asio::make_work_guard(c_context);
    AsyncConnectedFdFactory connector = [remote = acceptor.local_endpoint()]
        (auto, auto completion) { completion(connect_fd(remote)); };
    std::array<std::future<ZstdSourceTransferResult>, kJobs> results;
    results[0] = asio::co_spawn(
        c_context,
        sender->transfer_p51_route(
            armed[0], kPhysicalGeneration, connector,
            PrepareRequestKey{3, 18001}, deadline_tp, inputs[0]),
        asio::use_future);
    std::thread c_thread([&] { c_context.run(); });
    struct Cleanup {
        asio::io_context& c;
        asio::io_context& f;
        std::atomic<bool>& release_reader;
        std::thread& c_thread;
        std::thread& f_thread;
        ~Cleanup() {
            release_reader.store(false, std::memory_order_release);
            c.stop();
            f.stop();
            if (c_thread.joinable()) c_thread.join();
            if (f_thread.joinable()) f_thread.join();
        }
    } cleanup{c_context, f_context, hold_receipt_reader, c_thread, f_thread};

    {
        std::unique_lock lock(progress_mutex);
        CHECK(progress_cv.wait_until(lock, deadline_tp, [&] {
            return bundles_sent.load(std::memory_order_acquire) >= 1;
        }));
    }
    results[1] = asio::co_spawn(
        c_context,
        sender->transfer_p51_route(
            armed[1], kPhysicalGeneration, connector,
            PrepareRequestKey{3, 18002}, deadline_tp, inputs[1]),
        asio::use_future);
    {
        std::unique_lock lock(progress_mutex);
        CHECK(progress_cv.wait_until(lock, deadline_tp, [&] {
            return capacity_waiter_registered.load(std::memory_order_acquire) &&
                   bundles_sent.load(std::memory_order_acquire) == 1 &&
                   committed.load(std::memory_order_acquire) == 1;
        }));
    }
    CHECK(acknowledged.load(std::memory_order_acquire) == 0);
    CHECK(input_mismatches.load(std::memory_order_relaxed) == 0);
    hold_receipt_reader.store(false, std::memory_order_release);
    for (auto& result : results)
        CHECK(result.wait_until(deadline_tp + std::chrono::seconds(2)) ==
              std::future_status::ready);

    std::array<std::optional<ZstdSourceTransferResult>, kJobs> values;
    std::array<std::string, kJobs> exceptions;
    for (size_t index = 0; index != kJobs; ++index) {
        try {
            values[index] = results[index].get();
        } catch (const std::exception& error) {
            exceptions[index] = error.what();
        }
    }
    std::cerr << "P51_COMPLETED_LEDGER_CAP sent=" << bundles_sent.load()
              << " committed=" << committed.load()
              << " ack=" << acknowledged.load()
              << " result0=" << (values[0] ? static_cast<unsigned>(values[0]->status) : 999)
              << " result1=" << (values[1] ? static_cast<unsigned>(values[1]->status) : 999)
              << " exception0=" << exceptions[0]
              << " exception1=" << exceptions[1] << "\n";
    CHECK(values[0] && values[1]);
    CHECK(values[0]->status == ZstdSourceTransferStatus::Committed);
    CHECK(values[1]->status == ZstdSourceTransferStatus::Unavailable);
    CHECK(values[1]->replacement_required);
    CHECK(values[1]->replacement_trigger ==
          ReplacementTrigger::CompletedRequestCapacity);
    CHECK(values[1]->attempts == 0);
    CHECK((values[0]->committed_input == std::optional<InputRecordKey>{
        InputRecordKey{c_guid, TuSeq{0}}}));
    CHECK(values[0]->raw_bytes == inputs[0].size());
    CHECK(values[0]->raw_digest == icecc::digest128(inputs[0]));
    CHECK(binds[0].load(std::memory_order_acquire) == 1);
    CHECK(binds[1].load(std::memory_order_acquire) == 0);
    CHECK(bundles_sent.load(std::memory_order_acquire) == 1);
    CHECK(committed.load(std::memory_order_acquire) == 1);
    {
        std::unique_lock lock(ack_mutex);
        CHECK(ack_cv.wait_until(lock, deadline_tp + std::chrono::seconds(2), [&] {
            return acknowledged.load(std::memory_order_acquire) == 1;
        }));
    }
    CHECK(acknowledged.load(std::memory_order_acquire) == 1);
    CHECK(input_mismatches.load(std::memory_order_relaxed) == 0);
    std::atomic<unsigned> replay_connectors{0};
    auto replay = asio::co_spawn(
        c_context,
        sender->transfer_p51_route(
            armed[0], kPhysicalGeneration,
            [&](auto, auto completion) {
                replay_connectors.fetch_add(1, std::memory_order_relaxed);
                completion(-1);
            },
            PrepareRequestKey{3, 18001}, deadline_tp, inputs[0]),
        asio::use_future);
    CHECK(replay.wait_for(std::chrono::seconds(2)) ==
          std::future_status::ready);
    CHECK(replay.get().committed_input == values[0]->committed_input);
    CHECK(replay_connectors.load(std::memory_order_relaxed) == 0);
    std::promise<void> retired;
    auto retired_done = retired.get_future();
    asio::post(c_context, [&sender, &retired] {
        sender->retire_for_replacement();
        retired.set_value();
    });
    CHECK(retired_done.wait_for(std::chrono::seconds(5)) ==
          std::future_status::ready);
    CHECK(server_future.wait_for(std::chrono::seconds(5)) ==
          std::future_status::ready);
    CHECK(server_future.get().status == ServerRunStatus::Disconnected);
    c_work.reset();
    std::cerr << "P51_COMPLETED_LEDGER_CAP_SELECTOR PASS\n";
}

void test_p51_expired_retained_witness_has_bounded_cleanup(
    bool prompt_connector_failure) {
    constexpr uint32_t kWindow = 2;
    constexpr uint64_t kPhysicalGeneration = 27;
    const ProfileId profile = ProfileId::ZSTD_TU;
    const auto [c_guid, f_guid] = sender_r2_store_guids();
    const Id128 relationship_id = Id128::from_u64(0x5102);
    const auto old_armed = sender_r2_armed(
        sender_r2_arm(c_guid, 18201, 19201, kWindow, profile),
        f_guid, 0x518101, kWindow);
    const auto waiting_armed = sender_r2_armed(
        sender_r2_arm(c_guid, 18202, 19202, kWindow, profile),
        f_guid, 0x518102, kWindow);
    const auto after_armed = sender_r2_armed(
        sender_r2_arm(c_guid, 18203, 19203, kWindow, profile),
        f_guid, 0x518103, kWindow);
    const std::string old_text = "int ledger_expired_old = 1;\n";
    const std::string waiting_text = "int ledger_expired_waiting = 2;\n";
    const std::string after_text = "int ledger_expired_after = 3;\n";
    const std::vector<uint8_t> old_input(old_text.begin(), old_text.end());
    const std::vector<uint8_t> waiting_input(waiting_text.begin(), waiting_text.end());
    const std::vector<uint8_t> after_input(after_text.begin(), after_text.end());
    const auto old_deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(1);
    const auto waiter_deadline = old_deadline + std::chrono::seconds(2);

    std::mutex progress_mutex;
    std::condition_variable progress_cv;
    std::atomic<bool> capacity_waiter_registered{false};
    std::atomic<unsigned> bundles_sent{0};
    std::atomic<unsigned> binds{0};
    std::atomic<unsigned> committed{0};
    std::atomic<unsigned> acknowledged{0};
    std::atomic<unsigned> connector_calls{0};
    std::atomic<unsigned> connector_calls_after_expiry{0};
    std::atomic<unsigned> prepare_calls{0};
    std::atomic<unsigned> input_mismatches{0};
    std::atomic<bool> waiting_finished_after_old_deadline{false};

    EndpointCaps server_caps;
    server_caps.profile = profile;
    server_caps.supported_profiles = profile_bit(profile);
    server_caps.zstd.max_raw_bytes = 1U << 20;
    server_caps.zstd.max_encoded_body_bytes = 1U << 20;
    P50ServerEndpointConfig server_config;
    server_config.input_job_state = [&](CStoreGuid, const TxBegin& begin,
                                        const TxCommit& commit,
                                        std::span<const uint8_t> bytes) {
        if (begin.tu_seq.value != 0 || begin.profile != profile ||
            bytes.size() != old_input.size() ||
            !std::equal(bytes.begin(), bytes.end(), old_input.begin()) ||
            commit.raw_digest != icecc::digest128(old_input))
            input_mismatches.fetch_add(1, std::memory_order_relaxed);
        std::unique_lock lock(progress_mutex);
        if (!progress_cv.wait_for(lock, std::chrono::seconds(5), [&] {
                return capacity_waiter_registered.load(
                    std::memory_order_acquire);
            }))
            throw std::runtime_error(
                "fresh caller did not wait for the occupied ledger slot");
        return InputJobState::Open;
    };
    server_config.lookup_p51_link_reservation =
        [&, old_deadline](const LinkHello& hello)
            -> std::optional<P51SourceLinkLease> {
        if (hello.start_mode != LinkStartMode::Initial ||
            hello.profile != profile || hello.window != kWindow ||
            hello.relationship_id != relationship_id ||
            hello.c_store_guid != c_guid || hello.f_store_guid != f_guid ||
            hello.reservation_id != Id128{old_armed.reservation_id} ||
            hello.relationship_epoch != old_armed.relationship_epoch ||
            hello.physical_link_generation != kPhysicalGeneration)
            return std::nullopt;
        P51SourceLinkLease lease;
        lease.initial_armed = old_armed;
        lease.absolute_deadline = sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
            old_deadline,
            sidecar::process_monotonic_clock_identity().clock_domain_id,
            sidecar::process_monotonic_clock_identity().time_namespace_id);
        lease.relationship_epoch = hello.relationship_epoch;
        lease.history_nonce = hello.history_nonce;
        return lease;
    };
    server_config.consume_p51_job_reservation =
        [&, old_deadline](const LinkHello& hello, const JobBind& binding)
            -> std::optional<P51SourceJobLease> {
        if (hello.relationship_id != relationship_id ||
            hello.physical_link_generation != kPhysicalGeneration ||
            binding.relationship_ordinal != 1 || binding.tu_seq.value != 0 ||
            binding.reservation_id != Id128{old_armed.reservation_id} ||
            binding.raw_bytes != old_input.size() ||
            binding.raw_digest != icecc::digest128(old_input))
            return std::nullopt;
        binds.fetch_add(1, std::memory_order_release);
        P51SourceJobLease lease;
        lease.armed = old_armed;
        lease.absolute_deadline = sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
            old_deadline,
            sidecar::process_monotonic_clock_identity().clock_domain_id,
            sidecar::process_monotonic_clock_identity().time_namespace_id);
        lease.binding = binding;
        lease.binding_digest = compute_r2_binding_digest(binding);
        lease.input_key = InputRecordKey{c_guid, binding.tu_seq};
        return lease;
    };
    server_config.record_p51_job_commit =
        [&](const LinkHello& hello, const JobBind& binding,
            const R2TxCommit& commit) {
        if (hello.relationship_id != relationship_id ||
            binding.relationship_ordinal != 1 ||
            commit.relationship_ordinal != 1 ||
            commit.inner.tu_seq != binding.tu_seq ||
            commit.inner.raw_digest != icecc::digest128(old_input))
            return false;
        committed.fetch_add(1, std::memory_order_release);
        progress_cv.notify_all();
        return true;
    };
    server_config.acknowledge_p51_receipt =
        [&](const LinkHello& hello, const CommitAck& ack) {
        if (hello.relationship_id != relationship_id ||
            ack.relationship_id != relationship_id ||
            ack.physical_link_generation != hello.physical_link_generation)
            return false;
        acknowledged.fetch_add(1, std::memory_order_release);
        return true;
    };
    EndpointIoControl control;
    control.close_before_write = MessageType::R2_TX_COMMIT;
    P50ServerEndpoint server(f_guid, server_caps, nullptr, nullptr,
                             std::move(server_config));
    asio::io_context f_context;
    tcp::acceptor acceptor(f_context,
        {asio::ip::address_v4::loopback(), 0});
    auto server_future = asio::co_spawn(
        f_context, sender_r2_accept(acceptor, server, std::move(control)),
        asio::use_future);
    std::thread f_thread([&] { f_context.run(); });

    PreparationAuthorityLimits limits;
    limits.max_speculative_tus = kWindow;
    limits.max_speculative_raw_bytes = 1U << 20;
    limits.max_live_entries = 4;
    EndpointCaps caps = server_caps;
    auto authority = std::make_shared<P50PreparationAuthority>(
        c_guid, caps.zstd, limits, 1, profile);
    ZstdSourceTransferConfig sender_config = config();
    sender_config.maximum_duration = std::chrono::seconds(4);
    sender_config.endpoint_caps = caps;
    sender_config.authority_limits = limits;
    sender_config.max_completed_requests = 1;
    sender_config.before_prepare_for_route_for_test = [&] {
        prepare_calls.fetch_add(1, std::memory_order_relaxed);
    };
    sender_config.after_r2_bundle_sent_for_test = [&](uint64_t) {
        bundles_sent.fetch_add(1, std::memory_order_release);
        progress_cv.notify_all();
    };
    sender_config.after_r2_completed_capacity_waiter_registered_for_test =
        [&](PrepareRequestKey key) {
        if (key != PrepareRequestKey{3, 18202})
            input_mismatches.fetch_add(1, std::memory_order_relaxed);
        capacity_waiter_registered.store(true, std::memory_order_release);
        progress_cv.notify_all();
    };
    asio::io_context c_context;
    auto sender = std::make_shared<P50ZstdSourceSender>(
        authority, PreparationRouteKey{f_guid, 23, profile},
        PrepareRequestKey{3, 18201}, sender_config);
    auto c_work = asio::make_work_guard(c_context);
    const tcp::endpoint remote = acceptor.local_endpoint();
    AsyncConnectedFdFactory connector =
        [&](auto deadline, auto completion) {
        const unsigned call = connector_calls.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if (std::chrono::steady_clock::now() >= old_deadline)
            connector_calls_after_expiry.fetch_add(1,
                                                    std::memory_order_relaxed);
        if (call == 1) {
            completion(connect_fd(remote));
        } else if (prompt_connector_failure) {
            // F is unavailable throughout this bounded cleanup episode.
            // Recovery may be attempted, but must not replay the expired row
            // or let fresh callers restart the cleanup budget indefinitely.
            (void)deadline;
            completion(-1);
        } else {
            // Preserve the case where recovery setup withholds its completion
            // until this caller's own absolute deadline.
            auto timer = std::make_shared<asio::steady_timer>(c_context);
            timer->expires_at(deadline);
            timer->async_wait(
                [timer, completion = std::move(completion)](
                    const boost::system::error_code& error) mutable {
                    if (!error)
                        completion(-1);
                });
        }
    };
    auto first = asio::co_spawn(
        c_context,
        sender->transfer_p51_route(old_armed, kPhysicalGeneration, connector,
            PrepareRequestKey{3, 18201}, old_deadline, old_input),
        asio::use_future);
    std::thread c_thread([&] { c_context.run(); });
    struct ExpiryCleanup {
        asio::io_context& c;
        asio::io_context& f;
        std::thread& c_thread;
        std::thread& f_thread;
        ~ExpiryCleanup() {
            c.stop();
            f.stop();
            if (c_thread.joinable()) c_thread.join();
            if (f_thread.joinable()) f_thread.join();
        }
    } cleanup{c_context, f_context, c_thread, f_thread};
    {
        std::unique_lock lock(progress_mutex);
        const bool initial_bundle_sent = progress_cv.wait_until(lock, old_deadline, [&] {
            return bundles_sent.load(std::memory_order_acquire) == 1;
        });
        if (!initial_bundle_sent) {
            const bool first_ready = first.wait_for(std::chrono::milliseconds(0)) ==
                                     std::future_status::ready;
            std::optional<unsigned> first_status;
            if (first_ready)
                first_status = static_cast<unsigned>(first.get().status);
            std::cerr << "P51_COMPLETED_LEDGER_EXPIRED_WITNESS no_bundle connector_calls="
                      << connector_calls.load() << " first_ready="
                      << first_ready << " first_status="
                      << first_status.value_or(999) << " prepare_calls="
                      << prepare_calls.load() << " arm_valid="
                      << old_armed.valid() << " assignment="
                      << old_armed.arm.source.assignment_epoch << '/'
                      << old_armed.arm.source.assignment_nonce << "\n";
        }
        CHECK(initial_bundle_sent);
    }
    auto waiting_operation = [&]() -> asio::awaitable<ZstdSourceTransferResult> {
        auto result = co_await sender->transfer_p51_route(
            waiting_armed, kPhysicalGeneration, connector,
            PrepareRequestKey{3, 18202}, waiter_deadline, waiting_input);
        waiting_finished_after_old_deadline.store(
            std::chrono::steady_clock::now() >= old_deadline,
            std::memory_order_release);
        co_return result;
    };
    auto waiting = asio::co_spawn(c_context, waiting_operation(), asio::use_future);
    {
        std::unique_lock lock(progress_mutex);
        CHECK(progress_cv.wait_until(lock, old_deadline, [&] {
            return capacity_waiter_registered.load(std::memory_order_acquire) &&
                   committed.load(std::memory_order_acquire) == 1;
        }));
    }
    CHECK(bundles_sent.load(std::memory_order_acquire) == 1);
    CHECK(binds.load(std::memory_order_acquire) == 1);
    CHECK(first.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto expired = first.get();
    CHECK(expired.status == ZstdSourceTransferStatus::DeadlineExceeded);
    CHECK(waiting.wait_for(std::chrono::seconds(4)) ==
          std::future_status::ready);
    const auto replacement = waiting.get();
    std::cerr << "P51_COMPLETED_LEDGER_EXPIRED_WITNESS wait_status="
              << static_cast<unsigned>(replacement.status)
              << " replacement=" << replacement.replacement_required
              << " completed_after_deadline="
              << waiting_finished_after_old_deadline.load(
                     std::memory_order_acquire)
              << " connectors=" << connector_calls.load()
              << " post_expiry_connectors="
              << connector_calls_after_expiry.load() << "\n";
    CHECK(waiting_finished_after_old_deadline.load(
        std::memory_order_acquire));
    CHECK(replacement.status == ZstdSourceTransferStatus::Unavailable ||
          replacement.status == ZstdSourceTransferStatus::DeadlineExceeded);
    CHECK(!replacement.replacement_required);
    CHECK(replacement.attempts == 0);
    CHECK(std::chrono::steady_clock::now() <=
          waiter_deadline + std::chrono::seconds(1));
    CHECK(std::chrono::steady_clock::now() - old_deadline <
          std::chrono::seconds(4));
    CHECK(bundles_sent.load(std::memory_order_acquire) == 1);
    CHECK(binds.load(std::memory_order_acquire) == 1);
    CHECK(committed.load(std::memory_order_acquire) == 1);
    CHECK(acknowledged.load(std::memory_order_acquire) == 0);
    CHECK(connector_calls_after_expiry.load(std::memory_order_acquire) > 0);
    CHECK(connector_calls_after_expiry.load(std::memory_order_acquire) <= 20);
    CHECK(input_mismatches.load(std::memory_order_relaxed) == 0);

    const auto cleanup_deadline = old_deadline + sender_config.maximum_duration;
    std::this_thread::sleep_until(cleanup_deadline +
                                  std::chrono::milliseconds(50));
    const unsigned connectors_before_terminal =
        connector_calls.load(std::memory_order_acquire);
    const unsigned post_expiry_before_terminal =
        connector_calls_after_expiry.load(std::memory_order_acquire);
    const auto after_deadline = std::chrono::steady_clock::now() +
                                sender_config.maximum_duration;

    auto after = asio::co_spawn(
        c_context,
        sender->transfer_p51_route(after_armed, kPhysicalGeneration,
            connector, PrepareRequestKey{3, 18203}, after_deadline,
            after_input),
        asio::use_future);
    CHECK(after.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    const auto after_result = after.get();
    CHECK(after_result.status == ZstdSourceTransferStatus::Unavailable);
    CHECK(after_result.replacement_required);
    CHECK(after_result.replacement_trigger ==
          ReplacementTrigger::ExpiredUnresolvedWitness);
    CHECK(after_result.attempts == 0);
    CHECK(connector_calls.load(std::memory_order_acquire) ==
          connectors_before_terminal);
    auto terminal_again = asio::co_spawn(
        c_context,
        sender->transfer_p51_route(after_armed, kPhysicalGeneration,
            connector, PrepareRequestKey{3, 18203}, after_deadline,
            after_input),
        asio::use_future);
    CHECK(terminal_again.wait_for(std::chrono::seconds(2)) ==
          std::future_status::ready);
    const auto terminal_again_result = terminal_again.get();
    CHECK(terminal_again_result.replacement_required);
    CHECK(terminal_again_result.replacement_trigger ==
          ReplacementTrigger::ExpiredUnresolvedWitness);
    CHECK(connector_calls.load(std::memory_order_acquire) ==
          connectors_before_terminal);
    CHECK(bundles_sent.load(std::memory_order_acquire) == 1);
    CHECK(connector_calls_after_expiry.load(std::memory_order_acquire) ==
          post_expiry_before_terminal);
    CHECK(server_future.wait_for(std::chrono::seconds(5)) ==
          std::future_status::ready);
    CHECK(server_future.get().status == ServerRunStatus::Disconnected);
    std::cerr << "P51_COMPLETED_LEDGER_EXPIRED_WITNESS bounded-cleanup="
              << (prompt_connector_failure ? "prompt-failure" : "withheld")
              << " terminal-replacement=1 no-expired-replay=1\n";
}

asio::awaitable<ServerRunResult> sender_r2_accept(
    tcp::acceptor& acceptor, P50ServerEndpoint& endpoint,
    EndpointIoControl control, bool small_receive_buffer) {
    tcp::socket socket(co_await asio::this_coro::executor);
    co_await acceptor.async_accept(socket, asio::use_awaitable);
    if (small_receive_buffer)
        socket.set_option(tcp::socket::receive_buffer_size(4096));
    co_return co_await endpoint.run_adopted_r2(std::move(socket),
                                               std::move(control));
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

enum class RecoverResponseLoss { None, FirstResponse };

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
    bool reject_after_positive_receipt = false,
    std::atomic<bool>* positive_receipt_validated = nullptr,
    std::atomic<bool>* server_reset_complete = nullptr,
    std::atomic<bool>* reset_ack_loss_complete = nullptr,
    bool lose_reset_confirm = false,
    bool lose_reset_confirm_echo = false,
    bool mismatch_reset_confirm_echo = false,
    bool change_reset_ack_on_replay = false,
    std::atomic<unsigned>* mismatched_echoes_sent = nullptr,
    std::atomic<bool>* exact_echo_waiting = nullptr,
    std::atomic<bool>* release_exact_echo = nullptr,
    RecoverResponseLoss recover_response_loss = RecoverResponseLoss::None) {
    const bool lose_recover_response =
        recover_response_loss == RecoverResponseLoss::FirstResponse;
    const size_t connection_count =
        (reject_stale_reconnect || close_reconnect_after_hello ||
         reject_after_positive_receipt) ? 1
        : change_reset_ack_on_replay ? 4
        : (repeat_recovery_loss || lose_recover_response || lose_reset_confirm ||
           lose_reset_confirm_echo || mismatch_reset_confirm_echo) ? 3
        : (retire_after_positive_receipt || expire_after_positive_receipt) ? 1
                                                                           : 2;
    std::vector<ServerRunResult> results(connection_count);
    for (size_t index = 0; index != results.size(); ++index) {
        tcp::socket socket(co_await acceptor.async_accept(asio::use_awaitable));
        socket.set_option(tcp::socket::receive_buffer_size(4096));
        EndpointIoControl control;
        if (index == 0 && reject_after_positive_receipt) {
            socket.set_option(asio::socket_base::linger(true, 0));
            control.close_after_write = MessageType::R2_TX_COMMIT;
            control.outbound_message_observer =
                [positive_receipt_validated](ActorSide, const Message& message) {
                if (!std::holds_alternative<R2TxCommit>(message)) return;
                const auto until = std::chrono::steady_clock::now() +
                                   std::chrono::seconds(5);
                while (positive_receipt_validated != nullptr &&
                       !positive_receipt_validated->load(
                           std::memory_order_acquire) &&
                       std::chrono::steady_clock::now() < until)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if (positive_receipt_validated == nullptr ||
                    !positive_receipt_validated->load(
                        std::memory_order_acquire))
                    throw std::runtime_error(
                        "C did not validate positive R2 receipt before reset");
            };
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
        } else if (index == 1 && lose_recover_response) {
            // The recovery service has already returned its exact interval;
            // lose the first RECEIPTS frame before C can apply it.
            control.close_before_write = MessageType::RECEIPTS;
        } else if ((index == 1 && lose_reset_confirm_echo) ||
                   (index == 1 && change_reset_ack_on_replay)) {
            // F processes the exact confirmation, then loses the confirmation
            // echo before it can reach C. C must retain and retry this logical
            // operation instead of inventing a new RESET.
            control.close_before_write = MessageType::RESET_CONFIRM;
        } else if (mismatch_reset_confirm_echo && index == 1) {
            control.r2_reset_confirm_echo_transform_for_test =
                [mismatched_echoes_sent](const ResetConfirm& applied) {
                    if (mismatched_echoes_sent != nullptr)
                        mismatched_echoes_sent->fetch_add(
                            1, std::memory_order_release);
                    ResetConfirm wrong = applied;
                    wrong.operation_id.bytes.back() ^= 0x01;
                    if (wrong.operation_id == Id128{})
                        wrong.operation_id.bytes.front() = 0x01;
                    return wrong;
                };
        } else if (mismatch_reset_confirm_echo && index == 2) {
            control.r2_reset_confirm_echo_transform_for_test =
                [exact_echo_waiting, release_exact_echo](
                    const ResetConfirm& applied) {
                    if (exact_echo_waiting == nullptr ||
                        release_exact_echo == nullptr)
                        throw std::runtime_error(
                            "exact RESET_CONFIRM echo gate was not configured");
                    exact_echo_waiting->store(true, std::memory_order_release);
                    const auto until = std::chrono::steady_clock::now() +
                                       std::chrono::seconds(3);
                    while (!release_exact_echo->load(std::memory_order_acquire) &&
                           std::chrono::steady_clock::now() < until)
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(1));
                    if (!release_exact_echo->load(std::memory_order_acquire))
                        throw std::runtime_error(
                            "C did not wait for exact RESET_CONFIRM echo");
                    return applied;
                };
        }
        results[index] = co_await endpoint.run_adopted_r2(
            std::move(socket), std::move(control));
        if (index == 1 && repeat_recovery_loss &&
            reset_ack_loss_complete != nullptr)
            reset_ack_loss_complete->store(true, std::memory_order_release);
        if (index == 0 && reject_after_positive_receipt &&
            server_reset_complete != nullptr)
            server_reset_complete->store(true, std::memory_order_release);
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

void run_p51_sender_window_concurrent_callers(
    ProfileId profile, size_t kWindow, size_t kJobs,
    size_t required_occupancy, bool fail_first_connector = false,
    bool expect_underfilled_witness = false) {
    ScopedP50Diagnostics diagnostics;
    CHECK(kWindow != 0 && kWindow <= 30);
    CHECK(kJobs > kWindow && kJobs <= 31);
    CHECK(required_occupancy != 0);
    const char* const profile_name = profile == ProfileId::P29V1 ? "P29V1"
                                    : profile == ProfileId::ZSTD_ROUTE ? "ZSTD_ROUTE"
                                    : "ZSTD_TU";
    std::cerr << "P51_SENDER_WINDOW_PROFILE_START=" << profile_name
              << " selected_window=" << kWindow << " jobs=" << kJobs << "\n";
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
    std::atomic<bool> mismatch_detail_logged{false};
    std::mutex interval_observer_mutex;
    std::condition_variable interval_observer_cv;
    std::vector<R2WireControlSnapshot> observed_intervals;
    std::vector<bool> reservation_consumed(kJobs, false);
    std::vector<size_t> input_index_by_tu(kJobs, kJobs);
    std::mutex consumed_mutex;
    struct ObservedSocketBytes {
        uint64_t c_to_f = 0;
        uint64_t f_to_c = 0;
    };
    CompletionLog f_socket_observations;
    std::vector<ObservedSocketBytes> bytes_at_ack;
    P50ServerEndpointConfig server_config;
    EndpointCaps server_caps;
    server_caps.profile = profile;
    server_caps.supported_profiles = profile_bit(profile);
    server_config.input_job_state = [&, profile](CStoreGuid observed_c,
                                                  const TxBegin& begin,
                                                  const TxCommit& commit,
                                                  std::span<const uint8_t> bytes) {
        size_t input_index = kJobs;
        if (begin.tu_seq.value < input_index_by_tu.size()) {
            std::lock_guard lock(consumed_mutex);
            input_index = input_index_by_tu[begin.tu_seq.value];
        }
        const bool in_range = input_index < input.size();
        const bool same_bytes = in_range &&
            bytes.size() == input[input_index].size() &&
            std::equal(bytes.begin(), bytes.end(),
                       input[input_index].begin());
        const bool same_digest = in_range &&
            commit.raw_digest == icecc::digest128(input[input_index]);
        if (observed_c != c_guid || begin.profile != profile || !in_range ||
            !same_bytes || !same_digest) {
            exact_input_mismatches.fetch_add(1, std::memory_order_relaxed);
            if (!mismatch_detail_logged.exchange(true, std::memory_order_relaxed))
                std::cerr << "P51_SENDER_WINDOW_INPUT_MISMATCH profile="
                          << profile_name << " tu=" << begin.tu_seq.value
                          << " in_range=" << in_range
                          << " begin_profile="
                          << static_cast<unsigned>(begin.profile)
                          << " begin_raw=" << begin.raw_bytes
                          << " actual_size=" << bytes.size()
                          << " expected_size="
                          << (in_range ? input[input_index].size() : 0)
                          << " same_bytes=" << same_bytes
                          << " same_digest=" << same_digest << "\n";
        }
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
        if (index == kJobs || binding.tu_seq.value >= kJobs ||
            binding.raw_bytes != input[index].size() ||
            binding.raw_digest != icecc::digest128(input[index]) ||
            binding.wire_job_id != armed[index].arm.source.wire_job_id ||
            binding.assignment_nonce != armed[index].arm.source.assignment_nonce ||
            binding.source_request_id != armed[index].arm.source.source_request_id)
            return std::nullopt;
        {
            std::lock_guard lock(consumed_mutex);
            if (reservation_consumed[index] ||
                input_index_by_tu[binding.tu_seq.value] != kJobs)
                return std::nullopt;
            reservation_consumed[index] = true;
            input_index_by_tu[binding.tu_seq.value] = index;
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
            ObservedSocketBytes observed;
            for (const AsyncCompletion& completion :
                 f_socket_observations.completions()) {
                if (!completion.stamp.r2_traffic ||
                    completion.stamp.actor != ActorSide::F)
                    continue;
                uint64_t* total = nullptr;
                if (completion.stamp.operation ==
                        AsyncOperationKind::ReadHeader ||
                    completion.stamp.operation ==
                        AsyncOperationKind::ReadPayload)
                    total = &observed.c_to_f;
                else if (completion.stamp.operation ==
                         AsyncOperationKind::WriteFragment)
                    total = &observed.f_to_c;
                if (total != nullptr) {
                    CHECK(completion.transferred_bytes <=
                          std::numeric_limits<uint64_t>::max() - *total);
                    *total += completion.transferred_bytes;
                }
            }
            bytes_at_ack.push_back(observed);
            acknowledged.store(static_cast<unsigned>(ack.contiguous_verified_ordinal),
                               std::memory_order_release);
            ack_cv.notify_all();
            return true;
        };
    P50ServerEndpoint server(f_guid, server_caps, &f_socket_observations, nullptr,
                             std::move(server_config));
    auto server_future = asio::co_spawn(f_context,
        sender_r2_accept(acceptor, server, EndpointIoControl{}, false),
        asio::use_future);
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
    sender_config.r2_interval_observer = [&](const R2WireControlSnapshot& interval) {
        {
            std::lock_guard lock(interval_observer_mutex);
            observed_intervals.push_back(interval);
        }
        interval_observer_cv.notify_all();
        return true;
    };
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
        const unsigned call = connector_calls.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if (fail_first_connector && call == 1) {
            completion(-1);
            return;
        }
        const int fd = connect_fd(remote);
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
                if (fail_first_connector) {
                    // This selector intentionally checks that the original
                    // caller survives a connector failure before any R2
                    // socket, pending receipt, or bundle exists.
                    CHECK(first.status == ZstdSourceTransferStatus::Committed);
                }
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
    std::promise<void> submissions_started;
    std::future<void> submissions_started_future = submissions_started.get_future();
    asio::post(c_context, [&submissions_started] {
        submissions_started.set_value();
    });
    CHECK(submissions_started_future.wait_for(std::chrono::seconds(5)) ==
          std::future_status::ready);
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
    const unsigned sent_before_first_receipt =
        bundles_sent.load(std::memory_order_acquire);
    const unsigned ack_before_first_receipt =
        acknowledged.load(std::memory_order_acquire);
    size_t unsettled_job_calls = 0;
    for (size_t index = 0; index != results.size(); ++index) {
        if (index != 0 && index < 3) continue;
        if (results[index].wait_for(std::chrono::milliseconds(0)) !=
            std::future_status::ready)
            ++unsettled_job_calls;
    }
    const bool occupancy_witness_met =
        sent_before_first_receipt >= required_occupancy &&
        ack_before_first_receipt == 0;
    CHECK(unsettled_job_calls == kJobs);
    if (expect_underfilled_witness) {
        CHECK(required_occupancy == 30);
        CHECK(kWindow == 1 && kJobs == 31);
        CHECK(sent_before_first_receipt == 1);
        CHECK(ack_before_first_receipt == 0);
        CHECK(!occupancy_witness_met);
        std::cerr << "P51_SENDER_SERIAL_CONTROL profile=" << profile_name
                  << " selected_window=" << kWindow
                  << " queued_jobs=" << kJobs
                  << " unsettled_calls=" << unsettled_job_calls
                  << " demanded_peak=30 observed_peak="
                  << sent_before_first_receipt
                  << " witness=underfilled-as-expected\n";
    } else {
        CHECK(occupancy_witness_met);
        CHECK(sent_before_first_receipt == kWindow);
        CHECK(ack_before_first_receipt == 0);
        std::cerr << "P51_SENDER_WINDOW profile=" << profile_name
                  << " selected_window=" << kWindow
                  << " jobs=" << kJobs
                  << " unsettled_calls=" << unsettled_job_calls
                  << " before_first_receipt=" << sent_before_first_receipt
                  << " PASS\n";
    }
    hold_receipt_reader.store(false, std::memory_order_release);
    progress_cv.notify_all();

    const auto result_deadline = deadline.as_steady_time_point();
    const auto get_result_by_deadline = [&](auto& future) {
        CHECK(future.wait_until(result_deadline) == std::future_status::ready);
        return future.get();
    };
    const auto first_result = get_result_by_deadline(results.front());
    CHECK(first_result.status == ZstdSourceTransferStatus::Committed);
    std::vector<ZstdSourceTransferResult> unique_results(kJobs);
    unique_results[0] = first_result;
    const auto duplicate_result = get_result_by_deadline(results[1]);
    CHECK(duplicate_result.status == ZstdSourceTransferStatus::Committed ||
          duplicate_result.status == ZstdSourceTransferStatus::InvalidRequest);
    if (duplicate_result.status == ZstdSourceTransferStatus::Committed)
        CHECK(duplicate_result.committed_input == first_result.committed_input);
    CHECK(get_result_by_deadline(results[2]).status ==
          ZstdSourceTransferStatus::InvalidRequest);
    std::vector<bool> result_tu_seen(kJobs, false);
    const auto validate_committed_result = [&](const ZstdSourceTransferResult& result,
                                               size_t job_index) {
        CHECK(result.status == ZstdSourceTransferStatus::Committed);
        CHECK(result.committed_input.has_value());
        CHECK(result.committed_input->c_store_guid == c_guid);
        CHECK(result.committed_input->tu_seq.value < kJobs);
        CHECK(!result_tu_seen[result.committed_input->tu_seq.value]);
        result_tu_seen[result.committed_input->tu_seq.value] = true;
        CHECK(result.raw_bytes == input[job_index].size());
        CHECK(result.raw_digest == icecc::digest128(input[job_index]));
    };
    validate_committed_result(first_result, 0);
    for (size_t job_index = 1; job_index != kJobs; ++job_index) {
        unique_results[job_index] =
            get_result_by_deadline(results[job_index + 2]);
        validate_committed_result(unique_results[job_index], job_index);
    }
    CHECK(std::all_of(result_tu_seen.begin(), result_tu_seen.end(),
                      [](bool seen) { return seen; }));
    CHECK(committed.load(std::memory_order_acquire) == kJobs);
    CHECK(exact_input_mismatches.load(std::memory_order_relaxed) == 0);
    std::vector<bool> mapped_job_seen(kJobs, false);
    for (const size_t job_index : input_index_by_tu) {
        CHECK(job_index < kJobs);
        CHECK(!mapped_job_seen[job_index]);
        mapped_job_seen[job_index] = true;
    }
    CHECK(std::all_of(mapped_job_seen.begin(), mapped_job_seen.end(),
                      [](bool seen) { return seen; }));
    {
        std::unique_lock lock(ack_mutex);
        CHECK(ack_cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return acknowledged.load(std::memory_order_acquire) == kJobs;
        }));
    }
    CHECK(acknowledged.load(std::memory_order_acquire) == kJobs);
    CHECK(!bytes_at_ack.empty());
    CHECK(bytes_at_ack.back().c_to_f != 0 &&
          bytes_at_ack.back().f_to_c != 0);
    CHECK(server_future.wait_for(std::chrono::milliseconds(0)) !=
          std::future_status::ready);
    std::vector<R2WireControlSnapshot> link_intervals;
    uint64_t interval_c_to_f = 0;
    uint64_t interval_f_to_c = 0;
    for (size_t result_index = 0; result_index != unique_results.size();
         ++result_index) {
        const ZstdSourceTransferResult& result = unique_results[result_index];
        CHECK(result.r2_wire_accounting.has_value());
        CHECK(result.r2_wire_accounting->valid);
        CHECK(result.r2_wire_accounting->bundle_attempts == 1);
        CHECK(result.r2_wire_accounting->replay_attempts == 0);
        CHECK(result.r2_wire_accounting->c_to_f_bundle_bytes != 0);
        CHECK(result.r2_wire_accounting->f_to_c_receipt_bytes != 0);
        CHECK(result.r2_link_intervals_external);
        CHECK(result.r2_link_intervals.empty());
        CHECK(result.r2_link_intervals_valid);
    }
    {
        std::lock_guard lock(interval_observer_mutex);
        link_intervals = observed_intervals;
    }
    CHECK(!link_intervals.empty());
    uint64_t prior_interval_sequence = 0;
    uint64_t drained_ack_prefix = 0;
    for (const R2WireControlSnapshot& interval : link_intervals) {
        CHECK(interval.valid);
        CHECK(interval.link.c_store_guid == c_guid);
        CHECK(interval.link.f_store_guid == f_guid);
        CHECK(interval.link.logical_link_id == relationship_id);
        CHECK(interval.link.physical_link_generation == 27);
        CHECK(interval.interval_sequence > prior_interval_sequence);
        prior_interval_sequence = interval.interval_sequence;
        if (interval.end == R2WireIntervalEnd::DrainedAckCheckpoint) {
            CHECK(interval.drained_ack_prefix >= drained_ack_prefix);
            drained_ack_prefix = interval.drained_ack_prefix;
        } else {
            CHECK(interval.end == R2WireIntervalEnd::WindowPressure);
            CHECK(interval.drained_ack_prefix == 0);
        }
        uint64_t classified_c_to_f = interval.shared_c_to_f_bytes;
        uint64_t classified_f_to_c = interval.shared_f_to_c_bytes;
        for (const R2WireJobInterval& job : interval.jobs) {
            CHECK(job.valid);
            CHECK(job.link == interval.link);
            CHECK(job.c_to_f_bundle_bytes <=
                  std::numeric_limits<uint64_t>::max() - classified_c_to_f);
            CHECK(job.f_to_c_receipt_bytes <=
                  std::numeric_limits<uint64_t>::max() - classified_f_to_c);
            classified_c_to_f += job.c_to_f_bundle_bytes;
            classified_f_to_c += job.f_to_c_receipt_bytes;
        }
        CHECK(classified_c_to_f == interval.total_c_to_f_bytes);
        CHECK(classified_f_to_c == interval.total_f_to_c_bytes);
        CHECK(interval.total_c_to_f_bytes <=
              std::numeric_limits<uint64_t>::max() - interval_c_to_f);
        CHECK(interval.total_f_to_c_bytes <=
              std::numeric_limits<uint64_t>::max() - interval_f_to_c);
        interval_c_to_f += interval.total_c_to_f_bytes;
        interval_f_to_c += interval.total_f_to_c_bytes;
    }
    CHECK(interval_c_to_f == bytes_at_ack.back().c_to_f);
    CHECK(interval_f_to_c == bytes_at_ack.back().f_to_c);
    CHECK(drained_ack_prefix == kJobs);
    CHECK(sender->retained_completion_records_for_test() == 0);
    CHECK(connector_calls.load(std::memory_order_relaxed) ==
          (fail_first_connector ? 2U : 1U));
    {
        std::lock_guard lock(interval_observer_mutex);
        CHECK(std::none_of(observed_intervals.begin(), observed_intervals.end(),
                           [](const R2WireControlSnapshot& interval) {
            return interval.end == R2WireIntervalEnd::PhysicalLinkRetired;
        }));
    }
    std::promise<void> retirement_posted;
    auto retirement_done = retirement_posted.get_future();
    asio::post(c_context, [&sender, &retirement_posted] {
        sender->retire_for_replacement();
        sender->retire_for_replacement();
        retirement_posted.set_value();
    });
    CHECK(retirement_done.wait_for(std::chrono::seconds(5)) ==
          std::future_status::ready);
    CHECK(server_future.wait_for(std::chrono::seconds(5)) ==
          std::future_status::ready);
    c_work.reset();
    const ServerRunResult server_result = server_future.get();
    CHECK(server_result.status == ServerRunStatus::Disconnected);
    {
        std::unique_lock lock(interval_observer_mutex);
        CHECK(interval_observer_cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return std::any_of(observed_intervals.begin(), observed_intervals.end(),
                               [](const R2WireControlSnapshot& interval) {
                return interval.end == R2WireIntervalEnd::PhysicalLinkRetired;
            });
        }));
        const size_t terminal_count = static_cast<size_t>(std::count_if(
            observed_intervals.begin(), observed_intervals.end(),
            [](const R2WireControlSnapshot& interval) {
                return interval.end == R2WireIntervalEnd::PhysicalLinkRetired;
            }));
        CHECK(terminal_count == 1);
        CHECK(observed_intervals.back().end ==
              R2WireIntervalEnd::PhysicalLinkRetired);
        CHECK(observed_intervals.back().link.c_store_guid == c_guid);
        CHECK(observed_intervals.back().link.f_store_guid == f_guid);
        CHECK(observed_intervals.back().link.logical_link_id == relationship_id);
        CHECK(observed_intervals.back().link.relationship_epoch ==
              armed.front().relationship_epoch);
        CHECK(observed_intervals.back().link.physical_link_generation == 27);
        interval_c_to_f = 0;
        interval_f_to_c = 0;
        for (const R2WireControlSnapshot& interval : observed_intervals) {
            CHECK(interval.valid);
            CHECK(interval.total_c_to_f_bytes <=
                  std::numeric_limits<uint64_t>::max() - interval_c_to_f);
            CHECK(interval.total_f_to_c_bytes <=
                  std::numeric_limits<uint64_t>::max() - interval_f_to_c);
            interval_c_to_f += interval.total_c_to_f_bytes;
            interval_f_to_c += interval.total_f_to_c_bytes;
        }
    }
    uint64_t final_f_c_to_f = 0;
    uint64_t final_f_f_to_c = 0;
    for (const AsyncCompletion& completion : f_socket_observations.completions()) {
        if (!completion.stamp.r2_traffic ||
            completion.stamp.actor != ActorSide::F)
            continue;
        if (completion.stamp.operation == AsyncOperationKind::ReadHeader ||
            completion.stamp.operation == AsyncOperationKind::ReadPayload)
            final_f_c_to_f += completion.transferred_bytes;
        else if (completion.stamp.operation == AsyncOperationKind::WriteFragment)
            final_f_f_to_c += completion.transferred_bytes;
    }
    CHECK(interval_c_to_f == final_f_c_to_f);
    CHECK(interval_f_to_c == final_f_f_to_c);
    // The retirement observer is delivered by the C executor itself; leave
    // it running until that final snapshot has been observed above.
    c_context.stop();
    c_thread.join();
    f_context.stop();
    f_thread.join();
    if (kWindow == 30 && !expect_underfilled_witness)
        std::cerr << "P51_SENDER_W30_PROFILE=" << profile_name << " PASS\n";
}

void test_p51_sender_w30_concurrent_callers_refill_and_duplicate(
    bool fail_first_connector = false) {
    for (const ProfileId profile : {ProfileId::ZSTD_TU, ProfileId::P29V1,
                                    ProfileId::ZSTD_ROUTE})
        run_p51_sender_window_concurrent_callers(
            profile, 30, 31, 30, fail_first_connector);
}

void test_p51_sender_window_matrix() {
    constexpr std::array<size_t, 6> windows{1, 2, 4, 8, 16, 30};
    for (const ProfileId profile : {ProfileId::ZSTD_TU, ProfileId::P29V1,
                                    ProfileId::ZSTD_ROUTE}) {
        for (const size_t window : windows)
            run_p51_sender_window_concurrent_callers(
                profile, window, window + 1, window);
    }
}

void test_p51_sender_serial_w1_control_fails_w30_witness() {
    for (const ProfileId profile : {ProfileId::ZSTD_TU, ProfileId::P29V1,
                                    ProfileId::ZSTD_ROUTE})
        run_p51_sender_window_concurrent_callers(
            profile, 1, 31, 30, false, true);
}

void test_p51_sender_window_matrix_and_serial_control() {
    test_p51_sender_window_matrix();
    test_p51_sender_serial_w1_control_fails_w30_witness();
}

void test_p51_sender_serial_control_only() {
    test_p51_sender_serial_w1_control_fails_w30_witness();
}

#if defined(__linux__)
asio::awaitable<ServerRunResult> sender_r2_accept_with_first_commit_gate(
    tcp::acceptor& acceptor, P50ServerEndpoint& endpoint,
    std::atomic<int>& observed_peer_fd, std::atomic<bool>& gate_entered,
    std::mutex& gate_mutex, std::condition_variable& gate_cv,
    bool& release_gate) {
    tcp::socket socket(co_await asio::this_coro::executor);
    co_await acceptor.async_accept(socket, asio::use_awaitable);
    socket.set_option(tcp::socket::receive_buffer_size(4096));
    const int duplicate = ::dup(socket.native_handle());
    observed_peer_fd.store(duplicate, std::memory_order_release);
    if (duplicate < 0)
        throw std::runtime_error("D16 could not duplicate the accepted peer socket");

    EndpointIoControl control;
    control.outbound_message_observer =
        [&](ActorSide actor, const Message& message) {
            if (actor != ActorSide::F ||
                !std::holds_alternative<R2TxCommit>(message) ||
                std::get<R2TxCommit>(message).inner.tu_seq.value != 0)
                return;
            gate_entered.store(true, std::memory_order_release);
            gate_cv.notify_all();
            std::unique_lock lock(gate_mutex);
            if (!gate_cv.wait_for(lock, std::chrono::seconds(10), [&] {
                    return release_gate;
                }))
                throw std::runtime_error("D16 first-COMMIT gate expired");
        };
    co_return co_await endpoint.run_adopted_r2(std::move(socket),
                                               std::move(control));
}

asio::awaitable<void> sender_test_heartbeat(
    std::atomic<bool>& stop, std::atomic<uint64_t>& ticks) {
    asio::steady_timer timer(co_await asio::this_coro::executor);
    while (!stop.load(std::memory_order_acquire)) {
        timer.expires_after(std::chrono::milliseconds(5));
        boost::system::error_code error;
        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable,
                                                       error));
        if (error) co_return;
        ticks.fetch_add(1, std::memory_order_release);
    }
}

void run_p51_sender_writer_backpressure_case(
    ProfileId profile, bool retire_while_reader_and_writer_held = false) {
    ScopedP50Diagnostics diagnostics;
    constexpr size_t kLargeRawBytes = 512U << 10;
    constexpr uint64_t kPhysicalGeneration = 27;
    const auto [c_guid, f_guid] = sender_r2_store_guids();
    const char* const profile_name = profile == ProfileId::P29V1 ? "P29V1"
                                    : profile == ProfileId::ZSTD_ROUTE ? "ZSTD_ROUTE"
                                    : "ZSTD_TU";
    const Id128 relationship_id = Id128::from_u64(0x5102);
    constexpr uint32_t kWindow = 2;
    std::array<P51SourceArmedFields, 2> armed;
    for (size_t i = 0; i != armed.size(); ++i) {
        armed[i] = sender_r2_armed(
            sender_r2_arm(c_guid, 31001 + i,
                          static_cast<uint32_t>(31101 + i), kWindow, profile),
            f_guid, 0x531001 + i, kWindow);
    }
    std::array<std::vector<uint8_t>, 2> input;
    input[0] = {'D', '1', '6', '-', 'a', 'c', 'k'};
    input[1].resize(kLargeRawBytes);
    uint32_t random = 0x9e3779b9U;
    for (uint8_t& byte : input[1]) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        byte = static_cast<uint8_t>(random);
    }
    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto deadline = sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
        std::chrono::steady_clock::now() + std::chrono::seconds(30),
        clock.clock_domain_id, clock.time_namespace_id);

    asio::io_context c_context;
    asio::io_context f_context;
    std::thread c_thread;
    std::thread f_thread;
    tcp::acceptor acceptor(f_context,
        {asio::ip::address_v4::loopback(), 0});
    std::mutex f_gate_mutex;
    std::condition_variable f_gate_cv;
    bool release_f_gate = false;
    std::atomic<bool> f_gate_entered{false};
    std::atomic<int> observed_f_fd{-1};
    std::atomic<unsigned> commit_count{0};
    std::atomic<unsigned> input_mismatches{0};
    CompletionLog f_socket_observations;
    std::mutex interval_mutex;
    std::condition_variable interval_cv;
    std::vector<R2WireControlSnapshot> observed_intervals;
    std::atomic<unsigned> terminal_intervals{0};
    std::atomic<int> terminal_count_during_retire{-1};
    std::atomic<bool> observer_delivery_valid{true};

    P50ServerEndpointConfig server_config;
    server_config.input_job_state = [&](CStoreGuid observed_c,
                                        const TxBegin& begin,
                                        const TxCommit& commit,
                                        std::span<const uint8_t> bytes) {
        const size_t index = static_cast<size_t>(begin.tu_seq.value);
        if (observed_c != c_guid || index >= input.size() ||
            begin.profile != profile || bytes.size() != input[index].size() ||
            !std::equal(bytes.begin(), bytes.end(), input[index].begin()) ||
            commit.raw_digest != icecc::digest128(input[index]))
            input_mismatches.fetch_add(1, std::memory_order_relaxed);
        return InputJobState::Open;
    };
    server_config.lookup_p51_link_reservation =
        [&, deadline](const LinkHello& hello)
            -> std::optional<P51SourceLinkLease> {
        if (hello.profile != profile || hello.window != kWindow ||
            hello.relationship_id != relationship_id ||
            hello.relationship_epoch != armed[0].relationship_epoch ||
            hello.reservation_id != Id128{armed[0].reservation_id} ||
            hello.c_store_guid != c_guid || hello.f_store_guid != f_guid ||
            hello.physical_link_generation != kPhysicalGeneration)
            return std::nullopt;
        P51SourceLinkLease lease;
        lease.initial_armed = armed[0];
        lease.absolute_deadline = deadline;
        lease.relationship_epoch = hello.relationship_epoch;
        lease.history_nonce = hello.history_nonce;
        return lease;
    };
    server_config.consume_p51_job_reservation =
        [&, deadline](const LinkHello& hello, const JobBind& binding)
            -> std::optional<P51SourceJobLease> {
        const size_t index = static_cast<size_t>(binding.tu_seq.value);
        if (index >= armed.size() || hello.relationship_id != relationship_id ||
            binding.physical_link_generation != kPhysicalGeneration ||
            binding.profile != profile ||
            binding.reservation_id != Id128{armed[index].reservation_id} ||
            binding.raw_bytes != input[index].size() ||
            binding.raw_digest != icecc::digest128(input[index]))
            return std::nullopt;
        P51SourceJobLease lease;
        lease.armed = armed[index];
        lease.absolute_deadline = deadline;
        lease.binding = binding;
        lease.binding_digest = compute_r2_binding_digest(binding);
        lease.input_key = InputRecordKey{c_guid, binding.tu_seq};
        return lease;
    };
    server_config.record_p51_job_commit =
        [&](const LinkHello& hello, const JobBind& binding,
            const R2TxCommit&) {
        if (hello.relationship_id != relationship_id ||
            binding.tu_seq.value >= input.size())
            return false;
        commit_count.fetch_add(1, std::memory_order_release);
        return true;
    };
    server_config.acknowledge_p51_receipt =
        [&](const LinkHello& hello, const CommitAck& ack) {
        return hello.relationship_id == relationship_id &&
            ack.relationship_id == relationship_id &&
            ack.physical_link_generation == kPhysicalGeneration;
    };
    EndpointCaps server_caps;
    server_caps.profile = profile;
    server_caps.supported_profiles = profile_bit(profile);
    server_caps.zstd.max_raw_bytes = 1U << 20;
    server_caps.zstd.max_encoded_body_bytes = 1U << 20;
    P50ServerEndpoint server(f_guid, server_caps, &f_socket_observations, nullptr,
                             std::move(server_config));
    auto server_future = asio::co_spawn(
        f_context, sender_r2_accept_with_first_commit_gate(
            acceptor, server, observed_f_fd, f_gate_entered,
            f_gate_mutex, f_gate_cv, release_f_gate), asio::use_future);

    PreparationAuthorityLimits limits;
    limits.max_speculative_tus = kWindow;
    limits.max_speculative_raw_bytes = 2U << 20;
    limits.max_live_entries = 8;
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
    sender_config.r2_interval_observer = [&](const R2WireControlSnapshot& interval) {
        {
            std::lock_guard lock(interval_mutex);
            observed_intervals.push_back(interval);
        }
        if (!interval.valid)
            observer_delivery_valid.store(false, std::memory_order_release);
        if (interval.end == R2WireIntervalEnd::PhysicalLinkRetired)
            terminal_intervals.fetch_add(1, std::memory_order_release);
        interval_cv.notify_all();
        return true;
    };
    if (profile == ProfileId::ZSTD_ROUTE)
        sender_config.compression_level = 3;
    std::atomic<unsigned> complete_bundles{0};
    std::atomic<bool> first_receipt_validated{false};
    std::atomic<bool> hold_receipt_reader{true};
    std::mutex progress_mutex;
    std::condition_variable progress_cv;
    sender_config.after_r2_bundle_sent_for_test = [&](uint64_t) {
        complete_bundles.fetch_add(1, std::memory_order_release);
        progress_cv.notify_all();
    };
    sender_config.after_r2_receipt_validated_for_test = [&](uint64_t ordinal) {
        if (ordinal == 1)
            first_receipt_validated.store(true, std::memory_order_release);
        progress_cv.notify_all();
    };
    sender_config.hold_r2_receipt_reader_for_test = [&] {
        return hold_receipt_reader.load(std::memory_order_acquire);
    };
    auto sender = std::make_shared<P50ZstdSourceSender>(
        authority, route, PrepareRequestKey{3, 31001}, sender_config);

    auto c_work = asio::make_work_guard(c_context);
    std::atomic<int> observed_c_fd{-1};
    std::atomic<unsigned> connector_calls{0};
    const tcp::endpoint remote = acceptor.local_endpoint();
    AsyncConnectedFdFactory connector = [&](auto, auto completion) {
        const unsigned call = connector_calls.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if (call != 1) {
            completion(-1);
            return;
        }
        const int fd = connect_fd(remote);
        if (fd >= 0) {
            const int tiny = 4096;
            if (::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &tiny, sizeof(tiny)) != 0) {
                (void)::close(fd);
                completion(-1);
                return;
            }
            const int duplicate = ::dup(fd);
            observed_c_fd.store(duplicate, std::memory_order_release);
            if (duplicate < 0) {
                (void)::close(fd);
                completion(-1);
                return;
            }
        }
        completion(fd);
    };
    std::atomic<bool> stop_heartbeat{false};
    std::atomic<uint64_t> heartbeat_ticks{0};
    auto heartbeat = asio::co_spawn(c_context,
        sender_test_heartbeat(stop_heartbeat, heartbeat_ticks),
        asio::use_future);
    struct Cleanup {
        asio::io_context& c_context;
        asio::io_context& f_context;
        std::thread& c_thread;
        std::thread& f_thread;
        std::atomic<bool>& stop_heartbeat;
        std::atomic<int>& observed_c_fd;
        std::atomic<int>& observed_f_fd;
        std::mutex& f_gate_mutex;
        std::condition_variable& f_gate_cv;
        bool& release_f_gate;
        ~Cleanup() {
            const auto close_observed = [](std::atomic<int>& observed) {
                const int fd = observed.exchange(-1, std::memory_order_acq_rel);
                if (fd >= 0) {
                    (void)::shutdown(fd, SHUT_RDWR);
                    (void)::close(fd);
                }
            };
            {
                std::lock_guard lock(f_gate_mutex);
                release_f_gate = true;
            }
            f_gate_cv.notify_all();
            stop_heartbeat.store(true, std::memory_order_release);
            close_observed(observed_f_fd);
            close_observed(observed_c_fd);
            c_context.stop();
            f_context.stop();
            if (c_thread.joinable()) c_thread.join();
            if (f_thread.joinable()) f_thread.join();
            // Connector and accept handlers can publish their duplicate FDs
            // until the contexts have stopped and both threads have joined.
            close_observed(observed_f_fd);
            close_observed(observed_c_fd);
        }
    } cleanup{c_context, f_context, c_thread, f_thread, stop_heartbeat,
              observed_c_fd, observed_f_fd, f_gate_mutex, f_gate_cv,
              release_f_gate};

    // Install cleanup before either event loop thread can own a descriptor or
    // block in the deliberate F-side gate.
    f_thread = std::thread([&] { f_context.run(); });
    c_thread = std::thread([&] { c_context.run(); });

    auto first = asio::co_spawn(c_context,
        sender->transfer_p51_route(
            armed[0], kPhysicalGeneration, connector,
            PrepareRequestKey{3, 31001}, deadline.as_steady_time_point(), input[0]),
        asio::use_future);
    const auto wait_for_progress = [&](auto predicate,
                                       std::chrono::steady_clock::duration budget) {
        std::unique_lock lock(progress_mutex);
        return progress_cv.wait_for(lock, budget, predicate);
    };
    CHECK(wait_for_progress([&] {
        return complete_bundles.load(std::memory_order_acquire) == 1;
    }, std::chrono::seconds(5)));
    {
        std::unique_lock lock(f_gate_mutex);
        CHECK(f_gate_cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return f_gate_entered.load(std::memory_order_acquire);
        }));
    }
    CHECK(!first_receipt_validated.load(std::memory_order_acquire));
    CHECK(first.wait_for(std::chrono::milliseconds(0)) ==
          std::future_status::timeout);
    CHECK(commit_count.load(std::memory_order_acquire) == 1);

    auto second = asio::co_spawn(c_context,
        sender->transfer_p51_route(
            armed[1], kPhysicalGeneration, connector,
            PrepareRequestKey{3, 31002}, deadline.as_steady_time_point(), input[1]),
        asio::use_future);
    const uint64_t heartbeat_before = heartbeat_ticks.load(std::memory_order_acquire);
    const auto blocked_deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(5);
    int queued_bytes = 0;
    int effective_send_buffer = 0;
    bool socket_not_writable = false;
    bool kernel_backpressure_witness = false;
    while (std::chrono::steady_clock::now() < blocked_deadline) {
        const int fd = observed_c_fd.load(std::memory_order_acquire);
        if (fd >= 0) {
            socklen_t option_size = sizeof(effective_send_buffer);
            (void)::getsockopt(fd, SOL_SOCKET, SO_SNDBUF,
                               &effective_send_buffer, &option_size);
            pollfd descriptor{fd, POLLOUT, 0};
            const int ready = ::poll(&descriptor, 1, 0);
            socket_not_writable = (ready == 0 ||
                (ready > 0 && (descriptor.revents & POLLOUT) == 0)) &&
                (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) == 0;
            if (::ioctl(fd, SIOCOUTQ, &queued_bytes) == 0 &&
                socket_not_writable && effective_send_buffer > 0 &&
                queued_bytes >= std::max(4096, effective_send_buffer / 2) &&
                complete_bundles.load(std::memory_order_acquire) == 1 &&
                second.wait_for(std::chrono::milliseconds(0)) ==
                    std::future_status::timeout &&
                heartbeat_ticks.load(std::memory_order_acquire) > heartbeat_before) {
                kernel_backpressure_witness = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(kernel_backpressure_witness);
    CHECK(heartbeat_ticks.load(std::memory_order_acquire) > heartbeat_before);
    // The peer gate and receipt reader are held and the second C write is
    // demonstrably blocked; no physical-retirement event is valid yet.
    CHECK(terminal_intervals.load(std::memory_order_acquire) == 0);
    std::cerr << "P51_D16_BLOCKED_WRITE profile=" << profile_name
              << " outq=" << queued_bytes << " sndbuf=" << effective_send_buffer
              << " not_writable=" << socket_not_writable
              << " receipt1=held heartbeat=" << heartbeat_ticks.load()
              << " bundles_completed=" << complete_bundles.load() << "\n";

    if (retire_while_reader_and_writer_held) {
        std::promise<void> retirement_posted;
        auto retirement_done = retirement_posted.get_future();
        asio::post(c_context, [&sender, &retirement_posted,
                               &terminal_count_during_retire,
                               &terminal_intervals] {
            sender->retire_for_replacement();
            sender->retire_for_replacement();
            terminal_count_during_retire.store(
                static_cast<int>(terminal_intervals.load(
                    std::memory_order_acquire)), std::memory_order_release);
            retirement_posted.set_value();
        });
        CHECK(retirement_done.wait_for(std::chrono::seconds(2)) ==
              std::future_status::ready);
        // The stop request executes while this context is still inside the
        // held reader/writer state; their completion handlers cannot run
        // until this callback returns.
        CHECK(terminal_count_during_retire.load(std::memory_order_acquire) == 0);
        hold_receipt_reader.store(false, std::memory_order_release);
        progress_cv.notify_all();
    } else {
        hold_receipt_reader.store(false, std::memory_order_release);
        progress_cv.notify_all();
        CHECK(wait_for_progress([&] {
            return first_receipt_validated.load(std::memory_order_acquire);
        }, std::chrono::seconds(2)));
        CHECK(complete_bundles.load(std::memory_order_acquire) == 1);
        CHECK(second.wait_for(std::chrono::milliseconds(0)) ==
              std::future_status::timeout);
        CHECK(heartbeat_ticks.load(std::memory_order_acquire) > heartbeat_before);
        {
            const int fd = observed_c_fd.load(std::memory_order_acquire);
            CHECK(fd >= 0);
            pollfd descriptor{fd, POLLOUT, 0};
            const int ready = ::poll(&descriptor, 1, 0);
            int after_receipt_queue = 0;
            CHECK(::ioctl(fd, SIOCOUTQ, &after_receipt_queue) == 0);
            const bool still_blocked = (ready == 0 ||
                (ready > 0 && (descriptor.revents & POLLOUT) == 0)) &&
                (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) == 0;
            CHECK(still_blocked);
            CHECK(after_receipt_queue >= std::max(4096, effective_send_buffer / 2));
            queued_bytes = after_receipt_queue;
        }
        std::cerr << "P51_D16_READER_PROGRESS profile=" << profile_name
                  << " receipt1=validated still_blocked=1 outq=" << queued_bytes
                  << " heartbeat=" << heartbeat_ticks.load()
                  << " second_bundle_complete=0\n";
    }

    const int peer_fd = observed_f_fd.exchange(-1, std::memory_order_acq_rel);
    CHECK(peer_fd >= 0);
    CHECK(::shutdown(peer_fd, SHUT_RDWR) == 0);
    (void)::close(peer_fd);
    {
        std::lock_guard lock(f_gate_mutex);
        release_f_gate = true;
    }
    f_gate_cv.notify_all();
    if (!retire_while_reader_and_writer_held) {
        std::promise<void> retirement_posted;
        auto retirement_done = retirement_posted.get_future();
        asio::post(c_context, [&sender, &retirement_posted] {
            sender->retire_for_replacement();
            sender->retire_for_replacement();
            retirement_posted.set_value();
        });
        CHECK(retirement_done.wait_for(std::chrono::seconds(2)) ==
              std::future_status::ready);
    }
    CHECK(second.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    CHECK(second.get().status != ZstdSourceTransferStatus::Committed);
    CHECK(first.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    const ZstdSourceTransferResult first_result = first.get();
    if (retire_while_reader_and_writer_held)
        CHECK(first_result.status != ZstdSourceTransferStatus::Committed);
    else
        CHECK(first_result.status == ZstdSourceTransferStatus::Committed);
    CHECK(server_future.wait_for(std::chrono::seconds(2)) ==
          std::future_status::ready);
    CHECK(server_future.get().status == ServerRunStatus::Disconnected);
    {
        std::unique_lock lock(interval_mutex);
        CHECK(interval_cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return terminal_intervals.load(std::memory_order_acquire) == 1;
        }));
    }
    CHECK(terminal_intervals.load(std::memory_order_acquire) == 1);
    CHECK(observer_delivery_valid.load(std::memory_order_acquire));
    {
        std::lock_guard lock(interval_mutex);
        CHECK(!observed_intervals.empty());
        CHECK(observed_intervals.back().end ==
              R2WireIntervalEnd::PhysicalLinkRetired);
        CHECK(observed_intervals.back().link.c_store_guid == c_guid);
        CHECK(observed_intervals.back().link.f_store_guid == f_guid);
        CHECK(observed_intervals.back().link.logical_link_id == relationship_id);
        CHECK(observed_intervals.back().link.relationship_epoch ==
              armed[0].relationship_epoch);
        CHECK(observed_intervals.back().link.physical_link_generation ==
              kPhysicalGeneration);
        uint64_t prior_sequence = 0;
        for (const R2WireControlSnapshot& interval : observed_intervals) {
            CHECK(interval.interval_sequence > prior_sequence);
            prior_sequence = interval.interval_sequence;
        }
    }
    CHECK(commit_count.load(std::memory_order_acquire) == 1);
    CHECK(input_mismatches.load(std::memory_order_relaxed) == 0);
    CHECK(connector_calls.load(std::memory_order_acquire) <= 2);
    stop_heartbeat.store(true, std::memory_order_release);
    c_work.reset();
    CHECK(heartbeat.wait_for(std::chrono::seconds(1)) ==
          std::future_status::ready);
    if (c_thread.joinable()) c_thread.join();
    f_context.stop();
    if (f_thread.joinable()) f_thread.join();
    std::cerr << "P51_D16_BACKPRESSURE_ACK_SHUTDOWN profile=" << profile_name
              << " first_committed="
              << (!retire_while_reader_and_writer_held)
              << " blocked_write=1 receipt_reader_progress="
              << (!retire_while_reader_and_writer_held)
              << " held_io_retirement=" << retire_while_reader_and_writer_held
              << " bounded=1 PASS\n";
}

void test_p51_sender_writer_backpressure_ack_and_shutdown() {
    for (const ProfileId profile : {ProfileId::ZSTD_TU, ProfileId::P29V1,
                                    ProfileId::ZSTD_ROUTE})
        run_p51_sender_writer_backpressure_case(profile);
    run_p51_sender_writer_backpressure_case(ProfileId::ZSTD_TU, true);
}
#endif

void run_p51_sender_recovery_case(bool repeat_interrupted_materialization,
                                  bool retire_during_recovery = false,
                                  bool expire_during_recovery = false,
                                  bool fail_interval_observer = false) {
    const bool diagnostics_on = p50_diagnostics_enabled_for_test();
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
    std::optional<JobBind> retained_binding;
    std::atomic<unsigned> materialized{0};
    std::atomic<unsigned> consumed{0};
    std::atomic<unsigned> resets{0};
    std::atomic<unsigned> input_selectors{0};
    std::atomic<unsigned> acknowledged{0};
    std::atomic<bool> hold_ack_pump{true};
    std::atomic<unsigned> interval_observer_calls{0};
    std::mutex interval_observer_mutex;
    std::vector<R2WireControlSnapshot> observed_intervals;
    std::condition_variable ack_observed_cv;
    std::mutex ack_observed_mutex;
    std::mutex recovery_gate_mutex;
    std::condition_variable recovery_gate_cv;
    bool recovery_waiting = false;
    bool release_recovery = false;
    std::optional<ResetRequest> retained_reset;
    uint64_t recovery_floor_a = 0;
    uint64_t recovery_prefix_p = 0;
    Digest128 recovery_witness_digest{};
    P50ServerEndpointConfig server_config;
    CompletionLog f_socket_observations;
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
            lease.acknowledged_prefix_q = retained_reset
                ? retained_reset->settled_prefix_k : 0;
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
        {
            std::lock_guard lock(commit_mutex);
            retained_binding = binding;
        }
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
        [&, relationship_id](const LinkHello& hello, const CommitAck& ack) {
        const bool valid = hello.relationship_id == relationship_id &&
               ack.relationship_id == relationship_id &&
               ack.relationship_epoch == hello.relationship_epoch &&
               ack.physical_link_generation ==
                   hello.physical_link_generation &&
               ack.contiguous_verified_ordinal == 1;
        if (valid) {
            acknowledged.fetch_add(1, std::memory_order_release);
            ack_observed_cv.notify_all();
        }
        return valid;
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
        std::optional<JobBind> saved_binding;
        {
            std::lock_guard lock(commit_mutex);
            commit = retained_commit;
            saved_binding = retained_binding;
        }
        const bool committed_before_recovery =
            !repeat_interrupted_materialization;
        if (hello.start_mode != LinkStartMode::Reconnect ||
            begin.relationship_id != relationship_id || begin.witness_count != 1 ||
            begin.verified_floor_a != 0 || begin.prepared_prefix_p != 1 ||
            witnesses.size() != 1 || end.witness_count != 1 ||
            witnesses.front().relationship_ordinal != 1 ||
            !saved_binding || witnesses.front().binding != *saved_binding ||
            witnesses.front().inner.tu_seq.value != 0 ||
            witnesses.front().inner.raw_digest != icecc::digest128(source) ||
            (committed_before_recovery && !commit) ||
            (!committed_before_recovery && commit.has_value()))
            return std::nullopt;
        recovery_floor_a = begin.verified_floor_a;
        recovery_prefix_p = begin.prepared_prefix_p;
        recovery_witness_digest =
            compute_r2_recovery_witness_digest(begin, witnesses);
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
        ResetAck ack{request,
                     initial_route_digest(c_guid, request.new_history_nonce),
                     RelSeq{}};
        ack.recovery_verified_floor_a = recovery_floor_a;
        ack.recovery_prepared_prefix_p = recovery_prefix_p;
        ack.recovery_witness_digest = recovery_witness_digest;
        ack.unavailable_suffix_mask = 0;
        return ack;
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

    P50ServerEndpoint server(f_guid, server_caps, &f_socket_observations, nullptr,
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
    sender_config.r2_interval_observer = [&](const R2WireControlSnapshot& interval) {
        const unsigned call = interval_observer_calls.fetch_add(
            1, std::memory_order_acq_rel);
        if (fail_interval_observer && call == 0) return false;
        if (fail_interval_observer && call == 1)
            throw std::runtime_error("injected interval observer failure");
        std::lock_guard lock(interval_observer_mutex);
        observed_intervals.push_back(interval);
        return true;
    };
    sender_config.hold_r2_ack_pump_for_test = [&] {
        return repeat_interrupted_materialization &&
            hold_ack_pump.load(std::memory_order_acquire);
    };
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
    asio::io_context c_context;
    // PendingReceipt owns Asio timers bound to c_context, so destroy sender
    // before c_context tears down its timer service.
    auto sender = std::make_shared<P50ZstdSourceSender>(
        authority, route, PrepareRequestKey{3, 901}, sender_config);
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
        if (diagnostics_on) {
            CHECK(expired.r2_wire_accounting.has_value());
            CHECK(expired.r2_wire_accounting->valid);
            CHECK(expired.r2_wire_accounting->bundle_attempts == 1);
            CHECK(expired.r2_wire_accounting->replay_attempts == 0);
            CHECK(expired.r2_wire_accounting->c_to_f_bundle_bytes != 0);
            CHECK(expired.r2_wire_accounting_key.has_value());
            CHECK(expired.r2_wire_accounting_key->c_store_guid == c_guid);
            CHECK(expired.r2_wire_accounting_key->f_store_guid == f_guid);
            CHECK(expired.r2_wire_accounting_key->logical_link_id ==
                  relationship_id);
            CHECK(expired.r2_wire_accounting_key->tu_seq == TuSeq{0});
            CHECK(expired.r2_wire_accounting_key->raw_digest ==
                  icecc::digest128(source));
            CHECK(expired.r2_link_intervals_external);
            CHECK(expired.r2_link_intervals.empty());
        } else {
            CHECK(!expired.r2_wire_accounting.has_value());
            CHECK(!expired.r2_wire_accounting_key.has_value());
        }
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
    if (!retire_during_recovery && !expire_during_recovery) {
        const ZstdSourceTransferResult duplicate = asio::co_spawn(
            c_context,
            sender->transfer_p51_route(
                armed, 41, connector, PrepareRequestKey{3, 901},
                source_deadline, source),
            asio::use_future).get();
        CHECK(duplicate.status == ZstdSourceTransferStatus::Committed);
        CHECK(duplicate.r2_accounting_reference);
        CHECK(!duplicate.r2_wire_accounting.has_value());
        if (diagnostics_on) {
            CHECK(duplicate.r2_wire_accounting_key.has_value());
            CHECK(duplicate.r2_wire_accounting_key->c_store_guid == c_guid);
            CHECK(duplicate.r2_wire_accounting_key->f_store_guid == f_guid);
            CHECK(duplicate.r2_wire_accounting_key->logical_link_id ==
                  relationship_id);
            CHECK(duplicate.r2_wire_accounting_key->tu_seq == TuSeq{0});
            CHECK(duplicate.r2_wire_accounting_key->raw_digest ==
                  icecc::digest128(source));
        } else {
            CHECK(!duplicate.r2_wire_accounting_key.has_value());
        }
    }
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
    if (diagnostics_on) {
        CHECK(recovered.r2_wire_accounting.has_value());
        CHECK(recovered.r2_wire_accounting->valid);
        CHECK(recovered.r2_wire_accounting->bundle_attempts ==
              (repeat_interrupted_materialization ? 3U : 1U));
        CHECK(recovered.r2_wire_accounting->replay_attempts ==
              (repeat_interrupted_materialization ? 2U : 0U));
        CHECK(recovered.r2_wire_accounting->c_to_f_bundle_bytes != 0);
        CHECK(recovered.r2_wire_accounting_key.has_value());
        CHECK(recovered.r2_wire_accounting_key->c_store_guid == c_guid);
        CHECK(recovered.r2_wire_accounting_key->f_store_guid == f_guid);
        CHECK(recovered.r2_wire_accounting_key->logical_link_id ==
              relationship_id);
        CHECK(recovered.r2_wire_accounting_key->tu_seq == TuSeq{0});
        CHECK(recovered.r2_wire_accounting_key->raw_digest ==
              icecc::digest128(source));
        if (repeat_interrupted_materialization)
            CHECK(recovered.r2_wire_accounting->f_to_c_receipt_bytes != 0);
        else
            CHECK(recovered.r2_wire_accounting->f_to_c_receipt_bytes == 0);
        CHECK(recovered.r2_link_intervals_external);
        CHECK(recovered.r2_link_intervals.empty());
        CHECK(recovered.r2_link_intervals_valid != fail_interval_observer);
        if (fail_interval_observer)
            CHECK(interval_observer_calls.load(std::memory_order_acquire) >= 2);
    } else {
        CHECK(!recovered.r2_wire_accounting.has_value());
        CHECK(!recovered.r2_link_intervals_external);
        CHECK(!recovered.r2_link_intervals_valid);
        std::lock_guard lock(interval_observer_mutex);
        CHECK(observed_intervals.empty());
    }
    if (repeat_interrupted_materialization) {
        CHECK(acknowledged.load(std::memory_order_acquire) == 0);
        if (diagnostics_on) {
            std::lock_guard lock(interval_observer_mutex);
            CHECK(std::none_of(observed_intervals.begin(),
                               observed_intervals.end(),
                [](const R2WireControlSnapshot& interval) {
                    return interval.end ==
                               R2WireIntervalEnd::DrainedAckCheckpoint &&
                           interval.drained_ack_prefix >= 1;
                }));
        }
    }
    hold_ack_pump.store(false, std::memory_order_release);
    if (repeat_interrupted_materialization) {
        std::unique_lock lock(ack_observed_mutex);
        CHECK(ack_observed_cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return acknowledged.load(std::memory_order_acquire) == 1;
        }));
    } else {
        CHECK(acknowledged.load(std::memory_order_acquire) == 0);
    }
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
    std::vector<R2WireControlSnapshot> link_intervals;
    if (diagnostics_on && !fail_interval_observer) {
        std::lock_guard lock(interval_observer_mutex);
        link_intervals = observed_intervals;
    }
    if (diagnostics_on && !fail_interval_observer) {
    uint64_t c_to_f_interval_bytes = 0;
    uint64_t f_to_c_interval_bytes = 0;
    uint64_t prior_interval_sequence = 0;
    uint64_t final_ack_prefix = 0;
    uint64_t recovery_confirmed_prefix = 0;
    for (const R2WireControlSnapshot& interval : link_intervals) {
        CHECK(interval.valid);
        CHECK(interval.link.c_store_guid == c_guid);
        CHECK(interval.link.f_store_guid == f_guid);
        CHECK(interval.link.logical_link_id == relationship_id);
        CHECK(interval.interval_sequence > prior_interval_sequence);
        prior_interval_sequence = interval.interval_sequence;
        if (interval.end == R2WireIntervalEnd::DrainedAckCheckpoint)
            final_ack_prefix = std::max(final_ack_prefix,
                                        interval.drained_ack_prefix);
        else if (interval.end == R2WireIntervalEnd::RecoveryConfirmed) {
            CHECK(interval.drained_ack_prefix == 0);
            recovery_confirmed_prefix = std::max(
                recovery_confirmed_prefix,
                interval.recovery_confirmed_prefix);
        } else if (interval.end == R2WireIntervalEnd::WindowPressure) {
            CHECK(interval.drained_ack_prefix == 0);
            CHECK(interval.recovery_confirmed_prefix == 0);
        }
        uint64_t classified_c_to_f = interval.shared_c_to_f_bytes;
        uint64_t classified_f_to_c = interval.shared_f_to_c_bytes;
        for (const R2WireJobInterval& job : interval.jobs) {
            CHECK(job.valid);
            CHECK(job.key.c_store_guid == c_guid);
            CHECK(job.key.f_store_guid == f_guid);
            CHECK(job.key.logical_link_id == relationship_id);
            CHECK(job.key.tu_seq == TuSeq{0});
            CHECK(job.key.raw_digest == icecc::digest128(source));
            CHECK(job.c_to_f_bundle_bytes <=
                  std::numeric_limits<uint64_t>::max() - classified_c_to_f);
            CHECK(job.f_to_c_receipt_bytes <=
                  std::numeric_limits<uint64_t>::max() - classified_f_to_c);
            classified_c_to_f += job.c_to_f_bundle_bytes;
            classified_f_to_c += job.f_to_c_receipt_bytes;
        }
        CHECK(classified_c_to_f == interval.total_c_to_f_bytes);
        CHECK(classified_f_to_c == interval.total_f_to_c_bytes);
        CHECK(interval.total_c_to_f_bytes <=
              std::numeric_limits<uint64_t>::max() - c_to_f_interval_bytes);
        CHECK(interval.total_f_to_c_bytes <=
              std::numeric_limits<uint64_t>::max() - f_to_c_interval_bytes);
        c_to_f_interval_bytes += interval.total_c_to_f_bytes;
        f_to_c_interval_bytes += interval.total_f_to_c_bytes;
    }
    uint64_t f_observed_c_to_f = 0;
    uint64_t f_observed_f_to_c = 0;
    std::map<std::pair<uint64_t, uint64_t>, std::pair<uint64_t, uint64_t>>
        c_bytes_by_link, f_bytes_by_link;
    for (const R2WireControlSnapshot& interval : link_intervals) {
        auto& totals = c_bytes_by_link[{interval.link.relationship_epoch,
                                        interval.link.physical_link_generation}];
        totals.first += interval.total_c_to_f_bytes;
        totals.second += interval.total_f_to_c_bytes;
    }
    for (const AsyncCompletion& completion : f_socket_observations.completions()) {
        if (!completion.stamp.r2_traffic ||
            completion.stamp.actor != ActorSide::F)
            continue;
        uint64_t* total = nullptr;
        if (completion.stamp.operation == AsyncOperationKind::ReadHeader ||
            completion.stamp.operation == AsyncOperationKind::ReadPayload)
            total = &f_observed_c_to_f;
        else if (completion.stamp.operation == AsyncOperationKind::WriteFragment)
            total = &f_observed_f_to_c;
        if (!total)
            continue;
        CHECK(completion.transferred_bytes <=
              std::numeric_limits<uint64_t>::max() - *total);
        *total += completion.transferred_bytes;
        auto& link_totals = f_bytes_by_link[{completion.stamp.relationship_epoch,
                                             completion.stamp.physical_link_generation}];
        uint64_t& link_total = total == &f_observed_c_to_f
            ? link_totals.first : link_totals.second;
        CHECK(completion.transferred_bytes <=
              std::numeric_limits<uint64_t>::max() - link_total);
        link_total += completion.transferred_bytes;
    }
    CHECK(f_socket_observations.valid());
    if (c_to_f_interval_bytes != f_observed_c_to_f ||
        f_to_c_interval_bytes > f_observed_f_to_c) {
        std::cerr << "R2_RECOVERY_CONSERVATION_DIAG repeated_abort="
                  << repeat_interrupted_materialization << " interval="
                  << c_to_f_interval_bytes << '/' << f_to_c_interval_bytes
                  << " peer=" << f_observed_c_to_f << '/'
                  << f_observed_f_to_c << "\n";
        for (const auto& [identity, totals] : c_bytes_by_link)
            std::cerr << "R2_RECOVERY_C_LINK_DIAG epoch=" << identity.first
                      << " generation=" << identity.second << " bytes="
                      << totals.first << '/' << totals.second << "\n";
        for (const auto& [identity, totals] : f_bytes_by_link)
            std::cerr << "R2_RECOVERY_F_LINK_DIAG epoch=" << identity.first
                      << " generation=" << identity.second << " bytes="
                      << totals.first << '/' << totals.second << "\n";
        for (const auto& interval : link_intervals) {
            std::cerr << "R2_RECOVERY_INTERVAL_DIAG seq="
                      << interval.interval_sequence << " end="
                      << static_cast<unsigned>(interval.end) << " epoch="
                      << interval.link.relationship_epoch << " generation="
                      << interval.link.physical_link_generation << " bytes="
                      << interval.total_c_to_f_bytes << '/'
                      << interval.total_f_to_c_bytes << " shared="
                      << interval.shared_c_to_f_bytes << '/'
                      << interval.shared_f_to_c_bytes << " jobs="
                      << interval.jobs.size() << "\n";
            for (const auto& job : interval.jobs)
                std::cerr << "R2_RECOVERY_JOB_DIAG tu="
                          << job.key.tu_seq.value << " epoch="
                          << job.link.relationship_epoch << " generation="
                          << job.link.physical_link_generation << " attempts="
                          << job.bundle_attempts << '/'
                          << job.replay_attempts << " bytes="
                          << job.c_to_f_bundle_bytes << '/'
                          << job.f_to_c_receipt_bytes << "\n";
        }
    }
    CHECK(c_to_f_interval_bytes == f_observed_c_to_f);
    // The fixture deliberately drops several F writes before C can consume
    // them, so C-side reads are a lower bound on F-side writes, not equality.
    CHECK(f_to_c_interval_bytes <= f_observed_f_to_c);
    CHECK(final_ack_prefix == (repeat_interrupted_materialization ? 1U : 0U));
    CHECK(recovery_confirmed_prefix ==
          (repeat_interrupted_materialization ? 0U : 1U));
    std::cerr << "P51_SENDER_R2_RECOVERY_ACCOUNTING repeated_abort="
              << (repeat_interrupted_materialization ? 1 : 0)
              << " attempts=" << recovered.r2_wire_accounting->bundle_attempts
              << " replay_attempts="
              << recovered.r2_wire_accounting->replay_attempts
              << " intervals=" << link_intervals.size()
              << " final_ack_prefix=" << final_ack_prefix
              << " observed_bytes=" << f_observed_c_to_f << '/'
              << f_observed_f_to_c << " PASS\n";
    } else if (diagnostics_on) {
        // The transfer and positive commit remain valid, but a failed
        // diagnostic callback makes interval coverage fail closed.
        CHECK(recovered.r2_wire_accounting.has_value());
        CHECK(recovered.r2_wire_accounting->valid);
        CHECK(recovered.r2_link_intervals_external);
        CHECK(!recovered.r2_link_intervals_valid);
        std::cerr << "P51_SENDER_R2_OBSERVER_FAILURE fail_closed PASS\n";
    } else {
        CHECK(link_intervals.empty());
        std::cerr << "P51_SENDER_R2_RECOVERY_ACCOUNTING_DISABLED PASS\n";
    }
    std::cerr << "P51_SENDER_RECOVERY repeated_abort="
              << (repeat_interrupted_materialization ? "true" : "false")
              << " PASS\n";
}

void test_p51_sender_recovers_lost_commit_reply_after_connector_failure() {
    run_p51_sender_recovery_case(false);
    run_p51_sender_recovery_case(true);
}

void test_p51_sender_r2_observer_failure_is_fail_closed() {
    ScopedP50Diagnostics diagnostics;
    run_p51_sender_recovery_case(false, false, false, true);
}

void run_p51_sender_shared_failure_case(size_t kJobs, ProfileId profile,
                                        bool repeat_recovery_loss = false,
                                        bool retire_after_positive_receipt = false,
                                        bool expire_after_positive_receipt = false,
                                        bool reject_stale_reconnect = false,
                                        bool retire_during_retry_wait = false,
                                        bool close_reconnect_after_hello = false,
                                        bool reject_after_positive_receipt = false,
                                        bool post_reset_offer_probe = false,
                                        bool future_offer_during_recovery = false,
                                        bool lose_reset_confirm = false,
                                        bool lose_reset_confirm_echo = false,
                                        bool mismatch_reset_confirm_echo = false,
                                        bool change_reset_ack_on_replay = false,
                                        RecoverResponseLoss recover_response_loss =
                                            RecoverResponseLoss::None) {
    const bool lose_recover_response =
        recover_response_loss == RecoverResponseLoss::FirstResponse;
    CHECK(kJobs >= 1 && kJobs <= 30);
    CHECK(!post_reset_offer_probe || kJobs == 2);
    CHECK(!future_offer_during_recovery ||
          (kJobs == 2 && repeat_recovery_loss && !post_reset_offer_probe));
    CHECK(!(lose_reset_confirm && lose_reset_confirm_echo));
    CHECK(!(lose_reset_confirm || lose_reset_confirm_echo) ||
          (kJobs == 2 && !repeat_recovery_loss && !post_reset_offer_probe &&
           !future_offer_during_recovery));
    CHECK(!mismatch_reset_confirm_echo ||
          (kJobs == 2 && !repeat_recovery_loss && !post_reset_offer_probe &&
           !future_offer_during_recovery && !lose_reset_confirm &&
           !lose_reset_confirm_echo));
    CHECK(!change_reset_ack_on_replay ||
          (kJobs == 2 && !repeat_recovery_loss && !post_reset_offer_probe &&
           !future_offer_during_recovery && !lose_reset_confirm &&
           !lose_reset_confirm_echo && !mismatch_reset_confirm_echo));
    CHECK(!lose_recover_response ||
          ((kJobs == 2 || kJobs == 30) && !repeat_recovery_loss &&
           !post_reset_offer_probe &&
           !future_offer_during_recovery && !lose_reset_confirm &&
           !lose_reset_confirm_echo && !mismatch_reset_confirm_echo &&
           !change_reset_ack_on_replay));
    const size_t total_jobs = kJobs +
        ((post_reset_offer_probe || future_offer_during_recovery) ? 1 : 0);
    const uint32_t kWindow = static_cast<uint32_t>(kJobs);
    const char* const profile_name = profile == ProfileId::P29V1 ? "P29V1"
                                    : profile == ProfileId::ZSTD_ROUTE ? "ZSTD_ROUTE"
                                    : "ZSTD_TU";
    const auto [c_guid, f_guid] = sender_r2_store_guids();
    const Id128 relationship_id = Id128::from_u64(0x5102);
    std::vector<P51SourceArmedFields> armed(total_jobs);
    std::vector<std::vector<uint8_t>> input(total_jobs);
    std::vector<JobBind> expected_bindings(total_jobs);
    for (size_t index = 0; index != total_jobs; ++index) {
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
        JobBind expected;
        expected.reservation_id = Id128{armed[index].reservation_id};
        expected.physical_link_generation = 41;
        expected.relationship_ordinal = index + 1;
        expected.wire_job_id = armed[index].arm.source.wire_job_id;
        expected.assignment_epoch = armed[index].arm.source.assignment_epoch;
        expected.assignment_nonce = armed[index].arm.source.assignment_nonce;
        expected.logical_job = armed[index].arm.source.logical_job;
        expected.compiler_attempt = armed[index].arm.source.compiler_attempt;
        expected.source_request_id = armed[index].arm.source.source_request_id;
        expected.tu_seq = TuSeq{index};
        expected.profile = profile;
        expected.raw_bytes = input[index].size();
        expected.raw_digest = icecc::digest128(input[index]);
        expected_bindings[index] = expected;
    }
    if (future_offer_during_recovery) {
        // The F reset will verify epoch+1; this offer is deliberately one
        // epoch newer, so only this request should be rejected after the
        // shared recovery has established authoritative state.
        armed[kJobs].relationship_epoch = armed[0].relationship_epoch + 2;
        CHECK(armed[kJobs].valid());
    }
    const PrepareRequestKey future_offer_request{3, 921 + kJobs};
    const auto clock = sidecar::process_monotonic_clock_identity();
    // Keep the source lease's absolute deadline at its 60-second ceiling for
    // the W30 sanitizer case; W2 recovery checks retain their tighter budget.
    const auto deadline = sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
        std::chrono::steady_clock::now() +
             ((lose_recover_response && kJobs == 30)
                  ? std::chrono::seconds(60)
             : (lose_reset_confirm || lose_reset_confirm_echo ||
                lose_recover_response || mismatch_reset_confirm_echo ||
                change_reset_ack_on_replay)
                 ? std::chrono::seconds(10)
             : reject_after_positive_receipt
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
    std::atomic<bool> positive_receipt_validated{false};
    std::atomic<bool> server_reset_complete{false};
    std::atomic<bool> reset_ack_loss_complete{false};
    std::atomic<unsigned> reset_confirms_received{0};
    std::atomic<bool> drop_first_reset_confirm{lose_reset_confirm};
    std::atomic<bool> exact_reset_echo_waiting{false};
    std::atomic<bool> release_exact_reset_echo{false};
    std::atomic<unsigned> mismatched_echoes_sent{0};
    std::atomic<unsigned> changed_reset_acks_sent{0};
    std::atomic<unsigned> recovery_retry_waiters_seen{0};
    std::atomic<bool> future_offer_entered_recovery{false};
    std::atomic<bool> retry_wait_registered{false};
    std::atomic<int64_t> retry_wait_registered_ns{0};
    std::atomic<unsigned> retry_wait_count{0};
    std::atomic<int64_t> retry_wait_remaining_ns{0};
    std::atomic<unsigned> input_selections{0};
    std::atomic<unsigned> input_mismatches{0};
    std::atomic<unsigned> materialized{0};
    std::atomic<unsigned> acknowledged{0};
    std::mutex r2_interval_observer_mutex;
    std::vector<R2WireControlSnapshot> r2_observed_intervals;
    std::vector<std::atomic<unsigned>> binds(total_jobs);
    std::vector<std::atomic<unsigned>> commits(total_jobs);
    for (size_t index = 0; index != total_jobs; ++index) {
        binds[index].store(0, std::memory_order_relaxed);
        commits[index].store(0, std::memory_order_relaxed);
    }
    std::mutex commit_mutex;
    std::vector<std::optional<R2TxCommit>> retained_commits(total_jobs);
    std::vector<std::optional<JobBind>> retained_bindings(total_jobs);
    std::optional<ResetRequest> retained_reset;
    std::vector<ResetRequest> reset_requests;
    std::vector<ResetRequest> reset_validation_requests;
    std::optional<ResetAck> retained_reset_ack_snapshot;
    std::vector<ResetConfirm> reset_confirm_messages;
    struct RecoveryReplyEvidence {
        RecoverBegin request;
        RecoverEnd request_end;
        Digest128 witness_digest{};
        ReceiptsEnd end;
        std::vector<ReceiptRow> rows;
    };
    std::vector<RecoveryReplyEvidence> recovery_reply_evidence;
    bool retained_reset_confirmed = false;
    uint64_t committed_prefix_k = 0;
    uint64_t acknowledged_prefix_q = 0;
    std::optional<HistoryNonce> initial_history_nonce;
    uint64_t recovery_floor_a = 0;
    uint64_t recovery_prefix_p = 0;
    Digest128 recovery_witness_digest{};

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
        if (index >= total_jobs || begin.profile != profile ||
            (index < total_jobs && commit.raw_digest != icecc::digest128(input[index])) ||
            (index < total_jobs && bytes.size() != input[index].size()) ||
            (index < total_jobs &&
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
            const bool original_identity = initial_history_nonce &&
                hello.relationship_epoch == armed[0].relationship_epoch &&
                hello.history_nonce == *initial_history_nonce;
            const bool retained_reset_identity =
                (lose_reset_confirm || lose_reset_confirm_echo ||
                 mismatch_reset_confirm_echo || change_reset_ack_on_replay) &&
                retained_reset &&
                hello.relationship_epoch ==
                    retained_reset->new_relationship_epoch &&
                hello.history_nonce == retained_reset->new_history_nonce;
            if (!initial_history_nonce ||
                !(original_identity || retained_reset_identity))
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
            lease.committed_prefix_k = retained_reset
                ? committed_prefix_k : 1;
            lease.acknowledged_prefix_q = retained_reset
                ? acknowledged_prefix_q : 0;
        }
        return lease;
    };
    server_config.consume_p51_job_reservation =
        [&, deadline](const LinkHello& hello, const JobBind& binding)
            -> std::optional<P51SourceJobLease> {
        if (binding.relationship_ordinal == 0 ||
            binding.relationship_ordinal > total_jobs ||
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
            (hello.start_mode != LinkStartMode::Reconnect || index != 1 ||
             (!(lose_recover_response || lose_reset_confirm || lose_reset_confirm_echo ||
                mismatch_reset_confirm_echo || change_reset_ack_on_replay) &&
              prior != 1)))
            return std::nullopt;
        {
            std::lock_guard lock(commit_mutex);
            retained_bindings[index] = binding;
        }
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
            binding.relationship_ordinal > total_jobs ||
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
        committed_prefix_k = std::max<uint64_t>(
            committed_prefix_k, binding.relationship_ordinal);
        materialized.fetch_add(1, std::memory_order_release);
        return true;
    };
    server_config.acknowledge_p51_receipt =
        [&](const LinkHello& hello, const CommitAck& ack) {
        if (hello.relationship_id != relationship_id ||
            ack.relationship_id != relationship_id ||
            ack.relationship_epoch != hello.relationship_epoch ||
            ack.physical_link_generation != hello.physical_link_generation ||
            ack.contiguous_verified_ordinal > total_jobs)
            return false;
        acknowledged.store(static_cast<unsigned>(ack.contiguous_verified_ordinal),
                           std::memory_order_release);
        acknowledged_prefix_q = std::max<uint64_t>(
            acknowledged_prefix_q, ack.contiguous_verified_ordinal);
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
        recovery_floor_a = begin.verified_floor_a;
        recovery_prefix_p = begin.prepared_prefix_p;
        recovery_witness_digest =
            compute_r2_recovery_witness_digest(begin, witnesses);
        std::vector<R2TxCommit> available_commits;
        std::vector<std::optional<JobBind>> available_bindings;
        {
            std::lock_guard lock(commit_mutex);
            for (const auto& commit : retained_commits)
                if (commit) available_commits.push_back(*commit);
            available_bindings = retained_bindings;
        }
        if (lose_recover_response || lose_reset_confirm || lose_reset_confirm_echo ||
            mismatch_reset_confirm_echo || change_reset_ack_on_replay) {
            if (hello.start_mode != LinkStartMode::Reconnect ||
                begin.relationship_id != relationship_id ||
                begin.verified_floor_a > begin.prepared_prefix_p ||
                begin.prepared_prefix_p > total_jobs ||
                begin.witness_count !=
                    begin.prepared_prefix_p - begin.verified_floor_a ||
                begin.witness_count != witnesses.size() ||
                end.witness_count != witnesses.size() ||
                committed_prefix_k < begin.verified_floor_a ||
                committed_prefix_k > begin.prepared_prefix_p) {
                std::fprintf(stderr,
                    "fixture recover range mismatch A=%llu P=%llu count=%u vector=%zu end=%u K=%llu total=%zu mode=%u\n",
                    static_cast<unsigned long long>(begin.verified_floor_a),
                    static_cast<unsigned long long>(begin.prepared_prefix_p),
                    begin.witness_count, witnesses.size(), end.witness_count,
                    static_cast<unsigned long long>(committed_prefix_k),
                    total_jobs, static_cast<unsigned>(hello.start_mode));
                return std::nullopt;
            }
            for (size_t index = 0; index != witnesses.size(); ++index) {
                const auto& witness = witnesses[index];
                const size_t job = static_cast<size_t>(
                    begin.verified_floor_a + index);
                if (job >= total_jobs ||
                    job >= available_bindings.size() ||
                    witness.binding != expected_bindings[job] ||
                    (available_bindings[job] &&
                     witness.binding != *available_bindings[job]) ||
                    witness.relationship_ordinal != job + 1 ||
                    witness.inner.tu_seq.value != job ||
                    witness.inner.profile != profile ||
                    witness.inner.raw_bytes != input[job].size() ||
                    witness.inner.raw_digest != icecc::digest128(input[job])) {
                    std::fprintf(stderr,
                        "fixture recover witness mismatch index=%zu job=%zu has_binding=%d binding_eq=%d ordinal=%llu tu=%llu profile=%u bytes=%llu expected_bytes=%zu\n",
                        index, job,
                        job < available_bindings.size() &&
                            available_bindings[job].has_value(),
                        job < available_bindings.size() &&
                            available_bindings[job] &&
                            witness.binding == *available_bindings[job],
                        static_cast<unsigned long long>(
                            witness.relationship_ordinal),
                        static_cast<unsigned long long>(witness.inner.tu_seq.value),
                        static_cast<unsigned>(witness.inner.profile),
                        static_cast<unsigned long long>(witness.inner.raw_bytes),
                        job < input.size() ? input[job].size() : 0);
                    return std::nullopt;
                }
            }
            P51RecoveryReceiptInterval interval;
            for (uint64_t ordinal = begin.verified_floor_a + 1;
                 ordinal <= committed_prefix_k; ++ordinal) {
                auto commit = std::find_if(
                    available_commits.begin(), available_commits.end(),
                    [ordinal](const R2TxCommit& candidate) {
                        return candidate.relationship_ordinal == ordinal;
                    });
                const size_t witness_index = static_cast<size_t>(
                    ordinal - begin.verified_floor_a - 1);
                if (commit == available_commits.end() ||
                    witness_index >= witnesses.size() ||
                    commit->binding_digest !=
                        witnesses[witness_index].binding_digest ||
                    commit->transaction_digest !=
                        witnesses[witness_index].transaction_digest ||
                    commit->inner.history_nonce !=
                        witnesses[witness_index].inner.history_nonce ||
                    commit->inner.rel_seq !=
                        witnesses[witness_index].inner.rel_seq ||
                    commit->inner.tu_seq !=
                        witnesses[witness_index].inner.tu_seq ||
                    commit->inner.transaction_digest !=
                        witnesses[witness_index].inner.transaction_digest ||
                    commit->inner.raw_digest !=
                        witnesses[witness_index].inner.raw_digest ||
                    commit->inner.post_state_digest != compute_post_state_digest(
                        witnesses[witness_index].inner.pre_state_digest,
                        witnesses[witness_index].inner.history_nonce,
                        witnesses[witness_index].inner.rel_seq,
                        witnesses[witness_index].inner.tu_seq,
                        witnesses[witness_index].inner.transaction_digest))
                    return std::nullopt;
                interval.rows.push_back(ReceiptRow{
                    begin.relationship_id, begin.relationship_epoch,
                    begin.physical_link_generation, begin.operation_id,
                    *commit});
            }
            interval.end = ReceiptsEnd{
                begin.relationship_id, begin.relationship_epoch,
                begin.physical_link_generation, begin.operation_id,
                begin.verified_floor_a, committed_prefix_k,
                acknowledged_prefix_q,
                static_cast<uint32_t>(interval.rows.size())};
            if (lose_recover_response) {
                RecoveryReplyEvidence evidence;
                evidence.request = begin;
                evidence.request_end = end;
                evidence.witness_digest =
                    compute_r2_recovery_witness_digest(begin, witnesses);
                evidence.end = interval.end;
                evidence.rows = interval.rows;
                recovery_reply_evidence.push_back(std::move(evidence));
            }
            return interval;
        }
        std::optional<R2TxCommit> commit = available_commits.empty()
            ? std::nullopt : std::optional<R2TxCommit>{available_commits.front()};
        if (hello.start_mode != LinkStartMode::Reconnect ||
            !initial_history_nonce ||
            hello.history_nonce != *initial_history_nonce ||
            begin.relationship_id != relationship_id ||
            begin.verified_floor_a != 0 || begin.prepared_prefix_p != kJobs ||
            begin.witness_count != kJobs || witnesses.size() != kJobs ||
            end.witness_count != kJobs || !commit ||
            witnesses[0].relationship_ordinal != 1 ||
            commit->binding_digest != witnesses[0].binding_digest ||
            commit->transaction_digest != witnesses[0].transaction_digest ||
            commit->inner.tu_seq != witnesses[0].inner.tu_seq ||
            commit->inner.raw_digest != witnesses[0].inner.raw_digest)
            return std::nullopt;
        for (size_t index = 0; index != kJobs; ++index) {
            if (witnesses[index].relationship_ordinal != index + 1 ||
                witnesses[index].binding != expected_bindings[index] ||
                (index < available_bindings.size() &&
                 available_bindings[index] &&
                 witnesses[index].binding != *available_bindings[index]) ||
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
            begin.verified_floor_a, committed_prefix_k,
            acknowledged_prefix_q,
            static_cast<uint32_t>(interval.rows.size())};
        return interval;
    };
    server_config.validate_p51_reset =
        [&](const LinkHello& hello, const ResetRequest& request)
            -> std::optional<ResetAck> {
        if (lose_recover_response || lose_reset_confirm || lose_reset_confirm_echo ||
            mismatch_reset_confirm_echo || change_reset_ack_on_replay) {
            reset_validation_requests.push_back(request);
            if (request.relationship_id != hello.relationship_id ||
                request.physical_link_generation !=
                    hello.physical_link_generation)
                return std::nullopt;
            const bool duplicate = retained_reset &&
                retained_reset->operation_id == request.operation_id &&
                retained_reset->old_relationship_epoch ==
                    request.old_relationship_epoch &&
                retained_reset->new_relationship_epoch ==
                    request.new_relationship_epoch &&
                retained_reset->old_history_nonce ==
                    request.old_history_nonce &&
                retained_reset->new_history_nonce ==
                    request.new_history_nonce &&
                retained_reset->settled_prefix_k == request.settled_prefix_k;
            const auto active_epoch = retained_reset
                ? retained_reset->new_relationship_epoch
                : armed[0].relationship_epoch;
            const auto active_nonce = retained_reset
                ? retained_reset->new_history_nonce : *initial_history_nonce;
            if (duplicate) {
                // The old reset is a stable logical operation across physical
                // reconnects. The endpoint updates only its envelope's
                // physical generation before replaying the exact request.
            } else if ((retained_reset && !retained_reset_confirmed) ||
                       request.settled_prefix_k != committed_prefix_k ||
                       request.old_relationship_epoch != active_epoch ||
                       request.new_relationship_epoch != active_epoch + 1 ||
                       request.old_history_nonce != active_nonce ||
                       request.new_history_nonce == active_nonce ||
                       request.old_relationship_epoch != hello.relationship_epoch ||
                       request.old_history_nonce != hello.history_nonce) {
                return std::nullopt;
            }
            ResetAck ack{request,
                         initial_route_digest(c_guid,
                                              request.new_history_nonce),
                         RelSeq{}};
            ack.recovery_verified_floor_a = recovery_floor_a;
            ack.recovery_prepared_prefix_p = recovery_prefix_p;
            ack.recovery_witness_digest = recovery_witness_digest;
            ack.unavailable_suffix_mask = 0;
            if (change_reset_ack_on_replay) {
                if (!retained_reset_ack_snapshot) {
                    retained_reset_ack_snapshot = ack;
                } else if (changed_reset_acks_sent.exchange(
                               1, std::memory_order_acq_rel) == 0) {
                    CHECK(ack.recovery_prepared_prefix_p >
                          request.settled_prefix_k);
                    ResetAck changed = ack;
                    changed.unavailable_suffix_mask = 1;
                    return changed;
                } else {
                    ResetAck exact = *retained_reset_ack_snapshot;
                    exact.request.physical_link_generation =
                        request.physical_link_generation;
                    return exact;
                }
            }
            return ack;
        }
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
        ResetAck ack{request,
                     initial_route_digest(c_guid, request.new_history_nonce),
                     RelSeq{}};
        ack.recovery_verified_floor_a = recovery_floor_a;
        ack.recovery_prepared_prefix_p = recovery_prefix_p;
        ack.recovery_witness_digest = recovery_witness_digest;
        ack.unavailable_suffix_mask = 0;
        return ack;
    };
    server_config.commit_p51_reset =
        [&](const LinkHello&, const ResetRequest& request, const ResetAck&) {
        retained_reset = request;
        retained_reset_confirmed = false;
        acknowledged_prefix_q = request.settled_prefix_k;
        reset_requests.push_back(request);
        return true;
    };
    server_config.confirm_p51_reset =
        [&](const LinkHello& hello, const ResetConfirm& confirm) {
        reset_confirms_received.fetch_add(1, std::memory_order_release);
        reset_confirm_messages.push_back(confirm);
        const bool valid = hello.start_mode == LinkStartMode::Reconnect &&
            confirm.relationship_id == Id128{hello.relationship_id} &&
            retained_reset &&
            confirm.operation_id == retained_reset->operation_id &&
            confirm.settled_prefix_k == retained_reset->settled_prefix_k &&
            confirm.new_relationship_epoch ==
                retained_reset->new_relationship_epoch &&
            confirm.new_history_nonce == retained_reset->new_history_nonce;
        if (!valid) return false;
        bool expected = true;
        if (lose_reset_confirm &&
            drop_first_reset_confirm.compare_exchange_strong(
                expected, false, std::memory_order_acq_rel))
            return false; // Decoded after full write, deliberately not applied.
        retained_reset_confirmed = true;
        return true;
    };

    P50ServerEndpoint server(f_guid, server_caps, nullptr, nullptr,
                             std::move(server_config));
    AsyncConnectedFdFactory connector;
    std::future<ZstdSourceTransferResult> future_offer_result;
    asio::io_context f_context;
    tcp::acceptor acceptor(f_context, {asio::ip::address_v4::loopback(), 0});
    asio::io_context c_context;
    // PendingReceipt timers use c_context's executor; sender must destruct
    // before the context destroys that executor's timer service.
    std::shared_ptr<P50ZstdSourceSender> sender;
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
            close_reconnect_after_hello, reject_after_positive_receipt,
            &positive_receipt_validated, &server_reset_complete,
            future_offer_during_recovery ? &reset_ack_loss_complete : nullptr,
            lose_reset_confirm, lose_reset_confirm_echo,
            mismatch_reset_confirm_echo, change_reset_ack_on_replay,
            &mismatched_echoes_sent,
            &exact_reset_echo_waiting, &release_exact_reset_echo,
            recover_response_loss),
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
    sender_config.r2_interval_observer = [&](
        const R2WireControlSnapshot& interval) {
        std::lock_guard lock(r2_interval_observer_mutex);
        r2_observed_intervals.push_back(interval);
        return true;
    };
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
    } else if (reject_after_positive_receipt) {
        sender_config.after_r2_receipt_validated_for_test =
            [&](uint64_t ordinal) {
                if (ordinal != 1) return;
                positive_receipt_validated.store(true,
                                                 std::memory_order_release);
                const auto reset_deadline = std::chrono::steady_clock::now() +
                                            std::chrono::seconds(5);
                while (!server_reset_complete.load(std::memory_order_acquire) &&
                       std::chrono::steady_clock::now() < reset_deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if (!server_reset_complete.load(std::memory_order_acquire))
                    throw std::runtime_error(
                        "F did not reset after C validated positive R2 receipt");
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
    } else if (future_offer_during_recovery) {
        sender_config.before_r2_recovery_for_test =
            [&](PrepareRequestKey request) {
                if (request == future_offer_request)
                    future_offer_entered_recovery.store(
                        true, std::memory_order_release);
            };
        sender_config.after_r2_recovery_waiter_registered_for_test =
            [&](std::chrono::steady_clock::duration) {
                const unsigned waiter = recovery_retry_waiters_seen.fetch_add(
                    1, std::memory_order_acq_rel) + 1;
                if (waiter == 1) {
                    // Enqueue the future offer from the actual retry-wait
                    // transition. It becomes runnable before the registered
                    // retry timer can expire, and must enter the same
                    // recovery-required branch as a second waiter.
                    future_offer_result = asio::co_spawn(
                        c_context,
                        sender->transfer_p51_route(
                            armed[kJobs], 41, connector,
                            future_offer_request,
                            deadline.as_steady_time_point(), input[kJobs]),
                        asio::use_future);
                }
            };
    }
    sender = std::make_shared<P50ZstdSourceSender>(
        authority, route, PrepareRequestKey{3, 921}, sender_config);

    const tcp::endpoint remote = acceptor.local_endpoint();
    connector = [&](auto, auto completion) {
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

    if (mismatch_reset_confirm_echo) {
        const auto wait_until = std::min(
            deadline.as_steady_time_point(),
            std::chrono::steady_clock::now() + std::chrono::seconds(5));
        while (!exact_reset_echo_waiting.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < wait_until)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(exact_reset_echo_waiting.load(std::memory_order_acquire));
        // F has applied the reset and received a replayed confirm, but is
        // withholding the exact echo. Neither caller may report success until
        // C validates that echo rather than the deliberately mismatched one.
        for (auto& result : results)
            CHECK(result.wait_for(std::chrono::milliseconds(0)) ==
                  std::future_status::timeout);
        release_exact_reset_echo.store(true, std::memory_order_release);
    }

    std::vector<ZstdSourceTransferResult> outcomes(kJobs);
    for (size_t index = 0; index != kJobs; ++index) outcomes[index] = results[index].get();
    if (future_offer_during_recovery) {
        CHECK(future_offer_result.wait_until(deadline.as_steady_time_point()) ==
              std::future_status::ready);
        const auto rejected_future = future_offer_result.get();
        CHECK(rejected_future.status == ZstdSourceTransferStatus::InvalidRequest);
        CHECK(!rejected_future.committed_input);
        CHECK(!rejected_future.replacement_required);
        CHECK(!rejected_future.route_local_failure);
        CHECK(connector_calls.load(std::memory_order_acquire) == 3);
        CHECK(commits[kJobs].load(std::memory_order_acquire) == 0);
        CHECK(binds[kJobs].load(std::memory_order_acquire) == 0);
        CHECK(input_selections.load(std::memory_order_acquire) == kJobs);
        CHECK(recovery_retry_waiters_seen.load(std::memory_order_acquire) >= 2);
        CHECK(future_offer_entered_recovery.load(std::memory_order_acquire));
        CHECK(reset_ack_loss_complete.load(std::memory_order_acquire));
        CHECK(retained_reset.has_value());
        CHECK(retained_reset->new_relationship_epoch ==
              armed[0].relationship_epoch + 1);
        std::cerr << "P51_SENDER_RECOVERY_FUTURE_ARM request-local-reject "
                     "old-rows-preserved PASS\n";
    }
    if (reject_after_positive_receipt) {
        CHECK(kJobs == 1 || kJobs == 2);
        CHECK(positive_receipt_validated.load(std::memory_order_acquire));
        if (kJobs == 2) {
            CHECK(outcomes[1].status == ZstdSourceTransferStatus::TerminalError);
            CHECK(outcomes[1].route_local_failure);
            CHECK(!outcomes[1].replacement_required);
            CHECK(outcomes[1].r2_link_rejection.has_value());
            CHECK(outcomes[1].r2_link_rejection->reason ==
                  LinkRejectReason::ReservationMissing);
        }
        CHECK(outcomes[0].status == ZstdSourceTransferStatus::Committed);
        CHECK(outcomes[0].committed_input.has_value());
        CHECK((outcomes[0].committed_input ==
               InputRecordKey{c_guid, TuSeq{0}}));
        CHECK(outcomes[0].raw_digest == icecc::digest128(input[0]));
        if (kJobs == 1) {
            CHECK(outcomes[0].r2_link_rejection.has_value());
            CHECK(outcomes[0].r2_link_rejection->reason ==
                  LinkRejectReason::ReservationMissing);
        }
        CHECK(server_future.wait_for(std::chrono::seconds(5)) ==
              std::future_status::ready);
        const auto server_runs = server_future.get();
        CHECK(server_runs.size() == 1);
        CHECK(server_runs[0].status == ServerRunStatus::Disconnected);
        CHECK(rejected_reconnects.load(std::memory_order_acquire) == 1);
        // The same caller's validated exact commit survives a typed rejection
        // while its ACK/recovery phase is reconciling the closed link.
        const ZstdSourceTransferResult replay = asio::co_spawn(c_context,
            sender->transfer_p51_route(
                armed[0], 41, connector, PrepareRequestKey{3, 921},
                deadline.as_steady_time_point(), input[0]),
            asio::use_future).get();
        CHECK(replay.status == ZstdSourceTransferStatus::Committed);
        CHECK(replay.committed_input == outcomes[0].committed_input);
        CHECK(replay.raw_digest == outcomes[0].raw_digest);
        if (kJobs == 1) {
            CHECK(replay.r2_link_rejection.has_value());
            CHECK(replay.r2_link_rejection->reason ==
                  LinkRejectReason::ReservationMissing);
        }
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
        CHECK(duplicate.r2_accounting_reference);
        if (p50_diagnostics_enabled_for_test()) {
            CHECK(!duplicate.r2_wire_accounting.has_value());
            CHECK(duplicate.r2_wire_accounting_key.has_value());
            CHECK(duplicate.r2_wire_accounting_key->c_store_guid == c_guid);
            CHECK(duplicate.r2_wire_accounting_key->f_store_guid == f_guid);
            CHECK(duplicate.r2_wire_accounting_key->logical_link_id ==
                  relationship_id);
            CHECK(duplicate.r2_wire_accounting_key->tu_seq == TuSeq{0});
            CHECK(duplicate.r2_wire_accounting_key->raw_digest ==
                  icecc::digest128(input[0]));
        }
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
    if (lose_reset_confirm || lose_reset_confirm_echo ||
        mismatch_reset_confirm_echo || change_reset_ack_on_replay) {
        CHECK(reset_confirms_received.load(std::memory_order_acquire) >= 2);
        CHECK(!drop_first_reset_confirm.load(std::memory_order_acquire));
        CHECK(retained_reset.has_value());
        CHECK(reset_requests.size() == 1);
        CHECK(reset_validation_requests.size() >= 1);
        CHECK(reset_confirm_messages.size() >= 2);
        if (mismatch_reset_confirm_echo)
            CHECK(mismatched_echoes_sent.load(std::memory_order_acquire) == 1);
        if (change_reset_ack_on_replay) {
            CHECK(changed_reset_acks_sent.load(std::memory_order_acquire) == 1);
            CHECK(retained_reset_ack_snapshot.has_value());
            CHECK(retained_reset_ack_snapshot->recovery_prepared_prefix_p == 2);
            CHECK(retained_reset_ack_snapshot->request.settled_prefix_k == 1);
            CHECK(retained_reset_ack_snapshot->unavailable_suffix_mask == 0);
            CHECK(reset_validation_requests.size() == 3);
        }
        const ResetRequest& first_reset = reset_validation_requests.front();
        for (const ResetRequest& replay : reset_validation_requests) {
            CHECK(replay.operation_id == first_reset.operation_id);
            CHECK(replay.old_relationship_epoch ==
                  first_reset.old_relationship_epoch);
            CHECK(replay.new_relationship_epoch ==
                  first_reset.new_relationship_epoch);
            CHECK(replay.old_history_nonce == first_reset.old_history_nonce);
            CHECK(replay.new_history_nonce == first_reset.new_history_nonce);
            CHECK(replay.settled_prefix_k == first_reset.settled_prefix_k);
        }
        if (reset_validation_requests.size() > 1)
            CHECK(reset_validation_requests.back().physical_link_generation >
                  first_reset.physical_link_generation);
        for (const ResetConfirm& confirm : reset_confirm_messages) {
            CHECK(confirm.operation_id == first_reset.operation_id);
            CHECK(confirm.new_relationship_epoch ==
                  first_reset.new_relationship_epoch);
            CHECK(confirm.new_history_nonce == first_reset.new_history_nonce);
            CHECK(confirm.settled_prefix_k == first_reset.settled_prefix_k);
        }
        CHECK(retained_reset_confirmed);
        CHECK(first_reset.new_relationship_epoch ==
              first_reset.old_relationship_epoch + 1);
        CHECK(first_reset.settled_prefix_k == 1);
        CHECK(std::chrono::steady_clock::now() <
              deadline.as_steady_time_point());
        std::cerr << "P51_SENDER_LOST_RESET_CONFIRM profilescope="
                  << profile_name << " confirms="
                  << reset_confirms_received.load()
                  << " reset_replays=" << reset_validation_requests.size()
                  << " exact_suffix_commits=" << commits[1].load()
                  << " lost_echo=" << lose_reset_confirm_echo
                  << " mismatched_echo=" << mismatch_reset_confirm_echo
                  << " PASS\n";
    }
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
               ? 1U : (repeat_recovery_loss || lose_recover_response ||
                       lose_reset_confirm ||
                       lose_reset_confirm_echo ||
                       mismatch_reset_confirm_echo) ? 3U :
              change_reset_ack_on_replay ? 4U : 2U));
    CHECK(binds[0].load() == 1);
    for (size_t index = 1; index != kJobs; ++index)
        CHECK((lose_recover_response || lose_reset_confirm || lose_reset_confirm_echo ||
               mismatch_reset_confirm_echo || change_reset_ack_on_replay)
                  ? binds[index].load() >= 1 &&
                        binds[index].load() <= (change_reset_ack_on_replay ? 4 : 3)
                  : binds[index].load() == 1); // suffix binds after reset

    if (lose_recover_response) {
        CHECK(recovery_reply_evidence.size() == 2);
        const auto& lost = recovery_reply_evidence[0];
        const auto& retried = recovery_reply_evidence[1];
        CHECK(lost.request.relationship_id == relationship_id);
        CHECK(retried.request.relationship_id == lost.request.relationship_id);
        CHECK(retried.request.relationship_epoch ==
              lost.request.relationship_epoch);
        CHECK(retried.request.operation_id == lost.request.operation_id);
        CHECK(retried.request.verified_floor_a == lost.request.verified_floor_a);
        CHECK(retried.request.prepared_prefix_p == lost.request.prepared_prefix_p);
        CHECK(retried.request.witness_count == lost.request.witness_count);
        CHECK(lost.request.verified_floor_a == 0);
        CHECK(lost.request.prepared_prefix_p == kJobs);
        CHECK(lost.request.witness_count == kJobs);
        CHECK(retried.witness_digest == lost.witness_digest);
        CHECK(retried.request.physical_link_generation >
              lost.request.physical_link_generation);
        RecoverBegin normalized_lost_request = lost.request;
        RecoverBegin normalized_retry_request = retried.request;
        normalized_lost_request.physical_link_generation = 1;
        normalized_retry_request.physical_link_generation = 1;
        CHECK(normalized_lost_request == normalized_retry_request);
        RecoverEnd normalized_lost_end = lost.request_end;
        RecoverEnd normalized_retry_end = retried.request_end;
        normalized_lost_end.physical_link_generation = 1;
        normalized_retry_end.physical_link_generation = 1;
        // The transcript digest intentionally covers the physical generation;
        // the separately checked normalized witness digest proves the stable
        // logical input state across these two transports.
        normalized_lost_end.transcript_digest = {};
        normalized_retry_end.transcript_digest = {};
        CHECK(normalized_lost_end == normalized_retry_end);
        ReceiptsEnd normalized_lost_response = lost.end;
        ReceiptsEnd normalized_retry_response = retried.end;
        normalized_lost_response.physical_link_generation = 1;
        normalized_retry_response.physical_link_generation = 1;
        CHECK(normalized_lost_response == normalized_retry_response);
        CHECK(lost.end.verified_floor_a == 0);
        CHECK(lost.end.committed_prefix_k == 1);
        CHECK(lost.end.receipt_count == 1);
        CHECK(lost.rows.size() == 1 && retried.rows.size() == 1);
        CHECK(lost.rows.front().relationship_id == relationship_id);
        CHECK(retried.rows.front().relationship_id == relationship_id);
        CHECK(lost.rows.front().relationship_epoch == lost.request.relationship_epoch);
        CHECK(retried.rows.front().relationship_epoch == retried.request.relationship_epoch);
        CHECK(lost.rows.front().operation_id == lost.request.operation_id);
        CHECK(retried.rows.front().operation_id == retried.request.operation_id);
        CHECK(lost.rows.front().physical_link_generation ==
              lost.request.physical_link_generation);
        CHECK(retried.rows.front().physical_link_generation ==
              retried.request.physical_link_generation);
        CHECK(retried.rows.front().physical_link_generation >
              lost.rows.front().physical_link_generation);
        CHECK(lost.rows.front().receipt == retried.rows.front().receipt);
        CHECK(lost.rows.front().receipt.relationship_ordinal == 1);
        CHECK(commits[0].load(std::memory_order_acquire) == 1);
        CHECK(binds[0].load(std::memory_order_acquire) == 1);
        CHECK(acknowledged.load(std::memory_order_acquire) == kJobs);
        CHECK(std::chrono::steady_clock::now() < deadline.as_steady_time_point());
        auto live_entries_promise =
            std::make_shared<std::promise<size_t>>();
        auto live_entries_future = live_entries_promise->get_future();
        asio::post(c_context, [authority, live_entries_promise] {
            live_entries_promise->set_value(authority->live_entry_count());
        });
        CHECK(live_entries_future.wait_until(deadline.as_steady_time_point()) ==
              std::future_status::ready);
        CHECK(live_entries_future.get() == 0);
        CHECK(retained_reset.has_value());
        CHECK(retained_reset->settled_prefix_k == 1);
        CHECK(retained_reset_confirmed);
        std::cerr << "P51_SENDER_LOST_RECOVER_RESPONSE profile=" << profile_name
                  << " callers=" << kJobs
                  << " response_attempts=" << recovery_reply_evidence.size()
                  << " same_operation=1 same_witness=1 committed_prefix=1"
                  << " suffix=" << (kJobs - 1) << " credits=0 PASS\n";
    }

    if (post_reset_offer_probe) {
        CHECK(retained_reset.has_value());
        CHECK(retained_reset->new_relationship_epoch ==
              armed[0].relationship_epoch + 1);
        const unsigned connectors_after_reset =
            connector_calls.load(std::memory_order_acquire);

        P51SourceArmedFields future_epoch_arm = armed[kJobs];
        future_epoch_arm.relationship_epoch =
            retained_reset->new_relationship_epoch + 1;
        const auto future_result = asio::co_spawn(
            c_context,
            sender->transfer_p51_route(
                future_epoch_arm, 41, connector,
                PrepareRequestKey{3, 921 + kJobs},
                deadline.as_steady_time_point(), input[kJobs]),
            asio::use_future).get();
        CHECK(future_result.status == ZstdSourceTransferStatus::InvalidRequest);
        CHECK(!future_result.committed_input);
        CHECK(connector_calls.load(std::memory_order_acquire) ==
              connectors_after_reset);
        CHECK(commits[kJobs].load(std::memory_order_acquire) == 0);

        const auto drained_prefix_events = [&](uint64_t prefix) {
            std::lock_guard lock(r2_interval_observer_mutex);
            return std::count_if(r2_observed_intervals.begin(),
                                 r2_observed_intervals.end(),
                [&](const R2WireControlSnapshot& interval) {
                    return interval.end ==
                               R2WireIntervalEnd::DrainedAckCheckpoint &&
                           interval.drained_ack_prefix == prefix;
                });
        };
        const auto ack_prefix_events_before_fresh =
            drained_prefix_events(kJobs);
        CHECK(ack_prefix_events_before_fresh > 0);

        // A queued pre-RESET ARM remains valid: only its relationship epoch
        // is rebased to the verified current epoch. The same request must
        // then commit on the already-recovered physical link.
        const auto fresh_result = asio::co_spawn(
            c_context,
            sender->transfer_p51_route(
                armed[kJobs], 41, connector,
                PrepareRequestKey{3, 921 + kJobs},
                deadline.as_steady_time_point(), input[kJobs]),
            asio::use_future).get();
        CHECK(fresh_result.status == ZstdSourceTransferStatus::Committed);
        CHECK(fresh_result.raw_bytes == input[kJobs].size());
        CHECK(fresh_result.raw_digest == icecc::digest128(input[kJobs]));
        CHECK((fresh_result.committed_input ==
               InputRecordKey{c_guid, TuSeq{kJobs}}));
        CHECK(commits[kJobs].load(std::memory_order_acquire) == 1);
        // The pre-bundle ACK flush on the reset-confirmed link is a no-op.
        // It must not manufacture a second ACK-write interval at the already
        // recovered prefix; the fresh job's real ACK advances to kJobs + 1.
        if (p50_diagnostics_enabled_for_test())
            CHECK(drained_prefix_events(kJobs) ==
                  ack_prefix_events_before_fresh);
        CHECK(connector_calls.load(std::memory_order_acquire) ==
              connectors_after_reset);
        {
            std::unique_lock lock(ack_mutex);
            CHECK(ack_cv.wait_until(lock, deadline.as_steady_time_point(), [&] {
                return acknowledged.load(std::memory_order_acquire) == total_jobs;
            }));
        }
        CHECK(acknowledged.load(std::memory_order_acquire) == total_jobs);
    }

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
    CHECK(server_runs.size() == (change_reset_ack_on_replay ? 4U :
        (repeat_recovery_loss || lose_recover_response || lose_reset_confirm ||
                                  lose_reset_confirm_echo ||
                                  mismatch_reset_confirm_echo) ? 3U
        : (retire_after_positive_receipt || expire_after_positive_receipt)
            ? 1U : 2U));
    CHECK(server_runs[0].status == ServerRunStatus::Disconnected);
    if (server_runs.size() > 1)
        CHECK(server_runs[1].status == (lose_reset_confirm
              ? ServerRunStatus::TerminalError
              : ServerRunStatus::Disconnected));
    if (lose_reset_confirm) {
        CHECK(server_runs[1].terminal_error.has_value());
        CHECK(server_runs[1].terminal_error->detail ==
              "R2 RESET_CONFIRM does not match RESET_ACK");
    }
    if (server_runs.size() > 2)
        CHECK(server_runs[2].status == ServerRunStatus::Disconnected);
    std::cerr << "P51_SENDER_SHARED_FAILURE profile=" << profile_name
              << " callers=" << kJobs
              << " initial_sent=" << kJobs
              << " replayed_suffix=" << (kJobs - 1)
              << " repeated_loss=" << repeat_recovery_loss
              << " lost_confirm=" << lose_reset_confirm
              << " recovered_prefix=1 PASS\n";
}

void test_p51_sender_shared_failure_recovers_pending_callers() {
    for (const ProfileId profile : {ProfileId::ZSTD_TU, ProfileId::P29V1,
                                    ProfileId::ZSTD_ROUTE}) {
        run_p51_sender_shared_failure_case(2, profile);
        run_p51_sender_shared_failure_case(30, profile);
    }
}

void test_p51_sender_post_reset_offer_rebase_and_future_reject() {
    ScopedP50Diagnostics diagnostics;
    for (const ProfileId profile : {ProfileId::ZSTD_TU, ProfileId::P29V1,
                                    ProfileId::ZSTD_ROUTE}) {
        run_p51_sender_shared_failure_case(
            2, profile, false, false, false, false, false, false,
            false, true);
    }
}

void test_p51_sender_future_arm_during_lost_reset_ack_is_request_local() {
    run_p51_sender_shared_failure_case(
        2, ProfileId::ZSTD_TU, true, false, false, false, false, false,
        false, false, true);
}

void test_p51_sender_replays_lost_reset_confirm_for_all_profiles() {
    for (const ProfileId profile : {ProfileId::ZSTD_TU, ProfileId::P29V1,
                                    ProfileId::ZSTD_ROUTE}) {
        run_p51_sender_shared_failure_case(
            2, profile, false, false, false, false, false, false, false,
            false, false, true);
        run_p51_sender_shared_failure_case(
            2, profile, false, false, false, false, false, false, false,
            false, false, false, true);
    }
}

void test_p51_sender_rejects_mismatched_reset_confirm_echo_for_all_profiles() {
    for (const ProfileId profile : {ProfileId::ZSTD_TU, ProfileId::P29V1,
                                    ProfileId::ZSTD_ROUTE}) {
        run_p51_sender_shared_failure_case(
            2, profile, false, false, false, false, false, false, false,
            false, false, false, false, true);
    }
}

void test_p51_sender_rejects_changed_reset_ack_snapshot_for_all_profiles() {
    for (const ProfileId profile : {ProfileId::ZSTD_TU, ProfileId::P29V1,
                                    ProfileId::ZSTD_ROUTE}) {
        run_p51_sender_shared_failure_case(
            2, profile, false, false, false, false, false, false, false,
            false, false, false, false, false, true);
    }
}

void test_p51_sender_repeated_shared_failure_recovers_pending_callers() {
    for (const ProfileId profile : {ProfileId::ZSTD_TU, ProfileId::P29V1,
                                    ProfileId::ZSTD_ROUTE}) {
        run_p51_sender_shared_failure_case(2, profile, true);
        run_p51_sender_shared_failure_case(30, profile, true);
    }
}

void test_p51_sender_retries_lost_recover_response_all_profiles() {
    for (const ProfileId profile : {ProfileId::ZSTD_TU, ProfileId::P29V1,
                                    ProfileId::ZSTD_ROUTE}) {
        for (const size_t window : {2U, 30U})
            run_p51_sender_shared_failure_case(
                window, profile, false, false, false, false, false, false,
                false, false, false, false, false, false, false,
                RecoverResponseLoss::FirstResponse);
    }
}

void test_p51_sender_positive_commit_survives_later_typed_rejection() {
    run_p51_sender_shared_failure_case(
        1, ProfileId::ZSTD_TU, false, false, false, false, false, false, true);
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
    ScopedP50Diagnostics diagnostics;
    run_p51_sender_recovery_case(false, true);
    run_p51_sender_shared_failure_case(
        1, ProfileId::ZSTD_TU, false, true);
}

void test_p51_sender_deadline_during_recovery_and_ack() {
    ScopedP50Diagnostics diagnostics;
    run_p51_sender_recovery_case(false, false, true);
    run_p51_sender_shared_failure_case(
        1, ProfileId::ZSTD_TU, false, false, true);
}

void test_p51_sender_initial_connector_retry_is_bounded_and_cancellable() {
    const auto [c_guid, f_guid] = sender_r2_store_guids();
    const auto profile = ProfileId::ZSTD_TU;
    const auto armed = sender_r2_armed(
        sender_r2_arm(c_guid, 5701, 5702, 1, profile), f_guid, 5703, 1);
    EndpointCaps caps;
    caps.profile = profile;
    caps.supported_profiles = profile_bit(profile);
    caps.zstd.max_raw_bytes = 1U << 20;
    caps.zstd.max_encoded_body_bytes = 1U << 20;
    PreparationAuthorityLimits limits;
    limits.max_speculative_tus = 1;
    limits.max_speculative_raw_bytes = 1U << 20;
    limits.max_live_entries = 4;
    const PreparationRouteKey route{f_guid, 23, profile};
    const std::vector<uint8_t> source{'s', 'e', 't', 'u', 'p'};

    {
        auto authority = std::make_shared<P50PreparationAuthority>(
            c_guid, caps.zstd, limits, 1, profile);
        ZstdSourceTransferConfig sender_config = config();
        sender_config.endpoint_caps = caps;
        sender_config.authority_limits = limits;
        std::atomic<unsigned> prepares{0};
        sender_config.before_prepare_for_route_for_test = [&] {
            prepares.fetch_add(1, std::memory_order_relaxed);
        };
        auto sender = std::make_shared<P50ZstdSourceSender>(
            authority, route, PrepareRequestKey{3, 5701}, sender_config);
        asio::io_context context;
        std::atomic<unsigned> connectors{0};
        AsyncConnectedFdFactory connector = [&](auto, auto completion) {
            connectors.fetch_add(1, std::memory_order_relaxed);
            completion(-1);
        };
        const auto start = std::chrono::steady_clock::now();
        auto result = asio::co_spawn(context,
            sender->transfer_p51_route(
                armed, 27, connector, PrepareRequestKey{3, 5701},
                start + std::chrono::milliseconds(180), source),
            asio::use_future);
        context.run();
        const auto transfer = result.get();
        CHECK(transfer.status == ZstdSourceTransferStatus::DeadlineExceeded);
        CHECK(transfer.route_local_failure);
        CHECK(!transfer.committed_input);
        CHECK(connectors.load(std::memory_order_relaxed) >= 2);
        CHECK(connectors.load(std::memory_order_relaxed) < 20);
        CHECK(prepares.load(std::memory_order_relaxed) == 1);
        CHECK(std::chrono::steady_clock::now() - start <
              std::chrono::seconds(2));
        std::cerr << "P51_SENDER_INITIAL_CONNECTOR_DEADLINE attempts="
                  << connectors.load() << " prepares=" << prepares.load()
                  << " PASS\n";
    }

    {
        auto authority = std::make_shared<P50PreparationAuthority>(
            c_guid, caps.zstd, limits, 1, profile);
        const auto retirement_armed = sender_r2_armed(
            sender_r2_arm(c_guid, 5711, 5712, 1, profile), f_guid, 5713, 1);
        ZstdSourceTransferConfig sender_config = config();
        sender_config.endpoint_caps = caps;
        sender_config.authority_limits = limits;
        asio::io_context context;
        auto work = asio::make_work_guard(context);
        std::atomic<unsigned> connectors{0};
        std::atomic<bool> waiter_registered{false};
        std::weak_ptr<P50ZstdSourceSender> sender_weak;
        sender_config.after_r2_recovery_waiter_registered_for_test =
            [&](std::chrono::steady_clock::duration) {
                waiter_registered.store(true, std::memory_order_release);
                if (const auto active = sender_weak.lock())
                    active->retire_for_replacement();
            };
        auto sender = std::make_shared<P50ZstdSourceSender>(
            authority, route, PrepareRequestKey{3, 5711}, sender_config);
        sender_weak = sender;
        AsyncConnectedFdFactory connector = [&](auto, auto completion) {
            connectors.fetch_add(1, std::memory_order_relaxed);
            completion(-1);
        };
        const auto start = std::chrono::steady_clock::now();
        auto result = asio::co_spawn(context,
            sender->transfer_p51_route(
                retirement_armed, 27, connector, PrepareRequestKey{3, 5711},
                start + std::chrono::seconds(5), source),
            asio::use_future);
        std::thread runner([&] { context.run(); });
        struct RunnerCleanup {
            asio::io_context& context;
            std::thread& runner;
            ~RunnerCleanup() {
                context.stop();
                if (runner.joinable()) runner.join();
            }
        } runner_cleanup{context, runner};
        CHECK(result.wait_for(std::chrono::seconds(2)) ==
              std::future_status::ready);
        const auto transfer = result.get();
        CHECK(waiter_registered.load(std::memory_order_acquire));
        CHECK(transfer.status == ZstdSourceTransferStatus::Unavailable);
        CHECK(transfer.route_local_failure);
        CHECK(!transfer.committed_input);
        CHECK(connectors.load(std::memory_order_relaxed) == 1);
        CHECK(std::chrono::steady_clock::now() - start <
              std::chrono::seconds(2));
        work.reset();
        context.stop();
        runner.join();
        std::cerr << "P51_SENDER_INITIAL_CONNECTOR_RETIRE attempts="
                  << connectors.load() << " PASS\n";
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

void test_completion_log_bounded_r1_byte_accounting() {
    CHECK(!CompletionStamp{}.r2_traffic);
    CompletionLog detailed;
    CompletionStamp stamp;
    stamp.actor = ActorSide::C;
    stamp.operation = AsyncOperationKind::WriteFragment;
    detailed.record({stamp, 5, 0});
    stamp.operation = AsyncOperationKind::ReadHeader;
    detailed.record({stamp, 7, 0});
    stamp.operation = AsyncOperationKind::ReadPayload;
    detailed.record({stamp, 11, 104});
    stamp.operation = AsyncOperationKind::Connect;
    detailed.record({stamp, 13, 0});
    stamp.operation = AsyncOperationKind::WaitPeerClose;
    detailed.record({stamp, 17, 0});
    stamp.r2_traffic = true;
    stamp.operation = AsyncOperationKind::WriteFragment;
    detailed.record({stamp, 101, 0});
    stamp.r2_traffic = false;
    stamp.actor = ActorSide::F;
    detailed.record({stamp, 103, 0});
    CHECK(detailed.valid());
    CHECK(detailed.completions().size() == 7);
    uint64_t detailed_c_to_f = 0;
    uint64_t detailed_f_to_c = 0;
    for (const AsyncCompletion& completion : detailed.completions()) {
        if (completion.stamp.r2_traffic || completion.stamp.actor != ActorSide::C)
            continue;
        if (completion.stamp.operation == AsyncOperationKind::WriteFragment)
            detailed_c_to_f += completion.transferred_bytes;
        else if (completion.stamp.operation == AsyncOperationKind::ReadHeader ||
                 completion.stamp.operation == AsyncOperationKind::ReadPayload)
            detailed_f_to_c += completion.transferred_bytes;
    }
    CHECK(detailed_c_to_f == 5);
    CHECK(detailed_f_to_c == 18);

    CompletionLog counters{CompletionLog::StorageMode::ClientByteTotals};
    stamp.actor = ActorSide::C;
    stamp.operation = AsyncOperationKind::WriteFragment;
    counters.record({stamp, 5, 0});
    stamp.operation = AsyncOperationKind::ReadPayload;
    counters.record({stamp, 7, 0});
    counters.record({stamp, 11, 104});
    stamp.operation = AsyncOperationKind::Connect;
    counters.record({stamp, 13, 0});
    stamp.operation = AsyncOperationKind::WaitPeerClose;
    counters.record({stamp, 17, 0});
    stamp.r2_traffic = true;
    stamp.operation = AsyncOperationKind::WriteFragment;
    counters.record({stamp, 101, 0});
    stamp.r2_traffic = false;
    stamp.actor = ActorSide::F;
    counters.record({stamp, 103, 0});
    CHECK(counters.valid());
    CHECK(counters.retained_record_count() == 0);
    CHECK(counters.completions().empty());
    CHECK(counters.c_to_f_bytes() == detailed_c_to_f);
    CHECK(counters.f_to_c_bytes() == detailed_f_to_c);

    counters.clear();
    CHECK(counters.valid());
    CHECK(counters.c_to_f_bytes() == 0);
    CHECK(counters.f_to_c_bytes() == 0);
    CHECK(counters.retained_record_count() == 0);

    CompletionLog overflow{CompletionLog::StorageMode::ClientByteTotals};
    stamp.actor = ActorSide::C;
    stamp.r2_traffic = false;
    stamp.operation = AsyncOperationKind::WriteFragment;
    overflow.record({stamp, std::numeric_limits<uint64_t>::max(), 0});
    overflow.record({stamp, 1, 0});
    CHECK(!overflow.valid());
    CHECK(overflow.c_to_f_bytes() == std::numeric_limits<uint64_t>::max());
    overflow.clear();
    CHECK(overflow.valid());
    CHECK(overflow.c_to_f_bytes() == 0);

    CompletionLog zero_capacity{0};
    zero_capacity.record({stamp, 1, 0});
    CHECK(!zero_capacity.valid());
    CHECK(zero_capacity.completions().empty());
    CHECK(zero_capacity.c_to_f_bytes() == 0);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 &&
        std::string_view(argv[1]) == "--writer-backpressure-ack-shutdown") {
#if defined(__linux__)
        test_p51_sender_writer_backpressure_ack_and_shutdown();
        std::cerr << "P51_SENDER_WRITER_BACKPRESSURE_ACK_SHUTDOWN_SELECTOR PASS\n";
        return 0;
#else
        std::cerr << "SKIP: D16 SIOCOUTQ backpressure witness is Linux-only\n";
        return 77;
#endif
    }
    if (argc == 2 && std::string_view(argv[1]) == "--disconnected-retry") {
        test_disconnected_retry_is_bounded_and_exactly_once();
        std::cerr << "P50_SENDER_DISCONNECTED_RETRY_SELECTOR PASS\n";
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--route-ledger-replay") {
        test_route_completed_ledger_releases_live_entry();
        std::cerr << "P51_SENDER_ROUTE_LEDGER_REPLAY_SELECTOR PASS\n";
        return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--window-matrix") {
        test_p51_sender_window_matrix_and_serial_control();
        std::cerr << "P51_SENDER_WINDOW_MATRIX_SELECTOR PASS\n";
        return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--r2-wire-accounting-w30") {
        test_p51_sender_w30_concurrent_callers_refill_and_duplicate();
        std::cerr << "P51_SENDER_R2_WIRE_ACCOUNTING_W30_SELECTOR PASS\n";
        return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--r2-physical-retirement") {
        test_p51_sender_w30_concurrent_callers_refill_and_duplicate();
#if defined(__linux__)
        test_p51_sender_writer_backpressure_ack_and_shutdown();
#endif
        std::cerr << "P51_SENDER_R2_PHYSICAL_RETIREMENT_SELECTOR PASS\n";
        return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--serial-w1-control") {
        test_p51_sender_serial_control_only();
        std::cerr << "P51_SENDER_SERIAL_W1_CONTROL_SELECTOR PASS\n";
        return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--connector-first-failure-w30") {
        test_p51_sender_w30_concurrent_callers_refill_and_duplicate(true);
        std::cerr << "P51_SENDER_CONNECTOR_FIRST_FAILURE_W30_SELECTOR PASS\n";
        return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--initial-connector-bounds") {
        test_p51_sender_initial_connector_retry_is_bounded_and_cancellable();
        std::cerr << "P51_SENDER_INITIAL_CONNECTOR_BOUNDS_SELECTOR PASS\n";
        return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--recovery-lost-commit") {
        test_p51_sender_recovers_lost_commit_reply_after_connector_failure();
        std::cerr << "P51_SENDER_RECOVERY_SELECTOR PASS\n";
        return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--r2-observer-failure") {
        test_p51_sender_r2_observer_failure_is_fail_closed();
        std::cerr << "P51_SENDER_R2_OBSERVER_FAILURE_SELECTOR PASS\n";
        return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--completed-ledger-cap-w2") {
        test_p51_completed_ledger_reserves_live_capacity();
        return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--completed-ledger-expired-witness") {
        test_p51_expired_retained_witness_has_bounded_cleanup(false);
        test_p51_expired_retained_witness_has_bounded_cleanup(true);
        return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--shared-failure-recovery") {
        test_p51_sender_shared_failure_recovers_pending_callers();
        std::cerr << "P51_SENDER_SHARED_FAILURE_SELECTOR PASS\n";
        return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--post-reset-offer-rebase") {
        test_p51_sender_post_reset_offer_rebase_and_future_reject();
        std::cerr << "P51_SENDER_POST_RESET_OFFER_REBASE_SELECTOR PASS\n";
        return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--future-arm-during-recovery") {
        test_p51_sender_future_arm_during_lost_reset_ack_is_request_local();
        std::cerr << "P51_SENDER_FUTURE_ARM_DURING_RECOVERY_SELECTOR PASS\n";
        return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--lost-reset-confirm") {
        test_p51_sender_replays_lost_reset_confirm_for_all_profiles();
        std::cerr << "P51_SENDER_LOST_RESET_CONFIRM_SELECTOR PASS\n";
        return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--lost-recover-response") {
        test_p51_sender_retries_lost_recover_response_all_profiles();
        std::cerr << "P51_SENDER_LOST_RECOVER_RESPONSE_SELECTOR PASS\n";
        return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--changed-reset-ack-replay") {
        test_p51_sender_rejects_changed_reset_ack_snapshot_for_all_profiles();
        std::cerr << "P51_SENDER_CHANGED_RESET_ACK_REPLAY_SELECTOR PASS\n";
        return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--mismatched-reset-confirm-echo") {
        test_p51_sender_rejects_mismatched_reset_confirm_echo_for_all_profiles();
        std::cerr << "P51_SENDER_MISMATCHED_RESET_CONFIRM_ECHO_SELECTOR PASS\n";
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
    if (argc == 2 &&
        std::string_view(argv[1]) == "--completion-log-accounting") {
        test_completion_log_bounded_r1_byte_accounting();
        std::cerr << "P51_SENDER_COMPLETION_LOG_ACCOUNTING_SELECTOR PASS\n";
        return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--exact-network-r1") {
        test_exact_network_transfer();
        std::cerr << "P50_SENDER_EXACT_NETWORK_R1_SELECTOR PASS\n";
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
    run("completed_ledger_cap_w2",
        test_p51_completed_ledger_reserves_live_capacity);
    run("completed_ledger_expired_witness",
        [] {
            test_p51_expired_retained_witness_has_bounded_cleanup(false);
            test_p51_expired_retained_witness_has_bounded_cleanup(true);
        });
    run("window_matrix_and_serial_control",
        test_p51_sender_window_matrix_and_serial_control);
    run("connector_first_failure_w30", [] {
        test_p51_sender_w30_concurrent_callers_refill_and_duplicate(true);
    });
    run("initial_connector_bounds",
        test_p51_sender_initial_connector_retry_is_bounded_and_cancellable);
    run("typed_link_rejection", test_p51_sender_typed_link_rejection_is_terminal_and_exact);
    run("shared_typed_rejection", test_p51_sender_typed_rejection_is_shared_route_local);
    run("bad_link_reject_echo", test_p51_client_reject_echo_must_match_current_offer);
    run("positive_after_rejection", test_p51_sender_positive_commit_survives_later_typed_rejection);
    run("lost_commit_recovery", test_p51_sender_recovers_lost_commit_reply_after_connector_failure);
    run("r2_observer_failure",
        test_p51_sender_r2_observer_failure_is_fail_closed);
    run("shared_failure", test_p51_sender_shared_failure_recovers_pending_callers);
    run("post_reset_offer_rebase",
        test_p51_sender_post_reset_offer_rebase_and_future_reject);
    run("future_arm_during_recovery",
        test_p51_sender_future_arm_during_lost_reset_ack_is_request_local);
    run("lost_reset_confirm",
        test_p51_sender_replays_lost_reset_confirm_for_all_profiles);
    run("lost_recover_response",
        test_p51_sender_retries_lost_recover_response_all_profiles);
    run("mismatched_reset_confirm_echo",
        test_p51_sender_rejects_mismatched_reset_confirm_echo_for_all_profiles);
    run("changed_reset_ack_replay",
        test_p51_sender_rejects_changed_reset_ack_snapshot_for_all_profiles);
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
    run("completion_log_accounting",
        test_completion_log_bounded_r1_byte_accounting);
#if defined(__linux__)
    run("writer_backpressure_ack_shutdown",
        test_p51_sender_writer_backpressure_ack_and_shutdown);
#else
    std::cerr << "P51_TEST_SKIP writer_backpressure_ack_shutdown "
                 "requires Linux SIOCOUTQ\n";
#endif
}
