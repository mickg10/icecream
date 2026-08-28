#include "p50_profile.h"

#include "p50_zstd.h"
#include "p50_slice0.h"

// P29 is the reviewed CW_P29_BSC_Z3_M64 implementation from
// capability/grouprlz/p29_online_s1.h (SHA-256
// ac23199cc719a2ee28c4418e70fc358f9e2d13096713a5b6872ec028474879bf).
// This adapter uses its OnlineS1 catalogue/object closure directly; it does
// not substitute a delta or ZSTD codec.

#if defined(ICECC_P50_WITH_LIBBSC)
#include "p50_grz.h"
#endif

#include <stdexcept>
#include <utility>

namespace icecc::p50 {
namespace {

class P29LiveDialogue {
public:
    enum class State { Idle, ReceivingBody, BodyClosed, Materialized, Terminal };

    explicit P29LiveDialogue(uint32_t negotiated_profiles, CStoreGuid c_store_guid)
        : negotiated_profiles_(negotiated_profiles),
          store_(FStoreGuid::from_u64(UINT64_C(0x5032394c495645))),
          c_guid_(c_store_guid) {}

    void begin(const TxBegin& value) {
        if (value.profile != ProfileId::P29 ||
            value.p29_root_mode == P29RootMode::NotApplicable)
            fail("P29 TX_BEGIN is invalid");
        if ((negotiated_profiles_ & profile_bit(ProfileId::P29)) == 0)
            fail("P29 was not negotiated");
        if (state_ != State::Idle)
            fail("P29 TX_BEGIN arrived while dialogue is active");
        if (c_guid_ == CStoreGuid{})
            c_guid_ = c_guid_from_begin(value);
        if (!session_) {
            session_ = store_.connect(c_guid_);
            const SessionState state = store_.resume(*session_);
            if (!state.route_present)
                store_.start_route(*session_, value.history_nonce, value.pre_state_digest);
        }
        store_.begin(*session_, value);
        begin_ = value;
        dict_bytes_ = body_bytes_ = 0;
        remaining_.clear();
        need_sent_ = false;
        state_ = State::ReceivingBody;
    }

    void append_dict(const DictMessage& message) {
        require_active();
        store_.append_dict(*session_, message.bytes);
        dict_bytes_ += message.bytes.size();
        if (dict_bytes_ > begin_.dict.encoded_bytes)
            fail("P29 DICT exceeds TX_BEGIN descriptor");
    }

    void append_body(const BodyMessage& message) {
        require_active();
        store_.append_body(*session_, message.bytes);
        body_bytes_ += message.bytes.size();
        if (body_bytes_ > begin_.body.encoded_bytes)
            fail("P29 BODY exceeds TX_BEGIN descriptor");
        if (body_bytes_ == begin_.body.encoded_bytes && dict_bytes_ == begin_.dict.encoded_bytes &&
            remaining_.empty())
            state_ = State::BodyClosed;
    }

    std::vector<NeedMessage> need_messages(size_t max_payload) {
        require_active();
        if (dict_bytes_ != begin_.dict.encoded_bytes)
            return {};
        if (need_sent_)
            return {};
        if (!need_sent_) {
            const Need need = store_.need(*session_);
            remaining_.insert(need.missing.begin(), need.missing.end());
            need_sent_ = true;
        }
        const std::vector<Key64> keys(remaining_.begin(), remaining_.end());
        return encode_need_messages(keys, max_payload);
    }

    void receive_need(const NeedMessage&) { fail("P29 F received an unexpected NEED"); }

    void receive_fill(const FillMessage& message) {
        require_active();
        for (const ObjectApplied& applied : store_.append_fill(*session_, message))
            remaining_.erase(applied.key);
        if (body_bytes_ == begin_.body.encoded_bytes && dict_bytes_ == begin_.dict.encoded_bytes &&
            remaining_.empty())
        {
            store_.finish_fill(*session_);
            state_ = State::BodyClosed;
        }
    }

