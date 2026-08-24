#include "p50_input_attachment.h"

#include <functional>
#include <limits>
#include <stdexcept>

namespace icecc::p50 {

namespace {

size_t mix(size_t left, size_t right) noexcept {
    return left ^ (right + size_t{0x9e3779b9} + (left << 6) + (left >> 2));
}

}  // namespace

size_t InputAttachmentRequestHash::operator()(
    const InputAttachmentRequest& request) const noexcept {
    size_t result = InputRecordKeyHash{}(request.key);
    result = mix(result, std::hash<uint64_t>{}(request.attempt.logical_job));
    result = mix(result, std::hash<uint64_t>{}(request.attempt.attempt_id));
    return mix(result, std::hash<uint64_t>{}(request.request_id));
}

InputAttachmentCore::InputAttachmentCore(size_t max_records,
                                         uint64_t max_retained_bytes,
                                         size_t max_pending_ready,
                                         size_t max_replay_entries)
    : records_(max_records, max_retained_bytes),
      max_pending_ready_(max_pending_ready),
      max_replay_entries_(max_replay_entries == 0 ? max_pending_ready
                                                  : max_replay_entries) {
    if (max_pending_ready_ == 0 || max_replay_entries_ == 0)
        throw std::invalid_argument(
            "InputAttachmentCore limits must be nonzero");
}

void InputAttachmentCore::validate_attempt(InputAttempt owner) {
    if (owner.logical_job == 0 || owner.attempt_id == 0)
        throw std::invalid_argument("attachment owner identity must be nonzero");
}

void InputAttachmentCore::validate_key(InputRecordKey key) {
    // TU_SEQ zero is a valid first publication in the route model.  Only the
    // namespace GUID is reserved at zero here.
    if (key.c_store_guid == CStoreGuid{})
        throw std::invalid_argument("attachment key must contain a GUID");
}

void InputAttachmentCore::validate_request(
    const InputAttachmentRequest& request) {
    validate_key(request.key);
    validate_attempt(request.attempt);
    if (request.request_id == 0)
        throw std::invalid_argument("attachment request ID must be nonzero");
}

InputAttachmentCore::OwnerState& InputAttachmentCore::owner_for(
    InputRecordKey key, InputAttempt owner) {
    const auto [position, inserted] = owners_.try_emplace(
        key, OwnerState{owner.logical_job, owner.attempt_id, false});
    if (!inserted && position->second.logical_job != owner.logical_job)
        throw std::logic_error("InputRecord key is owned by another logical job");
    return position->second;
}

const InputAttachmentCore::OwnerState& InputAttachmentCore::owner_for(
    InputRecordKey key, InputAttempt owner) const {
    const auto position = owners_.find(key);
    if (position == owners_.end())
        throw std::out_of_range("attachment named no logical job");
    if (position->second.logical_job != owner.logical_job)
        throw std::logic_error("attachment named another logical job");
    return position->second;
}

void InputAttachmentCore::require_current(InputRecordKey key,
                                          InputAttempt owner) const {
    const OwnerState& state = owner_for(key, owner);
    if (state.current_attempt != owner.attempt_id)
        throw std::logic_error("attachment attempt is stale or revoked");
}

InputPublishResult InputAttachmentCore::commit_open(
    InputAttempt owner, CStoreGuid c_store_guid, const TxBegin& begin,
    const TxCommit& commit, std::vector<uint8_t> exact_input) {
    validate_attempt(owner);
    const InputRecordKey key{c_store_guid, begin.tu_seq};
    validate_key(key);
    OwnerState& state = owner_for(key, owner);
    if (state.current_attempt != owner.attempt_id)
        throw std::logic_error("stale attempt cannot commit InputRecord");

    const bool closed = state.closed;
    const bool already_present = records_.contains(key);
    if (!closed && !already_present &&
        pending_ready_.size() >= max_pending_ready_)
        throw std::length_error("pending input-ready table exhausted");

    if (closed) {
        const InputPublishResult result = records_.observe_closed_job_commit(
            c_store_guid, begin, commit, exact_input);
        // There is deliberately no ready insertion on a closed logical job.
        return result;
    }

    const InputPublishResult result = records_.publish(
        c_store_guid, begin, commit, std::move(exact_input));
    if (result == InputPublishResult::Published) {
        if (next_event_id_ == 0)
            throw std::overflow_error("input-ready event ID exhausted");
        pending_ready_.emplace(
            key, ReadyEvent{key, next_event_id_++, begin.raw_bytes,
                            begin.raw_digest});
    }
    return result;
}

InputAttachmentReply InputAttachmentCore::make_reply(
    const InputAttachmentRequest& request, const ReadyEvent& event,
    InputAttachmentStatus status, bool acknowledged) const {
    return InputAttachmentReply{request, event.event_id, event.raw_bytes,
                                event.raw_digest, status, acknowledged};
}

InputAttachmentReply InputAttachmentCore::request(
    const InputAttachmentRequest& request) {
    validate_request(request);
    const auto owner_position = owners_.find(request.key);
    if (owner_position == owners_.end())
        return InputAttachmentReply{request, 0, 0, {},
                                    InputAttachmentStatus::Unknown, false};
    const OwnerState& owner = owner_position->second;
    if (owner.logical_job != request.attempt.logical_job)
        return InputAttachmentReply{request, 0, 0, {},
                                    InputAttachmentStatus::Unknown, false};
    if (owner.current_attempt != request.attempt.attempt_id)
        return InputAttachmentReply{request, 0, 0, {},
                                    InputAttachmentStatus::StaleAttempt, false};
    if (owner.closed)
        return InputAttachmentReply{request, 0, 0, {},
                                    InputAttachmentStatus::Closed, false};

    const auto stored = replies_.find(request);
    if (stored != replies_.end()) {
        // A byte-for-byte request replay gets the original reply, including
        // its acknowledgement state.  It cannot be rebound to another key or
        // to a replacement attempt (the current-owner checks above run first).
        return stored->second.reply;
    }

    const auto ready = pending_ready_.find(request.key);
    if (ready == pending_ready_.end()) {
        // A committed record can outlive its notification after ACK.  This is
        // a replay observation, not a second ready event.
        if (!records_.contains(request.key))
            return InputAttachmentReply{request, 0, 0, {},
                                        InputAttachmentStatus::Unknown, false};
        const InputCursor cursor = records_.attach(request.key);
        (void)cursor;
        const InputAttachmentReply reply{request, 0, 0, {},
                                         InputAttachmentStatus::ReadyReplay,
                                         false};
        if (replies_.size() >= max_replay_entries_)
            prune_replies();
        if (replies_.size() >= max_replay_entries_)
            throw std::length_error("attachment replay table exhausted");
        replies_.emplace(request, StoredReply{reply, false});
        return reply;
    }

    const InputAttachmentReply reply =
        make_reply(request, ready->second, InputAttachmentStatus::Ready, false);
    if (replies_.size() >= max_replay_entries_)
        prune_replies();
    if (replies_.size() >= max_replay_entries_)
        throw std::length_error("attachment replay table exhausted");
    replies_.emplace(request, StoredReply{reply, false});
    return reply;
}

InputAttachmentAckResult InputAttachmentCore::acknowledge(
    const InputAttachmentRequest& request,
    const InputAttachmentReply& reply) {
    validate_request(request);
    if (reply.request != request)
        return InputAttachmentAckResult::Rejected;
    const auto owner_position = owners_.find(request.key);
    if (owner_position == owners_.end() ||
        owner_position->second.logical_job != request.attempt.logical_job ||
        owner_position->second.current_attempt != request.attempt.attempt_id ||
        owner_position->second.closed)
        return InputAttachmentAckResult::Rejected;
    const auto stored = replies_.find(request);
    if (stored == replies_.end())
        return InputAttachmentAckResult::Rejected;
    const InputAttachmentReply& expected = stored->second.reply;
    if (expected.request != reply.request || expected.event_id != reply.event_id ||
        expected.raw_bytes != reply.raw_bytes ||
        expected.raw_digest != reply.raw_digest ||
        expected.status != reply.status)
        return InputAttachmentAckResult::Rejected;
    if (stored->second.acknowledged)
        return InputAttachmentAckResult::AlreadyAccepted;
    if (reply.status != InputAttachmentStatus::Ready &&
        reply.status != InputAttachmentStatus::ReadyReplay)
        return InputAttachmentAckResult::Rejected;
    stored->second.acknowledged = true;
    stored->second.reply.acknowledged = true;
    if (reply.status == InputAttachmentStatus::Ready)
        pending_ready_.erase(request.key);
    return InputAttachmentAckResult::Accepted;
}

InputCursor InputAttachmentCore::attach(
    const InputAttachmentRequest& request,
    const InputAttachmentReply& reply) const {
    validate_request(request);
    if (reply.request != request ||
        (reply.status != InputAttachmentStatus::Ready &&
         reply.status != InputAttachmentStatus::ReadyReplay))
        throw std::logic_error("attachment reply identity or status mismatch");
    require_current(request.key, request.attempt);
    const OwnerState& state = owner_for(request.key, request.attempt);
    if (state.closed)
        throw std::logic_error("compiler attachment arrived after closure");
    const auto stored = replies_.find(request);
    if (stored == replies_.end())
        throw std::logic_error("attachment reply was never requested");
    const InputAttachmentReply& expected = stored->second.reply;
    if (expected.request != reply.request || expected.event_id != reply.event_id ||
        expected.raw_bytes != reply.raw_bytes ||
        expected.raw_digest != reply.raw_digest ||
        expected.status != reply.status)
        throw std::logic_error("attachment reply is stale or forged");
    return records_.attach(request.key);
}

void InputAttachmentCore::cancel(InputAttempt owner, InputRecordKey key) {
    validate_attempt(owner);
    validate_key(key);
    OwnerState& state = owner_for(key, owner);
    if (state.current_attempt != owner.attempt_id)
        throw std::logic_error("attachment attempt is stale or revoked");
    state.closed = true;
    pending_ready_.erase(key);
    if (records_.contains(key))
        records_.close_job(key);
}

void InputAttachmentCore::replace_attempt(InputAttempt old_owner,
                                           InputAttempt new_owner,
                                           InputRecordKey key) {
    validate_attempt(old_owner);
    validate_attempt(new_owner);
    validate_key(key);
    if (old_owner.logical_job != new_owner.logical_job)
        throw std::invalid_argument("replacement changed logical job identity");
    require_current(key, old_owner);
    if (owners_.at(key).closed)
        throw std::logic_error("closed logical job cannot be replaced");
    owners_.at(key).current_attempt = new_owner.attempt_id;
}

void InputAttachmentCore::retry(InputAttempt old_owner, uint64_t new_attempt_id,
                                InputRecordKey key) {
    if (new_attempt_id == 0)
        throw std::invalid_argument("retry ATTEMPT_ID must be nonzero");
    replace_attempt(old_owner,
                    InputAttempt{old_owner.logical_job, new_attempt_id}, key);
}

InputPublishResult InputAttachmentCore::observe_closed_commit(
    InputAttempt owner, CStoreGuid c_store_guid, const TxBegin& begin,
    const TxCommit& commit, std::span<const uint8_t> exact_input) {
    validate_attempt(owner);
    const InputRecordKey key{c_store_guid, begin.tu_seq};
    validate_key(key);
    const OwnerState& state = owner_for(key, owner);
    if (state.current_attempt != owner.attempt_id)
        throw std::logic_error("stale attempt cannot observe closed commit");
    if (!state.closed)
        throw std::logic_error("closed commit observed for open logical job");
    return records_.observe_closed_job_commit(c_store_guid, begin, commit,
                                               exact_input);
}

void InputAttachmentCore::prune_replies() {
    for (auto position = replies_.begin(); position != replies_.end();) {
        if (position->second.acknowledged)
            position = replies_.erase(position);
        else
            ++position;
    }
}

void InputAttachmentCore::collect_garbage() {
    records_.collect_garbage();
    for (auto position = replies_.begin(); position != replies_.end();) {
        if (position->second.acknowledged &&
            !records_.contains(position->first.key))
            position = replies_.erase(position);
        else
            ++position;
    }
}

void InputAttachmentCore::clear() {
    records_.clear();
    owners_.clear();
    pending_ready_.clear();
    replies_.clear();
    next_event_id_ = 1;
}

}  // namespace icecc::p50
