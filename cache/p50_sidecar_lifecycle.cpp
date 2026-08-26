#include "p50_sidecar_lifecycle.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <string>
#include <utility>

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

} // namespace

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
    case LifecycleAction::EnterDegradedLegacy: return "EnterDegradedLegacy";
    case LifecycleAction::RetryEligible: return "RetryEligible";
    }
    return "Unknown";
}

SidecarLifecycle::SidecarLifecycle(SidecarLifecycleConfig config) noexcept
    : config_(std::move(config)) {}

bool SidecarLifecycle::valid_config(const SidecarLifecycleConfig& config) noexcept {
    return config.control_generation != 0 && config.store_generation != 0 &&
           absolute_path(config.private_root) && bounded_timeout(config.launch_timeout) &&
           bounded_timeout(config.exec_timeout) && bounded_timeout(config.ready_timeout) &&
           bounded_timeout(config.grace_timeout) && bounded_timeout(config.kill_timeout) &&
           config.max_attempts != 0 && config.max_attempts <= 100000 &&
           config.identities != nullptr;
}

LifecycleActionResult SidecarLifecycle::result(LifecycleAction action) const noexcept {
    LifecycleActionResult value;
    value.action = action;
    value.state = state_;
    if (identity_.has_value()) {
        value.identity = *identity_;
        value.path = identity_->private_directory;
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
    LifecycleIdentity identity;
    identity.control = allocated->identity;
    identity.store_generation = config_.store_generation;
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
}

void SidecarLifecycle::withdraw() noexcept {
    // Clearing this before TERM is the advertisement/C eligibility fence.
    current_ready_lease_.reset();
}

void SidecarLifecycle::enter_termination(
    std::chrono::steady_clock::time_point now) noexcept {
    withdraw();
    term_sent_ = false;
    kill_sent_ = false;
    state_ = LifecycleState::TerminatingGrace;
    deadline_ = now + config_.grace_timeout;
}

void SidecarLifecycle::clear_incarnation() noexcept {
    identity_.reset();
    child_pid_ = -1;
    process_group_ = -1;
    leader_reaped_ = false;
    echild_observed_ = false;
    exec_succeeded_ = false;
    term_sent_ = false;
    kill_sent_ = false;
    ready_buffer_.clear();
}

LifecycleActionResult SidecarLifecycle::begin(
    std::chrono::steady_clock::time_point now) noexcept {
    if (state_ != LifecycleState::Stopped && state_ != LifecycleState::RetryEligible)
        return result(LifecycleAction::None);
    if (!valid_config(config_) || !allocate_identity()) {
        state_ = LifecycleState::DegradedLegacy;
        return result(LifecycleAction::EnterDegradedLegacy);
    }
    child_pid_ = -1;
    process_group_ = -1;
    leader_reaped_ = false;
    echild_observed_ = false;
    exec_succeeded_ = false;
    term_sent_ = false;
    kill_sent_ = false;
    ready_buffer_.clear();
    state_ = LifecycleState::LaunchPrepared;
    deadline_ = now + config_.launch_timeout;
    return result(LifecycleAction::LaunchPrepared);
}

bool SidecarLifecycle::accept_ready(const LifecycleObservation& observation) noexcept {
    if (!identity_.has_value() || child_pid_ <= 1 ||
        observation.ready != ReadyObservation::Complete ||
        !observation.ready_lease.has_value())
        return false;
    const ReadyLease& lease = *observation.ready_lease;
    return lease.valid() && lease.pid == child_pid_ &&
           lease.identity == identity_->control &&
           observation.store_generation == identity_->store_generation &&
           lease.store_root == identity_->store_root &&
           lease.c_store_guid == identity_->c_store_guid &&
           lease.f_store_guid == identity_->f_store_guid &&
           lease.private_directory == identity_->private_directory;
}

bool SidecarLifecycle::exact_group_absent(
    const LifecycleObservation& observation) const noexcept {
    return process_group_ > 1 && observation.observed_pgid == process_group_ &&
           observation.group == GroupObservation::Gone;
}

bool SidecarLifecycle::exact_path_absent(
    const LifecycleObservation& observation) const noexcept {
    if (!identity_.has_value() || !observation.path_absent)
        return false;
    // An empty path is the compact event-loop observation for the already
    // registered path.  When a watcher supplies a path, bind it exactly.
    return observation.observed_path.empty() ||
           observation.observed_path == identity_->private_directory;
}

LifecycleActionResult SidecarLifecycle::advance(
    std::chrono::steady_clock::time_point now,
    const LifecycleObservation& observation) noexcept {
    if (state_ == LifecycleState::DegradedLegacy || state_ == LifecycleState::Stopped)
        return result(LifecycleAction::None);

    if (observation.request_legacy) {
        withdraw();
        state_ = LifecycleState::DegradedLegacy;
        return result(LifecycleAction::EnterDegradedLegacy);
    }

    switch (state_) {
    case LifecycleState::LaunchPrepared:
        if (observation.exec == ExecObservation::Failed ||
            now >= deadline_) {
            if (!exact_path_absent(observation))
                return result(LifecycleAction::None);
            state_ = LifecycleState::RetryEligible;
            if (attempts_ >= config_.max_attempts) {
                state_ = LifecycleState::DegradedLegacy;
                return result(LifecycleAction::EnterDegradedLegacy);
            }
            return result(LifecycleAction::RetryEligible);
        }
        if (observation.pid > 1 && observation.observed_pgid == observation.pid) {
            child_pid_ = observation.pid;
            process_group_ = observation.observed_pgid;
            state_ = LifecycleState::ForkedAwaitExecAndReady;
            deadline_ = now + config_.exec_timeout;
        }
        return result(LifecycleAction::None);

    case LifecycleState::ForkedAwaitExecAndReady:
        if (observation.exec == ExecObservation::Failed ||
            observation.ready == ReadyObservation::Invalid || now >= deadline_) {
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
            ready_buffer_.append(observation.ready_bytes.data(),
                                 std::min(observation.ready_bytes.size(),
                                          kMaximumReadyBytes - ready_buffer_.size()));
        }
        if (accept_ready(observation)) {
            current_ready_lease_ = observation.ready_lease;
            state_ = LifecycleState::Ready;
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
            deadline_ = now + config_.kill_timeout;
            return result(LifecycleAction::SendKill);
        }
        return result(LifecycleAction::None);

    case LifecycleState::TerminatingKill:
        if (exact_group_absent(observation)) {
            state_ = LifecycleState::ReapAndGroupCheck;
            return result(LifecycleAction::None);
        }
        if (!kill_sent_) {
            kill_sent_ = true;
            deadline_ = now + config_.kill_timeout;
            return result(LifecycleAction::SendKill);
        }
        return result(LifecycleAction::None);

    case LifecycleState::ReapAndGroupCheck:
        if (exact_group_absent(observation) && exact_path_absent(observation)) {
            clear_incarnation();
            state_ = LifecycleState::RetryEligible;
            return result(LifecycleAction::RetryEligible);
        }
        return result(LifecycleAction::None);

    case LifecycleState::RetryEligible:
        return result(LifecycleAction::None);
    case LifecycleState::Stopped:
    case LifecycleState::DegradedLegacy:
        return result(LifecycleAction::None);
    }
    return result(LifecycleAction::None);
}

bool SidecarLifecycle::observe_child_reaped(pid_t pid, int status,
                                            bool echild) noexcept {
    (void)status;
    if (pid <= 1 || child_pid_ != pid || leader_reaped_ ||
        (state_ != LifecycleState::ForkedAwaitExecAndReady &&
         state_ != LifecycleState::Ready && state_ != LifecycleState::TerminatingGrace &&
         state_ != LifecycleState::TerminatingKill &&
         state_ != LifecycleState::ReapAndGroupCheck))
        return false;
    leader_reaped_ = true;
    echild_observed_ = echild;
    return true;
}

bool CentralChildReaperRegistry::register_owner(pid_t pid, pid_t pgid,
                                                SidecarLifecycle& owner) noexcept {
    if (pid <= 1 || pgid <= 1 || pid != pgid || owners_.find(pid) != owners_.end())
        return false;
    try {
        owners_.emplace(pid, Entry{pgid, &owner, false});
        order_.push_back(pid);
    } catch (...) {
        owners_.erase(pid);
        return false;
    }
    return true;
}

std::optional<pid_t> CentralChildReaperRegistry::next_unobserved_pid() noexcept {
    if (order_.empty()) return std::nullopt;
    for (size_t offset = 0; offset != order_.size(); ++offset) {
        const size_t index = (cursor_ + offset) % order_.size();
        const pid_t pid = order_[index];
        const auto iterator = owners_.find(pid);
        if (iterator != owners_.end() && !iterator->second.observed) {
            cursor_ = (index + 1) % order_.size();
            return pid;
        }
    }
    return std::nullopt;
}

bool CentralChildReaperRegistry::observe_child_reaped(pid_t pid, int status,
                                                       bool echild) noexcept {
    const auto iterator = owners_.find(pid);
    if (iterator == owners_.end() || iterator->second.observed ||
        iterator->second.owner == nullptr)
        return false;
    iterator->second.observed = true;
    return iterator->second.owner->observe_child_reaped(pid, status, echild);
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
    if (fields[0] != "READY" || fields[1] != "v2" || fields[8].substr(0, 5) != "PATH=" ||
        fields[9].substr(0, 7) != "DIGEST=" || fields[10].substr(0, 4) != "DEV=" ||
        fields[11].substr(0, 4) != "INO=") return false;
    const std::array<std::string_view, 11> keys = {
        "generation=", "attempt=", "DERIVATION_VERSION=", "pid=", "C_STORE_GUID=",
        "F_STORE_GUID=", "PATH=", "DIGEST=", "DEV=", "INO=", "STORE_GENERATION="};
    for (size_t i = 0; i != keys.size(); ++i)
        if (fields[i + 2].substr(0, keys[i].size()) != keys[i]) return false;
    uint64_t generation = 0, attempt = 0, version = 0, pid = 0, dev = 0, ino = 0,
             store_generation = 0;
    if (!parse_uint(fields[2].substr(11), generation) ||
        !parse_uint(fields[3].substr(8), attempt) ||
        !parse_uint(fields[4].substr(19), version) ||
        !parse_uint(fields[5].substr(4), pid) || !parse_uint(fields[10].substr(4), dev) ||
        !parse_uint(fields[11].substr(4), ino) ||
        !parse_uint(fields[12].substr(17), store_generation) ||
        store_generation != expected.store_generation || generation != expected.control.generation ||
        attempt != expected.control.attempt || pid != static_cast<uint64_t>(expected_pid) ||
        version != kStoreIdentityDerivationVersion || dev == 0 || ino == 0)
        return false;
    const std::string socket = std::string(fields[8].substr(5));
    const std::string expected_socket = expected.private_directory + "/cache.sock";
    if (socket != expected_socket ||
        fields[9].substr(7) != icecc::digest128_hex(icecc::digest128(expected_socket))) return false;
    std::array<uint8_t, 16> c_bytes{}, f_bytes{};
    if (!parse_hex(fields[6].substr(13), c_bytes) ||
        !parse_hex(fields[7].substr(13), f_bytes)) return false;
    if (c_bytes != expected.c_store_guid.bytes || f_bytes != expected.f_store_guid.bytes)
        return false;
    lease = {};
    lease.identity = expected.control;
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
