#include "client/p50_zstd_sender.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_future.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <exception>
#include <iostream>
#include <iterator>
#include <limits>
#include <mutex>
#include <netinet/in.h>
#include <stdexcept>
#include <set>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace icecc::p50;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

namespace {

void require(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
}

void require(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what);
}

void add_bytes(uint64_t& total, uint64_t delta) {
    require(delta <= std::numeric_limits<uint64_t>::max() - total,
            "byte accounting overflow");
    total += delta;
}

uint64_t percentile_us(std::vector<uint64_t> samples, unsigned percentile) {
    require(!samples.empty() && percentile > 0 && percentile <= 100,
            "invalid latency percentile input");
    std::sort(samples.begin(), samples.end());
    const size_t rank = (samples.size() * percentile + 99) / 100;
    return samples[rank - 1];
}

struct IoThreadGuard {
    asio::io_context& c_context;
    asio::io_context& f_context;
    std::thread& c_thread;
    std::thread& f_thread;
    ~IoThreadGuard() {
        if (c_thread.joinable() || f_thread.joinable()) {
            c_context.stop();
            f_context.stop();
            if (c_thread.joinable()) c_thread.join();
            if (f_thread.joinable()) f_thread.join();
        }
    }
};

struct DelayedFinalizerGate {
    DelayedFinalizerGate(size_t batch_size,
                         std::chrono::steady_clock::time_point deadline)
        : batch_size(batch_size), deadline(deadline) {}

    asio::awaitable<void> wait() {
        const auto executor = co_await asio::this_coro::executor;
        auto timer = std::make_shared<asio::steady_timer>(executor);
        timer->expires_at(deadline);
        std::vector<std::shared_ptr<asio::steady_timer>> release;
        {
            std::lock_guard lock(mutex);
            waiting.push_back(timer);
            if (waiting.size() == batch_size) {
                release.swap(waiting);
                released_batches.fetch_add(1, std::memory_order_relaxed);
            }
        }
        for (const auto& waiter : release) {
            if (waiter == timer) {
                waiter->expires_at(asio::steady_timer::time_point::min());
            } else {
                boost::system::error_code ignored;
                waiter->cancel(ignored);
            }
        }
        boost::system::error_code error;
        co_await timer->async_wait(asio::redirect_error(asio::use_awaitable,
                                                        error));
        require(error == asio::error::operation_aborted ||
                    (!error && std::chrono::steady_clock::now() < deadline &&
                     released_batches.load(std::memory_order_relaxed) != 0),
                "delayed finalizer gate timed out before all callers arrived");
    }

    const size_t batch_size;
    const std::chrono::steady_clock::time_point deadline;
    std::mutex mutex;
    std::vector<std::shared_ptr<asio::steady_timer>> waiting;
    std::atomic<size_t> released_batches{0};
};

void drain_ready(asio::io_context& context) {
    context.restart();
    size_t total = 0;
    for (;;) {
        const size_t count = context.poll();
        if (count == 0) return;
        require(count <= 1000000 - total, "unexpectedly large executor drain");
        total += count;
    }
}

template <class T>
std::future<T> co_spawn_future(asio::io_context& context,
                               asio::awaitable<T> operation) {
    auto promise = std::make_shared<std::promise<T>>();
    auto result = promise->get_future();
    asio::co_spawn(context, std::move(operation),
        asio::bind_executor(context.get_executor(),
            [promise](std::exception_ptr error, T value) mutable {
                if (error) promise->set_exception(std::move(error));
                else promise->set_value(std::move(value));
            }));
    return result;
}

std::future<void> co_spawn_future(asio::io_context& context,
                                  asio::awaitable<void> operation) {
    auto promise = std::make_shared<std::promise<void>>();
    auto result = promise->get_future();
    asio::co_spawn(context, std::move(operation),
        asio::bind_executor(context.get_executor(),
            [promise](std::exception_ptr error) mutable {
                if (error) promise->set_exception(std::move(error));
                else promise->set_value();
            }));
    return result;
}

void verify_missing_gate_participant_times_out() {
    asio::io_context context;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(50);
    auto gate = std::make_shared<DelayedFinalizerGate>(2, deadline);
    auto waiter = co_spawn_future(context, gate->wait());
    context.run();
    bool timed_out = false;
    try {
        waiter.get();
    } catch (const std::runtime_error& error) {
        timed_out = std::string_view(error.what()).find("timed out") !=
                    std::string_view::npos;
    }
    require(timed_out,
            "missing delayed-finalizer participant did not fail boundedly");
}

ClaimAttemptCapability128 capability(uint8_t seed) {
    ClaimAttemptCapability128 value;
    for (size_t i = 0; i < value.bytes.size(); ++i)
        value.bytes[i] = static_cast<uint8_t>(seed + i);
    return value;
}

std::pair<CStoreGuid, FStoreGuid> store_guids() {
    std::array<uint8_t, 16> c{}, f{};
    for (size_t i = 0; i < 16; ++i) {
        c[i] = static_cast<uint8_t>(7 + i);
        f[i] = static_cast<uint8_t>(71 + i);
    }
    c[kStoreIdentityRoleByte] &= static_cast<uint8_t>(~kStoreIdentityRoleMask);
    f[kStoreIdentityRoleByte] |= kStoreIdentityRoleMask;
    return {CStoreGuid{c}, FStoreGuid{f}};
}

P51SourceArmedFields make_armed(CStoreGuid c, FStoreGuid f, size_t i,
                                uint32_t window, ProfileId profile) {
    P51SourceArmFields arm;
    arm.source.wire_job_id = static_cast<uint32_t>(1000 + i);
    arm.source.assignment_epoch = 3;
    arm.source.assignment_nonce = 10000 + i;
    arm.source.selected_f_host = "127.0.0.1";
    arm.source.selected_f_ordinary_port = 42001;
    arm.source.selected_f_cache_port = 42002;
    arm.source.cache_protocol = 2;
    arm.source.cache_profile = profile == ProfileId::P29V1 ? CACHE_PROFILE_P29V1
        : profile == ProfileId::ZSTD_ROUTE ? CACHE_PROFILE_ZSTD_ROUTE
                                           : CACHE_PROFILE_ZSTD_TU;
    arm.source.logical_job = 2000 + i;
    arm.source.compiler_attempt = 3000 + i;
    arm.source.c_store_generation = 11;
    arm.source.c_store_derivation_version = kStoreIdentityDerivationVersion;
    arm.source.c_store_guid = c.bytes;
    arm.source.source_request_id = 10000 + i;
    arm.source.source_mode = profile == ProfileId::P29V1 ? P50_SOURCE_MODE_P29V1
        : profile == ProfileId::ZSTD_ROUTE ? P50_SOURCE_MODE_ZSTD_ROUTE
                                           : P50_SOURCE_MODE_ZSTD_TU;
    arm.source.c_control_generation = 12;
    arm.source.c_control_attempt = 13;
    arm.requested_window = window;
    P51SourceArmedFields out;
    out.arm = std::move(arm);
    out.f_control_generation = 21;
    out.f_control_attempt = 22;
    out.f_store_generation = 23;
    out.f_store_guid = f.bytes;
    out.f_store_derivation_version = kStoreIdentityDerivationVersion;
    out.arm_observation_id = 24;
    out.source_budget_msec = 60000;
    out.attempt_capability_1 = capability(31);
    out.attempt_capability_2 = capability(51);
    out.reservation_id = Id128::from_u64(0x520000 + i).bytes;
    out.logical_relationship_id = Id128::from_u64(0x5102).bytes;
    out.relationship_epoch = 25;
    out.selected_revision = CACHE_WIRE_REVISION_R2;
    out.selected_window = window;
    require(out.valid(), "invalid benchmark ARM");
    return out;
}

