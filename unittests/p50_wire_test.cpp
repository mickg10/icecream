#include "cache/protocol50.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using namespace icecc::p50;

[[noreturn]] void fail(std::string_view text) {
    std::cerr << "p50_wire_test: " << text << '\n';
    std::exit(1);
}

void require(bool value, std::string_view text) {
    if (!value) fail(text);
}

template<class Exception, class Callable>
void require_throws(Callable&& callable, std::string_view text) {
    try {
        callable();
    } catch (const Exception&) {
        return;
    } catch (...) {
        fail(std::string(text) + " (wrong exception)");
    }
    fail(std::string(text) + " (no exception)");
}

std::vector<uint8_t> bytes(std::string_view text) {
    return {text.begin(), text.end()};
}

Digest128 digest(std::string_view text) { return icecc::digest128(bytes(text)); }

SessionState receive_session_state(const SessionState& sent) {
    const std::vector<uint8_t> encoded = encode_frame(Message{sent});
    FrameParser parser;
    const std::vector<Frame> frames = parser.feed(encoded);
    parser.finish();
    require(frames.size() == 1 &&
                frames.front().type == MessageType::SESSION_STATE,
            "SESSION_STATE wire fixture did not produce one state frame");
    return std::get<SessionState>(
        decode_payload(frames.front().type, frames.front().payload));
}

void test_digest128_contract() {
    require(icecc::digest128_hex(digest("")) ==
                "99aa06d3014798d86001c324468d497f",
            "XXH3-128 empty-input canonical vector changed");
    icecc::Digest128Builder split;
    split.append("canonical ");
    split.append("stream");
    require(split.finish() == digest("canonical stream"),
            "streaming and one-shot XXH3-128 differ");
    require_throws<std::logic_error>([&] { (void)split.finish(); },
                                     "digest builder finished twice");
}

void test_key_layout_v1() {
    static_assert(profile_bit(ProfileId::P29) == 1);
    static_assert(profile_bit(ProfileId::GRZ) == 4);
    static_assert(profile_bit(static_cast<ProfileId>(0)) == 0);
    static_assert(profile_bit(static_cast<ProfileId>(32)) ==
                  (uint32_t{1} << 31));
    static_assert(profile_bit(static_cast<ProfileId>(33)) == 0);
    static_assert(KeyLayoutV1::type_bits == 5);
    static_assert(KeyLayoutV1::generation_bits == 10);
    static_assert(KeyLayoutV1::ordinal_bits == 49);
    static_assert(KeyLayoutV1::type_shift == 59);
    static_assert(KeyLayoutV1::generation_shift == 49);
    static_assert(sizeof(HistoryNonce) == sizeof(uint64_t));

    const auto maximum = Key64::make(
        ObjectType::Blob, static_cast<uint16_t>(KeyLayoutV1::generation_value_mask),
        KeyLayoutV1::ordinal_mask);
    require(maximum && maximum->type() == ObjectType::Blob &&
                maximum->generation() == KeyLayoutV1::generation_value_mask &&
                maximum->ordinal() == KeyLayoutV1::ordinal_mask,
            "KeyLayoutV1 maximum did not round-trip");
    const auto generation_zero = Key64::make(ObjectType::Line, 0, 1);
    require(generation_zero && generation_zero->generation() == 0,
            "generation zero must be usable");
    require(!Key64::make(ObjectType::Line, 0, 0), "ordinal zero must be invalid");
    require(!Key64::make(ObjectType::Line, 0, KeyLayoutV1::ordinal_mask + 1),
            "oversized ordinal must be invalid");
    require(!Key64::from_wire(0), "zero wire Key64 must be invalid");
    require(!Key64::from_wire((uint64_t{31} << KeyLayoutV1::type_shift) | 1),
            "unassigned object type must be invalid");
}

