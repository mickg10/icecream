#include "p50_daemon_sidecar_adapter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>

#include "services/digest128.h"

extern char **environ;

namespace icecc::p50::daemon {
namespace {

constexpr int64_t kMaximumTimeoutMilliseconds = 24 * 60 * 60 * 1000;
// Launch preparation is itself incremental (one syscall per turn), so a
// caller's short READY timeout must not expire halfway through the finite
// pre-fork descriptor plan.  This is one absolute setup deadline, not a
// per-phase renewal; post-fork exec/READY deadlines remain lifecycle-owned.
constexpr auto kMinimumLaunchSetupTimeout = std::chrono::milliseconds(1000);
// Private control phases that are intentionally kept in the adapter rather
// than hidden inside a connect/send/receive helper.  Phase 6 owns the one
// connect action after socket creation; phase 1 owns its later SO_ERROR
// observation.
constexpr uint8_t kAuthConnectPending = 6;
constexpr uint8_t kAuthClosePending = 7;

// Capture names are minted once when an exact cleanup phase is armed.  The
// name is carried across turns; no cleanup syscall is hidden in the minting
// operation and a failed allocation simply leaves the phase fail-closed.
std::atomic<uint64_t> g_cleanup_sequence{1};

// The launch plan is intentionally a finite sequence rather than a helper
// which performs setup in one call.  Each nonzero phase consumes one outer
// turn; the adapter never retries a fallible operation in an inner loop.
enum class OuterLaunchPhase : uint8_t {
    None = 0,
    MakeDirectory,
    ObserveDirectory,
    CreateListener,
    BindListener,
    ChmodListener,
    ListenListener,
    ObserveListener,
    CreateReadyPipe,
    ReadyReadFlags,
    ReadyReadSetFlags,
    CreateExecPipe,
    ExecReadFlags,
    ExecReadSetFlags,
    CreateGate,
    GateReadFlags,
    GateReadSetFlags,
    GateWriteFlags,
    GateWriteSetFlags,
    PrepareExec,
    Fork,
    CloseReadyWrite,
    CloseExecWrite,
    CloseGateRead,
    CloseListener,
    OpenPidfd,
    CaptureDomain,
    RegisterReaper,
    PermitChild,
    CloseGateWrite,
    AbortCloseReadyRead,
    AbortCloseReadyWrite,
    AbortCloseExecRead,
    AbortCloseExecWrite,
    AbortCloseGateRead,
    AbortCloseGateWrite,
    AbortCloseListener,
    AbortClosePidfd,
    AbortDisarmRegistration,
    AbortDone,
};

constexpr uint8_t launch_phase(OuterLaunchPhase phase) noexcept {
    return static_cast<uint8_t>(phase);
}

OuterLaunchPhase launch_phase(uint8_t phase) noexcept {
    return static_cast<OuterLaunchPhase>(phase);
}

enum class OuterCleanupStep : uint8_t {
    None = 0,
    OpenParent,
    StatTarget,
    CaptureTarget,
    StatCapture,
    UnlinkCapture,
    CloseParentSuccess,
    CloseParentFailure,
};

constexpr uint8_t cleanup_step(OuterCleanupStep step) noexcept {
    return static_cast<uint8_t>(step);
}

OuterCleanupStep cleanup_step(uint8_t step) noexcept {
    return static_cast<OuterCleanupStep>(step);
}

bool bounded_positive(std::chrono::milliseconds value) noexcept
{
    return value.count() > 0 && value.count() <= kMaximumTimeoutMilliseconds;
}

bool valid_id(uint64_t value) noexcept
{
    return value <= static_cast<uint64_t>(std::numeric_limits<uid_t>::max());
}

bool valid_gid(uint64_t value) noexcept
{
    return value <= static_cast<uint64_t>(std::numeric_limits<gid_t>::max());
}

bool has_nul(const std::string& value) noexcept
{
    return value.find('\0') != std::string::npos;
}

bool executable_file(const std::string& path) noexcept
{
    if (path.empty() || path.front() != '/' || has_nul(path))
        return false;
    struct stat info{};
    return ::stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
           ::access(path.c_str(), X_OK) == 0;
}

bool private_directory(const Config& config) noexcept
{
    struct stat info{};
    if (::lstat(config.runtime_directory.c_str(), &info) != 0 ||
        !S_ISDIR(info.st_mode) || info.st_uid != config.expected_daemon_uid ||
        info.st_gid != config.expected_daemon_gid ||
        (info.st_mode & 07777) != 0700)
        return false;
    return true;
}

bool owned_attempt_directory(const std::string& path, const Config& config) noexcept
{
    struct stat info{};
    return ::lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode) &&
           info.st_uid == config.expected_daemon_uid &&
           info.st_gid == config.expected_daemon_gid && (info.st_mode & 07777) == 0700;
}

void close_owned(int& fd) noexcept
{
    if (fd >= 0)
        (void)::close(fd);
    fd = -1;
}

bool set_cloexec(int fd, bool enabled) noexcept
{
    if (fd < 0)
        return false;
    int flags = ::fcntl(fd, F_GETFD);
    if (flags < 0)
        return false;
    const int wanted = enabled ? flags | FD_CLOEXEC : flags & ~FD_CLOEXEC;
    return wanted == flags || ::fcntl(fd, F_SETFD, wanted) == 0;
}

// One-syscall launch-plan primitives.  The outer plan uses these variants so
// a fallback fcntl sequence cannot hide behind pipe or socket creation.
bool make_pipe_once(int fds[2]) noexcept
{
    fds[0] = fds[1] = -1;
#if defined(__linux__) && defined(O_CLOEXEC)
    return ::pipe2(fds, O_CLOEXEC) == 0;
#else
    return ::pipe(fds) == 0;
#endif
}

bool make_gate_once(int fds[2]) noexcept
{
    fds[0] = fds[1] = -1;
    return ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0;
}

bool write_all_child(int fd, const void *data, size_t size) noexcept
{
    // All current child markers are four bytes and therefore atomic on a
    // pipe.  A partial/EINTR write is a setup failure; retrying here would
    // turn one outer launch into an unbounded hidden action loop.
    const ssize_t result = ::write(fd, data, size);
    return result == static_cast<ssize_t>(size);
}

bool read_gate(int fd) noexcept
{
    uint8_t byte = 0;
    const ssize_t result = ::read(fd, &byte, sizeof(byte));
    return result == static_cast<ssize_t>(sizeof(byte)) && byte == 0x50;
}

int open_pidfd(pid_t pid) noexcept
{
#if defined(__linux__) && defined(SYS_pidfd_open)
    // pidfd_open is one bounded launch/identity action.  EINTR is reported to
    // the reducer and retried only by a later lifecycle turn; this helper must
    // never hide an inner retry loop behind outer_prepare_launch().  The
    // descriptor is created after fork and is never inherited by the child,
    // so setting FD_CLOEXEC here would add a second hidden fcntl action.
    const long result = ::syscall(SYS_pidfd_open, pid, 0u);
    if (result >= 0 && result <= std::numeric_limits<int>::max())
        return static_cast<int>(result);
    return -1;
#else
    (void)pid;
    errno = ENOSYS;
    return -1;
#endif
}

class OuterKillDomainVerifier final : public sidecar::KillDomainVerifier {
public:
    ~OuterKillDomainVerifier() override
    {
        for (const Record& record : records_)
            if (record.pidfd >= 0)
                (void)::close(record.pidfd);
    }

    std::optional<sidecar::KillDomainLease> capture(pid_t pid,
                                                     pid_t pgid) noexcept override
    {
        if (pid <= 1 || pid != pgid)
            return std::nullopt;
        const int pidfd = open_pidfd(pid);
        if (pidfd < 0)
            return std::nullopt;
        const uint64_t serial = next_serial_ == 0 ? ++next_serial_ : next_serial_++;
        sidecar::KillDomainLease lease = issue(pid, pgid, serial);
        if (!lease.valid()) {
            (void)::close(pidfd);
            return std::nullopt;
        }
        try {
            records_.push_back(Record{std::move(lease), pid, pgid, pidfd});
        } catch (...) {
            (void)::close(pidfd);
            return std::nullopt;
        }
        return records_.back().lease;
    }

    bool proves_absent(const sidecar::KillDomainLease& lease, pid_t pid,
                       pid_t pgid,
                       const sidecar::LifecycleObservation& observation) const noexcept override
    {
        const auto record = std::find_if(
            records_.begin(), records_.end(), [&](const Record& candidate) {
                return candidate.pid == pid && candidate.pgid == pgid &&
                       candidate.lease == lease;
            });
        if (record == records_.end() || record->pidfd < 0 ||
            observation.group != sidecar::GroupObservation::Gone ||
            observation.observed_pgid != pgid)
            return false;
        // The outer group observation owns the single kill(-pgid, 0) probe
        // for this turn.  Keep this verifier callback pure: repeating an
        // fcntl()/kill() pair here would turn one reducer advance into hidden
        // extra syscalls.  The cgroup-v2 no-descendant/singleton census and
        // OUTPUT_QUIESCENCE_GRANTED proof remain an explicit integration
        // HOLD until the authoritative leaf owner is joined.
        return true;
    }

    std::optional<sidecar::KillDomainLease> lease_for(pid_t pid,
                                                       pid_t pgid) const noexcept
    {
        const auto record = std::find_if(
            records_.begin(), records_.end(), [&](const Record& candidate) {
                return candidate.pid == pid && candidate.pgid == pgid;
            });
        if (record == records_.end())
            return std::nullopt;
        return record->lease;
    }

private:
    struct Record {
        sidecar::KillDomainLease lease;
        pid_t pid = -1;
        pid_t pgid = -1;
        int pidfd = -1;
    };
    std::vector<Record> records_;
    uint64_t next_serial_ = 1;
};

sidecar::State advertisement_state(sidecar::LifecycleState state) noexcept
{
    switch (state) {
    case sidecar::LifecycleState::Ready:
        return sidecar::State::Ready;
    case sidecar::LifecycleState::DegradedLegacy:
        return sidecar::State::DegradedLegacy;
    case sidecar::LifecycleState::TerminatingGrace:
    case sidecar::LifecycleState::TerminatingKill:
    case sidecar::LifecycleState::ReapAndGroupCheck:
        return sidecar::State::Stopping;
    case sidecar::LifecycleState::Stopped:
        return sidecar::State::Stopped;
    case sidecar::LifecycleState::LaunchPrepared:
    case sidecar::LifecycleState::ForkedAwaitExecAndReady:
    case sidecar::LifecycleState::RetryEligible:
    case sidecar::LifecycleState::FailedClosed:
        return sidecar::State::Starting;
    }
    return sidecar::State::Stopped;
}

} // namespace

DaemonSidecarAdapter::DaemonSidecarAdapter(Config config) noexcept
    : config_(std::move(config))
{
    try {
        if (!config_.clock_identity.valid())
            config_.clock_identity = sidecar::process_monotonic_clock_identity();
        launch_identities_ = std::make_shared<sidecar::LaunchIdentityAllocator>(
            config_.generation, 1);
        outer_kill_domain_ = std::make_shared<OuterKillDomainVerifier>();
        sidecar::SidecarLifecycleConfig lifecycle_config;
        lifecycle_config.control_generation = config_.generation;
        lifecycle_config.private_root = config_.runtime_directory;
        lifecycle_config.launch_timeout = std::max(config_.readiness_timeout,
                                                   kMinimumLaunchSetupTimeout);
        lifecycle_config.exec_timeout = config_.readiness_timeout;
        lifecycle_config.ready_timeout = config_.readiness_timeout;
        lifecycle_config.grace_timeout = config_.shutdown_timeout;
        lifecycle_config.kill_timeout = config_.shutdown_timeout;
        // The outer reducer spends one allocator attempt per RetryEligible
        // turn.  max_attempts_per_recovery bounds the legacy synchronous API;
        // the live path must still have enough total identity slots to honor
        // the rolling restart budget and mint a fresh B after each exact A
        // retirement.  Keep the sum bounded before narrowing to uint32_t.
        const uint64_t outer_attempt_budget =
            std::min<uint64_t>(100000,
                               static_cast<uint64_t>(config_.max_restarts) +
                                   static_cast<uint64_t>(config_.max_attempts_per_recovery));
        lifecycle_config.max_attempts = static_cast<uint32_t>(
            std::max<uint64_t>(1, outer_attempt_budget));
        lifecycle_config.identities = launch_identities_;
        lifecycle_config.kill_domain_verifier = outer_kill_domain_;
        lifecycle_config.attempt_leaf_authority = config_.attempt_leaf_authority;
        outer_lifecycle_ = std::make_unique<sidecar::SidecarLifecycle>(
            std::move(lifecycle_config));
        outer_reaper_ = config_.central_reaper;
        outer_attempt_leaf_authority_ = config_.attempt_leaf_authority;
        // This is the only construction-time configuration/path check.  The
        // live reducer must not repeat lstat-backed valid_config() on every
        // daemon turn; changing attempt nodes are checked by their explicit
        // identity observations below.
        outer_config_valid_ = valid_config(config_);
    } catch (...) {
        launch_identities_.reset();
        outer_kill_domain_.reset();
        outer_lifecycle_.reset();
        outer_config_valid_ = false;
    }
}

DaemonSidecarAdapter::~DaemonSidecarAdapter()
{
    // The daemon must drive outer_request_shutdown() through its poll owner.
    // Destruction never sends a signal, waits, or performs emergency cleanup.
    outer_close_fds();
    outer_disarm_registration();
}

bool DaemonSidecarAdapter::valid_config(const Config& config) noexcept
{
    if (!executable_file(config.executable) ||
        config.runtime_directory.empty() ||
        config.runtime_directory.front() != '/' || has_nul(config.runtime_directory) ||
        config.runtime_directory.size() >= 180 ||
        config.generation == 0 ||
        config.public_listener_port > std::numeric_limits<uint16_t>::max() ||
        !valid_id(config.expected_daemon_uid) || !valid_gid(config.expected_daemon_gid) ||
        !valid_id(config.expected_service_uid) || !valid_gid(config.expected_service_gid) ||
        config.expected_daemon_uid != static_cast<uint64_t>(::geteuid()) ||
        config.expected_daemon_gid != static_cast<uint64_t>(::getegid()) ||
        config.expected_service_uid != config.expected_daemon_uid ||
        config.expected_service_gid != config.expected_daemon_gid ||
        !config.clock_identity.valid() ||
        !bounded_positive(config.readiness_timeout) ||
        !bounded_positive(config.connect_timeout) ||
        !bounded_positive(config.handoff_timeout) ||
        !bounded_positive(config.input_attachment_timeout) ||
        !bounded_positive(config.shutdown_timeout) ||
        !bounded_positive(config.restart_window) ||
        !bounded_positive(config.input_lifecycle_timeout) ||
        config.max_restarts == 0 ||
        config.max_restarts > 100000 || config.max_attempts_per_recovery == 0 ||
        config.max_attempts_per_recovery > 100000 ||
        config.max_pending_input_lifecycle == 0 ||
        config.max_pending_input_lifecycle > 100000 ||
        (config.drop_uid.has_value() != config.drop_gid.has_value()))
        return false;
    // The current local connector deliberately requires private pathname
    // ownership by the daemon's effective identity.  iceccd is deployed as
    // the unprivileged icecc user, so a distinct pre-bind drop would make a
    // valid SO_PEERCRED relationship unreachable.  Keep the explicit fields
    // for launch-argument compatibility but reject that impossible mode.
    if (config.drop_uid.has_value())
        return false;
    return private_directory(config);
}

void DaemonSidecarAdapter::observe_public_listener(bool bound, uint32_t port) noexcept
{
    public_listener_ = PublicListenerObservation{bound, port};
}

void DaemonSidecarAdapter::observe_public_listener(
    PublicListenerObservation observation) noexcept
{
    public_listener_ = observation;
}

bool DaemonSidecarAdapter::authenticated() const noexcept
{
    // A live outer incarnation is authenticated only after its incremental
    // HELLO/ACK/trailing-byte barrier.  The historical Supervisor branch has
    // no positive authority; a relationship is authenticated only when the
    // outer lifecycle has completed its retained control lease.
    return outer_authenticated_ && dispatcher_ != nullptr &&
           dispatcher_->available();
}

InputFdAttachmentResult DaemonSidecarAdapter::attach_input(
    InputRecordKey key, InputLeaseOwner owner, uint64_t request_id) noexcept
{
    InputFdAttachmentResult rejected_result;
    rejected_result.status = InputFdAttachmentStatus::InvalidArgument;
    if (key.c_store_guid == CStoreGuid{} ||
        !input_lease_owner_valid(owner) || request_id == 0)
        return rejected_result;

    rejected_result.status = InputFdAttachmentStatus::Disconnected;
    if (outer_lifecycle_ == nullptr ||
        outer_lifecycle_->state() != sidecar::LifecycleState::Ready ||
        !outer_authenticated_ || !outer_ready_lease_.has_value() ||
        !outer_ready_lease_->valid() || !runtime_nodes_valid())
        return rejected_result;

    const local::Identity identity{config_.generation,
                                   outer_ready_lease_->identity.attempt};
    // The service inherits a listener created before fork. Linux therefore
    // reports this daemon as the peer socket's creator; READY and the retained
    // pidfd separately bind the serving child to this exact incarnation.
    const local::CredentialExpectation expected{
        config_.expected_service_uid, config_.expected_service_gid,
        static_cast<uint64_t>(::getpid())};
    const auto deadline = std::chrono::steady_clock::now() +
                          config_.input_attachment_timeout;
    return InputFdAttachmentClient::attach(
        socket_path_, InputFdRequest{identity, key, owner, request_id},
        expected, deadline);
}

std::unique_ptr<InputFdAttachmentOperation> DaemonSidecarAdapter::begin_attach_input(
    InputRecordKey key, InputLeaseOwner owner, uint64_t request_id) noexcept
{
    if (key.c_store_guid == CStoreGuid{} || !input_lease_owner_valid(owner) ||
        request_id == 0 || outer_lifecycle_ == nullptr ||
        outer_lifecycle_->state() != sidecar::LifecycleState::Ready ||
        !outer_authenticated_ || !outer_ready_lease_.has_value() ||
        !outer_ready_lease_->valid() || !runtime_nodes_valid())
        return nullptr;
    const local::Identity identity{config_.generation,
                                   outer_ready_lease_->identity.attempt};
    const local::CredentialExpectation expected{
        config_.expected_service_uid, config_.expected_service_gid,
        static_cast<uint64_t>(::getpid())};
    try {
        return std::make_unique<InputFdAttachmentOperation>(
            socket_path_, InputFdRequest{identity, key, owner, request_id}, expected,
            std::chrono::steady_clock::now() + config_.input_attachment_timeout);
    } catch (...) {
        return nullptr;
    }
}

