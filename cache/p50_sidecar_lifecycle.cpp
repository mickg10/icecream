#include "p50_sidecar_lifecycle.h"

#include "services/logging.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <string>
#include <utility>
#include <cerrno>
#include <sys/stat.h>
#include <sys/wait.h>

namespace icecc::p50::sidecar {
namespace {

constexpr int64_t kMaximumTimeoutMilliseconds = 24 * 60 * 60 * 1000;

bool bounded_timeout(std::chrono::milliseconds value) noexcept {
    return value.count() > 0 && value.count() <= kMaximumTimeoutMilliseconds;
}

bool parse_uint(std::string_view text, uint64_t& result) noexcept {
    if (text.empty() || (text.size() > 1 && text.front() == '0'))
        return false;
    const char* first = text.data();
    const char* last = first + text.size();
    auto parsed = std::from_chars(first, last, result, 10);
    return parsed.ec == std::errc{} && parsed.ptr == last;
}

std::string bytes_hex(std::span<const uint8_t> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const uint8_t byte : bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

bool parse_hex(std::string_view text, std::array<uint8_t, 16>& bytes) noexcept {
    if (text.size() != bytes.size() * 2)
        return false;
    auto nibble = [](char value) noexcept -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i != bytes.size(); ++i) {
        const int high = nibble(text[i * 2]);
        const int low = nibble(text[i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        bytes[i] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
}

bool absolute_path(std::string_view path) noexcept {
    return detail::canonical_absolute_lease_path(path);
}

std::string identity_path(std::string_view root, const StoreIdentityRoot& store,
                          uint64_t attempt) {
    std::string path(root);
    path += "/attempt-";
    path += std::to_string(attempt);
    path += "-";
    path += bytes_hex(std::span<const uint8_t>(store.bytes.data(), store.bytes.size()));
    return path;
}

std::atomic<uint64_t> g_next_owner_id{1};

bool valid_kernel_exit_code(int code) noexcept {
    return code == CLD_EXITED || code == CLD_KILLED || code == CLD_DUMPED;
}

int wait_status_from_siginfo(const siginfo_t& information) noexcept {
    if (information.si_code == CLD_EXITED)
        return (information.si_status & 0xff) << 8;
    int status = information.si_status & 0x7f;
    if (information.si_code == CLD_DUMPED)
        status |= 0x80;
    return status;
}

// Probe one exact pidfd.  Linux exposes P_PIDFD as idtype value 3 even on
// libc headers that predate the spelling.  The first call leaves the kernel
// status waitable (WNOWAIT); the later call consumes that same exact handle.
// There is intentionally no EINTR retry here: a retry is a later outer-loop
// turn and therefore cannot hide multiple external actions in one turn.
bool observe_pidfd_status(int pidfd, pid_t expected_pid, bool consume,
                          int& code, int& status) noexcept {
#if defined(__linux__) && defined(WEXITED) && defined(WNOHANG) && defined(WNOWAIT)
    if (pidfd < 0 || expected_pid <= 1)
        return false;
    siginfo_t information{};
    int flags = WEXITED | WNOHANG;
    if (!consume)
        flags |= WNOWAIT;
    const int result = ::waitid(static_cast<idtype_t>(3),
                                static_cast<id_t>(pidfd), &information, flags);
    if (result != 0 || information.si_pid != expected_pid ||
        !valid_kernel_exit_code(information.si_code))
        return false;
    code = information.si_code;
    status = wait_status_from_siginfo(information);
    return true;
#else
    (void)pidfd;
    (void)expected_pid;
    (void)consume;
    (void)code;
    (void)status;
    return false;
#endif
}

// Compat counterpart of observe_pidfd_status() for kernel-3.10-class hosts
// where pidfd_open()/waitid(P_PIDFD) is ENOSYS (see
// cache/p50_compat_child_handle.h and doc/p50-platform-compat.md). Exactly
// one bounded syscall, no retry: a retry is a later outer-loop turn, same as
// the Native primitive above. Unlike compat_wait_bounded() in
// p50_compat_child_handle.cpp, this never sleeps or loops in a tight poll --
// it is called once per outer turn by the registry, which already owns the
// turn-by-turn scheduling. The starttime re-check immediately precedes the
// syscall rather than following it: once a WNOWAIT peek or a consuming wait
// succeeds, the kernel may remove the zombie's /proc entry from under a
// later re-check (consume) or a concurrent actor (peek), so checking after
// would reject successes this call itself just produced. This accepts the
// same pid_max-wrap residual risk CompatChildHandle::still_same_process()
// documents, and no more.
// invariant_violation is set true only when the (pid, starttime_ticks) fence
// itself is broken -- the /proc entry is gone past its bounded reach, or the
// pid has already been recycled by the kernel for an unrelated process.
// observed_starttime_ticks carries the mismatched value for diagnostics (0
// when the /proc entry could not be read at all). This is distinct from, and
// never set alongside, the benign "no change yet" result below (a WNOHANG
// waitid() that simply has nothing to report): a mismatched fence is a fatal
// invariant violation that must fail closed and never be retried, while an
// empty waitid() result is an ordinary not-yet-exited outcome polled again on
// a later outer turn. Callers must zero-initialize both out-params; this
// function only ever sets invariant_violation to true, never resets it.
bool observe_compat_status(pid_t pid, uint64_t starttime_ticks, bool consume,
                           int& code, int& status, bool& invariant_violation,
                           uint64_t& observed_starttime_ticks,
                           bool& observed_readable,
                           int& observed_errno) noexcept {
#if defined(__linux__) && defined(WEXITED) && defined(WNOHANG) && defined(WNOWAIT)
    if (pid <= 1 || starttime_ticks == 0)
        return false;
    unsigned long long current_starttime = 0;
    errno = 0;
    const bool stat_readable =
        icecc::p50::platform::read_starttime_ticks(pid, &current_starttime);
    const int stat_errno = errno;
    if (!stat_readable || current_starttime != starttime_ticks) {
        // fail closed: exited past /proc's reach, or pid reused
        invariant_violation = true;
        observed_starttime_ticks = current_starttime;
        observed_readable = stat_readable;
        // Only meaningful when !stat_readable: 0 there means a parse-only
        // failure (the read syscalls succeeded, but the content was
        // unparseable), nonzero names the real read failure. observed_readable
        // above, not this field, is what the caller must branch on.
        observed_errno = stat_readable ? 0 : stat_errno;
        return false;
    }
    siginfo_t information{};
    int flags = WEXITED | WNOHANG;
    if (!consume)
        flags |= WNOWAIT;
    const int result =
        ::waitid(P_PID, static_cast<id_t>(pid), &information, flags);
    if (result != 0 || information.si_pid != pid ||
        !valid_kernel_exit_code(information.si_code))
        return false;
    code = information.si_code;
    status = wait_status_from_siginfo(information);
    return true;
#else
    (void)pid;
    (void)starttime_ticks;
    (void)consume;
    (void)code;
    (void)status;
    (void)invariant_violation;
    (void)observed_starttime_ticks;
    (void)observed_readable;
    (void)observed_errno;
    return false;
#endif
}

} // namespace

KillDomainLease KillDomainVerifier::issue(pid_t pid, pid_t pgid,
                                          uint64_t serial) noexcept {
    if (pid <= 1 || pgid <= 1 || pid != pgid || serial == 0)
        return {};
    try {
        auto capability = std::make_shared<KillDomainLease::Capability>();
        capability->serial = serial;
        capability->pid = pid;
        capability->pgid = pgid;
        return KillDomainLease(std::move(capability));
    } catch (...) {
        return {};
    }
}

bool ReapMailbox::enqueue(ReapEvent event) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ >= kMaximumEvents)
        return false;
    try {
        events_[(head_ + count_) % kMaximumEvents] = std::move(event);
    } catch (...) {
        // LaunchIncarnation carries allocator-owned strings.  A mailbox
        // delivery failure must remain a bounded retry condition; it must not
        // terminate the daemon from this noexcept boundary or consume a slot.
        return false;
    }
    ++count_;
    return true;
}

bool ReapMailbox::dequeue(ReapEvent& event) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ == 0)
        return false;
    try {
        event = events_[head_];
    } catch (...) {
        // Keep the immutable event in place so a later bounded outer turn can
        // retry the publication instead of losing the only exact reap fact.
        return false;
    }
    head_ = (head_ + 1) % kMaximumEvents;
    --count_;
    return true;
}

