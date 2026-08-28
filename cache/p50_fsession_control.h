/*
    Protocol-50 FSession distributed control protocol (S2 hard-seam vertical).

    One logical FSessionOperation is realized as two owner-affine process-local
    reducers (DaemonFSessionOperation in iceccd, SidecarFSessionOperation in the
    cache sidecar), joined by ONE authenticated dedicated AF_UNIX control
    connection carrying identity-complete typed frames (issue-16 5443811178).

    This header defines the shared control vocabulary both reducers use:
      - FSessionOperationIdentity : the complete immutable cross-process identity
      - FSessionControlDirection + direction-typed message enums (legality is a
        compile-time/type property; 5444410383 sec.4)
      - FSessionControlEnvelope + codec (magic/version/identity/generation/
        direction/type/sequence/bounded-payload; 5444410383 sec.1)
      - OutboundSemanticSlot : bounded owner-affine outbound evidence, one
        canonical frame per semantic transition (5444539259 sec.2)

    Input snapshot: binding issue-16 rulings through head 5446088657 (Root
    execution directive 5446112752). Later correctness rulings are deltas.
*/
#ifndef ICECC_CACHE_P50_FSESSION_CONTROL_H
#define ICECC_CACHE_P50_FSESSION_CONTROL_H

#include "protocol50.h"                    // Id128 (CStoreGuid/FStoreGuid)
#include "p50_sidecar_supervisor.h"        // AbsoluteMonotonicDeadline

