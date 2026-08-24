#pragma once

#include "p50_input_record.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace icecc::p50 {

// ATTEMPT_ID and store_generation are deliberately ownership/observation
// identities. Neither is part of InputRecordKey, and therefore a replacement
// attempt reuses the exact committed input rather than publishing a second
// cache object. clear() advances store_generation to fence stale callbacks.
struct InputAttempt {
    uint64_t logical_job = 0;
    uint64_t attempt_id = 0;
    // F-store/session incarnation.  This is deliberately separate from the
    // retained cache key and fences callbacks that predate clear()/restart.
    uint64_t store_generation = 1;
    auto operator<=>(const InputAttempt&) const = default;
};

struct InputAttachmentRequest {
    InputRecordKey key{};
    InputAttempt attempt{};
    uint64_t request_id = 0;
    auto operator<=>(const InputAttachmentRequest&) const = default;
};

struct InputAttachmentRequestHash {
    size_t operator()(const InputAttachmentRequest& request) const noexcept;
};

enum class InputAttachmentStatus {
    Ready,
    ReadyReplay,
    Closed,
    Unknown,
    StaleAttempt,
};

// A reply is a capability observation.  All identity fields are echoed so a
// caller cannot accidentally acknowledge a reply for another GUID/TU/attempt.
// event_id remains stable across retries and replacement attempts.
struct InputAttachmentReply {
    InputAttachmentRequest request{};
    uint64_t event_id = 0;
    uint64_t raw_bytes = 0;
    Digest128 raw_digest{};
    InputAttachmentStatus status = InputAttachmentStatus::Unknown;
    bool acknowledged = false;
    auto operator<=>(const InputAttachmentReply&) const = default;
};

enum class InputAttachmentAckResult {
    Accepted,
    AlreadyAccepted,
    Rejected,
};

// Pure in-memory M3 attachment core.  It owns the logical-job lease and the
// bounded ready-notification table while InputRecordStore remains the owner of
// exact bytes and independent cursors.  Networking and compiler process
// lifetime are intentionally outside this class.
class InputAttachmentCore {
public:
    InputAttachmentCore(size_t max_records, uint64_t max_retained_bytes,
                        size_t max_pending_ready,
                        size_t max_replay_entries = 0,
                        size_t max_owner_entries = 0);

    // Commit an Open logical job.  The first exact commit publishes exactly
    // one ready event.  An exact duplicate is idempotent and publishes no
    // second event.  If the owner was cancelled before commit, the bytes are
    // validated through the closed-job path and no ready event is retained.
    InputPublishResult commit_open(
        InputAttempt owner, CStoreGuid c_store_guid, const TxBegin& begin,
        const TxCommit& commit, std::vector<uint8_t> exact_input);

    // Request/reply is identity-bound and replay-safe. Repeating an exact
    // request returns the same reply. A replacement attempt receives
    // ReadyReplay for the same event after an earlier attempt acknowledged it;
    // event metadata is never fabricated.
    [[nodiscard]] InputAttachmentReply request(
        const InputAttachmentRequest& request);

    // Acknowledgement must echo the complete request and reply identity.
    // It is idempotent for an exact replay and rejects stale/wrong identities.
    InputAttachmentAckResult acknowledge(
        const InputAttachmentRequest& request,
        const InputAttachmentReply& reply);

    // Attach only after a current-owner Ready reply. The returned cursor is
    // independent and starts at byte zero. Each logical attempt can admit one
    // cursor; acknowledgement is not required, but the reply must still be an
    // exact identity match. The canonical request remains valid after replay
    // pruning.
    [[nodiscard]] InputCursor attach(const InputAttachmentRequest& request,
                                     const InputAttachmentReply& reply) const;

    // Only the current attempt may cancel.  Cancellation closes the logical
    // job, removes an unobserved ready event, and preserves already-issued
    // cursors until they release.  It is idempotent for the current owner.
    void cancel(InputAttempt owner, InputRecordKey key);

    // Move ownership to a replacement attempt without changing cache identity
    // or emitting another ready event.  The old attempt cannot cancel, attach,
    // or acknowledge after this call.
    void replace_attempt(InputAttempt old_owner, InputAttempt new_owner,
                         InputRecordKey key);