int connect_fd(tcp::endpoint remote) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(remote.port());
    auto bytes = remote.address().to_v4().to_bytes();
    std::memcpy(&addr.sin_addr, bytes.data(), bytes.size());
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

asio::awaitable<ServerRunResult> accept_r2(tcp::acceptor& acceptor,
                                            P50ServerEndpoint& endpoint) {
    tcp::socket socket(co_await asio::this_coro::executor);
    co_await acceptor.async_accept(socket, asio::use_awaitable);
    co_return co_await endpoint.run_adopted_r2(std::move(socket));
}

asio::awaitable<ServerRunResult> accept_r2_owned(
    std::shared_ptr<tcp::acceptor> acceptor,
    std::shared_ptr<P50ServerEndpoint> endpoint) {
    // run_adopted_r2's session guard stores the endpoint Impl address. Keep
    // both endpoint and acceptor alive through deferred frame destruction.
    co_return co_await accept_r2(*acceptor, *endpoint);
}

asio::awaitable<void> accept_r1_batch(tcp::acceptor& acceptor,
                                      P50ServerEndpoint& endpoint, size_t jobs) {
    for (size_t i = 0; i < jobs; ++i) {
        const ServerRunResult result = co_await endpoint.accept_one(acceptor);
        require(result.status == ServerRunStatus::Completed,
                "R1 endpoint did not complete a source transaction");
    }
}

asio::awaitable<void> accept_r1_batch_owned(
    std::shared_ptr<tcp::acceptor> acceptor,
    std::shared_ptr<P50ServerEndpoint> endpoint, size_t jobs) {
    co_await accept_r1_batch(*acceptor, *endpoint, jobs);
}

asio::awaitable<ZstdSourceTransferResult> transfer_p51_route_owned(
    std::shared_ptr<P50ZstdSourceSender> sender,
    P51SourceArmedFields armed, uint64_t physical_generation,
    AsyncConnectedFdFactory connector, PrepareRequestKey request,
    std::chrono::steady_clock::time_point deadline,
    std::span<const uint8_t> source) {
    // The sender member coroutine stores a raw Impl pointer in its frame.
    // Keep the owning sender alive until that inner frame (and its request
    // guard) has been destroyed, even if co_spawn defers outer-frame cleanup.
    co_return co_await sender->transfer_p51_route(
        std::move(armed), physical_generation, std::move(connector), request,
        deadline, source);
}

struct R1BatchMeasurements {
    std::vector<ZstdSourceTransferResult> results;
    std::vector<uint64_t> latency_us;
    std::array<int64_t, 3> pass_elapsed_ms{};
    std::array<int64_t, 3> pass_process_cpu_ms{};
};

struct R1CommitMetrics {
    std::mutex mutex;
    std::array<std::chrono::steady_clock::time_point, 3> pass_started_at{};
    std::array<uint64_t, 3> first_commit_us{};
};

asio::awaitable<R1BatchMeasurements> transfer_r1_serial_batch(
    std::shared_ptr<P50ZstdSourceSender> sender, AsyncConnectedFdFactory connector,
    const std::vector<std::vector<uint8_t>>& input,
    const std::vector<std::vector<uint8_t>>* edited_input,
    R1CommitMetrics* commit_metrics,
    std::chrono::steady_clock::time_point deadline) {
    const size_t passes = edited_input ? 3 : 2;
    R1BatchMeasurements measurements;
    measurements.results.reserve(input.size() * passes);
    measurements.latency_us.reserve(input.size() * passes);
    for (size_t pass = 0; pass != passes; ++pass) {
        const auto started = std::chrono::steady_clock::now();
        const std::clock_t cpu_started = std::clock();
        {
            std::lock_guard lock(commit_metrics->mutex);
            commit_metrics->pass_started_at[pass] = started;
        }
        for (size_t i = 0; i < input.size(); ++i) {
            const size_t job = pass * input.size() + i;
            const auto& bytes = pass == 2 ? (*edited_input)[i] : input[i];
            const auto transfer_started = std::chrono::steady_clock::now();
            auto result = co_await sender->transfer_route(
                connector, PrepareRequestKey{3, 20000 + job}, deadline, bytes);
            measurements.latency_us.push_back(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - transfer_started).count()));
            measurements.results.push_back(std::move(result));
        }
        measurements.pass_elapsed_ms[pass] =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
        measurements.pass_process_cpu_ms[pass] = static_cast<int64_t>(
            (std::clock() - cpu_started) * 1000.0 / CLOCKS_PER_SEC);
    }
    co_return measurements;
}

std::vector<std::string> read_manifest(const std::string& path, size_t count) {
    std::ifstream in(path);
    require(static_cast<bool>(in), "cannot open corpus manifest");
    std::vector<std::string> paths;
    for (std::string line; paths.size() < count && std::getline(in, line);)
        if (!line.empty() && line.front() != '#') paths.push_back(line);
    require(paths.size() == count, "manifest has fewer entries than requested");
    return paths;
}

std::vector<std::string> read_and_validate_pair(const std::string& a_manifest,
                                                const std::string& b_manifest,
                                                const std::string& a_root,
                                                const std::string& b_root,
                                                size_t count) {
    auto a = read_manifest(a_manifest, count);
    auto b = read_manifest(b_manifest, count);
    require(!a_root.empty() && !b_root.empty(), "paired path roots must be explicit");
    auto relative_id = [](const std::string& path, const std::string& root) {
        auto has_dot_component = [](const std::filesystem::path& value) {
            for (const auto& component : value)
                if (component == "." || component == "..") return true;
            return false;
        };
        const std::filesystem::path root_path(root);
        require(root_path.is_absolute() && !has_dot_component(root_path),
                "paired path root must be absolute and contain no dot components");
        const std::string prefix = root.back() == '/' ? root : root + '/';
        require(path.starts_with(prefix), "paired path is outside its declared root");
        const std::string id = path.substr(prefix.size());
        require(!id.empty(), "paired path has an empty relative identity");
        const std::filesystem::path id_path(id);
        require(!id_path.is_absolute() && !has_dot_component(id_path) &&
                    id_path.lexically_normal().generic_string() == id,
                "paired translation-unit identity is not a canonical relative path");
        return id;
    };
    std::set<std::string> unique_ids;
    for (size_t i = 0; i < count; ++i) {
        const std::string a_id = relative_id(a[i], a_root);
        const std::string b_id = relative_id(b[i], b_root);
        require(unique_ids.insert(a_id).second, "paired manifest repeats a TU identity");
        require(a_id == b_id,
                "paired manifests have different translation-unit identity/order at index " +
                    std::to_string(i));
    }
    return b;
}

