/*
   S2 daemon-relay gate (BigOracle steer).

   A real iceccd, given a hand-constructed scheduler UseCS that carries a
   full P50 assignment identity and a valid cache-handoff tail, must relay
   that SAME validated triple -- not silently drop it -- to its own local
   C client, on BOTH of Daemon::scheduler_use_cs's branches:

   - Client A: the scheduler selects THIS daemon itself as F (the
     "127.0.0.1 rewrite" branch).  This branch's c->usecsmsg construction
     is the actual wire vehicle, delivered via the PENDING_USE_CS drain
     loop once a local slot is free.  The rewrite must change only derived
     host reachability (127.0.0.1) and the independently-validated cache
     projection -- never any other field.  BigOracle (5th gap, a REAL
     pre-existing product bug predating the cache work): this branch used
     to hand-rebuild the relay from individual fields, hardcoding
     got_env=true and client_id=1 regardless of what the scheduler
     actually sent -- client/remote.cpp's build_remote_int reads got_env
     to decide whether to send EnvTransferMsg, so a real got_env=false
     reply got silently overridden and a required environment transfer
     could be skipped.  A throwaway connection consumes daemon-assigned
     client_id 1 before Client A connects (so Client A's own id is
     verifiably NOT 1, the exact value the bug hardcoded), and the reply
     below sends got_env=false and a nonzero matched_job_id -- neither
     value the old bug's hardcoding could produce by coincidence.
     usecs_matches_except_host_and_cache asserts the client-visible frame
     equals the scheduler's own frame in every field except the two this
     branch is actually allowed to change.

   - Client C: the scheduler selects a DIFFERENT, remote host as F (the
     ordinary remote-worker branch).  BigOracle (d23d9c5d HOLD): this
     branch's c->usecsmsg is NOT its wire vehicle -- it exists only for
     introspection (dump_internals, the web JSON endpoints).  The actual
     client delivery is c->channel->send_msg(*msg), relaying the
     scheduler's own frame directly.  Nothing previously exercised this
     branch with a cache-bearing message at all, so a regression that
     stripped the cache triple from *msg specifically (leaving
     relay_cache_port/protocol/mask and c->usecsmsg untouched) would have
     passed every other test green.

   - Client D (BigOracle d23d9c5d HOLD, Gap 3): Daemon::scheduler_no_cs
     clears c->cacheHandoff on a NoCS decision so a handoff retained from
     an earlier dispatch on the same (reused) Client cannot leak forward.
     No real wire flow can hand this site a Client that both genuinely
     retained a prior handoff and is making a second live GetCS decision
     (Client::getcs_outstanding forbids a second GetCS on one connection
     outright), so this scenario uses the test-only
     ICECC_TEST_POISON_CACHE_HANDOFF_SITE hook to fabricate that
     precondition immediately before the real clear runs, then reads the
     result back via dump_internals (GetInternalStatus) -- the only way an
     external test process can observe one Client's private field.

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

/* Bounded retry over request_internals(): the daemon processes client D's
   status query and the scheduler's earlier NoCS reply on two DIFFERENT
   connections, so a single status snapshot is not guaranteed to already
   reflect NoCS having run.  Polls until the needle appears or the overall
   deadline passes, returning whatever the last snapshot was either way. */
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

/* S2 (BigOracle, 5th gap): the client-visible frame must equal the
   scheduler's own frame in every field except the local-rewrite's two
   allowed changes -- derived host reachability (hostname/port) and the
   independently-validated cache projection (cache_endpoint_port/
   cache_protocol/cache_profile_mask), which the caller checks
   separately.  Everything else -- job id, platform, got_env, client_id,
   matched_job_id, epoch, nonce -- must survive exactly. */
