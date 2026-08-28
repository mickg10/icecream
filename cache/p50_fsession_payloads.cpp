#include "p50_fsession_payloads.h"

namespace icecc::p50::fsession {
namespace {

// Local big-endian helpers (mirror the control codec's canonical encoding).
void put_u8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }
void put_u16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v));
}
void put_u64(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 7; i >= 0; --i)
        b.push_back(static_cast<uint8_t>(v >> (i * 8)));
}

struct Reader {
    std::span<const uint8_t> data;
    size_t off = 0;
    bool ok = true;
    bool need(size_t n) noexcept {
        if (!ok || off + n > data.size()) {
            ok = false;
            return false;
        }
        return true;
    }
    uint8_t u8() noexcept {
        if (!need(1))
            return 0;
        return data[off++];
    }
    uint16_t u16() noexcept {
        if (!need(2))
            return 0;
        uint16_t v = static_cast<uint16_t>((data[off] << 8) | data[off + 1]);
        off += 2;
        return v;
    }
    uint64_t u64() noexcept {
        if (!need(8))
            return 0;
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v = (v << 8) | data[off + i];
        off += 8;
        return v;
    }
    void bytes16(std::array<uint8_t, 16>& out) noexcept {
        if (!need(16))
            return;
        for (int i = 0; i < 16; ++i)
            out[i] = data[off + i];
        off += 16;
    }
};

// Shared decode preamble: message-type check, identity decode + envelope match.
template <typename Payload>
bool begin_decode(const FSessionControlEnvelope& envelope,
                  uint16_t expected_type, Reader& reader, Payload& payload) {
    if (envelope.message_type != expected_type)
        return false;
    size_t offset = 0;
    if (!decode_fsession_identity(envelope.payload, offset, payload.identity))
        return false;
    if (!(payload.identity == envelope.identity))
        return false; // payload identity must re-bind the envelope identity
    reader = Reader{envelope.payload, offset, true};
    return true;
}

template <typename Payload>
std::optional<Payload> finish_decode(Reader& reader, Payload payload) {
    if (!reader.ok || reader.off != reader.data.size())
        return std::nullopt; // short or trailing bytes
    if (!payload.valid())
        return std::nullopt;
    return payload;
}

} // namespace

// --- Daemon -> Sidecar ------------------------------------------------------

std::optional<std::vector<uint8_t>>
encode_OperationOffer(const OperationOfferPayload& p) {
    if (!p.valid())
        return std::nullopt;
    std::vector<uint8_t> b;
    encode_fsession_identity(b, p.identity);
    put_u64(b, p.client_connection_generation);
    put_u64(b, p.compile_file_lease);
    put_u64(b, p.wait_reservation);
    put_u64(b, p.consumed_claim_capability);
    put_u8(b, p.predecessor.cold ? 1 : 0);
    put_u64(b, p.predecessor.rel_seq);
    b.insert(b.end(), p.predecessor.state_digest.begin(),
             p.predecessor.state_digest.end());
    return b;
}

std::optional<OperationOfferPayload>
decode_OperationOffer(const FSessionControlEnvelope& e) {
    OperationOfferPayload p;
    Reader r;
    if (!begin_decode(e, static_cast<uint16_t>(DaemonToSidecarType::OperationOffer),
                      r, p))
        return std::nullopt;
    p.client_connection_generation = r.u64();
    p.compile_file_lease = r.u64();
    p.wait_reservation = r.u64();
    p.consumed_claim_capability = r.u64();
    p.predecessor.cold = r.u8() != 0;
    p.predecessor.rel_seq = r.u64();
    r.bytes16(p.predecessor.state_digest);
    return finish_decode(r, std::move(p));
}

std::optional<std::vector<uint8_t>> encode_OpCancel(const OpCancelPayload& p) {
    if (!p.valid())
        return std::nullopt;
    std::vector<uint8_t> b;
    encode_fsession_identity(b, p.identity);
    put_u16(b, p.reason);
    put_u64(b, p.cancellation_observation);
    return b;
}

