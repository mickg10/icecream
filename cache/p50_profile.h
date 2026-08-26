#pragma once

#include "p50_zstd.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace icecc::p50 {

// The transaction engine owns this stable, profile-neutral state vocabulary.
// Profile implementations may use a different internal state machine, but
// must preserve these observations at the transaction boundary.
enum class ProfileDialogueState : uint8_t {
    Idle,
    ReceivingBody,
    BodyClosed,
    Materialized,
    Terminal,
};

struct ProfileDialogueConfig {
    uint32_t negotiated_profiles = 0;
    ZstdTuLimits zstd{};
};

// A profile implementation is deliberately C-style at this boundary.  The
// transaction engine never names, allocates, or destroys a concrete profile
// dialogue; adding a profile adds one vtable/factory implementation instead.
struct ProfileDialogueVTable {
    ProfileId profile = ProfileId::ZSTD_TU;
    void (*destroy)(void*) noexcept = nullptr;
    void (*begin)(void*, const TxBegin&) = nullptr;
    void (*append_dict)(void*, const DictMessage&) = nullptr;
    void (*append_body)(void*, const BodyMessage&) = nullptr;
    void (*receive_need)(void*, const NeedMessage&) = nullptr;
    void (*receive_fill)(void*, const FillMessage&) = nullptr;
    std::vector<uint8_t> (*materialize)(void*) = nullptr;
    void (*commit_visible)(void*) = nullptr;
    void (*disconnect)(void*) noexcept = nullptr;
    ProfileDialogueState (*state)(const void*) noexcept = nullptr;
    bool (*terminal)(const void*) noexcept = nullptr;
    size_t (*pending_body_bytes)(const void*) noexcept = nullptr;
    const TxBegin* (*active_begin)(const void*) noexcept = nullptr;
};

class ProfileDialogue {
public:
    ProfileDialogue() = default;
    ~ProfileDialogue();

    ProfileDialogue(const ProfileDialogue&) = delete;
    ProfileDialogue& operator=(const ProfileDialogue&) = delete;
    ProfileDialogue(ProfileDialogue&& other) noexcept;
    ProfileDialogue& operator=(ProfileDialogue&& other) noexcept;

    [[nodiscard]] static ProfileDialogue create(ProfileId profile,
                                                 ProfileDialogueConfig config);

    [[nodiscard]] explicit operator bool() const noexcept { return object_ != nullptr; }
    [[nodiscard]] ProfileId profile() const noexcept { return table_->profile; }

    void begin(const TxBegin& begin_value) { table_->begin(object_, begin_value); }
    void append_dict(const DictMessage& message) { table_->append_dict(object_, message); }
    void append_body(const BodyMessage& message) { table_->append_body(object_, message); }
    void receive_need(const NeedMessage& message) { table_->receive_need(object_, message); }
    void receive_fill(const FillMessage& message) { table_->receive_fill(object_, message); }
    [[nodiscard]] std::vector<uint8_t> materialize() { return table_->materialize(object_); }
    void commit_visible() { table_->commit_visible(object_); }
    void disconnect() noexcept { table_->disconnect(object_); }

    [[nodiscard]] ProfileDialogueState state() const noexcept {
        return table_->state(object_);
    }
    [[nodiscard]] bool terminal() const noexcept { return table_->terminal(object_); }
    [[nodiscard]] size_t pending_body_bytes() const noexcept {
        return table_->pending_body_bytes(object_);
    }
    [[nodiscard]] const TxBegin* active_begin() const noexcept {
        return table_->active_begin(object_);
    }

private:
    ProfileDialogue(const ProfileDialogueVTable* table, void* object) noexcept
        : table_(table), object_(object) {}

    void reset() noexcept;

    const ProfileDialogueVTable* table_ = &empty_table();
    void* object_ = nullptr;

    static const ProfileDialogueVTable& empty_table() noexcept;

    friend ProfileDialogue make_profile_dialogue(ProfileId,
                                                 ProfileDialogueConfig);
};

// The factory is the only profile-selection point used by the transaction
// engine. Unsupported negotiated/selected profiles fail closed here rather
// than forcing a concrete-profile branch into pending transaction state.
ProfileDialogue make_profile_dialogue(ProfileId profile, ProfileDialogueConfig config);

} // namespace icecc::p50
