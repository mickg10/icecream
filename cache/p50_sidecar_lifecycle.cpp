#include "p50_sidecar_lifecycle.h"

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

void append_identity(Digest128Builder& builder,
                     const local::Identity& identity) {
    builder.append_u64(identity.generation);
    builder.append_u64(identity.attempt);
}

void append_incarnation(Digest128Builder& builder,
                        const LaunchIncarnation& incarnation) {
    append_identity(builder, incarnation.identity);
    builder.append_u64(incarnation.store_generation);
    builder.append(std::span<const uint8_t>(incarnation.store_root.bytes.data(),
                                            incarnation.store_root.bytes.size()));
    builder.append(std::span<const uint8_t>(incarnation.c_store_guid.bytes.data(),
                                            incarnation.c_store_guid.bytes.size()));
    builder.append(std::span<const uint8_t>(incarnation.f_store_guid.bytes.data(),
                                            incarnation.f_store_guid.bytes.size()));
}

void append_control(Digest128Builder& builder,
                    const AttemptControlBinding& control) {
    append_identity(builder, control.identity);
    builder.append_u64(control.control_binding_id);
    builder.append_u64(control.logical_job);
    builder.append_u64(control.assignment_epoch);
    builder.append_u64(control.assignment_nonce);
    builder.append_u64(control.request_id);
    builder.append_u64(control.f_store_generation);
    builder.append(std::span<const uint8_t>(control.store_root.bytes.data(),
                                            control.store_root.bytes.size()));
    builder.append(std::span<const uint8_t>(control.c_store_guid.bytes.data(),
                                            control.c_store_guid.bytes.size()));
    builder.append(std::span<const uint8_t>(control.f_store_guid.bytes.data(),
                                            control.f_store_guid.bytes.size()));
}

void append_leaf(Digest128Builder& builder, const AttemptLeafIdentity& leaf) {
    builder.append_u64(leaf.owner.owner_id);
    builder.append_u64(leaf.owner.generation);
    builder.append_u64(leaf.registry_generation);
    append_incarnation(builder, leaf.incarnation);
    builder.append_u64(static_cast<uint64_t>(leaf.pidfd));
    builder.append_u64(static_cast<uint64_t>(leaf.direct_worker_pid));
    builder.append_u64(leaf.process_starttime_ticks);
    builder.append_u64(static_cast<uint64_t>(leaf.leaf_device));
    builder.append_u64(static_cast<uint64_t>(leaf.leaf_inode));
    builder.append(leaf.leaf_path);
}

void append_stream(Digest128Builder& builder,
                   const OutputStreamObservation& stream) {
    builder.append_u64(stream.raw_bytes);
    builder.append(std::span<const uint8_t>(stream.raw_digest.bytes.data(),
                                            stream.raw_digest.bytes.size()));
    builder.append_u8(static_cast<uint8_t>(stream.terminal));
}

void append_source(Digest128Builder& builder,
                   const SourceTerminalObservation& source) {
    append_identity(builder, source.attempt);
    builder.append_u64(source.source_observation_id);
    builder.append_u8(static_cast<uint8_t>(source.reason));
    builder.append_u64(source.raw_bytes);
    builder.append(std::span<const uint8_t>(source.raw_digest.bytes.data(),
                                            source.raw_digest.bytes.size()));
    builder.append_u64(source.pending_bytes);
    builder.append_u64(source.discarded_bytes);
    builder.append_u8(source.ordinary_client_alive ? 1 : 0);
    builder.append_u8(source.exact_wire_terminal ? 1 : 0);
    builder.append_u8(source.integrity_valid ? 1 : 0);
}

