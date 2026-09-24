#include "p50_input_wait.h"

#include <fcntl.h>
#include <unistd.h>

#include <iostream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

using icecc::p50::Digest128;
using icecc::p50::FStoreGuid;
using icecc::p50::Id128;
using icecc::p50::P50InputReady;
using icecc::p50::P50SourceArm;
using ::P50SourceArmFields;
using icecc::p50::TuSeq;
using icecc::p50::daemon::P50InputWaitState;
using icecc::p50::daemon::P50SourceArmGate;

namespace {

void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

std::string hex(const std::vector<uint8_t>& bytes) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (uint8_t value : bytes)
        out << std::setw(2) << static_cast<unsigned>(value);
    return out.str();
}

P50SourceArm arm() {
    P50SourceArm result;
    result.wire_job_id = 17;
    result.assignment_epoch = 0x0102030405060708ULL;
    result.assignment_nonce = 0x1112131415161718ULL;
    result.selected_f_host = "f.example";
    result.selected_f_ordinary_port = 8765;
    result.selected_f_cache_port = 9876;
    result.cache_protocol = icecc::p50::kP50WireRevision;
    result.cache_profile = 2;
    result.logical_job = 19;
    result.attempt_id = 23;
    result.c_store_generation = 29;
    result.c_store_guid.bytes[15] = 31;
    result.source_request_id = 37;
    result.source_mode = 2;
    return result;
}

P50InputReady ready_for(const P50SourceArm& source_arm) {
    P50InputReady result;
    result.arm = source_arm;
    result.tu_seq = TuSeq{41};
    result.raw_bytes = 43;
    result.raw_digest = Digest128{};
    result.raw_digest.bytes[15] = 47;
    result.f_store_guid.bytes[15] = 53;
    result.attachment_store_generation = 59;
    result.attachment_request_id = source_arm.source_request_id;
    result.ready_event_id = 67;
    return result;
}

P50SourceArmFields canonical_arm() {
    P50SourceArmFields result;
    const P50SourceArm source = arm();
    result.wire_job_id = source.wire_job_id;
    result.assignment_epoch = source.assignment_epoch;
    result.assignment_nonce = source.assignment_nonce;
    result.selected_f_host = source.selected_f_host;
    result.selected_f_ordinary_port = source.selected_f_ordinary_port;
    result.selected_f_cache_port = source.selected_f_cache_port;
    result.cache_protocol = source.cache_protocol;
    result.cache_profile = source.cache_profile;
    result.logical_job = source.logical_job;
    result.compiler_attempt = source.attempt_id;
    result.c_store_generation = source.c_store_generation;
    result.c_store_derivation_version = icecc::p50::kStoreIdentityDerivationVersion;
    result.c_store_guid = source.c_store_guid.bytes;
    result.source_request_id = source.source_request_id;
    result.source_mode = source.source_mode;
    result.c_control_generation = 101;
    result.c_control_attempt = 103;
    return result;
}

