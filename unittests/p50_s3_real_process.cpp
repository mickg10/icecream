#include "cache/p50_endpoint.h"
#include "cache/p50_slice0.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_future.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using namespace icecc::p50;

[[noreturn]] void fail(std::string_view detail) {
    std::cerr << "p50_s3_real_process: " << detail << '\n';
    std::exit(1);
}

void require(bool condition, std::string_view detail) {
    if (!condition) fail(detail);
}

std::vector<uint8_t> fixture() {
    const std::string text =
        "Protocol-50 S3 real-process exact input; immutable bytes survive restart.\n";
    return {text.begin(), text.end()};
}

uint64_t rss_kb() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (!line.starts_with("VmRSS:")) continue;
        const size_t first_digit = line.find_first_of("0123456789");
        if (first_digit == std::string::npos) return 0;
        return std::stoull(line.substr(first_digit));
    }
    return 0;
}

size_t fd_count() {
    size_t result = 0;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd"))
        (void)entry, ++result;
    return result;
}

void write_all(int fd, std::string_view value) {
    while (!value.empty()) {
        const ssize_t written = ::write(fd, value.data(), value.size());
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) fail("readiness pipe write failed");
        value.remove_prefix(static_cast<size_t>(written));
    }
}

std::string read_line(int fd) {
    std::string result;
    char byte = 0;
    while (true) {
        const ssize_t count = ::read(fd, &byte, 1);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) fail("readiness pipe closed before READY");
        if (byte == '\n') return result;
        result.push_back(byte);
    }
}

uint16_t parse_port(std::string_view line) {
    require(line.starts_with("READY "), "server did not report READY");
    const unsigned long value = std::stoul(std::string(line.substr(6)));
    require(value > 0 && value <= 65535, "server reported an invalid port");
    return static_cast<uint16_t>(value);
}

void persist_exact(const std::filesystem::path& path, std::span<const uint8_t> input) {
    const auto temporary = path.string() + ".tmp." + std::to_string(::getpid());
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) fail("could not create exact-input staging file");
        output.write(reinterpret_cast<const char*>(input.data()),
                     static_cast<std::streamsize>(input.size()));
        output.flush();
        if (!output) fail("could not write exact-input staging file");
    }
    std::filesystem::rename(temporary, path);
}

std::vector<uint8_t> read_exact(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "retained exact-input file is absent after restart");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct TestClient {
    explicit TestClient(CStoreGuid guid)
        : authority(std::make_shared<P50PreparationAuthority>(guid)), endpoint(authority) {}

    PreparedTuHandle prepare(std::span<const uint8_t> input) {
        return authority->prepare(
            PrepareRequestKey{static_cast<uint64_t>(getpid()), next_request++}, input);
    }

    std::shared_ptr<P50PreparationAuthority> authority;
    P50ClientEndpoint endpoint;
    uint64_t next_request = 1;
};

ClientRunResult run_client(TestClient& client, uint16_t port, PreparedTuHandle prepared = {}) {
    asio::io_context context;
    std::future<ClientRunResult> result = asio::co_spawn(
        context, client.endpoint.run({asio::ip::address_v4::loopback(), port}, prepared),
        asio::use_future);
    context.run();
    return result.get();
}

int server_process(uint16_t requested_port, FStoreGuid f_guid,
                   const std::filesystem::path& retained, bool crash_installing,
                   int ready_fd) {
    const std::vector<uint8_t> expected =
        std::filesystem::exists(retained) ? read_exact(retained) : std::vector<uint8_t>{};
    P50ServerEndpointConfig config;
    config.input_job_state = [retained, expected, crash_installing](
                                 CStoreGuid, const TxBegin&, const TxCommit&,
                                 std::span<const uint8_t> input) {
        if (crash_installing) ::_exit(73);
        if (!expected.empty()) {
            if (!std::equal(input.begin(), input.end(), expected.begin(), expected.end()))
                throw std::logic_error("restart replay changed retained exact bytes");
        }
        persist_exact(retained, input);
        return InputJobState::Open;
    };
    P50ServerEndpoint server(f_guid, {}, nullptr, nullptr, std::move(config));
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), requested_port});
    write_all(ready_fd, "READY " + std::to_string(acceptor.local_endpoint().port()) + "\n");
    ::close(ready_fd);
    std::future<ServerRunResult> result =
        asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
    context.run();
    const ServerRunResult server_result = result.get();
    const P50ServerOwnerUsage usage = server.owner_usage();
    const uint64_t rss = rss_kb();
    const size_t fds = fd_count();
    const uint64_t staging = usage.pending_encoded_bytes + usage.pending_raw_bytes +
                             usage.decoder_window_bytes;
    const size_t pins = usage.retained_input_records;
    constexpr uint64_t kRssBoundKb = 256 * 1024;
    constexpr size_t kFdBound = 128;
    require(rss <= kRssBoundKb, "RSS bound exceeded");
    require(fds <= kFdBound, "FD bound exceeded");
    require(staging <= (uint64_t{8} << 30), "staging bound exceeded");
    require(pins <= 4096, "pin bound exceeded");
    std::cout << "S3_REAL_PROCESS_COUNTERS rss_kb=" << rss << " fd=" << fds
              << " staging_bytes=" << staging << " pins=" << pins << " namespaces="
              << usage.namespaces << " result=" << static_cast<int>(server_result.status)
              << '\n' << std::flush;
    return server_result.status == ServerRunStatus::Completed ? 0 : 2;
}

