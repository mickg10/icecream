#include "cache/protocol50.h"
#include "comm.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <stdexcept>
#include <string_view>
#include <variant>
#include <vector>

using namespace icecc::p50;

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

template <class Exception = std::exception, class Function>
void require_throws(Function&& function, const char* message) {
    try {
        function();
    } catch (const Exception&) {
        return;
    }
    require(false, message);
}

std::vector<uint8_t> bytes(std::string_view value) {
    return {value.begin(), value.end()};
}

Digest128 digest(std::string_view value) {
    const auto input = bytes(value);
    return icecc::digest128(input);
}

TxBegin make_begin(ProfileId profile, std::span<const uint8_t> body) {
    TxBegin begin;
    begin.history_nonce = HistoryNonce{7};
    begin.rel_seq = RelSeq{3};
    begin.tu_seq = TuSeq{11};
    begin.profile = profile;
    begin.pre_state_digest = digest("pre");
    begin.body = describe_component(static_cast<uint16_t>(profile), body, 91);
    begin.raw_bytes = 91;
    begin.raw_digest = digest("raw");
    begin.transaction_digest = compute_transaction_digest(begin, body);
    return begin;
}

TxCommit make_commit(const TxBegin& begin) {
    TxCommit commit;
    commit.history_nonce = begin.history_nonce;
    commit.rel_seq = begin.rel_seq;
    commit.tu_seq = begin.tu_seq;
    commit.transaction_digest = begin.transaction_digest;
    commit.raw_digest = begin.raw_digest;
    commit.post_state_digest = compute_post_state_digest(
        begin.pre_state_digest, begin.history_nonce, begin.rel_seq,
        begin.tu_seq, begin.transaction_digest);
    return commit;
}

void test_revision_one_registry() {
    static_assert(kP50WireRevision == 1);
    static_assert(static_cast<uint16_t>(ProfileId::P29V1) == 1);
    static_assert(static_cast<uint16_t>(ProfileId::ZSTD_TU) == 2);
    static_assert(static_cast<uint16_t>(ProfileId::ZSTD_ROUTE) == 3);
    static_assert(profile_bit(ProfileId::P29V1) == (1U << 0));
    static_assert(profile_bit(ProfileId::ZSTD_TU) == (1U << 1));
    static_assert(profile_bit(ProfileId::ZSTD_ROUTE) == (1U << 2));
    static_assert(kKnownProfileMask == 7);
    static_assert(CACHE_ADVERTISABLE_PROFILE_MASK == 7);
    static_assert(kMandatoryControlFramePayload == 116);
    static_assert(std::variant_size_v<Message> == 28);
    static_assert(static_cast<uint8_t>(MessageType::BODY) == 6);
    static_assert(static_cast<uint8_t>(MessageType::FILL) == 8);
    static_assert(static_cast<uint8_t>(MessageType::LINK_HELLO) == 10);
    static_assert(static_cast<uint8_t>(MessageType::R2_BODY) == 14);
    static_assert(static_cast<uint8_t>(MessageType::R2_FILL) == 15);
    static_assert(static_cast<uint8_t>(MessageType::COMMIT_ACK) == 18);
    static_assert(static_cast<uint8_t>(MessageType::RECOVER) == 19);
    static_assert(static_cast<uint8_t>(MessageType::RECEIPTS) == 20);
    static_assert(static_cast<uint8_t>(MessageType::RESET) == 21);
    static_assert(static_cast<uint8_t>(MessageType::RESET_ACK) == 22);
    static_assert(static_cast<uint8_t>(MessageType::RESET_CONFIRM) == 23);
    static_assert(static_cast<uint8_t>(MessageType::CLOSE) == 24);
    static_assert(static_cast<uint8_t>(MessageType::R2_LINK_REJECT) == 25);

    require(profile_name(ProfileId::P29V1) == "p29_v1" &&
                profile_name(ProfileId::ZSTD_TU) == "zstd_tu" &&
                profile_name(ProfileId::ZSTD_ROUTE) == "zstd_route",
            "revision-one profile names changed");
}

void require_be16(std::span<const uint8_t> wire, size_t offset,
                  uint16_t expected, const char* message) {
    require(offset + 2 <= wire.size(), message);
    const uint16_t value = static_cast<uint16_t>(wire[offset]) << 8 |
                           static_cast<uint16_t>(wire[offset + 1]);
    require(value == expected, message);
}

void require_be32(std::span<const uint8_t> wire, size_t offset,
                  uint32_t expected, const char* message) {
    require(offset + 4 <= wire.size(), message);
    uint32_t value = 0;
    for (size_t index = 0; index != 4; ++index)
        value = (value << 8) | wire[offset + index];
    require(value == expected, message);
}

void require_be64(std::span<const uint8_t> wire, size_t offset,
                  uint64_t expected, const char* message) {
    require(offset + 8 <= wire.size(), message);
    uint64_t value = 0;
    for (size_t index = 0; index != 8; ++index)
        value = (value << 8) | wire[offset + index];
    require(value == expected, message);
}

Id128 role_id(uint64_t value, bool file_role) {
    Id128 result = Id128::from_u64(value);
    if (file_role)
        result.bytes[kStoreIdentityRoleByte] |= kStoreIdentityRoleMask;
    else
        result.bytes[kStoreIdentityRoleByte] &=
            static_cast<uint8_t>(~kStoreIdentityRoleMask);
    return result;
}

Digest128 independent_r2_binding_digest(const JobBind& binding) {
    const auto canonical = encode_payload(Message{binding});
    std::vector<uint8_t> transcript{'R', '2', '-', 'b', 'i', 'n', 'd',
                                    'i', 'n', 'g', '-', 'v', '1'};
    transcript.insert(transcript.end(), canonical.begin(), canonical.end());
    return icecc::digest128(transcript);
}

Digest128 independent_r2_transaction_digest(
    const JobBind& binding, const TuBegin& begin,
    std::span<const R2BodyMessage> bodies,
    std::span<const R2FillMessage> fills) {
    const Digest128 bind_digest = independent_r2_binding_digest(binding);
    std::vector<uint8_t> transcript{'R', '2', '-', 't', 'r', 'a', 'n',
                                    's', 'a', 'c', 't', 'i', 'o', 'n',
                                    '-', 'v', '1'};
    transcript.insert(transcript.end(), bind_digest.bytes.begin(),
                      bind_digest.bytes.end());
    const auto append_frame = [&transcript](uint8_t type,
                                            std::span<const uint8_t> payload) {
        transcript.push_back(type);
        const uint64_t length = payload.size();
        for (int shift = 56; shift >= 0; shift -= 8)
            transcript.push_back(static_cast<uint8_t>(length >> shift));
        transcript.insert(transcript.end(), payload.begin(), payload.end());
    };
    const auto begin_payload = encode_payload(Message{begin});
    append_frame(static_cast<uint8_t>(MessageType::TU_BEGIN), begin_payload);
    for (const auto& body : bodies)
        append_frame(static_cast<uint8_t>(MessageType::R2_BODY), body.bytes);
    for (const auto& fill : fills)
        append_frame(static_cast<uint8_t>(MessageType::R2_FILL), fill.bytes);
    return icecc::digest128(transcript);
}

