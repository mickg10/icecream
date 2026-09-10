/*
   Deterministic G4 Login-attempt lifecycle gate.

   Start iceccd without a scheduler and establish one local fallback job.  A
   fake scheduler then accepts Login but withholds ConfCS while a second client
   submits GetCS.  The pending channel must receive no job traffic.  Closing
   that attempt must preserve the original local job and must not increment the
   established-session cleanup count.  A later Login + ConfCS activates one
   generation; duplicate ConfCS does not create another, and loss of that
   active session performs one cleanup.

   S2 Gap 3 (BigOracle d23d9c5d HOLD, reused-client clearing): client A's
   local-fallback request above also exercises handle_get_cs's no-scheduler
   clear of a poisoned (test-only fabricated) retained cache handoff -- see
   the comment at its assertion below, and
   test_poison_cache_handoff_if_armed's own comment in daemon/main.cpp.

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

/* Bounded retry over request_internals(): used where the event proving the
   real clear ran (e.g. handle_old_request's per-tick stranded-request
   sweep) is not synchronized to any single reply client A/B already
   received, so a single status snapshot could race ahead of it. */
static std::string wait_for_internals_containing(MsgChannel *client,
                                                  const std::string &needle,
                                                  int timeout_msec)
{
    const Clock::time_point deadline = Clock::now()
        + std::chrono::milliseconds(timeout_msec);
    std::string last;
    while (Clock::now() < deadline) {
        last = request_internals(client, 2000);
        if (last.find(needle) != std::string::npos) {
            return last;
        }
        usleep(50 * 1000);
    }
    return last;
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

    const char *temporary_root = std::getenv("TMPDIR");
    std::string temp_template =
        std::string(temporary_root && temporary_root[0] ? temporary_root : "/tmp")
        + "/icecream-g4-login.XXXXXX";
    char *temp = mkdtemp(&temp_template[0]);
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
        /* S2 Gap 3 (BigOracle d23d9c5d HOLD): arms the poison/record hook
           (see test_poison_cache_handoff_if_armed's own comment in
           daemon/main.cpp) for TWO sites this one daemon process visits,
           in order, non-overlapping: client A's local-fallback request
           below (get_cs_no_scheduler, before any scheduler connects), then
           much later client B's stranded held GetCS once the candidate
           Login attempt is abandoned (old_request_stranded).  Each result
           is read via request_internals immediately after its site fires,
           before the next one can. */
        setenv("ICECC_TEST_POISON_CACHE_HANDOFF_SITE",
               "get_cs_no_scheduler,old_request_stranded", 1);
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

    /* S2 Gap 3 (BigOracle d23d9c5d HOLD, reused-client clearing):
       handle_get_cs's no-scheduler branch clears c->cacheHandoff so a
       handoff retained from an earlier dispatch on this same (reused)
       Client cannot leak forward.  No real wire flow can hand this site a
       Client that both genuinely retained a prior handoff and is making a
       second live GetCS decision (Client::getcs_outstanding forbids a
       second GetCS on one connection outright), so
       ICECC_TEST_POISON_CACHE_HANDOFF_SITE=get_cs_no_scheduler (armed
       above) fabricated a retained handoff on client A immediately before
       this real, unmodified clear ran.  The clear happens synchronously
       within the SAME handle_get_cs call that built local_reply above --
       received on this same connection -- so one status query already
       reflects it; no retry/poll race like Client D's cross-connection
       case in cachehandoffdaemon.cpp. */
    const std::string no_scheduler_clear = request_internals(client_a, 5000);
    REQUIRE(no_scheduler_clear.find(
                "Cache-handoff clear test: site=get_cs_no_scheduler fired=1 "
                "valid=0 port=0 protocol=0 mask=0") != std::string::npos,
            "S2 Gap 3: handle_get_cs's no-scheduler clear reset a retained "
            "(poisoned) handoff back to canonical absence -- valid, port, "
            "protocol, and mask all zero");

    MsgChannel *client_remote = connect_unix_bounded(socket_path, 5000);
    REQUIRE(client_remote != nullptr,
            "remote-required client connected while schedulerless");
    GetCSMsg remote_request(
        Environments(), "remote-required.cpp", CompileJob::Lang_CXX,
        1, "x86_64", 0, std::string(), 50, 0, 0);
    remote_request.remote_required = 1;
    REQUIRE(client_remote && client_remote->send_msg(remote_request),
            "remote-required client requested one assignment");
    Msg *forbidden_local = wait_for_type(client_remote, Msg::USE_CS, 1000);
    REQUIRE(forbidden_local == nullptr && client_remote && !client_remote->at_eof(),
            "schedulerless remote-required request stayed held without local reply");
    delete forbidden_local;

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

    /* S2 Gap 3 (BigOracle d23d9c5d HOLD, reused-client clearing):
       handle_old_request's schedulerless-fallback branch resolves client
       B's now-stranded deferred_getcs (the candidate attempt above was
       abandoned before ConfCS) via the same canonical-cache-absent local
       fallback as client A's, clearing c->cacheHandoff first.  As with the
       other two Gap 3 sites, no real wire flow can hand this site a Client
       that both genuinely retained a prior handoff and is mid a second
       live GetCS decision, so old_request_stranded (armed above alongside
       get_cs_no_scheduler) fabricates that precondition immediately before
       the real, unmodified clear runs.  This is the ONLY one of the three
       Gap 3 sites reached by a stranded held request rather than a fresh
       one, and the only one not synchronized to a reply on the SAME
       connection queried -- handle_old_request's stranded sweep runs on
       its own per-tick schedule relative to client A's status-query
       connection, and the daemon has not yet been given anywhere to
       reconnect (the listener above is not accepting again until after
       this poll), so it is polled rather than checked once.  The bound is
       deliberately SHORT (not the seconds-scale margin used elsewhere in
       this file): the real sweep observably fires within a tick, well
       under 100ms, and iceccd's own scheduler-reconnect retry runs in a
       tight sub-second loop (see "Delaying reconnect." in its log) that
       starts refilling the listener's backlog immediately -- a multi-
       second poll bound here would let that backlog churn accumulate
       before accept_login_channel below ever runs, corrupting the
       replacement-scheduler handshake that follows regardless of this
       assertion's own outcome.  Confirmed by direct measurement: a 3000ms
       (let alone 10000ms) bound reliably produced a SECOND, unrelated
       failure downstream ("first ConfCS committed exactly one
       generation") whenever this assertion's own needle was absent and
       the poll ran to its full bound -- a test-harness timing artifact of
       the poll's OWN duration, not a second production defect; a mutant
       deleting the real clear reproduced that collateral at 3000/10000ms
       and stopped reproducing it at this bound. */
    const std::string stranded_clear = wait_for_internals_containing(
        client_a,
        "Cache-handoff clear test: site=old_request_stranded fired=1 "
        "valid=0 port=0 protocol=0 mask=0",
        400);
    REQUIRE(stranded_clear.find(
                "Cache-handoff clear test: site=old_request_stranded fired=1 "
                "valid=0 port=0 protocol=0 mask=0") != std::string::npos,
            "S2 Gap 3: handle_old_request's stranded schedulerless-fallback "
            "clear reset client B's retained (poisoned) handoff back to "
            "canonical absence -- valid, port, protocol, and mask all zero");

    Msg *relogin = nullptr;
    MsgChannel *active = accept_login_channel(listener, 20000, &relogin);
    REQUIRE(active != nullptr, "iceccd retried the scheduler connection");
    REQUIRE(relogin != nullptr, "replacement scheduler received Login");
    delete relogin;
    if (active) {
        const ConfCSMsg legacy_config(UINT64_C(0x4700000000000001),
                                      ConfCSMsg::Legacy);
        REQUIRE(active->send_msg(legacy_config),
                "replacement scheduler sent the activating ConfCS");
        REQUIRE(active->send_msg(legacy_config),
                "replacement scheduler sent a duplicate ConfCS");
    }
    Msg *redriven_wire = wait_for_type(active, Msg::GET_CS, 5000);
    GetCSMsg *redriven = dynamic_cast<GetCSMsg *>(redriven_wire);
    REQUIRE(redriven && redriven->remote_required == 1,
            "replacement scheduler received the held remote-required request");
    delete redriven_wire;
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
    REQUIRE(wait_eof(client_remote, 5000),
            "active-session loss cleaned the remote-required client");
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
    delete client_remote;
    delete client_c;
    close(listener);

    fprintf(stderr, "%s (%d failure%s)\n",
            failures ? "RESULT: FAIL" : "RESULT: PASS", failures,
            failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
