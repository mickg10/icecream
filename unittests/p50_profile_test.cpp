#include "cache/p50_profile.h"
#include "cache/p50_zstd.h"

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
    require_throws<std::invalid_argument>(
        [] {
            (void)make_profile_dialogue(
                ProfileId::GRZ,
                ProfileDialogueConfig{.negotiated_profiles = profile_bit(ProfileId::GRZ),
                                      .max_encoded_body_bytes = limits().max_encoded_body_bytes,
                                      .max_raw_bytes = limits().max_raw_bytes,
                                      .max_window_log = limits().max_window_log});
        },
        "factory admitted unsupported profile");
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
    std::cout << "p50profile_test: type-erased profile vtable gates passed\n";
    return 0;
}