std::vector<std::vector<uint8_t>> load_inputs(const std::vector<std::string>& paths) {
    constexpr uint64_t kAggregateRawCap = 512ULL << 20;
    std::vector<std::vector<uint8_t>> inputs;
    inputs.reserve(paths.size());
    uint64_t aggregate = 0;
    for (const auto& path : paths) {
        std::error_code error;
        const uint64_t length = std::filesystem::file_size(path, error);
        require(!error && length != 0 && length <= (64ULL << 20),
                "corpus entry missing, empty, or over the 64 MiB per-input cap");
        require(length <= kAggregateRawCap - aggregate,
                "corpus exceeds the 512 MiB aggregate raw cap");
        std::ifstream in(path, std::ios::binary);
        require(static_cast<bool>(in), "cannot open preprocessed corpus input");
        inputs.emplace_back(static_cast<size_t>(length));
        in.read(reinterpret_cast<char*>(inputs.back().data()),
                static_cast<std::streamsize>(length));
        require(in.gcount() == static_cast<std::streamsize>(length) &&
                    in.peek() == std::char_traits<char>::eof(),
                "corpus file changed while reading");
        aggregate += length;
    }
    return inputs;
}

std::vector<std::vector<uint8_t>> make_finalizer_regression_inputs() {
    std::vector<std::vector<uint8_t>> inputs;
    inputs.reserve(32);
    for (uint32_t job = 0; job < 32; ++job) {
        std::vector<uint8_t> bytes(1024 + job * 13);
        uint32_t state = 0x9e3779b9U ^ (job * 0x85ebca6bU);
        for (uint8_t& byte : bytes) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            byte = static_cast<uint8_t>(state >> 24);
        }
        inputs.push_back(std::move(bytes));
    }
    return inputs;
}

