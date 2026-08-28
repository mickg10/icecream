/*
 * Real F-daemon source-arm/WAIT runtime gate.
 *
 * The test drives one production iceccd process and a real READY sidecar.  A
 * fake scheduler supplies exact PREPARE records; no private cache socket,
 * SCM_RIGHTS, InputReady, compiler child, or CACHE_SESSION is used.  The
 * ordinary wrapper therefore exercises the same event-loop fd that the
 * source arm handler owns.
 */
#include "config.h"
#include "comm.h"
#include "../cache/p50_incarnation_identity.h"

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
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;
using icecc::p50::FStoreGuid;
using icecc::p50::StoreIdentityRoot;
using icecc::p50::store_identity_root_from_f_guid;

#if defined(HAVE_LIBCAP_NG)

static int failures = 0;
#define REQUIRE(condition, text) do { \
    if (condition) std::fprintf(stderr, "ok - %s\n", text); \
    else { std::fprintf(stderr, "FAILED - %s\n", text); ++failures; } \
} while (0)

static int listen_ephemeral(int *port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
        ::listen(fd, 8) != 0) {
        ::close(fd);
        return -1;
    }
    socklen_t length = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length) != 0) {
        ::close(fd);
        return -1;
    }
    *port = ntohs(address.sin_port);
    return fd;
}

static int reserve_port()
{
    int port = 0;
    const int fd = listen_ephemeral(&port);
    if (fd >= 0)
        ::close(fd);
    return port;
}

static MsgChannel *accept_channel(int listener, int timeout_msec)
{
    pollfd descriptor{listener, POLLIN, 0};
    if (::poll(&descriptor, 1, timeout_msec) <= 0)
        return nullptr;
    const int fd = ::accept(listener, nullptr, nullptr);
    if (fd < 0)
        return nullptr;
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    return Service::createChannel(fd, reinterpret_cast<sockaddr *>(&peer),
                                  sizeof(peer));
}

static MsgChannel *connect_tcp_bounded(int port, int timeout_msec)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (Clock::now() < deadline) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return nullptr;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<uint16_t>(port));
        if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0) {
            return Service::createChannel(fd, reinterpret_cast<sockaddr *>(&address),
                                          sizeof(address));
        }
        ::close(fd);
        ::usleep(20000);
    }
    return nullptr;
}

static Msg *wait_for_type(MsgChannel *channel, Msg::Value type, int timeout_msec)
{
    if (channel == nullptr)
        return nullptr;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (Clock::now() < deadline) {
        Msg *message = channel->get_msg(1, true);
        if (message != nullptr) {
            if (*message == type)
                return message;
            delete message;
        }
        if (channel->at_eof())
            return nullptr;
    }
    return nullptr;
}

static bool wait_for_job_done_and_positive_cache_login(
    MsgChannel *channel, uint32_t wire_id, uint32_t port, int timeout_msec,
    bool *login_seen)
{
    if (channel == nullptr)
        return false;
    bool job_done = false;
    bool positive_login = false;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (Clock::now() < deadline && (!job_done || !positive_login)) {
        Msg *message = channel->get_msg(1, true);
        if (message == nullptr)
            continue;
        auto *done = dynamic_cast<JobDoneMsg *>(message);
        if (done != nullptr && done->job_id == wire_id && done->is_from_server())
            job_done = true;
        auto *login = dynamic_cast<LoginMsg *>(message);
        if (login != nullptr && login->cache_endpoint_port == port &&
            login->cache_protocol == CACHE_WIRE_PROTOCOL_V1 &&
            login->cache_profile_mask == CACHE_PROFILE_ZSTD_TU)
            positive_login = true;
        delete message;
    }
    if (login_seen != nullptr)
        *login_seen = positive_login;
    return job_done;
}