std::optional<OpCancelPayload>
decode_OpCancel(const FSessionControlEnvelope& e) {
    OpCancelPayload p;
    Reader r;
    if (!begin_decode(e, static_cast<uint16_t>(DaemonToSidecarType::OpCancel), r,
                      p))
        return std::nullopt;
    p.reason = r.u16();
    p.cancellation_observation = r.u64();
    return finish_decode(r, std::move(p));
}

std::optional<std::vector<uint8_t>>
encode_PublicFdOffer(const PublicFdOfferPayload& p) {
    if (!p.valid())
        return std::nullopt;
    std::vector<uint8_t> b;
    encode_fsession_identity(b, p.identity);
    put_u64(b, p.public_fd_offer_id);
    put_u64(b, p.ancillary_attempt_id);
    put_u64(b, p.socket_cookie);
    return b;
}

std::optional<PublicFdOfferPayload>
decode_PublicFdOffer(const FSessionControlEnvelope& e) {
    PublicFdOfferPayload p;
    Reader r;
    if (!begin_decode(e, static_cast<uint16_t>(DaemonToSidecarType::PublicFdOffer),
                      r, p))
        return std::nullopt;
    p.public_fd_offer_id = r.u64();
    p.ancillary_attempt_id = r.u64();
    p.socket_cookie = r.u64();
    return finish_decode(r, std::move(p));
}

std::optional<std::vector<uint8_t>>
encode_DaemonFdAccepted(const DaemonFdAcceptedPayload& p) {
    if (!p.valid())
        return std::nullopt;
    std::vector<uint8_t> b;
    encode_fsession_identity(b, p.identity);
    put_u64(b, p.attachment_delivery_id);
    put_u64(b, p.acceptance_receipt_id);
    put_u8(b, p.replay);
    return b;
}

std::optional<DaemonFdAcceptedPayload>
decode_DaemonFdAccepted(const FSessionControlEnvelope& e) {
    DaemonFdAcceptedPayload p;
    Reader r;
    if (!begin_decode(e,
                      static_cast<uint16_t>(DaemonToSidecarType::DaemonFdAccepted),
                      r, p))
        return std::nullopt;
    p.attachment_delivery_id = r.u64();
    p.acceptance_receipt_id = r.u64();
    p.replay = r.u8();
    return finish_decode(r, std::move(p));
}

std::optional<std::vector<uint8_t>>
encode_DaemonFdRejected(const DaemonFdRejectedPayload& p) {
    if (!p.valid())
        return std::nullopt;
    std::vector<uint8_t> b;
    encode_fsession_identity(b, p.identity);
    put_u64(b, p.attachment_delivery_id);
    put_u16(b, p.reason);
    return b;
}

std::optional<DaemonFdRejectedPayload>
decode_DaemonFdRejected(const FSessionControlEnvelope& e) {
    DaemonFdRejectedPayload p;
    Reader r;
    if (!begin_decode(e,
                      static_cast<uint16_t>(DaemonToSidecarType::DaemonFdRejected),
                      r, p))
        return std::nullopt;
    p.attachment_delivery_id = r.u64();
    p.reason = r.u16();
    return finish_decode(r, std::move(p));
}

std::optional<std::vector<uint8_t>>
encode_TerminalAck(const TerminalAckPayload& p) {
    if (!p.valid())
        return std::nullopt;
    std::vector<uint8_t> b;
    encode_fsession_identity(b, p.identity);
    put_u64(b, p.ack_of_sidecar_sequence);
    put_u16(b, p.terminal_class_echo);
    put_u64(b, p.settlement_id);
    return b;
}

std::optional<TerminalAckPayload>
decode_TerminalAck(const FSessionControlEnvelope& e) {
    TerminalAckPayload p;
    Reader r;
    if (!begin_decode(e, static_cast<uint16_t>(DaemonToSidecarType::TerminalAck),
                      r, p))
        return std::nullopt;
    p.ack_of_sidecar_sequence = r.u64();
    p.terminal_class_echo = r.u16();
    p.settlement_id = r.u64();
    return finish_decode(r, std::move(p));
}

// --- Sidecar -> Daemon ------------------------------------------------------

