/*
    Exact identity-complete payload codecs for the F-session control protocol
    (S2 vertical; issue-16 5448067827 sec.3).

    Every payload re-binds the COMPLETE FSessionOperationIdentity plus its
    typed subordinate identity; the decoder validates the payload identity
    against the envelope identity and rejects trailing bytes. No field is ever
    inferred from connection-local mutable state. Positive admission stays
    closed until these codecs are installed at the staging sites.
*/
#ifndef ICECC_CACHE_P50_FSESSION_PAYLOADS_H
#define ICECC_CACHE_P50_FSESSION_PAYLOADS_H

#include "p50_fsession_control.h"
#include "p50_fsession_route.h" // RoutePredecessor

#include <optional>

namespace icecc::p50::fsession {

// --- Daemon -> Sidecar ------------------------------------------------------

// OperationOffer: the claim/WAIT binding from the live DaemonWaitLease plus
// the route predecessor the sidecar must present at route admission.
struct OperationOfferPayload {
    FSessionOperationIdentity identity{};
    uint64_t client_connection_generation = 0;
    uint64_t compile_file_lease = 0;
    uint64_t wait_reservation = 0;
    uint64_t consumed_claim_capability = 0;
    RoutePredecessor predecessor{};
    [[nodiscard]] bool valid() const noexcept {
        return identity.valid() && client_connection_generation != 0 &&
               compile_file_lease != 0 && wait_reservation != 0 &&
               consumed_claim_capability != 0 && predecessor.valid();
    }
};

struct OpCancelPayload {
    FSessionOperationIdentity identity{};
    uint16_t reason = 0;
    uint64_t cancellation_observation = 0;
    [[nodiscard]] bool valid() const noexcept {
        return identity.valid() && cancellation_observation != 0;
    }
};

struct PublicFdOfferPayload {
    FSessionOperationIdentity identity{};
    uint64_t public_fd_offer_id = 0;  // stable across retries
    uint64_t ancillary_attempt_id = 0; // fresh per rendezvous attempt
    uint64_t socket_cookie = 0;
    [[nodiscard]] bool valid() const noexcept {
        return identity.valid() && public_fd_offer_id != 0 &&
               ancillary_attempt_id != 0 && socket_cookie != 0;
    }
};

struct DaemonFdAcceptedPayload {
    FSessionOperationIdentity identity{};
    uint64_t attachment_delivery_id = 0;
    uint64_t acceptance_receipt_id = 0; // daemon-minted InputFdAcceptanceReceipt
    uint8_t replay = 0;                 // duplicate-delivery replay marker
    [[nodiscard]] bool valid() const noexcept {
        return identity.valid() && attachment_delivery_id != 0 &&
               acceptance_receipt_id != 0;
    }
};

struct DaemonFdRejectedPayload {
    FSessionOperationIdentity identity{};
    uint64_t attachment_delivery_id = 0;
    uint16_t reason = 0;
    [[nodiscard]] bool valid() const noexcept {
        return identity.valid() && attachment_delivery_id != 0;
    }
};

struct TerminalAckPayload {
    FSessionOperationIdentity identity{};
    uint64_t ack_of_sidecar_sequence = 0;
    uint16_t terminal_class_echo = 0;
    uint64_t settlement_id = 0; // daemon settlement/tombstone identity
    [[nodiscard]] bool valid() const noexcept {
        return identity.valid() && ack_of_sidecar_sequence != 0 &&
               settlement_id != 0;
    }
};

// --- Sidecar -> Daemon ------------------------------------------------------

struct OperationAcceptedPayload {
    FSessionOperationIdentity identity{};
    uint64_t ack_of_daemon_sequence = 0; // the accepted OperationOffer
    uint64_t sidecar_store_generation = 0;
    [[nodiscard]] bool valid() const noexcept {
        return identity.valid() && ack_of_daemon_sequence != 0 &&
               sidecar_store_generation != 0;
    }
};

struct PublicFdAdoptedReceiptPayload {
    FSessionOperationIdentity identity{};
    uint64_t public_fd_offer_id = 0;
    uint64_t endpoint_generation = 0;
    uint64_t endpoint_session_serial = 0;
    uint64_t run_sequence = 0;
    uint64_t socket_ownership_generation = 0;
    uint64_t route_admission_sequence = 0;
    uint64_t sidecar_owner_sequence = 0;
    [[nodiscard]] bool valid() const noexcept {
        return identity.valid() && public_fd_offer_id != 0 &&
               endpoint_generation != 0 && endpoint_session_serial != 0 &&
               run_sequence != 0 && socket_ownership_generation != 0 &&
               route_admission_sequence != 0 && sidecar_owner_sequence != 0;
    }
};

enum class EndpointEventKind : uint16_t {
    Started = 1,
    PreparedInputReady = 2,
    CommitSelected = 3,
    TxCommitProgress = 4,
    LocalTerminal = 5,
};

struct EndpointObservationPayload {
    FSessionOperationIdentity identity{};
    uint64_t run_sequence = 0;
    EndpointEventKind event = EndpointEventKind::Started;
    uint64_t progress = 0;
    uint64_t cancellation_observation = 0; // 0 unless owner-cancel-caused
    [[nodiscard]] bool valid() const noexcept {
        return identity.valid() && run_sequence != 0 &&
               static_cast<uint16_t>(event) >= 1 &&
               static_cast<uint16_t>(event) <= 5;
    }
};

struct InputCommittedPayload {
    FSessionOperationIdentity identity{};
    uint64_t ready_event_id = 0;   // canonical-store minted; referenced only
    uint64_t backing_id = 0;
    uint64_t successor_rel_seq = 0;
    std::array<uint8_t, 16> successor_digest{};
    uint64_t committed_receipt_id = 0;
    [[nodiscard]] bool valid() const noexcept {
        bool digest_nonzero = false;
        for (uint8_t b : successor_digest)
            digest_nonzero = digest_nonzero || b != 0;
        return identity.valid() && ready_event_id != 0 && backing_id != 0 &&
               successor_rel_seq != 0 && digest_nonzero &&
               committed_receipt_id != 0;
    }
};

struct InputAbortedPreDurablePayload {
    FSessionOperationIdentity identity{};
    uint64_t cancellation_observation = 0; // the owner proof event
    uint16_t reason = 0;
    [[nodiscard]] bool valid() const noexcept {
        return identity.valid() && cancellation_observation != 0;
    }
};

struct InputCancelledAfterCommitPayload {
    FSessionOperationIdentity identity{};
    uint64_t ready_event_id = 0; // retained durable bundle
    uint8_t delivery_suppressed = 0;
    [[nodiscard]] bool valid() const noexcept {
        return identity.valid() && ready_event_id != 0;
    }
};

struct DeliveryOfferPayload {
    FSessionOperationIdentity identity{};
    uint64_t attachment_delivery_id = 0;  // stable across retries
    uint64_t attachment_admission_id = 0;
    uint64_t ready_event_id = 0;
    uint64_t ancillary_attempt_id = 0;    // fresh per rendezvous attempt
    [[nodiscard]] bool valid() const noexcept {
        return identity.valid() && attachment_delivery_id != 0 &&
               attachment_admission_id != 0 && ready_event_id != 0 &&
               ancillary_attempt_id != 0;
    }
};

struct TerminalObservationPayload {
    FSessionOperationIdentity identity{};
    uint16_t terminal_class = 0; // nonzero typed terminal classification
    uint64_t ready_event_id = 0; // 0 when no durable bundle exists
    uint16_t delivery_state = 0;
    uint64_t highest_accepted_daemon_sequence = 0;
    [[nodiscard]] bool valid() const noexcept {
        return identity.valid() && terminal_class != 0 &&
               highest_accepted_daemon_sequence != 0;
    }
};

// --- codec ------------------------------------------------------------------
// encode_*: nullopt on an invalid payload. decode_*: validates the envelope's
// message type, the payload identity against the envelope identity, exact
// length (no trailing bytes), and payload validity.

#define ICECC_P50_FSESSION_PAYLOAD_CODEC(Name)                                 \
    [[nodiscard]] std::optional<std::vector<uint8_t>> encode_##Name(           \
        const Name##Payload& payload);                                         \
    [[nodiscard]] std::optional<Name##Payload> decode_##Name(                  \
        const FSessionControlEnvelope& envelope);