Digest128 grant_binding_digest(
    const AttemptControlBinding& control, const AttemptLeafCensus& census,
    const LocalDrainedObservation& drained,
    const AbsoluteMonotonicDeadline& original_deadline,
    uint64_t grant_sequence) noexcept {
    if (!control.valid() || !census.valid() || !drained.valid() ||
        !original_deadline.valid() ||
        grant_sequence == 0)
        return {};
    try {
        Digest128Builder builder;
        builder.append("P50-OUTPUT-QUIESCENCE-GRANT-V1");
        append_control(builder, control);
        append_leaf(builder, census.identity);
        builder.append_u64(static_cast<uint64_t>(census.sole_direct_worker_pid));
        builder.append_u8(census.no_descendants ? 1 : 0);
        builder.append_u8(census.singleton_cgroup_procs ? 1 : 0);
        append_control(builder, drained.control);
        builder.append_u64(drained.observation_id);
        builder.append_u64(drained.direct_worker.owner.owner_id);
        builder.append_u64(drained.direct_worker.owner.generation);
        builder.append_u64(drained.direct_worker.registry_generation);
        builder.append_u64(static_cast<uint64_t>(drained.direct_worker.pidfd));
        builder.append_u64(static_cast<uint64_t>(drained.direct_worker.pid));
        builder.append_u64(drained.direct_worker.starttime_ticks);
        append_incarnation(builder, drained.direct_worker.incarnation);
        builder.append_u64(static_cast<uint64_t>(drained.real_compiler.pid));
        builder.append_u64(drained.real_compiler.starttime_ticks);
        builder.append_u64(drained.real_compiler.identity_nonce);
        builder.append_u64(drained.wait_status.owner.owner_id);
        builder.append_u64(drained.wait_status.owner.generation);
        builder.append_u64(drained.wait_status.registry_generation);
        builder.append_u64(static_cast<uint64_t>(drained.wait_status.pidfd));
        builder.append_u64(static_cast<uint64_t>(drained.wait_status.pid));
        builder.append_u64(static_cast<uint64_t>(drained.wait_status.status));
        builder.append_u64(static_cast<uint64_t>(drained.wait_status.kernel_code));
        builder.append_u64(drained.wait_status.starttime_ticks);
        append_incarnation(builder, drained.wait_status.incarnation);
        append_stream(builder, drained.stdout_stream);
        append_stream(builder, drained.stderr_stream);
        append_source(builder, drained.source);
        builder.append(std::span<const uint8_t>(drained.observation_digest.bytes.data(),
                                                drained.observation_digest.bytes.size()));
        builder.append_u8(drained.cancellation_requested ? 1 : 0);
        builder.append_u64(static_cast<uint64_t>(original_deadline.expires_at_ns));
        builder.append_u64(original_deadline.clock_domain_id);
        builder.append_u64(original_deadline.time_namespace_id);
        builder.append_u64(grant_sequence);
        return builder.finish();
    } catch (...) {
        return {};
    }
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

} // namespace

DirectWorkerStatusConsumed::DirectWorkerStatusConsumed(
    DirectWorkerStatusConsumed&& other) noexcept
    : event_(std::move(other.event_)), valid_(other.valid_) {
    other.event_ = {};
    other.valid_ = false;
}

DirectWorkerStatusConsumed& DirectWorkerStatusConsumed::operator=(
    DirectWorkerStatusConsumed&& other) noexcept {
    if (this != &other) {
        event_ = std::move(other.event_);
        valid_ = other.valid_;
        other.event_ = {};
        other.valid_ = false;
    }
    return *this;
}

bool AttemptLeafIdentity::valid() const noexcept {
    if (!owner.valid() || registry_generation == 0 || !incarnation.valid() ||
        pidfd < 0 || direct_worker_pid <= 1 || process_starttime_ticks == 0 ||
        leaf_device == 0 ||
        leaf_inode == 0 || leaf_path.empty() || leaf_path.front() != '/')
        return false;
    return absolute_path(leaf_path);
}

bool AttemptLeafIdentity::matches(const ReapEvent& event) const noexcept {
    return valid() && event.exact_identity() && event.kind == ReaperOwnerKind::DirectWorker &&
           event.owner == owner && event.registry_generation == registry_generation &&
           event.pidfd == pidfd && event.pid == direct_worker_pid &&
           event.incarnation.identity == incarnation.identity &&
           event.incarnation.store_generation == incarnation.store_generation &&
           event.incarnation.store_root == incarnation.store_root &&
           event.incarnation.c_store_guid == incarnation.c_store_guid &&
           event.incarnation.f_store_guid == incarnation.f_store_guid &&
           event.starttime_ticks == process_starttime_ticks;
}

bool AttemptLeafIdentity::matches(local::Identity identity) const noexcept {
    return valid() && incarnation.identity == identity;
}