bool DaemonSidecarAdapter::next_input_lifecycle_operation(
    uint64_t& operation_id) noexcept
{
    if (next_input_lifecycle_operation_id_ == 0) {
        fail(AdapterError::InputLifecycleOperationExhausted);
        return false;
    }
    operation_id = next_input_lifecycle_operation_id_;
    if (next_input_lifecycle_operation_id_ == std::numeric_limits<uint64_t>::max())
        next_input_lifecycle_operation_id_ = 0;
    else
        ++next_input_lifecycle_operation_id_;
    return true;
}

InputLifecycleResult DaemonSidecarAdapter::apply_input_lifecycle(
    const InputFdRequest& lease, InputLifecycleAction action) noexcept
{
    InputLifecycleRequest request;
    request.identity = lease.identity;
    request.key = lease.key;
    request.owner = lease.owner;
    request.action = action;
    request.deadline = std::chrono::steady_clock::now() + config_.input_lifecycle_timeout;
    if (lease.identity.generation != config_.generation ||
        lease.identity.attempt == 0 || lease.key.c_store_guid == CStoreGuid{} ||
        !input_lease_owner_valid(lease.owner) || lease.request_id == 0 ||
        !input_lifecycle_action_valid(action))
        return InputLifecycleResult{InputLifecycleStatus::InvalidArgument, request};
    if (action == InputLifecycleAction::PrepareAttemptRetirement ||
        action == InputLifecycleAction::CommitAttemptReplacement ||
        action == InputLifecycleAction::CloseLogicalInputLease)
        return InputLifecycleResult{InputLifecycleStatus::RetirementProofRequired,
                                    request};
    if (!next_input_lifecycle_operation(request.operation_id)) {
        return InputLifecycleResult{InputLifecycleStatus::StoreReplaced,
                                    request};
    }
    // Legacy callers may still submit attempt-scoped cancel commands through
    // this ABI, but the operation is always queued for the daemon outer loop.
    // There is no synchronous InputLifecycleClient::apply() fallback and no
    // Supervisor-owned transport path.  Extended PREPARE/COMMIT/CLOSE requests
    // must use the typed methods below so their allocator-bound F identity and
    // retirement proof cannot be omitted.
    return queue_outer_input_request(std::move(request));
}

InputLifecycleResult DaemonSidecarAdapter::queue_outer_input_request(
    InputLifecycleRequest request) noexcept {
    const InputLifecycleResult invalid{InputLifecycleStatus::InvalidArgument, request};
    const bool retirement_action =
        request.action == InputLifecycleAction::PrepareAttemptRetirement ||
        request.action == InputLifecycleAction::CommitAttemptReplacement ||
        request.action == InputLifecycleAction::CloseLogicalInputLease;
    if (request.identity.generation != config_.generation ||
        request.identity.attempt == 0 || request.operation_id == 0 ||
        request.key.c_store_guid == CStoreGuid{} ||
        !input_lease_owner_valid(request.owner) ||
        !input_lifecycle_action_valid(request.action) ||
        request.deadline == std::chrono::steady_clock::time_point{} ||
        (retirement_action &&
         (!request.absolute_deadline.valid() ||
          !request.absolute_deadline.matches_clock(config_.clock_identity) ||
          request.deadline != request.absolute_deadline.as_steady_time_point())) ||
        request.deadline <= std::chrono::steady_clock::now())
        return invalid;
    if (request.identity.attempt != attempt_)
        // The retired synchronous position->identity.attempt < attempt_ rule
        // is now enforced at this typed outer-queue boundary; a stale A can
        // never be rebound to the current B lease.
        return InputLifecycleResult{
            request.identity.attempt < attempt_ ? InputLifecycleStatus::StoreReplaced
                                                 : InputLifecycleStatus::StaleIdentity,
            request};
    if (outer_lifecycle_ == nullptr ||
        outer_lifecycle_->state() != sidecar::LifecycleState::Ready ||
        !outer_ready_lease_.has_value() || !outer_ready_lease_->valid() ||
        !outer_authenticated_ || !runtime_nodes_valid() ||
        request.identity !=
            local::Identity{config_.generation, outer_ready_lease_->identity.attempt})
        return InputLifecycleResult{InputLifecycleStatus::StoreReplaced, request};
    if (retirement_action &&
        (request.f_store_generation != outer_ready_lease_->store_generation ||
         request.f_store_guid != outer_ready_lease_->f_store_guid ||
         request.key.c_store_guid != outer_ready_lease_->c_store_guid))
        return InputLifecycleResult{InputLifecycleStatus::GenerationMismatch, request};

    const auto same_operation = [&](const InputLifecycleRequest& candidate) {
        return candidate.identity == request.identity &&
               candidate.key == request.key && candidate.owner == request.owner &&
               candidate.operation_id == request.operation_id &&
               candidate.action == request.action &&
               candidate.f_store_generation == request.f_store_generation &&
               candidate.f_store_guid == request.f_store_guid &&
               candidate.immutable_size == request.immutable_size &&
               candidate.immutable_digest == request.immutable_digest &&
               candidate.retirement_id == request.retirement_id &&
               candidate.replacement_owner == request.replacement_owner &&
               candidate.deadline == request.deadline &&
               candidate.absolute_deadline == request.absolute_deadline;
    };
    // An operation id is a replay key, not an allocation hint.  A second
    // request carrying that id must either be byte-for-byte identical (and
    // receive the same queued/completed result) or be rejected locally before
    // two different mutations can reach the sidecar owner.
    const auto conflicting_operation_id = [&](const InputLifecycleRequest& candidate) {
        return candidate.operation_id == request.operation_id &&
               !same_operation(candidate);
    };
    if (std::any_of(completed_input_lifecycle_.begin(),
                    completed_input_lifecycle_.end(), conflicting_operation_id) ||
        std::any_of(pending_input_lifecycle_.begin(),
                    pending_input_lifecycle_.end(), conflicting_operation_id) ||
        (outer_input_request_.has_value() &&
         conflicting_operation_id(*outer_input_request_)))
        return InputLifecycleResult{InputLifecycleStatus::ConflictingReplay, request};
    if (const auto completed = std::find_if(
            completed_input_lifecycle_.begin(), completed_input_lifecycle_.end(),
            same_operation); completed != completed_input_lifecycle_.end())
        return InputLifecycleResult{InputLifecycleStatus::AlreadyApplied, *completed};
    if (const auto pending = std::find_if(
            pending_input_lifecycle_.begin(), pending_input_lifecycle_.end(),
            same_operation); pending != pending_input_lifecycle_.end())
        return InputLifecycleResult{InputLifecycleStatus::Disconnected, *pending};
    if (outer_input_request_.has_value() && same_operation(*outer_input_request_))
        return InputLifecycleResult{InputLifecycleStatus::Disconnected,
                                    *outer_input_request_};
    if (pending_input_lifecycle_.size() >= config_.max_pending_input_lifecycle) {
        retire_input_lifecycle_relationship(AdapterError::InputLifecycleCapacity);
        return InputLifecycleResult{InputLifecycleStatus::StoreReplaced, request};
    }
    try {
        pending_input_lifecycle_.push_back(std::move(request));
    } catch (...) {
        retire_input_lifecycle_relationship(AdapterError::InputLifecycleCapacity);
        return InputLifecycleResult{InputLifecycleStatus::StoreReplaced, request};
    }
    return InputLifecycleResult{InputLifecycleStatus::Disconnected,
                                pending_input_lifecycle_.back()};
}

InputLifecycleResult DaemonSidecarAdapter::outer_prepare_attempt_retirement(
    RemoteInputLeaseBinding binding,
    const sidecar::AbsoluteMonotonicDeadline& original_deadline) noexcept {
    InputLifecycleRequest request;
    request.identity = binding.identity;
    request.key = binding.key;
    request.owner = binding.owner;
    request.operation_id = binding.operation_id;
    request.action = InputLifecycleAction::PrepareAttemptRetirement;
    request.f_store_generation = binding.f_store_generation;
    request.f_store_guid = binding.f_store_guid;
    request.immutable_size = binding.immutable_size;
    request.immutable_digest = binding.immutable_digest;
    request.retirement_id = binding.retirement_id;
    request.absolute_deadline = original_deadline;
    request.deadline = original_deadline.as_steady_time_point();
    if (!binding.valid() || binding.identity.generation != config_.generation)
        return InputLifecycleResult{InputLifecycleStatus::InvalidArgument, request};
    // PREPARE is admitted only against the currently published allocator
    // tuple.  In particular, a syntactically valid stale A binding must not
    // reach the sidecar and must never be silently rebound to B.
    if (outer_lifecycle_ == nullptr ||
        outer_lifecycle_->state() != sidecar::LifecycleState::Ready ||
        !outer_ready_lease_.has_value() || !outer_ready_lease_->valid())
        return InputLifecycleResult{InputLifecycleStatus::StoreReplaced, request};
    const sidecar::ReadyLease& lease = *outer_ready_lease_;
    if (binding.identity != lease.identity ||
        binding.f_store_generation != lease.store_generation ||
        binding.f_store_guid != lease.f_store_guid ||
        binding.key.c_store_guid != lease.c_store_guid)
        return InputLifecycleResult{InputLifecycleStatus::GenerationMismatch, request};
    return queue_outer_input_request(std::move(request));
}

InputLifecycleResult DaemonSidecarAdapter::outer_commit_attempt_replacement(
    RemoteInputLeaseBinding binding, InputLeaseOwner replacement_owner,
    const InputRetirementProof& proof,
    const sidecar::AbsoluteMonotonicDeadline& original_deadline) noexcept {
    InputLifecycleRequest request;
    request.identity = binding.identity;
    request.key = binding.key;
    request.owner = binding.owner;
    request.operation_id = binding.operation_id;
    request.action = InputLifecycleAction::CommitAttemptReplacement;
    request.f_store_generation = binding.f_store_generation;
    request.f_store_guid = binding.f_store_guid;
    request.immutable_size = binding.immutable_size;
    request.immutable_digest = binding.immutable_digest;
    request.retirement_id = binding.retirement_id;
    request.replacement_owner = replacement_owner;
    request.absolute_deadline = original_deadline;
    request.deadline = original_deadline.as_steady_time_point();
    if (!binding.valid() || !proof.complete() || proof.binding != binding ||
        !input_lease_owner_valid(replacement_owner) ||
        replacement_owner.logical_job != binding.owner.logical_job ||
        replacement_owner == binding.owner)
        return InputLifecycleResult{InputLifecycleStatus::RetirementProofRequired,
                                    request};
    if (outer_lifecycle_ == nullptr ||
        outer_lifecycle_->state() != sidecar::LifecycleState::Ready ||
        !outer_ready_lease_.has_value() || !outer_ready_lease_->valid())
        return InputLifecycleResult{InputLifecycleStatus::StoreReplaced, request};
    const sidecar::ReadyLease& lease = *outer_ready_lease_;
    if (binding.identity != lease.identity ||
        binding.f_store_generation != lease.store_generation ||
        binding.f_store_guid != lease.f_store_guid ||
        binding.key.c_store_guid != lease.c_store_guid)
        return InputLifecycleResult{InputLifecycleStatus::GenerationMismatch, request};
    return queue_outer_input_request(std::move(request));
}

InputLifecycleResult DaemonSidecarAdapter::outer_close_logical_input_lease(
    RemoteInputLeaseBinding binding,
    const sidecar::AbsoluteMonotonicDeadline& original_deadline) noexcept {
    InputLifecycleRequest request;
    request.identity = binding.identity;
    request.key = binding.key;
    request.owner = binding.owner;
    request.operation_id = binding.operation_id;
    request.action = InputLifecycleAction::CloseLogicalInputLease;
    request.f_store_generation = binding.f_store_generation;
    request.f_store_guid = binding.f_store_guid;
    request.immutable_size = binding.immutable_size;
    request.immutable_digest = binding.immutable_digest;
    request.retirement_id = binding.retirement_id;
    request.absolute_deadline = original_deadline;
    request.deadline = original_deadline.as_steady_time_point();
    if (!binding.valid())
        return InputLifecycleResult{InputLifecycleStatus::InvalidArgument, request};
    if (outer_lifecycle_ == nullptr ||
        outer_lifecycle_->state() != sidecar::LifecycleState::Ready ||
        !outer_ready_lease_.has_value() || !outer_ready_lease_->valid())
        return InputLifecycleResult{InputLifecycleStatus::StoreReplaced, request};
    const sidecar::ReadyLease& lease = *outer_ready_lease_;
    if (binding.identity != lease.identity ||
        binding.f_store_generation != lease.store_generation ||
        binding.f_store_guid != lease.f_store_guid ||
        binding.key.c_store_guid != lease.c_store_guid)
        return InputLifecycleResult{InputLifecycleStatus::GenerationMismatch, request};
    return queue_outer_input_request(std::move(request));
}

bool DaemonSidecarAdapter::remember_completed_input_lifecycle(
    const InputLifecycleRequest& request) noexcept
{
    try {
        // Completed semantic deduplication is a bounded convenience on top of
        // the sidecar's exact operation replay table.  Evict the oldest proof
        // instead of forcing a healthy store restart after a fixed TU count.
        if (completed_input_lifecycle_.size() >=
            config_.max_pending_input_lifecycle)
            completed_input_lifecycle_.erase(
                completed_input_lifecycle_.begin());
        completed_input_lifecycle_.push_back(request);
        return true;
    } catch (...) {
        return false;
    }
}

void DaemonSidecarAdapter::retire_input_lifecycle_relationship(
    AdapterError error) noexcept
{
    // Input-lifecycle failure is another reducer observation.  It withdraws
    // eligibility immediately, then lets the normal lifecycle turns prove
    // exact child/group/path teardown.  In particular, this path never calls
    // Supervisor::shutdown, waits, or performs name-based emergency cleanup.
    fail(error);
    outer_input_failure_ = true;
    // Route the failure through the same A-retirement reducer used by
    // scheduler/runtime loss.  The relationship is withdrawn now, while the
    // input/control descriptors remain owned until later outer turns fence
    // and close them exactly once.
    outer_replacement_requested_ = true;
    outer_replacement_input_close_pending_ = true;
    disable_relationship();
    outer_authenticated_ = false;
    outer_auth_start_pending_ = false;
    outer_input_cancel_lease_.reset();
    pending_input_lifecycle_.clear();
    completed_input_lifecycle_.clear();
    advertisement::Update withdrawal;
    outer_observe(withdrawal);
    append_update(pending_advertisement_update_, withdrawal);
}

bool DaemonSidecarAdapter::runtime_nodes_valid() const noexcept
{
    if (!private_directory(config_) || attempt_directory_.empty() || socket_path_.empty())
        return false;

    struct stat directory_info{};
    if (!owned_attempt_directory(attempt_directory_, config_) ||
        ::lstat(attempt_directory_.c_str(), &directory_info) != 0 ||
        directory_info.st_dev != attempt_directory_device_ ||
        directory_info.st_ino != attempt_directory_inode_)
        return false;

    struct stat socket_info{};
    return ::lstat(socket_path_.c_str(), &socket_info) == 0 &&
           S_ISSOCK(socket_info.st_mode) &&
           socket_info.st_uid == config_.expected_service_uid &&
           socket_info.st_gid == config_.expected_service_gid &&
           (socket_info.st_mode & 07777) == 0600 &&
           socket_info.st_dev == socket_device_ &&
           socket_info.st_ino == socket_inode_;
}

void DaemonSidecarAdapter::disable_relationship() noexcept
{
    if (dispatcher_ != nullptr)
        dispatcher_->disable();
}

bool DaemonSidecarAdapter::reserve_outer_restart() noexcept
{
    const auto now = std::chrono::steady_clock::now();
    try {
        restart_times_.erase(std::remove_if(restart_times_.begin(), restart_times_.end(),
                                             [&](auto timestamp) {
                                                 return now - timestamp >= config_.restart_window;
                                             }),
                            restart_times_.end());
        if (restart_times_.size() >= config_.max_restarts)
            return false;
        restart_times_.push_back(now);
        return true;
    } catch (...) {
        // outer_begin_turn() is noexcept and is itself the allocator/restart
        // reducer boundary.  A failed budget mutation must fail closed in
        // that reducer, never terminate the daemon or mint an untracked B.
        return false;
    }
}

void DaemonSidecarAdapter::append_update(advertisement::Update& destination,
                                          const advertisement::Update& source) noexcept
{
    for (size_t index = 0; index < source.count && destination.count < 2; ++index)
        destination.transitions[destination.count++] = source.transitions[index];
    if (destination.error == advertisement::Error::None)
        destination.error = source.error;
}

void DaemonSidecarAdapter::append_pending_advertisement_update(
    advertisement::Update& destination) noexcept
{
    append_update(destination, pending_advertisement_update_);
    pending_advertisement_update_ = advertisement::Update{};
}

void DaemonSidecarAdapter::fail(AdapterError error) noexcept
{
    last_error_ = error;
    if (error != AdapterError::None)
        state_ = AdapterState::Absent;
}

void DaemonSidecarAdapter::shutdown(advertisement::Update* result) noexcept
{
    // Shutdown is itself an outer-loop request.  No descriptor, process, or
    // path is closed here, even when the old ABI is called accidentally.
    // In particular, the historical synchronous supervisor shutdown
    // operation is intentionally absent: the daemon outer loop owns every close/reap
    // turn and the central registry remains the sole wait-status consumer.
    outer_request_shutdown(result);
}

const std::optional<sidecar::ReadyLease>&
DaemonSidecarAdapter::outer_current_ready_lease() const noexcept
{
    return outer_ready_lease_;
}

pid_t DaemonSidecarAdapter::outer_child_pid() const noexcept
{
    return outer_pid_;
}

sidecar::LifecycleState DaemonSidecarAdapter::outer_lifecycle_state() const noexcept
{
    return outer_lifecycle_ != nullptr ? outer_lifecycle_->state()
                                       : sidecar::LifecycleState::Stopped;
}