ICECC_P50_FSESSION_PAYLOAD_CODEC(OperationOffer)
ICECC_P50_FSESSION_PAYLOAD_CODEC(OpCancel)
ICECC_P50_FSESSION_PAYLOAD_CODEC(PublicFdOffer)
ICECC_P50_FSESSION_PAYLOAD_CODEC(DaemonFdAccepted)
ICECC_P50_FSESSION_PAYLOAD_CODEC(DaemonFdRejected)
ICECC_P50_FSESSION_PAYLOAD_CODEC(TerminalAck)
ICECC_P50_FSESSION_PAYLOAD_CODEC(OperationAccepted)
ICECC_P50_FSESSION_PAYLOAD_CODEC(PublicFdAdoptedReceipt)
ICECC_P50_FSESSION_PAYLOAD_CODEC(EndpointObservation)
ICECC_P50_FSESSION_PAYLOAD_CODEC(InputCommitted)
ICECC_P50_FSESSION_PAYLOAD_CODEC(InputAbortedPreDurable)
ICECC_P50_FSESSION_PAYLOAD_CODEC(InputCancelledAfterCommit)
ICECC_P50_FSESSION_PAYLOAD_CODEC(DeliveryOffer)
ICECC_P50_FSESSION_PAYLOAD_CODEC(TerminalObservation)

#undef ICECC_P50_FSESSION_PAYLOAD_CODEC

} // namespace icecc::p50::fsession

#endif // ICECC_CACHE_P50_FSESSION_PAYLOADS_H
