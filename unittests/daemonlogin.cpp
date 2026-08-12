/*
   Deterministic G4 Login-attempt lifecycle gate.

   Start iceccd without a scheduler and establish one local fallback job.  A
   fake scheduler then accepts Login but withholds ConfCS while a second client
   submits GetCS.  The pending channel must receive no job traffic.  Closing
   that attempt must preserve the original local job and must not increment the
   established-session cleanup count.  A later Login + ConfCS activates one
   generation; duplicate ConfCS does not create another, and loss of that
   active session performs one cleanup.

   Usage: daemonlogin <iceccd>
*/

#include "comm.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
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
#include <string>

using Clock = std::chrono::steady_clock;

static int failures = 0;

#define REQUIRE(cond, what)                                             \
    do {                                                                \
        if (cond) {                                                     \
            fprintf(stderr, "ok       - %s\n", what);                   \
        } else {                                                        \
            fprintf(stderr, "FAILED   - %s\n", what);                   \
            ++failures;                                                 \
        }                                                               \
    } while (0)

static int listen_on_port(int requested_port, int *actual_port)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(requested_port));
    if (bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0
            || listen(fd, 8) != 0) {
        close(fd);
        return -1;
    }
    socklen_t len = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<struct sockaddr *>(&addr), &len) != 0) {
        close(fd);
        return -1;
    }
    *actual_port = ntohs(addr.sin_port);
    return fd;
}

static int reserve_port()
{
    int port = 0;
    const int fd = listen_on_port(0, &port);
    if (fd >= 0) {
        close(fd);
    }
    return port;
}

static MsgChannel *accept_channel(int listener, int timeout_msec)
{
    struct pollfd pfd = { listener, POLLIN, 0 };
    if (poll(&pfd, 1, timeout_msec) <= 0) {
        return nullptr;
    }
    const int fd = accept(listener, nullptr, nullptr);
    if (fd < 0) {
        return nullptr;
    }
    struct sockaddr_in peer;
    memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    return Service::createChannel(fd, reinterpret_cast<struct sockaddr *>(&peer),
                                  sizeof(peer));
}

static MsgChannel *connect_unix_bounded(const std::string &path, int timeout_msec)
{
    const Clock::time_point deadline = Clock::now()
        + std::chrono::milliseconds(timeout_msec);
    while (Clock::now() < deadline) {
        if (access(path.c_str(), F_OK) == 0) {
            MsgChannel *channel = Service::createChannel(path);
            if (channel) {
                return channel;
            }
        }
        usleep(20 * 1000);
    }
    return nullptr;
}

static Msg *wait_for_type(MsgChannel *channel, Msg::Value wanted,
                          int timeout_msec)
{
    if (!channel) {
        return nullptr;
    }
    const Clock::time_point deadline = Clock::now()
        + std::chrono::milliseconds(timeout_msec);
    while (Clock::now() < deadline) {
        Msg *msg = channel->get_msg(1, true);
        if (msg) {
            if (*msg == wanted) {
                return msg;
            }
            delete msg;
        }
        if (channel->at_eof()) {
            return nullptr;
        }
    }
    return nullptr;
}

static MsgChannel *accept_login_channel(int listener, int timeout_msec,
                                        Msg **login_out)
{
    *login_out = nullptr;
    const Clock::time_point deadline = Clock::now()
        + std::chrono::milliseconds(timeout_msec);
    while (Clock::now() < deadline) {
        const int remaining = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - Clock::now()).count());
        MsgChannel *channel = accept_channel(listener, remaining);
        if (!channel) {
            continue;
        }
        Msg *login = wait_for_type(channel, Msg::LOGIN,
                                   remaining < 5000 ? remaining : 5000);
        if (login) {
            *login_out = login;
            return channel;
        }
        delete channel;
    }
    return nullptr;
}

static std::string request_internals(MsgChannel *client, int timeout_msec)
{
    if (!client || !client->send_msg(GetInternalStatus())) {
        return std::string();
    }
    Msg *msg = wait_for_type(client, Msg::STATUS_TEXT, timeout_msec);
    std::string text;
    if (msg) {
        StatusTextMsg *status = dynamic_cast<StatusTextMsg *>(msg);
        if (status) {
            text = status->text;
        }
    }
    delete msg;
    return text;
}

static bool wait_eof(MsgChannel *channel, int timeout_msec)
{
    const Clock::time_point deadline = Clock::now()
        + std::chrono::milliseconds(timeout_msec);
    while (channel && Clock::now() < deadline) {
        Msg *msg = channel->get_msg(1, true);
        delete msg;
        if (channel->at_eof()) {
            return true;
        }
    }
    return channel && channel->at_eof();
}