static bool wait_eof(MsgChannel *channel, int timeout_msec)
{
    if (channel == nullptr)
        return false;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (Clock::now() < deadline) {
        Msg *message = channel->get_msg(1, true);
        delete message;
        if (channel->at_eof())
            return true;
    }
    return channel->at_eof();
}

static bool wait_child(pid_t pid, int timeout_msec, int *status)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (Clock::now() < deadline) {
        const pid_t result = ::waitpid(pid, status, WNOHANG);
        if (result == pid)
            return true;
        if (result < 0)
            return false;
        ::usleep(20000);
    }
    return false;
}

// The production daemon owns the cache service as a direct child.  Discover
// that exact child through /proc rather than killing an unrelated process by
// name; this keeps the replacement witness scoped to the daemon launched by
// this test and proves the real supervised READY replacement path.
static bool command_is(pid_t pid, const char *executable)
{
    std::ifstream input("/proc/" + std::to_string(pid) + "/cmdline",
                        std::ios::in | std::ios::binary);
    if (!input)
        return false;
    const std::string commandline{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    const size_t end = commandline.find('\0');
    return commandline.substr(0, end) == executable;
}

static pid_t find_sidecar_child(pid_t daemon_pid, const char *executable)
{
    std::ifstream input("/proc/" + std::to_string(daemon_pid) + "/task/" +
                        std::to_string(daemon_pid) + "/children");
    if (!input)
        return -1;
    pid_t child = -1;
    while (input >> child) {
        if (command_is(child, executable))
            return child;
    }
    return -1;
}

static pid_t wait_for_sidecar_child(pid_t daemon_pid, const char *executable,
                                    int timeout_msec)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (Clock::now() < deadline) {
        const pid_t child = find_sidecar_child(daemon_pid, executable);
        if (child > 1)
            return child;
        ::usleep(20000);
    }
    return -1;
}

static P50SourceArmFields source_arm(uint32_t wire_id, uint64_t epoch,
                                     uint64_t nonce, uint32_t cache_port,
                                     uint64_t request_id)
{
    P50SourceArmFields arm;
    arm.wire_job_id = wire_id;
    arm.assignment_epoch = epoch;
    arm.assignment_nonce = nonce;
    arm.selected_f_host = "127.0.0.1";
    arm.selected_f_ordinary_port = 0; // filled from Login below
    arm.selected_f_cache_port = cache_port;
    arm.cache_protocol = CACHE_WIRE_PROTOCOL_V1;
    arm.cache_profile = CACHE_PROFILE_ZSTD_TU;
    arm.logical_job = wire_id;
    arm.compiler_attempt = nonce;
    arm.c_store_generation = 1;
    arm.c_store_derivation_version = icecc::p50::kStoreIdentityDerivationVersion;
    arm.c_store_guid[0] = 0x11;
    arm.c_store_guid[1] = 0x22;
    arm.source_request_id = request_id;
    arm.source_mode = P50_SOURCE_MODE_ZSTD_TU;
    arm.c_control_generation = 1;
    arm.c_control_attempt = request_id;
    return arm;
}

static CompileJob *pending_compile(const P50SourceArmFields& arm)
{
    auto *job = new CompileJob;
    job->setJobID(arm.wire_job_id);
    job->setAssignmentIdentity(arm.assignment_epoch, arm.assignment_nonce);
    job->setLanguage(CompileJob::Lang_CXX);
    job->setEnvironmentVersion("__test");
    CompileInputIdentity input;
    input.profile = CompileInputIdentity::ZstdTuProfile;
    input.c_store_guid = arm.c_store_guid;
    input.attempt_id = arm.compiler_attempt;
    input.request_id = arm.source_request_id;
    job->setCompileInputIdentity(input);
    return job;
}

static bool wait_one_job_done(MsgChannel *scheduler, uint32_t expected,
                              int timeout_msec)
{
    Msg *message = wait_for_type(scheduler, Msg::JOB_DONE, timeout_msec);
    auto *done = dynamic_cast<JobDoneMsg *>(message);
    const bool match = done != nullptr && done->job_id == expected &&
                       done->is_from_server();
    delete message;
    return match;
}