void test_r2_fixed_wire_shapes_and_negative_cases() {
    const Message close_message{CloseMessage{}};
    require(encode_payload(close_message).empty(),
            "CLOSE payload is not the exact empty payload");
    require(message_type(close_message) == MessageType::CLOSE &&
                decode_payload(MessageType::CLOSE, {}) == close_message,
            "empty CLOSE message did not round-trip");
    const std::array<uint8_t, 1> nonempty_close_payload{0};
    require_throws<std::exception>(
        [&] {
            (void)decode_payload(MessageType::CLOSE, nonempty_close_payload);
        },
        "CLOSE accepted a non-empty payload");

    LinkHello hello;
    hello.profile = ProfileId::ZSTD_ROUTE;
    hello.window = 4;
    hello.max_frame_payload = 4096;
    hello.max_raw_bytes = 0x0102030405060708ULL;
    hello.max_encoded_bytes = 0x1112131415161718ULL;
    hello.max_output_bytes = 0x2122232425262728ULL;
    hello.reservation_id = Id128::from_u64(0x31);
    hello.relationship_id = Id128::from_u64(0x32);
    hello.relationship_epoch = 0x4142434445464748ULL;
    hello.physical_link_generation = 0x5152535455565758ULL;
    hello.c_store_guid = role_id(0x61, false);
    hello.c_store_generation = 0x7172737475767778ULL;
    hello.f_store_guid = role_id(0x81, true);
    hello.f_store_generation = 0x9192939495969798ULL;
    hello.c_control_generation = 0xa1a2a3a4a5a6a7a8ULL;
    hello.c_control_attempt = 0xb1b2b3b4b5b6b7b8ULL;
    hello.system_source_fingerprint = digest("r2-system-source-fingerprint");
    hello.history_nonce = HistoryNonce{0xc1c2c3c4c5c6c7c8ULL};
    hello.start_mode = LinkStartMode::Initial;

    LinkState state;
    state.profile = hello.profile;
    state.window = hello.window;
    state.reservation_id = hello.reservation_id;
    state.relationship_id = hello.relationship_id;
    state.relationship_epoch = hello.relationship_epoch;
    state.physical_link_generation = hello.physical_link_generation;
    state.c_store_guid = hello.c_store_guid;
    state.c_store_generation = hello.c_store_generation;
    state.f_store_guid = hello.f_store_guid;
    state.f_store_generation = hello.f_store_generation;
    state.c_control_generation = hello.c_control_generation;
    state.c_control_attempt = hello.c_control_attempt;
    state.selected_max_frame_payload = 4096;
    state.selected_max_raw_bytes = hello.max_raw_bytes;
    state.selected_max_encoded_bytes = hello.max_encoded_bytes;
    state.selected_max_output_bytes = hello.max_output_bytes;
    state.f_system_source_fingerprint = digest("r2-f-system-fingerprint");
    state.history_nonce = hello.history_nonce;
    state.next_rel_seq = RelSeq{7};
    state.state_digest = digest("r2-state-digest");
    state.committed_prefix_k = 6;
    state.acknowledged_prefix_q = 5;

    JobBind binding;
    binding.reservation_id = hello.reservation_id;
    binding.physical_link_generation = hello.physical_link_generation;
    binding.relationship_ordinal = 1;
    binding.wire_job_id = 2;
    binding.assignment_epoch = 3;
    binding.assignment_nonce = 4;
    binding.logical_job = 5;
    binding.compiler_attempt = 6;
    binding.source_request_id = 7;
    binding.tu_seq = TuSeq{8};
    binding.profile = ProfileId::ZSTD_TU;
    // Empty input is valid and still has an unambiguous binding/commit.
    binding.raw_bytes = 0;
    binding.raw_digest = Digest128{};

    TuBegin begin;
    begin.relationship_ordinal = 1;
    begin.inner.history_nonce = hello.history_nonce;
    begin.inner.rel_seq = RelSeq{6};
    begin.inner.tu_seq = binding.tu_seq;
    begin.inner.profile = binding.profile;
    begin.inner.raw_bytes = binding.raw_bytes;
    begin.inner.raw_digest = binding.raw_digest;
    const Digest128 binding_digest = compute_r2_binding_digest(binding);
    require(binding_digest == independent_r2_binding_digest(binding),
            "R2 binding digest differs from canonical domain+JOB_BIND bytes");
    const std::vector<R2BodyMessage> bodies{{std::vector<uint8_t>{0x10, 0x20}}};
    const std::vector<R2FillMessage> fills{{std::vector<uint8_t>{0x30, 0x40, 0x50}}};
    const Digest128 transaction_digest = compute_r2_transaction_digest(
        binding, begin, bodies, fills);
    require(transaction_digest == independent_r2_transaction_digest(
                                     binding, begin, bodies, fills),
            "R2 transaction digest differs from explicit type/BE-length transcript");
    TuEnd end{1, binding_digest, transaction_digest};
    TxCommit inner_commit;
    inner_commit.history_nonce = hello.history_nonce;
    inner_commit.rel_seq = begin.inner.rel_seq;
    inner_commit.tu_seq = begin.inner.tu_seq;
    inner_commit.transaction_digest = transaction_digest;
    R2TxCommit commit{1, binding_digest, transaction_digest, inner_commit};
    CommitAck ack{hello.relationship_id, hello.relationship_epoch,
                  hello.physical_link_generation, 1};

    const std::vector<Message> fixed_messages{
        Message{hello}, Message{state}, Message{binding}, Message{begin},
        Message{end}, Message{commit}, Message{ack}};
    const std::array<size_t, 7> expected_sizes{181, 212, 110, 124, 40, 112, 40};
    for (size_t index = 0; index != fixed_messages.size(); ++index) {
        const Message& message = fixed_messages[index];
        const auto payload = encode_payload(message);
        require(payload.size() == expected_sizes[index],
                "R2 control payload size changed");
        require(decode_payload(message_type(message), payload) == message,
                "R2 control payload did not round trip exactly");
        require_throws<std::exception>(
            [&] { (void)decode_payload(message_type(message),
                                       std::span<const uint8_t>(payload).first(
                                           payload.size() - 1)); },
            "truncated R2 control payload was accepted");
        std::vector<uint8_t> with_trailing = payload;
        with_trailing.push_back(0);
        require_throws<std::exception>(
            [&] { (void)decode_payload(message_type(message), with_trailing); },
            "R2 control payload with trailing bytes was accepted");
    }

    const auto hello_wire = encode_payload(Message{hello});
    Digest128 expected_offer_digest{};
    {
        std::vector<uint8_t> transcript = bytes("R2-link-offer-v1");
        transcript.insert(transcript.end(), hello_wire.begin(), hello_wire.end());
        expected_offer_digest = icecc::digest128(transcript);
    }
    const Digest128 offer_digest = compute_r2_link_offer_digest(hello);
    require(offer_digest == expected_offer_digest,
            "R2 offer digest differs from domain+canonical LINK_HELLO transcript");
    require(icecc::digest128_hex(offer_digest) ==
                "eca876283a11f86fec8efac86d6631ea",
            "R2_LINK_REJECT canonical offer digest golden changed");
    const LinkRejectMessage link_reject{
        LinkRejectReason::StoreReplaced, offer_digest};
    const Message link_reject_message{link_reject};
    const auto reject_payload = encode_payload(link_reject_message);
    require(reject_payload.size() == kR2LinkRejectPayloadBytes,
            "R2_LINK_REJECT payload length changed");
    require_be16(reject_payload, 0,
                 static_cast<uint16_t>(LinkRejectReason::StoreReplaced),
                 "R2_LINK_REJECT reason byte order changed");
    require(std::equal(offer_digest.bytes.begin(), offer_digest.bytes.end(),
                       reject_payload.begin() + 2),
            "R2_LINK_REJECT digest offset changed");
    require(message_type(link_reject_message) == MessageType::R2_LINK_REJECT &&
                decode_payload(MessageType::R2_LINK_REJECT, reject_payload) ==
                    link_reject_message,
            "R2_LINK_REJECT did not round-trip");
    const auto reject_frame = encode_frame(link_reject_message);
    require(decode_frame_header(
                std::span<const uint8_t>(reject_frame).first(4),
                kInitialMaxFramePayload).type == MessageType::R2_LINK_REJECT,
            "R2_LINK_REJECT frame discriminator changed");
    for (const size_t size : {size_t{0}, size_t{1}, size_t{17}}) {
        const std::vector<uint8_t> malformed(
            reject_payload.begin(), reject_payload.begin() + size);
        require_throws<std::exception>(
            [&] { (void)decode_payload(MessageType::R2_LINK_REJECT, malformed); },
            "R2_LINK_REJECT accepted a malformed exact length");
    }
    std::vector<uint8_t> trailing_reject = reject_payload;
    trailing_reject.push_back(0);
    require_throws<std::exception>(
        [&] {
            (void)decode_payload(MessageType::R2_LINK_REJECT, trailing_reject);
        },
        "R2_LINK_REJECT accepted trailing bytes");
    const LinkRejectMessage missing_reservation{
        LinkRejectReason::ReservationMissing, offer_digest};
    const auto missing_payload = encode_payload(Message{missing_reservation});
    require(decode_payload(MessageType::R2_LINK_REJECT, missing_payload) ==
                Message{missing_reservation},
            "ReservationMissing R2_LINK_REJECT did not round-trip");
    for (const uint16_t reason : {uint16_t{0}, uint16_t{3}, uint16_t{0xffff}}) {
        std::vector<uint8_t> malformed(reject_payload);
        malformed[0] = static_cast<uint8_t>(reason >> 8);
        malformed[1] = static_cast<uint8_t>(reason);
        require_throws<std::exception>(
            [&] { (void)decode_payload(MessageType::R2_LINK_REJECT, malformed); },
            "R2_LINK_REJECT accepted an unknown reason");
    }
    require_throws<std::exception>(
        [&] {
            (void)encode_payload(Message{LinkRejectMessage{
                static_cast<LinkRejectReason>(3), offer_digest}});
        },
        "R2_LINK_REJECT encoder accepted an unknown reason");
    LinkHello replaced_f = hello;
    ++replaced_f.f_store_generation;
    require(compute_r2_link_offer_digest(replaced_f) != offer_digest,
            "R2 offer digest omitted F store generation");
    LinkHello replaced_link = hello;
    ++replaced_link.physical_link_generation;
    require(compute_r2_link_offer_digest(replaced_link) != offer_digest,
            "R2 offer digest omitted physical link generation");
    LinkHello changed_control = hello;
    ++changed_control.c_control_attempt;
    require(compute_r2_link_offer_digest(changed_control) != offer_digest,
            "R2 offer digest omitted C control incarnation");
    LinkHello changed_reservation = hello;
    changed_reservation.reservation_id = Id128::from_u64(0x99);
    require(compute_r2_link_offer_digest(changed_reservation) != offer_digest,
            "R2 offer digest omitted reservation identity");
    LinkHello changed_relationship = hello;
    ++changed_relationship.relationship_epoch;
    require(compute_r2_link_offer_digest(changed_relationship) != offer_digest,
            "R2 offer digest omitted relationship epoch");
    LinkHello changed_profile = hello;
    changed_profile.profile = ProfileId::ZSTD_TU;
    require(compute_r2_link_offer_digest(changed_profile) != offer_digest,
            "R2 offer digest omitted selected profile");
    require_be16(hello_wire, 0, 2, "LINK_HELLO revision offset changed");
    require_be16(hello_wire, 2, static_cast<uint16_t>(hello.profile),
                 "LINK_HELLO profile offset changed");
    require_be32(hello_wire, 4, 4, "LINK_HELLO window offset changed");
    require_be32(hello_wire, 8, 4096, "LINK_HELLO frame cap offset changed");
    require_be64(hello_wire, 12, hello.max_raw_bytes,
                 "LINK_HELLO raw cap offset changed");
    require_be64(hello_wire, 164, hello.history_nonce.value,
                 "LINK_HELLO history nonce offset changed");
    require_be64(hello_wire, 172, 0,
                 "initial LINK_HELLO receipt floor must be zero");
    require(hello_wire[180] == static_cast<uint8_t>(LinkStartMode::Initial),
            "LINK_HELLO start mode offset changed");
    require(std::equal(hello.system_source_fingerprint.bytes.begin(),
                       hello.system_source_fingerprint.bytes.end(),
                       hello_wire.begin() + 148),
            "LINK_HELLO source fingerprint offset changed");
    const auto state_wire = encode_payload(Message{state});
    require_be32(state_wire, 4, 4, "LINK_STATE window offset changed");
    require_be64(state_wire, 40, hello.relationship_epoch,
                 "LINK_STATE relationship epoch offset changed");
    require_be64(state_wire, 48, hello.physical_link_generation,
                 "LINK_STATE physical generation offset changed");
    require_be32(state_wire, 120, 4096,
                 "LINK_STATE selected frame cap offset changed");
    require_be64(state_wire, 124, state.selected_max_raw_bytes,
                 "LINK_STATE selected raw cap offset changed");
    require_be64(state_wire, 132, state.selected_max_encoded_bytes,
                 "LINK_STATE selected encoded cap offset changed");
    require_be64(state_wire, 140, state.selected_max_output_bytes,
                 "LINK_STATE selected output cap offset changed");
    require(std::equal(state.f_system_source_fingerprint.bytes.begin(),
                       state.f_system_source_fingerprint.bytes.end(),
                       state_wire.begin() + 148),
            "LINK_STATE source fingerprint offset changed");
    require_be64(state_wire, 172, state.next_rel_seq.value,
                 "LINK_STATE next REL_SEQ offset changed");
    require_be64(state_wire, 196, state.committed_prefix_k,
                 "LINK_STATE committed prefix offset changed");
    require_be64(state_wire, 204, state.acknowledged_prefix_q,
                 "LINK_STATE acknowledged prefix offset changed");
    require(hello.system_source_fingerprint != state.f_system_source_fingerprint,
            "chosen C/F fingerprints unexpectedly collapsed");

    const auto binding_wire = encode_payload(Message{binding});
    require_be64(binding_wire, 16, binding.physical_link_generation,
                 "JOB_BIND physical generation offset changed");
    require_be64(binding_wire, 24, binding.relationship_ordinal,
                 "JOB_BIND ordinal offset changed");
    require_be32(binding_wire, 32, binding.wire_job_id,
                 "JOB_BIND wire job id offset changed");
    require_be64(binding_wire, 76, binding.tu_seq.value,
                 "JOB_BIND TU_SEQ offset changed");
    require_be64(binding_wire, 86, binding.raw_bytes,
                 "JOB_BIND raw length offset changed");
    require(std::equal(binding.raw_digest.bytes.begin(),
                       binding.raw_digest.bytes.end(),
                       binding_wire.begin() + 94),
            "JOB_BIND raw digest offset changed");
    const auto begin_wire = encode_payload(Message{begin});
    const auto inner_begin_wire = encode_payload(Message{begin.inner});
    require(begin_wire.size() == inner_begin_wire.size() + 8 &&
                std::equal(inner_begin_wire.begin(), inner_begin_wire.end(),
                           begin_wire.begin() + 8),
            "TU_BEGIN did not embed the exact R1 TX_BEGIN payload after ordinal");
    const auto end_wire = encode_payload(Message{end});
    require_be64(end_wire, 0, end.relationship_ordinal,
                 "TU_END ordinal offset changed");
    require(std::equal(end.binding_digest.bytes.begin(), end.binding_digest.bytes.end(),
                       end_wire.begin() + 8) &&
                std::equal(end.transaction_digest.bytes.begin(),
                           end.transaction_digest.bytes.end(), end_wire.begin() + 24),
            "TU_END digest offsets changed");
    const auto commit_wire = encode_payload(Message{commit});
    const auto inner_commit_wire = encode_payload(Message{inner_commit});
    require(commit_wire.size() == inner_commit_wire.size() + 40 &&
                std::equal(inner_commit_wire.begin(), inner_commit_wire.end(),
                           commit_wire.begin() + 40),
            "R2_TX_COMMIT did not embed the exact R1 TX_COMMIT payload");
    const auto ack_wire = encode_payload(Message{ack});
    require_be64(ack_wire, 16, ack.relationship_epoch,
                 "COMMIT_ACK relationship epoch offset changed");
    require_be64(ack_wire, 24, ack.physical_link_generation,
                 "COMMIT_ACK physical generation offset changed");
    require_be64(ack_wire, 32, ack.contiguous_verified_ordinal,
                 "COMMIT_ACK contiguous ordinal offset changed");

    const R2BodyMessage r2_body{std::vector<uint8_t>{0xaa, 0xbb}};
    const R2FillMessage r2_fill{std::vector<uint8_t>{0xaa, 0xbb}};
    const BodyMessage r1_body{r2_body.bytes};
    const FillMessage r1_fill{r2_fill.bytes};
    require(static_cast<uint8_t>(message_type(Message{r1_body})) == 6 &&
                static_cast<uint8_t>(message_type(Message{r1_fill})) == 8 &&
                static_cast<uint8_t>(message_type(Message{r2_body})) == 14 &&
                static_cast<uint8_t>(message_type(Message{r2_fill})) == 15,
            "R2 BODY/FILL reused an R1 frame identifier");
    require(encode_payload(Message{r2_body}) == r2_body.bytes &&
                encode_payload(Message{r2_fill}) == r2_fill.bytes,
            "R2 BODY/FILL opaque payload changed bytes");

    require(encode_frame(Message{r2_body})[0] == 14 &&
                encode_frame(Message{r2_fill})[0] == 15,
            "R2 BODY/FILL frame header identifiers changed");
    const auto body_frame = encode_frame(Message{r2_body});
    FrameParser truncated_frame;
    (void)truncated_frame.feed(std::span<const uint8_t>(body_frame).first(
        body_frame.size() - 1));
    require_throws<std::exception>([&] { truncated_frame.finish(); },
                                   "truncated R2 frame was accepted");

    require_throws<std::exception>(
        [&] { (void)encode_payload(Message{LinkHello{.revision = 1}}); },
        "R2 LINK_HELLO accepted revision 1");
    require_throws<std::exception>(
        [&] { (void)decode_payload(static_cast<MessageType>(25), {}); },
        "unknown post-R2 frame type was accepted");

    const Digest128 base_digest = compute_r2_transaction_digest(
        binding, begin, bodies, fills);
    JobBind changed_binding = binding;
    ++changed_binding.wire_job_id;
    require(compute_r2_transaction_digest(changed_binding, begin, bodies, fills) !=
                base_digest,
            "R2 transaction digest omitted the JOB_BIND identity");
    require(compute_r2_transaction_digest(changed_binding, begin, bodies, fills) ==
                independent_r2_transaction_digest(changed_binding, begin,
                                                  bodies, fills),
            "mutated R2 binding diverged from explicit digest transcript");
    auto changed_body = bodies;
    changed_body[0].bytes[0] ^= 1;
    require(compute_r2_transaction_digest(binding, begin, changed_body, fills) !=
                base_digest,
            "R2 transaction digest omitted a BODY byte");
    changed_body = bodies;
    changed_body[0].bytes.push_back(0x30);
    require(compute_r2_transaction_digest(binding, begin, changed_body, fills) !=
                base_digest,
            "R2 transaction digest omitted BODY length");
    auto changed_fill = fills;
    changed_fill[0].bytes[0] ^= 1;
    require(compute_r2_transaction_digest(binding, begin, bodies, changed_fill) !=
                base_digest,
            "R2 transaction digest omitted a FILL byte");
    changed_fill = fills;
    changed_fill[0].bytes.push_back(0x60);
    require(compute_r2_transaction_digest(binding, begin, bodies, changed_fill) !=
                base_digest,
            "R2 transaction digest omitted FILL length");
    const std::vector<R2BodyMessage> same_payload_as_body{{r2_body.bytes}};
    const std::vector<R2FillMessage> same_payload_as_fill{{r2_body.bytes}};
    require(compute_r2_transaction_digest(binding, begin,
                                          same_payload_as_body, {}) !=
                compute_r2_transaction_digest(binding, begin, {},
                                              same_payload_as_fill),
            "R2 transaction digest omitted BODY/FILL frame type");

    const Id128 recovery_operation = Id128::from_u64(0xe1);
    RecoverBegin recover_begin{hello.relationship_id,
                               hello.relationship_epoch,
                               hello.physical_link_generation,
                               recovery_operation, 0, 1, 1};
    RecoverWitness witness;
    witness.relationship_id = hello.relationship_id;
    witness.relationship_epoch = hello.relationship_epoch;
    witness.physical_link_generation = hello.physical_link_generation;
    witness.operation_id = recovery_operation;
    witness.relationship_ordinal = 1;
    witness.binding_digest = binding_digest;
    witness.transaction_digest = transaction_digest;
    witness.binding = binding;
    witness.inner = begin.inner;
    const std::array<RecoverWitness, 1> witnesses{witness};
    const Digest128 recovery_transcript =
        compute_r2_recovery_transcript_digest(recover_begin, witnesses);
    require(begin.inner.transaction_digest != transaction_digest,
            "wire test must distinguish R1 inner and R2 outer transaction digests");
    RecoverEnd recover_end{hello.relationship_id,
                           hello.relationship_epoch,
                           hello.physical_link_generation,
                           recovery_operation,
                           1,
                           recovery_transcript};
    ReceiptRow receipt_row{hello.relationship_id,
                           hello.relationship_epoch,
                           hello.physical_link_generation,
                           recovery_operation,
                           commit};
    ReceiptsEnd receipts_end{hello.relationship_id,
                             hello.relationship_epoch,
                             hello.physical_link_generation,
                             recovery_operation,
                             0,
                             1,
                             0,
                             1};
    ResetRequest reset_request{hello.relationship_id,
                               hello.relationship_epoch,
                               hello.relationship_epoch + 1,
                               hello.physical_link_generation,
                               recovery_operation,
                               1,
                               hello.history_nonce,
                               HistoryNonce{hello.history_nonce.value + 1}};
    ResetAck reset_ack{reset_request, digest("reset-initial-state"), RelSeq{0}};
    reset_ack.recovery_verified_floor_a = 0;
    reset_ack.recovery_prepared_prefix_p = 1;
    reset_ack.recovery_witness_digest =
        compute_r2_recovery_witness_digest(recover_begin, witnesses);
    ResetConfirm reset_confirm{hello.relationship_id,
                               reset_request.new_relationship_epoch,
                               hello.physical_link_generation,
                               recovery_operation,
                               reset_request.new_history_nonce,
                               reset_request.settled_prefix_k};
    const std::array<Message, 8> recovery_messages{
        Message{recover_begin}, Message{witness}, Message{recover_end},
        Message{receipt_row}, Message{receipts_end}, Message{reset_request},
        Message{reset_ack}, Message{reset_confirm}};
    const std::array<size_t, 8> recovery_sizes{
        kR2RecoverBeginPayloadBytes, kR2RecoverWitnessPayloadBytes,
        kR2RecoverEndPayloadBytes, kR2ReceiptRowPayloadBytes,
        kR2ReceiptsEndPayloadBytes, kR2ResetPayloadBytes,
        kR2ResetAckPayloadBytes, kR2ResetConfirmPayloadBytes};
    for (size_t index = 0; index != recovery_messages.size(); ++index) {
        const Message& message = recovery_messages[index];
        const auto payload = encode_payload(message);
        require(payload.size() == recovery_sizes[index],
                "R2 recovery payload size changed");
        require(decode_payload(message_type(message), payload) == message,
                "R2 recovery payload did not round-trip exactly");
        require_throws<std::exception>(
            [&] { (void)decode_payload(message_type(message),
                                       std::span<const uint8_t>(payload).first(
                                           payload.size() - 1)); },
            "truncated R2 recovery payload was accepted");
        std::vector<uint8_t> with_trailing = payload;
        with_trailing.push_back(0);
        require_throws<std::exception>(
            [&] { (void)decode_payload(message_type(message), with_trailing); },
            "R2 recovery payload with trailing bytes was accepted");
    }
    const auto witness_wire = encode_payload(Message{witness});
    require_be64(witness_wire, 49, witness.relationship_ordinal,
                 "RECOVER witness ordinal offset changed");
    require(std::equal(binding_digest.bytes.begin(), binding_digest.bytes.end(),
                       witness_wire.begin() + 57) &&
                std::equal(transaction_digest.bytes.begin(),
                           transaction_digest.bytes.end(),
                           witness_wire.begin() + 73),
            "RECOVER witness digest offsets changed");
    const auto encoded_binding = encode_payload(Message{binding});
    const auto encoded_inner_begin = encode_payload(Message{begin.inner});
    require(std::equal(encoded_binding.begin(), encoded_binding.end(),
                       witness_wire.begin() + 89) &&
                std::equal(encoded_inner_begin.begin(), encoded_inner_begin.end(),
                           witness_wire.begin() + 199),
            "RECOVER witness must carry exact JOB_BIND followed by TX_BEGIN");
    auto changed_embedded_binding = witness_wire;
    changed_embedded_binding[140] ^= 1;
    require_throws<std::exception>(
        [&] {
            (void)decode_payload(MessageType::RECOVER,
                                 changed_embedded_binding);
        },
        "RECOVER accepted a changed JOB_BIND with the old binding digest");
    const auto reset_ack_wire = encode_payload(Message{reset_ack});
    require_be64(reset_ack_wire, 104, reset_ack.recovery_verified_floor_a,
                 "RESET_ACK verified floor offset changed");
    require_be64(reset_ack_wire, 112, reset_ack.recovery_prepared_prefix_p,
                 "RESET_ACK prepared prefix offset changed");
    require(std::equal(reset_ack.recovery_witness_digest.bytes.begin(),
                       reset_ack.recovery_witness_digest.bytes.end(),
                       reset_ack_wire.begin() + 120),
            "RESET_ACK recovery digest offset changed");
    require_be32(reset_ack_wire, 136, reset_ack.unavailable_suffix_mask,
                 "RESET_ACK unavailable mask offset changed");
    auto bad_recover_subkind = encode_payload(Message{witness});
    bad_recover_subkind[48] = 0xff;
    require_throws<std::exception>(
        [&] { (void)decode_payload(MessageType::RECOVER, bad_recover_subkind); },
        "RECOVER accepted an unknown subkind");
    auto bad_receipt_subkind = encode_payload(Message{receipt_row});
    bad_receipt_subkind[48] = 0xff;
    require_throws<std::exception>(
        [&] { (void)decode_payload(MessageType::RECEIPTS, bad_receipt_subkind); },
        "RECEIPTS accepted an unknown subkind");
    auto bad_recover_ordinal = encode_payload(Message{witness});
    std::fill(bad_recover_ordinal.begin() + 49,
              bad_recover_ordinal.begin() + 57, 0);
    require_throws<std::exception>(
        [&] { (void)decode_payload(MessageType::RECOVER, bad_recover_ordinal); },
        "RECOVER accepted ordinal zero");
    auto bad_receipt_interval = encode_payload(Message{receipts_end});
    bad_receipt_interval.back() = 2;
    require_throws<std::exception>(
        [&] { (void)decode_payload(MessageType::RECEIPTS, bad_receipt_interval); },
        "RECEIPTS accepted a count inconsistent with its interval");
    auto bad_reset_epoch = encode_payload(Message{reset_request});
    std::fill(bad_reset_epoch.begin() + 24, bad_reset_epoch.begin() + 32, 0);
    require_throws<std::exception>(
        [&] { (void)decode_payload(MessageType::RESET, bad_reset_epoch); },
        "RESET accepted a non-successor relationship epoch");
    auto changed_witnesses = witnesses;
    changed_witnesses[0].transaction_digest.bytes[0] ^= 1;
    require(compute_r2_recovery_transcript_digest(recover_begin, witnesses) !=
                compute_r2_recovery_transcript_digest(recover_begin,
                                                     changed_witnesses),
            "R2 recovery transcript omitted witness identity bytes");

    // The RESET_ACK disposition covers the complete recovery interval, while
    // its mask is relative to K: here ordinals 2 and 4 survive, ordinal 3 is
    // explicitly unavailable. This leaves a two-row replayable suffix.
    RecoverBegin range_begin = recover_begin;
    range_begin.prepared_prefix_p = 4;
    range_begin.witness_count = 4;
    std::array<RecoverWitness, 4> range_witnesses{};
    for (size_t index = 0; index != range_witnesses.size(); ++index) {
        RecoverWitness& row = range_witnesses[index];
        row.relationship_id = range_begin.relationship_id;
        row.relationship_epoch = range_begin.relationship_epoch;
        row.physical_link_generation = range_begin.physical_link_generation;
        row.operation_id = range_begin.operation_id;
        row.relationship_ordinal = index + 1;
        row.binding = binding;
        row.binding.relationship_ordinal = index + 1;
        row.binding.wire_job_id = static_cast<uint32_t>(index + 2);
        row.binding_digest = compute_r2_binding_digest(row.binding);
        row.transaction_digest = transaction_digest;
        row.inner = begin.inner;
    }
    const Digest128 witness_set_digest =
        compute_r2_recovery_witness_digest(range_begin, range_witnesses);
    require(witness_set_digest != Digest128{},
            "normalized recovery witness digest was zero");
    auto reconnected_begin = range_begin;
    reconnected_begin.physical_link_generation += 10;
    auto reconnected_witnesses = range_witnesses;
    for (RecoverWitness& row : reconnected_witnesses)
        row.physical_link_generation = reconnected_begin.physical_link_generation;
    require(compute_r2_recovery_witness_digest(reconnected_begin,
                                               reconnected_witnesses) ==
                witness_set_digest,
            "stable recovery witness digest changed with transport generation");
    auto wrong_transport_identity = reconnected_witnesses;
    wrong_transport_identity[0].physical_link_generation += 1;
    require_throws<std::exception>(
        [&] {
            (void)compute_r2_recovery_witness_digest(reconnected_begin,
                                                     wrong_transport_identity);
        },
        "stable recovery digest normalized a mismatched transport generation");
    auto changed_assignment = range_witnesses;
    ++changed_assignment[0].binding.assignment_nonce;
    changed_assignment[0].binding_digest =
        compute_r2_binding_digest(changed_assignment[0].binding);
    require(compute_r2_recovery_witness_digest(range_begin,
                                               changed_assignment) !=
                witness_set_digest,
            "stable recovery digest omitted the exact nested JOB_BIND identity");
    auto changed_original_generation = range_witnesses;
    ++changed_original_generation[0].binding.physical_link_generation;
    changed_original_generation[0].binding_digest =
        compute_r2_binding_digest(changed_original_generation[0].binding);
    require(compute_r2_recovery_witness_digest(range_begin,
                                               changed_original_generation) !=
                witness_set_digest,
            "stable recovery digest normalized immutable original JOB_BIND generation");

    ResetAck disposition = reset_ack;
    disposition.request.settled_prefix_k = 1;
    disposition.recovery_verified_floor_a = 0;
    disposition.recovery_prepared_prefix_p = 4;
    disposition.recovery_witness_digest = witness_set_digest;
    disposition.unavailable_suffix_mask = 0b010;
    const auto disposition_wire = encode_payload(Message{disposition});
    require(disposition_wire.size() == kR2ResetAckPayloadBytes &&
                decode_payload(MessageType::RESET_ACK, disposition_wire) ==
                    Message{disposition},
            "RESET_ACK recovery disposition did not round-trip");
    require(disposition.unavailable_suffix_mask == 0b010,
            "RESET_ACK mask no longer identifies K+2 as unavailable");
    auto bad_ack = disposition;
    bad_ack.unavailable_suffix_mask = 0b1000;
    require_throws<std::exception>(
        [&] { (void)encode_payload(Message{bad_ack}); },
        "RESET_ACK accepted an unavailable bit outside its suffix interval");
    bad_ack = disposition;
    bad_ack.recovery_verified_floor_a = 2;
    require_throws<std::exception>(
        [&] { (void)encode_payload(Message{bad_ack}); },
        "RESET_ACK accepted verified floor beyond settled prefix");
    bad_ack = disposition;
    bad_ack.recovery_prepared_prefix_p = 0;
    require_throws<std::exception>(
        [&] { (void)encode_payload(Message{bad_ack}); },
        "RESET_ACK accepted prepared prefix below settled prefix");
    bad_ack = disposition;
    bad_ack.recovery_prepared_prefix_p = 31;
    require_throws<std::exception>(
        [&] { (void)encode_payload(Message{bad_ack}); },
        "RESET_ACK accepted a recovery interval wider than 30 witnesses");
    bad_ack = disposition;
    bad_ack.request.settled_prefix_k = 0;
    bad_ack.recovery_prepared_prefix_p = 30;
    bad_ack.unavailable_suffix_mask = (uint32_t{1} << 30) - 1;
    require(decode_payload(MessageType::RESET_ACK,
                           encode_payload(Message{bad_ack})) ==
                Message{bad_ack},
            "RESET_ACK rejected the maximum 30-witness unavailable mask");
    auto max_empty_interval = reset_ack;
    max_empty_interval.request.settled_prefix_k = UINT64_MAX;
    max_empty_interval.recovery_verified_floor_a = UINT64_MAX;
    max_empty_interval.recovery_prepared_prefix_p = UINT64_MAX;
    max_empty_interval.recovery_witness_digest = digest("max-empty-interval");
    max_empty_interval.unavailable_suffix_mask = 0;
    require(decode_payload(MessageType::RESET_ACK,
                           encode_payload(Message{max_empty_interval})) ==
                Message{max_empty_interval},
            "RESET_ACK rejected an empty interval at UINT64_MAX");
    auto max_one_row_interval = max_empty_interval;
    max_one_row_interval.request.settled_prefix_k = UINT64_MAX - 1;
    max_one_row_interval.recovery_verified_floor_a = UINT64_MAX - 1;
    max_one_row_interval.unavailable_suffix_mask = 1;
    require(decode_payload(MessageType::RESET_ACK,
                           encode_payload(Message{max_one_row_interval})) ==
                Message{max_one_row_interval},
            "RESET_ACK rejected a one-row suffix ending at UINT64_MAX");
    bad_ack = disposition;
    bad_ack.recovery_witness_digest = Digest128{};
    require_throws<std::exception>(
        [&] { (void)encode_payload(Message{bad_ack}); },
        "RESET_ACK accepted a zero recovery witness digest");
    auto bad_ack_wire = disposition_wire;
    bad_ack_wire.back() = 0x80;
    require_throws<std::exception>(
        [&] { (void)decode_payload(MessageType::RESET_ACK, bad_ack_wire); },
        "RESET_ACK decoder accepted high unavailable-mask bits");
}

