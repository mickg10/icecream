/*
   Deterministic G4 scheduler-loss ordering gate.

   A fake scheduler and two fake local clients drive one iceccd poll snapshot:
   client A is WAITFORCS, client B is connected but quiet, and the inherited-fd
   barrier stops iceccd immediately before poll().  The harness queues UseCS(A)
   and GetInternals(B), then releases the barrier.  A's UseCS delivery is cut
   before byte one and the narrowly scoped test hook fails its compensating
   JobDone, closing the scheduler inside scheduler_use_cs().

   Correct behavior ends the event-loop turn and cleans the old session before
   B is handled.  The guard-only G4a head remains alive but incorrectly sends B
   a StatusText response; the unguarded head exits abnormally.  Both outcomes
   make this same executable gate fail.

   Usage: daemonloss <iceccd>
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
#include <fstream>
#include <sstream>
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

static int make_listener(int *port)
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
    addr.sin_port = 0;
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
    *port = ntohs(addr.sin_port);
    return fd;
}

static int accept_bounded(int listener, int timeout_msec)
{
    struct pollfd pfd = { listener, POLLIN, 0 };
    if (poll(&pfd, 1, timeout_msec) <= 0) {
        return -1;
    }
    return accept(listener, nullptr, nullptr);
}

static MsgChannel *accept_channel(int listener, int timeout_msec)
{
    const int fd = accept_bounded(listener, timeout_msec);
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

static bool read_byte_bounded(int fd, int timeout_msec)
{
    struct pollfd pfd = { fd, POLLIN, 0 };
    if (poll(&pfd, 1, timeout_msec) <= 0) {
        return false;
    }
    char byte = 0;
    ssize_t got;
    do {
        got = read(fd, &byte, 1);
    } while (got < 0 && errno == EINTR);
    return got == 1;
}

static bool write_byte(int fd)
{
    const char byte = 'G';
    ssize_t wrote;
    do {
        wrote = write(fd, &byte, 1);
    } while (wrote < 0 && errno == EINTR);
    return wrote == 1;
}

static bool drain_until_eof(MsgChannel *channel, int timeout_msec,
                            bool *saw_status)
{
    const Clock::time_point deadline = Clock::now()
        + std::chrono::milliseconds(timeout_msec);
    while (channel && Clock::now() < deadline) {
        Msg *msg = channel->get_msg(1, true);
        if (msg) {
            if (*msg == Msg::STATUS_TEXT && saw_status) {
                *saw_status = true;
            }
            delete msg;
        }
        if (channel->at_eof()) {
            return true;
        }
    }
    return channel && channel->at_eof();
}

static std::string read_file(const std::string &path)
{
    std::ifstream input(path.c_str());
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

static size_t count_text(const std::string &haystack, const std::string &needle)
{
    size_t count = 0;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
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

    char temp_template[] = "/tmp/icecream-g4-loss.XXXXXX";
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

    int scheduler_port = 0;
    const int listener = make_listener(&scheduler_port);
    REQUIRE(listener >= 0, "fake scheduler listener created");
    if (listener < 0) {
        return 2;
    }

    int ready_pipe[2];
    int release_pipe[2];
    if (pipe(ready_pipe) != 0 || pipe(release_pipe) != 0) {
        perror("pipe");
        return 2;
    }

    const pid_t daemon_pid = fork();
    if (daemon_pid == 0) {
        close(listener);
        close(ready_pipe[0]);
        close(release_pipe[1]);

        char ready_fd[32];
        char release_fd[32];
        char scheduler_spec[64];
        snprintf(ready_fd, sizeof(ready_fd), "%d", ready_pipe[1]);
        snprintf(release_fd, sizeof(release_fd), "%d", release_pipe[0]);
        snprintf(scheduler_spec, sizeof(scheduler_spec), "127.0.0.1:%d",
                 scheduler_port);
        setenv("ICECC_TESTS", "1", 1);
        setenv("ICECC_TEST_SOCKET", socket_path.c_str(), 1);
        setenv("ICECC_TEST_USECS_CUT_AT", "0", 1);
        setenv("ICECC_TEST_USECS_ABORT_SEND_FAIL", "1", 1);
        setenv("ICECC_TEST_PRE_POLL_READY_FD", ready_fd, 1);
        setenv("ICECC_TEST_PRE_POLL_RELEASE_FD", release_fd, 1);

        execl(argv[1], argv[1], "--no-remote", "-m", "1", "-p", "10245",
              "-s", scheduler_spec, "-n", "g4-loss-gate", "-N", "g4-daemon",
              "-b", envdir.c_str(), "-l", daemon_log.c_str(),
              "-v", "-v", "-v", static_cast<char *>(nullptr));
        perror("execl iceccd");
        _exit(127);
    }
    REQUIRE(daemon_pid > 0, "iceccd process started");
    if (daemon_pid < 0) {
        return 2;
    }
    close(ready_pipe[1]);
    close(release_pipe[0]);

    MsgChannel *scheduler = accept_channel(listener, 10000);
    REQUIRE(scheduler != nullptr, "iceccd connected to fake scheduler");
    Msg *login = wait_for_type(scheduler, Msg::LOGIN, 5000);
    REQUIRE(login != nullptr, "fake scheduler received initial Login");
    delete login;
    if (scheduler) {
        REQUIRE(scheduler->send_msg(ConfCSMsg()),
                "fake scheduler activated the initial session with ConfCS");
    }

    MsgChannel *client_a = connect_unix_bounded(socket_path, 5000);
    REQUIRE(client_a != nullptr, "client A connected");
    GetCSMsg request(Environments{std::make_pair(std::string("x86_64"),
                                                 std::string("testenv"))},
                     "client-a.cpp", CompileJob::Lang_CXX, 1, "x86_64", 0,
                     std::string(), 0, 0, 0);
    REQUIRE(client_a && client_a->send_msg(request),
            "client A sent GetCS");

    MsgChannel *client_b = connect_unix_bounded(socket_path, 5000);
    REQUIRE(client_b != nullptr, "client B connected and remained quiet");

    Msg *forwarded = wait_for_type(scheduler, Msg::GET_CS, 5000);
    REQUIRE(forwarded != nullptr, "fake scheduler received A's GetCS");
    unsigned int client_id = 0;
    if (forwarded) {
        GetCSMsg *getcs = dynamic_cast<GetCSMsg *>(forwarded);
        if (getcs) {
            client_id = getcs->client_id;
        }
    }
    REQUIRE(client_id != 0, "forwarded GetCS carries A's client id");
    delete forwarded;

    REQUIRE(read_byte_bounded(ready_pipe[0], 5000),
            "iceccd reached the one-shot pre-poll barrier");
    REQUIRE(client_b && client_b->send_msg(GetInternalStatus()),
            "client B queued its same-poll discriminator");
    UseCSMsg usecs("x86_64", "127.0.0.2", 10245, 7001, true,
                   client_id, 0);
    REQUIRE(scheduler && scheduler->send_msg(usecs),
            "fake scheduler queued UseCS(A)");
    REQUIRE(write_byte(release_pipe[1]), "pre-poll barrier released");

    bool saw_status = false;
    const bool b_eof = drain_until_eof(client_b, 5000, &saw_status);
    REQUIRE(!saw_status,
            "client B received no StatusText after scheduler loss");
    REQUIRE(b_eof, "client B was closed by old-session cleanup");

    int early_status = 0;
    const pid_t early = waitpid(daemon_pid, &early_status, WNOHANG);
    REQUIRE(early == 0, "iceccd remained alive after the loss scenario");

    delete scheduler;
    scheduler = nullptr;
    MsgChannel *replacement = nullptr;
    if (early == 0) {
        replacement = accept_channel(listener, 10000);
    }
    REQUIRE(replacement != nullptr, "replacement scheduler connection arrived");
    Msg *relogin = wait_for_type(replacement, Msg::LOGIN, 5000);
    REQUIRE(relogin != nullptr, "replacement scheduler received a fresh Login");
    delete relogin;
    if (replacement) {
        REQUIRE(replacement->send_msg(ConfCSMsg()),
                "replacement scheduler activated with ConfCS");
    }

    const std::string log_before_shutdown = read_file(daemon_log);
    REQUIRE(count_text(log_before_shutdown, "G4 test hook: failing compensating JobDone send") == 1,
            "the exact nested send-failure hook fired once");
    REQUIRE(count_text(log_before_shutdown, "cleared children") == 1,
            "old-session cleanup completed once before shutdown");

    int final_status = early_status;
    bool reaped = early == daemon_pid;
    if (!reaped) {
        kill(daemon_pid, SIGTERM);
        reaped = wait_child(daemon_pid, 10000, &final_status);
    }
    if (!reaped) {
        kill(daemon_pid, SIGKILL);
        waitpid(daemon_pid, &final_status, 0);
    }
    REQUIRE(reaped, "iceccd honored bounded requested shutdown");
    REQUIRE(reaped && WIFEXITED(final_status) && WEXITSTATUS(final_status) == 0,
            "iceccd exited cleanly");

    delete client_a;
    delete client_b;
    delete replacement;
    close(listener);
    close(ready_pipe[0]);
    close(release_pipe[1]);

    fprintf(stderr, "%s (%d failure%s)\n",
            failures ? "RESULT: FAIL" : "RESULT: PASS", failures,
            failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