void wire_fixture() {
    const P50SourceArm source_arm = arm();
    const auto encoded = icecc::p50::encode_source_arm(source_arm);
    require(!encoded.empty(), "valid source arm did not encode");
    require(encoded.size() < 64u * 1024u, "source arm exceeded fixture cap");
    require(hex(encoded) ==
                "503530410032000100000065000000110102030405060708111213141516171800000009662e6578616d706c650000223d00002694000000010000000200000000000000130000000000000017000000000000001d0000000000000000000000000000001f000000000000002500000002",
            "source arm wire fixture drifted");
    require(encoded[0] == 'P' && encoded[1] == '5' && encoded[2] == '0' &&
                encoded[3] == 'A',
            "source arm magic drifted");
    const auto decoded = icecc::p50::decode_source_arm(encoded);
    require(decoded.has_value() && *decoded == source_arm,
            "source arm fixture did not round-trip");

    const P50InputReady ready = ready_for(source_arm);
    const auto ready_wire = icecc::p50::encode_input_ready(ready);
    require(!ready_wire.empty(), "valid input ready did not encode");
    require(hex(ready_wire) ==
                "5035304100320002000000ad000000110102030405060708111213141516171800000009662e6578616d706c650000223d00002694000000010000000200000000000000130000000000000017000000000000001d0000000000000000000000000000001f0000000000000025000000020000000000000029000000000000002b0000000000000000000000000000002f00000000000000000000000000000035000000000000003b00000000000000250000000000000043",
            "input-ready wire fixture drifted");
    const auto ready_decoded = icecc::p50::decode_input_ready(ready_wire);
    require(ready_decoded.has_value() && *ready_decoded == ready,
            "input ready fixture did not round-trip");

    P50InputReady first_ready = ready;
    first_ready.tu_seq = icecc::p50::TuSeq{0};
    const auto first_ready_wire = icecc::p50::encode_input_ready(first_ready);
    const auto first_ready_decoded =
        icecc::p50::decode_input_ready(first_ready_wire);
    require(!first_ready_wire.empty() && first_ready_decoded.has_value() &&
                *first_ready_decoded == first_ready,
            "zero-based first TU sequence did not round-trip");

    auto wrong_version = encoded;
    wrong_version[5] = 49;
    require(!icecc::p50::decode_source_arm(wrong_version),
            "protocol-49/source-arm version mutant was accepted");
    auto wrong_phase = encoded;
    wrong_phase[7] = 2;
    require(!icecc::p50::decode_source_arm(wrong_phase),
            "input-ready phase was accepted as source arm");

    P50SourceArm wrong_cache_revision = source_arm;
    wrong_cache_revision.cache_protocol = 50;
    require(!wrong_cache_revision.valid() &&
                icecc::p50::encode_source_arm(wrong_cache_revision).empty(),
            "ordinary protocol number was accepted as a CacheWire revision");
    P50SourceArm mismatched_profile_mode = source_arm;
    mismatched_profile_mode.source_mode = 1;
    require(!mismatched_profile_mode.valid() &&
                icecc::p50::encode_source_arm(mismatched_profile_mode).empty(),
            "mismatched source profile and mode were accepted");
    P50SourceArm unknown_profile_mode = source_arm;
    unknown_profile_mode.cache_profile = 8;
    unknown_profile_mode.source_mode = 4;
    require(!unknown_profile_mode.valid() &&
                icecc::p50::encode_source_arm(unknown_profile_mode).empty(),
            "unknown source profile and mode were accepted");
}

void arm_ack_order() {
    const P50SourceArm source_arm = arm();
    P50SourceArmGate gate;
    require(gate.send_arm(source_arm), "source arm was not sent");
    require(!gate.cache_transfer_permitted(),
            "cache transfer started before arm ACK");
    P50SourceArm wrong = source_arm;
    wrong.source_request_id++;
    require(!gate.receive_arm_ack(wrong), "wrong arm ACK was accepted");
    require(!gate.cache_transfer_permitted(), "wrong ACK unlocked transfer");
    require(gate.receive_arm_ack(source_arm), "exact arm ACK was rejected");
    require(gate.cache_transfer_permitted(), "exact arm ACK did not unlock transfer");
}