std::vector<Message> all_messages() {
    TxCommit commit{{99}, {4}, {7}, digest("tx"), digest("raw"), digest("post")};
    SessionState state;
    state.selected_protocol = kProtocolVersion;
    state.selected_profile = ProfileId::P29;
    state.limits = {65536, 1U << 24};
    state.f_store_guid = Id128::from_u64(2);
    state.namespace_present = true;
    state.route_present = true;
    state.history_nonce = {99};
    state.next_rel_seq = {5};
    state.state_digest = commit.post_state_digest;
    state.last_commit = commit;
    SessionHello hello;
    hello.min_protocol = 49;
    hello.max_protocol = kProtocolVersion;
    hello.c_store_guid = Id128::from_u64(1);
    hello.supported_profiles = profile_bit(ProfileId::P29) |
                               profile_bit(ProfileId::ZSTD_TU);
    hello.limits = {131072, 1U << 25};
    TxBegin begin;
    begin.history_nonce = {99};
    begin.rel_seq = {4};
    begin.tu_seq = {7};
    begin.profile = ProfileId::P29;
    begin.p29_root_mode = P29RootMode::RouteHistory;
    begin.pre_state_digest = digest("pre");
    begin.dict = {1, 3, 8, digest("dict")};
    begin.body = {2, 4, 9, digest("body")};
    begin.raw_bytes = 10;
    begin.raw_digest = digest("raw");
    begin.transaction_digest = digest("tx");
    return {
        hello,
        state,
        HistoryReset{{100}, digest("initial")},
        ErrorMessage{17, "closed transaction"},
        begin,
        DictMessage{bytes("dictionary")},
        BodyMessage{bytes("body")},
        NeedMessage{bytes("need")},
        FillMessage{bytes("fill")},
        commit,
    };
}

void test_session_negotiation() {
    SessionHello hello;
    hello.min_protocol = 49;
    hello.max_protocol = 51;
    hello.c_store_guid = Id128::from_u64(44);
    hello.supported_profiles = profile_bit(ProfileId::P29) |
                               profile_bit(ProfileId::ZSTD_TU);
    hello.limits = {256 * 1024, 8 * 1024 * 1024};
    const SessionSelection selected = negotiate_session(
        hello, 50, 52, profile_bit(ProfileId::ZSTD_TU),
        SessionLimits{128 * 1024, 16 * 1024 * 1024});
    require(selected.protocol == kProtocolVersion &&
                selected.profile == ProfileId::ZSTD_TU &&
                selected.limits.max_frame_payload == 128 * 1024 &&
                selected.limits.max_fill_record_bytes == 8 * 1024 * 1024,
            "session did not select implemented Protocol 50 and smaller limits");

    SessionHello no_version = hello;
    no_version.max_protocol = 49;
    require_throws<std::invalid_argument>(
        [&] { (void)negotiate_session(no_version); },
        "client range without Protocol 50 negotiated");
    require_throws<std::invalid_argument>(
        [&] {
            (void)negotiate_session(
                hello, 51, 52, profile_bit(ProfileId::ZSTD_TU));
        },
        "server range without Protocol 50 negotiated");
    require_throws<std::invalid_argument>(
        [&] {
            (void)negotiate_session(
                hello, 50, 50, profile_bit(ProfileId::GRZ));
        },
        "disjoint profile sets negotiated");
    SessionHello bad_limit = hello;
    bad_limit.limits.max_frame_payload = kInitialMaxFramePayload + 1;
    require_throws<std::invalid_argument>(
        [&] { (void)encode_frame(Message{bad_limit}); },
        "SESSION_HELLO accepted a frame cap above the V1 bound");

    TxBegin zstd;
    zstd.profile = ProfileId::ZSTD_TU;
    zstd.p29_root_mode = P29RootMode::NotApplicable;
    const std::vector<uint8_t> empty;
    zstd.dict = describe_component(0xfffe, empty, 0);
    zstd.body = describe_component(0xffff, empty, 0);
    zstd.raw_digest = icecc::digest128(empty);
    zstd.transaction_digest = compute_transaction_digest(zstd, empty, empty);
    const auto encoded = encode_frame(Message{zstd});
    FrameParser parser;
    const auto frames = parser.feed(encoded);
    require(frames.size() == 1 &&
                std::get<TxBegin>(decode_payload(frames[0].type,
                                                 frames[0].payload)) == zstd,
            "generic wire did not preserve future component encodings");
    zstd.p29_root_mode = P29RootMode::HistoryIndependent;
    require_throws<std::invalid_argument>(
        [&] { (void)encode_frame(Message{zstd}); },
        "non-P29 profile accepted a P29 root mode");
}