std::optional<std::vector<uint8_t>>
encode_OperationAccepted(const OperationAcceptedPayload& p) {
    if (!p.valid())
        return std::nullopt;
    std::vector<uint8_t> b;
    encode_fsession_identity(b, p.identity);
    put_u64(b, p.ack_of_daemon_sequence);
    put_u64(b, p.sidecar_store_generation);
    return b;
}

std::optional<OperationAcceptedPayload>
decode_OperationAccepted(const FSessionControlEnvelope& e) {
    OperationAcceptedPayload p;
    Reader r;
    if (!begin_decode(
            e, static_cast<uint16_t>(SidecarToDaemonType::OperationAccepted), r,
            p))
        return std::nullopt;
    p.ack_of_daemon_sequence = r.u64();
    p.sidecar_store_generation = r.u64();
    return finish_decode(r, std::move(p));
}

std::optional<std::vector<uint8_t>>
encode_PublicFdAdoptedReceipt(const PublicFdAdoptedReceiptPayload& p) {
    if (!p.valid())
        return std::nullopt;
    std::vector<uint8_t> b;
    encode_fsession_identity(b, p.identity);
    put_u64(b, p.public_fd_offer_id);
    put_u64(b, p.endpoint_generation);
    put_u64(b, p.endpoint_session_serial);
    put_u64(b, p.run_sequence);
    put_u64(b, p.socket_ownership_generation);
    put_u64(b, p.route_admission_sequence);
    put_u64(b, p.sidecar_owner_sequence);
    return b;
}

std::optional<PublicFdAdoptedReceiptPayload>
decode_PublicFdAdoptedReceipt(const FSessionControlEnvelope& e) {
    PublicFdAdoptedReceiptPayload p;
    Reader r;
    if (!begin_decode(
            e,
            static_cast<uint16_t>(SidecarToDaemonType::PublicFdAdoptedReceipt),
            r, p))
        return std::nullopt;
    p.public_fd_offer_id = r.u64();
    p.endpoint_generation = r.u64();
    p.endpoint_session_serial = r.u64();
    p.run_sequence = r.u64();
    p.socket_ownership_generation = r.u64();
    p.route_admission_sequence = r.u64();
    p.sidecar_owner_sequence = r.u64();
    return finish_decode(r, std::move(p));
}

std::optional<std::vector<uint8_t>>
encode_EndpointObservation(const EndpointObservationPayload& p) {
    if (!p.valid())
        return std::nullopt;
    std::vector<uint8_t> b;
    encode_fsession_identity(b, p.identity);
    put_u64(b, p.run_sequence);
    put_u16(b, static_cast<uint16_t>(p.event));
    put_u64(b, p.progress);
    put_u64(b, p.cancellation_observation);
    return b;
}

std::optional<EndpointObservationPayload>
decode_EndpointObservation(const FSessionControlEnvelope& e) {
    EndpointObservationPayload p;
    Reader r;
    if (!begin_decode(
            e, static_cast<uint16_t>(SidecarToDaemonType::EndpointObservation),
            r, p))
        return std::nullopt;
    p.run_sequence = r.u64();
    p.event = static_cast<EndpointEventKind>(r.u16());
    p.progress = r.u64();
    p.cancellation_observation = r.u64();
    return finish_decode(r, std::move(p));
}

std::optional<std::vector<uint8_t>>
encode_InputCommitted(const InputCommittedPayload& p) {
    if (!p.valid())
        return std::nullopt;
    std::vector<uint8_t> b;
    encode_fsession_identity(b, p.identity);
    put_u64(b, p.ready_event_id);
    put_u64(b, p.backing_id);
    put_u64(b, p.successor_rel_seq);
    b.insert(b.end(), p.successor_digest.begin(), p.successor_digest.end());
    put_u64(b, p.committed_receipt_id);
    return b;
}

std::optional<InputCommittedPayload>
decode_InputCommitted(const FSessionControlEnvelope& e) {
    InputCommittedPayload p;
    Reader r;
    if (!begin_decode(e,
                      static_cast<uint16_t>(SidecarToDaemonType::InputCommitted),
                      r, p))
        return std::nullopt;
    p.ready_event_id = r.u64();
    p.backing_id = r.u64();
    p.successor_rel_seq = r.u64();
    r.bytes16(p.successor_digest);
    p.committed_receipt_id = r.u64();
    return finish_decode(r, std::move(p));
}

