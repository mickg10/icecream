/*
 * Real positive iceccd/sidecar/READY integration gate.
 *
 * This is intentionally a process test, not another Controller unit test.  A
 * fake scheduler activates one real iceccd; the daemon must start the actual
 * icecc-cache-service, publish a positive Login only after READY, hand one
 * Protocol-50 CACHE_SESSION to it, publish the one-shot absent->present pair,
 * and withdraw before orderly scheduler teardown.
 */
#include "config.h"
#include "comm.h"
#include "../cache/p50_incarnation_identity.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

#if defined(HAVE_LIBCAP_NG)
static int failures = 0;
#define REQUIRE(condition, text) do { \
    if (condition) std::fprintf(stderr, "ok - %s\n", text); \
    else { std::fprintf(stderr, "FAILED - %s\n", text); ++failures; } \
} while (0)

static int listen_ephemeral(int *port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0
            || ::listen(fd, 8) != 0) {
        ::close(fd);
        return -1;
    }
    if (const char *bytes = ::getenv("ICECC_TEST_SCHEDULER_RCVBUF");
        bytes != nullptr && *bytes != '\0') {
        const int value = std::atoi(bytes);
        if (value > 0)
            (void)::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &value, sizeof(value));
    }
    socklen_t size = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &size) != 0) {
        ::close(fd);
        return -1;
    }
    *port = ntohs(address.sin_port);
    return fd;
}

static int reserve_port()
{
    int port = 0;
    const int fd = listen_ephemeral(&port);
    if (fd >= 0) ::close(fd);
    return port;
}

static MsgChannel *accept_channel(int listener, int timeout_msec)
{
    pollfd descriptor{listener, POLLIN, 0};
    if (::poll(&descriptor, 1, timeout_msec) <= 0) return nullptr;
    const int fd = ::accept(listener, nullptr, nullptr);
    if (fd < 0) return nullptr;
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    return Service::createChannel(fd, reinterpret_cast<sockaddr *>(&peer), sizeof(peer));
}

static Msg *wait_for_type(MsgChannel *channel, Msg::Value type, int timeout_msec)
{
    if (!channel) return nullptr;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (Clock::now() < deadline) {
        Msg *message = channel->get_msg(1, true);
        if (message != nullptr) {
            if (*message == type) return message;
            delete message;
        }
        if (channel->at_eof()) return nullptr;
    }
    return nullptr;
}

static MsgChannel *connect_tcp_bounded(int port, int timeout_msec)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (Clock::now() < deadline) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return nullptr;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<uint16_t>(port));
        if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0)
            return Service::createChannel(fd, reinterpret_cast<sockaddr *>(&address),
                                          sizeof(address));
        ::close(fd);
        ::usleep(20000);
    }
    return nullptr;
}

static int connect_raw_tcp(int port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(fd);
        return -1;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<uint16_t>(port));
    const int connected = ::connect(
        fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
    if (connected < 0 && errno != EINPROGRESS) {
        ::close(fd);
        return -1;
    }
    if (connected < 0) {
        pollfd waiter{fd, POLLOUT, 0};
        const auto connect_deadline = Clock::now() + std::chrono::milliseconds(250);
        int ready = -1;
        while (true) {
            const auto remaining = std::chrono::duration_cast<
                std::chrono::milliseconds>(connect_deadline - Clock::now()).count();
            if (remaining <= 0) break;
            ready = ::poll(&waiter, 1, static_cast<int>(remaining));
            if (ready >= 0 || errno != EINTR) break;
        }
        int socket_error = 0;
        socklen_t socket_error_size = sizeof(socket_error);
        if (ready <= 0 || ::getsockopt(fd, SOL_SOCKET, SO_ERROR,
                                       &socket_error, &socket_error_size) < 0 ||
            socket_error != 0) {
            ::close(fd);
            return -1;
        }
    }
    return fd;
}

static bool write_all(int fd, const void *data, size_t size)
{
    const char *position = static_cast<const char *>(data);
    while (size != 0) {
        const ssize_t written = ::send(fd, position, size, MSG_NOSIGNAL);
        if (written > 0) {
            position += written;
            size -= static_cast<size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

/* Queue a complete protocol-50 negotiation plus an ordinary CACHE_SESSION
   frame while the daemon process is stopped.  The listener can then resume
   with the entire burst already resident in its kernel accept queue, making
   admission-versus-message-handling order deterministic rather than a client
   scheduling race. */
static int connect_preframed_cache_session(int port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return -1;
    }

    unsigned char wire[16]{};
    wire[0] = static_cast<unsigned char>(PROTOCOL_VERSION);
    wire[4] = static_cast<unsigned char>(PROTOCOL_VERSION);
    const uint32_t frame_size = htonl(sizeof(uint32_t));
    const uint32_t message = htonl(static_cast<uint32_t>(Msg::CACHE_SESSION));
    std::memcpy(wire + 8, &frame_size, sizeof(frame_size));
    std::memcpy(wire + 12, &message, sizeof(message));
    if (!write_all(fd, wire, sizeof(wire))) {
        ::close(fd);
        return -1;
    }
    return fd;
}

static bool wait_raw_eof(int fd, int timeout_msec)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    char buffer[64];
    while (Clock::now() < deadline) {
        pollfd descriptor{fd, POLLIN | POLLHUP | POLLERR, 0};
        const int remaining = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - Clock::now()).count());
        int ready = -1;
        do {
            ready = ::poll(&descriptor, 1, remaining > 0 ? remaining : 0);
        } while (ready < 0 && errno == EINTR);
        if (ready <= 0) return false;
        const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);
        if (count == 0) return true;
        if (count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            return false;
    }
    return false;
}

static std::string read_file_suffix(const std::string& path, uintmax_t offset)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    stream.seekg(static_cast<std::streamoff>(offset));
    return std::string(std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>());
}

