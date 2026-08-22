/* Real-daemon Protocol-49 assignment-fence lifecycle gate.
   A fake scheduler speaks through production MsgChannel/message classes while
   an actual iceccd owns claim admission. */

#include "comm.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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

static bool send_claim(MsgChannel *client, uint32_t wire_id)
{
    CompileJob job;
    job.setJobID(wire_id);
    job.setCompilerName("g++");
    job.setLanguage(CompileJob::Lang_CXX);
    job.setEnvironmentVersion("p49-test-environment");
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
    char temp_template[] = "/tmp/icecream-p49-daemon.XXXXXX";
    char *temp = mkdtemp(temp_template);
    if (!temp) return 2;
    const std::string work(temp);
    const std::string envdir = work + "/envs";
    const std::string logfile = work + "/iceccd.log";
    const std::string socket_path = work + "/iceccd.sock";
    mkdir(envdir.c_str(), 0700);
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
        execl(argv[1], argv[1], "--no-remote", "-m", "1", "-p", "10245",
              "-s", scheduler_spec, "-n", "p49-worker", "-N", "p49-daemon",
              "-b", envdir.c_str(), "-l", logfile.c_str(), "-v", "-v", "-v",
              static_cast<char *>(nullptr));
        _exit(127);
    }
    REQUIRE(daemon_pid > 0, "real iceccd started");

    MsgChannel *scheduler = accept_channel(listener, 8000);
    Msg *login = wait_type(scheduler, Msg::LOGIN, 5000);
    REQUIRE(scheduler && login, "daemon logged in through the real protocol");
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
                ConfCSMsg(epoch, ConfCSMsg::Advisory)),
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

    /* Three terminals already exist.  Fill the eight-entry test bound, then
       prove that the next identity closes the session and the same exhausted
       epoch cannot be reactivated. */
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
    REQUIRE(scheduler->send_msg(RevokeBeforeStartMsg(
                epoch, 1005, UINT64_C(0x7000000000000005))),
            "identity beyond the epoch bound delivered");
    no_type(scheduler, Msg::REVOKE_RESULT, 1000);
    REQUIRE(scheduler->at_eof(),
            "terminal-table exhaustion closes the scheduler session");
    delete scheduler;
    scheduler = nullptr;
    usleep(300 * 1000);
    const std::string exhausted = request_internals(socket_path);
    REQUIRE(exhausted.find("retained=8") != std::string::npos
                && exhausted.find("exhausted=1") != std::string::npos,
            "bounded epoch state remains observable and fail-closed");

    scheduler = accept_channel(listener, 8000);
    login = wait_type(scheduler, Msg::LOGIN, 5000);
    REQUIRE(scheduler && login, "daemon retries the exhausted epoch");
    delete login;
    REQUIRE(scheduler && scheduler->send_msg(
                ConfCSMsg(epoch, ConfCSMsg::EnforcingCompat)),
            "same exhausted epoch configuration delivered");
    no_type(scheduler, Msg::STATUS_TEXT, 1000);
    REQUIRE(scheduler->at_eof(),
            "same exhausted epoch is deterministically refused");
    delete scheduler;
    scheduler = nullptr;

    const uint64_t advisory_epoch = epoch + 1;
    scheduler = accept_channel(listener, 8000);
    login = wait_type(scheduler, Msg::LOGIN, 5000);
    REQUIRE(scheduler && login, "daemon reconnects for a fresh epoch");
    delete login;
    REQUIRE(scheduler && scheduler->send_msg(
                ConfCSMsg(advisory_epoch, ConfCSMsg::Advisory)),
            "fresh ADVISORY epoch resets the bounded table");

    const uint32_t advisory_id = 1201;
    const uint64_t advisory_nonce = UINT64_C(0x8100000000001201);
    MsgChannel *advisory_claim = connect_daemon(socket_path);
    REQUIRE(advisory_claim && send_claim(advisory_claim, advisory_id),
            "ADVISORY admits an unknown nonce-less legacy claim");
    usleep(100 * 1000);
    REQUIRE(scheduler->send_msg(AssignPrepareMsg(
                advisory_epoch, advisory_id, advisory_nonce, 9)),
            "late PREPARE binds the Advisory claim identity");
    ready_wire = wait_type(scheduler, Msg::ASSIGN_READY, 3000);
    ready = dynamic_cast<AssignReadyMsg *>(ready_wire);
    REQUIRE(ready && ready->epoch() == advisory_epoch
                && ready->wire_id == advisory_id
                && ready->nonce() == advisory_nonce,
            "late PREPARE installs the full identity and emits READY");
    delete ready_wire;
    REQUIRE(scheduler->send_msg(RevokeBeforeStartMsg(
                advisory_epoch, advisory_id, advisory_nonce)),
            "Advisory claim and revoke race resolved by the one owner");
    terminal_wire = wait_type(scheduler, Msg::REVOKE_RESULT, 3000);
    terminal = dynamic_cast<RevokeResultMsg *>(terminal_wire);
    REQUIRE(terminal && terminal->result == RevokeResultMsg::ClaimedOrLater,
            "late PREPARE never regresses a consumed claim to reserved");
    delete terminal_wire;
    delete advisory_claim;

    const uint32_t cancel_id = 1202;
    const uint64_t cancel_nonce = UINT64_C(0x8100000000001202);
    REQUIRE(scheduler->send_msg(RevokeBeforeStartMsg(
                advisory_epoch, cancel_id, cancel_nonce)),
            "REVOKE may win before delayed PREPARE is consumed");
    terminal_wire = wait_type(scheduler, Msg::REVOKE_RESULT, 3000);
    terminal = dynamic_cast<RevokeResultMsg *>(terminal_wire);
    REQUIRE(terminal && terminal->result == RevokeResultMsg::Revoked,
            "revoke-first race retains the revoked outcome");
    delete terminal_wire;
    REQUIRE(scheduler->send_msg(AssignPrepareMsg(
                advisory_epoch, cancel_id, cancel_nonce, 9)),
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
            "late Advisory claim for cancelled identity sent");
    Msg *cancelled_end = wait_type(cancelled_claim, Msg::END, 3000);
    REQUIRE(cancelled_end || (cancelled_claim && cancelled_claim->at_eof()),
            "closed Advisory identity rejects its late nonce-less claim");
    delete cancelled_end;
    delete cancelled_claim;

    REQUIRE(scheduler->send_msg(AssignPrepareMsg(0, 1300, 1, 9)),
            "Advisory session closed at the reserved-zero boundary");
    no_type(scheduler, Msg::STATUS_TEXT, 1000);
    REQUIRE(scheduler->at_eof(), "Advisory loss reaches reconnect boundary");
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
    REQUIRE(scheduler && scheduler->send_msg(ConfCSMsg(
                advisory_epoch + 1, ConfCSMsg::StrictNonce)),
            "STRICT_NONCE configuration delivered to P49 daemon");
    no_type(scheduler, Msg::STATUS_TEXT, 1000);
    REQUIRE(scheduler->at_eof(),
            "P49 daemon deterministically refuses STRICT_NONCE activation");
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
