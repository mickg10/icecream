#include "p50_actions.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace icecc::p50 {
namespace {

std::string bytes_hex(std::span<const uint8_t> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(bytes.size() * 2, '0');
    for (size_t i = 0; i != bytes.size(); ++i) {
        result[i * 2] = digits[bytes[i] >> 4];
        result[i * 2 + 1] = digits[bytes[i] & 0x0f];
    }
    return result;
}

struct TxIdentity {
    HistoryNonce nonce{};
    RelSeq rel{};
    TuSeq tu{};
    Digest128 transaction_digest{};
    Digest128 raw_digest{};
    auto operator<=>(const TxIdentity&) const = default;
};

TxIdentity tx_identity(const ActionRecord& record) {
    return {record.history_nonce, record.rel_seq, record.tu_seq,
            record.transaction_digest, record.raw_digest};
}

struct CCheckerState {
    bool cursor_known = false;
    HistoryNonce nonce{};
    RelSeq next_rel{};
    Digest128 state_digest{};
    std::optional<TxIdentity> active;
};

struct FCheckerState {
    bool connected = false;
    uint64_t session_serial = 0;
    bool route = false;
    HistoryNonce nonce{};
    RelSeq next_rel{};
    Digest128 state_digest{};
    std::optional<TxIdentity> pending;
    bool dict_complete = false;
    bool body_complete = false;
    bool need_recorded = false;
    std::set<Key64> requested;
    std::set<Key64> remaining;
    bool materialized = false;
    std::optional<TxIdentity> last_commit;
    bool commit_unacknowledged = false;
    bool commit_disconnected = false;
    std::map<Key64, Digest128> installed;
};

struct RelationshipCheckerState {
    CCheckerState c;
    FCheckerState f;
};

void clear_f_pending(FCheckerState& state) {
    state.pending.reset();
    state.dict_complete = false;
    state.body_complete = false;
    state.need_recorded = false;
    state.requested.clear();
    state.remaining.clear();
    state.materialized = false;
}

}  // namespace

std::string_view action_name(ActionType action) {
    switch (action) {
    case ActionType::SESSION_OPENED: return "SESSION_OPENED";
    case ActionType::SESSION_REPLACED: return "SESSION_REPLACED";
    case ActionType::SESSION_DISCONNECTED: return "SESSION_DISCONNECTED";
    case ActionType::HISTORY_RESET: return "HISTORY_RESET";
    case ActionType::TX_BEGIN: return "TX_BEGIN";
    case ActionType::TX_ABORTED: return "TX_ABORTED";
    case ActionType::DICT_COMPLETE: return "DICT_COMPLETE";
    case ActionType::BODY_COMPLETE: return "BODY_COMPLETE";
    case ActionType::NEED_RECORDED: return "NEED_RECORDED";
    case ActionType::OBJECT_APPLIED: return "OBJECT_APPLIED";
    case ActionType::INPUT_MATERIALIZED: return "INPUT_MATERIALIZED";
    case ActionType::INPUT_COMMITTED: return "INPUT_COMMITTED";
    case ActionType::COMMIT_ACCEPTED: return "COMMIT_ACCEPTED";
    case ActionType::ACTIVE_REPLAYED: return "ACTIVE_REPLAYED";
    case ActionType::LOST_COMMIT_ACCEPTED: return "LOST_COMMIT_ACCEPTED";
    }
    throw std::logic_error("unknown Protocol-50 action");
}

std::string_view actor_name(ActorSide actor) {
    switch (actor) {
    case ActorSide::C: return "C";
    case ActorSide::F: return "F";
    }
    throw std::logic_error("unknown Protocol-50 actor");
}

std::string action_jsonl(const ActionRecord& record) {
    std::ostringstream out;
    out << "{\"action\":\"" << action_name(record.action)
        << "\",\"actor\":\"" << actor_name(record.actor)
        << "\",\"c_store_guid\":\"" << bytes_hex(record.c_store_guid.bytes)
        << "\",\"f_store_guid\":\"" << bytes_hex(record.f_store_guid.bytes)
        << "\",\"session_serial\":" << record.session_serial
        << ",\"history_nonce\":" << record.history_nonce.value
        << ",\"rel_seq\":" << record.rel_seq.value
        << ",\"tu_seq\":" << record.tu_seq.value
        << ",\"transaction_digest\":\""
        << digest128_hex(record.transaction_digest)
        << "\",\"raw_digest\":\"" << digest128_hex(record.raw_digest)
        << "\",\"state_digest\":\"" << digest128_hex(record.state_digest)
        << "\",\"content_digest\":\"" << digest128_hex(record.content_digest)
        << "\",\"key64\":";
    if (record.key) out << record.key->wire_value();
    else out << "null";
    out << ",\"need_keys\":[";
    for (size_t index = 0; index != record.need_keys.size(); ++index) {
        if (index) out << ',';
        out << record.need_keys[index].wire_value();
    }
    out << "],\"remaining_need\":" << record.remaining_need
        << ",\"duplicate\":" << (record.duplicate ? "true" : "false") << '}';
    return out.str();
}

