#include "cache/p50_profile.h"
#include "cache/p50_zstd.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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

constexpr uint64_t kMiB = uint64_t{1} << 20;

ZstdTuLimits limits() {
    return {kMiB, kMiB, 20, 4096};
}

ProfileDialogueConfig config_for(ProfileId profile) {
    return {
        .negotiated_profiles = profile_bit(profile),
        .c_store_guid = CStoreGuid::from_u64(17),
        .system_source_reuse = true,
        .max_encoded_body_bytes = limits().max_encoded_body_bytes,
        .max_raw_bytes = limits().max_raw_bytes,
        .max_window_log = limits().max_window_log,
        .max_history_bytes = limits().max_history_bytes,
    };
}

TxCommit commit_for(const TxBegin& begin) {
    return {
        begin.history_nonce,
        begin.rel_seq,
        begin.tu_seq,
        begin.transaction_digest,
        begin.raw_digest,
        compute_post_state_digest(begin.pre_state_digest, begin.history_nonce,
                                  begin.rel_seq, begin.tu_seq,
                                  begin.transaction_digest),
    };
}

void retain_suffix(std::vector<uint8_t>& history,
                   std::span<const uint8_t> input, size_t limit) {
    if (input.size() >= limit) {
        history.assign(input.end() - static_cast<std::ptrdiff_t>(limit),
                       input.end());
        return;
    }
    const size_t total = history.size() + input.size();
    if (total > limit)
        history.erase(history.begin(), history.begin() + (total - limit));
    history.insert(history.end(), input.begin(), input.end());
}

void test_exact_revision_one_factory() {
    static_assert(static_cast<uint16_t>(ProfileId::P29V1) == 1);
    static_assert(static_cast<uint16_t>(ProfileId::ZSTD_TU) == 2);
    static_assert(static_cast<uint16_t>(ProfileId::ZSTD_ROUTE) == 3);
    static_assert(kKnownProfileMask == UINT32_C(0x00000007));

    for (const ProfileId profile : {ProfileId::P29V1, ProfileId::ZSTD_TU,
                                    ProfileId::ZSTD_ROUTE}) {
        ProfileDialogue dialogue = ProfileDialogue::create(profile,
                                                            config_for(profile));
        require(dialogue && dialogue.profile() == profile,
                "factory did not preserve a retained profile ID");
    }

    require_throws<std::invalid_argument>(
        [] {
            (void)ProfileDialogue::create(static_cast<ProfileId>(4),
                                           config_for(ProfileId::P29V1));
        },
        "factory admitted a removed revision-one profile ID");
    require_throws<std::invalid_argument>(
        [] {
            auto config = config_for(ProfileId::ZSTD_TU);
            config.negotiated_profiles = 0;
            (void)ProfileDialogue::create(ProfileId::ZSTD_TU, config);
        },
        "factory admitted an unnegotiated profile");
    require_throws<std::invalid_argument>(
        [] {
            auto config = config_for(ProfileId::ZSTD_TU);
            config.negotiated_profiles |= UINT32_C(0x00000008);
            (void)ProfileDialogue::create(ProfileId::ZSTD_TU, config);
        },
        "factory admitted a removed profile bit in the negotiated mask");
}