#include "services/p50_cache_session_wire.h" // P50FSessionOperationId, P50WireLaunchIdentity

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace icecc::p50::fsession {

// ---------------------------------------------------------------------------
// Complete immutable cross-process operation identity (5443811178 sec.2).
// Every control frame and every subordinate typed ID binds this. Wrong launch /
// store / assignment / operation / sequence is stale-only and mutates no row.
// ---------------------------------------------------------------------------
struct FSessionOperationIdentity {
    // daemon side
    uint64_t daemon_launch_generation = 0;   // iceccd launch/registry generation
    uint64_t control_connection_generation = 0; // per dedicated control connection

    // sidecar side (canonical: carried inside the operation id as well)
    daemon::P50FSessionOperationId operation{}; // sidecar launch + role + op seq

    // stores
    Id128 c_store_guid{};
    Id128 f_store_guid{};

    // scheduler assignment binding (from the live daemon owner)
    uint64_t assignment_job = 0;
    uint64_t assignment_epoch = 0;
    uint64_t assignment_nonce = 0;

    // the exact accepted source-arm / WAIT observation identity (daemon owner).
    uint64_t arm_observation = 0;

    // original absolute monotonic expiry (carried unchanged across the operation)
    sidecar::AbsoluteMonotonicDeadline deadline{};

    [[nodiscard]] bool valid() const noexcept {
        return daemon_launch_generation != 0 &&
               control_connection_generation != 0 && operation.valid() &&
               c_store_guid != Id128{} && f_store_guid != Id128{} &&
               c_store_guid != f_store_guid && assignment_job != 0 &&
               assignment_epoch != 0 && assignment_nonce != 0 &&
               arm_observation != 0 && deadline.valid();
    }

    // Exact identity equality is required for every accept/replay decision;
    // a subset/digest is never sufficient (5444410383 sec.2).
    friend bool operator==(const FSessionOperationIdentity&,
                           const FSessionOperationIdentity&) = default;
};

// ---------------------------------------------------------------------------
// Direction + direction-typed message enums (5444410383 sec.4). A frame encoded
// with the wrong direction is rejected before reducer dispatch; TerminalAck is a
// reachable Daemon->Sidecar frame (never trapped in a sidecar-only phase table).
// ---------------------------------------------------------------------------
enum class FSessionControlDirection : uint8_t {
    DaemonToSidecar = 1,
    SidecarToDaemon = 2,
};

enum class DaemonToSidecarType : uint16_t {
    OperationOffer = 1,   // the SOLE row-creation frame (5444410383 sec.3)
    PublicFdOffer = 2,
    OpCancel = 3,
    DaemonFdAccepted = 4,
    DaemonFdRejected = 5,
    TerminalAck = 6,
};

enum class SidecarToDaemonType : uint16_t {
    OperationAccepted = 1,
    PublicFdAdoptedReceipt = 2,
    EndpointObservation = 3,
    InputCommitted = 4,
    InputAbortedPreDurable = 5,
    InputCancelledAfterCommit = 6,
    DeliveryOffer = 7,
    TerminalObservation = 8,
};

[[nodiscard]] bool direction_legal_daemon(DaemonToSidecarType type) noexcept;
[[nodiscard]] bool direction_legal_sidecar(SidecarToDaemonType type) noexcept;

// ---------------------------------------------------------------------------
// The wire envelope (5444410383 sec.1). Fixed header + bounded canonical payload.
// Two independent monotonic sequence spaces (one per direction) are owned by the
// reducers; the envelope carries the exact direction-local sequence.
// ---------------------------------------------------------------------------
inline constexpr uint32_t kFSessionControlMagic = UINT32_C(0x50354653); // "P5FS"
inline constexpr uint16_t kFSessionControlVersion = 1;
inline constexpr size_t kFSessionControlMaxPayload = 64u * 1024u;

struct FSessionControlEnvelope {
    FSessionOperationIdentity identity{};
    FSessionControlDirection direction = FSessionControlDirection::DaemonToSidecar;
    uint16_t message_type = 0;    // interpreted per direction
    uint64_t sequence = 0;        // nonzero direction-local sequence
    std::vector<uint8_t> payload; // bounded canonical payload (<= max)

    [[nodiscard]] bool header_valid() const noexcept;
};

// Codec. encode returns nullopt on an invalid envelope; decode enforces magic/
// version/direction/type legality/sequence!=0/bounded-length and full identity
// presence before returning.
[[nodiscard]] std::optional<std::vector<uint8_t>>
encode_fsession_control(const FSessionControlEnvelope& envelope);
[[nodiscard]] std::optional<FSessionControlEnvelope>
decode_fsession_control(std::span<const uint8_t> bytes);

// ---------------------------------------------------------------------------
// Bounded outbound semantic slot (5444539259 sec.2): at most one canonical frame
// per semantic transition; a byte-identical duplicate reuses the retained frame.
// Mandatory frames are pre-reserved before their irreversible transition.
// ---------------------------------------------------------------------------
enum class OutboundSlotState : uint8_t {
    Empty = 0,
    Reserved,      // storage reserved before the irreversible transition
    Queued,        // canonical bytes staged, not yet writing
    Writing,       // partially flushed (offset tracked)
    FullyFlushed,  // all bytes written to the transport
    AckedRetained, // peer acked; retained for exact replay/tombstone
    Retired,
};

struct OutboundSemanticSlot {
    uint64_t sequence = 0;         // exact direction-local sequence
    uint16_t message_type = 0;     // exact legal message type / phase
    std::vector<uint8_t> canonical_bytes; // the one canonical encoded frame
    OutboundSlotState state = OutboundSlotState::Empty;
    size_t write_offset = 0;       // for Writing

    [[nodiscard]] bool occupied() const noexcept {
        return state != OutboundSlotState::Empty &&
               state != OutboundSlotState::Retired;
    }
};

// ---------------------------------------------------------------------------
// Inbound control-sequence acceptor for ONE operation on the dedicated
// Daemon->Sidecar stream (5444410383 sec.2-3). Encodes the sidecar row-creation
// and exact-replay/gap law that fixes Root's rejected-probe counterexamples:
//   - OperationOffer(seq==1) is the SOLE row-creation frame; any earlier frame
//     (e.g. OpCancel) is phase-invalid and mints no row;
//   - seq == next_expected accepts exactly one owner transition and advances;
//   - seq <  next_expected accepts ONLY a byte-identical replay (zero mutation,
//     replay the prior response); a non-identical duplicate is an op-local error;
//   - seq >  next_expected is a gap (op-local error, no hiding buffer);
//   - a retired operation is never revived under the same identity.
// ---------------------------------------------------------------------------
enum class InboundDisposition : uint8_t {
    AcceptedNew,        // seq == next_expected: one owner transition, advance
    ExactReplay,        // seq <  next_expected, byte-identical: replay, no mutation
    DuplicateConflict,  // seq <  next_expected, non-identical: op-local error
    Gap,                // seq >  next_expected: op-local error
    PhaseInvalidNoRow,  // frame before an accepted OperationOffer: no row created
    StaleWrongIdentity, // identity mismatch or retired: stale-only, no mutation
};

class FSessionInboundControl {
public:
    // Sidecar-side acceptor: no row exists until an accepted OperationOffer
    // (the sole row-creation frame); expects Daemon->Sidecar frames.
    FSessionInboundControl() noexcept = default;

    // Daemon-side acceptor: the daemon minted the operation, so its row and
    // identity are pre-bound; expects Sidecar->Daemon frames starting at seq 1.
    [[nodiscard]] static FSessionInboundControl
    daemon_bound(const FSessionOperationIdentity& identity) noexcept {
        FSessionInboundControl in;
        in.identity_ = identity;
        in.row_created_ = true;
        in.expected_direction_ = FSessionControlDirection::SidecarToDaemon;
        return in;
    }

    // Classify one already-codec-validated inbound envelope for this acceptor's
    // expected direction. canonical_bytes must be the exact encoded frame, for
    // byte-identical replay comparison. A wrong-direction frame is rejected
    // before any sequence/row logic (direction legality is enforced here as
    // well as in the codec).
    InboundDisposition classify(const FSessionControlEnvelope& envelope,
                                std::span<const uint8_t> canonical_bytes);

    [[nodiscard]] bool row_created() const noexcept { return row_created_; }
    [[nodiscard]] bool retired() const noexcept { return retired_; }
    [[nodiscard]] uint64_t next_expected() const noexcept { return next_expected_; }
    [[nodiscard]] const FSessionOperationIdentity& identity() const noexcept {
        return identity_;
    }
    // Terminal retirement. Object storage may be reused only by minting a fresh
    // identity/connection-generation (a new object), never by clearing this one.
    void retire() noexcept { retired_ = true; }

private:
    FSessionOperationIdentity identity_{}; // bound by the offer / at daemon mint
    FSessionControlDirection expected_direction_ =
        FSessionControlDirection::DaemonToSidecar;
    bool row_created_ = false;
    bool retired_ = false;
    uint64_t next_expected_ = 1;
    std::vector<std::vector<uint8_t>> retained_; // retained_[s-1] = frame at seq s
};

// ---------------------------------------------------------------------------
// Bounded outbound control for one operation's direction (5444539259). Mandatory
// frames are RESERVED before their irreversible transition, so enqueue can never
// fail after the durable/authority-advancing step. At most one canonical frame
// per semantic transition; a byte-identical duplicate reuses the retained frame;
// acked frames are retained (for exact replay) until retirement. No unbounded
// message log: reservation fails closed when the bounded live set is full.
// ---------------------------------------------------------------------------
class FSessionOutboundControl {
public:
    explicit FSessionOutboundControl(size_t max_live_slots = 32) noexcept
        : max_live_(max_live_slots == 0 ? 1 : max_live_slots) {}

    // Reserve a slot for a mandatory frame BEFORE its irreversible transition.
    // Returns the direction-local sequence, or 0 if the bounded live set is full
    // (the caller must then NOT perform the irreversible transition).
    [[nodiscard]] uint64_t reserve(uint16_t message_type);

    // Stage the one canonical frame into a Reserved slot (Reserved -> Queued).
    [[nodiscard]] bool stage(uint64_t sequence,
                             std::span<const uint8_t> canonical_bytes);

    // Idempotent enqueue: if a live slot already holds a byte-identical frame of
    // this exact type, reuse its sequence; otherwise reserve + stage. Returns the
    // sequence, or 0 if capacity is full for a genuinely new frame.
    [[nodiscard]] uint64_t
    enqueue_idempotent(uint16_t message_type, std::span<const uint8_t> bytes);

    // Flush lifecycle. record_written advances Writing offset (-> FullyFlushed
    // when the whole frame is out); mark_acked retains for replay; retire frees.
    [[nodiscard]] bool record_written(uint64_t sequence, size_t nbytes);
    [[nodiscard]] bool mark_acked(uint64_t sequence);
    [[nodiscard]] bool retire(uint64_t sequence);

    [[nodiscard]] size_t live_slots() const noexcept;
    // Backing storage size (retired slots are reused, so this stays bounded by
    // the live-set cap; exposed for the no-unbounded-log invariant test).
    [[nodiscard]] size_t slot_storage() const noexcept { return slots_.size(); }
    [[nodiscard]] const OutboundSemanticSlot* find(uint64_t sequence) const noexcept;

private:
    OutboundSemanticSlot* mutable_find(uint64_t sequence) noexcept;

    size_t max_live_;
    uint64_t next_sequence_ = 1;
    std::vector<OutboundSemanticSlot> slots_;
};

} // namespace icecc::p50::fsession

#endif // ICECC_CACHE_P50_FSESSION_CONTROL_H