bool ReapMailbox::enqueue_violation(CompatInvariantViolation violation) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (violation_.has_value())
        return false;
    try {
        violation_ = std::move(violation);
    } catch (...) {
        // Same allocator-failure contract as enqueue(): a delivery failure
        // here is a bounded retry condition, not a reason to terminate the
        // daemon from this noexcept boundary.
        violation_.reset();
        return false;
    }
    return true;
}

bool ReapMailbox::dequeue_violation(CompatInvariantViolation& violation) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!violation_.has_value())
        return false;
    try {
        violation = *violation_;
    } catch (...) {
        // Keep the single-slot violation in place so a later turn can retry
        // the publication rather than losing the only exact invariant fact.
        return false;
    }
    violation_.reset();
    return true;
}

bool LifecycleIdentity::valid() const noexcept {
    return control.generation != 0 && control.attempt != 0 && store_generation != 0 &&
           store_root.valid() && c_store_guid == c_store_guid_for_root(store_root) &&
           f_store_guid == f_store_guid_for_root(store_root) &&
           c_store_guid != f_store_guid && absolute_path(private_directory);
}

const char* lifecycle_state_name(LifecycleState state) noexcept {
    switch (state) {
    case LifecycleState::Stopped: return "Stopped";
    case LifecycleState::LaunchPrepared: return "LaunchPrepared";
    case LifecycleState::ForkedAwaitExecAndReady: return "ForkedAwaitExecAndReady";
    case LifecycleState::Ready: return "Ready";
    case LifecycleState::TerminatingGrace: return "TerminatingGrace";
    case LifecycleState::TerminatingKill: return "TerminatingKill";
    case LifecycleState::ReapAndGroupCheck: return "ReapAndGroupCheck";
    case LifecycleState::RetryEligible: return "RetryEligible";
    case LifecycleState::DegradedLegacy: return "DegradedLegacy";
    case LifecycleState::FailedClosed: return "FailedClosed";
    }
    return "Unknown";
}

const char* lifecycle_action_name(LifecycleAction action) noexcept {
    switch (action) {
    case LifecycleAction::None: return "None";
    case LifecycleAction::LaunchPrepared: return "LaunchPrepared";
    case LifecycleAction::Withdraw: return "Withdraw";
    case LifecycleAction::SendTerm: return "SendTerm";
    case LifecycleAction::SendKill: return "SendKill";
    case LifecycleAction::PublishReady: return "PublishReady";
    case LifecycleAction::CleanupPath: return "CleanupPath";
    case LifecycleAction::EnterDegradedLegacy: return "EnterDegradedLegacy";
    case LifecycleAction::RetryEligible: return "RetryEligible";
    case LifecycleAction::FailedClosed: return "FailedClosed";
    }
    return "Unknown";
}

SidecarLifecycle::SidecarLifecycle(SidecarLifecycleConfig config) noexcept
    : config_(std::move(config)) {
    owner_id_ = g_next_owner_id.fetch_add(1, std::memory_order_relaxed);
    if (owner_id_ == 0)
        owner_id_ = g_next_owner_id.fetch_add(1, std::memory_order_relaxed);
    try {
        reap_mailbox_ = std::make_shared<ReapMailbox>();
    } catch (...) {
        reap_mailbox_.reset();
    }
    if (!config_.kill_domain_verifier) {
        try {
            kill_domain_verifier_ = std::make_shared<RejectingKillDomainVerifier>();
        } catch (...) {
            kill_domain_verifier_.reset();
        }
    } else {
        kill_domain_verifier_ = config_.kill_domain_verifier;
    }
}

bool SidecarLifecycle::valid_config(const SidecarLifecycleConfig& config) noexcept {
    return config.control_generation != 0 && absolute_path(config.private_root) &&
           bounded_timeout(config.launch_timeout) &&
           bounded_timeout(config.exec_timeout) && bounded_timeout(config.ready_timeout) &&
           bounded_timeout(config.grace_timeout) && bounded_timeout(config.kill_timeout) &&
           config.max_attempts != 0 && config.max_attempts <= 100000 &&
           config.identities != nullptr;
}

LifecycleActionResult SidecarLifecycle::result(LifecycleAction action) const noexcept {
    LifecycleActionResult value;
    value.action = action;
    value.state = state_;
    try {
        if (identity_.has_value()) {
            value.identity = *identity_;
            value.path = identity_->private_directory;
        }
    } catch (...) {
        // A result is a diagnostic/action value, not an allocation authority.
        // If copying its optional pathname fails, return the action with an
        // empty identity; the outer launcher then rejects it closed instead
        // of terminating from this noexcept reducer boundary.
        value.identity = LifecycleIdentity{};
        value.path.clear();
    }
    value.pid = child_pid_;
    value.pgid = process_group_;
    return value;
}

bool SidecarLifecycle::allocate_identity() noexcept {
    if (!config_.identities || attempts_ >= config_.max_attempts)
        return false;
    const std::optional<LaunchIncarnation> allocated = config_.identities->allocate();
    if (!allocated.has_value() || allocated->identity.generation != config_.control_generation ||
        !allocated->valid())
        return false;
    try {
        LifecycleIdentity identity;
        identity.control = allocated->identity;
        identity.store_generation = allocated->store_generation;
        identity.store_root = allocated->store_root;
        identity.c_store_guid = allocated->c_store_guid;
        identity.f_store_guid = allocated->f_store_guid;
        identity.private_directory = identity_path(config_.private_root,
                                                   identity.store_root,
                                                   identity.control.attempt);
        if (!identity.valid())
            return false;
        identity_ = std::move(identity);
        ++attempts_;
        return true;
    } catch (...) {
        // Identity allocation is a minting boundary.  A failed pathname
        // materialisation must not consume a partially visible identity or
        // leave the reducer in a state where a caller can retry it in-place.
        identity_.reset();
        return false;
    }
}

void SidecarLifecycle::withdraw() noexcept {
    // Clearing this before TERM is the advertisement/C eligibility fence.
    current_ready_lease_.reset();
}

void SidecarLifecycle::enter_termination(
    std::chrono::steady_clock::time_point now) noexcept {
    if (teardown_started_)
        return;
    withdraw();
    teardown_started_ = true;
    term_sent_ = false;
    kill_sent_ = false;
    state_ = LifecycleState::TerminatingGrace;
    deadline_ = now + config_.grace_timeout;
    teardown_deadline_ = now + config_.grace_timeout + config_.kill_timeout;
}

