// Focused unit test for the FSession control envelope/codec (S2 vertical
// foundation). Proves exact round-trip, direction legality as a type property,
// full-identity + sequence + no-trailing-byte validation (issue-16 5444410383).

#include "cache/p50_fsession_control.h"

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
    id.deadline.expires_at_ns = 123456789;
    id.deadline.clock_domain_id = 5;
    id.deadline.time_namespace_id = 9;
    return id;
}
} // namespace

int main() {
    check(make_identity().valid(), "constructed identity is valid");

    FSessionControlEnvelope e;
    e.identity = make_identity();
    e.direction = FSessionControlDirection::DaemonToSidecar;
    e.message_type = static_cast<uint16_t>(DaemonToSidecarType::OperationOffer);
    e.sequence = 1;
    e.payload = {0xAA, 0xBB, 0xCC};

    const auto enc = encode_fsession_control(e);
    check(enc.has_value(), "encode valid envelope");
    if (enc.has_value()) {
        const auto dec = decode_fsession_control(*enc);
        check(dec.has_value(), "decode valid envelope");
        if (dec.has_value()) {
            check(dec->identity == e.identity, "identity round-trips exactly");
            check(dec->direction == e.direction &&
                      dec->message_type == e.message_type &&
                      dec->sequence == e.sequence,
                  "header round-trips");
            check(dec->payload == e.payload, "payload round-trips");
        }
        auto trailing = *enc;
        trailing.push_back(0);
        check(!decode_fsession_control(trailing).has_value(),
              "trailing byte rejected (no smuggled second frame)");
        auto bad_magic = *enc;
        bad_magic[0] ^= 0xFFu;
        check(!decode_fsession_control(bad_magic).has_value(),
              "corrupt magic rejected");
    }

    // Direction legality is a type property: a sidecar-only type (8) in a
    // Daemon->Sidecar envelope is illegal (TerminalAck stays reachable at 6).
    FSessionControlEnvelope wrong_dir = e;
    wrong_dir.message_type = 8;
    check(!encode_fsession_control(wrong_dir).has_value(),
          "sidecar-only type rejected in daemon direction");

    // TerminalAck (6) is a legal, reachable Daemon->Sidecar frame.
    FSessionControlEnvelope term_ack = e;
    term_ack.message_type = static_cast<uint16_t>(DaemonToSidecarType::TerminalAck);
    check(encode_fsession_control(term_ack).has_value(),
          "TerminalAck reachable in daemon direction");

    FSessionControlEnvelope zero_seq = e;
    zero_seq.sequence = 0;
    check(!encode_fsession_control(zero_seq).has_value(), "sequence 0 rejected");

    FSessionControlEnvelope bad_id = e;
    bad_id.identity.assignment_job = 0;
    check(!encode_fsession_control(bad_id).has_value(),
          "incomplete identity rejected");

    if (g_fail != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("PASS p50fsessioncontrol\n");
    return 0;
}