void test_zstd_tu_lifecycle_and_move() {
    const std::vector<uint8_t> input{'p', 'r', 'o', 'f', 'i', 'l', 'e'};
    const ZstdTuEnvelope envelope = encode_zstd_tu(
        HistoryNonce{7}, RelSeq{0}, TuSeq{11}, Digest128{}, input, 1, limits());

    ProfileDialogue dialogue = ProfileDialogue::create(
        ProfileId::ZSTD_TU, config_for(ProfileId::ZSTD_TU));
    dialogue.begin(envelope.begin);
    require(dialogue.active_begin() != nullptr &&
                dialogue.state() == ProfileDialogueState::ReceivingBody,
            "ZSTD_TU begin identity was not exposed");

    const size_t split = envelope.body.size() / 2;
    dialogue.append_body(BodyMessage{{envelope.body.begin(),
                                      envelope.body.begin() + split}});
    dialogue.append_body(BodyMessage{{envelope.body.begin() + split,
                                      envelope.body.end()}});
    require(dialogue.state() == ProfileDialogueState::BodyClosed,
            "ZSTD_TU BODY did not close at the declared length");

    ProfileDialogue moved = std::move(dialogue);
    require(!dialogue && moved, "profile ownership did not move exactly once");
    require(moved.materialize() == input &&
                moved.commit_state() == ProfileCommitState::Tentative,
            "ZSTD_TU materialization was not exact and tentative");
    moved.commit_visible(commit_for(envelope.begin));
    require(moved.state() == ProfileDialogueState::Idle &&
                moved.active_begin() == nullptr && moved.pending_body_bytes() == 0,
            "ZSTD_TU commit did not clear per-transaction state");
}

void test_zstd_fail_closed_and_reset() {
    const std::vector<uint8_t> input{'x'};
    const ZstdTuEnvelope envelope = encode_zstd_tu(
        HistoryNonce{8}, RelSeq{0}, TuSeq{12}, Digest128{}, input, 1, limits());
    ProfileDialogue dialogue = make_profile_dialogue(
        ProfileId::ZSTD_TU, config_for(ProfileId::ZSTD_TU));
    dialogue.begin(envelope.begin);
    require_throws<std::invalid_argument>(
        [&] { dialogue.receive_need(NeedMessage{}); },
        "ZSTD_TU accepted an interactive NEED");
    require(dialogue.terminal(), "invalid ZSTD_TU input was not terminal");
    dialogue.reset();
    require(!dialogue.terminal() &&
                dialogue.state() == ProfileDialogueState::Idle,
            "ZSTD_TU terminal dialogue did not reset");
}

void test_zstd_route_transactional_history() {
    ZstdRouteCodec codec;
    std::vector<uint8_t> history;
    Digest128 state{};
    const HistoryNonce nonce{33};
    ProfileDialogue dialogue = ProfileDialogue::create(
        ProfileId::ZSTD_ROUTE, config_for(ProfileId::ZSTD_ROUTE));

    for (uint64_t index = 0; index != 96; ++index) {
        std::vector<uint8_t> input(257, static_cast<uint8_t>('A' + index % 13));
        const ZstdRouteEnvelope envelope = codec.encode(
            nonce, RelSeq{index}, TuSeq{index + 1}, state, history, input,
            limits());
        require(codec.decode(history, envelope.begin, envelope.body, limits()) ==
                    input,
                "ZSTD_ROUTE codec changed exact bytes");

        dialogue.begin(envelope.begin);
        dialogue.append_body(BodyMessage{envelope.body});
        require(dialogue.materialize() == input,
                "ZSTD_ROUTE dialogue changed exact bytes");
        if (index == 1) {
            dialogue.discard_tentative();
            dialogue.begin(envelope.begin);
            dialogue.append_body(BodyMessage{envelope.body});
            require(dialogue.materialize() == input,
                    "ZSTD_ROUTE retry changed the candidate");
        }
        dialogue.commit_visible(commit_for(envelope.begin));
        retain_suffix(history, input, limits().max_history_bytes);
        state = commit_for(envelope.begin).post_state_digest;
        require(history.size() <= limits().max_history_bytes,
                "ZSTD_ROUTE reference history exceeded its bound");
    }

    dialogue.disconnect();
    require(dialogue.terminal(), "ZSTD_ROUTE disconnect was not terminal");
    dialogue.reset();
    require(dialogue.state() == ProfileDialogueState::Idle,
            "ZSTD_ROUTE did not reset to idle");
}

