/* Real-daemon Protocol-49 assignment-fence lifecycle gate.
   A fake scheduler speaks through production MsgChannel/message classes while
   an actual iceccd owns claim admission. */

#include "comm.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
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

#define REQUIRE(cond, text) do {                                      \
    if (cond) { std::fprintf(stderr, "ok       - %s\n", text); }      \
    else { std::fprintf(stderr, "FAILED   - %s\n", text); ++failures; } \
} while (0)

static int listen_port(int requested, int *actual)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<uint16_t>(requested));
    if (fd < 0 || bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0
            || listen(fd, 8) != 0) {
        if (fd >= 0) close(fd);
        return -1;
    }
    socklen_t len = sizeof(address);
    getsockname(fd, reinterpret_cast<sockaddr *>(&address), &len);
    *actual = ntohs(address.sin_port);
    return fd;
}

static MsgChannel *accept_channel(int listener, int timeout_msec)
{
    pollfd pfd { listener, POLLIN, 0 };
    if (poll(&pfd, 1, timeout_msec) <= 0) return nullptr;
    int fd = accept(listener, nullptr, nullptr);
    if (fd < 0) return nullptr;
    sockaddr_in peer {};
    peer.sin_family = AF_INET;
    return Service::createChannel(fd, reinterpret_cast<sockaddr *>(&peer), sizeof(peer));
}

static Msg *wait_type(MsgChannel *channel, Msg::Value wanted, int timeout_msec)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (channel && Clock::now() < deadline) {
        Msg *msg = channel->get_msg(1, true);
        if (msg) {
            if (*msg == wanted) return msg;
            delete msg;
        }
        if (channel->at_eof()) break;
    }
    return nullptr;
}

/* TAP99: wait for EITHER of two message types (the late-PREPARE outcome
   race -- READY when the claim's job is still live, REVOKE_RESULT
   when its END was handled first).  Returns the first match, with its
   type left for the caller to inspect, or nullptr on timeout/EOF. */
static Msg *wait_either(MsgChannel *channel, Msg::Value first,
                        Msg::Value second, int timeout_msec)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (channel && Clock::now() < deadline) {
        Msg *msg = channel->get_msg(1, true);
        if (msg) {
            if (*msg == first || *msg == second) return msg;
            delete msg;
        }
        if (channel->at_eof()) break;
    }
    return nullptr;
}

static bool no_type(MsgChannel *channel, Msg::Value unwanted, int timeout_msec)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (channel && Clock::now() < deadline) {
        Msg *msg = channel->get_msg(1, true);
        if (msg) {
            const bool found = *msg == unwanted;
            delete msg;
            if (found) return false;
        }
        if (channel->at_eof()) break;
    }
    return true;
}

static MsgChannel *connect_daemon(const std::string &path)
{
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < deadline) {
        if (access(path.c_str(), F_OK) == 0) {
            MsgChannel *channel = Service::createChannel(path);
            if (channel) return channel;
        }
        usleep(20 * 1000);
    }
    return nullptr;
}

static bool read_exact(int fd, void *buffer, size_t size)
{
    char *out = static_cast<char *>(buffer);
    while (size) {
        const ssize_t count = read(fd, out, size);
        if (count <= 0) return false;
        out += count;
        size -= static_cast<size_t>(count);
    }
    return true;
}

static bool write_exact(int fd, const void *buffer, size_t size)
{
    const char *in = static_cast<const char *>(buffer);
    while (size) {
        const ssize_t count = write(fd, in, size);
        if (count <= 0) return false;
        in += count;
        size -= static_cast<size_t>(count);
    }
    return true;
}

static int connect_unix_raw(const std::string &path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) {
        close(fd);
        return -1;
    }
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Negotiate a genuine protocol-48 C->F connection while relaying every
   production frame byte-for-byte.  This exercises the real daemon admission
   decision, rather than merely constructing a v48-shaped CompileFile on a
   channel the daemon still knows is current. */
