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
    static_assert(std::variant_size_v<Message> == 9);

    require(profile_name(ProfileId::P29V1) == "p29_v1" &&
                profile_name(ProfileId::ZSTD_TU) == "zstd_tu" &&
                profile_name(ProfileId::ZSTD_ROUTE) == "zstd_route",
            "revision-one profile names changed");
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
    test_negotiation_and_revision_refusal();
    test_framing();
    test_need_and_fill_streams();
    test_p29v1_outer_streams_have_no_flags();
    test_body_only_transaction_closure();
    return 0;
}
