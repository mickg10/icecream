/*
   Deterministic G4 Login-attempt lifecycle + zero-count gate.

   Start iceccd without a scheduler and drive the real GetCS-derived lifecycle.
   A fake scheduler then accepts Login but withholds ConfCS while clients submit
   GetCS.  The pending scheduler channel must receive no application frame.  A
   later Login + ConfCS activates exactly one generation; duplicate ConfCS does
   not create another; loss of the active session performs one cleanup.

   Corrected child of 77af604 per bigoracle comment 5247580131 (five accepted
   fixture corrections):
     1. has_msg()-before-poll negative helper (a userspace-buffered frame is
        reported without waiting for a new kernel edge);
     2. a same-burst self-control that proves (1) -- restoring poll-first makes
        it RED;
     3. non-cascading setup guards (hard-stop the row on an invalid connection,
        UseCS decode, or job id -- never continue with job id zero);
     4. the full four-state zero-count table (DISCONNECTED / LOGIN_ATTEMPT /
        ACTIVE / NONZERO-OUTSTANDING);
     5. behavior assertions, not liveness (exactly-one, not "still open").

   Run the identical source against exact 85ee218 (RED) and 7e4eedf (GREEN).

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
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

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

/* A row whose precondition is destroyed (bad connection / decode / job id)
   must stop, not cascade into meaningless later assertions with junk state. */
#define REQUIRE_OR_ABORT(cond, what)                                    \
    do {                                                                \
        if (cond) {                                                     \
            fprintf(stderr, "ok       - %s\n", what);                   \
        } else {                                                        \
            fprintf(stderr, "FAILED   - %s\n", what);                   \
            ++failures;                                                 \
            fprintf(stderr, "ABORT    - precondition failed; "          \
                    "not continuing this run with invalid state\n");    \
            goto done;                                                  \
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

/* Correction 1 (bigoracle 5247580131).  Strict "no complete application frame"
   for a window in which NOTHING is allowed from the peer.  The buffered-message
   check MUST precede the poll: a frame already sitting in userspace inbuf (e.g.
   delivered in the same burst as a frame we just consumed) generates no fresh
   kernel edge, so a poll-first helper would block to its deadline and falsely
   pass.  Partial frames are tolerated until the absolute deadline.

   Exact loop:
     if has_msg(): decode -> fail with type (or fall to eof)
     if at_eof(): fail
     poll(POLLIN|POLLHUP|POLLERR): timeout -> succeed; EINTR -> retry;
        other error / POLLNVAL -> fail
     on POLLIN|POLLHUP: read_a_bit(); recheck has_msg()/at_eof() */
static bool expect_no_complete_frame(MsgChannel *channel, int timeout_msec,
                                     std::string *seen)
{
    const Clock::time_point deadline = Clock::now()
        + std::chrono::milliseconds(timeout_msec);
    while (channel && Clock::now() < deadline) {
        /* buffered complete frame first, before waiting for a new edge */
        if (channel->has_msg()) {
            Msg *msg = channel->get_msg(0, true);
            if (msg) {
                if (seen) {
                    *seen = msg->to_string();
                }
                delete msg;
                return false;
            }
            /* has_msg() is true on eof with no pending frame */
            if (channel->at_eof()) {
                if (seen) {
                    *seen = "<eof>";
                }
                return false;
            }
        }
        if (channel->at_eof()) {
            if (seen) {
                *seen = "<eof>";
            }
            return false;
        }
        const int remaining = std::max<int>(1,
            static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - Clock::now()).count()));
        struct pollfd pfd = { channel->fd, POLLIN | POLLHUP | POLLERR, 0 };
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
        if (pfd.revents & (POLLERR | POLLNVAL)) {
            if (seen) {
                *seen = "<poll error>";
            }
            return false;
        }
        if (pfd.revents & (POLLIN | POLLHUP)) {
            if (!channel->read_a_bit()) {
                if (seen) {
                    *seen = "<read error>";
                }
                return false;
            }
            if (channel->has_msg()) {
                Msg *msg = channel->get_msg(0, true);
                if (msg) {
                    if (seen) {
                        *seen = msg->to_string();
                    }
                    delete msg;
                    return false;
                }
                if (channel->at_eof()) {
                    if (seen) {
                        *seen = "<eof>";
                    }
                    return false;
                }
            }
            if (channel->at_eof()) {
                if (seen) {
                    *seen = "<eof>";
                }
                return false;
            }
        }
    }
    return true;
}