OutputQuiescenceGranted::OutputQuiescenceGranted(
    OutputQuiescenceGranted&& other) noexcept
    : control_(std::move(other.control_)), census_(std::move(other.census_)),
      drained_(std::move(other.drained_)),
      original_deadline_(other.original_deadline_),
      grant_sequence_(other.grant_sequence_),
      binding_digest_(other.binding_digest_) {
    other.control_ = {};
    other.census_ = {};
    other.drained_ = {};
    other.original_deadline_ = {};
    other.grant_sequence_ = 0;
    other.binding_digest_ = {};
}

OutputQuiescenceGranted& OutputQuiescenceGranted::operator=(
    OutputQuiescenceGranted&& other) noexcept {
    if (this != &other) {
        control_ = std::move(other.control_);
        census_ = std::move(other.census_);
        drained_ = std::move(other.drained_);
        original_deadline_ = other.original_deadline_;
        grant_sequence_ = other.grant_sequence_;
        binding_digest_ = other.binding_digest_;
        other.control_ = {};
        other.census_ = {};
        other.drained_ = {};
        other.original_deadline_ = {};
        other.grant_sequence_ = 0;
        other.binding_digest_ = {};
    }
    return *this;
}

bool OutputQuiescenceGranted::valid() const noexcept {
    return grant_sequence_ != 0 && binding_digest_ != Digest128{} &&
           control_.valid() && census_.valid() && drained_.preparation_eligible() &&
           control_.matches(census_.identity.incarnation) &&
           original_deadline_.valid() &&
           grant_binding_digest(control_, census_, drained_, original_deadline_,
                                grant_sequence_) == binding_digest_;
}

bool OutputQuiescenceGranted::matches_binding(
    const AttemptControlBinding& control, const AttemptLeafCensus& census,
    const LocalDrainedObservation& drained,
    const AbsoluteMonotonicDeadline& original_deadline) const noexcept {
    return valid() && control_ == control && census_ == census &&
           drained_ == drained && original_deadline_ == original_deadline &&
           grant_binding_digest(control, census, drained, original_deadline,
                                grant_sequence_) == binding_digest_;
}

AttemptLeafRetirementJoin::AttemptLeafRetirementJoin(
    OutputQuiescenceGranted quiescence,
    DirectWorkerStatusConsumed direct_worker_status,
    bool empty_after_population, bool leaf_cleanup_complete) noexcept
    : quiescence_(std::move(quiescence)),
      direct_worker_status_(std::move(direct_worker_status)),
      empty_after_population_(empty_after_population),
      leaf_cleanup_complete_(leaf_cleanup_complete) {}

AttemptLeafRetirementJoin::AttemptLeafRetirementJoin(
    AttemptLeafRetirementJoin&& other) noexcept
    : quiescence_(std::move(other.quiescence_)),
      direct_worker_status_(std::move(other.direct_worker_status_)),
      empty_after_population_(other.empty_after_population_),
      leaf_cleanup_complete_(other.leaf_cleanup_complete_) {
    other.empty_after_population_ = false;
    other.leaf_cleanup_complete_ = false;
}

AttemptLeafRetirementJoin& AttemptLeafRetirementJoin::operator=(
    AttemptLeafRetirementJoin&& other) noexcept {
    if (this != &other) {
        quiescence_ = std::move(other.quiescence_);
        direct_worker_status_ = std::move(other.direct_worker_status_);
        empty_after_population_ = other.empty_after_population_;
        leaf_cleanup_complete_ = other.leaf_cleanup_complete_;
        other.empty_after_population_ = false;
        other.leaf_cleanup_complete_ = false;
    }
    return *this;
}

bool AttemptLeafRetirementJoin::valid() const noexcept {
    return quiescence_.valid() && direct_worker_status_.valid() &&
           empty_after_population_ && leaf_cleanup_complete_ &&
           identity().matches(direct_worker_status_.event()) &&
           direct_worker_status_.status_identity() ==
               quiescence_.drained_observation().wait_status &&
           quiescence_.drained_observation().direct_worker.owner ==
               direct_worker_status_.owner() &&
           quiescence_.drained_observation().direct_worker.registry_generation ==
               direct_worker_status_.registry_generation() &&
           quiescence_.drained_observation().direct_worker.pidfd ==
               direct_worker_status_.pidfd() &&
           quiescence_.drained_observation().direct_worker.pid ==
               direct_worker_status_.pid() &&
           quiescence_.drained_observation().direct_worker.starttime_ticks ==
               direct_worker_status_.starttime_ticks();
}