void test_received_session_state_validation() {
    SessionHello hello;
    hello.min_protocol = kProtocolVersion;
    hello.max_protocol = kProtocolVersion + 1;
    hello.c_store_guid = Id128::from_u64(70);
    hello.supported_profiles = profile_bit(ProfileId::P29) |
                               profile_bit(ProfileId::ZSTD_TU) |
                               profile_bit(static_cast<ProfileId>(32));
    hello.limits = {128 * 1024, 8 * 1024 * 1024};

    const Digest128 post_state = digest("retained post state");
    SessionState state;
    state.selected_protocol = kProtocolVersion;
    state.selected_profile = ProfileId::P29;
    state.limits = {64 * 1024, 4 * 1024 * 1024};
    state.f_store_guid = Id128::from_u64(71);
    state.namespace_present = true;
    state.route_present = true;
    state.history_nonce = {72};
    state.next_rel_seq = {9};
    state.state_digest = post_state;
    state.last_commit = TxCommit{{72}, {8}, {73}, digest("transaction"),
                                 digest("raw"), post_state};

    const SessionState received = receive_session_state(state);
    validate_session_state(hello, received);

    SessionState fresh_route = state;
    fresh_route.next_rel_seq = {0};
    fresh_route.state_digest = digest("initial route");
    fresh_route.last_commit.reset();
    validate_session_state(hello, receive_session_state(fresh_route));

    SessionState absent;
    absent.selected_protocol = kProtocolVersion;
    absent.selected_profile = ProfileId::P29;
    absent.limits = state.limits;
    absent.f_store_guid = state.f_store_guid;
    validate_session_state(hello, receive_session_state(absent));

    SessionState absent_namespace_with_route_data = absent;
    absent_namespace_with_route_data.history_nonce = {1};
    require_throws<std::invalid_argument>(
        [&] { (void)receive_session_state(absent_namespace_with_route_data); },
        "absent namespace encoded noncanonical route data");
    SessionState absent_route_with_digest = absent;
    absent_route_with_digest.namespace_present = true;
    absent_route_with_digest.state_digest = digest("stale route");
    require_throws<std::invalid_argument>(
        [&] { (void)receive_session_state(absent_route_with_digest); },
        "absent route encoded a stale state digest");

    const auto validate_received = [&](const SessionState& candidate) {
        validate_session_state(hello, receive_session_state(candidate));
    };

    SessionState rejected = state;
    rejected.selected_protocol = kProtocolVersion - 1;
    require_throws<std::invalid_argument>(
        [&] { validate_received(rejected); },
        "SESSION_STATE selected an unimplemented lower protocol");
    rejected = state;
    rejected.selected_protocol = kProtocolVersion + 1;
    require_throws<std::invalid_argument>(
        [&] { validate_received(rejected); },
        "SESSION_STATE selected an unimplemented higher protocol");
    rejected = state;
    rejected.selected_profile = ProfileId::GRZ;
    require_throws<std::invalid_argument>(
        [&] { validate_received(rejected); },
        "SESSION_STATE selected a profile outside the client offer");
    rejected = state;
    rejected.limits.max_frame_payload = hello.limits.max_frame_payload + 1;
    require_throws<std::invalid_argument>(
        [&] { validate_received(rejected); },
        "SESSION_STATE increased the client's offered frame cap");
    rejected = state;
    rejected.limits.max_fill_record_bytes =
        hello.limits.max_fill_record_bytes + 1;
    require_throws<std::invalid_argument>(
        [&] { validate_received(rejected); },
        "SESSION_STATE increased the client's offered FILL-record cap");

    SessionHello invalid_hello = hello;
    invalid_hello.min_protocol = invalid_hello.max_protocol + 1;
    require_throws<std::invalid_argument>(
        [&] { validate_session_state(invalid_hello, received); },
        "client receive gate skipped intrinsic SESSION_HELLO validation");

    rejected = state;
    rejected.last_commit->history_nonce.value++;
    require_throws<std::invalid_argument>(
        [&] { validate_session_state(hello, rejected); },
        "SESSION_STATE retained a commit from another route nonce");
    rejected = state;
    rejected.last_commit->rel_seq = rejected.next_rel_seq;
    require_throws<std::invalid_argument>(
        [&] { validate_session_state(hello, rejected); },
        "SESSION_STATE retained a commit outside the preceding REL_SEQ");
    rejected = state;
    rejected.next_rel_seq = {0};
    rejected.last_commit->rel_seq = {std::numeric_limits<uint64_t>::max()};
    require_throws<std::invalid_argument>(
        [&] { validate_session_state(hello, rejected); },
        "SESSION_STATE accepted a wrapped predecessor at REL_SEQ zero");
    rejected = state;
    rejected.last_commit->post_state_digest = digest("different post state");
    require_throws<std::invalid_argument>(
        [&] { validate_session_state(hello, rejected); },
        "SESSION_STATE retained a commit with a different post-state digest");
    rejected = state;
    rejected.last_commit.reset();
    require_throws<std::invalid_argument>(
        [&] { validate_session_state(hello, rejected); },
        "SESSION_STATE omitted the latest commit for an advanced route");
}