void wait_state() {
    const P50SourceArm source_arm = arm();
    const P50InputReady ready = ready_for(source_arm);
    P50InputWaitState state;
    require(state.arm_input(source_arm), "WAITP50INPUT arm was rejected");

    P50InputWaitState canonical_state;
    const P50SourceArmFields complete = canonical_arm();
    require(canonical_state.arm_input(complete),
            "canonical WAITP50INPUT arm was rejected");
    P50SourceArmFields dropped = complete;
    dropped.c_control_generation++;
    int canonical_fds[2] = {-1, -1};
    require(::pipe(canonical_fds) == 0, "canonical pipe setup failed");
    require(!canonical_state.accept_ready(dropped, ready, canonical_fds[0]),
            "canonical WAIT accepted a changed C control generation");
    dropped = complete;
    dropped.c_control_attempt++;
    require(!canonical_state.accept_ready(dropped, ready, canonical_fds[0]),
            "canonical WAIT accepted a changed C control attempt");
    require(canonical_state.accept_ready(complete, ready, canonical_fds[0]),
            "canonical WAIT rejected the complete arm");
    require(canonical_state.take_for_fork() == canonical_fds[0],
            "canonical WAIT did not retain the sealed FD");
    ::close(canonical_fds[1]);
    require(state.state() == P50InputWaitState::State::WaitP50Input,
            "arm did not enter WAITP50INPUT");
    require(!state.can_fork() && state.take_for_fork() == -1,
            "WAITP50INPUT allowed a compiler fork");

    int pipe_fds[2] = {-1, -1};
    require(::pipe(pipe_fds) == 0, "pipe setup failed");
    P50InputReady wrong = ready;
    wrong.attachment_request_id++;
    require(!state.accept_ready(wrong, pipe_fds[0]),
            "wrong ready event was accepted");
    require(state.state() == P50InputWaitState::State::WaitP50Input,
            "wrong ready event changed state");
    require(!state.accept_ready(ready, -1), "missing sealed FD was accepted");
    require(state.accept_ready(ready, pipe_fds[0]),
            "exact ready/sealed FD was rejected");
    require(state.can_fork() && state.state() == P50InputWaitState::State::Ready,
            "exact ready did not make the state forkable");
    const int fork_fd = state.take_for_fork();
    require(fork_fd == pipe_fds[0], "fork did not receive the staged FD");
    require(state.state() == P50InputWaitState::State::Forked &&
                !state.can_fork() && state.take_for_fork() == -1,
            "fork was not one-shot");
    ::close(fork_fd);
    ::close(pipe_fds[1]);

    P50InputWaitState closed;
    require(closed.arm_input(source_arm), "second arm failed");
    int close_fds[2] = {-1, -1};
    require(::pipe(close_fds) == 0, "second pipe setup failed");
    require(closed.accept_ready(ready, close_fds[0]), "second ready failed");
    closed.close();
    require(closed.state() == P50InputWaitState::State::Closed,
            "closure did not leave terminal closed state");
    require(fcntl(close_fds[0], F_GETFD) == -1,
            "closed WAITP50INPUT retained a staged FD");
    ::close(close_fds[1]);
}

void r2_empty_input_and_revision_guard() {
    P50SourceArm r2 = arm();
    r2.cache_protocol = 2;
    require(r2.valid_for_cache_revision(2),
            "CacheWire revision 2 source arm was rejected");
    require(!r2.valid_for_cache_revision(1) &&
                !r2.valid_for_cache_revision(3),
            "R2 identity was accepted under a different revision");

    P50SourceArmFields canonical_r2 = canonical_arm();
    canonical_r2.cache_protocol = 2;
    require(canonical_r2.valid_for_cache_revision(2),
            "complete R2 arm was rejected");
    P50SourceArmFields unsupported = canonical_r2;
    unsupported.cache_protocol = 3;
    require(!unsupported.valid_for_cache_revision(3) &&
                !unsupported.valid_for_cache_revision(2),
            "unsupported CacheWire revision was accepted");

    P50InputReady empty_r2 = ready_for(r2);
    empty_r2.raw_bytes = 0;
    const auto empty_wire = icecc::p50::encode_input_ready(empty_r2);
    const auto empty_roundtrip = icecc::p50::decode_input_ready(empty_wire);
    require(!empty_wire.empty() && empty_roundtrip.has_value() &&
                *empty_roundtrip == empty_r2,
            "empty R2 TU failed exact input-ready roundtrip");

    P50InputReady empty_r1 = ready_for(arm());
    empty_r1.raw_bytes = 0;
    require(!empty_r1.valid() &&
                icecc::p50::encode_input_ready(empty_r1).empty(),
            "empty input was incorrectly enabled for legacy R1");

    P50InputWaitState wait;
    require(wait.arm_input(canonical_r2),
            "R2 complete arm failed to enter WAITP50INPUT");
    int fds[2] = {-1, -1};
    require(::pipe(fds) == 0, "R2 empty-input pipe setup failed");
    P50SourceArmFields wrong = canonical_r2;
    wrong.cache_protocol = 1;
    require(!wait.accept_ready(wrong, empty_r2, fds[0]),
            "R1 arm was allowed to consume an R2 empty-input READY");
    require(wait.accept_ready(canonical_r2, empty_r2, fds[0]),
            "exact R2 empty-input READY was rejected");
    const int fork_fd = wait.take_for_fork();
    require(fork_fd == fds[0],
            "R2 empty input did not transfer its sealed descriptor once");
    ::close(fork_fd);
    ::close(fds[1]);
}

}  // namespace

int main() {
    try {
        wire_fixture();
        arm_ack_order();
        wait_state();
        r2_empty_input_and_revision_guard();
        std::cout << "ok - P50 two-phase source arm and WAITP50INPUT reducer\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
