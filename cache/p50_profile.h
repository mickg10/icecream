#pragma once

#include "protocol50.h"

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
    CStoreGuid c_store_guid{};
    // Profile-neutral resource contract. Adapters translate these values to
    // codec-specific limits; the transaction engine never does.
    uint64_t max_encoded_body_bytes = 0;
    uint64_t max_raw_bytes = 0;
    int max_window_log = 27;
    uint64_t max_history_bytes = uint64_t{128} << 20;
};

enum class ProfileCommitState : uint8_t {
    Committed,
    Tentative,
};

// A profile implementation is deliberately C-style at this boundary.  The
// transaction engine never names, allocates, or destroys a concrete profile
// dialogue; adding a profile adds one vtable/factory implementation instead.
struct ProfileDialogueVTable {
    ProfileId profile = ProfileId::P29;
    void (*destroy)(void*) noexcept = nullptr;
    void (*begin)(void*, const TxBegin&) = nullptr;
    void (*append_dict)(void*, const DictMessage&) = nullptr;
    void (*append_body)(void*, const BodyMessage&) = nullptr;
    std::vector<NeedMessage> (*need_messages)(void*, size_t) = nullptr;
    void (*receive_need)(void*, const NeedMessage&) = nullptr;
    void (*receive_fill)(void*, const FillMessage&) = nullptr;
    std::vector<uint8_t> (*materialize)(void*) = nullptr;
    // Promotion is bound to the exact terminal identity emitted by the
    // reducer. A profile cannot make tentative bytes visible for a different
    // transaction.
    void (*commit_visible)(void*, const TxCommit&) = nullptr;
    void (*discard_tentative)(void*) noexcept = nullptr;
    void (*disconnect)(void*) noexcept = nullptr;
    void (*reset)(void*) = nullptr;
    ProfileDialogueState (*state)(const void*) noexcept = nullptr;
    bool (*terminal)(const void*) noexcept = nullptr;
    ProfileCommitState (*commit_state)(const void*) noexcept = nullptr;
    size_t (*pending_body_bytes)(const void*) noexcept = nullptr;
    uint64_t (*pending_segment_bytes)(const void*) noexcept = nullptr;
    Digest128 (*pending_segment_digest)(const void*) noexcept = nullptr;
    uint64_t (*window_limit_bytes)(const void*) noexcept = nullptr;
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
    [[nodiscard]] std::vector<NeedMessage> need_messages(size_t max_payload) {
        return table_->need_messages ? table_->need_messages(object_, max_payload)
                                     : std::vector<NeedMessage>{};
    }
    void receive_need(const NeedMessage& message) { table_->receive_need(object_, message); }
    void receive_fill(const FillMessage& message) { table_->receive_fill(object_, message); }
    [[nodiscard]] std::vector<uint8_t> materialize() { return table_->materialize(object_); }
    void commit_visible(const TxCommit& commit) { table_->commit_visible(object_, commit); }
    void discard_tentative() noexcept { table_->discard_tentative(object_); }
    void disconnect() noexcept { table_->disconnect(object_); }
    void reset() { table_->reset(object_); }

    [[nodiscard]] ProfileDialogueState state() const noexcept {
        return table_->state(object_);
    }
    [[nodiscard]] bool terminal() const noexcept { return table_->terminal(object_); }
    [[nodiscard]] ProfileCommitState commit_state() const noexcept {
        return table_->commit_state(object_);
    }
    [[nodiscard]] size_t pending_body_bytes() const noexcept {
        return table_->pending_body_bytes(object_);
    }
    [[nodiscard]] uint64_t pending_segment_bytes() const noexcept {
        return table_->pending_segment_bytes
                   ? table_->pending_segment_bytes(object_)
                   : 0;
    }
    [[nodiscard]] Digest128 pending_segment_digest() const noexcept {
        return table_->pending_segment_digest
                   ? table_->pending_segment_digest(object_)
                   : Digest128{};
    }
    [[nodiscard]] uint64_t window_limit_bytes() const noexcept {
        return table_->window_limit_bytes(object_);
    }
    [[nodiscard]] const TxBegin* active_begin() const noexcept {
        return table_->active_begin(object_);
    }

private:
    ProfileDialogue(const ProfileDialogueVTable* table, void* object) noexcept
        : table_(table), object_(object) {}

    void destroy() noexcept;

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