void SidecarLifecycle::clear_incarnation() noexcept {
    identity_.reset();
    child_pid_ = -1;
    process_group_ = -1;
    leader_reaped_ = false;
    leader_waitable_ = false;
    echild_observed_ = false;
    exec_succeeded_ = false;
    term_sent_ = false;
    kill_sent_ = false;
    identity_lost_ = false;
    teardown_started_ = false;
    legacy_requested_ = false;
    group_proof_required_ = false;
    group_domain_.reset();
    ready_buffer_.clear();
    teardown_deadline_ = {};
    // A consumed registration belongs only to the incarnation being retired.
    // Clear both exact-handle fields before RetryEligible/DegradedLegacy so a
    // late pidfd event cannot be accepted while the allocator is preparing a
    // successor.
    pidfd_ = -1;
    registry_generation_ = 0;
}

LifecycleActionResult SidecarLifecycle::begin(
    std::chrono::steady_clock::time_point now) noexcept {
    if (state_ != LifecycleState::Stopped && state_ != LifecycleState::RetryEligible)
        return result(LifecycleAction::None);
    if (!valid_config(config_) || !allocate_identity()) {
        state_ = LifecycleState::DegradedLegacy;
        deadline_ = {};
        return result(LifecycleAction::EnterDegradedLegacy);
    }
    child_pid_ = -1;
    process_group_ = -1;
    leader_reaped_ = false;
    leader_waitable_ = false;
    echild_observed_ = false;
    exec_succeeded_ = false;
    term_sent_ = false;
    kill_sent_ = false;
    identity_lost_ = false;
    teardown_started_ = false;
    legacy_requested_ = false;
    group_proof_required_ = false;
    group_domain_.reset();
    ready_buffer_.clear();
    teardown_deadline_ = {};
    if (++owner_generation_ == 0)
        ++owner_generation_;
    state_ = LifecycleState::LaunchPrepared;
    deadline_ = now + config_.launch_timeout;
    return result(LifecycleAction::LaunchPrepared);
}

bool SidecarLifecycle::accept_ready(const LifecycleObservation& observation) noexcept {
    if (!identity_.has_value() || child_pid_ <= 1 ||
        leader_waitable_ || leader_reaped_ || identity_lost_ || teardown_started_ ||
        observation.child_waitable || observation.identity_lost ||
        observation.request_replacement || observation.request_legacy ||
        observation.ready != ReadyObservation::Complete ||
        !observation.ready_lease.has_value())
        return false;
    const ReadyLease& lease = *observation.ready_lease;
    if (!lease.valid() || lease.pid != child_pid_ ||
        lease.identity != identity_->control ||
        observation.store_generation != identity_->store_generation ||
        lease.store_generation != identity_->store_generation ||
        lease.store_root != identity_->store_root ||
        lease.c_store_guid != identity_->c_store_guid ||
        lease.f_store_guid != identity_->f_store_guid ||
        lease.private_directory != identity_->private_directory ||
        lease.listener_device == 0 || lease.listener_inode == 0)
        return false;
    if ((identity_->listener_device != 0 &&
         identity_->listener_device != lease.listener_device) ||
        (identity_->listener_inode != 0 &&
         identity_->listener_inode != lease.listener_inode))
        return false;
    // Filesystem identity is observed by the daemon outer loop (the former
    // lstat(lease.socket_path...) operation belongs there).  The
    // reducer is deliberately syscall-free: accepting READY requires the
    // same-turn observation to name the expected socket node exactly, while
    // the adapter owns the lstat() that produced those facts.
    if (observation.path_absent ||
        observation.observed_device != lease.listener_device ||
        observation.observed_inode != lease.listener_inode)
        return false;
    // Capture the exact old node before publication.  Teardown must later
    // prove that this node, not a replacement at the same pathname, vanished.
    identity_->listener_device = lease.listener_device;
    identity_->listener_inode = lease.listener_inode;
    return true;
}

bool SidecarLifecycle::exact_group_absent(
    const LifecycleObservation& observation) const noexcept {
    if (!group_proof_required_)
        return false;
    // A numeric PGID/Gone observation is deliberately insufficient.  The
    // outer loop must bind the observation to an independently owned,
    // non-reusable kill-domain lease captured for this exact fork.
    // Do not let a permissive adapter turn caller-controlled facts into a
    // teardown authority.  These checks are deliberately independent of the
    // verifier callback: the observation must be the exact ESRCH/Gone proof,
    // for this fork's PGID, carrying the exact opaque lease captured for it.
    if (!leader_reaped_ || observation.group != GroupObservation::Gone ||
        observation.observed_pgid != process_group_ || !group_domain_.has_value() ||
        !group_domain_->valid() || !group_domain_->matches(child_pid_, process_group_) ||
        !observation.group_domain.valid() ||
        !(observation.group_domain == *group_domain_) || !kill_domain_verifier_)
        return false;
    return kill_domain_verifier_->proves_absent(*group_domain_, child_pid_,
                                                process_group_, observation);
}

bool SidecarLifecycle::exact_path_absent(
    const LifecycleObservation& observation) const noexcept {
    if (!identity_.has_value() || !observation.path_absent)
        return false;
    if (!observation.observed_path.empty() &&
        observation.observed_path != identity_->private_directory)
        return false;
    if (identity_->listener_device == 0 || identity_->listener_inode == 0)
        return observation.observed_device == 0 && observation.observed_inode == 0;
    // The outer loop owns the syscall which observed this pathname.  Never
    // re-lstat from the reducer: only the exact path/device/inode tuple
    // supplied by that observation can satisfy this pure transition.
    return observation.observed_device == identity_->listener_device &&
           observation.observed_inode == identity_->listener_inode;
}

bool SidecarLifecycle::consume_reap(const ReapEvent& event) noexcept {
    const bool carries_identity = event.pidfd >= 0 ||
                                  event.registry_generation != 0 ||
                                  event.incarnation.valid();
    if (event.owner != owner_key() || event.kind != ReaperOwnerKind::Sidecar ||
        event.pid <= 1 ||
        event.pid != child_pid_ || leader_waitable_ || leader_reaped_ ||
        // ECHILD means that some other authority consumed (or lost) the
        // waitable child; it is never a lifecycle reap proof.  When the
        // central registry supplied its immutable metadata, bind every field
        // independently so PID reuse or a stale registration cannot poison a
        // replacement incarnation.
        event.echild || !event.exact_identity() || !carries_identity ||
        !identity_.has_value() || pidfd_ < 0 || registry_generation_ == 0 ||
        event.pidfd != pidfd_ ||
        event.registry_generation != registry_generation_ ||
        event.incarnation.identity != identity_->control ||
        event.incarnation.store_generation != identity_->store_generation ||
        event.incarnation.store_root != identity_->store_root ||
        event.incarnation.c_store_guid != identity_->c_store_guid ||
        event.incarnation.f_store_guid != identity_->f_store_guid)
        return false;
    leader_waitable_ = true;
    leader_reaped_ = true;
    echild_observed_ = event.echild;
    return true;
}

