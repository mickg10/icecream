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
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
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

    // Stop only this private test daemon, queue a complete 36-connection burst
    // (the S70 workload concurrency), and resume it.  Every connection already
    // contains protocol negotiation and a deliberately unarmed CACHE_SESSION.
    // An admission-only batch must log all accepts before the first ordinary
    // message refusal; the predecessor handled one CACHE_SESSION between each
    // pair of accepts and fails this exact ordering check.
    constexpr size_t kAdmissionBurstCount = 36;
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
    const bool admission_ordered = admission_eof &&
        accepted_count == kAdmissionBurstCount &&
        refused_count == kAdmissionBurstCount &&
        last_accept != std::string::npos && first_refusal != std::string::npos &&
        last_accept < first_refusal;
    REQUIRE(stopped && admission_fds.size() == kAdmissionBurstCount && resumed,
            "full S70-concurrency protocol burst queued before daemon admission");
    REQUIRE(admission_eof,
            "all preframed burst clients are terminally handled after admission");
    REQUIRE(admission_ordered,
            "admission-only batch accepts the full burst before client activity");

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

        MsgChannel *ordinary = connect_tcp_bounded(daemon_port, 5000);
        if (index == 0) first_connected = ordinary != nullptr;
        if (ordinary == nullptr) {
            sequence_valid = false;
            break;
        }
        const P50SourceArmFields arm = source_arm(
            wire_id, epoch, nonce, static_cast<uint32_t>(daemon_port),
            static_cast<uint32_t>(daemon_port));
        const bool arm_sent = ordinary->send_msg(P50SourceArmMsg(arm));
        if (index == 0) first_arm_sent = arm_sent;
        if (!arm_sent) {
            delete ordinary;
            sequence_valid = false;
            break;
        }
        Msg *armed_message = wait_for_type(ordinary, Msg::P50_SOURCE_ARMED, 5000);
        auto *armed = dynamic_cast<P50SourceArmedMsg *>(armed_message);
        const bool armed_valid = armed != nullptr && armed->arm == arm &&
            armed->f_store_generation != 0;
        if (index == 0) first_armed = armed_valid;
        delete armed_message;
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
    REQUIRE(first_armed,
            "source-arm owner is acknowledged before CACHE_SESSION");
    REQUIRE(first_cache_session_sent,
            "real CACHE_SESSION entered the production daemon path");
    REQUIRE(first_adopted_live,
            "authenticated one-shot handoff keeps the adopted session live");
    REQUIRE(sequence_valid &&
                authoritative_sessions == kAuthoritativeSessionCount,
            "more than 64 sequential authoritative CacheSessions remain accepted");

    const int settled_daemon_fds = wait_for_fd_count_at_most(
        daemon_pid, baseline_daemon_fds + 2, 5000);
    REQUIRE(settled_daemon_fds >= 0 &&
                settled_daemon_fds <= baseline_daemon_fds + 2,
            "authoritative CacheSessions leave no retained P5FS descriptors");
    Msg *spurious_login = wait_for_type(scheduler, Msg::LOGIN, 250);
    REQUIRE(spurious_login == nullptr,
            "accepted handoff keeps the READY advertisement stable");
    delete spurious_login;

    ::kill(daemon_pid, SIGTERM);
    Msg *shutdown_message = wait_for_type(scheduler, Msg::LOGIN, 5000);
    LoginMsg *shutdown_login = dynamic_cast<LoginMsg *>(shutdown_message);
    REQUIRE(absent(shutdown_login), "orderly shutdown withdraws before scheduler teardown");
    delete shutdown_message;

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
