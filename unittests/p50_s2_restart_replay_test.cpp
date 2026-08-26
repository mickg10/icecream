#include "../cache/p50_phase_open.h"
#include "../cache/p50_reverse_fd_retry.h"
#include "../cache/p50_sidecar_supervisor.h"

#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <memory>
#include <signal.h>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using namespace icecc::p50;
using namespace icecc::p50::local;
using namespace icecc::p50::sidecar;

namespace {

void check(bool value, const char* expression) {
    if (!value)
        throw std::runtime_error(expression);
}

#define CHECK(expression) check((expression), #expression)

struct CaseCounters {
    uint64_t launches = 0;
    uint64_t ready = 0;
    uint64_t pre_ready_deaths = 0;
    uint64_t post_ready_deaths = 0;
    uint64_t restarts = 0;
    uint64_t handoff_ack = 0;
    uint64_t adopted = 0;
    uint64_t lost_ack = 0;
    uint64_t lease_withdrawals = 0;
    uint64_t lease_rotations = 0;
    uint64_t phase_accepts = 0;
    uint64_t phase_replays = 0;
    uint64_t phase_conflicts = 0;
    uint64_t phase_stale = 0;
    uint64_t reverse_accepts = 0;
    uint64_t reverse_replays = 0;
    uint64_t reverse_conflicts = 0;
    uint64_t reverse_stale = 0;
    uint64_t transitions = 0;
    uint64_t forks = 0;
    uint64_t compile_cursor_takes = 0;
};

void emit_case(std::string_view name, const CaseCounters& c) {
    std::cout << "CASE " << name
              << " launches=" << c.launches
              << " ready=" << c.ready
              << " pre_ready_deaths=" << c.pre_ready_deaths
              << " post_ready_deaths=" << c.post_ready_deaths
              << " restarts=" << c.restarts
              << " handoff_ack=" << c.handoff_ack
              << " adopted=" << c.adopted
              << " lost_ack=" << c.lost_ack
              << " lease_withdrawals=" << c.lease_withdrawals
              << " lease_rotations=" << c.lease_rotations
              << " phase_accepts=" << c.phase_accepts
              << " phase_replays=" << c.phase_replays
              << " phase_conflicts=" << c.phase_conflicts
              << " phase_stale=" << c.phase_stale
              << " reverse_accepts=" << c.reverse_accepts
              << " reverse_replays=" << c.reverse_replays
              << " reverse_conflicts=" << c.reverse_conflicts
              << " reverse_stale=" << c.reverse_stale
              << " transitions=" << c.transitions
              << " forks=" << c.forks
              << " compile_cursor_takes=" << c.compile_cursor_takes << '\n';
}

bool write_all(int fd, std::string_view value) {
    size_t written = 0;
    while (written != value.size()) {
        const ssize_t result = ::write(fd, value.data() + written, value.size() - written);
        if (result > 0)
            written += static_cast<size_t>(result);
        else if (result < 0 && errno == EINTR)
            continue;
        else
            return false;
    }
    return true;
}

int ready_fd() {
    const char* raw = std::getenv(kReadyFdEnvironment.data());
    if (raw == nullptr)
        return -1;
    return std::atoi(raw);
}

const char* environment(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' ? value : nullptr;
}

int inherited_fd(const char* name) {
    const char* raw = environment(name);
    if (raw == nullptr)
        return -1;
    errno = 0;
    char* end = nullptr;
    const long value = std::strtol(raw, &end, 10);
    if (errno != 0 || end == raw || *end != '\0' || value < 3 ||
        value > std::numeric_limits<int>::max())
        return -1;
    return static_cast<int>(value);
}

bool valid_nonzero_guid_hex(std::string_view value) {
    bool nonzero = false;
    if (value.size() != 32)
        return false;
    for (const char character : value) {
        if ((character < '0' || character > '9') &&
            (character < 'a' || character > 'f'))
            return false;
        nonzero = nonzero || character != '0';
    }
    return nonzero;
}

int structured_fake_child(int fd) {
    const char* format = environment("ICECC_CACHE_SERVICE_READY_FORMAT");
    const char* generation = environment("ICECC_CACHE_SERVICE_EXPECTED_GENERATION");
    const char* attempt = environment("ICECC_CACHE_SERVICE_EXPECTED_ATTEMPT");
    const char* store_generation =
        environment("ICECC_CACHE_SERVICE_EXPECTED_F_STORE_GENERATION");
    const char* c_guid = environment("ICECC_CACHE_SERVICE_EXPECTED_C_STORE_GUID");
    const char* f_guid = environment("ICECC_CACHE_SERVICE_EXPECTED_F_STORE_GUID");
    const char* path = environment("ICECC_CACHE_SERVICE_EXPECTED_SOCKET");
    const char* digest = environment("ICECC_CACHE_SERVICE_EXPECTED_SOCKET_DIGEST");
    if (format == nullptr || std::string_view(format) != "2" || generation == nullptr ||
        attempt == nullptr || store_generation == nullptr || c_guid == nullptr || f_guid == nullptr || path == nullptr ||
        digest == nullptr || !valid_nonzero_guid_hex(c_guid) ||
        !valid_nonzero_guid_hex(f_guid) || std::string_view(c_guid) == f_guid)
        return 90;
    const int listener = inherited_fd(kListenerFdEnvironment.data());
    if (listener < 0 || listener == fd)
        return 91;
    struct stat pathname{};
    struct stat descriptor{};
    sockaddr_un bound{};
    socklen_t bound_length = sizeof(bound);
    int accepting = 0;
    socklen_t accepting_length = sizeof(accepting);
    if (::lstat(path, &pathname) != 0 || !S_ISSOCK(pathname.st_mode) ||
        (pathname.st_mode & 07777) != 0600 || pathname.st_dev == 0 || pathname.st_ino == 0 ||
        ::fcntl(listener, F_GETFD) < 0 || ::fstat(listener, &descriptor) != 0 ||
        !S_ISSOCK(descriptor.st_mode) ||
        ::getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &bound_length) != 0 ||
        bound.sun_family != AF_UNIX ||
        std::string_view(bound.sun_path, ::strnlen(bound.sun_path, sizeof(bound.sun_path))) !=
            path ||
        ::getsockopt(listener, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &accepting_length) != 0 ||
        accepting_length != sizeof(accepting) || accepting != 1) {
        return 92;
    }
    const std::string ready =
        "READY v2 generation=" + std::string(generation) + " attempt=" + attempt +
        " F_STORE_GENERATION=" + std::string(store_generation) +
        " DERIVATION_VERSION=1" +
        " pid=" + std::to_string(static_cast<long long>(::getpid())) +
        " C_STORE_GUID=" + c_guid + " F_STORE_GUID=" + f_guid +
        " PATH=" + path + " DIGEST=" + digest +
        " DEV=" + std::to_string(static_cast<unsigned long long>(pathname.st_dev)) +
        " INO=" + std::to_string(static_cast<unsigned long long>(pathname.st_ino)) + "\n";
    if (!write_all(fd, ready)) {
        return 93;
    }
    (void)::close(fd);
    (void)::signal(SIGTERM, SIG_IGN);
    (void)::pause();
    (void)::close(listener);
    return 0;
}