void SidecarLifecycle::consume_reap_events() noexcept {
    if (!reap_mailbox_)
        return;
    // A Compat invariant violation is a single terminal fact about this
    // exact owner/incarnation (the registry removes the owner the moment it
    // publishes one -- see
    // CentralChildReaperRegistry::publish_compat_invariant_violation()), so
    // it is drained and cross-checked ahead of the bounded ReapEvent quota
    // below rather than folded into that quota. Detection only latches
    // compat_invariant_violation_ here; it deliberately does NOT call
    // fail_closed() itself. This function returns void, and fail_closed()'s
    // LifecycleActionResult (LifecycleAction::FailedClosed) must reach the
    // adapter so it performs real cleanup (disable_relationship(), closing
    // owned fds, resetting auth state) -- discarding that return value here
    // would silently skip all of it the moment advance() flips state_ to
    // FailedClosed and its very next statement short-circuits on that state.
    // advance() issues the actual fail-closing return once this latch is
    // observed. A violation that does not match this exact owner generation,
    // pid, and incarnation is stale (e.g. from an owner this object already
    // moved on from) and is discarded exactly like consume_reap() discards a
    // mismatched ReapEvent.
    if (!compat_invariant_violation_.has_value()) {
        CompatInvariantViolation violation;
        if (reap_mailbox_->dequeue_violation(violation)) {
            if (violation.owner == owner_key() &&
                violation.kind == ReaperOwnerKind::Sidecar &&
                violation.pid > 1 && violation.pid == child_pid_ &&
                identity_.has_value() && registry_generation_ != 0 &&
                violation.registry_generation == registry_generation_ &&
                violation.incarnation.identity == identity_->control &&
                violation.incarnation.store_generation ==
                    identity_->store_generation &&
                violation.incarnation.store_root == identity_->store_root &&
                violation.incarnation.c_store_guid ==
                    identity_->c_store_guid &&
                violation.incarnation.f_store_guid ==
                    identity_->f_store_guid) {
                compat_invariant_violation_ = violation;
            } else {
                // Stale/foreign fact: it must not be silently dropped, even
                // though it cannot be latched onto this owner. Log both the
                // expected identity this owner holds and the actual identity
                // the violation carried, so a mismatch is still visible for
                // diagnosis.
                log_warning()
                    << "P50 Compat invariant violation dropped: owner "
                       "mismatch (expected pid="
                    << child_pid_ << " registry_generation="
                    << registry_generation_ << " owner_id="
                    << owner_key().owner_id << " owner_generation="
                    << owner_key().generation
                    << "; got pid=" << violation.pid
                    << " registry_generation=" << violation.registry_generation
                    << " owner_id=" << violation.owner.owner_id
                    << " owner_generation=" << violation.owner.generation
                    << " kind=" << static_cast<int>(violation.kind) << ")"
                    << std::endl;
            }
        }
    }
    // A central reaper gets a fixed delivery quota per owner turn.  Stale
    // events are discarded by owner generation and cannot poison a new PID.
    // One bounded pure in-memory mailbox advance per daemon turn.  A larger
    // drain would let a single turn consume a hidden transition sequence and
    // violate the outer-loop fairness/action quota.
    constexpr size_t kReapEventsPerTurn = 1;
    ReapEvent event;
    for (size_t count = 0; count != kReapEventsPerTurn &&
                             reap_mailbox_->dequeue(event); ++count)
        (void)consume_reap(event);
}

bool SidecarLifecycle::teardown_expired(
    std::chrono::steady_clock::time_point now) const noexcept {
    return teardown_started_ && now >= teardown_deadline_;
}

LifecycleActionResult SidecarLifecycle::fail_closed() noexcept {
    withdraw();
    state_ = LifecycleState::FailedClosed;
    deadline_ = {};
    // Keep the exact identity/PID for diagnostics, but never expose a lease
    // or permit this object to reopen capacity after bounded teardown fails.
    return result(LifecycleAction::FailedClosed);
}

