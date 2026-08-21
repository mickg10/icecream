#pragma once

#include "p50_actions.h"
#include "protocol50.h"
#include "capability/grouprlz/p29_online_s1.h"

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <unordered_map>
#include <variant>
#include <vector>

namespace icecc::p50 {

struct BytesPayload {
    std::vector<uint8_t> bytes;
    auto operator<=>(const BytesPayload&) const = default;
};

struct ChildrenPayload {
    std::vector<Key64> children;
    auto operator<=>(const ChildrenPayload&) const = default;
};

using ObjectPayload = std::variant<BytesPayload, ChildrenPayload>;

constexpr uint16_t kP29KeyVectorEncoding = 1;

struct ImmutableObject {
    Key64 key{};
    ObjectPayload payload;
    Digest128 content_digest{};

    static ImmutableObject bytes(Key64 key, std::span<const uint8_t> payload);
    static ImmutableObject children(Key64 key, std::span<const Key64> payload);
    static ImmutableObject from_record(const FillRecord& record);

    [[nodiscard]] bool valid_shape() const;
    [[nodiscard]] std::vector<uint8_t> canonical_payload() const;
    [[nodiscard]] FillRecord fill_record() const;
    auto operator<=>(const ImmutableObject&) const = default;
};

enum class ObjectApplyResult { Applied, Duplicate };

struct ObjectApplied {
    Key64 key{};
    Digest128 content_digest{};
    ObjectApplyResult result = ObjectApplyResult::Applied;
    auto operator<=>(const ObjectApplied&) const = default;
};

class ImmutableObjectStore {
public:
    ObjectApplyResult apply(const ImmutableObject& object);
    [[nodiscard]] const ImmutableObject* find(Key64 key) const;
    [[nodiscard]] bool contains(Key64 key) const { return find(key) != nullptr; }
    [[nodiscard]] size_t size() const { return objects_.size(); }
    void clear() { objects_.clear(); }

private:
    std::unordered_map<Key64, ImmutableObject, Key64Hash> objects_;
};

enum class GenerationAdvanceResult { Advanced, GuidFlipRequired };

class CObjectArena {
public:
    explicit CObjectArena(CStoreGuid guid, uint16_t generation = 0,
                          uint64_t first_ordinal = 1);

    Key64 intern_bytes(ObjectType type, std::span<const uint8_t> payload);
    Key64 intern_children(ObjectType type, std::span<const Key64> payload);
    GenerationAdvanceResult advance_generation();

    [[nodiscard]] const ImmutableObject& object(Key64 key) const;
    [[nodiscard]] const ImmutableObjectStore& objects() const { return objects_; }
    [[nodiscard]] const CStoreGuid& guid() const { return guid_; }
    [[nodiscard]] uint16_t generation() const { return generation_; }

private:
    Key64 allocate(ObjectType type);
    std::optional<Key64> find_equal(ObjectType type,
                                    const ObjectPayload& payload,
                                    Digest128 content_digest) const;
    Key64 install(ObjectType type, ObjectPayload payload);

    CStoreGuid guid_{};
    uint16_t generation_ = 0;
    uint64_t first_ordinal_ = 1;
    std::array<uint64_t, 32> next_ordinal_{};
    ImmutableObjectStore objects_;
    std::map<Digest128, std::vector<Key64>> content_index_;
};

struct PreparedTU {
    TuSeq tu_seq{};
    uint64_t raw_bytes = 0;
    Digest128 raw_digest{};
    std::vector<Key64> regions;

private:
    friend class CAuthority;
    friend class CRoute;
    std::vector<uint32_t> dense_regions;
};

using PreparedTUPtr = std::shared_ptr<const PreparedTU>;

class CAuthority {
public:
    explicit CAuthority(CStoreGuid guid,
                        p29::OnlineS1::Config config = p29::OnlineS1::Config{},
                        uint16_t generation = 0, uint64_t first_ordinal = 1);

    Key64 intern_bytes(ObjectType type, std::span<const uint8_t> payload) {
        return arena_.intern_bytes(type, payload);
    }
    Key64 intern_children(ObjectType type, std::span<const Key64> payload) {
        return arena_.intern_children(type, payload);
    }
    GenerationAdvanceResult advance_generation() { return arena_.advance_generation(); }

    PreparedTUPtr prepare_tu(std::span<const uint8_t> exact_input,
                             std::span<const Key64> regions);
    PreparedTUPtr prepare_from_regions(
        std::span<const std::vector<uint8_t>> region_bytes);

    [[nodiscard]] const CStoreGuid& guid() const { return arena_.guid(); }
    [[nodiscard]] const CObjectArena& arena() const { return arena_; }
    [[nodiscard]] p29::BlockCatalogue& block_catalogue() { return block_catalogue_; }
    [[nodiscard]] p29::OnlineS1::Config s1_config() const { return s1_config_; }
    [[nodiscard]] Key64 dense_region_key(uint32_t id) const;
    [[nodiscard]] Key64 block_key(uint32_t id) const;
    void publish_new_p29_blocks();
    [[nodiscard]] std::vector<Key64> transitive_manifest(
        std::span<const Key64> roots) const;
    [[nodiscard]] std::vector<uint8_t> materialize(
        const ImmutableObjectStore& objects, std::span<const Key64> roots) const;

private:
    uint32_t dense_region(Key64 key);
    TuSeq allocate_tu_seq();

