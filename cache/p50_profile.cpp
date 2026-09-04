#include "p50_profile.h"

#include "p50_zstd.h"
#include "p50_slice0.h"
#include "codec/p29_wire.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace icecc::p50 {
namespace {

class P29V1LiveDialogue {
public:
    enum class State { Idle, ReceivingBody, BodyClosed, Materialized, Terminal };

    explicit P29V1LiveDialogue(ProfileDialogueConfig config)
        : negotiated_profiles_(config.negotiated_profiles),
          max_tu_bytes_(config.max_raw_bytes),
          need_inner_bound_(need_bound(config.max_raw_bytes)),
          fill_inner_bound_(fill_bound(config.max_raw_bytes)),
          store_(FStoreGuid::from_u64(UINT64_C(0x50323956314c4956)), 1,
                 nullptr, config.max_raw_bytes,
                 config.system_source_reuse),
          c_guid_(config.c_store_guid) {
        if (max_tu_bytes_ == 0 ||
            max_tu_bytes_ > std::numeric_limits<size_t>::max())
            throw std::invalid_argument("P29V1 max TU bytes are not addressable");
    }

    void begin(const TxBegin& value) {
        if (value.profile != ProfileId::P29V1 ||
            value.body.encoding !=
                static_cast<uint16_t>(ProfileId::P29V1) ||
            value.raw_bytes > max_tu_bytes_)
            fail("P29V1 TX_BEGIN is invalid");
        if ((negotiated_profiles_ & profile_bit(ProfileId::P29V1)) == 0)
            fail("P29V1 was not negotiated");
        if (state_ != State::Idle)
            fail("P29V1 TX_BEGIN arrived while dialogue is active");
        try {
            if (c_guid_ == CStoreGuid{})
                c_guid_ = c_guid_from_begin(value);
            if (!session_) {
                session_ = store_.connect(c_guid_);
                const SessionState route = store_.resume(*session_);
                if (!route.route_present)
                    store_.start_route(*session_, value.history_nonce,
                                       value.pre_state_digest);
            }
            store_.begin(*session_, value);
            begin_ = value;
            body_bytes_ = 0;
            body_complete_ = false;
            need_sent_ = false;
            need_inner_.clear();
            fill_decoder_.emplace(fill_inner_bound_);
            state_ = State::ReceivingBody;
        } catch (...) {
            terminalize();
            throw;
        }
    }

    void append_body(const BodyMessage& message) {
        require_receiving();
        try {
            if (message.bytes.size() > begin_.body.encoded_bytes - body_bytes_)
                fail("P29V1 BODY exceeds TX_BEGIN descriptor");
            store_.append_body(*session_, message.bytes);
            body_bytes_ += message.bytes.size();
            if (body_bytes_ == begin_.body.encoded_bytes) {
                need_inner_ = store_.p29v1_need_frames(*session_);
                body_complete_ = true;
            }
        } catch (...) {
            terminalize();
            throw;
        }
    }

    std::vector<NeedMessage> need_messages(size_t max_payload) {
        require_receiving();
        if (!body_complete_ || need_sent_)
            return {};
        try {
            std::vector<NeedMessage> result = encode_p29v1_need_messages(
                need_inner_, max_payload, need_inner_bound_);
            need_sent_ = true;
            return result;
        } catch (...) {
            terminalize();
            throw;
        }
    }

    void receive_need(const NeedMessage&) {
        fail("P29V1 F received an unexpected NEED");
    }

    void receive_fill(const FillMessage& message) {
        require_receiving();
        if (!body_complete_ || !need_sent_ || !fill_decoder_)
            fail("P29V1 FILL preceded BODY or NEED");
        try {
            fill_decoder_->push(message);
            if (fill_decoder_->complete()) {
                store_.append_fill_v1(*session_,
                                      fill_decoder_->take_inner_frames());
                state_ = State::BodyClosed;
            }
        } catch (...) {
            terminalize();
            throw;
        }
    }

    std::vector<uint8_t> materialize() {
        if (!session_ || state_ != State::BodyClosed)
            fail("P29V1 input cannot materialize before FILL closure");
        try {
            std::vector<uint8_t> result =
                store_.materialize_and_verify(*session_);
            state_ = State::Materialized;
            return result;
        } catch (...) {
            terminalize();
            throw;
        }
    }

    void commit_visible(const TxCommit& commit) {
        if (!session_ || state_ != State::Materialized)
            fail("P29V1 commit precedes materialization");
        try {
            if (store_.commit_input(*session_) != commit)
                fail("P29V1 TX_COMMIT does not close the materialized input");
            clear_per_tu();
            state_ = State::Idle;
        } catch (...) {
            terminalize();
            throw;
        }
    }

    void discard_tentative() noexcept {
        if (session_)
            store_.abandon_input(*session_);
        clear_per_tu();
        if (state_ != State::Terminal)
            state_ = State::Idle;
    }

    void disconnect() noexcept {
        terminalize();
    }