LifecycleActionResult SidecarLifecycle::advance(
    std::chrono::steady_clock::time_point now,
    const LifecycleObservation& observation) noexcept {
    // The outer launcher may have emitted the exact child event in the poll
    // turn immediately after fork, before this reducer has consumed its
    // pending identity report.  Bind that report before dequeuing the
    // central mailbox; otherwise consume_reap() would (correctly) reject an
    // event whose child_pid_ was still unset and the one-shot status would be
    // lost.  This is still one bounded pure state advance; no wait/reap occurs
    // in this reducer.
    if (state_ == LifecycleState::LaunchPrepared && child_pid_ <= 1 &&
        observation.pid > 1 && observation.observed_pgid == observation.pid) {
        child_pid_ = observation.pid;
        process_group_ = observation.observed_pgid;
        group_proof_required_ = true;
        // The opaque kill-domain capability is captured by the outer launcher
        // while it owns the launch action (`kill_domain_verifier_->capture`).
        // A reducer turn may only bind the
        // already-observed value; invoking capture here would hide a pidfd
        // syscall behind a pure state transition and could exceed the daemon's
        // one fallible-action quota.
        if (observation.group_domain.valid())
            group_domain_ = observation.group_domain;
        state_ = LifecycleState::ForkedAwaitExecAndReady;
        deadline_ = now + config_.exec_timeout;
    }
    consume_reap_events();
    // A latched Compat invariant violation is fatal and must fail closed on
    // the very next turn that observes it, exactly once (the state_ guard
    // makes this idempotent across every later turn, since the latch itself
    // is never cleared). This must be the explicit `return fail_closed();`
    // call -- not a flag flip inside consume_reap_events() -- so its
    // LifecycleActionResult reaches the adapter's real cleanup rather than
    // being swallowed by the state_ == FailedClosed early return just below.
    if (compat_invariant_violation_.has_value() &&
        state_ != LifecycleState::FailedClosed)
        return fail_closed();
    if (state_ == LifecycleState::DegradedLegacy ||
        state_ == LifecycleState::FailedClosed || state_ == LifecycleState::Stopped) {
        deadline_ = {};
        return result(LifecycleAction::None);
    }

    if (observation.child_waitable)
        leader_waitable_ = true;
    if (observation.identity_lost)
        identity_lost_ = true;

    if (observation.request_legacy) {
        legacy_requested_ = true;
        // Shutdown can race the launcher's first identity report.  Bind the
        // exact child before deciding that LaunchPrepared has no process;
        // otherwise a same-turn request would enter ReapAndGroupCheck with no
        // PGID lease and could neither terminate nor consume this child.
        if (state_ == LifecycleState::LaunchPrepared && child_pid_ <= 1 &&
            observation.pid > 1 &&
            observation.observed_pgid == observation.pid) {
            child_pid_ = observation.pid;
            process_group_ = observation.observed_pgid;
            group_proof_required_ = true;
            if (observation.group_domain.valid())
                group_domain_ = observation.group_domain;
            state_ = LifecycleState::ForkedAwaitExecAndReady;
            deadline_ = now + config_.exec_timeout;
        }
        if (state_ == LifecycleState::RetryEligible && child_pid_ <= 1) {
            // RetryEligible has already proved the prior child/group/path
            // absent.  Shutdown must not manufacture a fresh terminating
            // interval for a non-existent process; enter the terminal legacy
            // state through the same reducer turn.
            withdraw();
            clear_incarnation();
            state_ = LifecycleState::DegradedLegacy;
            deadline_ = {};
            return result(LifecycleAction::EnterDegradedLegacy);
        }
        if (state_ == LifecycleState::LaunchPrepared && child_pid_ <= 1) {
            withdraw();
            // A launch that never produced a child has no process/group
            // identity to terminate.  Route shutdown through the same
            // reducer, but enter the terminal legacy state so the outer
            // owner can perform any already-captured path cleanup without
            // waiting forever for a group lease that cannot exist.
            state_ = LifecycleState::DegradedLegacy;
            deadline_ = {};
            return result(LifecycleAction::EnterDegradedLegacy);
        }
        if (state_ != LifecycleState::TerminatingGrace &&
            state_ != LifecycleState::TerminatingKill &&
            state_ != LifecycleState::ReapAndGroupCheck) {
            enter_termination(now);
            return result(LifecycleAction::Withdraw);
        }
    }

    if ((leader_waitable_ || identity_lost_) &&
        state_ != LifecycleState::TerminatingGrace &&
        state_ != LifecycleState::TerminatingKill &&
        state_ != LifecycleState::ReapAndGroupCheck &&
        state_ != LifecycleState::LaunchPrepared) {
        enter_termination(now);
        return result(LifecycleAction::Withdraw);
    }

    switch (state_) {
    case LifecycleState::LaunchPrepared: {
        const bool launch_expired = now >= deadline_;
        // Bind the exact fork identity before classifying an exec failure or
        // launch deadline.  The launcher can report the child PID and an
        // exec/setup failure in separate outer turns; returning on the
        // failure first would leave child_pid_ unset and make the reducer
        // unable to order TERM/KILL before path cleanup.
        if (observation.pid > 1 && observation.observed_pgid == observation.pid) {
            // A second identity report must name the same fork.  Refusing a
            // replacement PID here prevents a late/stale scheduler or pidfd
            // observation from rebinding the LaunchPrepared slot.
            if (child_pid_ > 1 &&
                (child_pid_ != observation.pid ||
                 process_group_ != observation.observed_pgid))
                return result(LifecycleAction::None);
            child_pid_ = observation.pid;
            process_group_ = observation.observed_pgid;
            group_proof_required_ = true;
            // Never accept the caller's tuple/bool as authority.  The
            // configured verifier must have captured a private capability for
            // this exact fork during the outer launch action; binding a
            // caller-provided PID without that immutable value fails closed.
            if (observation.group_domain.valid())
                group_domain_ = observation.group_domain;
            state_ = LifecycleState::ForkedAwaitExecAndReady;
            deadline_ = now + config_.exec_timeout;
        }
        if (observation.exec == ExecObservation::Failed || launch_expired) {
            if (child_pid_ > 1) {
                enter_termination(now);
                return result(LifecycleAction::Withdraw);
            }
            if (!exact_path_absent(observation))
                return result(LifecycleAction::None);
            state_ = LifecycleState::RetryEligible;
            deadline_ = {};
            if (attempts_ >= config_.max_attempts) {
                state_ = LifecycleState::DegradedLegacy;
                return result(LifecycleAction::EnterDegradedLegacy);
            }
            return result(LifecycleAction::RetryEligible);
        }
        return result(LifecycleAction::None);
    }

    case LifecycleState::ForkedAwaitExecAndReady:
        if (observation.exec == ExecObservation::Failed ||
            observation.ready == ReadyObservation::Invalid || observation.request_replacement ||
            observation.identity_lost || observation.child_waitable || now >= deadline_) {
            enter_termination(now);
            return result(LifecycleAction::Withdraw);
        }
        if (!exec_succeeded_) {
            if (observation.exec != ExecObservation::Succeeded)
                return result(LifecycleAction::None);
            exec_succeeded_ = true;
            deadline_ = now + config_.ready_timeout;
        }
        if (!observation.ready_bytes.empty()) {
            // Bounded per-turn input; the caller retains any bytes beyond this
            // frame and feeds them on a later turn.
            constexpr size_t kMaximumReadyBytes = 2048;
            if (ready_buffer_.size() > kMaximumReadyBytes -
                                      std::min(observation.ready_bytes.size(),
                                               kMaximumReadyBytes)) {
                enter_termination(now);
                return result(LifecycleAction::Withdraw);
            }
            try {
                ready_buffer_.append(observation.ready_bytes.data(),
                                     std::min(observation.ready_bytes.size(),
                                              kMaximumReadyBytes - ready_buffer_.size()));
            } catch (...) {
                // A malformed/oversized or unmaterialisable READY frame is
                // an incarnation failure.  Preserve the exact child/group
                // identity and route teardown through the ordinary reducer;
                // never let bad_alloc escape this noexcept boundary.
                enter_termination(now);
                return result(LifecycleAction::Withdraw);
            }
        }
        if (accept_ready(observation)) {
            current_ready_lease_ = observation.ready_lease;
            state_ = LifecycleState::Ready;
            // A healthy READY starts a new life: reset the launch-attempt
            // budget so a long-lived F survives occasional replacements
            // without degrading permanently after max_attempts losses.
            attempts_ = 0;
            // Launch/exec/READY deadlines govern only startup.  Leaving the
            // READY deadline armed makes the daemon's outer poll timeout stay
            // at zero after it expires, burning one core for the lifetime of
            // an otherwise idle relationship.
            deadline_ = {};
            return result(LifecycleAction::PublishReady);
        }
        if (observation.ready == ReadyObservation::Complete) {
            enter_termination(now);
            return result(LifecycleAction::Withdraw);
        }
        return result(LifecycleAction::None);

    case LifecycleState::Ready:
        if (observation.request_replacement || leader_reaped_) {
            enter_termination(now);
            return result(LifecycleAction::Withdraw);
        }
        return result(LifecycleAction::None);

    case LifecycleState::TerminatingGrace:
        if (teardown_expired(now))
            return fail_closed();
        if (exact_group_absent(observation)) {
            state_ = LifecycleState::ReapAndGroupCheck;
            return result(LifecycleAction::None);
        }
        if (!term_sent_) {
            term_sent_ = true;
            deadline_ = now + config_.grace_timeout;
            return result(LifecycleAction::SendTerm);
        }
        if (now >= deadline_) {
            state_ = LifecycleState::TerminatingKill;
            kill_sent_ = true;
            deadline_ = std::min(now + config_.kill_timeout, teardown_deadline_);
            return result(LifecycleAction::SendKill);
        }
        return result(LifecycleAction::None);

    case LifecycleState::TerminatingKill:
        if (teardown_expired(now))
            return fail_closed();
        if (exact_group_absent(observation)) {
            state_ = LifecycleState::ReapAndGroupCheck;
            return result(LifecycleAction::None);
        }
        if (!kill_sent_) {
            kill_sent_ = true;
            deadline_ = std::min(now + config_.kill_timeout, teardown_deadline_);
            return result(LifecycleAction::SendKill);
        }
        return result(LifecycleAction::None);

    case LifecycleState::ReapAndGroupCheck:
        if (teardown_expired(now))
            return fail_closed();
        if (exact_group_absent(observation)) {
            // A pre-READY child can exit without running the sidecar's own
            // pathname cleanup.  Once the exact group/reap proof is complete,
            // ask the outer owner to remove the captured node in one separate
            // action; path absence is observed and fed back on a later turn.
            if (!exact_path_absent(observation))
                return result(LifecycleAction::CleanupPath);
            if (legacy_requested_) {
                withdraw();
                state_ = LifecycleState::DegradedLegacy;
                deadline_ = {};
                return result(LifecycleAction::EnterDegradedLegacy);
            }
            clear_incarnation();
            state_ = LifecycleState::RetryEligible;
            deadline_ = {};
            return result(LifecycleAction::RetryEligible);
        }
        return result(LifecycleAction::None);

    case LifecycleState::RetryEligible:
        return result(LifecycleAction::None);
    case LifecycleState::Stopped:
    case LifecycleState::DegradedLegacy:
    case LifecycleState::FailedClosed:
        return result(LifecycleAction::None);
    }
    return result(LifecycleAction::None);
}