void test_ten_messages_and_four_byte_parser() {
    require_throws<std::invalid_argument>([] { (void)FrameParser(0); },
                                          "zero frame cap was accepted");
    const std::vector<Message> messages = all_messages();
    require(messages.size() == 10, "fixture does not cover exactly ten messages");
    std::vector<uint8_t> stream;
    for (size_t index = 0; index != messages.size(); ++index) {
        require(static_cast<uint8_t>(message_type(messages[index])) == index + 1,
                "message identifiers are not the frozen ten-message vocabulary");
        const std::vector<uint8_t> frame = encode_frame(messages[index]);
        require(frame.size() >= 4 && frame[0] == index + 1,
                "frame header does not put type in the high byte");
        stream.insert(stream.end(), frame.begin(), frame.end());
    }

    FrameParser parser;
    std::vector<Frame> frames;
    for (uint8_t byte : stream) {
        const std::array<uint8_t, 1> one{byte};
        std::vector<Frame> produced = parser.feed(one);
        frames.insert(frames.end(), produced.begin(), produced.end());
    }
    parser.finish();
    require(frames.size() == messages.size(), "byte-at-a-time parser lost a frame");
    for (size_t index = 0; index != frames.size(); ++index)
        require(decode_payload(frames[index].type, frames[index].payload) == messages[index],
                "message payload did not round-trip");

    std::vector<uint8_t> short_frame = encode_frame(messages.front());
    short_frame.pop_back();
    FrameParser short_parser;
    (void)short_parser.feed(short_frame);
    require_throws<std::invalid_argument>([&] { short_parser.finish(); },
                                          "one-byte-short frame was accepted");

    std::vector<uint8_t> long_frame = encode_frame(messages.front());
    long_frame.push_back(0);
    FrameParser long_parser;
    require(long_parser.feed(long_frame).size() == 1,
            "valid prefix of one-byte-long stream was not parsed");
    require_throws<std::invalid_argument>([&] { long_parser.finish(); },
                                          "one-byte-long framed stream was accepted");

    const std::array<uint8_t, 4> unknown{0xff, 0, 0, 0};
    FrameParser unknown_parser;
    require_throws<std::invalid_argument>([&] { (void)unknown_parser.feed(unknown); },
                                          "unknown message type was accepted");

    MessageSessionGate gate;
    gate.observe(MessageType::SESSION_HELLO);
    gate.observe(MessageType::ERROR);
    require(gate.terminal(), "ERROR did not make the message session terminal");
    require_throws<std::logic_error>(
        [&] { gate.observe(MessageType::SESSION_STATE); },
        "message was accepted after terminal ERROR");
}

void test_need_delta_stream() {
    std::vector<Key64> keys;
    for (uint64_t ordinal : {1ULL, 2ULL, 129ULL, 1000000ULL})
        keys.push_back(*Key64::make(ObjectType::Line, 3, ordinal));
    const std::vector<NeedMessage> messages = encode_need_messages(keys, 17);
    require(messages.size() > 1 && messages.front().bytes.size() == 17,
            "NEED fixture did not exercise continuation frames");
    NeedStreamDecoder decoder;
    for (const NeedMessage& message : messages) {
        const std::vector<uint8_t> frame = encode_frame(Message{message});
        FrameParser parser;
        for (uint8_t byte : frame) {
            const std::array<uint8_t, 1> one{byte};
            for (Frame& parsed : parser.feed(one))
                decoder.push(std::get<NeedMessage>(
                    decode_payload(parsed.type, parsed.payload)));
        }
        parser.finish();
    }
    decoder.finish();
    require(decoder.keys() == keys, "split NEED delta stream changed the exact set");

    NeedStreamDecoder empty;
    const auto empty_messages = encode_need_messages({}, 16);
    require(empty_messages.size() == 1, "empty Need must have one totals payload");
    empty.push(empty_messages.front());
    empty.finish();
    require(empty.keys().empty(), "empty Need decoded nonempty");

    NeedMessage noncanonical;
    noncanonical.bytes.assign(16, 0);
    noncanonical.bytes[7] = 1;
    noncanonical.bytes[15] = 2;
    noncanonical.bytes.push_back(0x81);
    noncanonical.bytes.push_back(0x00);
    NeedStreamDecoder strict;
    strict.push(noncanonical);
    require_throws<std::invalid_argument>([&] { strict.finish(); },
                                          "non-minimal NEED delta was accepted");

    std::reverse(keys.begin(), keys.end());
    require_throws<std::invalid_argument>(
        [&] { (void)encode_need_messages(keys, 64); },
        "unsorted Need was accepted");
}

