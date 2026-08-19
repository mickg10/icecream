// One-pending-TU receiver ledger for the deployable P29 route.
//
// The receiver owns one instance per (C GUID, source generation, F cache epoch).  A committed
// transaction retains the exact Ack fields needed to answer a repeated delivery without
// installing objects or appending occurrence history twice.  Aborting leaves the expected
// sequence unchanged, so the identical transaction can be retried.
#ifndef P29_TRANSFER_TRANSACTION_H
#define P29_TRANSFER_TRANSACTION_H

#include <array>
#include <cstdint>
#include <optional>

namespace p29 {

using Opaque128 = std::array<uint8_t, 16>;

struct RouteScope {
    Opaque128 c_guid{};
    Opaque128 source_generation{};
    uint64_t f_cache_epoch = 0;
    bool operator==(const RouteScope& other) const {
        return c_guid == other.c_guid && source_generation == other.source_generation &&
               f_cache_epoch == other.f_cache_epoch;
    }
    bool operator!=(const RouteScope& other) const { return !(*this == other); }
};

struct TransferTxnId {
    RouteScope scope{};
    uint64_t route_sequence = 0;
    bool operator==(const TransferTxnId& other) const {
        return scope == other.scope && route_sequence == other.route_sequence;
    }
    bool operator!=(const TransferTxnId& other) const { return !(*this == other); }
};

struct Digest128 {
    uint64_t lo = 0, hi = 0;
    bool operator==(const Digest128& other) const { return lo == other.lo && hi == other.hi; }
    bool operator!=(const Digest128& other) const { return !(*this == other); }
};

struct AckReceipt {
    TransferTxnId id{};
    Digest128 transaction_digest{};
    Digest128 output_digest{};
    uint64_t output_extent = 0;
    bool operator==(const AckReceipt& other) const {
        return id == other.id && transaction_digest == other.transaction_digest &&
               output_digest == other.output_digest && output_extent == other.output_extent;
    }
};

enum class BeginResult {
    Ready,
    DuplicateCommitted,
    PendingDuplicate,
    Busy,
    ScopeMismatch,
    SequenceMismatch,
    DigestMismatch,
    SequenceExhausted
};

class ReceiverTxnLedger {
public:
    struct StateMark {
        uint64_t expected_sequence = 0;
        bool pending = false;
        uint64_t pending_sequence = 0;
        Digest128 pending_transaction_digest{};
        bool retained_ack = false;
        uint64_t retained_sequence = 0;
        Digest128 retained_transaction_digest{};
        Digest128 retained_output_digest{};
        uint64_t retained_output_extent = 0;
        bool operator==(const StateMark& other) const {
            return expected_sequence == other.expected_sequence && pending == other.pending &&
                   pending_sequence == other.pending_sequence &&
                   pending_transaction_digest == other.pending_transaction_digest &&
                   retained_ack == other.retained_ack &&
                   retained_sequence == other.retained_sequence &&
                   retained_transaction_digest == other.retained_transaction_digest &&
                   retained_output_digest == other.retained_output_digest &&
                   retained_output_extent == other.retained_output_extent;
        }
    };

    explicit ReceiverTxnLedger(RouteScope scope, uint64_t expected_sequence = 0)
        : scope_(scope), expected_sequence_(expected_sequence) {}

    BeginResult begin(const TransferTxnId& id, Digest128 transaction_digest,
                      AckReceipt* duplicate_ack = nullptr) {
        if (id.scope != scope_) return BeginResult::ScopeMismatch;
        if (pending_) {
            if (id != pending_->id) return BeginResult::Busy;
            return transaction_digest == pending_->transaction_digest
                       ? BeginResult::PendingDuplicate
                       : BeginResult::DigestMismatch;
        }
        if (id.route_sequence < expected_sequence_) {
            if (last_ack_ && id == last_ack_->id) {
                if (transaction_digest != last_ack_->transaction_digest)
                    return BeginResult::DigestMismatch;
                if (duplicate_ack) *duplicate_ack = *last_ack_;
                return BeginResult::DuplicateCommitted;
            }
            return BeginResult::SequenceMismatch;
        }
        if (id.route_sequence != expected_sequence_)
            return BeginResult::SequenceMismatch;
        if (id.route_sequence == UINT64_MAX)
            return BeginResult::SequenceExhausted;
        pending_ = Pending{id, transaction_digest};
        return BeginResult::Ready;
    }

    bool commit(Digest128 output_digest, uint64_t output_extent, AckReceipt& ack) {
        if (!pending_) return false;
        ack = {pending_->id, pending_->transaction_digest, output_digest, output_extent};
        last_ack_ = ack;
        expected_sequence_ = pending_->id.route_sequence + 1;
        pending_.reset();
        return true;
    }

    bool abort() {
        if (!pending_) return false;
        pending_.reset();
        return true;
    }

    bool has_pending() const { return pending_.has_value(); }
    uint64_t expected_sequence() const { return expected_sequence_; }
    const std::optional<AckReceipt>& retained_ack() const { return last_ack_; }
    StateMark state_mark() const {
        StateMark mark;
        mark.expected_sequence = expected_sequence_;
        mark.pending = pending_.has_value();
        if (pending_) {
            mark.pending_sequence = pending_->id.route_sequence;
            mark.pending_transaction_digest = pending_->transaction_digest;
        }
        mark.retained_ack = last_ack_.has_value();
        if (last_ack_) {
            mark.retained_sequence = last_ack_->id.route_sequence;
            mark.retained_transaction_digest = last_ack_->transaction_digest;
            mark.retained_output_digest = last_ack_->output_digest;
            mark.retained_output_extent = last_ack_->output_extent;
        }
        return mark;
    }

private:
    struct Pending { TransferTxnId id; Digest128 transaction_digest; };
    RouteScope scope_{};
    uint64_t expected_sequence_ = 0;
    std::optional<Pending> pending_;
    std::optional<AckReceipt> last_ack_;
};

}  // namespace p29

#endif