    std::vector<uint8_t> materialize() {
        require_active();
        if (state_ != State::BodyClosed)
            fail("P29 input cannot materialize before BODY and FILL closure");
        const std::vector<uint8_t> result = store_.materialize_and_verify(*session_);
        state_ = State::Materialized;
        return result;
    }

    void commit_visible(const TxCommit& commit) {
        require_active();
        if (state_ != State::Materialized || store_.commit_input(*session_) != commit)
            fail("P29 TX_COMMIT does not close the materialized input");
        begin_ = {};
        state_ = State::Idle;
    }

    void discard_tentative() noexcept {
        reset_active();
        if (state_ != State::Terminal)
            state_ = State::Idle;
    }
    void disconnect() noexcept { reset_active(); state_ = State::Terminal; }
    void reset() { reset_active(); state_ = State::Idle; }

    State state() const noexcept { return state_; }
    bool terminal() const noexcept { return state_ == State::Terminal; }
    size_t pending_body_bytes() const noexcept { return body_bytes_; }
    uint64_t window_limit_bytes() const noexcept { return UINT64_C(1) << 27; }
    const TxBegin* active_begin() const noexcept {
        return state_ == State::Idle || state_ == State::Terminal ? nullptr : &begin_;
    }

private:
    static CStoreGuid c_guid_from_begin(const TxBegin& begin) {
        Digest128Builder digest;
        digest.append("icecc-p50-p29-c-guid-v1");
        digest.append(begin.raw_digest.bytes);
        CStoreGuid result;
        result.bytes = digest.finish().bytes;
        if (result == CStoreGuid{}) result.bytes.back() = 1;
        return result;
    }

    [[noreturn]] void fail(const char* message) {
        state_ = State::Terminal;
        throw std::logic_error(message);
    }

    void require_active() const {
        if (!session_ || (state_ != State::ReceivingBody && state_ != State::BodyClosed &&
                          state_ != State::Materialized))
            throw std::logic_error("P29 dialogue has no active transaction");
    }

    void reset_active() noexcept {
        if (session_) {
            try { store_.disconnect(*session_); } catch (...) {}
            session_.reset();
        }
        begin_ = {};
        remaining_.clear();
        dict_bytes_ = body_bytes_ = 0;
        need_sent_ = false;
    }