static size_t count_text(const std::string& text, const std::string& needle)
{
    size_t count = 0;
    size_t position = 0;
    while ((position = text.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

static bool wait_eof(MsgChannel *channel, int timeout_msec)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (channel != nullptr && Clock::now() < deadline) {
        Msg *message = channel->get_msg(1, true);
        delete message;
        if (channel->at_eof()) return true;
    }
    return channel != nullptr && channel->at_eof();
}

static bool no_terminal_socket_event(MsgChannel *channel, int timeout_msec)
{
    if (channel == nullptr || channel->fd < 0)
        return false;
    pollfd descriptor{channel->fd, 0, 0};
    int result = -1;
    do {
        result = ::poll(&descriptor, 1, timeout_msec);
    } while (result < 0 && errno == EINTR);
    return result == 0 ||
           (result > 0 &&
            (descriptor.revents & (POLLHUP | POLLERR | POLLNVAL)) == 0);
}

static int process_fd_count(pid_t pid)
{
    std::error_code error;
    const std::filesystem::path directory =
        std::filesystem::path("/proc") / std::to_string(pid) / "fd";
    std::filesystem::directory_iterator position(directory, error);
    const std::filesystem::directory_iterator end;
    if (error) return -1;
    int count = 0;
    while (position != end) {
        ++count;
        position.increment(error);
        if (error) return -1;
    }
    return count;
}

static int wait_for_fd_count_at_most(pid_t pid, int maximum, int timeout_msec)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    int observed = process_fd_count(pid);
    while (observed > maximum && Clock::now() < deadline) {
        ::usleep(20000);
        observed = process_fd_count(pid);
    }
    return observed;
}

static bool wait_child(pid_t pid, int timeout_msec, int *status)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (Clock::now() < deadline) {
        const pid_t result = ::waitpid(pid, status, WNOHANG);
        if (result == pid) return true;
        if (result < 0) return false;
        ::usleep(20000);
    }
    return false;
}

static bool absent(const LoginMsg *login)
{
    return login != nullptr && login->cache_endpoint_port == 0
        && login->cache_protocol == 0 && login->cache_profile_mask == 0;
}

static bool present(const LoginMsg *login, uint32_t port)
{
    return login != nullptr && login->cache_endpoint_port == port
        && login->cache_protocol == CACHE_WIRE_REVISION
        && login->cache_profile_mask == CACHE_ADVERTISABLE_PROFILE_MASK;
}

static P50SourceArmFields source_arm(uint32_t wire_id, uint64_t epoch,
                                     uint64_t nonce, uint32_t daemon_port,
                                     uint32_t cache_port)
{
    P50SourceArmFields arm;
    arm.wire_job_id = wire_id;
    arm.assignment_epoch = epoch;
    arm.assignment_nonce = nonce;
    arm.selected_f_host = "127.0.0.1";
    arm.selected_f_ordinary_port = daemon_port;
    arm.selected_f_cache_port = cache_port;
    arm.cache_protocol = CACHE_WIRE_REVISION;
    arm.cache_profile = CACHE_PROFILE_ZSTD_TU;
    arm.logical_job = wire_id;
    arm.compiler_attempt = nonce;
    arm.c_store_generation = 1;
    arm.c_store_derivation_version =
        icecc::p50::kStoreIdentityDerivationVersion;
    arm.c_store_guid[0] = 0x11;
    arm.c_store_guid[1] = 0x22;
    arm.source_request_id = nonce;
    arm.source_mode = P50_SOURCE_MODE_ZSTD_TU;
    arm.c_control_generation = 1;
    arm.c_control_attempt = nonce;
    return arm;
}

static pid_t find_attachment_sidecar(pid_t daemon_pid, const char *service_path)
{
    const std::string wanted = service_path ? service_path : "";
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator("/proc", error)) {
        if (error) break;
        const std::string name = entry.path().filename().string();
        if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos)
            continue;
        std::ifstream stat(entry.path() / "stat");
        pid_t pid = -1, ppid = -1;
        char comm[256]{}, state = 0;
        if (!(stat >> pid >> comm >> state >> ppid) || ppid != daemon_pid) continue;
        std::ifstream cmd(entry.path() / "cmdline", std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(cmd)),
                                std::istreambuf_iterator<char>());
        if (!wanted.empty() && bytes.find(wanted) != std::string::npos) return pid;
    }
    return -1;
}

static bool wait_attachment_log(const std::string &path, uintmax_t offset,
                                const std::string &marker, int timeout_msec)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (Clock::now() < deadline) {
        std::ifstream stream(path, std::ios::binary);
        stream.seekg(static_cast<std::streamoff>(offset));
        const std::string suffix((std::istreambuf_iterator<char>(stream)),
                                 std::istreambuf_iterator<char>());
        if (suffix.find(marker) != std::string::npos) return true;
        ::usleep(10000);
    }
    return false;
}

static CompileJob attachment_compile_job(uint32_t wire_id, uint64_t epoch,
                                         uint64_t nonce,
                                         const P50SourceArmFields &arm)
{
    CompileJob job;
    job.setLanguage(CompileJob::Lang_CXX);
    job.setJobID(wire_id);
    job.setAssignmentIdentity(epoch, nonce);
    job.setCompileIdentity(31, 42);
    job.setEnvironmentVersion("env");
    job.setTargetPlatform("x86_64");
    job.setCompilerName("g++");
    job.setInputFile("in.ii");
    job.setWorkingDirectory("/tmp");
    job.setOutputFile("out.o");
    job.appendFlag("-O2", Arg_Remote);
    CompileInputIdentity input;
    input.profile = CompileInputIdentity::ZstdTuProfile;
    input.c_store_guid = arm.c_store_guid;
    input.tu_seq = 0;
    input.raw_bytes = 1;
    input.raw_digest.fill(0xa5);
    input.attempt_id = arm.compiler_attempt;
    input.request_id = arm.source_request_id;
    job.setCompileInputIdentity(input);
    return job;
}
#endif

int main(int argc, char **argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <iceccd> <icecc-cache-service>\n", argv[0]);
        return 2;
    }
    if (::getenv("ICECC_TEST_POSITIVE_DAEMON") == nullptr) {
        std::fprintf(stderr, "SKIP: set ICECC_TEST_POSITIVE_DAEMON=1 in an isolated root container\n");
        return 77;
    }
#if !defined(HAVE_LIBCAP_NG)
    std::fprintf(stderr, "SKIP: positive remote daemon test requires libcap-ng\n");
    return 77;