static bool usecs_matches_except_host_and_cache(const UseCSMsg &sent,
                                                const UseCSMsg &received)
{
    return received.job_id == sent.job_id
        && received.host_platform == sent.host_platform
        && received.got_env == sent.got_env
        && received.client_id == sent.client_id
        && received.matched_job_id == sent.matched_job_id
        && received.assignmentEpoch() == sent.assignmentEpoch()
        && received.assignmentNonce() == sent.assignmentNonce();
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
        /* S2 Gap 3 (BigOracle d23d9c5d HOLD): arms the NoCS-site poison/
           record hook (see test_poison_cache_handoff_if_armed's own
           comment in daemon/main.cpp) for Client D below.  Client A/B/C
           above never reach Daemon::scheduler_no_cs, so arming this for
           the whole daemon process cannot cross-contaminate their
           assertions. */
        setenv("ICECC_TEST_POISON_CACHE_HANDOFF_SITE", "no_cs", 1);
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

    /* S2 (BigOracle, 5th gap): consume client_id=1 with a throwaway
       connection before Client A, so Client A's own (daemon-assigned,
       first-come-first-served at accept time -- see
       Daemon's `client->client_id = ++new_client_id;`) client_id is
       verifiably NOT 1 -- the exact value the local-rewrite branch used
       to hardcode.  Without this, Client A would legitimately BE client
       id 1 as the first real connection, and a hardcoded-1 regression
       would be invisible. */
    MsgChannel *client_zero = connect_unix_bounded(socket_path, 5000);
    REQUIRE(client_zero != nullptr, "throwaway client zero connected to consume id 1");
    delete client_zero;

    /* This daemon selected as its OWN F (the 127.0.0.1 rewrite branch): a
       local client requests a job, the daemon forwards GetCS to (fake) S,
       and S replies with a hand-built UseCS whose hostname:port matches
       THIS daemon's own remote-observed identity, carrying a full P50
       identity and a valid cache tail.  The client must receive that SAME
       triple and identity unchanged.  S2 (BigOracle, 5th gap): got_env is
       deliberately false (the opposite of what the branch used to
       hardcode) and matched_job_id is deliberately nonzero, so neither
       can pass by coincidence -- see usecs_matches_except_host_and_cache
       below for the exact-field-equality check this fixture exists to
       drive. */
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
    REQUIRE(forwarded && forwarded->client_id != UINT32_C(1),
            "S2 Gap 5: client A's real daemon-assigned client_id is NOT "
            "1 -- the throwaway connection above did its job, so a "
            "hardcoded-1 regression cannot pass by coincidence");

    const uint32_t remote_client_id = forwarded ? forwarded->client_id : 0;
    const uint32_t wire_job_id = UINT32_C(0x00005201);
    const uint64_t assignment_epoch = UINT64_C(0x5200000000000001);
    const uint64_t assignment_nonce = UINT64_C(0x1122334455667788);
    const uint32_t expected_cache_port = UINT32_C(0x0000cafe);
    const uint32_t expected_matched_job_id = UINT32_C(77);

    UseCSMsg reply("x86_64", "127.0.0.1", observed_daemon_port,
                   wire_job_id, false, remote_client_id, expected_matched_job_id,
                   assignment_epoch, assignment_nonce,
                   expected_cache_port, CACHE_WIRE_PROTOCOL_V1,
                   CACHE_PROFILE_ZSTD_TU);
    if (scheduler && forwarded) {
        REQUIRE(scheduler->send_msg(reply),
                "fake scheduler sent a self-selected UseCS (got_env=false, "
                "nontrivial client_id, nonzero matched_job_id) with a "
                "valid cache tail");
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
    REQUIRE(client_use && client_use->hasCacheAdvertisement()
                && client_use->cache_endpoint_port == expected_cache_port
                && client_use->cache_protocol == CACHE_WIRE_PROTOCOL_V1
                && client_use->cache_profile_mask == CACHE_PROFILE_ZSTD_TU,
            "S2: the local-rewrite relay carries the SAME validated cache "
            "triple -- port/protocol/mask unchanged by the host rewrite");
    /* S2 (BigOracle, 5th gap): the client-visible frame must equal the
       scheduler's own frame in EVERY field except the allowed hostname/
       port rewrite and the independently-validated cache projection
       (already checked above) -- explicitly including job id, platform,
       got_env, client_id, matched_job_id, epoch, and nonce.  This is the
       exact property a hand-rebuilt constructor (the old bug, and any
       future one like it) cannot satisfy without enumerating every field
       correctly; the real fix (copying *msg and overriding only two
       fields) satisfies it by construction. */
    REQUIRE(client_use && usecs_matches_except_host_and_cache(reply, *client_use),
            "S2 Gap 5: the local-rewrite relay equals the scheduler's frame "
            "in every field except the allowed host/port rewrite and cache "
            "projection -- job id, platform, got_env, client_id, "
            "matched_job_id, epoch, and nonce all survive exactly");
    delete client_wire;

    /* Client C: the scheduler selects a REMOTE host as F -- hostname/port
       matching neither this daemon's own remote-observed identity nor
       127.0.0.1, so msg->hostname == remote_name && msg->port ==
       daemon_port is false and Daemon::scheduler_use_cs takes the
       ordinary remote-worker (else) branch.  This exercises the actual
       client wire vehicle for that branch -- c->channel->send_msg(*msg)
       -- which no earlier scenario in this file (or anywhere else)
       reached with a cache-bearing message. TEST-NET-1 (192.0.2.0/24, RFC
       5737) is used for the selected F's address specifically because it
       is guaranteed non-routable -- this test only checks what the daemon
       DELIVERS to the local client, never what the client would do next
       with that address. */
    MsgChannel *client_c = connect_unix_bounded(socket_path, 5000);
    REQUIRE(client_c != nullptr, "local client C connected");
    GetCSMsg request_c(Environments(), "s2-relay-c.cpp", CompileJob::Lang_CXX,
                       1, "x86_64", 0, std::string(), 0, 0, 0);
    REQUIRE(client_c && client_c->send_msg(request_c),
            "local client C requested a third assignment");

    Msg *forwarded_c_wire = scheduler ? wait_for_type(scheduler, Msg::GET_CS, 5000)
                                      : nullptr;
    GetCSMsg *forwarded_c = forwarded_c_wire
        ? dynamic_cast<GetCSMsg *>(forwarded_c_wire) : nullptr;
    REQUIRE(forwarded_c != nullptr,
            "fake scheduler received client C's forwarded GetCS");

    const std::string remote_f_host = "192.0.2.77";
    const uint32_t remote_f_port = UINT32_C(54321);
    const uint32_t remote_wire_job_id = UINT32_C(0x00005203);
    const uint64_t remote_assignment_epoch = UINT64_C(0x5200000000000002);
    const uint64_t remote_assignment_nonce = UINT64_C(0x99aabbccddeeff00);
    const uint64_t remote_c_guid = UINT64_C(0x52000000000000c3);
    const uint64_t remote_tu_seq = UINT64_C(37);
    const uint32_t remote_cache_port = UINT32_C(0x0000feed);
    /* BigOracle blueprint: also vary got_env and matched_job_id away from
       their zero/default constructor values, and require both -- plus
       client_id -- preserved exactly.  This future-proofs against a
       "normalized rebuild" of the remote-worker branch that reconstructs
       *msg's fields individually instead of relaying the scheduler's frame
       verbatim: such a rebuild could easily carry the cache triple and
       identity correctly while silently dropping or zeroing one of these
       three, and only an assertion on each specific field would catch it. */
    const uint32_t remote_got_env = true;
    const uint32_t remote_matched_job_id = UINT32_C(0x00000037);
    const uint32_t client_c_forwarded_id = forwarded_c ? forwarded_c->client_id : 0;

    UseCSMsg reply_c("x86_64", remote_f_host, remote_f_port,
                     remote_wire_job_id, remote_got_env, client_c_forwarded_id,
                     remote_matched_job_id,
                     remote_assignment_epoch, remote_assignment_nonce,
                     remote_cache_port, CACHE_WIRE_PROTOCOL_V1,
                     CACHE_PROFILE_ZSTD_TU);
    reply_c.setCompileIdentity(remote_c_guid, remote_tu_seq);
    if (scheduler && forwarded_c) {
        REQUIRE(scheduler->send_msg(reply_c),
                "fake scheduler sent a remote-selected-F UseCS with a valid "
                "cache tail");
    }
    delete forwarded_c_wire;

    Msg *client_c_wire = client_c ? wait_for_type(client_c, Msg::USE_CS, 5000)
                                  : nullptr;
    UseCSMsg *client_c_use = client_c_wire ? dynamic_cast<UseCSMsg *>(client_c_wire)
                                           : nullptr;
    REQUIRE(client_c_use != nullptr,
            "local client C received the relayed UseCS (remote-worker branch, "
            "delivered synchronously via send_msg(*msg))");
    REQUIRE(client_c_use && client_c_use->hostname == remote_f_host
                && client_c_use->port == remote_f_port
                && client_c_use->job_id == remote_wire_job_id,
            "S2: the remote-worker relay carries the exact selected-F "
            "hostname, port, and wire job id");
    REQUIRE(client_c_use && client_c_use->got_env == remote_got_env
                && client_c_use->client_id == client_c_forwarded_id
                && client_c_use->matched_job_id == remote_matched_job_id,
            "S2: the remote-worker relay preserves got_env, client_id, and "
            "matched_job_id exactly -- each varied away from its zero/"
            "default constructor value so this row cannot pass by accident");
    REQUIRE(client_c_use && client_c_use->assignmentEpoch() == remote_assignment_epoch
                && client_c_use->assignmentNonce() == remote_assignment_nonce
                && client_c_use->cGuid() == remote_c_guid
                && client_c_use->tuSeq() == remote_tu_seq,
            "S2: assignment and compile identities survive the remote-worker relay unchanged");
    REQUIRE(client_c_use && client_c_use->hasCacheAdvertisement()
                && client_c_use->cache_endpoint_port == remote_cache_port
                && client_c_use->cache_protocol == CACHE_WIRE_PROTOCOL_V1
                && client_c_use->cache_profile_mask == CACHE_PROFILE_ZSTD_TU,
            "S2: the remote-worker relay carries the SAME validated cache "
            "triple -- port/protocol/mask survive the actual client wire "
            "vehicle (send_msg(*msg)), not just c->usecsmsg's introspection "
            "copy");
    delete client_c_wire;
    delete client_c;

    /* A real compiler wrapper can disconnect after receiving UseCS but before
       it sends its own terminal message (for example, a local preprocessing or
       cache-source failure).  The submitter daemon has no CompileJob on this
       path; it must settle from the exact retained UseCS instead of emitting
       zero assignment/C_GUID/TU_SEQ and causing the scheduler to reject its
       whole connection. */
    Msg *client_c_done_wire = scheduler
        ? wait_for_type(scheduler, Msg::JOB_DONE, 5000) : nullptr;
    JobDoneMsg *client_c_done = client_c_done_wire
        ? dynamic_cast<JobDoneMsg *>(client_c_done_wire) : nullptr;
    REQUIRE(client_c_done != nullptr && !client_c_done->is_from_server()
                && client_c_done->job_id == remote_wire_job_id
                && client_c_done->assignmentEpoch() == remote_assignment_epoch
                && client_c_done->assignmentNonce() == remote_assignment_nonce
                && client_c_done->cGuid() == remote_c_guid
                && client_c_done->tuSeq() == remote_tu_seq,
            "submitter teardown settles the exact retained UseCS assignment and compile identity");
    delete client_c_done_wire;

    /* Client D (BigOracle d23d9c5d HOLD, Gap 3 -- reused-client clearing,
       doubling as the blueprint's "Focused test"): Daemon::scheduler_no_cs
       now routes through install_cache_absent_local_decision, which
       atomically clears c->cacheHandoff, deletes any prior usecsmsg, and
       installs the canonical cache-absent replacement -- so a handoff (and
       a stale usecsmsg) retained from an earlier dispatch on this same
       (reused) Client cannot leak into a later NoCS decision.  No real
       wire flow can hand this site a Client that both genuinely retained
       prior state and is making a second live GetCS decision --
       Client::getcs_outstanding (see its own comment in daemon/main.cpp)
       makes a second GetCS on one connection structurally unreachable, so
       there is no way to build that precise precondition with real UseCS
       traffic first.  The child process armed
       ICECC_TEST_POISON_CACHE_HANDOFF_SITE=no_cs above, which fabricates a
       retained handoff AND a stale nonzero-tail usecsmsg on this Client
       immediately before scheduler_no_cs's real, unmodified call runs.
       Two independent observations then prove the helper's contract: (1)
       dump_internals() (queried via GetInternalStatus, the only way this
       external test process can observe one Client's private field) shows
       the handoff reset to canonical absence; (2) the client's ACTUAL
       received UseCS -- the real installed replacement, not the deleted
       stale one -- decodes at all (proving wire-valid, since
       MsgChannel::get_msg already enforces valid_payload() on receipt) and
       carries cache triple exactly 0/0/0.  MUST run before Client B below:
       Client B deliberately makes MsgChannel::send_msg(reply_b) fail on
       this SAME `scheduler` channel (valid_payload() rejection calls
       set_error() on the sending channel), which permanently ERRORs that
       channel for the rest of the process -- placed after Client B, Client
       D's forwarded GetCS would never reach the fake scheduler at all. */
    MsgChannel *client_d = connect_unix_bounded(socket_path, 5000);
    REQUIRE(client_d != nullptr, "local client D connected");
    GetCSMsg request_d(Environments(), "s2-relay-d.cpp", CompileJob::Lang_CXX,
                       1, "x86_64", 0, std::string(), 0, 0, 0);
    REQUIRE(client_d && client_d->send_msg(request_d),
            "local client D requested a fourth assignment");

    Msg *forwarded_d_wire = scheduler ? wait_for_type(scheduler, Msg::GET_CS, 5000)
                                      : nullptr;
    GetCSMsg *forwarded_d = forwarded_d_wire
        ? dynamic_cast<GetCSMsg *>(forwarded_d_wire) : nullptr;
    REQUIRE(forwarded_d != nullptr,
            "fake scheduler received client D's forwarded GetCS");

    if (scheduler && forwarded_d) {
        NoCSMsg no_cs_reply(UINT32_C(0x00005204), forwarded_d->client_id);
        REQUIRE(scheduler->send_msg(no_cs_reply),
                "fake scheduler sent NoCS for client D");
    }
    delete forwarded_d_wire;

    Msg *client_d_wire = client_d ? wait_for_type(client_d, Msg::USE_CS, 5000)
                                  : nullptr;
    UseCSMsg *client_d_use = client_d_wire ? dynamic_cast<UseCSMsg *>(client_d_wire)
                                           : nullptr;
    REQUIRE(client_d_use != nullptr,
            "S2 Gap 3 focused test: client D received the REAL installed "
            "replacement UseCS, not the deleted stale one -- decoding at "
            "all proves it is wire-valid (MsgChannel::get_msg enforces "
            "valid_payload() on receipt)");
    REQUIRE(client_d_use && !client_d_use->hasCacheAdvertisement()
                && client_d_use->cache_endpoint_port == 0
                && client_d_use->cache_protocol == 0
                && client_d_use->cache_profile_mask == 0,
            "S2 Gap 3 focused test: the replacement's cache triple is "
            "exactly 0/0/0, not the stale poisoned nonzero tail");
    delete client_d_wire;

    const std::string clear_needle =
        "Cache-handoff clear test: site=no_cs fired=1 valid=0 port=0 "
        "protocol=0 mask=0";
    const std::string clear_state =
        wait_for_internals_containing(client_d, clear_needle, 5000);
    REQUIRE(clear_state.find(clear_needle) != std::string::npos,
            "S2 Gap 3: scheduler_no_cs's real clear reset a retained "
            "(poisoned) handoff back to canonical absence -- valid, port, "
            "protocol, and mask all zero");
    delete client_d;

    /* Client-binding check (BigOracle steer), now enforced one layer
       earlier than when this scenario was first written (d23d9c5d HOLD,
       identity-binding law): a cache tail that is INDIVIDUALLY valid (port
       in range, correct protocol, in-mask) but carries NO assignment
       identity used to project as absent only through the daemon's own
       re-validation in Daemon::scheduler_use_cs.  That re-validation still
       runs, factored into usecs_cache_handoff_admissible (services/comm.h)
       and unit-tested directly with a hand-constructed object in
       unittests/p50cacheadvertisement.cpp -- but UseCSMsg::valid_payload()
       now refuses to let this exact combination exist as a decoded
       message AT ALL, on either side of the wire, so it can no longer
       reach scheduler_use_cs through this (or any) real integration path.
       A real scheduler's project_cache_handoff already enforced this
       before it ever reached the wire; the wire itself is now a second,
       independent enforcement point, one level below the daemon.  This
       scenario now proves exactly that: the malformed combination is
       refused at construction/send time, before a single byte reaches the
       wire -- client B never receives anything. */
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
        REQUIRE(!scheduler->send_msg(reply_b),
                "S2: the wire refuses a valid-tail, identity-less UseCS "
                "outright -- UseCSMsg::valid_payload rejects it before any "
                "byte is sent, so client B never receives it at all");
    }
    delete forwarded_b_wire;
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