void smoke_r2(const std::vector<std::vector<uint8_t>>& input,
              ProfileId profile, uint32_t window,
              std::chrono::steady_clock::duration maximum_duration =
                  std::chrono::minutes(4),
              bool delay_finalizers_for_regression = false,
              const std::vector<std::vector<uint8_t>>* edited_input = nullptr) {
    require(!input.empty() && input.size() > window, "smoke needs jobs > window");
    require(!edited_input || edited_input->size() == input.size(),
            "edited pass must have the same number of TUs");
    const size_t active_passes = edited_input ? 3 : 2;
    const auto bytes_for_job = [&](size_t job) -> const std::vector<uint8_t>& {
        const size_t pass = job / input.size();
        const size_t item = job % input.size();
        return pass == 2 ? (*edited_input)[item] : input[item];
    };
    const auto [c_guid, f_guid] = store_guids();
    std::vector<Digest128> input_digests;
    input_digests.reserve(input.size() * active_passes);
    for (size_t pass = 0; pass < active_passes; ++pass)
        for (size_t i = 0; i < input.size(); ++i)
            input_digests.push_back(icecc::digest128(bytes_for_job(pass * input.size() + i)));
    const Id128 relationship{Id128::from_u64(0x5102)};
    const uint64_t physical_generation = 1;
    std::vector<P51SourceArmedFields> armed;
    constexpr size_t kPasses = 3;
    for (size_t i = 0; i < input.size() * active_passes; ++i)
        armed.push_back(make_armed(c_guid, f_guid, i, window, profile));

    asio::io_context f_context;
    asio::io_context c_context;
    std::thread c_thread;
    std::thread f_thread;
    auto acceptor = std::make_shared<tcp::acceptor>(
        f_context, tcp::endpoint{asio::ip::address_v4::loopback(), 0});
    std::vector<size_t> index_by_tu(input.size() * active_passes,
                                    input.size() * active_passes);
    std::vector<bool> consumed(input.size() * active_passes, false);
    std::mutex map_mutex;
    size_t commits = 0;
    std::array<std::chrono::steady_clock::time_point, kPasses> pass_started_at{};
    std::array<uint64_t, kPasses> first_commit_us{};
    std::mutex metric_mutex;
    std::vector<R2WireControlSnapshot> intervals;
    std::vector<size_t> job_by_ordinal(input.size() * active_passes + 1,
                                       input.size() * active_passes);
    struct OrdinalEvent {
        uint64_t ordinal = 0;
        bool sent = false;
        std::chrono::steady_clock::time_point at{};
    };
    std::vector<OrdinalEvent> ordinal_events;
    std::array<size_t, kPasses> peak_outstanding{};
    std::vector<std::chrono::steady_clock::time_point> request_started(
        input.size() * active_passes);
    std::vector<uint64_t> receipt_latency_us(input.size() * active_passes, 0);
    EndpointCaps f_caps;
    f_caps.profile = profile;
    f_caps.supported_profiles = profile_bit(profile);
    P50ServerEndpointConfig f_config;
    f_config.input_job_state = [&](CStoreGuid observed, const TxBegin& begin,
                                  const TxCommit& commit,
                                  std::span<const uint8_t> bytes) {
        const size_t tu = static_cast<size_t>(begin.tu_seq.value);
        std::lock_guard lock(map_mutex);
        require(observed == c_guid && begin.profile == profile &&
                    tu < index_by_tu.size(),
                "server received unknown input identity");
        const size_t i = index_by_tu[tu];
        const auto& expected = bytes_for_job(i);
        require(i < index_by_tu.size() && bytes.size() == expected.size() &&
                std::equal(bytes.begin(), bytes.end(), expected.begin()) &&
                commit.raw_digest == input_digests[i],
                "decoded input differs from corpus bytes");
        return InputJobState::Open;
    };
    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto lease_deadline = sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
        std::chrono::steady_clock::now() + std::chrono::minutes(10),
        clock.clock_domain_id, clock.time_namespace_id);
    f_config.lookup_p51_link_reservation =
        [&, lease_deadline]
        (const LinkHello& hello) -> P51SourceLinkLookupResult {
            if (hello.profile != profile || hello.window != window ||
                hello.relationship_id != relationship || hello.c_store_guid != c_guid ||
                hello.f_store_guid != f_guid ||
                hello.reservation_id != Id128{armed.front().reservation_id} ||
                hello.relationship_epoch != armed.front().relationship_epoch ||
                hello.c_store_generation != armed.front().arm.source.c_store_generation ||
                hello.f_store_generation != armed.front().f_store_generation ||
                hello.physical_link_generation != physical_generation)
                return {};
            P51SourceLinkLease lease;
            lease.initial_armed = armed.front();
            lease.absolute_deadline = lease_deadline;
            lease.relationship_epoch = hello.relationship_epoch;
            lease.history_nonce = hello.history_nonce;
            return {P51SourceLinkLookupStatus::Found, std::move(lease)};
        };
    f_config.consume_p51_job_reservation =
        [&, lease_deadline]
        (const LinkHello& hello, const JobBind& bind)
            -> std::optional<P51SourceJobLease> {
            size_t i = armed.size();
            for (size_t n = 0; n < armed.size(); ++n)
                if (bind.reservation_id == Id128{armed[n].reservation_id}) { i = n; break; }
            if (hello.relationship_id != relationship ||
                bind.physical_link_generation != physical_generation ||
                bind.profile != profile || i == armed.size() ||
                bind.tu_seq.value >= index_by_tu.size() ||
                bind.raw_bytes != bytes_for_job(i).size() ||
                bind.raw_digest != input_digests[i] ||
                bind.wire_job_id != armed[i].arm.source.wire_job_id ||
                bind.assignment_nonce != armed[i].arm.source.assignment_nonce ||
                bind.source_request_id != armed[i].arm.source.source_request_id)
                return std::nullopt;
            std::lock_guard lock(map_mutex);
            const size_t tu = static_cast<size_t>(bind.tu_seq.value);
            if (consumed[i] || index_by_tu[tu] != index_by_tu.size()) return std::nullopt;
            consumed[i] = true;
            index_by_tu[tu] = i;
            {
                std::lock_guard metric_lock(metric_mutex);
                require(bind.relationship_ordinal < job_by_ordinal.size(),
                        "R2 relationship ordinal out of range");
                job_by_ordinal[bind.relationship_ordinal] = i;
            }
            P51SourceJobLease lease;
            lease.armed = armed[i];
            lease.absolute_deadline = lease_deadline;
            lease.binding = bind;
            lease.binding_digest = compute_r2_binding_digest(bind);
            lease.input_key = InputRecordKey{c_guid, bind.tu_seq};
            return lease;
        };
    f_config.record_p51_job_commit = [&](const LinkHello&, const JobBind& bind, const R2TxCommit&) {
        std::lock_guard lock(map_mutex);
        ++commits;
        const size_t pass = static_cast<size_t>(bind.tu_seq.value) / input.size();
        if (pass < active_passes && first_commit_us[pass] == 0 &&
            pass_started_at[pass] != std::chrono::steady_clock::time_point{})
            first_commit_us[pass] = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - pass_started_at[pass]).count());
        return true;
    };
    f_config.acknowledge_p51_receipt = [](const LinkHello&, const CommitAck&) { return true; };
    CompletionLog f_completions;
    auto server = std::make_shared<P50ServerEndpoint>(
        f_guid, f_caps, &f_completions, nullptr, std::move(f_config));
    auto server_future = co_spawn_future(f_context,
        accept_r2_owned(acceptor, server));
    // Worker launch is delayed until all state captured by callbacks exists.

    PreparationAuthorityLimits limits;
    limits.max_speculative_tus = window;
    limits.max_speculative_raw_bytes = 1ULL << 30;
    limits.max_live_entries = armed.size() + 4;
    EndpointCaps c_caps;
    c_caps.profile = profile;
    c_caps.supported_profiles = profile_bit(profile);
    auto authority = std::make_shared<P50PreparationAuthority>(
        c_guid, c_caps.zstd, limits, 1, profile);
    ZstdSourceTransferConfig sender_config;
    sender_config.deadline = std::chrono::steady_clock::now() + maximum_duration;
    sender_config.endpoint_caps = c_caps;
    sender_config.authority_limits = limits;
    sender_config.r2_interval_observer = [&](const R2WireControlSnapshot& snapshot) {
        std::lock_guard lock(metric_mutex);
        intervals.push_back(snapshot);
        return true;
    };
    sender_config.after_r2_bundle_sent_for_test = [&](uint64_t ordinal) {
        std::lock_guard lock(metric_mutex);
        ordinal_events.push_back({ordinal, true, std::chrono::steady_clock::now()});
    };
    sender_config.after_r2_receipt_validated_for_test = [&](uint64_t ordinal) {
        std::lock_guard lock(metric_mutex);
        require(ordinal < job_by_ordinal.size() && job_by_ordinal[ordinal] < armed.size(),
                "R2 receipt ordinal has no exact reservation mapping");
        const size_t job = job_by_ordinal[ordinal];
        const size_t pass = job / input.size();
        const auto now = std::chrono::steady_clock::now();
        ordinal_events.push_back({ordinal, false, now});
        const auto latency = now - request_started[job];
        receipt_latency_us[job] = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(latency).count());
        (void)pass;
    };
    std::shared_ptr<DelayedFinalizerGate> delayed_finalizer_gate;
    const auto deadline = std::chrono::steady_clock::now() + maximum_duration;
    if (delay_finalizers_for_regression) {
        delayed_finalizer_gate =
            std::make_shared<DelayedFinalizerGate>(input.size(), deadline);
        sender_config.before_r2_transfer_finalization_for_test =
            [delayed_finalizer_gate](PrepareRequestKey)
                -> asio::awaitable<void> {
                co_await delayed_finalizer_gate->wait();
            };
    }
    if (profile == ProfileId::ZSTD_ROUTE) sender_config.compression_level = 3;
    auto sender = std::make_shared<P50ZstdSourceSender>(
        authority, PreparationRouteKey{f_guid, 23, profile},
        PrepareRequestKey{3, 10000}, sender_config);
    auto work = asio::make_work_guard(c_context);
    const auto remote = acceptor->local_endpoint();
    std::atomic<uint64_t> connection_count{0};
    AsyncConnectedFdFactory connector = [remote, &connection_count](auto, auto completion) {
        connection_count.fetch_add(1, std::memory_order_relaxed);
        completion(connect_fd(remote));
    };
    const auto start = std::chrono::steady_clock::now();
    uint64_t result_receipt = 0, result_sent = 0, raw = 0;
    std::array<uint64_t, kPasses> pass_raw{}, pass_job_sent{}, pass_job_receipt{};
    std::vector<size_t> result_job_by_tu(armed.size(), armed.size());
    std::array<int64_t, kPasses> pass_elapsed_ms{};
    std::array<int64_t, kPasses> pass_process_cpu_ms{};
    f_thread = std::thread([&] { f_context.run(); });
    IoThreadGuard thread_guard{c_context, f_context, c_thread, f_thread};
    c_thread = std::thread([&] { c_context.run(); });
    for (size_t pass = 0; pass < active_passes; ++pass) {
        const auto pass_start = std::chrono::steady_clock::now();
        const std::clock_t cpu_start = std::clock();
        {
            std::lock_guard lock(map_mutex);
            pass_started_at[pass] = pass_start;
        }
        std::vector<std::future<ZstdSourceTransferResult>> futures;
        futures.reserve(input.size());
        for (size_t i = 0; i < input.size(); ++i) {
            const size_t job = pass * input.size() + i;
            {
                std::lock_guard lock(metric_mutex);
                request_started[job] = std::chrono::steady_clock::now();
            }
            futures.push_back(co_spawn_future(c_context,
                transfer_p51_route_owned(sender, armed[job], physical_generation,
                    connector, PrepareRequestKey{3, 10000 + job}, deadline,
                    bytes_for_job(job))));
        }
        for (size_t i = 0; i < futures.size(); ++i) {
            require(futures[i].wait_until(deadline) == std::future_status::ready,
                    "sender future exceeded shared benchmark deadline");
            auto result = futures[i].get();
            if (result.status != ZstdSourceTransferStatus::Committed) {
                std::cerr << "transfer failure index=" << i
                          << " status=" << static_cast<unsigned>(result.status)
                          << " attempts=" << static_cast<unsigned>(result.attempts)
                          << " terminal="
                          << (result.terminal_error ? result.terminal_error->detail : "none")
                          << " replacement=" << result.replacement_required << '\n';
            }
            require(result.status == ZstdSourceTransferStatus::Committed,
                    "sender failed to commit corpus input");
            const size_t job = pass * input.size() + i;
            require(result.committed_input.has_value() &&
                        result.raw_bytes == bytes_for_job(job).size() &&
                        result.raw_digest == input_digests[job],
                    "sender result identity mismatch");
            const size_t result_tu =
                static_cast<size_t>(result.committed_input->tu_seq.value);
            require(result_tu < result_job_by_tu.size() &&
                        result_job_by_tu[result_tu] == armed.size(),
                    "duplicate or out-of-range committed TU identity");
            result_job_by_tu[result_tu] = job;
            if (!result.r2_wire_accounting.has_value() ||
                !result.r2_wire_accounting->valid ||
                !result.r2_wire_accounting_key.has_value())
                std::cerr << "R2 accounting absent job=" << job
                          << " measured=" << result.wire_bytes_measured
                          << " reference=" << result.r2_accounting_reference
                          << " c_to_f=" << result.c_to_f_bytes
                          << " f_to_c=" << result.f_to_c_bytes
                          << " has_key=" << result.r2_wire_accounting_key.has_value()
                          << " has_accounting=" << result.r2_wire_accounting.has_value()
                          << " accounting_valid="
                          << (result.r2_wire_accounting.has_value()
                                  ? result.r2_wire_accounting->valid : false)
                          << " status=" << static_cast<unsigned>(result.status) << '\n';
            require(result.r2_wire_accounting.has_value() &&
                        result.r2_wire_accounting->valid &&
                        result.r2_wire_accounting_key.has_value(),
                    "successful R2 result lacks valid per-job wire accounting");
            require(result.r2_wire_accounting_key->c_store_guid == c_guid &&
                    result.r2_wire_accounting_key->f_store_guid == f_guid &&
                    result.r2_wire_accounting_key->logical_link_id == relationship &&
                    result.r2_wire_accounting_key->raw_digest == result.raw_digest &&
                        result.r2_wire_accounting_key->tu_seq ==
                            result.committed_input->tu_seq,
                    "R2 accounting key does not identify its committed result");
            add_bytes(raw, result.raw_bytes);
            add_bytes(pass_raw[pass], result.raw_bytes);
            add_bytes(result_sent, result.r2_wire_accounting->c_to_f_bundle_bytes);
            add_bytes(result_receipt, result.r2_wire_accounting->f_to_c_receipt_bytes);
            add_bytes(pass_job_sent[pass],
                      result.r2_wire_accounting->c_to_f_bundle_bytes);
            add_bytes(pass_job_receipt[pass],
                      result.r2_wire_accounting->f_to_c_receipt_bytes);
        }
        pass_elapsed_ms[pass] = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - pass_start).count();
        pass_process_cpu_ms[pass] = static_cast<int64_t>(
            (std::clock() - cpu_start) * 1000.0 / CLOCKS_PER_SEC);
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    auto retired = std::make_shared<std::promise<void>>();
    auto retire_done = retired->get_future();
    asio::post(c_context, [sender, retired] {
        sender->retire_for_replacement();
        retired->set_value();
    });
    require(retire_done.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "owner executor did not retire sender");
    work.reset();
    if (c_thread.joinable()) c_thread.join();
    require(server_future.wait_for(std::chrono::seconds(10)) == std::future_status::ready,
            "F endpoint still active after shutdown");
    require(server_future.get().status == ServerRunStatus::Disconnected,
            "F endpoint did not observe ordinary sender retirement");
    if (f_thread.joinable()) f_thread.join();
    drain_ready(c_context);
    drain_ready(f_context);
    require(commits == armed.size(), "server commit count mismatch");
    for (size_t tu = 0; tu < result_job_by_tu.size(); ++tu)
        require(result_job_by_tu[tu] == index_by_tu[tu] &&
                    index_by_tu[tu] < armed.size(),
                "F reservation and C result map disagree on TU identity");
    {
        std::array<std::set<uint64_t>, kPasses> sent_by_pass, receipts_by_pass;
        for (const OrdinalEvent& event : ordinal_events) {
            require(event.ordinal < job_by_ordinal.size() &&
                        job_by_ordinal[event.ordinal] < armed.size(),
                    "ordinal event lacks completed exact request mapping");
            const size_t pass = job_by_ordinal[event.ordinal] / input.size();
            (event.sent ? sent_by_pass[pass] : receipts_by_pass[pass])
                .insert(event.ordinal);
            size_t outstanding = 0;
            for (const uint64_t sent : sent_by_pass[pass])
                if (!receipts_by_pass[pass].contains(sent)) ++outstanding;
            peak_outstanding[pass] = std::max(peak_outstanding[pass], outstanding);
        }
    }
    require(std::all_of(receipt_latency_us.begin(), receipt_latency_us.end(),
                        [](uint64_t value) { return value != 0; }),
            "missing per-request receipt latency witness");
    for (size_t pass = 0; pass < active_passes; ++pass)
        require(first_commit_us[pass] != 0, "missing first-commit latency witness");
    std::array<std::vector<uint64_t>, kPasses> pass_latency_us;
    for (size_t job = 0; job < receipt_latency_us.size(); ++job)
        pass_latency_us[job / input.size()].push_back(receipt_latency_us[job]);
    uint64_t interval_total_c_to_f = 0, interval_total_f_to_c = 0;
    uint64_t interval_job_c_to_f = 0, interval_job_f_to_c = 0;
    uint64_t interval_shared_c_to_f = 0, interval_shared_f_to_c = 0;
    std::set<uint64_t> interval_sequences;
    for (const auto& interval : intervals) {
        require(interval.valid && interval.interval_sequence != 0 &&
                    interval_sequences.insert(interval.interval_sequence).second &&
                    interval.link.c_store_guid == c_guid &&
                    interval.link.f_store_guid == f_guid &&
                    interval.link.logical_link_id == relationship &&
                    interval.link.relationship_epoch == 25 &&
                    interval.link.physical_link_generation == physical_generation,
                "invalid or duplicated R2 interval snapshot");
        add_bytes(interval_total_c_to_f, interval.total_c_to_f_bytes);
        add_bytes(interval_total_f_to_c, interval.total_f_to_c_bytes);
        add_bytes(interval_shared_c_to_f, interval.shared_c_to_f_bytes);
        add_bytes(interval_shared_f_to_c, interval.shared_f_to_c_bytes);
        for (const auto& job : interval.jobs) {
            require(job.valid, "invalid per-job interval accounting");
            add_bytes(interval_job_c_to_f, job.c_to_f_bundle_bytes);
            add_bytes(interval_job_f_to_c, job.f_to_c_receipt_bytes);
        }
    }
    require(interval_job_c_to_f == result_sent &&
                interval_job_f_to_c == result_receipt,
            "per-result and interval per-job byte totals disagree");
    require(interval_total_c_to_f == interval_job_c_to_f + interval_shared_c_to_f &&
                interval_total_f_to_c == interval_job_f_to_c + interval_shared_f_to_c,
            "interval total does not equal disjoint job plus shared control bytes");
    uint64_t f_socket_c_to_f = 0, f_socket_f_to_c = 0;
    for (const AsyncCompletion& completion : f_completions.completions()) {
        if (!completion.stamp.r2_traffic || completion.stamp.actor != ActorSide::F)
            continue;
        if (completion.stamp.operation == AsyncOperationKind::ReadHeader ||
            completion.stamp.operation == AsyncOperationKind::ReadPayload)
            add_bytes(f_socket_c_to_f, completion.transferred_bytes);
        else if (completion.stamp.operation == AsyncOperationKind::WriteFragment)
            add_bytes(f_socket_f_to_c, completion.transferred_bytes);
    }
    require(f_socket_c_to_f == interval_total_c_to_f &&
                f_socket_f_to_c == interval_total_f_to_c,
            "F async-socket byte totals disagree with R2 interval totals");
    if (delayed_finalizer_gate)
        require(delayed_finalizer_gate->released_batches.load(
                    std::memory_order_relaxed) == active_passes,
                "delayed finalizer did not release one full caller batch per pass");
    std::cout << "profile=" << static_cast<unsigned>(profile) << " protocol=R2 window="
              << window << " jobs_per_pass=" << input.size() << " passes=" << active_passes << " raw_bytes=" << raw
              << " job_cachewire_c_to_f_bytes=" << interval_job_c_to_f
              << " job_cachewire_f_to_c_bytes=" << interval_job_f_to_c
              << " shared_control_cachewire_c_to_f_bytes=" << interval_shared_c_to_f
              << " shared_control_cachewire_f_to_c_bytes=" << interval_shared_f_to_c
              << " total_cachewire_c_to_f_bytes=" << interval_total_c_to_f
              << " total_cachewire_f_to_c_bytes=" << interval_total_f_to_c
              << " F_socket_c_to_f_bytes=" << f_socket_c_to_f
              << " F_socket_f_to_c_bytes=" << f_socket_f_to_c
              << " observed_peak_outstanding="
              << std::max(peak_outstanding[0], peak_outstanding[1])
              << " fresh_peak_outstanding=" << peak_outstanding[0]
              << " retained_peak_outstanding=" << peak_outstanding[1]
              << " fresh_pass_raw_bytes=" << pass_raw[0]
              << " retained_pass_raw_bytes=" << pass_raw[1]
              << (active_passes == 3 ? " edited_pass_raw_bytes=" : "")
              << (active_passes == 3 ? std::to_string(pass_raw[2]) : "")
              << (active_passes == 3 ? " edited_peak_outstanding=" : "")
              << (active_passes == 3 ? std::to_string(peak_outstanding[2]) : "")
              << " fresh_pass_job_cachewire_c_to_f_bytes=" << pass_job_sent[0]
              << " retained_pass_job_cachewire_c_to_f_bytes=" << pass_job_sent[1]
              << (active_passes == 3 ? " edited_pass_job_cachewire_c_to_f_bytes=" : "")
              << (active_passes == 3 ? std::to_string(pass_job_sent[2]) : "")
              << " fresh_pass_job_cachewire_f_to_c_bytes=" << pass_job_receipt[0]
              << " retained_pass_job_cachewire_f_to_c_bytes=" << pass_job_receipt[1]
              << (active_passes == 3 ? " edited_pass_job_cachewire_f_to_c_bytes=" : "")
              << (active_passes == 3 ? std::to_string(pass_job_receipt[2]) : "")
              << " fresh_latency_p50_us=" << percentile_us(pass_latency_us[0], 50)
              << " fresh_latency_p95_us=" << percentile_us(pass_latency_us[0], 95)
              << " fresh_latency_p99_us=" << percentile_us(pass_latency_us[0], 99)
              << " retained_latency_p50_us=" << percentile_us(pass_latency_us[1], 50)
              << " retained_latency_p95_us=" << percentile_us(pass_latency_us[1], 95)
              << " retained_latency_p99_us=" << percentile_us(pass_latency_us[1], 99)
              << (active_passes == 3 ? " edited_latency_p50_us=" : "")
              << (active_passes == 3 ? std::to_string(percentile_us(pass_latency_us[2], 50)) : "")
              << (active_passes == 3 ? " edited_latency_p95_us=" : "")
              << (active_passes == 3 ? std::to_string(percentile_us(pass_latency_us[2], 95)) : "")
              << (active_passes == 3 ? " edited_latency_p99_us=" : "")
              << (active_passes == 3 ? std::to_string(percentile_us(pass_latency_us[2], 99)) : "")
              << " tcp_connections=" << connection_count.load(std::memory_order_relaxed)
              << " delayed_finalizer_batches="
              << (delayed_finalizer_gate
                      ? delayed_finalizer_gate->released_batches.load()
                      : 0)
              << " control_intervals=" << intervals.size()
              << " fresh_pass_ms=" << pass_elapsed_ms[0]
              << " retained_pass_ms=" << pass_elapsed_ms[1]
              << " fresh_process_cpu_ms=" << pass_process_cpu_ms[0]
              << " retained_process_cpu_ms=" << pass_process_cpu_ms[1]
              << " fresh_first_commit_us=" << first_commit_us[0]
              << " retained_first_commit_us=" << first_commit_us[1]
              << (active_passes == 3 ? " edited_first_commit_us=" : "")
              << (active_passes == 3 ? std::to_string(first_commit_us[2]) : "")
              << " cpu_scope=process_all_threads_no_cycles"
              << (active_passes == 3 ? " edited_pass_ms=" : "")
              << (active_passes == 3 ? std::to_string(pass_elapsed_ms[2]) : "")
              << (active_passes == 3 ? " edited_process_cpu_ms=" : "")
              << (active_passes == 3 ? std::to_string(pass_process_cpu_ms[2]) : "")
              << " elapsed_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
              << " raw_decoded_bytes_verified=" << raw << '\n';
}