bool AttemptLeafRetirementJoin::matches(local::Identity expected) const noexcept {
    return valid() && this->identity().matches(expected);
}

OutputQuiescenceGranted AttemptLeafAuthority::issue_output_quiescence(
    AttemptControlBinding control, AttemptLeafCensus census,
    LocalDrainedObservation drained,
    AbsoluteMonotonicDeadline original_deadline,
    uint64_t grant_sequence) noexcept {
    const Digest128 binding_digest = grant_binding_digest(
        control, census, drained, original_deadline, grant_sequence);
    if (binding_digest == Digest128{})
        return {};
    return OutputQuiescenceGranted(std::move(control), std::move(census),
                                   std::move(drained), original_deadline,
                                   grant_sequence, binding_digest);
}

AttemptLeafRetirementJoin AttemptLeafAuthority::issue_retirement_join(
    OutputQuiescenceGranted quiescence,
    DirectWorkerStatusConsumed direct_worker_status,
    bool empty_after_population, bool leaf_cleanup_complete,
    std::chrono::steady_clock::time_point now) noexcept {
    if (!quiescence.valid() || !direct_worker_status.valid() ||
        !empty_after_population || !leaf_cleanup_complete ||
        !quiescence.preparation_eligible(now) ||
        !quiescence.census().identity.matches(direct_worker_status.event()) ||
        direct_worker_status.status_identity() !=
            quiescence.drained_observation().wait_status)
        return {};
    return AttemptLeafRetirementJoin(std::move(quiescence),
                                     std::move(direct_worker_status),
                                     empty_after_population,
                                     leaf_cleanup_complete);
}

std::optional<OutputQuiescenceGranted>
AttemptLeafAuthority::grant_output_quiescence(
    const AttemptControlBinding&, const AttemptLeafCensus&,
    const LocalDrainedObservation&,
    const AbsoluteMonotonicDeadline&) noexcept {
    return std::nullopt;
}

std::optional<AttemptLeafRetirementJoin>
AttemptLeafAuthority::join_retirement(
    OutputQuiescenceGranted, DirectWorkerStatusConsumed, bool,
    bool, std::chrono::steady_clock::time_point) noexcept {
    return std::nullopt;
}

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
    attempt_leaf_authority_ = config_.attempt_leaf_authority;
}

bool SidecarLifecycle::valid_config(const SidecarLifecycleConfig& config) noexcept {
    return config.control_generation != 0 && absolute_path(config.private_root) &&
           bounded_timeout(config.launch_timeout) &&
           bounded_timeout(config.exec_timeout) && bounded_timeout(config.ready_timeout) &&
           bounded_timeout(config.grace_timeout) && bounded_timeout(config.kill_timeout) &&
           config.max_attempts != 0 && config.max_attempts <= 100000 &&
           config.identities != nullptr;
}

std::optional<OutputQuiescenceGranted>
SidecarLifecycle::grant_output_quiescence(
    const AttemptControlBinding& control,
    const AttemptLeafCensus& census,
    const LocalDrainedObservation& drained,
    const AbsoluteMonotonicDeadline& original_deadline) const noexcept {
    if (!attempt_leaf_authority_ || !control.valid() || !census.valid() ||
        !drained.preparation_eligible() ||
        !control.matches(census.identity.incarnation) ||
        drained.control != control ||
        !control.matches(drained.direct_worker.incarnation) ||
        !control.matches(drained.wait_status.incarnation) ||
        census.identity.owner != drained.direct_worker.owner ||
        census.identity.registry_generation !=
            drained.direct_worker.registry_generation ||
        census.identity.pidfd != drained.direct_worker.pidfd ||
        census.identity.direct_worker_pid != drained.direct_worker.pid ||
        census.identity.process_starttime_ticks !=
            drained.direct_worker.starttime_ticks ||
        !original_deadline.valid())
        return std::nullopt;
    try {
        auto granted = attempt_leaf_authority_->grant_output_quiescence(
            control, census, drained, original_deadline);
        if (!granted.has_value() ||
            !granted->matches_binding(control, census, drained,
                                      original_deadline))
            return std::nullopt;
        return granted;
    } catch (...) {
        // The positive token is an authority boundary.  An exception or
        // allocation failure is never converted into OUTPUT_QUIESCENCE_GRANTED.
        return std::nullopt;
    }
}

