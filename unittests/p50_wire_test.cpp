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
    static_assert(profile_bit(ProfileId::P29V1) == 32);
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
        ObjectType::P29Segment,
        static_cast<uint16_t>(KeyLayoutV1::generation_value_mask),
        KeyLayoutV1::ordinal_mask);
    require(maximum && maximum->type() == ObjectType::P29Segment &&
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

void append_p29_inner(std::vector<uint8_t>& output, uint8_t kind,
                      std::span<const uint8_t> payload) {
    output.push_back(kind);
    for (unsigned byte = 0; byte != 4; ++byte)
        output.push_back(static_cast<uint8_t>(payload.size() >> (8 * byte)));
    output.insert(output.end(), payload.begin(), payload.end());
}

void test_p29v1_outer_streams() {
    std::vector<uint8_t> need_inner;
    const std::vector<uint8_t> compressed_need(211, 0x5a);
    append_p29_inner(need_inner, 3, compressed_need);
    append_p29_inner(need_inner, 0xfe, {});
    const auto needs = encode_p29v1_need_messages(
        kP29V1SystemSourceReuseFlag, need_inner, 23, need_inner.size());
    require(needs.size() > 3, "P29V1 NEED did not fragment at a small cap");
    P29V1NeedStreamDecoder need_decoder(need_inner.size());
    for (const NeedMessage& message : needs)
        need_decoder.push(message);
    need_decoder.finish();
    require(need_decoder.flags() == kP29V1SystemSourceReuseFlag &&
                need_decoder.inner_frames() == need_inner,
            "P29V1 NEED fragments did not round-trip exactly");
    require_throws<std::invalid_argument>(
        [&] { need_decoder.push(NeedMessage{{}}); },
        "P29V1 accepted a second NEED stream");
    require_throws<std::invalid_argument>(
        [&] { (void)encode_p29v1_need_messages(2, need_inner, 64, 4096); },
        "P29V1 accepted an unknown NEED flag");
    require_throws<std::length_error>(
        [&] {
            (void)encode_p29v1_need_messages(0, need_inner, 64,
                                             need_inner.size() - 1);
        },
        "P29V1 accepted an oversized NEED inner stream");
    std::vector<uint8_t> no_need_end;
    append_p29_inner(no_need_end, 3, compressed_need);
    require_throws<std::invalid_argument>(
        [&] { (void)encode_p29v1_need_messages(0, no_need_end, 64, 4096); },
        "P29V1 accepted NEED without TU_END");
    P29V1NeedStreamDecoder short_need(need_inner.size());
    for (size_t index = 0; index + 1 < needs.size(); ++index)
        short_need.push(needs[index]);
    require_throws<std::logic_error>([&] { short_need.finish(); },
                                     "P29V1 accepted a truncated NEED stream");

    std::vector<uint8_t> fill_inner;
    const std::vector<uint8_t> control(307, 0x11);
    const std::vector<uint8_t> literal(509, 0x22);
    const std::vector<uint8_t> paths(71, 0x33);
    append_p29_inner(fill_inner, 8, control);
    append_p29_inner(fill_inner, 9, literal);
    append_p29_inner(fill_inner, 5, paths);
    const std::array<uint8_t, 1> mask{3};
    append_p29_inner(fill_inner, 0xfe, mask);
    const auto fills =
        encode_p29v1_fill_messages(fill_inner, 19, fill_inner.size());
    require(fills.size() > 20, "P29V1 FILL did not fragment at a small cap");
    P29V1FillStreamDecoder fill_decoder(fill_inner.size());
    for (const FillMessage& message : fills)
        fill_decoder.push(message);
    fill_decoder.finish();
    require(fill_decoder.inner_frames() == fill_inner,
            "P29V1 FILL fragments did not round-trip exactly");
    require_throws<std::invalid_argument>(
        [&] { fill_decoder.push(FillMessage{{}}); },
        "P29V1 accepted a second FILL stream");
    require_throws<std::length_error>(
        [&] {
            (void)encode_p29v1_fill_messages(fill_inner, 64,
                                             fill_inner.size() - 1);
        },
        "P29V1 accepted an oversized FILL inner stream");
    std::vector<uint8_t> wrong_order;
    append_p29_inner(wrong_order, 9, literal);
    append_p29_inner(wrong_order, 8, control);
    append_p29_inner(wrong_order, 0xfe, mask);
    require_throws<std::invalid_argument>(
        [&] { (void)encode_p29v1_fill_messages(wrong_order, 64, 4096); },
        "P29V1 accepted reordered FILL frames");
    std::vector<uint8_t> wrong_mask;
    append_p29_inner(wrong_mask, 8, control);
    const std::array<uint8_t, 1> literal_only_mask{2};
    append_p29_inner(wrong_mask, 0xfe, literal_only_mask);
    require_throws<std::invalid_argument>(
        [&] { (void)encode_p29v1_fill_messages(wrong_mask, 64, 4096); },
        "P29V1 accepted a FILL mask that differs from its frames");

    // The new profile uses existing descriptor fields; the legacy handshake
    // payloads remain byte-for-byte fixed-length for old readers.
    SessionHello hello;
    hello.c_store_guid = Id128::from_u64(0x2901);
    require(encode_payload(Message{hello}).size() == 36,
            "P29V1 changed the legacy SESSION_HELLO payload");
    SessionState state;
    state.f_store_guid = Id128::from_u64(0x2902);
    require(encode_payload(Message{state}).size() == 67,
            "P29V1 changed the legacy SESSION_STATE payload");
}

std::vector<Message> all_messages() {
    TxCommit commit{{99}, {4}, {7}, digest("tx"), digest("raw"), digest("post")};
    SessionState state;
    state.selected_protocol = kProtocolVersion;
    state.negotiated_profiles = profile_bit(ProfileId::ZSTD_TU) |
                                profile_bit(ProfileId::Z3_LONG);
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
    hello.supported_profiles = profile_bit(ProfileId::ZSTD_TU) |
                               profile_bit(ProfileId::Z3_LONG);
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
    hello.supported_profiles = profile_bit(ProfileId::ZSTD_TU) |
                               profile_bit(ProfileId::Z3_LONG);
    hello.limits = {256 * 1024, 8 * 1024 * 1024};
    const SessionSelection selected = negotiate_session(
        hello, 50, 52, profile_bit(ProfileId::ZSTD_TU),
        SessionLimits{128 * 1024, 16 * 1024 * 1024});
    require(selected.protocol == kProtocolVersion &&
                selected.negotiated_profiles == profile_bit(ProfileId::ZSTD_TU) &&
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
    bad_limit = hello;
    bad_limit.limits.max_frame_payload = kMandatoryControlFramePayload - 1;
    require_throws<std::invalid_argument>(
        [&] { (void)encode_frame(Message{bad_limit}); },
        "SESSION_HELLO accepted a frame cap too small for TX_BEGIN");
    SessionHello exact_control_cap = hello;
    exact_control_cap.limits.max_frame_payload = kMandatoryControlFramePayload;
    const SessionSelection exact_selection = negotiate_session(
        exact_control_cap, kProtocolVersion, kProtocolVersion,
        profile_bit(ProfileId::P29) | profile_bit(ProfileId::ZSTD_TU),
        SessionLimits{kInitialMaxFramePayload, kInitialMaxFillRecordBytes});
    require(exact_selection.limits.max_frame_payload == kMandatoryControlFramePayload &&
                exact_selection.negotiated_profiles == profile_bit(ProfileId::ZSTD_TU),
            "152-byte asymmetric control cap did not preserve the runnable profile intersection");

    TxBegin zstd;
    zstd.history_nonce = HistoryNonce{1};
    zstd.profile = ProfileId::ZSTD_TU;
    zstd.p29_root_mode = P29RootMode::NotApplicable;
    const std::vector<uint8_t> empty;
    zstd.dict = describe_component(0xfffe, empty, 0);
    zstd.body = describe_component(0xffff, empty, 0);
    zstd.raw_digest = icecc::digest128(empty);
    zstd.transaction_digest = compute_transaction_digest(zstd, empty, empty);
    const auto encoded = encode_frame(Message{zstd});
    require(encoded.size() == 4 + kMandatoryControlFramePayload,
            "TX_BEGIN no longer defines the mandatory control-frame minimum");
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
    hello.supported_profiles = profile_bit(ProfileId::ZSTD_TU) |
                               profile_bit(ProfileId::Z3_LONG) |
                               profile_bit(static_cast<ProfileId>(32));
    hello.limits = {128 * 1024, 8 * 1024 * 1024};

    const Digest128 post_state = digest("retained post state");
    SessionState state;
    state.selected_protocol = kProtocolVersion;
    state.negotiated_profiles = profile_bit(ProfileId::ZSTD_TU) |
                                profile_bit(ProfileId::Z3_LONG);
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
    absent.negotiated_profiles = profile_bit(ProfileId::ZSTD_TU);
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
    rejected.negotiated_profiles = profile_bit(ProfileId::GRZ);
    require_throws<std::invalid_argument>(
        [&] { validate_received(rejected); },
        "SESSION_STATE negotiated a profile outside the client offer");
    rejected = state;
    rejected.negotiated_profiles = 0;
    require_throws<std::invalid_argument>(
        [&] { validate_received(rejected); },
        "SESSION_STATE negotiated an empty profile set");
    rejected = state;
    rejected.negotiated_profiles = profile_bit(ProfileId::ZSTD_TU) |
                                   profile_bit(static_cast<ProfileId>(32));
    require_throws<std::invalid_argument>(
        [&] { validate_received(rejected); },
        "SESSION_STATE negotiated an unknown profile bit");
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

void test_reserved_zero_wire_values() {
    SessionHello hello;
    hello.c_store_guid = Id128::from_u64(79);
    const std::vector<uint8_t> hello_payload =
        encode_payload(Message{hello});
    SessionHello zero_c = hello;
    zero_c.c_store_guid = CStoreGuid{};
    require_throws<std::invalid_argument>(
        [&] { (void)encode_payload(Message{zero_c}); },
        "SESSION_HELLO encoded reserved zero C_STORE_GUID");
    std::vector<uint8_t> zero_c_payload = hello_payload;
    constexpr size_t c_guid_offset = 2 + 2;
    std::fill_n(zero_c_payload.begin() + c_guid_offset, 16, uint8_t{0});
    require_throws<std::invalid_argument>(
        [&] {
            (void)decode_payload(MessageType::SESSION_HELLO,
                                 zero_c_payload);
        },
        "SESSION_HELLO decoded reserved zero C_STORE_GUID");

    SessionState state;
    state.selected_protocol = kProtocolVersion;
    state.negotiated_profiles = profile_bit(ProfileId::ZSTD_TU);
    state.f_store_guid = Id128::from_u64(80);

    const SessionState absent_round_trip = std::get<SessionState>(decode_payload(
        MessageType::SESSION_STATE, encode_payload(Message{state})));
    require(absent_round_trip == state &&
                absent_round_trip.history_nonce == HistoryNonce{0} &&
                absent_round_trip.next_rel_seq == RelSeq{0} &&
                absent_round_trip.state_digest == Digest128{},
            "canonical absent route did not preserve its legal zero values");

    SessionState present_zero_digest = state;
    present_zero_digest.namespace_present = true;
    present_zero_digest.route_present = true;
    present_zero_digest.history_nonce = HistoryNonce{1};
    const std::vector<uint8_t> present_payload =
        encode_payload(Message{present_zero_digest});
    require(std::get<SessionState>(decode_payload(
                MessageType::SESSION_STATE, present_payload)) == present_zero_digest,
            "present route rejected legal REL_SEQ zero or all-zero state digest");

    SessionState zero_route_nonce = present_zero_digest;
    zero_route_nonce.history_nonce = HistoryNonce{0};
    require_throws<std::invalid_argument>(
        [&] { (void)encode_payload(Message{zero_route_nonce}); },
        "SESSION_STATE encoded a present route with zero HISTORY_NONCE");
    std::vector<uint8_t> zero_route_payload = present_payload;
    constexpr size_t state_nonce_offset = 2 + 4 + 4 + 8 + 16 + 1;
    std::fill_n(zero_route_payload.begin() + state_nonce_offset,
                sizeof(uint64_t), uint8_t{0});
    require_throws<std::invalid_argument>(
        [&] {
            (void)decode_payload(MessageType::SESSION_STATE,
                                 zero_route_payload);
        },
        "SESSION_STATE decoded a present route with zero HISTORY_NONCE");

    TxBegin legal_zeros;
    legal_zeros.history_nonce = HistoryNonce{1};
    legal_zeros.rel_seq = RelSeq{0};
    legal_zeros.tu_seq = TuSeq{0};
    legal_zeros.profile = ProfileId::P29;
    legal_zeros.p29_root_mode = P29RootMode::RouteHistory;
    legal_zeros.pre_state_digest = Digest128{};
    legal_zeros.dict = ComponentDescriptor{};
    legal_zeros.body = ComponentDescriptor{};
    legal_zeros.raw_bytes = 0;
    legal_zeros.raw_digest = Digest128{};
    legal_zeros.transaction_digest = Digest128{};
    const std::vector<uint8_t> legal_zero_begin_payload =
        encode_payload(Message{legal_zeros});
    require(std::get<TxBegin>(decode_payload(
                MessageType::TX_BEGIN, legal_zero_begin_payload)) == legal_zeros,
            "TX_BEGIN rejected legal zero cursor, empty input, or zero digest bits");
    TxBegin zero_begin_nonce = legal_zeros;
    zero_begin_nonce.history_nonce = HistoryNonce{0};
    require_throws<std::invalid_argument>(
        [&] { (void)encode_payload(Message{zero_begin_nonce}); },
        "TX_BEGIN encoded reserved zero HISTORY_NONCE");
    std::vector<uint8_t> zero_begin_payload = legal_zero_begin_payload;
    std::fill_n(zero_begin_payload.begin(), sizeof(uint64_t), uint8_t{0});
    require_throws<std::invalid_argument>(
        [&] { (void)decode_payload(MessageType::TX_BEGIN, zero_begin_payload); },
        "TX_BEGIN decoded reserved zero HISTORY_NONCE");

    const TxCommit legal_zero_commit{HistoryNonce{1}, RelSeq{0}, TuSeq{0},
                                     Digest128{}, Digest128{}, Digest128{}};
    const std::vector<uint8_t> legal_zero_commit_payload =
        encode_payload(Message{legal_zero_commit});
    require(std::get<TxCommit>(decode_payload(
                MessageType::TX_COMMIT, legal_zero_commit_payload)) ==
                legal_zero_commit,
            "TX_COMMIT rejected legal zero cursor or digest fields");
    TxCommit zero_commit_nonce = legal_zero_commit;
    zero_commit_nonce.history_nonce = HistoryNonce{0};
    require_throws<std::invalid_argument>(
        [&] { (void)encode_payload(Message{zero_commit_nonce}); },
        "TX_COMMIT encoded reserved zero HISTORY_NONCE");
    std::vector<uint8_t> zero_commit_payload = legal_zero_commit_payload;
    std::fill_n(zero_commit_payload.begin(), sizeof(uint64_t), uint8_t{0});
    require_throws<std::invalid_argument>(
        [&] {
            (void)decode_payload(MessageType::TX_COMMIT,
                                 zero_commit_payload);
        },
        "TX_COMMIT decoded reserved zero HISTORY_NONCE");

    SessionState retained_zero_digests = present_zero_digest;
    retained_zero_digests.next_rel_seq = RelSeq{1};
    retained_zero_digests.last_commit = legal_zero_commit;
    const std::vector<uint8_t> retained_payload =
        encode_payload(Message{retained_zero_digests});
    require(std::get<SessionState>(decode_payload(
                MessageType::SESSION_STATE, retained_payload)) ==
                retained_zero_digests,
            "retained commit rejected legal zero TU_SEQ or digest fields");
    SessionState retained_zero_nonce = retained_zero_digests;
    retained_zero_nonce.last_commit->history_nonce = HistoryNonce{0};
    require_throws<std::invalid_argument>(
        [&] { (void)encode_payload(Message{retained_zero_nonce}); },
        "SESSION_STATE encoded a retained commit with zero HISTORY_NONCE");
    std::vector<uint8_t> retained_zero_nonce_payload = retained_payload;
    constexpr size_t retained_commit_nonce_offset =
        state_nonce_offset + 8 + 8 + 16;
    std::fill_n(retained_zero_nonce_payload.begin() + retained_commit_nonce_offset,
                sizeof(uint64_t), uint8_t{0});
    require_throws<std::invalid_argument>(
        [&] {
            (void)decode_payload(MessageType::SESSION_STATE,
                                 retained_zero_nonce_payload);
        },
        "SESSION_STATE decoded a retained commit with zero HISTORY_NONCE");

    SessionState zero_f = state;
    zero_f.f_store_guid = FStoreGuid{};
    require_throws<std::invalid_argument>(
        [&] { (void)encode_payload(Message{zero_f}); },
        "SESSION_STATE encoded reserved zero F_STORE_GUID");

    std::vector<uint8_t> state_payload = encode_payload(Message{state});
    constexpr size_t f_guid_offset = 2 + 4 + 4 + 8;
    require(state_payload.size() >= f_guid_offset + 16,
            "SESSION_STATE zero-GUID fixture is too short");
    std::fill_n(state_payload.begin() + f_guid_offset, 16, uint8_t{0});
    require_throws<std::invalid_argument>(
        [&] {
            (void)decode_payload(MessageType::SESSION_STATE, state_payload);
        },
        "SESSION_STATE decoded reserved zero F_STORE_GUID");

    const HistoryReset reset{HistoryNonce{1}, digest("initial state")};
    require_throws<std::invalid_argument>(
        [&] {
            (void)encode_payload(
                Message{HistoryReset{HistoryNonce{0}, reset.initial_state_digest}});
        },
        "HISTORY_RESET encoded reserved zero nonce");
    std::vector<uint8_t> reset_payload = encode_payload(Message{reset});
    std::fill_n(reset_payload.begin(), sizeof(uint64_t), uint8_t{0});
    require_throws<std::invalid_argument>(
        [&] {
            (void)decode_payload(MessageType::HISTORY_RESET, reset_payload);
        },
        "HISTORY_RESET decoded reserved zero nonce");

    require_throws<std::invalid_argument>(
        [&] { (void)encode_payload(Message{ErrorMessage{0, "reserved"}}); },
        "ERROR encoded reserved zero code");
    std::vector<uint8_t> error_payload =
        encode_payload(Message{ErrorMessage{1, "bounded"}});
    error_payload[0] = 0;
    error_payload[1] = 0;
    require_throws<std::invalid_argument>(
        [&] { (void)decode_payload(MessageType::ERROR, error_payload); },
        "ERROR decoded reserved zero code");
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
    test_reserved_zero_wire_values();
    test_ten_messages_and_four_byte_parser();
    test_need_delta_stream();
    test_fill_records_cross_every_boundary();
    test_p29v1_outer_streams();
    test_non_circular_closure();
    std::cout << "p50_wire_test: all wire and closure gates passed\n";
    return 0;
}
