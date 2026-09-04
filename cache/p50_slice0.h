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
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace icecc::p50 {

class P50PreparationAuthority;

struct BytesPayload {
    std::vector<uint8_t> bytes;
    auto operator<=>(const BytesPayload&) const = default;
};

struct ChildrenPayload {
    std::vector<Key64> children;
    auto operator<=>(const ChildrenPayload&) const = default;
};

using ObjectPayload = std::variant<BytesPayload, ChildrenPayload>;

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
    ObjectApplyResult apply(ImmutableObject&& object);
    [[nodiscard]] const ImmutableObject* find(Key64 key) const;
    [[nodiscard]] bool contains(Key64 key) const { return find(key) != nullptr; }
    [[nodiscard]] size_t size() const { return objects_.size(); }
    void reserve(size_t count) { objects_.reserve(count); }
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
    void reserve_for_tu(size_t expected_lines);
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
    std::unordered_map<Digest128, std::vector<Key64>, Digest128Hash> content_index_;
};

// The resource owner is deliberately independent from a C/F relationship.
// F-side namespaces share one resident/staging budget and one slot pool, so
// this state cannot live in either route's transaction cursor.  Production
// code can attach a GlobalResourceTrace to retain the same action vocabulary
// used by formal/Protocol50Global.tla.
enum class GlobalActionType : uint8_t {
    NAMESPACE_ADMITTED,
    NAMESPACE_TOUCHED,
    TU_STARTED,
    TU_FINISHED,
    ARENA_INSTALLING,
    ARENA_RETRY_INSTALLING,
    ARENA_PRESENT,
    ARENA_PINNED,
    ARENA_UNPINNED,
    ARENA_RELEASED,
    INSTALL_CRASHED,
    CONTENT_CONFLICT_FATAL,
    NAMESPACE_EVICTED,
    GENERATION_ADVANCED,
    GENERATION_WRAP_STOPPED,
    C_GUID_FLIPPED,
};

std::string_view global_action_name(GlobalActionType action);

struct GlobalActionRecord {
    GlobalActionType action = GlobalActionType::NAMESPACE_ADMITTED;
    CStoreGuid c_store_guid{};
    CStoreGuid previous_c_store_guid{};
    uint16_t generation = 0;
    Key64 key{};
    uint32_t slot = 0;
    uint64_t bytes = 0;
    uint64_t lru = 0;
    Digest128 content_digest{};
};

class GlobalResourceTrace {
public:
    void record(GlobalActionRecord action) { records_.push_back(std::move(action)); }
    [[nodiscard]] const std::vector<GlobalActionRecord>& records() const { return records_; }
    void clear() { records_.clear(); }

private:
    std::vector<GlobalActionRecord> records_;
};

std::string global_action_jsonl(const GlobalActionRecord& record);
void write_global_trace(const GlobalResourceTrace& trace, const std::string& path);

struct GlobalResourceLimits {
    uint64_t max_aggregate_bytes = uint64_t{8} << 30;
    uint64_t max_namespace_bytes = uint64_t{4} << 30;
    uint64_t max_staging_bytes = uint64_t{4} << 30;
    uint64_t max_total_bytes = uint64_t{12} << 30;
    uint16_t max_generation = KeyLayoutV1::generation_value_mask;
    size_t max_staging_slots = 32;
    auto operator<=>(const GlobalResourceLimits&) const = default;
};

// Test-only fault switches make the safety boundary executable: a mutant may
// bypass one admission check, but check_invariants() must then reject the
// resulting state.  All defaults preserve the production rules.
struct GlobalResourceFaults {
    bool ignore_aggregate_cap = false;
    bool ignore_namespace_cap = false;
    bool ignore_staging_cap = false;
    bool ignore_total_cap = false;
    bool ignore_slot_ownership = false;
    bool ignore_lru = false;
    auto operator<=>(const GlobalResourceFaults&) const = default;
};

class GlobalResourceModel {
public:
    explicit GlobalResourceModel(GlobalResourceLimits limits = {},
                                 GlobalResourceFaults faults = {},
                                 GlobalResourceTrace* trace = nullptr);
    ~GlobalResourceModel();
    GlobalResourceModel(const GlobalResourceModel&) = delete;
    GlobalResourceModel& operator=(const GlobalResourceModel&) = delete;

