#include "cache/p50_profile.h"

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
                              .zstd = limits()});
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
    moved.commit_visible();
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
                              .zstd = limits()});
    dialogue.begin(envelope.begin);
    dialogue.disconnect();
    require(dialogue.terminal() && dialogue.state() == ProfileDialogueState::Terminal,
            "disconnect did not forward terminal state");
    require_throws<std::invalid_argument>(
        [&] { dialogue.append_body(BodyMessage{envelope.body}); },
        "terminal type-erased dialogue accepted BODY");
}

void test_factory_rejects_unsupported_or_unnegotiated() {
    require_throws<std::invalid_argument>(
        [] {
            (void)make_profile_dialogue(
                ProfileId::GRZ,
                ProfileDialogueConfig{.negotiated_profiles = profile_bit(ProfileId::GRZ),
                                      .zstd = limits()});
        },
        "factory admitted unsupported profile");
    require_throws<std::invalid_argument>(
        [] {
            (void)make_profile_dialogue(
                ProfileId::ZSTD_TU,
                ProfileDialogueConfig{.negotiated_profiles = 0, .zstd = limits()});
        },
        "factory admitted unnegotiated ZSTD_TU");
}

} // namespace

int main() {
    test_zstd_round_trip_and_move_lifetime();
    test_component_and_disconnect_forwarding();
    test_factory_rejects_unsupported_or_unnegotiated();
    std::cout << "p50profile_test: type-erased profile vtable gates passed\n";
    return 0;
}
