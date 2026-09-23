// Test-only reference component; not part of the live P50 implementation.
#pragma once

// Event-loop-owned C source ingress/finalization reducer.  This is a bounded
// integration seam: production CompileFile/private transport callsites remain
// deliberately unbound until they can supply the exact witnesses below.

#include "cache/p50_input_record.h"
#include "services/comm.h"
#include "services/digest128.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace icecc::p50::daemon {

inline constexpr size_t kHardMaxSourceBytes = 64u * 1024u * 1024u;
inline constexpr uint64_t kDefaultCacheSessionAttempt = 1;

enum class SourceIngressState : uint8_t {
    Receiving, Drained, Finalized, CacheWireStarted, Cancelled, Rejected,
    FailedCpp, Expired, Reconcile, CommitSent, Committed, SettledPreDurable
};

enum class CacheWireState : uint8_t {
    NotStarted, Ready, Begun, Body, CommitSent, Committed,
    SettledPreDurable, Reconcile
};

enum class SourceIngressResult : uint8_t {
    Accepted, Duplicate, WrongOwner, WrongPid, Invalid, InvalidAck,
    InvalidStatus, DeadlineExpired, Closed, FailedCpp, NotReady,
    InvalidOrder, ReconcileRequired
};

struct SourceIngressLease {
    // Before ACK only armed.arm is populated.  Once the exact wire ACK is
    // accepted, this becomes the complete canonical acknowledgement value.
    P50SourceArmedFields armed{};
    bool armed_wire_valid = false;
    uint64_t lease_serial = 0;
    pid_t cpp_pid = -1;

    [[nodiscard]] bool valid() const noexcept {
        return armed.arm.valid() && lease_serial != 0 && cpp_pid > 1;
    }
    [[nodiscard]] bool f_ack_valid() const noexcept {
        return armed_wire_valid && armed.semantic_valid();
    }
    auto operator<=>(const SourceIngressLease&) const = default;
};

struct FinalizeControl {
    bool active = true;
    bool consumed = false;
};

class SourceFinalize {
public:
    SourceFinalize() = default;
    SourceFinalize(const SourceFinalize&) = delete;
    SourceFinalize& operator=(const SourceFinalize&) = delete;
    SourceFinalize(SourceFinalize&&) noexcept = default;
    SourceFinalize& operator=(SourceFinalize&&) noexcept = default;

    [[nodiscard]] bool valid() const noexcept {
        return control_ && control_->active && !control_->consumed &&
               payload_ && !payload_->empty();
    }
    // The transport must consume this move-only capability exactly once before
    // emitting SOURCE_FINALIZE. Cancellation/replacement revokes it first.
    bool consume() noexcept {
        if (!valid()) return false;
        control_->consumed = true;
        return true;
    }
    [[nodiscard]] const SourceIngressLease& lease() const noexcept { return lease_; }
    [[nodiscard]] std::span<const uint8_t> bytes() const noexcept {
        return payload_ ? std::span<const uint8_t>(*payload_) : std::span<const uint8_t>{};
    }

private:
    friend class SourceIngress;
    SourceIngressLease lease_{};
    std::shared_ptr<const std::vector<uint8_t>> payload_;
    std::shared_ptr<FinalizeControl> control_;
};

enum class CacheWireFrame : uint8_t { TxBegin, TxBody, TxCommit };
enum class CacheWireSettlementDisposition : uint8_t {
    CommittedInput, ProvedPreDurable
};

// Lossless endpoint witness. Every field is checked against the live C lease,
// bound CacheWire operation, exact InputRecordKey, and retained raw payload.
struct CacheWireSettlementWitness {
    CacheWireSettlementDisposition disposition =
        CacheWireSettlementDisposition::ProvedPreDurable;
    SourceIngressLease lease{};
    uint64_t cache_session_attempt = 0;
    uint64_t operation_id = 0;
    icecc::p50::InputRecordKey input_key{};
    icecc::Digest128 raw_digest{};
    std::vector<uint8_t> raw_bytes;
    std::optional<icecc::p50::InputRecordKey> committed_input;
};

struct CacheWireStart {
    SourceIngressLease lease{};
    uint64_t cache_session_attempt = 0;
    uint64_t operation_id = 0;
    icecc::p50::InputRecordKey input_key{};
    icecc::Digest128 raw_digest{};
    std::shared_ptr<const std::vector<uint8_t>> raw_bytes;
};

class SourceIngressOwner;

class SourceIngress {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    SourceIngress() = delete;
    SourceIngress(const SourceIngress&) = delete;
    SourceIngress& operator=(const SourceIngress&) = delete;
    SourceIngress(SourceIngress&&) noexcept = default;
    SourceIngress& operator=(SourceIngress&&) noexcept = default;
    ~SourceIngress() = default;