static bool no_job_done(MsgChannel *scheduler, uint32_t unexpected,
                        int timeout_msec)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (Clock::now() < deadline) {
        Msg *message = scheduler->get_msg(1, true);
        if (message != nullptr) {
            if (*message == Msg::JOB_DONE) {
                auto *done = dynamic_cast<JobDoneMsg *>(message);
                const bool duplicate = done != nullptr && done->job_id == unexpected;
                delete message;
                if (duplicate)
                    return false;
                continue;
            }
            delete message;
        }
    }
    return true;
}

static bool run_live_test(const char *iceccd_path, const char *cache_service_path)
{
    passwd *icecc = ::getpwnam("icecc");
    if (icecc == nullptr || icecc->pw_uid == 0 || icecc->pw_gid == 0)
        return false;

    char pattern[] = "/tmp/p50-source-live.XXXXXX";
    char *created = ::mkdtemp(pattern);
    if (created == nullptr)
        return false;
    const std::filesystem::path root(created);
    const std::filesystem::path envdir = root / "envs";
    const std::filesystem::path runtime = root / "runtime";
    const std::filesystem::path log = root / "iceccd.log";
    (void)::mkdir(envdir.c_str(), 0700);
    (void)::mkdir(runtime.c_str(), 0700);
    (void)::chown(root.c_str(), icecc->pw_uid, icecc->pw_gid);
    (void)::chown(envdir.c_str(), icecc->pw_uid, icecc->pw_gid);
    (void)::chown(runtime.c_str(), icecc->pw_uid, icecc->pw_gid);
    (void)::chmod(root.c_str(), 0700);
    (void)::chmod(envdir.c_str(), 0700);
    (void)::chmod(runtime.c_str(), 0700);

    int scheduler_port = 0;
    const int scheduler_listener = listen_ephemeral(&scheduler_port);
    const int daemon_port = reserve_port();
    if (scheduler_listener < 0 || daemon_port <= 0) {
        std::filesystem::remove_all(root);
        return false;
    }

    const pid_t daemon_pid = ::fork();
    if (daemon_pid == 0) {
        char scheduler[64];
        char public_port[16];
        std::snprintf(scheduler, sizeof(scheduler), "127.0.0.1:%d", scheduler_port);
        std::snprintf(public_port, sizeof(public_port), "%d", daemon_port);
        ::setenv("ICECC_TESTS", "1", 1);
        ::setenv("ICECC_TEST_P50_SOURCE_BUDGET_MSEC", "3000", 1);
        ::execl(iceccd_path, iceccd_path, "-p", public_port, "-m", "1",
                "-s", scheduler, "-n", "p50-source-live", "-N", "p50-f",
                "-b", envdir.c_str(), "-l", log.c_str(),
                "--cache-service", cache_service_path,
                "--cache-runtime-dir", runtime.c_str(), "-v", "-v",
                static_cast<char *>(nullptr));
        ::_exit(127);
    }

    MsgChannel *scheduler = accept_channel(scheduler_listener, 10000);
    Msg *initial_message = wait_for_type(scheduler, Msg::LOGIN, 5000);
    auto *initial = dynamic_cast<LoginMsg *>(initial_message);
    const bool initial_ok = initial != nullptr &&
        initial->cache_endpoint_port == 0 && initial->cache_protocol == 0 &&
        initial->cache_profile_mask == 0;
    REQUIRE(initial_ok, "runtime initial Login is cache-absent");
    delete initial_message;

    const uint64_t epoch = UINT64_C(0x5000000000000001);
    if (scheduler == nullptr || !scheduler->send_msg(
            ConfCSMsg(epoch, ConfCSMsg::StrictNonce))) {
        failures++;
    }
    Msg *positive_message = wait_for_type(scheduler, Msg::LOGIN, 15000);
    auto *positive = dynamic_cast<LoginMsg *>(positive_message);
    const bool positive_ok = positive != nullptr &&
        positive->cache_endpoint_port == static_cast<uint32_t>(daemon_port) &&
        positive->cache_protocol == CACHE_WIRE_PROTOCOL_V1 &&
        positive->cache_profile_mask == CACHE_PROFILE_ZSTD_TU;
    REQUIRE(positive_ok, "runtime READY sidecar publishes exact F advertisement");
    const uint32_t cache_port = positive_ok ? positive->cache_endpoint_port : 0;
    delete positive_message;
    if (!positive_ok) {
        if (daemon_pid > 0) {
            (void)::kill(daemon_pid, SIGTERM);
            int status = 0;
            (void)::waitpid(daemon_pid, &status, 0);
        }
        delete scheduler;
        ::close(scheduler_listener);
        std::filesystem::remove_all(root);
        return false;
    }

    auto prepare = [&](uint32_t wire_id, uint64_t nonce) {
        if (scheduler == nullptr ||
            !scheduler->send_msg(AssignPrepareMsg(epoch, wire_id, nonce, 1))) {
            return false;
        }
        Msg *ready = wait_for_type(scheduler, Msg::ASSIGN_READY, 3000);
        const bool accepted = ready != nullptr;
        delete ready;
        return accepted;
    };

    // Silent WAIT expiry: the client stays open and produces no fd event.
    const uint32_t silent_id = 7001;
    const uint64_t silent_nonce = UINT64_C(0x7001000000000001);
    REQUIRE(prepare(silent_id, silent_nonce),
            "runtime exact PREPARE reaches source-arm admission");
    MsgChannel *silent = connect_tcp_bounded(daemon_port, 5000);
    P50SourceArmFields silent_arm = source_arm(
        silent_id, epoch, silent_nonce, cache_port, UINT64_C(0x701));
    silent_arm.selected_f_ordinary_port = static_cast<uint32_t>(daemon_port);
    REQUIRE(silent != nullptr && silent->send_msg(P50SourceArmMsg(silent_arm)),
            "runtime selected-F ordinary wrapper sends source arm");
    Msg *silent_ack_message = wait_for_type(silent, Msg::P50_SOURCE_ARMED, 3000);
    auto *silent_ack = dynamic_cast<P50SourceArmedMsg *>(silent_ack_message);
    REQUIRE(silent_ack != nullptr && silent_ack->arm == silent_arm &&
                silent_ack->f_store_generation != 0 &&
                silent_ack->source_budget_msec != 0 &&
                silent_ack->source_budget_msec <= P50SourceArmedFields::MaxSourceBudgetMsec,
            "runtime ACK proves WAIT owner installed with F generation and bounded budget");
    delete silent_ack_message;
    REQUIRE(wait_one_job_done(scheduler, silent_id, 5000),
            "silent owner deadline sweep sends one worker JobDone");
    REQUIRE(wait_eof(silent, 1000),
            "silent owner is closed after deadline without peer activity");
    REQUIRE(no_job_done(scheduler, silent_id, 300),
            "silent expiry has no duplicate scheduler settlement");
    delete silent;

    // Sidecar replacement is an ownership boundary, not a transparent retry:
    // an old WAIT owner must be withdrawn before the replacement READY lease
    // can be advertised or used by a new source arm.
    const uint32_t replacement_id = 7008;
    const uint64_t replacement_nonce = UINT64_C(0x7008000000000001);
    REQUIRE(prepare(replacement_id, replacement_nonce),
            "runtime replacement assignment is PREPARE-bound");
    MsgChannel *replacement = connect_tcp_bounded(daemon_port, 5000);
    P50SourceArmFields replacement_arm = source_arm(
        replacement_id, epoch, replacement_nonce, cache_port, UINT64_C(0x708));
    replacement_arm.selected_f_ordinary_port = static_cast<uint32_t>(daemon_port);
    REQUIRE(replacement != nullptr &&
                replacement->send_msg(P50SourceArmMsg(replacement_arm)),
            "runtime replacement owner sends exact arm");
    Msg *replacement_ack = wait_for_type(replacement, Msg::P50_SOURCE_ARMED, 3000);
    auto *replacement_armed = dynamic_cast<P50SourceArmedMsg *>(replacement_ack);
    REQUIRE(replacement_armed != nullptr &&
                replacement_armed->f_store_generation != 0,
            "runtime replacement owner receives F generation before restart");
    const uint64_t old_f_generation = replacement_armed == nullptr
        ? 0 : replacement_armed->f_store_generation;
    const uint64_t old_f_control_generation = replacement_armed == nullptr
        ? 0 : replacement_armed->f_control_generation;
    const uint64_t old_f_control_attempt = replacement_armed == nullptr
        ? 0 : replacement_armed->f_control_attempt;
    const FStoreGuid old_f_guid = replacement_armed == nullptr
        ? FStoreGuid{} : FStoreGuid{replacement_armed->f_store_guid};
    const StoreIdentityRoot old_f_root =
        store_identity_root_from_f_guid(old_f_guid);
    delete replacement_ack;
    const pid_t old_sidecar = wait_for_sidecar_child(
        daemon_pid, cache_service_path, 3000);
    REQUIRE(old_sidecar > 1, "runtime finds the supervised F sidecar child");
    REQUIRE(old_sidecar > 1 && ::kill(old_sidecar, SIGKILL) == 0,
            "runtime kills the exact supervised F sidecar incarnation");
    bool replacement_login_seen = false;
    REQUIRE(wait_for_job_done_and_positive_cache_login(
                scheduler, replacement_id, static_cast<uint32_t>(daemon_port),
                5000, &replacement_login_seen),
            "runtime sidecar replacement withdraws the old WAIT owner exactly once");
    REQUIRE(wait_eof(replacement, 1500),
            "runtime sidecar replacement closes the old WAIT wrapper");
    REQUIRE(no_job_done(scheduler, replacement_id, 300),
            "runtime sidecar replacement has no duplicate scheduler settlement");
    delete replacement;

    REQUIRE(replacement_login_seen,
            "runtime replacement publishes a fresh READY advertisement");
    const pid_t new_sidecar = wait_for_sidecar_child(
        daemon_pid, cache_service_path, 3000);
    REQUIRE(new_sidecar > 1 && new_sidecar != old_sidecar,
            "runtime replacement uses a new supervised F sidecar PID");
    const uint32_t replacement_followup_id = 7009;
    const uint64_t replacement_followup_nonce = UINT64_C(0x7009000000000001);
    REQUIRE(prepare(replacement_followup_id, replacement_followup_nonce),
            "runtime post-replacement assignment is PREPARE-bound");
    MsgChannel *replacement_followup = connect_tcp_bounded(daemon_port, 5000);
    P50SourceArmFields replacement_followup_arm = source_arm(
        replacement_followup_id, epoch, replacement_followup_nonce, cache_port,
        UINT64_C(0x709));
    replacement_followup_arm.selected_f_ordinary_port =
        static_cast<uint32_t>(daemon_port);
    REQUIRE(replacement_followup != nullptr &&
                replacement_followup->send_msg(
                    P50SourceArmMsg(replacement_followup_arm)),
            "runtime post-replacement wrapper sends exact arm");
    Msg *replacement_followup_ack = wait_for_type(
        replacement_followup, Msg::P50_SOURCE_ARMED, 3000);
    auto *replacement_followup_armed =
            dynamic_cast<P50SourceArmedMsg *>(replacement_followup_ack);
    REQUIRE(replacement_followup_armed != nullptr &&
                replacement_followup_armed->f_store_generation != 0 &&
                replacement_followup_armed->f_control_generation == old_f_control_generation &&
                replacement_followup_armed->f_control_attempt != old_f_control_attempt &&
                replacement_followup_armed->f_store_guid != old_f_guid.bytes &&
                store_identity_root_from_f_guid(
                    FStoreGuid{replacement_followup_armed->f_store_guid}) != old_f_root &&
                (old_f_generation == 0 ||
                 replacement_followup_armed->f_store_generation !=
                     old_f_generation),
            "runtime post-replacement arm carries a fresh F generation");
    delete replacement_followup_ack;
    delete replacement_followup;
    REQUIRE(wait_one_job_done(scheduler, replacement_followup_id, 5000),
            "runtime post-replacement owner settles exactly once");
    REQUIRE(no_job_done(scheduler, replacement_followup_id, 300),
            "runtime post-replacement owner has no duplicate settlement");

    // HUP/EOF path: half-close the actual ordinary peer after the exact ACK.
    // The daemon must observe EOF/HUP on the registered WAIT fd and settle the
    // retained owner; no second arm is used to trigger a synthetic rejection.
    const uint32_t hup_id = 7002;
    const uint64_t hup_nonce = UINT64_C(0x7002000000000001);
    REQUIRE(prepare(hup_id, hup_nonce), "runtime HUP assignment is PREPARE-bound");
    MsgChannel *hup = connect_tcp_bounded(daemon_port, 5000);
    P50SourceArmFields hup_arm = source_arm(
        hup_id, epoch, hup_nonce, cache_port, UINT64_C(0x702));
    hup_arm.selected_f_ordinary_port = static_cast<uint32_t>(daemon_port);
    REQUIRE(hup != nullptr && hup->send_msg(P50SourceArmMsg(hup_arm)),
            "runtime HUP owner sends exact arm");
    Msg *hup_ack = wait_for_type(hup, Msg::P50_SOURCE_ARMED, 3000);
    REQUIRE(dynamic_cast<P50SourceArmedMsg *>(hup_ack) != nullptr &&
                dynamic_cast<P50SourceArmedMsg *>(hup_ack)->f_store_generation != 0,
            "runtime HUP owner receives ACK with F generation before disconnect");
    delete hup_ack;
    REQUIRE(hup != nullptr && ::shutdown(hup->fd, SHUT_WR) == 0,
            "runtime ordinary peer half-closes the retained wrapper");
    REQUIRE(wait_eof(hup, 1000),
            "runtime POLLHUP/EOF closes the wrapper deterministically");
    delete hup;
    REQUIRE(wait_one_job_done(scheduler, hup_id, 3000),
            "runtime POLLHUP/EOF settles the exact assignment once");
    REQUIRE(no_job_done(scheduler, hup_id, 300),
            "runtime POLLHUP/EOF has no duplicate scheduler settlement");

    // A second live PREPARE triple on the same wrapper must never redirect
    // teardown away from the retained owner.  The second assignment remains
    // claimable by a fresh wrapper after the first owner is settled.
    const uint32_t retained_id = 7003;
    const uint64_t retained_nonce = UINT64_C(0x7003000000000001);
    const uint32_t replay_id = 7004;
    const uint64_t replay_nonce = UINT64_C(0x7004000000000001);
    REQUIRE(prepare(retained_id, retained_nonce),
            "runtime retained-owner assignment is PREPARE-bound");
    REQUIRE(prepare(replay_id, replay_nonce),
            "runtime replayed-owner assignment is PREPARE-bound");
    MsgChannel *retained = connect_tcp_bounded(daemon_port, 5000);
    P50SourceArmFields retained_arm = source_arm(
        retained_id, epoch, retained_nonce, cache_port, UINT64_C(0x704));
    retained_arm.selected_f_ordinary_port = static_cast<uint32_t>(daemon_port);
    P50SourceArmFields replay_arm = source_arm(
        replay_id, epoch, replay_nonce, cache_port, UINT64_C(0x705));
    replay_arm.selected_f_ordinary_port = static_cast<uint32_t>(daemon_port);
    REQUIRE(retained != nullptr && retained->send_msg(P50SourceArmMsg(retained_arm)),
            "runtime retained owner sends its exact arm");
    Msg *retained_ack = wait_for_type(retained, Msg::P50_SOURCE_ARMED, 3000);
    REQUIRE(dynamic_cast<P50SourceArmedMsg *>(retained_ack) != nullptr &&
                dynamic_cast<P50SourceArmedMsg *>(retained_ack)->f_store_generation != 0,
            "runtime retained owner receives ACK with F generation");
    delete retained_ack;
    REQUIRE(retained != nullptr && retained->send_msg(P50SourceArmMsg(replay_arm)),
            "runtime different live triple reaches the retained wrapper");
    REQUIRE(wait_eof(retained, 1000),
            "runtime different triple cannot redirect retained-wrapper teardown");
    delete retained;
    REQUIRE(wait_one_job_done(scheduler, retained_id, 3000),
            "runtime retained wrapper settles only its original token");
    REQUIRE(no_job_done(scheduler, replay_id, 300),
            "runtime different triple does not settle another assignment");

    MsgChannel *replay = connect_tcp_bounded(daemon_port, 5000);
    REQUIRE(replay != nullptr && replay->send_msg(P50SourceArmMsg(replay_arm)),
            "runtime untouched replay assignment remains claimable");
    Msg *replay_ack = wait_for_type(replay, Msg::P50_SOURCE_ARMED, 3000);
    REQUIRE(dynamic_cast<P50SourceArmedMsg *>(replay_ack) != nullptr &&
                dynamic_cast<P50SourceArmedMsg *>(replay_ack)->f_store_generation != 0,
            "runtime fresh wrapper ACKs the untouched replay assignment with F generation");
    delete replay_ack;
    delete replay;
    REQUIRE(wait_one_job_done(scheduler, replay_id, 3000),
            "runtime fresh wrapper settles the untouched assignment once");
    REQUIRE(no_job_done(scheduler, replay_id, 300),
            "runtime fresh wrapper has no duplicate settlement");

    // Buffered/later CompileFile: the exact same claimant is retained in
    // WAIT, never enters TOCOMPILE, and is eventually settled by the same
    // deadline rather than becoming operationally unreachable.
    const uint32_t buffered_id = 7006;
    const uint64_t buffered_nonce = UINT64_C(0x7006000000000001);
    REQUIRE(prepare(buffered_id, buffered_nonce),
            "runtime buffered CompileFile assignment is PREPARE-bound");
    MsgChannel *buffered = connect_tcp_bounded(daemon_port, 5000);
    P50SourceArmFields buffered_arm = source_arm(
        buffered_id, epoch, buffered_nonce, cache_port, UINT64_C(0x703));
    buffered_arm.selected_f_ordinary_port = static_cast<uint32_t>(daemon_port);
    CompileFileMsg buffered_compile(pending_compile(buffered_arm), true);
    REQUIRE(buffered != nullptr && buffered->send_msg(P50SourceArmMsg(buffered_arm)) &&
                buffered->send_msg(buffered_compile),
            "runtime arm and later CompileFile are buffered on one ordinary fd");
    Msg *buffered_ack = wait_for_type(buffered, Msg::P50_SOURCE_ARMED, 3000);
    REQUIRE(dynamic_cast<P50SourceArmedMsg *>(buffered_ack) != nullptr &&
                dynamic_cast<P50SourceArmedMsg *>(buffered_ack)->f_store_generation != 0,
            "runtime buffered owner ACKs before CompileFile processing with F generation");
    delete buffered_ack;
    REQUIRE(no_job_done(scheduler, buffered_id, 75),
            "runtime pending CompileFile is not prematurely terminalized or forked");
    REQUIRE(wait_one_job_done(scheduler, buffered_id, 3000),
            "runtime pending CompileFile remains reachable until deadline settlement");
    delete buffered;

    const uint32_t duplicate_compile_id = 7007;
    const uint64_t duplicate_compile_nonce = UINT64_C(0x7007000000000001);
    REQUIRE(prepare(duplicate_compile_id, duplicate_compile_nonce),
            "runtime duplicate CompileFile assignment is PREPARE-bound");
    MsgChannel *duplicate_compile = connect_tcp_bounded(daemon_port, 5000);
    P50SourceArmFields duplicate_arm = source_arm(
        duplicate_compile_id, epoch, duplicate_compile_nonce, cache_port,
        UINT64_C(0x707));
    duplicate_arm.selected_f_ordinary_port = static_cast<uint32_t>(daemon_port);
    CompileFileMsg first_compile(pending_compile(duplicate_arm), true);
    CompileFileMsg second_compile(pending_compile(duplicate_arm), true);
    REQUIRE(duplicate_compile != nullptr &&
                duplicate_compile->send_msg(P50SourceArmMsg(duplicate_arm)) &&
                duplicate_compile->send_msg(first_compile) &&
                duplicate_compile->send_msg(second_compile),
            "runtime duplicate CompileFile frames are buffered after one arm");
    Msg *duplicate_ack = wait_for_type(duplicate_compile, Msg::P50_SOURCE_ARMED, 3000);
    REQUIRE(dynamic_cast<P50SourceArmedMsg *>(duplicate_ack) != nullptr &&
                dynamic_cast<P50SourceArmedMsg *>(duplicate_ack)->f_store_generation != 0,
            "runtime duplicate CompileFile owner receives one ACK with F generation");
    delete duplicate_ack;
    REQUIRE(wait_eof(duplicate_compile, 1000),
            "runtime duplicate CompileFile closes after one retained claimant");
    delete duplicate_compile;
    REQUIRE(wait_one_job_done(scheduler, duplicate_compile_id, 3000),
            "runtime duplicate CompileFile settles the exact assignment once");
    REQUIRE(no_job_done(scheduler, duplicate_compile_id, 300),
            "runtime duplicate CompileFile has no duplicate scheduler settlement");

    int status = 0;
    if (daemon_pid > 0) {
        (void)::kill(daemon_pid, SIGTERM);
        if (!wait_child(daemon_pid, 5000, &status)) {
            (void)::kill(daemon_pid, SIGKILL);
            (void)::waitpid(daemon_pid, &status, 0);
        }
    }
    delete scheduler;
    ::close(scheduler_listener);
    std::filesystem::remove_all(root);
    return failures == 0;
}

#endif

int main(int argc, char **argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <iceccd> <icecc-cache-service>\n", argv[0]);
        return 2;
    }
    if (std::getenv("ICECC_TEST_SOURCE_ARM_LIVE") == nullptr) {
        std::fprintf(stderr, "SKIP: set ICECC_TEST_SOURCE_ARM_LIVE=1 in an isolated root container\n");
        return 77;
    }
#if !defined(HAVE_LIBCAP_NG)
    std::fprintf(stderr, "SKIP: live source-arm daemon test requires libcap-ng\n");
    return 77;
#else
    if (::geteuid() != 0) {
        std::fprintf(stderr, "SKIP: live source-arm daemon test requires container root\n");
        return 77;
    }
    passwd *icecc = ::getpwnam("icecc");
    if (icecc == nullptr || icecc->pw_uid == 0 || icecc->pw_gid == 0) {
        std::fprintf(stderr, "SKIP: live source-arm daemon test requires a non-root icecc user\n");
        return 77;
    }
    return run_live_test(argv[1], argv[2]) ? 0 : 1;
#endif
}
