/*
   S2 daemon-relay gate (BigOracle steer).

   A real iceccd, given a hand-constructed scheduler UseCS that carries a
   full P50 assignment identity and a valid cache-handoff tail selecting
   THIS daemon itself as F (the "127.0.0.1 rewrite" branch in
   Daemon::scheduler_use_cs), must relay that SAME validated triple -- not
   silently drop it -- to its own local C client.  This branch's
   c->usecsmsg construction is the actual wire vehicle (delivered via the
   PENDING_USE_CS drain loop once a local slot is free), unlike the ordinary
   remote-worker branch, which forwards the scheduler's original frame
   directly and so cannot regress this way.  The rewrite must change only
   derived host reachability (127.0.0.1) -- never the cache port/protocol/
   mask, nor the assignment identity.

   Usage: cachehandoffdaemon <iceccd>
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

    char temp_template[] = "/tmp/icecream-s2-daemon-relay.XXXXXX";
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
    const int daemon_port = reserve_port();
    REQUIRE(daemon_port > 0, "an unused daemon port was reserved");
    if (daemon_port <= 0) {
        return 2;
    }

    const pid_t daemon_pid = fork();
    if (daemon_pid == 0) {
        char scheduler_spec[64];
        snprintf(scheduler_spec, sizeof(scheduler_spec), "127.0.0.1:%d",
                 scheduler_port);
        char daemon_port_text[16];
        snprintf(daemon_port_text, sizeof(daemon_port_text), "%d", daemon_port);
        setenv("ICECC_TESTS", "1", 1);
        setenv("ICECC_TEST_SOCKET", socket_path.c_str(), 1);
        /* Deliberately NOT --no-remote: that flag zeroes d.daemon_port
           unconditionally (daemon/main.cpp's option parser), which would
           make msg->port == daemon_port structurally unsatisfiable and
           silently route every test through the remote-worker branch
           instead of the self-selected-F (127.0.0.1 rewrite) branch this
           test exists to exercise. */
        execl(argv[1], argv[1], "-m", "2", "-p", daemon_port_text,
              "-s", scheduler_spec, "-n", "s2-relay-gate", "-N", "s2-relay-daemon",
              "-b", envdir.c_str(), "-l", daemon_log.c_str(),
              "-v", "-v", "-v", static_cast<char *>(nullptr));
        perror("execl iceccd");
        _exit(127);
    }
    REQUIRE(daemon_pid > 0, "iceccd process started");
    if (daemon_pid < 0) {
        return 2;
    }

    int bound_port = 0;
    const int listener = listen_on_port(scheduler_port, &bound_port);
    REQUIRE(listener >= 0 && bound_port == scheduler_port,
            "fake scheduler became available on the reserved port");
    Msg *login_wire = nullptr;
    MsgChannel *scheduler = accept_login_channel(listener, 20000, &login_wire);
    REQUIRE(scheduler != nullptr, "iceccd logged in to the fake scheduler");
    LoginMsg *login = login_wire ? dynamic_cast<LoginMsg *>(login_wire) : nullptr;
    REQUIRE(login != nullptr, "candidate scheduler received a decodable Login");
    /* The daemon's own advertised port (LoginMsg::port, the FIRST field of
       its outgoing Login -- see daemon/main.cpp's LoginMsg construction)
       is read directly rather than assumed.  An unprivileged test process
       lacking CAP_SYS_CHROOT cannot safely accept remote jobs, so
       daemon/main.cpp's option parser forces this to 0 regardless of -p or
       --no-remote (see the noremote fallback there) -- which is exactly
       the real, observable value Daemon::scheduler_use_cs will compare
       msg->port against, so it is exactly what the self-selected-F UseCS
       below must also carry to hit that branch. */
    const uint32_t observed_daemon_port = login ? login->port : 0;
    delete login_wire;
    if (scheduler) {
        const ConfCSMsg legacy_config(UINT64_C(0x5200000000000001), ConfCSMsg::Legacy);
        REQUIRE(scheduler->send_msg(legacy_config),
                "fake scheduler activated the session");
    }

    /* This daemon selected as its OWN F (the 127.0.0.1 rewrite branch): a
       local client requests a job, the daemon forwards GetCS to (fake) S,
       and S replies with a hand-built UseCS whose hostname:port matches
       THIS daemon's own remote-observed identity, carrying a full P50
       identity and a valid cache tail.  The client must receive that SAME
       triple and identity unchanged. */
    MsgChannel *client = connect_unix_bounded(socket_path, 5000);
    REQUIRE(client != nullptr, "local client A connected");
    GetCSMsg request(Environments(), "s2-relay.cpp", CompileJob::Lang_CXX,
                     1, "x86_64", 0, std::string(), 0, 0, 0);
    REQUIRE(client && client->send_msg(request),
            "local client A requested one assignment");

    Msg *forwarded_wire = scheduler ? wait_for_type(scheduler, Msg::GET_CS, 5000)
                                    : nullptr;
    GetCSMsg *forwarded = forwarded_wire ? dynamic_cast<GetCSMsg *>(forwarded_wire)
                                         : nullptr;
    REQUIRE(forwarded != nullptr, "fake scheduler received the forwarded GetCS");

    const uint32_t remote_client_id = forwarded ? forwarded->client_id : 0;
    const uint32_t wire_job_id = UINT32_C(0x00005201);
    const uint64_t assignment_epoch = UINT64_C(0x5200000000000001);
    const uint64_t assignment_nonce = UINT64_C(0x1122334455667788);
    const uint32_t expected_cache_port = UINT32_C(0x0000cafe);

    if (scheduler && forwarded) {
        UseCSMsg reply("x86_64", "127.0.0.1", observed_daemon_port,
                       wire_job_id, true, remote_client_id, 0,
                       assignment_epoch, assignment_nonce,
                       expected_cache_port, CACHE_WIRE_PROTOCOL_V1,
                       CACHE_PROFILE_ZSTD_TU);
        REQUIRE(scheduler->send_msg(reply),
                "fake scheduler sent a self-selected UseCS with a valid cache tail");
    }
    delete forwarded_wire;

    Msg *client_wire = client ? wait_for_type(client, Msg::USE_CS, 5000) : nullptr;
    UseCSMsg *client_use = client_wire ? dynamic_cast<UseCSMsg *>(client_wire)
                                       : nullptr;
    REQUIRE(client_use != nullptr,
            "local client A received the relayed UseCS (PENDING_USE_CS drained)");
    REQUIRE(client_use && client_use->hostname == "127.0.0.1"
                && client_use->port == observed_daemon_port,
            "S2: the local rewrite changes only derived host reachability");
    REQUIRE(client_use && client_use->assignmentEpoch() == assignment_epoch
                && client_use->assignmentNonce() == assignment_nonce,
            "S2: assignment identity survives the local-rewrite relay unchanged");
    REQUIRE(client_use && client_use->hasCacheAdvertisement()
                && client_use->cache_endpoint_port == expected_cache_port
                && client_use->cache_protocol == CACHE_WIRE_PROTOCOL_V1
                && client_use->cache_profile_mask == CACHE_PROFILE_ZSTD_TU,
            "S2: the local-rewrite relay carries the SAME validated cache "
            "triple -- port/protocol/mask unchanged by the host rewrite");
    delete client_wire;

    /* Client-binding check (BigOracle steer): a cache tail that is
       INDIVIDUALLY valid (port in range, correct protocol, in-mask) but
       carries NO assignment identity must still project as absent -- the
       daemon's own re-validation (Daemon::scheduler_use_cs) must not trust
       the tail's own well-formedness alone.  A real scheduler's
       project_cache_handoff already enforces this before it ever reaches
       the wire, so this exercises the daemon's INDEPENDENT check directly
       by hand-crafting the one combination a real scheduler can never
       produce. */
    MsgChannel *client_b = connect_unix_bounded(socket_path, 5000);
    REQUIRE(client_b != nullptr, "local client B connected");
    GetCSMsg request_b(Environments(), "s2-relay-b.cpp", CompileJob::Lang_CXX,
                       1, "x86_64", 0, std::string(), 0, 0, 0);
    REQUIRE(client_b && client_b->send_msg(request_b),
            "local client B requested a second assignment");

    Msg *forwarded_b_wire = scheduler ? wait_for_type(scheduler, Msg::GET_CS, 5000)
                                      : nullptr;
    GetCSMsg *forwarded_b = forwarded_b_wire
        ? dynamic_cast<GetCSMsg *>(forwarded_b_wire) : nullptr;
    REQUIRE(forwarded_b != nullptr,
            "fake scheduler received client B's forwarded GetCS");

    if (scheduler && forwarded_b) {
        UseCSMsg reply_b("x86_64", "127.0.0.1", observed_daemon_port,
                         UINT32_C(0x00005202), true, forwarded_b->client_id, 0,
                         /* assignment_epoch */ 0, /* assignment_nonce */ 0,
                         expected_cache_port, CACHE_WIRE_PROTOCOL_V1,
                         CACHE_PROFILE_ZSTD_TU);
        REQUIRE(scheduler->send_msg(reply_b),
                "fake scheduler sent a valid-tail, identity-less UseCS for B");
    }
    delete forwarded_b_wire;

    Msg *client_b_wire = client_b ? wait_for_type(client_b, Msg::USE_CS, 5000)
                                  : nullptr;
    UseCSMsg *client_b_use = client_b_wire
        ? dynamic_cast<UseCSMsg *>(client_b_wire) : nullptr;
    REQUIRE(client_b_use != nullptr, "local client B received a UseCS");
    REQUIRE(client_b_use && !client_b_use->hasAssignmentIdentity(),
            "client B's assignment identity is genuinely absent (the fixture)");
    REQUIRE(client_b_use && !client_b_use->hasCacheAdvertisement()
                && client_b_use->cache_protocol == 0
                && client_b_use->cache_profile_mask == 0,
            "S2: an individually-valid cache tail with NO assignment "
            "identity still projects wholly absent -- the daemon does not "
            "trust tail well-formedness alone");
    delete client_b_wire;
    delete client_b;

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
    delete scheduler;
    close(listener);

    fprintf(stderr, "%s (%d failure%s)\n",
            failures ? "RESULT: FAIL" : "RESULT: PASS", failures,
            failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
