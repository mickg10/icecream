/* Real iceccd ordinary-link fail-closed integration.
 * A Protocol-50 CACHE_SESSION sent to the public daemon listener must be
 * classified by the production event loop and closed while no sidecar
 * relationship is available.  Login advertisement is not involved. */
#include "comm.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <unistd.h>

using Clock = std::chrono::steady_clock;

static int failures = 0;
#define REQUIRE(condition, text) do { \
    if (condition) std::fprintf(stderr, "ok - %s\n", text); \
    else { std::fprintf(stderr, "FAILED - %s\n", text); ++failures; } \
} while (0)

static MsgChannel *connect_unix_bounded(const std::string &path, int timeout_msec)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (Clock::now() < deadline) {
        if (::access(path.c_str(), F_OK) == 0) {
            MsgChannel *channel = Service::createChannel(path);
            if (channel) return channel;
        }
        ::usleep(20000);
    }
    return nullptr;
}

static bool wait_eof(MsgChannel *channel, int timeout_msec)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (channel && Clock::now() < deadline) {
        Msg *msg = channel->get_msg(1, true);
        delete msg;
        if (channel->at_eof()) return true;
    }
    return channel && channel->at_eof();
}

static int reserve_port()
{
    int port = 0;
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return 0;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
        ::listen(listener, 1) != 0) {
        ::close(listener);
        return 0;
    }
    socklen_t length = sizeof(address);
    const bool ok = ::getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length) == 0;
    port = ok ? ntohs(address.sin_port) : 0;
    ::close(listener);
    return port;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <iceccd>\n", argv[0]);
        return 2;
    }
    char work_template[] = "/tmp/icecc-s2-daemon-cache.XXXXXX";
    char *work_raw = ::mkdtemp(work_template);
    REQUIRE(work_raw != nullptr, "real daemon temporary directory created");
    if (!work_raw) return 2;
    const std::string work(work_raw);
    const std::string socket_path = work + "/iceccd.sock";
    const std::string envdir = work + "/envs";
    const std::string log_path = work + "/iceccd.log";
    ::mkdir(envdir.c_str(), 0700);
    const int scheduler_port = reserve_port();
    REQUIRE(scheduler_port > 0, "real daemon scheduler retry port reserved");

    const pid_t child = ::fork();
    if (child == 0) {
        char scheduler[64];
        std::snprintf(scheduler, sizeof(scheduler), "127.0.0.1:%d", scheduler_port);
        ::setenv("ICECC_TESTS", "1", 1);
        ::setenv("ICECC_TEST_SOCKET", socket_path.c_str(), 1);
        ::execl(argv[1], argv[1], "--no-remote", "-m", "0", "-p", "10245",
                "-s", scheduler, "-n", "s2-cache-dispatch", "-N", "s2-cache-daemon",
                "-b", envdir.c_str(), "-l", log_path.c_str(), static_cast<char *>(nullptr));
        ::_exit(127);
    }
    REQUIRE(child > 0, "real iceccd process started");
    if (child <= 0) return 2;

    MsgChannel *client = connect_unix_bounded(socket_path, 10000);
    REQUIRE(client != nullptr, "client reached the real public iceccd listener");
    if (client) {
        REQUIRE(client->protocol == 50 || client->protocol == 0,
                "ordinary link negotiated without changing legacy setup");
        // The production dispatcher is unavailable until a reviewed READY
        // adapter supplies an authenticated sidecar, so this must terminate
        // through the normal client teardown and never advertise cache.
        REQUIRE(client->send_msg(CacheSessionMsg()),
                "real client sent Protocol-50 CACHE_SESSION");
        REQUIRE(wait_eof(client, 5000),
                "real iceccd classified CACHE_SESSION and failed closed boundedly");
        delete client;
    }

    ::kill(child, SIGTERM);
    int status = 0;
    bool reaped = false;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < deadline) {
        const pid_t got = ::waitpid(child, &status, WNOHANG);
        if (got == child) { reaped = true; break; }
        if (got < 0) break;
        ::usleep(20000);
    }
    if (!reaped) {
        ::kill(child, SIGKILL);
        (void)::waitpid(child, &status, 0);
    }
    REQUIRE(reaped, "real iceccd exited under bounded shutdown");
    return failures ? 1 : 0;
}