    void reset() {
        if (session_) {
            store_.abandon_input(*session_);
            try {
                store_.forget_route(*session_);
                store_.disconnect(*session_);
                session_.reset();
            } catch (...) {
                terminalize();
                throw;
            }
        }
        clear_per_tu();
        state_ = State::Idle;
    }

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] bool terminal() const noexcept {
        return state_ == State::Terminal;
    }
    [[nodiscard]] size_t pending_body_bytes() const noexcept {
        return body_bytes_;
    }
    [[nodiscard]] uint64_t pending_segment_bytes() const noexcept {
        return session_ && state_ == State::Materialized
                   ? store_.pending_segment_bytes(*session_)
                   : 0;
    }
    [[nodiscard]] Digest128 pending_segment_digest() const noexcept {
        return session_ && state_ == State::Materialized
                   ? store_.pending_segment_digest(*session_)
                   : Digest128{};
    }
    [[nodiscard]] uint64_t window_limit_bytes() const noexcept {
        return max_tu_bytes_;
    }
    [[nodiscard]] const TxBegin* active_begin() const noexcept {
        return state_ == State::Idle || state_ == State::Terminal ? nullptr
                                                                  : &begin_;
    }

private:
    static uint64_t need_bound(uint64_t max_tu_bytes) {
        codec::P29WireLimits limits;
        limits.max_tu_bytes = static_cast<size_t>(std::min<uint64_t>(
            max_tu_bytes, std::numeric_limits<size_t>::max()));
        limits.max_region_bytes = limits.max_tu_bytes;
        return codec::p29v1_need_inner_bound(limits);
    }

    static uint64_t fill_bound(uint64_t max_tu_bytes) {
        codec::P29WireLimits limits;
        limits.max_tu_bytes = static_cast<size_t>(std::min<uint64_t>(
            max_tu_bytes, std::numeric_limits<size_t>::max()));
        limits.max_region_bytes = limits.max_tu_bytes;
        return codec::p29v1_fill_inner_bound(limits);
    }

    static CStoreGuid c_guid_from_begin(const TxBegin& begin) {
        Digest128Builder digest;
        digest.append("icecc-p50-p29v1-c-guid-v1");
        digest.append(begin.raw_digest.bytes);
        CStoreGuid result;
        result.bytes = digest.finish().bytes;
        if (result == CStoreGuid{})
            result.bytes.back() = 1;
        return result;
    }

    [[noreturn]] void fail(const char* message) {
        terminalize();
        throw std::logic_error(message);
    }

    void require_receiving() {
        if (!session_ || state_ != State::ReceivingBody)
            fail("P29V1 dialogue is not receiving a transaction");
    }

    void clear_per_tu() noexcept {
        begin_ = {};
        body_bytes_ = 0;
        body_complete_ = false;
        need_sent_ = false;
        need_inner_.clear();
        fill_decoder_.reset();
    }

    void terminalize() noexcept {
        if (session_) {
            store_.abandon_input(*session_);
            try {
                store_.disconnect(*session_);
            } catch (...) {
            }
            session_.reset();
        }
        clear_per_tu();
        state_ = State::Terminal;
    }