bool DaemonSidecarAdapter::outer_shutdown_complete() const noexcept
{
    if (!outer_shutdown_requested_)
        return false;
    const auto lifecycle_state = outer_lifecycle_state();
    // FailedClosed is a diagnostic terminal state, not proof that the
    // process/group/path was retired.  In particular, the reducer deliberately
    // keeps the exact identity when the bounded teardown deadline expires.
    // Never let the daemon mistake that state for a completed shutdown: doing
    // so would drop the central registration and make a live child/path
    // invisible to the only reaper authority.
    const bool terminal = lifecycle_state == sidecar::LifecycleState::Stopped ||
                          lifecycle_state == sidecar::LifecycleState::DegradedLegacy;
    return terminal && outer_cleanup_phase_ == 0 && outer_cleanup_step_ == 0 &&
           outer_cleanup_parent_fd_ < 0 && outer_cleanup_capture_name_.empty() &&
           outer_auth_fd_ < 0 &&
           !outer_auth_start_pending_ &&
           !outer_shutdown_input_close_pending_ &&
           outer_ready_fd_ < 0 && outer_exec_fd_ < 0 && outer_gate_fd_ < 0 &&
           outer_launch_listener_fd_ < 0 &&
           outer_launch_ready_write_fd_ < 0 &&
           outer_launch_exec_write_fd_ < 0 &&
           outer_launch_gate_read_fd_ < 0 && outer_launch_phase_ == 0 &&
           outer_pidfd_ < 0 && outer_pid_ <= 1 && !outer_registration_.valid() &&
           outer_input_operation_ == nullptr && outer_input_request_ == std::nullopt &&
           pending_input_lifecycle_.empty() && outer_directory_path_.empty() &&
           outer_directory_device_ == 0 && outer_directory_inode_ == 0 &&
           outer_listener_device_ == 0 && outer_listener_inode_ == 0;
}

std::optional<InputLifecycleResult>
DaemonSidecarAdapter::take_outer_input_lifecycle_result() noexcept
{
    std::optional<InputLifecycleResult> result =
        std::move(outer_last_input_lifecycle_result_);
    outer_last_input_lifecycle_result_.reset();
    return result;
}

std::chrono::steady_clock::time_point
DaemonSidecarAdapter::outer_input_next_deadline() const noexcept
{
    return outer_input_operation_ != nullptr
               ? outer_input_operation_->deadline()
               : std::chrono::steady_clock::time_point{};
}

std::optional<InputLifecycleOperationLease>
DaemonSidecarAdapter::outer_input_operation_lease() const noexcept
{
    const InputLifecycleRequest* request = nullptr;
    if (outer_input_request_.has_value())
        request = &*outer_input_request_;
    else if (!pending_input_lifecycle_.empty())
        request = &pending_input_lifecycle_.front();
    if (request == nullptr)
        return std::nullopt;
    const InputLifecycleOperationLease lease =
        make_input_lifecycle_operation_lease(*request);
    if (!lease.valid())
        return std::nullopt;
    return lease;
}

std::optional<sidecar::AbsoluteMonotonicDeadline>
DaemonSidecarAdapter::outer_input_absolute_deadline() const noexcept
{
    const auto lease = outer_input_operation_lease();
    if (!lease.has_value() || !lease->absolute_deadline.valid())
        return std::nullopt;
    return lease->absolute_deadline;
}

bool DaemonSidecarAdapter::outer_cancel_input_operation(
    const InputLifecycleOperationLease& lease) noexcept
{
    if (!lease.valid())
        return false;
    const auto current = outer_input_operation_lease();
    if (!current.has_value() || *current != lease)
        return false;
    // Repeating the exact cancellation post is idempotent.  A different
    // lease (including the same operation id with a renewed deadline) cannot
    // mutate this role slot.
    if (outer_input_cancel_lease_.has_value())
        return *outer_input_cancel_lease_ == lease;
    try {
        outer_input_cancel_lease_ = lease;
    } catch (...) {
        return false;
    }
    return true;
}

std::optional<sidecar::OutputQuiescenceGranted>
DaemonSidecarAdapter::outer_grant_output_quiescence(
    const sidecar::AttemptControlBinding& control,
    const sidecar::AttemptLeafCensus& census,
    const sidecar::LocalDrainedObservation& drained,
    const sidecar::AbsoluteMonotonicDeadline& original_deadline) const noexcept {
    if (!outer_lifecycle_ || !outer_attempt_leaf_authority_ ||
        !control.valid() || !census.valid() || !drained.valid() ||
        !original_deadline.matches_clock(config_.clock_identity))
        return std::nullopt;
    return outer_lifecycle_->grant_output_quiescence(
        control, census, drained, original_deadline);
}

std::optional<sidecar::AttemptLeafRetirementJoin>
DaemonSidecarAdapter::outer_join_attempt_leaf_retirement(
    sidecar::OutputQuiescenceGranted quiescence,
    sidecar::DirectWorkerStatusConsumed direct_worker_status,
    bool empty_after_population, bool leaf_cleanup_complete,
    std::chrono::steady_clock::time_point now) const noexcept {
    if (!outer_lifecycle_ || !outer_attempt_leaf_authority_ ||
        !quiescence.valid() ||
        !quiescence.original_deadline().matches_clock(config_.clock_identity))
        return std::nullopt;
    return outer_lifecycle_->join_attempt_leaf_retirement(
        std::move(quiescence), std::move(direct_worker_status),
        empty_after_population, leaf_cleanup_complete, now);
}

InputLifecycleStatus DaemonSidecarAdapter::map_input_control_status(
    local::DaemonControlStatus status) noexcept
{
    switch (status) {
    case local::DaemonControlStatus::Complete:
        return InputLifecycleStatus::Applied;
    case local::DaemonControlStatus::Timeout:
        return InputLifecycleStatus::Timeout;
    case local::DaemonControlStatus::Disconnected:
    case local::DaemonControlStatus::Truncated:
        return InputLifecycleStatus::Disconnected;
    case local::DaemonControlStatus::CredentialFailure:
        return InputLifecycleStatus::PeerUnauthenticated;
    case local::DaemonControlStatus::IdentityMismatch:
    case local::DaemonControlStatus::OperationMismatch:
    case local::DaemonControlStatus::Malformed:
    case local::DaemonControlStatus::ControlTruncated:
    case local::DaemonControlStatus::TrailingData:
    case local::DaemonControlStatus::ExtraFd:
    case local::DaemonControlStatus::MissingFd:
        return InputLifecycleStatus::MalformedResponse;
    case local::DaemonControlStatus::Idle:
    case local::DaemonControlStatus::InProgress:
    case local::DaemonControlStatus::InvalidArgument:
    case local::DaemonControlStatus::IoError:
        return InputLifecycleStatus::HandshakeFailed;
    }
    return InputLifecycleStatus::HandshakeFailed;
}

void DaemonSidecarAdapter::outer_close_fds() noexcept
{
    close_owned(outer_ready_fd_);
    close_owned(outer_exec_fd_);
    close_owned(outer_gate_fd_);
    close_owned(outer_launch_listener_fd_);
    close_owned(outer_launch_ready_write_fd_);
    close_owned(outer_launch_exec_write_fd_);
    close_owned(outer_launch_gate_read_fd_);
    close_owned(outer_auth_fd_);
    close_owned(outer_pidfd_);
    close_owned(outer_cleanup_parent_fd_);
    outer_launch_phase_ = launch_phase(OuterLaunchPhase::None);
    outer_launch_identity_valid_ = false;
    outer_launch_setup_failed_ = false;
    outer_launch_fcntl_flags_ = -1;
    outer_launch_deadline_ = {};
    outer_launch_environment_storage_.clear();
    outer_launch_environment_.clear();
    outer_launch_argv_storage_.clear();
    outer_launch_argv_.clear();
    outer_auth_phase_ = 0;
    outer_auth_expected_ = 0;
    outer_auth_offset_ = 0;
    outer_auth_read_ = 0;
    outer_auth_write_.clear();
    outer_auth_read_buffer_.clear();
    outer_launch_failure_path_pending_ = false;
}

void DaemonSidecarAdapter::outer_disarm_registration() noexcept
{
    outer_registration_.reset();
}

bool DaemonSidecarAdapter::outer_prepare_launch(
    const sidecar::LifecycleIdentity& identity,
    std::chrono::steady_clock::time_point now) noexcept
{
    // A live outer incarnation cannot operate without the daemon's central
    // exact-pidfd registry.  Keeping a null-reaper compatibility mode would
    // leave a child wait status with no positive owner and turn shutdown into
    // an unprovable numeric-PID fallback.
    if (!identity.valid() || outer_launch_started_ || outer_launch_phase_ != 0 ||
        outer_pid_ > 1 || outer_reaper_ == nullptr)
        return false;
    try {
        outer_launch_identity_ = identity;
        outer_launch_identity_valid_ = true;
        outer_directory_path_ = identity.private_directory;
        socket_path_ = outer_directory_path_ + "/cache.sock";
        attempt_directory_ = outer_directory_path_;
        outer_directory_device_ = outer_directory_inode_ = 0;
        outer_listener_device_ = outer_listener_inode_ = 0;
        attempt_directory_device_ = attempt_directory_inode_ = 0;
        socket_device_ = socket_inode_ = 0;
        outer_group_domain_.reset();
        outer_group_gone_observed_ = false;
        outer_group_gone_domain_ = {};
        close_owned(outer_cleanup_parent_fd_);
        outer_cleanup_phase_ = 0;
        outer_cleanup_step_ = cleanup_step(OuterCleanupStep::None);
        outer_cleanup_directory_target_ = false;
        outer_cleanup_capture_name_.clear();
        outer_cleanup_waiting_path_observation_ = false;
        outer_launch_failure_path_pending_ = false;
        outer_launch_setup_failed_ = false;
        outer_launch_fcntl_flags_ = -1;
        outer_launch_deadline_ = now + std::max(config_.readiness_timeout,
                                                kMinimumLaunchSetupTimeout);
        close_owned(outer_launch_listener_fd_);
        close_owned(outer_launch_ready_write_fd_);
        close_owned(outer_launch_exec_write_fd_);
        close_owned(outer_launch_gate_read_fd_);
        outer_launch_environment_storage_.clear();
        outer_launch_environment_.clear();
        outer_launch_argv_storage_.clear();
        outer_launch_argv_.clear();
        // The first outer turn is only the plan admission.  The next step is
        // one mkdir(2), performed by outer_advance_launch_step().
        outer_launch_phase_ = launch_phase(OuterLaunchPhase::MakeDirectory);
        return true;
    } catch (...) {
        outer_launch_identity_valid_ = false;
        outer_launch_phase_ = launch_phase(OuterLaunchPhase::AbortDone);
        return false;
    }
}

void DaemonSidecarAdapter::outer_finish_launch_failure() noexcept
{
    outer_launch_phase_ = launch_phase(OuterLaunchPhase::None);
    outer_launch_identity_valid_ = false;
    outer_launch_setup_failed_ = false;
    outer_launch_deadline_ = {};
    outer_launch_failed_ = true;
    // The exec/setup failure observation and the path-absence observation are
    // intentionally separate reducer inputs.  The former is consumed by the
    // next reducer turn; the latter must remain pending until every captured
    // cleanup phase has completed, so a pre-fork failure cannot strand the
    // reducer in LaunchPrepared.
    outer_launch_failure_path_pending_ = !outer_directory_path_.empty();
    // Keep only the exact path identity captured before the setup failure.
    // The normal reducer turn will first bind/reap any child (if fork had
    // already happened) and only then arm identity-checked path cleanup.
    if (outer_pid_ <= 1) {
        outer_cleanup_waiting_path_observation_ = false;
        if (outer_listener_inode_ != 0)
            outer_arm_cleanup(1);
        else if (outer_directory_inode_ != 0)
            outer_arm_cleanup(2);
        outer_cleanup_waiting_path_observation_ = outer_cleanup_phase_ != 0;
    }
}