#else
    if (::geteuid() != 0) {
        std::fprintf(stderr, "SKIP: positive remote daemon test requires container root\n");
        return 77;
    }
    passwd *icecc = ::getpwnam("icecc");
    if (icecc == nullptr || icecc->pw_uid == 0 || icecc->pw_gid == 0) {
        std::fprintf(stderr, "SKIP: isolated image has no unprivileged icecc identity\n");
        return 77;
    }

    ::signal(SIGPIPE, SIG_IGN);
    const char *temporary_root = ::getenv("TMPDIR");
    const std::string prefix = temporary_root && *temporary_root
        ? temporary_root : "/tmp";
    std::string pattern = prefix + "/p50dp.XXXXXX";
    std::vector<char> mutable_pattern(pattern.begin(), pattern.end());
    mutable_pattern.push_back('\0');
    char *created = ::mkdtemp(mutable_pattern.data());
    REQUIRE(created != nullptr, "positive daemon temporary root created");
    if (created == nullptr) return 2;
    const std::string work(created);
    const std::string envdir = work + "/envs";
    const std::string runtime = work + "/runtime";
    const std::string local_socket = work + "/iceccd.sock";
    const std::string log = work + "/iceccd.log";
    if (::getenv("ICECC_TEST_SCHEDULER_BACKPRESSURE") != nullptr) {
        const std::string marker = work + "/scheduler-send-eagain";
        ::setenv("ICECC_TEST_SEND_EAGAIN_MARKER", marker.c_str(), 1);
    }
    REQUIRE(::chown(work.c_str(), icecc->pw_uid, icecc->pw_gid) == 0
                && ::chmod(work.c_str(), 0700) == 0,
            "temporary root belongs only to the daemon identity");
    REQUIRE(::mkdir(envdir.c_str(), 0700) == 0
                && ::chown(envdir.c_str(), icecc->pw_uid, icecc->pw_gid) == 0,
            "environment directory has daemon ownership");
    REQUIRE(::mkdir(runtime.c_str(), 0700) == 0
                && ::chown(runtime.c_str(), icecc->pw_uid, icecc->pw_gid) == 0,
            "sidecar runtime directory has exact private ownership");

    int scheduler_port = 0;
    const int scheduler_listener = listen_ephemeral(&scheduler_port);
    const int daemon_port = reserve_port();
    REQUIRE(scheduler_listener >= 0 && scheduler_port > 0,
            "fake scheduler listens on an ephemeral port");
    REQUIRE(daemon_port > 0, "real daemon public port reserved");
    if (scheduler_listener < 0 || daemon_port <= 0) return 2;

    const pid_t daemon_pid = ::fork();
    if (daemon_pid == 0) {
        char scheduler[64];
        char public_port[16];
        std::snprintf(scheduler, sizeof(scheduler), "127.0.0.1:%d", scheduler_port);
        std::snprintf(public_port, sizeof(public_port), "%d", daemon_port);
        ::setenv("ICECC_TESTS", "1", 1);
        ::setenv("ICECC_TEST_SOCKET", local_socket.c_str(), 1);
        char scheduler_port_text[16];
        std::snprintf(scheduler_port_text, sizeof(scheduler_port_text), "%d", scheduler_port);
        ::setenv("ICECC_TEST_BACKPRESSURE_SCHED_PORT", scheduler_port_text, 1);
        if (const char *shim = ::getenv("ICECC_TEST_SNDBUF_SHIM");
            shim != nullptr && *shim != '\0')
            ::setenv("LD_PRELOAD", shim, 1);
        ::execl(argv[1], argv[1], "-p", public_port, "-m", "1",
                "-s", scheduler, "-n", "p50-daemon-positive", "-N", "p50-f",
                "-b", envdir.c_str(), "-l", log.c_str(),
                "--cache-service", argv[2], "--cache-runtime-dir", runtime.c_str(),
                "-v", "-v", "-v", static_cast<char *>(nullptr));
        ::_exit(127);
    }
    REQUIRE(daemon_pid > 0, "real positive iceccd process started");
    if (daemon_pid <= 0) return 2;

    MsgChannel *scheduler = accept_channel(scheduler_listener, 10000);
    Msg *initial_message = wait_for_type(scheduler, Msg::LOGIN, 5000);
    LoginMsg *initial = dynamic_cast<LoginMsg *>(initial_message);
    REQUIRE(scheduler != nullptr && initial != nullptr,
            "fake scheduler received the initial real Login");
    REQUIRE(absent(initial), "initial Login is canonical cache absence before ConfCS/READY");
    delete initial_message;

    MsgChannel *premature = connect_tcp_bounded(daemon_port, 5000);
    REQUIRE(premature != nullptr,
            "pre-activation Protocol-50 client reached the real public listener");
    REQUIRE(premature && premature->send_msg(CacheSessionMsg()),
            "pre-activation client sent CACHE_SESSION");
    REQUIRE(wait_eof(premature, 5000),
            "LOGIN_ATTEMPT cannot dispatch cache while scheduler is inactive");
    delete premature;

    const uint64_t epoch = UINT64_C(0x5000000000000001);
    const ConfCSMsg activate(epoch, ConfCSMsg::StrictNonce);
    REQUIRE(scheduler && scheduler->send_msg(activate),
            "first ConfCS activates the scheduler session");
    Msg *positive_message = wait_for_type(scheduler, Msg::LOGIN, 10000);
    LoginMsg *positive = dynamic_cast<LoginMsg *>(positive_message);
    REQUIRE(present(positive, static_cast<uint32_t>(daemon_port)),
            "real READY/authenticated sidecar publishes exact positive advertisement");
    delete positive_message;

    // Diagnostic regression: a synchronous compiler-input attachment must not
    // prevent a fresh ordinary peer from completing admission.  The sidecar
    // is test-owned and always resumed after a successful stop.
    MsgChannel *attach_healthy_peer = connect_tcp_bounded(daemon_port, 5000);
    const bool attach_healthy_greeting = attach_healthy_peer &&
        attach_healthy_peer->send_msg(CacheSessionMsg());
    delete attach_healthy_peer;
    REQUIRE(attach_healthy_greeting,
            "attachment diagnostic healthy-sidecar peer greeting succeeds");

    const bool disconnect_pending = std::getenv("ICECC_TEST_PENDING_DISCONNECT") != nullptr;
    const uint32_t attach_wire_id = 0x7a710101 + unsigned(disconnect_pending);
    // Assignment epochs are scheduler-session scoped; keep the active epoch
    // and mint a distinct wire/nonce pair for this diagnostic.
    const uint64_t attach_epoch = epoch;
    const uint64_t attach_nonce = UINT64_C(0x7a71010100000001) + unsigned(disconnect_pending);
    const P50SourceArmFields attach_arm = source_arm(
        attach_wire_id, attach_epoch, attach_nonce,
        static_cast<uint32_t>(daemon_port), static_cast<uint32_t>(daemon_port));
    REQUIRE(scheduler && scheduler->send_msg(
                AssignPrepareMsg(attach_epoch, attach_wire_id, attach_nonce, 1)),
            "attachment diagnostic PREPARE reaches the daemon");
    Msg *attach_ready_message = wait_for_type(scheduler, Msg::ASSIGN_READY, 5000);
    auto *attach_ready = dynamic_cast<AssignReadyMsg *>(attach_ready_message);
    REQUIRE(attach_ready != nullptr && attach_ready->wire_id == attach_wire_id &&
                attach_ready->epoch() == attach_epoch && attach_ready->nonce() == attach_nonce,
            "attachment diagnostic assignment is authenticated");
    delete attach_ready_message;

    MsgChannel *attach_wrapper = connect_tcp_bounded(daemon_port, 5000);
    REQUIRE(attach_wrapper && attach_wrapper->send_msg(P50SourceArmMsg(attach_arm)),
            "attachment diagnostic source arm is sent");
    Msg *attach_armed_message = wait_for_type(attach_wrapper, Msg::P50_SOURCE_ARMED, 5000);
    auto *attach_armed = dynamic_cast<P50SourceArmedMsg *>(attach_armed_message);
    REQUIRE(attach_armed != nullptr && attach_armed->arm == attach_arm,
            "attachment diagnostic source arm is acknowledged");
    delete attach_armed_message;
    REQUIRE(attach_wrapper && attach_wrapper->send_msg(CacheSessionMsg()),
            "attachment diagnostic wrapper enters CACHE_SESSION");
    ::usleep(100 * 1000);

    const pid_t attach_sidecar_pid = find_attachment_sidecar(daemon_pid, argv[2]);
    const bool attach_stop_sent = attach_sidecar_pid > 1 &&
        ::kill(attach_sidecar_pid, SIGSTOP) == 0;
    bool attach_stopped = false;
    if (attach_stop_sent) {
        const auto stop_deadline = Clock::now() + std::chrono::milliseconds(2000);
        while (Clock::now() < stop_deadline) {
            std::ifstream status(std::string("/proc/") +
                                 std::to_string(attach_sidecar_pid) + "/status");
            std::string line;
            while (std::getline(status, line))
                if (line.rfind("State:", 0) == 0 && line.find('T') != std::string::npos)
                    attach_stopped = true;
            if (attach_stopped) break;
            ::usleep(10000);
        }
    }
    REQUIRE(attach_stopped, "attachment diagnostic stops only its test-owned sidecar");

    std::error_code attach_log_error;
    const uintmax_t attach_log_offset = std::filesystem::file_size(log, attach_log_error);
    MsgChannel *attach_compile_client = connect_tcp_bounded(daemon_port, 5000);
    CompileJob attach_job = attachment_compile_job(
        attach_wire_id, attach_epoch, attach_nonce, attach_arm);
    const bool attach_compile_sent = attach_compile_client &&
        attach_compile_client->send_msg(CompileFileMsg(&attach_job));
    REQUIRE(attach_compile_sent, "attachment diagnostic valid CompileFile is sent");
    const std::string attach_begin_marker =
        "P50_INPUT_ATTACH_BEGIN job=" + std::to_string(attach_wire_id) +
        " epoch=" + std::to_string(attach_epoch) +
        " nonce=" + std::to_string(attach_nonce) +
        " request=" + std::to_string(attach_arm.source_request_id);
    const std::string attach_end_marker =
        "P50_INPUT_ATTACH_END job=" + std::to_string(attach_wire_id) +
        " epoch=" + std::to_string(attach_epoch) +
        " nonce=" + std::to_string(attach_nonce) +
        " request=" + std::to_string(attach_arm.source_request_id);
    const bool attach_begin_seen = attach_compile_sent && !attach_log_error &&
        wait_attachment_log(log, attach_log_offset, attach_begin_marker, 3000);
    REQUIRE(attach_begin_seen, "attachment handler begins exact authenticated attach");

    // While the authenticated CompileFile remains active, keep a bounded
    // remote admission burst outstanding and prove that a fresh source-arm
    // still receives its ACK within the existing four-second budget.  This
    // uses the real attachment path above as ordinary daemon work; it does
    // not substitute Ping traffic or assert compile success.
    constexpr size_t kCompileAdmissionBurstCount = 64;
    std::vector<int> compile_admission_fds;
    compile_admission_fds.reserve(kCompileAdmissionBurstCount);
    for (size_t peer = 0; peer != kCompileAdmissionBurstCount; ++peer) {
        const int fd = connect_raw_tcp(daemon_port);
        if (fd < 0) break;
        compile_admission_fds.push_back(fd);
    }
    const uint32_t concurrent_wire_id = attach_wire_id + 1;
    const uint64_t concurrent_nonce = attach_nonce + 1;
    const P50SourceArmFields concurrent_arm = source_arm(
        concurrent_wire_id, attach_epoch, concurrent_nonce,
        static_cast<uint32_t>(daemon_port), static_cast<uint32_t>(daemon_port));
    const bool concurrent_prepare = scheduler && scheduler->send_msg(
        AssignPrepareMsg(attach_epoch, concurrent_wire_id, concurrent_nonce, 1));
    Msg *concurrent_ready_message = concurrent_prepare
        ? wait_for_type(scheduler, Msg::ASSIGN_READY, 5000) : nullptr;
    auto *concurrent_ready = dynamic_cast<AssignReadyMsg *>(concurrent_ready_message);
    const bool concurrent_ready_valid = concurrent_ready != nullptr &&
        concurrent_ready->wire_id == concurrent_wire_id &&
        concurrent_ready->epoch() == attach_epoch &&
        concurrent_ready->nonce() == concurrent_nonce;
    delete concurrent_ready_message;
    MsgChannel *concurrent_source = concurrent_ready_valid
        ? connect_tcp_bounded(daemon_port, 5000) : nullptr;
    const auto concurrent_arm_started = Clock::now();
    const bool concurrent_arm_sent = concurrent_source &&
        concurrent_source->send_msg(P50SourceArmMsg(concurrent_arm));
    Msg *concurrent_armed_message = concurrent_arm_sent
        ? wait_for_type(concurrent_source, Msg::P50_SOURCE_ARMED, 5000) : nullptr;
    auto *concurrent_armed = dynamic_cast<P50SourceArmedMsg *>(concurrent_armed_message);
    const auto concurrent_arm_elapsed = std::chrono::duration_cast<
        std::chrono::milliseconds>(Clock::now() - concurrent_arm_started).count();
    const bool concurrent_arm_ack = concurrent_armed != nullptr &&
        concurrent_armed->arm == concurrent_arm && concurrent_arm_elapsed < 4000;
    delete concurrent_armed_message;
    delete concurrent_source;
    for (int fd : compile_admission_fds) ::close(fd);
    REQUIRE(compile_admission_fds.size() == kCompileAdmissionBurstCount,
            "remote admission burst remains active during an ordinary CompileFile");
    REQUIRE(concurrent_prepare && concurrent_ready_valid && concurrent_arm_sent &&
                concurrent_arm_ack,
            "source-arm ACK stays within budget during ordinary work and admissions");
    const bool attachment_still_pending =
        read_file_suffix(log, attach_log_offset).find(attach_end_marker) ==
        std::string::npos;
    REQUIRE(attachment_still_pending,
            "ordinary CompileFile remains pending during concurrent source admission");

    bool attach_resumed = false;
    if (disconnect_pending) {
        delete attach_compile_client;
        attach_compile_client = nullptr;
        attach_resumed = attach_stop_sent && ::kill(attach_sidecar_pid, SIGCONT) == 0;
    }

    std::atomic<bool> attach_peer_connected{false};
    std::atomic<bool> attach_peer_greeting{false};
    std::thread attach_peer([&] {
        MsgChannel *peer = connect_tcp_bounded(daemon_port, 5000);
        attach_peer_connected.store(peer != nullptr);
        const bool sent = peer && peer->send_msg(CacheSessionMsg());
        attach_peer_greeting.store(sent);
        delete peer;
    });
    ::usleep(1200 * 1000);
    REQUIRE(attach_begin_seen && attach_peer_connected.load() && attach_peer_greeting.load() &&
                read_file_suffix(log, attach_log_offset).find(attach_end_marker) == std::string::npos,
            "peer handshake completes while exact attachment remains pending");
    if (!disconnect_pending)
        attach_resumed = attach_stop_sent && ::kill(attach_sidecar_pid, SIGCONT) == 0;
    REQUIRE(attach_resumed, "attachment diagnostic resumes its stopped sidecar");
    attach_peer.join();
    const bool attach_compile_bounded = disconnect_pending ||
        (attach_compile_client && wait_eof(attach_compile_client, 8000));
    delete attach_compile_client;
    delete attach_wrapper;
    REQUIRE(attach_peer_connected.load() && attach_peer_greeting.load(),
            "peer admission resumes after attachment sidecar continuation");
    REQUIRE(attach_compile_bounded, "attachment diagnostic CompileFile terminates boundedly");
    if (disconnect_pending) {
        // No input was committed in this fixture: unknown-record proves the
        // exact pending cancellation reached the sidecar, not lease revocation.
        const std::string cancelled = "P50 input settlement job " +
            std::to_string(attach_wire_id) + " action 1 status unknown-record";
        REQUIRE(wait_attachment_log(log, attach_log_offset, cancelled, 3000),
                "pending disconnect delivers CancelAttempt to the sidecar");
        REQUIRE(read_file_suffix(log, attach_log_offset).find(attach_end_marker) ==
                    std::string::npos,
                "disconnected attachment is not resumed or published");
    } else {
        REQUIRE(wait_attachment_log(log, attach_log_offset, attach_end_marker, 2000),
                "attachment handler emits exact END after sidecar continuation");
    }
    // Stop only this private test daemon, queue one more connection than the
    // 64-socket accept quantum, and resume it.  Every connection already
    // contains protocol negotiation and a deliberately unarmed CACHE_SESSION.
    // The 65th socket crosses the accept quantum and was absent from the
    // original poll snapshot. Admission must progress between ordinary client
    // activities; otherwise a busy compile turn can strand its protocol
    // greeting past a legacy client's fixed deadline.
    constexpr size_t kAdmissionBurstCount = 65;
    std::error_code size_error;
    const uintmax_t admission_log_offset = std::filesystem::file_size(log, size_error);
    const bool stop_sent = ::kill(daemon_pid, SIGSTOP) == 0;
    int stop_status = 0;
    const bool stopped = stop_sent &&
        ::waitpid(daemon_pid, &stop_status, WUNTRACED) == daemon_pid &&
        WIFSTOPPED(stop_status);
    std::vector<int> admission_fds;
    if (stopped) {
        for (size_t index = 0; index != kAdmissionBurstCount; ++index) {
            const int fd = connect_preframed_cache_session(daemon_port);
            if (fd < 0) break;
            admission_fds.push_back(fd);
        }
    }
    const bool resumed = ::kill(daemon_pid, SIGCONT) == 0;
    bool admission_eof = stopped && resumed &&
        admission_fds.size() == kAdmissionBurstCount;
    for (int fd : admission_fds) {
        if (!wait_raw_eof(fd, 5000)) admission_eof = false;
        ::close(fd);
    }
    const std::string admission_log = size_error
        ? std::string() : read_file_suffix(log, admission_log_offset);
    const std::string accepted_marker = "accepted ";
    const std::string refused_marker =
        "CACHE_SESSION refused: no authenticated cache sidecar";
    const size_t accepted_count = count_text(admission_log, accepted_marker);
    const size_t refused_count = count_text(admission_log, refused_marker);
    const size_t last_accept = admission_log.rfind(accepted_marker);
    const size_t first_refusal = admission_log.find(refused_marker);
    size_t refusals_before_last_accept = 0;
    if (last_accept != std::string::npos) {
        size_t offset = admission_log.find(refused_marker);
        while (offset != std::string::npos && offset < last_accept) {
            ++refusals_before_last_accept;
            offset = admission_log.find(refused_marker,
                                        offset + refused_marker.size());
        }
    }
    const bool admission_ordered = admission_eof &&
        accepted_count == kAdmissionBurstCount &&
        refused_count == kAdmissionBurstCount &&
        last_accept != std::string::npos && first_refusal != std::string::npos &&
        refusals_before_last_accept <= 1;
    REQUIRE(stopped && admission_fds.size() == kAdmissionBurstCount && resumed,
            "burst beyond the accept quantum queued before daemon admission");
    REQUIRE(admission_eof,
            "all preframed burst clients are terminally handled after admission");
    REQUIRE(admission_ordered,
            "turn-boundary admission waits behind at most one client activity");

    // Fill the remote share of the asynchronous handshake table with silent
    // TCP peers. The protected local share must still admit Unix wrappers;
    // a single global cap lets remote silence starve this path indefinitely.
    const int pre_saturation_daemon_fds = process_fd_count(daemon_pid);
    constexpr size_t kRemoteSaturationCount = 256;
    std::vector<int> silent_remote_fds;
    silent_remote_fds.reserve(kRemoteSaturationCount);
    for (size_t index = 0; index != kRemoteSaturationCount; ++index) {
        const int fd = connect_raw_tcp(daemon_port);
        if (fd < 0) break;
        silent_remote_fds.push_back(fd);
    }
    // Give the running daemon a deterministic chance to consume the public
    // queue up to its remote sub-limit before presenting the local peer.
    ::usleep(100 * 1000);
    const auto local_admission_started = Clock::now();
    MsgChannel *reserved_local = Service::createChannel(local_socket);
    const auto local_admission_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - local_admission_started).count();
    const bool local_admitted = reserved_local != nullptr &&
        local_admission_elapsed < 4000;
    const bool local_refused_normally = reserved_local != nullptr &&
        reserved_local->send_msg(CacheSessionMsg()) &&
        wait_eof(reserved_local, 4000);
    delete reserved_local;
    for (int fd : silent_remote_fds) ::close(fd);
    REQUIRE(silent_remote_fds.size() == kRemoteSaturationCount,
            "remote silent peers saturate more than the protected remote share");
    REQUIRE(local_admitted && local_refused_normally,
            "remote handshake saturation preserves prompt Unix-client admission");
    const int post_saturation_daemon_fds = wait_for_fd_count_at_most(
        daemon_pid, pre_saturation_daemon_fds + 2, 5000);
    REQUIRE(post_saturation_daemon_fds >= 0 &&
                post_saturation_daemon_fds <= pre_saturation_daemon_fds + 2,
            "remote saturation teardown releases every pending descriptor");

    const int baseline_daemon_fds = process_fd_count(daemon_pid);
    REQUIRE(baseline_daemon_fds > 0,
            "real daemon descriptor baseline is observable");

    // The removed implementation opened and retained one second P5FS control
    // relationship for every authoritative CacheSession.  Its table was
    // bounded at 64, so 65 sequential handoffs are the smallest production
    // witness that distinguishes the single authoritative path from that
    // shadow-owner design.
    constexpr size_t kAuthoritativeSessionCount = 65;
    size_t authoritative_sessions = 0;
    bool sequence_valid = baseline_daemon_fds > 0;
    bool first_prepare = false;
    bool first_ready = false;
    bool first_connected = false;
    bool first_arm_sent = false;
    bool first_armed = false;
    bool first_stalled_peer_opened = false;
    bool first_concurrent_stalled_peers_opened = false;
    bool first_arm_bypassed_stalled_peer = false;
    bool first_cache_session_sent = false;
    bool first_adopted_live = false;
    for (size_t index = 0; sequence_valid &&
                           index != kAuthoritativeSessionCount; ++index) {
        const uint32_t wire_id = static_cast<uint32_t>(7101 + index);
        const uint64_t nonce = UINT64_C(0x7101000000000001) + index;
        const bool prepared = scheduler && scheduler->send_msg(
            AssignPrepareMsg(epoch, wire_id, nonce, 1));
        if (index == 0) first_prepare = prepared;
        if (!prepared) {
            sequence_valid = false;
            break;
        }

        Msg *ready_message = wait_for_type(scheduler, Msg::ASSIGN_READY, 5000);
        auto *ready = dynamic_cast<AssignReadyMsg *>(ready_message);
        const bool ready_valid = ready != nullptr && ready->wire_id == wire_id &&
            ready->epoch() == epoch && ready->nonce() == nonce;
        if (index == 0) first_ready = ready_valid;
        delete ready_message;
        if (!ready_valid) {
            sequence_valid = false;
            break;
        }

        int stalled_protocol_fd = -1;
        std::vector<int> concurrent_stalled_fds;
        Clock::time_point arm_admission_started{};
        if (index == 0) {
            const bool stop_sent = ::kill(daemon_pid, SIGSTOP) == 0;
            int stop_status = 0;
            const bool stopped = stop_sent &&
                ::waitpid(daemon_pid, &stop_status, WUNTRACED) == daemon_pid &&
                WIFSTOPPED(stop_status);
            constexpr size_t kConcurrentStalledPeerCount = 64;
            if (stopped)
                stalled_protocol_fd = connect_raw_tcp(daemon_port);
            if (stalled_protocol_fd >= 0) {
                concurrent_stalled_fds.reserve(kConcurrentStalledPeerCount);
                for (size_t peer = 0; peer != kConcurrentStalledPeerCount; ++peer) {
                    const int fd = connect_raw_tcp(daemon_port);
                    if (fd < 0) break;
                    concurrent_stalled_fds.push_back(fd);
                }
            }
            first_stalled_peer_opened = stalled_protocol_fd >= 0;
            first_concurrent_stalled_peers_opened =
                concurrent_stalled_fds.size() == kConcurrentStalledPeerCount;
            const bool resumed = ::kill(daemon_pid, SIGCONT) == 0;
            if (!stopped || !first_stalled_peer_opened ||
                !first_concurrent_stalled_peers_opened || !resumed) {
                if (stalled_protocol_fd >= 0) ::close(stalled_protocol_fd);
                for (int fd : concurrent_stalled_fds) ::close(fd);
                sequence_valid = false;
                break;
            }
            arm_admission_started = Clock::now();
            // Let the daemon accept the silent socket first. The predecessor
            // then blocks its only event-loop thread in wait_for_protocol().
            ::usleep(50 * 1000);
        }

        MsgChannel *ordinary = connect_tcp_bounded(daemon_port, 5000);
        if (index == 0) first_connected = ordinary != nullptr;
        if (ordinary == nullptr) {
            if (stalled_protocol_fd >= 0) ::close(stalled_protocol_fd);
            for (int fd : concurrent_stalled_fds) ::close(fd);
            sequence_valid = false;
            break;
        }
        const P50SourceArmFields arm = source_arm(
            wire_id, epoch, nonce, static_cast<uint32_t>(daemon_port),
            static_cast<uint32_t>(daemon_port));
        const bool arm_sent = ordinary->send_msg(P50SourceArmMsg(arm));
        if (index == 0) first_arm_sent = arm_sent;
        if (!arm_sent) {
            if (stalled_protocol_fd >= 0) ::close(stalled_protocol_fd);
            for (int fd : concurrent_stalled_fds) ::close(fd);
            delete ordinary;
            sequence_valid = false;
            break;
        }
        Msg *armed_message = wait_for_type(ordinary, Msg::P50_SOURCE_ARMED, 5000);
        auto *armed = dynamic_cast<P50SourceArmedMsg *>(armed_message);
        const bool armed_valid = armed != nullptr && armed->arm == arm &&
            armed->f_store_generation != 0;
        if (index == 0) {
            first_armed = armed_valid;
            const auto arm_admission_elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    Clock::now() - arm_admission_started).count();
            first_arm_bypassed_stalled_peer =
                armed_valid && arm_admission_elapsed < 4000;
        }
        delete armed_message;
        if (stalled_protocol_fd >= 0) ::close(stalled_protocol_fd);
        for (int fd : concurrent_stalled_fds) ::close(fd);
        if (!armed_valid) {
            delete ordinary;
            sequence_valid = false;
            break;
        }
        const bool cache_session_sent = ordinary->send_msg(CacheSessionMsg());
        if (index == 0) first_cache_session_sent = cache_session_sent;
        if (!cache_session_sent) {
            delete ordinary;
            sequence_valid = false;
            break;
        }
        // CACHE_SESSION changes this descriptor from framed icecream messages
        // to raw CacheWire.  A shadow-owner capacity refusal closes it here;
        // an accepted authoritative handoff remains live until this test ends
        // the intentionally empty raw session.
        const bool adopted_live = no_terminal_socket_event(ordinary, 50);
        if (index == 0) first_adopted_live = adopted_live;
        delete ordinary;
        if (!adopted_live) {
            sequence_valid = false;
            break;
        }
        ++authoritative_sessions;
        // Let the endpoint owner consume EOF and release its one live handoff
        // before presenting the next sequential session.
        ::usleep(20000);
    }
    REQUIRE(first_prepare, "source-arm PREPARE reaches the production daemon");
    REQUIRE(first_ready, "production daemon accepts the exact source-arm assignment");
    REQUIRE(first_connected,
            "ordinary Protocol-50 client reached the real public listener");
    REQUIRE(first_arm_sent, "exact source arm entered the production daemon path");
    REQUIRE(first_stalled_peer_opened,
            "silent protocol peer was admitted before the source-arm peer");
    REQUIRE(first_concurrent_stalled_peers_opened,
            "concurrent silent protocol peers were queued before the source-arm peer");
    REQUIRE(first_armed,
            "source-arm owner is acknowledged before CACHE_SESSION");
    REQUIRE(first_arm_bypassed_stalled_peer,
            "source-arm acknowledgement bypasses a silent accepted handshake");
    REQUIRE(first_cache_session_sent,
            "real CACHE_SESSION entered the production daemon path");
    REQUIRE(first_adopted_live,
            "authenticated one-shot handoff keeps the adopted session live");
    REQUIRE(sequence_valid &&
                authoritative_sessions == kAuthoritativeSessionCount,
            "more than 64 sequential authoritative CacheSessions remain accepted");

    // The event-loop conversion preserves the historical 15-second peer
    // budget: silence no longer blocks useful work, but it also cannot retain
    // a pending descriptor indefinitely.
    const int expiry_fd = connect_raw_tcp(daemon_port);
    const auto expiry_started = Clock::now();
    const bool expiry_eof = expiry_fd >= 0 && wait_raw_eof(expiry_fd, 17000);
    const auto expiry_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - expiry_started).count();
    if (expiry_fd >= 0) ::close(expiry_fd);
    REQUIRE(expiry_eof && expiry_elapsed >= 14000 && expiry_elapsed < 17000,
            "silent accepted handshake expires on the preserved bounded lifetime");

    const int settled_daemon_fds = wait_for_fd_count_at_most(
        daemon_pid, baseline_daemon_fds + 2, 5000);
    REQUIRE(settled_daemon_fds >= 0 &&
                settled_daemon_fds <= baseline_daemon_fds + 2,
            "authoritative CacheSessions leave no retained P5FS descriptors");
    Msg *spurious_login = wait_for_type(scheduler, Msg::LOGIN, 250);
    REQUIRE(spurious_login == nullptr,
            "accepted handoff keeps the READY advertisement stable");
    delete spurious_login;

    if (std::getenv("ICECC_TEST_SCHEDULER_BACKPRESSURE") != nullptr) {
        /*
         * This is deliberately opt-in: the fake scheduler stops reading its
         * established socket while the daemon processes authenticated
         * AssignPrepare requests.  AssignReady is emitted through the real
         * Daemon::send_scheduler() path, so a full scheduler receive window
         * exercises the same blocking flush as compile completion.  A fresh
         * protocol peer must still receive the daemon's greeting within the
         * existing five-second admission budget.
         *
         * The test is expected to fail against a daemon that blocks in
         * flush_writebuf(); the isolated daemon is terminated after the
         * bounded probe so the flood thread cannot outlive this test.
         */
        constexpr size_t kFloodCount = 20000;
        std::atomic<size_t> flood_sent{0};
        std::atomic<bool> flood_started{false};
        std::thread flood([&] {
            flood_started.store(true);
            for (size_t index = 0; index != kFloodCount; ++index) {
                const uint32_t wire_id = static_cast<uint32_t>(0x7f000000u + index);
                const uint64_t nonce = UINT64_C(0x7f00000000000001) + index;
                if (!scheduler || !scheduler->send_msg(
                        AssignPrepareMsg(epoch, wire_id, nonce, 1)))
                    break;
                ++flood_sent;
            }
        });
        while (!flood_started.load()) ::usleep(1000);
        const char *eagain_marker = std::getenv("ICECC_TEST_SEND_EAGAIN_MARKER");
        const auto eagain_deadline = Clock::now() + std::chrono::milliseconds(5000);
        while (eagain_marker && *eagain_marker && Clock::now() < eagain_deadline) {
            std::error_code marker_error;
            if (std::filesystem::file_size(eagain_marker, marker_error) > 0 &&
                !marker_error)
                break;
            ::usleep(10000);
        }
        std::error_code eagain_error;
        const uintmax_t eagain_count = eagain_marker && *eagain_marker
            ? std::filesystem::file_size(eagain_marker, eagain_error) : 0;
        const bool observed_backpressure = !eagain_error && eagain_count > 0;
        MsgChannel *probe = observed_backpressure ? Service::createChannelUntil(
            "127.0.0.1", static_cast<unsigned short>(daemon_port),
            Clock::now() + std::chrono::milliseconds(5000)) : nullptr;
        const bool greeting_valid = probe != nullptr;
        delete probe;
        REQUIRE(flood_sent.load() > 0,
                "scheduler backpressure flood entered the real daemon path");
        REQUIRE(observed_backpressure,
                "child daemon observed send-side EAGAIN under scheduler backpressure");
        REQUIRE(greeting_valid,
                "fresh protocol admission completes within five seconds after confirmed scheduler backpressure");
        if (greeting_valid && observed_backpressure) {
            flood.join();
            REQUIRE(flood_sent.load() == kFloodCount,
                    "finite scheduler input flood completed without loss");
            if (::getenv("ICECC_TEST_SCHEDULER_BACKPRESSURE_EXPIRE") != nullptr) {
                REQUIRE(wait_attachment_log(log, 0,
                            "scheduler deferred output deadline expired", 33000),
                        "silent scheduler backlog expires on its existing deadline");
            } else {
                size_t replies = 0;
                bool intact = true;
                const auto drain_deadline = Clock::now() + std::chrono::seconds(25);
                while (replies < kFloodCount && Clock::now() < drain_deadline) {
                    Msg *reply = scheduler->get_msg(1, true);
                    if (!reply) {
                        if (scheduler->at_eof()) break;
                        continue;
                    }
                    if (*reply == Msg::ASSIGN_READY) {
                        const auto *ready = dynamic_cast<const AssignReadyMsg *>(reply);
                        intact = intact && ready && ready->epoch() == epoch &&
                            ready->wire_id == static_cast<uint32_t>(0x7f000000u + replies) &&
                            ready->nonce() == UINT64_C(0x7f00000000000001) + replies;
                        ++replies;
                    }
                    delete reply;
                }
                REQUIRE(intact && replies == kFloodCount,
                        "all deferred scheduler replies drain intact in FIFO order");
            }
        }
        ::kill(daemon_pid, SIGKILL);
        int backpressure_status = 0;
        (void)::waitpid(daemon_pid, &backpressure_status, 0);
        if (flood.joinable()) flood.join();
        delete scheduler;
        ::close(scheduler_listener);
        std::fprintf(stderr, "retained backpressure work directory: %s; marker=%s; count=%ju; sent=%zu\n",
                     work.c_str(), eagain_marker ? eagain_marker : "missing",
                     eagain_count, flood_sent.load());
        return failures ? 1 : 0;
    }

    const int shutdown_pending_fd = connect_raw_tcp(daemon_port);
    ::usleep(50 * 1000);
    ::kill(daemon_pid, SIGTERM);
    Msg *shutdown_message = wait_for_type(scheduler, Msg::LOGIN, 5000);
    LoginMsg *shutdown_login = dynamic_cast<LoginMsg *>(shutdown_message);
    REQUIRE(absent(shutdown_login), "orderly shutdown withdraws before scheduler teardown");
    delete shutdown_message;
    const bool shutdown_pending_closed = shutdown_pending_fd >= 0 &&
        wait_raw_eof(shutdown_pending_fd, 5000);
    if (shutdown_pending_fd >= 0) ::close(shutdown_pending_fd);
    REQUIRE(shutdown_pending_closed,
            "orderly shutdown closes a still-pending protocol admission");

    int status = 0;
    bool reaped = wait_child(daemon_pid, 10000, &status);
    if (!reaped) {
        ::kill(daemon_pid, SIGKILL);
        (void)::waitpid(daemon_pid, &status, 0);
    }
    REQUIRE(reaped, "real positive iceccd exits under the shutdown bound");
    REQUIRE(reaped && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "real positive iceccd exits cleanly");

    delete scheduler;
    ::close(scheduler_listener);
    if (failures == 0) std::filesystem::remove_all(work);
    else std::fprintf(stderr, "retained failing work directory: %s\n", work.c_str());
    return failures ? 1 : 0;
#endif
}