    void admit(CStoreGuid c_store_guid, uint16_t generation = 0);
    void touch(CStoreGuid c_store_guid);
    void start_tu(CStoreGuid c_store_guid);
    void finish_tu(CStoreGuid c_store_guid);
    void begin_install(CStoreGuid c_store_guid, Key64 key, Digest128 content_digest,
                       uint64_t bytes, size_t slot, bool retry = false);
    // Validate the two installs that make one P29V1 route-dictionary/input
    // publication before either staged object becomes resident.  The owner
    // still publishes P29Segment first so a visible input never names missing
    // dictionary state.
    void preflight_publish_pair(CStoreGuid c_store_guid,
                                Key64 segment_key, size_t segment_slot,
                                Digest128 segment_digest,
                                Key64 input_key, size_t input_slot,
                                Digest128 input_digest) const;
    void publish(CStoreGuid c_store_guid, Key64 key, size_t slot,
                 Digest128 content_digest);
    void pin(CStoreGuid c_store_guid, Key64 key);
    void unpin(CStoreGuid c_store_guid, Key64 key);
    void crash_install(CStoreGuid c_store_guid, Key64 key, size_t slot);
    [[noreturn]] void conflict(CStoreGuid c_store_guid, Key64 key,
                               Digest128 content_digest);
    void evict(CStoreGuid c_store_guid);
    CStoreGuid evict_oldest();
    void advance_generation(CStoreGuid c_store_guid);
    void stop_generation_wrap(CStoreGuid c_store_guid);
    void flip_guid(CStoreGuid old_guid, CStoreGuid new_guid);

    [[nodiscard]] std::optional<std::string> check_invariants() const;
    [[nodiscard]] uint64_t resident_bytes() const;
    [[nodiscard]] uint64_t staging_bytes() const;
    [[nodiscard]] size_t live_namespace_count() const;
    [[nodiscard]] size_t free_staging_slots() const;
    [[nodiscard]] std::optional<size_t> first_free_staging_slot() const;
    [[nodiscard]] bool install_retry_required(CStoreGuid c_store_guid,
                                              Key64 key) const;
    void release(CStoreGuid c_store_guid, Key64 key);

    // Exposed only so the out-of-line implementation can keep the model's
    // state opaque to callers while remaining C++23-library friendly.
    struct Object;
    struct Namespace;

private:
    Namespace& require_namespace(CStoreGuid c_store_guid);
    const Namespace& require_namespace(CStoreGuid c_store_guid) const;
    void emit(GlobalActionRecord action);

    GlobalResourceLimits limits_;
    GlobalResourceFaults faults_;
    GlobalResourceTrace* trace_ = nullptr;
    uint64_t clock_ = 0;
    std::map<CStoreGuid, std::unique_ptr<Namespace>> namespaces_;
    std::map<size_t, std::pair<CStoreGuid, Key64>> slots_;
    std::optional<uint64_t> last_eviction_lru_;
};

struct PreparedTU {
    TuSeq tu_seq{};
    uint64_t raw_bytes = 0;
    Digest128 raw_digest{};

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
                        TuSeq first_tu_seq = {});
    ~CAuthority();
    CAuthority(const CAuthority&) = delete;
    CAuthority& operator=(const CAuthority&) = delete;

    [[nodiscard]] const CStoreGuid& guid() const { return guid_; }
    [[nodiscard]] p29::BlockCatalogue& block_catalogue() { return block_catalogue_; }
    [[nodiscard]] p29::OnlineS1::Config s1_config() const { return s1_config_; }
    [[nodiscard]] bool p29v1_runnable() const noexcept;
    [[nodiscard]] uint64_t p29v1_interner_reserved_bytes() const noexcept;
    [[nodiscard]] uint64_t p29v1_interner_committed_bytes() const noexcept;
private:
    friend class P50PreparationAuthority;
    // Reserve and commit are split so a failed preparation never consumes a
    // C-wide identity.  Both are owner-thread operations; commit accepts only
    // the currently reserved successor and cannot roll back a published TU.
    TuSeq reserve_tu_seq() const;
    void commit_tu_seq(TuSeq reserved);
    void enable_p29v1(uint64_t max_interner_reserved_bytes,
                      uint64_t max_tu_bytes);
    PreparedTUPtr prepare_p29v1_at_seq(std::span<const uint8_t> exact_input,
                                       TuSeq tu_seq,
                                       Digest128 exact_digest);

    struct P29V1State;

    CStoreGuid guid_{};
    p29::OnlineS1::Config s1_config_;
    p29::BlockCatalogue block_catalogue_;
    uint64_t next_tu_seq_ = 0;
    bool tu_seq_exhausted_ = false;
    std::unique_ptr<P29V1State> p29v1_;

    friend class CRoute;
};

struct CActiveTx {
    PreparedTUPtr prepared;
    TxBegin begin;
    std::vector<uint8_t> body;
    size_t region_count = 0;
    size_t block_use_count = 0;
    size_t new_block_count = 0;
};

class CRoute {
public:
    CRoute(CAuthority& authority, FStoreGuid f_store_guid,
           HistoryNonce history_nonce, ActionTrace* trace = nullptr);
    ~CRoute();
    CRoute(const CRoute&) = delete;
    CRoute& operator=(const CRoute&) = delete;

