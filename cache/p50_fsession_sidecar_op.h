/*
    SidecarFSessionOperation — the sidecar F-owner's process-local phase reducer
    for one distributed F-session operation (S2 vertical).

    One logical operation is realized as two owner-affine process-local reducers
    joined by the typed control protocol (issue-16 5443811178). This is the
    sidecar half. Single-writer: only the F-role owner invokes these methods;
    endpoint/codec/timer callbacks deliver identity-bound observations.

    Laws encoded here:
      - OperationOffer is consumed through FSessionInboundControl (sole
        row-creation frame); OperationAccepted is staged through the bounded
        outbound slots.
      - PublicFdAdoptedReceipt is minted only after the product preconditions
        hold (5444123889 sec.4): operation registered + identity match + live
        deadline + route lease consumed + endpoint-run registration + injected
        socket-level validation (connected-TCP/cookie/exclusive-P5CO supplied by
        the production wiring); the receipt must be fully flushed before the
        first public P5CO byte (enforced by the caller via the slot state).
      - PreparedInputReady waits for the owner CommitPermit; OpCancel races
        commit on THIS owner in both orders with exactly one durability outcome:
        cancel-before-commit -> AbortedPreDurable (no record/Ready advance);
        commit-before-cancel -> CancelledAfterCommit (durable bundle retained,
        delivery suppressed) (5431602632 / 5443811178 sec.6).
      - TerminalObservation is staged only after local facts are fixed; the row
        retires only after the matching TerminalAck is consumed; an ACK-loss
        replay replays the same observation bytes (5444410383 sec.5).
*/
#ifndef ICECC_CACHE_P50_FSESSION_SIDECAR_OP_H
#define ICECC_CACHE_P50_FSESSION_SIDECAR_OP_H

#include "p50_fsession_control.h"
#include "p50_fsession_route.h"

#include <functional>
#include <optional>

namespace icecc::p50::fsession {

enum class SidecarOpPhase : uint8_t {
    AwaitOffer = 0,
    Accepted,          // offer consumed; OperationAccepted staged
    PublicFdAdopted,   // receipt staged (flush gate is the slot state)
    EndpointRunning,   // CacheWire in progress
    PreparedAwaitPermit, // PreparedInputReady; waiting for owner CommitPermit
    Committed,         // durable bundle exists (InputCommitted staged)
    AbortedPreDurable, // cancel linearized before commit
    CancelledAfterCommit, // cancel after commit: bundle retained, delivery suppressed
    TerminalStaged,    // TerminalObservation staged, awaiting TerminalAck
    Retired,
};

// Socket-level adoption validation supplied by the production wiring
// (connected TCP stream, CLOEXEC/mode, cookie match, exclusive P5CO writer).
using PublicFdValidator = std::function<bool()>;

class SidecarFSessionOperation {
public:
    explicit SidecarFSessionOperation(size_t outbound_slots = 16)
        : outbound_(outbound_slots) {}

    [[nodiscard]] SidecarOpPhase phase() const noexcept { return phase_; }
    [[nodiscard]] FSessionInboundControl& inbound() noexcept { return inbound_; }
    [[nodiscard]] FSessionOutboundControl& outbound() noexcept { return outbound_; }

    // --- offer / accept ---------------------------------------------------
    // Consume one decoded inbound frame (with its exact canonical bytes).
    // Returns the disposition; on AcceptedNew of the OperationOffer, stages
    // OperationAccepted and enters Accepted.
    InboundDisposition consume_inbound(const FSessionControlEnvelope& envelope,
                                       std::span<const uint8_t> bytes,
                                       int64_t now_ns);

    // --- public-FD adoption ----------------------------------------------
    // Mint the PublicFdAdoptedReceipt after ALL preconditions hold. The route
    // lease is consumed (moved in); socket-level checks are injected. Returns
    // the staged receipt's outbound sequence, or 0 on refusal (no partial
    // authority: a refusal leaves phase unchanged and the lease unconsumed).
    [[nodiscard]] uint64_t
    adopt_public_fd(RouteSessionLease&& route_lease,
                    const PublicFdValidator& socket_valid, int64_t now_ns);

    // Endpoint began CacheWire (receipt fully flushed; caller checked slot).
    [[nodiscard]] bool endpoint_started();

    // --- prepared / commit-vs-cancel race ---------------------------------
    [[nodiscard]] bool prepared_input_ready();      // -> PreparedAwaitPermit
    // Owner grants the CommitPermit and the durable commit linearizes.
    // Returns the staged InputCommitted sequence, or 0 if illegal in phase.
    [[nodiscard]] uint64_t grant_commit_permit_and_commit();
    // Exact OpCancel consumed by the owner (already sequence-accepted through
    // consume_inbound). Applies the race law for the current phase.
    void apply_cancel() noexcept;

    // --- terminal handshake ----------------------------------------------
    // Stage the one TerminalObservation once local facts are fixed. Legal from
    // any post-offer terminal-capable phase. Returns its sequence (stable on
    // repeat call: replay reuses the same staged frame), or 0 if illegal.
    [[nodiscard]] uint64_t stage_terminal_observation();
    // Consume the matching TerminalAck (already sequence-accepted). Retires the
    // row. Returns false if no observation is staged.
    [[nodiscard]] bool consume_terminal_ack() noexcept;

    [[nodiscard]] bool delivery_suppressed() const noexcept {
        return delivery_suppressed_;
    }

private:
    [[nodiscard]] bool deadline_live(int64_t now_ns) const noexcept;

    SidecarOpPhase phase_ = SidecarOpPhase::AwaitOffer;
    FSessionInboundControl inbound_;
    FSessionOutboundControl outbound_;
    std::optional<RouteSessionLease> route_lease_;
    uint64_t terminal_observation_seq_ = 0;
    bool delivery_suppressed_ = false;
};

} // namespace icecc::p50::fsession

#endif // ICECC_CACHE_P50_FSESSION_SIDECAR_OP_H