    [[nodiscard]] const SourceIngressLease& lease() const noexcept { return lease_; }
    [[nodiscard]] SourceIngressState state() const noexcept { return state_; }
    [[nodiscard]] CacheWireState cache_wire_state() const noexcept { return wire_state_; }
    [[nodiscard]] TimePoint deadline() const noexcept { return deadline_; }
    [[nodiscard]] bool f_source_armed() const noexcept { return f_armed_; }
    [[nodiscard]] bool source_finalized() const noexcept { return source_finalized_; }
    [[nodiscard]] unsigned tx_emission_count() const noexcept { return tx_count_; }
    [[nodiscard]] bool cleanup_ready() const noexcept;

    SourceIngressResult append_wrapper_bytes(const SourceIngressLease&,
                                             std::span<const uint8_t>, TimePoint) noexcept;
    SourceIngressResult observe_wrapper_eof(const SourceIngressLease&, TimePoint) noexcept;
    SourceIngressResult accept_f_source_armed(const SourceIngressLease&,
                                              const P50SourceArmedMsg&, TimePoint) noexcept;
    SourceIngressResult observe_cpp_waitpid(const SourceIngressLease&, pid_t, int,
                                            TimePoint) noexcept;
    SourceIngressResult expire(const SourceIngressLease&, TimePoint) noexcept;
    std::optional<SourceFinalize> take_source_finalize(const SourceIngressLease&,
                                                       TimePoint) noexcept;
    std::optional<CacheWireStart> start_cache_wire(const SourceIngressLease&,
                                                   icecc::p50::InputRecordKey,
                                                   uint64_t cache_session_attempt,
                                                   uint64_t operation_id, TimePoint) noexcept;
    SourceIngressResult emit_cache_wire(const SourceIngressLease&, CacheWireFrame,
                                        TimePoint) noexcept;
    SourceIngressResult settle_cache_wire(const SourceIngressLease&,
                                          const CacheWireSettlementWitness&, TimePoint) noexcept;
    SourceIngressResult cancel(const SourceIngressLease&, TimePoint) noexcept;

private:
    friend class SourceIngressOwner;
    SourceIngress(SourceIngressLease lease, TimePoint deadline, size_t max_bytes)
        : lease_(std::move(lease)), deadline_(deadline), max_bytes_(max_bytes) {}
    [[nodiscard]] bool exact_owner(const SourceIngressLease&) const noexcept;
    bool expire_if_needed(TimePoint) noexcept;
    [[nodiscard]] bool terminal() const noexcept;
    void clear_prepared() noexcept;
    void revoke_finalize() noexcept;
    void reconcile() noexcept;
    void maybe_finalize() noexcept;
    [[nodiscard]] static bool add_budget(TimePoint, uint32_t, TimePoint&) noexcept;
    [[nodiscard]] bool witness_matches(const CacheWireSettlementWitness&) const noexcept;

    SourceIngressLease lease_{};
    TimePoint deadline_{};
    size_t max_bytes_ = 0;
    std::vector<uint8_t> staged_;
    std::shared_ptr<const std::vector<uint8_t>> finalized_;
    std::shared_ptr<FinalizeControl> finalize_control_;
    std::optional<icecc::p50::InputRecordKey> input_key_;
    icecc::Digest128 raw_digest_{};
    uint64_t cache_session_attempt_ = 0;
    uint64_t operation_id_ = 0;
    SourceIngressState state_ = SourceIngressState::Receiving;
    CacheWireState wire_state_ = CacheWireState::NotStarted;
    bool eof_ = false;
    bool cpp_ok_ = false;
    bool prepared_ = false;
    bool f_armed_ = false;
    bool source_finalized_ = false;
    bool finalize_taken_ = false;
    bool wire_started_ = false;
    bool commit_sent_ = false;
    unsigned tx_count_ = 0;
};

class SourceIngressOwner {
public:
    using Clock = SourceIngress::Clock;
    using TimePoint = SourceIngress::TimePoint;
    explicit SourceIngressOwner(size_t max_bytes = kHardMaxSourceBytes)
        : max_bytes_(max_bytes), valid_(max_bytes != 0 && max_bytes <= kHardMaxSourceBytes) {}
    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] bool busy() const noexcept { return active_.has_value(); }
    [[nodiscard]] uint64_t next_serial() const noexcept { return next_serial_; }
    std::optional<SourceIngressLease> allocate(const P50SourceArmMsg&, pid_t, TimePoint) noexcept;
    SourceIngress* activate(const SourceIngressLease&) noexcept;
    SourceIngressResult sweep(const SourceIngressLease&, TimePoint) noexcept;
    SourceIngressResult cleanup(const SourceIngressLease&) noexcept;

private:
    size_t max_bytes_;
    bool valid_ = false;
    uint64_t next_serial_ = 1;
    std::optional<SourceIngress> active_;
};

}  // namespace icecc::p50::daemon
