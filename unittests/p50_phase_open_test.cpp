#include "cache/p50_phase_open.h"
#include "../services/comm.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>

using icecc::p50::AttachmentPhaseOpen;
using icecc::p50::HandoffOffer;
using icecc::p50::OfferDecision;
using icecc::p50::P50HandoffAuthority;
using icecc::p50::P50InputReady;
using icecc::p50::P50PhaseOpenState;
using icecc::p50::P50SourceArm;
using icecc::p50::PhaseOpenResult;
using icecc::p50::PhaseOpenState;

namespace {

void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

P50SourceArm arm() {
    P50SourceArm result;
    result.wire_job_id = 71;
    result.assignment_epoch = 101;
    result.assignment_nonce = 103;
    result.selected_f_host = "f.example";
    result.selected_f_ordinary_port = 8765;
    result.selected_f_cache_port = 9876;
    result.cache_protocol = CACHE_WIRE_REVISION;
    result.cache_profile = CACHE_PROFILE_ZSTD_TU;
    result.logical_job = 107;
    result.attempt_id = 109;
    result.c_store_generation = 113;
    result.c_store_guid.bytes[15] = 127;
    result.source_request_id = 131;
    result.source_mode = P50_SOURCE_MODE_ZSTD_TU;
    return result;
}

HandoffOffer offer(uint64_t request_id = 131) {
    HandoffOffer result;
    result.request_id = request_id;
    result.source_arm = arm();
    result.source_arm.source_request_id = request_id;
    result.cache_profile = result.source_arm.cache_profile;
    return result;
}

AttachmentPhaseOpen phase_open(const HandoffOffer& value) {
    return AttachmentPhaseOpen{value.request_id, value.source_arm};
}

P50InputReady ready_for(const HandoffOffer& value) {
    P50InputReady result;
    result.arm = value.source_arm;
    result.tu_seq.value = 137;
    result.raw_bytes = 139;
    result.raw_digest.bytes[15] = 149;
    result.f_store_guid.bytes[15] = 151;
    result.attachment_store_generation = 157;
    result.attachment_request_id = value.request_id;
    result.ready_event_id = 163;
    return result;
}

void strict_wire() {
    const HandoffOffer expected = offer();
    const auto encoded_offer = icecc::p50::encode_handoff_offer(expected);
    require(!encoded_offer.empty(), "valid HandoffOffer did not encode");
    require(encoded_offer.size() >= 12 && encoded_offer[0] == 'P' &&
                encoded_offer[1] == '5' && encoded_offer[2] == '0' &&
                encoded_offer[3] == 'H' && encoded_offer[4] == 0 &&
                encoded_offer[5] == 50 && encoded_offer[6] == 0 &&
                encoded_offer[7] == 1,
            "HandoffOffer did not use the exact P50H/50/offer frame");
    const auto decoded_offer = icecc::p50::decode_handoff_offer(encoded_offer);
    require(decoded_offer.has_value() && *decoded_offer == expected,
            "HandoffOffer did not round-trip exactly");

    auto trailing = encoded_offer;
    trailing.push_back(0);
    require(!icecc::p50::decode_handoff_offer(trailing),
            "trailing HandoffOffer bytes crossed the framing barrier");
    auto wrong_phase = encoded_offer;
    wrong_phase[7] = 2;
    require(!icecc::p50::decode_handoff_offer(wrong_phase),
            "wrong HandoffOffer phase was accepted");

    const AttachmentPhaseOpen expected_open = phase_open(expected);
    const auto encoded_open = icecc::p50::encode_attachment_phase_open(expected_open);
    require(!encoded_open.empty(), "valid AttachmentPhaseOpen did not encode");
    require(encoded_open.size() >= 12 && encoded_open[0] == 'P' &&
                encoded_open[1] == '5' && encoded_open[2] == '0' &&
                encoded_open[3] == 'H' && encoded_open[4] == 0 &&
                encoded_open[5] == 50 && encoded_open[6] == 0 &&
                encoded_open[7] == 2,
            "AttachmentPhaseOpen did not use the exact P50H/50/open frame");
    const auto decoded_open = icecc::p50::decode_attachment_phase_open(encoded_open);
    require(decoded_open.has_value() && *decoded_open == expected_open,
            "AttachmentPhaseOpen did not round-trip exactly");
    auto wrong_request = encoded_open;
    wrong_request[19] ^= 1;
    require(!icecc::p50::decode_attachment_phase_open(wrong_request),
            "corrupted phase-open request was accepted");

    auto wrong_profile_wire = encoded_offer;
    wrong_profile_wire[20] ^= 1;
    require(!icecc::p50::decode_handoff_offer(wrong_profile_wire),
            "wire profile not bound to the source arm was accepted");

    HandoffOffer wrong_profile = expected;
    wrong_profile.cache_profile++;
    require(icecc::p50::encode_handoff_offer(wrong_profile).empty(),
            "offer with profile not bound to arm was encoded");
}

void global_offer_replay() {
    P50HandoffAuthority service_incarnation;
    // These aliases model two independently-created receiver connections.
    // They deliberately share only the service-incarnation authority.
    P50HandoffAuthority& receiver_one = service_incarnation;
    P50HandoffAuthority& receiver_two = service_incarnation;
    const HandoffOffer first = offer(131);
    require(receiver_one.offer(first) == OfferDecision::Accepted,
            "first offer was not accepted");
    require(receiver_two.request_high_water() == first.request_id,
            "offer high-water did not advance");

    require(receiver_two.offer(first) == OfferDecision::ExactReplay,
            "exact offer replay across receiver was not idempotent");
    HandoffOffer conflict = first;
    conflict.source_arm.attempt_id++;
    require(receiver_two.offer(conflict) == OfferDecision::Conflict,
            "same request with different arm crossed replay authority");
    require(receiver_one.offer(offer(130)) == OfferDecision::StaleRequest,
            "lower request crossed service high-water");

    const AttachmentPhaseOpen open = phase_open(first);
    require(receiver_two.phase_open(open) == OfferDecision::Accepted,
            "phase-open without an accepted offer was not established");
    require(receiver_one.phase_open(open) == OfferDecision::ExactReplay,
            "phase-open replay across receiver was not idempotent");
    AttachmentPhaseOpen wrong = open;
    wrong.source_arm.cache_profile = CACHE_PROFILE_P29V1;
    wrong.source_arm.source_mode = P50_SOURCE_MODE_P29V1;
    require(receiver_one.phase_open(wrong) == OfferDecision::Conflict,
            "wrong phase-open arm/profile was accepted");

    P50HandoffAuthority skipped;
    require(skipped.phase_open(open) == OfferDecision::NoOffer,
            "phase-open-before-offer was accepted");
}

void absolute_deadline_state() {
    using Clock = P50PhaseOpenState::Clock;
    const auto base = Clock::time_point{} + std::chrono::seconds(100);
    const HandoffOffer expected = offer();
    P50PhaseOpenState state;
    require(state.install_offer(expected, base, base + std::chrono::seconds(5),
                                base + std::chrono::seconds(10)) == PhaseOpenResult::Accepted,
            "absolute offer deadlines were rejected");
    require(state.accept_source_arrival(ready_for(expected), base) ==
                PhaseOpenResult::PhaseViolation,
            "source arrived before phase-open ACK");
    require(state.accept_phase_open(phase_open(expected), base + std::chrono::seconds(5)) ==
                PhaseOpenResult::Accepted,
            "phase-open at its absolute deadline was rejected");
    require(state.accept_source_arrival(ready_for(expected),
                                        base + std::chrono::seconds(10)) ==
                PhaseOpenResult::Accepted,
            "source arrival at its absolute deadline was rejected");
    require(state.state() == PhaseOpenState::SourceArrived,
            "accepted source did not enter SourceArrived");

    P50PhaseOpenState expired;
    require(expired.install_offer(expected, base, base + std::chrono::seconds(1),
                                  base + std::chrono::seconds(2)) == PhaseOpenResult::Accepted,
            "second absolute deadline fixture did not install");
    require(expired.accept_phase_open(phase_open(expected), base + std::chrono::seconds(2)) ==
                PhaseOpenResult::DeadlineExpired,
            "late phase-open restarted a relative deadline");
    require(expired.state() == PhaseOpenState::Expired,
            "late phase-open did not terminalize the state");
}

}  // namespace

int main() {
    try {
        strict_wire();
        global_offer_replay();
        absolute_deadline_state();
        std::cout << "ok - strict phase-open/replay/deadline precursor\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
