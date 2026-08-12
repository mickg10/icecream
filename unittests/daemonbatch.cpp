/*
   G4 count>1 (torepeat / GetCSMsg::count) batch decision gate.

   Start iceccd against a fake scheduler, activate the session with ConfCS, then
   a client submits GetCS(count=3).  The scheduler answers with three distinct
   remote UseCS for that client id.  The daemon must relay exactly three UseCS
   to the client -- one per decision, in order.

   RED before the batch ledger: only the first reply is delivered; the 2nd/3rd
   arrive while the client is no longer WAITFORCS and are terminalized as
   "unmatched", so a torepeat=3 client blocks waiting for its remaining copies.

   Usage: daemonbatch <iceccd>
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
#include <vector>

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

static Msg *wait_for_type(MsgChannel *channel, Msg::Value wanted, int timeout_msec)
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

static MsgChannel *accept_login_channel(int listener, int timeout_msec, Msg **login_out)
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
        Msg *login = wait_for_type(channel, Msg::LOGIN, remaining < 5000 ? remaining : 5000);
        if (login) {
            *login_out = login;
            return channel;
        }
        delete channel;
    }
    return nullptr;
}

struct SeenUseCS { uint32_t job_id; bool got_env; std::string host; };

/* Capture complete USE_CS frames delivered to the client (up to `want`),
   recording the exact fields so the test can assert field fidelity and order,
   not merely the message type. */
static std::vector<SeenUseCS> capture_use_cs(MsgChannel *client, int want, int timeout_msec)
{
    std::vector<SeenUseCS> seen;
    const Clock::time_point deadline = Clock::now()
        + std::chrono::milliseconds(timeout_msec);
    while (client && static_cast<int>(seen.size()) < want && Clock::now() < deadline) {
        Msg *msg = client->get_msg(1, true);
        if (msg) {
            if (*msg == Msg::USE_CS) {
                UseCSMsg *u = dynamic_cast<UseCSMsg *>(msg);
                if (u) {
                    SeenUseCS s;
                    s.job_id = u->job_id;
                    s.got_env = u->got_env != 0;
                    s.host = u->hostname;
                    seen.push_back(s);
                }
            }
            delete msg;
        }
        if (client->at_eof()) {
            break;
        }
    }
    return seen;
}

