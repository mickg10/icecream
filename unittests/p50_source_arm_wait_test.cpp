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
    result.cache_protocol = 50;
    result.cache_profile = 2;
    result.logical_job = 19;
    result.attempt_id = 23;
    result.c_store_generation = 29;
    result.c_store_guid.bytes[15] = 31;
    result.source_request_id = 37;
    result.source_mode = 1;
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

void wire_fixture() {
    const P50SourceArm source_arm = arm();
    const auto encoded = icecc::p50::encode_source_arm(source_arm);
    require(!encoded.empty(), "valid source arm did not encode");
    require(encoded.size() < 64u * 1024u, "source arm exceeded fixture cap");
    require(hex(encoded) ==
                "503530410032000100000065000000110102030405060708111213141516171800000009662e6578616d706c650000223d00002694000000320000000200000000000000130000000000000017000000000000001d0000000000000000000000000000001f000000000000002500000001",
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
                "5035304100320002000000ad000000110102030405060708111213141516171800000009662e6578616d706c650000223d00002694000000320000000200000000000000130000000000000017000000000000001d0000000000000000000000000000001f0000000000000025000000010000000000000029000000000000002b0000000000000000000000000000002f00000000000000000000000000000035000000000000003b00000000000000250000000000000043",
            "input-ready wire fixture drifted");
    const auto ready_decoded = icecc::p50::decode_input_ready(ready_wire);
    require(ready_decoded.has_value() && *ready_decoded == ready,
            "input ready fixture did not round-trip");

    auto wrong_version = encoded;
    wrong_version[5] = 49;
    require(!icecc::p50::decode_source_arm(wrong_version),
            "protocol-49/source-arm version mutant was accepted");
    auto wrong_phase = encoded;
    wrong_phase[7] = 2;
    require(!icecc::p50::decode_source_arm(wrong_phase),
            "input-ready phase was accepted as source arm");
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

}  // namespace

int main() {
    try {
        wire_fixture();
        arm_ack_order();
        wait_state();
        std::cout << "ok - P50 two-phase source arm and WAITP50INPUT reducer\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