/* Count complete frames of exactly `wanted` over a window, discarding other
   types (an ACTIVE scheduler channel legitimately carries Stats/ping; only the
   GetCS emission count is under test).  Distinct from expect_no_complete_frame,
   which is for windows where NO frame at all is permitted. */
static int count_frames_of_type(MsgChannel *channel, Msg::Value wanted,
                                int window_msec)
{
    int seen = 0;
    const Clock::time_point deadline = Clock::now()
        + std::chrono::milliseconds(window_msec);
    while (channel && Clock::now() < deadline) {
        Msg *msg = channel->get_msg(1, true);
        if (msg) {
            if (*msg == wanted) {
                ++seen;
            }
            delete msg;
        }
        if (channel->at_eof()) {
            break;
        }
    }
    return seen;
}

/* Connected MsgChannel pair over a socketpair; createChannel() completes the
   protocol handshake synchronously so both ends must be brought up together
   (pattern from backpressure.cpp). */
struct ChannelPair { MsgChannel *a = nullptr; MsgChannel *b = nullptr; };
static ChannelPair make_channel_pair()
{
    int fds[2];
    ChannelPair p;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        return p;
    }
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    std::thread ta([&] {
        p.a = Service::createChannel(fds[0], (struct sockaddr *)&sa, sizeof(sa));
    });
    std::thread tb([&] {
        p.b = Service::createChannel(fds[1], (struct sockaddr *)&sa, sizeof(sa));
    });
    ta.join();
    tb.join();
    return p;
}

/* Correction 2 (bigoracle 5247580131).  Same-burst self-control for the helper:
   the peer writes the expected Login and one forbidden application frame in a
   single burst; the reader consumes Login; the second frame is then already
   buffered in userspace with no further kernel edge coming.  The has_msg()-first
   helper must report it (return false).  A poll-first helper would block to its
   deadline and (wrongly) return true -> this control RED. */
