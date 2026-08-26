#include "p50_source_ingress.h"

#include <sys/wait.h>

#include <algorithm>
#include <limits>

namespace icecc::p50::daemon {
namespace {
constexpr uint32_t kHardSourceBudgetMsec = P50SourceArmedFields::MaxSourceBudgetMsec;
}

bool SourceIngress::exact_owner(const SourceIngressLease& owner) const noexcept {
    return owner == lease_;
}

bool SourceIngress::terminal() const noexcept {
    return state_ == SourceIngressState::Cancelled ||
           state_ == SourceIngressState::Rejected ||
           state_ == SourceIngressState::FailedCpp ||
           state_ == SourceIngressState::Expired ||
           state_ == SourceIngressState::Reconcile ||
           state_ == SourceIngressState::Committed ||
           state_ == SourceIngressState::SettledPreDurable;
}

bool SourceIngress::add_budget(TimePoint now, uint32_t budget,
                               TimePoint& result) noexcept {
    if (budget == 0 || budget > kHardSourceBudgetMsec) return false;
    const auto duration = std::chrono::milliseconds(budget);
    if (now > TimePoint::max() - duration) return false;
    result = now + duration;
    return result > now;
}

bool SourceIngress::expire_if_needed(TimePoint now) noexcept {
    if (terminal() || now < deadline_ ||
        wire_state_ == CacheWireState::Committed ||
        wire_state_ == CacheWireState::SettledPreDurable)
        return false;
    clear_prepared();
    if (wire_started_) {
        reconcile();
    } else {
        revoke_finalize();
        state_ = SourceIngressState::Expired;
    }
    return true;
}

void SourceIngress::revoke_finalize() noexcept {
    if (finalize_control_) finalize_control_->active = false;
    finalize_control_.reset();
    finalized_.reset();
}

void SourceIngress::clear_prepared() noexcept {
    staged_.clear();
    prepared_ = false;
    if (!wire_started_) revoke_finalize();
}

void SourceIngress::reconcile() noexcept {
    state_ = SourceIngressState::Reconcile;
    wire_state_ = CacheWireState::Reconcile;
}

void SourceIngress::maybe_finalize() noexcept {
    if (!prepared_ && !source_finalized_ && eof_ && cpp_ok_ && !staged_.empty())
        prepared_ = true;
}

SourceIngressResult SourceIngress::append_wrapper_bytes(
    const SourceIngressLease& owner, std::span<const uint8_t> bytes,
    TimePoint now) noexcept {
    if (!exact_owner(owner)) return SourceIngressResult::WrongOwner;
    if (expire_if_needed(now)) return SourceIngressResult::DeadlineExpired;
    if (terminal() || eof_ || source_finalized_ || wire_started_)
        return SourceIngressResult::Closed;
    if (bytes.empty()) return SourceIngressResult::Accepted;
    if (staged_.size() > max_bytes_ || bytes.size() > max_bytes_ - staged_.size()) {
        clear_prepared();
        state_ = SourceIngressState::Rejected;
        return SourceIngressResult::Invalid;
    }
    try {
        staged_.insert(staged_.end(), bytes.begin(), bytes.end());
    } catch (...) {
        clear_prepared();
        state_ = SourceIngressState::Rejected;
        return SourceIngressResult::Invalid;
    }
    return SourceIngressResult::Accepted;
}

SourceIngressResult SourceIngress::observe_wrapper_eof(
    const SourceIngressLease& owner, TimePoint now) noexcept {
    if (!exact_owner(owner)) return SourceIngressResult::WrongOwner;
    if (expire_if_needed(now)) return SourceIngressResult::DeadlineExpired;
    if (terminal() || source_finalized_ || wire_started_)
        return SourceIngressResult::Closed;
    if (eof_) return SourceIngressResult::Duplicate;
    if (staged_.empty()) return SourceIngressResult::Invalid;
    eof_ = true;
    state_ = SourceIngressState::Drained;
    maybe_finalize();
    return SourceIngressResult::Accepted;
}

SourceIngressResult SourceIngress::accept_f_source_armed(
    const SourceIngressLease& owner, const P50SourceArmedMsg& ack,
    TimePoint now) noexcept {
    if (!exact_owner(owner)) return SourceIngressResult::WrongOwner;
    if (!ack.valid_payload()) return SourceIngressResult::InvalidAck;
    if (ack.arm != lease_.armed.arm) return SourceIngressResult::WrongOwner;
    if (expire_if_needed(now)) return SourceIngressResult::DeadlineExpired;
    if (terminal() || wire_started_) return SourceIngressResult::Closed;
    const auto& ack_fields = static_cast<const P50SourceArmedFields&>(ack);
    if (f_armed_) {
        return lease_.armed_wire_valid && ack_fields == lease_.armed
                   ? SourceIngressResult::Duplicate
                   : SourceIngressResult::WrongOwner;
    }
    TimePoint ack_deadline;
    if (!add_budget(now, ack.source_budget_msec, ack_deadline))
        return SourceIngressResult::InvalidAck;
    deadline_ = std::min(deadline_, ack_deadline);
    if (deadline_ <= now) {
        clear_prepared();
        state_ = SourceIngressState::Expired;
        return SourceIngressResult::DeadlineExpired;
    }
    try {
        P50SourceArmedFields accepted = ack_fields;
        lease_.armed = std::move(accepted);
        lease_.armed_wire_valid = true;
    } catch (...) {
        clear_prepared();
        state_ = SourceIngressState::Rejected;
        return SourceIngressResult::InvalidAck;
    }
    if (!lease_.f_ack_valid()) {
        clear_prepared();
        state_ = SourceIngressState::Rejected;
        return SourceIngressResult::InvalidAck;
    }
    f_armed_ = true;
    return SourceIngressResult::Accepted;
}

SourceIngressResult SourceIngress::observe_cpp_waitpid(
    const SourceIngressLease& owner, pid_t reported_pid, int status,
    TimePoint now) noexcept {
    if (!exact_owner(owner)) return SourceIngressResult::WrongOwner;
    if (reported_pid != lease_.cpp_pid) return SourceIngressResult::WrongPid;
    if (expire_if_needed(now)) return SourceIngressResult::DeadlineExpired;
    if (terminal() || source_finalized_ || wire_started_)
        return SourceIngressResult::Closed;
    if (cpp_ok_) return SourceIngressResult::Duplicate;
    if (!WIFEXITED(status) && !WIFSIGNALED(status)) {
        clear_prepared();
        state_ = SourceIngressState::FailedCpp;
        return SourceIngressResult::InvalidStatus;
    }
    if (WIFSIGNALED(status) || WEXITSTATUS(status) != 0) {
        clear_prepared();
        state_ = SourceIngressState::FailedCpp;
        return SourceIngressResult::FailedCpp;
    }
    cpp_ok_ = true;
    maybe_finalize();
    return SourceIngressResult::Accepted;
}

SourceIngressResult SourceIngress::expire(const SourceIngressLease& owner,
                                          TimePoint now) noexcept {
    if (!exact_owner(owner)) return SourceIngressResult::WrongOwner;
    if (now < deadline_) return SourceIngressResult::NotReady;
    if (terminal()) return SourceIngressResult::Closed;
    (void)expire_if_needed(now);
    return wire_started_ && wire_state_ == CacheWireState::Reconcile
               ? SourceIngressResult::ReconcileRequired
               : SourceIngressResult::DeadlineExpired;
}

std::optional<SourceFinalize> SourceIngress::take_source_finalize(
    const SourceIngressLease& owner, TimePoint now) noexcept {
    if (!exact_owner(owner) || expire_if_needed(now) || terminal() ||
        !prepared_ || finalize_taken_ || staged_.empty())
        return std::nullopt;
    try {
        finalized_ = std::make_shared<const std::vector<uint8_t>>(std::move(staged_));
        finalize_control_ = std::make_shared<FinalizeControl>();
    } catch (...) {
        clear_prepared();
        state_ = SourceIngressState::Rejected;
        return std::nullopt;
    }
    finalize_taken_ = true;
    source_finalized_ = true;
    prepared_ = false;
    state_ = SourceIngressState::Finalized;
    SourceFinalize result;
    result.lease_ = lease_;
    result.payload_ = finalized_;
    result.control_ = finalize_control_;
    return result;
}

std::optional<CacheWireStart> SourceIngress::start_cache_wire(
    const SourceIngressLease& owner, icecc::p50::InputRecordKey key,
    uint64_t cache_session_attempt, uint64_t operation_id, TimePoint now) noexcept {
    if (!exact_owner(owner) || expire_if_needed(now) || terminal() || wire_started_ ||
        !f_armed_ || !source_finalized_ || !finalized_ || !finalize_taken_ ||
        !finalize_control_ || !finalize_control_->consumed ||
        finalized_->empty() || cache_session_attempt == 0 || operation_id == 0 ||
        key.tu_seq.value == 0 ||
        key.c_store_guid.bytes != lease_.armed.arm.c_store_guid)
        return std::nullopt;
    input_key_ = key;
    cache_session_attempt_ = cache_session_attempt;
    operation_id_ = operation_id;
    raw_digest_ = icecc::digest128(std::span<const uint8_t>(*finalized_));
    wire_started_ = true;
    wire_state_ = CacheWireState::Ready;
    state_ = SourceIngressState::CacheWireStarted;
    return CacheWireStart{lease_, cache_session_attempt_, operation_id_, key,
                          raw_digest_, finalized_};
}

SourceIngressResult SourceIngress::emit_cache_wire(const SourceIngressLease& owner,
                                                   CacheWireFrame frame,
                                                   TimePoint now) noexcept {
    if (!exact_owner(owner)) return SourceIngressResult::WrongOwner;
    if (expire_if_needed(now)) return SourceIngressResult::DeadlineExpired;
    if (!wire_started_ || terminal()) return SourceIngressResult::Closed;
    switch (frame) {
    case CacheWireFrame::TxBegin:
        if (wire_state_ != CacheWireState::Ready) {
            reconcile(); return SourceIngressResult::Duplicate;
        }
        wire_state_ = CacheWireState::Begun; ++tx_count_; return SourceIngressResult::Accepted;
    case CacheWireFrame::TxBody:
        if (wire_state_ != CacheWireState::Begun) {
            reconcile(); return SourceIngressResult::InvalidOrder;
        }
        wire_state_ = CacheWireState::Body; ++tx_count_; return SourceIngressResult::Accepted;
    case CacheWireFrame::TxCommit:
        if (wire_state_ != CacheWireState::Body) {
            reconcile(); return SourceIngressResult::InvalidOrder;
        }
        wire_state_ = CacheWireState::CommitSent;
        state_ = SourceIngressState::CommitSent;
        commit_sent_ = true; ++tx_count_; return SourceIngressResult::Accepted;
    }
    reconcile();
    return SourceIngressResult::InvalidOrder;
}

bool SourceIngress::witness_matches(const CacheWireSettlementWitness& witness) const noexcept {
    if (!exact_owner(witness.lease) || !input_key_.has_value() ||
        witness.cache_session_attempt != cache_session_attempt_ ||
        witness.operation_id != operation_id_ || witness.input_key != *input_key_ ||
        witness.raw_bytes.size() != finalized_->size() ||
        !std::equal(witness.raw_bytes.begin(), witness.raw_bytes.end(), finalized_->begin()) ||
        witness.raw_digest != raw_digest_ ||
        icecc::digest128(std::span<const uint8_t>(witness.raw_bytes)) != raw_digest_)
        return false;
    if (witness.disposition == CacheWireSettlementDisposition::CommittedInput)
        return witness.committed_input.has_value() &&
               *witness.committed_input == *input_key_;
    return !witness.committed_input.has_value();
}

SourceIngressResult SourceIngress::settle_cache_wire(
    const SourceIngressLease& owner, const CacheWireSettlementWitness& witness,
    TimePoint now) noexcept {
    (void)now;
    if (!exact_owner(owner)) return SourceIngressResult::WrongOwner;
    if (witness.disposition != CacheWireSettlementDisposition::CommittedInput &&
        witness.disposition != CacheWireSettlementDisposition::ProvedPreDurable)
        return SourceIngressResult::Invalid;
    if (wire_state_ == CacheWireState::Committed ||
        wire_state_ == CacheWireState::SettledPreDurable)
        return SourceIngressResult::Duplicate;
    if (wire_state_ != CacheWireState::CommitSent &&
        wire_state_ != CacheWireState::Reconcile)
        return SourceIngressResult::InvalidOrder;
    if (wire_state_ == CacheWireState::Reconcile && !commit_sent_ &&
        witness.disposition == CacheWireSettlementDisposition::CommittedInput)
        return SourceIngressResult::InvalidOrder;
    if (!finalized_ || !witness_matches(witness)) return SourceIngressResult::Invalid;
    if (witness.disposition == CacheWireSettlementDisposition::CommittedInput) {
        revoke_finalize();
        wire_state_ = CacheWireState::Committed;
        state_ = SourceIngressState::Committed;
    } else {
        clear_prepared();
        revoke_finalize();
        wire_state_ = CacheWireState::SettledPreDurable;
        state_ = SourceIngressState::SettledPreDurable;
    }
    return SourceIngressResult::Accepted;
}

SourceIngressResult SourceIngress::cancel(const SourceIngressLease& owner,
                                          TimePoint now) noexcept {
    if (!exact_owner(owner)) return SourceIngressResult::WrongOwner;
    if (wire_state_ == CacheWireState::Committed ||
        wire_state_ == CacheWireState::SettledPreDurable)
        return SourceIngressResult::Closed;
    if (wire_started_) {
        // Ambiguous CacheWire cancellation retains the canonical payload and
        // endpoint witness until a late committed/pre-durable settlement.
        clear_prepared(); reconcile(); return SourceIngressResult::ReconcileRequired;
    }
    revoke_finalize();
    if (expire_if_needed(now)) return SourceIngressResult::DeadlineExpired;
    if (terminal()) return SourceIngressResult::Closed;
    clear_prepared(); state_ = SourceIngressState::Cancelled;
    return SourceIngressResult::Accepted;
}

bool SourceIngress::cleanup_ready() const noexcept {
    return state_ == SourceIngressState::Cancelled ||
           state_ == SourceIngressState::Rejected ||
           state_ == SourceIngressState::FailedCpp ||
           state_ == SourceIngressState::Expired ||
           state_ == SourceIngressState::Committed ||
           state_ == SourceIngressState::SettledPreDurable;
}

std::optional<SourceIngressLease> SourceIngressOwner::allocate(
    const P50SourceArmMsg& request, pid_t cpp_pid, TimePoint now) noexcept {
    if (!valid_ || active_.has_value() || !request.valid_payload() || cpp_pid <= 1 ||
        next_serial_ == 0 || next_serial_ == std::numeric_limits<uint64_t>::max())
        return std::nullopt;
    try {
        SourceIngressLease lease;
        lease.armed.arm = request.arm;
        lease.lease_serial = next_serial_++;
        lease.cpp_pid = cpp_pid;
        TimePoint deadline;
        if (!SourceIngress::add_budget(now, kHardSourceBudgetMsec, deadline))
            return std::nullopt;
        active_.emplace(SourceIngress(std::move(lease), deadline, max_bytes_));
        return active_->lease();
    } catch (...) {
        active_.reset();
        return std::nullopt;
    }
}

SourceIngress* SourceIngressOwner::activate(const SourceIngressLease& lease) noexcept {
    if (!active_.has_value() || active_->lease() != lease) return nullptr;
    return &*active_;
}

SourceIngressResult SourceIngressOwner::sweep(const SourceIngressLease& lease,
                                              TimePoint now) noexcept {
    if (!active_.has_value() || active_->lease() != lease)
        return SourceIngressResult::WrongOwner;
    return active_->expire(lease, now);
}

SourceIngressResult SourceIngressOwner::cleanup(const SourceIngressLease& lease) noexcept {
    if (!active_.has_value() || active_->lease() != lease)
        return SourceIngressResult::WrongOwner;
    if (!active_->cleanup_ready()) return SourceIngressResult::NotReady;
    active_->revoke_finalize();
    active_.reset();
    return SourceIngressResult::Accepted;
}

}  // namespace icecc::p50::daemon
