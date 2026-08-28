#include "cache/p50_profile.h"
#include "cache/p50_zstd.h"
#include "cache/p50_grz.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

using namespace icecc::p50;

[[noreturn]] void fail(std::string_view text) {
    std::cerr << "p50profile_test: " << text << '\n';
    std::exit(1);
}

void require(bool value, std::string_view text) {
    if (!value)
        fail(text);
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

ZstdTuLimits limits() { return {1U << 20, 1U << 20}; }

TxCommit route_commit(const TxBegin& begin) {
    return TxCommit{begin.history_nonce, begin.rel_seq, begin.tu_seq,
                    begin.transaction_digest, begin.raw_digest,
                    compute_post_state_digest(begin.pre_state_digest,
                                              begin.history_nonce, begin.rel_seq,
                                              begin.tu_seq,
                                              begin.transaction_digest)};
}

ProfileDialogue route_dialogue() {
    return ProfileDialogue::create(
        ProfileId::Z3_LONG,
        ProfileDialogueConfig{.negotiated_profiles = profile_bit(ProfileId::Z3_LONG),
                              .max_encoded_body_bytes = limits().max_encoded_body_bytes,
                              .max_raw_bytes = limits().max_raw_bytes,
                              .max_window_log = limits().max_window_log});
}

void test_zstd_route_multi_tu_replay_and_terminal_rules() {
    const std::vector<uint8_t> first{'r', 'o', 'u', 't', 'e', '-', '1'};
    const std::vector<uint8_t> second{'r', 'o', 'u', 't', 'e', '-', '2'};
    ZstdRouteCodec codec;
    std::vector<uint8_t> predecessor;
    const auto one = codec.encode(HistoryNonce{33}, RelSeq{0}, TuSeq{1},
                                  Digest128{}, predecessor, first, limits());
    predecessor = first;
    const auto two = codec.encode(HistoryNonce{33}, RelSeq{1}, TuSeq{2},
                                  compute_post_state_digest(one.begin.pre_state_digest,
                                                            one.begin.history_nonce,
                                                            one.begin.rel_seq,
                                                            one.begin.tu_seq,
                                                            one.begin.transaction_digest),
                                  predecessor, second, limits());
    const auto one_replay = codec.encode(HistoryNonce{33}, RelSeq{0}, TuSeq{1},
                                         Digest128{}, first, limits());
    const auto two_replay = codec.encode(HistoryNonce{33}, RelSeq{1}, TuSeq{2},
                                         compute_post_state_digest(one.begin.pre_state_digest,
                                                                   one.begin.history_nonce,
                                                                   one.begin.rel_seq,
                                                                   one.begin.tu_seq,
                                                                   one.begin.transaction_digest),
                                         predecessor, second, limits());
    require(one_replay.body == one.body && two_replay.body == two.body,
            "route replay did not retain byte-exact stream prefixes");
    require(codec.decode(std::span<const uint8_t>{}, one.begin, one.body, limits()) == first,
            "route first TU did not decode");
    require(codec.decode(predecessor, two.begin, two.body, limits()) == second,
            "route predecessor replay did not decode");

    ProfileDialogue dialogue = route_dialogue();
    dialogue.begin(one.begin);
    dialogue.append_body(BodyMessage{one.body});
    require(dialogue.materialize() == first, "route materialization changed bytes");
    require(dialogue.commit_state() == ProfileCommitState::Tentative,
            "route materialization was not tentative");
    dialogue.commit_visible(route_commit(one.begin));
    dialogue.begin(two.begin);
    dialogue.append_body(BodyMessage{two.body});
    require(dialogue.materialize() == second, "route second TU changed bytes");
    TxCommit mismatch = route_commit(two.begin);
    mismatch.raw_digest.bytes[0] ^= 0xff;
    require_throws<std::invalid_argument>(
        [&] { dialogue.commit_visible(mismatch); },
        "route accepted a mismatched terminal commit");
    dialogue.discard_tentative();
    require(dialogue.state() == ProfileDialogueState::Idle,
            "route cancel did not reset tentative state");
    dialogue.begin(two.begin);
    dialogue.append_body(BodyMessage{two.body});
    require(dialogue.materialize() == second,
            "route exact retained bytes did not replay after cancel");
    dialogue.commit_visible(route_commit(two.begin));
    dialogue.disconnect();
    dialogue.reset();
    require_throws<std::logic_error>([&] { dialogue.reset(); },
                                     "route reset unexpectedly accepted twice");
}

void test_zstd_route_reset_and_fallback() {
    require_throws<std::exception>(
        [] {
            ZstdRouteCodec codec;
            (void)codec.encode(HistoryNonce{1}, RelSeq{0}, TuSeq{1}, Digest128{},
                                std::span<const uint8_t>{},
                                ZstdTuLimits{1, 1, 27});
        },
        "route admitted a body cap too small for its stream");
    require_throws<std::invalid_argument>(
        [] {
            (void)make_profile_dialogue(
                ProfileId::Z3_SHARED_LONG,
                ProfileDialogueConfig{.negotiated_profiles = profile_bit(ProfileId::Z3_SHARED_LONG),
                                      .max_encoded_body_bytes = limits().max_encoded_body_bytes,
                                      .max_raw_bytes = limits().max_raw_bytes});
        },
        "unsupported shared-long profile did not fall back closed");
}

void append_route_suffix(std::vector<uint8_t>& history,
                         std::span<const uint8_t> input, size_t limit) {
    if (input.size() >= limit) {
        history.assign(input.end() - static_cast<std::ptrdiff_t>(limit), input.end());
        return;
    }
    const size_t excess = history.size() + input.size() > limit
                              ? history.size() + input.size() - limit
                              : 0;
    if (excess != 0) history.erase(history.begin(), history.begin() + excess);
    history.insert(history.end(), input.begin(), input.end());
}

void test_zstd_route_bounded_rolling_history() {
    ZstdTuLimits bounded{2U << 20, 2U << 20, 12, 4U << 10};
    ZstdRouteCodec codec;
    std::vector<uint8_t> history;
    Digest128 state{};
    const HistoryNonce nonce{77};

    for (uint64_t index = 0; index != 129; ++index) {
        std::vector<uint8_t> input(1U << 20, static_cast<uint8_t>('A' + index % 17));
        const auto envelope = codec.encode(nonce, RelSeq{index}, TuSeq{index}, state,
                                           history, input, bounded);
        require(codec.decode(history, envelope.begin, envelope.body, bounded) == input,
                "bounded route changed bytes after cumulative 128 MiB");
        append_route_suffix(history, input, bounded.max_history_bytes);
        state = compute_post_state_digest(state, nonce, envelope.begin.rel_seq,
                                          envelope.begin.tu_seq,
                                          envelope.begin.transaction_digest);
        require(history.size() <= bounded.max_history_bytes,
                "route prefix exceeded its configured byte window");
    }

    bounded = ZstdTuLimits{1U << 10, 1U << 20, 12, 4U << 10};
    history.clear();
    state = Digest128{};
    ZstdRouteDialogue dialogue(profile_bit(ProfileId::Z3_LONG), bounded);
    for (uint64_t index = 0; index != 10000; ++index) {
        std::vector<uint8_t> input(31, static_cast<uint8_t>(index));
        const auto envelope = codec.encode(nonce, RelSeq{index}, TuSeq{index}, state,
                                           history, input, bounded);
        dialogue.begin(envelope.begin);
        dialogue.append_body(BodyMessage{envelope.body});
        require(dialogue.materialize() == input, "rolling dialogue changed small TU bytes");
        if (index == 0) {
            dialogue.discard_tentative();
            dialogue.begin(envelope.begin);
            dialogue.append_body(BodyMessage{envelope.body});
            require(dialogue.materialize() == input,
                    "rolling dialogue retry did not decode exact bytes");
        }
        dialogue.commit_visible(route_commit(envelope.begin));
        append_route_suffix(history, input, bounded.max_history_bytes);
        state = compute_post_state_digest(state, nonce, envelope.begin.rel_seq,
                                          envelope.begin.tu_seq,
                                          envelope.begin.transaction_digest);
        require(dialogue.retained_history_bytes() <= bounded.max_history_bytes &&
                    dialogue.retained_history_entries() <= 1,
                "rolling dialogue retained unbounded route state");
    }
    dialogue.disconnect();
    require(dialogue.retained_history_bytes() == 0,
            "route disconnect retained committed prefix");
    dialogue.reset();
    require(dialogue.state() == ZstdRouteDialogue::State::Idle,
            "route reset did not return to idle");
}

void test_zstd_round_trip_and_move_lifetime() {
    const std::vector<uint8_t> input{'p', 'r', 'o', 'f', 'i', 'l', 'e'};
    const ZstdTuEnvelope envelope = encode_zstd_tu(
        HistoryNonce{7}, RelSeq{0}, TuSeq{11}, Digest128{}, input);

    ProfileDialogue dialogue = ProfileDialogue::create(
        ProfileId::ZSTD_TU,
ProfileDialogueConfig{.negotiated_profiles = profile_bit(ProfileId::ZSTD_TU),
                              .max_encoded_body_bytes = limits().max_encoded_body_bytes,
                              .max_raw_bytes = limits().max_raw_bytes,
                              .max_window_log = limits().max_window_log});
    require(dialogue.profile() == ProfileId::ZSTD_TU,
            "factory returned the wrong profile");
    dialogue.begin(envelope.begin);
    require(dialogue.active_begin() != nullptr,
            "vtable did not expose the active transaction identity");
    dialogue.append_body(BodyMessage{envelope.body});
    require(dialogue.state() == ProfileDialogueState::BodyClosed,
            "vtable changed BODY completion state");
    ProfileDialogue moved = std::move(dialogue);
    require(!dialogue && moved,
            "move did not transfer type-erased dialogue ownership");
    require(moved.materialize() == input,
            "type-erased ZSTD_TU materialization changed exact bytes");
    moved.commit_visible(TxCommit{envelope.begin.history_nonce,
                                  envelope.begin.rel_seq,
                                  envelope.begin.tu_seq,
                                  envelope.begin.transaction_digest,
                                  envelope.begin.raw_digest,
                                  compute_post_state_digest(
                                      envelope.begin.pre_state_digest,
                                      envelope.begin.history_nonce,
                                      envelope.begin.rel_seq,
                                      envelope.begin.tu_seq,
                                      envelope.begin.transaction_digest)});
    require(moved.state() == ProfileDialogueState::Idle &&
                moved.pending_body_bytes() == 0 && moved.active_begin() == nullptr,
            "visible commit did not preserve dialogue cleanup");
}

void test_component_and_disconnect_forwarding() {
    const std::vector<uint8_t> input{'d', 'i', 's', 'c'};
    const ZstdTuEnvelope envelope = encode_zstd_tu(
        HistoryNonce{8}, RelSeq{0}, TuSeq{12}, Digest128{}, input);
    ProfileDialogue dialogue = make_profile_dialogue(
        ProfileId::ZSTD_TU,
ProfileDialogueConfig{.negotiated_profiles = profile_bit(ProfileId::ZSTD_TU),
                              .max_encoded_body_bytes = limits().max_encoded_body_bytes,
                              .max_raw_bytes = limits().max_raw_bytes,
                              .max_window_log = limits().max_window_log});
    dialogue.begin(envelope.begin);
    dialogue.disconnect();
    require(dialogue.terminal() && dialogue.state() == ProfileDialogueState::Terminal,
            "disconnect did not forward terminal state");
    require_throws<std::invalid_argument>(
        [&] { dialogue.append_body(BodyMessage{envelope.body}); },
        "terminal type-erased dialogue accepted BODY");
    dialogue.reset();
    require(!dialogue.terminal() && dialogue.state() == ProfileDialogueState::Idle,
            "terminal dialogue did not reset to the committed state");
}

void test_exact_terminal_promotion_and_discard() {
    const std::vector<uint8_t> input{'t', 'e', 'n', 't'};
    const ZstdTuEnvelope envelope = encode_zstd_tu(
        HistoryNonce{9}, RelSeq{0}, TuSeq{13}, Digest128{}, input);
    ProfileDialogue dialogue = make_profile_dialogue(
        ProfileId::ZSTD_TU,
        ProfileDialogueConfig{.negotiated_profiles = profile_bit(ProfileId::ZSTD_TU),
                              .max_encoded_body_bytes = limits().max_encoded_body_bytes,
                              .max_raw_bytes = limits().max_raw_bytes,
                              .max_window_log = limits().max_window_log});
    dialogue.begin(envelope.begin);
    dialogue.append_body(BodyMessage{envelope.body});
    require(dialogue.materialize() == input &&
                dialogue.commit_state() == ProfileCommitState::Tentative,
            "materialization did not create tentative state");
    TxCommit bad{envelope.begin.history_nonce, envelope.begin.rel_seq,
                 envelope.begin.tu_seq, envelope.begin.transaction_digest,
                 envelope.begin.raw_digest, Digest128{}};
    bad.raw_digest.bytes[0] ^= 0xff;
    require_throws<std::invalid_argument>(
        [&] { dialogue.commit_visible(bad); },
        "profile promoted a mismatched terminal commit");
    require(dialogue.commit_state() == ProfileCommitState::Tentative,
            "mismatched terminal commit discarded tentative state");
    dialogue.discard_tentative();
    require(dialogue.state() == ProfileDialogueState::Idle &&
                dialogue.commit_state() == ProfileCommitState::Committed,
            "rejection did not discard tentative profile state");
    require(dialogue.window_limit_bytes() == (uint64_t{1} << limits().max_window_log),
            "profile window bound was not exposed through neutral hooks");
}

void test_interactive_hooks_are_reachable_and_fail_closed_for_zstd() {
    const std::vector<uint8_t> input{'x'};
    const ZstdTuEnvelope envelope = encode_zstd_tu(
        HistoryNonce{10}, RelSeq{0}, TuSeq{14}, Digest128{}, input);
    const auto config = ProfileDialogueConfig{
        .negotiated_profiles = profile_bit(ProfileId::ZSTD_TU),
        .max_encoded_body_bytes = limits().max_encoded_body_bytes,
        .max_raw_bytes = limits().max_raw_bytes,
        .max_window_log = limits().max_window_log};
    for (const auto inject : {0, 1, 2}) {
        ProfileDialogue dialogue = make_profile_dialogue(ProfileId::ZSTD_TU, config);
        dialogue.begin(envelope.begin);
        require_throws<std::invalid_argument>(
            [&] {
                if (inject == 0)
                    dialogue.append_dict(DictMessage{});
                else if (inject == 1)
                    dialogue.receive_need(NeedMessage{});
                else
                    dialogue.receive_fill(FillMessage{});
            },
            "interactive profile hook did not reach the selected adapter");
        require(dialogue.terminal(), "interactive rejection was not terminal");
    }
}

void test_factory_rejects_unsupported_or_unnegotiated() {
#if defined(ICECC_P50_WITH_LIBBSC)
    for (uint8_t corpus = 1; corpus <= 3; ++corpus) {
        std::vector<uint8_t> input(200000U * corpus);
        for (size_t i = 0; i < input.size(); ++i)
            input[i] = static_cast<uint8_t>((i * 37U + corpus * 11U) ^ (i >> 5));
        const auto envelope = encode_grz_residual(HistoryNonce{8}, RelSeq{0}, TuSeq{12},
                                                   Digest128{}, input, limits());
        require(envelope.begin.profile == ProfileId::GRZ &&
                    envelope.begin.body.encoding == kGrzResidualBodyEncoding,
                "GRZ_RESIDUAL did not use the frozen profile and body encoding");
        require(decode_grz_residual(envelope.begin, envelope.body, limits()) == input,
                "GRZ_RESIDUAL corpus replay changed exact bytes");
        auto corrupt = envelope.body;
        corrupt.back() ^= 1;
        require_throws<std::exception>(
            [&] { (void)decode_grz_residual(envelope.begin, corrupt, limits()); },
            "GRZ_RESIDUAL accepted a corrupt object");
    }
    const std::vector<uint8_t> input{'g', 'r', 'z', '-', 'r', 'e', 's', 'i', 'd', 'u', 'a', 'l'};
    const auto envelope = encode_grz_residual(HistoryNonce{8}, RelSeq{0}, TuSeq{12},
                                               Digest128{}, input, limits());
    const auto config = ProfileDialogueConfig{
        .negotiated_profiles = profile_bit(ProfileId::GRZ),
        .max_encoded_body_bytes = limits().max_encoded_body_bytes,
        .max_raw_bytes = limits().max_raw_bytes,
        .max_window_log = limits().max_window_log};
    ProfileDialogue dialogue = make_profile_dialogue(ProfileId::GRZ, config);
    dialogue.begin(envelope.begin);
    dialogue.append_body(BodyMessage{envelope.body});
    require(dialogue.materialize() == input,
            "GRZ_RESIDUAL dialogue did not materialize exact bytes");
    dialogue.commit_visible(route_commit(envelope.begin));
#else
    require_throws<std::invalid_argument>(
        [] {
            (void)make_profile_dialogue(
                ProfileId::Z3_SHARED_LONG,
                ProfileDialogueConfig{.negotiated_profiles = profile_bit(ProfileId::Z3_SHARED_LONG),
                                      .max_encoded_body_bytes = limits().max_encoded_body_bytes,
                                      .max_raw_bytes = limits().max_raw_bytes,
                                      .max_window_log = limits().max_window_log});
        },
        "factory admitted unsupported profile");
    require_throws<std::invalid_argument>(
        [] {
            (void)make_profile_dialogue(
                ProfileId::GRZ,
                ProfileDialogueConfig{.negotiated_profiles = profile_bit(ProfileId::GRZ),
                                      .max_encoded_body_bytes = limits().max_encoded_body_bytes,
                                      .max_raw_bytes = limits().max_raw_bytes,
                                      .max_window_log = limits().max_window_log});
        },
        "factory admitted unsupported GRZ profile");
#endif
    require_throws<std::invalid_argument>(
        [] {
            (void)make_profile_dialogue(
                ProfileId::ZSTD_TU,
                ProfileDialogueConfig{.negotiated_profiles = 0,
                                      .max_encoded_body_bytes = limits().max_encoded_body_bytes,
                                      .max_raw_bytes = limits().max_raw_bytes,
                                      .max_window_log = limits().max_window_log});
        },
        "factory admitted unnegotiated ZSTD_TU");
    require_throws<std::invalid_argument>(
        [] {
            (void)make_profile_dialogue(
                ProfileId::ZSTD_TU,
                ProfileDialogueConfig{.negotiated_profiles =
                                          profile_bit(ProfileId::ZSTD_TU) | (uint32_t{1} << 31),
                                      .max_encoded_body_bytes = 1,
                                      .max_raw_bytes = 1});
        },
        "factory admitted an undeclared negotiated profile bit");
}

} // namespace

int main() {
    test_zstd_round_trip_and_move_lifetime();
    test_component_and_disconnect_forwarding();
    test_exact_terminal_promotion_and_discard();
    test_interactive_hooks_are_reachable_and_fail_closed_for_zstd();
    test_factory_rejects_unsupported_or_unnegotiated();
    test_zstd_route_multi_tu_replay_and_terminal_rules();
    test_zstd_route_reset_and_fallback();
    test_zstd_route_bounded_rolling_history();
    std::cout << "p50profile_test: type-erased profile vtable gates passed\n";
    return 0;
}
