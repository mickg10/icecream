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

   Usage: cachehandoffdaemon <iceccd> <icecc-cache-service>
*/
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
#include <fstream>
#include <sstream>
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

static bool wait_file_contains(const std::string &path,
                               const std::string &needle,
                               int timeout_msec)
{
    const Clock::time_point deadline = Clock::now()
        + std::chrono::milliseconds(timeout_msec);
    while (Clock::now() < deadline) {
        std::ifstream input(path);
        std::ostringstream contents;
        contents << input.rdbuf();
        if (contents.str().find(needle) != std::string::npos) {
            return true;
        }
        usleep(20 * 1000);
    }
    return false;
}

static bool wait_path_exists(const std::string &path, int timeout_msec)
{
    const Clock::time_point deadline = Clock::now()
        + std::chrono::milliseconds(timeout_msec);
    while (Clock::now() < deadline) {
        if (access(path.c_str(), F_OK) == 0)
            return true;
        usleep(20 * 1000);
    }
    return false;
}

static size_t count_occurrences(const std::string &text,
                                const std::string &needle)
{
    size_t count = 0;
    size_t offset = 0;
    while ((offset = text.find(needle, offset)) != std::string::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

static std::string read_file_contents(const std::string &path)
{
    std::ifstream input(path);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

static pid_t find_child_with_command(pid_t parent,
                                     const std::string &command_fragment)
{
    std::ifstream children("/proc/" + std::to_string(parent) +
                           "/task/" + std::to_string(parent) + "/children");
    pid_t child = 0;
    while (children >> child) {
        std::ifstream command("/proc/" + std::to_string(child) + "/cmdline",
                              std::ios::binary);
        std::ostringstream bytes;
        bytes << command.rdbuf();
        std::string value = bytes.str();
        for (char &byte : value) {
            if (byte == '\0') byte = ' ';
        }
        if (value.find(command_fragment) != std::string::npos)
            return child;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <iceccd> <icecc-cache-service>\n", argv[0]);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);

    const char *temporary_root = std::getenv("TMPDIR");
    std::string temp_template =
        std::string(temporary_root && temporary_root[0] ? temporary_root : "/tmp")
        + "/icecream-s2-daemon-relay.XXXXXX";
    char *temp = mkdtemp(&temp_template[0]);
    if (!temp) {
        perror("mkdtemp");
        return 2;
    }
    const std::string work(temp);
    const std::string envdir = work + "/envs";
    const std::string runtime = work + "/cache-runtime";
    const std::string socket_path = work + "/iceccd.sock";
    const std::string daemon_log = work + "/iceccd.log";
    const std::string ready_trace = work + "/ready.trace";
    const std::string service_wrapper = work + "/cache-service-wrapper.sh";
    const std::string service_launch_count = work + "/cache-service-launch-count";
    const std::string replacement_waiting = work + "/replacement-waiting";
    const std::string replacement_release = work + "/replacement-release";
    mkdir(envdir.c_str(), 0700);
    mkdir(runtime.c_str(), 0700);
    {
        std::ofstream wrapper(service_wrapper);
        wrapper
            << "#!/bin/sh\n"
            << "set -eu\n"
            << "count=0\n"
            << "if test -f \"$ICECC_TEST_CACHE_WRAPPER_COUNT\"; then "
               "IFS= read -r count < \"$ICECC_TEST_CACHE_WRAPPER_COUNT\"; fi\n"
            << "count=$((count + 1))\n"
            << "printf '%s\\n' \"$count\" > \"$ICECC_TEST_CACHE_WRAPPER_COUNT\"\n"
            << "if test \"$count\" -eq 2; then\n"
            << "  : > \"$ICECC_TEST_CACHE_REPLACEMENT_WAITING\"\n"
            << "  while test ! -e \"$ICECC_TEST_CACHE_REPLACEMENT_RELEASE\"; do sleep 0.01; done\n"
            << "  rm -f \"$ICECC_TEST_CACHE_REPLACEMENT_WAITING\" "
               "\"$ICECC_TEST_CACHE_REPLACEMENT_RELEASE\"\n"
            << "fi\n"
            << "exec \"$ICECC_TEST_REAL_CACHE_SERVICE\" \"$@\"\n";
    }
    REQUIRE(chmod(service_wrapper.c_str(), 0700) == 0,
            "cache-service replacement rendezvous wrapper is executable");
    std::string daemon_user;
    if (getuid() == 0) {
        const passwd *account = getpwnam("nobody");
        if (!account || account->pw_uid == 0 || account->pw_gid == 0) return 2;
        // The daemon and its child must own both the directories and wrapper
        // after switching away from the root test runner.
        for (const auto &path : {work, envdir, runtime, service_wrapper}) {
            if (chown(path.c_str(), account->pw_uid, account->pw_gid) != 0) return 2;
        }
        daemon_user = "nobody";
    }
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
        setenv("ICECC_P50_MODE", "on", 1);
        setenv("ICECC_P50_TEST_READY_TRACE", ready_trace.c_str(), 1);
        setenv("ICECC_TEST_REAL_CACHE_SERVICE", argv[2], 1);
        setenv("ICECC_TEST_CACHE_WRAPPER_COUNT", service_launch_count.c_str(), 1);
        setenv("ICECC_TEST_CACHE_REPLACEMENT_WAITING", replacement_waiting.c_str(), 1);
        setenv("ICECC_TEST_CACHE_REPLACEMENT_RELEASE", replacement_release.c_str(), 1);
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
        std::vector<std::string> arguments {
            argv[1], "-m", "2", "-p", daemon_port_text, "-s", scheduler_spec,
            "-n", "s2-relay-gate", "-N", "s2-relay-daemon", "-b", envdir,
            "-l", daemon_log, "--cache-service", service_wrapper,
            "--cache-runtime-dir", runtime, "-v", "-v", "-v"
        };
        if (!daemon_user.empty()) arguments.insert(arguments.end(), {"-u", daemon_user});
        std::vector<char *> pointers;
        for (auto &argument : arguments) pointers.push_back(argument.data());
        pointers.push_back(nullptr);
        execv(argv[1], pointers.data());
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
        const ConfCSMsg advisory_config(UINT64_C(0x5200000000000001),
                                        ConfCSMsg::Advisory);
        REQUIRE(scheduler->send_msg(advisory_config),
                "fake scheduler activated the session");
    }
    REQUIRE(wait_file_contains(daemon_log, "cache sidecar adapter state=2", 10000),
            "authenticated local cache sidecar reached READY before C capability publication");
    REQUIRE(wait_file_contains(ready_trace, "READY v2 ", 5000),
            "C sidecar emitted its exact READY-lease witness");
    std::string stable_ready_witness = read_file_contents(ready_trace);

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
    request.cache_protocol = CACHE_WIRE_REVISION;
    request.cache_profile_mask = CACHE_ADVERTISABLE_PROFILE_MASK;
    const std::string direct_retry_avoid_host = "192.0.2.88";
    const uint32_t direct_retry_avoid_port = UINT32_C(54444);
    request.cache_retry_avoid_host = direct_retry_avoid_host;
    request.cache_retry_avoid_port = direct_retry_avoid_port;
    REQUIRE(client && client->send_msg(request),
            "local client A requested one assignment");

    Msg *forwarded_wire = scheduler ? wait_for_type(scheduler, Msg::GET_CS, 5000)
                                    : nullptr;
    GetCSMsg *forwarded = forwarded_wire ? dynamic_cast<GetCSMsg *>(forwarded_wire)
                                         : nullptr;
    REQUIRE(forwarded != nullptr, "fake scheduler received the forwarded GetCS");
    REQUIRE(forwarded && forwarded->cache_protocol == CACHE_WIRE_REVISION
                && forwarded->cache_profile_mask ==
                    CACHE_ADVERTISABLE_PROFILE_MASK
                && forwarded->cache_affinity_profile_mask == 0
                && forwarded->cache_affinity_port == 0
                && forwarded->cache_affinity_host.empty()
                && forwarded->cache_retry_avoid_host ==
                    direct_retry_avoid_host
                && forwarded->cache_retry_avoid_port ==
                    direct_retry_avoid_port,
            "C daemon authors enabled capabilities and preserves the wrapper's "
            "exact request-local retry exclusion ahead of any warm hint");
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
                   expected_cache_port, CACHE_WIRE_REVISION,
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
                && client_use->cache_protocol == CACHE_WIRE_REVISION
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

    JobDoneMsg successful_done(
        wire_job_id, 0,
        static_cast<uint32_t>(JobDoneMsg::FROM_SUBMITTER) |
            static_cast<uint32_t>(JobDoneMsg::P50CacheRouteObservation), 0,
        assignment_epoch, assignment_nonce);
    REQUIRE(client && client->send_msg(successful_done),
            "client A reports the local-only successful cache-route observation");

    /* A UseCS may be delivered immediately before the supervised C sidecar
       retires at its bounded process-lifetime limit.  The wrapper has not
       started a source operation while it is waiting for the private control
       descriptor, so the daemon must retain that exact scheduler assignment,
       wait for the successor READY lease, and service the original request on
       the still-live ordinary client connection.  The executable wrapper used
       by this test gates only launch #2, making the otherwise tiny replacement
       window deterministic without adding a production test hook. */
    MsgChannel *client_rebind = connect_unix_bounded(socket_path, 5000);
    REQUIRE(client_rebind != nullptr,
            "replacement-race client connected before sidecar retirement");
    GetCSMsg request_rebind(
        Environments(), "replacement-race.cpp", CompileJob::Lang_CXX,
        1, "x86_64", 0, std::string(), 0, 0, 0);
    request_rebind.cache_protocol = CACHE_WIRE_REVISION;
    request_rebind.cache_profile_mask = CACHE_ADVERTISABLE_PROFILE_MASK;
    REQUIRE(client_rebind && client_rebind->send_msg(request_rebind),
            "replacement-race client requested one cache assignment");

    Msg *forwarded_rebind_wire = scheduler
        ? wait_for_type(scheduler, Msg::GET_CS, 5000) : nullptr;
    GetCSMsg *forwarded_rebind = forwarded_rebind_wire
        ? dynamic_cast<GetCSMsg *>(forwarded_rebind_wire) : nullptr;
    REQUIRE(forwarded_rebind != nullptr &&
                forwarded_rebind->cache_protocol == CACHE_WIRE_REVISION &&
                forwarded_rebind->cache_profile_mask ==
                    CACHE_ADVERTISABLE_PROFILE_MASK,
            "replacement-race GetCS retained the current C capability");

    const uint32_t rebind_wire_job_id = UINT32_C(0x00005210);
    const uint64_t rebind_assignment_epoch =
        UINT64_C(0x5200000000000010);
    const uint64_t rebind_assignment_nonce =
        UINT64_C(0x1020304050607080);
    const uint64_t rebind_c_guid = UINT64_C(0x52000000000000d0);
    const uint64_t rebind_tu_seq = UINT64_C(48);
    const std::string rebind_f_host = "192.0.2.110";
    const uint32_t rebind_f_port = UINT32_C(54310);
    const uint32_t rebind_cache_port = UINT32_C(0x0000fed0);
    UseCSMsg reply_rebind(
        "x86_64", rebind_f_host, rebind_f_port, rebind_wire_job_id, true,
        forwarded_rebind ? forwarded_rebind->client_id : 0,
        UINT32_C(0x00000048), rebind_assignment_epoch,
        rebind_assignment_nonce, rebind_cache_port, CACHE_WIRE_REVISION,
        CACHE_PROFILE_P29V1);
    reply_rebind.setCompileIdentity(rebind_c_guid, rebind_tu_seq);
    if (scheduler && forwarded_rebind) {
        REQUIRE(scheduler->send_msg(reply_rebind),
                "fake scheduler delivered the replacement-race UseCS");
    }
    delete forwarded_rebind_wire;

    Msg *client_rebind_wire = client_rebind
        ? wait_for_type(client_rebind, Msg::USE_CS, 5000) : nullptr;
    UseCSMsg *client_rebind_use = client_rebind_wire
        ? dynamic_cast<UseCSMsg *>(client_rebind_wire) : nullptr;
    REQUIRE(client_rebind_use != nullptr &&
                client_rebind_use->job_id == rebind_wire_job_id &&
                client_rebind_use->assignmentEpoch() ==
                    rebind_assignment_epoch &&
                client_rebind_use->assignmentNonce() ==
                    rebind_assignment_nonce &&
                client_rebind_use->cGuid() == rebind_c_guid &&
                client_rebind_use->tuSeq() == rebind_tu_seq &&
                client_rebind_use->cache_profile_mask == CACHE_PROFILE_P29V1,
            "replacement-race client retained the exact live assignment");
    delete client_rebind_wire;

    // Publish a second request under the old READY lease, but withhold its
    // scheduler decision until replacement completes. This is distinct from
    // the already-delivered assignment's descriptor race above.
    MsgChannel *client_inflight = connect_unix_bounded(socket_path, 5000);
    REQUIRE(client_inflight && client_inflight->send_msg(request_rebind),
            "inflight GetCS submitted before C-sidecar replacement");
    Msg *inflight_wire = scheduler
        ? wait_for_type(scheduler, Msg::GET_CS, 5000) : nullptr;
    GetCSMsg *inflight_getcs = inflight_wire
        ? dynamic_cast<GetCSMsg *>(inflight_wire) : nullptr;
    REQUIRE(inflight_getcs &&
                inflight_getcs->cache_protocol == CACHE_WIRE_REVISION &&
                inflight_getcs->cache_profile_mask == CACHE_ADVERTISABLE_PROFILE_MASK,
            "inflight GetCS published old-lease P50 capability to scheduler");
    UseCSMsg inflight_reply(
        "x86_64", rebind_f_host, rebind_f_port, UINT32_C(0x5211), true,
        inflight_getcs ? inflight_getcs->client_id : 0, 0,
        rebind_assignment_epoch, rebind_assignment_nonce + 1,
        rebind_cache_port, CACHE_WIRE_REVISION, CACHE_PROFILE_P29V1);
    inflight_reply.setCompileIdentity(rebind_c_guid, rebind_tu_seq + 1);
    delete inflight_wire;

    MsgChannel *client_recovering = connect_unix_bounded(socket_path, 5000);
    REQUIRE(client_recovering && client_recovering->send_msg(request_rebind),
            "recovery-window GetCS submitted under original lease");
    Msg *recovering_wire = scheduler
        ? wait_for_type(scheduler, Msg::GET_CS, 5000) : nullptr;
    GetCSMsg *recovering_getcs = recovering_wire
        ? dynamic_cast<GetCSMsg *>(recovering_wire) : nullptr;
    REQUIRE(recovering_getcs && recovering_getcs->cache_protocol == CACHE_WIRE_REVISION,
            "recovery-window GetCS published before retirement");
    UseCSMsg recovering_reply(inflight_reply);
    recovering_reply.job_id = UINT32_C(0x5212);
    recovering_reply.client_id = recovering_getcs ? recovering_getcs->client_id : 0;
    delete recovering_wire;

    const pid_t retired_sidecar = find_child_with_command(daemon_pid, argv[2]);
    REQUIRE(retired_sidecar > 0,
            "replacement-race test identified the authenticated sidecar PID");
    REQUIRE(retired_sidecar > 0 && kill(retired_sidecar, SIGKILL) == 0,
            "replacement-race test retired the exact sidecar incarnation");
    REQUIRE(wait_path_exists(replacement_waiting, 10000),
            "successor sidecar launch is held before READY");

    REQUIRE(scheduler && scheduler->send_msg(recovering_reply),
            "scheduler decision arrives while successor READY is gated");
    Msg *recovering_use_wire = client_recovering
        ? wait_for_type(client_recovering, Msg::USE_CS, 5000) : nullptr;
    UseCSMsg *recovering_use = recovering_use_wire
        ? dynamic_cast<UseCSMsg *>(recovering_use_wire) : nullptr;
    REQUIRE(recovering_use &&
                usecs_matches_except_host_and_cache(recovering_reply, *recovering_use) &&
                recovering_use->cache_endpoint_port == rebind_cache_port &&
                recovering_use->cache_protocol == CACHE_WIRE_REVISION &&
                recovering_use->cache_profile_mask == CACHE_PROFILE_P29V1,
            "recovery-window decision preserves the exact P50 handoff");
    delete recovering_use_wire;
    const P50CacheSessionFdRequestFields recovering_descriptor{
        recovering_reply.job_id, recovering_reply.assignmentEpoch(),
        recovering_reply.assignmentNonce(), CACHE_PROFILE_P29V1};
    REQUIRE(client_recovering && client_recovering->send_msg(
                P50CacheSessionFdRequestMsg(recovering_descriptor)),
            "recovery-window assignment requests descriptor before READY");
    REQUIRE(wait_file_contains(daemon_log,
                "deferred P50 C-cache control request across supervised replacement for assignment 21010",
                5000), "recovery-window descriptor waits for authenticated READY");

    const P50CacheSessionFdRequestFields descriptor_request{
        rebind_wire_job_id, rebind_assignment_epoch,
        rebind_assignment_nonce, CACHE_PROFILE_P29V1};
    REQUIRE(client_rebind && client_rebind->send_msg(
                P50CacheSessionFdRequestMsg(descriptor_request)),
            "wrapper requested its exact cache-control descriptor during replacement");
    REQUIRE(wait_file_contains(
                daemon_log,
                "deferred P50 C-cache control request across supervised replacement",
                5000),
            "daemon retained the descriptor request during supervised replacement");

    {
        std::ofstream release(replacement_release);
        release << "release\n";
    }
    P50CacheControlIdentity replacement_identity;
    const int replacement_control_fd = client_rebind
        ? client_rebind->receive_p50_cache_fd_reply(
              descriptor_request, replacement_identity,
              Clock::now() + std::chrono::seconds(15))
        : -1;
    REQUIRE(replacement_control_fd >= 0 && replacement_identity.valid(),
            "original wrapper received a valid successor control descriptor");
    if (replacement_control_fd >= 0)
        close(replacement_control_fd);
    REQUIRE(wait_file_contains(
                daemon_log,
                "rebound retained P50 assignment 21008 to successor C-cache READY lease",
                5000),
            "daemon rebound only the retained assignment's C-local READY lease");
    REQUIRE(wait_file_contains(
                daemon_log,
                "P50 C-cache control descriptor delivered for assignment 21008",
                5000),
            "daemon completed the deferred descriptor handoff");

    stable_ready_witness = read_file_contents(ready_trace);
    const std::string replacement_ready_identity =
        "READY v2 generation=" +
        std::to_string(replacement_identity.generation) + " attempt=" +
        std::to_string(replacement_identity.attempt) + " ";
    REQUIRE(count_occurrences(stable_ready_witness, "READY v2 ") == 2 &&
                stable_ready_witness.find(replacement_ready_identity) !=
                    std::string::npos,
            "returned descriptor identity names the one recorded successor READY lease");

    JobDoneMsg rebind_success(
        rebind_wire_job_id, 0,
        static_cast<uint32_t>(JobDoneMsg::FROM_SUBMITTER) |
            static_cast<uint32_t>(JobDoneMsg::P50CacheRouteObservation), 0,
        rebind_assignment_epoch, rebind_assignment_nonce,
        rebind_c_guid, rebind_tu_seq);
    REQUIRE(client_rebind && client_rebind->send_msg(rebind_success),
            "replacement-race wrapper reports success on its original assignment");
    REQUIRE(client_rebind &&
                !request_internals(client_rebind, 5000).empty(),
            "same-channel status orders the post-replacement observation");
    delete client_rebind;
    client_rebind = nullptr;

    Msg *rebind_done_wire = scheduler
        ? wait_for_type(scheduler, Msg::JOB_DONE, 5000) : nullptr;
    JobDoneMsg *rebind_done = rebind_done_wire
        ? dynamic_cast<JobDoneMsg *>(rebind_done_wire) : nullptr;
    REQUIRE(rebind_done != nullptr &&
                rebind_done->job_id == rebind_wire_job_id &&
                rebind_done->assignmentEpoch() == rebind_assignment_epoch &&
                rebind_done->assignmentNonce() == rebind_assignment_nonce &&
                rebind_done->cGuid() == rebind_c_guid &&
                rebind_done->tuSeq() == rebind_tu_seq,
            "replacement-race assignment settles once with its original identity");
    delete rebind_done_wire;

    P50CacheControlIdentity recovering_identity;
    const int recovering_fd = client_recovering
        ? client_recovering->receive_p50_cache_fd_reply(
              recovering_descriptor, recovering_identity,
              Clock::now() + std::chrono::seconds(5)) : -1;
    REQUIRE(recovering_fd >= 0 && recovering_identity.valid() &&
                recovering_identity.generation == replacement_identity.generation &&
                recovering_identity.attempt == replacement_identity.attempt,
            "recovery-window assignment receives only the successor descriptor");
    if (recovering_fd >= 0)
        close(recovering_fd);
    delete client_recovering;
    Msg *recovering_done = scheduler
        ? wait_for_type(scheduler, Msg::JOB_DONE, 5000) : nullptr;
    REQUIRE(recovering_done != nullptr,
            "recovery-window assignment settles after descriptor delivery");
    delete recovering_done;

    REQUIRE(scheduler && scheduler->send_msg(inflight_reply),
            "scheduler resolves old-lease GetCS after successor READY");
    Msg *inflight_use_wire = client_inflight
        ? wait_for_type(client_inflight, Msg::USE_CS, 5000) : nullptr;
    UseCSMsg *inflight_use = inflight_use_wire
        ? dynamic_cast<UseCSMsg *>(inflight_use_wire) : nullptr;
    REQUIRE(inflight_use &&
                usecs_matches_except_host_and_cache(inflight_reply, *inflight_use) &&
                inflight_use->hostname == inflight_reply.hostname &&
                inflight_use->port == inflight_reply.port &&
                inflight_use->cache_endpoint_port == rebind_cache_port &&
                inflight_use->cache_protocol == CACHE_WIRE_REVISION &&
                inflight_use->cache_profile_mask == CACHE_PROFILE_P29V1,
            "inflight GetCS preserves exact assignment and P50 handoff across C replacement");
    delete inflight_use_wire;
    const P50CacheSessionFdRequestFields inflight_descriptor{
        inflight_reply.job_id, inflight_reply.assignmentEpoch(),
        inflight_reply.assignmentNonce(), CACHE_PROFILE_P29V1};
    REQUIRE(client_inflight && client_inflight->send_msg(
                P50CacheSessionFdRequestMsg(inflight_descriptor)),
            "inflight assignment requests descriptor after successor READY");
    P50CacheControlIdentity inflight_identity;
    const int inflight_fd = client_inflight
        ? client_inflight->receive_p50_cache_fd_reply(
              inflight_descriptor, inflight_identity,
              Clock::now() + std::chrono::seconds(5)) : -1;
    REQUIRE(inflight_fd >= 0 && inflight_identity.valid() &&
                inflight_identity.generation == replacement_identity.generation &&
                inflight_identity.attempt == replacement_identity.attempt,
            "inflight old-lease offer obtains only the authenticated successor descriptor");
    if (inflight_fd >= 0)
        close(inflight_fd);
    delete client_inflight;
    Msg *inflight_done = scheduler
        ? wait_for_type(scheduler, Msg::JOB_DONE, 5000) : nullptr;
    REQUIRE(inflight_done != nullptr,
            "inflight replacement regression settles its assignment");
    delete inflight_done;

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
    request_c.cache_protocol = CACHE_WIRE_REVISION;
    request_c.cache_profile_mask = CACHE_ADVERTISABLE_PROFILE_MASK;
    REQUIRE(client_c && client_c->send_msg(request_c),
            "local client C requested a third assignment");

    Msg *forwarded_c_wire = scheduler ? wait_for_type(scheduler, Msg::GET_CS, 5000)
                                      : nullptr;
    GetCSMsg *forwarded_c = forwarded_c_wire
        ? dynamic_cast<GetCSMsg *>(forwarded_c_wire) : nullptr;
    REQUIRE(forwarded_c != nullptr,
            "fake scheduler received client C's forwarded GetCS");
    REQUIRE(forwarded_c &&
                forwarded_c->cache_protocol == CACHE_WIRE_REVISION &&
                forwarded_c->cache_profile_mask ==
                    CACHE_ADVERTISABLE_PROFILE_MASK &&
                forwarded_c->cache_retry_avoid_port == 0 &&
                forwarded_c->cache_retry_avoid_host.empty() &&
                forwarded_c->cache_affinity_profile_mask ==
                    CACHE_PROFILE_P29V1 &&
                forwarded_c->cache_affinity_port == rebind_f_port &&
                forwarded_c->cache_affinity_host == rebind_f_host,
            "next GetCS carries the exact post-replacement successful "
            "host/ordinary-port/profile warm hint");

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
                     remote_cache_port, CACHE_WIRE_REVISION,
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
                && client_c_use->cache_protocol == CACHE_WIRE_REVISION
                && client_c_use->cache_profile_mask == CACHE_PROFILE_ZSTD_TU,
            "S2: the remote-worker relay carries the SAME validated cache "
            "triple -- port/protocol/mask survive the actual client wire "
            "vehicle (send_msg(*msg)), not just c->usecsmsg's introspection "
            "copy");
    delete client_c_wire;

    /* Install a second assignment before C reports success, so both handoffs
       retain the same route-state generation.  C's success below advances
       that generation; the second wrapper's exact failure is therefore stale
       for affinity mutation but remains authoritative for withdrawing its
       own scheduler assignment. */
    MsgChannel *client_stale = connect_unix_bounded(socket_path, 5000);
    REQUIRE(client_stale != nullptr,
            "second remote client connected before route generation advances");
    GetCSMsg request_stale(Environments(), "stale-route-failure.cpp",
                           CompileJob::Lang_CXX, 1, "x86_64", 0,
                           std::string(), 0, 0, 0);
    request_stale.cache_protocol = CACHE_WIRE_REVISION;
    request_stale.cache_profile_mask = CACHE_ADVERTISABLE_PROFILE_MASK;
    REQUIRE(client_stale && client_stale->send_msg(request_stale),
            "second remote client requested an assignment");

    Msg *forwarded_stale_wire = scheduler
        ? wait_for_type(scheduler, Msg::GET_CS, 5000) : nullptr;
    GetCSMsg *forwarded_stale = forwarded_stale_wire
        ? dynamic_cast<GetCSMsg *>(forwarded_stale_wire) : nullptr;
    REQUIRE(forwarded_stale != nullptr,
            "fake scheduler received the second remote GetCS");
    const uint32_t stale_wire_job_id = UINT32_C(0x00005204);
    const uint64_t stale_assignment_epoch = UINT64_C(0x5200000000000004);
    const uint64_t stale_assignment_nonce = UINT64_C(0x0123456789abcdef);
    const uint64_t stale_c_guid = UINT64_C(0x52000000000000c4);
    const uint64_t stale_tu_seq = UINT64_C(38);
    const uint32_t stale_forwarded_id = forwarded_stale
        ? forwarded_stale->client_id : 0;
    UseCSMsg reply_stale(
        "x86_64", remote_f_host, remote_f_port, stale_wire_job_id, true,
        stale_forwarded_id, UINT32_C(0x00000038), stale_assignment_epoch,
        stale_assignment_nonce, remote_cache_port, CACHE_WIRE_REVISION,
        CACHE_PROFILE_ZSTD_TU);
    reply_stale.setCompileIdentity(stale_c_guid, stale_tu_seq);
    if (scheduler && forwarded_stale) {
        REQUIRE(scheduler->send_msg(reply_stale),
                "fake scheduler sent the second remote cache assignment");
    }
    delete forwarded_stale_wire;

    Msg *client_stale_wire = client_stale
        ? wait_for_type(client_stale, Msg::USE_CS, 5000) : nullptr;
    UseCSMsg *client_stale_use = client_stale_wire
        ? dynamic_cast<UseCSMsg *>(client_stale_wire) : nullptr;
    REQUIRE(client_stale_use != nullptr &&
                client_stale_use->job_id == stale_wire_job_id &&
                client_stale_use->assignmentEpoch() == stale_assignment_epoch &&
                client_stale_use->assignmentNonce() == stale_assignment_nonce &&
                client_stale_use->cGuid() == stale_c_guid &&
                client_stale_use->tuSeq() == stale_tu_seq,
            "second remote client received its exact assignment binding");
    delete client_stale_wire;

    JobDoneMsg remote_successful_observation(
        remote_wire_job_id, 0,
        static_cast<uint32_t>(JobDoneMsg::FROM_SUBMITTER) |
            static_cast<uint32_t>(JobDoneMsg::P50CacheRouteObservation), 0,
        remote_assignment_epoch, remote_assignment_nonce,
        remote_c_guid, remote_tu_seq);
    REQUIRE(client_c && client_c->send_msg(remote_successful_observation),
            "remote client C reports an exact successful cache-route observation");
    REQUIRE(client_c && !request_internals(client_c, 5000).empty(),
            "a same-channel status round trip orders the successful observation before the scheduler bounce");

    const int stale_failure_exitcode = 106;
    JobDoneMsg stale_failed_observation(
        stale_wire_job_id, stale_failure_exitcode,
        static_cast<uint32_t>(JobDoneMsg::FROM_SUBMITTER) |
            static_cast<uint32_t>(JobDoneMsg::P50CacheRouteObservation), 0,
        stale_assignment_epoch, stale_assignment_nonce,
        stale_c_guid, stale_tu_seq);
    REQUIRE(client_stale && client_stale->send_msg(stale_failed_observation),
            "second remote client reports an exact now-stale route failure");
    REQUIRE(client_stale && !request_internals(client_stale, 5000).empty(),
            "same-channel status orders the stale failure observation");

    Msg *stale_withdrawal_wire = scheduler
        ? wait_for_type(scheduler, Msg::JOB_DONE, 5000) : nullptr;
    JobDoneMsg *stale_withdrawal = stale_withdrawal_wire
        ? dynamic_cast<JobDoneMsg *>(stale_withdrawal_wire) : nullptr;
    REQUIRE(stale_withdrawal != nullptr &&
                stale_withdrawal->job_id == stale_wire_job_id &&
                stale_withdrawal->exitcode == stale_failure_exitcode &&
                stale_withdrawal->flags == JobDoneMsg::FROM_SUBMITTER &&
                stale_withdrawal->assignmentEpoch() == stale_assignment_epoch &&
                stale_withdrawal->assignmentNonce() == stale_assignment_nonce &&
                stale_withdrawal->cGuid() == stale_c_guid &&
                stale_withdrawal->tuSeq() == stale_tu_seq,
            "exact stale route failure becomes one clean scheduler withdrawal");
    delete stale_withdrawal_wire;
    delete client_stale;
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

    /* A scheduler bounce is not a C-sidecar incarnation change.  Drop the
       active fake-S connection, accept the daemon's reconnect, and submit a
       GetCS while that connection is still only a LOGIN_ATTEMPT.  ConfCS must
       re-drive it with the original capability and the warm remote-F hint;
       the exact READY trace must remain byte-identical, proving no new PID,
       lease, or C/F store identity was minted merely because S disappeared. */
    delete scheduler;
    scheduler = nullptr;
    Msg *relogin_wire = nullptr;
    scheduler = accept_login_channel(listener, 15000, &relogin_wire);
    LoginMsg *relogin = relogin_wire
        ? dynamic_cast<LoginMsg *>(relogin_wire) : nullptr;
    REQUIRE(scheduler != nullptr && relogin != nullptr,
            "iceccd reconnected to fake S after the established-session bounce");
    REQUIRE(relogin && !relogin->hasCacheAdvertisement(),
            "reconnecting Login remains canonically cache-absent before ConfCS");
    delete relogin_wire;

    MsgChannel *client_bounce = connect_unix_bounded(socket_path, 5000);
    REQUIRE(client_bounce != nullptr,
            "post-bounce client connected during the scheduler LOGIN_ATTEMPT");
    GetCSMsg request_bounce(Environments(), "s-bounce.cpp",
                            CompileJob::Lang_CXX, 1, "x86_64", 0,
                            std::string(), 0, 0, 0);
    request_bounce.cache_protocol = CACHE_WIRE_REVISION;
    request_bounce.cache_profile_mask = CACHE_ADVERTISABLE_PROFILE_MASK;
    const std::string deferred_retry_avoid_host = "198.51.100.23";
    const uint32_t deferred_retry_avoid_port = UINT32_C(54422);
    request_bounce.cache_retry_avoid_host = deferred_retry_avoid_host;
    request_bounce.cache_retry_avoid_port = deferred_retry_avoid_port;
    REQUIRE(client_bounce && client_bounce->send_msg(request_bounce),
            "post-bounce client submitted a P50-capable GetCS before ConfCS");

    const ConfCSMsg bounce_config(UINT64_C(0x5200000000000003),
                                  ConfCSMsg::Advisory);
    REQUIRE(scheduler && scheduler->send_msg(bounce_config),
            "fake S activated the replacement scheduler session");
    Msg *forwarded_bounce_wire = scheduler
        ? wait_for_type(scheduler, Msg::GET_CS, 5000) : nullptr;
    GetCSMsg *forwarded_bounce = forwarded_bounce_wire
        ? dynamic_cast<GetCSMsg *>(forwarded_bounce_wire) : nullptr;
    REQUIRE(forwarded_bounce != nullptr,
            "held post-bounce GetCS was re-driven immediately after ConfCS");
    REQUIRE(forwarded_bounce &&
                forwarded_bounce->cache_protocol == CACHE_WIRE_REVISION &&
                forwarded_bounce->cache_profile_mask ==
                    CACHE_ADVERTISABLE_PROFILE_MASK &&
                forwarded_bounce->cache_affinity_profile_mask == 0 &&
                forwarded_bounce->cache_affinity_port == 0 &&
                forwarded_bounce->cache_affinity_host.empty() &&
                forwarded_bounce->cache_retry_avoid_host ==
                    deferred_retry_avoid_host &&
                forwarded_bounce->cache_retry_avoid_port ==
                    deferred_retry_avoid_port,
            "same READY lease preserves capability and the exact retry "
            "exclusion across deferred scheduler reconnect without reviving "
            "the lower-priority warm hint");
    REQUIRE(read_file_contents(ready_trace) == stable_ready_witness,
            "S bounce preserved the exact sidecar PID, ReadyLease, and C/F store identities");

    if (scheduler && forwarded_bounce) {
        NoCSMsg no_cs_bounce(UINT32_C(0x00005205),
                             forwarded_bounce->client_id);
        REQUIRE(scheduler->send_msg(no_cs_bounce),
                "fake S resolved the post-bounce probe with NoCS");
    }
    delete forwarded_bounce_wire;
    Msg *client_bounce_wire = client_bounce
        ? wait_for_type(client_bounce, Msg::USE_CS, 5000) : nullptr;
    REQUIRE(client_bounce_wire != nullptr,
            "post-bounce probe received its bounded local fallback");
    delete client_bounce_wire;
    delete client_bounce;

    /* A held GetCS owns the exact C-sidecar READY lease observed when the
       wrapper offered P50.  Unlike the scheduler-only bounce above, replacing
       that sidecar invalidates the capability before deferred publication.
       The wrapper's failed-F exclusion narrows only a live P50 request, so it
       must be canonicalized away with the capability rather than survive as
       unversioned routing state. */
    delete scheduler;
    scheduler = nullptr;
    Msg *changed_relogin_wire = nullptr;
    scheduler = accept_login_channel(listener, 15000, &changed_relogin_wire);
    LoginMsg *changed_relogin = changed_relogin_wire
        ? dynamic_cast<LoginMsg *>(changed_relogin_wire) : nullptr;
    REQUIRE(scheduler != nullptr && changed_relogin != nullptr,
            "iceccd began another scheduler LOGIN_ATTEMPT for lease-change test");
    delete changed_relogin_wire;

    MsgChannel *client_changed_lease =
        connect_unix_bounded(socket_path, 5000);
    REQUIRE(client_changed_lease != nullptr,
            "lease-change client connected during scheduler LOGIN_ATTEMPT");
    GetCSMsg request_changed_lease(
        Environments(), "s-changed-lease.cpp", CompileJob::Lang_CXX,
        1, "x86_64", 0, std::string(), 0, 0, 0);
    request_changed_lease.cache_protocol = CACHE_WIRE_REVISION;
    request_changed_lease.cache_profile_mask =
        CACHE_ADVERTISABLE_PROFILE_MASK;
    const std::string changed_retry_avoid_host = "203.0.113.44";
    const uint32_t changed_retry_avoid_port = UINT32_C(54411);
    request_changed_lease.cache_retry_avoid_host =
        changed_retry_avoid_host;
    request_changed_lease.cache_retry_avoid_port =
        changed_retry_avoid_port;
    REQUIRE(client_changed_lease &&
                client_changed_lease->send_msg(request_changed_lease),
            "lease-change client submitted a P50 retry before ConfCS");
    REQUIRE(client_changed_lease &&
                !request_internals(client_changed_lease, 5000).empty(),
            "same-channel status orders the held GetCS before lease replacement");

    const pid_t old_sidecar = find_child_with_command(daemon_pid, argv[2]);
    REQUIRE(old_sidecar > 0,
            "current authenticated C-sidecar PID was identified exactly");
    REQUIRE(old_sidecar > 0 && kill(old_sidecar, SIGKILL) == 0,
            "current C-sidecar incarnation was killed to invalidate its lease");
    /* A replacement is intentionally not launched while S is still only a
       LOGIN_ATTEMPT.  Give the daemon several event-loop turns to observe
       child loss, then activate S well inside its 30-second deadline. */
    usleep(500 * 1000);

    const ConfCSMsg changed_lease_config(UINT64_C(0x5200000000000004),
                                         ConfCSMsg::Advisory);
    REQUIRE(scheduler && scheduler->send_msg(changed_lease_config),
            "fake S activated only after the C-sidecar lease was lost");
    Msg *forwarded_changed_wire = scheduler
        ? wait_for_type(scheduler, Msg::GET_CS, 5000) : nullptr;
    GetCSMsg *forwarded_changed = forwarded_changed_wire
        ? dynamic_cast<GetCSMsg *>(forwarded_changed_wire) : nullptr;
    REQUIRE(forwarded_changed != nullptr,
            "held changed-lease GetCS was re-driven after ConfCS");
    REQUIRE(forwarded_changed &&
                forwarded_changed->cache_protocol == 0 &&
                forwarded_changed->cache_profile_mask == 0 &&
                forwarded_changed->cache_affinity_profile_mask == 0 &&
                forwarded_changed->cache_affinity_port == 0 &&
                forwarded_changed->cache_affinity_host.empty() &&
                forwarded_changed->cache_retry_avoid_port == 0 &&
                forwarded_changed->cache_retry_avoid_host.empty(),
            "lost READY lease canonicalizes capability, warm hint, and retry exclusion away");
    if (scheduler && forwarded_changed) {
        REQUIRE(scheduler->send_msg(NoCSMsg(
                    UINT32_C(0x00005206), forwarded_changed->client_id)),
                "fake S resolved the changed-lease request with NoCS");
    }
    delete forwarded_changed_wire;
    Msg *changed_client_wire = client_changed_lease
        ? wait_for_type(client_changed_lease, Msg::USE_CS, 5000) : nullptr;
    REQUIRE(changed_client_wire != nullptr,
            "changed-lease request completed through canonical local fallback");
    delete changed_client_wire;
    delete client_changed_lease;

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
    REQUIRE(forwarded_d && forwarded_d->cache_protocol == 0
                && forwarded_d->cache_profile_mask == 0
                && forwarded_d->cache_affinity_profile_mask == 0
                && forwarded_d->cache_affinity_port == 0
                && forwarded_d->cache_affinity_host.empty()
                && forwarded_d->cache_retry_avoid_port == 0
                && forwarded_d->cache_retry_avoid_host.empty(),
            "canonical wrapper absence is a per-job downward opt-out and the "
            "prior retry exclusion cannot leak to a later request");

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
    request_b.cache_protocol = CACHE_WIRE_REVISION;
    request_b.cache_profile_mask = CACHE_ADVERTISABLE_PROFILE_MASK;
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
                         expected_cache_port, CACHE_WIRE_REVISION,
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