int s2_fake_child(const char* mode) {
    const int fd = ready_fd();
    if (fd < 0)
        return 94;
    const std::string_view value(mode);
    if (value == "before-ready")
        return 95;
    if (value == "ready-exit") {
        (void)write_all(fd, "READY\n");
        (void)::close(fd);
        return 0;
    }
    if (value == "ready-pause") {
        if (!write_all(fd, "READY\n"))
            return 96;
        (void)::close(fd);
        (void)::pause();
        return 0;
    }
    if (value == "structured")
        return structured_fake_child(fd);
    return 97;
}

Config fake_config(const char* mode, uint32_t max_restarts = 0) {
    Config config;
    config.executable = "/proc/self/exe";
    config.arguments = {"--s2-fake-child", mode};
    config.readiness_timeout = std::chrono::milliseconds(250);
    config.shutdown_timeout = std::chrono::milliseconds(80);
    config.restart_window = std::chrono::milliseconds(500);
    config.max_restarts = max_restarts;
    return config;
}

Config structured_config(const char* mode, const std::string& root,
                         const std::shared_ptr<LaunchIdentityAllocator>& allocator) {
    Config config = fake_config(mode, 2);
    config.lease_root = root;
    config.launch_identities = allocator;
    return config;
}

void wait_until_stopped(Supervisor& supervisor) {
    for (unsigned attempt = 0; attempt != 100 && supervisor.state() != State::DegradedLegacy;
         ++attempt) {
        (void)::usleep(5000);
        (void)supervisor.poll();
    }
}