void test_key_layout() {
    const auto key = Key64::make(ObjectType::P29Segment, 37, 99);
    require(key && key->valid() && key->type() == ObjectType::P29Segment &&
                key->generation() == 37 && key->ordinal() == 99,
            "Key64 did not round trip its fields");
    require(Key64::from_wire(key->wire_value()) == key,
            "Key64 wire value did not round trip");
    require(!Key64::make(ObjectType::Blob, 0, 0),
            "Key64 admitted reserved ordinal zero");
    require(!Key64::from_wire(0), "Key64 admitted the zero wire value");
}

void test_fixed_wire_shapes_and_round_trip() {
    const std::vector<uint8_t> body = bytes("one BODY stream");
    const TxBegin begin = make_begin(ProfileId::P29V1, body);
    const TxCommit commit = make_commit(begin);

    SessionHello hello;
    hello.c_store_guid = CStoreGuid::from_u64(1);
    hello.system_source_fingerprint = digest("c-system-source");

    SessionState absent;
    absent.f_store_guid = FStoreGuid::from_u64(2);
    absent.system_source_fingerprint = digest("f-system-source");

    SessionState present = absent;
    present.namespace_present = true;
    present.route_present = true;
    present.history_nonce = begin.history_nonce;
    present.next_rel_seq = RelSeq{begin.rel_seq.value + 1};
    present.state_digest = commit.post_state_digest;
    present.last_commit = commit;

    const std::vector<Message> messages{
        hello,
        absent,
        present,
        HistoryReset{begin.history_nonce, begin.pre_state_digest},
        ErrorMessage{static_cast<uint16_t>(ErrorCode::WIRE_REVISION_MISMATCH),
                     "revision mismatch"},
        begin,
        BodyMessage{body},
        NeedMessage{bytes("need")},
        FillMessage{bytes("fill")},
        commit,
    };
    for (const Message& message : messages) {
        const auto payload = encode_payload(message);
        require(decode_payload(message_type(message), payload) == message,
                "message payload did not round trip");
    }

    require(encode_payload(Message{hello}).size() == 50,
            "SESSION_HELLO payload is not 50 bytes");
    require(encode_payload(Message{absent}).size() == 83,
            "absent SESSION_STATE payload is not 83 bytes");
    require(encode_payload(Message{present}).size() == 155,
            "SESSION_STATE with last commit is not 155 bytes");
    require(encode_payload(Message{begin}).size() == 116,
            "TX_BEGIN payload is not 116 bytes");
    require(encode_payload(Message{commit}).size() == 72,
            "TX_COMMIT payload is not 72 bytes");
}