bool DaemonSidecarAdapter::outer_advance_launch_step() noexcept
{
    const OuterLaunchPhase phase = launch_phase(outer_launch_phase_);
    if (phase == OuterLaunchPhase::None)
        return false;
    auto fail_setup = [&]() noexcept {
        outer_launch_setup_failed_ = true;
        outer_launch_phase_ = launch_phase(OuterLaunchPhase::AbortCloseReadyRead);
    };
    auto next = [&](OuterLaunchPhase value) noexcept {
        outer_launch_phase_ = launch_phase(value);
    };

    switch (phase) {
    case OuterLaunchPhase::MakeDirectory:
        if (::mkdir(outer_directory_path_.c_str(), 0700) != 0) {
            if (errno == EINTR) {
                next(OuterLaunchPhase::ObserveDirectory);
                return true;
            }
            fail_setup();
            return true;
        }
        next(OuterLaunchPhase::ObserveDirectory);
        return true;
    case OuterLaunchPhase::ObserveDirectory: {
        struct stat info{};
        if (::lstat(outer_directory_path_.c_str(), &info) != 0) {
            if (errno == EINTR)
                return true;
            fail_setup();
            return true;
        }
        if (!S_ISDIR(info.st_mode) || info.st_uid != config_.expected_daemon_uid ||
            info.st_gid != config_.expected_daemon_gid ||
            (info.st_mode & 07777) != 0700) {
            fail_setup();
            return true;
        }
        outer_directory_device_ = info.st_dev;
        outer_directory_inode_ = info.st_ino;
        attempt_directory_device_ = info.st_dev;
        attempt_directory_inode_ = info.st_ino;
        next(OuterLaunchPhase::CreateListener);
        return true;
    }
    case OuterLaunchPhase::CreateListener: {
        if (socket_path_.size() >= sizeof(sockaddr_un::sun_path)) {
            fail_setup();
            return true;
        }
        int socket_type = SOCK_STREAM;
#ifdef SOCK_CLOEXEC
        socket_type |= SOCK_CLOEXEC;
#endif
        const int fd = ::socket(AF_UNIX, socket_type, 0);
        if (fd < 0) {
            if (errno == EINTR)
                return true;
            fail_setup();
            return true;
        }
        outer_launch_listener_fd_ = fd;
        next(OuterLaunchPhase::BindListener);
        return true;
    }
    case OuterLaunchPhase::BindListener: {
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, socket_path_.data(), socket_path_.size());
        address.sun_path[socket_path_.size()] = '\0';
        if (::bind(outer_launch_listener_fd_, reinterpret_cast<sockaddr*>(&address),
                   static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) +
                                          socket_path_.size() + 1)) != 0) {
            fail_setup();
            return true;
        }
        next(OuterLaunchPhase::ChmodListener);
        return true;
    }
    case OuterLaunchPhase::ChmodListener:
        if (::chmod(socket_path_.c_str(), 0600) != 0) {
            fail_setup();
            return true;
        }
        next(OuterLaunchPhase::ListenListener);
        return true;
    case OuterLaunchPhase::ListenListener:
        // A single C sidecar serves twenty relationships with two F slots;
        // its inherited listener must absorb the corresponding control
        // connection burst without refusing a valid session.
        if (::listen(outer_launch_listener_fd_, 64) != 0) {
            fail_setup();
            return true;
        }
        next(OuterLaunchPhase::ObserveListener);
        return true;
    case OuterLaunchPhase::ObserveListener: {
        struct stat info{};
        if (::lstat(socket_path_.c_str(), &info) != 0) {
            if (errno == EINTR)
                return true;
            fail_setup();
            return true;
        }
        if (!S_ISSOCK(info.st_mode) || info.st_uid != config_.expected_daemon_uid ||
            info.st_gid != config_.expected_daemon_gid ||
            (info.st_mode & 07777) != 0600 || info.st_dev == 0 || info.st_ino == 0) {
            fail_setup();
            return true;
        }
        outer_listener_device_ = info.st_dev;
        outer_listener_inode_ = info.st_ino;
        socket_device_ = info.st_dev;
        socket_inode_ = info.st_ino;
        if (outer_lifecycle_ != nullptr)
            outer_lifecycle_->bind_listener_identity(info.st_dev, info.st_ino);
        next(OuterLaunchPhase::CreateReadyPipe);
        return true;
    }
    case OuterLaunchPhase::CreateReadyPipe: {
        int fds[2] = {-1, -1};
        if (!make_pipe_once(fds)) {
            if (errno == EINTR)
                return true;
            fail_setup();
            return true;
        }
        outer_ready_fd_ = fds[0];
        outer_launch_ready_write_fd_ = fds[1];
        next(OuterLaunchPhase::ReadyReadFlags);
        return true;
    }
    case OuterLaunchPhase::ReadyReadFlags:
        outer_launch_fcntl_flags_ = ::fcntl(outer_ready_fd_, F_GETFL);
        if (outer_launch_fcntl_flags_ < 0) {
            if (errno == EINTR)
                return true;
            fail_setup();
            return true;
        }
        if ((outer_launch_fcntl_flags_ & O_NONBLOCK) != 0)
            next(OuterLaunchPhase::CreateExecPipe);
        else
            next(OuterLaunchPhase::ReadyReadSetFlags);
        return true;
    case OuterLaunchPhase::ReadyReadSetFlags:
        if (::fcntl(outer_ready_fd_, F_SETFL,
                    outer_launch_fcntl_flags_ | O_NONBLOCK) < 0) {
            if (errno == EINTR)
                return true;
            fail_setup();
            return true;
        }
        next(OuterLaunchPhase::CreateExecPipe);
        return true;
    case OuterLaunchPhase::CreateExecPipe: {
        int fds[2] = {-1, -1};
        if (!make_pipe_once(fds)) {
            if (errno == EINTR)
                return true;
            fail_setup();
            return true;
        }
        outer_exec_fd_ = fds[0];
        outer_launch_exec_write_fd_ = fds[1];
        next(OuterLaunchPhase::ExecReadFlags);
        return true;
    }
    case OuterLaunchPhase::ExecReadFlags:
        outer_launch_fcntl_flags_ = ::fcntl(outer_exec_fd_, F_GETFL);
        if (outer_launch_fcntl_flags_ < 0) {
            if (errno == EINTR)
                return true;
            fail_setup();
            return true;
        }
        if ((outer_launch_fcntl_flags_ & O_NONBLOCK) != 0)
            next(OuterLaunchPhase::CreateGate);
        else
            next(OuterLaunchPhase::ExecReadSetFlags);
        return true;
    case OuterLaunchPhase::ExecReadSetFlags:
        if (::fcntl(outer_exec_fd_, F_SETFL,
                    outer_launch_fcntl_flags_ | O_NONBLOCK) < 0) {
            if (errno == EINTR)
                return true;
            fail_setup();
            return true;
        }
        next(OuterLaunchPhase::CreateGate);
        return true;
    case OuterLaunchPhase::CreateGate: {
        int fds[2] = {-1, -1};
        if (!make_gate_once(fds)) {
            if (errno == EINTR)
                return true;
            fail_setup();
            return true;
        }
        outer_launch_gate_read_fd_ = fds[0];
        outer_gate_fd_ = fds[1];
        next(OuterLaunchPhase::GateReadFlags);
        return true;
    }
    case OuterLaunchPhase::GateReadFlags:
        outer_launch_fcntl_flags_ = ::fcntl(outer_launch_gate_read_fd_, F_GETFD);
        if (outer_launch_fcntl_flags_ < 0) {
            if (errno == EINTR)
                return true;
            fail_setup();
            return true;
        }
        next((outer_launch_fcntl_flags_ & FD_CLOEXEC) != 0
                 ? OuterLaunchPhase::GateWriteFlags
                 : OuterLaunchPhase::GateReadSetFlags);
        return true;
    case OuterLaunchPhase::GateReadSetFlags:
        if (::fcntl(outer_launch_gate_read_fd_, F_SETFD,
                    outer_launch_fcntl_flags_ | FD_CLOEXEC) < 0) {
            if (errno == EINTR)
                return true;
            fail_setup();
            return true;
        }
        next(OuterLaunchPhase::GateWriteFlags);
        return true;
    case OuterLaunchPhase::GateWriteFlags:
        outer_launch_fcntl_flags_ = ::fcntl(outer_gate_fd_, F_GETFD);
        if (outer_launch_fcntl_flags_ < 0) {
            if (errno == EINTR)
                return true;
            fail_setup();
            return true;
        }
        next((outer_launch_fcntl_flags_ & FD_CLOEXEC) != 0
                 ? OuterLaunchPhase::PrepareExec
                 : OuterLaunchPhase::GateWriteSetFlags);
        return true;
    case OuterLaunchPhase::GateWriteSetFlags:
        if (::fcntl(outer_gate_fd_, F_SETFD,
                    outer_launch_fcntl_flags_ | FD_CLOEXEC) < 0) {
            if (errno == EINTR)
                return true;
            fail_setup();
            return true;
        }
        next(OuterLaunchPhase::PrepareExec);
        return true;
    case OuterLaunchPhase::PrepareExec: {
        if (!outer_launch_identity_valid_) {
            fail_setup();
            return true;
        }
        const sidecar::LifecycleIdentity& identity = outer_launch_identity_;
        try {
            outer_launch_environment_storage_.reserve(32);
            const auto managed = [](const std::string& value) {
                static constexpr std::array<const char *, 11> names = {
                    "ICECC_CACHE_SERVICE_READY_FD=",
                    "ICECC_CACHE_SERVICE_LISTENER_FD=",
                    "ICECC_CACHE_SERVICE_READY_FORMAT=",
                    "ICECC_CACHE_SERVICE_EXPECTED_GENERATION=",
                    "ICECC_CACHE_SERVICE_EXPECTED_ATTEMPT=",
                    "ICECC_CACHE_SERVICE_EXPECTED_F_STORE_GENERATION=",
                    "ICECC_CACHE_SERVICE_EXPECTED_DERIVATION_VERSION=",
                    "ICECC_CACHE_SERVICE_EXPECTED_C_STORE_GUID=",
                    "ICECC_CACHE_SERVICE_EXPECTED_F_STORE_GUID=",
                    "ICECC_CACHE_SERVICE_EXPECTED_SOCKET=",
                    "ICECC_CACHE_SERVICE_EXPECTED_SOCKET_DIGEST="};
                for (const char *name : names)
                    if (value.rfind(name, 0) == 0)
                        return true;
                return false;
            };
            for (char **entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
                const std::string value(*entry);
                if (!managed(value))
                    outer_launch_environment_storage_.push_back(value);
            }
            const auto bytes_hex_local = [](const auto& bytes) {
                static constexpr char digits[] = "0123456789abcdef";
                std::string result;
                result.reserve(bytes.size() * 2);
                for (const uint8_t byte : bytes) {
                    result.push_back(digits[byte >> 4]);
                    result.push_back(digits[byte & 0x0f]);
                }
                return result;
            };
            outer_launch_environment_storage_.push_back(
                "ICECC_CACHE_SERVICE_READY_FD=" +
                std::to_string(outer_launch_ready_write_fd_));
            outer_launch_environment_storage_.push_back(
                "ICECC_CACHE_SERVICE_LISTENER_FD=" +
                std::to_string(outer_launch_listener_fd_));
            outer_launch_environment_storage_.push_back("ICECC_CACHE_SERVICE_READY_FORMAT=2");
            outer_launch_environment_storage_.push_back(
                "ICECC_CACHE_SERVICE_EXPECTED_GENERATION=" +
                std::to_string(identity.control.generation));
            outer_launch_environment_storage_.push_back(
                "ICECC_CACHE_SERVICE_EXPECTED_ATTEMPT=" +
                std::to_string(identity.control.attempt));
            outer_launch_environment_storage_.push_back(
                "ICECC_CACHE_SERVICE_EXPECTED_F_STORE_GENERATION=" +
                std::to_string(identity.store_generation));
            outer_launch_environment_storage_.push_back(
                "ICECC_CACHE_SERVICE_EXPECTED_DERIVATION_VERSION=" +
                std::to_string(kStoreIdentityDerivationVersion));
            outer_launch_environment_storage_.push_back(
                "ICECC_CACHE_SERVICE_EXPECTED_C_STORE_GUID=" +
                bytes_hex_local(identity.c_store_guid.bytes));
            outer_launch_environment_storage_.push_back(
                "ICECC_CACHE_SERVICE_EXPECTED_F_STORE_GUID=" +
                bytes_hex_local(identity.f_store_guid.bytes));
            outer_launch_environment_storage_.push_back(
                "ICECC_CACHE_SERVICE_EXPECTED_SOCKET=" + socket_path_);
            outer_launch_environment_storage_.push_back(
                "ICECC_CACHE_SERVICE_EXPECTED_SOCKET_DIGEST=" +
                icecc::digest128_hex(icecc::digest128(socket_path_)));
            outer_launch_environment_.reserve(
                outer_launch_environment_storage_.size() + 1);
            for (std::string& value : outer_launch_environment_storage_)
                outer_launch_environment_.push_back(value.data());
            outer_launch_environment_.push_back(nullptr);

            outer_launch_argv_storage_.reserve(24);
            outer_launch_argv_storage_.push_back(config_.executable);
            outer_launch_argv_storage_.push_back("--peer-uid");
            outer_launch_argv_storage_.push_back(std::to_string(config_.expected_daemon_uid));
            outer_launch_argv_storage_.push_back("--peer-gid");
            outer_launch_argv_storage_.push_back(std::to_string(config_.expected_daemon_gid));
            outer_launch_argv_storage_.push_back("--socket");
            outer_launch_argv_storage_.push_back(socket_path_);
            outer_launch_argv_storage_.push_back("--generation");
            outer_launch_argv_storage_.push_back(std::to_string(identity.control.generation));
            outer_launch_argv_storage_.push_back("--attempt");
            outer_launch_argv_storage_.push_back(std::to_string(identity.control.attempt));
            outer_launch_argv_storage_.push_back("--f-store-generation");
            outer_launch_argv_storage_.push_back(std::to_string(identity.store_generation));
            outer_launch_argv_storage_.push_back("--store-derivation-version");
            outer_launch_argv_storage_.push_back(std::to_string(kStoreIdentityDerivationVersion));
            outer_launch_argv_storage_.push_back("--c-store-guid");
            outer_launch_argv_storage_.push_back(bytes_hex_local(identity.c_store_guid.bytes));
            outer_launch_argv_storage_.push_back("--f-store-guid");
            outer_launch_argv_storage_.push_back(bytes_hex_local(identity.f_store_guid.bytes));
            outer_launch_argv_.reserve(outer_launch_argv_storage_.size() + 1);
            for (std::string& value : outer_launch_argv_storage_)
                outer_launch_argv_.push_back(value.data());
            outer_launch_argv_.push_back(nullptr);
        } catch (...) {
            fail_setup();
            return true;
        }
        next(OuterLaunchPhase::Fork);
        return true;
    }
    case OuterLaunchPhase::Fork: {
        const pid_t child = ::fork();
        if (child < 0) {
            if (errno == EINTR)
                return true;
            fail_setup();
            return true;
        }
        if (child == 0) {
            close_owned(outer_ready_fd_);
            close_owned(outer_exec_fd_);
            close_owned(outer_gate_fd_);
            if (!read_gate(outer_launch_gate_read_fd_))
                _exit(127);
            close_owned(outer_launch_gate_read_fd_);
            if (::setsid() < 0)
                _exit(127);
            constexpr uint32_t marker = 0x50355353u;
            if (!write_all_child(outer_launch_exec_write_fd_, &marker, sizeof(marker)))
                _exit(127);
#if defined(__linux__) && defined(SYS_close_range)
            constexpr unsigned int close_range_cloexec = 1u << 2;
            const long close_result = ::syscall(SYS_close_range, 3u, UINT_MAX,
                                                close_range_cloexec);
            if (close_result != 0 && errno != ENOSYS && errno != EINVAL)
                _exit(127);
#endif
            if (!set_cloexec(outer_launch_ready_write_fd_, false) ||
                !set_cloexec(outer_launch_listener_fd_, false))
                _exit(127);
            // Keep stderr on iceccd's configured stream.  A diagnostic file
            // inside the attempt directory prevents that directory from
            // being retired after a sidecar crash and blocks restart.
            ::execve(config_.executable.c_str(), outer_launch_argv_.data(),
                     outer_launch_environment_.data());
            const int error = errno;
            (void)write_all_child(outer_launch_exec_write_fd_, &error, sizeof(error));
            _exit(127);
        }
        outer_pid_ = child;
        outer_pgid_ = child;
        outer_identity_report_pending_ = true;
        // Publish the parent-owned ends before any later phase can observe
        // the pidfd.  The child remains gate-blocked until PermitChild.
        next(OuterLaunchPhase::CloseReadyWrite);
        return true;
    }
    case OuterLaunchPhase::CloseReadyWrite:
        close_owned(outer_launch_ready_write_fd_);
        next(OuterLaunchPhase::CloseExecWrite);
        return true;
    case OuterLaunchPhase::CloseExecWrite:
        close_owned(outer_launch_exec_write_fd_);
        next(OuterLaunchPhase::CloseGateRead);
        return true;
    case OuterLaunchPhase::CloseGateRead:
        close_owned(outer_launch_gate_read_fd_);
        next(OuterLaunchPhase::CloseListener);
        return true;
    case OuterLaunchPhase::CloseListener:
        close_owned(outer_launch_listener_fd_);
        next(OuterLaunchPhase::OpenPidfd);
        return true;
    case OuterLaunchPhase::OpenPidfd:
        outer_pidfd_ = open_pidfd(outer_pid_);
        if (outer_pidfd_ < 0) {
            fail_setup();
            return true;
        }
        next(OuterLaunchPhase::CaptureDomain);
        return true;
    case OuterLaunchPhase::CaptureDomain:
        if (outer_kill_domain_ == nullptr ||
            !(outer_group_domain_ = outer_kill_domain_->capture(outer_pid_, outer_pgid_)).has_value() ||
            !outer_group_domain_->valid()) {
            fail_setup();
            return true;
        }
        next(OuterLaunchPhase::RegisterReaper);
        return true;
    case OuterLaunchPhase::RegisterReaper: {
        if (outer_lifecycle_ == nullptr || !outer_launch_identity_valid_) {
            fail_setup();
            return true;
        }
        sidecar::LaunchIncarnation incarnation;
        incarnation.identity = outer_launch_identity_.control;
        incarnation.store_generation = outer_launch_identity_.store_generation;
        incarnation.store_root = outer_launch_identity_.store_root;
        incarnation.c_store_guid = outer_launch_identity_.c_store_guid;
        incarnation.f_store_guid = outer_launch_identity_.f_store_guid;
        outer_registration_ = outer_reaper_->register_owner(
            outer_pid_, outer_pgid_, outer_lifecycle_->owner_key(),
            outer_lifecycle_->reap_mailbox(), outer_listener_device_,
            outer_listener_inode_, outer_pidfd_, std::move(incarnation));
        if (!outer_registration_.valid()) {
            fail_setup();
            return true;
        }
        outer_lifecycle_->bind_reaper_identity(
            outer_pidfd_, outer_registration_.registry_generation());
        next(OuterLaunchPhase::PermitChild);
        return true;
    }
    case OuterLaunchPhase::PermitChild: {
        if (outer_pidfd_ < 0 || !outer_registration_.valid() ||
            !outer_group_domain_.has_value() || !outer_group_domain_->valid()) {
            fail_setup();
            return true;
        }
        const uint8_t permission = 0x50;
        const ssize_t written = ::send(outer_gate_fd_, &permission,
                                       sizeof(permission), MSG_NOSIGNAL);
        if (written == static_cast<ssize_t>(sizeof(permission))) {
            next(OuterLaunchPhase::CloseGateWrite);
            return true;
        }
        if (written < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            return true;
        fail_setup();
        return true;
    }
    case OuterLaunchPhase::CloseGateWrite:
        close_owned(outer_gate_fd_);
        outer_launch_phase_ = launch_phase(OuterLaunchPhase::None);
        outer_launch_identity_valid_ = false;
        outer_launch_started_ = true;
        return true;
    case OuterLaunchPhase::AbortCloseReadyRead:
        close_owned(outer_ready_fd_);
        next(OuterLaunchPhase::AbortCloseReadyWrite);
        return true;
    case OuterLaunchPhase::AbortCloseReadyWrite:
        close_owned(outer_launch_ready_write_fd_);
        next(OuterLaunchPhase::AbortCloseExecRead);
        return true;
    case OuterLaunchPhase::AbortCloseExecRead:
        close_owned(outer_exec_fd_);
        next(OuterLaunchPhase::AbortCloseExecWrite);
        return true;
    case OuterLaunchPhase::AbortCloseExecWrite:
        close_owned(outer_launch_exec_write_fd_);
        next(OuterLaunchPhase::AbortCloseGateRead);
        return true;
    case OuterLaunchPhase::AbortCloseGateRead:
        close_owned(outer_launch_gate_read_fd_);
        next(OuterLaunchPhase::AbortCloseGateWrite);
        return true;
    case OuterLaunchPhase::AbortCloseGateWrite:
        close_owned(outer_gate_fd_);
        next(OuterLaunchPhase::AbortCloseListener);
        return true;
    case OuterLaunchPhase::AbortCloseListener:
        close_owned(outer_launch_listener_fd_);
        next(OuterLaunchPhase::AbortClosePidfd);
        return true;
    case OuterLaunchPhase::AbortClosePidfd:
        close_owned(outer_pidfd_);
        next(OuterLaunchPhase::AbortDisarmRegistration);
        return true;
    case OuterLaunchPhase::AbortDisarmRegistration:
        outer_registration_.reset();
        next(OuterLaunchPhase::AbortDone);
        return true;
    case OuterLaunchPhase::AbortDone:
        outer_finish_launch_failure();
        return true;
    case OuterLaunchPhase::None:
        break;
    }
    return false;
}

bool DaemonSidecarAdapter::outer_group_action(int signal_number) noexcept
{
    if (outer_pgid_ <= 1 || outer_pidfd_ < 0 ||
        (outer_lifecycle_ != nullptr && !outer_lifecycle_->leader_reaped() &&
         outer_pid_ <= 1))
        return false;
    errno = 0;
    if (::kill(-outer_pgid_, signal_number) == 0)
        return true;
    return errno == ESRCH;
}

bool DaemonSidecarAdapter::outer_read_exec() noexcept
{
    if (outer_exec_fd_ < 0)
        return false;
    std::array<uint8_t, 8> bytes{};
    const ssize_t count = ::read(outer_exec_fd_, bytes.data(), bytes.size());
    if (count > 0) {
        try {
            outer_exec_bytes_.append(reinterpret_cast<const char *>(bytes.data()),
                                     static_cast<size_t>(count));
        } catch (...) {
            outer_exec_complete_ = true;
            outer_exec_failed_ = true;
            close_owned(outer_exec_fd_);
            return true;
        }
        if (outer_exec_bytes_.size() >= sizeof(uint32_t) &&
            outer_exec_bytes_.size() < 2 * sizeof(uint32_t)) {
            uint32_t marker = 0;
            std::memcpy(&marker, outer_exec_bytes_.data(), sizeof(marker));
            // The child writes the marker before execve and, only on an
            // exec/setup failure, appends errno.  Keep the read end open
            // after the marker so a successful CLOEXEC close and a
            // marker+errno failure are distinguishable without a blocking
            // wait or a second hidden read in this turn.
            if (marker != 0x50355353u) {
                outer_exec_complete_ = true;
                outer_exec_failed_ = true;
                close_owned(outer_exec_fd_);
            }
        } else if (outer_exec_bytes_.size() >= 2 * sizeof(uint32_t)) {
            uint32_t marker = 0;
            std::memcpy(&marker, outer_exec_bytes_.data(), sizeof(marker));
            outer_exec_complete_ = true;
            outer_exec_failed_ = marker != 0x50355353u;
            // A second four-byte record is errno from execve.  Its mere
            // presence proves that exec failed, regardless of the value.
            if (outer_exec_bytes_.size() >= 2 * sizeof(uint32_t))
                outer_exec_failed_ = true;
            close_owned(outer_exec_fd_);
        }
        return true;
    }
    if (count == 0) {
        // Successful exec closes the CLOEXEC pipe after the marker.  EOF
        // before a complete marker is still an exec/setup failure.
        if (outer_exec_bytes_.size() >= sizeof(uint32_t)) {
            uint32_t marker = 0;
            std::memcpy(&marker, outer_exec_bytes_.data(), sizeof(marker));
            outer_exec_complete_ = true;
            outer_exec_failed_ = marker != 0x50355353u;
        }
        // EOF before the marker is an exec/setup failure, never success.
        outer_exec_complete_ = true;
        outer_exec_failed_ = outer_exec_failed_ ||
                             outer_exec_bytes_.size() < sizeof(uint32_t);
        close_owned(outer_exec_fd_);
        return true;
    }
    return true; // EINTR/EAGAIN are still one attempted receive action.
}

bool DaemonSidecarAdapter::outer_read_ready() noexcept
{
    if (outer_ready_fd_ < 0)
        return false;
    std::array<char, 2048> bytes{};
    const ssize_t count = ::read(outer_ready_fd_, bytes.data(), bytes.size());
    if (count > 0) {
        if (outer_ready_bytes_.size() > 2048 -
                                      static_cast<size_t>(count)) {
            outer_ready_invalid_ = true;
        } else {
            try {
                outer_ready_bytes_.append(bytes.data(), static_cast<size_t>(count));
            } catch (...) {
                outer_ready_invalid_ = true;
                outer_ready_complete_ = true;
                close_owned(outer_ready_fd_);
                return true;
            }
        }
        if (outer_ready_bytes_.find('\n') != std::string::npos) {
            outer_ready_complete_ = true;
            sidecar::ReadyLease parsed;
            if (outer_ready_bytes_.size() > 2048 ||
                outer_lifecycle_ == nullptr ||
                !outer_lifecycle_->identity().has_value() ||
                !sidecar::parse_ready_frame(outer_ready_bytes_,
                                             *outer_lifecycle_->identity(),
                                             outer_pid_, parsed))
                outer_ready_invalid_ = true;
            if (!outer_ready_invalid_) {
                parsed.directory_device = outer_directory_device_;
                parsed.directory_inode = outer_directory_inode_;
                outer_ready_lease_ = std::move(parsed);
            }
            close_owned(outer_ready_fd_);
        }
        return true;
    }
    if (count == 0) {
        if (outer_ready_bytes_.find('\n') == std::string::npos)
            outer_ready_invalid_ = true;
        outer_ready_complete_ = true;
        close_owned(outer_ready_fd_);
        return true;
    }
    return true; // EINTR/EAGAIN remain bounded receive attempts.
}