std::optional<AttemptLeafRetirementJoin>
SidecarLifecycle::join_attempt_leaf_retirement(
    OutputQuiescenceGranted quiescence,
    DirectWorkerStatusConsumed direct_worker_status,
    bool empty_after_population, bool leaf_cleanup_complete,
    std::chrono::steady_clock::time_point now) const noexcept {
    if (!attempt_leaf_authority_ || !quiescence.valid() ||
        !direct_worker_status.valid() || !empty_after_population ||
        !leaf_cleanup_complete || !quiescence.preparation_eligible(now) ||
        direct_worker_status.status_identity() !=
            quiescence.drained_observation().wait_status ||
        !quiescence.census().identity.matches(direct_worker_status.event()))
        return std::nullopt;
    const AttemptControlBinding expected_control = quiescence.control_binding();
    const Digest128 expected_binding_digest = quiescence.binding_digest();
    const uint64_t expected_sequence = quiescence.grant_sequence();
    try {
        auto joined = attempt_leaf_authority_->join_retirement(
            std::move(quiescence), std::move(direct_worker_status),
            empty_after_population, leaf_cleanup_complete, now);
        if (!joined.has_value() || !joined->valid() ||
            joined->control_binding() != expected_control ||
            joined->quiescence().binding_digest() != expected_binding_digest ||
            joined->quiescence().grant_sequence() != expected_sequence ||
            !joined->original_deadline().valid() ||
            joined->original_deadline().expired(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    now.time_since_epoch()).count(),
                joined->original_deadline().clock_domain_id,
                joined->original_deadline().time_namespace_id))
            return std::nullopt;
        return joined;
    } catch (...) {
        // A consumed status cannot be safely replayed after an authority
        // failure.  Leave the compiler lane fail-closed with no join token.
        return std::nullopt;
    }
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
    std::weak_ptr<ReapMailbox> mailbox, dev_t listener_device,
    ino_t listener_inode, int pidfd, LaunchIncarnation incarnation,
    ReaperOwnerKind kind, uint64_t starttime_ticks) noexcept {
    if (!state_ || pid <= 1 || pgid <= 1 || pid != pgid || !owner.valid() ||
        ((kind == ReaperOwnerKind::Sidecar) && mailbox.expired()) ||
        pidfd < 0 || !incarnation.valid() ||
        !reaper_owner_kind_valid(kind) ||
        (kind == ReaperOwnerKind::DirectWorker && starttime_ticks == 0))
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
                                          std::move(incarnation), registry_generation,
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

CentralChildReaperRegistry::Registration
CentralChildReaperRegistry::register_direct_worker(
    pid_t pid, pid_t pgid, ReaperOwnerKey owner, int pidfd,
    LaunchIncarnation incarnation, uint64_t starttime_ticks) noexcept {
    return register_owner(pid, pgid, owner, {}, 0, 0, pidfd,
                           std::move(incarnation), ReaperOwnerKind::DirectWorker,
                           starttime_ticks);
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
        iterator->second.pidfd < 0 || !iterator->second.incarnation.valid())
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
                          entry.registry_generation, entry.incarnation,
                          entry.observed_code};
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

