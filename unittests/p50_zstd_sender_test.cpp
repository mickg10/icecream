#include "client/p50_zstd_sender.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_future.hpp>

#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <future>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
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
    char path[] = "/tanksmall/scratch/ictmp/p50-zstd-sender-test-XXXXXX";
    const int write_fd = ::mkstemp(path);
    CHECK(write_fd >= 0);
    CHECK(::fchmod(write_fd, 0600) == 0);
    const std::string source = "#define owned_fd 1\n";
    CHECK(::write(write_fd, source.data(), source.size()) ==
          static_cast<ssize_t>(source.size()));
    CHECK(::close(write_fd) == 0);
    const int read_fd = ::open(path, O_RDONLY | O_CLOEXEC);
    CHECK(read_fd >= 0);

    asio::io_context context;
    P50ZstdSourceSender sender(Id128::from_u64(7003), PrepareRequestKey{92, 8}, config());
    auto result = asio::co_spawn(
        context,
        sender.transfer({asio::ip::address_v4::loopback(), 0}, OwnedSourceFd(read_fd)),
        asio::use_future);
    context.run();
    CHECK(result.get().status == ZstdSourceTransferStatus::InvalidRequest);
    CHECK(::unlink(path) == 0);
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
    test_route_failure_requires_cold_replacement();
    test_explicit_route_operations_bind_request_and_deadline();
    test_explicit_route_retry_is_bounded_then_replaced();
    test_owned_fd_and_fail_closed_validation();
    test_adopted_fd_factory_exact_transfer();
    test_absolute_deadline_is_required();
    test_disconnected_retry_is_bounded_and_exactly_once();
    test_factory_cannot_extend_absolute_deadline();
}