bool DaemonSidecarAdapter::outer_begin_authentication(
    std::chrono::steady_clock::time_point now) noexcept
{
    if (outer_auth_fd_ >= 0 || outer_lifecycle_ == nullptr ||
        !outer_ready_lease_.has_value() || !outer_ready_lease_->valid())
        return false;
    const std::string& path = outer_ready_lease_->socket_path;
    if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path))
        return false;
    int socket_type = SOCK_STREAM;
#ifdef SOCK_CLOEXEC
    socket_type |= SOCK_CLOEXEC;
#endif
#ifdef SOCK_NONBLOCK
    socket_type |= SOCK_NONBLOCK;
#else
    // The outer authentication state machine cannot repair a blocking fd
    // with a hidden fcntl sequence.  A platform without atomic
    // SOCK_NONBLOCK creation is therefore an explicit fail-closed port.
    return false;
#endif
    const int fd = ::socket(AF_UNIX, socket_type, 0);
    if (fd < 0)
        return false;
    outer_auth_fd_ = fd;
    // Socket creation is the only fallible action in this turn.  Connect is
    // deliberately a separate phase so the daemon outer loop never couples
    // socket()+connect() (or their retries) behind one adapter call.
    outer_auth_phase_ = kAuthConnectPending;
    outer_auth_deadline_ = now + config_.connect_timeout;
    outer_auth_offset_ = outer_auth_read_ = outer_auth_expected_ = 0;
    outer_auth_read_buffer_.clear();
    outer_auth_write_.clear();
    const local::Identity identity{config_.generation, outer_ready_lease_->identity.attempt};
    try {
        outer_auth_write_ = local::encode_frame(
            local::make_hello(local::PeerRole::Daemon, identity));
    } catch (...) {
        // Keep the newly-created descriptor until a later close phase.  A
        // failed allocation is not permission to pair socket()+close() in
        // this one outer action.
        outer_auth_phase_ = kAuthClosePending;
        return true;
    }
    if (outer_auth_write_.empty()) {
        outer_auth_phase_ = kAuthClosePending;
        return true;
    }
    return true;
}

bool DaemonSidecarAdapter::outer_advance_authentication(
    std::chrono::steady_clock::time_point now, short revents) noexcept
{
    if (outer_auth_fd_ < 0 || outer_auth_phase_ == 0)
        return false;
    if (now >= outer_auth_deadline_ ||
        (revents & POLLNVAL) != 0 ||
        ((revents & (POLLERR | POLLHUP)) != 0 &&
         (revents & (POLLIN | POLLOUT)) == 0)) {
        close_owned(outer_auth_fd_);
        outer_auth_phase_ = 0;
        return true;
    }
    if (outer_auth_phase_ == kAuthClosePending) {
        close_owned(outer_auth_fd_);
        outer_auth_phase_ = 0;
        return true;
    }
    if (outer_auth_phase_ == kAuthConnectPending) {
        if (!outer_ready_lease_.has_value() ||
            outer_ready_lease_->socket_path.empty() ||
            outer_ready_lease_->socket_path.size() >= sizeof(sockaddr_un::sun_path)) {
            close_owned(outer_auth_fd_);
            outer_auth_phase_ = 0;
            return true;
        }
        const std::string& path = outer_ready_lease_->socket_path;
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.data(), path.size());
        address.sun_path[path.size()] = '\0';
        const int result = ::connect(
            outer_auth_fd_, reinterpret_cast<sockaddr*>(&address),
            static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) +
                                   path.size() + 1));
        if (result == 0) {
            // Keep connect success separate from credential observation.
            outer_auth_phase_ = 5;
            return true;
        }
        if (errno == EINPROGRESS || errno == EALREADY || errno == EAGAIN) {
            outer_auth_phase_ = 1;
            return true;
        }
        if (errno == EINTR)
            return true;
        close_owned(outer_auth_fd_);
        outer_auth_phase_ = 0;
        return true;
    }
    if (outer_auth_phase_ == 1) {
        int error = 0;
        socklen_t length = sizeof(error);
        if (::getsockopt(outer_auth_fd_, SOL_SOCKET, SO_ERROR, &error, &length) != 0) {
            if (errno == EINTR)
                return true;
            close_owned(outer_auth_fd_);
            outer_auth_phase_ = 0;
            return true;
        }
        if (error != 0) {
            close_owned(outer_auth_fd_);
            outer_auth_phase_ = 0;
            return true;
        }
        // Keep connect completion and credential observation in separate
        // outer turns.  Both are fallible kernel observations; coalescing
        // them would violate the one externally visible action quota.
        outer_auth_phase_ = 5;
        return true;
    }
    if (outer_auth_phase_ == 5) {
        const auto peer = local::query_peer_credentials(outer_auth_fd_);
        // The listener is pre-bound by the daemon and inherited by the
        // supervised child.  Linux SO_PEERCRED consequently reports the
        // creator of that listener (the daemon), rather than the inherited
        // acceptor's PID.  The exact child PID is already fenced by the
        // structured READY lease (which was parsed against outer_pid_), while
        // SO_PEERCRED still proves the service UID/GID.  Requiring the socket
        // creator to equal outer_pid_ would reject every valid pre-bound
        // listener and strand authentication before HELLO is sent.
        const bool peer_pid_is_creator =
            peer.has_value() && peer->pid == static_cast<uint64_t>(::getpid());
        if (!peer.has_value() || peer->uid != config_.expected_service_uid ||
            peer->gid != config_.expected_service_gid ||
            (peer->pid != static_cast<uint64_t>(outer_pid_) &&
             !peer_pid_is_creator) ||
            !outer_ready_lease_.has_value() ||
            outer_ready_lease_->pid != outer_pid_) {
            close_owned(outer_auth_fd_);
            outer_auth_phase_ = 0;
            return true;
        }
        outer_auth_phase_ = 2;
        return true;
    }
    if (outer_auth_phase_ == 2) {
        if (outer_auth_offset_ == outer_auth_write_.size()) {
            outer_auth_phase_ = 3;
            return true;
        }
        const size_t amount = std::min<size_t>(4096,
                                               outer_auth_write_.size() -
                                                   outer_auth_offset_);
        const ssize_t written = ::send(outer_auth_fd_,
                                       outer_auth_write_.data() + outer_auth_offset_,
                                       amount, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (written > 0)
            outer_auth_offset_ += static_cast<size_t>(written);
        else if (written < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            close_owned(outer_auth_fd_);
            outer_auth_phase_ = 0;
        }
        return true;
    }
    if (outer_auth_phase_ == 3) {
        std::array<uint8_t, 4096> bytes{};
        const ssize_t count = ::recv(outer_auth_fd_, bytes.data(), bytes.size(),
                                     MSG_DONTWAIT);
        if (count > 0) {
            try {
                outer_auth_read_buffer_.insert(outer_auth_read_buffer_.end(),
                                               bytes.begin(), bytes.begin() + count);
            } catch (...) {
                close_owned(outer_auth_fd_);
                outer_auth_phase_ = 0;
                return true;
            }
            if (outer_auth_read_buffer_.size() >= local::kFrameHeaderSize &&
                outer_auth_expected_ == 0) {
                const auto &buffer = outer_auth_read_buffer_;
                const uint32_t payload = (uint32_t(buffer[8]) << 24) |
                                         (uint32_t(buffer[9]) << 16) |
                                         (uint32_t(buffer[10]) << 8) | uint32_t(buffer[11]);
                if (payload > local::kMaxFramePayload) {
                    close_owned(outer_auth_fd_);
                    outer_auth_phase_ = 0;
                    return true;
                }
                outer_auth_expected_ = local::kFrameHeaderSize + payload;
            }
            if (outer_auth_expected_ != 0 &&
                outer_auth_read_buffer_.size() >= outer_auth_expected_) {
                if (outer_auth_read_buffer_.size() != outer_auth_expected_) {
                    close_owned(outer_auth_fd_);
                    outer_auth_phase_ = 0;
                    return true;
                }
                local::Frame frame;
                const local::Identity identity{config_.generation,
                                               outer_ready_lease_->identity.attempt};
                if (local::decode_frame(outer_auth_read_buffer_, frame) != local::Status::Ok ||
                    local::validate_handshake(frame, local::MessageType::HelloAck,
                                              local::PeerRole::Sidecar,
                                              identity) != local::Status::Ok) {
                    close_owned(outer_auth_fd_);
                    outer_auth_phase_ = 0;
                    return true;
                }
                outer_auth_phase_ = 4;
            }
        } else if (count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            close_owned(outer_auth_fd_);
            outer_auth_phase_ = 0;
        }
        return true;
    }
    // The final peek is the trailing-byte barrier for the authentication frame.
    if (outer_auth_phase_ == 4) {
        uint8_t byte = 0;
        const ssize_t count = ::recv(outer_auth_fd_, &byte, sizeof(byte),
                                     MSG_PEEK | MSG_DONTWAIT);
        if (count < 0 && errno == EINTR) {
            // Preserve the authenticated frame and lease across an
            // interrupted trailing probe; a later outer turn retries this
            // same one-byte observation without renegotiating.
        } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            outer_authenticated_ = true;
            // PHASE_OPEN/control ownership is persistent after the
            // HELLO/ACK trailing-byte barrier.  Do not close this descriptor
            // as a generic handoff-completion side effect; replacement and
            // shutdown are the only reducers allowed to release it.
            outer_auth_phase_ = 0;
        } else {
            close_owned(outer_auth_fd_);
            outer_auth_phase_ = 0;
        }
        return true;
    }
    return false;
}

bool DaemonSidecarAdapter::outer_group_observation(
    sidecar::LifecycleObservation& observation) noexcept
{
    if (outer_pid_ <= 1 || outer_pgid_ <= 1)
        return false;
    observation.observed_pgid = outer_pgid_;
    if (outer_group_gone_observed_ && outer_group_gone_domain_.valid()) {
        observation.group = sidecar::GroupObservation::Gone;
        observation.group_domain = outer_group_gone_domain_;
        return true;
    }
    errno = 0;
    if (::kill(-outer_pgid_, 0) == 0 || errno == EPERM) {
        observation.group = sidecar::GroupObservation::Present;
        return true;
    }
    if (errno == ESRCH) {
        observation.group = sidecar::GroupObservation::Gone;
        if (outer_kill_domain_ != nullptr) {
            auto verifier = std::dynamic_pointer_cast<OuterKillDomainVerifier>(
                outer_kill_domain_);
            if (verifier != nullptr) {
                const auto lease = verifier->lease_for(outer_pid_, outer_pgid_);
                if (lease.has_value()) {
                    observation.group_domain = *lease;
                    outer_group_gone_domain_ = *lease;
                    outer_group_gone_observed_ = true;
                }
            }
        }
        return true;
    }
    observation.group = sidecar::GroupObservation::Unknown;
    return false;
}

bool DaemonSidecarAdapter::outer_path_observation(
    sidecar::LifecycleObservation& observation) noexcept
{
    if (outer_directory_path_.empty())
        return false;
    try {
        const std::string socket_path = outer_directory_path_ + "/cache.sock";
        struct stat info{};
        if (::lstat(socket_path.c_str(), &info) == 0) {
            observation.path_absent = false;
            observation.observed_path = outer_directory_path_;
            observation.observed_device = info.st_dev;
            observation.observed_inode = info.st_ino;
            return true;
        }
        if (errno == ENOENT) {
            observation.path_absent = true;
            observation.observed_path = outer_directory_path_;
            observation.observed_device = outer_listener_device_;
            observation.observed_inode = outer_listener_inode_;
            // A pre-fork/setup failure has no child/group to wait for.  Once
            // this exact socket-path absence is observed, the separate launch
            // failure/path obligation is discharged; the reducer may then
            // move from LaunchPrepared to RetryEligible or DegradedLegacy.
            outer_launch_failure_path_pending_ = false;
            return true;
        }
    } catch (...) {
        // A failed observation is not path absence.  The reducer keeps the
        // exact teardown identity and will either retry on a later turn or
        // fail closed at its absolute deadline.
    }
    return false;
}

void DaemonSidecarAdapter::outer_arm_cleanup(uint8_t target) noexcept
{
    // This method only arms a state machine.  It deliberately performs no
    // pathname syscall: the first open is owned by a later outer turn.
    if (outer_cleanup_parent_fd_ >= 0) {
        // A live parent descriptor must never be silently replaced.  Leaving
        // the phase at a nonzero/no-step value keeps shutdown fail-closed and
        // makes an ownership bug visible to the outer coordinator.
        outer_cleanup_phase_ = target;
        outer_cleanup_step_ = cleanup_step(OuterCleanupStep::None);
        outer_cleanup_directory_target_ = target == 2;
        outer_cleanup_capture_name_.clear();
        return;
    }
    if ((target != 1 && target != 2) || outer_directory_path_.empty()) {
        outer_cleanup_phase_ = 0;
        outer_cleanup_step_ = cleanup_step(OuterCleanupStep::None);
        outer_cleanup_directory_target_ = false;
        outer_cleanup_capture_name_.clear();
        return;
    }
    const dev_t expected_device = target == 1 ? outer_listener_device_
                                              : outer_directory_device_;
    const ino_t expected_inode = target == 1 ? outer_listener_inode_
                                              : outer_directory_inode_;
    if (expected_device == 0 || expected_inode == 0) {
        outer_cleanup_phase_ = 0;
        outer_cleanup_step_ = cleanup_step(OuterCleanupStep::None);
        outer_cleanup_directory_target_ = false;
        outer_cleanup_capture_name_.clear();
        return;
    }

    char capture_name[96];
    const uint64_t serial = g_cleanup_sequence.fetch_add(
        1, std::memory_order_relaxed);
    const int written = std::snprintf(
        capture_name, sizeof(capture_name), ".icecc-capture-%lld-%llu",
        static_cast<long long>(::getpid()),
        static_cast<unsigned long long>(serial == 0 ? 1 : serial));
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(capture_name)) {
        outer_cleanup_phase_ = target;
        outer_cleanup_step_ = cleanup_step(OuterCleanupStep::None);
        outer_cleanup_directory_target_ = target == 2;
        outer_cleanup_capture_name_.clear();
        return;
    }
    try {
        outer_cleanup_capture_name_.assign(capture_name,
                                           static_cast<size_t>(written));
    } catch (...) {
        outer_cleanup_phase_ = target;
        outer_cleanup_step_ = cleanup_step(OuterCleanupStep::None);
        outer_cleanup_directory_target_ = target == 2;
        outer_cleanup_capture_name_.clear();
        return;
    }
    outer_cleanup_phase_ = target;
    outer_cleanup_directory_target_ = target == 2;
    outer_cleanup_step_ = cleanup_step(OuterCleanupStep::OpenParent);
}