bool SidecarLifecycle::observe_child_reaped(pid_t pid, int status,
                                            bool echild) noexcept {
    (void)status;
    (void)echild;
    (void)pid;
    // A numeric PID/status (including an ECHILD enum) is not an immutable
    // kernel-bound event and therefore cannot advance a production lifecycle.
    // Keep this overload only so old standalone callers fail closed at the
    // API boundary; CentralChildReaperRegistry::reap_one() is the sole
    // positive route below.
    return false;
}

bool SidecarLifecycle::observe_child_reaped(const ReapEvent& event) noexcept {
    return consume_reap(event);
}

CentralChildReaperRegistry::CentralChildReaperRegistry() noexcept {
    try {
        state_ = std::make_shared<SharedState>();
    } catch (...) {
        state_.reset();
    }
}

CentralChildReaperRegistry::Registration::~Registration() {
    reset();
}

CentralChildReaperRegistry::Registration::Registration(
    Registration&& other) noexcept
    : state_(std::move(other.state_)), pid_(other.pid_), owner_(other.owner_),
      registry_generation_(other.registry_generation_) {
    other.pid_ = -1;
    other.owner_ = {};
    other.registry_generation_ = 0;
}

CentralChildReaperRegistry::Registration&
CentralChildReaperRegistry::Registration::operator=(Registration&& other) noexcept {
    if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        pid_ = other.pid_;
        owner_ = other.owner_;
        registry_generation_ = other.registry_generation_;
        other.pid_ = -1;
        other.owner_ = {};
        other.registry_generation_ = 0;
    }
    return *this;
}

void CentralChildReaperRegistry::Registration::reset() noexcept {
    if (!state_)
        return;
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto iterator = state_->owners.find(pid_);
    if (iterator != state_->owners.end() && iterator->second.owner == owner_ &&
        iterator->second.registry_generation == registry_generation_) {
        const size_t slot = iterator->second.slot;
        if (slot < state_->slots.size()) {
            state_->slots[slot] = -1;
        }
        state_->owners.erase(iterator);
        if (!state_->slots.empty())
            state_->cursor %= state_->slots.size();
    }
    state_.reset();
    pid_ = -1;
    owner_ = {};
    registry_generation_ = 0;
}

CentralChildReaperRegistry::Registration
CentralChildReaperRegistry::register_owner(
    pid_t pid, pid_t pgid, ReaperOwnerKey owner,
    std::weak_ptr<ReapMailbox> mailbox, ChildHandleBackend backend,
    dev_t listener_device, ino_t listener_inode, int pidfd,
    LaunchIncarnation incarnation, ReaperOwnerKind kind,
    uint64_t starttime_ticks) noexcept {
    const bool handle_valid = (backend == ChildHandleBackend::Native)
                                   ? (pidfd >= 0)
                                   : (pidfd < 0 && starttime_ticks != 0);
    // Direct-worker registrations are scoped to the Native handle primitive
    // only.  The compat (pidfd-less, starttime-fenced) identity is exercised
    // and tested solely through the real sidecar-launch path; a Compat
    // direct worker has no covering observe/kill-domain path and must be
    // rejected outright regardless of starttime_ticks, not merely when the
    // starttime fence is unset.
    if (!state_ || pid <= 1 || pgid <= 1 || pid != pgid || !owner.valid() ||
        ((kind == ReaperOwnerKind::Sidecar) && mailbox.expired()) ||
        !handle_valid || !incarnation.valid() ||
        !reaper_owner_kind_valid(kind) ||
        (kind == ReaperOwnerKind::DirectWorker &&
         (starttime_ticks == 0 || backend == ChildHandleBackend::Compat)))
        return {};
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->owners.find(pid) != state_->owners.end() ||
        state_->owners.size() >= SharedState::kMaximumOwners)
        return {};
    size_t slot = 0;
    bool reused = false;
    try {
        const auto hole = std::find(state_->slots.begin(), state_->slots.end(), -1);
        if (hole != state_->slots.end()) {
            slot = static_cast<size_t>(hole - state_->slots.begin());
            reused = true;
            state_->slots[slot] = pid;
        } else {
            if (state_->slots.size() >= SharedState::kMaximumOwners)
                return {};
            slot = state_->slots.size();
            state_->slots.push_back(pid);
        }
        // Do not wrap the registry generation: a wrapped slot could make an
        // old immutable reap event look current after PID/slot reuse.
        if (state_->next_registry_generation == 0 ||
            state_->next_registry_generation == std::numeric_limits<uint64_t>::max()) {
            if (reused)
                state_->slots[slot] = -1;
            else if (slot < state_->slots.size())
                state_->slots.pop_back();
            return {};
        }
        const uint64_t registry_generation = state_->next_registry_generation++;
        state_->owners.emplace(pid, Entry{pgid, owner, std::move(mailbox),
                                          listener_device, listener_inode, pidfd,
                                          backend, std::move(incarnation),
                                          registry_generation,
                                          false, false, 0, 0, false, slot,
                                          kind, starttime_ticks});
    } catch (...) {
        state_->owners.erase(pid);
        if (reused) {
            state_->slots[slot] = -1;
        } else if (slot < state_->slots.size()) {
            state_->slots.pop_back();
        }
        return {};
    }
    return Registration(state_, pid, owner,
                        state_->owners.find(pid)->second.registry_generation);
}

bool CentralChildReaperRegistry::unregister_owner(pid_t pid,
                                                  ReaperOwnerKey owner) noexcept {
    if (!state_ || pid <= 1 || !owner.valid())
        return false;
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto iterator = state_->owners.find(pid);
    if (iterator == state_->owners.end() || iterator->second.owner != owner)
        return false;
    const size_t slot = iterator->second.slot;
    if (slot < state_->slots.size()) {
        state_->slots[slot] = -1;
    }
    state_->owners.erase(iterator);
    if (!state_->slots.empty())
        state_->cursor %= state_->slots.size();
    return true;
}

bool CentralChildReaperRegistry::observe_child_reaped(
    pid_t pid, ReaperOwnerKey owner, int status, bool echild) noexcept {
    (void)pid;
    (void)owner;
    (void)status;
    (void)echild;
    // A caller-supplied status (and especially an ECHILD enum) is not a
    // kernel observation.  Only reap_one(), below, may publish a status after
    // the exact pidfd WNOWAIT/consume sequence.
    return false;
}

std::optional<ReapEvent> CentralChildReaperRegistry::publish_observed(
    pid_t pid, ReaperOwnerKey owner) noexcept {
    if (!state_ || pid <= 1 || !owner.valid())
        return std::nullopt;
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto iterator = state_->owners.find(pid);
    if (iterator == state_->owners.end() || iterator->second.owner != owner ||
        iterator->second.kind != ReaperOwnerKind::Sidecar ||
        iterator->second.observed || !iterator->second.wait_consumed ||
        (iterator->second.backend == ChildHandleBackend::Native
             ? iterator->second.pidfd < 0
             : (iterator->second.pidfd >= 0 ||
                iterator->second.starttime_ticks == 0)) ||
        !iterator->second.incarnation.valid())
        return std::nullopt;
    const auto target = iterator->second.mailbox.lock();
    if (!target) {
        const size_t slot = iterator->second.slot;
        if (slot < state_->slots.size())
            state_->slots[slot] = -1;
        state_->owners.erase(iterator);
        return std::nullopt;
    }
    // Reserve delivery while holding the registry lock.  If the bounded
    // mailbox is full, keep the consumed status in this entry and retry the
    // pure publication step on a later outer turn; no second wait syscall is
    // needed and no status can be consumed twice.
    const Entry& entry = iterator->second;
    ReapEvent event;
    try {
        event = ReapEvent{owner, pid, entry.observed_status, false, entry.pidfd,
                          entry.backend, entry.registry_generation,
                          entry.incarnation, entry.observed_code, entry.kind,
                          entry.starttime_ticks};
        if (!target->enqueue(event))
            return std::nullopt;
    } catch (...) {
        // Do not turn allocator pressure in the immutable event/mailbox into
        // a lost status or a process-wide exception.  The consumed registry
        // entry remains pending and publish_pending() can retry in memory.
        return std::nullopt;
    }
    iterator->second.observed = true;
    return event;
}