pid_t start_server(const char* self, FStoreGuid f_guid,
                   const std::filesystem::path& retained, bool crash_installing,
                   uint16_t* port) {
    int ready[2];
    require(::pipe(ready) == 0, "could not create readiness pipe");
    const pid_t child = ::fork();
    require(child >= 0, "could not fork server process");
    if (child == 0) {
        ::close(ready[0]);
        const std::string guid = std::to_string(f_guid.bytes[15]);
        const std::string mode = crash_installing ? "--crash" : "--normal";
        if (ready[1] != 3) {
            if (::dup2(ready[1], 3) != 3) ::_exit(126);
            ::close(ready[1]);
        }
        ::execl(self, self, "--server", mode.c_str(), guid.c_str(), retained.c_str(),
                static_cast<char*>(nullptr));
        ::_exit(127);
    }
    ::close(ready[1]);
    *port = parse_port(read_line(ready[0]));
    ::close(ready[0]);
    return child;
}

void wait_success(pid_t child, int expected_exit = 0) {
    int status = 0;
    require(::waitpid(child, &status, 0) == child, "waitpid failed for server process");
    require(WIFEXITED(status) && WEXITSTATUS(status) == expected_exit,
            "server process returned an unexpected status");
}

void generation_wrap_gate() {
    const CStoreGuid old_c = Id128::from_u64(801);
    const CStoreGuid new_c = Id128::from_u64(802);
    CObjectArena old_arena(old_c, KeyLayoutV1::generation_value_mask);
    const std::array<uint8_t, 5> payload{'w', 'r', 'a', 'p', '!'};
    const Key64 old_key = old_arena.intern_bytes(ObjectType::Line, payload);
    require(old_arena.advance_generation() == GenerationAdvanceResult::GuidFlipRequired,
            "generation wrap did not stop admission");
    CObjectArena replacement(new_c);
    const Key64 replacement_key = replacement.intern_bytes(ObjectType::Line, payload);
    require(old_arena.guid() != replacement.guid(), "C GUID was reused at wrap");
    require(old_arena.object(old_key).canonical_payload() ==
                replacement.object(replacement_key).canonical_payload(),
            "replacement did not retain exact bytes");

    P50ServerEndpoint f_endpoint(Id128::from_u64(803));
    const FStoreGuid old_f = f_endpoint.f_store_guid();
    f_endpoint.reset_store(Id128::from_u64(804));
    require(old_f != f_endpoint.f_store_guid(), "F GUID was reused on replacement");
    require(f_endpoint.namespace_count() == 0 && f_endpoint.revision_count() == 0,
            "F replacement retained cross-namespace state");
}

int run_gate(const char* self) {
    generation_wrap_gate();
    const std::filesystem::path retained =
        std::filesystem::temp_directory_path() /
        ("p50-s3-real-process-" + std::to_string(::getpid()) + ".bin");
    std::filesystem::remove(retained);
    const auto input = fixture();
    TestClient client(Id128::from_u64(805));
    const PreparedTuHandle prepared = client.prepare(input);

    uint16_t port = 0;
    const pid_t first = start_server(self, Id128::from_u64(806), retained, false, &port);
    const ClientRunResult first_result = run_client(client, port, prepared);
    require(first_result.status == ClientRunStatus::Committed,
            "initial real-process endpoint run did not commit");
    wait_success(first);
    require(read_exact(retained) == input, "initial process did not retain exact bytes");

    const pid_t restarted = start_server(self, Id128::from_u64(809), retained, false, &port);
    const ClientRunResult restart_result = run_client(client, port, prepared);
    require(restart_result.status == ClientRunStatus::Committed,
            "restart did not replay the retained transaction");
    wait_success(restarted);
    require(read_exact(retained) == input, "restart changed retained exact bytes");

    TestClient crash_client(Id128::from_u64(807));
    const PreparedTuHandle crash_prepared = crash_client.prepare(input);
    const pid_t crashed = start_server(self, Id128::from_u64(808), retained, true, &port);
    const ClientRunResult crash_result = run_client(crash_client, port, crash_prepared);
    require(crash_result.status == ClientRunStatus::Disconnected,
            "INSTALLING crash was not observed as a disconnect");
    wait_success(crashed, 73);
    const pid_t retry = start_server(self, Id128::from_u64(810), retained, false, &port);
    const ClientRunResult retry_result = run_client(crash_client, port, crash_prepared);
    require(retry_result.status == ClientRunStatus::Committed,
            "retry after INSTALLING crash was not idempotent");
    wait_success(retry);
    require(read_exact(retained) == input, "crash retry changed exact bytes");
    std::filesystem::remove(retained);
    std::cout << "p50_s3_real_process: PASS restart crash-wrap counters\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 5 && std::string_view(argv[1]) == "--server") {
        const bool crash = std::string_view(argv[2]) == "--crash";
        const FStoreGuid guid = Id128::from_u64(std::stoull(argv[3]));
        return server_process(0, guid, argv[4], crash, 3);
    }
    if (argc != 1) fail("usage: p50s3realprocess");
    return run_gate(argv[0]);
}