void write_action_trace(const ActionTrace& trace, const std::string& path) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot open action trace output");
    for (const ActionRecord& record : trace.records())
        output << action_jsonl(record) << '\n';
    if (!output) throw std::runtime_error("cannot write action trace output");
}

std::optional<std::string> check_action_trace(std::span<const ActionRecord> records) {
    using RelationshipKey = std::pair<CStoreGuid, FStoreGuid>;
    std::map<RelationshipKey, RelationshipCheckerState> relationships;
    for (size_t index = 0; index != records.size(); ++index) {
        const ActionRecord& record = records[index];
        RelationshipCheckerState& relationship =
            relationships[{record.c_store_guid, record.f_store_guid}];
        CCheckerState& c = relationship.c;
        FCheckerState& f = relationship.f;
        const auto error = [&](std::string_view detail) -> std::optional<std::string> {
            return "action " + std::to_string(index) + " (" +
                   std::string(actor_name(record.actor)) + "/" +
                   std::string(action_name(record.action)) + "): " +
                   std::string(detail);
        };
        switch (record.action) {
        case ActionType::SESSION_OPENED:
        case ActionType::SESSION_REPLACED:
            if (record.actor != ActorSide::F)
                return error("session transition was not emitted by F");
            if (record.session_serial == 0) return error("session serial is zero");
            if (f.connected != (record.action == ActionType::SESSION_REPLACED))
                return error("open/replace does not match the current F session");
            if (record.action == ActionType::SESSION_REPLACED &&
                f.commit_unacknowledged)
                f.commit_disconnected = true;
            f.connected = true;
            f.session_serial = record.session_serial;
            clear_f_pending(f);
            break;
        case ActionType::SESSION_DISCONNECTED:
            if (record.actor != ActorSide::F || !f.connected ||
                record.session_serial != f.session_serial)
                return error("disconnect does not name the current session");
            f.connected = false;
            if (f.commit_unacknowledged) f.commit_disconnected = true;
            clear_f_pending(f);
            break;
        case ActionType::HISTORY_RESET:
            if (record.actor != ActorSide::F || !f.connected)
                return error("history reset without a current F session");
            if (f.pending) return error("history reset while F has a pending transaction");
            if (c.active) return error("history reset while C has an active transaction");
            if (f.commit_unacknowledged)
                return error("history reset skipped an unacknowledged durable commit");
            if (f.route && record.history_nonce == f.nonce)
                return error("history reset reused its HISTORY_NONCE");
            f.route = true;
            f.nonce = record.history_nonce;
            f.next_rel = RelSeq{0};
            f.state_digest = record.state_digest;
            f.last_commit.reset();
            f.commit_unacknowledged = false;
            f.commit_disconnected = false;
            break;
        case ActionType::TX_BEGIN: {
            const TxIdentity identity = tx_identity(record);
            if (record.actor == ActorSide::C) {
                if (c.active) return error("C opened a second active transaction");
                if (!c.cursor_known || record.history_nonce != c.nonce) {
                    if (record.rel_seq.value != 0)
                        return error("new C history did not start at REL_SEQ zero");
                    c.cursor_known = true;
                    c.nonce = record.history_nonce;
                    c.next_rel = record.rel_seq;
                    c.state_digest = record.state_digest;
                } else if (record.rel_seq != c.next_rel ||
                           record.state_digest != c.state_digest) {
                    return error("C transaction does not match its route cursor");
                }
                c.active = identity;
                break;
            }
            if (!f.connected || !f.route)
                return error("F accepted TX_BEGIN without a current route");
            if (f.pending) return error("F accepted a second pending transaction");
            if (!c.active || *c.active != identity)
                return error("F TX_BEGIN does not name C's active transaction");
            if (record.history_nonce != f.nonce || record.rel_seq != f.next_rel ||
                record.state_digest != f.state_digest)
                return error("F TX_BEGIN does not match its route cursor");
            clear_f_pending(f);
            f.pending = identity;
            break;
        }
        case ActionType::TX_ABORTED:
            if (record.actor != ActorSide::C || !c.active ||
                *c.active != tx_identity(record))
                return error("abort does not name C's active transaction");
            c.active.reset();
            break;
        case ActionType::DICT_COMPLETE:
            if (record.actor != ActorSide::F || !f.pending ||
                *f.pending != tx_identity(record) || f.dict_complete)
                return error("DICT completion does not name F's pending transaction");
            f.dict_complete = true;
            break;
        case ActionType::BODY_COMPLETE:
            if (record.actor != ActorSide::F || !f.pending ||
                *f.pending != tx_identity(record) || f.body_complete)
                return error("BODY completion does not name F's pending transaction");
            f.body_complete = true;
            break;
        case ActionType::NEED_RECORDED:
            if (record.actor != ActorSide::F || !f.pending ||
                *f.pending != tx_identity(record) || !f.dict_complete ||
                f.need_recorded)
                return error("NEED was recorded before the exact DICT");
            if (!std::is_sorted(record.need_keys.begin(), record.need_keys.end()) ||
                std::adjacent_find(record.need_keys.begin(), record.need_keys.end()) !=
                    record.need_keys.end() ||
                record.remaining_need != record.need_keys.size())
                return error("recorded Need is not one exact sorted set");
            f.need_recorded = true;
            f.requested = {record.need_keys.begin(), record.need_keys.end()};
            f.remaining = f.requested;
            break;
        case ActionType::OBJECT_APPLIED:
            if (record.actor != ActorSide::F || !f.pending ||
                *f.pending != tx_identity(record) || !f.need_recorded || !record.key ||
                !f.requested.contains(*record.key))
                return error("object application is outside the active Need");
            if (const auto installed = f.installed.find(*record.key);
                installed != f.installed.end()) {
                if (installed->second != record.content_digest)
                    return error("Key64 was applied with different immutable content");
            } else {
                f.installed.emplace(*record.key, record.content_digest);
            }
            if (f.remaining.erase(*record.key) != 0) {
                if (record.duplicate)
                    return error("first application of a missing object was marked duplicate");
            } else if (!record.duplicate) {
                return error("repeat object application was not marked duplicate");
            }
            if (record.remaining_need != f.remaining.size())
                return error("object application changed the exact Need incorrectly");
            break;
        case ActionType::INPUT_MATERIALIZED:
            if (record.actor != ActorSide::F || !f.pending ||
                *f.pending != tx_identity(record) || !f.dict_complete ||
                !f.body_complete || !f.need_recorded || !f.remaining.empty() ||
                f.materialized)
                return error("input materialized before all prerequisites completed");
            f.materialized = true;
            break;
        case ActionType::INPUT_COMMITTED:
            if (record.actor != ActorSide::F || !f.pending ||
                *f.pending != tx_identity(record) || !f.materialized)
                return error("F commit does not close its exactly materialized transaction");
            if (f.next_rel.value == std::numeric_limits<uint64_t>::max())
                return error("F REL_SEQ exhausted");
            f.last_commit = *f.pending;
            f.commit_unacknowledged = true;
            f.commit_disconnected = false;
            clear_f_pending(f);
            ++f.next_rel.value;
            f.state_digest = record.state_digest;
            break;
        case ActionType::COMMIT_ACCEPTED:
        case ActionType::LOST_COMMIT_ACCEPTED:
            if (record.actor != ActorSide::C || !c.active ||
                *c.active != tx_identity(record))
                return error("C acceptance does not name its active transaction");
            if (!f.last_commit || *f.last_commit != *c.active ||
                record.state_digest != f.state_digest)
                return error("C acceptance lacks the matching durable F commit");
            if (!f.commit_unacknowledged ||
                (record.action == ActionType::COMMIT_ACCEPTED &&
                 f.commit_disconnected) ||
                (record.action == ActionType::LOST_COMMIT_ACCEPTED &&
                 !f.commit_disconnected))
                return error("C acceptance used the wrong normal/lost path");
            if (c.next_rel.value == std::numeric_limits<uint64_t>::max())
                return error("C REL_SEQ exhausted");
            c.active.reset();
            ++c.next_rel.value;
            c.state_digest = record.state_digest;
            f.commit_unacknowledged = false;
            f.commit_disconnected = false;
            break;
        case ActionType::ACTIVE_REPLAYED:
            if (record.actor != ActorSide::F || !f.connected || !f.route || f.pending ||
                !c.active || *c.active != tx_identity(record))
                return error("replay does not name C's retained active transaction");
            if (record.history_nonce != f.nonce || record.rel_seq != f.next_rel ||
                record.state_digest != f.state_digest)
                return error("replay does not match F's route cursor");
            clear_f_pending(f);
            f.pending = tx_identity(record);
            break;
        }
    }
    return std::nullopt;
}

}  // namespace icecc::p50