void CentralChildReaperRegistry::publish_compat_invariant_violation(
    pid_t pid, ReaperOwnerKey owner, uint64_t observed_starttime_ticks,
    bool observed_readable, int observed_errno) noexcept {
    if (!state_ || pid <= 1 || !owner.valid())
        return;
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto iterator = state_->owners.find(pid);
    if (iterator == state_->owners.end() || iterator->second.owner != owner ||
        iterator->second.backend != ChildHandleBackend::Compat)
        return;
    const Entry& entry = iterator->second;
    CompatInvariantViolation violation{};
    violation.kind = entry.kind;
    violation.pid = pid;
    violation.owner = owner;
    violation.registry_generation = entry.registry_generation;
    violation.incarnation = entry.incarnation;
    violation.expected_starttime_ticks = entry.starttime_ticks;
    violation.observed_starttime_ticks = observed_starttime_ticks;
    violation.observed_readable = observed_readable;
    violation.observed_errno = observed_errno;
    const auto target = iterator->second.mailbox.lock();
    if (target) {
        // Best-effort delivery.  Whether or not it lands, the owner slot
        // below is removed unconditionally: an invariant violation is never
        // retried, so there is no reason to keep the pid slot occupied
        // waiting for a mailbox that might stay full or expired (e.g. an
        // owner on the reserved direct-worker arm, which carries no live
        // mailbox by contract). This gives that arm pid-slot hygiene even
        // with no state machine to deliver a FailClosed signal into.
        (void)target->enqueue_violation(violation);
    }
    const size_t slot = iterator->second.slot;
    if (slot < state_->slots.size())
        state_->slots[slot] = -1;
    state_->owners.erase(iterator);
}

std::optional<ReapEvent> CentralChildReaperRegistry::reap_one(
    pid_t pid, int pidfd) noexcept {
    if (!state_ || pid <= 1)
        return std::nullopt;

    ReaperOwnerKey owner{};
    ChildHandleBackend backend = ChildHandleBackend::Native;
    uint64_t starttime_ticks = 0;
    bool wait_observed = false;
    bool wait_consumed = false;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto iterator = state_->owners.find(pid);
        if (iterator == state_->owners.end() ||
            iterator->second.kind != ReaperOwnerKind::Sidecar ||
            iterator->second.observed ||
            iterator->second.pidfd != pidfd || !iterator->second.incarnation.valid())
            return std::nullopt;
        owner = iterator->second.owner;
        backend = iterator->second.backend;
        starttime_ticks = iterator->second.starttime_ticks;
        wait_observed = iterator->second.wait_observed;
        wait_consumed = iterator->second.wait_consumed;
    }
    if (backend == ChildHandleBackend::Compat && starttime_ticks == 0)
        return std::nullopt;

    if (wait_consumed)
        return publish_observed(pid, owner);

    if (!wait_observed) {
        // First exact observation: WNOWAIT leaves the status anchored to the
        // pidfd (Native) or re-checked starttime (Compat), so a later
        // numeric PID reuse cannot be mistaken for this incarnation.  No
        // event is emitted until the bounded consume step.
        int code = 0;
        int status = 0;
        bool invariant_violation = false;
        uint64_t observed_starttime_ticks = 0;
        bool observed_readable = false;
        int observed_errno = 0;
        const bool observed = (backend == ChildHandleBackend::Native)
                                   ? observe_pidfd_status(pidfd, pid, false,
                                                          code, status)
                                   : observe_compat_status(
                                         pid, starttime_ticks, false, code,
                                         status, invariant_violation,
                                         observed_starttime_ticks,
                                         observed_readable, observed_errno);
        if (!observed) {
            if (backend == ChildHandleBackend::Compat && invariant_violation)
                publish_compat_invariant_violation(pid, owner,
                                                    observed_starttime_ticks,
                                                    observed_readable,
                                                    observed_errno);
            return std::nullopt;
        }
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto iterator = state_->owners.find(pid);
        if (iterator == state_->owners.end() ||
            iterator->second.kind != ReaperOwnerKind::Sidecar ||
            iterator->second.observed ||
            iterator->second.owner != owner || iterator->second.pidfd != pidfd ||
            iterator->second.wait_observed || iterator->second.wait_consumed)
            return std::nullopt;
        iterator->second.wait_observed = true;
        iterator->second.observed_code = code;
        iterator->second.observed_status = status;
        return std::nullopt;
    }

    // Second exact operation: consume the status from the same pidfd (Native)
    // or the same re-checked starttime (Compat).  EINTR, ECHILD, an unready
    // status, and every identity mismatch remain failures; none is converted
    // into a lifecycle reap event.
    int code = 0;
    int status = 0;
    bool invariant_violation = false;
    uint64_t observed_starttime_ticks = 0;
    bool observed_readable = false;
    int observed_errno = 0;
    const bool observed = (backend == ChildHandleBackend::Native)
                               ? observe_pidfd_status(pidfd, pid, true, code,
                                                      status)
                               : observe_compat_status(
                                     pid, starttime_ticks, true, code, status,
                                     invariant_violation,
                                     observed_starttime_ticks,
                                     observed_readable, observed_errno);
    if (!observed) {
        if (backend == ChildHandleBackend::Compat && invariant_violation)
            publish_compat_invariant_violation(pid, owner,
                                               observed_starttime_ticks,
                                               observed_readable,
                                               observed_errno);
        return std::nullopt;
    }
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto iterator = state_->owners.find(pid);
        if (iterator == state_->owners.end() ||
            iterator->second.kind != ReaperOwnerKind::Sidecar ||
            iterator->second.observed ||
            iterator->second.owner != owner || iterator->second.pidfd != pidfd ||
            !iterator->second.wait_observed || iterator->second.wait_consumed ||
            iterator->second.observed_code != code ||
            iterator->second.observed_status != status)
            return std::nullopt;
        iterator->second.wait_consumed = true;
    }
    return publish_observed(pid, owner);
}


std::optional<ReapEvent> CentralChildReaperRegistry::reap_one_unobserved() noexcept {
    const std::optional<pid_t> pid = next_unobserved_pid();
    if (!pid.has_value() || !state_)
        return std::nullopt;
    int pidfd = -1;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto iterator = state_->owners.find(*pid);
        if (iterator == state_->owners.end() ||
            iterator->second.kind != ReaperOwnerKind::Sidecar ||
            iterator->second.observed)
            return std::nullopt;
        pidfd = iterator->second.pidfd;
    }
    return reap_one(*pid, pidfd);
}