void test_negotiation_and_revision_refusal() {
    SessionHello hello;
    hello.c_store_guid = CStoreGuid::from_u64(3);
    hello.system_source_fingerprint = digest("client-fingerprint");
    hello.supported_profiles = profile_bit(ProfileId::P29V1) |
                               profile_bit(ProfileId::ZSTD_ROUTE);
    hello.limits = SessionLimits{4096, 8192};

    const SessionSelection selected = negotiate_session(
        hello, kP50WireRevision,
        profile_bit(ProfileId::ZSTD_TU) |
            profile_bit(ProfileId::ZSTD_ROUTE),
        SessionLimits{2048, 4096});
    require(selected.wire_revision == kP50WireRevision &&
                selected.negotiated_profiles ==
                    profile_bit(ProfileId::ZSTD_ROUTE) &&
                selected.limits == SessionLimits{2048, 4096},
            "revision-one negotiation selected the wrong intersection");

    try {
        (void)negotiate_session(hello, 2, kKnownProfileMask);
        require(false, "revision skew was accepted");
    } catch (const ProtocolError& error) {
        require(error.code() == ErrorCode::WIRE_REVISION_MISMATCH &&
                    static_cast<uint16_t>(error.code()) == 4,
                "revision skew did not use error code 4");
    }

    SessionState state;
    state.f_store_guid = FStoreGuid::from_u64(4);
    state.negotiated_profiles = profile_bit(ProfileId::ZSTD_ROUTE);
    state.limits = SessionLimits{2048, 4096};
    validate_session_state(hello, state);
    state.wire_revision = 2;
    require_throws<ProtocolError>(
        [&] { validate_session_state(hello, state); },
        "client accepted a SESSION_STATE from another revision");
}