std::optional<std::vector<uint8_t>>
encode_InputAbortedPreDurable(const InputAbortedPreDurablePayload& p) {
    if (!p.valid())
        return std::nullopt;
    std::vector<uint8_t> b;
    encode_fsession_identity(b, p.identity);
    put_u64(b, p.cancellation_observation);
    put_u16(b, p.reason);
    return b;
}

std::optional<InputAbortedPreDurablePayload>
decode_InputAbortedPreDurable(const FSessionControlEnvelope& e) {
    InputAbortedPreDurablePayload p;
    Reader r;
    if (!begin_decode(
            e,
            static_cast<uint16_t>(SidecarToDaemonType::InputAbortedPreDurable),
            r, p))
        return std::nullopt;
    p.cancellation_observation = r.u64();
    p.reason = r.u16();
    return finish_decode(r, std::move(p));
}

std::optional<std::vector<uint8_t>>
encode_InputCancelledAfterCommit(const InputCancelledAfterCommitPayload& p) {
    if (!p.valid())
        return std::nullopt;
    std::vector<uint8_t> b;
    encode_fsession_identity(b, p.identity);
    put_u64(b, p.ready_event_id);
    put_u8(b, p.delivery_suppressed);
    return b;
}

std::optional<InputCancelledAfterCommitPayload>
decode_InputCancelledAfterCommit(const FSessionControlEnvelope& e) {
    InputCancelledAfterCommitPayload p;
    Reader r;
    if (!begin_decode(
            e,
            static_cast<uint16_t>(
                SidecarToDaemonType::InputCancelledAfterCommit),
            r, p))
        return std::nullopt;
    p.ready_event_id = r.u64();
    p.delivery_suppressed = r.u8();
    return finish_decode(r, std::move(p));
}

std::optional<std::vector<uint8_t>>
encode_DeliveryOffer(const DeliveryOfferPayload& p) {
    if (!p.valid())
        return std::nullopt;
    std::vector<uint8_t> b;
    encode_fsession_identity(b, p.identity);
    put_u64(b, p.attachment_delivery_id);
    put_u64(b, p.attachment_admission_id);
    put_u64(b, p.ready_event_id);
    put_u64(b, p.ancillary_attempt_id);
    return b;
}

std::optional<DeliveryOfferPayload>
decode_DeliveryOffer(const FSessionControlEnvelope& e) {
    DeliveryOfferPayload p;
    Reader r;
    if (!begin_decode(e,
                      static_cast<uint16_t>(SidecarToDaemonType::DeliveryOffer),
                      r, p))
        return std::nullopt;
    p.attachment_delivery_id = r.u64();
    p.attachment_admission_id = r.u64();
    p.ready_event_id = r.u64();
    p.ancillary_attempt_id = r.u64();
    return finish_decode(r, std::move(p));
}

std::optional<std::vector<uint8_t>>
encode_TerminalObservation(const TerminalObservationPayload& p) {
    if (!p.valid())
        return std::nullopt;
    std::vector<uint8_t> b;
    encode_fsession_identity(b, p.identity);
    put_u16(b, p.terminal_class);
    put_u64(b, p.ready_event_id);
    put_u16(b, p.delivery_state);
    put_u64(b, p.highest_accepted_daemon_sequence);
    return b;
}

std::optional<TerminalObservationPayload>
decode_TerminalObservation(const FSessionControlEnvelope& e) {
    TerminalObservationPayload p;
    Reader r;
    if (!begin_decode(
            e, static_cast<uint16_t>(SidecarToDaemonType::TerminalObservation),
            r, p))
        return std::nullopt;
    p.terminal_class = r.u16();
    p.ready_event_id = r.u64();
    p.delivery_state = r.u16();
    p.highest_accepted_daemon_sequence = r.u64();
    return finish_decode(r, std::move(p));
}

} // namespace icecc::p50::fsession