std::optional<ReapEvent> CentralChildReaperRegistry::publish_pending() noexcept {
    if (!state_)
        return std::nullopt;

    pid_t pid = -1;
    ReaperOwnerKey owner{};
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->slots.empty())
            return std::nullopt;
        constexpr size_t kScanQuota = 32;
        const size_t scans = std::min(kScanQuota, state_->slots.size());
        for (size_t offset = 0; offset != scans; ++offset) {
            const size_t index = (state_->cursor + offset) % state_->slots.size();
            const pid_t candidate = state_->slots[index];
            const auto iterator = state_->owners.find(candidate);
            if (candidate > 1 && iterator != state_->owners.end() &&
                iterator->second.kind == ReaperOwnerKind::Sidecar &&
                iterator->second.wait_consumed && !iterator->second.observed &&
                (iterator->second.backend == ChildHandleBackend::Native
                     ? iterator->second.pidfd >= 0
                     : iterator->second.starttime_ticks != 0) &&
                iterator->second.incarnation.valid()) {
                pid = candidate;
                owner = iterator->second.owner;
                state_->cursor = (index + 1) % state_->slots.size();
                break;
            }
        }
    }
    if (pid <= 1 || !owner.valid())
        return std::nullopt;
    // publish_observed() performs only the bounded mailbox enqueue and exact
    // owner revalidation.  It intentionally does not call waitid(): this
    // path is a retry after the status was already consumed.
    return publish_observed(pid, owner);
}

bool CentralChildReaperRegistry::observe_child_reaped(pid_t pid, int status,
                                                       bool echild) noexcept {
    (void)pid;
    (void)status;
    (void)echild;
    // No owner can be selected from a bare PID, and a caller-provided status
    // is not a kernel-bound observation.  Route through reap_one(pid,pidfd).
    return false;
}

std::optional<std::pair<dev_t, ino_t>>
CentralChildReaperRegistry::listener_node(pid_t pid,
                                          ReaperOwnerKey owner) const noexcept {
    if (!state_ || pid <= 1 || !owner.valid())
        return std::nullopt;
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto iterator = state_->owners.find(pid);
    if (iterator == state_->owners.end() || iterator->second.owner != owner)
        return std::nullopt;
    return std::pair<dev_t, ino_t>{iterator->second.listener_device,
                                   iterator->second.listener_inode};
}

std::optional<pid_t> CentralChildReaperRegistry::next_unobserved_pid() noexcept {
    if (!state_)
        return std::nullopt;
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->slots.empty())
        return std::nullopt;
    constexpr size_t kScanQuota = 32;
    const size_t scans = std::min(kScanQuota, state_->slots.size());
    for (size_t offset = 0; offset != scans; ++offset) {
        const size_t index = (state_->cursor + offset) % state_->slots.size();
        const pid_t pid = state_->slots[index];
        const auto iterator = state_->owners.find(pid);
        if (pid > 1 && iterator != state_->owners.end() &&
            iterator->second.kind == ReaperOwnerKind::Sidecar &&
            !iterator->second.observed &&
            (iterator->second.backend == ChildHandleBackend::Native
                 ? iterator->second.pidfd >= 0
                 : iterator->second.starttime_ticks != 0) &&
            iterator->second.incarnation.valid()) {
            state_->cursor = (index + 1) % state_->slots.size();
            return pid;
        }
    }
    state_->cursor = (state_->cursor + scans) % state_->slots.size();
    return std::nullopt;
}

size_t CentralChildReaperRegistry::size() const noexcept {
    if (!state_)
        return 0;
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->owners.size();
}

bool parse_exec_status(std::string_view bytes, ExecObservation& result) noexcept {
    if (bytes == "EXEC\n") {
        result = ExecObservation::Succeeded;
        return true;
    }
    if (bytes == "EXEC-FAILED\n") {
        result = ExecObservation::Failed;
        return true;
    }
    return false;
}

bool parse_ready_frame(std::string_view bytes, const LifecycleIdentity& expected,
                       pid_t expected_pid, ReadyLease& lease) noexcept {
    if (!expected.valid() || expected_pid <= 1 || bytes.size() < 10 ||
        bytes.back() != '\n' || bytes.find('\0') != std::string_view::npos)
        return false;
    bytes.remove_suffix(1);
    std::array<std::string_view, 13> fields{};
    size_t begin = 0;
    for (size_t index = 0; index != fields.size(); ++index) {
        if (begin >= bytes.size()) return false;
        const size_t end = bytes.find(' ', begin);
        fields[index] = bytes.substr(begin, end == std::string_view::npos
                                             ? bytes.size() - begin : end - begin);
        if (fields[index].empty()) return false;
        if (end == std::string_view::npos) {
            if (index + 1 != fields.size()) return false;
            begin = bytes.size();
            break;
        }
        if (index + 1 == fields.size() || end + 1 >= bytes.size() ||
            bytes[end + 1] == ' ') return false;
        begin = end + 1;
    }
    if (fields[0] != "READY" || fields[1] != "v2" ||
        fields[9].substr(0, 5) != "PATH=" ||
        fields[10].substr(0, 7) != "DIGEST=" ||
        fields[11].substr(0, 4) != "DEV=" ||
        fields[12].substr(0, 4) != "INO=")
        return false;
    const std::array<std::string_view, 11> keys = {
        "generation=", "attempt=", "F_STORE_GENERATION=", "DERIVATION_VERSION=",
        "pid=", "C_STORE_GUID=", "F_STORE_GUID=", "PATH=", "DIGEST=", "DEV=",
        "INO="};
    for (size_t i = 0; i != keys.size(); ++i)
        if (fields[i + 2].substr(0, keys[i].size()) != keys[i]) return false;
    uint64_t generation = 0, attempt = 0, version = 0, pid = 0, dev = 0, ino = 0,
             store_generation = 0;
    if (!parse_uint(fields[2].substr(11), generation) ||
        !parse_uint(fields[3].substr(8), attempt) ||
        !parse_uint(fields[4].substr(keys[2].size()), store_generation) ||
        !parse_uint(fields[5].substr(19), version) ||
        !parse_uint(fields[6].substr(4), pid) ||
        !parse_uint(fields[11].substr(4), dev) ||
        !parse_uint(fields[12].substr(4), ino) ||
        store_generation != expected.store_generation || generation != expected.control.generation ||
        attempt != expected.control.attempt || pid != static_cast<uint64_t>(expected_pid) ||
        version != kStoreIdentityDerivationVersion || dev == 0 || ino == 0)
        return false;
    const std::string socket = std::string(fields[9].substr(5));
    const std::string expected_socket = expected.private_directory + "/cache.sock";
    if (socket != expected_socket ||
        fields[10].substr(7) != icecc::digest128_hex(icecc::digest128(expected_socket)))
        return false;
    std::array<uint8_t, 16> c_bytes{}, f_bytes{};
    if (!parse_hex(fields[7].substr(13), c_bytes) ||
        !parse_hex(fields[8].substr(13), f_bytes))
        return false;
    if (c_bytes != expected.c_store_guid.bytes || f_bytes != expected.f_store_guid.bytes)
        return false;
    lease = {};
    lease.identity = expected.control;
    lease.store_generation = store_generation;
    lease.pid = expected_pid;
    lease.store_root = expected.store_root;
    lease.store_derivation_version = version;
    lease.c_store_guid = expected.c_store_guid;
    lease.f_store_guid = expected.f_store_guid;
    lease.private_directory = expected.private_directory;
    lease.socket_path = expected_socket;
    lease.socket_path_digest = icecc::digest128(expected_socket);
    lease.listener_device = static_cast<dev_t>(dev);
    lease.listener_inode = static_cast<ino_t>(ino);
    lease.directory_device = lease.listener_device;
    lease.directory_inode = lease.listener_inode;
    return lease.valid();
}

} // namespace icecc::p50::sidecar
