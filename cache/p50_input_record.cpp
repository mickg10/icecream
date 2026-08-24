#include "p50_input_record.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <stdexcept>
#include <utility>

namespace icecc::p50 {

size_t InputRecordKeyHash::operator()(const InputRecordKey& key) const noexcept {
    const size_t guid_hash = Id128Hash{}(key.c_store_guid);
    const size_t tu_hash = std::hash<uint64_t>{}(key.tu_seq.value);
    return guid_hash ^ (tu_hash + size_t{0x9e3779b9} +
                        (guid_hash << 6) + (guid_hash >> 2));
}

size_t InputCursor::remaining() const {
    if (!backing_) return 0;
    if (offset_ > backing_->size())
        throw std::logic_error("InputCursor offset exceeded immutable input");
    return backing_->size() - offset_;
}

size_t InputCursor::read(std::span<uint8_t> output) {
    if (!backing_)
        throw std::logic_error("read from an empty or moved-from InputCursor");
    const size_t count = std::min(output.size(), remaining());
    if (count != 0)
        std::copy_n(backing_->begin() + offset_, count, output.begin());
    offset_ += count;
    return count;
}

InputRecordStore::InputRecordStore(size_t max_records,
                                   uint64_t max_retained_bytes)
    : max_records_(max_records),
      max_retained_bytes_(max_retained_bytes) {
    if (max_records_ == 0 || max_retained_bytes_ == 0)
        throw std::invalid_argument(
            "InputRecordStore record and byte limits must be nonzero");
}

void InputRecordStore::validate_commit(
    const TxBegin& begin, const TxCommit& commit,
    std::span<const uint8_t> exact_input) {
    if (begin.rel_seq.value == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("InputRecord cannot publish terminal REL_SEQ");
    if (commit.history_nonce != begin.history_nonce ||
        commit.rel_seq != begin.rel_seq ||
        commit.tu_seq != begin.tu_seq ||
        commit.transaction_digest != begin.transaction_digest ||
        commit.raw_digest != begin.raw_digest)
        throw std::invalid_argument(
            "InputRecord commit identity differs from TX_BEGIN");

    const Digest128 expected_post = compute_post_state_digest(
        begin.pre_state_digest, begin.history_nonce, begin.rel_seq,
        begin.tu_seq, begin.transaction_digest);
    if (commit.post_state_digest != expected_post)
        throw std::invalid_argument(
            "InputRecord commit has the wrong post-state digest");

    if (begin.raw_bytes > std::numeric_limits<size_t>::max() ||
        exact_input.size() != static_cast<size_t>(begin.raw_bytes))
        throw std::invalid_argument(
            "InputRecord exact input length differs from TX_BEGIN");
    if (icecc::digest128(exact_input) != begin.raw_digest)
        throw std::invalid_argument(
            "InputRecord exact input digest differs from TX_BEGIN");
}

void InputRecordStore::validate_existing(
    const Entry& entry, const TxBegin& begin, const TxCommit& commit,
    std::span<const uint8_t> exact_input) {
    if (entry.begin != begin || entry.commit != commit ||
        entry.raw_bytes != begin.raw_bytes ||
        entry.raw_digest != begin.raw_digest ||
        !entry.backing ||
        entry.backing->size() != exact_input.size() ||
        !std::equal(entry.backing->begin(), entry.backing->end(),
                    exact_input.begin()))
        throw std::logic_error(
            "one InputRecord key was assigned conflicting exact input");
}

InputPublishResult InputRecordStore::publish(
    CStoreGuid c_store_guid, const TxBegin& begin, const TxCommit& commit,
    std::vector<uint8_t> exact_input) {
    if (c_store_guid == CStoreGuid{})
        throw std::invalid_argument("InputRecord C_STORE_GUID zero is reserved");

    validate_commit(begin, commit, exact_input);
    const InputRecordKey key{c_store_guid, begin.tu_seq};
    const auto existing = records_.find(key);
    if (existing != records_.end()) {
        validate_existing(existing->second, begin, commit, exact_input);
        if (!existing->second.logical_job_open)
            throw std::logic_error(
                "open logical job named an already-closed InputRecord");
        return InputPublishResult::Existing;
    }

    if (records_.size() >= max_records_)
        throw std::length_error("InputRecordStore record limit exceeded");
    if (begin.raw_bytes > max_retained_bytes_ - retained_bytes_)
        throw std::length_error("InputRecordStore byte limit exceeded");

    auto mutable_backing =
        std::make_shared<std::vector<uint8_t>>(std::move(exact_input));
    std::shared_ptr<const std::vector<uint8_t>> backing =
        std::move(mutable_backing);
    Entry entry{begin.raw_bytes, begin.raw_digest, begin, commit,
                std::move(backing), true};
    const auto [position, inserted] = records_.emplace(key, std::move(entry));
    (void)position;
    if (!inserted)
        throw std::logic_error("InputRecordStore insertion lost key ownership");
    retained_bytes_ += begin.raw_bytes;
    return InputPublishResult::Published;
}

InputPublishResult InputRecordStore::observe_closed_job_commit(
    CStoreGuid c_store_guid, const TxBegin& begin, const TxCommit& commit,
    std::span<const uint8_t> exact_input) {
    if (c_store_guid == CStoreGuid{})
        throw std::invalid_argument("InputRecord C_STORE_GUID zero is reserved");

    validate_commit(begin, commit, exact_input);
    const InputRecordKey key{c_store_guid, begin.tu_seq};
    const auto existing = records_.find(key);
    if (existing == records_.end())
        return InputPublishResult::NotRetainedJobClosed;

    validate_existing(existing->second, begin, commit, exact_input);
    existing->second.logical_job_open = false;
    return InputPublishResult::Existing;
}

InputCursor InputRecordStore::attach(InputRecordKey key) const {
    const auto position = records_.find(key);
    if (position == records_.end())
        throw std::out_of_range("compiler attachment named no InputRecord");
    const Entry& entry = position->second;
    if (!entry.logical_job_open)
        throw std::logic_error(
            "compiler attachment arrived after logical job closure");
    if (!entry.backing)
        throw std::logic_error("InputRecord lost its immutable backing");
    return InputCursor(entry.backing, entry.raw_digest);
}

void InputRecordStore::close_job(InputRecordKey key) {
    const auto position = records_.find(key);
    if (position == records_.end())
        throw std::out_of_range("logical job closure named no InputRecord");
    position->second.logical_job_open = false;
}

void InputRecordStore::collect_garbage() {
    for (auto position = records_.begin(); position != records_.end();) {
        const Entry& entry = position->second;
        if (!entry.logical_job_open && entry.backing &&
            entry.backing.use_count() == 1) {
            if (entry.raw_bytes > retained_bytes_)
                throw std::logic_error(
                    "InputRecordStore retained-byte accounting underflow");
            retained_bytes_ -= entry.raw_bytes;
            position = records_.erase(position);
        } else {
            ++position;
        }
    }
}

void InputRecordStore::clear() {
    records_.clear();
    retained_bytes_ = 0;
}

void InputRecordStore::rollback_new_record(InputRecordKey key) {
    const auto position = records_.find(key);
    if (position == records_.end())
        throw std::logic_error("transactional InputRecord rollback lost key");
    Entry& entry = position->second;
    if (!entry.logical_job_open || !entry.backing ||
        entry.backing.use_count() != 1)
        throw std::logic_error("transactional InputRecord rollback was not exclusive");
    if (entry.raw_bytes > retained_bytes_)
        throw std::logic_error("InputRecord rollback accounting underflow");
    retained_bytes_ -= entry.raw_bytes;
    records_.erase(position);
}

bool InputRecordStore::contains(InputRecordKey key) const {
    return records_.contains(key);
}

bool InputRecordStore::job_open(InputRecordKey key) const {
    const auto position = records_.find(key);
    return position != records_.end() && position->second.logical_job_open;
}

}  // namespace icecc::p50