bool DaemonSidecarAdapter::outer_cleanup_exact() noexcept
{
    if (outer_cleanup_phase_ == 0 || outer_cleanup_step_ == 0)
        return false;

    // Complete a successful target only after the one syscall in the current
    // phase has returned.  The transition to the next target is pure state
    // work; its first open remains deferred to the next outer turn.
    const auto finish_target = [&]() noexcept {
        const uint8_t completed = outer_cleanup_phase_;
        outer_cleanup_step_ = cleanup_step(OuterCleanupStep::None);
        outer_cleanup_parent_fd_ = -1;
        outer_cleanup_capture_name_.clear();
        if (completed == 1 && outer_directory_inode_ != 0) {
            outer_cleanup_phase_ = 0;
            outer_cleanup_directory_target_ = false;
            outer_arm_cleanup(2);
            return;
        }
        outer_cleanup_phase_ = 0;
        outer_cleanup_directory_target_ = false;
        if (!outer_cleanup_waiting_path_observation_) {
            outer_directory_path_.clear();
            outer_directory_device_ = outer_directory_inode_ = 0;
            outer_listener_device_ = outer_listener_inode_ = 0;
        }
        outer_cleanup_waiting_path_observation_ = false;
    };
    const auto close_failed = [&]() noexcept {
        // A failed close is retained as a terminal cleanup hold.  No later
        // turn may guess whether the capture was removed or restore a path
        // with a second rename, which could race a pathname newcomer.
        close_owned(outer_cleanup_parent_fd_);
        outer_cleanup_step_ = cleanup_step(OuterCleanupStep::None);
    };

    try {
        const bool directory = outer_cleanup_directory_target_;
        const char *target_name = directory ? nullptr : "cache.sock";
        std::string parent_path;
        if (directory) {
            const size_t slash = outer_directory_path_.rfind('/');
            if (slash == std::string::npos || slash + 1 >= outer_directory_path_.size()) {
                outer_cleanup_step_ = cleanup_step(OuterCleanupStep::None);
                return false;
            }
            parent_path = slash == 0 ? "/" : outer_directory_path_.substr(0, slash);
            target_name = outer_directory_path_.c_str() + slash + 1;
        } else {
            parent_path = outer_directory_path_;
        }
        const dev_t expected_device = directory ? outer_directory_device_
                                                 : outer_listener_device_;
        const ino_t expected_inode = directory ? outer_directory_inode_
                                                : outer_listener_inode_;
        const OuterCleanupStep step = cleanup_step(outer_cleanup_step_);
        switch (step) {
        case OuterCleanupStep::OpenParent: {
            const int parent_fd = ::open(
                parent_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (parent_fd >= 0) {
                outer_cleanup_parent_fd_ = parent_fd;
                outer_cleanup_step_ = cleanup_step(OuterCleanupStep::StatTarget);
                return true;
            }
            if (errno == EINTR)
                return true;
            // If the parent vanished, the target is already absent.  This is
            // a successful absence observation, not permission to remove a
            // different pathname; no fd is available to unlink anything.
            if (errno == ENOENT) {
                finish_target();
                return true;
            }
            outer_cleanup_step_ = cleanup_step(OuterCleanupStep::None);
            return true;
        }
        case OuterCleanupStep::StatTarget: {
            if (outer_cleanup_parent_fd_ < 0 || target_name == nullptr ||
                expected_device == 0 || expected_inode == 0) {
                close_failed();
                return true;
            }
            struct stat info{};
            if (::fstatat(outer_cleanup_parent_fd_, target_name, &info,
                          AT_SYMLINK_NOFOLLOW) == 0) {
                const bool type_matches = directory ? S_ISDIR(info.st_mode)
                                                    : S_ISSOCK(info.st_mode);
                if (!type_matches || info.st_dev != expected_device ||
                    info.st_ino != expected_inode) {
                    outer_cleanup_step_ = cleanup_step(
                        OuterCleanupStep::CloseParentFailure);
                } else {
                    outer_cleanup_step_ = cleanup_step(
                        OuterCleanupStep::CaptureTarget);
                }
                return true;
            }
            if (errno == ENOENT)
                outer_cleanup_step_ = cleanup_step(OuterCleanupStep::CloseParentSuccess);
            else
                outer_cleanup_step_ = cleanup_step(OuterCleanupStep::CloseParentFailure);
            return true;
        }
        case OuterCleanupStep::CaptureTarget:
#if defined(__linux__) && defined(SYS_renameat2)
            if (outer_cleanup_parent_fd_ < 0 || target_name == nullptr ||
                outer_cleanup_capture_name_.empty()) {
                close_failed();
                return true;
            }
            // Prefer a no-replace capture.  Linux ZFS returns EINVAL for
            // RENAME_NOREPLACE on a socket node, so use the ordinary rename
            // operation there; the attempt directory and capture name are
            // private to this adapter, and StatCapture still verifies the
            // exact device/inode before removal.
            if (::syscall(SYS_renameat2, outer_cleanup_parent_fd_, target_name,
                          outer_cleanup_parent_fd_, outer_cleanup_capture_name_.c_str(),
                          1u) == 0) {
                outer_cleanup_step_ = cleanup_step(OuterCleanupStep::StatCapture);
                return true;
            }
            if ((errno == EINVAL || errno == ENOSYS || errno == EOPNOTSUPP) &&
                ::renameat(outer_cleanup_parent_fd_, target_name,
                           outer_cleanup_parent_fd_,
                           outer_cleanup_capture_name_.c_str()) == 0) {
                outer_cleanup_step_ = cleanup_step(OuterCleanupStep::StatCapture);
                return true;
            }
            if (errno == ENOENT)
                outer_cleanup_step_ = cleanup_step(OuterCleanupStep::CloseParentSuccess);
            else
                outer_cleanup_step_ = cleanup_step(OuterCleanupStep::CloseParentFailure);
            return true;
#else
            close_failed();
            return true;
#endif
        case OuterCleanupStep::StatCapture: {
            if (outer_cleanup_parent_fd_ < 0 || outer_cleanup_capture_name_.empty()) {
                close_failed();
                return true;
            }
            struct stat captured{};
            if (::fstatat(outer_cleanup_parent_fd_, outer_cleanup_capture_name_.c_str(),
                          &captured, AT_SYMLINK_NOFOLLOW) == 0) {
                const bool type_matches = directory ? S_ISDIR(captured.st_mode)
                                                    : S_ISSOCK(captured.st_mode);
                if (type_matches && captured.st_dev == expected_device &&
                    captured.st_ino == expected_inode)
                    outer_cleanup_step_ = cleanup_step(OuterCleanupStep::UnlinkCapture);
                else
                    outer_cleanup_step_ = cleanup_step(
                        OuterCleanupStep::CloseParentFailure);
                return true;
            }
            outer_cleanup_step_ = cleanup_step(OuterCleanupStep::CloseParentFailure);
            return true;
        }
        case OuterCleanupStep::UnlinkCapture: {
            if (outer_cleanup_parent_fd_ < 0 || outer_cleanup_capture_name_.empty()) {
                close_failed();
                return true;
            }
            const int flags = directory ? AT_REMOVEDIR : 0;
            if (::unlinkat(outer_cleanup_parent_fd_, outer_cleanup_capture_name_.c_str(),
                           flags) == 0 || errno == ENOENT)
                outer_cleanup_step_ = cleanup_step(OuterCleanupStep::CloseParentSuccess);
            else
                outer_cleanup_step_ = cleanup_step(OuterCleanupStep::CloseParentFailure);
            return true;
        }
        case OuterCleanupStep::CloseParentSuccess:
            close_owned(outer_cleanup_parent_fd_);
            finish_target();
            return true;
        case OuterCleanupStep::CloseParentFailure:
            close_failed();
            return true;
        case OuterCleanupStep::None:
            break;
        }
    } catch (...) {
        // Allocation is not a cleanup proof.  Retain the exact phase and
        // close only the already-owned parent on a later bounded turn.
        if (outer_cleanup_parent_fd_ >= 0)
            outer_cleanup_step_ = cleanup_step(OuterCleanupStep::CloseParentFailure);
        else
            outer_cleanup_step_ = cleanup_step(OuterCleanupStep::None);
        return false;
    }
    return false;
}

void DaemonSidecarAdapter::outer_apply_action(
    const sidecar::LifecycleActionResult& action,
    std::chrono::steady_clock::time_point now) noexcept
{
    if (action.action == sidecar::LifecycleAction::None)
        return;
    // Every reducer action consumes this adapter's one per-turn action quota,
    // including withdrawal/cleanup transitions whose syscall work is
    // performed below.  A caller must never infer that it may advance again
    // merely because the particular action happened to fail closed.
    outer_action_taken_ = true;
    switch (action.action) {
    case sidecar::LifecycleAction::LaunchPrepared:
        if (!outer_prepare_launch(action.identity, now)) {
            std::fprintf(stderr,
                         "cache sidecar launch preparation refused"
                         " (identity_valid=%d started=%d phase=%d pid=%ld"
                         " reaper=%d)\n",
                         int(action.identity.valid()),
                         int(outer_launch_started_), int(outer_launch_phase_),
                         long(outer_pid_), int(outer_reaper_ != nullptr));
            outer_launch_failed_ = true;
            // A failed setup may have created only the attempt directory, or
            // may have failed before owning any pathname at all.  Never arm
            // an inode-removal phase with a zero identity: that would leave a
            // permanently retrying cleanup turn (or invite name-based
            // deletion).  The next turn only cleans objects whose exact
            // device/inode pair was captured by outer_prepare_launch().
            // If fork already transferred a child into the exact fields, the
            // reducer must first observe/reap and prove its group absent.  A
            // setup failure is not permission to remove the listener while
            // that child may still be alive behind the launch gate.
            // Even a setup failure before fork may have created and captured
            // the attempt node.  Keep that exact path identity alive through
            // one later absence observation; clearing it as soon as the
            // cleanup action returns would leave LaunchPrepared unable to
            // prove the path fence and strand the reducer at its deadline.
            outer_cleanup_waiting_path_observation_ = false;
            if (outer_pid_ <= 1) {
                if (outer_listener_inode_ != 0)
                    outer_arm_cleanup(1);
                else if (outer_directory_inode_ != 0)
                    outer_arm_cleanup(2);
                outer_cleanup_waiting_path_observation_ = outer_cleanup_phase_ != 0;
            }
        } else {
            // Plan admission is pure; spend this turn on exactly the first
            // launch phase.  Every later phase is resumed by outer_begin_turn
            // before the daemon's next poll, never by an inner setup loop.
            (void)outer_advance_launch_step();
        }
        break;
    case sidecar::LifecycleAction::Withdraw:
        // An unrequested withdrawal is an incarnation failure (for example
        // an exact child reap, invalid READY, or lost identity), not a
        // terminal shutdown.  Preserve the withdrawal edge and ask the same
        // outer reducer to prove A teardown before a later turn can mint B.
        // Explicit shutdown already owns its terminal path and must never
        // turn into a successor launch.
        if (!outer_shutdown_requested_) {
            outer_replacement_requested_ = true;
            outer_replacement_input_close_pending_ = true;
        }
        if (outer_reap_was_ready_ && !outer_post_ready_exit_counted_) {
            if (cumulative_post_ready_exits_ != std::numeric_limits<uint64_t>::max())
                ++cumulative_post_ready_exits_;
            outer_post_ready_exit_counted_ = true;
        }
        if (outer_reap_was_ready_) {
            outer_reap_was_ready_ = false;
            outer_ready_had_been_published_ = false;
        }
        outer_authenticated_ = false;
        outer_ready_lease_.reset();
        disable_relationship();
        close_owned(outer_auth_fd_);
        outer_auth_phase_ = 0;
        outer_auth_failure_ = false;
        // A sidecar replacement fences every outstanding remote input
        // operation.  Its request/result is never rebound to the next
        // allocator incarnation.
        outer_input_operation_.reset();
        outer_input_request_.reset();
        outer_input_cancel_lease_.reset();
        pending_input_lifecycle_.clear();
        outer_observe(pending_advertisement_update_);
        break;
    case sidecar::LifecycleAction::SendTerm:
        if (outer_group_action(SIGTERM))
            outer_action_taken_ = true;
        break;
    case sidecar::LifecycleAction::SendKill:
        if (outer_group_action(SIGKILL))
            outer_action_taken_ = true;
        break;
    case sidecar::LifecycleAction::PublishReady: {
        try {
            if (outer_lifecycle_ != nullptr)
                outer_ready_lease_ = outer_lifecycle_->current_ready_lease();
            if (!outer_ready_lease_.has_value() || !outer_ready_lease_->valid()) {
                outer_launch_failed_ = true;
                outer_replacement_requested_ = true;
                outer_replacement_input_close_pending_ = true;
                outer_authenticated_ = false;
                disable_relationship();
                outer_ready_lease_.reset();
                break;
            }
            outer_directory_path_ = outer_ready_lease_->private_directory;
            outer_directory_device_ = outer_ready_lease_->directory_device;
            outer_directory_inode_ = outer_ready_lease_->directory_inode;
            outer_listener_device_ = outer_ready_lease_->listener_device;
            outer_listener_inode_ = outer_ready_lease_->listener_inode;
            socket_path_ = outer_ready_lease_->socket_path;
            attempt_directory_ = outer_directory_path_;
            attempt_directory_device_ = outer_directory_device_;
            attempt_directory_inode_ = outer_directory_inode_;
            socket_device_ = outer_listener_device_;
            socket_inode_ = outer_listener_inode_;
            attempt_ = outer_ready_lease_->identity.attempt;
            dispatcher_.reset();
            OnDemandEndpoint endpoint;
            endpoint.socket_path = socket_path_;
            endpoint.expected_peer = local::CredentialExpectation{
                config_.expected_service_uid, config_.expected_service_gid,
                static_cast<uint64_t>(outer_pid_)};
            endpoint.lease_identity = local::Identity{config_.generation, attempt_};
            endpoint.f_store_generation = outer_ready_lease_->store_generation;
            endpoint.store_root = outer_ready_lease_->store_root;
            endpoint.store_derivation_version = kStoreIdentityDerivationVersion;
            endpoint.c_store_guid = outer_ready_lease_->c_store_guid;
            endpoint.f_store_guid = outer_ready_lease_->f_store_guid;
            endpoint.socket_path_digest = outer_ready_lease_->socket_path_digest;
            endpoint.listener_device = outer_ready_lease_->listener_device;
            endpoint.listener_inode = outer_ready_lease_->listener_inode;
            dispatcher_ = std::make_unique<CacheSessionDispatcher>(
                endpoint.lease_identity, std::move(endpoint), config_.handoff_timeout);
            // This is the sole publication point for the incarnation's
            // post-READY classification.  It survives an auth/replacement
            // withdrawal until the central exact-pid reap is delivered.
            outer_ready_had_been_published_ = true;
            outer_post_ready_exit_counted_ = false;
            outer_reap_was_ready_ = false;
            outer_auth_start_pending_ = true;
            outer_auth_failure_ = false;
        } catch (...) {
            dispatcher_.reset();
            outer_launch_failed_ = true;
            // Publication/materialisation failure is an incarnation failure,
            // not a permission to keep A advertised or retry its auth lane.
            // Feed it into the common replacement reducer on the next turn.
            outer_replacement_requested_ = true;
            outer_replacement_input_close_pending_ = true;
            outer_authenticated_ = false;
            disable_relationship();
            outer_ready_lease_.reset();
            outer_auth_start_pending_ = false;
            outer_auth_failure_ = false;
        }
        break;
    }
    case sidecar::LifecycleAction::CleanupPath:
        // The reducer has already proved exact leader consumption and the
        // non-reusable group domain's absence.  Arm only the captured socket
        // identity; the actual remove is one fallible outer action on a later
        // turn, and path absence then returns to the reducer for RetryEligible.
        outer_cleanup_waiting_path_observation_ = false;
        if (outer_listener_inode_ != 0)
            outer_arm_cleanup(1);
        else if (outer_directory_inode_ != 0)
            outer_arm_cleanup(2);
        outer_cleanup_waiting_path_observation_ = outer_cleanup_phase_ != 0;
        break;
    case sidecar::LifecycleAction::RetryEligible:
        // The old socket is already absent by the reducer proof.  Directory
        // removal is one separate outer action; allocation cannot happen until
        // this phase has completed on a later turn.
        if (outer_directory_inode_ != 0) {
            outer_cleanup_waiting_path_observation_ = false;
            outer_arm_cleanup(2);
        } else {
            // A fork/setup failure may have happened before the attempt
            // directory was captured.  Never arm an inode-removal phase with
            // a zero identity: doing so would retry forever and could turn a
            // later pathname occupant into a cleanup target.
            outer_cleanup_phase_ = 0;
            outer_cleanup_step_ = cleanup_step(OuterCleanupStep::None);
            outer_cleanup_directory_target_ = false;
            outer_cleanup_capture_name_.clear();
            outer_cleanup_waiting_path_observation_ = false;
            outer_directory_path_.clear();
            outer_directory_device_ = outer_directory_inode_ = 0;
            outer_listener_device_ = outer_listener_inode_ = 0;
        }
        outer_close_fds();
        outer_disarm_registration();
        outer_group_domain_.reset();
        outer_group_gone_observed_ = false;
        outer_group_gone_domain_ = {};
        outer_launch_started_ = false;
        outer_identity_report_pending_ = false;
        outer_pid_ = outer_pgid_ = -1;
        outer_exec_bytes_.clear();
        outer_ready_bytes_.clear();
        outer_ready_reported_ = 0;
        outer_exec_complete_ = outer_exec_failed_ = false;
        outer_ready_complete_ = outer_ready_invalid_ = false;
        outer_ready_lease_.reset();
        outer_input_operation_.reset();
        outer_input_request_.reset();
        outer_input_cancel_lease_.reset();
        pending_input_lifecycle_.clear();
        break;
    case sidecar::LifecycleAction::EnterDegradedLegacy:
        outer_authenticated_ = false;
        outer_ready_lease_.reset();
        disable_relationship();
        close_owned(outer_auth_fd_);
        outer_auth_phase_ = 0;
        outer_close_fds();
        outer_disarm_registration();
        outer_pid_ = outer_pgid_ = -1;
        outer_input_operation_.reset();
        outer_input_request_.reset();
        outer_input_cancel_lease_.reset();
        pending_input_lifecycle_.clear();
        outer_cleanup_waiting_path_observation_ = false;
        if (outer_listener_inode_ != 0)
            outer_arm_cleanup(1);
        else if (outer_directory_inode_ != 0)
            outer_arm_cleanup(2);
        outer_observe(pending_advertisement_update_);
        break;
    case sidecar::LifecycleAction::FailedClosed:
        // This is not an exact teardown proof.  Withdraw all externally
        // visible relationship state and close only observation/control
        // descriptors, but retain the pidfd, registration, PID/PGID, and
        // captured path identity for the central reaper/identity-aware
        // external cleanup authority.  Calling outer_close_fds() or
        // outer_disarm_registration() here would falsely turn a still-live
        // child into a successful shutdown and permit PID/path reuse.
        outer_authenticated_ = false;
        outer_ready_lease_.reset();
        disable_relationship();
        close_owned(outer_ready_fd_);
        close_owned(outer_exec_fd_);
        close_owned(outer_gate_fd_);
        close_owned(outer_auth_fd_);
        outer_auth_phase_ = 0;
        outer_auth_expected_ = 0;
        outer_auth_offset_ = 0;
        outer_auth_read_ = 0;
        outer_auth_write_.clear();
        outer_auth_read_buffer_.clear();
        outer_auth_start_pending_ = false;
        outer_input_operation_.reset();
        outer_input_request_.reset();
        outer_input_cancel_lease_.reset();
        pending_input_lifecycle_.clear();
        // Keep cleanup disarmed until an independent authority proves both
        // group and path absence.  FailedClosed remains an explicit HOLD.
        outer_cleanup_phase_ = 0;
        outer_cleanup_step_ = cleanup_step(OuterCleanupStep::None);
        outer_cleanup_directory_target_ = false;
        outer_cleanup_capture_name_.clear();
        close_owned(outer_cleanup_parent_fd_);
        outer_observe(pending_advertisement_update_);
        break;
    case sidecar::LifecycleAction::None:
        break;
    }
}

void DaemonSidecarAdapter::outer_observe(advertisement::Update& update) noexcept
{
    if (outer_lifecycle_ == nullptr)
        return;
    const sidecar::LifecycleState lifecycle_state = outer_lifecycle_->state();
    const bool exact_listener = public_listener_.bound &&
                                public_listener_.port == config_.public_listener_port;
    const bool nodes = outer_ready_lease_.has_value() && runtime_nodes_valid();
    const bool ready = lifecycle_state == sidecar::LifecycleState::Ready &&
                       outer_ready_lease_.has_value() && outer_ready_lease_->valid();
    if (ready && !nodes && !outer_shutdown_requested_ &&
        !outer_replacement_requested_) {
        // A READY relationship is no longer trustworthy when either the
        // captured attempt directory or listener inode has changed.  This is
        // a reducer observation, not a cleanup authority: withdraw first and
        // let the next outer turn prove exact A teardown before any allocator
        // successor can be launched.
        outer_replacement_requested_ = true;
        outer_replacement_input_close_pending_ = true;
        outer_authenticated_ = false;
        outer_auth_start_pending_ = false;
        disable_relationship();
    }
    advertisement::Observation observation;
    observation.public_listener_bound = exact_listener;
    observation.public_listener_port = exact_listener ? public_listener_.port : 0;
    observation.supervisor_state = advertisement_state(lifecycle_state);
    observation.private_relationship_authenticated =
        ready && outer_authenticated_ && nodes;
    observation.cumulative_post_ready_exits = cumulative_post_ready_exits_;
    // Advertisement and compiler eligibility are downstream of the private
    // relationship.  A valid READY frame alone is not a public/lease fence;
    // the exact peer credentials and HELLO/ACK exchange must have completed.
    observation.current_lease_matches = ready && outer_authenticated_ && nodes;
    const advertisement::Update observed = controller_.observe(observation);
    append_update(update, observed);
    if (state_ != AdapterState::ShuttingDown && state_ != AdapterState::Failed)
        state_ = ready && outer_authenticated_ && nodes ? AdapterState::Ready
                                                        : AdapterState::Absent;
}

void DaemonSidecarAdapter::outer_append_pollfds(
    std::vector<pollfd>& pollfds) const noexcept
{
    const auto append = [&](int fd, short events) {
        if (fd >= 0)
            pollfds.push_back(pollfd{fd, events, 0});
    };
    append(outer_exec_fd_, POLLIN | POLLERR | POLLHUP);
    append(outer_ready_fd_, POLLIN | POLLERR | POLLHUP);
    append(outer_pidfd_, POLLIN | POLLERR | POLLHUP);
    append(outer_auth_fd_,
           (outer_auth_phase_ == kAuthConnectPending || outer_auth_phase_ == 1 ||
            outer_auth_phase_ == 2 ||
            outer_auth_phase_ == 4 || outer_auth_phase_ == 5)
               ? static_cast<short>(POLLOUT | POLLERR | POLLHUP)
                                  : static_cast<short>(POLLIN | POLLERR | POLLHUP));
    if (outer_input_operation_ != nullptr)
        append(outer_input_operation_->native_handle(),
               outer_input_operation_->desired_events() | POLLERR | POLLHUP);
}

std::chrono::steady_clock::time_point
DaemonSidecarAdapter::outer_next_deadline() const noexcept
{
    auto deadline = outer_lifecycle_ != nullptr ? outer_lifecycle_->next_deadline()
                                                : std::chrono::steady_clock::time_point{};
    if (outer_launch_phase_ != launch_phase(OuterLaunchPhase::None) &&
        outer_launch_deadline_ != std::chrono::steady_clock::time_point{} &&
        (deadline == std::chrono::steady_clock::time_point{} ||
         outer_launch_deadline_ < deadline))
        deadline = outer_launch_deadline_;
    // The authenticated control descriptor intentionally remains open for
    // later operations.  Its connect/HELLO deadline applies only while the
    // authentication state machine is active; carrying that expired deadline
    // after phase 0 forces the daemon into poll(..., 0) forever.
    if (outer_auth_fd_ >= 0 && outer_auth_phase_ != 0 &&
        outer_auth_deadline_ != std::chrono::steady_clock::time_point{} &&
        (deadline == std::chrono::steady_clock::time_point{} ||
         outer_auth_deadline_ < deadline))
        deadline = outer_auth_deadline_;
    if (outer_input_operation_ != nullptr &&
        (deadline == std::chrono::steady_clock::time_point{} ||
         outer_input_operation_->deadline() < deadline))
        deadline = outer_input_operation_->deadline();
    if (outer_input_operation_ == nullptr && !pending_input_lifecycle_.empty() &&
        pending_input_lifecycle_.front().deadline !=
            std::chrono::steady_clock::time_point{} &&
        (deadline == std::chrono::steady_clock::time_point{} ||
         pending_input_lifecycle_.front().deadline < deadline))
        deadline = pending_input_lifecycle_.front().deadline;
    return deadline;
}

bool DaemonSidecarAdapter::outer_immediate_turn_required() const noexcept
{
    const sidecar::LifecycleState lifecycle_state =
        outer_lifecycle_ != nullptr ? outer_lifecycle_->state()
                                    : sidecar::LifecycleState::Stopped;
    const bool cleanup_step_ready =
        outer_cleanup_phase_ != 0 && outer_cleanup_step_ != 0;
    const bool path_observation_ready =
        outer_cleanup_phase_ == 0 &&
        lifecycle_state == sidecar::LifecycleState::ReapAndGroupCheck;
    const bool retry_ready =
        lifecycle_state == sidecar::LifecycleState::RetryEligible &&
        outer_replacement_requested_ && !outer_shutdown_requested_ &&
        outer_scheduler_owner_active_;
    const bool reducer_request_ready =
        (outer_shutdown_requested_ || outer_replacement_requested_) &&
        (lifecycle_state == sidecar::LifecycleState::LaunchPrepared ||
         lifecycle_state == sidecar::LifecycleState::ForkedAwaitExecAndReady ||
         lifecycle_state == sidecar::LifecycleState::Ready);
    const bool signal_step_ready =
        outer_lifecycle_ != nullptr &&
        ((lifecycle_state == sidecar::LifecycleState::TerminatingGrace &&
          !outer_lifecycle_->term_sent()) ||
         (lifecycle_state == sidecar::LifecycleState::TerminatingKill &&
          !outer_lifecycle_->kill_sent()));
    return outer_launch_phase_ != 0 || cleanup_step_ready ||
           path_observation_ready || retry_ready || reducer_request_ready ||
           signal_step_ready ||
           outer_pending_action_.has_value() || outer_reap_event_pending_ ||
           outer_shutdown_input_close_pending_ ||
           outer_replacement_input_close_pending_ ||
           outer_auth_start_pending_ || outer_auth_failure_ ||
           outer_input_cancel_lease_.has_value() ||
           outer_identity_report_pending_ || outer_launch_failed_ ||
           (outer_ready_complete_ && !outer_ready_invalid_ &&
            lifecycle_state == sidecar::LifecycleState::ForkedAwaitExecAndReady);
}

bool DaemonSidecarAdapter::outer_advance_input(
    std::chrono::steady_clock::time_point now,
    const std::vector<pollfd>& pollfds,
    advertisement::Update& update) noexcept
{
    // A remote InputRecord operation is serialized behind the authenticated
    // sidecar relationship.  The adapter owns only this typed lease binding;
    // all record/cursor/replay state remains in the sidecar owner.
    if (outer_input_operation_ == nullptr) {
        if (pending_input_lifecycle_.empty() || !outer_authenticated_ ||
            !outer_ready_lease_.has_value() || !outer_ready_lease_->valid() ||
            !runtime_nodes_valid())
            return false;
        const InputLifecycleRequest request = pending_input_lifecycle_.front();
        const auto request_deadline =
            request.deadline == std::chrono::steady_clock::time_point{}
                ? now + config_.input_lifecycle_timeout
                : request.deadline;
        if (now >= request_deadline) {
            pending_input_lifecycle_.erase(pending_input_lifecycle_.begin());
            outer_last_input_lifecycle_result_ = InputLifecycleResult{
                InputLifecycleStatus::Timeout, request};
            retire_input_lifecycle_relationship(AdapterError::InputLifecycleProtocol);
            outer_action_taken_ = true;
            return true;
        }
        if (request.identity !=
            local::Identity{config_.generation, outer_ready_lease_->identity.attempt}) {
            pending_input_lifecycle_.erase(pending_input_lifecycle_.begin());
            outer_last_input_lifecycle_result_ = InputLifecycleResult{
                InputLifecycleStatus::StoreReplaced, request};
            outer_action_taken_ = true;
            return true;
        }
        try {
            outer_input_operation_ = std::make_unique<local::DaemonControlOperation>();
            outer_input_request_ = request;
            const local::ControlOperation operation =
                local::make_input_lifecycle_operation(request);
            // The service accepts on the listener created by iceccd before
            // fork, so Linux reports iceccd as the peer socket's creator.
            // READY still binds the serving child and store incarnation.
            const local::CredentialExpectation credentials{
                config_.expected_service_uid, config_.expected_service_gid,
                static_cast<uint64_t>(::getpid())};
            int socket_type = SOCK_STREAM;
#ifdef SOCK_CLOEXEC
            socket_type |= SOCK_CLOEXEC;
#endif
#ifdef SOCK_NONBLOCK
            socket_type |= SOCK_NONBLOCK;
#else
            int control_fd = -1;
#endif
#ifdef SOCK_NONBLOCK
            int control_fd = ::socket(AF_UNIX, socket_type, 0);
#endif
            const local::DaemonControlStatus status = control_fd >= 0
                ? outer_input_operation_->begin_connecting(
                      socket_path_, control_fd, operation, -1, credentials,
                      request_deadline,
                      local::DaemonControlLimits{1, 4096},
                      local::DaemonControlFdOwnership::Owned)
                : local::DaemonControlStatus::IoError;
            if (status != local::DaemonControlStatus::InProgress) {
                outer_last_input_lifecycle_result_ = InputLifecycleResult{
                    map_input_control_status(status), request};
                outer_input_operation_.reset();
                outer_input_request_.reset();
                outer_input_cancel_lease_.reset();
                // Every control/input failure is an incarnation observation,
                // not a private retry authority.  Route it through the same
                // withdrawal/teardown reducer as scheduler/runtime loss so a
                // disconnected or malformed operation cannot leave A live
                // with an uncertain record owner.
                retire_input_lifecycle_relationship(AdapterError::InputLifecycleProtocol);
                outer_action_taken_ = true;
                return true;
            }
        } catch (...) {
            outer_input_operation_.reset();
            outer_input_request_.reset();
            outer_input_cancel_lease_.reset();
            retire_input_lifecycle_relationship(AdapterError::InputLifecycleCapacity);
            outer_action_taken_ = true;
            return true;
        }
        // Socket creation is the single control action for this turn.  The
        // newly-created descriptor is connected by the operation on a later
        // poll turn; connect/send/receive are never hidden in this adapter
        // call.
        outer_action_taken_ = true;
        return true;
    }

    short revents = 0;
    for (const pollfd& descriptor : pollfds)
        if (descriptor.fd == outer_input_operation_->native_handle()) {
            revents = descriptor.revents;
            break;
        }
    const local::DaemonControlStatus status =
        outer_input_operation_->advance(now, revents);
    outer_action_taken_ = true;
    if (!outer_input_operation_->done())
        return true;

    const InputLifecycleRequest request = outer_input_request_.value_or(
        pending_input_lifecycle_.empty() ? InputLifecycleRequest{}
                                         : pending_input_lifecycle_.front());
    InputLifecycleStatus result_status = map_input_control_status(status);
    bool relationship_failure_routed = false;
    if (status == local::DaemonControlStatus::Complete) {
        const auto& remote = outer_input_operation_->lifecycle_result();
        if (!remote.has_value()) {
            result_status = InputLifecycleStatus::MalformedResponse;
            retire_input_lifecycle_relationship(AdapterError::InputLifecycleProtocol);
            relationship_failure_routed = true;
        } else {
            switch (*remote) {
            case InputLifecycleApplyStatus::Applied:
                result_status = InputLifecycleStatus::Applied;
                break;
            case InputLifecycleApplyStatus::AlreadyApplied:
                result_status = InputLifecycleStatus::AlreadyApplied;
                break;
            case InputLifecycleApplyStatus::InvalidArgument:
                result_status = InputLifecycleStatus::InvalidArgument;
                break;
            case InputLifecycleApplyStatus::UnknownRecord:
                result_status = InputLifecycleStatus::UnknownRecord;
                break;
            case InputLifecycleApplyStatus::StaleOwner:
                result_status = InputLifecycleStatus::StaleOwner;
                break;
            case InputLifecycleApplyStatus::ConflictingReplay:
                result_status = InputLifecycleStatus::ConflictingReplay;
                break;
            case InputLifecycleApplyStatus::CapacityExceeded:
                result_status = InputLifecycleStatus::CapacityExceeded;
                break;
            case InputLifecycleApplyStatus::AttemptQuiescedRecordRetained:
                result_status = InputLifecycleStatus::AttemptQuiescedRecordRetained;
                break;
            case InputLifecycleApplyStatus::ReplacementInstalledRecordRetained:
                result_status = InputLifecycleStatus::ReplacementInstalledRecordRetained;
                break;
            case InputLifecycleApplyStatus::JobClosedRecordRetained:
                result_status = InputLifecycleStatus::JobClosedRecordRetained;
                break;
            case InputLifecycleApplyStatus::JobClosedRecordReclaimed:
                result_status = InputLifecycleStatus::JobClosedRecordReclaimed;
                break;
            case InputLifecycleApplyStatus::RetirementProofRequired:
                result_status = InputLifecycleStatus::RetirementProofRequired;
                break;
            case InputLifecycleApplyStatus::GenerationMismatch:
                result_status = InputLifecycleStatus::GenerationMismatch;
                break;
            }
        }
    }
    outer_last_input_lifecycle_result_ = InputLifecycleResult{result_status, request};
    const bool success =
        result_status == InputLifecycleStatus::Applied ||
        result_status == InputLifecycleStatus::AlreadyApplied ||
        result_status == InputLifecycleStatus::AttemptQuiescedRecordRetained ||
        result_status == InputLifecycleStatus::ReplacementInstalledRecordRetained ||
        result_status == InputLifecycleStatus::JobClosedRecordRetained ||
        result_status == InputLifecycleStatus::JobClosedRecordReclaimed;
    if (success && !remember_completed_input_lifecycle(request)) {
        retire_input_lifecycle_relationship(AdapterError::InputLifecycleCapacity);
    } else if (success && !pending_input_lifecycle_.empty() &&
               pending_input_lifecycle_.front() == request) {
        pending_input_lifecycle_.erase(pending_input_lifecycle_.begin());
    } else if (!success && !relationship_failure_routed) {
        // Timeout, EOF, stale/mismatched identity, a rejected replay, and
        // every malformed semantic result all retire the current
        // incarnation.  There is no second input-specific cleanup/retry path;
        // the lifecycle reducer owns the resulting replacement ordering.
        retire_input_lifecycle_relationship(AdapterError::InputLifecycleProtocol);
    }
    outer_input_operation_.reset();
    outer_input_request_.reset();
    outer_input_cancel_lease_.reset();
    (void)update;
    return true;
}

bool DaemonSidecarAdapter::outer_observe_child_reaped(pid_t pid, int status,
                                                       bool echild) noexcept
{
    (void)pid;
    (void)status;
    (void)echild;
    // A scalar PID/status (including an ECHILD marker) has no pidfd,
    // registry-generation, or LaunchIncarnation binding and therefore cannot
    // affect counters or lifecycle state.  The central registry's immutable
    // ReapEvent overload below is the sole positive route.
    return false;
}

bool DaemonSidecarAdapter::outer_observe_child_reaped(
    const sidecar::ReapEvent& event) noexcept
{
    if (outer_lifecycle_ == nullptr || !event.exact_identity() ||
        event.echild || event.kind != sidecar::ReaperOwnerKind::Sidecar ||
        event.owner != outer_lifecycle_->owner_key() || event.pid != outer_pid_ ||
        outer_pidfd_ < 0 || event.pidfd != outer_pidfd_ ||
        !outer_registration_.valid() ||
        event.registry_generation != outer_registration_.registry_generation())
        return false;
    // The registry has already enqueued this immutable value event.  Record
    // the readiness classification for advertisement counters and let the
    // reducer consume the mailbox on its next bounded advance.
    const bool post_ready =
        outer_reap_was_ready_ ||
        outer_ready_had_been_published_ ||
        outer_lifecycle_->state() == sidecar::LifecycleState::Ready ||
        outer_ready_lease_.has_value() || outer_authenticated_;
    outer_reap_was_ready_ = post_ready;
    if (post_ready && !outer_post_ready_exit_counted_) {
        if (cumulative_post_ready_exits_ != std::numeric_limits<uint64_t>::max())
            ++cumulative_post_ready_exits_;
        outer_post_ready_exit_counted_ = true;
        outer_ready_had_been_published_ = false;
    }
    outer_reap_event_pending_ = true;
    return true;
}

bool DaemonSidecarAdapter::outer_begin_turn(
    std::chrono::steady_clock::time_point now, advertisement::Update* result) noexcept
{
    advertisement::Update update;
    append_pending_advertisement_update(update);
    outer_action_taken_ = false;
    if (outer_lifecycle_ == nullptr || !outer_config_valid_ ||
        outer_reaper_ == nullptr) {
        fail(AdapterError::InvalidConfiguration);
        outer_observe(update);
        if (result != nullptr)
            *result = update;
        return false;
    }
    if (outer_shutdown_requested_ ||
        outer_lifecycle_->state() == sidecar::LifecycleState::FailedClosed ||
        outer_lifecycle_->state() == sidecar::LifecycleState::DegradedLegacy) {
        outer_observe(update);
        if (result != nullptr)
            *result = update;
        return false;
    }
    if (outer_cleanup_phase_ != 0 || outer_pending_action_.has_value() ||
        outer_auth_fd_ >= 0 || outer_auth_start_pending_) {
        outer_observe(update);
        if (result != nullptr)
            *result = update;
        return state_ == AdapterState::Ready;
    }
    // A scheduler/runtime replacement request deliberately parks at
    // RetryEligible after exact A teardown.  Only a later active scheduler
    // turn may release that park and ask the allocator for B; the scheduler
    // loss path never launches while the relationship is absent.
    // `outer_begin_turn()` is called once at the beginning of each daemon
    // iteration only to reset the adapter's action quota and publish the
    // current level.  The initial Stopped->LaunchPrepared transition is the
    // sole reducer transition owned here.  RetryEligible is deliberately
    // started by `outer_advance_turn()` on a later iteration, after the
    // normal poll boundary, so one daemon turn cannot execute begin() and
    // advance() back-to-back.
    if (outer_lifecycle_->state() == sidecar::LifecycleState::Stopped) {
        const sidecar::LifecycleActionResult action =
            outer_lifecycle_->begin(now);
        if (action.action != sidecar::LifecycleAction::None)
            outer_apply_action(action, now);
    }
    outer_observe(update);
    if (result != nullptr)
        *result = update;
    return state_ == AdapterState::Ready;
}

bool DaemonSidecarAdapter::outer_advance_turn(
    std::chrono::steady_clock::time_point now, const std::vector<pollfd>& pollfds,
    advertisement::Update* result) noexcept
{
    advertisement::Update update;
    append_pending_advertisement_update(update);
    outer_action_taken_ = false;
    if (outer_lifecycle_ == nullptr) {
        outer_reap_event_pending_ = false;
        if (result != nullptr)
            *result = update;
        return false;
    }
    if (outer_launch_phase_ != launch_phase(OuterLaunchPhase::None)) {
        // A shutdown/replacement can arrive while the allocator's launch
        // plan is still pre-fork.  Abort that plan one close phase at a time;
        // do not create B (or even a child) after the caller has withdrawn
        // eligibility.  Once fork has transferred a child, finish the
        // ownership-registration phases so the central reaper can retire it
        // exactly rather than abandoning a gate-blocked process.
        const OuterLaunchPhase launch_state = launch_phase(outer_launch_phase_);
        const bool abort_phase =
            launch_state == OuterLaunchPhase::AbortCloseReadyRead ||
            launch_state == OuterLaunchPhase::AbortCloseReadyWrite ||
            launch_state == OuterLaunchPhase::AbortCloseExecRead ||
            launch_state == OuterLaunchPhase::AbortCloseExecWrite ||
            launch_state == OuterLaunchPhase::AbortCloseGateRead ||
            launch_state == OuterLaunchPhase::AbortCloseGateWrite ||
            launch_state == OuterLaunchPhase::AbortCloseListener ||
            launch_state == OuterLaunchPhase::AbortClosePidfd ||
            launch_state == OuterLaunchPhase::AbortDisarmRegistration ||
            launch_state == OuterLaunchPhase::AbortDone;
        const bool launch_expired =
            outer_launch_deadline_ != std::chrono::steady_clock::time_point{} &&
            now >= outer_launch_deadline_;
        if ((outer_shutdown_requested_ || outer_replacement_requested_ ||
             launch_expired) &&
            outer_pid_ <= 1 && !abort_phase &&
            // The mkdir phase deliberately hands off to this one exact lstat
            // before an abort.  Without that observation a pre-fork shutdown
            // would know the pathname text but not its device/inode and could
            // neither remove the attempt leaf nor prove that it was absent.
            launch_state != OuterLaunchPhase::ObserveDirectory) {
            outer_launch_setup_failed_ = true;
            outer_launch_phase_ = launch_phase(OuterLaunchPhase::AbortCloseReadyRead);
        }
        (void)outer_advance_launch_step();
        outer_action_taken_ = true;
        outer_observe(update);
        if (result != nullptr)
            *result = update;
        return state_ == AdapterState::Ready;
    }
    // The central reaper publishes an immutable exact-PID event after the
    // preceding lifecycle turn.  Consume that mailbox event before starting
    // another fallible auth/input/cleanup action; any reducer action caused by
    // the event is deferred one turn below.
    const bool reaper_delivery_pending = outer_reap_event_pending_;

    // A reducer action deferred after a receive is the only work permitted in
    // this turn.  This is the explicit one-action gate for launch/TERM/KILL.
    if (outer_pending_action_.has_value()) {
        const sidecar::LifecycleActionResult action = *outer_pending_action_;
        outer_pending_action_.reset();
        outer_apply_action(action, now);
        outer_observe(update);
        if (result != nullptr)
            *result = update;
        return state_ == AdapterState::Ready;
    }
    if (outer_input_cancel_lease_.has_value() && !reaper_delivery_pending) {
        // Cancellation is posted by the role owner, but the owner performs
        // the descriptor close and common-retirement transition only from its
        // serialized outer turn.  Revalidate the complete lease before doing
        // anything: a late timer/completion from A cannot cancel B after the
        // role slot has been reused.
        const auto current = outer_input_operation_lease();
        if (!current.has_value() || *current != *outer_input_cancel_lease_) {
            outer_input_cancel_lease_.reset();
        } else {
            if (!outer_input_request_.has_value() &&
                pending_input_lifecycle_.empty()) {
                outer_input_cancel_lease_.reset();
                outer_observe(update);
                if (result != nullptr)
                    *result = update;
                return state_ == AdapterState::Ready;
            }
            const InputLifecycleRequest request =
                outer_input_request_.value_or(pending_input_lifecycle_.front());
            if (outer_input_operation_ != nullptr)
                outer_input_operation_.reset();
            if (!pending_input_lifecycle_.empty() &&
                pending_input_lifecycle_.front() == request)
                pending_input_lifecycle_.erase(pending_input_lifecycle_.begin());
            outer_input_request_.reset();
            outer_last_input_lifecycle_result_ = InputLifecycleResult{
                InputLifecycleStatus::Disconnected, request};
            outer_input_cancel_lease_.reset();
            // Explicit cancellation is an input-lifecycle failure and enters
            // the same A withdrawal/teardown reducer as timeout, EOF, and
            // malformed control.  It never starts a local retry or B.
            retire_input_lifecycle_relationship(
                AdapterError::InputLifecycleProtocol);
            outer_action_taken_ = true;
            outer_observe(update);
            if (result != nullptr)
                *result = update;
            return state_ == AdapterState::Ready;
        }
    }
    if (outer_cleanup_phase_ != 0 && !reaper_delivery_pending) {
        const bool cleanup_action = outer_cleanup_exact();
        if (cleanup_action)
            outer_action_taken_ = true;
        // Path replacement or an unavailable identity-aware removal primitive
        // must not turn CleanupPath into an unbounded inner retry loop.  Once
        // the lifecycle's absolute teardown deadline is reached, feed the
        // pure timeout observation back to the reducer; FailedClosed retains
        // the PID/registration/path identity for the external authority.
        if (!cleanup_action && outer_lifecycle_ != nullptr &&
            now >= outer_lifecycle_->next_deadline()) {
            const sidecar::LifecycleActionResult timeout_action =
                outer_lifecycle_->advance(now, {});
            if (timeout_action.action != sidecar::LifecycleAction::None)
                outer_apply_action(timeout_action, now);
        }
        outer_observe(update);
        if (result != nullptr)
            *result = update;
        return state_ == AdapterState::Ready;
    }
    if (outer_shutdown_requested_ && outer_shutdown_input_close_pending_ &&
        !reaper_delivery_pending) {
        // Fence queued/active InputRecord commands before touching the
        // authenticated control lease.  Resetting the operation closes at
        // most one owned descriptor; the reducer gets a later turn.
        if (outer_input_operation_ != nullptr)
            outer_input_operation_.reset();
        outer_input_request_.reset();
        outer_input_cancel_lease_.reset();
        pending_input_lifecycle_.clear();
        outer_shutdown_input_close_pending_ = false;
        outer_action_taken_ = true;
        outer_observe(update);
        if (result != nullptr)
            *result = update;
        return state_ == AdapterState::Ready;
    }
    if (outer_replacement_requested_ &&
        outer_replacement_input_close_pending_ && !reaper_delivery_pending) {
        // Scheduler/runtime loss has the same ownership fence as shutdown,
        // but leaves the allocator/lifecycle eligible to mint a successor
        // once exact A teardown reaches RetryEligible.
        if (outer_input_operation_ != nullptr)
            outer_input_operation_.reset();
        outer_input_request_.reset();
        outer_input_cancel_lease_.reset();
        pending_input_lifecycle_.clear();
        outer_replacement_input_close_pending_ = false;
        outer_action_taken_ = true;
        outer_observe(update);
        if (result != nullptr)
            *result = update;
        return state_ == AdapterState::Ready;
    }
    if ((outer_shutdown_requested_ || outer_replacement_requested_) &&
        (outer_auth_fd_ >= 0 || outer_auth_start_pending_) &&
        !reaper_delivery_pending) {
        // The control fd is a retained authenticated lease until this one
        // explicit shutdown/replacement-close action.  It is never closed as
        // a generic handoff completion side effect.
        close_owned(outer_auth_fd_);
        outer_auth_start_pending_ = false;
        outer_auth_phase_ = 0;
        outer_auth_failure_ = false;
        outer_action_taken_ = true;
        outer_observe(update);
        if (result != nullptr)
            *result = update;
        return state_ == AdapterState::Ready;
    }
    if (outer_auth_start_pending_ && !reaper_delivery_pending) {
        outer_auth_start_pending_ = false;
        if (!outer_begin_authentication(now)) {
            outer_auth_phase_ = 0;
            outer_auth_failure_ = true;
        }
        outer_action_taken_ = true;
        outer_observe(update);
        if (result != nullptr)
            *result = update;
        return state_ == AdapterState::Ready;
    }
    if (outer_auth_fd_ >= 0 && outer_auth_phase_ != 0 &&
        !reaper_delivery_pending) {
        short events = 0;
        for (const pollfd& descriptor : pollfds)
            if (descriptor.fd == outer_auth_fd_)
                events = descriptor.revents;
        (void)outer_advance_authentication(now, events);
        if (outer_auth_phase_ == 0 && !outer_authenticated_ &&
            outer_auth_fd_ < 0)
            outer_auth_failure_ = true;
        outer_action_taken_ = true;
        outer_observe(update);
        if (result != nullptr)
            *result = update;
        return state_ == AdapterState::Ready;
    }

    // A phase-open lease remains readable in the outer inventory, but it is
    // not an active receive operation.  Only a terminal poll event is an
    // externally observable failure that should close it and re-enter the
    // common replacement reducer; ordinary POLLIN is left for the future
    // typed result/session owner.
    if (outer_auth_fd_ >= 0 && outer_auth_phase_ == 0 &&
        !reaper_delivery_pending) {
        short events = 0;
        for (const pollfd& descriptor : pollfds)
            if (descriptor.fd == outer_auth_fd_)
                events = descriptor.revents;
        if ((events & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            close_owned(outer_auth_fd_);
            outer_authenticated_ = false;
            outer_auth_failure_ = true;
            outer_action_taken_ = true;
            outer_observe(update);
            if (result != nullptr)
                *result = update;
            return state_ == AdapterState::Ready;
        }
    }

    if (outer_auth_failure_ && !reaper_delivery_pending) {
        outer_auth_failure_ = false;
        sidecar::LifecycleObservation failure;
        failure.request_replacement = true;
        const sidecar::LifecycleActionResult action = outer_lifecycle_->advance(
            now, failure);
        if (action.action != sidecar::LifecycleAction::None)
            outer_apply_action(action, now);
        outer_observe(update);
        if (result != nullptr)
            *result = update;
        return state_ == AdapterState::Ready;
    }

    // InputRecord retirement/settlement has one independent incremental
    // control operation.  It gets the whole turn when present; the sidecar
    // lifecycle reducer is not advanced again in the same turn.
    if (!reaper_delivery_pending && outer_advance_input(now, pollfds, update)) {
        outer_observe(update);
        if (result != nullptr)
            *result = update;
        return state_ == AdapterState::Ready;
    }

    // A completed A teardown leaves the reducer at RetryEligible.  Minting B
    // is a new allocator/reducer turn, and must not be hidden in the
    // begin-turn quota reset above.  The replacement request is consumed only
    // here, after all queued input/control work and exact cleanup have
    // completed; shutdown never enters this branch and therefore can never
    // launch after an unproved retirement.
    if (!reaper_delivery_pending && outer_scheduler_owner_active_ &&
        !outer_shutdown_requested_ &&
        outer_replacement_requested_ &&
        outer_lifecycle_->state() == sidecar::LifecycleState::RetryEligible) {
        if (!reserve_outer_restart()) {
            outer_shutdown_requested_ = true;
            fail(AdapterError::AttemptExhausted);
            outer_observe(update);
            if (result != nullptr)
                *result = update;
            return false;
        }
        outer_replacement_requested_ = false;
        const sidecar::LifecycleActionResult replacement =
            outer_lifecycle_->begin(now);
        if (replacement.action != sidecar::LifecycleAction::None)
            outer_apply_action(replacement, now);
        outer_observe(update);
        if (result != nullptr)
            *result = update;
        return state_ == AdapterState::Ready;
    }

    sidecar::LifecycleObservation observation;
    if (outer_shutdown_requested_)
        observation.request_legacy = true;
    if (outer_replacement_requested_)
        observation.request_replacement = true;
    if (outer_launch_failed_) {
        observation.exec = sidecar::ExecObservation::Failed;
        outer_launch_failed_ = false;
    }
    if (outer_identity_report_pending_) {
        observation.pid = outer_pid_;
        observation.observed_pgid = outer_pgid_;
        outer_identity_report_pending_ = false;
    }
    if (outer_exec_complete_)
        observation.exec = outer_exec_failed_ ? sidecar::ExecObservation::Failed
                                              : sidecar::ExecObservation::Succeeded;
    if (outer_ready_complete_) {
        observation.ready = outer_ready_invalid_ ? sidecar::ReadyObservation::Invalid
                                                  : sidecar::ReadyObservation::Complete;
        if (outer_ready_lease_.has_value())
            observation.ready_lease = outer_ready_lease_;
        observation.store_generation = outer_ready_lease_.has_value()
                                           ? outer_ready_lease_->store_generation
                                           : 0;
        // The adapter owns the accumulated READY bytes and has already
        // parsed them at the pipe boundary.  Do not replay the whole buffer
        // into the reducer on every quiet turn; the reducer receives only the
        // typed completion/identity facts below.
    }

    // Select one descriptor action.  PIDFD readiness is only a hint: the
    // central reaper's exact-PID mailbox remains the sole reap authority.
    // A group/path observation is itself one fallible kernel action.  Keep
    // that action separate from TERM/KILL (and from any other externally
    // visible action) by deferring a reducer signal to the next turn.
    bool observation_action_taken = false;
    int selected_fd = -1;
    short selected_revents = 0;
    for (const pollfd& descriptor : pollfds) {
        if ((descriptor.revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) == 0)
            continue;
        if (descriptor.fd == outer_exec_fd_ || descriptor.fd == outer_ready_fd_) {
            selected_fd = descriptor.fd;
            selected_revents = descriptor.revents;
            break;
        }
    }
    bool received = false;
    if (selected_fd == outer_exec_fd_)
        received = outer_read_exec();
    else if (selected_fd == outer_ready_fd_)
        received = outer_read_ready();
    if (received) {
        outer_action_taken_ = true;
        observation.exec = outer_exec_complete_
                               ? (outer_exec_failed_ ? sidecar::ExecObservation::Failed
                                                      : sidecar::ExecObservation::Succeeded)
                               : sidecar::ExecObservation::None;
        if (outer_ready_complete_) {
            observation.ready = outer_ready_invalid_ ? sidecar::ReadyObservation::Invalid
                                                      : sidecar::ReadyObservation::Complete;
            if (outer_ready_lease_.has_value()) {
                observation.ready_lease = outer_ready_lease_;
                observation.store_generation = outer_ready_lease_->store_generation;
            }
            // A newly completed valid READY frame still needs the outer
            // loop's exact pathname observation.  Defer the pure reducer
            // acceptance to the next turn; otherwise a read action would be
            // coupled to a second lstat() and the reducer would have no
            // device/inode facts to compare.  Invalid READY remains a
            // fail-closed observation and may be reduced immediately.
            if (received && !outer_ready_invalid_)
                observation.ready = sidecar::ReadyObservation::None;
        } else if (!outer_ready_bytes_.empty()) {
            observation.ready = sidecar::ReadyObservation::Partial;
            // Feed only the bytes appended by this receive action.  The
            // adapter retains the complete prefix for framing, but replaying
            // that prefix on every quiet/EAGAIN turn would make the reducer's
            // bounded READY buffer grow without receiving any new data.
            if (outer_ready_reported_ > outer_ready_bytes_.size())
                outer_ready_reported_ = 0;
            if (outer_ready_reported_ < outer_ready_bytes_.size()) {
                observation.ready_bytes = std::string_view(
                    outer_ready_bytes_).substr(outer_ready_reported_);
                outer_ready_reported_ = outer_ready_bytes_.size();
            }
        }
    } else if (!reaper_delivery_pending) {
        // Group ESRCH and pathname lstat are independent kernel observations;
        // never issue both in one daemon turn.  A proven Gone group is carried
        // as an immutable in-memory fact while the next turn obtains the
        // path observation needed by ReapAndGroupCheck.
        const sidecar::LifecycleState lifecycle_state = outer_lifecycle_->state();
        const bool needs_path =
            (outer_ready_complete_ && !outer_ready_invalid_) ||
            (outer_launch_failure_path_pending_ && outer_pid_ <= 1);
        const bool needs_group =
            lifecycle_state == sidecar::LifecycleState::TerminatingGrace ||
            lifecycle_state == sidecar::LifecycleState::TerminatingKill ||
            lifecycle_state == sidecar::LifecycleState::ReapAndGroupCheck;
        if (lifecycle_state == sidecar::LifecycleState::ReapAndGroupCheck) {
            if (!outer_group_gone_observed_)
                observation_action_taken = outer_group_observation(observation);
            else {
                observation.group = sidecar::GroupObservation::Gone;
                observation.observed_pgid = outer_pgid_;
                observation.group_domain = outer_group_gone_domain_;
                observation_action_taken = outer_path_observation(observation);
            }
        } else if (needs_group) {
            observation_action_taken = outer_group_observation(observation);
        } else if (needs_path) {
            observation_action_taken = outer_path_observation(observation);
        }
    }
    // The kill-domain capability was captured by outer_prepare_launch().  It
    // is an immutable fact carried alongside every reducer observation; in
    // particular, a GroupObservation::Gone without this exact token can never
    // satisfy teardown.
    if (outer_group_domain_.has_value())
        observation.group_domain = *outer_group_domain_;

    const sidecar::LifecycleActionResult action = outer_lifecycle_->advance(now,
                                                                             observation);
    outer_reap_event_pending_ = false;
    if (action.action != sidecar::LifecycleAction::None) {
        const bool external = action.action == sidecar::LifecycleAction::SendTerm ||
                              action.action == sidecar::LifecycleAction::SendKill ||
                              action.action == sidecar::LifecycleAction::LaunchPrepared;
        if (reaper_delivery_pending || (received && external) ||
            (observation_action_taken && external))
            outer_pending_action_ = action;
        else
            outer_apply_action(action, now);
    }
    if (outer_lifecycle_->state() == sidecar::LifecycleState::Ready &&
        !outer_authenticated_ && outer_auth_fd_ < 0 && !outer_auth_start_pending_)
        outer_auth_start_pending_ = true;
    outer_observe(update);
    if (result != nullptr)
        *result = update;
    (void)selected_revents;
    return state_ == AdapterState::Ready;
}

void DaemonSidecarAdapter::outer_request_shutdown(advertisement::Update* result) noexcept
{
    outer_scheduler_owner_active_ = false;
    outer_replacement_requested_ = false;
    outer_replacement_input_close_pending_ = false;
    outer_shutdown_requested_ = true;
    outer_shutdown_input_close_pending_ = true;
    advertisement::Update update;
    append_pending_advertisement_update(update);
    // Shutdown is only a reducer request.  Do not advance the lifecycle,
    // signal, close the authenticated lease, or consume input in this call;
    // the daemon's next outer turn owns exactly one poll and one action.
    outer_authenticated_ = false;
    outer_ready_lease_.reset();
    disable_relationship();
    outer_auth_start_pending_ = false;
    outer_observe(update);
    if (result != nullptr)
        *result = update;
}

void DaemonSidecarAdapter::outer_set_scheduler_owner(bool active) noexcept
{
    outer_scheduler_owner_active_ = active;
}

void DaemonSidecarAdapter::outer_request_replacement() noexcept
{
    if (outer_shutdown_requested_ || outer_replacement_requested_)
        return;

    // Scheduler/runtime loss is a replacement request, not a synchronous
    // teardown operation.  Withdraw the relationship immediately so no
    // caller can retain eligibility, but keep the exact A lease, PID, path,
    // and central registration until the reducer proves their retirement.
    outer_replacement_requested_ = true;
    outer_replacement_input_close_pending_ = true;
    outer_authenticated_ = false;
    outer_auth_start_pending_ = false;
    disable_relationship();

    advertisement::Update withdrawal;
    outer_observe(withdrawal);
    append_update(pending_advertisement_update_, withdrawal);
}

} // namespace icecc::p50::daemon
