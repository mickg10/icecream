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

} // namespace icecc::p50::fsession

#endif // ICECC_CACHE_P50_FSESSION_CONTROL_H
