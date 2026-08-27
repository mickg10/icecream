#pragma once

#include "protocol50.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace icecc::p50 {

class InputAttachmentCore;

// Input identity is deliberately independent of compiler ATTEMPT_ID. A
// replacement compiler on the same F attaches to the same exact input without
// creating another cache transaction or advancing route history again.
struct InputRecordKey {
    CStoreGuid c_store_guid{};
    TuSeq tu_seq{};
    auto operator<=>(const InputRecordKey&) const = default;
};

struct InputRecordKeyHash {
    size_t operator()(const InputRecordKey& key) const noexcept;
};

// One compiler attempt owns one logical cursor. Cursors share immutable bytes
// but never a mutable file/read offset. This is the in-memory capability form;
// a later file-backed implementation must preserve the same per-attempt cursor
// semantics (for example with pread(), not dup()-shared offsets).
class InputCursor {
public:
    InputCursor() = default;
    InputCursor(InputCursor&&) noexcept = default;
    InputCursor& operator=(InputCursor&&) noexcept = default;
    InputCursor(const InputCursor&) = delete;
    InputCursor& operator=(const InputCursor&) = delete;

    size_t read(std::span<uint8_t> output);

    [[nodiscard]] size_t remaining() const;
    [[nodiscard]] bool eof() const { return remaining() == 0; }
    [[nodiscard]] explicit operator bool() const {
        return static_cast<bool>(backing_);
    }
    [[nodiscard]] Digest128 raw_digest() const { return raw_digest_; }

private:
    friend class InputRecordStore;
    InputCursor(std::shared_ptr<const std::vector<uint8_t>> backing,
                Digest128 raw_digest)
        : backing_(std::move(backing)), raw_digest_(raw_digest) {}

    std::shared_ptr<const std::vector<uint8_t>> backing_;
    size_t offset_ = 0;
    Digest128 raw_digest_{};
};

enum class InputPublishResult {
    Published,
    Existing,
    NotRetainedJobClosed,
};

// This store owns the logical-job retention lease for exact committed input.
// Route reconciliation remains a separate owner: closing a job/input lease must
// never erase an unresolved C/F commit witness or permit REL_SEQ reuse.
class InputRecordStore {
public:
    // Move-only, prevalidated publication authority. Exact-byte digesting,
    // immutable backing allocation, and unordered-map node allocation are
    // complete before this object reaches the store owner. It exposes no
    // mutation or publication operation; only InputRecordStore can consume
    // its preallocated node at the durability linearization point.
    class PreparedPublish {
    public:
        PreparedPublish() noexcept;
        ~PreparedPublish();
        PreparedPublish(PreparedPublish&&) noexcept;
        PreparedPublish& operator=(PreparedPublish&&) noexcept;
        PreparedPublish(const PreparedPublish&) = delete;
        PreparedPublish& operator=(const PreparedPublish&) = delete;

        [[nodiscard]] bool valid() const noexcept;
        [[nodiscard]] InputRecordKey key() const;
        [[nodiscard]] std::span<const uint8_t> exact_input() const;

    private:
        friend class InputRecordStore;
        struct State;
        explicit PreparedPublish(std::unique_ptr<State> state) noexcept;
        std::unique_ptr<State> state_;
    };

    InputRecordStore(size_t max_records, uint64_t max_retained_bytes);

    [[nodiscard]] static PreparedPublish prepare_publish(
        CStoreGuid c_store_guid, const TxBegin& begin,
        const TxCommit& commit, std::vector<uint8_t> exact_input);

    // Allocation-free owner transition for an open logical job. The map's
    // bucket arena was reserved at store construction, and the node was
    // allocated by prepare_publish().
    InputPublishResult commit_prepared(PreparedPublish prepared);

    // Closed-job counterpart: validate against a retained exact record if one
    // exists, otherwise consume and discard the prepared node without
    // recreating compiler-visible authority.
    InputPublishResult observe_closed_job_commit(PreparedPublish prepared);

    // For an open logical job, call this before making the corresponding route
    // commit/TX_COMMIT visible. Failure leaves this store unchanged. An exact
    // retained duplicate is accepted only while its logical-job lease remains
    // open; naming an already-closed record through this API is a stale owner
    // decision and fails closed. Closed-job route completions must use
    // observe_closed_job_commit(). The caller commits route state only after
    // this call succeeds.
    InputPublishResult publish(CStoreGuid c_store_guid,
                               const TxBegin& begin,
                               const TxCommit& commit,
                               std::vector<uint8_t> exact_input);

    // A losing route may finish after RESULT_ACCEPTED or definitive job
    // cancellation. The route owner must still validate/commit its exact cache
    // transaction, but it must not recreate a compiler-visible lease. Call this
    // transition when the serialized logical-job state is already closed.
    //
    // If the same exact record is still retained, it is closed idempotently and
    // any already-authorized cursor remains valid. If no record exists, the
    // exact bytes are validated but not retained. This store deliberately keeps
    // no tombstone: the logical-job owner must select this transition for every
    // late commit after closure.
    InputPublishResult observe_closed_job_commit(
        CStoreGuid c_store_guid,
        const TxBegin& begin,
        const TxCommit& commit,
        std::span<const uint8_t> exact_input);

    // Every attachment begins at byte zero and receives an independent cursor.
    // New attachments are rejected after the logical job closes, while already
    // attached cursors continue to own and consume their immutable input.
    [[nodiscard]] InputCursor attach(InputRecordKey key) const;

    // Idempotently close the logical-job lease after one result is accepted or
    // the logical job is definitively cancelled. This does not invalidate any
    // cursor already handed to an authorized compiler attempt. Callers that
    // close before an InputRecord exists retain that state in the logical-job
    // owner and use observe_closed_job_commit() if a route commits later.
    void close_job(InputRecordKey key);

    // Release closed records only after every outstanding cursor is gone.
    void collect_garbage();

    // Drop the store-owned lease for every record during an explicit F-store
    // incarnation replacement. Already-authorized cursors keep their shared
    // immutable bytes and remain readable.
    void clear();

    [[nodiscard]] bool contains(InputRecordKey key) const;
    [[nodiscard]] bool job_open(InputRecordKey key) const;
    [[nodiscard]] size_t record_count() const { return records_.size(); }
    [[nodiscard]] uint64_t retained_bytes() const { return retained_bytes_; }
    [[nodiscard]] size_t max_records() const { return max_records_; }
    [[nodiscard]] uint64_t max_retained_bytes() const {
        return max_retained_bytes_;
    }

private:
    friend class InputAttachmentCore;

    // Roll back the one record just published by a transactional caller
    // before any cursor can escape.  This is intentionally private to the
    // attachment core rather than a general deletion API.
    void rollback_new_record(InputRecordKey key);

    struct Entry {
        uint64_t raw_bytes = 0;
        Digest128 raw_digest{};
        TxBegin begin{};
        TxCommit commit{};
        std::shared_ptr<const std::vector<uint8_t>> backing;
        bool logical_job_open = true;
    };
    using Records =
        std::unordered_map<InputRecordKey, Entry, InputRecordKeyHash>;

    static void validate_commit(const TxBegin& begin,
                                const TxCommit& commit,
                                std::span<const uint8_t> exact_input);
    static void validate_existing(const Entry& entry,
                                  const TxBegin& begin,
                                  const TxCommit& commit,
                                  std::span<const uint8_t> exact_input);

    size_t max_records_ = 0;
    uint64_t max_retained_bytes_ = 0;
    uint64_t retained_bytes_ = 0;
    Records records_;
};

}  // namespace icecc::p50