static bool run_same_burst_control(std::string *detail)
{
    ChannelPair p = make_channel_pair();
    if (!p.a || !p.b) {
        if (detail) *detail = "channel pair not established";
        return false;
    }
    bool ok = false;
    if (p.a->send_msg(LoginMsg()) && p.a->send_msg(EndMsg())) {
        p.a->flush_pending();
        /* let both frames land in the reader's socket buffer as one burst */
        usleep(50 * 1000);
        Msg *login = wait_for_type(p.b, Msg::LOGIN, 2000);
        if (login) {
            delete login;
            std::string seen;
            /* helper must DETECT the already-buffered forbidden frame */
            const bool no_frame = expect_no_complete_frame(p.b, 1500, &seen);
            ok = !no_frame;
            if (detail) {
                *detail = no_frame ? "helper missed buffered frame (poll-first?)"
                                   : ("detected buffered " + seen);
            }
        } else if (detail) {
            *detail = "reader did not receive Login";
        }
    } else if (detail) {
        *detail = "peer could not send the burst";
    }
    delete p.a;
    delete p.b;
    return ok;
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

static GetCSMsg make_getcs(const char *file, unsigned int count)
{
    return GetCSMsg(Environments(), file, CompileJob::Lang_CXX, count,
                    "x86_64", 0, std::string(), 0, 0, 0);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <iceccd>\n", argv[0]);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);

    /* All locals the abort path (goto done) could cross are declared and
       initialized here, up front, so no jump crosses an initialization. */
    pid_t daemon_pid = -1;
    int listener = -1;
    int bound_port = 0;
    uint32_t local_job_id = 0;
    uint32_t sched_client = 0;
    MsgChannel *client_a = nullptr, *client_b = nullptr, *client_c = nullptr;
    MsgChannel *client_d = nullptr, *client_e = nullptr;
    MsgChannel *attempt = nullptr, *active = nullptr;
    Msg *local_reply = nullptr, *login = nullptr, *relogin = nullptr,
        *forwarded = nullptr;

    /* Correction 2: prove the negative helper before relying on it. */
    {
        std::string d;
        REQUIRE(run_same_burst_control(&d),
                "same-burst control: helper reports a userspace-buffered frame "
                "without a new kernel edge");
        fprintf(stderr, "         (%s)\n", d.c_str());
    }

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

    daemon_pid = fork();
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

    /* ==================================================================
       ROW 1 -- DISCONNECTED zero-count, on the client that then becomes
       the long-lived local job.  Same client: count=0 then count=1.
       count=0 -> zero reply/state; count=1 -> exactly one synthetic local
       UseCS.  (At 85ee218 the schedulerless path synthesizes a UseCS for
       count=0 -> the zero-reply assertion is RED.)
       ================================================================== */
    client_a = connect_unix_bounded(socket_path, 5000);
    REQUIRE_OR_ABORT(client_a != nullptr, "client A connected (DISCONNECTED)");

    REQUIRE(client_a->send_msg(make_getcs("dis-zero.cpp", 0)),
            "DISCONNECTED: sent GetCS(count=0)");
    {
        std::string s;
        REQUIRE(expect_no_complete_frame(client_a, 2000, &s),
                "DISCONNECTED count=0: zero UseCS reply");
        if (!s.empty())
            fprintf(stderr, "         (count=0 produced: %s)\n", s.c_str());
    }

    REQUIRE_OR_ABORT(client_a->send_msg(make_getcs("local-a.cpp", 1)),
                     "DISCONNECTED: same client sent GetCS(count=1)");
    local_reply = wait_for_type(client_a, Msg::USE_CS, 5000);
    REQUIRE_OR_ABORT(local_reply != nullptr,
                     "DISCONNECTED count=1: a UseCS was delivered");
    {
        UseCSMsg *u = dynamic_cast<UseCSMsg *>(local_reply);
        REQUIRE_OR_ABORT(u != nullptr, "DISCONNECTED count=1: reply is UseCS");
        local_job_id = u->job_id;
    }
    delete local_reply;
    local_reply = nullptr;
    REQUIRE_OR_ABORT(local_job_id != 0,
                     "DISCONNECTED count=1: captured a non-zero synthetic job id");
    {
        std::string s;
        REQUIRE(expect_no_complete_frame(client_a, 1000, &s),
                "DISCONNECTED count=1: exactly one UseCS (no second)");
    }

    /* ==================================================================
       Bring up the fake scheduler; accept Login, withhold ConfCS.  The
       daemon is now in LOGIN_ATTEMPT.
       ================================================================== */
    listener = listen_on_port(scheduler_port, &bound_port);
    REQUIRE_OR_ABORT(listener >= 0 && bound_port == scheduler_port,
                     "fake scheduler bound the reserved port");
    attempt = accept_login_channel(listener, 20000, &login);
    REQUIRE_OR_ABORT(attempt != nullptr, "iceccd opened a candidate scheduler channel");
    REQUIRE_OR_ABORT(login != nullptr, "candidate scheduler received Login");
    delete login;

    /* First login attempt (#1): ConfCS withheld.  This attempt exercises the D1
       pre-active silence gate and is then abandoned to prove an unactivated
       attempt loss performs zero established-session cleanups.  The LOGIN_ATTEMPT
       zero-count rows run on the SECOND attempt (below): bigoracle requires the
       held count=1 to flush on a ConfCS delivered to the SAME attempt that
       received it, not one carried across an abandoned attempt.

       D1 pre-active silence: drive the real schedulerless GetCS-derived
       lifecycle on client A (CompileFile then JobDone with the captured
       synthetic job id), traversing handle_compile_file()/handle_job_done().
       No application frame may leak to the withheld scheduler. */
    {
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
    }
    {
        std::string leaked;
        REQUIRE(expect_no_complete_frame(attempt, 4000, &leaked),
                "no pre-active application frame on the Login attempt channel");
        if (!leaked.empty())
            fprintf(stderr, "         (leaked pre-active frame: %s)\n", leaked.c_str());
    }

    delete attempt;
    attempt = nullptr;
    {
        const std::string preactive = request_internals(client_a, 5000);
        REQUIRE(!preactive.empty(),
                "client A alive after the Login attempt closed");
        REQUIRE(preactive.find("cleanup_attempts=0") != std::string::npos,
                "Login-attempt loss performed zero established-session cleanups");
    }

    active = accept_login_channel(listener, 20000, &relogin);
    REQUIRE_OR_ABORT(active != nullptr, "iceccd retried the scheduler connection");
    REQUIRE_OR_ABORT(relogin != nullptr, "replacement scheduler received Login");
    delete relogin;

    /* ROW 2 -- LOGIN_ATTEMPT zero-count, on this second attempt while ConfCS is
       still withheld.  count=0 -> zero daemon->S frames; count=1 -> retained
       privately (no S frame yet); ConfCS -> exactly one GetCS reaches S. */
    client_b = connect_unix_bounded(socket_path, 5000);
    REQUIRE_OR_ABORT(client_b != nullptr, "client B connected (LOGIN_ATTEMPT)");
    REQUIRE(client_b->send_msg(make_getcs("attempt-zero.cpp", 0)),
            "LOGIN_ATTEMPT: client B sent GetCS(count=0)");
    {
        std::string s;
        REQUIRE(expect_no_complete_frame(active, 2000, &s),
                "LOGIN_ATTEMPT count=0: zero daemon-to-scheduler frames");
    }
    REQUIRE(client_b->send_msg(make_getcs("attempt-one.cpp", 1)),
            "LOGIN_ATTEMPT: client B sent GetCS(count=1)");
    {
        std::string s;
        REQUIRE(expect_no_complete_frame(active, 2000, &s),
                "LOGIN_ATTEMPT count=1: retained privately (no S frame yet)");
    }

    REQUIRE(active->send_msg(ConfCSMsg()),
            "replacement scheduler sent the activating ConfCS");
    REQUIRE(active->send_msg(ConfCSMsg()),
            "replacement scheduler sent a duplicate ConfCS");

    /* ROW 2 conclusion: the retained count=1 from client B flushes as exactly
       one GetCS on the now-active scheduler channel. */
    {
        const int n = count_frames_of_type(active, Msg::GET_CS, 4000);
        REQUIRE(n == 1,
                "LOGIN_ATTEMPT->ACTIVE: retained count=1 flushed exactly one GetCS");
        fprintf(stderr, "         (GetCS reaching S after ConfCS: %d)\n", n);
    }

    {
        const std::string active_state = request_internals(client_a, 5000);
        REQUIRE(active_state.find("ownership_failed=0 gen=1") != std::string::npos,
                "first ConfCS committed exactly one generation");
    }

    /* ==================================================================
       ROW 3 -- ACTIVE zero-count.  Fresh client: count=0 -> zero GetCS
       reaches S; count=1 -> exactly one GetCS reaches S.  (Stats frames on
       the active channel are ignored by count_frames_of_type.)
       ================================================================== */
    client_d = connect_unix_bounded(socket_path, 5000);
    REQUIRE_OR_ABORT(client_d != nullptr, "client D connected (ACTIVE)");
    REQUIRE(client_d->send_msg(make_getcs("active-zero.cpp", 0)),
            "ACTIVE: client D sent GetCS(count=0)");
    {
        const int n = count_frames_of_type(active, Msg::GET_CS, 2000);
        REQUIRE(n == 0, "ACTIVE count=0: zero GetCS reaches S");
        fprintf(stderr, "         (GetCS after count=0: %d)\n", n);
    }
    REQUIRE(client_d->send_msg(make_getcs("active-one.cpp", 1)),
            "ACTIVE: client D sent GetCS(count=1)");
    {
        const int n = count_frames_of_type(active, Msg::GET_CS, 3000);
        REQUIRE(n == 1, "ACTIVE count=1: exactly one GetCS reaches S");
        fprintf(stderr, "         (GetCS after count=1: %d)\n", n);
    }

    /* ==================================================================
       ROW 4 -- NONZERO OUTSTANDING.  Publish count=1 (reaches S), then send
       count=0 on the SAME client: it must not close, replace, or consume the
       outstanding request, and the original scheduler decision must still be
       delivered exactly once.
       ================================================================== */
    client_e = connect_unix_bounded(socket_path, 5000);
    REQUIRE_OR_ABORT(client_e != nullptr, "client E connected (NONZERO OUTSTANDING)");
    REQUIRE(client_e->send_msg(make_getcs("outstanding.cpp", 1)),
            "OUTSTANDING: client E published GetCS(count=1)");
    forwarded = wait_for_type(active, Msg::GET_CS, 4000);
    REQUIRE_OR_ABORT(forwarded != nullptr, "OUTSTANDING: original GetCS reached S");
    {
        GetCSMsg *g = dynamic_cast<GetCSMsg *>(forwarded);
        REQUIRE_OR_ABORT(g != nullptr, "OUTSTANDING: forwarded frame is a GetCS");
        sched_client = g->client_id;  /* daemon->S correlation is by client_id */
    }
    delete forwarded;
    forwarded = nullptr;

    REQUIRE(client_e->send_msg(make_getcs("outstanding-zero.cpp", 0)),
            "OUTSTANDING: client E sent GetCS(count=0) while count=1 pending");
    {
        const int n = count_frames_of_type(active, Msg::GET_CS, 1500);
        REQUIRE(n == 0,
                "OUTSTANDING count=0: no new/duplicate GetCS reaches S");
    }
    /* deliver the decision for the ORIGINAL request; it must arrive once and
       the client must not have been closed by the interleaved count=0. */
    REQUIRE(active->send_msg(UseCSMsg("x86_64", "127.0.0.1", 10245,
                                      9001 /*S-assigned job id*/, true,
                                      sched_client, 0)),
            "OUTSTANDING: scheduler delivered the decision for the original job");
    {
        Msg *decision = wait_for_type(client_e, Msg::USE_CS, 4000);
        REQUIRE(decision != nullptr,
                "OUTSTANDING: original decision delivered to client E exactly once");
        delete decision;
        std::string s;
        REQUIRE(expect_no_complete_frame(client_e, 1000, &s),
                "OUTSTANDING: no second decision to client E");
    }

    /* ==================================================================
       Active-session loss cleanup: exactly one established-session cleanup.
       ================================================================== */
    delete active;
    active = nullptr;
    REQUIRE(wait_eof(client_a, 5000),
            "active-session loss cleaned the original local client");
    REQUIRE(wait_eof(client_b, 5000),
            "active-session loss cleaned the pending client");
    client_c = connect_unix_bounded(socket_path, 5000);
    REQUIRE(client_c != nullptr,
            "fresh observer connected after active-session cleanup");
    {
        const std::string after_loss = request_internals(client_c, 5000);
        REQUIRE(after_loss.find("cleanup_attempts=1") != std::string::npos,
                "active-session cleanup completed exactly once");
    }

done:
    if (daemon_pid > 0) {
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
    }

    delete client_a;
    delete client_b;
    delete client_c;
    delete client_d;
    delete client_e;
    delete attempt;
    delete active;
    if (listener >= 0) {
        close(listener);
    }

    fprintf(stderr, "%s (%d failure%s)\n",
            failures ? "RESULT: FAIL" : "RESULT: PASS", failures,
            failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
