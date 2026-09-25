#include "cache/p50_endpoint.h"
#include "cache/p50_adopted_outcome_writer.h"
#include "unittests/support/p50_adopted_socket_lease.h"
#include "cache/p50_slice0.h"
#include "cache/codec/p29_wire.h"

#include <zstd.h>

#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <future>
#include <functional>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <vector>

namespace icecc::p50 {

struct P50PreparationAuthorityTestAccess {
    static PreparedInputPtr resolve(const P50PreparationAuthority& authority,
                                    PreparedTuHandle handle) {
        return authority.resolve(handle);
    }
};

} // namespace icecc::p50

namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using namespace icecc::p50;

[[noreturn]] void fail(std::string_view detail) {
    std::cerr << "p50_endpoint_test: " << detail << '\n';
    std::exit(1);
}

void require(bool value, std::string_view detail) {
    if (!value)
        fail(detail);
}

void test_action_hold_rendezvous() {
    char root_template[] = "/tmp/p50-action-hold-XXXXXX";
    char* root_value = ::mkdtemp(root_template);
    require(root_value != nullptr, "cannot create action-hold test directory");
    const std::filesystem::path root(root_value);
    const std::filesystem::path marker = root / "marker";
    const std::filesystem::path release = root / "release";
    const std::filesystem::path trace_path = root / "f-action-trace.jsonl";

    require(::setenv("ICECC_P50_TEST_ACTION_HOLD", "F:TX_BEGIN", 1) == 0 &&
                ::setenv("ICECC_P50_TEST_ACTION_HOLD_MARKER",
                         marker.c_str(), 1) == 0 &&
                ::setenv("ICECC_P50_TEST_ACTION_HOLD_RELEASE",
                         release.c_str(), 1) == 0 &&
                ::setenv("ICECC_P50_TEST_ACTION_HOLD_TIMEOUT_MS", "5000", 1) == 0 &&
                ::setenv("ICECC_P50_F_ACTION_TRACE", trace_path.c_str(), 1) == 0,
            "cannot configure action-hold rendezvous");

    std::atomic<bool> marker_observed{false};
    std::thread releaser([&] {
        for (size_t attempt = 0; attempt != 500; ++attempt) {
            std::ifstream input(marker);
            if (input) {
                std::string header;
                std::getline(input, header);
                std::string json;
                std::getline(input, json);
                std::ifstream trace_input(trace_path);
                std::string trace;
                std::getline(trace_input, trace);
                marker_observed = header.starts_with("P50_ACTION_HOLD pid=") &&
                                  header.find(" actor=F action=TX_BEGIN") !=
                                      std::string::npos &&
                                  json.starts_with(
                                      "{\"action\":\"TX_BEGIN\",\"actor\":\"F\",") &&
                                  trace == json;
                std::ofstream output(release);
                output << "release\n";
                output.close();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    ActionRecord record;
    record.actor = ActorSide::F;
    record.action = ActionType::TX_BEGIN;
    record.c_store_guid = Id128::from_u64(991);
    record.f_store_guid = Id128::from_u64(992);
    record.history_nonce = HistoryNonce{1};
    record.rel_seq = RelSeq{2};
    record.tu_seq = TuSeq{3};
    ActionTrace trace;
    trace.record(record);
    releaser.join();
    require(trace.valid() && marker_observed,
            "named action did not complete its exact rendezvous");

    // The marker is the one-shot fence inherited by a replacement sidecar.
    // A second matching action must not wait again.
    std::filesystem::remove(release);
    const auto second_start = std::chrono::steady_clock::now();
    trace.record(record);
    require(trace.valid() &&
                std::chrono::steady_clock::now() - second_start <
                    std::chrono::milliseconds(250),
            "one-shot action rendezvous blocked a replacement");

    const std::filesystem::path stale_marker = root / "stale-marker";
    {
        std::ofstream output(stale_marker);
        output << "incomplete\n";
    }
    require(::setenv("ICECC_P50_TEST_ACTION_HOLD_MARKER",
                     stale_marker.c_str(), 1) == 0,
            "cannot configure stale action-hold marker");
    ActionTrace stale;
    stale.record(record);
    require(!stale.valid(), "incomplete action-hold marker was accepted");

    const std::filesystem::path bad_marker = root / "bad-marker";
    const std::filesystem::path bad_release = root / "bad-release";
    std::filesystem::create_symlink(bad_marker, bad_release);
    require(::setenv("ICECC_P50_TEST_ACTION_HOLD_MARKER",
                     bad_marker.c_str(), 1) == 0 &&
                ::setenv("ICECC_P50_TEST_ACTION_HOLD_RELEASE",
                         bad_release.c_str(), 1) == 0,
            "cannot configure invalid action-hold release");
    ActionTrace invalid;
    invalid.record(record);
    require(!invalid.valid(), "symlink action-hold release was accepted");

    ::unsetenv("ICECC_P50_TEST_ACTION_HOLD");
    ::unsetenv("ICECC_P50_TEST_ACTION_HOLD_MARKER");
    ::unsetenv("ICECC_P50_TEST_ACTION_HOLD_RELEASE");
    ::unsetenv("ICECC_P50_TEST_ACTION_HOLD_TIMEOUT_MS");
    ::unsetenv("ICECC_P50_F_ACTION_TRACE");
    std::filesystem::remove_all(root);
}

template <class Exception, class Function>
void require_throws(Function&& function, std::string_view detail) {
    try {
        std::forward<Function>(function)();
    } catch (const Exception&) {
        return;
    } catch (...) {
        fail(std::string(detail) + " (wrong exception type)");
    }
    fail(std::string(detail) + " (no exception)");
}

std::vector<uint8_t> bytes(std::string_view text) { return {text.begin(), text.end()}; }

struct P5coStoreGuids {
    CStoreGuid c;
    FStoreGuid f;
};

P5coStoreGuids p5co_store_guids(uint8_t seed) {
    std::array<uint8_t, 16> client{};
    std::array<uint8_t, 16> file{};
    for (size_t index = 0; index != client.size(); ++index) {
        client[index] = static_cast<uint8_t>(seed + index);
        file[index] = static_cast<uint8_t>(seed + 41 + index);
    }
    client[kStoreIdentityRoleByte] &=
        static_cast<uint8_t>(~kStoreIdentityRoleMask);
    file[kStoreIdentityRoleByte] |= kStoreIdentityRoleMask;
    require(store_identity_guid_valid_for_role(client,
                                               kStoreIdentityClientRole) &&
                store_identity_guid_valid_for_role(file,
                                                   kStoreIdentityFileRole) &&
                !store_identity_file_guid_matches_client(client, file),
            "P5CO test StoreGuid identities are invalid or aliased");
    return {CStoreGuid{client}, FStoreGuid{file}};
}

ClaimAttemptCapability128 p5co_attempt_capability(uint8_t seed) {
    ClaimAttemptCapability128 result;
    for (size_t index = 0; index != result.bytes.size(); ++index)
        result.bytes[index] = static_cast<uint8_t>(seed + index);
    require(result.valid(), "P5CO test capability is invalid");
    return result;
}

daemon::P50CacheSessionOutcome p5co_adopted_outcome(
    const P5coStoreGuids& guids, uint64_t operation_sequence) {
    daemon::P50CacheSessionWireClaim claim;
    auto& arm = claim.binding.arm;
    arm.wire_job_id = 1701;
    arm.assignment_epoch = 1801;
    arm.assignment_nonce = 1901;
    arm.selected_f_host = "f.p50-endpoint.test";
    arm.selected_f_ordinary_port = 10250;
    arm.selected_f_cache_port = 10251;
    arm.cache_protocol = CACHE_WIRE_REVISION;
    arm.cache_profile = CACHE_PROFILE_ZSTD_TU;
    arm.logical_job = 2001;
    arm.compiler_attempt = 2101;
    arm.c_store_generation = 2201;
    arm.c_store_derivation_version = kStoreIdentityDerivationVersion;
    arm.c_store_guid = guids.c.bytes;
    arm.source_request_id = 2301;
    arm.source_mode = P50_SOURCE_MODE_ZSTD_TU;
    arm.c_control_generation = 2401;
    arm.c_control_attempt = 2501;
    claim.binding.f_control_generation = 3101;
    claim.binding.f_control_attempt = 3201;
    claim.binding.f_store_generation = 3301;
    claim.binding.f_store_guid = guids.f.bytes;
    claim.binding.f_store_derivation_version =
        kStoreIdentityDerivationVersion;
    claim.binding.arm_observation_id = 3401;
    claim.binding.source_budget_msec = 5000;
    claim.attempt = {1, p5co_attempt_capability(51)};
    require(claim.valid(), "P5CO endpoint claim is invalid");

    daemon::P50CacheSessionOutcome outcome;
    outcome.kind = daemon::P50CacheSessionOutcomeKind::Adopted;
    outcome.canonical_claim = daemon::encode_cache_session_wire_claim(claim);
    outcome.f_sidecar_launch = claim.binding.f_control_identity();
    outcome.f_store_guid = claim.binding.f_store_guid;
    outcome.operation = {outcome.f_sidecar_launch,
                         daemon::P50SessionOperationRole::FSession,
                         operation_sequence};
    require(outcome.valid(), "P5CO endpoint outcome is invalid");
    return outcome;
}

sidecar::AbsoluteMonotonicDeadline p5co_deadline_after(
    std::chrono::nanoseconds duration) {
    sidecar::SystemMonotonicObservationSource observations;
    const std::optional<sidecar::MonotonicObservation> observed =
        observations.observe();
    require(observed.has_value() && observed->valid() && duration.count() > 0,
            "CLOCK_MONOTONIC endpoint deadline setup failed");
    require(observed->now_ns <=
                std::numeric_limits<int64_t>::max() - duration.count(),
            "CLOCK_MONOTONIC endpoint deadline overflowed");
    return {observed->now_ns + duration.count(),
            observed->clock.clock_domain_id,
            observed->clock.time_namespace_id};
}

class FixedMonotonicObservations final
    : public sidecar::MonotonicObservationSource {
public:
    explicit FixedMonotonicObservations(
        sidecar::MonotonicObservation observation)
        : observation_(observation) {}

    std::optional<sidecar::MonotonicObservation> observe() noexcept override {
        return observation_;
    }

private:
    sidecar::MonotonicObservation observation_;
};

struct EndpointFenceProbe {
    size_t revalidations = 0;
    size_t releases = 0;
    size_t fences = 0;
    size_t successful_revalidations =
        std::numeric_limits<size_t>::max();
    int release_fd = -1;
    bool fenced = false;
};

class EndpointFenceLease final : public sidecar::P5coAdoptedSocketLease {
public:
    EndpointFenceLease(
        daemon::P50CacheSessionOutcome outcome,
        sidecar::AbsoluteMonotonicDeadline deadline,
        std::shared_ptr<EndpointFenceProbe> probe)
        : outcome_(std::move(outcome)), deadline_(deadline),
          probe_(std::move(probe)) {}

    bool revalidate(
        const daemon::P50CacheSessionOutcome& outcome,
        const sidecar::AbsoluteMonotonicDeadline& deadline) const noexcept override {
        ++probe_->revalidations;
        return !probe_->fenced && outcome == outcome_ && deadline == deadline_ &&
               probe_->revalidations <= probe_->successful_revalidations;
    }

    sidecar::P5coWriteResult send_nonblocking(
        std::span<const uint8_t> bytes, uint8_t flags) noexcept override {
        const uint8_t required =
            sidecar::P5coSendFlag::DontWait |
            sidecar::P5coSendFlag::NoSignal;
        if (bytes.empty() || flags != required || probe_->fenced)
            return {sidecar::P5coWriteKind::Error, 0};
        return {sidecar::P5coWriteKind::Sent, bytes.size()};
    }

    void fence() noexcept override {
        if (probe_->fenced)
            return;
        probe_->fenced = true;
        ++probe_->fences;
        if (probe_->release_fd >= 0) {
            (void)::close(probe_->release_fd);
            probe_->release_fd = -1;
        }
    }

private:
    int release_native_fd_for_endpoint(
        const daemon::P50CacheSessionOutcome& outcome,
        const sidecar::AbsoluteMonotonicDeadline& deadline) noexcept override {
        if (probe_->fenced || outcome != outcome_ || deadline != deadline_)
            return -1;
        ++probe_->releases;
        return std::exchange(probe_->release_fd, -1);
    }

    daemon::P50CacheSessionOutcome outcome_;
    sidecar::AbsoluteMonotonicDeadline deadline_;
    std::shared_ptr<EndpointFenceProbe> probe_;
};

void set_cloexec_or_fail(int fd, std::string_view detail) {
    const int flags = ::fcntl(fd, F_GETFD);
    require(flags >= 0 && ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0,
            detail);
}

struct P5coTcpPair {
    P5coTcpPair() {
        const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
        require(listener >= 0, "P5CO TCP listener creation failed");
        set_cloexec_or_fail(listener, "P5CO TCP listener CLOEXEC failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        require(::bind(listener, reinterpret_cast<const sockaddr*>(&address),
                       sizeof(address)) == 0 &&
                    ::listen(listener, 1) == 0,
                "P5CO TCP listener setup failed");
        socklen_t address_size = sizeof(address);
        require(::getsockname(listener, reinterpret_cast<sockaddr*>(&address),
                              &address_size) == 0,
                "P5CO TCP listener address failed");

        client = ::socket(AF_INET, SOCK_STREAM, 0);
        require(client >= 0, "P5CO TCP client creation failed");
        set_cloexec_or_fail(client, "P5CO TCP client CLOEXEC failed");
        require(::connect(client, reinterpret_cast<const sockaddr*>(&address),
                          sizeof(address)) == 0,
                "P5CO TCP client connect failed");
        server = ::accept(listener, nullptr, nullptr);
        const int accept_error = errno;
        (void)::close(listener);
        errno = accept_error;
        require(server >= 0, "P5CO TCP accept failed");
        set_cloexec_or_fail(server, "P5CO TCP accepted descriptor CLOEXEC failed");
    }

    ~P5coTcpPair() {
        if (server >= 0)
            (void)::close(server);
        if (client >= 0)
            (void)::close(client);
    }

    P5coTcpPair(const P5coTcpPair&) = delete;
    P5coTcpPair& operator=(const P5coTcpPair&) = delete;

    int take_server() noexcept { return std::exchange(server, -1); }
    int take_client() noexcept { return std::exchange(client, -1); }

    int server = -1;
    int client = -1;
};

void receive_exact_native(int fd, std::span<uint8_t> output) {
    size_t offset = 0;
    while (offset != output.size()) {
        const ssize_t count =
            ::recv(fd, output.data() + offset, output.size() - offset, 0);
        if (count < 0 && errno == EINTR)
            continue;
        require(count > 0, "P5CO peer did not receive the complete outcome");
        offset += static_cast<size_t>(count);
    }
}

sidecar::P5coEndpointHandoff flush_p5co_for_endpoint_with_observations(
    int server_fd, int client_fd,
    const daemon::P50CacheSessionOutcome& outcome,
    const sidecar::AbsoluteMonotonicDeadline& deadline,
    std::unique_ptr<sidecar::MonotonicObservationSource> observations) {
    auto lease = std::make_unique<sidecar::P5coRetainedSocketLease>(
        local::HandoffFd(server_fd), outcome, deadline);
    sidecar::AdoptedOutcomeWriter writer(
        std::move(lease), outcome, std::move(observations), deadline);
    const std::vector<uint8_t> expected(writer.canonical_frame().begin(),
                                        writer.canonical_frame().end());
    require(!expected.empty(), "P5CO endpoint writer encoded no outcome");
    for (size_t turn = 0;
         turn != 8 && writer.state() != sidecar::P5coWriterState::FullyFlushed;
         ++turn) {
        const sidecar::P5coWriterState state = writer.advance(POLLOUT);
        require(state == sidecar::P5coWriterState::Writing ||
                    state == sidecar::P5coWriterState::FullyFlushed,
                "P5CO endpoint writer did not make bounded progress");
    }
    require(writer.state() == sidecar::P5coWriterState::FullyFlushed,
            "P5CO endpoint writer did not fully flush");
    std::vector<uint8_t> observed(expected.size());
    receive_exact_native(client_fd, observed);
    require(observed == expected,
            "P5CO peer observed bytes other than the canonical outcome");
    const std::optional<daemon::P50CacheSessionOutcome> decoded =
        daemon::decode_cache_session_outcome(observed);
    require(decoded.has_value() && *decoded == outcome,
            "P5CO peer did not decode the exact adopted outcome");
    std::optional<sidecar::P5coEndpointHandoff> handoff =
        writer.take_for_endpoint();
    require(handoff.has_value() && handoff->valid(),
            "fully flushed P5CO did not yield an endpoint handoff");
    return std::move(*handoff);
}

sidecar::P5coEndpointHandoff flush_p5co_for_endpoint(
    int server_fd, int client_fd,
    const daemon::P50CacheSessionOutcome& outcome,
    const sidecar::AbsoluteMonotonicDeadline& deadline) {
    return flush_p5co_for_endpoint_with_observations(
        server_fd, client_fd, outcome, deadline,
        std::make_unique<sidecar::SystemMonotonicObservationSource>());
}

struct TestClient {
    explicit TestClient(CStoreGuid c_store_guid, EndpointCaps caps = {},
                        HistoryNonce first_history_nonce = HistoryNonce{1},
                        CompletionLog* completions = nullptr, ActionTrace* actions = nullptr,
                        PreparationAuthorityLimits authority_limits = {},
                        TuSeq first_tu_seq = {})
        : authority(std::make_shared<P50PreparationAuthority>(
              c_store_guid, caps.zstd, authority_limits, 1,
              caps.profile, first_tu_seq)),
          endpoint(authority, caps, first_history_nonce, completions, actions) {}

    operator P50ClientEndpoint&() { return endpoint; }

    PreparedTuHandle prepare(PrepareRequestKey request, std::span<const uint8_t> input) {
        return authority->prepare(request, input);
    }

    [[nodiscard]] CStoreGuid c_store_guid() const { return endpoint.c_store_guid(); }
    [[nodiscard]] bool has_active_transaction() const {
        return endpoint.has_active_transaction();
    }
    [[nodiscard]] bool has_reconciliation_work() const {
        return endpoint.has_reconciliation_work();
    }

    std::shared_ptr<P50PreparationAuthority> authority;
    P50ClientEndpoint endpoint;
};

PreparedTuHandle admit(TestClient& client, std::span<const uint8_t> input) {
    static uint64_t next_request_token = 1;
    return client.prepare(PrepareRequestKey{1, next_request_token++}, input);
}

std::vector<uint8_t> pseudo_random_bytes(size_t count) {
    std::vector<uint8_t> result(count);
    uint32_t state = 0x6d2b79f5U;
    for (uint8_t& byte : result) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        byte = static_cast<uint8_t>(state);
    }
    return result;
}

std::vector<uint8_t> drain_input(InputCursor& cursor, size_t chunk_size = 113) {
    std::vector<uint8_t> result;
    std::vector<uint8_t> chunk(chunk_size);
    while (!cursor.eof()) {
        const size_t count = cursor.read(chunk);
        require(count != 0, "non-EOF InputRecord cursor made no progress");
        result.insert(result.end(), chunk.begin(), chunk.begin() + count);
    }
    return result;
}

std::optional<std::vector<uint8_t>> copy_input(P50ServerEndpoint& server,
                                                CStoreGuid c_store_guid) {
    const std::optional<InputRecordKey> key =
        server.last_committed_input(c_store_guid);
    if (!key)
        return std::nullopt;
    try {
        InputCursor cursor = server.attach_input(*key);
        return drain_input(cursor);
    } catch (const std::logic_error&) {
        return std::nullopt;
    }
}

std::vector<uint8_t> standalone_zstd_frame(std::span<const uint8_t> input) {
    std::vector<uint8_t> result(ZSTD_compressBound(input.size()));
    const size_t encoded =
        ZSTD_compress(result.data(), result.size(), input.data(), input.size(), 1);
    if (ZSTD_isError(encoded))
        fail(std::string("standalone zstd fixture failed: ") + ZSTD_getErrorName(encoded));
    result.resize(encoded);
    return result;
}

bool nonzero(Digest128 digest) {
    return std::any_of(digest.bytes.begin(), digest.bytes.end(),
                       [](uint8_t byte) { return byte != 0; });
}

struct PairResult {
    ClientRunResult client;
    ServerRunResult server;
};

asio::awaitable<void> raw_stall_after_connect(tcp::acceptor& acceptor);

struct CompetingPairResult {
    ClientRunResult first_client;
    ClientRunResult second_client;
    ServerRunResult first_server;
    ServerRunResult second_server;
    P50ServerOwnerUsage usage;
};

PairResult run_pair(P50ClientEndpoint& client, P50ServerEndpoint& server,
                    PreparedTuHandle prepared = {}, EndpointIoControl client_control = {},
                    EndpointIoControl server_control = {}) {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    std::future<ServerRunResult> server_result =
        asio::co_spawn(context, server.accept_one(acceptor, server_control), asio::use_future);
    std::future<ClientRunResult> client_result = asio::co_spawn(
        context, client.run(acceptor.local_endpoint(), prepared, client_control),
        asio::use_future);
    context.run();
    return {client_result.get(), server_result.get()};
}

size_t staging_file_count();
uint64_t resident_bytes();
void report_resource_checkpoint(std::string_view label,
                                const P50ServerOwnerUsage& usage);

void test_zstd_route_endpoint_continuation_and_retry() {
    const P5coStoreGuids guids = p5co_store_guids(211);
    EndpointCaps caps;
    caps.profile = ProfileId::ZSTD_ROUTE;
    caps.zstd.max_raw_bytes = 1U << 20;
    caps.zstd.max_encoded_body_bytes = 1U << 20;
    P50ServerEndpoint server(guids.f, caps);
    TestClient client(guids.c, caps);
    const std::vector<uint8_t> first(64 * 1024, 0x41);
    const std::vector<uint8_t> second(64 * 1024, 0x41);
    const PreparedTuHandle first_prepared = admit(client, first);
    require_throws<std::logic_error>(
        [&] { (void)admit(client, second); },
        "ZSTD_ROUTE prepared a successor before its predecessor committed");
    const PairResult first_pair = run_pair(client, server, first_prepared);
    require(first_pair.client.status == ClientRunStatus::Committed &&
                first_pair.server.status == ServerRunStatus::Completed &&
                copy_input(server, guids.c) == first,
            "ZSTD_ROUTE first endpoint TU did not commit exact bytes");

    const PreparedTuHandle second_prepared = admit(client, second);
    const PairResult second_pair = run_pair(client, server, second_prepared);
    require(second_pair.client.status == ClientRunStatus::Committed &&
                second_pair.server.status == ServerRunStatus::Completed &&
                copy_input(server, guids.c) == second,
            "ZSTD_ROUTE second endpoint TU did not use the live route");

    const std::vector<uint8_t> failed(64 * 1024, 0x42);
    const PreparedTuHandle failed_prepared = admit(client, failed);
    EndpointIoControl disconnect;
    disconnect.close_before_write = MessageType::BODY;
    const PairResult disconnected =
        run_pair(client, server, failed_prepared, disconnect);
    require(disconnected.client.status == ClientRunStatus::Disconnected &&
                disconnected.server.status == ServerRunStatus::Disconnected &&
                client.has_active_transaction() &&
                client.endpoint.next_rel_seq().value == 2,
            "ZSTD_ROUTE failed TU did not retain exact retry identity");
    const PairResult retried = run_pair(client, server, failed_prepared);
    require(retried.client.status == ClientRunStatus::Committed &&
                retried.server.status == ServerRunStatus::Completed &&
                copy_input(server, guids.c) == failed,
            "ZSTD_ROUTE retry did not discard tentative state and commit exact bytes");
}

void test_zstd_route_authority_bounded_history() {
    EndpointCaps caps;
    caps.profile = ProfileId::ZSTD_ROUTE;
    caps.zstd.max_raw_bytes = 2U << 20;
    caps.zstd.max_encoded_body_bytes = 2U << 20;
    caps.zstd.max_window_log = 12;
    caps.zstd.max_history_bytes = 4U << 10;
    PreparationAuthorityLimits authority_limits;
    authority_limits.max_live_entries = 1;
    authority_limits.max_retained_encoded_bytes = 8U << 20;
    P50PreparationAuthority authority(Id128::from_u64(18001), caps.zstd,
                                      authority_limits, 3, ProfileId::ZSTD_ROUTE);

    for (uint64_t index = 0; index != 129; ++index) {
        const std::vector<uint8_t> input(1U << 20,
                                         static_cast<uint8_t>('A' + index % 17));
        const PreparedTuHandle handle =
            authority.prepare(PrepareRequestKey{180, index + 1}, input);
        require(authority.live_entry_count() == 1,
                "route authority did not retain its one active preparation");
        authority.commit(handle);
        require(authority.route_history_bytes() <= caps.zstd.max_history_bytes &&
                    authority.route_history_entries() <= 1,
                "route authority retained cumulative predecessor state");
        require(authority.release(handle) == 0 && authority.live_entry_count() == 0 &&
                    authority.retained_encoded_bytes() == 0,
                "route authority did not release the committed preparation");
    }
}

void test_server_routes_are_isolated_by_profile() {
    const P5coStoreGuids guids = p5co_store_guids(212);
    EndpointCaps server_caps;
    server_caps.supported_profiles = kOperationalProfileMask;
    ActionTrace actions;
    P50ServerEndpoint server(guids.f, server_caps, nullptr, &actions);

    EndpointCaps p29_caps;
    p29_caps.profile = ProfileId::P29V1;
    EndpointCaps zstd_caps;
    zstd_caps.profile = ProfileId::ZSTD_TU;
    TestClient p29(guids.c, p29_caps, HistoryNonce{1}, nullptr, &actions);
    // Both clients deliberately represent the same C store.  Give the second
    // preparation authority a disjoint C-wide TU sequence so this test probes
    // route identity rather than duplicate input-record admission.
    TestClient zstd(guids.c, zstd_caps, HistoryNonce{100}, nullptr, &actions,
                    {}, TuSeq{100});

    const PairResult first_p29 =
        run_pair(p29, server, admit(p29, bytes("profile-isolated p29 first\n")));
    const PairResult first_zstd =
        run_pair(zstd, server, admit(zstd, bytes("profile-isolated zstd\n")));
    const PairResult second_p29 =
        run_pair(p29, server, admit(p29, bytes("profile-isolated p29 second\n")));

    require(first_p29.client.status == ClientRunStatus::Committed &&
                first_p29.server.status == ServerRunStatus::Completed &&
                first_zstd.client.status == ClientRunStatus::Committed &&
                first_zstd.server.status == ServerRunStatus::Completed &&
                second_p29.client.status == ClientRunStatus::Committed &&
                second_p29.server.status == ServerRunStatus::Completed &&
                second_p29.client.reconnect ==
                    EndpointReconnectOutcome::ExactMatch,
            "a second profile replaced the retained route for the first profile");
    if (const auto error = check_action_trace(actions.records()))
        fail("P29V1/ZSTD_TU/P29V1 profile-isolation trace: " + *error);
    const auto zstd_action = std::find_if(
        actions.records().begin(), actions.records().end(),
        [](const ActionRecord& record) {
            return record.profile == ProfileId::ZSTD_TU;
        });
    require(zstd_action != actions.records().end(),
            "mixed-profile action trace omitted every ZSTD_TU record");
    const std::string zstd_json = zstd_action == actions.records().end()
        ? std::string() : action_jsonl(*zstd_action);
    require(zstd_json.find("\"profile\":\"zstd_tu\"") != std::string::npos,
            "action JSON omitted the exact route profile");
}

void test_p29v1_endpoint_route_dialogue_lifetime() {
    const P5coStoreGuids guids = p5co_store_guids(230);
    EndpointCaps caps;
    caps.profile = ProfileId::P29V1;
    caps.zstd.max_raw_bytes = 1U << 20;
    caps.zstd.max_encoded_body_bytes = 1U << 20;
    GlobalResourceTrace global_trace;
    P50ServerEndpointConfig config;
    config.global_resource_trace = &global_trace;
    P50ServerEndpoint server(guids.f, caps, nullptr, nullptr,
                             std::move(config));
    PreparationAuthorityLimits authority_limits;
    authority_limits.max_interner_reserved_bytes = UINT64_C(1044398080);
    TestClient client(guids.c, caps, HistoryNonce{1}, nullptr, nullptr,
                      authority_limits);
    const std::vector<uint8_t> repeated = bytes(
        "# 1 \"/tmp/p29v1-fixture.cc\"\n"
        "same-line\nsame-line\nsame-line\nsame-line\ntail\n");

    const PairResult first = run_pair(client, server, admit(client, repeated));
    const std::optional<std::vector<uint8_t>> first_input =
        copy_input(server, guids.c);
    if (first.client.status != ClientRunStatus::Committed ||
        first.server.status != ServerRunStatus::Completed ||
        first_input != repeated || client.endpoint.next_rel_seq().value != 1) {
        std::cerr << "P29V1 first diagnostic: client_status="
                  << static_cast<unsigned>(first.client.status)
                  << " server_status="
                  << static_cast<unsigned>(first.server.status)
                  << " exact=" << (first_input == repeated)
                  << " next_rel=" << client.endpoint.next_rel_seq().value;
        if (first.client.terminal_error)
            std::cerr << " client_error=" << first.client.terminal_error->detail;
        if (first.server.terminal_error)
            std::cerr << " server_error=" << first.server.terminal_error->detail;
        std::cerr << '\n';
    }
    require(first.client.status == ClientRunStatus::Committed &&
                first.server.status == ServerRunStatus::Completed &&
                first_input == repeated &&
                client.endpoint.next_rel_seq().value == 1,
            "P29V1 endpoint first TU did not commit exact route state");
    std::optional<size_t> segment_present;
    std::optional<size_t> blob_present;
    Key64 segment_key;
    Key64 blob_key;
    for (size_t index = 0; index < global_trace.records().size(); ++index) {
        const GlobalActionRecord& record = global_trace.records()[index];
        if (record.action != GlobalActionType::ARENA_PRESENT)
            continue;
        if (record.key.type() == ObjectType::P29Segment) {
            segment_present = index;
            segment_key = record.key;
        } else if (record.key.type() == ObjectType::Blob) {
            blob_present = index;
            blob_key = record.key;
        }
    }
    require(segment_present && blob_present &&
                *segment_present < *blob_present &&
                segment_key.generation() == blob_key.generation() &&
                segment_key.ordinal() == blob_key.ordinal(),
            "P29V1 did not publish its same-TU segment before the input Blob");

    require(first.server.committed_input.has_value(),
            "P29V1 open job did not expose its committed input lease");
    server.close_input_job(*first.server.committed_input);
    server.collect_input_garbage();
    require(server.owner_usage().retained_input_records == 0 &&
                server.owner_usage().retained_input_bytes == 0 &&
                !server.global_resource_invariant_for_test(),
            "P29V1 Blob collection rejected its surviving route segment");
    const auto first_segment_release = std::find_if(
        global_trace.records().begin(), global_trace.records().end(),
        [segment_key](const GlobalActionRecord& record) {
            return record.action == GlobalActionType::ARENA_RELEASED &&
                   record.key == segment_key;
        });
    require(first_segment_release == global_trace.records().end(),
            "P29V1 Blob collection prematurely released route dictionary state");

    const PairResult second = run_pair(client, server, admit(client, repeated));
    require(second.client.status == ClientRunStatus::Committed &&
                second.server.status == ServerRunStatus::Completed &&
                copy_input(server, guids.c) == repeated &&
                client.endpoint.next_rel_seq().value == 2,
            "P29V1 endpoint same-route TU2 lost the committed codec state");

    const std::vector<uint8_t> retry_input = bytes(
        "# 1 \"/tmp/p29v1-retry.cc\"\nretry-v1\nretry-v1\n");
    const PreparedTuHandle retry_handle = admit(client, retry_input);
    EndpointIoControl disconnect;
    disconnect.close_before_write = MessageType::FILL;
    const PairResult interrupted =
        run_pair(client, server, retry_handle, disconnect);
    require(interrupted.client.status == ClientRunStatus::Disconnected &&
                interrupted.server.status == ServerRunStatus::Disconnected &&
                client.has_active_transaction() &&
                client.endpoint.next_rel_seq().value == 2,
            "P29V1 interrupted FILL did not retain exact retry identity");
    const PairResult retried = run_pair(client, server, retry_handle);
    const std::optional<std::vector<uint8_t>> retried_input =
        copy_input(server, guids.c);
    if (retried.client.status != ClientRunStatus::Committed ||
        retried.server.status != ServerRunStatus::Completed ||
        retried_input != retry_input ||
        client.endpoint.next_rel_seq().value != 3) {
        std::cerr << "P29V1 retry diagnostic: client_status="
                  << static_cast<unsigned>(retried.client.status)
                  << " server_status="
                  << static_cast<unsigned>(retried.server.status)
                  << " exact=" << (retried_input == retry_input)
                  << " next_rel=" << client.endpoint.next_rel_seq().value;
        if (retried.client.terminal_error)
            std::cerr << " client_error="
                      << retried.client.terminal_error->detail;
        if (retried.server.terminal_error)
            std::cerr << " server_error="
                      << retried.server.terminal_error->detail;
        std::cerr << '\n';
    }
    require(retried.client.status == ClientRunStatus::Committed &&
                retried.server.status == ServerRunStatus::Completed &&
                retried_input == retry_input &&
                client.endpoint.next_rel_seq().value == 3,
            "P29V1 exact retry did not reproduce and commit the input");

    server.reset_store(
        FStoreGuid::from_u64(UINT64_C(0x5032395631525354)));
    const std::vector<uint8_t> after_reset = bytes("after-p29v1-reset\n");
    const PairResult reset =
        run_pair(client, server, admit(client, after_reset));
    const std::optional<std::vector<uint8_t>> reset_input =
        copy_input(server, guids.c);
    if (reset.client.status != ClientRunStatus::Committed ||
        reset.client.reconnect != EndpointReconnectOutcome::ColdFStore ||
        reset.server.status != ServerRunStatus::Completed ||
        reset_input != after_reset || client.endpoint.next_rel_seq().value != 1) {
        std::cerr << "P29V1 reset diagnostic: client_status="
                  << static_cast<unsigned>(reset.client.status)
                  << " reconnect="
                  << static_cast<unsigned>(reset.client.reconnect)
                  << " server_status="
                  << static_cast<unsigned>(reset.server.status)
                  << " exact=" << (reset_input == after_reset)
                  << " next_rel=" << client.endpoint.next_rel_seq().value;
        if (reset.client.terminal_error)
            std::cerr << " client_error=" << reset.client.terminal_error->detail;
        if (reset.server.terminal_error)
            std::cerr << " server_error=" << reset.server.terminal_error->detail;
        std::cerr << '\n';
    }
    require(reset.client.status == ClientRunStatus::Committed &&
                reset.client.reconnect == EndpointReconnectOutcome::ColdFStore &&
                reset.server.status == ServerRunStatus::Completed &&
                reset_input == after_reset &&
                client.endpoint.next_rel_seq().value == 1,
            "P29V1 cold-store reset did not rebuild both codec routes");

    GlobalResourceTrace closed_trace;
    P50ServerEndpointConfig closed_config;
    closed_config.global_resource_trace = &closed_trace;
    closed_config.input_job_state =
        [](CStoreGuid, const TxBegin&, const TxCommit&,
           std::span<const uint8_t>) { return InputJobState::Closed; };
    const P5coStoreGuids closed_guids = p5co_store_guids(231);
    P50ServerEndpoint closed_server(closed_guids.f, caps, nullptr, nullptr,
                                    std::move(closed_config));
    TestClient closed_client(closed_guids.c, caps, HistoryNonce{1}, nullptr,
                             nullptr, authority_limits);
    const PairResult closed = run_pair(
        closed_client, closed_server, admit(closed_client, repeated));
    require(closed.client.status == ClientRunStatus::Committed &&
                closed.server.status == ServerRunStatus::Completed &&
                closed.server.completed_input.has_value() &&
                !closed.server.committed_input &&
                closed_server.owner_usage().retained_input_records == 0 &&
                !closed_server.global_resource_invariant_for_test(),
            "P29V1 closed job did not commit without a resident Blob");
    const auto closed_segment_present = std::find_if(
        closed_trace.records().begin(), closed_trace.records().end(),
        [](const GlobalActionRecord& record) {
            return record.action == GlobalActionType::ARENA_PRESENT &&
                   record.key.type() == ObjectType::P29Segment;
        });
    const auto closed_blob_crashed = std::find_if(
        closed_trace.records().begin(), closed_trace.records().end(),
        [](const GlobalActionRecord& record) {
            return record.action == GlobalActionType::INSTALL_CRASHED &&
                   record.key.type() == ObjectType::Blob;
        });
    const auto closed_blob_present = std::find_if(
        closed_trace.records().begin(), closed_trace.records().end(),
        [](const GlobalActionRecord& record) {
            return record.action == GlobalActionType::ARENA_PRESENT &&
                   record.key.type() == ObjectType::Blob;
        });
    require(closed_segment_present != closed_trace.records().end() &&
                closed_blob_crashed != closed_trace.records().end() &&
                closed_blob_present == closed_trace.records().end(),
            "P29V1 closed job did not publish Segment and crash Blob");
    closed_server.collect_input_garbage();
    require(!closed_server.global_resource_invariant_for_test(),
            "P29V1 closed-job garbage collection broke global invariants");

    GlobalResourceTrace capped_trace;
    P50ServerEndpointConfig capped_config;
    capped_config.global_resource_trace = &capped_trace;
    const P5coStoreGuids capped_guids = p5co_store_guids(232);
    P50ServerEndpoint capped_server(capped_guids.f, caps, nullptr, nullptr,
                                    std::move(capped_config));
    PreparationAuthorityLimits capped_limits = authority_limits;
    capped_limits.max_route_state_bytes = 1;
    TestClient capped_client(capped_guids.c, caps, HistoryNonce{1}, nullptr,
                             nullptr, capped_limits);
    const PairResult capped = run_pair(
        capped_client, capped_server, admit(capped_client, repeated));
    require(capped.client.status == ClientRunStatus::TerminalError &&
                capped.server.status == ServerRunStatus::Disconnected &&
                capped.client.terminal_error.has_value() &&
                capped.client.terminal_error->detail.find(
                    "P29V1 route state exceeds its budget") !=
                    std::string::npos &&
                capped_server.owner_usage().retained_input_records == 0 &&
                !capped_server.global_resource_invariant_for_test(),
            "P29V1 projected route cap did not reject before FILL publication");
    const bool capped_published = std::any_of(
        capped_trace.records().begin(), capped_trace.records().end(),
        [](const GlobalActionRecord& record) {
            return record.action == GlobalActionType::ARENA_PRESENT;
        });
    require(!capped_published,
            "P29V1 projected route-cap failure published receiver state");
}

void test_profile_materialized_result_digest_gates() {
    uint64_t identity = 19000;
    for (const ProfileId profile : {ProfileId::P29V1, ProfileId::ZSTD_TU,
                                    ProfileId::ZSTD_ROUTE}) {
        EndpointCaps caps;
        caps.profile = profile;
        caps.zstd.max_raw_bytes = 1U << 20;
        caps.zstd.max_encoded_body_bytes = 1U << 20;
        const CStoreGuid c_guid = Id128::from_u64(identity++);
        P50ServerEndpoint server(Id128::from_u64(identity++), caps);
        TestClient client(c_guid, caps);
        const std::vector<uint8_t> input = bytes(
            "# 1 \"/tmp/profile-digest-mutant.cc\"\n"
            "same materialized bytes\nsame materialized bytes\n");
        const PreparedTuHandle prepared = admit(client, input);
        EndpointIoControl corrupt;
        corrupt.outbound_begin_transform =
            [](const TxBegin& original, std::span<const uint8_t> body) {
                TxBegin changed = original;
                changed.raw_digest.bytes[0] ^= 0x80;
                changed.transaction_digest =
                    compute_transaction_digest(changed, body);
                return changed;
            };
        const PairResult rejected =
            run_pair(client, server, prepared, corrupt);
        require(rejected.server.status == ServerRunStatus::TerminalError &&
                    !copy_input(server, c_guid).has_value(),
                std::string(profile_name(profile)) +
                    " accepted or published materialized bytes whose digest differed");
    }
}

void test_s3_resource_storm_product_path() {
    const char* requested = std::getenv("ICECC_P50_S3_RESOURCE_STORM");
    const bool storm_only = requested != nullptr &&
                            std::string_view(requested) == "storm";
    P50ServerEndpointConfig soak_config;
    soak_config.owner_limits.max_namespaces = 4;
    soak_config.owner_limits.max_retained_input_records = 1;
    soak_config.owner_limits.max_retained_input_bytes = 4096;
    P50ServerEndpoint soak_server(Id128::from_u64(12000), {}, nullptr, nullptr,
                                  std::move(soak_config));
    TestClient soak_client(Id128::from_u64(12001));
    const uint64_t soak_iterations = storm_only ? 0 : 10000;
    for (uint64_t index = 0; index != soak_iterations; ++index) {
        const std::vector<uint8_t> input{
            static_cast<uint8_t>(index),
            static_cast<uint8_t>(index >> 8),
            static_cast<uint8_t>(index >> 16),
            static_cast<uint8_t>(index >> 24)};
        const PreparedTuHandle prepared = admit(soak_client, input);
        const PairResult result = run_pair(soak_client, soak_server, prepared);
        require(result.client.status == ClientRunStatus::Committed &&
                    result.server.status == ServerRunStatus::Completed &&
                    result.server.committed_input.has_value() &&
                    copy_input(soak_server, soak_client.c_store_guid()) == input,
                "10,000-TU product soak failed exact endpoint commit");
        soak_server.close_input_job(*result.server.committed_input);
        soak_server.collect_input_garbage();
        require(soak_client.authority->release(prepared) == 0,
                "10,000-TU product soak retained a producer preparation");
        require(soak_server.owner_usage().retained_input_records == 0 &&
                    soak_server.owner_usage().retained_input_bytes == 0,
                "10,000-TU product soak retained a released input");
        if ((index + 1) % 2500 == 0)
            report_resource_checkpoint("soak-" + std::to_string(index + 1),
                                       soak_server.owner_usage());
    }
    if (!storm_only)
        report_resource_checkpoint("soak-final", soak_server.owner_usage());

    P50ServerEndpointConfig storm_config;
    storm_config.owner_limits.max_namespaces = 256;
    storm_config.owner_limits.max_retained_input_records = 8;
    storm_config.owner_limits.max_retained_input_bytes = 8 * 64;
    P50ServerEndpoint storm_server(Id128::from_u64(13000), {}, nullptr, nullptr,
                                   std::move(storm_config));
    std::vector<std::unique_ptr<TestClient>> pinned_clients;
    std::vector<InputCursor> pinned_cursors;
    pinned_clients.reserve(8);
    pinned_cursors.reserve(8);
    for (uint64_t index = 0; index != 8; ++index) {
        auto client = std::make_unique<TestClient>(Id128::from_u64(13100 + index));
        const std::vector<uint8_t> input(64, static_cast<uint8_t>(index + 1));
        const PairResult result =
            run_pair(*client, storm_server, admit(*client, input));
        require(result.client.status == ClientRunStatus::Committed &&
                    result.server.status == ServerRunStatus::Completed &&
                    result.server.committed_input.has_value(),
                "multi-C_GUID storm failed initial product commit");
        pinned_cursors.emplace_back(
            storm_server.attach_input(*result.server.committed_input));
        storm_server.close_input_job(*result.server.committed_input);
        pinned_clients.emplace_back(std::move(client));
    }
    report_resource_checkpoint("storm-pinned", storm_server.owner_usage());

    auto blocked_client = std::make_unique<TestClient>(Id128::from_u64(13200));
    const std::vector<uint8_t> blocked_input(64, 0xa5);
    const PreparedTuHandle blocked_prepared = admit(*blocked_client, blocked_input);
    const PairResult blocked = run_pair(*blocked_client, storm_server,
                                        blocked_prepared);
    require(blocked.client.status == ClientRunStatus::TerminalError &&
                blocked.server.status == ServerRunStatus::TerminalError &&
                blocked_client->has_active_transaction() &&
                storm_server.owner_usage().retained_input_records == 8 &&
                storm_server.owner_usage().retained_input_bytes == 8 * 64,
            "cursor-pinned product capacity failure changed retained state");
    report_resource_checkpoint("storm-failed-install", storm_server.owner_usage());

    pinned_cursors.clear();
    const PairResult recovered = run_pair(*blocked_client, storm_server);
    require(recovered.client.status == ClientRunStatus::Committed &&
                recovered.server.status == ServerRunStatus::Completed &&
                recovered.client.reconnect == EndpointReconnectOutcome::ExactMatch &&
                recovered.server.committed_input.has_value() &&
                copy_input(storm_server, blocked_client->c_store_guid()) == blocked_input,
            "failed product install did not retry exact bytes after cursor release");
    storm_server.close_input_job(*recovered.server.committed_input);
    report_resource_checkpoint("storm-recovered", storm_server.owner_usage());

    for (uint64_t index = 0; index != 512; ++index) {
        TestClient client(Id128::from_u64(14000 + index));
        const std::vector<uint8_t> input(64, static_cast<uint8_t>(index));
        const PairResult result =
            run_pair(client, storm_server, admit(client, input));
        require(result.client.status == ClientRunStatus::Committed &&
                    result.server.status == ServerRunStatus::Completed &&
                    result.server.committed_input.has_value(),
                "multi-C_GUID product eviction storm failed a commit");
        storm_server.close_input_job(*result.server.committed_input);
        require(storm_server.owner_usage().retained_input_records <= 8 &&
                    storm_server.owner_usage().retained_input_bytes <= 8 * 64,
                "multi-C_GUID product eviction storm exceeded retained bounds");
        if ((index + 1) % 128 == 0)
            report_resource_checkpoint("storm-" + std::to_string(index + 1),
                                       storm_server.owner_usage());
    }
    require(storm_server.owner_usage().namespaces <= 256,
            "multi-C_GUID product storm exceeded the namespace cap");
    report_resource_checkpoint("storm-final", storm_server.owner_usage());
}

void test_live_global_resource_trace() {
    GlobalResourceTrace trace;
    P50ServerEndpointConfig config;
    config.global_resource_trace = &trace;
    P50ServerEndpoint server(Id128::from_u64(15000), {}, nullptr, nullptr,
                             std::move(config));
    TestClient client(Id128::from_u64(15001));
    const std::vector<uint8_t> input = bytes("live endpoint global trace\n");
    const PairResult result = run_pair(client, server, admit(client, input));
    require(result.client.status == ClientRunStatus::Committed &&
                result.server.status == ServerRunStatus::Completed &&
                result.server.committed_input.has_value() &&
                copy_input(server, client.c_store_guid()) == input,
            "live global trace transaction did not commit exact input");
    server.close_input_job(*result.server.committed_input);
    server.collect_input_garbage();
    server.reset_store(Id128::from_u64(15002));

    const auto action_index = [&](GlobalActionType action) {
        const auto position = std::find_if(
            trace.records().begin(), trace.records().end(),
            [action](const GlobalActionRecord& record) {
                return record.action == action;
            });
        require(position != trace.records().end(),
                std::string("live endpoint omitted global action ") +
                    std::string(global_action_name(action)));
        return static_cast<size_t>(position - trace.records().begin());
    };
    const size_t admitted = action_index(GlobalActionType::NAMESPACE_ADMITTED);
    const size_t started = action_index(GlobalActionType::TU_STARTED);
    const size_t installing = action_index(GlobalActionType::ARENA_INSTALLING);
    const size_t present = action_index(GlobalActionType::ARENA_PRESENT);
    const size_t finished = action_index(GlobalActionType::TU_FINISHED);
    const size_t released = action_index(GlobalActionType::ARENA_RELEASED);
    const size_t evicted = action_index(GlobalActionType::NAMESPACE_EVICTED);
    require(admitted < started && started < installing && installing < present &&
                present < finished && finished < released && released < evicted,
            "live endpoint global trace lifecycle order is invalid");

    if (const char* path = std::getenv("P50_ENDPOINT_GLOBAL_TRACE_PATH"))
        write_global_trace(trace, path);
}

void test_automatic_action_trace_is_complete_past_1024_records() {
    char trace_path[] = "/tmp/p50endpoint-f-action-trace-XXXXXX";
    const int trace_fd = ::mkstemp(trace_path);
    require(trace_fd >= 0, "automatic action-trace fixture could not create a path");
    require(::close(trace_fd) == 0 && ::unlink(trace_path) == 0,
            "automatic action-trace fixture could not prepare an absent path");
    require(::setenv("ICECC_P50_F_ACTION_TRACE", trace_path, 1) == 0,
            "automatic action-trace fixture could not enable the F sink");

    {
        P50ServerEndpoint server(Id128::from_u64(15101));
        TestClient client(Id128::from_u64(15102));
        for (uint64_t index = 0; index != 180; ++index) {
            const std::vector<uint8_t> input{
                static_cast<uint8_t>(index),
                static_cast<uint8_t>(index >> 8),
                0x50,
                0x38,
            };
            const PreparedTuHandle prepared = admit(client, input);
            const PairResult result = run_pair(client, server, prepared);
            require(result.client.status == ClientRunStatus::Committed &&
                        result.server.status == ServerRunStatus::Completed &&
                        result.server.committed_input.has_value(),
                    "automatic action-trace fixture failed a product transaction");
            server.close_input_job(*result.server.committed_input);
            server.collect_input_garbage();
            require(client.authority->release(prepared) == 0,
                    "automatic action-trace fixture retained a preparation");
        }
    }

    require(::unsetenv("ICECC_P50_F_ACTION_TRACE") == 0,
            "automatic action-trace fixture could not disable the F sink");
    std::ifstream trace(trace_path, std::ios::binary);
    require(static_cast<bool>(trace),
            "automatic action-trace fixture did not create its requested trace");
    const std::string contents((std::istreambuf_iterator<char>(trace)),
                               std::istreambuf_iterator<char>());
    require(static_cast<size_t>(std::count(contents.begin(), contents.end(), '\n')) > 1024 &&
                contents.find("\"tu_seq\":179") != std::string::npos,
            "automatically owned action trace truncated after 1024 records");
    require(::unlink(trace_path) == 0,
            "automatic action-trace fixture could not remove its trace");
}

bool s3_resource_storm_requested() {
    const char* value = std::getenv("ICECC_P50_S3_RESOURCE_STORM");
    return value != nullptr &&
           (std::string_view(value) == "1" || std::string_view(value) == "storm");
}

PairResult run_adopted_pair(P50ClientEndpoint& client, P50ServerEndpoint& server,
                            PreparedTuHandle prepared, bool use_native_adoption,
                            int* consumed_fd) {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    std::future<ServerRunResult> server_result = asio::co_spawn(
        context,
        [&]() -> asio::awaitable<ServerRunResult> {
            const auto executor = co_await asio::this_coro::executor;
            tcp::socket accepted(executor);
            boost::system::error_code error;
            co_await acceptor.async_accept(
                accepted, asio::redirect_error(asio::use_awaitable, error));
            require(!error, "loopback accept for adopted endpoint failed");
            const int accepted_fd = accepted.native_handle();
            if (!use_native_adoption) {
                if (consumed_fd != nullptr)
                    *consumed_fd = accepted_fd;
                co_return co_await server.run_adopted(std::move(accepted));
            }

            const int duplicate_fd = ::dup(accepted_fd);
            require(duplicate_fd >= 0, "duplicating accepted fd failed");
            boost::system::error_code adopt_error;
            auto adopted = P50ServerEndpoint::adopt_connected_fd(
                executor, duplicate_fd, adopt_error);
            require(adopted.has_value() && !adopt_error,
                    "native accepted fd was not adopted");
            require((::fcntl(adopted->native_handle(), F_GETFD) & FD_CLOEXEC) != 0,
                    "native adoption did not establish CLOEXEC");
            tcp::no_delay no_delay;
            adopted->get_option(no_delay, adopt_error);
            require(!adopt_error && no_delay.value(),
                    "native adoption did not disable Nagle");
            if (consumed_fd != nullptr)
                *consumed_fd = adopted->native_handle();
            boost::system::error_code close_error;
            accepted.close(close_error);
            co_return co_await server.run_adopted(std::move(*adopted));
        },
        asio::use_future);
    std::future<ClientRunResult> client_result = asio::co_spawn(
        context, client.run(acceptor.local_endpoint(), prepared), asio::use_future);
    context.run();
    return {client_result.get(), server_result.get()};
}

void require_closed_fd(int fd, std::string_view detail) {
    errno = 0;
    require(fd >= 0 && ::fcntl(fd, F_GETFD) < 0 && errno == EBADF, detail);
}

size_t open_fd_count() {
    DIR* directory = ::opendir("/proc/self/fd");
    require(directory != nullptr, "could not open /proc/self/fd");
    size_t count = 0;
    while (const dirent* entry = ::readdir(directory)) {
        if (std::string_view(entry->d_name) != "." &&
            std::string_view(entry->d_name) != "..")
            ++count;
    }
    require(::closedir(directory) == 0, "could not close /proc/self/fd");
    return count;
}

size_t staging_file_count() {
    std::error_code error;
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path(error);
    if (error)
        return 0;
    size_t count = 0;
    std::filesystem::directory_iterator entries(directory, error);
    for (const auto& entry : entries) {
        if (error)
            break;
        const std::string name = entry.path().filename().string();
        if (name.find("icecc") == std::string::npos &&
            name.find("p50") == std::string::npos)
            continue;
        std::error_code status_error;
        if (entry.is_regular_file(status_error) && !status_error)
            ++count;
    }
    return count;
}

uint64_t resident_bytes() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (!line.starts_with("VmRSS:"))
            continue;
        std::istringstream fields(line);
        std::string key;
        std::string unit;
        uint64_t value = 0;
        if (fields >> key >> value >> unit && key == "VmRSS:" && unit == "kB")
            return value * 1024;
        return 0;
    }
    return 0;
}

void report_resource_checkpoint(std::string_view label,
                                const P50ServerOwnerUsage& usage) {
    const uint64_t rss = resident_bytes();
    require(rss != 0, "could not read the product-gate RSS checkpoint");
    std::cerr << "p50_s3_resource checkpoint=" << label
              << " retained_records=" << usage.retained_input_records
              << " retained_bytes=" << usage.retained_input_bytes
              << " namespaces=" << usage.namespaces
              << " staging_files=" << staging_file_count()
              << " open_fds=" << open_fd_count()
              << " rss_bytes=" << rss << '\n';
}

void test_complete_p5co_endpoint_handoff() {
    const P5coStoreGuids guids = p5co_store_guids(71);
    const daemon::P50CacheSessionOutcome outcome =
        p5co_adopted_outcome(guids, 9101);
    const sidecar::AbsoluteMonotonicDeadline deadline =
        p5co_deadline_after(std::chrono::seconds(5));
    CompletionLog completions;
    P50ServerEndpoint server(guids.f, {}, &completions);
    TestClient client(guids.c);
    const std::vector<uint8_t> input = pseudo_random_bytes(32 * 1024);
    const PreparedTuHandle prepared = admit(client, input);

    P5coTcpPair pair;
    const int owned_server_fd = pair.server;
    const int owned_client_fd = pair.client;
    sidecar::P5coEndpointHandoff handoff = flush_p5co_for_endpoint(
        pair.take_server(), pair.client, outcome, deadline);

    asio::io_context context;
    boost::system::error_code adoption_error;
    std::optional<tcp::socket> client_socket =
        P50ClientEndpoint::adopt_connected_fd(
            context.get_executor(), pair.take_client(), adoption_error);
    require(client_socket.has_value() && !adoption_error,
            "P5CO client descriptor adoption failed");
    std::future<ServerRunResult> server_result = asio::co_spawn(
        context, server.run_adopted(std::move(handoff)), asio::use_future);
    std::future<ClientRunResult> client_result = asio::co_spawn(
        context,
        client.endpoint.run(std::move(*client_socket), prepared, {},
                            deadline.as_steady_time_point()),
        asio::use_future);
    context.run();

    const ClientRunResult client_value = client_result.get();
    const ServerRunResult server_value = server_result.get();
    require(client_value.status == ClientRunStatus::Committed &&
                server_value.status == ServerRunStatus::Completed &&
                server_value.c_store_guid == guids.c &&
                server_value.committed_input.has_value() &&
                copy_input(server, guids.c) == input &&
                server.live_session_count() == 0,
            "complete P5CO endpoint handoff did not commit exact input");
    require(!completions.completions().empty() &&
                completions.completions().front().stamp.operation ==
                    AsyncOperationKind::ReadHeader &&
                std::all_of(completions.completions().begin(),
                            completions.completions().end(),
                            [&](const AsyncCompletion& completion) {
                                return completion.stamp.cache_session_operation ==
                                           outcome.operation &&
                                       completion.stamp.absolute_deadline ==
                                           deadline;
                            }) &&
                std::none_of(completions.completions().begin(),
                             completions.completions().end(),
                             [](const AsyncCompletion& completion) {
                                 return completion.stamp.operation ==
                                        AsyncOperationKind::Accept;
                             }),
            "P5CO endpoint performed work before direct socket ownership");
    require_closed_fd(owned_server_fd,
                      "P5CO endpoint did not close the retained server fd");
    require_closed_fd(owned_client_fd,
                      "P5CO client did not close its adopted fd");
    require(context.poll() == 0,
            "complete P5CO endpoint run retained a timer or socket handler");
}

void test_p5co_endpoint_absolute_deadline_and_binding() {
    {
        const P5coStoreGuids guids = p5co_store_guids(91);
        const daemon::P50CacheSessionOutcome outcome =
            p5co_adopted_outcome(guids, 9201);
        const sidecar::AbsoluteMonotonicDeadline deadline =
            p5co_deadline_after(std::chrono::milliseconds(150));
        CompletionLog completions;
        P50ServerEndpoint server(guids.f, {}, &completions);
        P5coTcpPair pair;
        const int owned_server_fd = pair.server;
        sidecar::P5coEndpointHandoff handoff = flush_p5co_for_endpoint(
            pair.take_server(), pair.client, outcome, deadline);

        asio::io_context context;
        bool external_watchdog_fired = false;
        asio::steady_timer external_watchdog(context);
        external_watchdog.expires_after(std::chrono::milliseconds(500));
        external_watchdog.async_wait([&](const boost::system::error_code& error) {
            if (!error) {
                external_watchdog_fired = true;
                (void)::shutdown(pair.client, SHUT_RDWR);
            }
        });
        const auto started = std::chrono::steady_clock::now();
        std::promise<ServerRunResult> completion;
        std::future<ServerRunResult> result = completion.get_future();
        asio::co_spawn(context, server.run_adopted(std::move(handoff)),
            [&](std::exception_ptr error, ServerRunResult value) {
                if (error)
                    completion.set_exception(error);
                else
                    completion.set_value(std::move(value));
                boost::system::error_code ignored;
                external_watchdog.cancel(ignored);
            });
        context.run();
        const auto elapsed = std::chrono::steady_clock::now() - started;
        require(result.get().status == ServerRunStatus::DeadlineExceeded &&
                    !external_watchdog_fired &&
                    elapsed < std::chrono::seconds(2) &&
                    server.live_session_count() == 0,
                "P5CO stalled CacheWire read did not expire before external watchdog");
        require(!completions.completions().empty() &&
                    completions.completions().front().stamp.operation ==
                        AsyncOperationKind::ReadHeader,
                "P5CO endpoint did not arm its deadline before the first read");
        require_closed_fd(owned_server_fd,
                          "expired P5CO endpoint retained its server fd");
        require(context.poll() == 0,
                "expired P5CO endpoint retained a timer or socket handler");
    }

    {
        const P5coStoreGuids guids = p5co_store_guids(101);
        const daemon::P50CacheSessionOutcome outcome =
            p5co_adopted_outcome(guids, 9251);
        sidecar::SystemMonotonicObservationSource system_observations;
        const std::optional<sidecar::MonotonicObservation> system_now =
            system_observations.observe();
        require(system_now.has_value() && system_now->valid(),
                "wrong-clock P5CO setup could not observe CLOCK_MONOTONIC");
        uint64_t wrong_namespace =
            system_now->clock.time_namespace_id + 1;
        if (wrong_namespace == 0)
            wrong_namespace = 1;
        const sidecar::MonotonicClockIdentity wrong_clock{
            system_now->clock.clock_domain_id, wrong_namespace};
        const sidecar::AbsoluteMonotonicDeadline wrong_deadline{
            system_now->now_ns + 2000000000LL,
            wrong_clock.clock_domain_id,
            wrong_clock.time_namespace_id};
        P50ServerEndpoint server(guids.f);
        P5coTcpPair pair;
        const int owned_server_fd = pair.server;
        sidecar::P5coEndpointHandoff handoff =
            flush_p5co_for_endpoint_with_observations(
                pair.take_server(), pair.client, outcome, wrong_deadline,
                std::make_unique<FixedMonotonicObservations>(
                    sidecar::MonotonicObservation{system_now->now_ns,
                                                  wrong_clock}));

        asio::io_context context;
        std::future<ServerRunResult> result = asio::co_spawn(
            context, server.run_adopted(std::move(handoff)), asio::use_future);
        context.run();
        require(result.get().status == ServerRunStatus::DeadlineExceeded &&
                    server.live_session_count() == 0,
                "P5CO endpoint accepted another CLOCK_MONOTONIC domain");
        require_closed_fd(owned_server_fd,
                          "wrong-clock P5CO endpoint did not fence its fd");
        require(context.poll() == 0,
                "wrong-clock P5CO endpoint retained a handler");
    }

    {
        const P5coStoreGuids claim_guids = p5co_store_guids(111);
        const P5coStoreGuids other_guids = p5co_store_guids(131);
        const daemon::P50CacheSessionOutcome outcome =
            p5co_adopted_outcome(claim_guids, 9301);
        const sidecar::AbsoluteMonotonicDeadline deadline =
            p5co_deadline_after(std::chrono::seconds(2));
        P50ServerEndpoint wrong_server(other_guids.f);
        P5coTcpPair pair;
        const int owned_server_fd = pair.server;
        sidecar::P5coEndpointHandoff handoff = flush_p5co_for_endpoint(
            pair.take_server(), pair.client, outcome, deadline);

        asio::io_context context;
        std::future<ServerRunResult> result = asio::co_spawn(
            context, wrong_server.run_adopted(std::move(handoff)),
            asio::use_future);
        context.run();
        require(result.get().status == ServerRunStatus::Disconnected &&
                    wrong_server.live_session_count() == 0,
                "P5CO endpoint accepted another F-store binding");
        require_closed_fd(owned_server_fd,
                          "wrong-F P5CO endpoint did not fence its retained fd");
        require(context.poll() == 0,
                "wrong-F P5CO endpoint retained an asynchronous handler");
    }

    {
        const P5coStoreGuids claim_guids = p5co_store_guids(151);
        const P5coStoreGuids other_guids = p5co_store_guids(171);
        const daemon::P50CacheSessionOutcome outcome =
            p5co_adopted_outcome(claim_guids, 9401);
        const sidecar::AbsoluteMonotonicDeadline deadline =
            p5co_deadline_after(std::chrono::seconds(2));
        P50ServerEndpoint server(claim_guids.f);
        TestClient wrong_client(other_guids.c);
        const PreparedTuHandle prepared =
            admit(wrong_client, bytes("wrong claimed C identity\n"));
        P5coTcpPair pair;
        const int owned_server_fd = pair.server;
        const int owned_client_fd = pair.client;
        sidecar::P5coEndpointHandoff handoff = flush_p5co_for_endpoint(
            pair.take_server(), pair.client, outcome, deadline);

        asio::io_context context;
        boost::system::error_code adoption_error;
        std::optional<tcp::socket> client_socket =
            P50ClientEndpoint::adopt_connected_fd(
                context.get_executor(), pair.take_client(), adoption_error);
        require(client_socket.has_value() && !adoption_error,
                "wrong-C P5CO client descriptor adoption failed");
        std::future<ServerRunResult> server_result = asio::co_spawn(
            context, server.run_adopted(std::move(handoff)), asio::use_future);
        std::future<ClientRunResult> client_result = asio::co_spawn(
            context,
            wrong_client.endpoint.run(std::move(*client_socket), prepared, {},
                                      deadline.as_steady_time_point()),
            asio::use_future);
        context.run();
        require(server_result.get().status == ServerRunStatus::TerminalError &&
                    client_result.get().status == ClientRunStatus::TerminalError &&
                    server.namespace_count() == 0 &&
                    server.revision_count() == 0 &&
                    server.live_session_count() == 0,
                "P5CO endpoint accepted a SESSION_HELLO from another C store");
        require_closed_fd(owned_server_fd,
                          "wrong-C P5CO endpoint retained its server fd");
        require_closed_fd(owned_client_fd,
                          "wrong-C P5CO client retained its descriptor");
        require(context.poll() == 0,
                "wrong-C P5CO endpoint retained an asynchronous handler");
    }

    {
        const P5coStoreGuids guids = p5co_store_guids(181);
        const daemon::P50CacheSessionOutcome outcome =
            p5co_adopted_outcome(guids, 9451);
        const sidecar::AbsoluteMonotonicDeadline deadline =
            p5co_deadline_after(std::chrono::seconds(2));
        P50ServerEndpoint server(guids.f);
        TestClient client(guids.c);
        const PreparedTuHandle prepared =
            admit(client, bytes("stale P5CO operation completion\n"));
        P5coTcpPair pair;
        sidecar::P5coEndpointHandoff handoff = flush_p5co_for_endpoint(
            pair.take_server(), pair.client, outcome, deadline);

        asio::io_context context;
        boost::system::error_code adoption_error;
        std::optional<tcp::socket> client_socket =
            P50ClientEndpoint::adopt_connected_fd(
                context.get_executor(), pair.take_client(), adoption_error);
        require(client_socket.has_value() && !adoption_error,
                "stale-operation client descriptor adoption failed");
        bool mutated = false;
        EndpointIoControl server_control;
        server_control.before_live_identity_check =
            [&](const CompletionStamp& expected,
                CompletionLiveIdentity& live) {
                if (!mutated && expected.cache_session_operation.has_value()) {
                    require(live.cache_session_operation.has_value(),
                            "typed endpoint live identity lost its P5CO operation");
                    ++live.cache_session_operation->operation_sequence;
                    mutated = true;
                }
            };
        std::future<ServerRunResult> server_result = asio::co_spawn(
            context,
            server.run_adopted(std::move(handoff), std::move(server_control)),
            asio::use_future);
        std::future<ClientRunResult> client_result = asio::co_spawn(
            context,
            client.endpoint.run(std::move(*client_socket), prepared, {},
                                deadline.as_steady_time_point()),
            asio::use_future);
        context.run();
        require(mutated &&
                    server_result.get().status == ServerRunStatus::Disconnected &&
                    client_result.get().status == ClientRunStatus::Disconnected &&
                    server.namespace_count() == 0 &&
                    server.revision_count() == 0 &&
                    server.live_session_count() == 0,
                "stale P5CO operation completion advanced the endpoint");
        require(context.poll() == 0,
                "stale P5CO operation completion retained a handler");
    }
}

void test_server_completion_rechecks_after_live_callback() {
    for (const bool cancel_in_callback : {false, true}) {
        const P5coStoreGuids guids =
            p5co_store_guids(cancel_in_callback ? 222 : 221);
        const daemon::P50CacheSessionOutcome outcome =
            p5co_adopted_outcome(guids,
                                 cancel_in_callback ? 9752 : 9751);
        const sidecar::AbsoluteMonotonicDeadline deadline =
            p5co_deadline_after(cancel_in_callback
                                    ? std::chrono::seconds(2)
                                    : std::chrono::milliseconds(150));
        P50ServerEndpoint server(guids.f);
        TestClient client(guids.c);
        const PreparedTuHandle prepared =
            admit(client, bytes("server completion callback fence\n"));
        P5coTcpPair pair;
        sidecar::P5coEndpointHandoff handoff = flush_p5co_for_endpoint(
            pair.take_server(), pair.client, outcome, deadline);

        asio::io_context context;
        boost::system::error_code adoption_error;
        std::optional<tcp::socket> client_socket =
            P50ClientEndpoint::adopt_connected_fd(
                context.get_executor(), pair.take_client(), adoption_error);
        require(client_socket.has_value() && !adoption_error,
                "server-callback client descriptor adoption failed");
        size_t unbound_payload_completions = 0;
        bool crossed_in_callback = false;
        EndpointIoControl server_control;
        server_control.before_live_identity_check =
            [&](const CompletionStamp& expected,
                CompletionLiveIdentity&) {
                if (expected.actor != ActorSide::F ||
                    expected.operation != AsyncOperationKind::ReadPayload ||
                    expected.transaction_bound)
                    return;
                ++unbound_payload_completions;
                if (unbound_payload_completions != 2)
                    return;
                crossed_in_callback = true;
                if (cancel_in_callback)
                    server.request_cancel_for_test();
                else
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(300));
            };
        std::future<ServerRunResult> server_result = asio::co_spawn(
            context,
            server.run_adopted(std::move(handoff),
                               std::move(server_control)),
            asio::use_future);
        std::future<ClientRunResult> client_result = asio::co_spawn(
            context,
            client.endpoint.run(std::move(*client_socket), prepared, {},
                                deadline.as_steady_time_point()),
            asio::use_future);
        context.run();

        const ServerRunResult server_value = server_result.get();
        const ClientRunResult client_value = client_result.get();
        require(crossed_in_callback && unbound_payload_completions == 2 &&
                    server_value.status ==
                        (cancel_in_callback
                             ? ServerRunStatus::Disconnected
                             : ServerRunStatus::DeadlineExceeded) &&
                    client_value.status ==
                        (cancel_in_callback
                             ? ClientRunStatus::Disconnected
                             : ClientRunStatus::DeadlineExceeded) &&
                    server.namespace_count() == 0 &&
                    server.revision_count() == 0 &&
                    server.live_session_count() == 0 &&
                    server.owner_usage().retained_input_records == 0,
                "server completion callback crossed operation authority");
        require(context.poll() == 0,
                "server completion callback retained an asynchronous handler");
    }
}

sidecar::P5coEndpointHandoff make_fence_probe_handoff(
    const daemon::P50CacheSessionOutcome& outcome,
    const sidecar::AbsoluteMonotonicDeadline& deadline,
    const std::shared_ptr<EndpointFenceProbe>& probe) {
    sidecar::AdoptedOutcomeWriter writer(
        std::make_unique<EndpointFenceLease>(outcome, deadline, probe),
        outcome,
        std::make_unique<sidecar::SystemMonotonicObservationSource>(),
        deadline);
    require(writer.advance(POLLOUT) ==
                sidecar::P5coWriterState::FullyFlushed,
            "fence-probe P5CO writer did not flush");
    std::optional<sidecar::P5coEndpointHandoff> handoff =
        writer.take_for_endpoint();
    require(handoff.has_value() && handoff->valid(),
            "fence-probe P5CO writer did not transfer whole authority");
    return std::move(*handoff);
}

void test_p5co_endpoint_fences_post_transfer_failures() {
    const P5coStoreGuids guids = p5co_store_guids(241);
    const daemon::P50CacheSessionOutcome outcome =
        p5co_adopted_outcome(guids, 9801);

    {
        const sidecar::AbsoluteMonotonicDeadline deadline =
            p5co_deadline_after(std::chrono::seconds(2));
        auto probe = std::make_shared<EndpointFenceProbe>();
        // One flush turn performs two exact validations and transfer performs
        // the third.  The fourth belongs to the endpoint after it consumes the
        // handoff and is deliberately stale.
        probe->successful_revalidations = 3;
        sidecar::P5coEndpointHandoff handoff =
            make_fence_probe_handoff(outcome, deadline, probe);
        P50ServerEndpoint server(guids.f);
        asio::io_context context;
        std::future<ServerRunResult> result = asio::co_spawn(
            context, server.run_adopted(std::move(handoff)), asio::use_future);
        context.run();
        require(result.get().status == ServerRunStatus::Disconnected &&
                    probe->revalidations == 4 && probe->releases == 0 &&
                    probe->fences == 1 && probe->fenced &&
                    server.live_session_count() == 0,
                "post-transfer stale lease was not fenced exactly once");
    }

    {
        const sidecar::AbsoluteMonotonicDeadline deadline =
            p5co_deadline_after(std::chrono::seconds(2));
        int pipe_fds[2] = {-1, -1};
        require(::pipe(pipe_fds) == 0,
                "post-transfer adoption-failure pipe setup failed");
        const int consumed_fd = pipe_fds[0];
        auto probe = std::make_shared<EndpointFenceProbe>();
        probe->release_fd = consumed_fd;
        sidecar::P5coEndpointHandoff handoff =
            make_fence_probe_handoff(outcome, deadline, probe);
        P50ServerEndpoint server(guids.f);
        asio::io_context context;
        std::future<ServerRunResult> result = asio::co_spawn(
            context, server.run_adopted(std::move(handoff)), asio::use_future);
        context.run();
        require(result.get().status == ServerRunStatus::Disconnected &&
                    probe->releases == 1 && probe->fences == 1 &&
                    probe->fenced && server.live_session_count() == 0,
                "post-release socket-adoption failure lacked an exact fence");
        require_closed_fd(consumed_fd,
                          "failed endpoint adoption retained its descriptor");
        (void)::close(pipe_fds[1]);
    }

    {
        // Hold the sole configured live-session slot open, then consume a
        // second complete handoff.  Allocation fails after descriptor release;
        // the RAII transfer guard must still fence the exact authority and the
        // adopted socket must close during unwinding.
        P50ServerEndpointConfig config;
        config.owner_limits.max_live_sessions = 1;
        P50ServerEndpoint server(guids.f, {}, nullptr, nullptr,
                                 std::move(config));
        asio::io_context context;

        P5coTcpPair occupied_pair;
        boost::system::error_code adoption_error;
        std::optional<tcp::socket> occupied_socket =
            P50ServerEndpoint::adopt_connected_fd(
                context.get_executor(), occupied_pair.take_server(),
                adoption_error);
        require(occupied_socket.has_value() && !adoption_error,
                "live-slot fence probe failed to adopt its first socket");
        std::future<ServerRunResult> occupied = asio::co_spawn(
            context, server.run_adopted(std::move(*occupied_socket)),
            asio::use_future);
        context.poll();
        context.restart();
        require(server.owner_usage().live_sessions == 1,
                "live-slot fence probe did not occupy its first session");

        const sidecar::AbsoluteMonotonicDeadline deadline =
            p5co_deadline_after(std::chrono::seconds(2));
        P5coTcpPair excess_pair;
        const int consumed_fd = excess_pair.server;
        auto probe = std::make_shared<EndpointFenceProbe>();
        probe->release_fd = excess_pair.take_server();
        sidecar::P5coEndpointHandoff handoff =
            make_fence_probe_handoff(outcome, deadline, probe);
        std::future<ServerRunResult> excess = asio::co_spawn(
            context, server.run_adopted(std::move(handoff)),
            asio::use_future);
        context.poll();
        context.restart();
        require_throws<std::length_error>(
            [&] { (void)excess.get(); },
            "post-release session-allocation failure did not propagate");
        require(probe->releases == 1 && probe->fences == 1 && probe->fenced,
                "post-release session-allocation failure was not fenced");
        require_closed_fd(consumed_fd,
                          "post-release session-allocation failure retained fd");

        server.request_cancel_for_test();
        context.run();
        require(occupied.get().status == ServerRunStatus::Disconnected &&
                    server.owner_usage().live_sessions == 0,
                "live-slot fence probe did not release its occupied session");
    }
}

void test_p5co_worker_completion_is_stale_after_deadline_or_cancel() {
    const size_t descriptor_baseline = open_fd_count();
    {
        const P5coStoreGuids guids = p5co_store_guids(191);
        const daemon::P50CacheSessionOutcome outcome =
            p5co_adopted_outcome(guids, 9501);
        const sidecar::AbsoluteMonotonicDeadline deadline =
            p5co_deadline_after(std::chrono::milliseconds(150));
        ActionTrace actions;
        P50ServerEndpoint server(guids.f, {}, nullptr, &actions);
        TestClient client(guids.c);
        const PreparedTuHandle prepared =
            admit(client, pseudo_random_bytes(64 * 1024));
        P5coTcpPair pair;
        const int owned_server_fd = pair.server;
        const int owned_client_fd = pair.client;
        sidecar::P5coEndpointHandoff handoff = flush_p5co_for_endpoint(
            pair.take_server(), pair.client, outcome, deadline);

        std::atomic<bool> worker_started{false};
        {
            asio::io_context context;
            boost::system::error_code adoption_error;
            std::optional<tcp::socket> client_socket =
                P50ClientEndpoint::adopt_connected_fd(
                    context.get_executor(), pair.take_client(), adoption_error);
            require(client_socket.has_value() && !adoption_error,
                    "blocked-codec client descriptor adoption failed");
            EndpointIoControl server_control;
            server_control.before_materialize_on_worker = [&] {
                worker_started.store(true, std::memory_order_release);
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
            };
            const auto started = std::chrono::steady_clock::now();
            std::optional<std::chrono::steady_clock::time_point> sentinel_observed;
            bool sentinel_saw_worker = false;
            asio::steady_timer sentinel(context,
                                        started + std::chrono::milliseconds(100));
            sentinel.async_wait([&](const boost::system::error_code& error) {
                require(!error, "blocked-codec owner sentinel was cancelled");
                sentinel_observed = std::chrono::steady_clock::now();
                sentinel_saw_worker =
                    worker_started.load(std::memory_order_acquire);
            });
            std::future<ServerRunResult> server_result = asio::co_spawn(
                context,
                server.run_adopted(std::move(handoff), std::move(server_control)),
                asio::use_future);
            std::future<ClientRunResult> client_result = asio::co_spawn(
                context,
                client.endpoint.run(std::move(*client_socket), prepared, {},
                                    deadline.as_steady_time_point()),
                asio::use_future);
            context.run();
            const auto endpoint_elapsed =
                std::chrono::steady_clock::now() - started;

            require(sentinel_observed.has_value() && sentinel_saw_worker &&
                        *sentinel_observed - started <
                            std::chrono::milliseconds(250),
                    "codec work blocked the endpoint timer owner");
            require(endpoint_elapsed < std::chrono::milliseconds(300),
                    "deadline timer did not promptly settle while codec worker was blocked");
            require(server_result.get().status ==
                            ServerRunStatus::DeadlineExceeded &&
                        client_result.get().status ==
                            ClientRunStatus::DeadlineExceeded &&
                        server.live_session_count() == 0 &&
                        !server.last_committed_input(guids.c).has_value() &&
                        server.owner_usage().retained_input_records == 0 &&
                        std::none_of(actions.records().begin(),
                                     actions.records().end(),
                                     [](const ActionRecord& record) {
                                         return record.action ==
                                                ActionType::INPUT_COMMITTED;
                                     }),
                    "post-deadline codec completion published durable input");
            require_closed_fd(
                owned_server_fd,
                "post-deadline codec completion retained server fd");
            require_closed_fd(
                owned_client_fd,
                "post-deadline codec completion retained client fd");
            require(context.poll() == 0,
                    "post-deadline codec completion retained a handler");
        }
        // The endpoint result is already final.  Let the deliberately blocked
        // global-pool job drain after destroying its owner execution_context.
        // The shared worker state must contain no Asio object tied to that
        // dead context, and the next row must see zero inherited inventory.
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
    }
    require(open_fd_count() == descriptor_baseline,
            "deadline-abandoned worker leaked eventfd/timerfd authority");

    {
        const P5coStoreGuids guids = p5co_store_guids(211);
        const daemon::P50CacheSessionOutcome outcome =
            p5co_adopted_outcome(guids, 9601);
        const sidecar::AbsoluteMonotonicDeadline deadline =
            p5co_deadline_after(std::chrono::seconds(2));
        ActionTrace actions;
        P50ServerEndpoint server(guids.f, {}, nullptr, &actions);
        TestClient client(guids.c);
        const PreparedTuHandle prepared =
            admit(client, pseudo_random_bytes(64 * 1024));
        P5coTcpPair pair;
        sidecar::P5coEndpointHandoff handoff = flush_p5co_for_endpoint(
            pair.take_server(), pair.client, outcome, deadline);

        asio::io_context context;
        boost::system::error_code adoption_error;
        std::optional<tcp::socket> client_socket =
            P50ClientEndpoint::adopt_connected_fd(
                context.get_executor(), pair.take_client(), adoption_error);
        require(client_socket.has_value() && !adoption_error,
                "cancelled-codec client descriptor adoption failed");
        std::atomic<bool> worker_started{false};
        EndpointIoControl server_control;
        server_control.before_materialize_on_worker = [&] {
            worker_started.store(true, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(350));
        };
        bool cancel_saw_worker = false;
        asio::steady_timer cancel(context, std::chrono::milliseconds(100));
        cancel.async_wait([&](const boost::system::error_code& error) {
            require(!error, "blocked-codec cancellation timer was cancelled");
            cancel_saw_worker =
                worker_started.load(std::memory_order_acquire);
            server.request_cancel_for_test();
        });
        std::future<ServerRunResult> server_result = asio::co_spawn(
            context,
            server.run_adopted(std::move(handoff), std::move(server_control)),
            asio::use_future);
        std::future<ClientRunResult> client_result = asio::co_spawn(
            context,
            client.endpoint.run(std::move(*client_socket), prepared, {},
                                deadline.as_steady_time_point()),
            asio::use_future);
        context.run();

        require(cancel_saw_worker &&
                    server_result.get().status ==
                        ServerRunStatus::Disconnected &&
                    client_result.get().status == ClientRunStatus::Disconnected &&
                    server.live_session_count() == 0 &&
                    !server.last_committed_input(guids.c).has_value() &&
                    server.owner_usage().retained_input_records == 0 &&
                    std::none_of(actions.records().begin(),
                                 actions.records().end(),
                                 [](const ActionRecord& record) {
                                     return record.action ==
                                            ActionType::INPUT_COMMITTED;
                                 }),
                "post-cancel codec completion published durable input");
        require(context.poll() == 0,
                "post-cancel codec completion retained a handler");
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
    }
    require(open_fd_count() == descriptor_baseline,
            "cancel-abandoned worker leaked eventfd/timerfd authority");
}

void test_p5co_cancel_keeps_materializing_bytes_charged() {
    const P5coStoreGuids guids = p5co_store_guids(213);
    const daemon::P50CacheSessionOutcome outcome =
        p5co_adopted_outcome(guids, 9603);
    const sidecar::AbsoluteMonotonicDeadline deadline =
        p5co_deadline_after(std::chrono::seconds(2));
    P50ServerEndpointConfig server_config;
    server_config.owner_limits.max_pending_encoded_bytes = 2 * 1024 * 1024;
    server_config.owner_limits.max_pending_raw_bytes = 2 * 1024 * 1024;
    server_config.owner_limits.max_decoder_window_bytes = uint64_t{128} << 20;
    P50ServerEndpoint server(guids.f, {}, nullptr, nullptr,
                             std::move(server_config));
    TestClient client(guids.c);
    constexpr size_t raw_size = 64 * 1024;
    const PreparedTuHandle prepared =
        admit(client, pseudo_random_bytes(raw_size));
    P5coTcpPair pair;
    sidecar::P5coEndpointHandoff handoff = flush_p5co_for_endpoint(
        pair.take_server(), pair.client, outcome, deadline);

    struct WorkerGate {
        std::mutex mutex;
        std::condition_variable changed;
        bool entered = false;
        bool released = false;
        bool hook_exited = false;
    };
    const auto gate = std::make_shared<WorkerGate>();
    const auto release_gate = [gate] {
        {
            std::lock_guard lock(gate->mutex);
            gate->released = true;
        }
        gate->changed.notify_all();
    };
    struct ReleaseGateOnExit {
        std::function<void()> release;
        ~ReleaseGateOnExit() { release(); }
    } release_on_exit{release_gate};
    EndpointIoControl server_control;
    server_control.before_materialize_on_worker = [gate] {
        std::unique_lock lock(gate->mutex);
        gate->entered = true;
        gate->changed.notify_all();
        gate->changed.wait(lock, [&] { return gate->released; });
        gate->hook_exited = true;
        gate->changed.notify_all();
    };

    asio::io_context context;
    boost::system::error_code adoption_error;
    std::optional<tcp::socket> client_socket =
        P50ClientEndpoint::adopt_connected_fd(
            context.get_executor(), pair.take_client(), adoption_error);
    require(client_socket.has_value() && !adoption_error,
            "credit-pin client descriptor adoption failed");
    bool cancel_saw_worker = false;
    bool worker_entry_timed_out = false;
    const auto worker_entry_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    auto poll_worker_entry = [&]() -> asio::awaitable<void> {
        const auto executor = co_await asio::this_coro::executor;
        asio::steady_timer poll(executor);
        bool entered = false;
        while (!entered && std::chrono::steady_clock::now() <
                               worker_entry_deadline) {
            {
                std::lock_guard lock(gate->mutex);
                entered = gate->entered;
            }
            if (entered)
                break;
            poll.expires_after(std::chrono::milliseconds(1));
            boost::system::error_code error;
            co_await poll.async_wait(
                asio::redirect_error(asio::use_awaitable, error));
            if (error)
                break;
        }
        {
            std::lock_guard lock(gate->mutex);
            entered = entered || gate->entered;
        }
        worker_entry_timed_out = !entered;
        cancel_saw_worker = entered;
        server.request_cancel_for_test();
        co_return;
    };
    std::future<void> poll_worker = asio::co_spawn(
        context, poll_worker_entry(), asio::use_future);
    std::future<ServerRunResult> server_result = asio::co_spawn(
        context,
        server.run_adopted(std::move(handoff), std::move(server_control)),
        asio::use_future);
    std::future<ClientRunResult> client_result = asio::co_spawn(
        context,
        client.endpoint.run(std::move(*client_socket), prepared, {},
                            deadline.as_steady_time_point()),
        asio::use_future);
    context.run();
    poll_worker.get();

    bool worker_is_still_held = false;
    {
        std::lock_guard lock(gate->mutex);
        worker_is_still_held = gate->entered && !gate->released;
    }
    const P50ServerOwnerUsage usage_while_worker_held = server.owner_usage();
    std::cerr << "D06_CREDIT_PIN held=" << worker_is_still_held
              << " pending_encoded=" << usage_while_worker_held.pending_encoded_bytes
              << " pending_raw=" << usage_while_worker_held.pending_raw_bytes
              << " decoder_window=" << usage_while_worker_held.decoder_window_bytes
              << '\n';
    // Always release the global codec worker before assertions, including the
    // expected red assertion on the unfixed implementation.
    release_gate();
    {
        std::unique_lock lock(gate->mutex);
        require(gate->changed.wait_for(lock, std::chrono::seconds(2),
                                       [&] { return gate->hook_exited; }),
                "credit-pin worker did not leave its test barrier");
    }
    const ServerRunResult server_value = server_result.get();
    const ClientRunResult client_value = client_result.get();
    auto wait_for_worker_budget_release = [&]() -> asio::awaitable<bool> {
        const auto executor = co_await asio::this_coro::executor;
        asio::steady_timer poll(executor);
        const auto until =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (server.owner_usage().pending_raw_bytes != 0 &&
               std::chrono::steady_clock::now() < until) {
            poll.expires_after(std::chrono::milliseconds(1));
            boost::system::error_code error;
            co_await poll.async_wait(
                asio::redirect_error(asio::use_awaitable, error));
            if (error)
                co_return false;
        }
        co_return server.owner_usage().pending_raw_bytes == 0;
    };
    context.restart();
    std::future<bool> budget_drained = asio::co_spawn(
        context, wait_for_worker_budget_release(), asio::use_future);
    context.run();
    const bool worker_budget_released = budget_drained.get();
    require(!worker_entry_timed_out && cancel_saw_worker &&
                worker_is_still_held &&
                server_value.status == ServerRunStatus::Disconnected &&
                client_value.status == ClientRunStatus::Disconnected &&
                worker_budget_released,
            "credit-pin fixture did not cancel with materialization held");
    require(usage_while_worker_held.pending_raw_bytes >= raw_size &&
                usage_while_worker_held.pending_encoded_bytes != 0 &&
                usage_while_worker_held.decoder_window_bytes != 0,
            "disconnect released pending-byte budget before worker ownership ended");
}

void test_p5co_codec_queue_is_bounded() {
    constexpr size_t run_count = 9;
    asio::io_context context;
    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    size_t worker_started = 0;
    bool release_workers = false;

    std::vector<std::unique_ptr<P50ServerEndpoint>> servers;
    std::vector<std::unique_ptr<TestClient>> clients;
    std::vector<std::future<ServerRunResult>> server_results;
    std::vector<std::future<ClientRunResult>> client_results;
    servers.reserve(run_count);
    clients.reserve(run_count);
    server_results.reserve(run_count);
    client_results.reserve(run_count);

    for (size_t index = 0; index != run_count; ++index) {
        const P5coStoreGuids guids =
            p5co_store_guids(static_cast<uint8_t>(44 + index));
        servers.emplace_back(std::make_unique<P50ServerEndpoint>(guids.f));
        clients.emplace_back(std::make_unique<TestClient>(guids.c));
        const PreparedTuHandle prepared =
            admit(*clients.back(), pseudo_random_bytes(1024 + index));
        const daemon::P50CacheSessionOutcome outcome =
            p5co_adopted_outcome(guids, 9900 + index);
        const sidecar::AbsoluteMonotonicDeadline deadline =
            p5co_deadline_after(std::chrono::seconds(5));
        P5coTcpPair pair;
        sidecar::P5coEndpointHandoff handoff = flush_p5co_for_endpoint(
            pair.take_server(), pair.client, outcome, deadline);
        boost::system::error_code adoption_error;
        std::optional<tcp::socket> client_socket =
            P50ClientEndpoint::adopt_connected_fd(
                context.get_executor(), pair.take_client(), adoption_error);
        require(client_socket.has_value() && !adoption_error,
                "codec-queue client descriptor adoption failed");

        EndpointIoControl server_control;
        server_control.before_materialize_on_worker = [&] {
            std::unique_lock lock(gate_mutex);
            ++worker_started;
            gate_changed.notify_all();
            gate_changed.wait(lock, [&] { return release_workers; });
        };
        server_results.emplace_back(asio::co_spawn(
            context,
            servers.back()->run_adopted(std::move(handoff),
                                        std::move(server_control)),
            asio::use_future));
        client_results.emplace_back(asio::co_spawn(
            context,
            clients.back()->endpoint.run(
                std::move(*client_socket), prepared, {},
                deadline.as_steady_time_point()),
            asio::use_future));
    }

    bool both_workers_blocked = false;
    bool queue_rejected = false;
    // Preparation and endpoint state are deliberately single-owner.  Keep the
    // io_context on this thread (the preparation owner), and use a read-only
    // observer thread solely to detect the bounded rejection and release the
    // two blocked codec workers.
    std::thread observer([&] {
        {
            std::unique_lock lock(gate_mutex);
            both_workers_blocked = gate_changed.wait_for(
                lock, std::chrono::seconds(2),
                [&] { return worker_started >= 2; });
        }
        const auto rejection_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (both_workers_blocked &&
               std::chrono::steady_clock::now() < rejection_deadline) {
            queue_rejected = std::any_of(
                server_results.begin(), server_results.end(),
                [](std::future<ServerRunResult>& result) {
                    return result.wait_for(std::chrono::milliseconds(0)) ==
                           std::future_status::ready;
                });
            if (queue_rejected)
                break;
            std::this_thread::yield();
        }
        {
            std::lock_guard lock(gate_mutex);
            release_workers = true;
        }
        gate_changed.notify_all();
    });
    context.run();
    observer.join();

    size_t completed = 0;
    size_t rejected = 0;
    for (size_t index = 0; index != run_count; ++index) {
        const ServerRunResult server = server_results[index].get();
        const ClientRunResult client = client_results[index].get();
        if (server.status == ServerRunStatus::Completed &&
            client.status == ClientRunStatus::Committed) {
            ++completed;
        } else if (server.status == ServerRunStatus::TerminalError &&
                   client.status == ClientRunStatus::TerminalError) {
            const P50ServerOwnerUsage usage = servers[index]->owner_usage();
            require(usage.live_sessions == 0 &&
                        usage.pending_encoded_bytes == 0 &&
                        usage.pending_raw_bytes == 0 &&
                        usage.retained_input_records == 0,
                    "rejected codec-queue admission retained owner inventory");
            ++rejected;
        }
    }
    require(both_workers_blocked && queue_rejected && completed == 8 &&
                rejected == 1,
            "codec pool did not enforce its running-plus-queued job bound");

    // Every completed/abandoned worker must release exactly one admission
    // slot.  A second wave catches a guard deletion which the initial 8/9
    // saturation result alone cannot distinguish from a permanent slot leak.
    const P5coStoreGuids recovery_guids = p5co_store_guids(79);
    P50ServerEndpoint recovery_server(recovery_guids.f);
    TestClient recovery_client(recovery_guids.c);
    const PreparedTuHandle recovery_prepared =
        admit(recovery_client, bytes("codec admission slot recovery\n"));
    const daemon::P50CacheSessionOutcome recovery_outcome =
        p5co_adopted_outcome(recovery_guids, 9991);
    const sidecar::AbsoluteMonotonicDeadline recovery_deadline =
        p5co_deadline_after(std::chrono::seconds(2));
    P5coTcpPair recovery_pair;
    sidecar::P5coEndpointHandoff recovery_handoff =
        flush_p5co_for_endpoint(recovery_pair.take_server(),
                                recovery_pair.client, recovery_outcome,
                                recovery_deadline);
    boost::system::error_code recovery_adoption_error;
    std::optional<tcp::socket> recovery_client_socket =
        P50ClientEndpoint::adopt_connected_fd(
            context.get_executor(), recovery_pair.take_client(),
            recovery_adoption_error);
    require(recovery_client_socket.has_value() && !recovery_adoption_error,
            "codec admission recovery descriptor adoption failed");
    context.restart();
    std::future<ServerRunResult> recovery_server_result = asio::co_spawn(
        context, recovery_server.run_adopted(std::move(recovery_handoff)),
        asio::use_future);
    std::future<ClientRunResult> recovery_client_result = asio::co_spawn(
        context,
        recovery_client.endpoint.run(
            std::move(*recovery_client_socket), recovery_prepared, {},
            recovery_deadline.as_steady_time_point()),
        asio::use_future);
    context.run();
    require(recovery_server_result.get().status ==
                    ServerRunStatus::Completed &&
                recovery_client_result.get().status ==
                    ClientRunStatus::Committed,
            "codec worker completion leaked an admission slot");
    require(context.poll() == 0,
            "bounded codec-queue run retained an asynchronous handler");
}

void test_p5co_deadline_wins_after_owner_job_selector() {
    const P5coStoreGuids guids = p5co_store_guids(231);
    bool block_first_selection = true;
    size_t selector_calls = 0;
    P50ServerEndpointConfig config;
    config.input_job_state =
        [&](CStoreGuid, const TxBegin&, const TxCommit&,
            std::span<const uint8_t>) {
            ++selector_calls;
            if (std::exchange(block_first_selection, false))
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            return InputJobState::Open;
        };
    ActionTrace actions;
    P50ServerEndpoint server(guids.f, {}, nullptr, &actions,
                             std::move(config));
    TestClient client(guids.c);
    const std::vector<uint8_t> input = pseudo_random_bytes(32 * 1024);
    const PreparedTuHandle prepared = admit(client, input);

    {
        const daemon::P50CacheSessionOutcome outcome =
            p5co_adopted_outcome(guids, 9701);
        const sidecar::AbsoluteMonotonicDeadline deadline =
            p5co_deadline_after(std::chrono::milliseconds(150));
        P5coTcpPair pair;
        sidecar::P5coEndpointHandoff handoff = flush_p5co_for_endpoint(
            pair.take_server(), pair.client, outcome, deadline);
        asio::io_context context;
        boost::system::error_code adoption_error;
        std::optional<tcp::socket> client_socket =
            P50ClientEndpoint::adopt_connected_fd(
                context.get_executor(), pair.take_client(), adoption_error);
        require(client_socket.has_value() && !adoption_error,
                "selector-deadline client descriptor adoption failed");
        std::future<ServerRunResult> server_result = asio::co_spawn(
            context, server.run_adopted(std::move(handoff)), asio::use_future);
        std::future<ClientRunResult> client_result = asio::co_spawn(
            context,
            client.endpoint.run(std::move(*client_socket), prepared, {},
                                deadline.as_steady_time_point()),
            asio::use_future);
        context.run();

        const ServerRunResult server_value = server_result.get();
        require(selector_calls == 1 &&
                    server_value.status == ServerRunStatus::DeadlineExceeded &&
                    client_result.get().status ==
                        ClientRunStatus::DeadlineExceeded &&
                    !server_value.candidate_input.has_value() &&
                    !server_value.completed_input.has_value() &&
                    !server_value.committed_input.has_value() &&
                    !server.last_committed_input(guids.c).has_value() &&
                    server.owner_usage().retained_input_records == 0 &&
                    server.owner_usage().pending_raw_bytes == 0 &&
                    server.live_session_count() == 0 &&
                    std::none_of(actions.records().begin(),
                                 actions.records().end(),
                                 [](const ActionRecord& record) {
                                     return record.action ==
                                            ActionType::INPUT_COMMITTED;
                                 }),
                "owner job selector published after the absolute deadline");
        require(context.poll() == 0,
                "selector-deadline run retained an asynchronous handler");
    }

    // The timed-out transaction remains the exact interrupted identity.  A
    // fresh operation/deadline may replay it; the expired operation itself did
    // not publish or advance the route.
    {
        const daemon::P50CacheSessionOutcome outcome =
            p5co_adopted_outcome(guids, 9702);
        const sidecar::AbsoluteMonotonicDeadline deadline =
            p5co_deadline_after(std::chrono::seconds(2));
        P5coTcpPair pair;
        sidecar::P5coEndpointHandoff handoff = flush_p5co_for_endpoint(
            pair.take_server(), pair.client, outcome, deadline);
        asio::io_context context;
        boost::system::error_code adoption_error;
        std::optional<tcp::socket> client_socket =
            P50ClientEndpoint::adopt_connected_fd(
                context.get_executor(), pair.take_client(), adoption_error);
        require(client_socket.has_value() && !adoption_error,
                "selector-deadline replay descriptor adoption failed");
        std::future<ServerRunResult> server_result = asio::co_spawn(
            context, server.run_adopted(std::move(handoff)), asio::use_future);
        std::future<ClientRunResult> client_result = asio::co_spawn(
            context,
            client.endpoint.run(std::move(*client_socket), prepared, {},
                                deadline.as_steady_time_point()),
            asio::use_future);
        context.run();
        require(selector_calls == 2 &&
                    server_result.get().status == ServerRunStatus::Completed &&
                    client_result.get().status == ClientRunStatus::Committed &&
                    copy_input(server, guids.c) == input,
                "fresh operation did not replay the exact selector-timeout input");
        require(context.poll() == 0,
                "selector-timeout replay retained an asynchronous handler");
    }
}

void test_adopted_endpoint_exact_zstd_and_ownership() {
    for (const bool use_native_adoption : {false, true}) {
        CompletionLog completions;
        P50ServerEndpoint server(Id128::from_u64(use_native_adoption ? 910 : 911), {},
                                 &completions);
        TestClient client(Id128::from_u64(use_native_adoption ? 912 : 913));
        const std::vector<uint8_t> input = pseudo_random_bytes(8192);
        int consumed_fd = -1;
        const PairResult result = run_adopted_pair(
            client, server, admit(client, input), use_native_adoption, &consumed_fd);
        require(result.client.status == ClientRunStatus::Committed &&
                    result.server.status == ServerRunStatus::Completed &&
                    copy_input(server, client.c_store_guid()) == input,
                use_native_adoption ? "native adopted ZSTD_TU changed exact bytes"
                                    : "socket adopted ZSTD_TU changed exact bytes");
        require_closed_fd(consumed_fd, "adopted endpoint did not close its owned fd exactly once");
        require(!completions.completions().empty() &&
                    completions.completions().front().stamp.operation ==
                        AsyncOperationKind::ReadHeader,
                "adopted endpoint consumed an accept or input operation before ownership");
        require(std::none_of(completions.completions().begin(), completions.completions().end(),
                             [](const AsyncCompletion& completion) {
                                 return completion.stamp.operation == AsyncOperationKind::Accept;
                             }),
                "adopted endpoint unexpectedly performed a second accept");
    }
}

void test_client_deadline_and_direct_socket_ownership() {
    CompletionLog completions;
    TestClient client(Id128::from_u64(930), {}, HistoryNonce{1}, &completions);
    const PreparedTuHandle prepared = admit(client, bytes("deadline input\n"));

    // An already-expired deadline must not allocate a session, enqueue work, or
    // attempt a connect.
    {
        asio::io_context context;
        std::future<ClientRunResult> future = asio::co_spawn(
            context,
            client.endpoint.run(tcp::endpoint{asio::ip::address_v4::loopback(), 1}, prepared,
                                 {}, std::chrono::steady_clock::now() - std::chrono::seconds(1)),
            asio::use_future);
        context.run();
        const ClientRunResult result = future.get();
        require(result.status == ClientRunStatus::DeadlineExceeded &&
                    !client.has_reconciliation_work(),
                "deadline-before-start changed client reconciliation state");
    }

    // The peer accepts and then never replies.  The endpoint-owned timer must
    // close the socket and complete in a bounded interval, with no timer left
    // keeping the io_context alive after the run result is delivered.
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    std::future<void> peer = asio::co_spawn(context, raw_stall_after_connect(acceptor),
                                             asio::use_future);
    const auto started = std::chrono::steady_clock::now();
    std::future<ClientRunResult> future = asio::co_spawn(
        context,
        client.endpoint.run(acceptor.local_endpoint(), prepared, {},
                             std::chrono::steady_clock::now() + std::chrono::milliseconds(100)),
        asio::use_future);
    context.run();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    peer.get();
    const ClientRunResult result = future.get();
    require(result.status == ClientRunStatus::DeadlineExceeded &&
                elapsed < std::chrono::seconds(2) && client.has_reconciliation_work(),
            "unresponsive peer was not bounded by the client deadline");
    // No handler/timer may retain the socket after completion.
    require(context.poll() == 0, "deadline run left a live socket/timer callback");

    // The connected-socket overload consumes a descriptor that was connected
    // by the ordinary listener path and drives the same reducer/witness path.
    TestClient adopted_client(Id128::from_u64(931));
    const PreparedTuHandle adopted_prepared = admit(adopted_client, bytes("adopted input\n"));
    P50ServerEndpoint server(Id128::from_u64(932));
    asio::io_context adopted_context;
    tcp::acceptor adopted_acceptor(adopted_context,
                                   {asio::ip::address_v4::loopback(), 0});
    std::future<ServerRunResult> server_result = asio::co_spawn(
        adopted_context,
        [&]() -> asio::awaitable<ServerRunResult> {
            const auto executor = co_await asio::this_coro::executor;
            tcp::socket accepted(executor);
            co_await adopted_acceptor.async_accept(accepted, asio::use_awaitable);
            co_return co_await server.run_adopted(std::move(accepted));
        },
        asio::use_future);
    std::future<ClientRunResult> client_result = asio::co_spawn(
        adopted_context,
        [&]() -> asio::awaitable<ClientRunResult> {
            const auto executor = co_await asio::this_coro::executor;
            tcp::socket connected(executor);
            co_await connected.async_connect(adopted_acceptor.local_endpoint(), asio::use_awaitable);
            co_return co_await adopted_client.endpoint.run(std::move(connected),
                                                           adopted_prepared);
        },
        asio::use_future);
    adopted_context.run();
    const ClientRunResult adopted_result = client_result.get();
    require(adopted_result.status == ClientRunStatus::Committed &&
                adopted_result.committed_commit.has_value() &&
                adopted_result.committed_input.has_value() &&
                server_result.get().status == ServerRunStatus::Completed,
            "connected-socket client overload did not commit its direct witness");
}

void test_client_completion_deadline_is_fresh_before_first_write() {
    TestClient client(Id128::from_u64(933));
    const PreparedTuHandle prepared = admit(client, bytes("completion deadline input\n"));

    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    std::future<void> peer = asio::co_spawn(
        context, raw_stall_after_connect(acceptor), asio::use_future);

    bool delayed_connect_completion = false;
    bool attempted_first_remote_write = false;
    EndpointIoControl control;
    control.before_completion_check = [&](const CompletionStamp& observed) {
        if (delayed_connect_completion || observed.actor != ActorSide::C ||
            observed.operation != AsyncOperationKind::Connect)
            return;
        delayed_connect_completion = true;
        // The timer shares this owner executor, so it cannot run while this
        // completion validator is blocked. The reducer itself must freshly
        // sample the deadline before it initiates the first CacheWire write.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    };
    control.before_first_remote_write = [&] {
        attempted_first_remote_write = true;
    };

    std::future<ClientRunResult> run = asio::co_spawn(
        context,
        client.endpoint.run(
            acceptor.local_endpoint(), prepared, std::move(control),
            std::chrono::steady_clock::now() + std::chrono::milliseconds(50)),
        asio::use_future);
    context.run();
    peer.get();
    const ClientRunResult result = run.get();
    require(delayed_connect_completion &&
                result.status == ClientRunStatus::DeadlineExceeded &&
                !attempted_first_remote_write &&
                client.has_reconciliation_work() &&
                context.poll() == 0,
            "post-deadline client completion attempted the first CacheWire write");
}

void test_adopted_endpoint_disconnect_and_invalid_rows() {
    P50ServerEndpoint server(Id128::from_u64(920));
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    int consumed_fd = -1;
    std::future<ServerRunResult> server_result = asio::co_spawn(
        context,
        [&]() -> asio::awaitable<ServerRunResult> {
            const auto executor = co_await asio::this_coro::executor;
            tcp::socket accepted(executor);
            boost::system::error_code error;
            co_await acceptor.async_accept(
                accepted, asio::redirect_error(asio::use_awaitable, error));
            require(!error, "disconnect row accept failed");
            consumed_fd = accepted.native_handle();
            co_return co_await server.run_adopted(std::move(accepted));
        },
        asio::use_future);
    std::future<void> disconnecting_client = asio::co_spawn(
        context,
        [&]() -> asio::awaitable<void> {
            const auto executor = co_await asio::this_coro::executor;
            tcp::socket socket(executor);
            boost::system::error_code error;
            co_await socket.async_connect(acceptor.local_endpoint(),
                                          asio::redirect_error(asio::use_awaitable, error));
            require(!error, "disconnect row connect failed");
            socket.close(error);
            co_return;
        },
        asio::use_future);
    context.run();
    require(server_result.get().status == ServerRunStatus::Disconnected &&
                server.live_session_count() == 0,
            "adopted disconnect did not fail closed and release its session");
    disconnecting_client.get();
    require_closed_fd(consumed_fd, "adopted disconnect leaked its owned fd");

    asio::io_context invalid_context;
    boost::system::error_code error;
    auto reject_fd = [&](int fd, std::string_view detail) {
        auto adopted = P50ServerEndpoint::adopt_connected_fd(invalid_context.get_executor(), fd,
                                                              error);
        require(!adopted.has_value() && !!error, detail);
        require_closed_fd(fd, "invalid native adoption did not consume and close fd");
        error.clear();
    };

    int pipe_fds[2] = {-1, -1};
    require(::pipe(pipe_fds) == 0, "pipe row setup failed");
    reject_fd(pipe_fds[0], "non-socket native fd was adopted");
    (void)::close(pipe_fds[1]);

    int unix_pair[2] = {-1, -1};
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, unix_pair) == 0,
            "wrong-family row setup failed");
    reject_fd(unix_pair[0], "wrong-family native fd was adopted");
    (void)::close(unix_pair[1]);

    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    require(listener >= 0, "unconnected listener row setup failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
            "unconnected listener row bind failed");
    require(::listen(listener, 1) == 0, "unconnected listener row listen failed");
    reject_fd(listener, "unconnected listener fd was adopted");

    int closed_pair[2] = {-1, -1};
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, closed_pair) == 0,
            "closed-fd row setup failed");
    const int closed_fd = closed_pair[0];
    (void)::close(closed_fd);
    (void)::close(closed_pair[1]);
    auto adopted_closed = P50ServerEndpoint::adopt_connected_fd(
        invalid_context.get_executor(), closed_fd, error);
    require(!adopted_closed.has_value() && !!error,
            "closed native fd was adopted");
    error.clear();

    tcp::socket invalid_socket(invalid_context.get_executor());
    std::future<ServerRunResult> invalid_result = asio::co_spawn(
        invalid_context, server.run_adopted(std::move(invalid_socket)), asio::use_future);
    invalid_context.run();
    require(invalid_result.get().status == ServerRunStatus::Disconnected &&
                server.live_session_count() == 0,
            "closed adopted socket did not fail closed");
}

void test_adopted_cross_executor_releases_registration() {
    P50ServerEndpoint server(Id128::from_u64(930));
    // Bind the endpoint owner to this thread. The adopted socket's I/O below
    // runs on a different executor, forcing the reducer's owner check from a
    // completion callback without permitting a session-row leak.
    server.reset_store(Id128::from_u64(931));

    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    require(listener >= 0, "cross-executor listener setup failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
            "cross-executor listener bind failed");
    require(::listen(listener, 1) == 0, "cross-executor listener listen failed");
    socklen_t address_length = sizeof(address);
    require(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_length) == 0,
            "cross-executor listener address failed");
    const int client = ::socket(AF_INET, SOCK_STREAM, 0);
    require(client >= 0, "cross-executor client setup failed");
    require(::connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
            "cross-executor client connect failed");
    const int accepted_fd = ::accept(listener, nullptr, nullptr);
    require(accepted_fd >= 0, "cross-executor accept failed");
    (void)::close(listener);
    (void)::close(client);

    asio::io_context owner_context;
    asio::io_context worker_context;
    auto worker_work = asio::make_work_guard(worker_context);
    tcp::socket adopted(worker_context.get_executor());
    boost::system::error_code assign_error;
    adopted.assign(tcp::v4(), accepted_fd, assign_error);
    require(!assign_error, "cross-executor socket assign failed");

    EndpointIoControl control;
    control.before_completion_check = [](const CompletionStamp&) {
        throw std::logic_error("forced adopted reducer entry failure");
    };
    std::future<ServerRunResult> result = asio::co_spawn(
        owner_context, server.run_adopted(std::move(adopted), std::move(control)),
        asio::use_future);
    std::thread worker([&] { worker_context.run(); });
    owner_context.run();
    worker_work.reset();
    worker.join();
    require(result.get().status == ServerRunStatus::TerminalError,
            "cross-executor owner rejection was not terminal");
    require(server.live_session_count() == 0,
            "cross-executor owner rejection stranded a live session");
    require_closed_fd(accepted_fd, "cross-executor adopted fd was not closed");
}

CompetingPairResult run_competing_pair(P50ServerEndpointConfig config,
                                        uint64_t identity_base,
                                        size_t first_raw_bytes,
                                        size_t second_raw_bytes) {
    P50ServerEndpoint server(Id128::from_u64(identity_base), {}, nullptr, nullptr,
                             std::move(config));
    TestClient first(Id128::from_u64(identity_base + 1));
    TestClient second(Id128::from_u64(identity_base + 2));
    const PreparedTuHandle first_prepared =
        admit(first, pseudo_random_bytes(first_raw_bytes));
    const PreparedTuHandle second_prepared =
        admit(second, pseudo_random_bytes(second_raw_bytes));
    EndpointIoControl first_control;
    first_control.max_write_fragment = 1;
    EndpointIoControl second_control;
    second_control.max_write_fragment = 1;

    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    std::future<ServerRunResult> first_server =
        asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
    std::future<ServerRunResult> second_server =
        asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
    std::future<ClientRunResult> first_client = asio::co_spawn(
        context,
        first.endpoint.run(acceptor.local_endpoint(), first_prepared, first_control),
        asio::use_future);
    std::future<ClientRunResult> second_client = asio::co_spawn(
        context,
        second.endpoint.run(acceptor.local_endpoint(), second_prepared, second_control),
        asio::use_future);
    context.run();
    return {first_client.get(), second_client.get(), first_server.get(),
            second_server.get(), server.owner_usage()};
}

void require_trace(const ActionTrace& trace, std::string_view context) {
    if (const auto error = check_action_trace(trace.records()))
        fail(std::string(context) + ": " + *error);
}

TxBegin make_begin(const ZstdTuEnvelope& prepared, HistoryNonce nonce, Digest128 pre_state,
                   RelSeq rel = RelSeq{0}) {
    TxBegin begin = prepared.begin;
    begin.history_nonce = nonce;
    begin.rel_seq = rel;
    begin.pre_state_digest = pre_state;
    begin.transaction_digest = compute_transaction_digest(begin, prepared.body);
    return begin;
}

asio::awaitable<void> raw_write(tcp::socket& socket, Message message) {
    const std::vector<uint8_t> frame = encode_frame(message);
    co_await asio::async_write(socket, asio::buffer(frame), asio::use_awaitable);
    co_return;
}

asio::awaitable<void> raw_write_fragmented(tcp::socket& socket,
                                           Message message,
                                           size_t max_fragment) {
    if (max_fragment == 0)
        throw std::invalid_argument("raw fragment size is zero");
    const std::vector<uint8_t> frame = encode_frame(message);
    for (size_t offset = 0; offset < frame.size();) {
        const size_t count = std::min(max_fragment, frame.size() - offset);
        co_await asio::async_write(
            socket, asio::buffer(frame.data() + offset, count),
            asio::use_awaitable);
        offset += count;
    }
}

asio::awaitable<void> raw_write_bytes(tcp::socket& socket,
                                      std::span<const uint8_t> bytes_to_write) {
    co_await asio::async_write(socket, asio::buffer(bytes_to_write.data(), bytes_to_write.size()),
                               asio::use_awaitable);
    co_return;
}

asio::awaitable<Frame> raw_read(tcp::socket& socket, uint32_t max_payload) {
    std::array<uint8_t, 4> raw_header{};
    co_await asio::async_read(socket, asio::buffer(raw_header), asio::use_awaitable);
    const FrameHeader header = decode_frame_header(raw_header, max_payload);
    Frame result{header.type, std::vector<uint8_t>(header.payload_bytes)};
    if (!result.payload.empty())
        co_await asio::async_read(socket, asio::buffer(result.payload), asio::use_awaitable);
    co_return result;
}

asio::awaitable<void> raw_cancel_client_after_hello(
    tcp::acceptor& acceptor, P50ClientEndpoint& client) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    co_await acceptor.async_accept(socket, asio::use_awaitable);
    const Frame hello = co_await raw_read(
        socket, EndpointCaps{}.wire.max_frame_payload);
    require(hello.type == MessageType::SESSION_HELLO,
            "cancellation peer did not observe the first CacheWire frame");
    client.request_cancel_for_test();
    boost::system::error_code ignored;
    socket.close(ignored);
    co_return;
}

template <class T> T raw_decode(const Frame& frame) {
    Message decoded = decode_payload(frame.type, frame.payload);
    T* result = std::get_if<T>(&decoded);
    if (!result)
        throw std::invalid_argument("raw peer received an unexpected message");
    return std::move(*result);
}

struct RawRoute {
    SessionHello hello;
    SessionState state;
};

asio::awaitable<RawRoute> raw_open(tcp::socket& socket, const tcp::endpoint& remote,
                                   CStoreGuid c_guid, SessionLimits limits = {});
asio::awaitable<SessionState> raw_reset(tcp::socket& socket, const RawRoute& open,
                                        HistoryNonce nonce);
asio::awaitable<void> raw_wait_for_close(tcp::socket& socket);

struct HelloRaceState {
    bool first_pending = false;
    bool second_finished = false;
};

asio::awaitable<void> raw_pending_then_finish(tcp::endpoint remote, CStoreGuid c_guid,
                                              std::span<const uint8_t> input,
                                              HelloRaceState& coordination) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    RawRoute open = co_await raw_open(socket, remote, c_guid);
    SessionState route = co_await raw_reset(socket, open, HistoryNonce{101});
    const ZstdTuEnvelope prepared = encode_zstd_tu(
        HistoryNonce{1}, RelSeq{0}, TuSeq{11}, Digest128{}, input);
    const TxBegin begin =
        make_begin(prepared, route.history_nonce, route.state_digest, route.next_rel_seq);
    co_await raw_write(socket, begin);
    coordination.first_pending = true;
    asio::steady_timer timer(executor);
    while (!coordination.second_finished) {
        timer.expires_after(std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
    }
    Message body_message = BodyMessage{prepared.body};
    co_await raw_write(socket, std::move(body_message));
    const TxCommit commit =
        raw_decode<TxCommit>(co_await raw_read(socket, route.limits.max_frame_payload));
    if (commit.transaction_digest != begin.transaction_digest ||
        commit.raw_digest != begin.raw_digest)
        throw std::logic_error("pending fixture received a different TX_COMMIT");
    boost::system::error_code ignored;
    socket.close(ignored);
    co_return;
}

asio::awaitable<void> raw_incompatible_hello(tcp::endpoint remote, CStoreGuid c_guid,
                                             HelloRaceState& coordination) {
    const auto executor = co_await asio::this_coro::executor;
    asio::steady_timer timer(executor);
    while (!coordination.first_pending) {
        timer.expires_after(std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
    }
    tcp::socket socket(executor);
    co_await socket.async_connect(remote, asio::use_awaitable);
    SessionHello hello;
    hello.c_store_guid = c_guid;
    // Use a reserved profile bit so this remains deliberately incompatible
    // when every currently implemented revision-1 profile is enabled.
    hello.supported_profiles = uint32_t{1} << 31;
    co_await raw_write(socket, hello);
    const Frame terminal = co_await raw_read(socket, hello.limits.max_frame_payload);
    coordination.second_finished = true;
    if (terminal.type != MessageType::ERROR)
        throw std::logic_error("incompatible HELLO did not receive terminal ERROR");
    (void)raw_decode<ErrorMessage>(terminal);
    std::array<uint8_t, 1> extra{};
    boost::system::error_code error;
    (void)co_await socket.async_read_some(
        asio::buffer(extra), asio::redirect_error(asio::use_awaitable, error));
    if (error != asio::error::eof && error != asio::error::connection_reset)
        throw std::logic_error("incompatible HELLO connection stayed open");
    co_return;
}

asio::awaitable<void> raw_invalid_first_mutation(tcp::endpoint remote,
                                                 CStoreGuid c_guid,
                                                 HelloRaceState& coordination) {
    const auto executor = co_await asio::this_coro::executor;
    asio::steady_timer timer(executor);
    while (!coordination.first_pending) {
        timer.expires_after(std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
    }
    tcp::socket socket(executor);
    const RawRoute open = co_await raw_open(socket, remote, c_guid);
    Message invalid = BodyMessage{bytes("not a first mutation")};
    co_await raw_write(socket, std::move(invalid));
    const Frame terminal =
        co_await raw_read(socket, open.state.limits.max_frame_payload);
    coordination.second_finished = true;
    if (terminal.type != MessageType::ERROR)
        throw std::logic_error(
            "invalid compatible candidate did not receive terminal ERROR");
    (void)raw_decode<ErrorMessage>(terminal);
    co_await raw_wait_for_close(socket);
}

struct CandidateRevisionRace {
    bool first_staged = false;
    bool route_changed = false;
};

asio::awaitable<void> raw_stale_candidate(tcp::endpoint remote,
                                          CStoreGuid c_guid,
                                          CandidateRevisionRace& coordination) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    const RawRoute open = co_await raw_open(socket, remote, c_guid);
    coordination.first_staged = true;
    asio::steady_timer timer(executor);
    while (!coordination.route_changed) {
        timer.expires_after(std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
    }

    const HistoryNonce replacement{open.state.history_nonce.value + 100};
    Message reset = HistoryReset{
        replacement, initial_route_digest(c_guid, replacement)};
    try {
        co_await raw_write(socket, std::move(reset));
        const Frame reply =
            co_await raw_read(socket, open.state.limits.max_frame_payload);
        if (reply.type == MessageType::ERROR) {
            (void)raw_decode<ErrorMessage>(reply);
            co_return;
        }
        throw std::logic_error("stale candidate received a non-ERROR reply");
    } catch (const boost::system::system_error& error) {
        if (error.code() != asio::error::eof &&
            error.code() != asio::error::connection_reset &&
            error.code() != asio::error::broken_pipe)
            throw;
    }
}

asio::awaitable<void> raw_commit_after_candidate_staged(
    tcp::endpoint remote, CStoreGuid c_guid, std::span<const uint8_t> input,
    CandidateRevisionRace& coordination) {
    const auto executor = co_await asio::this_coro::executor;
    asio::steady_timer timer(executor);
    while (!coordination.first_staged) {
        timer.expires_after(std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
    }
    tcp::socket socket(executor);
    const RawRoute open = co_await raw_open(socket, remote, c_guid);
    if (!open.state.route_present)
        throw std::logic_error("candidate-revision fixture has no established route");
    const ZstdTuEnvelope prepared = encode_zstd_tu(
        HistoryNonce{1}, RelSeq{0}, TuSeq{15}, Digest128{}, input);
    const TxBegin begin = make_begin(prepared, open.state.history_nonce,
                                     open.state.state_digest,
                                     open.state.next_rel_seq);
    co_await raw_write(socket, begin);
    Message body = BodyMessage{prepared.body};
    co_await raw_write(socket, std::move(body));
    const TxCommit commit = raw_decode<TxCommit>(
        co_await raw_read(socket, open.state.limits.max_frame_payload));
    if (commit.transaction_digest != begin.transaction_digest ||
        commit.raw_digest != begin.raw_digest)
        throw std::logic_error("candidate-revision fixture received another commit");
    boost::system::error_code ignored;
    socket.close(ignored);
    coordination.route_changed = true;
}

struct InterruptedRawTu {
    TxBegin begin;
    std::vector<uint8_t> body;
};

enum class TerminalBodyFailure {
    ZeroProgress,
    Excess,
    ComponentDigest,
    ZstdDecode,
    RawDigest,
};

asio::awaitable<InterruptedRawTu> raw_partial_body_disconnect(
    tcp::endpoint remote, CStoreGuid c_guid, std::span<const uint8_t> input) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    RawRoute open = co_await raw_open(socket, remote, c_guid);
    SessionState route = open.state;
    if (!route.route_present)
        route = co_await raw_reset(socket, open, HistoryNonce{121});
    ZstdTuEnvelope prepared = encode_zstd_tu(
        HistoryNonce{1}, RelSeq{0}, TuSeq{21}, Digest128{}, input);
    TxBegin begin = make_begin(prepared, route.history_nonce,
                               route.state_digest, route.next_rel_seq);
    co_await raw_write(socket, begin);
    if (prepared.body.size() < 2)
        throw std::logic_error("partial-BODY fixture encoded fewer than two bytes");
    BodyMessage partial;
    partial.bytes.assign(prepared.body.begin(),
                         prepared.body.begin() + prepared.body.size() / 2);
    Message partial_message = std::move(partial);
    co_await raw_write(socket, std::move(partial_message));
    boost::system::error_code ignored;
    socket.close(ignored);
    co_return InterruptedRawTu{std::move(begin), std::move(prepared.body)};
}

asio::awaitable<InterruptedRawTu> raw_terminal_body_failure(
    tcp::endpoint remote, CStoreGuid c_guid, std::span<const uint8_t> input,
    TerminalBodyFailure scenario, bool receive_terminal = true) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    RawRoute open = co_await raw_open(socket, remote, c_guid);
    SessionState route = open.state;
    if (!route.route_present)
        route = co_await raw_reset(socket, open, HistoryNonce{701});
    ZstdTuEnvelope prepared = encode_zstd_tu(
        HistoryNonce{1}, RelSeq{0}, TuSeq{81}, Digest128{}, input);
    TxBegin begin = make_begin(prepared, route.history_nonce,
                               route.state_digest, route.next_rel_seq);
    std::vector<uint8_t> sent_body = prepared.body;
    if (scenario == TerminalBodyFailure::ZstdDecode) {
        std::fill(sent_body.begin(), sent_body.end(), uint8_t{0});
        begin.body = describe_component(kZstdTuBodyEncoding, sent_body,
                                        begin.raw_bytes);
        begin.transaction_digest = compute_transaction_digest(begin, sent_body);
        prepared.body = sent_body;
    } else if (scenario == TerminalBodyFailure::RawDigest) {
        begin.raw_digest.bytes[0] ^= 0x80;
        begin.transaction_digest = compute_transaction_digest(begin, sent_body);
    }

    co_await raw_write(socket, begin);
    switch (scenario) {
    case TerminalBodyFailure::ZeroProgress:
        sent_body.clear();
        break;
    case TerminalBodyFailure::Excess:
        sent_body.push_back(0xff);
        break;
    case TerminalBodyFailure::ComponentDigest:
        if (sent_body.empty())
            throw std::logic_error("component-digest fixture encoded an empty BODY");
        sent_body.front() ^= 0x80;
        break;
    case TerminalBodyFailure::ZstdDecode:
    case TerminalBodyFailure::RawDigest:
        break;
    }
    Message body_message = BodyMessage{std::move(sent_body)};
    co_await raw_write(socket, std::move(body_message));
    if (!receive_terminal) {
        boost::system::error_code ignored;
        socket.close(ignored);
        co_return InterruptedRawTu{std::move(begin), std::move(prepared.body)};
    }
    const Frame terminal =
        co_await raw_read(socket, route.limits.max_frame_payload);
    if (terminal.type != MessageType::ERROR)
        throw std::logic_error("terminal BODY fixture did not receive ERROR");
    (void)raw_decode<ErrorMessage>(terminal);
    co_await raw_wait_for_close(socket);
    co_return InterruptedRawTu{std::move(begin), std::move(prepared.body)};
}

asio::awaitable<void> raw_different_begin_rejected(
    tcp::endpoint remote, CStoreGuid c_guid, const TxBegin& interrupted) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    const RawRoute open = co_await raw_open(socket, remote, c_guid);
    const std::vector<uint8_t> different_input =
        pseudo_random_bytes(4097);
    const ZstdTuEnvelope different = encode_zstd_tu(
        HistoryNonce{1}, RelSeq{0}, TuSeq{interrupted.tu_seq.value + 1},
        Digest128{}, different_input);
    const TxBegin begin = make_begin(different, open.state.history_nonce,
                                     open.state.state_digest,
                                     open.state.next_rel_seq);
    if (begin == interrupted)
        throw std::logic_error("different-begin fixture reproduced the same identity");
    co_await raw_write(socket, begin);
    const Frame terminal =
        co_await raw_read(socket, open.state.limits.max_frame_payload);
    if (terminal.type != MessageType::ERROR)
        throw std::logic_error("different begin did not receive terminal ERROR");
    (void)raw_decode<ErrorMessage>(terminal);
    co_await raw_wait_for_close(socket);
}

asio::awaitable<void> raw_exact_whole_tu_replay(
    tcp::endpoint remote, CStoreGuid c_guid, const InterruptedRawTu& interrupted) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    const RawRoute open = co_await raw_open(socket, remote, c_guid);
    if (open.state.history_nonce != interrupted.begin.history_nonce ||
        open.state.next_rel_seq != interrupted.begin.rel_seq ||
        open.state.state_digest != interrupted.begin.pre_state_digest)
        throw std::logic_error("interrupted route cursor changed before exact replay");
    co_await raw_write(socket, interrupted.begin);
    Message body = BodyMessage{interrupted.body};
    co_await raw_write(socket, std::move(body));
    const TxCommit commit = raw_decode<TxCommit>(
        co_await raw_read(socket, open.state.limits.max_frame_payload));
    if (commit.transaction_digest != interrupted.begin.transaction_digest ||
        commit.raw_digest != interrupted.begin.raw_digest)
        throw std::logic_error("exact replay received a different commit identity");
    boost::system::error_code ignored;
    socket.close(ignored);
}

asio::awaitable<void> raw_exact_whole_tu_rejected(
    tcp::endpoint remote, CStoreGuid c_guid,
    const InterruptedRawTu& interrupted) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    const RawRoute open = co_await raw_open(socket, remote, c_guid);
    if (open.state.history_nonce != interrupted.begin.history_nonce ||
        open.state.next_rel_seq != interrupted.begin.rel_seq ||
        open.state.state_digest != interrupted.begin.pre_state_digest)
        throw std::logic_error(
            "deterministic-failure route cursor changed before exact replay");
    co_await raw_write(socket, interrupted.begin);
    Message body_message = BodyMessage{interrupted.body};
    co_await raw_write(socket, std::move(body_message));
    const Frame terminal =
        co_await raw_read(socket, open.state.limits.max_frame_payload);
    if (terminal.type != MessageType::ERROR)
        throw std::logic_error(
            "deterministic-failure exact replay did not receive ERROR");
    (void)raw_decode<ErrorMessage>(terminal);
    co_await raw_wait_for_close(socket);
}

asio::awaitable<RawRoute> raw_open(tcp::socket& socket, const tcp::endpoint& remote,
                                   CStoreGuid c_guid, SessionLimits limits) {
    co_await socket.async_connect(remote, asio::use_awaitable);
    SessionHello hello;
    hello.c_store_guid = c_guid;
    hello.supported_profiles = profile_bit(ProfileId::ZSTD_TU);
    hello.limits = limits;
    co_await raw_write(socket, hello);
    SessionState state =
        raw_decode<SessionState>(co_await raw_read(socket, limits.max_frame_payload));
    validate_session_state(hello, state);
    co_return RawRoute{hello, state};
}

asio::awaitable<SessionState> raw_reset(tcp::socket& socket, const RawRoute& open,
                                        HistoryNonce nonce) {
    const HistoryReset reset{nonce, initial_route_digest(open.hello.c_store_guid, nonce)};
    co_await raw_write(socket, reset);
    SessionState state =
        raw_decode<SessionState>(co_await raw_read(socket, open.state.limits.max_frame_payload));
    if (!state.route_present || state.history_nonce != nonce || state.next_rel_seq.value != 0 ||
        state.state_digest != reset.initial_state_digest)
        throw std::logic_error("raw HISTORY_RESET acknowledgement differs");
    co_return state;
}

asio::awaitable<void> raw_reset_only(tcp::endpoint remote, CStoreGuid c_guid, HistoryNonce nonce,
                                     SessionLimits limits = {}) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    const RawRoute open = co_await raw_open(socket, remote, c_guid, limits);
    (void)co_await raw_reset(socket, open, nonce);
    boost::system::error_code ignored;
    socket.close(ignored);
    co_return;
}

asio::awaitable<bool> raw_reset_rejected(tcp::endpoint remote, CStoreGuid c_guid,
                                         HistoryNonce nonce, SessionLimits limits = {}) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    const RawRoute open = co_await raw_open(socket, remote, c_guid, limits);
    co_await raw_write(socket,
                       HistoryReset{nonce, initial_route_digest(c_guid, nonce)});
    const Frame reply = co_await raw_read(socket, open.state.limits.max_frame_payload);
    co_return reply.type == MessageType::ERROR;
}

asio::awaitable<void> raw_wait_for_close(tcp::socket& socket) {
    std::array<uint8_t, 1> extra{};
    boost::system::error_code error;
    (void)co_await socket.async_read_some(
        asio::buffer(extra), asio::redirect_error(asio::use_awaitable, error));
    if (error != asio::error::eof && error != asio::error::connection_reset)
        throw std::logic_error("client did not close after its terminal result");
}

enum class ResetAckMutation {
    WireRevision,
    ProfileMask,
    FrameLimit,
    FillLimit,
};

struct ScriptedSessionState {
    FStoreGuid f_guid{};
    bool route_present = false;
    HistoryNonce history_nonce{};
    RelSeq next_rel_seq{};
    Digest128 state_digest{};
};

asio::awaitable<void> raw_bad_reset_ack_peer(tcp::acceptor& acceptor,
                                             ScriptedSessionState scripted,
                                             ResetAckMutation mutation) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    co_await acceptor.async_accept(socket, asio::use_awaitable);
    const SessionHello hello = raw_decode<SessionHello>(
        co_await raw_read(socket, kInitialMaxFramePayload));

    SessionState initial;
    initial.wire_revision = kP50WireRevision;
    initial.negotiated_profiles = profile_bit(ProfileId::ZSTD_TU);
    initial.limits = hello.limits;
    initial.f_store_guid = scripted.f_guid;
    initial.namespace_present = scripted.route_present;
    initial.route_present = scripted.route_present;
    initial.history_nonce = scripted.history_nonce;
    initial.next_rel_seq = scripted.next_rel_seq;
    initial.state_digest = scripted.state_digest;
    co_await raw_write(socket, initial);

    const HistoryReset reset = raw_decode<HistoryReset>(
        co_await raw_read(socket, initial.limits.max_frame_payload));
    SessionState ack = initial;
    ack.namespace_present = true;
    ack.route_present = true;
    ack.history_nonce = reset.history_nonce;
    ack.next_rel_seq = RelSeq{0};
    ack.state_digest = reset.initial_state_digest;
    if (mutation == ResetAckMutation::ProfileMask) {
        // Use the other runnable source profile so the peer can encode the
        // state and the client, rather than the fixture itself, observes the
        // negotiated-profile mismatch.
        ack.negotiated_profiles = profile_bit(ProfileId::ZSTD_ROUTE);
        co_await raw_write(socket, ack);
    } else if (mutation == ResetAckMutation::FrameLimit) {
        require(ack.limits.max_frame_payload > kMandatoryControlFramePayload,
                "reset-ack frame-limit fixture lacks room to decrease");
        --ack.limits.max_frame_payload;
        co_await raw_write(socket, ack);
    } else if (mutation == ResetAckMutation::FillLimit) {
        require(ack.limits.max_fill_record_bytes > 32,
                "reset-ack FILL-limit fixture lacks room to decrease");
        --ack.limits.max_fill_record_bytes;
        co_await raw_write(socket, ack);
    } else {
        std::vector<uint8_t> frame = encode_frame(ack);
        frame[4] = 0;
        frame[5] = static_cast<uint8_t>(kP50WireRevision + 1);
        co_await raw_write_bytes(socket, frame);
    }
    co_await raw_wait_for_close(socket);
}

asio::awaitable<void> raw_bounded_error_peer(tcp::acceptor& acceptor, uint32_t payload_cap) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    co_await acceptor.async_accept(socket, asio::use_awaitable);
    const SessionHello hello = raw_decode<SessionHello>(
        co_await raw_read(socket, kInitialMaxFramePayload));
    if (hello.limits.max_frame_payload != payload_cap || payload_cap < 6)
        throw std::logic_error("bounded terminal fixture negotiated an unexpected cap");
    Message terminal = ErrorMessage{91, std::string(payload_cap - 6, 'e')};
    co_await raw_write(socket, std::move(terminal));
    co_await raw_wait_for_close(socket);
}

asio::awaitable<void> raw_unknown_frame_peer(tcp::acceptor& acceptor) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    co_await acceptor.async_accept(socket, asio::use_awaitable);
    (void)raw_decode<SessionHello>(
        co_await raw_read(socket, kInitialMaxFramePayload));
    const std::array<uint8_t, 4> unknown_header{0xff, 0, 0, 0};
    co_await raw_write_bytes(socket, unknown_header);
    co_await raw_wait_for_close(socket);
}

asio::awaitable<void> raw_route_mismatch_peer(tcp::acceptor& acceptor,
                                              ScriptedSessionState scripted,
                                              bool expect_error = true) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    co_await acceptor.async_accept(socket, asio::use_awaitable);
    const SessionHello hello = raw_decode<SessionHello>(
        co_await raw_read(socket, kInitialMaxFramePayload));

    SessionState state;
    state.wire_revision = kP50WireRevision;
    state.negotiated_profiles = profile_bit(ProfileId::ZSTD_TU);
    state.limits = hello.limits;
    state.f_store_guid = scripted.f_guid;
    state.namespace_present = true;
    state.route_present = true;
    state.history_nonce = scripted.history_nonce;
    state.next_rel_seq = scripted.next_rel_seq;
    state.state_digest = scripted.state_digest;
    co_await raw_write(socket, state);
    if (expect_error) {
        const ErrorMessage terminal = raw_decode<ErrorMessage>(
            co_await raw_read(socket, state.limits.max_frame_payload));
        if (terminal.code == 0)
            throw std::logic_error("route mismatch received a zero-code ERROR");
    }
    co_await raw_wait_for_close(socket);
}

asio::awaitable<void> raw_zero_f_store_state_peer(tcp::acceptor& acceptor) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    co_await acceptor.async_accept(socket, asio::use_awaitable);
    const SessionHello hello = raw_decode<SessionHello>(
        co_await raw_read(socket, kInitialMaxFramePayload));

    SessionState state;
    state.wire_revision = kP50WireRevision;
    state.negotiated_profiles = profile_bit(ProfileId::ZSTD_TU);
    state.limits = hello.limits;
    state.f_store_guid = Id128::from_u64(6000);
    std::vector<uint8_t> frame = encode_frame(Message{state});
    constexpr size_t frame_header_bytes = 4;
    constexpr size_t f_guid_payload_offset = 2 + 4 + 4 + 8;
    std::fill_n(frame.begin() + frame_header_bytes + f_guid_payload_offset,
                16, uint8_t{0});
    co_await raw_write_bytes(socket, frame);
    co_await raw_wait_for_close(socket);
}

asio::awaitable<void> raw_zero_c_store_hello(tcp::endpoint remote) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    co_await socket.async_connect(remote, asio::use_awaitable);
    SessionHello hello;
    hello.c_store_guid = Id128::from_u64(6001);
    hello.supported_profiles = profile_bit(ProfileId::ZSTD_TU);
    std::vector<uint8_t> frame = encode_frame(Message{hello});
    constexpr size_t frame_header_bytes = 4;
    constexpr size_t c_guid_payload_offset = 2;
    std::fill_n(frame.begin() + frame_header_bytes + c_guid_payload_offset,
                16, uint8_t{0});
    co_await raw_write_bytes(socket, frame);
    const ErrorMessage terminal = raw_decode<ErrorMessage>(
        co_await raw_read(socket, kInitialMaxFramePayload));
    if (terminal.code == 0)
        throw std::logic_error("zero C_STORE_GUID received a zero-code ERROR");
    co_await raw_wait_for_close(socket);
}

asio::awaitable<void> raw_zero_error_code_peer(tcp::acceptor& acceptor) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    co_await acceptor.async_accept(socket, asio::use_awaitable);
    (void)raw_decode<SessionHello>(
        co_await raw_read(socket, kInitialMaxFramePayload));

    std::vector<uint8_t> frame =
        encode_frame(Message{ErrorMessage{1, "reserved-code fixture"}});
    frame[4] = 0;
    frame[5] = 0;
    co_await raw_write_bytes(socket, frame);
    co_await raw_wait_for_close(socket);
}

asio::awaitable<void> raw_stall_after_connect(tcp::acceptor& acceptor) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    boost::system::error_code error;
    co_await acceptor.async_accept(socket, asio::redirect_error(asio::use_awaitable, error));
    if (error)
        co_return;
    std::array<uint8_t, 1> byte{};
    while (!error)
        co_await socket.async_read_some(asio::buffer(byte),
                                       asio::redirect_error(asio::use_awaitable, error));
    co_return;
}

asio::awaitable<void> raw_zero_history_reset(tcp::endpoint remote,
                                              CStoreGuid c_guid) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    const RawRoute open = co_await raw_open(socket, remote, c_guid);
    const HistoryReset valid{
        HistoryNonce{1}, initial_route_digest(c_guid, HistoryNonce{1})};
    std::vector<uint8_t> frame = encode_frame(Message{valid});
    std::fill_n(frame.begin() + 4, sizeof(uint64_t), uint8_t{0});
    co_await raw_write_bytes(socket, frame);
    const ErrorMessage terminal = raw_decode<ErrorMessage>(
        co_await raw_read(socket, open.state.limits.max_frame_payload));
    if (terminal.code == 0)
        throw std::logic_error("zero HISTORY_NONCE received a zero-code ERROR");
    co_await raw_wait_for_close(socket);
}

template <class Peer>
ClientRunResult run_client_with_raw_peer(P50ClientEndpoint& client,
                                         PreparedTuHandle prepared, Peer peer,
                                         EndpointIoControl control = {}) {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    std::future<void> peer_result =
        asio::co_spawn(context, peer(acceptor), asio::use_future);
    std::future<ClientRunResult> client_result = asio::co_spawn(
        context, client.run(acceptor.local_endpoint(), prepared, control),
        asio::use_future);
    context.run();
    peer_result.get();
    return client_result.get();
}

enum class RawCase {
    ShortBody,
    ExcessBody,
    EmptyBodyProgress,
    DescriptorCap,
    DecodedCap,
    FrameCap,
    ComponentDigest,
    TransactionDigest,
    RawDigest,
    WrongProfile,
    WrongEncoding,
    FrameContentSize,
    TrailingByte,
    AppendedEmptyFrame,
    AppendedNonemptyFrame,
};

struct RawCaseResult {
    bool received_error = false;
    bool closed_after_error = false;
    size_t error_payload_bytes = 0;
};

asio::awaitable<RawCaseResult> run_raw_case(tcp::endpoint remote, CStoreGuid c_guid,
                                            EndpointCaps server_caps, RawCase scenario) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    RawRoute open = co_await raw_open(socket, remote, c_guid, server_caps.wire);
    SessionState route = open.state;
    if (!route.route_present)
        route = co_await raw_reset(socket, open, HistoryNonce{71});

    if (scenario == RawCase::FrameCap) {
        const auto header =
            encode_frame_header(MessageType::TX_BEGIN, server_caps.wire.max_frame_payload + 1);
        co_await raw_write_bytes(socket, header);
    } else {
        const std::vector<uint8_t> raw = pseudo_random_bytes(512);
        const ZstdTuEnvelope prepared = encode_zstd_tu(
            HistoryNonce{1}, RelSeq{0}, TuSeq{9}, Digest128{}, raw);
        TxBegin begin =
            make_begin(prepared, route.history_nonce, route.state_digest, route.next_rel_seq);
        std::vector<uint8_t> body = prepared.body;
        std::vector<uint8_t> raw_begin_frame;
        if (scenario == RawCase::DescriptorCap) {
            body.assign(static_cast<size_t>(server_caps.zstd.max_encoded_body_bytes + 1),
                        uint8_t{0x5a});
            begin.body = describe_component(kZstdTuBodyEncoding, body, 1);
            begin.raw_bytes = 1;
            const std::array<uint8_t, 1> one_raw{0};
            begin.raw_digest = icecc::digest128(one_raw);
            begin.transaction_digest = compute_transaction_digest(begin, body);
        } else if (scenario == RawCase::DecodedCap) {
            begin.raw_bytes = server_caps.zstd.max_raw_bytes + 1;
            begin.body.decoded_bytes = begin.raw_bytes;
            begin.transaction_digest = compute_transaction_digest(begin, body);
        } else if (scenario == RawCase::WrongProfile) {
            raw_begin_frame = encode_frame(begin);
            raw_begin_frame[4 + 24] = 0;
            raw_begin_frame[4 + 25] = static_cast<uint8_t>(ProfileId::P29V1);
        } else if (scenario == RawCase::WrongEncoding) {
            begin.body.encoding = 77;
            begin.transaction_digest = compute_transaction_digest(begin, body);
        } else if (scenario == RawCase::ComponentDigest) {
            begin.body.digest.bytes[0] ^= 0x80;
        } else if (scenario == RawCase::TransactionDigest) {
            begin.transaction_digest.bytes[0] ^= 0x80;
        } else if (scenario == RawCase::RawDigest) {
            begin.raw_digest.bytes[0] ^= 0x80;
            begin.transaction_digest = compute_transaction_digest(begin, body);
        } else if (scenario == RawCase::FrameContentSize) {
            ++begin.raw_bytes;
            begin.body.decoded_bytes = begin.raw_bytes;
            begin.transaction_digest = compute_transaction_digest(begin, body);
        } else if (scenario == RawCase::TrailingByte) {
            body.push_back(0xa5);
            begin.body =
                describe_component(kZstdTuBodyEncoding, body, begin.raw_bytes);
            begin.transaction_digest = compute_transaction_digest(begin, body);
        } else if (scenario == RawCase::AppendedEmptyFrame) {
            const std::vector<uint8_t> appended = standalone_zstd_frame({});
            body.insert(body.end(), appended.begin(), appended.end());
            begin.body =
                describe_component(kZstdTuBodyEncoding, body, begin.raw_bytes);
            begin.transaction_digest = compute_transaction_digest(begin, body);
        } else if (scenario == RawCase::AppendedNonemptyFrame) {
            const std::vector<uint8_t> appended =
                standalone_zstd_frame(bytes("second frame"));
            body.insert(body.end(), appended.begin(), appended.end());
            begin.body =
                describe_component(kZstdTuBodyEncoding, body, begin.raw_bytes);
            begin.transaction_digest = compute_transaction_digest(begin, body);
        }
        if (raw_begin_frame.empty())
            co_await raw_write(socket, begin);
        else
            co_await raw_write_bytes(socket, raw_begin_frame);
        if (scenario == RawCase::DescriptorCap || scenario == RawCase::DecodedCap ||
            scenario == RawCase::WrongProfile ||
            scenario == RawCase::WrongEncoding) {
            // The descriptor itself is rejected before any component allocation.
        } else {
            if (scenario == RawCase::ShortBody) {
                require(body.size() > 1, "short-body fixture is too small");
                body.pop_back();
                BodyMessage short_body{body};
                co_await raw_write(socket, short_body);
                boost::system::error_code ignored;
                socket.close(ignored);
                co_return RawCaseResult{};
            }
            if (scenario == RawCase::ExcessBody)
                body.push_back(0xff);
            if (scenario == RawCase::EmptyBodyProgress)
                body.clear();
            Message body_message = BodyMessage{std::move(body)};
            co_await raw_write(socket, std::move(body_message));
        }
    }

    RawCaseResult result;
    Frame terminal = co_await raw_read(socket, server_caps.wire.max_frame_payload);
    result.received_error = terminal.type == MessageType::ERROR;
    result.error_payload_bytes = terminal.payload.size();
    if (result.received_error)
        (void)raw_decode<ErrorMessage>(terminal);
    std::array<uint8_t, 4> next{};
    boost::system::error_code error;
    (void)co_await asio::async_read(socket, asio::buffer(next),
                                    asio::redirect_error(asio::use_awaitable, error));
    result.closed_after_error = error == asio::error::eof || error == asio::error::connection_reset;
    co_return result;
}

std::pair<RawCaseResult, ServerRunResult> run_raw_server_case(EndpointCaps caps, RawCase scenario,
                                                              uint64_t c_id) {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    P50ServerEndpoint server(Id128::from_u64(900 + c_id), caps);
    std::future<ServerRunResult> server_future =
        asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
    std::future<RawCaseResult> peer_future = asio::co_spawn(
        context, run_raw_case(acceptor.local_endpoint(), Id128::from_u64(c_id), caps, scenario),
        asio::use_future);
    context.run();
    return {peer_future.get(), server_future.get()};
}

ServerRunResult force_route_reset(P50ServerEndpoint& server, CStoreGuid c_guid,
                                  HistoryNonce nonce) {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    std::future<ServerRunResult> server_future =
        asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
    std::future<void> peer_future = asio::co_spawn(
        context, raw_reset_only(acceptor.local_endpoint(), c_guid, nonce), asio::use_future);
    context.run();
    peer_future.get();
    return server_future.get();
}


bool force_rejected_route_reset(P50ServerEndpoint& server, CStoreGuid c_guid,
                                HistoryNonce nonce) {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    std::future<ServerRunResult> server_future =
        asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
    std::future<bool> peer_future = asio::co_spawn(
        context, raw_reset_rejected(acceptor.local_endpoint(), c_guid, nonce), asio::use_future);
    context.run();
    const bool rejected = peer_future.get();
    require(server_future.get().status == ServerRunStatus::TerminalError,
            "invalid route reset did not terminate the F session");
    return rejected;
}

std::pair<InterruptedRawTu, ServerRunResult> force_terminal_body_failure(
    P50ServerEndpoint& server, CStoreGuid c_guid,
    std::span<const uint8_t> input, TerminalBodyFailure scenario,
    bool receive_terminal = true) {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    std::future<ServerRunResult> server_future =
        asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
    std::future<InterruptedRawTu> peer_future = asio::co_spawn(
        context,
        raw_terminal_body_failure(acceptor.local_endpoint(), c_guid, input,
                                  scenario, receive_terminal),
        asio::use_future);
    context.run();
    InterruptedRawTu interrupted = peer_future.get();
    return {std::move(interrupted), server_future.get()};
}

ServerRunResult force_different_begin_rejected(
    P50ServerEndpoint& server, CStoreGuid c_guid, const TxBegin& interrupted) {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    std::future<ServerRunResult> server_future =
        asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
    std::future<void> peer_future = asio::co_spawn(
        context,
        raw_different_begin_rejected(acceptor.local_endpoint(), c_guid,
                                     interrupted),
        asio::use_future);
    context.run();
    peer_future.get();
    return server_future.get();
}

ServerRunResult force_exact_whole_tu_replay(
    P50ServerEndpoint& server, CStoreGuid c_guid,
    const InterruptedRawTu& interrupted, bool expect_commit) {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    std::future<ServerRunResult> server_future =
        asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
    std::future<void> peer_future;
    if (expect_commit) {
        peer_future = asio::co_spawn(
            context,
            raw_exact_whole_tu_replay(acceptor.local_endpoint(), c_guid,
                                      interrupted),
            asio::use_future);
    } else {
        peer_future = asio::co_spawn(
            context,
            raw_exact_whole_tu_rejected(acceptor.local_endpoint(), c_guid,
                                        interrupted),
            asio::use_future);
    }
    context.run();
    peer_future.get();
    return server_future.get();
}

void test_normal_zero_and_completion_stamps() {
    CompletionLog completions;
    ActionTrace actions;
    P50ServerEndpoint server(Id128::from_u64(200), {}, &completions, &actions);
    TestClient client(Id128::from_u64(100), {}, HistoryNonce{10}, &completions, &actions);
    const std::vector<uint8_t> input = pseudo_random_bytes(4096);
    const PairResult first = run_pair(client, server, admit(client, input));
    require(first.client.status == ClientRunStatus::Committed &&
                first.server.status == ServerRunStatus::Completed,
            "normal loopback did not complete");
    require(first.client.reconnect == EndpointReconnectOutcome::ColdFStore,
            "first loopback did not take the explicit cold-F path");
    require(copy_input(server, client.c_store_guid()) == input,
            "normal loopback did not retain exact input");

    const std::vector<uint8_t> empty;
    const PairResult second = run_pair(client, server, admit(client, empty));
    require(second.client.status == ClientRunStatus::Committed &&
                second.client.reconnect == EndpointReconnectOutcome::ExactMatch,
            "zero-length component transaction did not complete on the exact route");
    require(copy_input(server, client.c_store_guid()) == empty,
            "zero-length ZSTD_TU did not materialize exactly");

    size_t bound_c = 0;
    size_t bound_f = 0;
    for (const AsyncCompletion& completion : completions.completions()) {
        require(completion.stamp.session_serial != 0,
                "async completion omitted its session serial");
        (void)async_operation_name(completion.stamp.operation);
        if (completion.stamp.transaction_bound) {
            require(completion.stamp.history_nonce.value != 0 &&
                        nonzero(completion.stamp.transaction_digest),
                    "transaction-bound completion omitted route identity");
            if (completion.stamp.actor == ActorSide::C)
                ++bound_c;
            else
                ++bound_f;
        }
    }
    require(bound_c != 0 && bound_f != 0,
            "loopback did not capture full transaction identity on both sides");
    require_trace(actions, "normal/zero completion trace");
}

enum class CompletionStampField {
    Actor,
    Operation,
    CStoreGuid,
    FStoreGuid,
    SessionSerial,
    HistoryNonce,
    RelSeq,
    TuSeq,
    TransactionDigest,
    RawDigest,
    TransactionBound,
};

void change_completion_field(CompletionStamp& stamp, CompletionStampField field) {
    switch (field) {
    case CompletionStampField::Actor:
        stamp.actor = stamp.actor == ActorSide::C ? ActorSide::F : ActorSide::C;
        break;
    case CompletionStampField::Operation:
        stamp.operation = stamp.operation == AsyncOperationKind::WriteFragment
                              ? AsyncOperationKind::ReadPayload
                              : AsyncOperationKind::WriteFragment;
        break;
    case CompletionStampField::CStoreGuid:
        stamp.c_store_guid.bytes[0] ^= 0x80;
        break;
    case CompletionStampField::FStoreGuid:
        stamp.f_store_guid.bytes[0] ^= 0x80;
        break;
    case CompletionStampField::SessionSerial:
        ++stamp.session_serial;
        break;
    case CompletionStampField::HistoryNonce:
        ++stamp.history_nonce.value;
        break;
    case CompletionStampField::RelSeq:
        ++stamp.rel_seq.value;
        break;
    case CompletionStampField::TuSeq:
        ++stamp.tu_seq.value;
        break;
    case CompletionStampField::TransactionDigest:
        stamp.transaction_digest.bytes[0] ^= 0x80;
        break;
    case CompletionStampField::RawDigest:
        stamp.raw_digest.bytes[0] ^= 0x80;
        break;
    case CompletionStampField::TransactionBound:
        stamp.transaction_bound = !stamp.transaction_bound;
        break;
    }
}

void change_live_identity_field(CompletionLiveIdentity& identity,
                                CompletionStampField field) {
    switch (field) {
    case CompletionStampField::Actor:
        identity.actor = identity.actor == ActorSide::C ? ActorSide::F : ActorSide::C;
        break;
    case CompletionStampField::Operation:
        fail("operation is not part of the live endpoint identity");
    case CompletionStampField::CStoreGuid:
        identity.c_store_guid.bytes[0] ^= 0x80;
        break;
    case CompletionStampField::FStoreGuid:
        identity.f_store_guid.bytes[0] ^= 0x80;
        break;
    case CompletionStampField::SessionSerial:
        ++identity.session_serial;
        break;
    case CompletionStampField::HistoryNonce:
        ++identity.history_nonce.value;
        break;
    case CompletionStampField::RelSeq:
        ++identity.rel_seq.value;
        break;
    case CompletionStampField::TuSeq:
        ++identity.tu_seq.value;
        break;
    case CompletionStampField::TransactionDigest:
        identity.transaction_digest.bytes[0] ^= 0x80;
        break;
    case CompletionStampField::RawDigest:
        identity.raw_digest.bytes[0] ^= 0x80;
        break;
    case CompletionStampField::TransactionBound:
        identity.transaction_bound = !identity.transaction_bound;
        break;
    }
}

void test_completion_stamp_correspondence() {
    const std::array fields{
        std::pair{CompletionStampField::Actor, "actor"},
        std::pair{CompletionStampField::Operation, "operation"},
        std::pair{CompletionStampField::CStoreGuid, "C_STORE_GUID"},
        std::pair{CompletionStampField::FStoreGuid, "F_STORE_GUID"},
        std::pair{CompletionStampField::SessionSerial, "session serial"},
        std::pair{CompletionStampField::HistoryNonce, "HISTORY_NONCE"},
        std::pair{CompletionStampField::RelSeq, "REL_SEQ"},
        std::pair{CompletionStampField::TuSeq, "TU_SEQ"},
        std::pair{CompletionStampField::TransactionDigest, "transaction digest"},
        std::pair{CompletionStampField::RawDigest, "raw digest"},
        std::pair{CompletionStampField::TransactionBound, "transaction-bound flag"},
    };

    uint64_t identity = 3000;
    for (const ActorSide actor : {ActorSide::C, ActorSide::F}) {
        const AsyncOperationKind target = actor == ActorSide::C
                                              ? AsyncOperationKind::WriteFragment
                                              : AsyncOperationKind::ReadPayload;
        for (const auto& [field, name] : fields) {
            const CompletionStampField selected_field = field;
            const std::string_view field_name = name;
            P50ServerEndpoint server(Id128::from_u64(identity++));
            TestClient client(Id128::from_u64(identity++));
            const std::vector<uint8_t> input = pseudo_random_bytes(4096);
            bool changed = false;
            EndpointIoControl client_control;
            EndpointIoControl server_control;
            EndpointIoControl& selected =
                actor == ActorSide::C ? client_control : server_control;
            selected.before_completion_check = [&](CompletionStamp& observed) {
                if (changed || observed.actor != actor ||
                    observed.operation != target || !observed.transaction_bound)
                    return;
                change_completion_field(observed, selected_field);
                changed = true;
            };

            const PairResult rejected = run_pair(client, server, admit(client, input),
                                                 client_control, server_control);
            std::string context = actor == ActorSide::C ? "C " : "F ";
            context.append(field_name);
            require(changed, context + " completion mutation did not run");
            require(rejected.client.status == ClientRunStatus::Disconnected &&
                        rejected.server.status == ServerRunStatus::Disconnected &&
                        client.has_active_transaction() &&
                        !copy_input(server, client.c_store_guid()),
                    context + " completion mutation was not rejected exactly");

            const PairResult replayed = run_pair(client, server);
            require(replayed.client.status == ClientRunStatus::Committed &&
                        replayed.server.status == ServerRunStatus::Completed &&
                        replayed.client.reconnect == EndpointReconnectOutcome::ExactMatch &&
                        !client.has_active_transaction() &&
                        copy_input(server, client.c_store_guid()) == input,
                    context + " completion rejection did not permit exact follow-up");
        }
    }

    P50ServerEndpoint server(Id128::from_u64(identity++));
    TestClient client(Id128::from_u64(identity++));
    bool saw_unbound_c = false;
    bool saw_unbound_f = false;
    EndpointIoControl client_control;
    EndpointIoControl server_control;
    client_control.before_completion_check = [&](CompletionStamp& observed) {
        saw_unbound_c = saw_unbound_c ||
                        (observed.actor == ActorSide::C && !observed.transaction_bound);
    };
    server_control.before_completion_check = [&](CompletionStamp& observed) {
        saw_unbound_f = saw_unbound_f ||
                        (observed.actor == ActorSide::F && !observed.transaction_bound);
    };
    const std::vector<uint8_t> first_input = pseudo_random_bytes(1024);
    const PairResult first = run_pair(client, server, admit(client, first_input),
                                      client_control, server_control);
    require(first.client.status == ClientRunStatus::Committed && saw_unbound_c && saw_unbound_f,
            "legal non-transaction completions did not complete on both endpoints");
    const std::vector<uint8_t> second_input = pseudo_random_bytes(1536);
    const PairResult follow_up = run_pair(client, server, admit(client, second_input));
    require(follow_up.client.status == ClientRunStatus::Committed &&
                follow_up.server.status == ServerRunStatus::Completed &&
                follow_up.client.reconnect == EndpointReconnectOutcome::ExactMatch &&
                copy_input(server, client.c_store_guid()) == second_input,
            "legal non-transaction completions did not preserve exact follow-up");
}

void test_completion_live_identity_correspondence() {
    const std::array fields{
        std::pair{CompletionStampField::Actor, "actor"},
        std::pair{CompletionStampField::CStoreGuid, "C_STORE_GUID"},
        std::pair{CompletionStampField::FStoreGuid, "F_STORE_GUID"},
        std::pair{CompletionStampField::SessionSerial, "session serial"},
        std::pair{CompletionStampField::HistoryNonce, "HISTORY_NONCE"},
        std::pair{CompletionStampField::RelSeq, "REL_SEQ"},
        std::pair{CompletionStampField::TuSeq, "TU_SEQ"},
        std::pair{CompletionStampField::TransactionDigest, "transaction digest"},
        std::pair{CompletionStampField::RawDigest, "raw digest"},
        std::pair{CompletionStampField::TransactionBound, "transaction-bound state"},
    };

    uint64_t identity = 4000;
    size_t rejection_gaps = 0;
    for (const ActorSide actor : {ActorSide::C, ActorSide::F}) {
        const AsyncOperationKind target = actor == ActorSide::C
                                              ? AsyncOperationKind::WriteFragment
                                              : AsyncOperationKind::ReadPayload;
        for (const auto& [field, name] : fields) {
            const CompletionStampField selected_field = field;
            const std::string_view field_name = name;
            P50ServerEndpoint server(Id128::from_u64(identity++));
            TestClient client(Id128::from_u64(identity++));
            const std::vector<uint8_t> input = pseudo_random_bytes(4096);
            bool changed = false;
            EndpointIoControl client_control;
            EndpointIoControl server_control;
            EndpointIoControl& selected =
                actor == ActorSide::C ? client_control : server_control;
            selected.before_live_identity_check =
                [&](const CompletionStamp& expected, CompletionLiveIdentity& live) {
                    if (changed || expected.actor != actor ||
                        expected.operation != target || !expected.transaction_bound)
                        return;
                    change_live_identity_field(live, selected_field);
                    changed = true;
                };

            const PairResult rejected = run_pair(client, server, admit(client, input),
                                                 client_control, server_control);
            std::string context = actor == ActorSide::C ? "C live " : "F live ";
            context.append(field_name);
            require(changed, context + " mutation did not run");
            const bool rejected_exactly =
                rejected.client.status == ClientRunStatus::Disconnected &&
                rejected.server.status == ServerRunStatus::Disconnected &&
                client.has_active_transaction() &&
                !copy_input(server, client.c_store_guid());
            if (!rejected_exactly) {
                std::cerr << "p50_endpoint_test: " << context
                          << " mutation was not rejected exactly\n";
                ++rejection_gaps;
                continue;
            }

            const PairResult replayed = run_pair(client, server);
            require(replayed.client.status == ClientRunStatus::Committed &&
                        replayed.server.status == ServerRunStatus::Completed &&
                        replayed.client.reconnect == EndpointReconnectOutcome::ExactMatch &&
                        !client.has_active_transaction() &&
                        copy_input(server, client.c_store_guid()) == input,
                    context + " rejection did not permit exact follow-up");
        }
    }
    require(rejection_gaps == 0,
            "one or more live identity mutations were not rejected exactly");

    P50ServerEndpoint server(Id128::from_u64(identity++));
    TestClient client(Id128::from_u64(identity++));
    bool saw_unbound_c = false;
    bool saw_unbound_f = false;
    EndpointIoControl client_control;
    EndpointIoControl server_control;
    client_control.before_live_identity_check =
        [&](const CompletionStamp& expected, CompletionLiveIdentity& live) {
            saw_unbound_c = saw_unbound_c ||
                            (expected.actor == ActorSide::C &&
                             !expected.transaction_bound && !live.transaction_bound);
        };
    server_control.before_live_identity_check =
        [&](const CompletionStamp& expected, CompletionLiveIdentity& live) {
            saw_unbound_f = saw_unbound_f ||
                            (expected.actor == ActorSide::F &&
                             !expected.transaction_bound && !live.transaction_bound);
        };
    const std::vector<uint8_t> first_input = pseudo_random_bytes(1024);
    const PairResult first = run_pair(client, server, admit(client, first_input),
                                      client_control, server_control);
    require(first.client.status == ClientRunStatus::Committed && saw_unbound_c && saw_unbound_f,
            "legal unbound live identities did not complete on both endpoints");
    const std::vector<uint8_t> second_input = pseudo_random_bytes(1536);
    const PairResult follow_up = run_pair(client, server, admit(client, second_input));
    require(follow_up.client.status == ClientRunStatus::Committed &&
                follow_up.server.status == ServerRunStatus::Completed &&
                follow_up.client.reconnect == EndpointReconnectOutcome::ExactMatch &&
                copy_input(server, client.c_store_guid()) == second_input,
            "legal unbound live identities did not preserve exact follow-up");
}

void test_idempotent_prepare_admission() {
    TestClient client(Id128::from_u64(150));
    const PrepareRequestKey request{7, 91};
    const std::vector<uint8_t> input = bytes("one canonical local request\n");
    const PreparedTuHandle first = client.prepare(request, input);
    const PreparedTuHandle replay = client.prepare(request, input);
    require(first == replay && client.authority->live_entry_count() == 1 &&
                client.authority->retained_encoded_bytes() != 0,
            "same prepare key/input did not return one retained authority entry");
    std::vector<uint8_t> same_length_different = input;
    same_length_different[same_length_different.size() / 2] ^= 1;
    require_throws<std::invalid_argument>(
        [&] { (void)client.prepare(request, same_length_different); },
        "prepare key was rebound to different same-length input");

    auto other_authority =
        std::make_shared<P50PreparationAuthority>(Id128::from_u64(149));
    require_throws<std::invalid_argument>([&] { (void)other_authority->retain(first); },
                                          "foreign preparation handle was accepted");
    require(client.authority->release(replay) == 0 &&
                client.authority->live_entry_count() == 0 &&
                client.authority->retained_encoded_bytes() == 0,
            "idempotent replay acquired an additional reference");
    require_throws<std::invalid_argument>([&] { (void)client.authority->release(first); },
                                          "zero-reference handle was released twice");

    const PreparedTuHandle retained = client.prepare({7, 92}, input);
    require(client.authority->retain(retained) == 2 &&
                client.authority->release(retained) == 1 &&
                client.authority->release(retained) == 0,
            "explicit retain did not represent one additional owner");

    PreparationAuthorityLimits one_entry;
    one_entry.max_live_entries = 1;
    auto bounded = std::make_shared<P50PreparationAuthority>(
        Id128::from_u64(148), EndpointCaps{}.zstd, one_entry);
    const PreparedTuHandle bounded_first =
        bounded->prepare(PrepareRequestKey{9, 1}, bytes("bounded first\n"));
    const PreparedTuHandle bounded_replay =
        bounded->prepare(PrepareRequestKey{9, 1}, bytes("bounded first\n"));
    require(bounded_first == bounded_replay && bounded->live_entry_count() == 1,
            "idempotent replay consumed another bounded entry");
    require_throws<std::length_error>(
        [&] { (void)bounded->prepare(PrepareRequestKey{9, 2}, bytes("bounded second\n")); },
        "preparation authority exceeded its live-entry bound");
    require(bounded->release(bounded_replay) == 0,
            "bounded replay acquired an additional reference");
    require_throws<std::invalid_argument>(
        [&] { (void)bounded->release(bounded_first); },
        "bounded replay retained an invisible owner");

    PreparationAuthorityLimits one_byte;
    one_byte.max_retained_encoded_bytes = 1;
    P50PreparationAuthority byte_bounded(Id128::from_u64(147), EndpointCaps{}.zstd,
                                         one_byte);
    require_throws<std::length_error>(
        [&] {
            (void)byte_bounded.prepare(PrepareRequestKey{10, 1},
                                       bytes("retained bytes exceed one"));
        },
        "preparation authority exceeded its retained-byte bound");
    require(byte_bounded.live_entry_count() == 0 &&
                byte_bounded.retained_encoded_bytes() == 0,
            "failed retained-byte admission published a partial entry");

    EndpointCaps tight;
    tight.zstd.max_raw_bytes = 4;
    CompletionLog completions;
    TestClient capped(Id128::from_u64(151), tight, HistoryNonce{1}, &completions);
    const PrepareRequestKey retried{8, 1};
    require_throws<std::length_error>(
        [&] { (void)capped.prepare(retried, bytes("too long")); },
        "failed preparation ignored its decoded-input cap");
    const PreparedTuHandle admitted = capped.prepare(retried, bytes("okay"));
    P50ServerEndpoint server(Id128::from_u64(152), tight, &completions);
    require(run_pair(capped, server, admitted).client.status == ClientRunStatus::Committed,
            "admitted preparation did not complete after a bounded failure");
    require(std::any_of(completions.completions().begin(), completions.completions().end(),
                        [](const AsyncCompletion& completion) {
                            return completion.stamp.actor == ActorSide::C &&
                                   completion.stamp.transaction_bound &&
                                   completion.stamp.tu_seq.value == 0;
                        }),
            "failed preparation consumed the next TU_SEQ");
    require(capped.authority->release(admitted) == 0,
            "completed preparation did not release explicitly to zero");

    require_throws<std::invalid_argument>(
        [&] { P50PreparationAuthority rejected(CStoreGuid{}); },
        "zero C_STORE_GUID was accepted by the preparation authority");
    require_throws<std::invalid_argument>(
        [&] { (void)client.prepare({0, 1}, input); },
        "zero producer session was accepted by the preparation authority");
    require_throws<std::invalid_argument>(
        [&] { (void)client.prepare({1, 0}, input); },
        "zero request token was accepted by the preparation authority");
    require_throws<std::invalid_argument>(
        [&] { P50ServerEndpoint rejected(FStoreGuid{}); },
        "zero F_STORE_GUID was accepted by the endpoint");
    P50ServerEndpointConfig zero_error;
    zero_error.protocol_error_code = 0;
    require_throws<std::invalid_argument>(
        [&] {
            P50ServerEndpoint rejected(Id128::from_u64(153), {}, nullptr,
                                       nullptr, zero_error);
        },
        "zero protocol ERROR code was accepted by the endpoint");
    P50ServerEndpoint reset_target(Id128::from_u64(154));
    require_throws<std::invalid_argument>(
        [&] { reset_target.reset_store(FStoreGuid{}); },
        "zero F_STORE_GUID was accepted as an incarnation replacement");

    for (const int invalid_window_log : {9, 32}) {
        EndpointCaps invalid;
        invalid.zstd.max_window_log = invalid_window_log;
        require_throws<std::invalid_argument>(
            [&] {
                P50PreparationAuthority rejected(Id128::from_u64(155),
                                                 invalid.zstd);
            },
            "preparation authority accepted an out-of-range Zstd window log");

        auto valid_authority = std::make_shared<P50PreparationAuthority>(
            Id128::from_u64(156));
        require_throws<std::invalid_argument>(
            [&] { P50ClientEndpoint rejected(valid_authority, invalid); },
            "client endpoint accepted an out-of-range Zstd window log");
        require_throws<std::invalid_argument>(
            [&] { P50ServerEndpoint rejected(Id128::from_u64(157), invalid); },
            "server endpoint accepted an out-of-range Zstd window log");
    }
}

void test_preparation_authority_window_refill_and_receipts() {
    const std::array<ProfileId, 3> profiles{
        ProfileId::P29V1, ProfileId::ZSTD_TU, ProfileId::ZSTD_ROUTE};
    uint64_t store_id = 0x5252500000000000ULL;
    uint64_t request_id = 1;
    for (const ProfileId profile : profiles) {
        PreparationAuthorityLimits limits;
        limits.max_speculative_tus = 2;
        limits.max_speculative_raw_bytes = 1024;
        limits.max_retained_encoded_bytes = 1U << 20;
        auto authority = std::make_shared<P50PreparationAuthority>(
            Id128::from_u64(store_id++), EndpointCaps{}.zstd, limits, 1,
            profile);
        const PreparationRouteKey route{
            Id128::from_u64(store_id++), 1, profile};
        const auto stage = [&](PreparedTuHandle handle) {
            if (profile == ProfileId::P29V1) {
                authority->pin_p29v1_system_source_reuse(handle, Digest128{});
                const std::vector<uint8_t> predicted =
                    authority->predicted_p29v1_need(handle);
                (void)authority->answer_p29v1_need(handle, predicted);
                authority->advance_p29v1_speculative(handle);
            } else {
                authority->advance_speculative(handle);
            }
        };
        const auto receipt_for = [&](PreparedTuHandle handle) {
            const PreparedInputPtr prepared =
                P50PreparationAuthorityTestAccess::resolve(*authority, handle);
            const TxBegin& begin = prepared->begin;
            return TxCommit{begin.history_nonce, begin.rel_seq, begin.tu_seq,
                            begin.transaction_digest, begin.raw_digest,
                            compute_post_state_digest(
                                begin.pre_state_digest, begin.history_nonce,
                                begin.rel_seq, begin.tu_seq,
                                begin.transaction_digest)};
        };

        const PreparedTuHandle first = authority->prepare_for_route(
            route, {71, request_id++}, bytes("route window first\n"));
        stage(first);
        const PreparedTuHandle second = authority->prepare_for_route(
            route, {71, request_id++}, bytes("route window second\n"));
        stage(second);
        const PreparedInputPtr first_prepared =
            P50PreparationAuthorityTestAccess::resolve(*authority, first);
        const PreparedInputPtr second_prepared =
            P50PreparationAuthorityTestAccess::resolve(*authority, second);
        if (profile != ProfileId::ZSTD_TU)
            require(first_prepared->begin.rel_seq.value == 0 &&
                        second_prepared->begin.rel_seq.value == 1,
                    "speculative route REL_SEQ did not advance monotonically");

        require_throws<std::logic_error>(
            [&] { authority->commit(first); },
            "legacy synthetic commit accepted a staged R2 TU");
        require_throws<std::logic_error>(
            [&] { (void)authority->release(first); },
            "authority released a staged prefix before its receipt");

        const TxCommit first_receipt = receipt_for(first);
        authority->accept_commit(first, first_receipt);
        const PreparedTuHandle third = authority->prepare_for_route(
            route, {71, request_id++}, bytes("route window third\n"));
        stage(third);
        const PreparedInputPtr third_prepared =
            P50PreparationAuthorityTestAccess::resolve(*authority, third);
        if (profile == ProfileId::ZSTD_ROUTE)
            require(third_prepared->begin.rel_seq.value == 2,
                    "partial ACK refill reused a ZSTD_ROUTE REL_SEQ");

        require_throws<std::logic_error>(
            [&] { authority->accept_commit(third, receipt_for(third)); },
            "authority accepted an out-of-order receiver receipt");
        TxCommit incorrect_second = receipt_for(second);
        incorrect_second.raw_digest = icecc::digest128(bytes("wrong source"));
        require_throws<std::logic_error>(
            [&] { authority->accept_commit(second, incorrect_second); },
            "authority accepted a receipt with a mismatched raw witness");
        authority->accept_commit(second, receipt_for(second));
        authority->accept_commit(third, receipt_for(third));
        for (const PreparedTuHandle& handle : {first, second, third})
            require(authority->release(handle) == 0,
                    "confirmed authority entry did not release cleanly");
        require(authority->live_entry_count() == 0 &&
                    authority->retained_encoded_bytes() == 0,
                "ordered receipt drain leaked retained preparation entries");
    }

    // A staged P29 prefix is not locally rewindable: cancellation must leave
    // it owned until ordered receipt or coordinated history reset/rebuild.
    PreparationAuthorityLimits p29_limits;
    p29_limits.max_speculative_tus = 2;
    p29_limits.max_speculative_raw_bytes = 1024;
    P50PreparationAuthority p29(Id128::from_u64(store_id++),
                                EndpointCaps{}.zstd, p29_limits, 1,
                                ProfileId::P29V1);
    const PreparationRouteKey p29_route{
        Id128::from_u64(store_id++), 1, ProfileId::P29V1};
    const PreparedTuHandle p29_first = p29.prepare_for_route(
        p29_route, {72, request_id++}, bytes("cancellation predecessor\n"));
    p29.pin_p29v1_system_source_reuse(p29_first, Digest128{});
    const auto need = p29.predicted_p29v1_need(p29_first);
    (void)p29.answer_p29v1_need(p29_first, need);
    p29.advance_p29v1_speculative(p29_first);
    const PreparedTuHandle p29_second = p29.prepare_for_route(
        p29_route, {72, request_id++}, bytes("cancellation successor\n"));
    p29.pin_p29v1_system_source_reuse(p29_second, Digest128{});
    const auto second_need = p29.predicted_p29v1_need(p29_second);
    (void)p29.answer_p29v1_need(p29_second, second_need);
    p29.advance_p29v1_speculative(p29_second);
    require_throws<std::logic_error>(
        [&] { (void)p29.release(p29_first); },
        "P29 cancellation rewound a staged successor-dependent prefix");
    require(p29.contains(p29_first) && p29.contains(p29_second),
            "failed P29 cancellation dropped a retained route witness");
}

void test_zstd_route_recovery_rebuild_cursor() {
    PreparationAuthorityLimits limits;
    limits.max_speculative_tus = 3;
    limits.max_speculative_raw_bytes = 1U << 16;
    limits.max_retained_encoded_bytes = 1U << 20;
    const CStoreGuid c_guid = Id128::from_u64(0x525245434f564552ULL);
    const PreparationRouteKey route{
        Id128::from_u64(0x525245434f555445ULL), 17, ProfileId::ZSTD_ROUTE};
    auto recovering = std::make_shared<P50PreparationAuthority>(
        c_guid, EndpointCaps{}.zstd, limits, 3, ProfileId::ZSTD_ROUTE);
    auto reference = std::make_shared<P50PreparationAuthority>(
        c_guid, EndpointCaps{}.zstd, limits, 3, ProfileId::ZSTD_ROUTE);

    const std::array<std::string, 3> texts{
        "# 1 \"/tmp/recovery-a.cc\"\nint common_route_value = 41;\n",
        "# 1 \"/tmp/recovery-b.cc\"\nint common_route_value = 42;\n",
        "# 1 \"/tmp/recovery-c.cc\"\nint common_route_value = 43;\n"};
    std::array<PreparedTuHandle, 3> recovering_handles;
    std::array<PreparedInputPtr, 3> reference_inputs;
    for (size_t index = 0; index < texts.size(); ++index) {
        const std::vector<uint8_t> raw(texts[index].begin(), texts[index].end());
        recovering_handles[index] = recovering->prepare_for_route(
            route, PrepareRequestKey{100, 1000 + index}, raw);
        const PreparedTuHandle reference_handle = reference->prepare_for_route(
            route, PrepareRequestKey{100, 1000 + index}, raw);
        reference_inputs[index] =
            P50PreparationAuthorityTestAccess::resolve(*reference,
                                                        reference_handle);
        recovering->advance_speculative(recovering_handles[index]);
        reference->advance_speculative(reference_handle);
    }

    recovering->reset_r2_route_for_recovery(
        route, FStoreGuid{Id128::from_u64(0x52524653544f5245ULL)},
        HistoryNonce{73});

    // An out-of-order rebuild must leave the cursor/history unchanged; the
    // complete suffix can then be rebuilt in relationship order.
    require_throws<std::logic_error>(
        [&] {
            recovering->rebuild_r2_entry_for_recovery(
                recovering_handles[1], Digest128{});
        },
        "ZSTD_ROUTE recovery rebuilt a suffix TU before its predecessor");

    for (size_t index = 0; index < texts.size(); ++index) {
        const PreparedTuHandle handle = recovering_handles[index];
        recovering->rebuild_r2_entry_for_recovery(handle, Digest128{});
        const PreparedInputPtr rebuilt =
            P50PreparationAuthorityTestAccess::resolve(*recovering, handle);
        require(rebuilt->begin.rel_seq.value == index,
                "ZSTD_ROUTE recovery reused a speculative REL_SEQ");
        require(rebuilt->body == reference_inputs[index]->body &&
                    rebuilt->begin.pre_state_digest ==
                        reference_inputs[index]->begin.pre_state_digest,
                "ZSTD_ROUTE recovery rebuilt suffix with the wrong history prefix");
        require(rebuilt->begin.raw_digest == reference_inputs[index]->begin.raw_digest &&
                    rebuilt->begin.raw_bytes == reference_inputs[index]->begin.raw_bytes,
                "ZSTD_ROUTE recovery changed an immutable raw TU witness");
        recovering->advance_speculative(handle);
    }
    require(recovering->live_entry_count() == texts.size(),
            "ZSTD_ROUTE recovery rebuild lost a retained suffix witness");
}

struct R2EndpointTestJob {
    JobBind binding;
    P51SourceJobLease lease;
    PreparedInputPtr prepared;
    std::vector<R2FillMessage> fills;
};

sidecar::AbsoluteMonotonicDeadline r2_test_deadline(
    std::chrono::seconds lifetime = std::chrono::seconds(10)) {
    const auto identity = sidecar::process_monotonic_clock_identity();
    return sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
        std::chrono::steady_clock::now() + lifetime,
        identity.clock_domain_id, identity.time_namespace_id);
}

P50SourceArmFields r2_test_arm(const CStoreGuid& c_guid, uint64_t request,
                               uint32_t wire_job,
                               ProfileId profile = ProfileId::ZSTD_TU) {
    P50SourceArmFields source;
    source.wire_job_id = wire_job;
    source.assignment_epoch = 3;
    source.assignment_nonce = 100 + request;
    source.selected_f_host = "127.0.0.1";
    source.selected_f_ordinary_port = 42001;
    source.selected_f_cache_port = 42002;
    source.cache_protocol = 2;
    source.cache_profile = profile == ProfileId::P29V1
                               ? CACHE_PROFILE_P29V1
                           : profile == ProfileId::ZSTD_ROUTE
                               ? CACHE_PROFILE_ZSTD_ROUTE
                               : CACHE_PROFILE_ZSTD_TU;
    source.logical_job = 700 + wire_job;
    source.compiler_attempt = 800 + wire_job;
    source.c_store_generation = 11;
    source.c_store_derivation_version = kStoreIdentityDerivationVersion;
    source.c_store_guid = c_guid.bytes;
    source.source_request_id = request;
    source.source_mode = profile == ProfileId::P29V1
                             ? P50_SOURCE_MODE_P29V1
                         : profile == ProfileId::ZSTD_ROUTE
                             ? P50_SOURCE_MODE_ZSTD_ROUTE
                             : P50_SOURCE_MODE_ZSTD_TU;
    source.c_control_generation = 12;
    source.c_control_attempt = 13;
    require(source.valid_for_cache_revision(2),
            "R2 endpoint fixture produced an invalid source ARM");
    return source;
}

P51SourceArmedFields r2_test_armed(P51SourceArmFields arm,
                                    const FStoreGuid& f_guid,
                                    uint64_t reservation_seed,
                                    uint32_t selected_window = 1) {
    P51SourceArmedFields armed;
    armed.arm = std::move(arm);
    armed.f_control_generation = 21;
    armed.f_control_attempt = 22;
    armed.f_store_generation = 23;
    armed.f_store_guid = f_guid.bytes;
    armed.f_store_derivation_version = kStoreIdentityDerivationVersion;
    armed.arm_observation_id = 24;
    armed.source_budget_msec = 9000;
    armed.attempt_capability_1 = p5co_attempt_capability(31);
    armed.attempt_capability_2 = p5co_attempt_capability(51);
    armed.reservation_id = Id128::from_u64(reservation_seed).bytes;
    armed.logical_relationship_id = Id128::from_u64(0x5102).bytes;
    armed.relationship_epoch = 25;
    armed.selected_revision = 2;
    armed.selected_window = selected_window;
    require(armed.valid(), "R2 endpoint fixture produced invalid ARMED data");
    return armed;
}

struct R2ObservedSocketBytes {
    uint64_t c_to_f = 0;
    uint64_t f_to_c = 0;
};

R2ObservedSocketBytes r2_observed_server_socket_bytes(
    const CompletionLog& observations) {
    R2ObservedSocketBytes result;
    for (const AsyncCompletion& completion : observations.completions()) {
        if (!completion.stamp.r2_traffic ||
            completion.stamp.actor != ActorSide::F)
            continue;
        uint64_t* total = nullptr;
        if (completion.stamp.operation == AsyncOperationKind::ReadHeader ||
            completion.stamp.operation == AsyncOperationKind::ReadPayload)
            total = &result.c_to_f;
        else if (completion.stamp.operation == AsyncOperationKind::WriteFragment)
            total = &result.f_to_c;
        if (total != nullptr) {
            require(completion.transferred_bytes <=
                        std::numeric_limits<uint64_t>::max() - *total,
                    "independent F socket byte counter overflowed");
            *total += completion.transferred_bytes;
        }
    }
    return result;
}

asio::awaitable<void> r2_client_write_window_before_receipts(
    tcp::endpoint endpoint, P50ClientEndpoint& client, LinkHello hello,
    std::atomic<unsigned>& server_commits,
    std::function<std::pair<JobBind, PreparedTuHandle>(size_t)> make_job,
    CompletionLog* accounting = nullptr,
    std::atomic<unsigned>* server_acks = nullptr,
    std::vector<R2WireAccountingSnapshot>* job_accounting = nullptr,
    std::vector<R2WireControlSnapshot>* interval_accounting = nullptr,
    CompletionLog* server_observations = nullptr,
    std::vector<R2ObservedSocketBytes>* peer_boundaries = nullptr) {
    tcp::socket socket(co_await asio::this_coro::executor);
    co_await socket.async_connect(endpoint, asio::use_awaitable);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(20);
    (void)co_await client.open_r2_link(socket, hello, deadline);

    std::vector<R2SentBundle> sent;
    constexpr size_t initial_window = 30;
    require(hello.window == 30 && make_job,
            "R2 W30 peer requires exactly 30 credits plus one refill bundle");
    sent.reserve(initial_window);
    for (size_t index = 0; index != initial_window; ++index) {
        const auto [binding, prepared] = make_job(index);
        sent.push_back(co_await client.write_r2_bundle(
            socket, binding, prepared, deadline));
    }

    // Wait for F to materialize/commit the whole offered window.  No receipt
    // read or COMMIT_ACK has occurred yet, so this proves the admission window
    // was exercised across the real TCP receiver, not just accepted by C.
    asio::steady_timer timer(co_await asio::this_coro::executor);
    const auto commit_wait_deadline = std::chrono::steady_clock::now() +
                                      std::chrono::seconds(5);
    while (server_commits.load(std::memory_order_acquire) < initial_window &&
           std::chrono::steady_clock::now() < commit_wait_deadline) {
        timer.expires_after(std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
    }
    const unsigned commits_before_reads =
        server_commits.load(std::memory_order_acquire);
    if (commits_before_reads != initial_window)
        std::cerr << "R2 W30 F commits before receipt reads: "
                  << commits_before_reads << '/' << initial_window << '\n';
    if (commits_before_reads != initial_window)
        throw std::runtime_error(
            "F committed only " + std::to_string(commits_before_reads) +
            "/30 R2 jobs before C receipt reads");

    for (const R2SentBundle& bundle : sent) {
        const ClientRunResult receipt = co_await client.read_r2_receipt(
            socket, bundle, deadline);
        require(receipt.status == ClientRunStatus::Committed &&
                    receipt.committed_input.has_value() &&
                    receipt.committed_input->tu_seq == bundle.binding.tu_seq,
                "C did not accept the exact receipt prefix in ordinal order");
        if (accounting != nullptr && job_accounting != nullptr) {
            const R2WireAccountingKey key{
                CStoreGuid{hello.c_store_guid}, FStoreGuid{hello.f_store_guid},
                hello.relationship_id, bundle.binding.tu_seq,
                bundle.binding.raw_digest};
            job_accounting->push_back(accounting->finish_r2_job(key));
        }
    }
    if (server_observations != nullptr && peer_boundaries != nullptr)
        peer_boundaries->push_back(
            r2_observed_server_socket_bytes(*server_observations));
    const uint64_t full_prefix = initial_window;
    co_await client.write_r2_ack(socket, full_prefix, deadline);
    if (accounting != nullptr && server_acks != nullptr &&
        interval_accounting != nullptr) {
        asio::steady_timer ack_wait(co_await asio::this_coro::executor);
        const auto ack_deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(5);
        while (server_acks->load(std::memory_order_acquire) < 1 &&
               std::chrono::steady_clock::now() < ack_deadline) {
            ack_wait.expires_after(std::chrono::milliseconds(1));
            co_await ack_wait.async_wait(asio::use_awaitable);
        }
        require(server_acks->load(std::memory_order_acquire) >= 1,
                "F did not observe the first cumulative ACK checkpoint");
        accounting->request_r2_interval_checkpoint(
            R2WireLinkIdentity{CStoreGuid{hello.c_store_guid},
                               FStoreGuid{hello.f_store_guid},
                               hello.relationship_id,
                               hello.relationship_epoch,
                               hello.physical_link_generation},
            R2WireIntervalEnd::DrainedAckCheckpoint);
        auto intervals = accounting->drain_r2_interval_snapshots();
        interval_accounting->insert(interval_accounting->end(),
                                    std::make_move_iterator(intervals.begin()),
                                    std::make_move_iterator(intervals.end()));
    }

    // The 31st bundle is queued only after cumulative Q=30 was written.  It
    // must fit the same live link without a reconnect or fresh HELLO.
    const auto [refill_binding, refill_prepared] = make_job(initial_window);
    require(refill_binding.relationship_ordinal == 31,
            "R2 W30 refill was not ordinal 31");
    const R2SentBundle refill = co_await client.write_r2_bundle(
        socket, refill_binding, refill_prepared, deadline);
    const ClientRunResult refill_receipt = co_await client.read_r2_receipt(
        socket, refill, deadline);
    require(refill_receipt.status == ClientRunStatus::Committed &&
                refill_receipt.committed_input.has_value(),
            "R2 window did not refill after cumulative ACK");
    if (accounting != nullptr && job_accounting != nullptr) {
        const R2WireAccountingKey key{
            CStoreGuid{hello.c_store_guid}, FStoreGuid{hello.f_store_guid},
            hello.relationship_id, refill.binding.tu_seq,
            refill.binding.raw_digest};
        job_accounting->push_back(accounting->finish_r2_job(key));
    }
    co_await client.write_r2_ack(socket, full_prefix + 1, deadline);
    if (accounting != nullptr && server_acks != nullptr &&
        interval_accounting != nullptr) {
        asio::steady_timer ack_wait(co_await asio::this_coro::executor);
        const auto ack_deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(5);
        while (server_acks->load(std::memory_order_acquire) < 2 &&
               std::chrono::steady_clock::now() < ack_deadline) {
            ack_wait.expires_after(std::chrono::milliseconds(1));
            co_await ack_wait.async_wait(asio::use_awaitable);
        }
        require(server_acks->load(std::memory_order_acquire) >= 2,
                "F did not observe the refill ACK checkpoint");
        accounting->request_r2_interval_checkpoint(
            R2WireLinkIdentity{CStoreGuid{hello.c_store_guid},
                               FStoreGuid{hello.f_store_guid},
                               hello.relationship_id,
                               hello.relationship_epoch,
                               hello.physical_link_generation},
            R2WireIntervalEnd::DrainedAckCheckpoint);
        auto intervals = accounting->drain_r2_interval_snapshots();
        interval_accounting->insert(interval_accounting->end(),
                                    std::make_move_iterator(intervals.begin()),
                                    std::make_move_iterator(intervals.end()));
    }
    co_await raw_write(socket, Message{CloseMessage{}});
    boost::system::error_code ignored;
    socket.shutdown(tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
}

asio::awaitable<std::vector<R2TxCommit>> r2_two_job_peer(
    tcp::endpoint endpoint, LinkHello hello,
    std::vector<R2EndpointTestJob> jobs) {
    tcp::socket socket(co_await asio::this_coro::executor);
    co_await socket.async_connect(endpoint, asio::use_awaitable);
    co_await raw_write(socket, Message{hello});
    const Frame state_frame = co_await raw_read(socket, hello.max_frame_payload);
    require(state_frame.type == MessageType::LINK_STATE,
            "R2 endpoint did not answer LINK_HELLO with LINK_STATE");
    LinkState state = std::get<LinkState>(decode_payload(
        state_frame.type, state_frame.payload));
    require(state.reservation_id == hello.reservation_id &&
                state.relationship_id == hello.relationship_id &&
                state.physical_link_generation ==
                    hello.physical_link_generation &&
                state.window == hello.window,
            "R2 LINK_STATE did not echo the admitted link identity");

    std::vector<R2TxCommit> commits;
    for (R2EndpointTestJob& job : jobs) {
        TuBegin begin;
        begin.relationship_ordinal = job.binding.relationship_ordinal;
        begin.inner = job.prepared->begin;
        begin.inner.history_nonce = state.history_nonce;
        begin.inner.rel_seq = state.next_rel_seq;
        begin.inner.pre_state_digest = state.state_digest;
        begin.inner.transaction_digest = compute_transaction_digest(
            begin.inner, job.prepared->body);

        const std::array<R2BodyMessage, 1> bodies{
            R2BodyMessage{job.prepared->body}};
        const Digest128 binding_digest =
            compute_r2_binding_digest(job.binding);
        const Digest128 transaction_digest = compute_r2_transaction_digest(
            job.binding, begin, bodies, job.fills);
        const TuEnd end{job.binding.relationship_ordinal, binding_digest,
                        transaction_digest};

        co_await raw_write(socket, Message{job.binding});
        co_await raw_write(socket, Message{begin});
        co_await raw_write(socket, Message{bodies.front()});
        for (const R2FillMessage& fill : job.fills)
            co_await raw_write(socket, Message{fill});
        co_await raw_write(socket, Message{end});
        const Frame commit_frame =
            co_await raw_read(socket, hello.max_frame_payload);
        require(commit_frame.type == MessageType::R2_TX_COMMIT,
                "R2 endpoint closed or rejected the link after a TU");
        const R2TxCommit commit = std::get<R2TxCommit>(decode_payload(
            commit_frame.type, commit_frame.payload));
        require(commit.relationship_ordinal == job.binding.relationship_ordinal &&
                    commit.binding_digest == binding_digest &&
                    commit.transaction_digest == transaction_digest &&
                    commit.inner.tu_seq == begin.inner.tu_seq &&
                    commit.inner.raw_digest == begin.inner.raw_digest,
                "R2 endpoint committed a different source transaction");
        commits.push_back(commit);
        state.history_nonce = commit.inner.history_nonce;
        state.next_rel_seq = RelSeq{commit.inner.rel_seq.value + 1};
        state.state_digest = commit.inner.post_state_digest;

        co_await raw_write(
            socket,
            Message{CommitAck{hello.relationship_id,
                              hello.relationship_epoch,
                              hello.physical_link_generation,
                              job.binding.relationship_ordinal}});
    }
    co_await raw_write(socket, Message{CloseMessage{}});
    boost::system::error_code ignored;
    socket.shutdown(tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
    co_return commits;
}

asio::awaitable<ServerRunResult> r2_accept_one(
    tcp::acceptor& acceptor, P50ServerEndpoint& endpoint,
    EndpointIoControl control = {}) {
    tcp::socket socket(co_await asio::this_coro::executor);
    co_await acceptor.async_accept(socket, asio::use_awaitable);
    co_return co_await endpoint.run_adopted_r2(std::move(socket),
                                                std::move(control));
}

asio::awaitable<std::array<ServerRunResult, 2>> r2_accept_two(
    tcp::acceptor& acceptor, P50ServerEndpoint& endpoint,
    EndpointIoControl first_control) {
    const auto executor = co_await asio::this_coro::executor;
    struct CompletionState {
        std::array<std::optional<ServerRunResult>, 2> results;
        std::array<std::exception_ptr, 2> errors;
        size_t completed = 0;
    };
    const auto completion = std::make_shared<CompletionState>();
    for (size_t index = 0; index != completion->results.size(); ++index) {
        tcp::socket socket(executor);
        co_await acceptor.async_accept(socket, asio::use_awaitable);
        EndpointIoControl control = index == 0
                                         ? first_control
                                         : EndpointIoControl{};
        asio::co_spawn(
            executor,
            endpoint.run_adopted_r2(std::move(socket), std::move(control)),
            [completion, index](std::exception_ptr error,
                                ServerRunResult result) {
                completion->errors[index] = error;
                if (!error)
                    completion->results[index] = std::move(result);
                ++completion->completed;
            });
    }
    asio::steady_timer timer(executor);
    const auto finish_by = std::chrono::steady_clock::now() +
                           std::chrono::seconds(20);
    while (completion->completed != completion->results.size() &&
           std::chrono::steady_clock::now() < finish_by) {
        timer.expires_after(std::chrono::milliseconds(1));
        boost::system::error_code ignored;
        co_await timer.async_wait(
            asio::redirect_error(asio::use_awaitable, ignored));
    }
    require(completion->completed == completion->results.size(),
            "two-link endpoint fixture exceeded its completion watchdog");
    for (const std::exception_ptr& error : completion->errors)
        if (error)
            std::rethrow_exception(error);
    require(completion->results[0].has_value() &&
                completion->results[1].has_value(),
            "two-link endpoint fixture lost one server result");
    co_return std::array<ServerRunResult, 2>{
        std::move(*completion->results[0]),
        std::move(*completion->results[1])};
}

asio::awaitable<bool> r2_silent_peer(tcp::endpoint endpoint) {
    tcp::socket socket(co_await asio::this_coro::executor);
    co_await socket.async_connect(endpoint, asio::use_awaitable);
    std::array<uint8_t, 1> byte{};
    boost::system::error_code error;
    const size_t count = co_await asio::async_read(
        socket, asio::buffer(byte), asio::redirect_error(asio::use_awaitable,
                                                         error));
    boost::system::error_code ignored;
    socket.close(ignored);
    co_return count == 0 && error == asio::error::eof;
}

asio::awaitable<bool> r2_expect_typed_reject(
    tcp::endpoint endpoint, P50ClientEndpoint& client, LinkHello hello,
    LinkRejectReason expected_reason = LinkRejectReason::StoreReplaced) {
    tcp::socket socket(co_await asio::this_coro::executor);
    co_await socket.async_connect(endpoint, asio::use_awaitable);
    try {
        (void)co_await client.open_r2_link(
            socket, hello, std::chrono::steady_clock::now() +
                               std::chrono::seconds(5));
    } catch (const R2LinkRejected& rejected) {
        co_return rejected.rejection.reason == expected_reason &&
                  rejected.rejection.offered_hello_digest ==
                      compute_r2_link_offer_digest(hello) &&
                  rejected.offered == hello;
    } catch (...) {
        co_return false;
    }
    co_return false;
}

asio::awaitable<bool> r2_expect_untyped_hello_failure(
    tcp::endpoint endpoint, P50ClientEndpoint& client, LinkHello hello) {
    tcp::socket socket(co_await asio::this_coro::executor);
    co_await socket.async_connect(endpoint, asio::use_awaitable);
    try {
        (void)co_await client.open_r2_link(
            socket, hello, std::chrono::steady_clock::now() +
                               std::chrono::seconds(5));
    } catch (const R2LinkRejected&) {
        co_return false;
    } catch (...) {
        co_return true;
    }
    co_return false;
}

void test_r2_store_replaced_rejects_same_guid_old_generation() {
    const P5coStoreGuids stores = p5co_store_guids(0x72);
    EndpointCaps caps;
    caps.profile = ProfileId::ZSTD_TU;
    caps.supported_profiles = profile_bit(ProfileId::ZSTD_TU);
    P50ServerEndpointConfig config;
    config.f_store_generation = 42;
    std::atomic<unsigned> lookup_calls{0};
    config.lookup_p51_link_reservation = [&](const LinkHello&) {
        ++lookup_calls;
        return std::optional<P51SourceLinkLease>{};
    };
    P50ServerEndpoint server(stores.f, caps, nullptr, nullptr,
                             std::move(config));
    TestClient client(stores.c, caps);

    LinkHello hello;
    hello.profile = ProfileId::ZSTD_TU;
    hello.window = 1;
    hello.max_frame_payload = kInitialMaxFramePayload;
    hello.max_raw_bytes = 1U << 20;
    hello.max_encoded_bytes = 1U << 20;
    hello.max_output_bytes = 1U << 20;
    hello.reservation_id = Id128::from_u64(0x7201);
    hello.relationship_id = Id128::from_u64(0x7202);
    hello.relationship_epoch = 1;
    hello.physical_link_generation = 1;
    hello.c_store_guid = stores.c;
    hello.c_store_generation = 7;
    hello.f_store_guid = stores.f;
    // Same F GUID, stale allocator generation: only the explicit authoritative
    // generation in P50ServerEndpointConfig can classify this precisely.
    hello.f_store_generation = 41;
    hello.c_control_generation = 8;
    hello.c_control_attempt = 9;
    hello.system_source_fingerprint = icecc::digest128("same-guid generation retry");
    hello.history_nonce = HistoryNonce{10};

    asio::io_context context;
    tcp::acceptor acceptor(context,
                           {asio::ip::address_v4::loopback(), 0});
    auto accept_future = asio::co_spawn(
        context, r2_accept_one(acceptor, server), asio::use_future);
    auto client_future = asio::co_spawn(
        context, r2_expect_typed_reject(acceptor.local_endpoint(),
                                        client.endpoint, hello),
        asio::use_future);
    context.run();
    const ServerRunResult server_result = accept_future.get();
    require(client_future.get() && lookup_calls.load() == 0 &&
                server_result.status == ServerRunStatus::Disconnected,
            "same-GUID stale-generation LINK_HELLO was not exactly rejected before lease lookup");
    std::puts("P51_R2_ENDPOINT same-GUID stale-generation typed reject: ok");
}

void test_r2_definite_missing_reservation_is_typed_only_for_absence() {
    const P5coStoreGuids stores = p5co_store_guids(0x73);
    EndpointCaps caps;
    caps.profile = ProfileId::ZSTD_TU;
    caps.supported_profiles = profile_bit(ProfileId::ZSTD_TU);
    TestClient client(stores.c, caps);

    auto make_hello = [&] {
        LinkHello hello;
        hello.profile = ProfileId::ZSTD_TU;
        hello.window = 1;
        hello.max_frame_payload = kInitialMaxFramePayload;
        hello.max_raw_bytes = 1U << 20;
        hello.max_encoded_bytes = 1U << 20;
        hello.max_output_bytes = 1U << 20;
        hello.reservation_id = Id128::from_u64(0x7301);
        hello.relationship_id = Id128::from_u64(0x7302);
        hello.relationship_epoch = 1;
        hello.physical_link_generation = 1;
        hello.c_store_guid = stores.c;
        hello.c_store_generation = 7;
        hello.f_store_guid = stores.f;
        hello.f_store_generation = 42;
        hello.c_control_generation = 8;
        hello.c_control_attempt = 9;
        hello.system_source_fingerprint = icecc::digest128("missing lease offer");
        hello.history_nonce = HistoryNonce{11};
        return hello;
    };

    P50ServerEndpointConfig missing_config;
    missing_config.f_store_generation = 42;
    missing_config.lookup_p51_link_reservation = [](const LinkHello&) {
        return P51SourceLinkLookupResult{
            P51SourceLinkLookupStatus::ReservationMissing, std::nullopt};
    };
    P50ServerEndpoint missing_server(stores.f, caps, nullptr, nullptr,
                                     std::move(missing_config));
    LinkHello missing_hello = make_hello();
    asio::io_context missing_context;
    tcp::acceptor missing_acceptor(
        missing_context, {asio::ip::address_v4::loopback(), 0});
    auto missing_server_future = asio::co_spawn(
        missing_context, r2_accept_one(missing_acceptor, missing_server),
        asio::use_future);
    auto missing_client_future = asio::co_spawn(
        missing_context,
        r2_expect_typed_reject(missing_acceptor.local_endpoint(),
                                client.endpoint, missing_hello,
                                LinkRejectReason::ReservationMissing),
        asio::use_future);
    missing_context.run();
    const auto missing_result = missing_server_future.get();
    require(missing_client_future.get() &&
                missing_result.status == ServerRunStatus::Disconnected,
            "definitely absent initial reservation did not produce exact digest-bound R2_LINK_REJECT");

    P50ServerEndpointConfig invalid_config;
    invalid_config.f_store_generation = 42;
    // The compatibility conversion from an empty optional is deliberately
    // Invalid, not ReservationMissing: it carries no proof that the exact
    // reservation is absent rather than stale or otherwise mismatched.
    invalid_config.lookup_p51_link_reservation = [](const LinkHello&) {
        return std::optional<P51SourceLinkLease>{};
    };
    P50ServerEndpoint invalid_server(stores.f, caps, nullptr, nullptr,
                                     std::move(invalid_config));
    LinkHello invalid_hello = make_hello();
    asio::io_context invalid_context;
    tcp::acceptor invalid_acceptor(
        invalid_context, {asio::ip::address_v4::loopback(), 0});
    auto invalid_server_future = asio::co_spawn(
        invalid_context, r2_accept_one(invalid_acceptor, invalid_server),
        asio::use_future);
    auto invalid_client_future = asio::co_spawn(
        invalid_context,
        r2_expect_untyped_hello_failure(invalid_acceptor.local_endpoint(),
                                        client.endpoint, invalid_hello),
        asio::use_future);
    invalid_context.run();
    const auto invalid_result = invalid_server_future.get();
    require(invalid_client_future.get() &&
                invalid_result.status == ServerRunStatus::TerminalError,
            "ambiguous empty lookup was incorrectly promoted to typed ReservationMissing");
    std::puts("P51_R2_ENDPOINT definite ReservationMissing vs invalid lookup: ok");
}

void test_r2_endpoint_commits_two_jobs_on_one_link() {
    const P5coStoreGuids stores = p5co_store_guids(0x71);
    EndpointCaps caps;
    caps.profile = ProfileId::ZSTD_TU;
    caps.supported_profiles = profile_bit(ProfileId::ZSTD_TU);
    PreparationAuthorityLimits authority_limits;
    authority_limits.max_speculative_tus = 2;
    authority_limits.max_speculative_raw_bytes = 1U << 20;
    P50PreparationAuthority authority(stores.c, caps.zstd,
                                      authority_limits, 1,
                                      ProfileId::ZSTD_TU);
    const PreparationRouteKey route{stores.f, 23, ProfileId::ZSTD_TU};
    const std::array<std::vector<uint8_t>, 2> inputs{
        std::vector<uint8_t>{'R','2',' ','f','i','r','s','t','\n'},
        std::vector<uint8_t>{'R','2',' ','s','e','c','o','n','d','\n'}};
    std::array<PreparedInputPtr, 2> prepared;
    std::array<PreparedTuHandle, 2> handles;
    for (size_t index = 0; index != inputs.size(); ++index) {
        handles[index] = authority.prepare_for_route(
            route, PrepareRequestKey{100 + index, 200 + index}, inputs[index]);
        authority.advance_speculative(handles[index]);
        prepared[index] = P50PreparationAuthorityTestAccess::resolve(
            authority, handles[index]);
    }

    const P51SourceArmFields first_arm{
        r2_test_arm(stores.c, 301, 401), 1};
    const P51SourceArmFields second_arm{
        r2_test_arm(stores.c, 302, 402), 1};
    const P51SourceArmedFields first_armed =
        r2_test_armed(first_arm, stores.f, 0x5101);
    const P51SourceArmedFields second_armed =
        r2_test_armed(second_arm, stores.f, 0x5103);
    const sidecar::AbsoluteMonotonicDeadline deadline = r2_test_deadline();

    LinkHello hello;
    hello.profile = ProfileId::ZSTD_TU;
    hello.window = 1;
    hello.max_frame_payload = kInitialMaxFramePayload;
    hello.max_raw_bytes = 1U << 20;
    hello.max_encoded_bytes = 1U << 20;
    hello.max_output_bytes = 1U << 20;
    hello.reservation_id = Id128{first_armed.reservation_id};
    hello.relationship_id = Id128{first_armed.logical_relationship_id};
    hello.relationship_epoch = first_armed.relationship_epoch;
    hello.physical_link_generation = 26;
    hello.c_store_guid = stores.c;
    hello.c_store_generation = first_arm.source.c_store_generation;
    hello.f_store_guid = stores.f;
    hello.f_store_generation = first_armed.f_store_generation;
    hello.c_control_generation = first_arm.source.c_control_generation;
    hello.c_control_attempt = first_arm.source.c_control_attempt;
    hello.system_source_fingerprint = icecc::digest128("R2 endpoint fixture");
    hello.history_nonce = HistoryNonce{0x51f00d};
    hello.start_mode = LinkStartMode::Initial;

    std::vector<R2EndpointTestJob> jobs;
    for (size_t index = 0; index != prepared.size(); ++index) {
        const P51SourceArmFields& source =
            index == 0 ? first_arm : second_arm;
        const P51SourceArmedFields& armed =
            index == 0 ? first_armed : second_armed;
        JobBind binding;
        binding.reservation_id = Id128{armed.reservation_id};
        binding.physical_link_generation = hello.physical_link_generation;
        binding.relationship_ordinal = index + 1;
        binding.wire_job_id = source.source.wire_job_id;
        binding.assignment_epoch = source.source.assignment_epoch;
        binding.assignment_nonce = source.source.assignment_nonce;
        binding.logical_job = source.source.logical_job;
        binding.compiler_attempt = source.source.compiler_attempt;
        binding.source_request_id = source.source.source_request_id;
        binding.tu_seq = prepared[index]->begin.tu_seq;
        binding.profile = ProfileId::ZSTD_TU;
        binding.raw_bytes = inputs[index].size();
        binding.raw_digest = prepared[index]->begin.raw_digest;

        P51SourceJobLease lease;
        lease.armed = armed;
        lease.absolute_deadline = deadline;
        lease.binding = binding;
        lease.binding_digest = compute_r2_binding_digest(binding);
        lease.input_key = InputRecordKey{stores.c, binding.tu_seq};
        jobs.push_back(R2EndpointTestJob{binding, lease, prepared[index], {}});
    }

    std::atomic<unsigned> lookup_calls{0};
    std::atomic<unsigned> consume_calls{0};
    std::atomic<unsigned> commit_calls{0};
    std::atomic<unsigned> ack_calls{0};
    std::atomic<unsigned> terminal_calls{0};
    std::array<bool, 2> consumed{};
    P50ServerEndpointConfig config;
    config.lookup_p51_link_reservation =
        [&, deadline](const LinkHello& observed)
            -> std::optional<P51SourceLinkLease> {
            ++lookup_calls;
            if (observed != hello)
                return std::nullopt;
            P51SourceLinkLease lease{first_armed, deadline};
            lease.relationship_epoch = observed.relationship_epoch;
            return lease;
        };
    config.consume_p51_job_reservation =
        [&](const LinkHello& observed, const JobBind& binding)
            -> std::optional<P51SourceJobLease> {
            ++consume_calls;
            if (observed != hello)
                return std::nullopt;
            for (size_t index = 0; index != jobs.size(); ++index) {
                const R2EndpointTestJob& job = jobs[index];
                if (job.binding == binding && !consumed[index]) {
                    consumed[index] = true;
                    return job.lease;
                }
            }
            return std::nullopt;
        };
    config.record_p51_job_commit =
        [&](const LinkHello& observed, const JobBind& binding,
            const R2TxCommit& commit) {
            if (observed != hello ||
                commit.relationship_ordinal != binding.relationship_ordinal ||
                commit.inner.tu_seq != binding.tu_seq)
                return false;
            ++commit_calls;
            return true;
        };
    config.acknowledge_p51_receipt =
        [&](const LinkHello& observed, const CommitAck& ack) {
            if (observed != hello || ack.relationship_id != hello.relationship_id)
                return false;
            ++ack_calls;
            return true;
        };
    config.on_p51_link_terminal = [&](
        const LinkHello&, const std::optional<JobBind>&) {
        ++terminal_calls;
    };

    P50ServerEndpoint endpoint(stores.f, caps, nullptr, nullptr,
                               std::move(config));
    asio::io_context context;
    tcp::acceptor acceptor(context,
                           {asio::ip::address_v4::loopback(), 0});
    auto accept_future = asio::co_spawn(
        context, r2_accept_one(acceptor, endpoint), asio::use_future);
    auto peer_future = asio::co_spawn(
        context, r2_two_job_peer(acceptor.local_endpoint(), hello, jobs),
        asio::use_future);
    context.run();
    const ServerRunResult server_result = accept_future.get();
    if (server_result.terminal_error)
        std::cerr << "R2 fixture F terminal error: "
                  << server_result.terminal_error->detail << '\n';
    std::vector<R2TxCommit> commits;
    try {
        commits = peer_future.get();
    } catch (const std::exception& error) {
        std::cerr << "R2 fixture peer error: " << error.what() << '\n';
        throw;
    }
    require(server_result.status == ServerRunStatus::Completed &&
                commits.size() == 2 && lookup_calls == 1 &&
                consume_calls == 2 && commit_calls == 2 && ack_calls == 2 &&
                terminal_calls == 1,
            "R2 F endpoint did not keep one reservation through two commits and CLOSE");
    for (size_t index = 0; index != inputs.size(); ++index) {
        InputCursor cursor = endpoint.attach_input(
            InputRecordKey{stores.c, prepared[index]->begin.tu_seq});
        std::vector<uint8_t> recovered(inputs[index].size());
        require(cursor.read(recovered) == recovered.size() &&
                    recovered == inputs[index],
                "R2 F endpoint materialized different source bytes");
    }
    std::puts("P51_R2_ENDPOINT two-jobs-one-link exact-input: ok");
}

void test_r2_wire_accounting_interval_conservation() {
    CompletionLog accounting{CompletionLog::StorageMode::ClientByteTotals};
    accounting.enable_r2_accounting();
    const CStoreGuid c_guid{Id128::from_u64(0xacc001)};
    const FStoreGuid f_guid{Id128::from_u64(0xacc002)};
    const Id128 link_id = Id128::from_u64(0xacc003);
    const R2WireLinkIdentity link{c_guid, f_guid, link_id, 7, 19};
    const R2WireAccountingKey key{
        c_guid, f_guid, link_id, TuSeq{0}, icecc::digest128("acct-input")};
    require(accounting.begin_r2_job(key),
            "R2 accounting did not admit one bounded active row");

    CompletionStamp stamp;
    stamp.actor = ActorSide::C;
    stamp.r2_traffic = true;
    stamp.operation = AsyncOperationKind::WriteFragment;
    stamp.c_store_guid = c_guid;
    stamp.f_store_guid = f_guid;
    stamp.logical_link_id = link_id;
    stamp.relationship_epoch = link.relationship_epoch;
    stamp.physical_link_generation = link.physical_link_generation;
    stamp.transaction_bound = true;
    stamp.rel_seq = RelSeq{0}; // First transaction REL_SEQ is valid.
    stamp.tu_seq = key.tu_seq;
    stamp.raw_digest = key.raw_digest;
    accounting.begin_r2_bundle_attempt(stamp);
    accounting.record(AsyncCompletion{stamp, 11, 0});

    accounting.begin_r2_receipt_read(key, link);
    stamp.operation = AsyncOperationKind::ReadHeader;
    accounting.record(AsyncCompletion{stamp, 4, 0});
    stamp.operation = AsyncOperationKind::ReadPayload;
    accounting.record(AsyncCompletion{stamp, 6, 0});
    accounting.request_r2_interval_checkpoint(
        link, R2WireIntervalEnd::DrainedAckCheckpoint);
    require(accounting.drain_r2_interval_snapshots().empty(),
            "checkpoint escaped before the active receipt was classified");
    accounting.confirm_r2_receipt(key);
    auto intervals = accounting.drain_r2_interval_snapshots();
    if (intervals.size() != 1 || intervals[0].jobs.size() != 1 ||
        intervals.empty() || !intervals[0].valid ||
        intervals[0].total_c_to_f_bytes != 11 ||
        intervals[0].total_f_to_c_bytes != 10 ||
        intervals[0].shared_c_to_f_bytes != 0 ||
        intervals[0].shared_f_to_c_bytes != 0) {
        std::cerr << "synthetic first interval diagnostic n="
                  << intervals.size();
        if (!intervals.empty()) {
            const auto& value = intervals[0];
            std::cerr << " valid=" << value.valid
                      << " seq=" << value.interval_sequence
                      << " totals=" << value.total_c_to_f_bytes << '/'
                      << value.total_f_to_c_bytes
                      << " shared=" << value.shared_c_to_f_bytes << '/'
                      << value.shared_f_to_c_bytes
                      << " jobs=" << value.jobs.size();
            if (!value.jobs.empty())
                std::cerr << " jobvalid=" << value.jobs[0].valid
                          << " keyeq=" << (value.jobs[0].key == key)
                          << " jobbytes=" << value.jobs[0].c_to_f_bundle_bytes
                          << '/' << value.jobs[0].f_to_c_receipt_bytes
                          << " attempts=" << value.jobs[0].bundle_attempts;
        }
        std::cerr << '\n';
    }
    require(intervals.size() == 1 && intervals[0].valid &&
                intervals[0].interval_sequence == 1 &&
                intervals[0].link == link &&
                intervals[0].end ==
                    R2WireIntervalEnd::DrainedAckCheckpoint &&
                intervals[0].total_c_to_f_bytes == 11 &&
                intervals[0].total_f_to_c_bytes == 10 &&
                intervals[0].shared_c_to_f_bytes == 0 &&
                intervals[0].shared_f_to_c_bytes == 0 &&
                intervals[0].jobs.size() == 1 &&
                intervals[0].jobs[0].key == key &&
                intervals[0].jobs[0].c_to_f_bundle_bytes == 11 &&
                intervals[0].jobs[0].f_to_c_receipt_bytes == 10 &&
                intervals[0].jobs[0].bundle_attempts == 1,
            "validated receipt did not conserve the first interval exactly");

    accounting.begin_r2_receipt_read(key, link);
    stamp.operation = AsyncOperationKind::ReadHeader;
    accounting.record(AsyncCompletion{stamp, 3, 0});
    accounting.request_r2_interval_checkpoint(
        link, R2WireIntervalEnd::PhysicalLinkRetired);
    require(accounting.drain_r2_interval_snapshots().empty(),
            "retirement checkpoint did not defer an incomplete receipt");
    accounting.reject_r2_receipt(key);
    intervals = accounting.drain_r2_interval_snapshots();
    require(intervals.size() == 1 && intervals[0].valid &&
                intervals[0].interval_sequence == 2 &&
                intervals[0].end == R2WireIntervalEnd::PhysicalLinkRetired &&
                intervals[0].total_c_to_f_bytes == 0 &&
                intervals[0].total_f_to_c_bytes == 3 &&
                intervals[0].shared_f_to_c_bytes == 3 &&
                intervals[0].jobs.empty(),
            "partial receipt was not conserved as shared bytes at retirement");

    stamp.operation = AsyncOperationKind::WriteFragment;
    stamp.replay_attempt = true;
    accounting.begin_r2_bundle_attempt(stamp);
    accounting.record(AsyncCompletion{stamp, 7, 0});
    const R2WireAccountingSnapshot lifetime = accounting.finish_r2_job(key);
    accounting.request_r2_interval_checkpoint(
        link, R2WireIntervalEnd::DrainedAckCheckpoint);
    intervals = accounting.drain_r2_interval_snapshots();
    require(lifetime.valid && lifetime.c_to_f_bundle_bytes == 18 &&
                lifetime.f_to_c_receipt_bytes == 10 &&
                lifetime.bundle_attempts == 2 && lifetime.replay_attempts == 1 &&
                intervals.size() == 1 && intervals[0].valid &&
                intervals[0].interval_sequence == 3 &&
                intervals[0].total_c_to_f_bytes == 7 &&
                intervals[0].jobs.size() == 1 &&
                intervals[0].jobs[0].key == key &&
                intervals[0].jobs[0].c_to_f_bundle_bytes == 7 &&
                intervals[0].jobs[0].bundle_attempts == 1 &&
                intervals[0].jobs[0].replay_attempts == 1,
            "reindexed replay accounting duplicated or lost a bounded interval");

    CompletionLog poisoned;
    poisoned.enable_r2_accounting();
    require(poisoned.begin_r2_job(key),
            "poison test failed to start accounting row");
    poisoned.mark_r2_accounting_unavailable();
    require(!poisoned.finish_r2_job(key).valid,
            "accounting poison was cleared by a successful later finish");
    std::puts("P51_R2_WIRE_ACCOUNTING bounded checkpoint conservation: ok");
}

struct R2WorkerHistoryGate {
    std::mutex mutex;
    std::condition_variable changed;
    size_t entered_count = 0;
    bool release_second = false;
    bool second_exited = false;
};

void release_r2_worker_history_gate(
    const std::shared_ptr<R2WorkerHistoryGate>& gate) noexcept {
    {
        std::lock_guard lock(gate->mutex);
        gate->release_second = true;
    }
    gate->changed.notify_all();
}

struct R2PeerCloseWorkerGate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
    bool exited = false;
};

void release_r2_peer_close_worker_gate(
    const std::shared_ptr<R2PeerCloseWorkerGate>& gate) noexcept {
    {
        std::lock_guard lock(gate->mutex);
        gate->release = true;
    }
    gate->changed.notify_all();
}

enum class R2DeadlineStage {
    PeerClose,
    BeforeBind,
    ReturnedExpiredLease,
    DuringDecode,
    BeforePublication,
};

asio::awaitable<bool> r2_peer_close_after_worker_entered(
    tcp::endpoint endpoint, LinkHello hello, R2EndpointTestJob job,
    std::shared_ptr<R2PeerCloseWorkerGate> gate,
    std::atomic<bool>& peer_closed,
    R2DeadlineStage stage) {
    tcp::socket socket(co_await asio::this_coro::executor);
    co_await socket.async_connect(endpoint, asio::use_awaitable);
    co_await raw_write(socket, Message{hello});
    const Frame state_frame = co_await raw_read(socket, hello.max_frame_payload);
    require(state_frame.type == MessageType::LINK_STATE,
            "peer-close fixture did not receive R2 LINK_STATE");
    const LinkState state = std::get<LinkState>(decode_payload(
        state_frame.type, state_frame.payload));
    require(state.reservation_id == hello.reservation_id &&
                state.relationship_id == hello.relationship_id &&
                state.physical_link_generation ==
                    hello.physical_link_generation,
            "peer-close fixture received a mismatched R2 LINK_STATE");

    if (stage == R2DeadlineStage::BeforeBind) {
        const auto source_deadline =
            job.lease.absolute_deadline.as_steady_time_point();
        if (std::chrono::steady_clock::now() >= source_deadline)
            co_return false;
        std::this_thread::sleep_until(
            source_deadline + std::chrono::milliseconds(2));
    }

    TuBegin begin;
    begin.relationship_ordinal = job.binding.relationship_ordinal;
    begin.inner = job.prepared->begin;
    begin.inner.history_nonce = state.history_nonce;
    begin.inner.rel_seq = state.next_rel_seq;
    begin.inner.pre_state_digest = state.state_digest;
    begin.inner.transaction_digest = compute_transaction_digest(
        begin.inner, job.prepared->body);
    const std::array<R2BodyMessage, 1> bodies{
        R2BodyMessage{job.prepared->body}};
    const Digest128 binding_digest = compute_r2_binding_digest(job.binding);
    const Digest128 transaction_digest = compute_r2_transaction_digest(
        job.binding, begin, bodies, job.fills);
    const TuEnd end{job.binding.relationship_ordinal, binding_digest,
                    transaction_digest};
    co_await raw_write(socket, Message{job.binding});
    if (stage != R2DeadlineStage::BeforeBind &&
        stage != R2DeadlineStage::ReturnedExpiredLease) {
        co_await raw_write(socket, Message{begin});
        co_await raw_write(socket, Message{bodies.front()});
        for (const R2FillMessage& fill : job.fills)
            co_await raw_write(socket, Message{fill});
        co_await raw_write(socket, Message{end});
    }

    const auto executor = co_await asio::this_coro::executor;
    asio::steady_timer poll(executor);
    const auto until = std::chrono::steady_clock::now() +
                       std::chrono::seconds(5);
    bool entered = false;
    const bool expect_worker_entry =
        stage == R2DeadlineStage::PeerClose ||
        stage == R2DeadlineStage::DuringDecode ||
        stage == R2DeadlineStage::BeforePublication;
    while (expect_worker_entry && !entered &&
           std::chrono::steady_clock::now() < until) {
        {
            std::lock_guard lock(gate->mutex);
            entered = gate->entered;
        }
        if (entered)
            break;
        poll.expires_after(std::chrono::milliseconds(1));
        boost::system::error_code error;
        co_await poll.async_wait(
            asio::redirect_error(asio::use_awaitable, error));
        if (error)
            break;
    }
    if (stage != R2DeadlineStage::PeerClose) {
        try {
            const Frame response = co_await raw_read(
                socket, hello.max_frame_payload);
            if (response.type == MessageType::R2_TX_COMMIT)
                co_return false;
        } catch (...) {
            // Expiry closes the stream without an R2 receipt.
        }
    }
    boost::system::error_code ignored;
    socket.shutdown(tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
    peer_closed.store(true, std::memory_order_release);
    co_return entered;
}

asio::awaitable<bool> r2_peer_observes_reset_during_second_worker(
    tcp::endpoint endpoint, LinkHello hello,
    std::vector<R2EndpointTestJob> jobs, std::string& failure) {
    try {
        (void)co_await r2_two_job_peer(endpoint, std::move(hello),
                                      std::move(jobs));
        co_return false;
    } catch (const std::exception& error) {
        // The peer must see the reset fence after submitting the complete
        // second BODY/END; a normal two-commit CLOSE is the wrong outcome.
        failure = error.what();
        co_return true;
    } catch (...) {
        failure = "unknown peer exception";
        co_return true;
    }
}

void test_r2_persistent_history_charge_survives_worker_reset(ProfileId profile) {
    require(profile == ProfileId::P29V1 || profile == ProfileId::ZSTD_ROUTE,
            "worker history reset fixture requires a persistent profile");
    const P5coStoreGuids stores = p5co_store_guids(
        0x5900 + static_cast<uint64_t>(profile));
    const FStoreGuid replacement_f = p5co_store_guids(
        static_cast<uint8_t>(0x90 + static_cast<unsigned>(profile))).f;
    EndpointCaps caps;
    caps.profile = profile;
    caps.supported_profiles = profile_bit(profile);
    caps.zstd.max_raw_bytes = 1U << 20;
    caps.zstd.max_encoded_body_bytes = 1U << 20;
    caps.zstd.max_history_bytes = 1U << 20;
    PreparationAuthorityLimits authority_limits;
    authority_limits.max_speculative_tus = 3;
    authority_limits.max_speculative_raw_bytes = 1U << 20;
    auto authority = std::make_shared<P50PreparationAuthority>(
        stores.c, caps.zstd, authority_limits, 1, profile);
    const PreparationRouteKey route{stores.f, 23, profile};
    const sidecar::AbsoluteMonotonicDeadline deadline =
        r2_test_deadline(std::chrono::seconds(15));
    const std::array<std::vector<uint8_t>, 2> inputs{
        std::vector<uint8_t>(64U << 10, static_cast<uint8_t>('A')),
        std::vector<uint8_t>(64U << 10, static_cast<uint8_t>('A'))};
    std::array<PreparedTuHandle, 2> handles;
    std::array<PreparedInputPtr, 2> prepared;
    std::array<std::vector<R2FillMessage>, 2> prepared_fills;
    std::array<P51SourceArmFields, 2> arms;
    std::array<P51SourceArmedFields, 2> armed;
    for (size_t index = 0; index != inputs.size(); ++index) {
        arms[index] = P51SourceArmFields{
            r2_test_arm(stores.c, 5900 + index, 6900 + index, profile), 1};
        armed[index] = r2_test_armed(
            arms[index], stores.f, 0x5901 + index);
        handles[index] = authority->prepare_for_route(
            route, PrepareRequestKey{5900 + index, 6900 + index}, inputs[index]);
        if (profile == ProfileId::P29V1) {
            authority->pin_p29v1_system_source_reuse(handles[index], Digest128{});
            const std::vector<uint8_t> need =
                authority->predicted_p29v1_need(handles[index]);
            const std::span<const uint8_t> fill =
                authority->answer_p29v1_need(handles[index], need);
            icecc::codec::P29WireLimits wire_limits;
            wire_limits.max_tu_bytes =
                static_cast<size_t>(caps.zstd.max_raw_bytes);
            wire_limits.max_region_bytes = wire_limits.max_tu_bytes;
            const std::vector<FillMessage> encoded =
                encode_p29v1_fill_messages(
                    fill, kInitialMaxFramePayload,
                    icecc::codec::p29v1_fill_inner_bound(wire_limits));
            prepared_fills[index].reserve(encoded.size());
            for (const FillMessage& message : encoded)
                prepared_fills[index].push_back(R2FillMessage{message.bytes});
            authority->advance_p29v1_speculative(handles[index]);
        } else {
            authority->advance_speculative(handles[index]);
        }
        prepared[index] = P50PreparationAuthorityTestAccess::resolve(
            *authority, handles[index]);
    }

    LinkHello hello;
    hello.profile = profile;
    hello.window = 1;
    hello.max_frame_payload = kInitialMaxFramePayload;
    hello.max_raw_bytes = 1U << 20;
    hello.max_encoded_bytes = 1U << 20;
    hello.max_output_bytes = 1U << 20;
    hello.reservation_id = Id128{armed[0].reservation_id};
    hello.relationship_id = Id128{armed[0].logical_relationship_id};
    hello.relationship_epoch = armed[0].relationship_epoch;
    hello.physical_link_generation = 59;
    hello.c_store_guid = stores.c;
    hello.c_store_generation = arms[0].source.c_store_generation;
    hello.f_store_guid = stores.f;
    hello.f_store_generation = armed[0].f_store_generation;
    hello.c_control_generation = arms[0].source.c_control_generation;
    hello.c_control_attempt = arms[0].source.c_control_attempt;
    hello.system_source_fingerprint = profile == ProfileId::P29V1
        ? authority->p29v1_system_source_fingerprint(
              handles[0])
        : icecc::digest128("R2 held-worker ZSTD_ROUTE history fixture");
    hello.history_nonce = prepared[0]->begin.history_nonce;
    hello.start_mode = LinkStartMode::Initial;

    std::vector<R2EndpointTestJob> jobs;
    jobs.reserve(2);
    for (size_t index = 0; index != inputs.size(); ++index) {
        JobBind binding;
        binding.reservation_id = Id128{armed[index].reservation_id};
        binding.physical_link_generation = hello.physical_link_generation;
        binding.relationship_ordinal = index + 1;
        binding.wire_job_id = arms[index].source.wire_job_id;
        binding.assignment_epoch = arms[index].source.assignment_epoch;
        binding.assignment_nonce = arms[index].source.assignment_nonce;
        binding.logical_job = arms[index].source.logical_job;
        binding.compiler_attempt = arms[index].source.compiler_attempt;
        binding.source_request_id = arms[index].source.source_request_id;
        binding.tu_seq = prepared[index]->begin.tu_seq;
        binding.profile = profile;
        binding.raw_bytes = prepared[index]->begin.raw_bytes;
        binding.raw_digest = prepared[index]->begin.raw_digest;
        P51SourceJobLease lease;
        lease.armed = armed[index];
        lease.absolute_deadline = deadline;
        lease.binding = binding;
        lease.binding_digest = compute_r2_binding_digest(binding);
        lease.input_key = InputRecordKey{stores.c, binding.tu_seq};
        jobs.push_back(R2EndpointTestJob{
            binding, lease, prepared[index], prepared_fills[index]});
    }

    const std::vector<uint8_t> successor_input(
        48U << 10, static_cast<uint8_t>('B'));
    const P51SourceArmFields successor_arm{
        r2_test_arm(stores.c, 5902, 6902, profile), 1};
    P51SourceArmedFields successor_armed = r2_test_armed(
        successor_arm, replacement_f, 0x5910);
    successor_armed.logical_relationship_id =
        Id128::from_u64(0x5911).bytes;
    successor_armed.relationship_epoch = armed[0].relationship_epoch + 1;
    successor_armed.f_store_generation = armed[0].f_store_generation + 1;
    require(successor_armed.valid(),
            "fresh-F successor assignment is invalid after identity advance");
    const PreparationRouteKey successor_route{
        replacement_f, successor_armed.f_store_generation, profile};
    const PreparedTuHandle successor_handle = authority->prepare_for_route(
        successor_route, PrepareRequestKey{5902, 6902}, successor_input);
    std::vector<R2FillMessage> successor_fills;
    if (profile == ProfileId::P29V1) {
        authority->pin_p29v1_system_source_reuse(
            successor_handle, Digest128{});
        const std::vector<uint8_t> need =
            authority->predicted_p29v1_need(successor_handle);
        const std::span<const uint8_t> fill =
            authority->answer_p29v1_need(successor_handle, need);
        icecc::codec::P29WireLimits wire_limits;
        wire_limits.max_tu_bytes =
            static_cast<size_t>(caps.zstd.max_raw_bytes);
        wire_limits.max_region_bytes = wire_limits.max_tu_bytes;
        const std::vector<FillMessage> encoded =
            encode_p29v1_fill_messages(
                fill, kInitialMaxFramePayload,
                icecc::codec::p29v1_fill_inner_bound(wire_limits));
        successor_fills.reserve(encoded.size());
        for (const FillMessage& message : encoded)
            successor_fills.push_back(R2FillMessage{message.bytes});
        authority->advance_p29v1_speculative(successor_handle);
    } else {
        authority->advance_speculative(successor_handle);
    }
    const PreparedInputPtr successor_prepared =
        P50PreparationAuthorityTestAccess::resolve(
            *authority, successor_handle);
    LinkHello successor_hello = hello;
    successor_hello.reservation_id =
        Id128{successor_armed.reservation_id};
    successor_hello.relationship_id =
        Id128{successor_armed.logical_relationship_id};
    successor_hello.relationship_epoch = successor_armed.relationship_epoch;
    successor_hello.physical_link_generation =
        hello.physical_link_generation + 1;
    successor_hello.f_store_guid = replacement_f;
    successor_hello.f_store_generation = successor_armed.f_store_generation;
    successor_hello.c_control_generation =
        successor_arm.source.c_control_generation;
    successor_hello.c_control_attempt = successor_arm.source.c_control_attempt;
    successor_hello.system_source_fingerprint = profile == ProfileId::P29V1
        ? authority->p29v1_system_source_fingerprint(successor_handle)
        : icecc::digest128("R2 held-worker ZSTD_ROUTE successor fixture");
    successor_hello.history_nonce = successor_prepared->begin.history_nonce;
    successor_hello.start_mode = LinkStartMode::Initial;
    JobBind successor_binding;
    successor_binding.reservation_id = successor_hello.reservation_id;
    successor_binding.physical_link_generation =
        successor_hello.physical_link_generation;
    successor_binding.relationship_ordinal = 1;
    successor_binding.wire_job_id = successor_arm.source.wire_job_id;
    successor_binding.assignment_epoch = successor_arm.source.assignment_epoch;
    successor_binding.assignment_nonce = successor_arm.source.assignment_nonce;
    successor_binding.logical_job = successor_arm.source.logical_job;
    successor_binding.compiler_attempt = successor_arm.source.compiler_attempt;
    successor_binding.source_request_id = successor_arm.source.source_request_id;
    successor_binding.tu_seq = successor_prepared->begin.tu_seq;
    successor_binding.profile = profile;
    successor_binding.raw_bytes = successor_prepared->begin.raw_bytes;
    successor_binding.raw_digest = successor_prepared->begin.raw_digest;
    P51SourceJobLease successor_lease;
    successor_lease.armed = successor_armed;
    successor_lease.absolute_deadline = deadline;
    successor_lease.binding = successor_binding;
    successor_lease.binding_digest =
        compute_r2_binding_digest(successor_binding);
    successor_lease.input_key =
        InputRecordKey{stores.c, successor_binding.tu_seq};
    R2EndpointTestJob successor_job{
        successor_binding, successor_lease, successor_prepared,
        std::move(successor_fills)};

    auto gate = std::make_shared<R2WorkerHistoryGate>();
    struct ReleaseWorkerHistoryOnExit {
        std::shared_ptr<R2WorkerHistoryGate> gate;
        ~ReleaseWorkerHistoryOnExit() {
            release_r2_worker_history_gate(gate);
        }
    } release_worker_on_exit{gate};
    std::array<bool, 2> consumed{};
    bool successor_consumed = false;
    std::string peer_failure;
    std::atomic<unsigned> commits{0};
    std::atomic<unsigned> input_state_calls{0};
    std::atomic<unsigned> unexpected_input_state_calls{0};
    std::atomic<unsigned> acknowledged_receipts{0};
    P50ServerEndpointConfig config;
    config.lookup_p51_link_reservation =
        [&](const LinkHello& observed) -> std::optional<P51SourceLinkLease> {
            if (observed == hello) {
                P51SourceLinkLease link{armed[0], deadline};
                link.relationship_epoch = hello.relationship_epoch;
                link.history_nonce = hello.history_nonce;
                return link;
            }
            if (observed == successor_hello) {
                P51SourceLinkLease link{successor_armed, deadline};
                link.relationship_epoch = successor_hello.relationship_epoch;
                link.history_nonce = successor_hello.history_nonce;
                return link;
            }
            return std::nullopt;
        };
    config.consume_p51_job_reservation =
        [&](const LinkHello& observed, const JobBind& binding)
            -> std::optional<P51SourceJobLease> {
            if (observed == hello) {
                for (size_t index = 0; index != jobs.size(); ++index) {
                    if (!consumed[index] && jobs[index].binding == binding) {
                        consumed[index] = true;
                        return jobs[index].lease;
                    }
                }
                return std::nullopt;
            }
            if (observed == successor_hello && !successor_consumed &&
                binding == successor_job.binding) {
                successor_consumed = true;
                return successor_job.lease;
            }
            return std::nullopt;
        };
    config.input_job_state =
        [&](CStoreGuid c_guid, const TxBegin&, const TxCommit& commit,
            std::span<const uint8_t> exact) {
            ++input_state_calls;
            if (c_guid == stores.c && commits.load() == 0 &&
                commit.tu_seq == jobs[0].binding.tu_seq &&
                std::ranges::equal(exact, inputs[0]))
                return InputJobState::Open;
            if (c_guid == stores.c && commits.load() == 1 &&
                commit.tu_seq == successor_binding.tu_seq &&
                std::ranges::equal(exact, successor_input))
                return InputJobState::Open;
            ++unexpected_input_state_calls;
            return InputJobState::Closed;
        };
    config.record_p51_job_commit =
        [&](const LinkHello& observed, const JobBind& binding,
            const R2TxCommit& commit) {
            const bool original = observed == hello &&
                binding == jobs[0].binding &&
                commit.relationship_ordinal == 1 &&
                commit.inner.raw_digest == jobs[0].binding.raw_digest;
            const bool successor = observed == successor_hello &&
                binding == successor_job.binding &&
                commit.relationship_ordinal == 1 &&
                commit.inner.raw_digest == successor_job.binding.raw_digest;
            if (!original && !successor)
                return false;
            ++commits;
            return true;
        };
    config.acknowledge_p51_receipt =
        [&](const LinkHello& observed, const CommitAck& ack) {
            if ((observed != hello && observed != successor_hello) ||
                ack.relationship_id != observed.relationship_id ||
                ack.relationship_epoch != observed.relationship_epoch ||
                ack.physical_link_generation != observed.physical_link_generation ||
                ack.contiguous_verified_ordinal != 1)
                return false;
            ++acknowledged_receipts;
            return true;
        };
    P50ServerEndpoint server(stores.f, caps, nullptr, nullptr,
                             std::move(config));
    EndpointIoControl server_control;
    server_control.before_materialize_on_worker = [gate] {
        std::unique_lock lock(gate->mutex);
        ++gate->entered_count;
        gate->changed.notify_all();
        if (gate->entered_count == 2) {
            gate->changed.wait(lock, [&] { return gate->release_second; });
            gate->second_exited = true;
            gate->changed.notify_all();
        }
    };

    asio::io_context context;
    tcp::acceptor acceptor(context,
                           {asio::ip::address_v4::loopback(), 0});

    P50ServerOwnerUsage usage_while_held;
    P50ServerOwnerUsage usage_after_reset;
    bool reset_observed_worker = false;
    bool predecessor_input_exact = false;
    bool successor_completed_while_worker_held = false;
    bool successor_result_exact = false;
    bool successor_attachment_exact = false;
    bool detached_usage_drained = false;
    std::vector<R2TxCommit> successor_commits;
    auto reset_when_worker_held = [&]() -> asio::awaitable<void> {
        ReleaseWorkerHistoryOnExit release_on_coroutine_exit{gate};
        const auto executor = co_await asio::this_coro::executor;
        asio::steady_timer poll(executor);
        const auto until = std::chrono::steady_clock::now() +
                           std::chrono::seconds(5);
        bool held = false;
        while (!held && std::chrono::steady_clock::now() < until) {
            {
                std::lock_guard lock(gate->mutex);
                held = gate->entered_count >= 2;
            }
            if (held)
                break;
            poll.expires_after(std::chrono::milliseconds(2));
            boost::system::error_code error;
            co_await poll.async_wait(
                asio::redirect_error(asio::use_awaitable, error));
            if (error)
                break;
        }
        reset_observed_worker = held;
        if (held) {
            usage_while_held = server.owner_usage();
            const auto first_key = server.last_committed_input(stores.c);
            if (first_key && first_key->tu_seq == jobs[0].binding.tu_seq) {
                InputCursor first = server.attach_input(*first_key);
                std::vector<uint8_t> recovered(inputs[0].size());
                predecessor_input_exact =
                    first.read(recovered) == recovered.size() &&
                    recovered == inputs[0];
            }
            server.reset_store(replacement_f);
            usage_after_reset = server.owner_usage();
            std::vector<R2EndpointTestJob> successor_peer_jobs;
            successor_peer_jobs.push_back(successor_job);
            successor_commits = co_await r2_two_job_peer(
                acceptor.local_endpoint(), successor_hello,
                std::move(successor_peer_jobs));
            successor_completed_while_worker_held = true;
            successor_result_exact = successor_commits.size() == 1 &&
                successor_commits[0].relationship_ordinal == 1 &&
                successor_commits[0].inner.tu_seq ==
                    successor_binding.tu_seq &&
                successor_commits[0].inner.raw_digest ==
                    successor_binding.raw_digest;
        }
        release_r2_worker_history_gate(gate);
        if (held) {
            const auto drain_until = std::chrono::steady_clock::now() +
                                     std::chrono::seconds(3);
            auto charges_are_zero = [&] {
                const P50ServerOwnerUsage usage = server.owner_usage();
                return usage.pending_raw_bytes == 0 &&
                       usage.pending_encoded_bytes == 0 &&
                       usage.decoder_window_bytes == 0 &&
                       usage.detached_history_bytes == 0 &&
                       usage.global_detached_resident_bytes == 0;
            };
            while (!charges_are_zero() &&
                   std::chrono::steady_clock::now() < drain_until) {
                poll.expires_after(std::chrono::milliseconds(1));
                boost::system::error_code error;
                co_await poll.async_wait(
                    asio::redirect_error(asio::use_awaitable, error));
                if (error)
                    break;
            }
            detached_usage_drained = charges_are_zero();
        }
    };
    auto reset_future = asio::co_spawn(context, reset_when_worker_held(),
                                        asio::use_future);
    auto server_future = asio::co_spawn(
        context, r2_accept_two(acceptor, server, std::move(server_control)),
        asio::use_future);
    auto peer_future = asio::co_spawn(
        context, r2_peer_observes_reset_during_second_worker(
                     acceptor.local_endpoint(), hello, jobs, peer_failure),
        asio::use_future);
    context.run();
    reset_future.get();
    const std::array<ServerRunResult, 2> server_results = server_future.get();
    const bool peer_saw_reset = peer_future.get();
    const P50ServerOwnerUsage final_usage = server.owner_usage();
    const auto invariant = server.global_resource_invariant_for_test();
    {
        InputCursor successor = server.attach_input(
            InputRecordKey{stores.c, successor_binding.tu_seq});
        std::vector<uint8_t> recovered(successor_input.size());
        successor_attachment_exact =
            successor.read(recovered) == recovered.size() &&
            recovered == successor_input;
    }
    const bool charges_held_by_profile = profile == ProfileId::P29V1
        ? usage_after_reset.global_detached_resident_bytes != 0 &&
              usage_after_reset.detached_history_bytes == 0
        : usage_after_reset.detached_history_bytes != 0 &&
              usage_after_reset.global_detached_resident_bytes == 0;
    std::cerr << "P51_R2_ENDPOINT held-worker details profile="
              << static_cast<unsigned>(profile)
              << " reset_observed=" << reset_observed_worker
              << " predecessor_exact=" << predecessor_input_exact
              << " held(raw,enc,window)=" << usage_while_held.pending_raw_bytes
              << ',' << usage_while_held.pending_encoded_bytes << ','
              << usage_while_held.decoder_window_bytes
              << " reset(raw,enc,window,detached,global)="
              << usage_after_reset.pending_raw_bytes << ','
              << usage_after_reset.pending_encoded_bytes << ','
              << usage_after_reset.decoder_window_bytes << ','
              << usage_after_reset.detached_history_bytes << ','
              << usage_after_reset.global_detached_resident_bytes
              << " profile_charge=" << charges_held_by_profile
              << " peer_reset=" << peer_saw_reset
              << " peer_error=" << peer_failure
              << " server_status(old,new)="
              << static_cast<unsigned>(server_results[0].status) << ','
              << static_cast<unsigned>(server_results[1].status)
              << " commits=" << commits.load()
              << " acked=" << acknowledged_receipts.load()
              << " state_calls=" << input_state_calls.load()
              << " unexpected_state_calls="
              << unexpected_input_state_calls.load()
              << " successor(held,exact,attach)="
              << successor_completed_while_worker_held << ','
              << successor_result_exact << ',' << successor_attachment_exact
              << " consumed=" << consumed[0] << ',' << consumed[1]
              << ',' << successor_consumed
              << " drained=" << detached_usage_drained
              << " final(raw,enc,window,detached,global,namespaces,revisions,records)="
              << final_usage.pending_raw_bytes << ','
              << final_usage.pending_encoded_bytes << ','
              << final_usage.decoder_window_bytes << ','
              << final_usage.detached_history_bytes << ','
              << final_usage.global_detached_resident_bytes << ','
              << final_usage.namespaces << ',' << final_usage.revisions << ','
              << final_usage.retained_input_records
              << " invariant=" << invariant.value_or("none")
              << " terminal_detail="
              << (server_results[0].terminal_error
                      ? server_results[0].terminal_error->detail
                      : std::string("none"))
              << '\n';
    require(reset_observed_worker && predecessor_input_exact &&
                usage_while_held.pending_raw_bytes != 0 &&
                usage_while_held.pending_encoded_bytes != 0 &&
                usage_while_held.decoder_window_bytes != 0 &&
                usage_after_reset.pending_raw_bytes != 0 &&
                usage_after_reset.pending_encoded_bytes != 0 &&
                usage_after_reset.decoder_window_bytes != 0 &&
                charges_held_by_profile && peer_saw_reset &&
                server_results[0].status != ServerRunStatus::Completed &&
                server_results[1].status == ServerRunStatus::Completed &&
                successor_completed_while_worker_held &&
                successor_result_exact && successor_attachment_exact &&
                commits.load() == 2 && acknowledged_receipts.load() == 2 &&
                input_state_calls.load() == 2 &&
                unexpected_input_state_calls.load() == 0 &&
                consumed[0] && consumed[1] && successor_consumed &&
                detached_usage_drained &&
                final_usage.pending_raw_bytes == 0 &&
                final_usage.pending_encoded_bytes == 0 &&
                final_usage.decoder_window_bytes == 0 &&
                final_usage.detached_history_bytes == 0 &&
                final_usage.global_detached_resident_bytes == 0 &&
                final_usage.namespaces == 1 && final_usage.revisions == 1 &&
                final_usage.retained_input_records == 1 && !invariant,
            std::string(profile_name(profile)) +
                " reset did not retain and drain detached worker history exactly");
    std::cout << "P51_R2_ENDPOINT held-worker reset/successor profile="
              << static_cast<unsigned>(profile) << ": ok\n";
}

void test_r2_persistent_history_charge_survives_worker_reset() {
    for (const ProfileId profile : {ProfileId::P29V1,
                                    ProfileId::ZSTD_ROUTE})
        test_r2_persistent_history_charge_survives_worker_reset(profile);
}

void test_r2_deadline_stage(ProfileId profile, R2DeadlineStage stage) {
    const P5coStoreGuids stores = p5co_store_guids(
        0x5b00 + static_cast<uint64_t>(profile));
    EndpointCaps caps;
    caps.profile = profile;
    caps.supported_profiles = profile_bit(profile);
    caps.zstd.max_raw_bytes = 1U << 20;
    caps.zstd.max_encoded_body_bytes = 1U << 20;
    caps.zstd.max_history_bytes = 1U << 20;
    PreparationAuthorityLimits authority_limits;
    authority_limits.max_speculative_tus = 1;
    authority_limits.max_speculative_raw_bytes = 1U << 20;
    auto authority = std::make_shared<P50PreparationAuthority>(
        stores.c, caps.zstd, authority_limits, 1, profile);
    const PreparationRouteKey route{stores.f, 23, profile};
    const std::vector<uint8_t> input(64U << 10, static_cast<uint8_t>('C'));
    const P51SourceArmFields arm{
        r2_test_arm(stores.c, 8801, 9801, profile), 1};
    const P51SourceArmedFields armed = r2_test_armed(arm, stores.f, 0x5b01);
    const sidecar::AbsoluteMonotonicDeadline deadline = r2_test_deadline(
        stage == R2DeadlineStage::PeerClose ? std::chrono::seconds(8)
                                            : std::chrono::seconds(2));
    const PreparedTuHandle handle = authority->prepare_for_route(
        route, PrepareRequestKey{8801, 9801}, input);
    std::vector<R2FillMessage> fills;
    if (profile == ProfileId::P29V1) {
        authority->pin_p29v1_system_source_reuse(handle, Digest128{});
        const std::vector<uint8_t> need =
            authority->predicted_p29v1_need(handle);
        const std::span<const uint8_t> fill =
            authority->answer_p29v1_need(handle, need);
        icecc::codec::P29WireLimits wire_limits;
        wire_limits.max_tu_bytes =
            static_cast<size_t>(caps.zstd.max_raw_bytes);
        wire_limits.max_region_bytes = wire_limits.max_tu_bytes;
        const std::vector<FillMessage> encoded = encode_p29v1_fill_messages(
            fill, kInitialMaxFramePayload,
            icecc::codec::p29v1_fill_inner_bound(wire_limits));
        fills.reserve(encoded.size());
        for (const FillMessage& message : encoded)
            fills.push_back(R2FillMessage{message.bytes});
        authority->advance_p29v1_speculative(handle);
    } else {
        authority->advance_speculative(handle);
    }
    const PreparedInputPtr prepared =
        P50PreparationAuthorityTestAccess::resolve(*authority, handle);

    LinkHello hello;
    hello.profile = profile;
    hello.window = 1;
    hello.max_frame_payload = kInitialMaxFramePayload;
    hello.max_raw_bytes = 1U << 20;
    hello.max_encoded_bytes = 1U << 20;
    hello.max_output_bytes = 1U << 20;
    hello.reservation_id = Id128{armed.reservation_id};
    hello.relationship_id = Id128{armed.logical_relationship_id};
    hello.relationship_epoch = armed.relationship_epoch;
    hello.physical_link_generation = 26;
    hello.c_store_guid = stores.c;
    hello.c_store_generation = arm.source.c_store_generation;
    hello.f_store_guid = stores.f;
    hello.f_store_generation = armed.f_store_generation;
    hello.c_control_generation = arm.source.c_control_generation;
    hello.c_control_attempt = arm.source.c_control_attempt;
    hello.system_source_fingerprint = profile == ProfileId::P29V1
        ? authority->p29v1_system_source_fingerprint(handle)
        : icecc::digest128("R2 peer-close ZSTD_ROUTE fixture");
    hello.history_nonce = prepared->begin.history_nonce;
    hello.start_mode = LinkStartMode::Initial;

    JobBind binding;
    binding.reservation_id = hello.reservation_id;
    binding.physical_link_generation = hello.physical_link_generation;
    binding.relationship_ordinal = 1;
    binding.wire_job_id = arm.source.wire_job_id;
    binding.assignment_epoch = arm.source.assignment_epoch;
    binding.assignment_nonce = arm.source.assignment_nonce;
    binding.logical_job = arm.source.logical_job;
    binding.compiler_attempt = arm.source.compiler_attempt;
    binding.source_request_id = arm.source.source_request_id;
    binding.tu_seq = prepared->begin.tu_seq;
    binding.profile = profile;
    binding.raw_bytes = prepared->begin.raw_bytes;
    binding.raw_digest = prepared->begin.raw_digest;
    P51SourceJobLease job_lease;
    job_lease.armed = armed;
    job_lease.absolute_deadline = deadline;
    job_lease.binding = binding;
    job_lease.binding_digest = compute_r2_binding_digest(binding);
    job_lease.input_key = InputRecordKey{stores.c, binding.tu_seq};
    R2EndpointTestJob job{binding, job_lease, prepared, std::move(fills)};
    P51SourceLinkLease link_lease{armed, deadline};
    link_lease.relationship_epoch = hello.relationship_epoch;

    std::atomic<bool> consumed{false};
    std::atomic<unsigned> consume_calls{0};
    std::atomic<bool> peer_closed{false};
    std::atomic<unsigned> state_calls{0};
    std::atomic<unsigned> commit_calls{0};
    std::atomic<unsigned> ack_calls{0};
    P50ServerEndpointConfig config;
    config.lookup_p51_link_reservation =
        [&](const LinkHello& observed) -> std::optional<P51SourceLinkLease> {
            if (observed != hello)
                return std::nullopt;
            return link_lease;
        };
    config.consume_p51_job_reservation =
        [&](const LinkHello& observed,
            const JobBind& observed_binding)
            -> std::optional<P51SourceJobLease> {
            ++consume_calls;
            if (stage == R2DeadlineStage::BeforeBind &&
                std::chrono::steady_clock::now() >=
                    deadline.as_steady_time_point())
                return std::nullopt;
            bool expected = false;
            if (observed != hello || observed_binding != binding ||
                !consumed.compare_exchange_strong(expected, true))
                return std::nullopt;
            if (stage == R2DeadlineStage::ReturnedExpiredLease) {
                P51SourceJobLease expired = job_lease;
                expired.absolute_deadline =
                    r2_test_deadline(std::chrono::seconds(-1));
                return expired;
            }
            return job_lease;
        };
    config.input_job_state =
        [&, deadline](CStoreGuid c_guid, const TxBegin&,
            const TxCommit& commit, std::span<const uint8_t> exact) {
            ++state_calls;
            if (stage == R2DeadlineStage::BeforePublication)
                std::this_thread::sleep_until(
                    deadline.as_steady_time_point() +
                    std::chrono::milliseconds(2));
            return c_guid == stores.c &&
                   commit.tu_seq == binding.tu_seq &&
                   commit.raw_digest == binding.raw_digest &&
                   std::ranges::equal(exact, input)
                       ? InputJobState::Open
                       : InputJobState::Closed;
        };
    std::atomic<unsigned> authorize_calls{0};
    config.authorize_p51_job_publication =
        [&](const LinkHello& observed, const JobBind& observed_binding) {
            if (observed != hello || observed_binding != binding)
                return false;
            ++authorize_calls;
            return true;
        };
    config.record_p51_job_commit =
        [&](const LinkHello& observed, const JobBind& observed_binding,
            const R2TxCommit& commit) {
            if (observed != hello || observed_binding != binding ||
                commit.relationship_ordinal != 1 ||
                commit.inner.tu_seq != binding.tu_seq ||
                commit.inner.raw_digest != binding.raw_digest)
                return false;
            ++commit_calls;
            return true;
        };
    config.acknowledge_p51_receipt =
        [&](const LinkHello& observed, const CommitAck& ack) {
            if (observed != hello ||
                ack.relationship_id != hello.relationship_id ||
                ack.relationship_epoch != hello.relationship_epoch ||
                ack.physical_link_generation !=
                    hello.physical_link_generation ||
                ack.contiguous_verified_ordinal != 1)
                return false;
            ++ack_calls;
            return true;
        };
    P50ServerEndpoint server(stores.f, caps, nullptr, nullptr,
                             std::move(config));
    auto gate = std::make_shared<R2PeerCloseWorkerGate>();
    EndpointIoControl control;
    control.before_materialize_on_worker = [gate, stage] {
        std::unique_lock lock(gate->mutex);
        gate->entered = true;
        gate->changed.notify_all();
        if (stage == R2DeadlineStage::BeforePublication)
            return;
        gate->changed.wait(lock, [&] { return gate->release; });
        gate->exited = true;
        gate->changed.notify_all();
    };
    struct ReleasePeerCloseGateOnExit {
        std::shared_ptr<R2PeerCloseWorkerGate> gate;
        ~ReleasePeerCloseGateOnExit() {
            release_r2_peer_close_worker_gate(gate);
        }
    } release_on_exit{gate};

    asio::io_context context;
    tcp::acceptor acceptor(context,
                           {asio::ip::address_v4::loopback(), 0});
    P50ServerOwnerUsage usage_while_peer_closed;
    bool worker_held_at_observation = false;
    unsigned owner_timer_ticks_while_held = 0;
    bool usage_drained_after_release = false;
    auto observe_close_and_release = [&]() -> asio::awaitable<void> {
        if (stage == R2DeadlineStage::BeforePublication)
            co_return;
        const auto executor = co_await asio::this_coro::executor;
        asio::steady_timer timer(executor);
        const auto until = std::chrono::steady_clock::now() +
                           std::chrono::seconds(6);
        while (!peer_closed.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < until) {
            timer.expires_after(std::chrono::milliseconds(1));
            boost::system::error_code error;
            co_await timer.async_wait(
                asio::redirect_error(asio::use_awaitable, error));
            if (error)
                break;
        }
        {
            std::lock_guard lock(gate->mutex);
            worker_held_at_observation = gate->entered && !gate->release;
        }
        if (peer_closed.load(std::memory_order_acquire))
            usage_while_peer_closed = server.owner_usage();
        timer.expires_after(std::chrono::milliseconds(10));
        boost::system::error_code error;
        co_await timer.async_wait(
            asio::redirect_error(asio::use_awaitable, error));
        if (!error) {
            std::lock_guard lock(gate->mutex);
            if (gate->entered && !gate->release)
                ++owner_timer_ticks_while_held;
        }
        release_r2_peer_close_worker_gate(gate);
        const auto drain_until = std::chrono::steady_clock::now() +
                                 std::chrono::seconds(3);
        auto charges_are_zero = [&] {
            const P50ServerOwnerUsage usage = server.owner_usage();
            return usage.pending_raw_bytes == 0 &&
                   usage.pending_encoded_bytes == 0 &&
                   usage.decoder_window_bytes == 0 &&
                   usage.detached_history_bytes == 0 &&
                   usage.global_detached_resident_bytes == 0;
        };
        while (!charges_are_zero() &&
               std::chrono::steady_clock::now() < drain_until) {
            timer.expires_after(std::chrono::milliseconds(1));
            boost::system::error_code drain_error;
            co_await timer.async_wait(
                asio::redirect_error(asio::use_awaitable, drain_error));
            if (drain_error)
                break;
        }
        usage_drained_after_release = charges_are_zero();
    };
    auto server_future = asio::co_spawn(
        context, r2_accept_one(acceptor, server, std::move(control)),
        asio::use_future);
    auto peer_future = asio::co_spawn(
        context, r2_peer_close_after_worker_entered(
                     acceptor.local_endpoint(), hello, std::move(job), gate,
                     peer_closed, stage),
        asio::use_future);
    auto observer_future = asio::co_spawn(
        context, observe_close_and_release(), asio::use_future);
    context.run();
    const bool closed_after_worker = peer_future.get();
    observer_future.get();
    const ServerRunResult server_result = server_future.get();
    if (stage == R2DeadlineStage::BeforePublication)
        usage_drained_after_release = true;
    const P50ServerOwnerUsage final_usage = server.owner_usage();
    bool committed_input_exact = false;
    if (commit_calls.load() == 1) {
        InputCursor cursor = server.attach_input(InputRecordKey{
            stores.c, binding.tu_seq});
        std::vector<uint8_t> recovered(input.size());
        committed_input_exact = cursor.read(recovered) == recovered.size() &&
                                recovered == input;
    } else {
        committed_input_exact = commit_calls.load() == 0 &&
                                !server.last_committed_input(stores.c);
    }
    const auto invariant = server.global_resource_invariant_for_test();
    const bool charge_retained_while_closed =
        usage_while_peer_closed.pending_raw_bytes >= input.size() &&
        usage_while_peer_closed.pending_encoded_bytes != 0 &&
        usage_while_peer_closed.decoder_window_bytes != 0;
    const bool charges_drained = usage_drained_after_release &&
        final_usage.pending_raw_bytes == 0 &&
        final_usage.pending_encoded_bytes == 0 &&
        final_usage.decoder_window_bytes == 0 &&
        final_usage.detached_history_bytes == 0 &&
        final_usage.global_detached_resident_bytes == 0;
    std::cerr << "P51_R2_ENDPOINT peer-close details profile="
              << static_cast<unsigned>(profile)
              << " closed=" << peer_closed.load()
              << " worker_held=" << worker_held_at_observation
              << " owner_ticks=" << owner_timer_ticks_while_held
              << " retained(raw,enc,window)="
              << usage_while_peer_closed.pending_raw_bytes << ','
              << usage_while_peer_closed.pending_encoded_bytes << ','
              << usage_while_peer_closed.decoder_window_bytes
              << " server_status=" << static_cast<unsigned>(server_result.status)
              << " consumed=" << consumed.load()
              << " state_calls=" << state_calls.load()
              << " commits=" << commit_calls.load()
              << " acks=" << ack_calls.load()
              << " input_exact=" << committed_input_exact
              << " final(raw,enc,window,detached,global)="
              << final_usage.pending_raw_bytes << ','
              << final_usage.pending_encoded_bytes << ','
              << final_usage.decoder_window_bytes << ','
              << final_usage.detached_history_bytes << ','
              << final_usage.global_detached_resident_bytes
              << " invariant=" << invariant.value_or("none") << '\n';
    if (stage == R2DeadlineStage::PeerClose) {
        require(closed_after_worker && peer_closed.load() &&
                    worker_held_at_observation && owner_timer_ticks_while_held > 0 &&
                    charge_retained_while_closed &&
                    server_result.status == ServerRunStatus::Disconnected &&
                    consumed.load() && commit_calls.load() <= 1 &&
                    state_calls.load() <= 1 && authorize_calls.load() <= 1 &&
                    ack_calls.load() == 0 && committed_input_exact &&
                    charges_drained && !invariant,
                std::string(profile_name(profile)) +
                    " peer close while R2 materialization was held lost owner progress or exact settlement");
        std::cout << "P51_R2_ENDPOINT peer-close worker profile="
                  << static_cast<unsigned>(profile) << ": ok\n";
        return;
    }
    if (stage == R2DeadlineStage::BeforeBind) {
        require(!closed_after_worker && peer_closed.load() &&
                    consume_calls.load() == 1 && !consumed.load() &&
                    server_result.status == ServerRunStatus::TerminalError &&
                    state_calls.load() == 0 && authorize_calls.load() == 0 &&
                    commit_calls.load() == 0 && ack_calls.load() == 0 &&
                    committed_input_exact && charges_drained && !invariant,
                std::string(profile_name(profile)) +
                    " expired source reservation was consumed or published before bind");
        std::cout << "P51_R2_DEADLINE stage=before_bind profile="
                  << static_cast<unsigned>(profile)
                  << " consume_attempts=" << consume_calls.load()
                  << " consumed=" << consumed.load()
                  << " commits=" << commit_calls.load() << ": ok\n";
        return;
    }
    if (stage == R2DeadlineStage::ReturnedExpiredLease) {
        require(!closed_after_worker && peer_closed.load() &&
                    consume_calls.load() == 1 && consumed.load() &&
                    server_result.status == ServerRunStatus::DeadlineExceeded &&
                    state_calls.load() == 0 && authorize_calls.load() == 0 &&
                    commit_calls.load() == 0 && ack_calls.load() == 0 &&
                    committed_input_exact && charges_drained && !invariant,
                std::string(profile_name(profile)) +
                    " endpoint admitted a JOB_BIND with an already-expired lease");
        std::cout << "P51_R2_DEADLINE stage=returned_expired_lease profile="
                  << static_cast<unsigned>(profile)
                  << " consume_attempts=" << consume_calls.load()
                  << " consumed=" << consumed.load()
                  << " commits=" << commit_calls.load() << ": ok\n";
        return;
    }
    const unsigned expected_state_calls =
        stage == R2DeadlineStage::BeforePublication ? 1 : 0;
    require(closed_after_worker && peer_closed.load() && consumed.load() &&
                server_result.status == ServerRunStatus::DeadlineExceeded &&
                state_calls.load() == expected_state_calls &&
                authorize_calls.load() == 0 && commit_calls.load() == 0 &&
                ack_calls.load() == 0 && committed_input_exact &&
                charges_drained && !invariant &&
                (stage != R2DeadlineStage::DuringDecode ||
                 (worker_held_at_observation &&
                  owner_timer_ticks_while_held > 0 &&
                  charge_retained_while_closed)),
            std::string(profile_name(profile)) +
                " R2 source deadline expired at the requested stage but publication or cleanup was incorrect");
    std::cout << "P51_R2_DEADLINE stage="
              << (stage == R2DeadlineStage::DuringDecode ? "decode" : "before_publication")
              << " profile=" << static_cast<unsigned>(profile)
              << " consumed=" << consumed.load()
              << " state=" << state_calls.load()
              << " authorize=" << authorize_calls.load()
              << " commits=" << commit_calls.load()
              << " status=" << static_cast<unsigned>(server_result.status)
              << ": ok\n";
}

void test_r2_peer_close_during_materialization(ProfileId profile) {
    test_r2_deadline_stage(profile, R2DeadlineStage::PeerClose);
}

void test_r2_peer_close_during_materialization() {
    for (const ProfileId profile : {ProfileId::P29V1,
                                    ProfileId::ZSTD_ROUTE})
        test_r2_peer_close_during_materialization(profile);
}

void test_r2_deadline_expiry_stages() {
    for (const ProfileId profile : {ProfileId::P29V1,
                                    ProfileId::ZSTD_ROUTE,
                                    ProfileId::ZSTD_TU}) {
        test_r2_deadline_stage(profile, R2DeadlineStage::BeforeBind);
        test_r2_deadline_stage(profile, R2DeadlineStage::ReturnedExpiredLease);
        test_r2_deadline_stage(profile, R2DeadlineStage::DuringDecode);
        test_r2_deadline_stage(profile, R2DeadlineStage::BeforePublication);
    }
}

void test_r2_endpoint_window30_receipts_and_refill(ProfileId profile) {
    constexpr size_t window = 30;
    constexpr size_t total_jobs = window + 1;
    const P5coStoreGuids stores = p5co_store_guids(0x74);
    EndpointCaps caps;
    caps.profile = profile;
    caps.supported_profiles = profile_bit(profile);
    PreparationAuthorityLimits authority_limits;
    authority_limits.max_speculative_tus = window;
    authority_limits.max_speculative_raw_bytes = 1U << 20;
    auto authority = std::make_shared<P50PreparationAuthority>(
        stores.c, caps.zstd, authority_limits, 1, profile);
    const PreparationRouteKey route{stores.f, 23, profile};

    std::vector<std::vector<uint8_t>> inputs;
    std::vector<JobBind> bindings;
    std::vector<R2EndpointTestJob> jobs;
    inputs.reserve(total_jobs);
    bindings.reserve(total_jobs);
    jobs.reserve(total_jobs);
    std::vector<P51SourceArmFields> arms;
    std::vector<P51SourceArmedFields> armed;
    std::vector<bool> consumed;
    arms.reserve(total_jobs);
    armed.reserve(total_jobs);

    const sidecar::AbsoluteMonotonicDeadline deadline = r2_test_deadline();
    arms.push_back(P51SourceArmFields{
        r2_test_arm(stores.c, 3000, 4000, profile),
        static_cast<uint32_t>(window)});
    armed.push_back(r2_test_armed(arms.front(), stores.f, 0x6000,
                                  static_cast<uint32_t>(window)));

    const std::string first_text = "W30 exact input ordinal=1\n";
    inputs.emplace_back(first_text.begin(), first_text.end());
    const PreparedTuHandle first_handle = authority->prepare_for_route(
        route, PrepareRequestKey{1000, 2000}, inputs.front());
    const PreparedInputPtr first_input =
        P50PreparationAuthorityTestAccess::resolve(*authority, first_handle);

    LinkHello hello;
    hello.profile = profile;
    hello.window = static_cast<uint32_t>(window);
    hello.max_frame_payload = kInitialMaxFramePayload;
    hello.max_raw_bytes = 1U << 20;
    hello.max_encoded_bytes = 1U << 20;
    hello.max_output_bytes = 1U << 20;
    hello.reservation_id = Id128{armed.front().reservation_id};
    hello.relationship_id = Id128{armed.front().logical_relationship_id};
    hello.relationship_epoch = armed.front().relationship_epoch;
    hello.physical_link_generation = 27;
    hello.c_store_guid = stores.c;
    hello.c_store_generation = arms.front().source.c_store_generation;
    hello.f_store_guid = stores.f;
    hello.f_store_generation = armed.front().f_store_generation;
    hello.c_control_generation = arms.front().source.c_control_generation;
    hello.c_control_attempt = arms.front().source.c_control_attempt;
    hello.system_source_fingerprint = profile == ProfileId::P29V1
        ? authority->p29v1_system_source_fingerprint(first_handle)
        : icecc::digest128("R2 W30 fixture");
    // The link begins at the CRoute's actual initial cursor. Hard-coding a
    // fixture nonce here can make the receiver reject before TU1 is sent.
    hello.history_nonce = first_input->begin.history_nonce;
    hello.start_mode = LinkStartMode::Initial;

    // Stateful codecs must be prepared one at a time: each preceding bundle
    // advances the speculative preparation cursor before the next TU is built.
    // The same factory drives TU, P29, and ROUTE profiles through one harness.
    auto make_job = [&, authority, route](size_t index) {
        require(index <= window,
                "R2 W30 peer attempted to prepare beyond refill ordinal 31");
        PreparedTuHandle handle = first_handle;
        if (index != 0) {
            const std::string text = "W30 exact input ordinal=" +
                                     std::to_string(index + 1) + "\n";
            inputs.emplace_back(text.begin(), text.end());
            handle = authority->prepare_for_route(
                route, PrepareRequestKey{1000 + index, 2000 + index},
                inputs.back());
        }
        const PreparedInputPtr input =
            P50PreparationAuthorityTestAccess::resolve(*authority, handle);
        if (index != 0) {
            arms.push_back(P51SourceArmFields{
                r2_test_arm(stores.c, 3000 + index, 4000 + index, profile),
                static_cast<uint32_t>(window)});
            armed.push_back(r2_test_armed(arms.back(), stores.f,
                                          0x6000 + index,
                                          static_cast<uint32_t>(window)));
        }

        JobBind binding;
        binding.reservation_id = Id128{armed.back().reservation_id};
        binding.physical_link_generation = hello.physical_link_generation;
        binding.relationship_ordinal = index + 1;
        binding.wire_job_id = arms.back().source.wire_job_id;
        binding.assignment_epoch = arms.back().source.assignment_epoch;
        binding.assignment_nonce = arms.back().source.assignment_nonce;
        binding.logical_job = arms.back().source.logical_job;
        binding.compiler_attempt = arms.back().source.compiler_attempt;
        binding.source_request_id = arms.back().source.source_request_id;
        binding.tu_seq = input->begin.tu_seq;
        binding.profile = profile;
        binding.raw_bytes = input->begin.raw_bytes;
        binding.raw_digest = input->begin.raw_digest;
        bindings.push_back(binding);

        P51SourceJobLease lease;
        lease.armed = armed.back();
        lease.absolute_deadline = deadline;
        lease.binding = binding;
        lease.binding_digest = compute_r2_binding_digest(binding);
        lease.input_key = InputRecordKey{stores.c, binding.tu_seq};
        jobs.push_back(R2EndpointTestJob{binding, lease, input, {}});
        consumed.push_back(false);
        return std::pair{binding, handle};
    };

    std::atomic<unsigned> lookup_calls{0};
    std::atomic<unsigned> consume_calls{0};
    std::atomic<unsigned> commit_calls{0};
    std::atomic<unsigned> ack_calls{0};
    std::atomic<unsigned> terminal_calls{0};
    CompletionLog f_socket_observations;
    std::vector<R2ObservedSocketBytes> peer_boundaries;
    uint64_t last_ack = 0;
    P50ServerEndpointConfig config;
    config.lookup_p51_link_reservation =
        [&, deadline](const LinkHello& observed)
            -> std::optional<P51SourceLinkLease> {
            ++lookup_calls;
            if (observed != hello)
                return std::nullopt;
            P51SourceLinkLease lease{armed.front(), deadline};
            lease.relationship_epoch = observed.relationship_epoch;
            return lease;
        };
    config.consume_p51_job_reservation =
        [&](const LinkHello& observed, const JobBind& binding)
            -> std::optional<P51SourceJobLease> {
            ++consume_calls;
            if (observed != hello)
                return std::nullopt;
            for (size_t index = 0; index != jobs.size(); ++index) {
                if (jobs[index].binding == binding && !consumed[index]) {
                    consumed[index] = true;
                    return jobs[index].lease;
                }
            }
            return std::nullopt;
        };
    config.record_p51_job_commit =
        [&](const LinkHello& observed, const JobBind& binding,
            const R2TxCommit& commit) {
            if (observed != hello ||
                commit.relationship_ordinal != binding.relationship_ordinal ||
                commit.inner.tu_seq != binding.tu_seq)
                return false;
            ++commit_calls;
            return true;
        };
    config.acknowledge_p51_receipt =
        [&](const LinkHello& observed, const CommitAck& ack) {
            if (observed != hello ||
                ack.relationship_id != hello.relationship_id ||
                ack.contiguous_verified_ordinal <= last_ack ||
                ack.contiguous_verified_ordinal > commit_calls.load())
                return false;
            last_ack = ack.contiguous_verified_ordinal;
            const unsigned ack_index =
                ack_calls.fetch_add(1, std::memory_order_acq_rel);
            if (ack_index < 2)
                peer_boundaries.push_back(
                    r2_observed_server_socket_bytes(f_socket_observations));
            else
                return false;
            return true;
        };
    config.on_p51_link_terminal = [&](
        const LinkHello& observed, const std::optional<JobBind>&) {
        if (observed == hello)
            ++terminal_calls;
    };

    P50ServerEndpoint server(stores.f, caps, &f_socket_observations, nullptr,
                             std::move(config));
    CompletionLog c_accounting{CompletionLog::StorageMode::ClientByteTotals};
    P50ClientEndpoint client(authority, caps, hello.history_nonce,
                             &c_accounting);
    std::vector<R2WireAccountingSnapshot> job_accounting;
    std::vector<R2WireControlSnapshot> interval_accounting;
    asio::io_context context;
    tcp::acceptor acceptor(context,
                           {asio::ip::address_v4::loopback(), 0});
    auto accept_future = asio::co_spawn(
        context, r2_accept_one(acceptor, server), asio::use_future);
    auto client_future = asio::co_spawn(
        context, r2_client_write_window_before_receipts(
                     acceptor.local_endpoint(), client, hello, commit_calls,
                     make_job, &c_accounting, &ack_calls, &job_accounting,
                     &interval_accounting, &f_socket_observations,
                     &peer_boundaries),
        asio::use_future);
    context.run();
    const ServerRunResult server_result = accept_future.get();
    if (server_result.terminal_error)
        std::cerr << "R2 W30 F terminal error: "
                  << server_result.terminal_error->detail << '\n';
    try {
        client_future.get();
    } catch (const std::exception& error) {
        std::cerr << "R2 W30 C peer error: " << error.what() << '\n';
        throw;
    }
    require(server_result.status == ServerRunStatus::Completed &&
                lookup_calls == 1 && consume_calls == total_jobs &&
                commit_calls == total_jobs && ack_calls == 2 &&
                last_ack == total_jobs && terminal_calls == 1,
            "R2 W30 link failed exact 30-credit drain, cumulative ACK, and refill");

    if (job_accounting.size() != total_jobs || interval_accounting.size() != 3 ||
        peer_boundaries.size() != 3 ||
        std::any_of(job_accounting.begin(), job_accounting.end(),
                    [](const R2WireAccountingSnapshot& value) {
                        return !value.valid || value.bundle_attempts != 1 ||
                            value.replay_attempts != 0 ||
                            value.c_to_f_bundle_bytes == 0 ||
                            value.f_to_c_receipt_bytes == 0;
                    }) ||
        (interval_accounting.size() == 3 &&
         (interval_accounting[0].jobs.size() != window ||
          !interval_accounting[1].jobs.empty() ||
          interval_accounting[2].jobs.size() != 1))) {
        std::cerr << "W30 accounting diagnostic jobs=" << job_accounting.size()
                  << " intervals=" << interval_accounting.size();
        for (const R2WireControlSnapshot& interval : interval_accounting) {
            std::cerr << " [seq=" << interval.interval_sequence
                      << " valid=" << interval.valid
                      << " end=" << static_cast<unsigned>(interval.end)
                      << " jobs=" << interval.jobs.size()
                      << " total=" << interval.total_c_to_f_bytes << '/'
                      << interval.total_f_to_c_bytes
                      << " shared=" << interval.shared_c_to_f_bytes << '/'
                      << interval.shared_f_to_c_bytes << ']';
        }
        for (size_t i = 0; i != std::min<size_t>(job_accounting.size(), 3); ++i) {
            const auto& job = job_accounting[i];
            std::cerr << " job" << i << "={" << job.valid << ','
                      << job.bundle_attempts << ',' << job.replay_attempts << ','
                      << job.c_to_f_bundle_bytes << ','
                      << job.f_to_c_receipt_bytes << '}';
        }
        std::cerr << '\n';
    }
    require(job_accounting.size() == total_jobs &&
                bindings.front().tu_seq.value == 0 &&
                std::all_of(job_accounting.begin(), job_accounting.end(),
                            [](const R2WireAccountingSnapshot& value) {
                                return value.valid &&
                                    value.bundle_attempts == 1 &&
                                    value.replay_attempts == 0 &&
                                    value.c_to_f_bundle_bytes != 0 &&
                                    value.f_to_c_receipt_bytes != 0;
                            }) &&
                peer_boundaries.size() == 3 &&
                interval_accounting.size() == 3 &&
                std::all_of(interval_accounting.begin(),
                            interval_accounting.end(),
                            [](const R2WireControlSnapshot& value) {
                                return value.valid;
                            }) &&
                interval_accounting[0].end ==
                    R2WireIntervalEnd::WindowPressure &&
                interval_accounting[1].end ==
                    R2WireIntervalEnd::DrainedAckCheckpoint &&
                interval_accounting[2].end ==
                    R2WireIntervalEnd::DrainedAckCheckpoint &&
                interval_accounting[0].jobs.size() == window &&
                interval_accounting[1].jobs.empty() &&
                interval_accounting[2].jobs.size() == 1,
                "R2 W30 accounting omitted exact jobs or ACK-drained intervals");

    R2ObservedSocketBytes prior_peer{};
    for (size_t index = 0; index != interval_accounting.size(); ++index) {
        const R2WireControlSnapshot& interval = interval_accounting[index];
        uint64_t classified_c_to_f = interval.shared_c_to_f_bytes;
        uint64_t classified_f_to_c = interval.shared_f_to_c_bytes;
        for (const R2WireJobInterval& job : interval.jobs) {
            require(job.valid && job.link == interval.link,
                    "R2 interval job row escaped its stable physical identity");
            classified_c_to_f += job.c_to_f_bundle_bytes;
            classified_f_to_c += job.f_to_c_receipt_bytes;
        }
        const R2ObservedSocketBytes peer = peer_boundaries[index];
        require(interval.total_c_to_f_bytes == classified_c_to_f &&
                    interval.total_f_to_c_bytes == classified_f_to_c &&
                    interval.total_c_to_f_bytes == peer.c_to_f -
                        prior_peer.c_to_f &&
                    interval.total_f_to_c_bytes == peer.f_to_c -
                        prior_peer.f_to_c,
                "R2 interval bytes did not conserve against independent F socket reads/writes");
        prior_peer = peer;
    }

    for (size_t index = 0; index != total_jobs; ++index) {
        InputCursor cursor = server.attach_input(
            InputRecordKey{stores.c, bindings[index].tu_seq});
        std::vector<uint8_t> recovered(inputs[index].size());
        require(cursor.read(recovered) == recovered.size() &&
                    recovered == inputs[index],
                "R2 W30 endpoint changed materialized source bytes");
    }
    std::cout << "P51_R2_ENDPOINT W30 commits-before-receipts + ACK/refill profile="
              << static_cast<unsigned>(profile) << ": ok\n";
}

asio::awaitable<ClientRunResult> r2_fragmented_single_job_client(
    tcp::endpoint remote, P50ClientEndpoint& client, LinkHello hello,
    JobBind binding, PreparedTuHandle prepared,
    std::chrono::steady_clock::time_point deadline,
    EndpointIoControl bundle_control,
    std::vector<std::pair<MessageType, size_t>>& observed) {
    tcp::socket socket(co_await asio::this_coro::executor);
    co_await socket.async_connect(remote, asio::use_awaitable);
    (void)co_await client.open_r2_link(socket, hello, deadline);
    bundle_control.outbound_message_observer =
        [&](ActorSide actor, const Message& message) {
            if (actor == ActorSide::C)
                observed.emplace_back(message_type(message),
                                      encode_frame(message).size());
        };
    const R2SentBundle sent = co_await client.write_r2_bundle(
        socket, binding, prepared, deadline, std::move(bundle_control));
    const ClientRunResult receipt = co_await client.read_r2_receipt(
        socket, sent, deadline);
    co_await client.write_r2_ack(socket, binding.relationship_ordinal,
                                 deadline);
    co_await raw_write_fragmented(socket, Message{CloseMessage{}}, 1);
    boost::system::error_code ignored;
    socket.shutdown(tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
    co_return receipt;
}

void test_r2_fragmented_one_job_each_profile() {
    for (const ProfileId profile : {ProfileId::ZSTD_TU, ProfileId::P29V1,
                                    ProfileId::ZSTD_ROUTE}) {
        const P5coStoreGuids stores = p5co_store_guids(
            0x7b + static_cast<uint64_t>(profile));
        EndpointCaps caps;
        caps.profile = profile;
        caps.supported_profiles = profile_bit(profile);
        PreparationAuthorityLimits authority_limits;
        authority_limits.max_speculative_tus = 1;
        authority_limits.max_speculative_raw_bytes = 1U << 20;
        auto authority = std::make_shared<P50PreparationAuthority>(
            stores.c, caps.zstd, authority_limits, 1, profile);
        const PreparationRouteKey route{stores.f, 23, profile};
        const std::string text = "int r2_fragmented_profile = 17;\n";
        const std::vector<uint8_t> input(text.begin(), text.end());
        const P51SourceArmFields arm{
            r2_test_arm(stores.c, 6100 + static_cast<uint64_t>(profile),
                        7100 + static_cast<uint32_t>(profile), profile),
            1};
        const P51SourceArmedFields armed =
            r2_test_armed(arm, stores.f,
                          0x7100 + static_cast<uint64_t>(profile), 1);
        const PreparedTuHandle prepared = authority->prepare_for_route(
            route, PrepareRequestKey{6100, 7100}, input);
        const PreparedInputPtr prepared_input =
            P50PreparationAuthorityTestAccess::resolve(*authority, prepared);
        const sidecar::AbsoluteMonotonicDeadline job_deadline =
            r2_test_deadline(std::chrono::seconds(20));

        LinkHello hello;
        hello.profile = profile;
        hello.window = 1;
        hello.max_frame_payload = kInitialMaxFramePayload;
        hello.max_raw_bytes = 1U << 20;
        hello.max_encoded_bytes = 1U << 20;
        hello.max_output_bytes = 1U << 20;
        hello.reservation_id = Id128{armed.reservation_id};
        hello.relationship_id = Id128{armed.logical_relationship_id};
        hello.relationship_epoch = armed.relationship_epoch;
        hello.physical_link_generation = 27;
        hello.c_store_guid = stores.c;
        hello.c_store_generation = arm.source.c_store_generation;
        hello.f_store_guid = stores.f;
        hello.f_store_generation = armed.f_store_generation;
        hello.c_control_generation = arm.source.c_control_generation;
        hello.c_control_attempt = arm.source.c_control_attempt;
        hello.system_source_fingerprint = profile == ProfileId::P29V1
            ? authority->p29v1_system_source_fingerprint(prepared)
            : icecc::digest128("R2 fragmented one-job fixture");
        hello.history_nonce = prepared_input->begin.history_nonce;
        hello.start_mode = LinkStartMode::Initial;

        JobBind binding;
        binding.reservation_id = Id128{armed.reservation_id};
        binding.physical_link_generation = hello.physical_link_generation;
        binding.relationship_ordinal = 1;
        binding.wire_job_id = arm.source.wire_job_id;
        binding.assignment_epoch = arm.source.assignment_epoch;
        binding.assignment_nonce = arm.source.assignment_nonce;
        binding.logical_job = arm.source.logical_job;
        binding.compiler_attempt = arm.source.compiler_attempt;
        binding.source_request_id = arm.source.source_request_id;
        binding.tu_seq = prepared_input->begin.tu_seq;
        binding.profile = profile;
        binding.raw_bytes = prepared_input->begin.raw_bytes;
        binding.raw_digest = prepared_input->begin.raw_digest;

        P51SourceJobLease job_lease;
        job_lease.armed = armed;
        job_lease.absolute_deadline = job_deadline;
        job_lease.binding = binding;
        job_lease.binding_digest = compute_r2_binding_digest(binding);
        job_lease.input_key = InputRecordKey{stores.c, binding.tu_seq};
        std::atomic<unsigned> link_lookups{0};
        std::atomic<unsigned> job_consumes{0};
        std::atomic<unsigned> commits{0};
        std::atomic<unsigned> acknowledgements{0};
        std::atomic<unsigned> materializations{0};
        std::vector<std::pair<MessageType, size_t>> f_messages;
        P50ServerEndpointConfig server_config;
        server_config.lookup_p51_link_reservation =
            [&](const LinkHello& observed)
                -> std::optional<P51SourceLinkLease> {
                ++link_lookups;
                if (observed != hello)
                    return std::nullopt;
                P51SourceLinkLease lease{armed, job_deadline};
                lease.relationship_epoch = hello.relationship_epoch;
                lease.history_nonce = hello.history_nonce;
                return lease;
            };
        server_config.consume_p51_job_reservation =
            [&](const LinkHello& observed, const JobBind& offered)
                -> std::optional<P51SourceJobLease> {
                if (observed != hello || offered != binding ||
                    job_consumes.fetch_add(1) != 0)
                    return std::nullopt;
                return job_lease;
            };
        server_config.input_job_state =
            [&](CStoreGuid c_guid, const TxBegin&, const TxCommit& commit,
                std::span<const uint8_t> exact) {
                if (c_guid != stores.c || commit.tu_seq != binding.tu_seq ||
                    !std::ranges::equal(exact, input) ||
                    commit.raw_digest != icecc::digest128(input))
                    throw std::runtime_error(
                        "fragmented R2 materialization changed exact input");
                ++materializations;
                return InputJobState::Open;
            };
        server_config.record_p51_job_commit =
            [&](const LinkHello& observed, const JobBind& committed_binding,
                const R2TxCommit& commit) {
                if (observed != hello || committed_binding != binding ||
                    commit.relationship_ordinal != 1 ||
                    commit.binding_digest != job_lease.binding_digest ||
                    commit.inner.tu_seq != binding.tu_seq ||
                    commit.inner.raw_digest != binding.raw_digest)
                    return false;
                ++commits;
                return true;
            };
        server_config.acknowledge_p51_receipt =
            [&](const LinkHello& observed, const CommitAck& ack) {
                if (observed != hello ||
                    ack.relationship_id != hello.relationship_id ||
                    ack.relationship_epoch != hello.relationship_epoch ||
                    ack.physical_link_generation !=
                        hello.physical_link_generation ||
                    ack.contiguous_verified_ordinal != 1)
                    return false;
                ++acknowledgements;
                return true;
            };

        CompletionLog c_completions;
        CompletionLog f_completions;
        P50ServerEndpoint server(stores.f, caps, &f_completions, nullptr,
                                 std::move(server_config));
        P50ClientEndpoint client(authority, caps, hello.history_nonce,
                                 &c_completions, nullptr, std::nullopt, {}, {},
                                 route);
        EndpointIoControl server_control;
        server_control.max_write_fragment = 1;
        server_control.outbound_message_observer =
            [&](ActorSide actor, const Message& message) {
                if (actor == ActorSide::F)
                    f_messages.emplace_back(message_type(message),
                                            encode_frame(message).size());
            };
        EndpointIoControl bundle_control;
        bundle_control.max_write_fragment = 1;
        std::vector<std::pair<MessageType, size_t>> c_messages;
        asio::io_context context;
        tcp::acceptor acceptor(context,
                               {asio::ip::address_v4::loopback(), 0});
        asio::steady_timer watchdog(context);
        bool timed_out = false;
        watchdog.expires_after(std::chrono::seconds(25));
        watchdog.async_wait([&](const boost::system::error_code& error) {
            if (!error) {
                timed_out = true;
                boost::system::error_code ignored;
                acceptor.close(ignored);
                context.stop();
            }
        });
        std::optional<ServerRunResult> server_result;
        std::optional<ClientRunResult> client_result;
        std::exception_ptr server_error;
        std::exception_ptr client_error;
        bool server_done = false;
        bool client_done = false;
        const auto finish_if_done = [&] {
            if (server_done && client_done) {
                boost::system::error_code ignored;
                watchdog.cancel(ignored);
            }
        };
        asio::co_spawn(
            context, r2_accept_one(acceptor, server, server_control),
            [&](std::exception_ptr error, ServerRunResult result) {
                server_error = error;
                if (!error)
                    server_result = std::move(result);
                server_done = true;
                finish_if_done();
            });
        asio::co_spawn(
            context, r2_fragmented_single_job_client(
                         acceptor.local_endpoint(), client, hello, binding,
                         prepared, job_deadline.as_steady_time_point(),
                         bundle_control, c_messages),
            [&](std::exception_ptr error, ClientRunResult result) {
                client_error = error;
                if (!error)
                    client_result = std::move(result);
                client_done = true;
                finish_if_done();
            });
        context.run();
        boost::system::error_code ignored;
        require(!timed_out && server_done && client_done,
                "fragmented R2 one-job dialogue exceeded its watchdog");
        if (server_error)
            std::rethrow_exception(server_error);
        if (client_error)
            std::rethrow_exception(client_error);
        require(server_result->status == ServerRunStatus::Completed &&
                    client_result->status == ClientRunStatus::Committed &&
                    link_lookups == 1 && job_consumes == 1 && commits == 1 &&
                    acknowledgements == 1 && materializations == 1,
                "fragmented R2 one-job dialogue did not commit exactly once");

        std::vector<MessageType> client_types;
        for (const auto& [type, size] : c_messages) {
            (void)size;
            client_types.push_back(type);
        }
        require(client_types.size() >= 4 &&
                    client_types.front() == MessageType::JOB_BIND &&
                    client_types[1] == MessageType::TU_BEGIN &&
                    client_types.back() == MessageType::TU_END &&
                    std::ranges::find(client_types, MessageType::R2_BODY) !=
                        client_types.end(),
                "fragmented R2 C bundle omitted a mandatory record");
        const size_t fill_records = static_cast<size_t>(std::ranges::count(
            client_types, MessageType::R2_FILL));
        require((profile == ProfileId::P29V1) == (fill_records != 0),
                "fragmented R2 profile emitted an unexpected FILL sequence");
        std::vector<MessageType> server_types;
        for (const auto& [type, size] : f_messages) {
            (void)size;
            server_types.push_back(type);
        }
        require(server_types.size() == 2 &&
                    server_types[0] == MessageType::LINK_STATE &&
                    server_types[1] == MessageType::R2_TX_COMMIT,
                "fragmented R2 F output did not include exact STATE and COMMIT");
        uint64_t c_fragment_bytes = 0;
        for (const AsyncCompletion& completion : c_completions.completions()) {
            if (completion.stamp.actor == ActorSide::C &&
                completion.stamp.operation == AsyncOperationKind::WriteFragment &&
                completion.stamp.transaction_bound) {
                require(completion.transferred_bytes == 1,
                        "R2 bundle fragment completion exceeded one byte");
                c_fragment_bytes += completion.transferred_bytes;
            }
        }
        const uint64_t expected_c_bytes = std::accumulate(
            c_messages.begin(), c_messages.end(), uint64_t{0},
            [](uint64_t total, const auto& item) { return total + item.second; });
        require(c_fragment_bytes == expected_c_bytes,
                "R2 C transaction records were not all written bytewise");
        uint64_t f_commit_fragment_bytes = 0;
        for (const AsyncCompletion& completion : f_completions.completions()) {
            if (completion.stamp.actor == ActorSide::F &&
                completion.stamp.operation == AsyncOperationKind::WriteFragment &&
                completion.stamp.transaction_bound) {
                require(completion.transferred_bytes == 1,
                        "R2 COMMIT fragment completion exceeded one byte");
                f_commit_fragment_bytes += completion.transferred_bytes;
            }
        }
        require(f_commit_fragment_bytes == f_messages.back().second,
                "R2 F TX_COMMIT was not completely written bytewise");
        InputCursor cursor = server.attach_input(
            InputRecordKey{stores.c, binding.tu_seq});
        std::vector<uint8_t> recovered(input.size());
        require(cursor && cursor.read(recovered) == recovered.size() &&
                    recovered == input,
                "fragmented R2 exact input attachment changed bytes");
        std::cout << "P51_R2_FRAGMENTED profile="
                  << static_cast<unsigned>(profile)
                  << " C-records=JOB_BIND,TU_BEGIN,R2_BODY"
                  << (fill_records ? ",R2_FILL" : "")
                  << ",TU_END F-records=LINK_STATE,R2_TX_COMMIT"
                  << " one-byte-write-completions: ok\n";
    }
}

struct InjectedR2BodyFragmentCut {};

asio::awaitable<void> r2_proxy_reverse_frames(
    std::shared_ptr<tcp::socket> f_socket,
    std::shared_ptr<tcp::socket> c_socket) {
    for (;;) {
        std::array<uint8_t, 4> header_bytes{};
        boost::system::error_code error;
        (void)co_await asio::async_read(
            *f_socket, asio::buffer(header_bytes),
            asio::redirect_error(asio::use_awaitable, error));
        if (error)
            break;
        const FrameHeader header =
            decode_frame_header(header_bytes, kInitialMaxFramePayload);
        std::vector<uint8_t> payload(header.payload_bytes);
        if (!payload.empty()) {
            (void)co_await asio::async_read(
                *f_socket, asio::buffer(payload),
                asio::redirect_error(asio::use_awaitable, error));
            if (error)
                break;
        }
        (void)co_await asio::async_write(
            *c_socket, asio::buffer(header_bytes),
            asio::redirect_error(asio::use_awaitable, error));
        if (error)
            break;
        if (!payload.empty()) {
            (void)co_await asio::async_write(
                *c_socket, asio::buffer(payload),
                asio::redirect_error(asio::use_awaitable, error));
            if (error)
                break;
        }
    }
}

struct R2FrameCutSpec {
    MessageType type;
    size_t occurrence;
    size_t frame_prefix_bytes;
    std::string_view label;
};

struct R2ProxyCutResult {
    MessageType cut_frame_type{};
    size_t cut_prefix_bytes = 0;
    size_t cut_frame_bytes = 0;
};

const char* r2_cut_message_name(MessageType type) {
    switch (type) {
    case MessageType::JOB_BIND: return "JOB_BIND";
    case MessageType::TU_BEGIN: return "TU_BEGIN";
    case MessageType::R2_BODY: return "R2_BODY";
    case MessageType::R2_FILL: return "R2_FILL";
    case MessageType::TU_END: return "TU_END";
    default: return "unexpected";
    }
}

asio::awaitable<R2ProxyCutResult> r2_proxy_cut_frame(
    tcp::acceptor& proxy_acceptor, tcp::endpoint f_endpoint,
    R2FrameCutSpec cut, std::vector<MessageType>& observed_frames,
    std::vector<MessageType>& fully_forwarded_frames) {
    const auto executor = co_await asio::this_coro::executor;
    auto c_socket = std::make_shared<tcp::socket>(executor);
    co_await proxy_acceptor.async_accept(*c_socket, asio::use_awaitable);
    auto f_socket = std::make_shared<tcp::socket>(executor);
    co_await f_socket->async_connect(f_endpoint, asio::use_awaitable);
    asio::co_spawn(executor, r2_proxy_reverse_frames(f_socket, c_socket),
                   [](std::exception_ptr) {});

    R2ProxyCutResult result;
    size_t matching_occurrences = 0;
    for (;;) {
        std::array<uint8_t, 4> header_bytes{};
        co_await asio::async_read(*c_socket, asio::buffer(header_bytes),
                                  asio::use_awaitable);
        const FrameHeader header =
            decode_frame_header(header_bytes, kInitialMaxFramePayload);
        observed_frames.push_back(header.type);
        const bool target = header.type == cut.type &&
                            matching_occurrences++ == cut.occurrence;
        if (target) {
            const size_t frame_bytes = header_bytes.size() +
                                       header.payload_bytes;
            if (cut.frame_prefix_bytes == 0 ||
                cut.frame_prefix_bytes >= frame_bytes)
                throw std::logic_error(
                    "R2 proxy cut must stop inside the selected frame");
            const size_t header_prefix =
                std::min(cut.frame_prefix_bytes, header_bytes.size());
            co_await asio::async_write(
                *f_socket,
                asio::buffer(header_bytes.data(), header_prefix),
                asio::use_awaitable);
            if (cut.frame_prefix_bytes > header_bytes.size()) {
                const size_t payload_prefix =
                    cut.frame_prefix_bytes - header_bytes.size();
                std::vector<uint8_t> prefix(payload_prefix);
                co_await asio::async_read(*c_socket, asio::buffer(prefix),
                                          asio::use_awaitable);
                co_await asio::async_write(*f_socket, asio::buffer(prefix),
                                           asio::use_awaitable);
            }
            result.cut_frame_type = header.type;
            result.cut_prefix_bytes = cut.frame_prefix_bytes;
            result.cut_frame_bytes = frame_bytes;
            break;
        }
        co_await asio::async_write(*f_socket, asio::buffer(header_bytes),
                                   asio::use_awaitable);
        std::vector<uint8_t> payload(header.payload_bytes);
        if (!payload.empty()) {
            co_await asio::async_read(*c_socket, asio::buffer(payload),
                                      asio::use_awaitable);
            co_await asio::async_write(*f_socket, asio::buffer(payload),
                                       asio::use_awaitable);
        }
        fully_forwarded_frames.push_back(header.type);
    }
    boost::system::error_code ignored;
    c_socket->shutdown(tcp::socket::shutdown_both, ignored);
    c_socket->close(ignored);
    f_socket->shutdown(tcp::socket::shutdown_both, ignored);
    f_socket->close(ignored);
    co_return result;
}

asio::awaitable<ClientRunResult> r2_interrupt_frame_then_recover(
    tcp::endpoint proxy, tcp::endpoint remote, P50ClientEndpoint& client,
    P50PreparationAuthority& authority, PreparationRouteKey route,
    const LinkHello& initial_hello,
    const JobBind& initial_binding, PreparedTuHandle prepared,
    R2FrameCutSpec cut, std::chrono::steady_clock::time_point deadline,
    ServerRunResult& first_server_result, bool& first_server_done,
    unsigned& binding_consumptions,
    std::atomic<unsigned>& materializations,
    std::atomic<unsigned>& commits) {
    tcp::socket interrupted_socket(co_await asio::this_coro::executor);
    co_await interrupted_socket.async_connect(proxy, asio::use_awaitable);
    (void)co_await client.open_r2_link(interrupted_socket, initial_hello,
                                      deadline);
    bool writer_observed_disconnect = false;
    try {
        const R2SentBundle sent = co_await client.write_r2_bundle(
            interrupted_socket, initial_binding, prepared, deadline);
        try {
            (void)co_await client.read_r2_receipt(
                interrupted_socket, sent, deadline);
            throw std::logic_error(
                "F returned a receipt for an intentionally partial R2 frame");
        } catch (const boost::system::system_error&) {
            writer_observed_disconnect = true;
        }
    } catch (const boost::system::system_error&) {
        writer_observed_disconnect = true;
    }
    boost::system::error_code ignored;
    interrupted_socket.shutdown(tcp::socket::shutdown_both, ignored);
    interrupted_socket.close(ignored);
    require(writer_observed_disconnect,
            "R2 sender did not observe the proxy frame interruption");

    const auto executor = co_await asio::this_coro::executor;
    asio::steady_timer first_result_wait(executor);
    for (size_t attempt = 0; attempt != 2000 && !first_server_done; ++attempt) {
        first_result_wait.expires_after(std::chrono::milliseconds(1));
        co_await first_result_wait.async_wait(asio::use_awaitable);
    }
    require(first_server_done &&
                first_server_result.status == ServerRunStatus::Disconnected &&
                materializations.load(std::memory_order_acquire) == 0 &&
                commits.load(std::memory_order_acquire) == 0,
            "partial R2 frame became visible or failed to disconnect cleanly");
    const unsigned expected_before_replay =
        cut.type == MessageType::JOB_BIND ? 0 : 1;
    require(binding_consumptions == expected_before_replay,
            "partial R2 frame consumed the reservation at the wrong boundary");

    std::vector<R2SentBundle> witnesses = client.r2_pending_witnesses(0);
    require(witnesses.size() == 1 &&
                witnesses.front().binding == initial_binding &&
                witnesses.front().prepared == prepared,
            "partial R2 frame did not retain one exact recovery witness");
    LinkHello reconnect = initial_hello;
    reconnect.start_mode = LinkStartMode::Reconnect;
    reconnect.physical_link_generation =
        initial_hello.physical_link_generation + 1;
    reconnect.verified_receipt_floor = 0;
    const uint64_t cut_identity =
        static_cast<uint64_t>(cut.type) * 100000 +
        cut.occurrence * 10000 + cut.frame_prefix_bytes;
    const Id128 reset_operation = Id128::from_u64(
        0xd0300000ULL + static_cast<uint64_t>(initial_hello.profile) * 1000000 +
        cut_identity);
    const HistoryNonce replacement_nonce{
        0xd0400000ULL + static_cast<uint64_t>(initial_hello.profile) * 1000000 +
        cut_identity};
    tcp::socket recovery_socket(executor);
    co_await recovery_socket.async_connect(remote, asio::use_awaitable);
    const RecoverBegin expected_recovery_begin{
        reconnect.relationship_id, reconnect.relationship_epoch,
        reconnect.physical_link_generation, reset_operation, 0, 1, 1};
    std::vector<RecoverWitness> expected_recovery_witnesses;
    expected_recovery_witnesses.reserve(witnesses.size());
    for (const R2SentBundle& sent : witnesses) {
        RecoverWitness witness;
        witness.relationship_id = reconnect.relationship_id;
        witness.relationship_epoch = reconnect.relationship_epoch;
        witness.physical_link_generation =
            reconnect.physical_link_generation;
        witness.operation_id = reset_operation;
        witness.relationship_ordinal = sent.binding.relationship_ordinal;
        witness.binding_digest = sent.binding_digest;
        witness.transaction_digest = sent.transaction_digest;
        witness.binding = sent.binding;
        witness.inner = sent.begin.inner;
        expected_recovery_witnesses.push_back(std::move(witness));
    }
    const Digest128 expected_recovery_witness_digest =
        compute_r2_recovery_witness_digest(
            expected_recovery_begin, expected_recovery_witnesses);
    const R2RecoveryResult recovered = co_await client.recover_r2_link(
        recovery_socket, reconnect, witnesses, 0, reset_operation,
        initial_hello.relationship_epoch + 1, replacement_nonce, deadline);
    require(recovered.committed_receipts.empty() &&
                recovered.reset_request.settled_prefix_k == 0 &&
                recovered.reset_request.operation_id == reset_operation &&
                recovered.reset_request.new_history_nonce == replacement_nonce &&
                recovered.reset_ack.request == recovered.reset_request &&
                recovered.reset_ack.initial_state_digest ==
                    recovered.link_state.state_digest &&
                recovered.reset_ack.next_rel_seq.value == 0 &&
                recovered.reset_ack.recovery_verified_floor_a == 0 &&
                recovered.reset_ack.recovery_prepared_prefix_p == 1 &&
                recovered.reset_ack.recovery_witness_digest ==
                    expected_recovery_witness_digest &&
                recovered.reset_ack.unavailable_suffix_mask == 0,
            "partial R2 frame recovery did not settle the exact empty prefix");

    authority.reset_r2_route_for_recovery(
        route, recovered.link_state.f_store_guid, replacement_nonce);
    authority.rebuild_r2_entry_for_recovery(
        prepared, recovered.link_state.f_system_source_fingerprint);
    JobBind replay_binding = initial_binding;
    replay_binding.physical_link_generation =
        reconnect.physical_link_generation;
    const R2SentBundle replayed = co_await client.write_r2_bundle(
        recovery_socket, replay_binding, prepared, deadline,
        EndpointIoControl{.max_write_fragment = 1});
    ClientRunResult receipt = co_await client.read_r2_receipt(
        recovery_socket, replayed, deadline);
    require(receipt.status == ClientRunStatus::Committed &&
                receipt.committed_input.has_value() &&
                receipt.committed_input->tu_seq == initial_binding.tu_seq &&
                receipt.committed_commit &&
                receipt.committed_commit->raw_digest ==
                    initial_binding.raw_digest,
            "R2 frame recovery did not return the exact source commit");
    co_await client.write_r2_ack(recovery_socket, 1, deadline);
    co_await raw_write(recovery_socket, Message{CloseMessage{}});
    recovery_socket.shutdown(tcp::socket::shutdown_both, ignored);
    recovery_socket.close(ignored);
    require(materializations.load(std::memory_order_acquire) == 1 &&
                commits.load(std::memory_order_acquire) == 1,
            "R2 frame recovery materialized or committed more than once");
    co_return receipt;
}

asio::awaitable<void> r2_two_accepts(
    tcp::acceptor& acceptor, P50ServerEndpoint& server,
    ServerRunResult& first, ServerRunResult& second, bool& first_done) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket first_socket(executor);
    co_await acceptor.async_accept(first_socket, asio::use_awaitable);
    first = co_await server.run_adopted_r2(std::move(first_socket));
    first_done = true;
    tcp::socket second_socket(executor);
    co_await acceptor.async_accept(second_socket, asio::use_awaitable);
    second = co_await server.run_adopted_r2(std::move(second_socket));
}

void test_r2_fragmented_frame_interruption_recovery() {
    for (const ProfileId profile : {ProfileId::ZSTD_TU, ProfileId::P29V1,
                                    ProfileId::ZSTD_ROUTE}) {
        const size_t case_count = profile == ProfileId::P29V1 ? 9 : 8;
        for (size_t case_index = 0; case_index != case_count; ++case_index) {
            const uint64_t case_number = case_index + 1;
            const P5coStoreGuids stores = p5co_store_guids(
                0x7d + static_cast<uint64_t>(profile) * 32 + case_number);
            EndpointCaps caps;
            caps.profile = profile;
            caps.supported_profiles = profile_bit(profile);
            PreparationAuthorityLimits limits;
            limits.max_speculative_tus = 1;
            limits.max_speculative_raw_bytes = 1U << 20;
            auto authority = std::make_shared<P50PreparationAuthority>(
                stores.c, caps.zstd, limits, 1, profile);
            const PreparationRouteKey route{stores.f, 23, profile};
            const std::vector<uint8_t> input = pseudo_random_bytes(32768);
            const PreparedTuHandle prepared = authority->prepare_for_route(
                route, PrepareRequestKey{6200 + case_number,
                                         7200 + case_number}, input);
            const PreparedInputPtr retained =
                P50PreparationAuthorityTestAccess::resolve(*authority, prepared);
            const size_t encoded_body_bytes = retained->body.size();
            std::vector<R2FrameCutSpec> cut_cases{
                {MessageType::JOB_BIND, 0, 5, "JOB_BIND payload"},
                {MessageType::TU_BEGIN, 0, 5, "TU_BEGIN payload"},
                {MessageType::R2_BODY, 0, 1, "R2_BODY header byte 1"},
                {MessageType::R2_BODY, 0, 2, "R2_BODY header byte 2"},
                {MessageType::R2_BODY, 0, 3, "R2_BODY header byte 3"},
                {MessageType::R2_BODY, 0,
                 4 + std::max<size_t>(1, encoded_body_bytes / 8),
                 "R2_BODY early payload"},
                {MessageType::R2_BODY, 0,
                 4 + std::max<size_t>(1, encoded_body_bytes / 2),
                 "R2_BODY middle payload"},
                {MessageType::TU_END, 0, 5, "TU_END payload"},
            };
            if (profile == ProfileId::P29V1)
                cut_cases.push_back(
                    {MessageType::R2_FILL, 0, 5, "R2_FILL payload"});
            require(cut_cases.size() == case_count &&
                        case_index < cut_cases.size(),
                    "R2 frame cut case count diverged from its plan");
            const R2FrameCutSpec cut = cut_cases[case_index];
            const sidecar::AbsoluteMonotonicDeadline job_deadline =
                r2_test_deadline(std::chrono::seconds(20));
            const P51SourceArmFields arm{
                r2_test_arm(stores.c, 6200 + case_number,
                            static_cast<uint32_t>(7200 + case_number), profile),
                1};
            const P51SourceArmedFields armed = r2_test_armed(
                arm, stores.f, 0xd050 + static_cast<uint64_t>(profile) * 10 +
                                  case_number);

            LinkHello hello;
            hello.profile = profile;
            hello.window = 1;
            hello.max_frame_payload = kInitialMaxFramePayload;
            hello.max_raw_bytes = 1U << 20;
            hello.max_encoded_bytes = 1U << 20;
            hello.max_output_bytes = 1U << 20;
            hello.reservation_id = Id128{armed.reservation_id};
            hello.relationship_id = Id128{armed.logical_relationship_id};
            hello.relationship_epoch = armed.relationship_epoch;
            hello.physical_link_generation = 41;
            hello.c_store_guid = stores.c;
            hello.c_store_generation = arm.source.c_store_generation;
            hello.f_store_guid = stores.f;
            hello.f_store_generation = armed.f_store_generation;
            hello.c_control_generation = arm.source.c_control_generation;
            hello.c_control_attempt = arm.source.c_control_attempt;
            hello.system_source_fingerprint = profile == ProfileId::P29V1
                ? authority->p29v1_system_source_fingerprint(prepared)
                : icecc::digest128("R2 fragmented BODY recovery");
            hello.history_nonce = retained->begin.history_nonce;
            hello.start_mode = LinkStartMode::Initial;

            JobBind binding;
            binding.reservation_id = hello.reservation_id;
            binding.physical_link_generation = hello.physical_link_generation;
            binding.relationship_ordinal = 1;
            binding.wire_job_id = arm.source.wire_job_id;
            binding.assignment_epoch = arm.source.assignment_epoch;
            binding.assignment_nonce = arm.source.assignment_nonce;
            binding.logical_job = arm.source.logical_job;
            binding.compiler_attempt = arm.source.compiler_attempt;
            binding.source_request_id = arm.source.source_request_id;
            binding.tu_seq = retained->begin.tu_seq;
            binding.profile = profile;
            binding.raw_bytes = retained->begin.raw_bytes;
            binding.raw_digest = retained->begin.raw_digest;
            const Digest128 binding_digest = compute_r2_binding_digest(binding);

            std::atomic<unsigned> materializations{0};
            std::atomic<unsigned> commit_count{0};
            std::atomic<unsigned> ack_count{0};
            unsigned bind_count = 0;
            bool reset_committed = false;
            std::optional<ResetRequest> retained_reset;
            uint64_t recovery_floor_a = 0;
            uint64_t recovery_prefix_p = 0;
            Digest128 recovery_witness_digest{};
            P50ServerEndpointConfig config;
            config.lookup_p51_link_reservation =
                [&, job_deadline](const LinkHello& observed)
                    -> P51SourceLinkLookupResult {
                if (observed.reservation_id != hello.reservation_id ||
                    observed.relationship_id != hello.relationship_id ||
                    observed.c_store_guid != hello.c_store_guid ||
                    observed.f_store_guid != hello.f_store_guid ||
                    observed.physical_link_generation <
                        hello.physical_link_generation)
                    return {P51SourceLinkLookupStatus::Invalid, std::nullopt};
                P51SourceLinkLease lease{armed, job_deadline};
                lease.reconnect =
                    observed.start_mode == LinkStartMode::Reconnect;
                lease.relationship_epoch = hello.relationship_epoch;
                lease.history_nonce = hello.history_nonce;
                lease.committed_prefix_k = 0;
                lease.acknowledged_prefix_q = 0;
                return {P51SourceLinkLookupStatus::Found, std::move(lease)};
            };
            config.consume_p51_job_reservation =
                [&, job_deadline](const LinkHello& observed,
                                  const JobBind& offered)
                    -> std::optional<P51SourceJobLease> {
                if (offered.reservation_id != binding.reservation_id ||
                    offered.wire_job_id != binding.wire_job_id ||
                    offered.assignment_nonce != binding.assignment_nonce ||
                    offered.source_request_id != binding.source_request_id ||
                    offered.relationship_ordinal != 1 ||
                    offered.tu_seq != binding.tu_seq ||
                    offered.raw_digest != binding.raw_digest ||
                    offered.profile != profile ||
                    offered.physical_link_generation !=
                        observed.physical_link_generation ||
                    ++bind_count > 2)
                    return std::nullopt;
                P51SourceJobLease lease;
                lease.armed = armed;
                lease.armed.relationship_epoch = observed.relationship_epoch;
                lease.absolute_deadline = job_deadline;
                lease.binding = offered;
                lease.binding_digest = compute_r2_binding_digest(offered);
                lease.input_key = InputRecordKey{stores.c, offered.tu_seq};
                return lease;
            };
            config.input_job_state =
                [&, input](CStoreGuid c_guid, const TxBegin&,
                           const TxCommit& commit,
                           std::span<const uint8_t> bytes) {
                if (c_guid != stores.c || commit.tu_seq != binding.tu_seq ||
                    !std::ranges::equal(bytes, input) ||
                    commit.raw_digest != icecc::digest128(input))
                    throw std::runtime_error(
                        "recovered R2 BODY materialized different raw input");
                ++materializations;
                return InputJobState::Open;
            };
            config.settle_p51_interrupted_job =
                [](const LinkHello& observed) {
                    return observed.start_mode == LinkStartMode::Reconnect;
                };
            config.recover_p51_receipts =
                [&](const LinkHello& observed, const RecoverBegin& begin,
                    std::span<const RecoverWitness> witnesses,
                    const RecoverEnd& end)
                    -> std::optional<P51RecoveryReceiptInterval> {
                if (observed.start_mode != LinkStartMode::Reconnect ||
                    begin.relationship_id != hello.relationship_id ||
                    begin.relationship_epoch != hello.relationship_epoch ||
                    begin.physical_link_generation !=
                        observed.physical_link_generation ||
                    begin.verified_floor_a != 0 || begin.prepared_prefix_p != 1 ||
                    begin.witness_count != 1 || witnesses.size() != 1 ||
                    end.witness_count != 1 ||
                    witnesses.front().relationship_ordinal != 1 ||
                    witnesses.front().binding != binding ||
                    witnesses.front().binding_digest != binding_digest ||
                    witnesses.front().inner.raw_digest != binding.raw_digest ||
                    end.operation_id != begin.operation_id)
                    return std::nullopt;
                recovery_floor_a = begin.verified_floor_a;
                recovery_prefix_p = begin.prepared_prefix_p;
                recovery_witness_digest =
                    compute_r2_recovery_witness_digest(begin, witnesses);
                P51RecoveryReceiptInterval interval;
                interval.end.relationship_id = begin.relationship_id;
                interval.end.relationship_epoch = begin.relationship_epoch;
                interval.end.physical_link_generation =
                    begin.physical_link_generation;
                interval.end.operation_id = begin.operation_id;
                interval.end.verified_floor_a = 0;
                interval.end.committed_prefix_k = 0;
                interval.end.acknowledged_prefix_q = 0;
                interval.end.receipt_count = 0;
                return interval;
            };
            config.validate_p51_reset =
                [&](const LinkHello& observed, const ResetRequest& request)
                    -> std::optional<ResetAck> {
                if (observed.start_mode != LinkStartMode::Reconnect ||
                    request.relationship_id != hello.relationship_id ||
                    request.old_relationship_epoch != hello.relationship_epoch ||
                    request.new_relationship_epoch !=
                        hello.relationship_epoch + 1 ||
                    request.physical_link_generation !=
                        observed.physical_link_generation ||
                    request.settled_prefix_k != 0 ||
                    request.old_history_nonce != hello.history_nonce ||
                    request.operation_id == Id128{} ||
                    (retained_reset && *retained_reset != request))
                    return std::nullopt;
                ResetAck ack{
                    request,
                    initial_route_digest(stores.c, request.new_history_nonce),
                    RelSeq{}};
                ack.recovery_verified_floor_a = recovery_floor_a;
                ack.recovery_prepared_prefix_p = recovery_prefix_p;
                ack.recovery_witness_digest = recovery_witness_digest;
                ack.unavailable_suffix_mask = 0;
                return ack;
            };
            config.commit_p51_reset =
                [&](const LinkHello&, const ResetRequest& request,
                    const ResetAck&) {
                retained_reset = request;
                reset_committed = true;
                return true;
            };
            config.confirm_p51_reset =
                [&](const LinkHello& observed, const ResetConfirm& confirm) {
                return reset_committed &&
                       observed.start_mode == LinkStartMode::Reconnect &&
                       retained_reset &&
                       confirm.relationship_id == hello.relationship_id &&
                       confirm.new_relationship_epoch ==
                           retained_reset->new_relationship_epoch &&
                       confirm.physical_link_generation ==
                           observed.physical_link_generation &&
                       confirm.operation_id == retained_reset->operation_id &&
                       confirm.new_history_nonce ==
                           retained_reset->new_history_nonce &&
                       confirm.settled_prefix_k == 0;
            };
            config.record_p51_job_commit =
                [&](const LinkHello& observed, const JobBind& offered,
                    const R2TxCommit& commit) {
                if (offered.relationship_ordinal != 1 ||
                    offered.physical_link_generation !=
                        observed.physical_link_generation ||
                    commit.relationship_ordinal != 1 ||
                    commit.binding_digest !=
                        compute_r2_binding_digest(offered) ||
                    commit.inner.tu_seq != binding.tu_seq ||
                    commit.inner.raw_digest != binding.raw_digest)
                    return false;
                ++commit_count;
                return true;
            };
            config.acknowledge_p51_receipt =
                [&](const LinkHello& observed, const CommitAck& ack) {
                if (ack.relationship_id != hello.relationship_id ||
                    ack.relationship_epoch != observed.relationship_epoch ||
                    ack.physical_link_generation !=
                        observed.physical_link_generation ||
                    ack.contiguous_verified_ordinal != 1)
                    return false;
                ++ack_count;
                return true;
            };

            P50ServerEndpoint server(stores.f, caps, nullptr, nullptr,
                                     std::move(config));
            P50ClientEndpoint client(authority, caps,
                                     retained->begin.history_nonce, nullptr,
                                     nullptr, std::nullopt, {}, {}, route);
            asio::io_context context;
            tcp::acceptor acceptor(
                context, {asio::ip::address_v4::loopback(), 0});
            tcp::acceptor proxy_acceptor(
                context, {asio::ip::address_v4::loopback(), 0});
            ServerRunResult first_result;
            ServerRunResult second_result;
            bool first_done = false;
            bool server_done = false;
            bool client_done = false;
            bool proxy_done = false;
            bool timed_out = false;
            std::optional<ClientRunResult> client_result;
            std::exception_ptr server_error;
            std::exception_ptr client_error;
            std::exception_ptr proxy_error;
            R2ProxyCutResult proxy_cut_result;
            std::vector<MessageType> proxy_observed_frames;
            std::vector<MessageType> proxy_fully_forwarded_frames;
            asio::steady_timer watchdog(context);
            watchdog.expires_after(std::chrono::seconds(25));
            watchdog.async_wait([&](const boost::system::error_code& error) {
                if (!error) {
                    timed_out = true;
                    boost::system::error_code ignored;
                    acceptor.close(ignored);
                    proxy_acceptor.close(ignored);
                    context.stop();
                }
            });
            const auto cancel_watchdog_if_done = [&] {
                if (server_done && client_done && proxy_done) {
                    boost::system::error_code ignored;
                    watchdog.cancel(ignored);
                }
            };
            asio::co_spawn(
                context, r2_two_accepts(acceptor, server, first_result,
                                        second_result, first_done),
                [&](std::exception_ptr error) {
                    server_error = error;
                    server_done = true;
                    cancel_watchdog_if_done();
                });
            asio::co_spawn(
                context,
                r2_proxy_cut_frame(proxy_acceptor,
                                   acceptor.local_endpoint(), cut,
                                   proxy_observed_frames,
                                   proxy_fully_forwarded_frames),
                [&](std::exception_ptr error, R2ProxyCutResult result) {
                    proxy_error = error;
                    if (!error)
                        proxy_cut_result = std::move(result);
                    proxy_done = true;
                    cancel_watchdog_if_done();
                });
            const auto started = std::chrono::steady_clock::now();
            asio::co_spawn(
                context,
                r2_interrupt_frame_then_recover(
                    proxy_acceptor.local_endpoint(), acceptor.local_endpoint(),
                    client, *authority, route, hello, binding, prepared,
                    cut,
                    job_deadline.as_steady_time_point(), first_result,
                    first_done, bind_count, materializations, commit_count),
                [&](std::exception_ptr error, ClientRunResult result) {
                    client_error = error;
                    if (!error)
                        client_result = std::move(result);
                    client_done = true;
                    cancel_watchdog_if_done();
                });
            context.run();
            require(!timed_out && server_done && client_done && proxy_done,
                    "fragmented R2 frame recovery exceeded the watchdog");
            if (server_error)
                std::rethrow_exception(server_error);
            if (client_error)
                std::rethrow_exception(client_error);
            if (proxy_error)
                std::rethrow_exception(proxy_error);
            std::vector<MessageType> expected_completed{
                MessageType::LINK_HELLO};
            switch (cut.type) {
            case MessageType::JOB_BIND:
                break;
            case MessageType::TU_BEGIN:
                expected_completed.push_back(MessageType::JOB_BIND);
                break;
            case MessageType::R2_BODY:
                expected_completed.push_back(MessageType::JOB_BIND);
                expected_completed.push_back(MessageType::TU_BEGIN);
                break;
            case MessageType::R2_FILL:
            case MessageType::TU_END:
                expected_completed.push_back(MessageType::JOB_BIND);
                expected_completed.push_back(MessageType::TU_BEGIN);
                expected_completed.push_back(MessageType::R2_BODY);
                if (cut.type == MessageType::TU_END &&
                    profile == ProfileId::P29V1)
                    expected_completed.push_back(MessageType::R2_FILL);
                break;
            default:
                throw std::logic_error("unexpected R2 frame cut target");
            }
            std::vector<MessageType> expected_observed = expected_completed;
            expected_observed.push_back(cut.type);
            const bool expected_prefix =
                proxy_fully_forwarded_frames == expected_completed &&
                proxy_observed_frames == expected_observed &&
                std::ranges::find(proxy_fully_forwarded_frames,
                                  MessageType::TU_END) ==
                    proxy_fully_forwarded_frames.end();
            const unsigned expected_bind_count =
                cut.type == MessageType::JOB_BIND ? 1 : 2;
            if (first_result.status != ServerRunStatus::Disconnected ||
                second_result.status != ServerRunStatus::Completed ||
                proxy_cut_result.cut_frame_type != cut.type ||
                proxy_cut_result.cut_prefix_bytes != cut.frame_prefix_bytes ||
                proxy_cut_result.cut_frame_bytes <= cut.frame_prefix_bytes ||
                !expected_prefix || !client_result ||
                client_result->status != ClientRunStatus::Committed ||
                !retained_reset || bind_count != expected_bind_count ||
                materializations != 1 || commit_count != 1 || ack_count != 1) {
                std::cerr << "FRAME_RECOVERY_DIAG profile="
                          << static_cast<unsigned>(profile)
                          << " case=" << case_index
                          << " target=" << r2_cut_message_name(cut.type)
                          << " label=" << cut.label
                          << " first=" << static_cast<unsigned>(first_result.status)
                          << " second=" << static_cast<unsigned>(second_result.status)
                          << " cut=" << proxy_cut_result.cut_prefix_bytes
                          << '/' << proxy_cut_result.cut_frame_bytes
                          << " forwarded=";
                for (const MessageType type : proxy_observed_frames)
                    std::cerr << static_cast<unsigned>(type) << ',';
                std::cerr << " client="
                          << (client_result
                                  ? static_cast<unsigned>(client_result->status)
                                  : 999U)
                          << " reset=" << static_cast<bool>(retained_reset)
                          << " binds=" << bind_count
                          << '/' << expected_bind_count
                          << " materializations=" << materializations
                          << " commits=" << commit_count
                          << " acks=" << ack_count
                          << " expected_prefix=" << expected_prefix << '\n';
            }
            require(first_result.status == ServerRunStatus::Disconnected &&
                        second_result.status == ServerRunStatus::Completed &&
                        proxy_cut_result.cut_frame_type == cut.type &&
                        proxy_cut_result.cut_prefix_bytes ==
                            cut.frame_prefix_bytes &&
                        proxy_cut_result.cut_frame_bytes >
                            cut.frame_prefix_bytes &&
                        expected_prefix &&
                        client_result &&
                        client_result->status == ClientRunStatus::Committed &&
                        retained_reset && bind_count == expected_bind_count &&
                        materializations == 1 && commit_count == 1 &&
                        ack_count == 1 &&
                        std::chrono::steady_clock::now() - started <
                            std::chrono::seconds(20),
                    "fragmented R2 frame interruption did not recover exactly once");
            InputCursor cursor = server.attach_input(
                InputRecordKey{stores.c, binding.tu_seq});
            std::vector<uint8_t> attached(input.size());
            require(cursor && cursor.read(attached) == attached.size() &&
                        attached == input,
                    "fragmented R2 recovery changed attached raw bytes");
            std::cout << "P51_R2_FRAME_RECOVERY profile="
                      << static_cast<unsigned>(profile)
                      << " target=" << r2_cut_message_name(cut.type)
                      << " label=" << cut.label
                      << " case=" << case_index
                      << " frame-prefix-bytes="
                      << proxy_cut_result.cut_prefix_bytes << '/'
                      << proxy_cut_result.cut_frame_bytes
                      << " fwd-complete="
                      << proxy_fully_forwarded_frames.size()
                      << " bind-consumes=" << bind_count
                      << " exact-replay-once: ok\n";
        }
    }
}

void test_r2_silent_setup_cancelled_before_hello() {
    StoreIdentityRoot root{};
    root.bytes[15] = 0x2d;
    SidecarLaunchIdentity launch;
    launch.identity = {71, 1};
    launch.store_generation = 23;
    launch.store_root = root;
    launch.c_store_guid = c_store_guid_for_root(root);
    launch.f_store_guid = f_store_guid_for_root(root);
    require(launch.valid(), "silent R2 setup fixture has invalid launch identity");

    P50ServerEndpointConfig config;
    config.sidecar_launch = launch;
    P50ServerEndpoint endpoint(launch.f_store_guid, {}, nullptr, nullptr,
                               std::move(config));
    size_t hook_calls = 0;
    size_t cancelled_setups = 0;
    EndpointIoControl control;
    control.after_r2_setup_registered = [&] {
        ++hook_calls;
        cancelled_setups = endpoint.cancel_all_for_incarnation(launch);
    };

    asio::io_context context;
    tcp::acceptor acceptor(context,
                           {asio::ip::address_v4::loopback(), 0});
    auto accept_future = asio::co_spawn(
        context, r2_accept_one(acceptor, endpoint, std::move(control)),
        asio::use_future);
    auto peer_future = asio::co_spawn(
        context, r2_silent_peer(acceptor.local_endpoint()), asio::use_future);
    const auto started = std::chrono::steady_clock::now();
    context.run();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const ServerRunResult server_result = accept_future.get();
    const bool peer_saw_eof = peer_future.get();
    require(hook_calls == 1 && cancelled_setups == 1 && peer_saw_eof &&
                server_result.status == ServerRunStatus::Disconnected &&
                elapsed < std::chrono::seconds(1),
            "silent pre-HELLO R2 setup was not promptly cancelled by its exact F incarnation");
    std::puts("P51_R2_ENDPOINT silent-pre-HELLO exact-incarnation-cancel: ok");
}

void test_candidate_stage_has_no_revision_residue() {
    P50ServerEndpoint server(Id128::from_u64(154));
    for (uint64_t index = 1; index != 17; ++index) {
        TestClient candidate(Id128::from_u64(154 + index));
        EndpointIoControl stop_after_hello;
        stop_after_hello.close_after_write = MessageType::SESSION_HELLO;
        const PairResult stopped = run_pair(
            candidate, server, admit(candidate, bytes("candidate-only input\n")),
            stop_after_hello);
        require(stopped.client.status == ClientRunStatus::Disconnected &&
                    stopped.server.status == ServerRunStatus::Disconnected &&
                    server.namespace_count() == 0 && server.revision_count() == 0 &&
                    server.live_session_count() == 0,
                "unactivated candidate retained namespace, revision, or live-session state");
    }

    TestClient active(Id128::from_u64(171));
    const PairResult committed =
        run_pair(active, server, admit(active, bytes("activated input\n")));
    require(committed.client.status == ClientRunStatus::Committed &&
                committed.server.status == ServerRunStatus::Completed &&
                server.namespace_count() == 1 && server.revision_count() == 1 &&
                server.live_session_count() == 0,
            "activated namespace did not acquire exactly one revision owner");
}

void test_input_record_owner_and_aggregate_limits() {
    {
        P50ServerEndpoint server(Id128::from_u64(172));
        TestClient client(Id128::from_u64(173));
        const std::vector<uint8_t> input = pseudo_random_bytes(32 * 1024);
        const PairResult completed = run_pair(client, server, admit(client, input));
        require(completed.client.status == ClientRunStatus::Committed &&
                    completed.server.status == ServerRunStatus::Completed &&
                    completed.server.committed_input.has_value() &&
                    completed.server.committed_input ==
                        server.last_committed_input(client.c_store_guid()) &&
                    server.owner_usage().retained_input_records == 1 &&
                    server.owner_usage().retained_input_bytes == input.size(),
                "route commit did not atomically publish one InputRecord key");

        InputCursor authorized = server.attach_input(*completed.server.committed_input);
        std::array<uint8_t, 257> prefix{};
        require(authorized.read(prefix) == prefix.size() &&
                    std::equal(prefix.begin(), prefix.end(), input.begin()),
                "authorized InputRecord cursor read the wrong prefix");
        server.close_input_job(*completed.server.committed_input);
        require_throws<std::logic_error>(
            [&] { (void)server.attach_input(*completed.server.committed_input); },
            "closed logical job accepted a new InputRecord attachment");
        server.collect_input_garbage();
        require(server.owner_usage().retained_input_records == 1,
                "job close reclaimed input owned by an authorized cursor");
        std::vector<uint8_t> reconstructed(prefix.begin(), prefix.end());
        std::vector<uint8_t> remainder = drain_input(authorized);
        reconstructed.insert(reconstructed.end(), remainder.begin(), remainder.end());
        require(reconstructed == input,
                "authorized cursor lost exact input after logical-job close");
        authorized = InputCursor{};
        server.collect_input_garbage();
        require(server.owner_usage().retained_input_records == 0 &&
                    server.owner_usage().retained_input_bytes == 0,
                "closed InputRecord did not collect after its final cursor");
    }

    {
        P50ServerEndpointConfig config;
        config.input_job_state =
            [](CStoreGuid, const TxBegin&, const TxCommit&, std::span<const uint8_t>) {
                return InputJobState::Closed;
            };
        P50ServerEndpoint server(Id128::from_u64(174), {}, nullptr, nullptr,
                                 std::move(config));
        TestClient client(Id128::from_u64(175));
        const PairResult completed =
            run_pair(client, server, admit(client, bytes("late closed job input\n")));
        require(completed.client.status == ClientRunStatus::Committed &&
                    completed.server.status == ServerRunStatus::Completed &&
                    !completed.server.committed_input &&
                    server.owner_usage().retained_input_records == 0 &&
                    server.owner_usage().retained_input_bytes == 0,
                "closed-before-commit job created a compiler-visible lease");
    }

    {
        P50ServerEndpoint server(Id128::from_u64(188));
        TestClient client(Id128::from_u64(189));
        const std::vector<uint8_t> input = pseudo_random_bytes(8192);
        const PairResult completed = run_pair(client, server, admit(client, input));
        require(completed.server.committed_input.has_value(),
                "store-reset fixture did not publish an InputRecord");
        InputCursor authorized = server.attach_input(*completed.server.committed_input);
        server.reset_store(Id128::from_u64(190));
        const P50ServerOwnerUsage reset_usage = server.owner_usage();
        require(reset_usage.live_sessions == 0 && reset_usage.namespaces == 0 &&
                    reset_usage.revisions == 0 && reset_usage.pending_encoded_bytes == 0 &&
                    reset_usage.pending_raw_bytes == 0 &&
                    reset_usage.decoder_window_bytes == 0 &&
                    reset_usage.retained_input_records == 0 &&
                    reset_usage.retained_input_bytes == 0 &&
                    drain_input(authorized) == input,
                "F-store replacement lost an authorized cursor or retained owner state");
    }

    {
        P50ServerEndpointConfig config;
        config.owner_limits.max_namespaces = 1;
        P50ServerEndpoint server(Id128::from_u64(176), {}, nullptr, nullptr,
                                 std::move(config));
        TestClient first(Id128::from_u64(177));
        TestClient second(Id128::from_u64(178));
        const PairResult retained =
            run_pair(first, server, admit(first, bytes("first namespace\n")));
        require(retained.client.status == ClientRunStatus::Committed &&
                    retained.server.committed_input.has_value(),
                "first namespace did not fit its aggregate bound");
        server.close_input_job(*retained.server.committed_input);
        const PairResult replacement =
            run_pair(second, server, admit(second, bytes("second namespace\n")));
        require(replacement.client.status == ClientRunStatus::Committed &&
                    replacement.server.status == ServerRunStatus::Completed &&
                    replacement.server.committed_input.has_value() &&
                    server.owner_usage().namespaces == 1 &&
                    server.owner_usage().revisions == 1 &&
                    !server.last_committed_input(first.c_store_guid()).has_value() &&
                    copy_input(server, second.c_store_guid()) ==
                        bytes("second namespace\n"),
                "whole-namespace LRU did not replace the closed oldest namespace");
    }

    {
        P50ServerEndpointConfig config;
        config.owner_limits.max_retained_input_records = 1;
        P50ServerEndpoint server(Id128::from_u64(208), {}, nullptr, nullptr,
                                 std::move(config));
        TestClient first(Id128::from_u64(209));
        TestClient second(Id128::from_u64(210));
        const PairResult retained =
            run_pair(first, server, admit(first, bytes("evictable retained input\n")));
        require(retained.server.committed_input.has_value(),
                "retained-input LRU fixture did not publish its first record");
        InputCursor pinned = server.attach_input(*retained.server.committed_input);
        server.close_input_job(*retained.server.committed_input);

        const PreparedTuHandle blocked_input =
            admit(second, bytes("blocked by cursor\n"));
        const PairResult held =
            run_pair(second, server, blocked_input);
        require(held.client.status == ClientRunStatus::TerminalError &&
                    held.server.status == ServerRunStatus::TerminalError &&
                    server.owner_usage().retained_input_records == 1 &&
                    server.owner_usage().retained_input_bytes ==
                        bytes("evictable retained input\n").size(),
                "cursor-pinned capacity failure evicted retained input or published new input");

        pinned = InputCursor{};
        const PairResult retried = run_pair(second, server);
        require(retried.client.status == ClientRunStatus::Committed &&
                    retried.server.status == ServerRunStatus::Completed &&
                    retried.client.reconnect == EndpointReconnectOutcome::ExactMatch &&
                    server.owner_usage().retained_input_records == 1 &&
                    server.owner_usage().namespaces == 2 &&
                    server.owner_usage().revisions == 2,
                "released cursor did not permit atomic whole-namespace capacity recovery");
        require(retried.server.committed_input.has_value(),
                "capacity-recovery retry did not publish its retained input");
        server.close_input_job(*retried.server.committed_input);
        const PairResult first_again =
            run_pair(first, server,
                     admit(first, bytes("first route after payload eviction\n")));
        require(first_again.client.status == ClientRunStatus::Committed &&
                    first_again.server.status == ServerRunStatus::Completed &&
                    first_again.client.reconnect == EndpointReconnectOutcome::ExactMatch &&
                    server.owner_usage().retained_input_records == 1 &&
                    server.owner_usage().namespaces == 2 &&
                    server.owner_usage().revisions == 2,
                "cache-payload eviction destroyed the surviving protocol route");
    }

    {
        P50ServerEndpointConfig config;
        config.owner_limits.max_pending_encoded_bytes = 1;
        P50ServerEndpoint server(Id128::from_u64(179), {}, nullptr, nullptr,
                                 std::move(config));
        TestClient client(Id128::from_u64(180));
        const PairResult rejected =
            run_pair(client, server, admit(client, pseudo_random_bytes(4096)));
        const P50ServerOwnerUsage usage = server.owner_usage();
        require(rejected.client.status == ClientRunStatus::TerminalError &&
                    rejected.server.status == ServerRunStatus::TerminalError &&
                    client.has_active_transaction() &&
                    usage.pending_encoded_bytes == 0 && usage.pending_raw_bytes == 0 &&
                    usage.decoder_window_bytes == 0 && usage.retained_input_records == 0,
                "pending-input cap failure leaked an aggregate reservation");
    }

    {
        P50ServerEndpointConfig config;
        config.owner_limits.max_pending_encoded_bytes = 4096;
        config.owner_limits.max_decoder_window_bytes =
            uint64_t{2} << EndpointCaps{}.zstd.max_window_log;
        const CompetingPairResult result =
            run_competing_pair(std::move(config), 196, 3000, 3001);
        const size_t committed =
            static_cast<size_t>(result.first_client.status ==
                                ClientRunStatus::Committed) +
            static_cast<size_t>(result.second_client.status ==
                                ClientRunStatus::Committed);
        const size_t completed =
            static_cast<size_t>(result.first_server.status ==
                                ServerRunStatus::Completed) +
            static_cast<size_t>(result.second_server.status ==
                                ServerRunStatus::Completed);
        const size_t terminal =
            static_cast<size_t>(result.first_server.status ==
                                ServerRunStatus::TerminalError) +
            static_cast<size_t>(result.second_server.status ==
                                ServerRunStatus::TerminalError);
        require(committed == 1 && completed == 1 && terminal == 1 &&
                    result.usage.pending_encoded_bytes == 0 &&
                    result.usage.pending_raw_bytes == 0 &&
                    result.usage.decoder_window_bytes == 0 &&
                    result.usage.retained_input_records == 1,
                "aggregate encoded-byte budget admitted overlapping dialogues");
    }

    {
        P50ServerEndpointConfig config;
        config.owner_limits.max_pending_encoded_bytes = 8192;
        config.owner_limits.max_pending_raw_bytes = 4096;
        config.owner_limits.max_decoder_window_bytes =
            uint64_t{2} << EndpointCaps{}.zstd.max_window_log;
        const CompetingPairResult result =
            run_competing_pair(std::move(config), 199, 3000, 3001);
        const size_t committed =
            static_cast<size_t>(result.first_client.status ==
                                ClientRunStatus::Committed) +
            static_cast<size_t>(result.second_client.status ==
                                ClientRunStatus::Committed);
        const size_t completed =
            static_cast<size_t>(result.first_server.status ==
                                ServerRunStatus::Completed) +
            static_cast<size_t>(result.second_server.status ==
                                ServerRunStatus::Completed);
        const size_t terminal =
            static_cast<size_t>(result.first_server.status ==
                                ServerRunStatus::TerminalError) +
            static_cast<size_t>(result.second_server.status ==
                                ServerRunStatus::TerminalError);
        require(committed == 1 && completed == 1 && terminal == 1 &&
                    result.usage.pending_encoded_bytes == 0 &&
                    result.usage.pending_raw_bytes == 0 &&
                    result.usage.decoder_window_bytes == 0 &&
                    result.usage.retained_input_records == 1,
                "aggregate raw-byte budget admitted overlapping dialogues");
    }

    {
        P50ServerEndpointConfig config;
        config.owner_limits.max_retained_input_bytes = 8;
        P50ServerEndpoint server(Id128::from_u64(181), {}, nullptr, nullptr,
                                 std::move(config));
        TestClient client(Id128::from_u64(182));
        const PairResult rejected =
            run_pair(client, server, admit(client, bytes("larger than eight bytes")));
        const P50ServerOwnerUsage usage = server.owner_usage();
        require(rejected.client.status == ClientRunStatus::TerminalError &&
                    rejected.server.status == ServerRunStatus::TerminalError &&
                    client.has_active_transaction() && usage.retained_input_records == 0 &&
                    usage.retained_input_bytes == 0 && usage.pending_encoded_bytes == 0 &&
                    usage.pending_raw_bytes == 0 && usage.decoder_window_bytes == 0,
                "InputRecord-cap failure committed or leaked owner resources");
    }

    {
        P50ServerEndpointConfig config;
        config.owner_limits.max_retained_input_records = 1;
        P50ServerEndpoint server(Id128::from_u64(202), {}, nullptr, nullptr,
                                 std::move(config));
        TestClient client(Id128::from_u64(203));
        const PairResult first =
            run_pair(client, server, admit(client, bytes("first retained input\n")));
        const PairResult second =
            run_pair(client, server, admit(client, bytes("second retained input\n")));
        const P50ServerOwnerUsage usage = server.owner_usage();
        require(first.client.status == ClientRunStatus::Committed &&
                    first.server.status == ServerRunStatus::Completed &&
                    second.client.status == ClientRunStatus::TerminalError &&
                    second.server.status == ServerRunStatus::TerminalError &&
                    usage.retained_input_records == 1 &&
                    usage.pending_encoded_bytes == 0 &&
                    usage.pending_raw_bytes == 0 &&
                    usage.decoder_window_bytes == 0,
                "InputRecord-count bound admitted a second retained input");
    }

    {
        CompletionLog completions(0);
        ActionTrace actions(0);
        P50ServerEndpoint server(Id128::from_u64(183), {}, &completions, &actions);
        TestClient client(Id128::from_u64(184), {}, HistoryNonce{1}, &completions,
                          &actions);
        const PairResult completed =
            run_pair(client, server, admit(client, bytes("diagnostic loss is not state\n")));
        require(completed.client.status == ClientRunStatus::Committed &&
                    completed.server.status == ServerRunStatus::Completed &&
                    !completions.valid() && completions.completions().empty() &&
                    !actions.valid() && actions.records().empty() &&
                    server.owner_usage().retained_input_records == 1,
                "bounded diagnostic loss changed the durable protocol result");
    }

    {
        P50ServerEndpointConfig config;
        config.owner_limits.max_live_sessions = 1;
        P50ServerEndpoint server(Id128::from_u64(191), {}, nullptr, nullptr,
                                 std::move(config));
        TestClient client(Id128::from_u64(192));
        const PreparedTuHandle prepared =
            admit(client, bytes("one accepted live session\n"));
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> accepted =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<ServerRunResult> excess =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<ClientRunResult> client_result = asio::co_spawn(
            context, client.endpoint.run(acceptor.local_endpoint(), prepared),
            asio::use_future);
        context.run();
        require(client_result.get().status == ClientRunStatus::Committed &&
                    accepted.get().status == ServerRunStatus::Completed,
                "live-session bound rejected its admitted dialogue");
        require_throws<std::length_error>(
            [&] { (void)excess.get(); },
            "live-session aggregate bound admitted an extra dialogue");
        require(server.owner_usage().live_sessions == 0,
                "live-session bound retained a completed session");
    }

    {
        P50ServerEndpointConfig config;
        config.owner_limits.max_decoder_window_bytes =
            uint64_t{1} << EndpointCaps{}.zstd.max_window_log;
        P50ServerEndpoint server(Id128::from_u64(193), {}, nullptr, nullptr,
                                 std::move(config));
        TestClient first(Id128::from_u64(194));
        TestClient second(Id128::from_u64(195));
        const PreparedTuHandle first_prepared =
            admit(first, pseudo_random_bytes(4096));
        const PreparedTuHandle second_prepared =
            admit(second, pseudo_random_bytes(4097));
        EndpointIoControl fragment_first;
        fragment_first.max_write_fragment = 1;
        EndpointIoControl fragment_second;
        fragment_second.max_write_fragment = 1;
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> first_server =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<ServerRunResult> second_server =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<ClientRunResult> first_client = asio::co_spawn(
            context,
            first.endpoint.run(acceptor.local_endpoint(), first_prepared,
                               fragment_first),
            asio::use_future);
        std::future<ClientRunResult> second_client = asio::co_spawn(
            context,
            second.endpoint.run(acceptor.local_endpoint(), second_prepared,
                                fragment_second),
            asio::use_future);
        context.run();
        const ClientRunResult first_client_result = first_client.get();
        const ClientRunResult second_client_result = second_client.get();
        const ServerRunResult first_server_result = first_server.get();
        const ServerRunResult second_server_result = second_server.get();
        const size_t committed_clients =
            static_cast<size_t>(first_client_result.status == ClientRunStatus::Committed) +
            static_cast<size_t>(second_client_result.status == ClientRunStatus::Committed);
        const size_t completed_servers =
            static_cast<size_t>(first_server_result.status == ServerRunStatus::Completed) +
            static_cast<size_t>(second_server_result.status == ServerRunStatus::Completed);
        const size_t terminal_servers =
            static_cast<size_t>(first_server_result.status == ServerRunStatus::TerminalError) +
            static_cast<size_t>(second_server_result.status == ServerRunStatus::TerminalError);
        const P50ServerOwnerUsage usage = server.owner_usage();
        require(committed_clients == 1 && completed_servers == 1 && terminal_servers == 1 &&
                    usage.pending_encoded_bytes == 0 && usage.pending_raw_bytes == 0 &&
                    usage.decoder_window_bytes == 0 &&
                    usage.retained_input_records == 1,
                "aggregate decoder-window budget admitted overlapping dialogues");
    }

    {
        auto authority =
            std::make_shared<P50PreparationAuthority>(Id128::from_u64(185));
        const PreparedTuHandle prepared =
            authority->prepare({1, 1}, bytes("single owner thread\n"));
        bool rejected = false;
        std::thread other([&] {
            try {
                (void)authority->retain(prepared);
            } catch (const std::logic_error&) {
                rejected = true;
            }
        });
        other.join();
        require(rejected && authority->release(prepared) == 0,
                "mutable preparation state crossed its established owner thread");
    }

    {
        P50ServerEndpoint server(Id128::from_u64(204));
        TestClient client(Id128::from_u64(205));
        require(run_pair(client, server,
                         admit(client, bytes("endpoint owner thread\n")))
                        .client.status == ClientRunStatus::Committed,
                "endpoint thread-owner fixture did not commit");
        bool client_rejected = false;
        bool server_rejected = false;
        std::thread other([&] {
            try {
                (void)client.endpoint.next_rel_seq();
            } catch (const std::logic_error&) {
                client_rejected = true;
            }
            try {
                server.collect_input_garbage();
            } catch (const std::logic_error&) {
                server_rejected = true;
            }
        });
        other.join();
        require(client_rejected && server_rejected,
                "client or server mutable state crossed its established owner thread");
    }

    {
        P50ServerEndpointConfig invalid;
        invalid.owner_limits.max_live_sessions = 0;
        require_throws<std::invalid_argument>(
            [&] {
                P50ServerEndpoint rejected(Id128::from_u64(186), {}, nullptr, nullptr,
                                           invalid);
            },
            "zero aggregate live-session bound was accepted");
        invalid = {};
        invalid.owner_limits.max_decoder_window_bytes =
            (uint64_t{1} << EndpointCaps{}.zstd.max_window_log) - 1;
        require_throws<std::invalid_argument>(
            [&] {
                P50ServerEndpoint rejected(Id128::from_u64(187), {}, nullptr, nullptr,
                                           invalid);
            },
            "decoder-window aggregate bound admitted no complete dialogue");
    }
}

void test_fragmentation_at_every_control_and_body_boundary() {
    const std::vector<uint8_t> input = pseudo_random_bytes(96);
    const ZstdTuEnvelope encoded = encode_zstd_tu(
        HistoryNonce{1}, RelSeq{0}, TuSeq{0}, Digest128{}, input);
    const size_t frame_bytes =
        std::max(4 + encoded.body.size(), size_t{4 + kMandatoryControlFramePayload});
    require(frame_bytes > 8, "fragmentation fixture is too small");
    for (size_t split = 1; split != frame_bytes; ++split) {
        P50ServerEndpoint server(Id128::from_u64(1000 + split));
        TestClient client(Id128::from_u64(2000 + split));
        EndpointIoControl client_control;
        client_control.max_write_fragment = split;
        EndpointIoControl server_control;
        server_control.max_write_fragment = split;
        const PairResult result =
            run_pair(client, server, admit(client, input), client_control, server_control);
        if (result.client.status != ClientRunStatus::Committed ||
            result.server.status != ServerRunStatus::Completed)
            fail("fragmentation failed at split " + std::to_string(split));
    }
}

void test_exact_replay_and_lost_final() {
    {
        ActionTrace actions;
        P50ServerEndpoint server(Id128::from_u64(300), {}, nullptr, &actions);
        TestClient client(Id128::from_u64(301), {}, HistoryNonce{20}, nullptr, &actions);
        EndpointIoControl stop;
        stop.close_after_write = MessageType::TX_BEGIN;
        const std::vector<uint8_t> input = bytes("exact replay input\n");
        const PairResult interrupted = run_pair(client, server, admit(client, input), stop);
        require(interrupted.client.status == ClientRunStatus::Disconnected &&
                    client.has_active_transaction(),
                "uncertain disconnect erased C's active transaction");
        const PairResult replayed = run_pair(client, server);
        require(replayed.client.status == ClientRunStatus::Committed &&
                    replayed.client.reconnect == EndpointReconnectOutcome::ExactMatch,
                "exact replay did not commit on the unchanged route");
        require(replayed.client.committed_commit.has_value() &&
                    replayed.client.committed_input.has_value() &&
                    replayed.client.committed_input ==
                        server.last_committed_input(client.c_store_guid()),
                "normal commit did not return its exact direct witness");
        require(copy_input(server, client.c_store_guid()) == input,
                "exact replay materialized different bytes");
        require(std::any_of(actions.records().begin(), actions.records().end(),
                            [](const ActionRecord& record) {
                                return record.action == ActionType::ACTIVE_REPLAYED;
                            }),
                "exact replay omitted ACTIVE_REPLAYED correspondence");
        require_trace(actions, "exact replay trace");
    }

    {
        ActionTrace actions;
        P50ServerEndpoint server(Id128::from_u64(400), {}, nullptr, &actions);
        TestClient client(Id128::from_u64(401), {}, HistoryNonce{30}, nullptr, &actions);
        EndpointIoControl lose_final;
        lose_final.close_before_write = MessageType::TX_COMMIT;
        const std::vector<uint8_t> input = bytes("lost final acknowledgement\n");
        const PairResult interrupted =
            run_pair(client, server, admit(client, input), {}, lose_final);
        require(interrupted.client.status == ClientRunStatus::Disconnected &&
                    client.has_active_transaction(),
                "lost-final window did not retain C active state");
        require(copy_input(server, client.c_store_guid()) == input,
                "F did not retain its completed input before final-message loss");
        const PairResult reconciled = run_pair(client, server);
        require(reconciled.client.status == ClientRunStatus::Committed &&
                    reconciled.client.reconnect ==
                        EndpointReconnectOutcome::LostFinalAcknowledgement &&
                    !client.has_active_transaction(),
                "lost-final reconciliation did not accept the exact retained commit");
        require(reconciled.client.committed_commit.has_value() &&
                    reconciled.client.committed_input.has_value() &&
                    reconciled.client.committed_input ==
                        server.last_committed_input(client.c_store_guid()),
                "lost-final reconciliation did not return its exact direct witness");
        require_trace(actions, "lost-final trace");
    }

    {
        ActionTrace actions;
        size_t publication_attempts = 0;
        const std::vector<uint8_t> input =
            bytes("publication must precede route commit\n");
        P50ServerEndpointConfig config;
        config.input_job_state =
            [&](CStoreGuid published_guid, const TxBegin& begin,
                const TxCommit& commit, std::span<const uint8_t> exact) {
                ++publication_attempts;
                require(published_guid == Id128::from_u64(411) &&
                            commit.history_nonce == begin.history_nonce &&
                            commit.rel_seq == begin.rel_seq &&
                            commit.tu_seq == begin.tu_seq &&
                            commit.transaction_digest == begin.transaction_digest &&
                            commit.raw_digest == begin.raw_digest &&
                            std::ranges::equal(exact, input),
                        "precommit publisher received the wrong exact tuple or bytes");
                if (publication_attempts == 1)
                    throw std::bad_alloc();
                return InputJobState::Open;
            };
        P50ServerEndpoint server(Id128::from_u64(410), {}, nullptr, &actions,
                                 std::move(config));
        TestClient client(Id128::from_u64(411), {}, HistoryNonce{31}, nullptr,
                          &actions);
        const PairResult rejected =
            run_pair(client, server, admit(client, input));
        require(rejected.client.status == ClientRunStatus::TerminalError &&
                    rejected.client.observation != ClientRunObservation::ExactCommitObserved &&
                    rejected.server.status == ServerRunStatus::TerminalError &&
                    client.has_active_transaction() &&
                    !copy_input(server, client.c_store_guid()) &&
                    publication_attempts == 1,
                "failed precommit publication advanced or discarded the transaction");
        const PairResult replayed = run_pair(client, server);
        require(replayed.client.status == ClientRunStatus::Committed &&
                    replayed.client.reconnect == EndpointReconnectOutcome::ExactMatch &&
                    copy_input(server, client.c_store_guid()) == input &&
                    publication_attempts == 2,
                "allocation-failure transaction did not replay and publish exactly");
        require(std::any_of(actions.records().begin(), actions.records().end(),
                            [](const ActionRecord& record) {
                                return record.actor == ActorSide::F &&
                                       record.action == ActionType::ACTIVE_REPLAYED;
                            }),
                "allocation-failure replay omitted ACTIVE_REPLAYED");
        require_trace(actions, "precommit publication replay trace");
    }
}

void test_completion_identity_and_store_replacement() {
    {
        ActionTrace actions;
        P50ServerEndpoint server(Id128::from_u64(450), {}, nullptr, &actions);
        TestClient client(Id128::from_u64(451), {}, HistoryNonce{31}, nullptr, &actions);
        EndpointIoControl wrong;
        wrong.wrong_digest_completion = AsyncOperationKind::WriteFragment;
        const std::vector<uint8_t> input = pseudo_random_bytes(2048);
        const PairResult rejected = run_pair(client, server, admit(client, input), wrong);
        require(rejected.client.status == ClientRunStatus::Disconnected &&
                    client.has_active_transaction() &&
                    !copy_input(server, client.c_store_guid()),
                "wrong-digest completion changed endpoint state");
        const PairResult replayed = run_pair(client, server);
        require(replayed.client.status == ClientRunStatus::Committed &&
                    copy_input(server, client.c_store_guid()) == input,
                "wrong-digest completion was not recoverable by exact replay");
        require_trace(actions, "wrong-digest completion trace");
    }

    for (bool alter_raw_digest : {false, true}) {
        P50ServerEndpoint server(Id128::from_u64(alter_raw_digest ? 455 : 453));
        TestClient client(Id128::from_u64(alter_raw_digest ? 456 : 454));
        EndpointIoControl wrong;
        if (alter_raw_digest)
            wrong.wrong_raw_digest_completion = AsyncOperationKind::ReadPayload;
        else
            wrong.wrong_digest_completion = AsyncOperationKind::ReadPayload;
        const std::vector<uint8_t> input = pseudo_random_bytes(3072);
        const PairResult rejected =
            run_pair(client, server, admit(client, input), {}, wrong);
        require(rejected.server.status == ServerRunStatus::Disconnected &&
                    rejected.client.status == ClientRunStatus::Disconnected &&
                    client.has_active_transaction() &&
                    !copy_input(server, client.c_store_guid()),
                alter_raw_digest
                    ? "late raw-digest completion changed endpoint state"
                    : "late transaction-digest completion changed endpoint state");
        const PairResult replayed = run_pair(client, server);
        require(replayed.client.status == ClientRunStatus::Committed &&
                    copy_input(server, client.c_store_guid()) == input,
                alter_raw_digest
                    ? "late raw-digest completion did not permit exact replay"
                    : "late transaction-digest completion did not permit exact replay");
    }

    {
        ActionTrace actions;
        CompletionLog completions;
        const FStoreGuid first_guid = Id128::from_u64(460);
        const FStoreGuid second_guid = Id128::from_u64(461);
        P50ServerEndpoint server(first_guid, {}, &completions, &actions);
        TestClient client(Id128::from_u64(462), {}, HistoryNonce{32}, &completions,
                                 &actions);
        bool reset_done = false;
        EndpointIoControl reset_during_body;
        reset_during_body.before_completion_check = [&](const CompletionStamp& stamp) {
            if (!reset_done && stamp.actor == ActorSide::F && stamp.transaction_bound &&
                stamp.operation == AsyncOperationKind::ReadPayload) {
                reset_done = true;
                server.reset_store(second_guid);
            }
        };
        const std::vector<uint8_t> input = pseudo_random_bytes(4096);
        const PairResult invalidated =
            run_pair(client, server, admit(client, input), {}, reset_during_body);
        require(reset_done && invalidated.server.status == ServerRunStatus::Disconnected &&
                    invalidated.client.status == ClientRunStatus::Disconnected &&
                    client.has_active_transaction() && server.f_store_guid() == second_guid &&
                    !copy_input(server, client.c_store_guid()),
                "F reset did not fence its old in-flight completion");
        const PairResult replaced = run_pair(client, server);
        require(replaced.client.status == ClientRunStatus::Committed &&
                    replaced.client.reconnect == EndpointReconnectOutcome::ColdFStore &&
                    replaced.client.observation == ClientRunObservation::ExactCommitObserved &&
                    replaced.server.session_serial > invalidated.server.session_serial &&
                    copy_input(server, client.c_store_guid()) == input,
                "precommit F-incarnation replacement did not preserve exact prepared work");
        require(std::any_of(actions.records().begin(), actions.records().end(),
                            [](const ActionRecord& record) {
                                return record.action == ActionType::F_STORE_INCAR_REPLACED;
                            }),
                "precommit F replacement omitted its explicit action");
        require_trace(actions, "precommit F replacement trace");

        bool saw_old_completion = false;
        bool saw_new_completion = false;
        for (const AsyncCompletion& completion : completions.completions()) {
            if (completion.stamp.actor != ActorSide::F)
                continue;
            if (completion.stamp.f_store_guid == first_guid)
                saw_old_completion = true;
            if (completion.stamp.f_store_guid == second_guid)
                saw_new_completion = true;
        }
        require(saw_old_completion && saw_new_completion,
                "completion log did not retain distinct old/new F identities");
    }

    {
        ActionTrace actions;
        P50ServerEndpoint server(Id128::from_u64(470), {}, nullptr, &actions);
        TestClient client(Id128::from_u64(471), {}, HistoryNonce{33}, nullptr, &actions);
        EndpointIoControl lose_final;
        lose_final.close_before_write = MessageType::TX_COMMIT;
        const std::vector<uint8_t> input = bytes("durable before F reset\n");
        const PairResult durable =
            run_pair(client, server, admit(client, input), {}, lose_final);
        require(durable.client.status == ClientRunStatus::Disconnected &&
                    client.has_active_transaction() &&
                    copy_input(server, client.c_store_guid()) == input,
                "postcommit/pre-ack replacement fixture did not reach its durable window");
        server.reset_store(Id128::from_u64(472));
        const PairResult replaced = run_pair(client, server);
        require(replaced.client.status == ClientRunStatus::Committed &&
                    replaced.client.reconnect == EndpointReconnectOutcome::ColdFStore &&
                    replaced.client.observation == ClientRunObservation::ExactCommitObserved &&
                    copy_input(server, client.c_store_guid()) == input,
                "postcommit/pre-ack F replacement did not replay exact prepared work");
        require_trace(actions, "postcommit/pre-ack F replacement trace");
    }
}

void test_same_f_route_reset() {
    {
        ActionTrace actions;
        P50ServerEndpoint server(Id128::from_u64(550), {}, nullptr, &actions);
        TestClient client(Id128::from_u64(551), {}, HistoryNonce{50}, nullptr, &actions);
        require(
            run_pair(client, server, admit(client, bytes("route before reset\n"))).client.status ==
                ClientRunStatus::Committed,
            "route-reset fixture did not establish its first route");
        const ServerRunResult forced =
            force_route_reset(server, client.c_store_guid(), HistoryNonce{999});
        require(forced.status == ServerRunStatus::Disconnected &&
                    forced.c_store_guid == client.c_store_guid(),
                "independent route reset did not close as expected");
        const std::vector<uint8_t> input = bytes("route after reset\n");
        const PairResult repaired = run_pair(client, server, admit(client, input));
        require(repaired.client.status == ClientRunStatus::Committed &&
                    repaired.client.reconnect == EndpointReconnectOutcome::RouteHistoryReset,
                "same-F route mismatch did not take the history-reset outcome");
        require(copy_input(server, client.c_store_guid()) == input,
                "route-reset retry did not materialize exact input");
        require_trace(actions, "route-history-reset trace");
    }
}

void test_reset_ack_equality_and_terminal_result() {
    const FStoreGuid f_guid = Id128::from_u64(570);
    for (const ResetAckMutation mutation :
         {ResetAckMutation::WireRevision, ResetAckMutation::ProfileMask,
          ResetAckMutation::FrameLimit, ResetAckMutation::FillLimit}) {
        TestClient client(Id128::from_u64(571 + static_cast<uint8_t>(mutation)));
        const std::vector<uint8_t> input = bytes("reset acknowledgement equality\n");
        const PreparedTuHandle prepared = admit(client, input);
        const ClientRunResult terminal = run_client_with_raw_peer(
            client, prepared, [&](tcp::acceptor& acceptor) {
                return raw_bad_reset_ack_peer(
                    acceptor, ScriptedSessionState{.f_guid = f_guid}, mutation);
            });
        require(terminal.status == ClientRunStatus::TerminalError &&
                    terminal.observation != ClientRunObservation::ExactCommitObserved && client.has_reconciliation_work() &&
                    !client.has_active_transaction(),
                "bad HISTORY_RESET acknowledgement lost queued reconciliation work");

        P50ServerEndpoint good(f_guid);
        const PairResult recovered = run_pair(client, good);
        require(recovered.client.status == ClientRunStatus::Committed &&
                    copy_input(good, client.c_store_guid()) == input,
                "bad HISTORY_RESET acknowledgement changed the queued PreparedTU");
    }

    {
        EndpointCaps exact_cap;
        exact_cap.wire.max_frame_payload = kMandatoryControlFramePayload;
        TestClient client(Id128::from_u64(580), exact_cap);
        const std::vector<uint8_t> input = bytes("bounded peer terminal result\n");
        const PreparedTuHandle prepared = admit(client, input);
        const ClientRunResult terminal = run_client_with_raw_peer(
            client, prepared, [&](tcp::acceptor& acceptor) {
                return raw_bounded_error_peer(acceptor, kMandatoryControlFramePayload);
            });
        require(terminal.status == ClientRunStatus::TerminalError &&
                    terminal.observation != ClientRunObservation::ExactCommitObserved && terminal.terminal_error &&
                    encode_payload(Message{*terminal.terminal_error}).size() ==
                        kMandatoryControlFramePayload &&
                    client.has_reconciliation_work(),
                "cap-sized peer ERROR did not use the bounded terminal-result path");
        P50ServerEndpoint good(Id128::from_u64(581), exact_cap);
        require(run_pair(client, good).client.status == ClientRunStatus::Committed &&
                    copy_input(good, client.c_store_guid()) == input,
                "terminal peer ERROR changed the queued PreparedTU");
    }

    {
        TestClient client(Id128::from_u64(585));
        const std::vector<uint8_t> input = bytes("client framing terminal result\n");
        const PreparedTuHandle prepared = admit(client, input);
        const ClientRunResult terminal = run_client_with_raw_peer(
            client, prepared,
            [&](tcp::acceptor& acceptor) { return raw_unknown_frame_peer(acceptor); });
        require(terminal.status == ClientRunStatus::TerminalError &&
                    terminal.observation != ClientRunObservation::ExactCommitObserved && terminal.terminal_error &&
                    client.has_reconciliation_work() && !client.has_active_transaction(),
                "client framing failure bypassed the bounded terminal-result path");
        P50ServerEndpoint good(Id128::from_u64(586));
        require(run_pair(client, good).client.status == ClientRunStatus::Committed &&
                    copy_input(good, client.c_store_guid()) == input,
                "client framing failure changed the queued PreparedTU");
    }

    {
        ActionTrace actions;
        P50ServerEndpoint established(f_guid, {}, nullptr, &actions);
        TestClient client(Id128::from_u64(590), {}, HistoryNonce{1}, nullptr, &actions);
        const std::vector<uint8_t> input = bytes("active identity survives route mismatch\n");
        EndpointIoControl stop;
        stop.close_after_write = MessageType::TX_BEGIN;
        require(run_pair(client, established, admit(client, input), stop).client.status ==
                        ClientRunStatus::Disconnected &&
                    client.has_active_transaction(),
                "active terminal fixture did not retain its original transaction");
        const auto initial_position = std::find_if(
            actions.records().begin(), actions.records().end(),
            [](const ActionRecord& record) {
                return record.actor == ActorSide::C && record.action == ActionType::TX_BEGIN;
            });
        require(initial_position != actions.records().end(),
                "active terminal fixture omitted its canonical C TX_BEGIN");
        const ActionRecord initial_identity = *initial_position;
        const RelSeq initial_next_rel = client.endpoint.next_rel_seq();
        const Digest128 initial_state = client.endpoint.state_digest();
        const size_t action_count_before_mismatch = actions.records().size();

        const ClientRunResult terminal = run_client_with_raw_peer(
            client, {}, [&](tcp::acceptor& acceptor) {
                return raw_route_mismatch_peer(
                    acceptor,
                    ScriptedSessionState{.f_guid = f_guid,
                                         .route_present = true,
                                         .history_nonce = HistoryNonce{991},
                                         .next_rel_seq = RelSeq{0},
                                         .state_digest = icecc::digest128("other route")});
            });
        require(terminal.status == ClientRunStatus::TerminalError &&
                    terminal.reconnect == EndpointReconnectOutcome::RouteHistoryReset &&
                    terminal.observation != ClientRunObservation::ExactCommitObserved && client.has_active_transaction() &&
                    client.has_reconciliation_work() &&
                    client.endpoint.next_rel_seq() == initial_next_rel &&
                    client.endpoint.state_digest() == initial_state &&
                    actions.records().size() == action_count_before_mismatch,
                "active same-F route mismatch erased reconciliation identity");
        const PairResult recovered = run_pair(client, established);
        require(recovered.client.status == ClientRunStatus::Committed &&
                    recovered.client.reconnect == EndpointReconnectOutcome::ExactMatch &&
                    copy_input(established, client.c_store_guid()) == input,
                "terminal route-ack error changed exact replay identity");
        const auto replay_position = std::find_if(
            actions.records().begin(), actions.records().end(),
            [](const ActionRecord& record) {
                return record.actor == ActorSide::F &&
                       record.action == ActionType::ACTIVE_REPLAYED;
            });
        require(replay_position != actions.records().end() &&
                    replay_position->c_store_guid == initial_identity.c_store_guid &&
                    replay_position->f_store_guid == initial_identity.f_store_guid &&
                    replay_position->history_nonce == initial_identity.history_nonce &&
                    replay_position->rel_seq == initial_identity.rel_seq &&
                    replay_position->tu_seq == initial_identity.tu_seq &&
                    replay_position->transaction_digest == initial_identity.transaction_digest &&
                    replay_position->raw_digest == initial_identity.raw_digest &&
                    replay_position->state_digest == initial_identity.state_digest,
                "same-F mismatch changed the digest-bound active tuple before exact replay");
        require_trace(actions, "same-F active route-mismatch trace");

        EndpointIoControl stop_terminal_report;
        stop_terminal_report.close_before_write = MessageType::ERROR;
        require(run_pair(client, established, admit(client, input), stop).client.status ==
                        ClientRunStatus::Disconnected &&
                    client.has_active_transaction(),
                "terminal-report fixture did not retain a second active transaction");
        const ClientRunResult local_terminal = run_client_with_raw_peer(
            client, {},
            [&](tcp::acceptor& acceptor) {
                return raw_route_mismatch_peer(
                    acceptor,
                    ScriptedSessionState{.f_guid = f_guid,
                                         .route_present = true,
                                         .history_nonce = HistoryNonce{992},
                                         .next_rel_seq = RelSeq{0},
                                         .state_digest = icecc::digest128("third route")},
                    false);
            },
            stop_terminal_report);
        require(local_terminal.status == ClientRunStatus::TerminalError &&
                    local_terminal.observation != ClientRunObservation::ExactCommitObserved &&
                    client.has_active_transaction(),
                "failed terminal-report write relabeled a known route mismatch");
    }
}

void test_handshake_binding_and_namespace_rules() {
    {
        P50ServerEndpoint server(Id128::from_u64(610));
        const CStoreGuid c_guid = Id128::from_u64(611);
        const std::vector<uint8_t> input = bytes("pending survives incompatible HELLO\n");
        HelloRaceState coordination;
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> first_server =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<ServerRunResult> second_server =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<void> first_peer = asio::co_spawn(
            context, raw_pending_then_finish(acceptor.local_endpoint(), c_guid, input, coordination),
            asio::use_future);
        std::future<void> incompatible_peer = asio::co_spawn(
            context, raw_incompatible_hello(acceptor.local_endpoint(), c_guid, coordination),
            asio::use_future);
        context.run();
        first_peer.get();
        incompatible_peer.get();
        const ServerRunResult first = first_server.get();
        const ServerRunResult second = second_server.get();
        require(first.status == ServerRunStatus::Completed &&
                    second.status == ServerRunStatus::TerminalError &&
                    copy_input(server, c_guid) == input,
                "incompatible HELLO bound or changed the live namespace");
    }

    {
        P50ServerEndpoint server(Id128::from_u64(612));
        const CStoreGuid c_guid = Id128::from_u64(613);
        const std::vector<uint8_t> input =
            bytes("pending survives invalid compatible candidate\n");
        HelloRaceState coordination;
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> first_server =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<ServerRunResult> second_server =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<void> first_peer = asio::co_spawn(
            context, raw_pending_then_finish(acceptor.local_endpoint(), c_guid, input, coordination),
            asio::use_future);
        std::future<void> invalid_peer = asio::co_spawn(
            context, raw_invalid_first_mutation(acceptor.local_endpoint(), c_guid, coordination),
            asio::use_future);
        context.run();
        first_peer.get();
        invalid_peer.get();
        const ServerRunResult first = first_server.get();
        const ServerRunResult second = second_server.get();
        require(first.status == ServerRunStatus::Completed &&
                    second.status == ServerRunStatus::TerminalError &&
                    server.namespace_count() == 1 &&
                    copy_input(server, c_guid) == input,
                "invalid compatible candidate installed or replaced the live namespace");
    }

    {
        P50ServerEndpoint server(Id128::from_u64(614));
        const CStoreGuid c_guid = Id128::from_u64(615);
        HelloRaceState coordination{.first_pending = true};
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> server_result =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<void> peer_result = asio::co_spawn(
            context, raw_invalid_first_mutation(acceptor.local_endpoint(), c_guid, coordination),
            asio::use_future);
        context.run();
        peer_result.get();
        require(server_result.get().status == ServerRunStatus::TerminalError &&
                    server.namespace_count() == 0 && !copy_input(server, c_guid),
                "cold compatible candidate installed a namespace before validation");
    }

    {
        P50ServerEndpoint server(Id128::from_u64(616));
        const CStoreGuid c_guid = Id128::from_u64(617);
        require(force_route_reset(server, c_guid, HistoryNonce{110}).status ==
                    ServerRunStatus::Disconnected,
                "candidate-revision fixture did not establish its route");
        const std::vector<uint8_t> input =
            bytes("newly activated session invalidates an older snapshot\n");
        CandidateRevisionRace coordination;
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> first_server =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<ServerRunResult> second_server =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<void> stale_peer = asio::co_spawn(
            context, raw_stale_candidate(acceptor.local_endpoint(), c_guid, coordination),
            asio::use_future);
        std::future<void> current_peer = asio::co_spawn(
            context, raw_commit_after_candidate_staged(
                         acceptor.local_endpoint(), c_guid, input, coordination),
            asio::use_future);
        context.run();
        stale_peer.get();
        current_peer.get();
        require(first_server.get().status == ServerRunStatus::Disconnected &&
                    second_server.get().status == ServerRunStatus::Completed &&
                    server.namespace_count() == 1 &&
                    copy_input(server, c_guid) == input,
                "stale candidate replaced state from the newer activated session");
    }

    {
        const FStoreGuid guid = Id128::from_u64(620);
        P50ServerEndpoint established(guid);
        TestClient client(Id128::from_u64(621));
        EndpointIoControl stop;
        stop.close_after_write = MessageType::TX_BEGIN;
        const std::vector<uint8_t> input = bytes("same GUID missing namespace\n");
        const PairResult interrupted =
            run_pair(client, established, admit(client, input), stop);
        require(interrupted.client.status == ClientRunStatus::Disconnected &&
                    client.has_active_transaction(),
                "same-GUID inconsistency fixture lost its active transaction");

        P50ServerEndpoint inconsistent(guid);
        const PairResult refused = run_pair(client, inconsistent);
        require(refused.client.status == ClientRunStatus::TerminalError &&
                    client.has_active_transaction() &&
                    !copy_input(inconsistent, client.c_store_guid()),
                "same GUID with an absent established namespace was treated as cold");

        const PairResult recovered = run_pair(client, established);
        require(recovered.client.status == ClientRunStatus::Committed &&
                    copy_input(established, client.c_store_guid()) == input,
                "same-GUID inconsistency changed the retained retry identity");
    }

    {
        P50ServerEndpoint server(Id128::from_u64(630));
        const CStoreGuid c_guid = Id128::from_u64(631);
        require(force_route_reset(server, c_guid, HistoryNonce{500}).status ==
                    ServerRunStatus::Disconnected,
                "monotonic nonce fixture did not establish its route");
        require(force_rejected_route_reset(server, c_guid, HistoryNonce{500}),
                "reused HISTORY_NONCE was accepted");
        require(force_rejected_route_reset(server, c_guid, HistoryNonce{499}),
                "decreasing HISTORY_NONCE was accepted");
        require(force_route_reset(server, c_guid, HistoryNonce{501}).status ==
                    ServerRunStatus::Disconnected,
                "strictly increasing HISTORY_NONCE was rejected");
    }
}


// Port of the transplant lane's unique mask-law assertions onto the accepted
// production endpoint (bigoracle S1 transplant ruling, bounded convergence):
// a TX_BEGIN whose profile is intrinsically valid on the wire but outside the
// session's negotiated
// mask must be rejected by the endpoint's own negotiated-mask law -- bound by
// EXACT detail text, because the ZSTD_TU shape validator behind it rejects
// the same frame with a different text ("transaction is not a ZSTD_TU
// profile"), and std::invalid_argument IS-A std::logic_error so exception
// classes cannot separate the layers. The rejection must not move the route:
// a second connection then completes a transaction at the untouched cursor.
asio::awaitable<void> raw_unnegotiated_begin_rejected(tcp::endpoint remote,
                                                      CStoreGuid c_guid,
                                                      std::span<const uint8_t> input) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    RawRoute open = co_await raw_open(socket, remote, c_guid);
    if ((open.state.negotiated_profiles & profile_bit(ProfileId::P29V1)) != 0)
        throw std::logic_error("fixture requires P29V1 outside the negotiated mask");
    SessionState route = co_await raw_reset(socket, open, HistoryNonce{771});
    const ZstdTuEnvelope prepared = encode_zstd_tu(
        HistoryNonce{1}, RelSeq{0}, TuSeq{41}, Digest128{}, input);
    TxBegin begin = make_begin(prepared, route.history_nonce, route.state_digest,
                               route.next_rel_seq);
    begin.profile = ProfileId::P29V1;
    co_await raw_write(socket, begin);
    // No further traffic: a server that wrongly admits this begin sees EOF
    // with an open transaction and fails through a DIFFERENT detail text, so
    // the exact-text assertion below stays the discriminator without a hang.
    boost::system::error_code shutdown_error;
    socket.shutdown(tcp::socket::shutdown_send, shutdown_error);
    const Frame terminal = co_await raw_read(socket, route.limits.max_frame_payload);
    if (terminal.type != MessageType::ERROR)
        throw std::logic_error("unnegotiated TX_BEGIN did not receive terminal ERROR");
    const ErrorMessage rejection = raw_decode<ErrorMessage>(terminal);
    if (rejection.detail != "TX_BEGIN profile was not negotiated")
        throw std::logic_error(
            "unnegotiated TX_BEGIN was not rejected by the endpoint mask law: " +
            rejection.detail);
    boost::system::error_code ignored;
    socket.close(ignored);
    co_return;
}

asio::awaitable<void> raw_complete_at_preserved_cursor(tcp::endpoint remote,
                                                       CStoreGuid c_guid,
                                                       std::span<const uint8_t> input) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    RawRoute open = co_await raw_open(socket, remote, c_guid);
    if (!open.state.route_present || open.state.history_nonce != HistoryNonce{771} ||
        open.state.next_rel_seq != RelSeq{0})
        throw std::logic_error("rejected unnegotiated TX_BEGIN moved the endpoint route");
    const ZstdTuEnvelope prepared = encode_zstd_tu(
        HistoryNonce{1}, RelSeq{0}, TuSeq{42}, Digest128{}, input);
    const TxBegin begin = make_begin(prepared, open.state.history_nonce,
                                     open.state.state_digest, open.state.next_rel_seq);
    co_await raw_write(socket, begin);
    Message body_message = BodyMessage{prepared.body};
    co_await raw_write(socket, std::move(body_message));
    const TxCommit commit = raw_decode<TxCommit>(
        co_await raw_read(socket, open.state.limits.max_frame_payload));
    if (commit.transaction_digest != begin.transaction_digest ||
        commit.raw_digest != begin.raw_digest)
        throw std::logic_error("preserved-cursor completion received a different TX_COMMIT");
    boost::system::error_code ignored;
    socket.close(ignored);
    co_return;
}

void test_unnegotiated_begin_rejected_and_route_preserved() {
    P50ServerEndpoint server(Id128::from_u64(770));
    const CStoreGuid c_guid = Id128::from_u64(771);
    const std::vector<uint8_t> input = bytes("route survives unnegotiated TX_BEGIN\n");
    {
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> serving =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<void> peer = asio::co_spawn(
            context,
            raw_unnegotiated_begin_rejected(acceptor.local_endpoint(), c_guid, input),
            asio::use_future);
        context.run();
        peer.get();
        require(serving.get().status == ServerRunStatus::TerminalError,
                "unnegotiated TX_BEGIN left the server run non-terminal");
    }
    {
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> serving =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<void> peer = asio::co_spawn(
            context,
            raw_complete_at_preserved_cursor(acceptor.local_endpoint(), c_guid, input),
            asio::use_future);
        context.run();
        peer.get();
        require(serving.get().status == ServerRunStatus::Completed &&
                    copy_input(server, c_guid) == input,
                "post-rejection completion at the preserved cursor failed");
    }
}


// local-oracle's b19e41a9 HOLD closure: the client-side outbound mask law was
// a deletion survivor (session-level ResetAckMutation::ProfileMask rejects a
// bad SESSION_STATE long before the outbound guard; the client's own prepare
// path only mints ZSTD_TU begins, so no public flow reaches the clause). The
// law now lives in the shared require_outbound_profile_negotiated seam --
// called by the production send path and driven here directly with a COPIED
// begin against a live established session, mutating no retained state. The
// wire-level backstop for a call-site deletion remains the server's own mask
// law, deletion-tested separately.
void test_client_outbound_mask_law() {
    TestClient client(Id128::from_u64(910));
    P50ServerEndpoint server(Id128::from_u64(911));
    const std::vector<uint8_t> input = bytes("outbound mask law: first exact input\n");
    const PairResult first = run_pair(client.endpoint, server, admit(client, input));
    require(first.client.status == ClientRunStatus::Committed &&
                first.server.status == ServerRunStatus::Completed,
            "outbound-mask fixture could not establish its live session");

    const ZstdTuEnvelope shaped = encode_zstd_tu(
        HistoryNonce{1}, RelSeq{0}, TuSeq{77}, Digest128{}, input);
    TxBegin crafted = make_begin(shaped, HistoryNonce{1}, Digest128{});
    crafted.profile = ProfileId::P29V1;  // wire-valid, outside the TU-only mask
    bool rejected_by_the_mask_law = false;
    try {
        require_outbound_profile_negotiated(profile_bit(ProfileId::ZSTD_TU), crafted);
    } catch (const std::logic_error& error) {
        rejected_by_the_mask_law =
            std::string_view(error.what()) ==
            "C selected a profile outside the negotiated mask";
    }
    require(rejected_by_the_mask_law,
            "client outbound mask law did not reject an unnegotiated begin copy");
    require_outbound_profile_negotiated(profile_bit(ProfileId::ZSTD_TU) |
                                            profile_bit(ProfileId::P29V1),
                                        crafted);  // negotiated -> must not throw

    const std::vector<uint8_t> second_input = bytes("outbound mask law: follow-up commit\n");
    const PairResult second =
        run_pair(client.endpoint, server, admit(client, second_input));
    require(second.client.status == ClientRunStatus::Committed &&
                second.server.status == ServerRunStatus::Completed &&
                copy_input(server, Id128::from_u64(910)) == second_input,
            "outbound mask probe disturbed the live route");
}


// local-oracle's 414a917e HOLD closure: the production CLIENT CALLSITE of the
// outbound mask law, exercised end-to-end. The copied-begin control hook
// flips the coroutine's own outbound TX_BEGIN copy to wire-valid P29V1 after
// construction and before validation/send; the production validator must
// reject through the production call, the fake F must observe complete
// silence after route establishment (no TX_BEGIN/BODY frame ever arrives),
// and the SAME prepared work must then commit exactly on a clean retry.
// Deleting either the predicate clause or the production callsite makes the
// begin reach the peer, which trips the silence assertion below.
asio::awaitable<void> raw_established_then_silent_peer(tcp::acceptor& acceptor,
                                                       FStoreGuid f_guid) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    co_await acceptor.async_accept(socket, asio::use_awaitable);
    const SessionHello hello = raw_decode<SessionHello>(
        co_await raw_read(socket, kInitialMaxFramePayload));

    SessionState fresh;
    fresh.wire_revision = kP50WireRevision;
    fresh.negotiated_profiles = profile_bit(ProfileId::ZSTD_TU);
    fresh.limits = hello.limits;
    fresh.f_store_guid = f_guid;
    co_await raw_write(socket, fresh);

    const HistoryReset reset = raw_decode<HistoryReset>(
        co_await raw_read(socket, fresh.limits.max_frame_payload));
    SessionState ack = fresh;
    ack.namespace_present = true;
    ack.route_present = true;
    ack.history_nonce = reset.history_nonce;
    ack.next_rel_seq = RelSeq{0};
    ack.state_digest = reset.initial_state_digest;
    co_await raw_write(socket, ack);

    // The rejected outbound begin must never reach the wire: the next event
    // on this socket has to be the client closing it, not a frame.
    co_await raw_wait_for_close(socket);
}

void test_client_outbound_callsite_law() {
    TestClient client(Id128::from_u64(920));
    const std::vector<uint8_t> input = bytes("callsite law: prepared survives local rejection\n");
    const PreparedTuHandle prepared = admit(client, input);

    EndpointIoControl unnegotiated_control;
    unnegotiated_control.outbound_begin_transform = [](
        const TxBegin& begin, std::span<const uint8_t>) {
        TxBegin crafted = begin;
        crafted.profile = ProfileId::P29V1;  // wire-valid, unnegotiated
        return crafted;
    };
    const ClientRunResult rejected = run_client_with_raw_peer(
        client, prepared,
        [&](tcp::acceptor& acceptor) {
            return raw_established_then_silent_peer(acceptor, Id128::from_u64(921));
        },
        unnegotiated_control);
    require(rejected.status == ClientRunStatus::TerminalError &&
                rejected.terminal_error &&
                rejected.terminal_error->detail ==
                    "C selected a profile outside the negotiated mask",
            "production callsite did not reject the transformed unnegotiated begin");
    require(!client.has_active_transaction() || client.has_reconciliation_work(),
            "local outbound rejection left the client without recoverable work");

    P50ServerEndpoint good(Id128::from_u64(922));
    require(run_pair(client, good).client.status == ClientRunStatus::Committed &&
                copy_input(good, client.c_store_guid()) == input,
            "outbound callsite rejection lost or changed the prepared work");
}

void test_reserved_zero_endpoint_values() {
    {
        P50ServerEndpoint server(Id128::from_u64(628));
        asio::io_context context;
        tcp::acceptor acceptor(context,
                               {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> server_result =
            asio::co_spawn(context, server.accept_one(acceptor),
                           asio::use_future);
        std::future<void> peer_result = asio::co_spawn(
            context,
            raw_zero_c_store_hello(acceptor.local_endpoint()),
            asio::use_future);
        context.run();
        peer_result.get();
        require(server_result.get().status == ServerRunStatus::TerminalError &&
                    server.namespace_count() == 0,
                "zero C_STORE_GUID left an F namespace or route");

        TestClient recovered(Id128::from_u64(629));
        const std::vector<uint8_t> input =
            bytes("valid relationship after reserved zero C GUID\n");
        require(run_pair(recovered, server, admit(recovered, input))
                            .client.status == ClientRunStatus::Committed &&
                    copy_input(server, recovered.c_store_guid()) == input,
                "zero C_STORE_GUID changed the subsequent cold relationship");
    }

    {
        P50ServerEndpoint server(Id128::from_u64(632));
        const CStoreGuid c_guid = Id128::from_u64(633);
        asio::io_context context;
        tcp::acceptor acceptor(context,
                               {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> server_result =
            asio::co_spawn(context, server.accept_one(acceptor),
                           asio::use_future);
        std::future<void> peer_result = asio::co_spawn(
            context,
            raw_zero_history_reset(acceptor.local_endpoint(), c_guid),
            asio::use_future);
        context.run();
        peer_result.get();
        require(server_result.get().status == ServerRunStatus::TerminalError &&
                    server.namespace_count() == 0 &&
                    !copy_input(server, c_guid),
                "zero HISTORY_NONCE left an F namespace or route");

        TestClient recovered(c_guid);
        const std::vector<uint8_t> input =
            bytes("valid reset after reserved zero nonce\n");
        require(run_pair(recovered, server, admit(recovered, input))
                            .client.status == ClientRunStatus::Committed &&
                    copy_input(server, c_guid) == input,
                "zero HISTORY_NONCE changed the subsequent cold relationship");
    }

    {
        TestClient client(Id128::from_u64(634));
        const std::vector<uint8_t> input =
            bytes("reserved zero F_STORE_GUID\n");
        const ClientRunResult terminal = run_client_with_raw_peer(
            client, admit(client, input), [](tcp::acceptor& acceptor) {
                return raw_zero_f_store_state_peer(acceptor);
            });
        require(terminal.status == ClientRunStatus::TerminalError &&
                    terminal.observation != ClientRunObservation::ExactCommitObserved &&
                    client.has_reconciliation_work() &&
                    !client.has_active_transaction() &&
                    !client.endpoint.f_store_guid() &&
                    client.endpoint.next_rel_seq() == RelSeq{0} &&
                    client.endpoint.state_digest() == Digest128{},
                "zero F_STORE_GUID left C relationship state");
        P50ServerEndpoint good(Id128::from_u64(635));
        require(run_pair(client, good).client.status ==
                        ClientRunStatus::Committed &&
                    copy_input(good, client.c_store_guid()) == input,
                "zero F_STORE_GUID changed queued retry work");
    }

    {
        TestClient client(Id128::from_u64(636));
        const std::vector<uint8_t> input = bytes("reserved zero ERROR code\n");
        const ClientRunResult terminal = run_client_with_raw_peer(
            client, admit(client, input), [](tcp::acceptor& acceptor) {
                return raw_zero_error_code_peer(acceptor);
            });
        require(terminal.status == ClientRunStatus::TerminalError &&
                    terminal.observation != ClientRunObservation::ExactCommitObserved && terminal.terminal_error &&
                    terminal.terminal_error->code != 0 &&
                    client.has_reconciliation_work() &&
                    !client.has_active_transaction() &&
                    !client.endpoint.f_store_guid() &&
                    client.endpoint.next_rel_seq() == RelSeq{0} &&
                    client.endpoint.state_digest() == Digest128{},
                "zero ERROR code left C relationship state");
        P50ServerEndpoint good(Id128::from_u64(637));
        require(run_pair(client, good).client.status ==
                        ClientRunStatus::Committed &&
                    copy_input(good, client.c_store_guid()) == input,
                "zero ERROR code changed queued retry work");
    }
}

void test_interrupted_begin_identity() {
    ActionTrace actions;
    P50ServerEndpoint server(Id128::from_u64(640), {}, nullptr, &actions);
    const CStoreGuid c_guid = Id128::from_u64(641);
    const std::vector<uint8_t> input = pseudo_random_bytes(8192);

    InterruptedRawTu interrupted;
    {
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> server_result =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<InterruptedRawTu> peer_result = asio::co_spawn(
            context,
            raw_partial_body_disconnect(acceptor.local_endpoint(), c_guid, input),
            asio::use_future);
        context.run();
        interrupted = peer_result.get();
        require(server_result.get().status == ServerRunStatus::Disconnected &&
                    !copy_input(server, c_guid),
                "partial BODY disconnect materialized or committed input");
    }

    {
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> server_result =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<void> peer_result = asio::co_spawn(
            context,
            raw_different_begin_rejected(acceptor.local_endpoint(), c_guid,
                                         interrupted.begin),
            asio::use_future);
        context.run();
        peer_result.get();
        require(server_result.get().status == ServerRunStatus::TerminalError &&
                    !copy_input(server, c_guid),
                "different begin at the interrupted cursor replaced retained identity");
    }

    {
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> server_result =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<void> peer_result = asio::co_spawn(
            context,
            raw_exact_whole_tu_replay(acceptor.local_endpoint(), c_guid, interrupted),
            asio::use_future);
        context.run();
        peer_result.get();
        require(server_result.get().status == ServerRunStatus::Completed &&
                    copy_input(server, c_guid) == input,
                "exact whole-TU replay did not commit after partial BODY disconnect");
    }

    require(std::any_of(actions.records().begin(), actions.records().end(),
                        [&](const ActionRecord& record) {
                            return record.actor == ActorSide::F &&
                                   record.action == ActionType::ACTIVE_REPLAYED &&
                                   record.transaction_digest ==
                                       interrupted.begin.transaction_digest;
                        }),
            "exact whole-TU replay omitted the retained transaction identity");
}

void test_terminal_body_failure_identity() {
    const std::vector<uint8_t> input = pseudo_random_bytes(8192);
    uint64_t identity = 650;
    for (const auto& [scenario, label] :
         std::array{std::pair{TerminalBodyFailure::ZeroProgress,
                              "zero-progress BODY"},
                    std::pair{TerminalBodyFailure::Excess,
                              "excess BODY"},
                    std::pair{TerminalBodyFailure::ComponentDigest,
                              "component digest"}}) {
        const FStoreGuid f_guid = Id128::from_u64(identity++);
        CompletionLog completions;
        P50ServerEndpoint server(f_guid, {}, &completions);
        const CStoreGuid c_guid = Id128::from_u64(identity++);
        auto failure = force_terminal_body_failure(server, c_guid, input, scenario);
        InterruptedRawTu interrupted = std::move(failure.first);
        ServerRunResult terminal = std::move(failure.second);
        require(terminal.status == ServerRunStatus::TerminalError &&
                    !copy_input(server, c_guid),
                std::string(label) +
                    " failure materialized or committed input");
        require(std::any_of(
                    completions.completions().begin(),
                    completions.completions().end(),
                    [&](const AsyncCompletion& completion) {
                        const CompletionStamp& stamp = completion.stamp;
                        return stamp.actor == ActorSide::F &&
                               stamp.operation == AsyncOperationKind::WriteFragment &&
                               stamp.c_store_guid == c_guid &&
                               stamp.f_store_guid == f_guid &&
                               stamp.session_serial == terminal.session_serial &&
                               stamp.history_nonce == interrupted.begin.history_nonce &&
                               stamp.rel_seq == interrupted.begin.rel_seq &&
                               stamp.tu_seq == interrupted.begin.tu_seq &&
                               stamp.transaction_digest ==
                                   interrupted.begin.transaction_digest &&
                               stamp.raw_digest == interrupted.begin.raw_digest &&
                               stamp.transaction_bound;
                    }),
                std::string(label) +
                    " terminal completion lost the installed operation identity");
        require(force_different_begin_rejected(server, c_guid,
                                               interrupted.begin)
                        .status == ServerRunStatus::TerminalError &&
                    !copy_input(server, c_guid),
                std::string(label) +
                    " failure did not retain the exact TX_BEGIN identity");
        require(force_exact_whole_tu_replay(server, c_guid, interrupted, true)
                            .status == ServerRunStatus::Completed &&
                    copy_input(server, c_guid) == input,
                std::string(label) +
                    " failure did not accept exact whole-TU replay");
    }

    {
        P50ServerEndpoint server(Id128::from_u64(identity++));
        const CStoreGuid c_guid = Id128::from_u64(identity++);
        const auto [interrupted, terminal] = force_terminal_body_failure(
            server, c_guid, input, TerminalBodyFailure::ZeroProgress, false);
        require(terminal.status == ServerRunStatus::TerminalError &&
                    !copy_input(server, c_guid),
                "lost terminal report changed the interrupted transaction");
        require(force_exact_whole_tu_replay(server, c_guid, interrupted, true)
                            .status == ServerRunStatus::Completed &&
                    copy_input(server, c_guid) == input,
                "lost terminal report did not permit exact whole-TU replay");
    }

    for (const auto& [scenario, label] :
         std::array{std::pair{TerminalBodyFailure::ZstdDecode,
                              "Zstd decode"},
                    std::pair{TerminalBodyFailure::RawDigest,
                              "raw digest"}}) {
        P50ServerEndpoint server(Id128::from_u64(identity++));
        const CStoreGuid c_guid = Id128::from_u64(identity++);
        const auto [interrupted, terminal] = force_terminal_body_failure(
            server, c_guid, input, scenario);
        require(terminal.status == ServerRunStatus::TerminalError &&
                    !copy_input(server, c_guid),
                std::string(label) +
                    " failure materialized or committed input");
        require(force_different_begin_rejected(server, c_guid,
                                               interrupted.begin)
                        .status == ServerRunStatus::TerminalError,
                std::string(label) +
                    " failure accepted a different TX_BEGIN at the retained cursor");
        require(force_exact_whole_tu_replay(server, c_guid, interrupted, false)
                            .status == ServerRunStatus::TerminalError &&
                    !copy_input(server, c_guid),
                std::string(label) +
                    " failure did not retain its deterministic exact identity");
    }
}

void test_disconnect_at_each_message_boundary() {
    const std::array<MessageType, 4> client_messages{
        MessageType::SESSION_HELLO, MessageType::HISTORY_RESET, MessageType::TX_BEGIN,
        MessageType::BODY};
    for (size_t index = 0; index != client_messages.size(); ++index) {
        P50ServerEndpoint server(Id128::from_u64(700 + index));
        TestClient client(Id128::from_u64(710 + index));
        EndpointIoControl stop;
        stop.close_after_write = client_messages[index];
        const std::vector<uint8_t> input = bytes("boundary disconnect\n");
        const PairResult result = run_pair(client, server, admit(client, input), stop);
        require(result.client.status == ClientRunStatus::Disconnected,
                "client message-boundary close did not stop the dialogue");
        if (client_messages[index] == MessageType::TX_BEGIN ||
            client_messages[index] == MessageType::BODY)
            require(client.has_active_transaction(),
                    "uncertain client boundary erased active identity");
        const PairResult recovered = run_pair(client, server);
        require(recovered.client.status == ClientRunStatus::Committed &&
                    copy_input(server, client.c_store_guid()) == input,
                "client message-boundary reconnect did not commit exact input");
    }

    const std::array<MessageType, 1> server_messages{MessageType::SESSION_STATE};
    for (size_t index = 0; index != server_messages.size(); ++index) {
        P50ServerEndpoint server(Id128::from_u64(720 + index));
        TestClient client(Id128::from_u64(730 + index));
        EndpointIoControl stop;
        stop.close_after_write = server_messages[index];
        const std::vector<uint8_t> input = bytes("server boundary\n");
        const PairResult result = run_pair(client, server, admit(client, input), {}, stop);
        require(result.client.status == ClientRunStatus::Disconnected,
                "server message-boundary close did not stop the dialogue");
        const PairResult recovered = run_pair(client, server);
        require(recovered.client.status == ClientRunStatus::Committed &&
                    copy_input(server, client.c_store_guid()) == input,
                "server message-boundary reconnect did not commit exact input");
    }

    {
        P50ServerEndpoint server(Id128::from_u64(740));
        TestClient client(Id128::from_u64(741));
        EndpointIoControl stop;
        stop.close_after_write = MessageType::TX_COMMIT;
        const std::vector<uint8_t> input = bytes("close after final commit frame\n");
        const PairResult result = run_pair(client, server, admit(client, input), {}, stop);
        require(result.client.status == ClientRunStatus::Committed &&
                    copy_input(server, client.c_store_guid()) == input,
                "close after the complete TX_COMMIT changed the committed result");
    }
}

void test_client_operation_scoped_cancellation() {
    const std::vector<uint8_t> input = bytes("operation scoped cancellation\n");

    {
        TestClient client(Id128::from_u64(742));
        EndpointIoControl control;
        control.before_first_remote_write = [&client] {
            client.endpoint.request_cancel_for_test();
        };
        asio::io_context context;
        // A listening socket is sufficient for connect completion; the
        // deterministic hook cancels before the first CacheWire write.
        tcp::acceptor acceptor(context,
                               {asio::ip::address_v4::loopback(), 0});
        std::future<ClientRunResult> run = asio::co_spawn(
            context,
            client.endpoint.run(acceptor.local_endpoint(),
                                admit(client, input), control),
            asio::use_future);
        context.run();
        const ClientRunResult cancelled = run.get();
        require(cancelled.status == ClientRunStatus::Disconnected &&
                    cancelled.cancellation ==
                        ClientCancellationDisposition::ReconcileRequired &&
                    (client.has_active_transaction() ||
                     client.has_reconciliation_work()),
                "pre-wire C cancellation was incorrectly settled locally");
    }

    {
        TestClient client(Id128::from_u64(743));
        const PreparedTuHandle prepared = admit(client, input);
        asio::io_context context;
        tcp::acceptor acceptor(context,
                               {asio::ip::address_v4::loopback(), 0});
        std::future<void> peer = asio::co_spawn(
            context, raw_cancel_client_after_hello(acceptor, client.endpoint),
            asio::use_future);
        std::future<ClientRunResult> run = asio::co_spawn(
            context,
            client.endpoint.run(acceptor.local_endpoint(), prepared, {}),
            asio::use_future);
        context.run();
        peer.get();
        const ClientRunResult cancelled = run.get();
        require(cancelled.status == ClientRunStatus::Disconnected &&
                    cancelled.cancellation ==
                        ClientCancellationDisposition::ReconcileRequired &&
                    client.has_reconciliation_work(),
                "post-hello C cancellation silently authorized an abort");

        P50ServerEndpoint server(Id128::from_u64(744));
        const PairResult reconciled = run_pair(client, server);
        require(reconciled.client.status == ClientRunStatus::Committed &&
                    reconciled.client.cancellation ==
                        ClientCancellationDisposition::None &&
                    !client.has_reconciliation_work() &&
                    copy_input(server, client.c_store_guid()) == input,
                "post-hello C cancellation did not preserve exact retry work");
    }
}

void test_component_and_allocation_caps() {
    EndpointCaps ordinary;
    {
        const auto [peer, server] = run_raw_server_case(ordinary, RawCase::ShortBody, 800);
        require(!peer.received_error && server.status == ServerRunStatus::Disconnected,
                "short component was allowed to materialize or commit");
    }
    {
        const auto [peer, server] = run_raw_server_case(ordinary, RawCase::ExcessBody, 801);
        require(peer.received_error && peer.closed_after_error &&
                    server.status == ServerRunStatus::TerminalError,
                "excess component did not terminate the message session");
    }
    for (const auto& [scenario, name, id] :
         std::array{std::tuple{RawCase::EmptyBodyProgress, "zero-progress BODY", uint64_t{805}},
                    std::tuple{RawCase::ComponentDigest, "component digest", uint64_t{806}},
                    std::tuple{RawCase::TransactionDigest, "transaction digest", uint64_t{807}},
                    std::tuple{RawCase::RawDigest, "raw digest", uint64_t{808}},
                    std::tuple{RawCase::WrongProfile, "profile", uint64_t{809}},
                    std::tuple{RawCase::WrongEncoding, "encoding", uint64_t{811}},
                    std::tuple{RawCase::FrameContentSize, "zstd frame size", uint64_t{812}},
                    std::tuple{RawCase::TrailingByte, "trailing zstd byte", uint64_t{813}},
                    std::tuple{RawCase::AppendedEmptyFrame, "appended empty zstd frame",
                               uint64_t{814}},
                    std::tuple{RawCase::AppendedNonemptyFrame, "appended nonempty zstd frame",
                               uint64_t{815}}}) {
        const auto [peer, server] = run_raw_server_case(ordinary, scenario, id);
        require(peer.received_error && peer.closed_after_error &&
                    server.status == ServerRunStatus::TerminalError,
                std::string("invalid ") + name + " did not close terminally");
    }

    EndpointCaps tight;
    tight.zstd.max_encoded_body_bytes = 32;
    tight.zstd.max_raw_bytes = 128;
    {
        const auto [peer, server] = run_raw_server_case(tight, RawCase::DescriptorCap, 802);
        require(peer.received_error && peer.closed_after_error &&
                    server.status == ServerRunStatus::TerminalError,
                "encoded-component descriptor cap was not enforced before data");
    }
    {
        const auto [peer, server] = run_raw_server_case(tight, RawCase::DecodedCap, 803);
        require(peer.received_error && peer.closed_after_error &&
                    server.status == ServerRunStatus::TerminalError,
                "decoded-input cap was not enforced before expansion");
    }

    EndpointCaps frame_tight;
    frame_tight.wire.max_frame_payload = kMandatoryControlFramePayload;
    {
        const auto [peer, server] = run_raw_server_case(frame_tight, RawCase::FrameCap, 804);
        require(peer.received_error && peer.closed_after_error &&
                    peer.error_payload_bytes <= kMandatoryControlFramePayload &&
                    server.status == ServerRunStatus::TerminalError,
                "declared frame cap did not reject before payload allocation");
    }


    EndpointCaps below_control;
    below_control.wire.max_frame_payload = kMandatoryControlFramePayload - 1;
    require_throws<std::invalid_argument>(
        [&] { TestClient rejected(Id128::from_u64(820), below_control); },
        "151-byte client frame cap was accepted");
    require_throws<std::invalid_argument>(
        [&] { P50ServerEndpoint rejected(Id128::from_u64(821), below_control); },
        "151-byte server frame cap was accepted");

    EndpointCaps exact_control;
    exact_control.wire.max_frame_payload = kMandatoryControlFramePayload;
    for (bool client_is_tight : {false, true}) {
        const EndpointCaps client_caps = client_is_tight ? exact_control : ordinary;
        const EndpointCaps server_caps = client_is_tight ? ordinary : exact_control;
        TestClient client(Id128::from_u64(830 + client_is_tight), client_caps);
        P50ServerEndpoint server(Id128::from_u64(840 + client_is_tight), server_caps);
        const std::vector<uint8_t> input = pseudo_random_bytes(4096);
        const PairResult result = run_pair(client, server, admit(client, input));
        require(result.client.status == ClientRunStatus::Committed &&
                    copy_input(server, client.c_store_guid()) == input,
                "asymmetric exact-152-byte negotiation did not complete");
    }
}

void test_two_client_one_server_isolation() {
    ActionTrace actions;
    CompletionLog completions;
    P50ServerEndpoint server(Id128::from_u64(860), {}, &completions, &actions);
    TestClient first(Id128::from_u64(861), {}, HistoryNonce{201},
                     &completions, &actions);
    TestClient second(Id128::from_u64(862), {}, HistoryNonce{301},
                      &completions, &actions);
    const std::vector<uint8_t> first_input =
        bytes("C-one exact input on shared F\n");
    const std::vector<uint8_t> second_input =
        bytes("C-two independent exact input on shared F\n");
    const PreparedTuHandle first_prepared = admit(first, first_input);
    const PreparedTuHandle second_prepared = admit(second, second_input);

    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    std::future<ServerRunResult> first_server =
        asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
    std::future<ServerRunResult> second_server =
        asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
    std::future<ClientRunResult> first_client = asio::co_spawn(
        context, first.endpoint.run(acceptor.local_endpoint(), first_prepared),
        asio::use_future);
    std::future<ClientRunResult> second_client = asio::co_spawn(
        context, second.endpoint.run(acceptor.local_endpoint(), second_prepared),
        asio::use_future);
    context.run();

    require(first_client.get().status == ClientRunStatus::Committed &&
                second_client.get().status == ClientRunStatus::Committed &&
                first_server.get().status == ServerRunStatus::Completed &&
                second_server.get().status == ServerRunStatus::Completed &&
                server.namespace_count() == 2 &&
                copy_input(server, first.c_store_guid()) == first_input &&
                copy_input(server, second.c_store_guid()) == second_input &&
                first.endpoint.next_rel_seq() == RelSeq{1} &&
                second.endpoint.next_rel_seq() == RelSeq{1},
            "concurrent C2F1 dialogues crossed namespace or route state");

    const std::vector<uint8_t> first_followup =
        bytes("C-one advances without changing C-two\n");
    const PairResult advanced =
        run_pair(first, server, admit(first, first_followup));
    require(advanced.client.status == ClientRunStatus::Committed &&
                server.namespace_count() == 2 &&
                copy_input(server, first.c_store_guid()) == first_followup &&
                copy_input(server, second.c_store_guid()) == second_input &&
                first.endpoint.next_rel_seq() == RelSeq{2} &&
                second.endpoint.next_rel_seq() == RelSeq{1},
            "one C route advance changed the other C namespace");

    bool first_f_completion = false;
    bool second_f_completion = false;
    for (const AsyncCompletion& completion : completions.completions()) {
        if (completion.stamp.actor != ActorSide::F)
            continue;
        first_f_completion = first_f_completion ||
                             completion.stamp.c_store_guid == first.c_store_guid();
        second_f_completion = second_f_completion ||
                              completion.stamp.c_store_guid == second.c_store_guid();
    }
    require(first_f_completion && second_f_completion,
            "C2F1 completion identities did not retain both C namespaces");
    require_trace(actions, "C2F1 endpoint-isolation trace");
}

void report_zstd1_metrics(bool enforce_performance_floor) {
    constexpr size_t input_bytes = size_t{16} << 20;
    constexpr unsigned iterations = 4;
    std::vector<uint8_t> input(input_bytes);
    static constexpr std::string_view line =
        "template<class T> inline T p50_value(T value) { return value + 1; }\n";
    for (size_t offset = 0; offset != input.size();) {
        const size_t count = std::min(line.size(), input.size() - offset);
        std::copy_n(line.begin(), count, input.begin() + offset);
        offset += count;
    }
    for (size_t offset = 4096; offset < input.size(); offset += 4096)
        input[offset] ^= static_cast<uint8_t>(offset >> 12);

    ZstdTuCodec codec(1);
    ZstdTuEnvelope prepared;
    const auto encode_start = std::chrono::steady_clock::now();
    for (unsigned iteration = 0; iteration != iterations; ++iteration)
        prepared = codec.encode(HistoryNonce{1}, RelSeq{0}, TuSeq{iteration}, Digest128{},
                                input, {uint64_t{64} << 20, uint64_t{64} << 20});
    const auto encode_stop = std::chrono::steady_clock::now();
    TxBegin begin = make_begin(prepared, HistoryNonce{1},
                               initial_route_digest(Id128::from_u64(1), HistoryNonce{1}));
    std::vector<uint8_t> decoded;
    const auto decode_start = std::chrono::steady_clock::now();
    for (unsigned iteration = 0; iteration != iterations; ++iteration)
        decoded = codec.decode(begin, prepared.body,
                               {uint64_t{64} << 20, uint64_t{64} << 20});
    const auto decode_stop = std::chrono::steady_clock::now();
    require(decoded == input, "Zstd1 metric loop did not decode exactly");

    const double encoded_seconds =
        std::chrono::duration<double>(encode_stop - encode_start).count();
    const double decoded_seconds =
        std::chrono::duration<double>(decode_stop - decode_start).count();
    const double total_gb = static_cast<double>(input.size()) * iterations / 1'000'000'000.0;
    const double encode_gbps = total_gb / encoded_seconds;
    const double decode_gbps = total_gb / decoded_seconds;
    std::cout << "p50_endpoint_test: zstd1 raw_bytes=" << input.size()
              << " compressed_bytes=" << prepared.body.size() << " encode_GBps=" << encode_gbps
              << " decode_GBps=" << decode_gbps << '\n';
    if (enforce_performance_floor)
        require(encode_gbps >= 0.5 && decode_gbps >= 0.5,
                "quietbox Zstd1 throughput is below 0.5 GB/s");
}

} // namespace


// ===== same_commit negative coverage: ordinary commit path =====
// local-oracle ruling (#16, 2026-08-23T16:54Z): a well-formed fake F must not be able to
// close C's active transaction with a TxCommit differing from the live TxBegin in any of the
// six identity fields.  Each mismatch must be rejected exactly, leave the active transaction
// unchanged, and permit a later exact retry to commit.  The baseline (exact commit) proves the
// fake F speaks the protocol; without it a broken fake would fake-pass the matrix.

enum class CommitMismatchField {
    None,
    HistoryNonce,
    RelSeq,
    TuSeq,
    TransactionDigest,
    RawDigest,
    PostStateDigest,
};

TxCommit correct_commit_for(const TxBegin& begin) {
    TxCommit commit;
    commit.history_nonce = begin.history_nonce;
    commit.rel_seq = begin.rel_seq;
    commit.tu_seq = begin.tu_seq;
    commit.transaction_digest = begin.transaction_digest;
    commit.raw_digest = begin.raw_digest;
    commit.post_state_digest =
        compute_post_state_digest(begin.pre_state_digest, begin.history_nonce, begin.rel_seq,
                                  begin.tu_seq, begin.transaction_digest);
    return commit;
}

void mutate_commit(TxCommit& commit, CommitMismatchField field) {
    switch (field) {
    case CommitMismatchField::None:
        break;
    case CommitMismatchField::HistoryNonce:
        commit.history_nonce.value += 1;
        break;
    case CommitMismatchField::RelSeq:
        commit.rel_seq.value += 1;
        break;
    case CommitMismatchField::TuSeq:
        commit.tu_seq.value += 1;
        break;
    case CommitMismatchField::TransactionDigest:
        commit.transaction_digest.bytes[0] ^= 0x80;
        break;
    case CommitMismatchField::RawDigest:
        commit.raw_digest.bytes[0] ^= 0x80;
        break;
    case CommitMismatchField::PostStateDigest:
        commit.post_state_digest.bytes[0] ^= 0x80;
        break;
    }
}

// A raw fake F: accepts one real-client connection, performs the exact fresh-route dance
// (HELLO -> fresh SESSION_STATE -> HISTORY_RESET -> bound acknowledgement), consumes the
// TX_BEGIN and BODY components, then answers with a commit derived from the received begin
// with exactly one field mutated (or none for the baseline), and waits for the peer to close.
asio::awaitable<void> raw_f_serve_and_commit(tcp::acceptor& acceptor, FStoreGuid f_guid,
                                             CommitMismatchField field, TxBegin& out_begin,
                                             std::vector<uint8_t>* out_body = nullptr) {
    tcp::socket socket = co_await acceptor.async_accept(asio::use_awaitable);
    const SessionHello hello =
        raw_decode<SessionHello>(co_await raw_read(socket, kInitialMaxFramePayload));
    const SessionSelection selection =
        negotiate_session(hello, kP50WireRevision, kKnownProfileMask);
    const uint32_t cap = selection.limits.max_frame_payload;
    SessionState fresh;
    fresh.wire_revision = selection.wire_revision;
    fresh.negotiated_profiles = selection.negotiated_profiles;
    fresh.limits = selection.limits;
    fresh.f_store_guid = f_guid;
    co_await raw_write(socket, Message{fresh});
    Frame next = co_await raw_read(socket, cap);
    TxBegin& begin = out_begin;
    if (next.type == MessageType::HISTORY_RESET) {
        const HistoryReset reset = raw_decode<HistoryReset>(next);
        SessionState ack = fresh;
        ack.namespace_present = true;
        ack.route_present = true;
        ack.history_nonce = reset.history_nonce;
        ack.next_rel_seq = RelSeq{0};
        ack.state_digest = reset.initial_state_digest;
        co_await raw_write(socket, Message{ack});
        begin = raw_decode<TxBegin>(co_await raw_read(socket, cap));
    } else {
        begin = raw_decode<TxBegin>(next);
    }
    uint64_t body_bytes = 0;
    while (body_bytes < begin.body.encoded_bytes) {
        const BodyMessage body = raw_decode<BodyMessage>(co_await raw_read(socket, cap));
        if (body.bytes.empty())
            break;
        body_bytes += body.bytes.size();
        if (out_body)
            out_body->insert(out_body->end(), body.bytes.begin(), body.bytes.end());
    }
    TxCommit commit = correct_commit_for(begin);
    mutate_commit(commit, field);
    co_await raw_write(socket, Message{commit});
    co_await raw_wait_for_close(socket);
    co_return;
}


// Ordinary same-F exact retry (bigoracle endpoint-HOLD shape B): after a mismatch refusal the
// SAME F presents the unchanged retained route; the client must replay the IDENTICAL TxBegin
// and BODY and commit with reconnect == ExactMatch.  The fake itself throws if C resets,
// rebinds, or reconstructs any begin/body identity.
asio::awaitable<void> raw_f_exact_retry(tcp::acceptor& acceptor, FStoreGuid same_f_guid,
                                        TxBegin captured_begin,
                                        std::vector<uint8_t> captured_body) {
    tcp::socket socket = co_await acceptor.async_accept(asio::use_awaitable);
    const SessionHello hello =
        raw_decode<SessionHello>(co_await raw_read(socket, kInitialMaxFramePayload));
    const SessionSelection selection =
        negotiate_session(hello, kP50WireRevision, kKnownProfileMask);
    SessionState peer;
    peer.wire_revision = selection.wire_revision;
    peer.negotiated_profiles = selection.negotiated_profiles;
    peer.limits = selection.limits;
    peer.f_store_guid = same_f_guid;
    peer.namespace_present = true;
    peer.route_present = true;
    peer.history_nonce = captured_begin.history_nonce;
    peer.next_rel_seq = captured_begin.rel_seq;
    peer.state_digest = captured_begin.pre_state_digest;
    co_await raw_write(socket, Message{peer});
    const TxBegin replayed =
        raw_decode<TxBegin>(co_await raw_read(socket, peer.limits.max_frame_payload));
    if (replayed != captured_begin)
        throw std::logic_error("ordinary retry changed retained TxBegin identity");
    std::vector<uint8_t> replay_body;
    while (replay_body.size() < replayed.body.encoded_bytes) {
        BodyMessage part = raw_decode<BodyMessage>(
            co_await raw_read(socket, peer.limits.max_frame_payload));
        if (part.bytes.empty())
            break;
        replay_body.insert(replay_body.end(), part.bytes.begin(), part.bytes.end());
    }
    if (replay_body != captured_body)
        throw std::logic_error("ordinary retry changed retained BODY bytes");
    co_await raw_write(socket, Message{correct_commit_for(captured_begin)});
    co_await raw_wait_for_close(socket);
    co_return;
}

void test_commit_identity_negative_matrix() {
    const std::array cases{
        std::pair{CommitMismatchField::HistoryNonce, "history nonce"},
        std::pair{CommitMismatchField::RelSeq, "REL_SEQ"},
        std::pair{CommitMismatchField::TuSeq, "TU_SEQ"},
        std::pair{CommitMismatchField::TransactionDigest, "transaction digest"},
        std::pair{CommitMismatchField::RawDigest, "raw digest"},
        std::pair{CommitMismatchField::PostStateDigest, "post-state digest"},
    };
    uint64_t identity = 7000;

    {
        TestClient client(Id128::from_u64(identity++));
        const std::vector<uint8_t> input = pseudo_random_bytes(4096);
        const PreparedTuHandle prepared = admit(client, input);
        TxBegin baseline_begin;
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<void> fake = asio::co_spawn(
            context,
            raw_f_serve_and_commit(acceptor, Id128::from_u64(identity++),
                                   CommitMismatchField::None, baseline_begin),
            asio::use_future);
        std::future<ClientRunResult> run = asio::co_spawn(
            context, client.endpoint.run(acceptor.local_endpoint(), prepared, {}),
            asio::use_future);
        context.run();
        fake.get();
        const ClientRunResult baseline = run.get();
        require(baseline.status == ClientRunStatus::Committed && !client.has_active_transaction(),
                "raw fake F baseline exact commit was not accepted");
    }

    for (const auto& [field, name] : cases) {
        // Client A: refusal -> SAME-F exact retry (the REQUIRED exact-retry close: identical
        // TxBegin and BODY replayed on the unchanged route, commit with ExactMatch).
        {
            TestClient client(Id128::from_u64(identity++));
            const std::vector<uint8_t> input = pseudo_random_bytes(4096);
            const PreparedTuHandle prepared = admit(client, input);
            TxBegin observed_begin;
            std::vector<uint8_t> observed_body;
            const FStoreGuid fake_f = Id128::from_u64(identity++);
            {
                asio::io_context context;
                tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
                std::future<void> fake = asio::co_spawn(
                    context,
                    raw_f_serve_and_commit(acceptor, fake_f, field, observed_begin,
                                           &observed_body),
                    asio::use_future);
                std::future<ClientRunResult> run = asio::co_spawn(
                    context, client.endpoint.run(acceptor.local_endpoint(), prepared, {}),
                    asio::use_future);
                context.run();
                fake.get();
                const ClientRunResult rejected = run.get();
                require(rejected.status == ClientRunStatus::TerminalError &&
                            client.has_active_transaction() &&
                            client.endpoint.next_rel_seq() == observed_begin.rel_seq &&
                            client.endpoint.state_digest() ==
                                observed_begin.pre_state_digest &&
                            client.endpoint.f_store_guid() ==
                                std::optional<FStoreGuid>(fake_f),
                        std::string(name) +
                            " commit mismatch rejection did not preserve the transaction cursor");
            }
            {
                asio::io_context context;
                tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
                std::future<void> fake = asio::co_spawn(
                    context,
                    raw_f_exact_retry(acceptor, fake_f, observed_begin, observed_body),
                    asio::use_future);
                std::future<ClientRunResult> run = asio::co_spawn(
                    context, client.endpoint.run(acceptor.local_endpoint(), {}, {}),
                    asio::use_future);
                context.run();
                fake.get();
                const ClientRunResult retried = run.get();
                require(retried.status == ClientRunStatus::Committed &&
                            retried.reconnect == EndpointReconnectOutcome::ExactMatch &&
                            !client.has_active_transaction(),
                        std::string(name) +
                            " refusal did not permit the same-F exact retry to commit");
            }
        }
        // Client B: refusal -> different-F ColdFStore row, retained as a DISTINCT control:
        // prepared work survives rejection across an incarnation replacement.
        {
            TestClient client(Id128::from_u64(identity++));
            const std::vector<uint8_t> input = pseudo_random_bytes(4096);
            const PreparedTuHandle prepared = admit(client, input);
            TxBegin observed_begin;
            const FStoreGuid fake_f = Id128::from_u64(identity++);
            asio::io_context context;
            tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
            std::future<void> fake = asio::co_spawn(
                context,
                raw_f_serve_and_commit(acceptor, fake_f, field, observed_begin),
                asio::use_future);
            std::future<ClientRunResult> run = asio::co_spawn(
                context, client.endpoint.run(acceptor.local_endpoint(), prepared, {}),
                asio::use_future);
            context.run();
            fake.get();
            require(run.get().status == ClientRunStatus::TerminalError &&
                        client.has_active_transaction(),
                    std::string(name) + " (cold-F row) mismatch was not refused");
            P50ServerEndpoint real_server(Id128::from_u64(identity++));
            const PairResult retried = run_pair(client, real_server);
            require(retried.client.status == ClientRunStatus::Committed &&
                        retried.client.reconnect == EndpointReconnectOutcome::ColdFStore &&
                        !client.has_active_transaction() &&
                        copy_input(real_server, client.c_store_guid()) == input,
                    std::string(name) +
                        " prepared work did not survive incarnation replacement");
        }
    }
}


// ===== same_commit negative coverage: lost-final reconnect path =====
// Ruling part 2: after a lost final commit, a fake F presenting a well-formed
// SessionState.last_commit differing from the active TxBegin in any identity field must be
// refused (no LostFinalAcknowledgement), must leave the active transaction in place, and an
// exact subsequent reconciliation must commit.  The presented peer state is kept
// self-consistent (state_digest == last_commit.post_state_digest, next_rel_seq == begin+1,
// route nonce echoed) so only the same_commit comparison against the live begin can object.

asio::awaitable<void> raw_f_receive_then_drop(tcp::acceptor& acceptor, FStoreGuid f_guid,
                                              TxBegin& out_begin) {
    tcp::socket socket = co_await acceptor.async_accept(asio::use_awaitable);
    const SessionHello hello =
        raw_decode<SessionHello>(co_await raw_read(socket, kInitialMaxFramePayload));
    const SessionSelection selection =
        negotiate_session(hello, kP50WireRevision, kKnownProfileMask);
    const uint32_t cap = selection.limits.max_frame_payload;
    SessionState fresh;
    fresh.wire_revision = selection.wire_revision;
    fresh.negotiated_profiles = selection.negotiated_profiles;
    fresh.limits = selection.limits;
    fresh.f_store_guid = f_guid;
    co_await raw_write(socket, Message{fresh});
    Frame next = co_await raw_read(socket, cap);
    if (next.type == MessageType::HISTORY_RESET) {
        const HistoryReset reset = raw_decode<HistoryReset>(next);
        SessionState ack = fresh;
        ack.namespace_present = true;
        ack.route_present = true;
        ack.history_nonce = reset.history_nonce;
        ack.next_rel_seq = RelSeq{0};
        ack.state_digest = reset.initial_state_digest;
        co_await raw_write(socket, Message{ack});
        next = co_await raw_read(socket, cap);
    }
    out_begin = raw_decode<TxBegin>(next);
    uint64_t body_bytes = 0;
    while (body_bytes < out_begin.body.encoded_bytes) {
        const BodyMessage body = raw_decode<BodyMessage>(co_await raw_read(socket, cap));
        if (body.bytes.empty())
            break;
        body_bytes += body.bytes.size();
    }
    // Lost final: the F "commits" but the acknowledgement never reaches C.
    boost::system::error_code ignored;
    socket.shutdown(tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
    co_return;
}

asio::awaitable<void> raw_f_present_last_commit(tcp::acceptor& acceptor, FStoreGuid f_guid,
                                                TxBegin begin, CommitMismatchField field) {
    tcp::socket socket = co_await acceptor.async_accept(asio::use_awaitable);
    const SessionHello hello =
        raw_decode<SessionHello>(co_await raw_read(socket, kInitialMaxFramePayload));
    const SessionSelection selection =
        negotiate_session(hello, kP50WireRevision, kKnownProfileMask);
    SessionState peer;
    peer.wire_revision = selection.wire_revision;
    peer.negotiated_profiles = selection.negotiated_profiles;
    peer.limits = selection.limits;
    peer.f_store_guid = f_guid;
    peer.namespace_present = true;
    peer.route_present = true;
    TxCommit last = correct_commit_for(begin);
    mutate_commit(last, field);
    // Wire rule: a retained commit must be internally consistent with its route
    // (nonce equal, next_rel = commit.rel + 1, state digest = commit post-state), so the
    // presented state derives from the (possibly mutated) commit.  Nonce/REL_SEQ mismatches
    // are then refused by the client's route checks; TU_SEQ and the digests are decided by
    // same_commit alone.
    peer.history_nonce = last.history_nonce;
    peer.next_rel_seq = RelSeq{last.rel_seq.value + 1};
    peer.state_digest = last.post_state_digest;
    peer.last_commit = last;
    co_await raw_write(socket, Message{peer});
    // Acceptance closes silently; refusal first sends a terminal ERROR frame.  Consume
    // whatever arrives until the peer closes.
    try {
        for (;;)
            (void)co_await raw_read(socket, peer.limits.max_frame_payload);
    } catch (const boost::system::system_error&) {
        // peer closed
    }
    co_return;
}

void test_lost_final_commit_identity_negative_matrix() {
    const std::array cases{
        std::pair{CommitMismatchField::HistoryNonce, "history nonce"},
        std::pair{CommitMismatchField::RelSeq, "REL_SEQ"},
        std::pair{CommitMismatchField::TuSeq, "TU_SEQ"},
        std::pair{CommitMismatchField::TransactionDigest, "transaction digest"},
        std::pair{CommitMismatchField::RawDigest, "raw digest"},
        std::pair{CommitMismatchField::PostStateDigest, "post-state digest"},
    };
    uint64_t identity = 7100;

    // Control: the exact last_commit closes the lost-final window.
    {
        TestClient client(Id128::from_u64(identity++));
        const FStoreGuid f_guid = Id128::from_u64(identity++);
        const std::vector<uint8_t> input = pseudo_random_bytes(4096);
        const PreparedTuHandle prepared = admit(client, input);
        TxBegin begin;
        {
            asio::io_context context;
            tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
            std::future<void> fake = asio::co_spawn(
                context, raw_f_receive_then_drop(acceptor, f_guid, begin), asio::use_future);
            std::future<ClientRunResult> run = asio::co_spawn(
                context, client.endpoint.run(acceptor.local_endpoint(), prepared, {}),
                asio::use_future);
            context.run();
            fake.get();
            const ClientRunResult dropped = run.get();
            require(dropped.status == ClientRunStatus::Disconnected &&
                        client.has_active_transaction(),
                    "lost-final setup did not leave an active transaction");
        }
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<void> fake = asio::co_spawn(
            context,
            raw_f_present_last_commit(acceptor, f_guid, begin, CommitMismatchField::None),
            asio::use_future);
        std::future<ClientRunResult> run = asio::co_spawn(
            context, client.endpoint.run(acceptor.local_endpoint(), {}, {}), asio::use_future);
        context.run();
        fake.get();
        const ClientRunResult accepted = run.get();
        require(accepted.status == ClientRunStatus::Committed &&
                    accepted.reconnect == EndpointReconnectOutcome::LostFinalAcknowledgement &&
                    !client.has_active_transaction(),
                "exact lost-final acknowledgement was not accepted");
    }

    for (const auto& [field, name] : cases) {
        TestClient client(Id128::from_u64(identity++));
        const FStoreGuid f_guid = Id128::from_u64(identity++);
        const std::vector<uint8_t> input = pseudo_random_bytes(4096);
        const PreparedTuHandle prepared = admit(client, input);
        TxBegin begin;
        {
            asio::io_context context;
            tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
            std::future<void> fake = asio::co_spawn(
                context, raw_f_receive_then_drop(acceptor, f_guid, begin), asio::use_future);
            std::future<ClientRunResult> run = asio::co_spawn(
                context, client.endpoint.run(acceptor.local_endpoint(), prepared, {}),
                asio::use_future);
            context.run();
            fake.get();
            require(run.get().status == ClientRunStatus::Disconnected &&
                        client.has_active_transaction(),
                    std::string(name) + " lost-final setup did not retain the transaction");
        }
        {
            const std::optional<FStoreGuid> f_before = client.endpoint.f_store_guid();
            asio::io_context context;
            tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
            std::future<void> fake = asio::co_spawn(
                context, raw_f_present_last_commit(acceptor, f_guid, begin, field),
                asio::use_future);
            std::future<ClientRunResult> run = asio::co_spawn(
                context, client.endpoint.run(acceptor.local_endpoint(), {}, {}),
                asio::use_future);
            context.run();
            fake.get();
            const ClientRunResult refused = run.get();
            // RouteHistoryReset is the SITE's refusal shape (:1508); a bypassed site would
            // fall through to accept()'s internal check, which throws with the default
            // reconnect outcome instead -- so this assertion proves the call site itself.
            // Cursor preservation: the refusal must leave rel/state/route untouched.
            require(refused.status == ClientRunStatus::TerminalError &&
                        refused.reconnect == EndpointReconnectOutcome::RouteHistoryReset &&
                        client.has_active_transaction() &&
                        client.endpoint.next_rel_seq() == begin.rel_seq &&
                        client.endpoint.state_digest() == begin.pre_state_digest &&
                        client.endpoint.f_store_guid() == f_before,
                    std::string(name) +
                        " mismatched last_commit refusal did not preserve the cursor");
        }
        {
            asio::io_context context;
            tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
            std::future<void> fake = asio::co_spawn(
                context,
                raw_f_present_last_commit(acceptor, f_guid, begin, CommitMismatchField::None),
                asio::use_future);
            std::future<ClientRunResult> run = asio::co_spawn(
                context, client.endpoint.run(acceptor.local_endpoint(), {}, {}),
                asio::use_future);
            context.run();
            fake.get();
            const ClientRunResult reconciled = run.get();
            require(reconciled.status == ClientRunStatus::Committed &&
                        reconciled.reconnect ==
                            EndpointReconnectOutcome::LostFinalAcknowledgement &&
                        !client.has_active_transaction(),
                    std::string(name) + " refusal blocked the exact reconciliation");
        }
    }
}

int main(int argc, char** argv) {
    const bool performance_gate = argc == 2 && std::string_view(argv[1]) == "--performance";
    if (argc > 2 || (argc == 2 && !performance_gate))
        fail("usage: p50endpoint [--performance]");
    if (std::getenv("ICECC_P50_ENDPOINT_CODEC_QUEUE_FOCUS") != nullptr) {
        test_p5co_codec_queue_is_bounded();
        std::cout << "p50_endpoint_test: focused codec queue PASS\n";
        return 0;
    }
    if (std::getenv("ICECC_P50_P5CO_WORKER_CREDIT_PIN_FOCUS") != nullptr) {
        test_p5co_cancel_keeps_materializing_bytes_charged();
        std::cout << "p50_endpoint_test: focused materialization worker credit pin PASS\n";
        return 0;
    }
    if (std::getenv("ICECC_P50_ENDPOINT_MUTANT_FOCUS") != nullptr) {
        test_complete_p5co_endpoint_handoff();
        test_p5co_endpoint_absolute_deadline_and_binding();
        test_server_completion_rechecks_after_live_callback();
        test_p5co_endpoint_fences_post_transfer_failures();
        test_p5co_worker_completion_is_stale_after_deadline_or_cancel();
        test_p5co_codec_queue_is_bounded();
        test_p5co_deadline_wins_after_owner_job_selector();
        test_client_completion_deadline_is_fresh_before_first_write();
        std::cout << "p50_endpoint_test: focused P5CO PASS\n";
        return 0;
    }
    if (std::getenv("ICECC_P50_P29_DIALOGUE_FOCUS") != nullptr) {
        test_p29v1_endpoint_route_dialogue_lifetime();
        std::cout << "p50_endpoint_test: focused P29V1 dialogue PASS\n";
        return 0;
    }
    if (std::getenv("ICECC_P50_PROFILE_DIGEST_MUTANT_FOCUS") != nullptr) {
        test_profile_materialized_result_digest_gates();
        std::cout << "p50_endpoint_test: focused profile digest gates PASS\n";
        return 0;
    }
    if (std::getenv("ICECC_P50_R2_ENDPOINT_FOCUS") != nullptr) {
        test_r2_endpoint_commits_two_jobs_on_one_link();
        std::cout << "p50_endpoint_test: focused R2 persistent-link PASS\n";
        return 0;
    }
    if (std::getenv("ICECC_P50_R2_WIRE_ACCOUNTING_FOCUS") != nullptr) {
        test_r2_wire_accounting_interval_conservation();
        std::cout << "p50_endpoint_test: focused R2 accounting PASS\n";
        return 0;
    }
    if (std::getenv("ICECC_P50_P5CO_WORKER_HISTORY_PIN_FOCUS") != nullptr) {
        test_r2_persistent_history_charge_survives_worker_reset();
        std::cout << "p50_endpoint_test: focused P29/ROUTE held-worker reset PASS\n";
        return 0;
    }
    if (std::getenv("ICECC_P50_R2_PEER_CLOSE_WORKER_FOCUS") != nullptr) {
        test_r2_peer_close_during_materialization();
        std::cout << "p50_endpoint_test: focused P29/ROUTE peer-close worker PASS\n";
        return 0;
    }
    if (std::getenv("ICECC_P50_R2_DEADLINE_STAGES_FOCUS") != nullptr) {
        test_r2_deadline_expiry_stages();
        std::cout << "p50_endpoint_test: focused R2 deadline stages PASS\n";
        return 0;
    }
    if (std::getenv("ICECC_P50_R2_REPLACED_GENERATION_FOCUS") != nullptr) {
        test_r2_store_replaced_rejects_same_guid_old_generation();
        std::cout << "p50_endpoint_test: focused same-GUID stale-generation reject PASS\n";
        return 0;
    }
    if (std::getenv("ICECC_P50_R2_MISSING_RESERVATION_FOCUS") != nullptr) {
        test_r2_definite_missing_reservation_is_typed_only_for_absence();
        std::cout << "p50_endpoint_test: focused definite ReservationMissing PASS\n";
        return 0;
    }
    if (std::getenv("ICECC_P50_R2_SETUP_CANCEL_FOCUS") != nullptr) {
        test_r2_silent_setup_cancelled_before_hello();
        std::cout << "p50_endpoint_test: focused R2 pre-HELLO cancellation PASS\n";
        return 0;
    }
    if (std::getenv("ICECC_P50_R2_FRAGMENT_SUCCESS_FOCUS") != nullptr) {
        test_r2_fragmented_one_job_each_profile();
        std::cout << "p50_endpoint_test: focused fragmented R2 success all profiles PASS\n";
        return 0;
    }
    if (std::getenv("ICECC_P50_R2_FRAGMENT_FRAME_RECOVERY_FOCUS") != nullptr ||
        std::getenv("ICECC_P50_R2_FRAGMENT_BODY_RECOVERY_FOCUS") != nullptr) {
        test_r2_fragmented_frame_interruption_recovery();
        std::cout << "p50_endpoint_test: focused fragmented R2 frame recovery all profiles PASS\n";
        return 0;
    }
    if (std::getenv("ICECC_P50_R2_ROUTE_RECOVERY_FOCUS") != nullptr) {
        test_zstd_route_recovery_rebuild_cursor();
        std::cout << "p50_endpoint_test: focused ZSTD_ROUTE recovery rebuild PASS\n";
        return 0;
    }
    if (std::getenv("ICECC_P50_R2_W30_FOCUS") != nullptr) {
        test_r2_endpoint_window30_receipts_and_refill(ProfileId::ZSTD_TU);
        std::cout << "p50_endpoint_test: focused R2 W30/refill ZSTD_TU PASS\n";
        return 0;
    }
    if (std::getenv("ICECC_P50_R2_W30_P29_FOCUS") != nullptr) {
        test_r2_endpoint_window30_receipts_and_refill(ProfileId::P29V1);
        std::cout << "p50_endpoint_test: focused R2 W30/refill P29 PASS\n";
        return 0;
    }
    if (std::getenv("ICECC_P50_R2_W30_ROUTE_FOCUS") != nullptr) {
        test_r2_endpoint_window30_receipts_and_refill(ProfileId::ZSTD_ROUTE);
        std::cout << "p50_endpoint_test: focused R2 W30/refill ZSTD_ROUTE PASS\n";
        return 0;
    }
    if (std::getenv("ICECC_P50_R2_W30_ALL_FOCUS") != nullptr) {
        test_r2_endpoint_window30_receipts_and_refill(ProfileId::ZSTD_TU);
        test_r2_endpoint_window30_receipts_and_refill(ProfileId::P29V1);
        test_r2_endpoint_window30_receipts_and_refill(ProfileId::ZSTD_ROUTE);
        std::cout << "p50_endpoint_test: focused R2 W30/refill all profiles PASS\n";
        return 0;
    }
    test_action_hold_rendezvous();
    test_normal_zero_and_completion_stamps();
    test_completion_stamp_correspondence();
    test_completion_live_identity_correspondence();
    test_commit_identity_negative_matrix();
    test_lost_final_commit_identity_negative_matrix();
    test_idempotent_prepare_admission();
    test_preparation_authority_window_refill_and_receipts();
    test_zstd_route_recovery_rebuild_cursor();
    test_r2_store_replaced_rejects_same_guid_old_generation();
    test_r2_definite_missing_reservation_is_typed_only_for_absence();
    test_r2_endpoint_commits_two_jobs_on_one_link();
    test_r2_wire_accounting_interval_conservation();
    test_r2_persistent_history_charge_survives_worker_reset();
    test_r2_peer_close_during_materialization();
    test_r2_deadline_expiry_stages();
    test_r2_fragmented_one_job_each_profile();
    test_r2_fragmented_frame_interruption_recovery();
    test_candidate_stage_has_no_revision_residue();
    test_input_record_owner_and_aggregate_limits();
    test_fragmentation_at_every_control_and_body_boundary();
    test_exact_replay_and_lost_final();
    test_completion_identity_and_store_replacement();
    test_same_f_route_reset();
    test_reset_ack_equality_and_terminal_result();
    test_handshake_binding_and_namespace_rules();
    test_unnegotiated_begin_rejected_and_route_preserved();
    test_client_outbound_mask_law();
    test_client_outbound_callsite_law();
    test_reserved_zero_endpoint_values();
    test_interrupted_begin_identity();
    test_terminal_body_failure_identity();
    test_disconnect_at_each_message_boundary();
    test_client_operation_scoped_cancellation();
    test_component_and_allocation_caps();
    test_client_deadline_and_direct_socket_ownership();
    test_client_completion_deadline_is_fresh_before_first_write();
    test_complete_p5co_endpoint_handoff();
    test_p5co_endpoint_absolute_deadline_and_binding();
    test_server_completion_rechecks_after_live_callback();
    test_p5co_endpoint_fences_post_transfer_failures();
    test_p5co_worker_completion_is_stale_after_deadline_or_cancel();
    test_p5co_cancel_keeps_materializing_bytes_charged();
    test_p5co_codec_queue_is_bounded();
    test_p5co_deadline_wins_after_owner_job_selector();
    test_adopted_endpoint_exact_zstd_and_ownership();
    test_adopted_endpoint_disconnect_and_invalid_rows();
    test_adopted_cross_executor_releases_registration();
    test_two_client_one_server_isolation();
    test_zstd_route_endpoint_continuation_and_retry();
    test_zstd_route_authority_bounded_history();
    test_server_routes_are_isolated_by_profile();
    test_p29v1_endpoint_route_dialogue_lifetime();
    test_live_global_resource_trace();
    test_automatic_action_trace_is_complete_past_1024_records();
    if (s3_resource_storm_requested())
        test_s3_resource_storm_product_path();
    report_zstd1_metrics(performance_gate);
    std::cout << "p50_endpoint_test: PASS\n";
    return 0;
}
