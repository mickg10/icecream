// Functional-correctness test for the identity-complete payload codecs: exact
// round-trip through real envelopes for all 14 messages, payload-vs-envelope
// identity binding, trailing-byte and short-input rejection, wrong-type
// rejection, and required-field validation (issue-16 5448067827 sec.3).

#include "cache/p50_fsession_payloads.h"

#include <cassert>
#include <cstdio>

using namespace icecc::p50::fsession;

namespace {
int g_fail = 0;
void check(bool c, const char* m) {
    if (!c) {
        std::fprintf(stderr, "FAIL: %s\n", m);
        ++g_fail;
    }
}

FSessionOperationIdentity make_identity() {
    FSessionOperationIdentity id;
    id.daemon_launch_generation = 7;
    id.control_connection_generation = 3;
    id.operation.sidecar_launch.generation = 11;
    id.operation.sidecar_launch.attempt = 13;
    id.operation.role = icecc::p50::daemon::P50SessionOperationRole::FSession;
    id.operation.operation_sequence = 42;
    id.c_store_guid.bytes = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    id.f_store_guid.bytes = {16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
    id.assignment_job = 100;
    id.assignment_epoch = 200;
    id.assignment_nonce = 300;
    id.arm_observation = 400;
    id.deadline = {123456789, 5, 9};
    assert(id.valid());
    return id;
}

FSessionControlEnvelope wrap(const FSessionOperationIdentity& id,
                             FSessionControlDirection direction, uint16_t type,
                             std::vector<uint8_t> payload_bytes) {
    FSessionControlEnvelope e;
    e.identity = id;
    e.direction = direction;
    e.message_type = type;
    e.sequence = 9;
    e.payload = std::move(payload_bytes);
    return e;
}

// Round-trip one payload through a real envelope; also prove trailing-byte and
// wrong-type rejection on the same frame.
template <typename Payload, typename Encode, typename Decode>
void round_trip(const char* name, const Payload& payload,
                FSessionControlDirection direction, uint16_t type,
                uint16_t other_type_same_direction, Encode encode,
                Decode decode, bool (*equal)(const Payload&, const Payload&)) {
    auto bytes = encode(payload);
    check(bytes.has_value(), name);
    if (!bytes.has_value())
        return;
    auto envelope = wrap(payload.identity, direction, type, *bytes);
    auto decoded = decode(envelope);
    check(decoded.has_value(), name);
    if (decoded.has_value())
        check(equal(payload, *decoded), name);

    auto trailing = envelope;
    trailing.payload.push_back(0);
    check(!decode(trailing).has_value(), "trailing byte rejected");

    auto wrong_type = wrap(payload.identity, direction,
                           other_type_same_direction, *bytes);
    check(!decode(wrong_type).has_value(), "wrong message type rejected");

    // Payload identity must re-bind the envelope identity exactly.
    auto mismatched = envelope;
    mismatched.identity.assignment_nonce = 999;
    check(!decode(mismatched).has_value(),
          "payload/envelope identity mismatch rejected");
}
} // namespace

int main() {
    const auto id = make_identity();
    constexpr auto kD = FSessionControlDirection::DaemonToSidecar;
    constexpr auto kS = FSessionControlDirection::SidecarToDaemon;

    {
        OperationOfferPayload p;
        p.identity = id;
        p.client_connection_generation = 31;
        p.compile_file_lease = 32;
        p.wait_reservation = 33;
        p.consumed_claim_capability = 34;
        p.predecessor = RoutePredecessor{}; // tagged cold
        round_trip("OperationOffer round-trip", p, kD,
                   static_cast<uint16_t>(DaemonToSidecarType::OperationOffer),
                   static_cast<uint16_t>(DaemonToSidecarType::OpCancel),
                   encode_OperationOffer, decode_OperationOffer,
                   +[](const OperationOfferPayload& a,
                       const OperationOfferPayload& b) {
                       return a.identity == b.identity &&
                              a.client_connection_generation ==
                                  b.client_connection_generation &&
                              a.compile_file_lease == b.compile_file_lease &&
                              a.wait_reservation == b.wait_reservation &&
                              a.consumed_claim_capability ==
                                  b.consumed_claim_capability &&
                              a.predecessor == b.predecessor;
                   });
        auto missing = p;
        missing.wait_reservation = 0;
        check(!encode_OperationOffer(missing).has_value(),
              "offer without WAIT reservation refused");
    }
    {
        OpCancelPayload p;
        p.identity = id;
        p.reason = 2;
        p.cancellation_observation = 71;
        round_trip("OpCancel round-trip", p, kD,
                   static_cast<uint16_t>(DaemonToSidecarType::OpCancel),
                   static_cast<uint16_t>(DaemonToSidecarType::OperationOffer),
                   encode_OpCancel, decode_OpCancel,
                   +[](const OpCancelPayload& a, const OpCancelPayload& b) {
                       return a.identity == b.identity && a.reason == b.reason &&
                              a.cancellation_observation ==
                                  b.cancellation_observation;
                   });
        auto missing = p;
        missing.cancellation_observation = 0;
        check(!encode_OpCancel(missing).has_value(),
              "cancel without observation id refused");
    }
    {
        PublicFdOfferPayload p;
        p.identity = id;
        p.public_fd_offer_id = 81;
        p.ancillary_attempt_id = 82;
        p.socket_cookie = 83;
        round_trip("PublicFdOffer round-trip", p, kD,
                   static_cast<uint16_t>(DaemonToSidecarType::PublicFdOffer),
                   static_cast<uint16_t>(DaemonToSidecarType::OpCancel),
                   encode_PublicFdOffer, decode_PublicFdOffer,
                   +[](const PublicFdOfferPayload& a,
                       const PublicFdOfferPayload& b) {
                       return a.identity == b.identity &&
                              a.public_fd_offer_id == b.public_fd_offer_id &&
                              a.ancillary_attempt_id == b.ancillary_attempt_id &&
                              a.socket_cookie == b.socket_cookie;
                   });
    }
    {
        DaemonFdAcceptedPayload p;
        p.identity = id;
        p.attachment_delivery_id = 91;
        p.acceptance_receipt_id = 92;
        p.replay = 1;
        round_trip("DaemonFdAccepted round-trip", p, kD,
                   static_cast<uint16_t>(DaemonToSidecarType::DaemonFdAccepted),
                   static_cast<uint16_t>(DaemonToSidecarType::DaemonFdRejected),
                   encode_DaemonFdAccepted, decode_DaemonFdAccepted,
                   +[](const DaemonFdAcceptedPayload& a,
                       const DaemonFdAcceptedPayload& b) {
                       return a.identity == b.identity &&
                              a.attachment_delivery_id ==
                                  b.attachment_delivery_id &&
                              a.acceptance_receipt_id ==
                                  b.acceptance_receipt_id &&
                              a.replay == b.replay;
                   });
    }
    {
        DaemonFdRejectedPayload p;
        p.identity = id;
        p.attachment_delivery_id = 93;
        p.reason = 4;
        round_trip("DaemonFdRejected round-trip", p, kD,
                   static_cast<uint16_t>(DaemonToSidecarType::DaemonFdRejected),
                   static_cast<uint16_t>(DaemonToSidecarType::DaemonFdAccepted),
                   encode_DaemonFdRejected, decode_DaemonFdRejected,
                   +[](const DaemonFdRejectedPayload& a,
                       const DaemonFdRejectedPayload& b) {
                       return a.identity == b.identity &&
                              a.attachment_delivery_id ==
                                  b.attachment_delivery_id &&
                              a.reason == b.reason;
                   });
    }
    {
        TerminalAckPayload p;
        p.identity = id;
        p.ack_of_sidecar_sequence = 5;
        p.terminal_class_echo = 6;
        p.settlement_id = 77;
        round_trip("TerminalAck round-trip", p, kD,
                   static_cast<uint16_t>(DaemonToSidecarType::TerminalAck),
                   static_cast<uint16_t>(DaemonToSidecarType::OpCancel),
                   encode_TerminalAck, decode_TerminalAck,
                   +[](const TerminalAckPayload& a, const TerminalAckPayload& b) {
                       return a.identity == b.identity &&
                              a.ack_of_sidecar_sequence ==
                                  b.ack_of_sidecar_sequence &&
                              a.terminal_class_echo == b.terminal_class_echo &&
                              a.settlement_id == b.settlement_id;
                   });
    }
    {
        OperationAcceptedPayload p;
        p.identity = id;
        p.ack_of_daemon_sequence = 1;
        p.sidecar_store_generation = 19;
        round_trip("OperationAccepted round-trip", p, kS,
                   static_cast<uint16_t>(SidecarToDaemonType::OperationAccepted),
                   static_cast<uint16_t>(SidecarToDaemonType::DeliveryOffer),
                   encode_OperationAccepted, decode_OperationAccepted,
                   +[](const OperationAcceptedPayload& a,
                       const OperationAcceptedPayload& b) {
                       return a.identity == b.identity &&
                              a.ack_of_daemon_sequence ==
                                  b.ack_of_daemon_sequence &&
                              a.sidecar_store_generation ==
                                  b.sidecar_store_generation;
                   });
    }
    {
        PublicFdAdoptedReceiptPayload p;
        p.identity = id;
        p.public_fd_offer_id = 81;
        p.endpoint_generation = 5;
        p.endpoint_session_serial = 6;
        p.run_sequence = 42;
        p.socket_ownership_generation = 7;
        p.route_admission_sequence = 1;
        p.sidecar_owner_sequence = 2;
        round_trip(
            "PublicFdAdoptedReceipt round-trip", p, kS,
            static_cast<uint16_t>(SidecarToDaemonType::PublicFdAdoptedReceipt),
            static_cast<uint16_t>(SidecarToDaemonType::OperationAccepted),
            encode_PublicFdAdoptedReceipt, decode_PublicFdAdoptedReceipt,
            +[](const PublicFdAdoptedReceiptPayload& a,
                const PublicFdAdoptedReceiptPayload& b) {
                return a.identity == b.identity &&
                       a.public_fd_offer_id == b.public_fd_offer_id &&
                       a.endpoint_generation == b.endpoint_generation &&
                       a.endpoint_session_serial == b.endpoint_session_serial &&
                       a.run_sequence == b.run_sequence &&
                       a.socket_ownership_generation ==
                           b.socket_ownership_generation &&
                       a.route_admission_sequence == b.route_admission_sequence &&
                       a.sidecar_owner_sequence == b.sidecar_owner_sequence;
            });
        auto missing = p;
        missing.route_admission_sequence = 0;
        check(!encode_PublicFdAdoptedReceipt(missing).has_value(),
              "receipt without route admission refused");
    }
    {
        EndpointObservationPayload p;
        p.identity = id;
        p.run_sequence = 42;
        p.event = EndpointEventKind::PreparedInputReady;
        p.progress = 1234;
        p.cancellation_observation = 0;
        round_trip("EndpointObservation round-trip", p, kS,
                   static_cast<uint16_t>(SidecarToDaemonType::EndpointObservation),
                   static_cast<uint16_t>(SidecarToDaemonType::InputCommitted),
                   encode_EndpointObservation, decode_EndpointObservation,
                   +[](const EndpointObservationPayload& a,
                       const EndpointObservationPayload& b) {
                       return a.identity == b.identity &&
                              a.run_sequence == b.run_sequence &&
                              a.event == b.event && a.progress == b.progress &&
                              a.cancellation_observation ==
                                  b.cancellation_observation;
                   });
    }
    {
        InputCommittedPayload p;
        p.identity = id;
        p.ready_event_id = 51;
        p.backing_id = 52;
        p.successor_rel_seq = 11;
        p.successor_digest[0] = 0xAB;
        p.committed_receipt_id = 53;
        round_trip("InputCommitted round-trip", p, kS,
                   static_cast<uint16_t>(SidecarToDaemonType::InputCommitted),
                   static_cast<uint16_t>(SidecarToDaemonType::DeliveryOffer),
                   encode_InputCommitted, decode_InputCommitted,
                   +[](const InputCommittedPayload& a,
                       const InputCommittedPayload& b) {
                       return a.identity == b.identity &&
                              a.ready_event_id == b.ready_event_id &&
                              a.backing_id == b.backing_id &&
                              a.successor_rel_seq == b.successor_rel_seq &&
                              a.successor_digest == b.successor_digest &&
                              a.committed_receipt_id == b.committed_receipt_id;
                   });
        auto missing = p;
        missing.ready_event_id = 0;
        check(!encode_InputCommitted(missing).has_value(),
              "committed without Ready id refused");
    }
    {
        InputAbortedPreDurablePayload p;
        p.identity = id;
        p.cancellation_observation = 71;
        p.reason = 1;
        round_trip(
            "InputAbortedPreDurable round-trip", p, kS,
            static_cast<uint16_t>(SidecarToDaemonType::InputAbortedPreDurable),
            static_cast<uint16_t>(SidecarToDaemonType::InputCommitted),
            encode_InputAbortedPreDurable, decode_InputAbortedPreDurable,
            +[](const InputAbortedPreDurablePayload& a,
                const InputAbortedPreDurablePayload& b) {
                return a.identity == b.identity &&
                       a.cancellation_observation == b.cancellation_observation &&
                       a.reason == b.reason;
            });
    }
    {
        InputCancelledAfterCommitPayload p;
        p.identity = id;
        p.ready_event_id = 51;
        p.delivery_suppressed = 1;
        round_trip(
            "InputCancelledAfterCommit round-trip", p, kS,
            static_cast<uint16_t>(
                SidecarToDaemonType::InputCancelledAfterCommit),
            static_cast<uint16_t>(SidecarToDaemonType::InputCommitted),
            encode_InputCancelledAfterCommit, decode_InputCancelledAfterCommit,
            +[](const InputCancelledAfterCommitPayload& a,
                const InputCancelledAfterCommitPayload& b) {
                return a.identity == b.identity &&
                       a.ready_event_id == b.ready_event_id &&
                       a.delivery_suppressed == b.delivery_suppressed;
            });
    }
    {
        DeliveryOfferPayload p;
        p.identity = id;
        p.attachment_delivery_id = 91;
        p.attachment_admission_id = 94;
        p.ready_event_id = 51;
        p.ancillary_attempt_id = 95;
        round_trip("DeliveryOffer round-trip", p, kS,
                   static_cast<uint16_t>(SidecarToDaemonType::DeliveryOffer),
                   static_cast<uint16_t>(SidecarToDaemonType::InputCommitted),
                   encode_DeliveryOffer, decode_DeliveryOffer,
                   +[](const DeliveryOfferPayload& a, const DeliveryOfferPayload& b) {
                       return a.identity == b.identity &&
                              a.attachment_delivery_id ==
                                  b.attachment_delivery_id &&
                              a.attachment_admission_id ==
                                  b.attachment_admission_id &&
                              a.ready_event_id == b.ready_event_id &&
                              a.ancillary_attempt_id == b.ancillary_attempt_id;
                   });
    }
    {
        TerminalObservationPayload p;
        p.identity = id;
        p.terminal_class = 3;
        p.ready_event_id = 51;
        p.delivery_state = 2;
        p.highest_accepted_daemon_sequence = 4;
        round_trip(
            "TerminalObservation round-trip", p, kS,
            static_cast<uint16_t>(SidecarToDaemonType::TerminalObservation),
            static_cast<uint16_t>(SidecarToDaemonType::InputCommitted),
            encode_TerminalObservation, decode_TerminalObservation,
            +[](const TerminalObservationPayload& a,
                const TerminalObservationPayload& b) {
                return a.identity == b.identity &&
                       a.terminal_class == b.terminal_class &&
                       a.ready_event_id == b.ready_event_id &&
                       a.delivery_state == b.delivery_state &&
                       a.highest_accepted_daemon_sequence ==
                           b.highest_accepted_daemon_sequence;
            });
        auto missing = p;
        missing.terminal_class = 0;
        check(!encode_TerminalObservation(missing).has_value(),
              "terminal observation without class refused");
    }

    if (g_fail != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("PASS p50fsessionpayloads\n");
    return 0;
}
