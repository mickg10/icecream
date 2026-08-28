#include "p50_profile.h"

#include "p50_zstd.h"

#include <stdexcept>
#include <utility>

namespace icecc::p50 {
namespace {

ProfileDialogueState map_state(ZstdTuDialogue::State state) noexcept {
    switch (state) {
    case ZstdTuDialogue::State::Idle:
        return ProfileDialogueState::Idle;
    case ZstdTuDialogue::State::ReceivingBody:
        return ProfileDialogueState::ReceivingBody;
    case ZstdTuDialogue::State::BodyClosed:
        return ProfileDialogueState::BodyClosed;
    case ZstdTuDialogue::State::Materialized:
        return ProfileDialogueState::Materialized;
    case ZstdTuDialogue::State::Terminal:
        return ProfileDialogueState::Terminal;
    }
    return ProfileDialogueState::Terminal;
}

void destroy_zstd(void* object) noexcept {
    delete static_cast<ZstdTuDialogue*>(object);
}

ProfileDialogueState map_route_state(ZstdRouteDialogue::State state) noexcept {
    switch (state) {
    case ZstdRouteDialogue::State::Idle:
        return ProfileDialogueState::Idle;
    case ZstdRouteDialogue::State::ReceivingBody:
        return ProfileDialogueState::ReceivingBody;
    case ZstdRouteDialogue::State::BodyClosed:
        return ProfileDialogueState::BodyClosed;
    case ZstdRouteDialogue::State::Materialized:
        return ProfileDialogueState::Materialized;
    case ZstdRouteDialogue::State::Terminal:
        return ProfileDialogueState::Terminal;
    }
    return ProfileDialogueState::Terminal;
}

void destroy_route(void* object) noexcept {
    delete static_cast<ZstdRouteDialogue*>(object);
}

void begin_route(void* object, const TxBegin& begin) {
    static_cast<ZstdRouteDialogue*>(object)->begin(begin);
}

void append_dict_route(void* object, const DictMessage& message) {
    static_cast<ZstdRouteDialogue*>(object)->append_dict(message);
}

void append_body_route(void* object, const BodyMessage& message) {
    static_cast<ZstdRouteDialogue*>(object)->append_body(message);
}

void receive_need_route(void* object, const NeedMessage& message) {
    static_cast<ZstdRouteDialogue*>(object)->receive_need(message);
}

void receive_fill_route(void* object, const FillMessage& message) {
    static_cast<ZstdRouteDialogue*>(object)->receive_fill(message);
}

std::vector<uint8_t> materialize_route(void* object) {
    return static_cast<ZstdRouteDialogue*>(object)->materialize();
}

void commit_visible_route(void* object, const TxCommit& commit) {
    static_cast<ZstdRouteDialogue*>(object)->commit_visible(commit);
}

void discard_tentative_route(void* object) noexcept {
    static_cast<ZstdRouteDialogue*>(object)->discard_tentative();
}

void disconnect_route(void* object) noexcept {
    static_cast<ZstdRouteDialogue*>(object)->disconnect();
}

void reset_route(void* object) {
    static_cast<ZstdRouteDialogue*>(object)->reset();
}

ProfileDialogueState state_route(const void* object) noexcept {
    return map_route_state(static_cast<const ZstdRouteDialogue*>(object)->state());
}

bool terminal_route(const void* object) noexcept {
    return static_cast<const ZstdRouteDialogue*>(object)->terminal();
}

ProfileCommitState commit_state_route(const void* object) noexcept {
    const auto state = static_cast<const ZstdRouteDialogue*>(object)->state();
    return state == ZstdRouteDialogue::State::ReceivingBody ||
                   state == ZstdRouteDialogue::State::BodyClosed ||
                   state == ZstdRouteDialogue::State::Materialized
               ? ProfileCommitState::Tentative
               : ProfileCommitState::Committed;
}

size_t pending_body_bytes_route(const void* object) noexcept {
    return static_cast<const ZstdRouteDialogue*>(object)->pending_body_bytes();
}

uint64_t window_limit_bytes_route(const void* object) noexcept {
    return static_cast<const ZstdRouteDialogue*>(object)->window_limit_bytes();
}

const TxBegin* active_begin_route(const void* object) noexcept {
    const auto& active = static_cast<const ZstdRouteDialogue*>(object)->active_begin();
    return active ? &*active : nullptr;
}

void begin_zstd(void* object, const TxBegin& begin) {
    static_cast<ZstdTuDialogue*>(object)->begin(begin);
}

void append_dict_zstd(void* object, const DictMessage& message) {
    static_cast<ZstdTuDialogue*>(object)->append_dict(message);
}

void append_body_zstd(void* object, const BodyMessage& message) {
    static_cast<ZstdTuDialogue*>(object)->append_body(message);
}

void receive_need_zstd(void* object, const NeedMessage& message) {
    static_cast<ZstdTuDialogue*>(object)->receive_need(message);
}

void receive_fill_zstd(void* object, const FillMessage& message) {
    static_cast<ZstdTuDialogue*>(object)->receive_fill(message);
}

std::vector<uint8_t> materialize_zstd(void* object) {
    return static_cast<ZstdTuDialogue*>(object)->materialize();
}

void commit_visible_zstd(void* object, const TxCommit& commit) {
    static_cast<ZstdTuDialogue*>(object)->commit_visible(commit);
}

void discard_tentative_zstd(void* object) noexcept {
    static_cast<ZstdTuDialogue*>(object)->discard_tentative();
}

void disconnect_zstd(void* object) noexcept {
    static_cast<ZstdTuDialogue*>(object)->disconnect();
}

void reset_zstd(void* object) {
    static_cast<ZstdTuDialogue*>(object)->reset();
}

ProfileDialogueState state_zstd(const void* object) noexcept {
    return map_state(static_cast<const ZstdTuDialogue*>(object)->state());
}

bool terminal_zstd(const void* object) noexcept {
    return static_cast<const ZstdTuDialogue*>(object)->terminal();
}

ProfileCommitState commit_state_zstd(const void* object) noexcept {
    const ZstdTuDialogue::State state =
        static_cast<const ZstdTuDialogue*>(object)->state();
    return state == ZstdTuDialogue::State::ReceivingBody ||
                   state == ZstdTuDialogue::State::BodyClosed ||
                   state == ZstdTuDialogue::State::Materialized
               ? ProfileCommitState::Tentative
               : ProfileCommitState::Committed;
}

size_t pending_body_bytes_zstd(const void* object) noexcept {
    return static_cast<const ZstdTuDialogue*>(object)->pending_body_bytes();
}

uint64_t window_limit_bytes_zstd(const void* object) noexcept {
    return static_cast<const ZstdTuDialogue*>(object)->window_limit_bytes();
}

const TxBegin* active_begin_zstd(const void* object) noexcept {
    const auto& active = static_cast<const ZstdTuDialogue*>(object)->active_begin();
    return active ? &*active : nullptr;
}

const ProfileDialogueVTable kZstdTuVTable{
    .profile = ProfileId::ZSTD_TU,
    .destroy = destroy_zstd,
    .begin = begin_zstd,
    .append_dict = append_dict_zstd,
    .append_body = append_body_zstd,
    .receive_need = receive_need_zstd,
    .receive_fill = receive_fill_zstd,
    .materialize = materialize_zstd,
    .commit_visible = commit_visible_zstd,
    .discard_tentative = discard_tentative_zstd,
    .disconnect = disconnect_zstd,
    .reset = reset_zstd,
    .state = state_zstd,
    .terminal = terminal_zstd,
    .commit_state = commit_state_zstd,
    .pending_body_bytes = pending_body_bytes_zstd,
    .window_limit_bytes = window_limit_bytes_zstd,
    .active_begin = active_begin_zstd,
};

const ProfileDialogueVTable kZstdRouteVTable{
    .profile = ProfileId::Z3_LONG,
    .destroy = destroy_route,
    .begin = begin_route,
    .append_dict = append_dict_route,
    .append_body = append_body_route,
    .receive_need = receive_need_route,
    .receive_fill = receive_fill_route,
    .materialize = materialize_route,
    .commit_visible = commit_visible_route,
    .discard_tentative = discard_tentative_route,
    .disconnect = disconnect_route,
    .reset = reset_route,
    .state = state_route,
    .terminal = terminal_route,
    .commit_state = commit_state_route,
    .pending_body_bytes = pending_body_bytes_route,
    .window_limit_bytes = window_limit_bytes_route,
    .active_begin = active_begin_route,
};

} // namespace

const ProfileDialogueVTable& ProfileDialogue::empty_table() noexcept {
    static const ProfileDialogueVTable empty{
        .profile = ProfileId::P29,
    };
    return empty;
}

void ProfileDialogue::destroy() noexcept {
    if (object_ != nullptr)
        table_->destroy(object_);
    table_ = &empty_table();
    object_ = nullptr;
}

ProfileDialogue::~ProfileDialogue() { destroy(); }

ProfileDialogue::ProfileDialogue(ProfileDialogue&& other) noexcept
    : table_(other.table_), object_(other.object_) {
    other.table_ = &empty_table();
    other.object_ = nullptr;
}

ProfileDialogue& ProfileDialogue::operator=(ProfileDialogue&& other) noexcept {
    if (this != &other) {
        destroy();
        table_ = other.table_;
        object_ = other.object_;
        other.table_ = &empty_table();
        other.object_ = nullptr;
    }
    return *this;
}

ProfileDialogue ProfileDialogue::create(ProfileId profile, ProfileDialogueConfig config) {
    return make_profile_dialogue(profile, config);
}

ProfileDialogue make_profile_dialogue(ProfileId profile, ProfileDialogueConfig config) {
    const uint32_t selected_bit = profile_bit(profile);
    if (selected_bit == 0 ||
        (config.negotiated_profiles & ~kDeclaredProfileMask) != 0 ||
        (config.negotiated_profiles & selected_bit) != selected_bit)
        throw std::invalid_argument("transaction profile was not negotiated");
    ZstdTuLimits zstd_limits{config.max_encoded_body_bytes,
                             config.max_raw_bytes,
                             config.max_window_log};
    switch (profile) {
    case ProfileId::ZSTD_TU:
        return ProfileDialogue(&kZstdTuVTable,
                               new ZstdTuDialogue(config.negotiated_profiles,
                                                  zstd_limits));
    case ProfileId::Z3_LONG:
        return ProfileDialogue(&kZstdRouteVTable,
                               new ZstdRouteDialogue(config.negotiated_profiles,
                                                     zstd_limits));
    default:
        throw std::invalid_argument("transaction profile has no dialogue implementation");
    }
}

} // namespace icecc::p50