void test_framing() {
    SessionHello hello;
    hello.c_store_guid = CStoreGuid::from_u64(5);
    const auto first = encode_frame(Message{hello});
    const auto second = encode_frame(Message{ErrorMessage{9, "terminal"}});
    std::vector<uint8_t> stream = first;
    stream.insert(stream.end(), second.begin(), second.end());

    FrameParser parser;
    std::vector<Frame> frames;
    for (uint8_t byte : stream) {
        auto ready = parser.feed(std::span<const uint8_t>(&byte, 1));
        frames.insert(frames.end(), ready.begin(), ready.end());
    }
    parser.finish();
    require(frames.size() == 2 &&
                decode_payload(frames[0].type, frames[0].payload) == Message{hello} &&
                std::holds_alternative<ErrorMessage>(
                    decode_payload(frames[1].type, frames[1].payload)),
            "fragmented frame stream did not parse exactly");

    FrameParser truncated;
    truncated.feed(std::span<const uint8_t>(first).first(first.size() - 1));
    require_throws([&] { truncated.finish(); },
                   "truncated frame stream was accepted");

    MessageSessionGate gate;
    gate.observe(MessageType::SESSION_HELLO);
    gate.observe(MessageType::ERROR);
    require(gate.terminal(), "ERROR did not close the message session");
    require_throws<std::logic_error>(
        [&] { gate.observe(MessageType::BODY); },
        "message session admitted data after ERROR");
}