static bool wait_child(pid_t pid, int timeout_msec, int *status)
{
    const Clock::time_point deadline = Clock::now()
        + std::chrono::milliseconds(timeout_msec);
    while (Clock::now() < deadline) {
        const pid_t got = waitpid(pid, status, WNOHANG);
        if (got == pid) {
            return true;
        }
        if (got < 0) {
            return false;
        }
        usleep(20 * 1000);
    }
    return false;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <iceccd>\n", argv[0]);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);

    const char *tmproot = getenv("TMPDIR");
    if (!tmproot || !*tmproot) {
        tmproot = "/tmp";
    }
    char temp_template[4096];
    snprintf(temp_template, sizeof(temp_template), "%s/icecream-g4-login.XXXXXX", tmproot);
    char *temp = mkdtemp(temp_template);
    if (!temp) {
        perror("mkdtemp");
        return 2;
    }
    const std::string work(temp);
    const std::string envdir = work + "/envs";
    const std::string socket_path = work + "/iceccd.sock";
    const std::string daemon_log = work + "/iceccd.log";
    mkdir(envdir.c_str(), 0700);
    fprintf(stderr, "retained work directory: %s\n", work.c_str());

    const int scheduler_port = reserve_port();
    REQUIRE(scheduler_port > 0, "an unused scheduler port was reserved");
    if (scheduler_port <= 0) {
        return 2;
    }

    const pid_t daemon_pid = fork();
    if (daemon_pid == 0) {
        char scheduler_spec[64];
        snprintf(scheduler_spec, sizeof(scheduler_spec), "127.0.0.1:%d",
                 scheduler_port);
        setenv("ICECC_TESTS", "1", 1);
        setenv("ICECC_TEST_SOCKET", socket_path.c_str(), 1);
        execl(argv[1], argv[1], "--no-remote", "-m", "1", "-p", "10245",
              "-s", scheduler_spec, "-n", "g4-login-gate", "-N", "g4-daemon",
              "-b", envdir.c_str(), "-l", daemon_log.c_str(),
              "-v", "-v", "-v", static_cast<char *>(nullptr));
        perror("execl iceccd");
        _exit(127);
    }
    REQUIRE(daemon_pid > 0, "iceccd process started without a scheduler");
    if (daemon_pid < 0) {
        return 2;
    }

    MsgChannel *client_a = connect_unix_bounded(socket_path, 5000);
    REQUIRE(client_a != nullptr, "local client A connected while disconnected");
    GetCSMsg local_request(Environments(), "local-a.cpp", CompileJob::Lang_CXX,
                           1, "x86_64", 0, std::string(), 0, 0, 0);
    REQUIRE(client_a && client_a->send_msg(local_request),
            "local client A requested one assignment");
    Msg *local_reply = wait_for_type(client_a, Msg::USE_CS, 5000);
    REQUIRE(local_reply != nullptr,
            "local client A entered the existing local fallback path");
    delete local_reply;

    int bound_port = 0;
    const int listener = listen_on_port(scheduler_port, &bound_port);
    REQUIRE(listener >= 0 && bound_port == scheduler_port,
            "fake scheduler became available on the reserved port");
    Msg *login = nullptr;
    MsgChannel *attempt = accept_login_channel(listener, 20000, &login);
    REQUIRE(attempt != nullptr, "iceccd opened a candidate scheduler channel");
    REQUIRE(login != nullptr, "candidate scheduler received Login");
    delete login;

    MsgChannel *client_b = connect_unix_bounded(socket_path, 5000);
    REQUIRE(client_b != nullptr, "client B connected during Login attempt");
    GetCSMsg pending_request(Environments(), "pending-b.cpp", CompileJob::Lang_CXX,
                             1, "x86_64", 0, std::string(), 0, 0, 0);
    REQUIRE(client_b && client_b->send_msg(pending_request),
            "client B queued GetCS before ConfCS");
    Msg *early_getcs = wait_for_type(attempt, Msg::GET_CS, 4000);
    REQUIRE(early_getcs == nullptr,
            "candidate scheduler received no GetCS before ConfCS");
    delete early_getcs;

    delete attempt;
    attempt = nullptr;
    const std::string preactive = request_internals(client_a, 5000);
    REQUIRE(!preactive.empty(),
            "local client A remained alive after the Login attempt closed");
    REQUIRE(preactive.find("cleanup_attempts=0") != std::string::npos,
            "Login-attempt loss performed zero established-session cleanups");

    Msg *relogin = nullptr;
    MsgChannel *active = accept_login_channel(listener, 20000, &relogin);
    REQUIRE(active != nullptr, "iceccd retried the scheduler connection");
    REQUIRE(relogin != nullptr, "replacement scheduler received Login");
    delete relogin;
    if (active) {
        REQUIRE(active->send_msg(ConfCSMsg()),
                "replacement scheduler sent the activating ConfCS");
        REQUIRE(active->send_msg(ConfCSMsg()),
                "replacement scheduler sent a duplicate ConfCS");
    }
    usleep(100 * 1000);
    const std::string active_state = request_internals(client_a, 5000);
    REQUIRE(active_state.find("ownership_failed=0 gen=1") != std::string::npos,
            "first ConfCS committed exactly one generation");

    delete active;
    active = nullptr;
    REQUIRE(wait_eof(client_a, 5000),
            "active-session loss cleaned the original local client");
    REQUIRE(wait_eof(client_b, 5000),
            "active-session loss cleaned the pending client");
    MsgChannel *client_c = connect_unix_bounded(socket_path, 5000);
    REQUIRE(client_c != nullptr,
            "fresh observer connected after active-session cleanup");
    const std::string after_loss = request_internals(client_c, 5000);
    REQUIRE(after_loss.find("cleanup_attempts=1") != std::string::npos,
            "active-session cleanup completed exactly once");

    int status = 0;
    kill(daemon_pid, SIGTERM);
    bool reaped = wait_child(daemon_pid, 10000, &status);
    if (!reaped) {
        kill(daemon_pid, SIGKILL);
        waitpid(daemon_pid, &status, 0);
    }
    REQUIRE(reaped, "iceccd honored bounded requested shutdown");
    REQUIRE(reaped && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "iceccd exited cleanly");

    delete client_a;
    delete client_b;
    delete client_c;
    close(listener);

    fprintf(stderr, "%s (%d failure%s)\n",
            failures ? "RESULT: FAIL" : "RESULT: PASS", failures,
            failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
