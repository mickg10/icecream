/*
    DaemonFSessionOperation — the iceccd outer-poll owner's process-local phase
    reducer for one distributed F-session operation (S2 vertical; the daemon
    half of issue-16 5443811178).

    Laws encoded here:
      - The operation is created only by consuming/adopting a move-only
        DaemonWaitLease referencing the exact live daemon-owned Client/
        assignment/WAIT facts (5444285506 sec.1). Authority is never
        reconstructed from a Client pointer + status enum + copied integers;
        the production mint site is the outer-loop owner (wired later).
      - After SCM_RIGHTS send MAY have occurred the daemon holds
        PublicFdOffered(PublicFdOfferId) -- not adopted/settled -- and retains
        the offer ledger and original deadline until it consumes the exact
        PublicFdAdoptedReceipt or reaches a typed terminal/reconciliation
        result. An internal transfer retry reuses the SAME PublicFdOfferId; no
        second client claim is opened (5444123889 sec.5).
      - InputFdAcceptanceReceipt is minted by THIS daemon owner exactly once
        per AttachmentDeliveryId after FD validation/adoption; an ACK-loss
        retry of the same delivery finds the retained receipt, closes the
        duplicate descriptor, and replays the same acceptance -- it never
        creates a second WAITP50INPUT -> TOCOMPILE transition (5443811178
        sec.6).
      - TerminalObservation settles at most once; the daemon retains an exact
        terminal tombstone and replays the same TerminalAck on observation
        replay (5444410383 sec.5).
*/
#ifndef ICECC_CACHE_P50_FSESSION_DAEMON_OP_H
#define ICECC_CACHE_P50_FSESSION_DAEMON_OP_H

#include "p50_fsession_control.h"
#include "p50_fsession_payloads.h"

#include <optional>
#include <vector>

namespace icecc::p50::fsession {

// Move-only owner view over the exact live daemon-owned WAIT/assignment facts.
// Minted/adopted only by the daemon outer-loop owner; carries no independently
// invented identity. The revalidation hook lets the owner confirm the same row
// is still current at consumption time (a stale lease refuses adoption).
class DaemonWaitLease {
public:
    struct Facts {
        uint64_t client_connection_generation = 0;
        uint64_t compile_file_lease = 0;
        uint64_t assignment_job = 0;
        uint64_t assignment_epoch = 0;
        uint64_t assignment_nonce = 0;
        uint64_t arm_observation = 0;
        uint64_t wait_reservation = 0;
        uint64_t consumed_claim_capability = 0;
        sidecar::AbsoluteMonotonicDeadline deadline{};

        [[nodiscard]] bool valid() const noexcept {
            return client_connection_generation != 0 &&
                   compile_file_lease != 0 && assignment_job != 0 &&
                   assignment_epoch != 0 && assignment_nonce != 0 &&
                   arm_observation != 0 && wait_reservation != 0 &&
                   consumed_claim_capability != 0 && deadline.valid();
        }
    };

    DaemonWaitLease() = default;
    explicit DaemonWaitLease(Facts facts) noexcept
        : facts_(facts), valid_(facts.valid()) {}
    DaemonWaitLease(const DaemonWaitLease&) = delete;
    DaemonWaitLease& operator=(const DaemonWaitLease&) = delete;
    DaemonWaitLease(DaemonWaitLease&& other) noexcept { *this = std::move(other); }
    DaemonWaitLease& operator=(DaemonWaitLease&& other) noexcept {
        facts_ = other.facts_;
        valid_ = other.valid_;
        other.valid_ = false;
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const Facts& facts() const noexcept { return facts_; }

private:
    Facts facts_{};
    bool valid_ = false;
};

// Daemon-minted acceptance evidence: exactly one per AttachmentDeliveryId.
struct InputFdAcceptanceReceipt {
    uint64_t receipt_id = 0;
    uint64_t delivery_id = 0;
    uint64_t operation_sequence = 0; // of the accepted operation