/* Count JOB_DONE frames the scheduler receives whose job id is in `wanted`. */
static int count_job_done(MsgChannel *sched, const std::vector<uint32_t> &wanted,
                          int timeout_msec)
{
    int seen = 0;
    const Clock::time_point deadline = Clock::now()
        + std::chrono::milliseconds(timeout_msec);
    while (sched && seen < static_cast<int>(wanted.size()) && Clock::now() < deadline) {
        Msg *msg = sched->get_msg(1, true);
        if (msg) {
            if (*msg == Msg::JOB_DONE) {
                JobDoneMsg *d = dynamic_cast<JobDoneMsg *>(msg);
                if (d) {
                    for (uint32_t w : wanted) {
                        if (w == d->job_id) { ++seen; break; }
                    }
                }
            }
            delete msg;
        }
        if (sched->at_eof()) {
            break;
        }
    }
    return seen;
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
    snprintf(temp_template, sizeof(temp_template), "%s/icecream-g4-batch.XXXXXX", tmproot);
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
    int bound_port = 0;
    const int listener = listen_on_port(scheduler_port, &bound_port);
    REQUIRE(listener >= 0 && bound_port == scheduler_port,
            "fake scheduler bound the reserved port");
    if (listener < 0) {
        return 2;
    }

    const pid_t daemon_pid = fork();
    if (daemon_pid == 0) {
        char scheduler_spec[64];
        snprintf(scheduler_spec, sizeof(scheduler_spec), "127.0.0.1:%d", scheduler_port);
        setenv("ICECC_TESTS", "1", 1);
        setenv("ICECC_TEST_SOCKET", socket_path.c_str(), 1);
        execl(argv[1], argv[1], "--no-remote", "-m", "1", "-p", "10245",
              "-s", scheduler_spec, "-n", "g4-batch-gate", "-N", "g4-daemon",
              "-b", envdir.c_str(), "-l", daemon_log.c_str(),
              "-v", "-v", "-v", static_cast<char *>(nullptr));
        perror("execl iceccd");
        _exit(127);
    }
    REQUIRE(daemon_pid > 0, "iceccd process started");
    if (daemon_pid < 0) {
        return 2;
    }

    /* Activate the session: accept the daemon's Login and send ConfCS. */
    Msg *login = nullptr;
    MsgChannel *sched = accept_login_channel(listener, 20000, &login);
    REQUIRE(sched != nullptr, "scheduler accepted the daemon Login");
    REQUIRE(login != nullptr, "scheduler received Login");
    delete login;
    REQUIRE(sched && sched->send_msg(ConfCSMsg()),
            "scheduler sent the activating ConfCS");
    usleep(150 * 1000);

    /* A client submits one GetCS asking for three copies (torepeat=3). */
    MsgChannel *client = connect_unix_bounded(socket_path, 5000);
    REQUIRE(client != nullptr, "batch client connected");
    GetCSMsg batch(Environments(), "batch.cpp", CompileJob::Lang_CXX,
                   3, "x86_64", 0, std::string(), 0, 0, 0);
    REQUIRE(client && client->send_msg(batch), "batch client sent GetCS(count=3)");

    /* The daemon forwards the GetCS to S; read it and answer with three
       distinct remote UseCS for that client id. */
    Msg *fwd = wait_for_type(sched, Msg::GET_CS, 5000);
    REQUIRE(fwd != nullptr, "scheduler received the forwarded GetCS");
    GetCSMsg *fwd_getcs = dynamic_cast<GetCSMsg *>(fwd);
    const uint32_t client_id = fwd_getcs ? fwd_getcs->client_id : 0;
    const uint32_t fwd_count = fwd_getcs ? fwd_getcs->count : 0;
    REQUIRE(fwd_count == 3, "forwarded GetCS preserved count=3");
    delete fwd;

    /* Three distinct decisions with VARYING got_env, so a hardcoded
       got_env=true relay (bigoracle 00:35 #1) is caught. */
    struct { uint32_t jid; bool env; } dec[3] = { {5000u, true}, {5001u, false}, {5002u, true} };
    bool sent_all = client_id != 0;
    for (int i = 0; i < 3 && sent_all; ++i) {
        UseCSMsg use("x86_64", "10.11.12.13", 3632u, dec[i].jid, dec[i].env, client_id, 0);
        sent_all = sched->send_msg(use);
    }
    REQUIRE(sent_all, "scheduler sent three distinct remote UseCS");

    std::vector<SeenUseCS> got = capture_use_cs(client, 3, 6000);
    fprintf(stderr, "         (client received %zu of 3 UseCS)\n", got.size());
    REQUIRE(got.size() == 3, "client received exactly three UseCS for the count=3 request");
    const bool order_ok = got.size() == 3
        && got[0].job_id == 5000u && got[1].job_id == 5001u && got[2].job_id == 5002u;
    REQUIRE(order_ok, "batch UseCS delivered in exact FIFO job-id order");
    const bool env_ok = got.size() == 3
        && got[0].got_env && !got[1].got_env && got[2].got_env;
    REQUIRE(env_ok, "batch relay preserved got_env exactly (not hardcoded true)");
    const bool host_ok = got.size() == 3 && got[0].host == "10.11.12.13";
    REQUIRE(host_ok, "batch relay preserved the remote host exactly");

    /* Duplicate of decision 2 and a fourth distinct decision: neither may reach
       the client (dedup + excess), and the excess is terminalized to S. */
    sched->send_msg(UseCSMsg("x86_64", "10.11.12.13", 3632u, 5001u, true, client_id, 0)); // duplicate
    sched->send_msg(UseCSMsg("x86_64", "10.11.12.13", 3632u, 5003u, true, client_id, 0)); // excess
    std::vector<SeenUseCS> extra = capture_use_cs(client, 1, 2000);
    REQUIRE(extra.empty(), "duplicate and excess UseCS are not relayed (no fourth reply)");
    const int excess_term = count_job_done(sched, std::vector<uint32_t>{5003u}, 3000);
    REQUIRE(excess_term == 1, "excess job terminalized to the scheduler by exact job id");

    /* Client completes all three: exactly three JobDones must reach S. */
    for (uint32_t jid : { 5000u, 5001u, 5002u }) {
        JobDoneMsg d(jid, 0, JobDoneMsg::FROM_SUBMITTER);
        d.real_msec = 1;
        d.user_msec = 1;
        client->send_msg(d);
    }
    const int fwd_done = count_job_done(sched, std::vector<uint32_t>{5000u, 5001u, 5002u}, 5000);
    REQUIRE(fwd_done == 3, "all three batch JobDones were forwarded to the scheduler");

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

    delete client;
    delete sched;
    close(listener);

    fprintf(stderr, "%s (%d failure%s)\n",
            failures ? "RESULT: FAIL" : "RESULT: PASS", failures,
            failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