    const CActiveTx& begin_v1(const PreparedTUPtr& prepared,
                              uint64_t max_route_state_bytes);
    void pin_v1_system_source_reuse(bool reuse);
    std::span<const uint8_t> build_fill_v1(
        std::span<const uint8_t> inner_need);
    void restart_v1_for_transport_retry();
    void reset_v1_route(FStoreGuid f_store_guid,
                        HistoryNonce history_nonce);
    void accept_commit(const TxCommit& committed,
                       ActionType action = ActionType::COMMIT_ACCEPTED);
    void abandon_active();

    [[nodiscard]] const std::optional<CActiveTx>& active() const { return active_; }
    [[nodiscard]] const CStoreGuid& c_store_guid() const { return authority_.guid(); }
    [[nodiscard]] const FStoreGuid& f_store_guid() const { return f_store_guid_; }
    [[nodiscard]] HistoryNonce history_nonce() const { return history_nonce_; }
    [[nodiscard]] RelSeq next_rel_seq() const { return next_rel_seq_; }
    [[nodiscard]] Digest128 state_digest() const { return state_digest_; }
    [[nodiscard]] uint64_t p29v1_route_state_bytes() const noexcept;
    [[nodiscard]] std::optional<bool> p29v1_system_source_reuse() const noexcept;

private:
    void reset_history(FStoreGuid f_store_guid, HistoryNonce history_nonce);
    void record(ActionType action, const CActiveTx& active);

    CAuthority& authority_;
    FStoreGuid f_store_guid_{};
    HistoryNonce history_nonce_{};
    RelSeq next_rel_seq_{};
    Digest128 state_digest_{};
    std::optional<CActiveTx> active_;
    struct P29V1State;
    std::unique_ptr<P29V1State> p29v1_;
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
                    ActionTrace* trace = nullptr,
                    uint64_t p29v1_max_tu_bytes = uint64_t{1} << 30,
                    bool p29v1_system_source_reuse = false);
    ~FStore();
    FStore(const FStore&) = delete;
    FStore& operator=(const FStore&) = delete;

    SessionHandle connect(CStoreGuid c_store_guid);
    void disconnect(SessionHandle session);
    [[nodiscard]] SessionState resume(SessionHandle session) const;
    void start_route(SessionHandle session, HistoryNonce history_nonce,
                     Digest128 initial_state_digest);

    void begin(SessionHandle session, const TxBegin& begin, bool replay = false);
    void append_body(SessionHandle session, std::span<const uint8_t> bytes);
    [[nodiscard]] std::vector<uint8_t> p29v1_need_frames(SessionHandle session) const;
    [[nodiscard]] bool p29v1_system_source_reuse(SessionHandle session) const;
    void append_fill_v1(SessionHandle session,
                        std::vector<uint8_t> inner_fill);
    std::vector<uint8_t> materialize_and_verify(SessionHandle session);
    TxCommit commit_input(SessionHandle session);
    void abandon_input(SessionHandle session) noexcept;

    [[nodiscard]] uint64_t pending_segment_bytes(SessionHandle session) const noexcept;
    [[nodiscard]] Digest128 pending_segment_digest(SessionHandle session) const noexcept;

    void forget_route(SessionHandle session);
    void destructive_cache_reset(FStoreGuid new_guid);

    [[nodiscard]] const FStoreGuid& guid() const { return guid_; }
private:
    struct Namespace;
    Namespace& require_namespace(SessionHandle session);
    const Namespace& require_namespace(SessionHandle session) const;
    void abandon_pending(Namespace& space) noexcept;
    void append_component(SessionHandle session,
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
    uint64_t p29v1_max_tu_bytes_ = uint64_t{1} << 30;
    bool p29v1_system_source_reuse_ = false;
};

// Start the process-wide P29V1 system-source fingerprint worker.  Product
// startup calls this before advertising readiness; it never waits for the
// filesystem scan.  An absolute cache directory enables the persistent
// metadata-keyed digest cache.  Empty/invalid paths retain safe uncached
// operation.
void start_p29_system_source_fingerprint(
    std::string cache_directory = {}) noexcept;

// Simulator-only synchronization: product request paths never call this.
// It returns immediately when no worker was started.
void wait_p29_system_source_fingerprint() noexcept;

// Nonblocking snapshot used only by the P29V1 profile guard.  Until the
// background worker publishes a complete result this returns zero and turns
// source reuse off.  Enumeration/hash failures likewise publish zero without
// making any other profile unavailable.
[[nodiscard]] Digest128 p29_system_source_fingerprint() noexcept;

}  // namespace icecc::p50