void test_need_and_fill_streams() {
    const std::array<Key64, 3> keys{
        *Key64::make(ObjectType::Atom, 1, 1),
        *Key64::make(ObjectType::Atom, 1, 2),
        *Key64::make(ObjectType::Blob, 2, 9),
    };
    const auto need_messages = encode_need_messages(keys, 16);
    NeedStreamDecoder need_decoder;
    for (const auto& message : need_messages)
        need_decoder.push(message);
    need_decoder.finish();
    require(need_decoder.keys() == std::vector<Key64>(keys.begin(), keys.end()),
            "generic NEED continuation did not round trip");

    const std::vector<uint8_t> object = bytes("object bytes");
    const std::array<FillRecord, 1> records{{
        {keys[2], icecc::digest128(object), object},
    }};
    FillStreamDecoder fill_decoder(1024);
    std::vector<FillRecord> decoded;
    for (const auto& message : encode_fill_messages(records, 32)) {
        auto part = fill_decoder.push(message);
        decoded.insert(decoded.end(), part.begin(), part.end());
    }
    fill_decoder.finish();
    require(decoded == std::vector<FillRecord>(records.begin(), records.end()),
            "generic FILL continuation did not round trip");
}

void test_p29v1_outer_streams_have_no_flags() {
    // Inner frame header is KIND + little-endian u32 length.
    const std::vector<uint8_t> need_inner{0xfe, 0, 0, 0, 0};
    const auto needs = encode_p29v1_need_messages(need_inner, 9, 64);
    require(needs.size() == 2 && needs.front().bytes.size() == 9 &&
                needs.front().bytes[8] == need_inner.front(),
            "P29V1 NEED did not use an eight-byte length prefix with no flags");
    P29V1NeedStreamDecoder need_decoder(64);
    for (const auto& message : needs)
        need_decoder.push(message);
    need_decoder.finish();
    require(need_decoder.inner_frames() == need_inner,
            "P29V1 NEED outer stream changed inner bytes");

    const std::vector<uint8_t> fill_inner{0xfe, 1, 0, 0, 0, 0};
    const auto fills = encode_p29v1_fill_messages(fill_inner, 9, 64);
    P29V1FillStreamDecoder fill_decoder(64);
    for (const auto& message : fills)
        fill_decoder.push(message);
    fill_decoder.finish();
    require(fill_decoder.take_inner_frames() == fill_inner,
            "P29V1 FILL outer stream changed inner bytes");

    require_throws(
        [&] { (void)encode_p29v1_need_messages(need_inner, 7, 64); },
        "P29V1 NEED admitted a cap smaller than its prefix");
    require_throws<std::length_error>(
        [&] { (void)encode_p29v1_fill_messages(fill_inner, 9, 5); },
        "P29V1 FILL admitted an inner stream over its bound");
}

void test_body_only_transaction_closure() {
    const auto body = bytes("BODY-only closure");
    TxBegin begin = make_begin(ProfileId::ZSTD_TU, body);
    const Digest128 first = begin.transaction_digest;
    require(compute_transaction_digest(begin, body) == first,
            "transaction digest is not deterministic");

    auto changed = body;
    changed.back() ^= 1;
    require_throws(
        [&] { (void)compute_transaction_digest(begin, changed); },
        "transaction digest accepted BODY bytes outside its descriptor");

    begin.body = describe_component(2, changed, begin.body.decoded_bytes);
    require(compute_transaction_digest(begin, changed) != first,
            "transaction digest did not close over BODY bytes");
}

}  // namespace

int main() {
    test_revision_one_registry();
    test_key_layout();
    test_fixed_wire_shapes_and_round_trip();
    test_r2_fixed_wire_shapes_and_negative_cases();
    test_negotiation_and_revision_refusal();
    test_framing();
    test_need_and_fill_streams();
    test_p29v1_outer_streams_have_no_flags();
    test_body_only_transaction_closure();
    std::puts("p50wire: R1/R2 golden and negative checks passed");
    return 0;
}
