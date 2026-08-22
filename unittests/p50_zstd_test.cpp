#include "cache/p50_zstd.h"

#include <zstd.h>

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace icecc::p50;

[[noreturn]] void fail(std::string_view text) {
    std::cerr << "p50_zstd_test: " << text << '\n';
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

ZstdTuLimits limits(uint64_t encoded = 1U << 20,
                    uint64_t raw = 1U << 20) {
    ZstdTuLimits result{encoded, raw, 0};
#if ZSTD_VERSION_NUMBER >= 10400
    result.max_window_log = 20;
#endif
    return result;
}

std::vector<uint8_t> sample_input() {
    std::vector<uint8_t> result(128 * 1024);
    for (size_t i = 0; i != result.size(); ++i)
        result[i] = static_cast<uint8_t>((i * 37 + i / 97) & 0xff);
    return result;
}

TxBegin describe_modified_body(TxBegin begin,
                               std::span<const uint8_t> body) {
    begin.body = describe_component(kZstdTuBodyEncoding, body,
                                    begin.raw_bytes);
    begin.transaction_digest = compute_transaction_digest(
        begin, std::span<const uint8_t>{}, body);
    return begin;
}

void append_one_byte_at_a_time(ZstdTuDialogue& dialogue,
                               std::span<const uint8_t> body) {
    for (uint8_t byte : body) {
        const std::array<uint8_t, 1> one{byte};
        dialogue.append_body(BodyMessage{{one.begin(), one.end()}});
    }
}

void test_round_trip_and_persistent_dialogue() {
    const std::vector<uint8_t> input = sample_input();
    const ZstdTuEnvelope first = encode_zstd_tu(
        HistoryNonce{10}, RelSeq{0}, TuSeq{20}, icecc::digest128("pre"), input);
    require(first.begin.profile == ProfileId::ZSTD_TU &&
                first.begin.p29_root_mode == P29RootMode::NotApplicable &&
                first.begin.dict.encoded_bytes == 0 && !first.body.empty(),
            "encoder did not produce canonical ZSTD_TU components");
    require(decode_zstd_tu(first.begin, first.body, limits()) == input,
            "direct ZSTD_TU decode changed exact input");

    ZstdTuDialogue dialogue(profile_bit(ProfileId::ZSTD_TU), limits());
    dialogue.begin(first.begin);
    require_throws<std::logic_error>([&] { (void)dialogue.materialize(); },
                                     "incomplete BODY materialized");
    require(dialogue.state() == ZstdTuDialogue::State::ReceivingBody,
            "early materialize changed dialogue state");
    append_one_byte_at_a_time(dialogue, first.body);
    require(dialogue.state() == ZstdTuDialogue::State::BodyClosed,
            "exact BODY count did not close the logical stream");
    require(dialogue.materialize() == input,
            "dialogue ZSTD_TU decode changed exact input");
    dialogue.commit_visible();
    require(dialogue.state() == ZstdTuDialogue::State::Idle &&
                dialogue.pending_body_bytes() == 0 &&
                !dialogue.active_begin(),
            "visible commit did not release the dialogue body");

    const std::vector<uint8_t> empty;
    const ZstdTuEnvelope second = encode_zstd_tu(
        HistoryNonce{10}, RelSeq{1}, TuSeq{21}, first.begin.raw_digest, empty);
    dialogue.begin(second.begin);
    dialogue.append_body(BodyMessage{second.body});
    require(dialogue.materialize().empty(), "empty input decoded nonempty");
    dialogue.commit_visible();
}

void test_single_frame_exactness() {
    const std::vector<uint8_t> input = sample_input();
    const ZstdTuEnvelope envelope = encode_zstd_tu(
        HistoryNonce{1}, RelSeq{0}, TuSeq{2}, icecc::digest128("pre"), input);

    std::vector<uint8_t> trailing = envelope.body;
    trailing.push_back(0);
    const TxBegin trailing_begin = describe_modified_body(envelope.begin, trailing);
    require_throws<std::invalid_argument>(
        [&] { (void)decode_zstd_tu(trailing_begin, trailing, limits()); },
        "trailing byte after the Zstd frame was accepted");

    const std::vector<uint8_t> empty;
    const ZstdTuEnvelope empty_frame = encode_zstd_tu(
        HistoryNonce{1}, RelSeq{1}, TuSeq{3}, icecc::digest128("pre2"), empty);
    std::vector<uint8_t> concatenated = envelope.body;
    concatenated.insert(concatenated.end(), empty_frame.body.begin(),
                        empty_frame.body.end());
    const TxBegin concatenated_begin =
        describe_modified_body(envelope.begin, concatenated);
    require_throws<std::invalid_argument>(
        [&] {
            (void)decode_zstd_tu(concatenated_begin, concatenated, limits());
        },
        "concatenated second Zstd frame was accepted");

    std::vector<uint8_t> malformed{1, 2, 3, 4, 5, 6};
    TxBegin malformed_begin = describe_modified_body(envelope.begin, malformed);
    require_throws<std::invalid_argument>(
        [&] { (void)decode_zstd_tu(malformed_begin, malformed, limits()); },
        "non-Zstd BODY was accepted");
}

void test_digest_and_shape_gates() {
    const std::vector<uint8_t> input = sample_input();
    const ZstdTuEnvelope envelope = encode_zstd_tu(
        HistoryNonce{5}, RelSeq{0}, TuSeq{6}, icecc::digest128("pre"), input);

    std::vector<uint8_t> corrupted = envelope.body;
    corrupted[corrupted.size() / 2] ^= 1;
    require_throws<std::invalid_argument>(
        [&] { (void)decode_zstd_tu(envelope.begin, corrupted, limits()); },
        "encoded BODY digest mismatch was accepted");

    TxBegin wrong_transaction = envelope.begin;
    wrong_transaction.transaction_digest = icecc::digest128("wrong tx");
    require_throws<std::invalid_argument>(
        [&] {
            (void)decode_zstd_tu(wrong_transaction, envelope.body, limits());
        },
        "transaction digest mismatch was accepted");

    TxBegin wrong_raw = envelope.begin;
    wrong_raw.raw_digest = icecc::digest128("wrong raw");
    wrong_raw.transaction_digest = compute_transaction_digest(
        wrong_raw, std::span<const uint8_t>{}, envelope.body);
    require_throws<std::invalid_argument>(
        [&] { (void)decode_zstd_tu(wrong_raw, envelope.body, limits()); },
        "raw digest mismatch was accepted");

    TxBegin nonempty_dict = envelope.begin;
    const std::array<uint8_t, 1> byte{7};
    nonempty_dict.dict = describe_component(1, byte, 1);
    nonempty_dict.transaction_digest = compute_transaction_digest(
        nonempty_dict, byte, envelope.body);
    require_throws<std::invalid_argument>(
        [&] {
            (void)decode_zstd_tu(nonempty_dict, envelope.body, limits());
        },
        "ZSTD_TU accepted a nonempty DICT");

    TxBegin terminal_rel = envelope.begin;
    terminal_rel.rel_seq = {std::numeric_limits<uint64_t>::max()};
    terminal_rel.transaction_digest = compute_transaction_digest(
        terminal_rel, std::span<const uint8_t>{}, envelope.body);
    require_throws<std::overflow_error>(
        [&] { (void)decode_zstd_tu(terminal_rel, envelope.body, limits()); },
        "ZSTD_TU accepted terminal REL_SEQ");
}

void test_caps_and_terminal_dialogue_errors() {
    const std::vector<uint8_t> input = sample_input();
    const ZstdTuEnvelope envelope = encode_zstd_tu(
        HistoryNonce{7}, RelSeq{0}, TuSeq{8}, icecc::digest128("pre"), input);

    require_throws<std::length_error>(
        [&] {
            (void)decode_zstd_tu(
                envelope.begin, envelope.body,
                limits(envelope.body.size() - 1, input.size()));
        },
        "encoded BODY cap was not enforced before decode");
    require_throws<std::length_error>(
        [&] {
            (void)decode_zstd_tu(
                envelope.begin, envelope.body,
                limits(envelope.body.size(), input.size() - 1));
        },
        "raw input cap was not enforced before allocation");
    require_throws<std::invalid_argument>(
        [] { (void)ZstdTuDialogue(profile_bit(ProfileId::ZSTD_TU), {0, 1, 0}); },
        "zero encoded cap was accepted");
    require_throws<std::invalid_argument>(
        [] { (void)ZstdTuDialogue(profile_bit(ProfileId::ZSTD_TU), {1, 1, -1}); },
        "negative window-log cap was accepted");

    {
        ZstdTuDialogue dialogue(profile_bit(ProfileId::P29), limits());
        require_throws<std::invalid_argument>([&] { dialogue.begin(envelope.begin); },
                                              "unnegotiated ZSTD_TU began");
        require(dialogue.terminal(), "unnegotiated profile was not terminal");
    }
    {
        ZstdTuDialogue dialogue(profile_bit(ProfileId::ZSTD_TU), limits());
        dialogue.begin(envelope.begin);
        require_throws<std::invalid_argument>([&] { dialogue.begin(envelope.begin); },
                                              "same-session duplicate begin passed");
        require(dialogue.terminal(), "duplicate begin was not terminal");
    }
    {
        ZstdTuDialogue dialogue(profile_bit(ProfileId::ZSTD_TU), limits());
        dialogue.begin(envelope.begin);
        require_throws<std::invalid_argument>(
            [&] { dialogue.append_body(BodyMessage{}); },
            "empty BODY continuation was accepted");
        require(dialogue.terminal(), "empty continuation was not terminal");
    }
    {
        ZstdTuDialogue dialogue(profile_bit(ProfileId::ZSTD_TU), limits());
        dialogue.begin(envelope.begin);
        std::vector<uint8_t> overrun = envelope.body;
        overrun.push_back(0);
        require_throws<std::invalid_argument>(
            [&] { dialogue.append_body(BodyMessage{overrun}); },
            "BODY overrun was accepted");
        require(dialogue.terminal(), "BODY overrun was not terminal");
    }
    {
        ZstdTuDialogue dialogue(profile_bit(ProfileId::ZSTD_TU), limits());
        dialogue.begin(envelope.begin);
        dialogue.append_body(BodyMessage{envelope.body});
        require_throws<std::invalid_argument>(
            [&] { dialogue.append_body(BodyMessage{}); },
            "extra empty BODY after closure was accepted");
        require(dialogue.terminal(), "post-closure BODY was not terminal");
    }
    {
        ZstdTuDialogue dialogue(profile_bit(ProfileId::ZSTD_TU), limits());
        dialogue.begin(envelope.begin);
        require_throws<std::invalid_argument>(
            [&] { dialogue.append_dict(DictMessage{}); },
            "DICT after canonical empty closure was accepted");
        require(dialogue.terminal(), "unexpected DICT was not terminal");
    }
    {
        ZstdTuDialogue dialogue(profile_bit(ProfileId::ZSTD_TU), limits());
        dialogue.begin(envelope.begin);
        dialogue.disconnect();
        require(dialogue.terminal() && dialogue.pending_body_bytes() == 0 &&
                    !dialogue.active_begin(),
                "disconnect retained an incomplete overlay");
    }
}

}  // namespace

int main() {
    test_round_trip_and_persistent_dialogue();
    test_single_frame_exactness();
    test_digest_and_shape_gates();
    test_caps_and_terminal_dialogue_errors();
    std::cout << "p50_zstd_test: all exact ZSTD_TU dialogue gates passed\n";
    return 0;
}