static pid_t start_p48_unix_proxy(const std::string &daemon_path,
                                  const std::string &proxy_path)
{
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    if (listener < 0 || proxy_path.size() >= sizeof(address.sun_path)) {
        if (listener >= 0) close(listener);
        return -1;
    }
    std::memcpy(address.sun_path, proxy_path.c_str(), proxy_path.size() + 1);
    unlink(proxy_path.c_str());
    if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0
            || listen(listener, 1) != 0) {
        close(listener);
        unlink(proxy_path.c_str());
        return -1;
    }
    pid_t child = fork();
    if (child != 0) {
        close(listener);
        return child;
    }
    int downstream = accept(listener, nullptr, nullptr);
    close(listener);
    int upstream = -1;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (upstream < 0 && Clock::now() < deadline) {
        upstream = connect_unix_raw(daemon_path);
        if (upstream < 0) usleep(20 * 1000);
    }
    unsigned char down_version[4] {};
    unsigned char up_version[4] {};
    const unsigned char p48[4] { 48, 0, 0, 0 };
    if (downstream < 0 || upstream < 0
            || !read_exact(downstream, down_version, sizeof(down_version))
            || !read_exact(upstream, up_version, sizeof(up_version))
            || !write_exact(downstream, p48, sizeof(p48))
            || !write_exact(upstream, p48, sizeof(p48))
            || !read_exact(downstream, down_version, sizeof(down_version))
            || !read_exact(upstream, up_version, sizeof(up_version))
            || !write_exact(downstream, up_version, sizeof(up_version))
            || !write_exact(upstream, down_version, sizeof(down_version))) {
        if (downstream >= 0) close(downstream);
        if (upstream >= 0) close(upstream);
        _exit(2);
    }
    for (;;) {
        pollfd descriptors[2] {
            { downstream, POLLIN, 0 }, { upstream, POLLIN, 0 }
        };
        if (poll(descriptors, 2, -1) <= 0) continue;
        char bytes[8192];
        for (int index = 0; index != 2; ++index) {
            if (!(descriptors[index].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            const int source = index == 0 ? downstream : upstream;
            const int destination = index == 0 ? upstream : downstream;
            const ssize_t count = read(source, bytes, sizeof(bytes));
            if (count <= 0 || !write_exact(
                    destination, bytes, static_cast<size_t>(count))) {
                close(downstream);
                close(upstream);
                _exit(0);
            }
        }
    }
}

static bool send_claim(MsgChannel *client, uint32_t wire_id,
                       uint64_t epoch = 0, uint64_t nonce = 0,
                       const char *environment = "p49-test-environment",
                       uint64_t c_guid = 0, uint64_t tu_seq = 0)
{
    CompileJob job;
    job.setJobID(wire_id);
    job.setAssignmentIdentity(epoch, nonce);
    job.setCompileIdentity(c_guid != 0 ? c_guid
                                      : UINT64_C(0x5049000000000001),
                           tu_seq);
    job.setCompilerName("g++");
    job.setLanguage(CompileJob::Lang_CXX);
    job.setEnvironmentVersion(environment);
    job.setTargetPlatform("x86_64");
    job.setInputFile("p49-test.ii");
    job.setOutputFile("p49-test.o");
    CompileFileMsg claim(&job);
    return client->send_msg(claim);
}

static std::string request_internals(const std::string &socket_path)
{
    MsgChannel *client = connect_daemon(socket_path);
    if (!client || !client->send_msg(GetInternalStatus())) {
        delete client;
        return std::string();
    }
    Msg *wire = wait_type(client, Msg::STATUS_TEXT, 3000);
    StatusTextMsg *status = dynamic_cast<StatusTextMsg *>(wire);
    const std::string text = status ? status->text : std::string();
    delete wire;
    delete client;
    return text;
}

static bool wait_child(pid_t pid, int timeout_msec)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    int status = 0;
    while (Clock::now() < deadline) {
        if (waitpid(pid, &status, WNOHANG) == pid) return true;
        usleep(20 * 1000);
    }
    return false;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <iceccd>\n", argv[0]);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    const char *temporary_root = std::getenv("TMPDIR");
    std::string temp_template =
        std::string(temporary_root && temporary_root[0] ? temporary_root : "/tmp")
        + "/icecream-p49-daemon.XXXXXX";
    char *temp = mkdtemp(&temp_template[0]);
    if (!temp) return 2;
    const std::string work(temp);
    const std::string envdir = work + "/envs";
    const std::string logfile = work + "/iceccd.log";
    const std::string socket_path = work + "/iceccd.sock";
    if (mkdir(envdir.c_str(), 0700) != 0) {
        std::perror("mkdir daemon environment directory");
        return 2;
    }
    std::string daemon_user;
    if (getuid() == 0) {
        struct passwd *nobody = getpwnam("nobody");
        if (!nobody || nobody->pw_uid == 0 || nobody->pw_gid == 0) {
            std::fprintf(stderr, "p49daemon: a non-root nobody account is required when run as root\n");
            return 2;
        }
        if (chown(work.c_str(), nobody->pw_uid, nobody->pw_gid) != 0
                || chown(envdir.c_str(), nobody->pw_uid, nobody->pw_gid) != 0) {
            std::perror("chown daemon fixture to nobody");
            return 2;
        }
        daemon_user = "nobody";
    }
    std::fprintf(stderr, "retained work directory: %s\n", work.c_str());

    int scheduler_port = 0;
    int listener = listen_port(0, &scheduler_port);
    REQUIRE(listener >= 0 && scheduler_port > 0, "loopback scheduler port reserved");
    if (listener < 0) return 2;

    pid_t daemon_pid = fork();
    if (daemon_pid == 0) {
        char scheduler_spec[64];
        std::snprintf(scheduler_spec, sizeof(scheduler_spec), "127.0.0.1:%d",
                      scheduler_port);
        setenv("ICECC_TESTS", "1", 1);
        setenv("ICECC_TEST_SOCKET", socket_path.c_str(), 1);
        setenv("ICECC_TEST_ASSIGNMENT_TABLE_LIMIT", "8", 1);
        /* Retention below the hard admission limit: the terminal table is
           a bounded FIFO (k415), so inserts evict the oldest retained
           terminal instead of exhausting the epoch.  4 makes the eviction
           observable within this test's handful of terminals. */
        setenv("ICECC_TEST_ASSIGNMENT_TERMINAL_RETENTION", "4", 1);
        std::vector<std::string> arguments {
            argv[1], "--no-remote", "-m", "1", "-p", "10245", "-s",
            scheduler_spec, "-n", "p49-worker", "-N", "p49-daemon", "-b",
            envdir, "-l", logfile, "-v", "-v", "-v"
        };
        if (!daemon_user.empty()) {
            arguments.insert(arguments.begin() + 2, {"-u", daemon_user});
        }
        std::vector<char *> argument_pointers;
        argument_pointers.reserve(arguments.size() + 1);
        for (std::string &argument : arguments) {
            argument_pointers.push_back(const_cast<char *>(argument.c_str()));
        }
        argument_pointers.push_back(nullptr);
        execv(argv[1], argument_pointers.data());
        _exit(127);
    }
    REQUIRE(daemon_pid > 0, "real iceccd started");
    if (daemon_pid < 0) {
        close(listener);
        return 2;
    }

    MsgChannel *scheduler = accept_channel(listener, 8000);
    Msg *login = wait_type(scheduler, Msg::LOGIN, 5000);
    REQUIRE(scheduler && login, "daemon logged in through the real protocol");
    if (!scheduler || !login) {
        delete login;
        delete scheduler;
        close(listener);
        kill(daemon_pid, SIGTERM);
        if (!wait_child(daemon_pid, 5000)) {
            kill(daemon_pid, SIGKILL);
            waitpid(daemon_pid, nullptr, 0);
        }
        return 1;
    }
    LoginMsg *typed_login = dynamic_cast<LoginMsg *>(login);
    REQUIRE(typed_login && !typed_login->hasCacheAdvertisement()
                && typed_login->cache_protocol == 0
                && typed_login->cache_profile_mask == 0,
            "real daemon advertises canonical cache absence without a ready owner");
    delete login;

    const uint64_t epoch = UINT64_C(0x1234567800000049);
    REQUIRE(scheduler && scheduler->send_msg(
                ConfCSMsg(epoch, ConfCSMsg::EnforcingCompat)),
            "EnforcingCompat scheduler session activated");

    const uint32_t revoked_id = 701;
    const uint64_t revoked_nonce = UINT64_C(0xabc0000000000701);
    REQUIRE(scheduler->send_msg(
                AssignPrepareMsg(epoch, revoked_id, revoked_nonce, 3)),
            "PREPARE sent for revoke-first assignment");
    Msg *ready_wire = wait_type(scheduler, Msg::ASSIGN_READY, 3000);
    AssignReadyMsg *ready = dynamic_cast<AssignReadyMsg *>(ready_wire);
    REQUIRE(ready && ready->epoch() == epoch && ready->wire_id == revoked_id
                && ready->nonce() == revoked_nonce,
            "READY follows installed full identity");
    delete ready_wire;

    REQUIRE(scheduler->send_msg(
                AssignPrepareMsg(epoch, revoked_id, revoked_nonce, 3)),
            "duplicate exact PREPARE replayed");
    ready_wire = wait_type(scheduler, Msg::ASSIGN_READY, 3000);
    ready = dynamic_cast<AssignReadyMsg *>(ready_wire);
    REQUIRE(ready && ready->epoch() == epoch && ready->wire_id == revoked_id
                && ready->nonce() == revoked_nonce,
            "duplicate PREPARE returns the same READY without replacement");
    delete ready_wire;

    usleep(1100 * 1000);
    REQUIRE(scheduler->send_msg(
                AssignPrepareMsg(epoch, revoked_id, revoked_nonce, 3)),
            "exact PREPARE replayed after a reserved hold interval");
    ready_wire = wait_type(scheduler, Msg::ASSIGN_READY, 3000);
    ready = dynamic_cast<AssignReadyMsg *>(ready_wire);
    REQUIRE(ready && ready->wire_id == revoked_id
                && ready->nonce() == revoked_nonce,
            "RESERVED has no independent silent lease expiry");
    delete ready_wire;

    REQUIRE(scheduler->send_msg(
                RevokeBeforeStartMsg(epoch, revoked_id, revoked_nonce)),
            "ordered revoke sent before claim");
    Msg *terminal_wire = wait_type(scheduler, Msg::REVOKE_RESULT, 3000);
    RevokeResultMsg *terminal = dynamic_cast<RevokeResultMsg *>(terminal_wire);
    REQUIRE(terminal && terminal->epoch() == epoch
                && terminal->wire_id == revoked_id
                && terminal->nonce() == revoked_nonce
                && terminal->result == RevokeResultMsg::Revoked,
            "revoke-first installs rejection record before terminal");
    delete terminal_wire;

    REQUIRE(scheduler->send_msg(
                RevokeBeforeStartMsg(epoch, revoked_id, revoked_nonce)),
            "duplicate exact revoke replayed");
    terminal_wire = wait_type(scheduler, Msg::REVOKE_RESULT, 3000);
    terminal = dynamic_cast<RevokeResultMsg *>(terminal_wire);
    REQUIRE(terminal && terminal->epoch() == epoch
                && terminal->wire_id == revoked_id
                && terminal->nonce() == revoked_nonce
                && terminal->result == RevokeResultMsg::Revoked,
            "duplicate revoke returns the one retained logical outcome");
    delete terminal_wire;

    MsgChannel *late = connect_daemon(socket_path);
    REQUIRE(late && send_claim(late, revoked_id), "late legacy claim sent");
    Msg *late_end = wait_type(late, Msg::END, 3000);
    REQUIRE(late_end || (late && late->at_eof()),
            "late claim is rejected after revoked terminal");
    delete late_end;
    delete late;
    REQUIRE(no_type(scheduler, Msg::JOB_BEGIN, 400),
            "rejected late claim creates no compiler begin");

    /* Reuse the wire id with a new nonce, then replay every old control. */
    const uint64_t new_nonce = UINT64_C(0xdef0000000000701);
    REQUIRE(scheduler->send_msg(
                AssignPrepareMsg(epoch, revoked_id, new_nonce, 3)),
            "wire id reused with a new full identity");
    ready_wire = wait_type(scheduler, Msg::ASSIGN_READY, 3000);
    ready = dynamic_cast<AssignReadyMsg *>(ready_wire);
    REQUIRE(ready && ready->nonce() == new_nonce,
            "new identity installs despite retained old terminal");
    delete ready_wire;

    REQUIRE(scheduler->send_msg(
                RevokeBeforeStartMsg(epoch, revoked_id, revoked_nonce)),
            "delayed old revoke replayed after wire-id reuse");
    terminal_wire = wait_type(scheduler, Msg::REVOKE_RESULT, 3000);
    terminal = dynamic_cast<RevokeResultMsg *>(terminal_wire);
    REQUIRE(terminal && terminal->nonce() == revoked_nonce
                && terminal->result == RevokeResultMsg::Revoked,
            "old revoke receives only its retained old outcome");
    delete terminal_wire;

    MsgChannel *claim = connect_daemon(socket_path);
    REQUIRE(claim && send_claim(claim, revoked_id),
            "legacy client claims the current prepared wire-id record");
    usleep(100 * 1000);
    REQUIRE(scheduler->send_msg(
                RevokeBeforeStartMsg(epoch, revoked_id, new_nonce)),
            "revoke races after current claim consumption");
    terminal_wire = wait_type(scheduler, Msg::REVOKE_RESULT, 3000);
    terminal = dynamic_cast<RevokeResultMsg *>(terminal_wire);
    REQUIRE(terminal && terminal->nonce() == new_nonce
                && terminal->result == RevokeResultMsg::ClaimedOrLater,
            "claim-first race retains the CLAIMED_OR_LATER outcome");
    delete terminal_wire;
    delete claim;

    /* An unknown id in enforcing mode is refused before queueing or Begin. */
    MsgChannel *unknown = connect_daemon(socket_path);
    REQUIRE(unknown && send_claim(unknown, 999),
            "unknown EnforcingCompat claim sent");
    Msg *unknown_end = wait_type(unknown, Msg::END, 3000);
    REQUIRE(unknown_end || (unknown && unknown->at_eof()),
            "unknown EnforcingCompat claim is rejected boundedly");
    delete unknown_end;
    delete unknown;
    REQUIRE(no_type(scheduler, Msg::JOB_BEGIN, 400),
            "unknown claim creates no compiler begin");

    /* S50's old-C/new-F quadrant has no possible PREPARE: protocol 48 cannot
       carry the identity from UseCS into CompileFile.  EnforcingCompat must
       admit that exact negotiated legacy connection while retaining the
       fail-closed current-peer check immediately above. */
    const std::string p48_proxy_path = work + "/p48-client.sock";
    pid_t p48_proxy = start_p48_unix_proxy(socket_path, p48_proxy_path);
    MsgChannel *legacy_unknown = connect_daemon(p48_proxy_path);
    REQUIRE(p48_proxy > 0 && legacy_unknown && legacy_unknown->protocol == 48,
            "legacy claim connection genuinely negotiated protocol 48");
    const uint32_t legacy_unknown_id = 1701;
    REQUIRE(legacy_unknown && send_claim(legacy_unknown, legacy_unknown_id),
            "unprepared negotiated-legacy EnforcingCompat claim sent");
    Msg *legacy_begin = wait_type(scheduler, Msg::JOB_BEGIN, 3000);
    JobBeginMsg *legacy_begin_typed =
        dynamic_cast<JobBeginMsg *>(legacy_begin);
    REQUIRE(legacy_begin_typed
                && legacy_begin_typed->job_id == legacy_unknown_id,
            "EnforcingCompat admits only the genuinely old unprepared claim");
    delete legacy_begin;
    delete legacy_unknown;
    REQUIRE(wait_child(p48_proxy, 3000),
            "protocol-48 claim relay stopped cleanly");
    unlink(p48_proxy_path.c_str());

    const std::string p48_second_path = work + "/p48-client-second.sock";
    pid_t p48_second_proxy =
        start_p48_unix_proxy(socket_path, p48_second_path);
    MsgChannel *legacy_second = connect_daemon(p48_second_path);
    REQUIRE(p48_second_proxy > 0 && legacy_second
                && legacy_second->protocol == 48,
            "second legacy claim genuinely negotiated protocol 48");
    const uint32_t legacy_second_id = 1702;
    REQUIRE(legacy_second && send_claim(legacy_second, legacy_second_id),
            "second unprepared negotiated-legacy claim sent");
    legacy_begin = wait_type(scheduler, Msg::JOB_BEGIN, 3000);
    legacy_begin_typed = dynamic_cast<JobBeginMsg *>(legacy_begin);
    REQUIRE(legacy_begin_typed
                && legacy_begin_typed->job_id == legacy_second_id,
            "EnforcingCompat admits a successive old-client assignment");
    delete legacy_begin;
    delete legacy_second;
    REQUIRE(wait_child(p48_second_proxy, 3000),
            "second protocol-48 claim relay stopped cleanly");
    unlink(p48_second_path.c_str());

    const uint32_t loss_id = 888;
    const uint64_t loss_nonce = UINT64_C(0x9990000000000888);
    REQUIRE(scheduler->send_msg(
                AssignPrepareMsg(epoch, loss_id, loss_nonce, 3)),
            "live assignment installed before scheduler-session loss");
    ready_wire = wait_type(scheduler, Msg::ASSIGN_READY, 3000);
    ready = dynamic_cast<AssignReadyMsg *>(ready_wire);
    REQUIRE(ready && ready->wire_id == loss_id && ready->nonce() == loss_nonce,
            "session-loss discriminator reached installed state");
    delete ready_wire;

    REQUIRE(scheduler->send_msg(AssignPrepareMsg(
                epoch, loss_id, loss_nonce ^ UINT64_C(0x55), 3)),
            "conflicting live same-epoch wire PREPARE delivered");
    REQUIRE(no_type(scheduler, Msg::ASSIGN_READY, 500),
            "conflicting PREPARE never replaces ownership or emits READY");
    REQUIRE(scheduler->at_eof(),
            "conflicting live PREPARE terminates the scheduler session");
    delete scheduler;
    usleep(500 * 1000);
    const std::string after_loss = request_internals(socket_path);
    const std::string retained_epoch =
        "Assignment fence: mode=2 epoch=" + std::to_string(epoch)
        + " live=0 retained=3";
    REQUIRE(after_loss.find(retained_epoch) != std::string::npos,
            "scheduler-session loss closes records for the full epoch");

    scheduler = accept_channel(listener, 8000);
    login = wait_type(scheduler, Msg::LOGIN, 5000);
    REQUIRE(scheduler && login, "daemon reconnects after scheduler loss");
    typed_login = dynamic_cast<LoginMsg *>(login);
    REQUIRE(typed_login && !typed_login->hasCacheAdvertisement()
                && typed_login->cache_protocol == 0
                && typed_login->cache_profile_mask == 0,
            "daemon reconnect retains the canonical absent snapshot");
    delete login;
    REQUIRE(scheduler && scheduler->send_msg(
                ConfCSMsg(epoch, ConfCSMsg::EnforcingCompat)),
            "same epoch and immutable policy reconnect");

    MsgChannel *after_loss_claim = connect_daemon(socket_path);
    REQUIRE(after_loss_claim && send_claim(after_loss_claim, loss_id),
            "delayed claim from the lost session sent after reconnect");
    Msg *after_loss_end = wait_type(after_loss_claim, Msg::END, 3000);
    REQUIRE(after_loss_end || (after_loss_claim && after_loss_claim->at_eof()),
            "epoch-lifetime close record rejects delayed claim");
    delete after_loss_end;
    delete after_loss_claim;

    REQUIRE(scheduler->send_msg(AssignPrepareMsg(0, 889, 1, 3)),
            "zero-epoch control closes the retained reconnect");
    no_type(scheduler, Msg::STATUS_TEXT, 1000);
    REQUIRE(scheduler->at_eof(),
            "reserved zero identity terminates the scheduler session");
    delete scheduler;
    scheduler = nullptr;

    scheduler = accept_channel(listener, 8000);
    login = wait_type(scheduler, Msg::LOGIN, 5000);
    REQUIRE(scheduler && login, "daemon reconnects for same-epoch mode probe");
    delete login;
    REQUIRE(scheduler && scheduler->send_msg(
                ConfCSMsg(epoch, ConfCSMsg::Legacy)),
            "changed mode delivered for the live epoch");
    no_type(scheduler, Msg::STATUS_TEXT, 1000);
    REQUIRE(scheduler->at_eof(),
            "same-epoch mode mismatch is deterministically refused");
    delete scheduler;
    scheduler = nullptr;

    scheduler = accept_channel(listener, 8000);
    login = wait_type(scheduler, Msg::LOGIN, 5000);
    REQUIRE(scheduler && login,
            "daemon reconnects with the immutable enforcing mode");
    delete login;
    REQUIRE(scheduler && scheduler->send_msg(
                ConfCSMsg(epoch, ConfCSMsg::EnforcingCompat)),
            "same-epoch enforcing mode rebinds retained state");

    /* Three terminals already exist.  Fill past the four-entry test
       RETENTION bound (the hard admission limit stays 8): the terminal
       table is a bounded FIFO (k415), so every insert beyond the bound
       evicts the OLDEST retained terminal instead of exhausting the
       epoch.  A late duplicate for an evicted identity re-derives its
       own deterministic outcome (Revoked, since no live record exists)
       rather than replaying a row that was evicted. */
    for (uint32_t i = 0; i != 5; ++i) {
        const uint32_t id = 1000 + i;
        const uint64_t nonce = UINT64_C(0x7000000000000000) + i;
        REQUIRE(scheduler->send_msg(RevokeBeforeStartMsg(epoch, id, nonce)),
                "bounded terminal-table slot requested");
        terminal_wire = wait_type(scheduler, Msg::REVOKE_RESULT, 3000);
        terminal = dynamic_cast<RevokeResultMsg *>(terminal_wire);
        REQUIRE(terminal && terminal->wire_id == id
                    && terminal->nonce() == nonce
                    && terminal->result == RevokeResultMsg::Revoked,
                "bounded terminal-table slot retained deterministically");
        delete terminal_wire;
    }
    usleep(300 * 1000);
    const std::string bounded = request_internals(socket_path);
    REQUIRE(bounded.find("retained=4") != std::string::npos
                && bounded.find("exhausted=0") != std::string::npos,
            "terminal retention stays bounded without exhaustion");

    /* The identity that must have been EVICTED by the five inserts above
       (the first revoked terminal): its duplicate is answered with a
       fresh deterministic Revoked, never a replay of the evicted row,
       and the channel stays open the whole time. */
    REQUIRE(scheduler->send_msg(
                RevokeBeforeStartMsg(epoch, 701, revoked_nonce)),
            "duplicate revoke for the evicted identity delivered");
    terminal_wire = wait_type(scheduler, Msg::REVOKE_RESULT, 3000);
    terminal = dynamic_cast<RevokeResultMsg *>(terminal_wire);
    REQUIRE(terminal && terminal->wire_id == 701
                && terminal->nonce() == revoked_nonce
                && terminal->result == RevokeResultMsg::Revoked,
            "evicted identity re-derives its deterministic outcome");
    delete terminal_wire;
    REQUIRE(!scheduler->at_eof(),
            "terminal-table retention never closes the scheduler session");

    /* Admission through a full-but-bounded table: a fresh PREPARE evicts
       as needed and reaches READY instead of being refused. */
    const uint32_t fresh_id = 1005;
    const uint64_t fresh_nonce = UINT64_C(0x7000000000000005);
    REQUIRE(scheduler->send_msg(
                AssignPrepareMsg(epoch, fresh_id, fresh_nonce, 3)),
            "fresh PREPARE delivered past the retention bound");
    ready_wire = wait_type(scheduler, Msg::ASSIGN_READY, 3000);
    ready = dynamic_cast<AssignReadyMsg *>(ready_wire);
    REQUIRE(ready && ready->wire_id == fresh_id
                && ready->nonce() == fresh_nonce,
            "admission still works past the terminal retention bound");
    delete ready_wire;

    /* Scheduler-session loss with a full-but-bounded table, then the
       same-epoch EnforcingCompat re-login: the k415 flap shape.  The
       ConfCS must be ACCEPTED (an unchanged epoch+fence is not a
       configuration change, and a full table is a bounded service
       condition), the session must activate, and a further assignment
       must still work. */
    delete scheduler;
    scheduler = nullptr;
    scheduler = accept_channel(listener, 8000);
    login = wait_type(scheduler, Msg::LOGIN, 5000);
    REQUIRE(scheduler && login, "daemon reconnects with a bounded table");
    delete login;
    REQUIRE(scheduler && scheduler->send_msg(
                ConfCSMsg(epoch, ConfCSMsg::EnforcingCompat)),
            "same epoch and mode delivered with a bounded full table");
    REQUIRE(!scheduler->at_eof(),
            "same-epoch ConfCS is not refused while the table is full");
    const uint32_t reload_id = 1006;
    const uint64_t reload_nonce = UINT64_C(0x7000000000000006);
    REQUIRE(scheduler->send_msg(
                AssignPrepareMsg(epoch, reload_id, reload_nonce, 3)),
            "post-reconnect PREPARE delivered");
    ready_wire = wait_type(scheduler, Msg::ASSIGN_READY, 3000);
    ready = dynamic_cast<AssignReadyMsg *>(ready_wire);
    REQUIRE(ready && ready->wire_id == reload_id
                && ready->nonce() == reload_nonce,
            "post-reconnect admission works on the same bounded epoch");
    delete ready_wire;
    /* Close the fresh live rows so the reopened-epoch assertions below
       start from a clean live set. */
    REQUIRE(scheduler->send_msg(RevokeBeforeStartMsg(epoch, fresh_id, fresh_nonce)),
            "fresh live row revoked before epoch reopen");
    terminal_wire = wait_type(scheduler, Msg::REVOKE_RESULT, 3000);
    delete terminal_wire;
    REQUIRE(scheduler->send_msg(RevokeBeforeStartMsg(epoch, reload_id, reload_nonce)),
            "reloaded live row revoked before epoch reopen");
    terminal_wire = wait_type(scheduler, Msg::REVOKE_RESULT, 3000);
    delete terminal_wire;

    const uint64_t reopened_epoch = epoch + 1;
    delete scheduler;
    scheduler = nullptr;
    scheduler = accept_channel(listener, 8000);
    login = wait_type(scheduler, Msg::LOGIN, 5000);
    REQUIRE(scheduler && login, "daemon reconnects for a fresh epoch");
    delete login;
    REQUIRE(scheduler && scheduler->send_msg(
                ConfCSMsg(reopened_epoch, ConfCSMsg::EnforcingCompat)),
            "fresh EnforcingCompat epoch resets the bounded table");

    /* Wire id zero is rejected before any admission check, independent of
       fence mode; this is the only place that behavior is exercised, so it
       is kept here rather than duplicated under every mode. */
    MsgChannel *zero_claim = connect_daemon(socket_path);
    REQUIRE(zero_claim && send_claim(zero_claim, 0),
            "remote claim with wholly absent identity sent");
    Msg *zero_end = wait_type(zero_claim, Msg::END, 3000);
    REQUIRE(zero_end || (zero_claim && zero_claim->at_eof()),
            "remote zero-wire claim is rejected before admission");
    delete zero_end;
    delete zero_claim;
    REQUIRE(no_type(scheduler, Msg::JOB_BEGIN, 400),
            "remote zero-wire rejection emits no JobBegin(0)");
    const std::string after_zero_claim = request_internals(socket_path);
    const std::string empty_reopened_epoch =
        "Assignment fence: mode=2 epoch=" + std::to_string(reopened_epoch)
        + " live=0 retained=0 closed=0";
    REQUIRE(after_zero_claim.find(empty_reopened_epoch) != std::string::npos,
            "remote zero-wire rejection creates no live assignment entry");

    /* REVOKE may legitimately win a race against a delayed PREPARE that
       never arrives in time, or arrives after the identity is already
       cancelled; both outcomes must retain exactly the revoked terminal,
       never READY, and a later nonce-less claim for the same wire id must
       be rejected the same way an already-claimed id is (see the "late
       claim is rejected after revoked terminal" case above). */
    const uint32_t cancel_id = 1202;
    const uint64_t cancel_nonce = UINT64_C(0x8100000000001202);
    REQUIRE(scheduler->send_msg(RevokeBeforeStartMsg(
                reopened_epoch, cancel_id, cancel_nonce)),
            "REVOKE may win before delayed PREPARE is consumed");
    terminal_wire = wait_type(scheduler, Msg::REVOKE_RESULT, 3000);
    terminal = dynamic_cast<RevokeResultMsg *>(terminal_wire);
    REQUIRE(terminal && terminal->result == RevokeResultMsg::Revoked,
            "revoke-first race retains the revoked outcome");
    delete terminal_wire;
    REQUIRE(scheduler->send_msg(AssignPrepareMsg(
                reopened_epoch, cancel_id, cancel_nonce, 9)),
            "delayed PREPARE arrives after cancellation");
    terminal_wire = wait_type(scheduler, Msg::REVOKE_RESULT, 3000);
    terminal = dynamic_cast<RevokeResultMsg *>(terminal_wire);
    REQUIRE(terminal && terminal->result == RevokeResultMsg::Revoked,
            "delayed PREPARE replays terminal instead of READY");
    delete terminal_wire;
    REQUIRE(no_type(scheduler, Msg::ASSIGN_READY, 400),
            "cancelled identity can never publish READY");

    MsgChannel *cancelled_claim = connect_daemon(socket_path);
    REQUIRE(cancelled_claim && send_claim(cancelled_claim, cancel_id),
            "late claim for cancelled identity sent");
    Msg *cancelled_end = wait_type(cancelled_claim, Msg::END, 3000);
    REQUIRE(cancelled_end || (cancelled_claim && cancelled_claim->at_eof()),
            "closed identity rejects its late nonce-less claim");
    delete cancelled_end;
    delete cancelled_claim;

    REQUIRE(scheduler->send_msg(AssignPrepareMsg(0, 1300, 1, 9)),
            "session closed at the reserved-zero boundary");
    no_type(scheduler, Msg::STATUS_TEXT, 1000);
    REQUIRE(scheduler->at_eof(), "zero-identity control reaches reconnect boundary");
    delete scheduler;
    scheduler = nullptr;

    scheduler = accept_channel(listener, 8000);
    login = wait_type(scheduler, Msg::LOGIN, 5000);
    REQUIRE(scheduler && login, "daemon reconnects for retired-epoch probe");
    delete login;
    REQUIRE(scheduler && scheduler->send_msg(
                ConfCSMsg(epoch, ConfCSMsg::EnforcingCompat)),
            "cleared prior epoch is offered again");
    no_type(scheduler, Msg::STATUS_TEXT, 1000);
    REQUIRE(scheduler->at_eof(),
            "clear-and-replace retires and refuses the prior epoch");
    delete scheduler;
    scheduler = nullptr;

    scheduler = accept_channel(listener, 8000);
    login = wait_type(scheduler, Msg::LOGIN, 5000);
    REQUIRE(scheduler && login, "daemon reconnects for STRICT_NONCE probe");
    delete login;
    const uint64_t strict_epoch = reopened_epoch + 1;
    REQUIRE(scheduler && scheduler->send_msg(
                ConfCSMsg(strict_epoch, ConfCSMsg::StrictNonce)),
            "explicit STRICT_NONCE configuration activates on P50");

    auto prepare_strict = [&](uint32_t id, uint64_t nonce) {
        if (!scheduler->send_msg(AssignPrepareMsg(
                strict_epoch, id, nonce, 17))) return false;
        Msg *wire = wait_type(scheduler, Msg::ASSIGN_READY, 3000);
        AssignReadyMsg *strict_ready = dynamic_cast<AssignReadyMsg *>(wire);
        const bool exact = strict_ready && strict_ready->epoch() == strict_epoch
            && strict_ready->wire_id == id && strict_ready->nonce() == nonce;
        delete wire;
        return exact;
    };
    auto rejected_claim = [&](uint32_t id, uint64_t claim_epoch,
                              uint64_t claim_nonce, const char *label) {
        MsgChannel *client = connect_daemon(socket_path);
        const bool sent = client && send_claim(
            client, id, claim_epoch, claim_nonce);
        Msg *end = sent ? wait_type(client, Msg::END, 3000) : nullptr;
        const bool rejected = sent && (end || (client && client->at_eof()));
        delete end;
        delete client;
        REQUIRE(rejected, label);
        REQUIRE(no_type(scheduler, Msg::JOB_BEGIN, 300),
                "rejected strict claim creates no JobBegin residue");
        return rejected;
    };
    auto admitted_claim = [&](uint32_t id, uint64_t nonce,
                              const char *label) {
        MsgChannel *client = connect_daemon(socket_path);
        const bool sent = client && send_claim(
            client, id, strict_epoch, nonce);
        Msg *begin = sent ? wait_type(scheduler, Msg::JOB_BEGIN, 3000) : nullptr;
        JobBeginMsg *typed = dynamic_cast<JobBeginMsg *>(begin);
        const bool admitted = typed && typed->job_id == id;
        delete begin;
        delete client;
        REQUIRE(admitted, label);
        return admitted;
    };

    const uint32_t absent_id = 1401;
    const uint64_t absent_nonce = UINT64_C(0x9200000000001401);
    REQUIRE(prepare_strict(absent_id, absent_nonce),
            "strict assignment installed for absent-identity probe");
    rejected_claim(absent_id, 0, 0,
                   "STRICT_NONCE rejects nonce-less remote claim");
    admitted_claim(absent_id, absent_nonce,
                   "exact claim is admitted after absent rejection (no residue)");

    const uint32_t nonce_id = 1402;
    const uint64_t nonce = UINT64_C(0x9200000000001402);
    REQUIRE(prepare_strict(nonce_id, nonce),
            "strict assignment installed for stale-nonce probe");
    rejected_claim(nonce_id, strict_epoch, nonce ^ 1,
                   "STRICT_NONCE rejects stale nonce");
    admitted_claim(nonce_id, nonce,
                   "exact claim is admitted after stale nonce (no residue)");

    const uint32_t epoch_id = 1403;
    const uint64_t epoch_nonce = UINT64_C(0x9200000000001403);
    REQUIRE(prepare_strict(epoch_id, epoch_nonce),
            "strict assignment installed for stale-epoch probe");
    rejected_claim(epoch_id, strict_epoch - 1, epoch_nonce,
                   "STRICT_NONCE rejects stale epoch");
    admitted_claim(epoch_id, epoch_nonce,
                   "exact claim is admitted after stale epoch (no residue)");

    const uint32_t wire_id = 1404;
    const uint64_t wire_nonce = UINT64_C(0x9200000000001404);
    REQUIRE(prepare_strict(wire_id, wire_nonce),
            "strict assignment installed for stale-wire probe");
    rejected_claim(wire_id + 100, strict_epoch, wire_nonce,
                   "STRICT_NONCE rejects stale wire id");
    admitted_claim(wire_id, wire_nonce,
                   "exact claim is admitted after stale wire id (no residue)");

    /* Exercise the real submitter-daemon relay, not only the codec unit. */
    MsgChannel *relay_client = connect_daemon(socket_path);
    GetCSMsg relay_request(
        Environments { std::make_pair(std::string("x86_64"),
                                      std::string("relay-env")) },
        "relay.cpp", CompileJob::Lang_CXX, 1, "x86_64", 0, "",
        MIN_PROTOCOL_VERSION, 0, 0);
    REQUIRE(relay_client && relay_client->send_msg(relay_request),
            "real-daemon relay GetCS sent in strict mode");
    Msg *relay_get_wire = wait_type(scheduler, Msg::GET_CS, 3000);
    GetCSMsg *relay_get = dynamic_cast<GetCSMsg *>(relay_get_wire);
    const uint32_t relay_client_id = relay_get ? relay_get->client_id : 0;
    const uint32_t relay_wire_id = 1490;
    const uint64_t relay_nonce = UINT64_C(0x9200000000001490);
    REQUIRE(relay_get && scheduler->send_msg(UseCSMsg(
                "x86_64", "127.0.0.2", 10246, relay_wire_id, true,
                relay_client_id, 0, strict_epoch, relay_nonce)),
            "full strict UseCS delivered to the real submitter daemon");
    delete relay_get_wire;
    Msg *relayed_wire = wait_type(relay_client, Msg::USE_CS, 3000);
    UseCSMsg *relayed = dynamic_cast<UseCSMsg *>(relayed_wire);
    REQUIRE(relayed && relayed->job_id == relay_wire_id
                && relayed->assignmentEpoch() == strict_epoch
                && relayed->assignmentNonce() == relay_nonce,
            "real submitter daemon preserves full identity through relay");
    delete relayed_wire;
    delete relay_client;

    /* A local NoCS decision never uses a fulfillment daemon and therefore
       remains exempt even while the operator-selected fleet mode is strict. */
    MsgChannel *local = connect_daemon(socket_path);
    Environments local_envs;
    local_envs.push_back(std::make_pair(std::string("x86_64"),
                                        std::string("local-env")));
    GetCSMsg local_request(local_envs, "local.cpp", CompileJob::Lang_CXX,
                           1, "x86_64", 0, "", MIN_PROTOCOL_VERSION,
                           0, 0);
    REQUIRE(local && local->send_msg(local_request),
            "local-exemption GetCS sent in strict mode");
    Msg *get_wire = wait_type(scheduler, Msg::GET_CS, 3000);
    GetCSMsg *get = dynamic_cast<GetCSMsg *>(get_wire);
    const uint32_t local_client_id = get ? get->client_id : 0;
    REQUIRE(get && scheduler->send_msg(NoCSMsg(
                1501, local_client_id, 0, 0, strict_epoch, 0)),
            "scheduler selects local NoCS in strict mode");
    delete get_wire;
    Msg *use_wire = wait_type(local, Msg::USE_CS, 3000);
    UseCSMsg *local_use = dynamic_cast<UseCSMsg *>(use_wire);
    REQUIRE(local_use && !local_use->hasAssignmentIdentity(),
            "local decision carries wholly absent assignment identity");
    const bool local_sent = local_use
        && local_use->cGuid() == strict_epoch
        && send_claim(local, local_use->job_id,
                      local_use->assignmentEpoch(),
                      local_use->assignmentNonce(), "__client",
                      local_use->cGuid(), local_use->tuSeq());
    delete use_wire;
    Msg *local_begin = local_sent
        ? wait_type(scheduler, Msg::JOB_BEGIN, 3000) : nullptr;
    JobBeginMsg *local_begin_typed = dynamic_cast<JobBeginMsg *>(local_begin);
    REQUIRE(local_begin_typed && local_begin_typed->job_id == 1501,
            "strict mode leaves local CLIENTWORK admission exempt");
    delete local_begin;
    delete local;

    /* TAP99, the READY arm (the k310 agent's strace finding, anchored on
       the nonce bytes): a claim whose job is STILL LIVE when its late
       PREPARE arrives must answer ASSIGN_READY with the full identity.
       The old racy case above could not pin this arm because its
       fixture job dies immediately (no toolchain env) and its END won
       the ordering ~25% under load; this case keeps the claim channel
       OPEN -- no client JobDone on it -- so the job stays live and the
       READY arm is exercised deterministically.  This is a TEST race,
       not a daemon bug: the daemon's answers are correct in both
       orders. */
    {
        const uint32_t held_id = 1451;
        const uint64_t held_nonce = UINT64_C(0x9200000000001451);
        REQUIRE(prepare_strict(held_id, held_nonce),
                "strict assignment installed for the held-open probe");
        MsgChannel *held_claim = connect_daemon(socket_path);
        REQUIRE(held_claim && send_claim(
                    held_claim, held_id, strict_epoch, held_nonce),
                "held-open strict claim sent");
        Msg *held_begin = wait_type(scheduler, Msg::JOB_BEGIN, 3000);
        JobBeginMsg *held_begin_typed = dynamic_cast<JobBeginMsg *>(held_begin);
        REQUIRE(held_begin_typed
                    && held_begin_typed->job_id == held_id,
                "held-open claim reaches admission");
        delete held_begin;

        /* The PREPARE arrives while the job is ALIVE (admitted, not yet
           done): the buffered READY must surface WITHOUT any further
           event on this channel -- no JobDone, no next claim, nothing.
           RED at c8bb615f under load: the READY strands in the read
           buffer until an unrelated wakeup ~4 s later; the 3 s wait
           times out. */
        REQUIRE(scheduler->send_msg(AssignPrepareMsg(
                    strict_epoch, held_id, held_nonce, 17)),
                "late PREPARE sent while the claim's job is held open");
        Msg *held_ready_wire = wait_type(scheduler, Msg::ASSIGN_READY, 3000);
        AssignReadyMsg *held_ready =
            dynamic_cast<AssignReadyMsg *>(held_ready_wire);
        REQUIRE(held_ready && held_ready->epoch() == strict_epoch
                    && held_ready->wire_id == held_id
                    && held_ready->nonce() == held_nonce,
                "buffered late PREPARE surfaces READY without an unrelated event");
        delete held_ready_wire;

        /* The explicit after-JobDone case: complete the held job, then
           REVOKE the identity -- a CONSUMED claim must resolve
           ClaimedOrLater, never regress to reserved.  (The JobDone is
           what a real client sends on its channel; the REVOKE that
           follows is the scheduler-side loss path for the same id.) */
        REQUIRE(held_claim->send_msg(JobDoneMsg(held_id, 0)),
                "held-open job completes with an explicit JobDone");
        REQUIRE(scheduler->send_msg(RevokeBeforeStartMsg(
                    strict_epoch, held_id, held_nonce)),
                "REVOKE sent for the JobDone-consumed identity");
        Msg *held_revoke_wire = wait_type(scheduler, Msg::REVOKE_RESULT, 3000);
        RevokeResultMsg *held_revoke =
            dynamic_cast<RevokeResultMsg *>(held_revoke_wire);
        REQUIRE(held_revoke
                    && held_revoke->result == RevokeResultMsg::ClaimedOrLater,
                "revoke after JobDone resolves ClaimedOrLater, never reserved");
        delete held_revoke_wire;
        Msg *held_end = wait_type(held_claim, Msg::END, 3000);
        REQUIRE(held_end || (held_claim && held_claim->at_eof()),
                "held-open claim channel closes after its JobDone");
        delete held_end;
        delete held_claim;
    }

    /* TAP99, the REPLAY arm, deterministic (forced ordering, no
       stress): the documented semantics of the one immutable terminal
       (doc/p49-assignment-fence.md) -- an EXACT late PREPARE for an
       identity whose claim's END was already handled replays
       REVOKE_RESULT(ClaimedOrLater) and NEVER emits ASSIGN_READY.
       This is the outcome the old racy case hit under load when its
       fixture job's END won the ordering, and it is CORRECT daemon
       behaviour, not a bug: the claim was consumed. */
    {
        const uint32_t ended_id = 1452;
        const uint64_t ended_nonce = UINT64_C(0x9200000000001452);
        REQUIRE(prepare_strict(ended_id, ended_nonce),
                "strict assignment installed for the END-first replay probe");
        MsgChannel *ended_claim = connect_daemon(socket_path);
        REQUIRE(ended_claim && send_claim(
                    ended_claim, ended_id, strict_epoch, ended_nonce),
                "END-first claim sent");
        Msg *ended_begin = wait_type(scheduler, Msg::JOB_BEGIN, 3000);
        JobBeginMsg *ended_begin_typed =
            dynamic_cast<JobBeginMsg *>(ended_begin);
        REQUIRE(ended_begin_typed
                    && ended_begin_typed->job_id == ended_id,
                "END-first claim reaches admission");
        delete ended_begin;
        /* Force the ordering: wait for the claim's OWN END first (the
           fixture job fails fast -- no toolchain env), so the terminal
           is retained BEFORE the late PREPARE lands. */
        Msg *claim_end = wait_type(ended_claim, Msg::END, 3000);
        REQUIRE(claim_end || (ended_claim && ended_claim->at_eof()),
                "END-first claim's job finished before the late PREPARE");
        delete claim_end;
        REQUIRE(scheduler->send_msg(AssignPrepareMsg(
                    strict_epoch, ended_id, ended_nonce, 17)),
                "EXACT late PREPARE sent after the claim's END was handled");
        Msg *replay_wire = wait_either(
            scheduler, Msg::REVOKE_RESULT, Msg::ASSIGN_READY, 3000);
        REQUIRE(replay_wire, "late PREPARE after END produced no outcome");
        REQUIRE(*replay_wire == Msg::REVOKE_RESULT,
                "late PREPARE after END answered ASSIGN_READY (must replay the terminal)");
        RevokeResultMsg *replay = dynamic_cast<RevokeResultMsg *>(replay_wire);
        REQUIRE(replay && replay->result == RevokeResultMsg::ClaimedOrLater,
                "END-first exact replay is ClaimedOrLater");
        delete replay_wire;
        delete ended_claim;
        REQUIRE(no_type(scheduler, Msg::ASSIGN_READY, 400),
                "consumed identity never publishes READY afterwards");
    }

    /* A fresh session demonstrates EnforcingCompat's intentional dual
       admission: nonce-less legacy claims and exact P50 claims. */
    delete scheduler;
    scheduler = nullptr;
    scheduler = accept_channel(listener, 8000);
    login = wait_type(scheduler, Msg::LOGIN, 5000);
    REQUIRE(scheduler && login,
            "daemon reconnects for EnforcingCompat dual-admission probe");
    delete login;
    const uint64_t compat_epoch = strict_epoch + 1;
    REQUIRE(scheduler && scheduler->send_msg(
                ConfCSMsg(compat_epoch, ConfCSMsg::EnforcingCompat)),
            "fresh EnforcingCompat epoch activated");
    const uint32_t compat_legacy_id = 1601;
    const uint64_t compat_legacy_nonce = UINT64_C(0x9300000000001601);
    REQUIRE(scheduler->send_msg(AssignPrepareMsg(
                compat_epoch, compat_legacy_id, compat_legacy_nonce, 19)),
            "compat assignment prepared for nonce-less claim");
    delete wait_type(scheduler, Msg::ASSIGN_READY, 3000);
    MsgChannel *compat_legacy = connect_daemon(socket_path);
    REQUIRE(compat_legacy && send_claim(compat_legacy, compat_legacy_id),
            "EnforcingCompat nonce-less claim sent");
    Msg *compat_begin = wait_type(scheduler, Msg::JOB_BEGIN, 3000);
    JobBeginMsg *compat_begin_typed = dynamic_cast<JobBeginMsg *>(compat_begin);
    REQUIRE(compat_begin_typed
                && compat_begin_typed->job_id == compat_legacy_id,
            "EnforcingCompat admits prepared nonce-less claim");
    delete compat_begin;
    delete compat_legacy;

    const uint32_t compat_full_id = 1602;
    const uint64_t compat_full_nonce = UINT64_C(0x9300000000001602);
    REQUIRE(scheduler->send_msg(AssignPrepareMsg(
                compat_epoch, compat_full_id, compat_full_nonce, 19)),
            "compat assignment prepared for full claim");
    delete wait_type(scheduler, Msg::ASSIGN_READY, 3000);
    MsgChannel *compat_full = connect_daemon(socket_path);
    REQUIRE(compat_full && send_claim(
                compat_full, compat_full_id, compat_epoch, compat_full_nonce),
            "EnforcingCompat full claim sent");
    compat_begin = wait_type(scheduler, Msg::JOB_BEGIN, 3000);
    compat_begin_typed = dynamic_cast<JobBeginMsg *>(compat_begin);
    REQUIRE(compat_begin_typed && compat_begin_typed->job_id == compat_full_id,
            "EnforcingCompat admits exact nonce-bearing claim");
    delete compat_begin;
    delete compat_full;
    delete scheduler;
    scheduler = nullptr;

    close(listener);
    listener = -1;
    kill(daemon_pid, SIGTERM);
    if (!wait_child(daemon_pid, 5000)) {
        kill(daemon_pid, SIGKILL);
        waitpid(daemon_pid, nullptr, 0);
        REQUIRE(false, "daemon stopped cleanly");
    } else {
        REQUIRE(true, "daemon stopped cleanly");
    }

    std::fprintf(stderr, "%s: %d failure(s)\n",
                 failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
