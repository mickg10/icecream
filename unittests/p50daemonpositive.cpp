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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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
        && login->cache_protocol == CACHE_WIRE_PROTOCOL_V1
        && login->cache_profile_mask == CACHE_PROFILE_ZSTD_TU;
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

    const ConfCSMsg activate(UINT64_C(0x5000000000000001), ConfCSMsg::Legacy);
    REQUIRE(scheduler && scheduler->send_msg(activate),
            "first ConfCS activates the scheduler session");
    Msg *positive_message = wait_for_type(scheduler, Msg::LOGIN, 10000);
    LoginMsg *positive = dynamic_cast<LoginMsg *>(positive_message);
    REQUIRE(present(positive, static_cast<uint32_t>(daemon_port)),
            "real READY/authenticated sidecar publishes exact positive advertisement");
    delete positive_message;

    MsgChannel *ordinary = connect_tcp_bounded(daemon_port, 5000);
    REQUIRE(ordinary != nullptr, "ordinary Protocol-50 client reached the real public listener");
    REQUIRE(ordinary && ordinary->send_msg(CacheSessionMsg()),
            "real CACHE_SESSION entered the production daemon path");

    Msg *withdrawn_message = wait_for_type(scheduler, Msg::LOGIN, 5000);
    LoginMsg *withdrawn = dynamic_cast<LoginMsg *>(withdrawn_message);
    REQUIRE(absent(withdrawn), "one-shot handoff publishes withdrawal first");
    delete withdrawn_message;
    // The sidecar deliberately keeps the adopted P50 session active.  End
    // this synthetic empty session before asking it to accept the fresh daemon
    // control relationship; a real transaction reaches the same boundary at
    // its terminal frame.
    delete ordinary;
    ordinary = nullptr;
    Msg *recovered_message = wait_for_type(scheduler, Msg::LOGIN, 5000);
    LoginMsg *recovered = dynamic_cast<LoginMsg *>(recovered_message);
    REQUIRE(present(recovered, static_cast<uint32_t>(daemon_port)),
            "fresh authenticated relationship republishes presence second");
    delete recovered_message;

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