void case_before_ready() {
    Supervisor supervisor(fake_config("before-ready"));
    CaseCounters counters;
    CHECK(!supervisor.start());
    counters.launches = supervisor.counters().launches;
    counters.pre_ready_deaths = supervisor.counters().pre_ready_exits +
                                supervisor.counters().invalid_ready_messages +
                                supervisor.counters().exec_failures;
    CHECK(counters.launches == 1 && counters.pre_ready_deaths >= 1);
    emit_case("sidecar_death_before_READY", counters);
}

void case_after_ready_before_handoff() {
    Supervisor supervisor(fake_config("ready-pause"));
    CaseCounters counters;
    CHECK(supervisor.start());
    counters.ready = 1;
    CHECK(::kill(supervisor.child_pid(), SIGKILL) == 0);
    wait_until_stopped(supervisor);
    counters.launches = supervisor.counters().launches;
    counters.post_ready_deaths = supervisor.counters().post_ready_exits;
    CHECK(counters.ready == 1 && counters.post_ready_deaths >= 1 &&
          counters.handoff_ack == 0 && counters.adopted == 0);
    emit_case("sidecar_death_after_READY_before_handoff", counters);
}

void case_restart_budget_exhaustion() {
    Supervisor supervisor(fake_config("ready-exit", 2));
    CaseCounters counters;
    CHECK(supervisor.start());
    counters.ready = 1;
    wait_until_stopped(supervisor);
    counters.launches = supervisor.counters().launches;
    counters.post_ready_deaths = supervisor.counters().post_ready_exits;
    counters.restarts = supervisor.counters().restarts;
    CHECK(supervisor.state() == State::DegradedLegacy && counters.launches == 3 &&
          counters.restarts >= 2 && counters.post_ready_deaths >= 1);
    emit_case("restart_budget_exhaustion", counters);
}

void case_lease_withdrawal_rotation() {
    char root_template[] = "/tmp/icecc-s2-replay-XXXXXX";
    char* root_value = ::mkdtemp(root_template);
    CHECK(root_value != nullptr);
    const std::string root(root_value);
    CHECK(::chmod(root.c_str(), 0700) == 0);
    const auto allocator = std::make_shared<LaunchIdentityAllocator>(601, 1);
    Supervisor first(structured_config("structured", root, allocator));
    CHECK(first.start() && first.current_lease().has_value());
    const ReadyLease first_lease = *first.current_lease();
    first.shutdown();
    CHECK(!first.current_lease().has_value());

    Supervisor second(structured_config("structured", root, allocator));
    CHECK(second.start() && second.current_lease().has_value());
    const ReadyLease second_lease = *second.current_lease();
    second.shutdown();
    CHECK(first_lease.identity.attempt == 1 && second_lease.identity.attempt == 2 &&
          first_lease.socket_path != second_lease.socket_path);

    CaseCounters counters;
    counters.launches = 2;
    counters.ready = 2;
    counters.lease_withdrawals = 1;
    counters.lease_rotations = 1;
    emit_case("lease_withdrawal_and_rotation", counters);
    CHECK(::rmdir(root.c_str()) == 0);
}

P50SourceArm source_arm() {
    P50SourceArm value;
    value.wire_job_id = 71;
    value.assignment_epoch = 73;
    value.assignment_nonce = 79;
    value.selected_f_host = "f.example";
    value.selected_f_ordinary_port = 8765;
    value.selected_f_cache_port = 9876;
    value.cache_protocol = 50;
    value.cache_profile = 2;
    value.logical_job = 83;
    value.attempt_id = 89;
    value.c_store_generation = 97;
    value.c_store_guid.bytes[15] = 101;
    value.source_request_id = 103;
    value.source_mode = 1;
    return value;
}

HandoffOffer phase_offer(uint64_t request_id) {
    HandoffOffer result;
    result.request_id = request_id;
    result.source_arm = source_arm();
    result.source_arm.source_request_id = request_id;
    result.cache_profile = result.source_arm.cache_profile;
    return result;
}

