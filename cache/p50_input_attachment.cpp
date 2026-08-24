#include "p50_input_attachment.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <new>
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
    result = mix(result,
                 std::hash<uint64_t>{}(request.attempt.store_generation));
    return mix(result, std::hash<uint64_t>{}(request.request_id));
}

InputAttachmentCore::InputAttachmentCore(size_t max_records,
                                         uint64_t max_retained_bytes,
                                         size_t max_pending_ready,
                                         size_t max_replay_entries,
                                         size_t max_owner_entries)
    : records_(max_records, max_retained_bytes),
      max_pending_ready_(max_pending_ready),
      max_replay_entries_(max_replay_entries == 0 ? max_pending_ready
                                                  : max_replay_entries),
      max_owner_entries_(max_owner_entries == 0 ? max_records
                                                : max_owner_entries) {
    if (max_pending_ready_ == 0 || max_replay_entries_ == 0 ||
        max_owner_entries_ == 0)
        throw std::invalid_argument(
            "InputAttachmentCore limits must be nonzero");
}

void InputAttachmentCore::validate_attempt(InputAttempt owner) {
    if (owner.logical_job == 0 || owner.attempt_id == 0 ||
        owner.store_generation == 0)
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

void InputAttachmentCore::require_generation(InputAttempt owner) const {
    if (owner.store_generation != store_generation_)
        throw std::logic_error("attachment session generation is stale");
}

InputAttachmentCore::Lifecycle& InputAttachmentCore::owner_for(
    InputRecordKey key, InputAttempt owner) {
    require_generation(owner);
    const auto position = lifecycles_.find(key);
    if (position != lifecycles_.end()) {
        if (position->second.logical_job != owner.logical_job)
            throw std::logic_error(
                "InputRecord key is owned by another logical job");
        return position->second;
    }
    if (lifecycles_.size() >= max_owner_entries_)
        throw std::length_error("attachment owner table exhausted");
    const Lifecycle state{owner.logical_job,
                          owner.attempt_id,
                          owner.store_generation,
                          false,
                          false,
                          false,
                          std::nullopt,
                          false,
                          false,
                          {},
                          {}};
    const auto [inserted_position, inserted] = lifecycles_.emplace(key, state);
    if (!inserted)
        throw std::logic_error("attachment lifecycle insertion lost key");
    return inserted_position->second;
}

InputAttachmentCore::Lifecycle& InputAttachmentCore::existing_owner_for(
    InputRecordKey key, InputAttempt owner) {
    require_generation(owner);
    const auto position = lifecycles_.find(key);
    if (position == lifecycles_.end())
        throw std::out_of_range("attachment named no logical job");
    if (position->second.logical_job != owner.logical_job)
        throw std::logic_error("attachment named another logical job");
    return position->second;
}

const InputAttachmentCore::Lifecycle& InputAttachmentCore::owner_for(
    InputRecordKey key, InputAttempt owner) const {
    require_generation(owner);
    const auto position = lifecycles_.find(key);
    if (position == lifecycles_.end())
        throw std::out_of_range("attachment named no logical job");
    if (position->second.logical_job != owner.logical_job)
        throw std::logic_error("attachment named another logical job");
    return position->second;
}

void InputAttachmentCore::require_current(InputRecordKey key,
                                          InputAttempt owner) const {
    const Lifecycle& state = owner_for(key, owner);
    if (state.current_attempt != owner.attempt_id)
        throw std::logic_error("attachment attempt is stale or revoked");
}

bool InputAttachmentCore::can_admit_ready() const {
    return pending_ready_count() < max_pending_ready_;
}

InputPublishResult InputAttachmentCore::commit_open(
    InputAttempt owner, CStoreGuid c_store_guid, const TxBegin& begin,
    const TxCommit& commit, std::vector<uint8_t> exact_input) {
    validate_attempt(owner);
    require_generation(owner);
    const InputRecordKey key{c_store_guid, begin.tu_seq};
    validate_key(key);
    const auto existing = lifecycles_.find(key);
    if (existing != lifecycles_.end()) {
        Lifecycle& state = existing->second;
        if (state.logical_job != owner.logical_job)
            throw std::logic_error(
                "InputRecord key is owned by another logical job");
        if (state.current_attempt != owner.attempt_id)
            throw std::logic_error("stale attempt cannot commit InputRecord");
        if (state.closed) {
            const InputPublishResult result =
                records_.observe_closed_job_commit(
                    c_store_guid, begin, commit, exact_input);
            // This was the late commit for which the closed tombstone was
            // retained.  Mark it observed only after exact validation, then
            // release a record-free owner immediately.
            state.closed_commit_observed = true;
            collect_lifecycles();
            return result;
        }
        if (state.ready) {
            // InputRecordStore validates exact identity before accepting an
            // existing duplicate; no attachment state changes on failure.
            return records_.publish(c_store_guid, begin, commit,
                                    std::move(exact_input));
        }

        // Attempt replacement may legitimately precede the route commit.  In
        // that case owner_for() already created the canonical lifecycle but
        // no record/Ready event exists yet.  Publication must complete the
        // same transaction as a first commit rather than silently retaining
        // compiler-invisible bytes.
        if (records_.contains(key))
            throw std::logic_error(
                "InputRecord exists without its canonical Ready event");
        if (next_event_id_ == 0)
            throw std::overflow_error("input-ready event ID exhausted");
        if (!can_admit_ready())
            throw std::length_error("pending input-ready table exhausted");
        const InputPublishResult result = records_.publish(
            c_store_guid, begin, commit, std::move(exact_input));
        if (result != InputPublishResult::Published)
            throw std::logic_error(
                "pre-created lifecycle unexpectedly found an InputRecord");
        state.ready = ReadyEvent{key, next_event_id_, begin.raw_bytes,
                                 begin.raw_digest};
        state.ready_pending = true;
        ++next_event_id_;
        return result;
    }

    if (next_event_id_ == 0)
        throw std::overflow_error("input-ready event ID exhausted");
    if (!can_admit_ready())
        throw std::length_error("pending input-ready table exhausted");
    if (lifecycles_.size() >= max_owner_entries_)
        throw std::length_error("attachment owner table exhausted");

    // Publish the immutable bytes first, but roll them back if the canonical
    // lifecycle node cannot be installed.  No owner is inserted before exact
    // validation, capacity checks, and this transaction's commit point.
    const InputPublishResult result = records_.publish(
        c_store_guid, begin, commit, std::move(exact_input));
    if (result != InputPublishResult::Published)
        throw std::logic_error("new InputRecord unexpectedly already exists");

    try {
#ifdef P50_ATTACHMENT_TEST_SEAMS
        if (fail_lifecycle_insert_) {
            fail_lifecycle_insert_ = false;
            throw std::bad_alloc();
        }
#endif
        const Lifecycle state{
            owner.logical_job,
            owner.attempt_id,
            owner.store_generation,
            false,
            false,
            true,
            ReadyEvent{key, next_event_id_, begin.raw_bytes, begin.raw_digest},
            false,
            false,
            {},
            {}};
        const auto [position, inserted] = lifecycles_.emplace(key, state);
        if (!inserted)
            throw std::logic_error("InputRecord lifecycle insertion lost key");
        ++next_event_id_;
        return result;
    } catch (...) {
        records_.rollback_new_record(key);
        throw;
    }
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
    require_generation(request.attempt);
    const auto position = lifecycles_.find(request.key);
    if (position == lifecycles_.end())
        return InputAttachmentReply{request, 0, 0, {},
                                    InputAttachmentStatus::Unknown, false};
    Lifecycle& state = position->second;
    if (state.logical_job != request.attempt.logical_job)
        return InputAttachmentReply{request, 0, 0, {},
                                    InputAttachmentStatus::Unknown, false};
    if (state.current_attempt != request.attempt.attempt_id)
        return InputAttachmentReply{request, 0, 0, {},
                                    InputAttachmentStatus::StaleAttempt, false};
    if (state.closed)
        return InputAttachmentReply{request, 0, 0, {},
                                    InputAttachmentStatus::Closed, false};
    if (!state.ready)
        return InputAttachmentReply{request, 0, 0, {},
                                    InputAttachmentStatus::Unknown, false};

    const auto stored = replies_.find(request);
    if (stored != replies_.end())
        return stored->second.reply;
    if (state.attachment_request_valid &&
        request == state.attachment_request)
        return state.attachment_reply;

    const InputAttachmentStatus status =
        state.ready_pending ? InputAttachmentStatus::Ready
                            : InputAttachmentStatus::ReadyReplay;
    const InputAttachmentReply reply =
        make_reply(request, *state.ready, status, false);
    if (replies_.size() >= max_replay_entries_)
        prune_replies();
    if (replies_.size() >= max_replay_entries_)
        throw std::length_error("attachment replay table exhausted");

    try {
#ifdef P50_ATTACHMENT_TEST_SEAMS
        if (fail_reply_insert_) {
            fail_reply_insert_ = false;
            throw std::bad_alloc();
        }
#endif
        const auto [inserted, was_inserted] =
            replies_.emplace(request, StoredReply{reply, false});
        if (!was_inserted)
            return inserted->second.reply;
    } catch (...) {
        // No per-key observation is installed before its bounded replay entry
        // is durable, so a failed insertion leaves state unchanged.
        throw;
    }

    // The first request is the canonical one-attempt authorization token. A
    // later request ID may be observed/replayed, but cannot create a second
    // compiler cursor; exact replay of this token remains valid after pruning.
    if (!state.attachment_request_valid) {
        state.attachment_request = request;
        state.attachment_reply = reply;
        state.attachment_request_valid = true;
    }
    return reply;
}

InputAttachmentAckResult InputAttachmentCore::acknowledge(
    const InputAttachmentRequest& request,
    const InputAttachmentReply& reply) {
    validate_request(request);
    require_generation(request.attempt);
    if (reply.request != request)
        return InputAttachmentAckResult::Rejected;
    const auto position = lifecycles_.find(request.key);
    if (position == lifecycles_.end())
        return InputAttachmentAckResult::Rejected;
    Lifecycle& state = position->second;
    if (state.logical_job != request.attempt.logical_job ||
        state.current_attempt != request.attempt.attempt_id || state.closed ||
        !state.ready)
        return InputAttachmentAckResult::Rejected;

    const auto stored = replies_.find(request);
    const InputAttachmentReply* expected = nullptr;
    bool already_acknowledged = false;
    if (stored != replies_.end()) {
        expected = &stored->second.reply;
        already_acknowledged = stored->second.acknowledged;
    } else if (state.attachment_request_valid &&
               request == state.attachment_request) {
        expected = &state.attachment_reply;
        already_acknowledged = state.attachment_reply.acknowledged;
    }
    if (!expected || expected->request != reply.request ||
        expected->event_id != reply.event_id ||
        expected->raw_bytes != reply.raw_bytes ||
        expected->raw_digest != reply.raw_digest || expected->status != reply.status)
        return InputAttachmentAckResult::Rejected;
    if (already_acknowledged)
        return InputAttachmentAckResult::AlreadyAccepted;
    if (expected->acknowledged != reply.acknowledged)
        return InputAttachmentAckResult::Rejected;
    if (reply.status != InputAttachmentStatus::Ready &&
        reply.status != InputAttachmentStatus::ReadyReplay)
        return InputAttachmentAckResult::Rejected;

    state.attachment_reply.acknowledged = true;
    // Every request for the current key observes the same canonical event.
    // Once any exact reply is ACKed, mark all of those bounded observations
    // acknowledged so pruning cannot strand an older request ID forever.
    for (auto& position : replies_) {
        if (position.first.key == request.key &&
            position.second.reply.event_id == reply.event_id) {
            position.second.acknowledged = true;
            position.second.reply.acknowledged = true;
        }
    }
    if (reply.status == InputAttachmentStatus::Ready)
        state.ready_pending = false;
    return InputAttachmentAckResult::Accepted;
}

InputCursor InputAttachmentCore::attach(
    const InputAttachmentRequest& request,
    const InputAttachmentReply& reply) const {
    validate_request(request);
    require_generation(request.attempt);
    if (reply.request != request ||
        (reply.status != InputAttachmentStatus::Ready &&
         reply.status != InputAttachmentStatus::ReadyReplay))
        throw std::logic_error("attachment reply identity or status mismatch");
    const Lifecycle& state = owner_for(request.key, request.attempt);
    if (state.closed)
        throw std::logic_error("compiler attachment arrived after closure");
    if (state.attachment_admitted)
        throw std::logic_error("logical attempt already owns an attachment");
    if (!state.attachment_request_valid ||
        request != state.attachment_request)
        throw std::logic_error("attachment request is not the canonical attempt token");
    const InputAttachmentReply& expected = state.attachment_reply;
    if (expected.request != reply.request || expected.event_id != reply.event_id ||
        expected.raw_bytes != reply.raw_bytes ||
        expected.raw_digest != reply.raw_digest || expected.status != reply.status)
        throw std::logic_error("attachment reply is stale or forged");
    InputCursor cursor = records_.attach(request.key);
    // The cursor owns the immutable backing. Only after that handoff succeeds
    // does this attempt become permanently single-attachment.
    state.attachment_admitted = true;
    return cursor;
}

void InputAttachmentCore::cancel(InputAttempt owner, InputRecordKey key) {
    validate_attempt(owner);
    validate_key(key);
    Lifecycle& state = owner_for(key, owner);
    if (state.current_attempt != owner.attempt_id)
        throw std::logic_error("attachment attempt is stale or revoked");
    state.closed = true;
    state.ready_pending = false;
    purge_replies(key);
    if (records_.contains(key)) {
        // A retained record proves its route commit was already observed;
        // after cursor-backed record reclamation no additional tombstone is
        // needed for this owner.
        state.closed_commit_observed = true;
        records_.close_job(key);
    }
}

void InputAttachmentCore::replace_attempt(InputAttempt old_owner,
                                           InputAttempt new_owner,
                                           InputRecordKey key) {
    validate_attempt(old_owner);
    validate_attempt(new_owner);
    validate_key(key);
    require_generation(old_owner);
    require_generation(new_owner);
    if (old_owner.logical_job != new_owner.logical_job)
        throw std::invalid_argument("replacement changed logical job identity");
    if (old_owner.attempt_id == new_owner.attempt_id)
        throw std::invalid_argument("replacement reused ATTEMPT_ID");
    if (old_owner.store_generation != new_owner.store_generation)
        throw std::invalid_argument("replacement changed store generation");
    Lifecycle& state = owner_for(key, old_owner);
    if (state.current_attempt != old_owner.attempt_id)
        throw std::logic_error("attachment attempt is stale or revoked");
    if (state.closed)
        throw std::logic_error("closed logical job cannot be replaced");
    purge_replies(key);
    state.current_attempt = new_owner.attempt_id;
    state.attachment_admitted = false;
    state.attachment_request_valid = false;
    state.attachment_reply = {};
}

void InputAttachmentCore::retry(InputAttempt old_owner, uint64_t new_attempt_id,
                                InputRecordKey key) {
    if (new_attempt_id == 0)
        throw std::invalid_argument("retry ATTEMPT_ID must be nonzero");
    replace_attempt(old_owner,
                    InputAttempt{old_owner.logical_job, new_attempt_id,
                                 old_owner.store_generation},
                    key);
}

InputPublishResult InputAttachmentCore::observe_closed_commit(
    InputAttempt owner, CStoreGuid c_store_guid, const TxBegin& begin,
    const TxCommit& commit, std::span<const uint8_t> exact_input) {
    validate_attempt(owner);
    require_generation(owner);
    const InputRecordKey key{c_store_guid, begin.tu_seq};
    validate_key(key);
    Lifecycle& state = existing_owner_for(key, owner);
    if (state.current_attempt != owner.attempt_id)
        throw std::logic_error("stale attempt cannot observe closed commit");
    if (!state.closed)
        throw std::logic_error("closed commit observed for open logical job");
    const InputPublishResult result = records_.observe_closed_job_commit(
        c_store_guid, begin, commit, exact_input);
    state.closed_commit_observed = true;
    collect_lifecycles();
    return result;
}

void InputAttachmentCore::release_closed_owner(InputAttempt owner,
                                                InputRecordKey key) {
    validate_attempt(owner);
    validate_key(key);
    Lifecycle& state = existing_owner_for(key, owner);
    if (state.current_attempt != owner.attempt_id || !state.closed)
        throw std::logic_error("only the current closed owner can be released");
    if (records_.contains(key))
        throw std::logic_error("closed owner still retains an InputRecord");
    state.closed_commit_observed = true;
    purge_replies(key);
    collect_lifecycles();
}

void InputAttachmentCore::prune_replies() {
    for (auto position = replies_.begin(); position != replies_.end();) {
        if (position->second.acknowledged)
            position = replies_.erase(position);
        else
            ++position;
    }
}

void InputAttachmentCore::purge_replies(InputRecordKey key) {
    for (auto position = replies_.begin(); position != replies_.end();) {
        if (position->first.key == key)
            position = replies_.erase(position);
        else
            ++position;
    }
}

void InputAttachmentCore::collect_lifecycles() {
    for (auto position = lifecycles_.begin(); position != lifecycles_.end();) {
        const Lifecycle& state = position->second;
        const bool has_replies = std::any_of(
            replies_.begin(), replies_.end(), [&](const auto& reply) {
                return reply.first.key == position->first;
            });
        if (state.closed && state.closed_commit_observed &&
            !records_.contains(position->first) && !has_replies)
            position = lifecycles_.erase(position);
        else
            ++position;
    }
}

void InputAttachmentCore::collect_garbage() {
    records_.collect_garbage();
    for (auto position = replies_.begin(); position != replies_.end();) {
        if (!records_.contains(position->first.key) &&
            lifecycles_.contains(position->first.key) &&
            lifecycles_.at(position->first.key).closed)
            position = replies_.erase(position);
        else
            ++position;
    }
    collect_lifecycles();
}

size_t InputAttachmentCore::pending_ready_count() const {
    size_t count = 0;
    for (const auto& position : lifecycles_)
        if (position.second.ready_pending) ++count;
    return count;
}

void InputAttachmentCore::clear() {
    if (store_generation_ == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("attachment store generation exhausted");
    records_.clear();
    lifecycles_.clear();
    replies_.clear();
    ++store_generation_;
    next_event_id_ = 1;
}

}  // namespace icecc::p50