    [[nodiscard]] bool valid() const noexcept {
        return receipt_id != 0 && delivery_id != 0;
    }
};

enum class DaemonOpPhase : uint8_t {
    Minted = 0,        // lease consumed; OperationOffer staged
    AcceptedByPeer,    // OperationAccepted consumed
    PublicFdOffered,   // SCM_RIGHTS send may have occurred; awaiting receipt
    FdAdoptedObserved, // exact PublicFdAdoptedReceipt consumed
    SourceAccepted,    // delivery adopted; receipt minted; TOCOMPILE permitted
    CancelRequested,   // OpCancel staged; awaiting terminal observation
    Settled,           // terminal observation consumed once; tombstone retained
    ReconcileRequired, // control loss after transfer may have occurred
    Retired,
};

struct DeliveryAcceptance {
    InputFdAcceptanceReceipt receipt{};
    bool replay = false; // true => duplicate delivery: close the dup descriptor
};

class DaemonFSessionOperation {
public:
    // Create the operation by consuming the wait lease (moved only on
    // success). Returns nullopt if the lease or identity is invalid or the
    // lease facts disagree with the identity's assignment binding.
    // predecessor: the route cursor this operation presents at admission
    // (tagged cold for a fresh route; the exact committed successor for a
    // next-TU operation). Carried in the real OperationOffer payload.
    [[nodiscard]] static std::optional<DaemonFSessionOperation>
    mint(const FSessionOperationIdentity& identity, DaemonWaitLease&& lease,
         const RoutePredecessor& predecessor = RoutePredecessor{},
         size_t outbound_slots = 16);

    [[nodiscard]] DaemonOpPhase phase() const noexcept { return phase_; }
    [[nodiscard]] FSessionInboundControl& inbound() noexcept { return inbound_; }
    [[nodiscard]] FSessionOutboundControl& outbound() noexcept { return outbound_; }
    [[nodiscard]] const FSessionOperationIdentity& identity() const noexcept {
        return identity_;
    }

    // Consume one decoded, sequence-accepted Sidecar->Daemon frame.
    InboundDisposition consume_inbound(const FSessionControlEnvelope& envelope,
                                       std::span<const uint8_t> bytes);

    // The public socket transfer is about to be attempted (or retried). Mints
    // the one PublicFdOfferId on first call; every retry reuses it but mints a
    // FRESH AncillaryAttemptId (5448067827 sec.4). socket_cookie identifies
    // the offered kernel socket. Only legal from AcceptedByPeer onward.
    [[nodiscard]] uint64_t offer_public_fd(uint64_t socket_cookie);

    // A delivery arrived (identified by AttachmentDeliveryId; descriptor
    // validation injected by the wiring). Mints the InputFdAcceptanceReceipt
    // exactly once per delivery id; a replay returns the SAME receipt with
    // replay=true (close the duplicate; no second TOCOMPILE transition).
    [[nodiscard]] std::optional<DeliveryAcceptance>
    accept_delivery(uint64_t delivery_id, bool descriptor_valid);

    // Daemon-side cancellation decision: stages the real OpCancel payload with
    // a minted cancellation-observation identity (no socket mutation here;
    // owner-linearized execution is the sidecar's, 5444264451).
    [[nodiscard]] uint64_t request_cancel(uint16_t reason = 1);

    // Control loss after transfer may have occurred: typed reconciliation, no
    // WAIT rollback, no second claim.
    void control_lost() noexcept;

    [[nodiscard]] uint64_t settlement_count() const noexcept {
        return settlement_count_;
    }
    [[nodiscard]] uint64_t tocompile_transitions() const noexcept {
        return tocompile_transitions_;
    }
    [[nodiscard]] bool retire() noexcept; // legal only after Settled + ack flushed

private:
    DaemonFSessionOperation() = default;
    [[nodiscard]] bool
    retiredish_replay_probe(const FSessionControlEnvelope& envelope,
                            std::span<const uint8_t> bytes) const;

    FSessionOperationIdentity identity_{};
    DaemonWaitLease lease_;
    DaemonOpPhase phase_ = DaemonOpPhase::Minted;
    FSessionInboundControl inbound_;
    FSessionOutboundControl outbound_{16};
    RoutePredecessor predecessor_{};
    uint64_t public_fd_offer_id_ = 0;
    uint64_t next_ancillary_attempt_ = 1;
    uint64_t next_cancellation_observation_ = 1;
    std::optional<DeliveryOfferPayload> delivery_offer_;
    std::vector<InputFdAcceptanceReceipt> acceptance_ledger_;
    uint64_t next_receipt_id_ = 1;
    uint64_t settlement_count_ = 0;
    uint64_t tocompile_transitions_ = 0;
    uint64_t terminal_ack_seq_ = 0;
};

} // namespace icecc::p50::fsession

#endif // ICECC_CACHE_P50_FSESSION_DAEMON_OP_H