void smoke_r1(const std::vector<std::vector<uint8_t>>& input, ProfileId profile,
              const std::vector<std::vector<uint8_t>>* edited_input = nullptr) {
    require(!input.empty(), "R1 smoke needs at least one input");
    require(!edited_input || edited_input->size() == input.size(),
            "edited pass must have the same number of TUs");
    const size_t active_passes = edited_input ? 3 : 2;
    const auto bytes_for_job = [&](size_t job) -> const std::vector<uint8_t>& {
        const size_t pass = job / input.size();
        const size_t item = job % input.size();
        return pass == 2 ? (*edited_input)[item] : input[item];
    };
    const auto [c_guid, f_guid] = store_guids();
    std::vector<Digest128> input_digests;
    input_digests.reserve(input.size() * active_passes);
    for (size_t job = 0; job < input.size() * active_passes; ++job)
        input_digests.push_back(icecc::digest128(bytes_for_job(job)));
    std::mutex observation_mutex;
    size_t exact_receipts = 0;
    size_t exact_commits = 0;
    bool all_wire_measured = true;
    EndpointCaps f_caps;
    f_caps.profile = profile;
    f_caps.supported_profiles = profile_bit(profile);
    P50ServerEndpointConfig f_config;
    f_config.input_job_state = [&](CStoreGuid observed, const TxBegin& begin,
                                  const TxCommit& commit,
                                  std::span<const uint8_t> bytes) {
        const size_t tu = static_cast<size_t>(begin.tu_seq.value);
        require(observed == c_guid && tu < input.size() * active_passes,
                "R1 decoded input identity out of range");
        const auto& expected = bytes_for_job(tu);
        require(bytes.size() == expected.size() &&
                    std::equal(bytes.begin(), bytes.end(), expected.begin()) &&
                    commit.raw_digest == input_digests[tu],
                "R1 decoded input differs from the exact TU witness");
        std::lock_guard lock(observation_mutex);
        ++exact_receipts;
        return InputJobState::Open;
    };
    R1CommitMetrics commit_metrics;
    bool bad_commit_callback = false;
    f_config.on_input_committed = [&](InputRecordKey key, bool committed) {
        std::lock_guard lock(observation_mutex);
        if (key.c_store_guid != c_guid || key.tu_seq.value >= input.size() * active_passes ||
            !committed)
            bad_commit_callback = true;
        const size_t pass = static_cast<size_t>(key.tu_seq.value) / input.size();
        {
            std::lock_guard metrics_lock(commit_metrics.mutex);
            if (pass < active_passes && commit_metrics.first_commit_us[pass] == 0 &&
                commit_metrics.pass_started_at[pass] != std::chrono::steady_clock::time_point{})
                commit_metrics.first_commit_us[pass] = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() -
                        commit_metrics.pass_started_at[pass]).count());
        }
        ++exact_commits;
    };
    auto server = std::make_shared<P50ServerEndpoint>(
        f_guid, f_caps, nullptr, nullptr, std::move(f_config));
    asio::io_context f_context;
    asio::io_context c_context;
    std::thread c_thread;
    std::thread f_thread;
    auto acceptor = std::make_shared<tcp::acceptor>(
        f_context, tcp::endpoint{asio::ip::address_v4::loopback(), 0});
    auto server_future = co_spawn_future(f_context,
        accept_r1_batch_owned(acceptor, server, input.size() * active_passes));
    PreparationAuthorityLimits limits;
    limits.max_live_entries = input.size() * active_passes + 4;
    limits.max_speculative_tus = 1;
    limits.max_speculative_raw_bytes = 512ULL << 20;
    auto authority = std::make_shared<P50PreparationAuthority>(
        c_guid, f_caps.zstd, limits, 1, profile);
    ZstdSourceTransferConfig sender_config;
    sender_config.deadline = std::chrono::steady_clock::now() + std::chrono::minutes(4);
    sender_config.endpoint_caps = f_caps;
    sender_config.authority_limits = limits;
    if (profile == ProfileId::ZSTD_ROUTE) sender_config.compression_level = 3;
    auto sender = std::make_shared<P50ZstdSourceSender>(
        authority, PreparationRouteKey{f_guid, 23, profile},
        PrepareRequestKey{3, 20000}, sender_config);
    auto c_work = asio::make_work_guard(c_context);
    const auto remote = acceptor->local_endpoint();
    std::atomic<uint64_t> connection_count{0};
    AsyncConnectedFdFactory connector = [remote, &connection_count](auto, auto completion) {
        connection_count.fetch_add(1, std::memory_order_relaxed);
        completion(connect_fd(remote));
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(4);
    const auto start = std::chrono::steady_clock::now();
    f_thread = std::thread([&] { f_context.run(); });
    IoThreadGuard thread_guard{c_context, f_context, c_thread, f_thread};
    auto results_future = co_spawn_future(c_context,
        transfer_r1_serial_batch(sender, connector, input, edited_input,
                                 &commit_metrics, deadline));
    c_thread = std::thread([&] { c_context.run(); });
    const auto measurements = results_future.get();
    const auto& results = measurements.results;
    std::array<uint64_t, 3> raw_by_pass{}, c_to_f_by_pass{}, f_to_c_by_pass{};
    std::array<std::vector<uint64_t>, 3> pass_latency_us;
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& result = results[i];
        const size_t pass = i / input.size();
        pass_latency_us[pass].push_back(measurements.latency_us[i]);
        const auto& expected = bytes_for_job(i);
        if (result.status != ZstdSourceTransferStatus::Committed ||
            !result.committed_input || result.raw_bytes != expected.size() ||
            result.raw_digest != input_digests[i])
            std::cerr << "R1 result mismatch index=" << i
                      << " status=" << static_cast<unsigned>(result.status)
                      << " raw=" << result.raw_bytes << " expected=" << expected.size()
                      << " digest_match="
                      << (result.raw_digest == input_digests[i])
                      << " committed=" << result.committed_input.has_value()
                      << " terminal="
                      << (result.terminal_error ? result.terminal_error->detail : "none")
                      << '\n';
        require(result.status == ZstdSourceTransferStatus::Committed &&
                    result.committed_input.has_value() &&
                    result.raw_bytes == expected.size() &&
                    result.raw_digest == input_digests[i],
                "R1 sender result does not match its exact corpus input");
        add_bytes(raw_by_pass[pass], result.raw_bytes);
        if (result.wire_bytes_measured) {
            add_bytes(c_to_f_by_pass[pass], result.c_to_f_bytes);
            add_bytes(f_to_c_by_pass[pass], result.f_to_c_bytes);
        } else {
            all_wire_measured = false;
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    c_work.reset();
    if (c_thread.joinable()) c_thread.join();
    require(server_future.wait_for(std::chrono::seconds(10)) == std::future_status::ready,
            "R1 server batch did not drain");
    server_future.get();
    if (f_thread.joinable()) f_thread.join();
    drain_ready(c_context);
    drain_ready(f_context);
    require(exact_receipts == input.size() * active_passes &&
                exact_commits == input.size() * active_passes,
            "R1 server receipt/commit count mismatch");
    require(!bad_commit_callback, "R1 endpoint published an invalid commit callback");
    uint64_t raw_total = 0, cachewire_c_to_f = 0, cachewire_f_to_c = 0;
    for (size_t pass = 0; pass < active_passes; ++pass) {
        add_bytes(raw_total, raw_by_pass[pass]);
        add_bytes(cachewire_c_to_f, c_to_f_by_pass[pass]);
        add_bytes(cachewire_f_to_c, f_to_c_by_pass[pass]);
    }
    for (size_t pass = 0; pass < active_passes; ++pass)
        require(commit_metrics.first_commit_us[pass] != 0,
                "missing R1 first-commit latency witness");
    std::cout << "profile=" << static_cast<unsigned>(profile) << " protocol=R1 window=1 jobs_per_pass="
              << input.size() << " raw_bytes=" << raw_total << " cachewire_bytes_measured="
              << all_wire_measured << " cachewire_c_to_f_bytes="
              << (all_wire_measured ? std::to_string(cachewire_c_to_f) : "unmeasured")
              << " cachewire_f_to_c_bytes="
              << (all_wire_measured ? std::to_string(cachewire_f_to_c) : "unmeasured")
              << " fresh_pass_raw_bytes=" << raw_by_pass[0]
              << " retained_pass_raw_bytes=" << raw_by_pass[1]
              << (active_passes == 3 ? " edited_pass_raw_bytes=" : "")
              << (active_passes == 3 ? std::to_string(raw_by_pass[2]) : "")
              << " fresh_pass_cachewire_c_to_f_bytes="
              << (all_wire_measured ? std::to_string(c_to_f_by_pass[0]) : "unmeasured")
              << " retained_pass_cachewire_c_to_f_bytes="
              << (all_wire_measured ? std::to_string(c_to_f_by_pass[1]) : "unmeasured")
              << " fresh_pass_cachewire_f_to_c_bytes="
              << (all_wire_measured ? std::to_string(f_to_c_by_pass[0]) : "unmeasured")
              << " retained_pass_cachewire_f_to_c_bytes="
              << (all_wire_measured ? std::to_string(f_to_c_by_pass[1]) : "unmeasured")
              << (active_passes == 3 ? " edited_pass_cachewire_c_to_f_bytes=" : "")
              << (active_passes == 3
                      ? (all_wire_measured ? std::to_string(c_to_f_by_pass[2]) : "unmeasured")
                      : "")
              << (active_passes == 3 ? " edited_pass_cachewire_f_to_c_bytes=" : "")
              << (active_passes == 3
                      ? (all_wire_measured ? std::to_string(f_to_c_by_pass[2]) : "unmeasured")
                      : "")
              << " fresh_pass_ms=" << measurements.pass_elapsed_ms[0]
              << " retained_pass_ms=" << measurements.pass_elapsed_ms[1]
              << " fresh_process_cpu_ms=" << measurements.pass_process_cpu_ms[0]
              << " retained_process_cpu_ms=" << measurements.pass_process_cpu_ms[1]
              << " fresh_first_commit_us=" << commit_metrics.first_commit_us[0]
              << " retained_first_commit_us=" << commit_metrics.first_commit_us[1]
              << " cpu_scope=process_all_threads_no_cycles"
              << (active_passes == 3 ? " edited_pass_ms=" : "")
              << (active_passes == 3 ? std::to_string(measurements.pass_elapsed_ms[2]) : "")
              << (active_passes == 3 ? " edited_process_cpu_ms=" : "")
              << (active_passes == 3 ? std::to_string(measurements.pass_process_cpu_ms[2]) : "")
              << (active_passes == 3 ? " edited_first_commit_us=" : "")
              << (active_passes == 3 ? std::to_string(commit_metrics.first_commit_us[2]) : "")
              << " fresh_latency_p50_us=" << percentile_us(pass_latency_us[0], 50)
              << " fresh_latency_p95_us=" << percentile_us(pass_latency_us[0], 95)
              << " fresh_latency_p99_us=" << percentile_us(pass_latency_us[0], 99)
              << " retained_latency_p50_us=" << percentile_us(pass_latency_us[1], 50)
              << " retained_latency_p95_us=" << percentile_us(pass_latency_us[1], 95)
              << " retained_latency_p99_us=" << percentile_us(pass_latency_us[1], 99)
              << (active_passes == 3 ? " edited_latency_p50_us=" : "")
              << (active_passes == 3 ? std::to_string(percentile_us(pass_latency_us[2], 50)) : "")
              << (active_passes == 3 ? " edited_latency_p95_us=" : "")
              << (active_passes == 3 ? std::to_string(percentile_us(pass_latency_us[2], 95)) : "")
              << (active_passes == 3 ? " edited_latency_p99_us=" : "")
              << (active_passes == 3 ? std::to_string(percentile_us(pass_latency_us[2], 99)) : "")
              << " tcp_connections=" << connection_count.load(std::memory_order_relaxed)
              << " elapsed_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
              << " raw_decoded_bytes_verified=" << raw_total << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        require(::setenv("ICECC_P50_DIAGNOSTICS", "1", 1) == 0,
                "could not enable exact CacheWire accounting");
        if (argc == 1 ||
            (argc == 2 &&
             std::string_view(argv[1]) == "--delayed-finalizer-regression")) {
            verify_missing_gate_participant_times_out();
            const auto inputs = make_finalizer_regression_inputs();
            constexpr ProfileId profiles[] = {
                ProfileId::ZSTD_TU, ProfileId::P29V1,
                ProfileId::ZSTD_ROUTE};
            for (ProfileId profile : profiles)
                smoke_r2(inputs, profile, 4, std::chrono::seconds(20), true);
            std::cout << "delayed_finalizer_regression=PASS profiles=3 jobs=32 "
                         "passes=2 window=4\n";
            return 0;
        }
        if (argc == 10 && std::string_view(argv[1]) == "--paired") {
            const std::string protocol = argv[6];
            const size_t jobs = std::stoul(argv[7]);
            const unsigned profile = static_cast<unsigned>(std::stoul(argv[8]));
            const uint32_t window = static_cast<uint32_t>(std::stoul(argv[9]));
            require(protocol == "R1" || protocol == "R2",
                    "paired protocol must be R1 or R2");
            require(profile < 3, "profile index must be 0,1,2");
            require(jobs > 1 &&
                        (protocol == "R1" ? window == 1
                                           : (window > 0 && window <= 30 && window < jobs)),
                    "invalid paired window/job count");
            const ProfileId profiles[] = {ProfileId::ZSTD_TU, ProfileId::P29V1,
                                          ProfileId::ZSTD_ROUTE};
            const auto b_paths = read_and_validate_pair(argv[2], argv[3], argv[4], argv[5], jobs);
            const auto a_paths = read_manifest(argv[2], jobs);
            const auto a = load_inputs(a_paths);
            const auto b = load_inputs(b_paths);
            size_t changed = 0;
            for (size_t i = 0; i < jobs; ++i)
                changed += icecc::digest128(a[i]) != icecc::digest128(b[i]);
            require(changed != 0, "paired edited pass contains no changed TU bytes");
            if (protocol == "R1") smoke_r1(a, profiles[profile], &b);
            else smoke_r2(a, profiles[profile], window, std::chrono::minutes(4), false, &b);
            std::cout << "paired_inputs=PASS jobs=" << jobs
                      << " changed_raw_digests=" << changed
                      << " unchanged_raw_digests=" << jobs - changed
                      << " ordered_tu_ids=verified os_cache_state=inherited_uncontrolled"
                      << " pass_semantics=A_fresh,A_retained,B_edited"
                      << " profile=" << profile << " window="
                      << (protocol == "R1" ? 1 : window) << '\n';
            return 0;
        }
        if (argc != 6) {
            std::cerr << "usage: p50-transfer-window-bench MANIFEST PROTOCOL JOBS PROFILE_INDEX WINDOW\n"
                         "   or: p50-transfer-window-bench --paired A_MANIFEST B_MANIFEST A_ROOT B_ROOT PROTOCOL JOBS PROFILE_INDEX WINDOW\n";
            return 2;
        }
        const std::string protocol = argv[2];
        const size_t jobs = std::stoul(argv[3]);
        const unsigned profile = static_cast<unsigned>(std::stoul(argv[4]));
        const uint32_t window = static_cast<uint32_t>(std::stoul(argv[5]));
        require(protocol == "R1" || protocol == "R2", "protocol must be R1 or R2");
        require(profile < 3, "profile index must be 0,1,2");
        require(jobs > 0 && (protocol == "R1" ||
                    (window > 0 && window <= 30 && window < jobs)),
                "invalid window/job count");
        const ProfileId profiles[] = {ProfileId::ZSTD_TU, ProfileId::P29V1,
                                      ProfileId::ZSTD_ROUTE};
        auto paths = read_manifest(argv[1], jobs);
        auto inputs = load_inputs(paths);
        if (protocol == "R1") smoke_r1(inputs, profiles[profile]);
        else smoke_r2(inputs, profiles[profile], window);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "benchmark error: " << e.what() << '\n';
        return 1;
    }
}