void test_p29v1_is_explicit_and_fail_closed() {
    ProfileDialogue dialogue = ProfileDialogue::create(
        ProfileId::P29V1, config_for(ProfileId::P29V1));
    require(dialogue.profile() == ProfileId::P29V1,
            "P29V1 factory silently selected another profile");

    TxBegin malformed;
    malformed.history_nonce = HistoryNonce{1};
    malformed.profile = ProfileId::P29V1;
    malformed.body.encoding = static_cast<uint16_t>(ProfileId::ZSTD_TU);
    require_throws<std::logic_error>(
        [&] { dialogue.begin(malformed); },
        "P29V1 accepted a BODY encoding from another profile");
    require(dialogue.terminal(), "invalid P29V1 begin was not terminal");
    dialogue.reset();

    require_throws<std::invalid_argument>(
        [] {
            (void)ProfileDialogue::create(ProfileId::P29V1,
                                           config_for(ProfileId::ZSTD_TU));
        },
        "P29V1 factory accepted an unnegotiated profile");
}

#ifdef ICECC_P50_PROFILE_TEST_HOOKS
struct AdversarialProfile {
    TxBegin begin{};
    std::vector<uint8_t> output;
    ProfileDialogueState after_materialize =
        ProfileDialogueState::Materialized;
    ProfileDialogueState state = ProfileDialogueState::BodyClosed;
};

void destroy_adversarial(void* object) noexcept {
    delete static_cast<AdversarialProfile*>(object);
}

std::vector<uint8_t> materialize_adversarial(void* object) {
    auto* profile = static_cast<AdversarialProfile*>(object);
    profile->state = profile->after_materialize;
    return profile->output;
}

ProfileDialogueState state_adversarial(const void* object) noexcept {
    return static_cast<const AdversarialProfile*>(object)->state;
}

const TxBegin* active_begin_adversarial(const void* object) noexcept {
    return &static_cast<const AdversarialProfile*>(object)->begin;
}

const ProfileDialogueVTable kAdversarialVTable{
    .profile = ProfileId::ZSTD_TU,
    .destroy = destroy_adversarial,
    .materialize = materialize_adversarial,
    .state = state_adversarial,
    .active_begin = active_begin_adversarial,
};

ProfileDialogue adversarial_dialogue(
    TxBegin begin, std::vector<uint8_t> output,
    ProfileDialogueState after_materialize) {
    auto* object = new AdversarialProfile{
        std::move(begin), std::move(output), after_materialize,
        ProfileDialogueState::BodyClosed};
    return ProfileDialogue::create_for_test(&kAdversarialVTable, object);
}

void test_verified_materialization_rejects_bad_vtable_results() {
    const std::vector<uint8_t> exact{'p', 'r', 'o', 'o', 'f'};
    const ZstdTuEnvelope envelope = encode_zstd_tu(
        HistoryNonce{91}, RelSeq{0}, TuSeq{92}, Digest128{}, exact, 1,
        limits());

    {
        std::vector<uint8_t> short_output = exact;
        short_output.pop_back();
        ProfileDialogue dialogue = adversarial_dialogue(
            envelope.begin, std::move(short_output),
            ProfileDialogueState::Materialized);
        require_throws<std::logic_error>(
            [&] { (void)dialogue.materialize_verified(); },
            "verified proof was minted for a wrong-length materialization");
    }
    {
        ProfileDialogue dialogue = adversarial_dialogue(
            envelope.begin, exact, ProfileDialogueState::BodyClosed);
        require_throws<std::logic_error>(
            [&] { (void)dialogue.materialize_verified(); },
            "verified proof was minted before the profile reached Materialized");
    }
}
#endif

} // namespace

int main() {
    test_exact_revision_one_factory();
    test_zstd_tu_lifecycle_and_move();
    test_zstd_fail_closed_and_reset();
    test_zstd_route_transactional_history();
    test_p29v1_is_explicit_and_fail_closed();
#ifdef ICECC_P50_PROFILE_TEST_HOOKS
    test_verified_materialization_rejects_bad_vtable_results();
#endif
    std::cout << "p50profile_test: revision-one profile gates passed\n";
    return 0;
}