void test_fill_records_cross_every_boundary() {
    require_throws<std::invalid_argument>([] { (void)FillStreamDecoder(31); },
                                          "undersized FILL-record cap was accepted");
    std::vector<uint8_t> large((1U << 20) + 33333);
    for (size_t i = 0; i != large.size(); ++i) large[i] = static_cast<uint8_t>(i * 17);
    const FillRecord first{*Key64::make(ObjectType::Blob, 7, 1), digest("first"),
                           bytes("small")};
    const FillRecord second{*Key64::make(ObjectType::Blob, 7, 2), digest("second"),
                            large};
    const std::array<FillRecord, 2> records{first, second};
    const std::vector<FillMessage> messages = encode_fill_messages(records, 997);
    require(messages.size() > 1000, "large FILL record did not cross frame boundaries");
    FillStreamDecoder decoder(large.size() + 64);
    std::vector<FillRecord> decoded;
    for (const FillMessage& message : messages) {
        std::vector<FillRecord> part = decoder.push(message);
        decoded.insert(decoded.end(), part.begin(), part.end());
    }
    decoder.finish();
    require(decoded == std::vector<FillRecord>(records.begin(), records.end()),
            "self-delimiting FILL records changed across frames");

    std::vector<FillMessage> truncated = messages;
    truncated.back().bytes.pop_back();
    FillStreamDecoder short_decoder(large.size() + 64);
    for (const FillMessage& message : truncated) (void)short_decoder.push(message);
    require_throws<std::invalid_argument>([&] { short_decoder.finish(); },
                                          "truncated FILL record was accepted");
    require(encode_fill_messages({}, 64).empty(), "empty Need must require no Fill");
}

void test_non_circular_closure() {
    const std::vector<uint8_t> dict = bytes("dict-stream");
    const std::vector<uint8_t> body = bytes("body-stream");
    TxBegin begin;
    begin.history_nonce = {std::numeric_limits<uint64_t>::max()};
    begin.rel_seq = {4};
    begin.tu_seq = {3};
    begin.profile = ProfileId::P29;
    begin.p29_root_mode = P29RootMode::RouteHistory;
    begin.pre_state_digest = digest("pre");
    begin.dict = describe_component(1, dict, 17);
    begin.body = describe_component(1, body, 19);
    begin.raw_bytes = 23;
    begin.raw_digest = digest("raw");
    const Digest128 first = compute_transaction_digest(begin, dict, body);
    begin.transaction_digest = digest("ignored self field");
    require(compute_transaction_digest(begin, dict, body) == first,
            "transaction digest circularly includes itself");
    require(compute_post_state_digest(begin.pre_state_digest, begin.history_nonce,
                                      begin.rel_seq, begin.tu_seq, first) != first,
            "post-state digest did not bind the transaction tuple");

    const std::vector<uint8_t> empty;
    TxBegin zero;
    zero.dict = describe_component(0, empty, 0);
    zero.body = describe_component(0, empty, 0);
    zero.raw_digest = icecc::digest128(empty);
    zero.transaction_digest = compute_transaction_digest(zero, empty, empty);
    require(zero.dict.encoded_bytes == 0 && zero.body.encoded_bytes == 0,
            "zero component descriptors are not representable");
}

}  // namespace

int main() {
    test_digest128_contract();
    test_key_layout_v1();
    test_session_negotiation();
    test_received_session_state_validation();
    test_ten_messages_and_four_byte_parser();
    test_need_delta_stream();
    test_fill_records_cross_every_boundary();
    test_non_circular_closure();
    std::cout << "p50_wire_test: all wire and closure gates passed\n";
    return 0;
}