std::optional<ReapEvent> CentralChildReaperRegistry::reap_one(
    pid_t pid, int pidfd) noexcept {
    if (!state_ || pid <= 1 || pidfd < 0)
        return std::nullopt;

    ReaperOwnerKey owner{};
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
        wait_observed = iterator->second.wait_observed;
        wait_consumed = iterator->second.wait_consumed;
    }

    if (wait_consumed)
        return publish_observed(pid, owner);

    if (!wait_observed) {
        // First exact observation: WNOWAIT leaves the status anchored to the
        // pidfd, so a later numeric PID reuse cannot be mistaken for this
        // incarnation.  No event is emitted until the bounded consume step.
        int code = 0;
        int status = 0;
        if (!observe_pidfd_status(pidfd, pid, false, code, status))
            return std::nullopt;
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

    // Second exact operation: consume the status from the same pidfd.  EINTR,
    // ECHILD, an unready status, and every identity mismatch remain failures;
    // none is converted into a lifecycle reap event.
    int code = 0;
    int status = 0;
    if (!observe_pidfd_status(pidfd, pid, true, code, status))
        return std::nullopt;
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
                iterator->second.pidfd >= 0 && iterator->second.incarnation.valid()) {
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

std::optional<DirectWorkerStatusConsumed>
CentralChildReaperRegistry::reap_one_direct_worker(pid_t pid,
                                                   int pidfd) noexcept {
    // This is deliberately the same exact-pidfd route as sidecar reaping.  A
    // compiler reducer receives only the move-only consumed value and may not
    // call wait* itself or search for an unregistered child.  Do not delegate
    // to reap_one(): that path publishes a copy into the sidecar mailbox, and
    // returning that same status to a compiler reducer would create two
    // consumers for one child.  A direct-worker registration reserves the
    // consumed event for this move-only result instead.
    if (!state_ || pid <= 1 || pidfd < 0)
        return std::nullopt;

    ReaperOwnerKey owner{};
    bool wait_observed = false;
    bool wait_consumed = false;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto iterator = state_->owners.find(pid);
        if (iterator == state_->owners.end() ||
            iterator->second.kind != ReaperOwnerKind::DirectWorker ||
            iterator->second.observed ||
            iterator->second.pidfd != pidfd ||
            !iterator->second.incarnation.valid())
            return std::nullopt;
        owner = iterator->second.owner;
        wait_observed = iterator->second.wait_observed;
        wait_consumed = iterator->second.wait_consumed;
    }
    if (wait_consumed)
        return std::nullopt;

    int observed_code = 0;
    int observed_status = 0;
    if (!wait_observed) {
        if (!observe_pidfd_status(pidfd, pid, false, observed_code,
                                  observed_status))
            return std::nullopt;
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto iterator = state_->owners.find(pid);
        if (iterator == state_->owners.end() ||
            iterator->second.kind != ReaperOwnerKind::DirectWorker ||
            iterator->second.observed ||
            iterator->second.owner != owner || iterator->second.pidfd != pidfd ||
            iterator->second.wait_observed || iterator->second.wait_consumed)
            return std::nullopt;
        iterator->second.wait_observed = true;
        iterator->second.observed_code = observed_code;
        iterator->second.observed_status = observed_status;
        return std::nullopt;
    }

    // The WNOWAIT observation happened on the preceding outer turn.  Re-read
    // only the immutable classification captured in the registry; do not
    // issue another observation or manufacture a status from caller input.
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto iterator = state_->owners.find(pid);
        if (iterator == state_->owners.end() ||
            iterator->second.kind != ReaperOwnerKind::DirectWorker ||
            iterator->second.observed || iterator->second.owner != owner ||
            iterator->second.pidfd != pidfd || !iterator->second.wait_observed ||
            iterator->second.wait_consumed)
            return std::nullopt;
        observed_code = iterator->second.observed_code;
        observed_status = iterator->second.observed_status;
    }

    // The caller's first direct invocation observed WNOWAIT in a prior turn.
    // Consume that exact status now; no numeric PID or anonymous wait is ever
    // accepted as a substitute.
    if (!observe_pidfd_status(pidfd, pid, true, observed_code,
                              observed_status))
        return std::nullopt;
    ReapEvent event;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto iterator = state_->owners.find(pid);
        if (iterator == state_->owners.end() ||
            iterator->second.kind != ReaperOwnerKind::DirectWorker ||
            iterator->second.observed ||
            iterator->second.owner != owner || iterator->second.pidfd != pidfd ||
            !iterator->second.wait_observed || iterator->second.wait_consumed ||
            iterator->second.observed_code != observed_code ||
            iterator->second.observed_status != observed_status)
            return std::nullopt;
        iterator->second.wait_consumed = true;
        iterator->second.observed = true;
        try {
            event = ReapEvent{owner, pid, observed_status, false,
                              iterator->second.pidfd,
                              iterator->second.registry_generation,
                              iterator->second.incarnation,
                              iterator->second.observed_code,
                              ReaperOwnerKind::DirectWorker,
                              iterator->second.starttime_ticks};
        } catch (...) {
            // The kernel status has already been consumed.  Keep the entry
            // marked consumed/observed rather than manufacturing a second
            // wait; the direct caller receives no token and the owning
            // registry retains the exact identity for its external failure
            // authority.
            return std::nullopt;
        }
    }
    if (!event.exact_identity())
        return std::nullopt;
    return DirectWorkerStatusConsumed(std::move(event));
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
            !iterator->second.observed && iterator->second.pidfd >= 0 &&
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