    uint32_t negotiated_profiles_ = 0;
    FStore store_;
    CStoreGuid c_guid_{};
    std::optional<SessionHandle> session_;
    TxBegin begin_{};
    std::set<Key64> remaining_;
    size_t dict_bytes_ = 0;
    size_t body_bytes_ = 0;
    bool need_sent_ = false;
    State state_ = State::Idle;
};

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

void destroy_p29(void* object) noexcept {
    delete static_cast<P29LiveDialogue*>(object);
}

void begin_p29(void* object, const TxBegin& begin) {
    static_cast<P29LiveDialogue*>(object)->begin(begin);
}

void append_dict_p29(void* object, const DictMessage& message) {
    static_cast<P29LiveDialogue*>(object)->append_dict(message);
}

void append_body_p29(void* object, const BodyMessage& message) {
    static_cast<P29LiveDialogue*>(object)->append_body(message);
}

std::vector<NeedMessage> need_p29(void* object, size_t max_payload) {
    return static_cast<P29LiveDialogue*>(object)->need_messages(max_payload);
}

void receive_need_p29(void* object, const NeedMessage& message) {
    static_cast<P29LiveDialogue*>(object)->receive_need(message);
}

void receive_fill_p29(void* object, const FillMessage& message) {
    static_cast<P29LiveDialogue*>(object)->receive_fill(message);
}

std::vector<uint8_t> materialize_p29(void* object) {
    return static_cast<P29LiveDialogue*>(object)->materialize();
}

void commit_visible_p29(void* object, const TxCommit& commit) {
    static_cast<P29LiveDialogue*>(object)->commit_visible(commit);
}

void discard_p29(void* object) noexcept {
    static_cast<P29LiveDialogue*>(object)->discard_tentative();
}

void disconnect_p29(void* object) noexcept {
    static_cast<P29LiveDialogue*>(object)->disconnect();
}

void reset_p29(void* object) {
    static_cast<P29LiveDialogue*>(object)->reset();
}

ProfileDialogueState state_p29(const void* object) noexcept {
    switch (static_cast<const P29LiveDialogue*>(object)->state()) {
    case P29LiveDialogue::State::Idle: return ProfileDialogueState::Idle;
    case P29LiveDialogue::State::ReceivingBody: return ProfileDialogueState::ReceivingBody;
    case P29LiveDialogue::State::BodyClosed: return ProfileDialogueState::BodyClosed;
    case P29LiveDialogue::State::Materialized: return ProfileDialogueState::Materialized;
    case P29LiveDialogue::State::Terminal: return ProfileDialogueState::Terminal;
    }
    return ProfileDialogueState::Terminal;
}

bool terminal_p29(const void* object) noexcept {
    return static_cast<const P29LiveDialogue*>(object)->terminal();
}

ProfileCommitState commit_state_p29(const void* object) noexcept {
    const auto state = static_cast<const P29LiveDialogue*>(object)->state();
    return state == P29LiveDialogue::State::ReceivingBody ||
                   state == P29LiveDialogue::State::BodyClosed ||
                   state == P29LiveDialogue::State::Materialized
               ? ProfileCommitState::Tentative
               : ProfileCommitState::Committed;
}

size_t pending_p29(const void* object) noexcept {
    return static_cast<const P29LiveDialogue*>(object)->pending_body_bytes();
}

uint64_t window_p29(const void* object) noexcept {
    return static_cast<const P29LiveDialogue*>(object)->window_limit_bytes();
}

const TxBegin* active_p29(const void* object) noexcept {
    return static_cast<const P29LiveDialogue*>(object)->active_begin();
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

#if defined(ICECC_P50_WITH_LIBBSC)
ProfileDialogueState map_grz_state(GrzResidualDialogue::State state) noexcept {
    switch (state) {
    case GrzResidualDialogue::State::Idle: return ProfileDialogueState::Idle;
    case GrzResidualDialogue::State::ReceivingBody: return ProfileDialogueState::ReceivingBody;
    case GrzResidualDialogue::State::BodyClosed: return ProfileDialogueState::BodyClosed;
    case GrzResidualDialogue::State::Materialized: return ProfileDialogueState::Materialized;
    case GrzResidualDialogue::State::Terminal: return ProfileDialogueState::Terminal;
    }
    return ProfileDialogueState::Terminal;
}
void destroy_grz(void* object) noexcept { delete static_cast<GrzResidualDialogue*>(object); }
void begin_grz(void* object, const TxBegin& value) { static_cast<GrzResidualDialogue*>(object)->begin(value); }
void append_dict_grz(void* object, const DictMessage& value) { static_cast<GrzResidualDialogue*>(object)->append_dict(value); }
void append_body_grz(void* object, const BodyMessage& value) { static_cast<GrzResidualDialogue*>(object)->append_body(value); }
void receive_need_grz(void* object, const NeedMessage& value) { static_cast<GrzResidualDialogue*>(object)->receive_need(value); }
void receive_fill_grz(void* object, const FillMessage& value) { static_cast<GrzResidualDialogue*>(object)->receive_fill(value); }
std::vector<uint8_t> materialize_grz(void* object) { return static_cast<GrzResidualDialogue*>(object)->materialize(); }
void commit_visible_grz(void* object, const TxCommit& value) { static_cast<GrzResidualDialogue*>(object)->commit_visible(value); }
void discard_tentative_grz(void* object) noexcept { static_cast<GrzResidualDialogue*>(object)->discard_tentative(); }
void disconnect_grz(void* object) noexcept { static_cast<GrzResidualDialogue*>(object)->disconnect(); }
void reset_grz(void* object) { static_cast<GrzResidualDialogue*>(object)->reset(); }
ProfileDialogueState state_grz(const void* object) noexcept { return map_grz_state(static_cast<const GrzResidualDialogue*>(object)->state()); }
bool terminal_grz(const void* object) noexcept { return static_cast<const GrzResidualDialogue*>(object)->terminal(); }
ProfileCommitState commit_state_grz(const void* object) noexcept {
    const auto state = static_cast<const GrzResidualDialogue*>(object)->state();
    return state == GrzResidualDialogue::State::ReceivingBody ||
                   state == GrzResidualDialogue::State::BodyClosed ||
                   state == GrzResidualDialogue::State::Materialized
               ? ProfileCommitState::Tentative : ProfileCommitState::Committed;
}
size_t pending_body_bytes_grz(const void* object) noexcept { return static_cast<const GrzResidualDialogue*>(object)->pending_body_bytes(); }
uint64_t window_limit_bytes_grz(const void* object) noexcept { return static_cast<const GrzResidualDialogue*>(object)->window_limit_bytes(); }
const TxBegin* active_begin_grz(const void* object) noexcept {
    const auto& active = static_cast<const GrzResidualDialogue*>(object)->active_begin();
    return active ? &*active : nullptr;
}
#endif

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

const ProfileDialogueVTable kP29VTable{
    .profile = ProfileId::P29,
    .destroy = destroy_p29,
    .begin = begin_p29,
    .append_dict = append_dict_p29,
    .append_body = append_body_p29,
    .need_messages = need_p29,
    .receive_need = receive_need_p29,
    .receive_fill = receive_fill_p29,
    .materialize = materialize_p29,
    .commit_visible = commit_visible_p29,
    .discard_tentative = discard_p29,
    .disconnect = disconnect_p29,
    .reset = reset_p29,
    .state = state_p29,
    .terminal = terminal_p29,
    .commit_state = commit_state_p29,
    .pending_body_bytes = pending_p29,
    .window_limit_bytes = window_p29,
    .active_begin = active_p29,
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

#if defined(ICECC_P50_WITH_LIBBSC)
const ProfileDialogueVTable kGrzVTable{
    .profile = ProfileId::GRZ,
    .destroy = destroy_grz,
    .begin = begin_grz,
    .append_dict = append_dict_grz,
    .append_body = append_body_grz,
    .receive_need = receive_need_grz,
    .receive_fill = receive_fill_grz,
    .materialize = materialize_grz,
    .commit_visible = commit_visible_grz,
    .discard_tentative = discard_tentative_grz,
    .disconnect = disconnect_grz,
    .reset = reset_grz,
    .state = state_grz,
    .terminal = terminal_grz,
    .commit_state = commit_state_grz,
    .pending_body_bytes = pending_body_bytes_grz,
    .window_limit_bytes = window_limit_bytes_grz,
    .active_begin = active_begin_grz,
};
#endif

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
                             config.max_window_log,
                             config.max_history_bytes};
    switch (profile) {
    case ProfileId::P29:
        return ProfileDialogue(&kP29VTable,
                               new P29LiveDialogue(config.negotiated_profiles,
                                                   config.c_store_guid));
    case ProfileId::ZSTD_TU:
        return ProfileDialogue(&kZstdTuVTable,
                               new ZstdTuDialogue(config.negotiated_profiles,
                                                  zstd_limits));
    case ProfileId::Z3_LONG:
        return ProfileDialogue(&kZstdRouteVTable,
                               new ZstdRouteDialogue(config.negotiated_profiles,
                                                     zstd_limits));
#if defined(ICECC_P50_WITH_LIBBSC)
    case ProfileId::GRZ:
        return ProfileDialogue(&kGrzVTable,
                               new GrzResidualDialogue(config.negotiated_profiles,
                                                       zstd_limits));
#endif
    default:
        throw std::invalid_argument("transaction profile has no dialogue implementation");
    }
}

} // namespace icecc::p50
