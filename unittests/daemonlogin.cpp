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

#include <algorithm>
#include <cerrno>
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

/* G4 (bigoracle 21:51): a strict "no complete application frame" assertion for
   the pre-activation window.  Unlike wait_for_type() -- which deletes non-matching
   frames and would silently pass a leaked JobBegin/JobDone/Stats/re-Login -- this
   fails on the FIRST complete frame of ANY type.  Partial frames are tolerated
   until the absolute deadline. */
static bool expect_no_complete_frame(MsgChannel *channel, int timeout_msec,
                                     std::string *seen)
{
    const Clock::time_point deadline = Clock::now()
        + std::chrono::milliseconds(timeout_msec);
    while (channel && Clock::now() < deadline) {
        const int remaining = std::max<int>(1,
            static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - Clock::now()).count()));
        struct pollfd pfd = { channel->fd, POLLIN, 0 };
        const int rc = poll(&pfd, 1, remaining);
        if (rc == 0) {
            return true;
        }
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        Msg *msg = channel->get_msg(0, true);
        if (msg) {
            if (seen) {
                *seen = msg->to_string();
            }
            delete msg;
            return false;
        }
        if (channel->at_eof()) {
            return false;
        }
        /* Partial frame only: keep reading until it completes or the deadline. */
    }
    return true;
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

    char temp_template[] = "/tmp/icecream-g4-login.XXXXXX";
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
    UseCSMsg *local_use = dynamic_cast<UseCSMsg *>(local_reply);
    REQUIRE(local_use != nullptr, "local reply decoded as UseCS");
    const uint32_t local_job_id = local_use ? local_use->job_id : 0;
    delete local_reply;

    /* G4 (local-oracle 21:57 #5): zero-count request in the DISCONNECTED state.
       count=0 must yield zero UseCS replies and leave the connection usable for a
       later ordinary count=1.  (At this head the schedulerless fallback still
       synthesizes one UseCS for count=0 -> RED until the zero-count product
       correction.) */
    {
        MsgChannel *client_z = connect_unix_bounded(socket_path, 5000);
        REQUIRE(client_z != nullptr, "zero-count client connected (disconnected)");
        GetCSMsg zero_request(Environments(), "zero.cpp", CompileJob::Lang_CXX,
                              0, "x86_64", 0, std::string(), 0, 0, 0);
        REQUIRE(client_z && client_z->send_msg(zero_request),
                "zero-count client sent GetCS(count=0)");
        std::string zseen;
        REQUIRE(expect_no_complete_frame(client_z, 2000, &zseen),
                "zero-count produced no UseCS reply (disconnected)");
        GetCSMsg one_after_zero(Environments(), "zero-then-one.cpp",
                                CompileJob::Lang_CXX, 1, "x86_64", 0,
                                std::string(), 0, 0, 0);
        REQUIRE(client_z && client_z->send_msg(one_after_zero),
                "zero-count connection still usable: sent count=1");
        /* count=0 must NOT have occupied the client: had it set getcs_outstanding,
           the count=1 would hit the single-outstanding guard and handle_end would
           close this client (EOF).  Assert the connection is ACCEPTED (not
           closed); actual UseCS delivery is capacity-gated by the -m1 slot that
           client A already holds, so it is not required here. */
        REQUIRE(!wait_eof(client_z, 1500),
                "zero-count connection accepted the later count=1 (not closed)");
        delete client_z;
    }

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

    /* G4 (bigoracle 21:51 / local-oracle 21:57): while ConfCS is withheld, drive
       the real schedulerless GetCS-derived lifecycle on client A -- a production
       CompileFile then JobDone using the captured synthetic job id.  This
       traverses handle_compile_file() and handle_job_done() (the D1 owner gates)
       without an external compiler.  Then the pending channel must observe ZERO
       complete application frames of ANY type before ConfCS (the old
       wait_for_type(GET_CS) negative silently deleted a leaked non-GET_CS frame
       and falsely passed). */
    CompileJob local_job;
    local_job.setLanguage(CompileJob::Lang_CXX);
    local_job.setCompilerName("g++");
    local_job.setJobID(local_job_id);
    local_job.setEnvironmentVersion("__client");
    local_job.setTargetPlatform("x86_64");
    local_job.setInputFile(work + "/local-a.cpp");
    local_job.setOutputFile(work + "/local-a.o");
    local_job.setWorkingDirectory(work);
    CompileFileMsg compile(&local_job);
    REQUIRE(client_a->send_msg(compile),
            "schedulerless fallback sent production CompileFile");
    JobDoneMsg fallback_done(local_job_id, 0, JobDoneMsg::FROM_SUBMITTER);
    fallback_done.real_msec = 1;
    fallback_done.user_msec = 1;
    REQUIRE(client_a->send_msg(fallback_done),
            "schedulerless fallback sent production JobDone");

    std::string leaked;
    REQUIRE(expect_no_complete_frame(attempt, 4000, &leaked),
            "no pre-active application frame on the Login attempt channel");
    if (!leaked.empty()) {
        fprintf(stderr, "         (leaked pre-active frame: %s)\n", leaked.c_str());
    }

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