    // Convenience form for a retry in the same logical job.  It is exactly
    // replacement with a caller-selected new ATTEMPT_ID.
    void retry(InputAttempt old_owner, uint64_t new_attempt_id,
               InputRecordKey key);

    // Observe a route commit after cancellation.  This validates exact bytes
    // but never creates a compiler-visible ready event.
    InputPublishResult observe_closed_commit(
        InputAttempt owner, CStoreGuid c_store_guid, const TxBegin& begin,
        const TxCommit& commit, std::span<const uint8_t> exact_input);

    void collect_garbage();
    void clear();

    // A closed owner may be forgotten only after its route owner guarantees
    // that no late closed-job commit can still arrive.  Until then the small
    // tombstone remains so observe_closed_commit() stays valid.
    void release_closed_owner(InputAttempt owner, InputRecordKey key);

    [[nodiscard]] size_t pending_ready_count() const;
    [[nodiscard]] size_t replay_entry_count() const { return replies_.size(); }
    [[nodiscard]] size_t max_pending_ready() const {
        return max_pending_ready_;
    }
    [[nodiscard]] bool contains(InputRecordKey key) const {
        return records_.contains(key);
    }
    [[nodiscard]] bool job_open(InputRecordKey key) const {
        return records_.job_open(key);
    }
    [[nodiscard]] size_t record_count() const { return records_.record_count(); }
    [[nodiscard]] uint64_t retained_bytes() const {
        return records_.retained_bytes();
    }
    [[nodiscard]] size_t owner_count() const { return lifecycles_.size(); }
    [[nodiscard]] size_t max_owner_entries() const {
        return max_owner_entries_;
    }
    [[nodiscard]] uint64_t store_generation() const {
        return store_generation_;
    }

#ifdef P50_ATTACHMENT_TEST_SEAMS
    void test_set_next_event_id(uint64_t value) { next_event_id_ = value; }
    void test_fail_next_lifecycle_insert() { fail_lifecycle_insert_ = true; }
    void test_fail_next_reply_insert() { fail_reply_insert_ = true; }
#endif

private:
    struct ReadyEvent {
        InputRecordKey key{};
        uint64_t event_id = 0;
        uint64_t raw_bytes = 0;
        Digest128 raw_digest{};
    };

    struct Lifecycle {
        uint64_t logical_job = 0;
        uint64_t current_attempt = 0;
        uint64_t store_generation = 0;
        bool closed = false;
        bool closed_commit_observed = false;
        bool ready_pending = false;
        std::optional<ReadyEvent> ready;
        mutable bool attachment_admitted = false;
        bool attachment_request_valid = false;
        InputAttachmentRequest attachment_request{};
        InputAttachmentReply attachment_reply{};
    };

    struct StoredReply {
        InputAttachmentReply reply{};
        bool acknowledged = false;
    };

    static void validate_attempt(InputAttempt owner);
    static void validate_request(const InputAttachmentRequest& request);
    static void validate_key(InputRecordKey key);
    Lifecycle& owner_for(InputRecordKey key, InputAttempt owner);
    Lifecycle& existing_owner_for(InputRecordKey key, InputAttempt owner);
    const Lifecycle& owner_for(InputRecordKey key,
                               InputAttempt owner) const;
    void require_current(InputRecordKey key, InputAttempt owner) const;
    void require_generation(InputAttempt owner) const;
    [[nodiscard]] InputAttachmentReply make_reply(
        const InputAttachmentRequest& request, const ReadyEvent& event,
        InputAttachmentStatus status, bool acknowledged) const;
    void prune_replies();
    void purge_replies(InputRecordKey key);
    void collect_lifecycles();
    [[nodiscard]] bool can_admit_ready() const;

    InputRecordStore records_;
    size_t max_pending_ready_ = 0;
    size_t max_replay_entries_ = 0;
    size_t max_owner_entries_ = 0;
    uint64_t store_generation_ = 1;
    uint64_t next_event_id_ = 1;
    std::unordered_map<InputRecordKey, Lifecycle, InputRecordKeyHash>
        lifecycles_;
    std::unordered_map<InputAttachmentRequest, StoredReply,
                       InputAttachmentRequestHash>
        replies_;
#ifdef P50_ATTACHMENT_TEST_SEAMS
    bool fail_lifecycle_insert_ = false;
    bool fail_reply_insert_ = false;
#endif
};

}  // namespace icecc::p50