    CObjectArena arena_;
    p29::OnlineS1::Config s1_config_;
    p29::BlockCatalogue block_catalogue_;
    std::unordered_map<Key64, uint32_t, Key64Hash> region_to_dense_;
    std::vector<Key64> dense_to_region_;
    std::vector<Key64> block_keys_;
    uint64_t next_tu_seq_ = 0;
    bool tu_seq_exhausted_ = false;
};

struct Need {
    HistoryNonce history_nonce{};
    RelSeq rel_seq{};
    TuSeq tu_seq{};
    Digest128 transaction_digest{};
    std::vector<Key64> missing;
    auto operator<=>(const Need&) const = default;
};

struct CActiveTx {
    PreparedTUPtr prepared;
    TxBegin begin;
    std::vector<uint8_t> dict;
    std::vector<uint8_t> body;
    std::vector<Key64> root;
    std::vector<Key64> manifest;
};

class FStore;
struct ReconnectResult;

class CRoute {
public:
    CRoute(CAuthority& authority, FStoreGuid f_store_guid,
           HistoryNonce history_nonce, ActionTrace* trace = nullptr);
    ~CRoute();
    CRoute(const CRoute&) = delete;
    CRoute& operator=(const CRoute&) = delete;

    const CActiveTx& begin(
        const PreparedTUPtr& prepared,
        P29RootMode root_mode = P29RootMode::RouteHistory);
    std::vector<ImmutableObject> build_fill(const Need& need) const;
    void accept_commit(const TxCommit& committed,
                       ActionType action = ActionType::COMMIT_ACCEPTED);

    [[nodiscard]] const std::optional<CActiveTx>& active() const { return active_; }
    [[nodiscard]] const CStoreGuid& c_store_guid() const { return authority_.guid(); }
    [[nodiscard]] const FStoreGuid& f_store_guid() const { return f_store_guid_; }
    [[nodiscard]] HistoryNonce history_nonce() const { return history_nonce_; }
    [[nodiscard]] RelSeq next_rel_seq() const { return next_rel_seq_; }
    [[nodiscard]] Digest128 state_digest() const { return state_digest_; }

private:
    friend ReconnectResult reconnect(CRoute&, FStore&, HistoryNonce);
    void abandon_active();
    void reset_history(FStoreGuid f_store_guid, HistoryNonce history_nonce);
    void record(ActionType action, const CActiveTx& active);

    CAuthority& authority_;
    FStoreGuid f_store_guid_{};
    HistoryNonce history_nonce_{};
    RelSeq next_rel_seq_{};
    Digest128 state_digest_{};
    std::unique_ptr<p29::OnlineS1> matcher_;
    std::optional<CActiveTx> active_;
    ActionTrace* trace_ = nullptr;
};

struct SessionHandle {
    CStoreGuid c_store_guid{};
    FStoreGuid f_store_guid{};
    uint64_t serial = 0;
    auto operator<=>(const SessionHandle&) const = default;
};

class FStore {
public:
    explicit FStore(FStoreGuid guid, uint64_t first_session_serial = 1,
                    ActionTrace* trace = nullptr);
    ~FStore();
    FStore(const FStore&) = delete;
    FStore& operator=(const FStore&) = delete;

    SessionHandle connect(CStoreGuid c_store_guid);
    void disconnect(SessionHandle session);
    [[nodiscard]] SessionState resume(SessionHandle session) const;
    void start_route(SessionHandle session, HistoryNonce history_nonce,
                     Digest128 initial_state_digest);

    void begin(SessionHandle session, const TxBegin& begin, bool replay = false);
    void append_dict(SessionHandle session, std::span<const uint8_t> bytes);
    void append_body(SessionHandle session, std::span<const uint8_t> bytes);
    [[nodiscard]] Need need(SessionHandle session) const;
    ObjectApplied apply_object(SessionHandle session, const ImmutableObject& object);
    std::vector<ObjectApplied> append_fill(SessionHandle session,
                                           const FillMessage& message);
    void finish_fill(SessionHandle session) const;
    std::vector<uint8_t> materialize_and_verify(SessionHandle session);
    TxCommit commit_input(SessionHandle session);

    void forget_route(SessionHandle session);
    void destructive_cache_reset(FStoreGuid new_guid);

    [[nodiscard]] const FStoreGuid& guid() const { return guid_; }
    [[nodiscard]] size_t object_count(CStoreGuid c_store_guid) const;
    [[nodiscard]] bool contains(CStoreGuid c_store_guid, Key64 key) const;

private:
    struct Namespace;
    Namespace& require_namespace(SessionHandle session);
    const Namespace& require_namespace(SessionHandle session) const;
    void append_component(SessionHandle session, bool dict,
                          std::span<const uint8_t> bytes);
    void record(ActionType action, SessionHandle session, const TxBegin* begin,
                std::optional<Key64> key = std::nullopt,
                Digest128 content_digest = {},
                uint64_t remaining_need = 0, bool duplicate = false,
                std::span<const Key64> need_keys = {});

    FStoreGuid guid_{};
    uint64_t next_session_serial_ = 1;
    bool session_serial_exhausted_ = false;
    std::unordered_map<CStoreGuid, std::unique_ptr<Namespace>, Id128Hash> namespaces_;
    ActionTrace* trace_ = nullptr;
};

enum class ReconnectOutcome {
    ExactMatch,
    LostFinalAcknowledgement,
    ColdFStore,
    RouteHistoryReset,
};

struct ReconnectResult {
    ReconnectOutcome outcome = ReconnectOutcome::ExactMatch;
    SessionHandle session{};
    bool replay_active = false;
};

ReconnectResult reconnect(CRoute& c_route, FStore& f_store,
                          HistoryNonce fresh_history_nonce);

}  // namespace icecc::p50
