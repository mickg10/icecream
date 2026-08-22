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

enum class InputPublishResult { Published, Existing };

// This store owns the logical-job retention lease for exact committed input.
// Route reconciliation remains a separate owner: closing a job/input lease must
// never erase an unresolved C/F commit witness or permit REL_SEQ reuse.
class InputRecordStore {
public:
    InputRecordStore(size_t max_records, uint64_t max_retained_bytes);

    // For an open logical job, call this before making the corresponding route
    // commit/TX_COMMIT visible. Failure leaves this store unchanged. The caller
    // is responsible for committing route state only after this call succeeds.
    InputPublishResult publish(CStoreGuid c_store_guid,
                               const TxBegin& begin,
                               const TxCommit& commit,
                               std::vector<uint8_t> exact_input);

    // Every attachment begins at byte zero and receives an independent cursor.
    // New attachments are rejected after the logical job closes, while already
    // attached cursors continue to own and consume their immutable input.
    [[nodiscard]] InputCursor attach(InputRecordKey key) const;

    // Idempotently close the logical-job lease after one result is accepted or
    // the logical job is definitively cancelled. This does not invalidate any
    // cursor already handed to an authorized compiler attempt.
    void close_job(InputRecordKey key);

    // Release closed records only after every outstanding cursor is gone.
    void collect_garbage();

    [[nodiscard]] bool contains(InputRecordKey key) const;
    [[nodiscard]] bool job_open(InputRecordKey key) const;
    [[nodiscard]] size_t record_count() const { return records_.size(); }
    [[nodiscard]] uint64_t retained_bytes() const { return retained_bytes_; }
    [[nodiscard]] size_t max_records() const { return max_records_; }
    [[nodiscard]] uint64_t max_retained_bytes() const {
        return max_retained_bytes_;
    }

private:
    struct Entry {
        uint64_t raw_bytes = 0;
        Digest128 raw_digest{};
        std::shared_ptr<const std::vector<uint8_t>> backing;
        bool logical_job_open = true;
    };

    static void validate_commit(const TxBegin& begin,
                                const TxCommit& commit,
                                std::span<const uint8_t> exact_input);

    size_t max_records_ = 0;
    uint64_t max_retained_bytes_ = 0;
    uint64_t retained_bytes_ = 0;
    std::unordered_map<InputRecordKey, Entry, InputRecordKeyHash> records_;
};

}  // namespace icecc::p50
