#pragma once

#include "protocol50.h"

#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace icecc::p50 {

enum class ActionType : uint8_t {
    SESSION_OPENED,
    SESSION_REPLACED,
    SESSION_DISCONNECTED,
    HISTORY_RESET,
    TX_BEGIN,
    TX_ABORTED,
    DICT_COMPLETE,
    BODY_COMPLETE,
    NEED_RECORDED,
    OBJECT_APPLIED,
    INPUT_MATERIALIZED,
    INPUT_COMMITTED,
    COMMIT_ACCEPTED,
    ACTIVE_REPLAYED,
    LOST_COMMIT_ACCEPTED,
    F_STORE_INCAR_REPLACED,
};

std::string_view action_name(ActionType action);

enum class ActorSide : uint8_t { C, F };
std::string_view actor_name(ActorSide actor);

struct ActionRecord {
    ActionType action = ActionType::SESSION_OPENED;
    ActorSide actor = ActorSide::F;
    CStoreGuid c_store_guid{};
    FStoreGuid f_store_guid{};
    FStoreGuid previous_f_store_guid{};
    uint64_t session_serial = 0;
    HistoryNonce history_nonce{};
    RelSeq rel_seq{};
    TuSeq tu_seq{};
    Digest128 transaction_digest{};
    Digest128 raw_digest{};
    Digest128 state_digest{};
    std::optional<Key64> key;
    Digest128 content_digest{};
    std::vector<Key64> need_keys;
    uint64_t remaining_need = 0;
    bool duplicate = false;
};

class ActionTrace {
public:
    explicit ActionTrace(size_t max_records = std::numeric_limits<size_t>::max())
        : max_records_(max_records) {}

    void record(ActionRecord record) noexcept {
        if (!valid_)
            return;
        if (records_.size() >= max_records_) {
            valid_ = false;
            return;
        }
        try {
            records_.push_back(std::move(record));
        } catch (...) {
            valid_ = false;
        }
    }
    [[nodiscard]] const std::vector<ActionRecord>& records() const { return records_; }
    [[nodiscard]] bool valid() const { return valid_; }
    void clear() {
        records_.clear();
        valid_ = true;
    }

private:
    size_t max_records_ = std::numeric_limits<size_t>::max();
    bool valid_ = true;
    std::vector<ActionRecord> records_;
};

std::string action_jsonl(const ActionRecord& record);
void write_action_trace(const ActionTrace& trace, const std::string& path);
std::optional<std::string> check_action_trace(std::span<const ActionRecord> records);

}  // namespace icecc::p50
