#include "p50_input_lifecycle.h"
#include "p50_control_operation.h"

#include <algorithm>
#include <stdexcept>

namespace icecc::p50 {

namespace {

InputLifecycleResult lifecycle_rejected(InputLifecycleStatus status,
                                        InputLifecycleRequest request) noexcept {
    return InputLifecycleResult{status, request};
}

InputLifecycleStatus lifecycle_status_for_local(local::Status status) noexcept {
    switch (status) {
    case local::Status::Timeout:
        return InputLifecycleStatus::Timeout;
    case local::Status::StaleGeneration:
    case local::Status::IdentityMismatch:
        return InputLifecycleStatus::StaleIdentity;
    case local::Status::CleanEof:
    case local::Status::Truncated:
    case local::Status::IoError:
        return InputLifecycleStatus::Disconnected;
    case local::Status::PeerCredentialUnavailable:
    case local::Status::PeerCredentialMismatch:
        return InputLifecycleStatus::PeerUnauthenticated;
    case local::Status::InvalidArgument:
    case local::Status::InvalidPath:
        return InputLifecycleStatus::InvalidArgument;
    default:
        return InputLifecycleStatus::HandshakeFailed;
    }
}

InputLifecycleStatus lifecycle_status_for_apply(
    InputLifecycleApplyStatus status) noexcept {
    switch (status) {
    case InputLifecycleApplyStatus::Applied:
        return InputLifecycleStatus::Applied;
    case InputLifecycleApplyStatus::AlreadyApplied:
        return InputLifecycleStatus::AlreadyApplied;
    case InputLifecycleApplyStatus::InvalidArgument:
        return InputLifecycleStatus::InvalidArgument;
    case InputLifecycleApplyStatus::UnknownRecord:
        return InputLifecycleStatus::UnknownRecord;
    case InputLifecycleApplyStatus::StaleOwner:
        return InputLifecycleStatus::StaleOwner;
    case InputLifecycleApplyStatus::ConflictingReplay:
        return InputLifecycleStatus::ConflictingReplay;
    case InputLifecycleApplyStatus::CapacityExceeded:
        return InputLifecycleStatus::CapacityExceeded;
    }
    return InputLifecycleStatus::Rejected;
}

}  // namespace

InputLifecycleRegistry::InputLifecycleRegistry(size_t max_owners,
                                               size_t max_replays)
    : max_owners_(max_owners), max_replays_(max_replays) {
    if (max_owners_ == 0 || max_replays_ == 0)
        throw std::invalid_argument(
            "input lifecycle owner and replay limits must be nonzero");
}

bool InputLifecycleRegistry::key_valid(InputRecordKey key) noexcept {
    return key.c_store_guid != CStoreGuid{};
}

InputLifecycleCommitDecision InputLifecycleRegistry::prepare_route_commit(
    InputRecordKey key) noexcept {
    if (!key_valid(key))
        return InputLifecycleCommitDecision::CapacityExceeded;
    auto position = leases_.find(key);
    if (position == leases_.end()) {
        if (leases_.size() >= max_owners_)
            return InputLifecycleCommitDecision::CapacityExceeded;
        try {
            position = leases_.try_emplace(key).first;
        } catch (...) {
            return InputLifecycleCommitDecision::CapacityExceeded;
        }
    }
    Lease& lease = position->second;
    lease.route_commit_pending = true;
    return lease.job_closed ? InputLifecycleCommitDecision::Closed
                            : InputLifecycleCommitDecision::Open;
}

void InputLifecycleRegistry::abort_route_commit(InputRecordKey key) noexcept {
    const auto position = leases_.find(key);
    if (position == leases_.end())
        return;
    Lease& lease = position->second;
    lease.route_commit_pending = false;
    if (!lease.committed && !lease.route_commit_observed &&
        !lease.job_closed && !lease.owner.has_value())
        erase_lease(position);
}

bool InputLifecycleRegistry::observe_route_commit(InputRecordKey key,
                                                  bool retained) noexcept {
    const auto position = leases_.find(key);
    if (position == leases_.end())
        return false;
    Lease& lease = position->second;
    if (!lease.route_commit_pending && !lease.route_commit_observed)
        return false;
    if (retained && lease.job_closed)
        return false;
    lease.route_commit_pending = false;
    lease.route_commit_observed = true;
    lease.committed = retained;
    if (!retained)
        lease.job_closed = true;
    collect_closed(key);
    return true;
}

bool InputLifecycleRegistry::begin_attachment(InputRecordKey key,
                                              InputLeaseOwner owner,
                                              uint64_t request_id) {
    if (!key_valid(key) || !input_lease_owner_valid(owner) || request_id == 0)
        return false;
    const auto position = leases_.find(key);
    if (position == leases_.end())
        return false;
    Lease& lease = position->second;
    if (!lease.committed || lease.job_closed || lease.attachment_pending)
        return false;

    if (!lease.owner.has_value()) {
        lease.owner = owner;
    } else if (*lease.owner == owner) {
        // Attempt cancellation permanently revokes this exact assignment
        // owner, including when cancellation races a lost descriptor ACK.
        // Only the fresh-owner replacement arm below may reuse retained
        // bytes after CancelAttempt.
        if (lease.attempt_cancelled)
            return false;
    } else {
        // Replacement is legal only after the exact current attempt was
        // cancelled.  It keeps the same logical job and must carry a fresh
        // assignment nonce/ATTEMPT_ID.
        if (!lease.attempt_cancelled ||
            lease.owner->logical_job != owner.logical_job ||
            lease.owner->assignment_nonce == owner.assignment_nonce ||
            std::find(lease.retired_owners.begin(),
                      lease.retired_owners.end(), owner) !=
                lease.retired_owners.end() ||
            retired_owner_count_ >= max_replays_)
            return false;
        try {
            lease.retired_owners.push_back(*lease.owner);
        } catch (...) {
            return false;
        }
        ++retired_owner_count_;
        lease.owner = owner;
        lease.attachment_authorized = false;
        lease.attempt_cancelled = false;
        lease.attachment_request_id = 0;
    }

    if (lease.attachment_authorized || lease.attachment_request_id != 0)
        return false;
    lease.attachment_request_id = request_id;
    lease.attachment_pending = true;
    return true;
}

void InputLifecycleRegistry::finish_attachment(InputRecordKey key,
                                               InputLeaseOwner owner,
                                               uint64_t request_id,
                                               bool authorized) noexcept {
    const auto position = leases_.find(key);
    if (position == leases_.end())
        return;
    Lease& lease = position->second;
    if (!lease.owner.has_value() || *lease.owner != owner ||
        !lease.attachment_pending ||
        lease.attachment_request_id != request_id)
        return;
    lease.attachment_pending = false;
    lease.attachment_authorized = authorized;
    if (!authorized)
        lease.attachment_request_id = 0;
    collect_closed(key);
}

bool InputLifecycleRegistry::replay_conflicts(
    const InputLifecycleRequest& request,
    InputLifecycleApplyResult& result) const noexcept {
    const auto replay = replays_.find(request.operation_id);
    if (replay == replays_.end())
        return false;
    if (replay->second.request != request || !replay->second.complete) {
        result.status = InputLifecycleApplyStatus::ConflictingReplay;
    } else if (replay->second.status == InputLifecycleApplyStatus::Applied) {
        result.status = InputLifecycleApplyStatus::AlreadyApplied;
    } else {
        result.status = replay->second.status;
    }
    return true;
}

void InputLifecycleRegistry::complete_replay(
    const InputLifecycleRequest& request,
    InputLifecycleApplyStatus status) noexcept {
    const auto position = replays_.find(request.operation_id);
    if (position == replays_.end() || position->second.request != request)
        return;
    position->second.status = status;
    position->second.complete = true;
}

void InputLifecycleRegistry::erase_lease(
    std::map<InputRecordKey, Lease>::iterator position) noexcept {
    if (position == leases_.end())
        return;
    const size_t retired = position->second.retired_owners.size();
    retired_owner_count_ = retired > retired_owner_count_
                               ? 0
                               : retired_owner_count_ - retired;
    leases_.erase(position);
}

bool InputLifecycleRegistry::reserve_replay(
    const InputLifecycleRequest& request) noexcept {
    try {
        if (replays_.size() >= max_replays_) {
            if (replay_order_.empty())
                return false;
            const uint64_t oldest = replay_order_.front();
            const auto position = replays_.find(oldest);
            if (position == replays_.end() || !position->second.complete)
                return false;
            replays_.erase(position);
            replay_order_.pop_front();
        }
        const auto [position, inserted] = replays_.emplace(
            request.operation_id,
            Replay{request, InputLifecycleApplyStatus::Applied, false});
        (void)position;
        if (!inserted)
            return false;
        replay_order_.push_back(request.operation_id);
        return true;
    } catch (...) {
        replays_.erase(request.operation_id);
        if (!replay_order_.empty() && replay_order_.back() == request.operation_id)
            replay_order_.pop_back();
        return false;
    }
}

void InputLifecycleRegistry::forget_replay(uint64_t operation_id) noexcept {
    replays_.erase(operation_id);
    const auto position =
        std::find(replay_order_.begin(), replay_order_.end(), operation_id);
    if (position != replay_order_.end())
        replay_order_.erase(position);
}

InputLifecycleApplyResult InputLifecycleRegistry::begin_apply(
    const InputLifecycleRequest& request) noexcept {
    InputLifecycleApplyResult result;
    if (request.identity.generation == 0 || request.identity.attempt == 0 ||
        request.operation_id == 0 || !key_valid(request.key) ||
        !input_lease_owner_valid(request.owner) ||
        !input_lifecycle_action_valid(request.action))
        return result;
    if (replay_conflicts(request, result))
        return result;
    if (!reserve_replay(request)) {
        result.status = InputLifecycleApplyStatus::CapacityExceeded;
        return result;
    }

    auto position = leases_.find(request.key);
    bool created = false;
    if (position == leases_.end()) {
        if (request.action == InputLifecycleAction::CancelAttempt) {
            result.status = InputLifecycleApplyStatus::UnknownRecord;
            complete_replay(request, result.status);
            return result;
        }
        if (leases_.size() >= max_owners_) {
            result.status = InputLifecycleApplyStatus::CapacityExceeded;
            complete_replay(request, result.status);
            return result;
        }
        try {
            position = leases_.try_emplace(request.key).first;
        } catch (...) {
            result.status = InputLifecycleApplyStatus::CapacityExceeded;
            complete_replay(request, result.status);
            return result;
        }
        position->second.owner = request.owner;
        created = true;
    }

    Lease& lease = position->second;
    if (!lease.owner.has_value())
        lease.owner = request.owner;
    if (*lease.owner != request.owner || lease.apply_pending) {
        if (created)
            erase_lease(position);
        result.status = InputLifecycleApplyStatus::StaleOwner;
        complete_replay(request, result.status);
        return result;
    }
    if ((lease.job_closed &&
         request.action != InputLifecycleAction::CancelAttempt) ||
        (request.action == InputLifecycleAction::CancelAttempt &&
         lease.attempt_cancelled)) {
        result.status = InputLifecycleApplyStatus::AlreadyApplied;
        complete_replay(request, result.status);
        return result;
    }
    if (lease.job_closed) {
        if (created)
            erase_lease(position);
        result.status = InputLifecycleApplyStatus::StaleOwner;
        complete_replay(request, result.status);
        return result;
    }

    lease.apply_pending = true;
    lease.apply_created = created;
    result.status = InputLifecycleApplyStatus::Applied;
    if (request.action != InputLifecycleAction::CancelAttempt && lease.committed) {
        result.close_record = true;
        result.collect_record = true;
    }
    return result;
}

void InputLifecycleRegistry::finish_apply(
    const InputLifecycleRequest& request,
    bool endpoint_mutation_succeeded) noexcept {
    const auto replay = replays_.find(request.operation_id);
    const auto position = leases_.find(request.key);
    if (replay == replays_.end() || replay->second.request != request ||
        replay->second.complete || position == leases_.end())
        return;
    Lease& lease = position->second;
    if (!lease.apply_pending || !lease.owner.has_value() ||
        *lease.owner != request.owner)
        return;

    if (!endpoint_mutation_succeeded) {
        const bool erase = lease.apply_created;
        lease.apply_pending = false;
        lease.apply_created = false;
        forget_replay(request.operation_id);
        if (erase) {
            const auto created = leases_.find(request.key);
            if (created != leases_.end())
                erase_lease(created);
        }
        return;
    }

    lease.apply_pending = false;
    lease.apply_created = false;
    if (request.action == InputLifecycleAction::CancelAttempt) {
        lease.attempt_cancelled = true;
    } else {
        lease.job_closed = true;
        lease.attempt_cancelled = true;
    }
    replay->second.status = InputLifecycleApplyStatus::Applied;
    replay->second.complete = true;
    collect_closed(request.key);
}

void InputLifecycleRegistry::collect_closed(InputRecordKey key) noexcept {
    const auto position = leases_.find(key);
    if (position == leases_.end())
        return;
    const Lease& lease = position->second;
    // A pre-commit terminal tombstone must remain for input_job_state().  Once
    // a committed record has been closed/collected, exact operation replays are
    // answered by the separately bounded replay table and the owner row can go.
    if (lease.job_closed && lease.route_commit_observed &&
        !lease.attachment_pending)
        erase_lease(position);
}

bool InputLifecycleRegistry::job_closed(InputRecordKey key) const noexcept {
    const auto position = leases_.find(key);
    return position != leases_.end() && position->second.job_closed;
}

void InputLifecycleRegistry::clear() noexcept {
    leases_.clear();
    replays_.clear();
    replay_order_.clear();
    retired_owner_count_ = 0;
}

const char* input_lifecycle_status_name(InputLifecycleStatus status) noexcept {
    switch (status) {
    case InputLifecycleStatus::Applied: return "applied";
    case InputLifecycleStatus::AlreadyApplied: return "already-applied";
    case InputLifecycleStatus::StoreReplaced: return "store-replaced";
    case InputLifecycleStatus::InvalidArgument: return "invalid-argument";
    case InputLifecycleStatus::PeerUnauthenticated: return "peer-unauthenticated";
    case InputLifecycleStatus::HandshakeFailed: return "handshake-failed";
    case InputLifecycleStatus::MalformedResponse: return "malformed-response";
    case InputLifecycleStatus::StaleIdentity: return "stale-identity";
    case InputLifecycleStatus::UnknownRecord: return "unknown-record";
    case InputLifecycleStatus::StaleOwner: return "stale-owner";
    case InputLifecycleStatus::ConflictingReplay: return "conflicting-replay";
    case InputLifecycleStatus::CapacityExceeded: return "capacity-exceeded";
    case InputLifecycleStatus::Rejected: return "rejected";
    case InputLifecycleStatus::Timeout: return "timeout";
    case InputLifecycleStatus::Disconnected: return "disconnected";
    }
    return "unknown";
}

InputLifecycleResult InputLifecycleClient::apply(
    const std::string& socket_path, InputLifecycleRequest request,
    const local::CredentialExpectation& expected_peer,
    std::chrono::steady_clock::time_point deadline) noexcept {
    if (request.identity.generation == 0 || request.identity.attempt == 0 ||
        request.operation_id == 0 || request.key.c_store_guid == CStoreGuid{} ||
        !input_lease_owner_valid(request.owner) ||
        !input_lifecycle_action_valid(request.action) || !expected_peer.specified())
        return lifecycle_rejected(InputLifecycleStatus::InvalidArgument, request);

    local::Status io = local::Status::InvalidArgument;
    local::Connection connection =
        local::connect_unix_until(socket_path, deadline, &io);
    if (!connection.valid())
        return lifecycle_rejected(lifecycle_status_for_local(io), request);
    io = connection.verify_peer_credentials(expected_peer);
    if (io != local::Status::Ok)
        return lifecycle_rejected(InputLifecycleStatus::PeerUnauthenticated, request);
    io = connection.send_until(
        local::make_hello(local::PeerRole::Daemon, request.identity), deadline);
    if (io != local::Status::Ok)
        return lifecycle_rejected(lifecycle_status_for_local(io), request);

    local::Frame reply;
    io = connection.receive_until(reply, deadline);
    if (io != local::Status::Ok)
        return lifecycle_rejected(lifecycle_status_for_local(io), request);
    io = local::validate_handshake(reply, local::MessageType::HelloAck,
                                   local::PeerRole::Sidecar,
                                   request.identity);
    if (io != local::Status::Ok)
        return lifecycle_rejected(lifecycle_status_for_local(io), request);

    const local::ControlOperation expected =
        local::make_input_lifecycle_operation(request);
    const std::vector<uint8_t> payload = local::encode_control_operation(expected);
    if (payload.empty())
        return lifecycle_rejected(InputLifecycleStatus::InvalidArgument, request);
    local::Frame operation_frame{local::kProtocolVersion,
                                 local::MessageType::Data,
                                 request.identity, payload};
    io = connection.send_until(operation_frame, deadline);
    if (io != local::Status::Ok)
        return lifecycle_rejected(lifecycle_status_for_local(io), request);

    local::Frame response;
    io = connection.receive_until(response, deadline);
    if (io != local::Status::Ok)
        return lifecycle_rejected(lifecycle_status_for_local(io), request);
    if (response.type != local::MessageType::Data ||
        local::validate_identity(response, request.identity) != local::Status::Ok)
        return lifecycle_rejected(InputLifecycleStatus::MalformedResponse, request);
    local::ControlOperation observed;
    if (!local::decode_control_operation(response.payload, observed))
        return lifecycle_rejected(InputLifecycleStatus::MalformedResponse, request);

    // Keep the responder from closing while unread response bytes can still
    // produce POLLIN|POLLHUP.  The sidecar waits for this exact final ACK; a
    // lost ACK is harmless because its replay table already owns the result.
    const local::Frame acknowledgement{local::kProtocolVersion,
                                       local::MessageType::Goodbye,
                                       request.identity, {}};
    (void)connection.send_until(acknowledgement, deadline);

    if (observed.kind != expected.kind || observed.identity != expected.identity ||
        observed.request_id != expected.request_id || observed.input != expected.input ||
        observed.owner != expected.owner ||
        observed.lifecycle_action != expected.lifecycle_action ||
        !observed.lifecycle_result.has_value())
        return lifecycle_rejected(InputLifecycleStatus::MalformedResponse, request);
    return InputLifecycleResult{
        lifecycle_status_for_apply(*observed.lifecycle_result), request};
}

}  // namespace icecc::p50