    uint32_t negotiated_profiles_ = 0;
    uint64_t max_tu_bytes_ = 0;
    uint64_t need_inner_bound_ = 0;
    uint64_t fill_inner_bound_ = 0;
    FStore store_;
    CStoreGuid c_guid_{};
    std::optional<SessionHandle> session_;
    TxBegin begin_{};
    size_t body_bytes_ = 0;
    bool body_complete_ = false;
    bool need_sent_ = false;
    std::vector<uint8_t> need_inner_;
    std::optional<P29V1FillStreamDecoder> fill_decoder_;
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

void destroy_p29v1(void* object) noexcept {
    delete static_cast<P29V1LiveDialogue*>(object);
}

void begin_p29v1(void* object, const TxBegin& begin) {
    static_cast<P29V1LiveDialogue*>(object)->begin(begin);
}

void append_body_p29v1(void* object, const BodyMessage& message) {
    static_cast<P29V1LiveDialogue*>(object)->append_body(message);
}

std::vector<NeedMessage> need_p29v1(void* object, size_t max_payload) {
    return static_cast<P29V1LiveDialogue*>(object)->need_messages(max_payload);
}

void receive_need_p29v1(void* object, const NeedMessage& message) {
    static_cast<P29V1LiveDialogue*>(object)->receive_need(message);
}

void receive_fill_p29v1(void* object, const FillMessage& message) {
    static_cast<P29V1LiveDialogue*>(object)->receive_fill(message);
}

std::vector<uint8_t> materialize_p29v1(void* object) {
    return static_cast<P29V1LiveDialogue*>(object)->materialize();
}

void commit_visible_p29v1(void* object, const TxCommit& commit) {
    static_cast<P29V1LiveDialogue*>(object)->commit_visible(commit);
}

void discard_p29v1(void* object) noexcept {
    static_cast<P29V1LiveDialogue*>(object)->discard_tentative();
}

void disconnect_p29v1(void* object) noexcept {
    static_cast<P29V1LiveDialogue*>(object)->disconnect();
}

void reset_p29v1(void* object) {
    static_cast<P29V1LiveDialogue*>(object)->reset();
}

ProfileDialogueState state_p29v1(const void* object) noexcept {
    switch (static_cast<const P29V1LiveDialogue*>(object)->state()) {
    case P29V1LiveDialogue::State::Idle: return ProfileDialogueState::Idle;
    case P29V1LiveDialogue::State::ReceivingBody:
        return ProfileDialogueState::ReceivingBody;
    case P29V1LiveDialogue::State::BodyClosed:
        return ProfileDialogueState::BodyClosed;
    case P29V1LiveDialogue::State::Materialized:
        return ProfileDialogueState::Materialized;
    case P29V1LiveDialogue::State::Terminal:
        return ProfileDialogueState::Terminal;
    }
    return ProfileDialogueState::Terminal;
}

bool terminal_p29v1(const void* object) noexcept {
    return static_cast<const P29V1LiveDialogue*>(object)->terminal();
}

ProfileCommitState commit_state_p29v1(const void* object) noexcept {
    const auto state = static_cast<const P29V1LiveDialogue*>(object)->state();
    return state == P29V1LiveDialogue::State::ReceivingBody ||
                   state == P29V1LiveDialogue::State::BodyClosed ||
                   state == P29V1LiveDialogue::State::Materialized
               ? ProfileCommitState::Tentative
               : ProfileCommitState::Committed;
}

size_t pending_body_bytes_p29v1(const void* object) noexcept {
    return static_cast<const P29V1LiveDialogue*>(object)->pending_body_bytes();
}

uint64_t pending_segment_bytes_p29v1(const void* object) noexcept {
    return static_cast<const P29V1LiveDialogue*>(object)->pending_segment_bytes();
}

Digest128 pending_segment_digest_p29v1(const void* object) noexcept {
    return static_cast<const P29V1LiveDialogue*>(object)->pending_segment_digest();
}

uint64_t window_limit_bytes_p29v1(const void* object) noexcept {
    return static_cast<const P29V1LiveDialogue*>(object)->window_limit_bytes();
}

const TxBegin* active_begin_p29v1(const void* object) noexcept {
    return static_cast<const P29V1LiveDialogue*>(object)->active_begin();
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

const ProfileDialogueVTable kP29V1VTable{
    .profile = ProfileId::P29V1,
    .destroy = destroy_p29v1,
    .begin = begin_p29v1,
    .append_body = append_body_p29v1,
    .need_messages = need_p29v1,
    .receive_need = receive_need_p29v1,
    .receive_fill = receive_fill_p29v1,
    .materialize = materialize_p29v1,
    .commit_visible = commit_visible_p29v1,
    .discard_tentative = discard_p29v1,
    .disconnect = disconnect_p29v1,
    .reset = reset_p29v1,
    .state = state_p29v1,
    .terminal = terminal_p29v1,
    .commit_state = commit_state_p29v1,
    .pending_body_bytes = pending_body_bytes_p29v1,
    .pending_segment_bytes = pending_segment_bytes_p29v1,
    .pending_segment_digest = pending_segment_digest_p29v1,
    .window_limit_bytes = window_limit_bytes_p29v1,
    .active_begin = active_begin_p29v1,
};

const ProfileDialogueVTable kZstdRouteVTable{
    .profile = ProfileId::ZSTD_ROUTE,
    .destroy = destroy_route,
    .begin = begin_route,
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
        .profile = ProfileId::P29V1,
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

VerifiedMaterialization ProfileDialogue::materialize_verified() {
    const TxBegin* active = active_begin();
    if (active == nullptr)
        throw std::logic_error(
            "profile cannot verify materialization without an active TX_BEGIN");
    const TxBegin verified_begin = *active;
    std::vector<uint8_t> exact = table_->materialize(object_);
    const TxBegin* materialized_begin = active_begin();
    if (state() != ProfileDialogueState::Materialized ||
        materialized_begin == nullptr || *materialized_begin != verified_begin ||
        verified_begin.raw_bytes > std::numeric_limits<size_t>::max() ||
        exact.size() != static_cast<size_t>(verified_begin.raw_bytes))
        throw std::logic_error(
            "profile materialization did not preserve its verified TX_BEGIN");
    return VerifiedMaterialization(verified_begin, std::move(exact));
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
    case ProfileId::P29V1:
        return ProfileDialogue(&kP29V1VTable,
                               new P29V1LiveDialogue(config));
    case ProfileId::ZSTD_TU:
        return ProfileDialogue(&kZstdTuVTable,
                               new ZstdTuDialogue(config.negotiated_profiles,
                                                  zstd_limits));
    case ProfileId::ZSTD_ROUTE:
        return ProfileDialogue(&kZstdRouteVTable,
                               new ZstdRouteDialogue(config.negotiated_profiles,
                                                     zstd_limits));
    default:
        throw std::invalid_argument("transaction profile has no dialogue implementation");
    }
}

} // namespace icecc::p50
