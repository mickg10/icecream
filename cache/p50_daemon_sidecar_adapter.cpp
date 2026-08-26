#include "p50_daemon_sidecar_adapter.h"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace icecc::p50::daemon {
namespace {

constexpr int64_t kMaximumTimeoutMilliseconds = 24 * 60 * 60 * 1000;

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

} // namespace

DaemonSidecarAdapter::DaemonSidecarAdapter(Config config) noexcept
    : config_(std::move(config))
{
    try {
        launch_identities_ = std::make_shared<sidecar::LaunchIdentityAllocator>(
            config_.generation, 1);
    } catch (...) {
        launch_identities_.reset();
    }
}

DaemonSidecarAdapter::~DaemonSidecarAdapter()
{
    shutdown();
}

bool DaemonSidecarAdapter::valid_config(const Config& config) noexcept
{
    sidecar::Config supervisor_config;
    supervisor_config.executable = config.executable;
    supervisor_config.readiness_timeout = config.readiness_timeout;
    supervisor_config.shutdown_timeout = config.shutdown_timeout;
    supervisor_config.restart_window = config.restart_window;
    supervisor_config.max_restarts = 0;
    supervisor_config.max_attempts_per_recovery = 1;
    if (!sidecar::Supervisor::valid_config(supervisor_config) ||
        config.executable.empty() || config.executable.front() != '/' ||
        config.runtime_directory.empty() ||
        config.runtime_directory.front() != '/' || has_nul(config.runtime_directory) ||
        config.runtime_directory.size() >= 180 ||
        config.generation == 0 || config.public_listener_port == 0 ||
        config.public_listener_port > std::numeric_limits<uint16_t>::max() ||
        !valid_id(config.expected_daemon_uid) || !valid_gid(config.expected_daemon_gid) ||
        !valid_id(config.expected_service_uid) || !valid_gid(config.expected_service_gid) ||
        config.expected_daemon_uid != static_cast<uint64_t>(::geteuid()) ||
        config.expected_daemon_gid != static_cast<uint64_t>(::getegid()) ||
        config.expected_service_uid != config.expected_daemon_uid ||
        config.expected_service_gid != config.expected_daemon_gid ||
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
    return dispatcher_ != nullptr && dispatcher_->available();
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
    if (state_ != AdapterState::Ready || supervisor_ == nullptr ||
        supervisor_->state() != sidecar::State::Ready ||
        supervisor_->child_pid() <= 1 || !runtime_nodes_valid())
        return rejected_result;

    const local::Identity identity{config_.generation, attempt_};
    const local::CredentialExpectation expected{
        config_.expected_service_uid, config_.expected_service_gid,
        static_cast<uint64_t>(supervisor_->child_pid())};
    const auto deadline = std::chrono::steady_clock::now() +
                          config_.input_attachment_timeout;
    const InputFdRequest request{identity, key, owner, request_id};
    InputFdAttachmentResult result = InputFdAttachmentClient::attach(
        socket_path_, request, expected, deadline);
    // Even a failed materialization/FD exchange names the exact store attempt
    // against which teardown must later settle or retry the job observation.
    result.lease = request;
    return result;
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
    if (lease.identity.generation != config_.generation ||
        lease.identity.attempt == 0 || lease.key.c_store_guid == CStoreGuid{} ||
        !input_lease_owner_valid(lease.owner) || lease.request_id == 0 ||
        !input_lifecycle_action_valid(action))
        return InputLifecycleResult{InputLifecycleStatus::InvalidArgument, request};

    if (lease.identity.attempt != attempt_) {
        const InputLifecycleStatus status = lease.identity.attempt < attempt_
                                                ? InputLifecycleStatus::StoreReplaced
                                                : InputLifecycleStatus::StaleIdentity;
        return InputLifecycleResult{status, request};
    }

    const auto same_operation = [&](const InputLifecycleRequest& candidate) {
        return candidate.identity == request.identity &&
               candidate.key == request.key && candidate.owner == request.owner &&
               candidate.action == request.action;
    };
    const auto completed = std::find_if(completed_input_lifecycle_.begin(),
                                        completed_input_lifecycle_.end(),
                                        same_operation);
    if (completed != completed_input_lifecycle_.end())
        return InputLifecycleResult{InputLifecycleStatus::AlreadyApplied,
                                    *completed};
    const auto pending = std::find_if(pending_input_lifecycle_.begin(),
                                      pending_input_lifecycle_.end(),
                                      same_operation);
    if (pending != pending_input_lifecycle_.end())
        return InputLifecycleResult{InputLifecycleStatus::Disconnected,
                                    *pending};
    if (!next_input_lifecycle_operation(request.operation_id)) {
        retire_input_lifecycle_relationship(
            AdapterError::InputLifecycleOperationExhausted);
        return InputLifecycleResult{InputLifecycleStatus::StoreReplaced,
                                    request};
    }

    InputLifecycleResult result{InputLifecycleStatus::Disconnected, request};
    if (state_ == AdapterState::Ready && supervisor_ != nullptr &&
        supervisor_->state() == sidecar::State::Ready &&
        supervisor_->child_pid() > 1 && runtime_nodes_valid()) {
        const local::CredentialExpectation expected{
            config_.expected_service_uid, config_.expected_service_gid,
            static_cast<uint64_t>(supervisor_->child_pid())};
        result = InputLifecycleClient::apply(
            socket_path_, request, expected,
            std::chrono::steady_clock::now() +
                config_.input_lifecycle_timeout);
    }
    if (result.status == InputLifecycleStatus::Applied ||
        result.status == InputLifecycleStatus::AlreadyApplied) {
        if (!remember_completed_input_lifecycle(request)) {
            retire_input_lifecycle_relationship(
                AdapterError::InputLifecycleCapacity);
            result.status = InputLifecycleStatus::StoreReplaced;
        }
    } else if (result.status == InputLifecycleStatus::CapacityExceeded) {
        retire_input_lifecycle_relationship(
            AdapterError::InputLifecycleCapacity);
        result.status = InputLifecycleStatus::StoreReplaced;
    } else if (result.status == InputLifecycleStatus::MalformedResponse ||
               result.status == InputLifecycleStatus::ConflictingReplay ||
               result.status == InputLifecycleStatus::PeerUnauthenticated ||
               result.status == InputLifecycleStatus::StaleIdentity ||
               result.status == InputLifecycleStatus::InvalidArgument ||
               result.status == InputLifecycleStatus::Rejected ||
               result.status == InputLifecycleStatus::StoreReplaced) {
        retire_input_lifecycle_relationship(
            AdapterError::InputLifecycleProtocol);
        result.status = InputLifecycleStatus::StoreReplaced;
    } else if (result.status == InputLifecycleStatus::Timeout ||
               result.status == InputLifecycleStatus::Disconnected ||
               result.status == InputLifecycleStatus::HandshakeFailed) {
        if (pending_input_lifecycle_.size() >=
            config_.max_pending_input_lifecycle) {
            retire_input_lifecycle_relationship(
                AdapterError::InputLifecycleCapacity);
            return InputLifecycleResult{
                InputLifecycleStatus::StoreReplaced, request};
        }
        try {
            pending_input_lifecycle_.push_back(request);
        } catch (...) {
            retire_input_lifecycle_relationship(
                AdapterError::InputLifecycleCapacity);
            return InputLifecycleResult{
                InputLifecycleStatus::StoreReplaced, request};
        }
    }
    return result;
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
    // If a lifecycle command cannot be durably applied or retained for exact
    // retry, synchronously destroy the named store incarnation.  Destruction
    // is the fail-closed reclaim proof: no command is dropped while its
    // InputRecord could remain attachable, and no stale command is rebound to
    // the replacement attempt.
    fail(error);
    disable_relationship();
    if (supervisor_ != nullptr)
        supervisor_->shutdown();
    dispatcher_.reset();
    supervisor_.reset();
    cleanup_attempt_node();
    pending_input_lifecycle_.clear();
    completed_input_lifecycle_.clear();
    advertisement::Update withdrawal;
    apply_observation(withdrawal);
    append_update(pending_advertisement_update_, withdrawal);
}

bool DaemonSidecarAdapter::drain_input_lifecycle() noexcept
{
    for (auto position = pending_input_lifecycle_.begin();
         position != pending_input_lifecycle_.end();) {
        if (position->identity.generation != config_.generation ||
            position->identity.attempt < attempt_) {
            // A proven replacement destroyed the old store.  Already-issued
            // sealed compiler descriptors remain independent; no stale command
            // is ever rebound to the new sidecar.
            position = pending_input_lifecycle_.erase(position);
            continue;
        }
        if (position->identity.attempt != attempt_ ||
            state_ != AdapterState::Ready || supervisor_ == nullptr ||
            supervisor_->state() != sidecar::State::Ready ||
            supervisor_->child_pid() <= 1 || !runtime_nodes_valid()) {
            ++position;
            continue;
        }
        const local::CredentialExpectation expected{
            config_.expected_service_uid, config_.expected_service_gid,
            static_cast<uint64_t>(supervisor_->child_pid())};
        const InputLifecycleResult result = InputLifecycleClient::apply(
            socket_path_, *position, expected,
            std::chrono::steady_clock::now() +
                config_.input_lifecycle_timeout);
        if (result.status == InputLifecycleStatus::Applied ||
            result.status == InputLifecycleStatus::AlreadyApplied) {
            if (!remember_completed_input_lifecycle(*position)) {
                retire_input_lifecycle_relationship(
                    AdapterError::InputLifecycleCapacity);
                return false;
            }
            position = pending_input_lifecycle_.erase(position);
        } else if (result.status == InputLifecycleStatus::Timeout ||
                   result.status == InputLifecycleStatus::Disconnected ||
                   result.status == InputLifecycleStatus::HandshakeFailed) {
            ++position;
        } else {
            if (result.status == InputLifecycleStatus::CapacityExceeded)
                retire_input_lifecycle_relationship(
                    AdapterError::InputLifecycleCapacity);
            else if (result.status == InputLifecycleStatus::MalformedResponse ||
                     result.status == InputLifecycleStatus::ConflictingReplay ||
                     result.status == InputLifecycleStatus::PeerUnauthenticated ||
                     result.status == InputLifecycleStatus::StaleIdentity ||
                     result.status == InputLifecycleStatus::InvalidArgument ||
                     result.status == InputLifecycleStatus::Rejected ||
                     result.status == InputLifecycleStatus::StoreReplaced)
                retire_input_lifecycle_relationship(
                    AdapterError::InputLifecycleProtocol);
            if (supervisor_ == nullptr)
                return false;
            position = pending_input_lifecycle_.erase(position);
        }
        // poll() must never spend one timeout budget per queued lease.  Retry at
        // most one current-incarnation command on each service-loop turn.
        return true;
    }
    return true;
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

void DaemonSidecarAdapter::cleanup_attempt_node() noexcept
{
    // Structured Supervisor owns the exact lease directory/socket and performs
    // identity-checked cleanup after child/group teardown.  The adapter only
    // drops its observation; it never unlinks a path by name.
    attempt_directory_.clear();
    socket_path_.clear();
    attempt_directory_device_ = 0;
    attempt_directory_inode_ = 0;
    socket_device_ = 0;
    socket_inode_ = 0;
}

void DaemonSidecarAdapter::disable_relationship() noexcept
{
    if (dispatcher_ != nullptr)
        dispatcher_->disable();
}

bool DaemonSidecarAdapter::attach_current() noexcept
{
    if (supervisor_ == nullptr || supervisor_->state() != sidecar::State::Ready ||
        supervisor_->child_pid() <= 1 || socket_path_.empty() ||
        !supervisor_->current_lease().has_value())
        return false;
    const sidecar::ReadyLease& lease = *supervisor_->current_lease();
    if (!lease.valid() || lease.identity.generation != config_.generation)
        return false;
    OnDemandEndpoint endpoint;
    endpoint.socket_path = lease.socket_path;
    endpoint.expected_peer = local::CredentialExpectation{
        config_.expected_service_uid, config_.expected_service_gid,
        static_cast<uint64_t>(supervisor_->child_pid())};
    endpoint.lease_identity = lease.identity;
    endpoint.store_root = lease.store_root;
    endpoint.store_derivation_version = lease.store_derivation_version;
    endpoint.c_store_guid = lease.c_store_guid;
    endpoint.f_store_guid = lease.f_store_guid;
    endpoint.socket_path_digest = lease.socket_path_digest;
    endpoint.listener_device = lease.listener_device;
    endpoint.listener_inode = lease.listener_inode;
    if (!endpoint.valid() || !endpoint.current_path_matches())
        return false;
    socket_path_ = lease.socket_path;
    attempt_directory_ = lease.private_directory;
    attempt_directory_device_ = lease.directory_device;
    attempt_directory_inode_ = lease.directory_inode;
    socket_device_ = lease.listener_device;
    socket_inode_ = lease.listener_inode;
    attempt_ = lease.identity.attempt;
    if (dispatcher_ == nullptr ||
        !dispatcher_->set_on_demand_endpoint(std::move(endpoint)))
        return false;
    return true;
}

bool DaemonSidecarAdapter::begin_attempt(bool /*recovery*/) noexcept
{
    if (attempt_ == std::numeric_limits<uint64_t>::max()) {
        fail(AdapterError::AttemptOverflow);
        return false;
    }
    if (launch_identities_ == nullptr) {
        fail(AdapterError::StartupFailure);
        return false;
    }
    next_input_lifecycle_operation_id_ = 1;
    completed_input_lifecycle_.clear();

    try {
        sidecar::Config supervisor_config;
        supervisor_config.executable = config_.executable;
        supervisor_config.arguments = {
            "--peer-uid", std::to_string(config_.expected_daemon_uid),
            "--peer-gid", std::to_string(config_.expected_daemon_gid)};
        supervisor_config.lease_root = config_.runtime_directory;
        supervisor_config.launch_identities = launch_identities_;
        supervisor_config.readiness_timeout = config_.readiness_timeout;
        supervisor_config.shutdown_timeout = config_.shutdown_timeout;
        supervisor_config.restart_window = config_.restart_window;
        supervisor_config.max_restarts = 0;
        supervisor_config.max_attempts_per_recovery = 1;
        supervisor_ = std::make_unique<sidecar::Supervisor>(std::move(supervisor_config));
        state_ = AdapterState::Starting;
        if (!supervisor_->start()) {
            fail(AdapterError::StartupFailure);
            supervisor_->shutdown();
            supervisor_.reset();
            cleanup_attempt_node();
            return false;
        }
        if (!supervisor_->current_lease().has_value() ||
            !supervisor_->current_lease()->valid()) {
            fail(AdapterError::RuntimeNodeFailure);
            supervisor_->shutdown();
            supervisor_.reset();
            cleanup_attempt_node();
            return false;
        }
        const sidecar::ReadyLease& lease = *supervisor_->current_lease();
        socket_path_ = lease.socket_path;
        attempt_directory_ = lease.private_directory;
        attempt_directory_device_ = lease.directory_device;
        attempt_directory_inode_ = lease.directory_inode;
        socket_device_ = lease.listener_device;
        socket_inode_ = lease.listener_inode;
        attempt_ = lease.identity.attempt;
        prior_supervisor_post_ready_exits_ = supervisor_->counters().post_ready_exits;
        prior_counter_observed_ = true;
        dispatcher_ = std::make_unique<CacheSessionDispatcher>(
            local::Identity{config_.generation, attempt_}, config_.handoff_timeout);
        if (!attach_current()) {
            fail(AdapterError::AuthenticationFailure);
            disable_relationship();
            supervisor_->shutdown();
            dispatcher_.reset();
            supervisor_.reset();
            cleanup_attempt_node();
            return false;
        }
        state_ = AdapterState::Ready;
        last_error_ = AdapterError::None;
        if (!drain_input_lifecycle())
            return false;
        return true;
    } catch (...) {
        fail(AdapterError::StartupFailure);
        disable_relationship();
        if (supervisor_ != nullptr)
            supervisor_->shutdown();
        dispatcher_.reset();
        supervisor_.reset();
        cleanup_attempt_node();
        return false;
    }
}

bool DaemonSidecarAdapter::reserve_outer_restart() noexcept
{
    const auto now = std::chrono::steady_clock::now();
    restart_times_.erase(std::remove_if(restart_times_.begin(), restart_times_.end(),
                                         [&](auto timestamp) {
                                             return now - timestamp >= config_.restart_window;
                                         }),
                        restart_times_.end());
    if (restart_times_.size() >= config_.max_restarts)
        return false;
    restart_times_.push_back(now);
    return true;
}

bool DaemonSidecarAdapter::collect_counter_delta() noexcept
{
    if (supervisor_ == nullptr || !prior_counter_observed_)
        return true;
    const uint64_t current = supervisor_->counters().post_ready_exits;
    if (current == std::numeric_limits<uint64_t>::max() ||
        current < prior_supervisor_post_ready_exits_) {
        counter_failed_ = true;
        fail(current == std::numeric_limits<uint64_t>::max()
                 ? AdapterError::CounterSaturated
                 : AdapterError::CounterRegression);
        return false;
    }
    const uint64_t delta = current - prior_supervisor_post_ready_exits_;
    if (delta > std::numeric_limits<uint64_t>::max() - cumulative_post_ready_exits_) {
        counter_failed_ = true;
        fail(AdapterError::CounterSaturated);
        return false;
    }
    cumulative_post_ready_exits_ += delta;
    prior_supervisor_post_ready_exits_ = current;
    return true;
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

void DaemonSidecarAdapter::apply_observation(advertisement::Update& update) noexcept
{
    const bool exact_listener = public_listener_.bound &&
                                public_listener_.port == config_.public_listener_port;
    advertisement::Observation observation;
    observation.public_listener_bound = exact_listener;
    observation.public_listener_port = exact_listener ? public_listener_.port : 0;
    observation.supervisor_state = supervisor_ != nullptr
                                       ? supervisor_->state()
                                       : sidecar::State::Stopped;
    observation.private_relationship_authenticated =
        authenticated() && runtime_nodes_valid();
    observation.current_lease_matches =
        supervisor_ != nullptr && supervisor_->current_lease().has_value() &&
        supervisor_->current_lease()->valid() && runtime_nodes_valid();
    observation.cumulative_post_ready_exits = cumulative_post_ready_exits_;
    const advertisement::Update observed = controller_.observe(observation);
    append_update(update, observed);
    if (observed.error == advertisement::Error::CounterRegression) {
        counter_failed_ = true;
        fail(AdapterError::CounterRegression);
    } else if (observed.error == advertisement::Error::CounterSaturated) {
        counter_failed_ = true;
        fail(AdapterError::CounterSaturated);
    }
    if (state_ != AdapterState::ShuttingDown && state_ != AdapterState::Failed)
        state_ = (supervisor_ != nullptr && supervisor_->state() == sidecar::State::Ready &&
                  authenticated())
                     ? AdapterState::Ready
                     : AdapterState::Absent;
}

bool DaemonSidecarAdapter::recover(advertisement::Update& update) noexcept
{
    for (uint32_t count = 0; count < config_.max_attempts_per_recovery; ++count) {
        if (counter_failed_ || !reserve_outer_restart()) {
            fail(counter_failed_ ? last_error_ : AdapterError::AttemptExhausted);
            apply_observation(update);
            return false;
        }
        if (begin_attempt(true)) {
            apply_observation(update);
            return true;
        }
        if (last_error_ == AdapterError::AttemptOverflow || counter_failed_)
            break;
    }
    if (last_error_ != AdapterError::AttemptOverflow && !counter_failed_)
        fail(AdapterError::AttemptExhausted);
    apply_observation(update);
    return false;
}

bool DaemonSidecarAdapter::start(advertisement::Update* result) noexcept
{
    advertisement::Update update;
    append_pending_advertisement_update(update);
    if (!valid_config(config_)) {
        fail(AdapterError::InvalidConfiguration);
        apply_observation(update);
        if (result != nullptr)
            *result = update;
        return false;
    }
    if (state_ == AdapterState::Ready) {
        apply_observation(update);
        if (result != nullptr)
            *result = update;
        return controller_.snapshot().present();
    }
    shutdown();
    state_ = AdapterState::Absent;
    for (uint32_t count = 0; count < config_.max_attempts_per_recovery; ++count) {
        if (count != 0 && !reserve_outer_restart()) {
            fail(AdapterError::AttemptExhausted);
            break;
        }
        if (begin_attempt(count != 0)) {
            apply_observation(update);
            if (result != nullptr)
                *result = update;
            return controller_.snapshot().present();
        }
        if (last_error_ == AdapterError::AttemptOverflow || counter_failed_)
            break;
    }
    if (last_error_ != AdapterError::AttemptOverflow && !counter_failed_)
        fail(AdapterError::AttemptExhausted);
    apply_observation(update);
    if (result != nullptr)
        *result = update;
    return false;
}

bool DaemonSidecarAdapter::poll(advertisement::Update* result) noexcept
{
    advertisement::Update update;
    append_pending_advertisement_update(update);
    if (state_ == AdapterState::Stopped) {
        apply_observation(update);
        if (result != nullptr)
            *result = update;
        return false;
    }
    if (counter_failed_) {
        apply_observation(update);
        if (result != nullptr)
            *result = update;
        return false;
    }
    if (supervisor_ == nullptr || supervisor_->state() != sidecar::State::Ready) {
        (void)recover(update);
        if (result != nullptr)
            *result = update;
        return controller_.snapshot().present();
    }

    const bool was_ready = supervisor_->poll();
    if (!collect_counter_delta()) {
        disable_relationship();
        apply_observation(update);
        if (supervisor_ != nullptr)
            supervisor_->shutdown();
        dispatcher_.reset();
        supervisor_.reset();
        cleanup_attempt_node();
        if (result != nullptr)
            *result = update;
        return false;
    }
    if (supervisor_->state() != sidecar::State::Ready || !was_ready) {
        // The old relationship is withdrawn before any replacement can be
        // observed.  This also makes the absent->present ordering explicit
        // when recovery succeeds in this same poll.
        disable_relationship();
        apply_observation(update);
        supervisor_->shutdown();
        dispatcher_.reset();
        supervisor_.reset();
        cleanup_attempt_node();
        (void)recover(update);
        if (result != nullptr)
            *result = update;
        return controller_.snapshot().present();
    }
    if (!runtime_nodes_valid()) {
        // The pathname trust boundary is a live invariant, not merely a
        // startup precondition.  Check it after observing child exit so a
        // service-owned unlink during normal death is still counted as a
        // post-READY exit, then withdraw before terminating a live child.
        disable_relationship();
        fail(AdapterError::RuntimeNodeFailure);
        apply_observation(update);
        supervisor_->shutdown();
        dispatcher_.reset();
        supervisor_.reset();
        cleanup_attempt_node();
        if (result != nullptr)
            *result = update;
        return false;
    }
    if (!authenticated()) {
        // A successful one-shot handoff consumes the dispatcher relationship.
        // Publish the withdrawal first; only then may a bounded fresh control
        // connection restore presence on this still-live sidecar.
        apply_observation(update);
        if (attach_current())
            apply_observation(update);
    } else {
        apply_observation(update);
    }
    if (!drain_input_lifecycle())
        apply_observation(update);
    if (result != nullptr)
        *result = update;
    return controller_.snapshot().present();
}

void DaemonSidecarAdapter::fail(AdapterError error) noexcept
{
    last_error_ = error;
    if (error != AdapterError::None)
        state_ = AdapterState::Absent;
}

void DaemonSidecarAdapter::shutdown(advertisement::Update* result) noexcept
{
    advertisement::Update update;
    append_pending_advertisement_update(update);
    if (state_ == AdapterState::ShuttingDown) {
        if (result != nullptr)
            *result = update;
        return;
    }
    state_ = AdapterState::ShuttingDown;
    // Relationship first: no ordinary daemon channel may hand off while the
    // child is being terminated.
    disable_relationship();
    dispatcher_.reset();
    if (supervisor_ != nullptr) {
        supervisor_->shutdown();
        supervisor_.reset();
    }
    cleanup_attempt_node();
    pending_input_lifecycle_.clear();
    completed_input_lifecycle_.clear();
    // Advance the controller and return the ordered withdrawal so the Login
    // owner cannot retain stale presence after sidecar ownership has ended.
    append_update(update, controller_.observe(advertisement::Observation{
                              false, 0, sidecar::State::Stopped, false,
                              cumulative_post_ready_exits_}));
    state_ = AdapterState::Stopped;
    last_error_ = AdapterError::None;
    if (result != nullptr)
        *result = update;
}

} // namespace icecc::p50::daemon