void case_phase_replay() {
    CaseCounters counters;
    P50HandoffAuthority authority;
    const HandoffOffer first = phase_offer(107);
    CHECK(authority.offer(first) == OfferDecision::Accepted);
    ++counters.phase_accepts;
    const AttachmentPhaseOpen open{first.request_id, first.source_arm};
    CHECK(authority.phase_open(open) == OfferDecision::Accepted);
    ++counters.phase_accepts;
    CHECK(authority.offer(first) == OfferDecision::ExactReplay);
    ++counters.phase_replays;
    CHECK(authority.phase_open(open) == OfferDecision::ExactReplay);
    ++counters.phase_replays;
    HandoffOffer conflict = first;
    ++conflict.source_arm.attempt_id;
    CHECK(authority.offer(conflict) == OfferDecision::Conflict);
    ++counters.phase_conflicts;
    CHECK(authority.offer(phase_offer(106)) == OfferDecision::StaleRequest);
    ++counters.phase_stale;
    emit_case("exact_phase_open_replay_and_rejection", counters);
}

ReverseFdAttempt reverse_attempt(uint64_t delivery_id, uint64_t token) {
    return ReverseFdAttempt{Identity{109, 113}, delivery_id, token, 100, source_arm()};
}

void case_adoption_lost_ack_and_reverse_replay() {
    CaseCounters counters;
    auto owner = ReverseFdOwner::stage(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("sealed-input"), 12),
        reverse_attempt(127, 131), ReverseFdOwner::Clock::now() + std::chrono::seconds(10));
    CHECK(owner.has_value() && owner->valid() && owner->cloexec());
    ReverseFdReceiverLedger ledger(137);
    const auto now = ReverseFdOwner::Clock::now();
    CHECK(ledger.arm_input(source_arm(), now, now + std::chrono::seconds(5)) ==
          ReverseFdDecision::Accepted);
    const int first_fd = ::fcntl(owner->master_fd(), F_DUPFD_CLOEXEC, 0);
    CHECK(first_fd >= 0 && (::fcntl(first_fd, F_GETFD) & FD_CLOEXEC) != 0);
    CHECK(ledger.accept(reverse_attempt(127, 131), HandoffFd(first_fd), now) ==
          ReverseFdDecision::Accepted);
    counters.adopted = 1;
    counters.lost_ack = 1;
    ++counters.reverse_accepts;
    const int replay_fd = ::fcntl(owner->master_fd(), F_DUPFD_CLOEXEC, 0);
    CHECK(replay_fd >= 0 && (::fcntl(replay_fd, F_GETFD) & FD_CLOEXEC) != 0);
    CHECK(ledger.accept(reverse_attempt(127, 131), HandoffFd(replay_fd), now) ==
          ReverseFdDecision::ExactReplay);
    ++counters.reverse_replays;
    ReverseFdAttempt conflict = reverse_attempt(127, 131);
    ++conflict.token;
    const int conflict_fd = ::fcntl(owner->master_fd(), F_DUPFD_CLOEXEC, 0);
    CHECK(conflict_fd >= 0);
    CHECK(ledger.accept(conflict, HandoffFd(conflict_fd), now) == ReverseFdDecision::Conflict);
    ++counters.reverse_conflicts;
    ReverseFdAttempt stale = reverse_attempt(126, 131);
    const int stale_fd = ::fcntl(owner->master_fd(), F_DUPFD_CLOEXEC, 0);
    CHECK(stale_fd >= 0);
    CHECK(ledger.accept(stale, HandoffFd(stale_fd), now) == ReverseFdDecision::Stale);
    ++counters.reverse_stale;
    CHECK(ledger.transition_count() == 1 && ledger.fork_count() == 1);
    counters.transitions = ledger.transition_count();
    counters.forks = ledger.fork_count();
    CHECK(ledger.take_for_compile().valid());
    counters.compile_cursor_takes = 1;
    CHECK(ledger.take_for_compile().valid() == false);
    owner->cancel();
    emit_case("adopted_fd_lost_ACK_exact_retry", counters);
}

void run_all() {
    case_before_ready();
    case_after_ready_before_handoff();
    case_restart_budget_exhaustion();
    case_lease_withdrawal_rotation();
    case_phase_replay();
    case_adoption_lost_ack_and_reverse_replay();
    std::cout << "HOLD real_C/F_CompileFile_wiring=not_in_this_precursor\n";
    std::cout << "HOLD wire_lost_ACK_process_kill=requires_product_receiver_injection\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::string_view(argv[1]) == "--s2-fake-child")
        return s2_fake_child(argv[2]);
    try {
        run_all();
        std::cout << "ok - bounded S2 restart/replay exit precursor\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
