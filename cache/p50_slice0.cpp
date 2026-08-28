#include "p50_slice0.h"
#include "p50_p29_residual.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace icecc::p50 {
namespace {

constexpr std::array<uint8_t, 4> kP29ResidualMagic{'P', '2', '9', 'R'};

void append_u64le(std::vector<uint8_t>& out, uint64_t value) {
    for (unsigned shift = 0; shift != 64; shift += 8)
        out.push_back(static_cast<uint8_t>(value >> shift));
}

uint64_t read_u64le(std::span<const uint8_t> bytes, size_t& offset) {
    if (bytes.size() - offset < 8)
        throw std::invalid_argument("P29 residual BODY ended before its length");
    uint64_t result = 0;
    for (unsigned shift = 0; shift != 64; shift += 8)
        result |= uint64_t(bytes[offset++]) << shift;
    return result;
}

std::vector<uint8_t> encode_p29_residual_body(std::span<const uint8_t> root,
                                              std::span<const uint8_t> residual) {
    std::vector<uint8_t> result;
    result.reserve(kP29ResidualMagic.size() + 16 + root.size() + residual.size());
    result.insert(result.end(), kP29ResidualMagic.begin(), kP29ResidualMagic.end());
    append_u64le(result, root.size());
    append_u64le(result, residual.size());
    result.insert(result.end(), root.begin(), root.end());
    result.insert(result.end(), residual.begin(), residual.end());
    return result;
}

struct P29ResidualBody {
    std::vector<uint8_t> root;
    std::vector<uint8_t> residual;
};

P29ResidualBody decode_p29_residual_body(std::span<const uint8_t> bytes) {
    if (bytes.size() < kP29ResidualMagic.size() + 16 ||
        !std::equal(kP29ResidualMagic.begin(), kP29ResidualMagic.end(), bytes.begin()))
        throw std::invalid_argument("P29 BODY is not a residual-group envelope");
    size_t offset = kP29ResidualMagic.size();
    const uint64_t root_size = read_u64le(bytes, offset);
    const uint64_t residual_size = read_u64le(bytes, offset);
    if (root_size > bytes.size() - offset || residual_size > bytes.size() - offset - root_size ||
        offset + root_size + residual_size != bytes.size())
        throw std::invalid_argument("P29 residual BODY lengths are not exact");
    P29ResidualBody result;
    result.root.assign(bytes.begin() + offset, bytes.begin() + offset + root_size);
    offset += static_cast<size_t>(root_size);
    result.residual.assign(bytes.begin() + offset, bytes.end());
    return result;
}

void append_unacknowledged_line_payload(
    const CObjectArena& arena, std::span<const Key64> regions,
    const std::set<Key64>& acknowledged, std::vector<uint8_t>& output) {
    for (Key64 region_key : regions) {
        const auto* region = std::get_if<ChildrenPayload>(&arena.object(region_key).payload);
        if (!region || region->children.size() != 1)
            throw std::logic_error("P29 Region does not contain exactly one Line");
        const Key64 line_key = region->children.front();
        if (acknowledged.contains(line_key)) continue;
        const auto* line = std::get_if<BytesPayload>(&arena.object(line_key).payload);
        if (!line) throw std::logic_error("P29 Region child is not a Line payload");
        const std::vector<uint8_t> payload =
            residual_group::line_payload(line->bytes);
        output.insert(output.end(), payload.begin(), payload.end());
    }
}

std::vector<uint8_t> residual_for_root(
    const ImmutableObjectStore& objects, std::span<const Key64> roots,
    const std::set<Key64>& acknowledged) {
    std::vector<uint8_t> result;
    std::function<void(Key64)> visit = [&](Key64 key) {
        const ImmutableObject& object = *objects.find(key);
        if (object.key.type() == ObjectType::Region) {
            const auto* region = std::get_if<ChildrenPayload>(&object.payload);
            if (!region || region->children.size() != 1)
                throw std::logic_error("P29 F Region does not contain one Line");
            const Key64 line_key = region->children.front();
            if (acknowledged.contains(line_key)) return;
            const auto* line = std::get_if<BytesPayload>(&objects.find(line_key)->payload);
            if (!line) throw std::logic_error("P29 F Region child is not a Line payload");
            const std::vector<uint8_t> payload = residual_group::line_payload(line->bytes);
            result.insert(result.end(), payload.begin(), payload.end());
        } else if (object.key.type() == ObjectType::Block) {
            const auto* block = std::get_if<ChildrenPayload>(&object.payload);
            if (!block) throw std::logic_error("P29 F Block is not a child object");
            for (Key64 child : block->children) visit(child);
        } else {
            throw std::logic_error("P29 F root is not a Region or Block");
        }
    };
    for (Key64 root : roots) visit(root);
    return result;
}

std::vector<uint8_t> reconstruct_root_with_residual(
    const ImmutableObjectStore& objects, std::span<const Key64> roots,
    const std::set<Key64>& acknowledged, std::span<const uint8_t> residual) {
    std::vector<uint8_t> result;
    size_t residual_offset = 0;
    std::function<void(Key64)> visit = [&](Key64 key) {
        const ImmutableObject* object = objects.find(key);
        if (!object) throw std::logic_error("P29 F root references an absent object");
        if (object->key.type() == ObjectType::Block) {
            const auto* block = std::get_if<ChildrenPayload>(&object->payload);
            if (!block) throw std::logic_error("P29 F Block is not a child object");
            for (Key64 child : block->children) visit(child);
            return;
        }
        if (object->key.type() != ObjectType::Region)
            throw std::logic_error("P29 F root is not a Region or Block");
        const auto* region = std::get_if<ChildrenPayload>(&object->payload);
        if (!region || region->children.size() != 1)
            throw std::logic_error("P29 F Region does not contain one Line");
        const Key64 line_key = region->children.front();
        const ImmutableObject* line_object = objects.find(line_key);
        const auto* line = line_object
                               ? std::get_if<BytesPayload>(&line_object->payload)
                               : nullptr;
        if (!line) throw std::logic_error("P29 F Region child is not a Line payload");
        if (acknowledged.contains(line_key)) {
            result.insert(result.end(), line->bytes.begin(), line->bytes.end());
            return;
        }
        for (uint8_t byte : line->bytes) {
            if (byte == '\n') {
                result.push_back(byte);
            } else {
                if (residual_offset == residual.size())
                    throw std::logic_error("P29 residual ended during reconstruction");
                result.push_back(residual[residual_offset++]);
            }
        }
    };
    for (Key64 root : roots) visit(root);
    if (residual_offset != residual.size())
        throw std::logic_error("P29 residual has trailing bytes after reconstruction");
    return result;
}

bool byte_object(ObjectType type) {
    return type == ObjectType::Atom || type == ObjectType::Line ||
           type == ObjectType::Material || type == ObjectType::Path ||
           type == ObjectType::Blob;
}

std::vector<uint8_t> encode_payload(ObjectType type, const ObjectPayload& payload) {
    std::vector<uint8_t> result;
    const auto append_u64 = [&](uint64_t value) {
        for (int shift = 56; shift >= 0; shift -= 8)
            result.push_back(static_cast<uint8_t>(value >> shift));
    };
    if (const auto* bytes = std::get_if<BytesPayload>(&payload)) {
        result.push_back(0);
        append_u64(bytes->bytes.size());
        result.insert(result.end(), bytes->bytes.begin(), bytes->bytes.end());
    } else {
        const auto& children = std::get<ChildrenPayload>(payload).children;
        result.push_back(1);
        append_u64(children.size());
        for (Key64 child : children) append_u64(child.wire_value());
    }
    (void)type;
    return result;
}

uint64_t read_u64(std::span<const uint8_t> bytes, size_t& offset) {
    if (bytes.size() - offset < 8)
        throw std::invalid_argument("canonical object payload ended early");
    uint64_t result = 0;
    for (unsigned i = 0; i != 8; ++i) result = (result << 8) | bytes[offset++];
    return result;
}

ObjectPayload decode_object_payload(ObjectType type, std::span<const uint8_t> bytes) {
    if (bytes.empty()) throw std::invalid_argument("canonical object payload is empty");
    size_t offset = 1;
    const uint64_t count = read_u64(bytes, offset);
    if (bytes[0] == 0) {
        if (!byte_object(type) || count != bytes.size() - offset)
            throw std::invalid_argument("byte object payload has the wrong shape");
        return BytesPayload{{bytes.begin() + offset, bytes.end()}};
    }
    if (bytes[0] != 1 || byte_object(type) || count > (bytes.size() - offset) / 8 ||
        count * 8 != bytes.size() - offset)
        throw std::invalid_argument("child object payload has the wrong shape");
    ChildrenPayload result;
    result.children.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i != count; ++i) {
        const auto key = Key64::from_wire(read_u64(bytes, offset));
        if (!key) throw std::invalid_argument("child object contains invalid Key64");
        result.children.push_back(*key);
    }
    return result;
}

Digest128 object_digest(ObjectType type, const ObjectPayload& payload) {
    const std::vector<uint8_t> encoded = encode_payload(type, payload);
    Digest128Builder out;
    out.append("ICECC-P50-OBJECT-V1");
    out.append_u8(static_cast<uint8_t>(type));
    out.append_u64(encoded.size());
    out.append(encoded);
    return out.finish();
}

void append_materialized(const ImmutableObjectStore& objects, Key64 key,
                         std::set<Key64>& visiting, std::set<Key64>& reached,
                         std::vector<uint8_t>& output) {
    const ImmutableObject* object = objects.find(key);
    if (!object) throw std::logic_error("materialization references a missing object");
    if (!visiting.insert(key).second)
        throw std::logic_error("immutable object graph contains a cycle");
    reached.insert(key);
    if (const auto* bytes = std::get_if<BytesPayload>(&object->payload)) {
        if (bytes->bytes.size() > output.max_size() - output.size())
            throw std::overflow_error("materialized input exceeds addressable size");
        output.insert(output.end(), bytes->bytes.begin(), bytes->bytes.end());
    } else {
        for (Key64 child : std::get<ChildrenPayload>(object->payload).children)
            append_materialized(objects, child, visiting, reached, output);
    }
    visiting.erase(key);
}

std::vector<uint8_t> materialize_objects(const ImmutableObjectStore& objects,
                                         std::span<const Key64> roots,
                                         std::set<Key64>* reached = nullptr) {
    std::set<Key64> visiting;
    std::set<Key64> local_reached;
    std::vector<uint8_t> result;
    for (Key64 root : roots)
        append_materialized(objects, root, visiting, local_reached, result);
    if (reached) *reached = std::move(local_reached);
    return result;
}

std::vector<uint8_t> encode_key_vector(std::span<const Key64> keys) {
    if (keys.empty()) return {};
    if (keys.size() > std::numeric_limits<uint32_t>::max())
        throw std::overflow_error("Key64 vector exceeds u32 count");
    std::vector<uint8_t> result;
    result.reserve(4 + keys.size() * 8);
    const uint32_t count = static_cast<uint32_t>(keys.size());
    for (int shift = 24; shift >= 0; shift -= 8)
        result.push_back(static_cast<uint8_t>(count >> shift));
    for (Key64 key : keys)
        for (int shift = 56; shift >= 0; shift -= 8)
            result.push_back(static_cast<uint8_t>(key.wire_value() >> shift));
    return result;
}

std::vector<Key64> decode_key_vector(std::span<const uint8_t> bytes) {
    if (bytes.empty()) return {};
    if (bytes.size() < 4) throw std::invalid_argument("key vector ended before count");
    uint32_t count = 0;
    for (unsigned i = 0; i != 4; ++i) count = (count << 8) | bytes[i];
    if (bytes.size() != 4 + uint64_t(count) * 8)
        throw std::invalid_argument("key vector length does not match count");
    std::vector<Key64> result;
    result.reserve(count);
    size_t offset = 4;
    for (uint32_t i = 0; i != count; ++i) {
        uint64_t raw = 0;
        for (unsigned j = 0; j != 8; ++j) raw = (raw << 8) | bytes[offset++];
        const auto key = Key64::from_wire(raw);
        if (!key) throw std::invalid_argument("key vector contains invalid Key64");
        result.push_back(*key);
    }
    return result;
}

bool same_commit(const TxCommit& commit, const CActiveTx& active) {
    const Digest128 expected_post = compute_post_state_digest(
        active.begin.pre_state_digest, active.begin.history_nonce,
        active.begin.rel_seq, active.begin.tu_seq, active.begin.transaction_digest);
    return commit.history_nonce == active.begin.history_nonce &&
           commit.rel_seq == active.begin.rel_seq &&
           commit.tu_seq == active.begin.tu_seq &&
           commit.transaction_digest == active.begin.transaction_digest &&
           commit.raw_digest == active.begin.raw_digest &&
           commit.post_state_digest == expected_post;
}

bool same_begin(const TxBegin& left, const TxBegin& right) { return left == right; }

}  // namespace

ImmutableObject ImmutableObject::bytes(Key64 key,
                                       std::span<const uint8_t> payload) {
    ImmutableObject result{key, BytesPayload{{payload.begin(), payload.end()}}, {}};
    result.content_digest = object_digest(key.type(), result.payload);
    if (!result.valid_shape()) throw std::invalid_argument("invalid byte object shape");
    return result;
}

ImmutableObject ImmutableObject::children(Key64 key,
                                          std::span<const Key64> payload) {
    ImmutableObject result{key, ChildrenPayload{{payload.begin(), payload.end()}}, {}};
    result.content_digest = object_digest(key.type(), result.payload);
    if (!result.valid_shape()) throw std::invalid_argument("invalid child object shape");
    return result;
}

ImmutableObject ImmutableObject::from_record(const FillRecord& record) {
    if (!record.key.valid()) throw std::invalid_argument("FILL object has invalid Key64");
    ImmutableObject result{record.key,
                           decode_object_payload(record.key.type(), record.object_bytes),
                           record.content_digest};
    if (!result.valid_shape())
        throw std::invalid_argument("FILL object content does not match its digest or type");
    return result;
}

bool ImmutableObject::valid_shape() const {
    if (!key.valid() || content_digest != object_digest(key.type(), payload)) return false;
    if (byte_object(key.type())) return std::holds_alternative<BytesPayload>(payload);
    const auto* children = std::get_if<ChildrenPayload>(&payload);
    if (!children) return false;
    if (key.type() == ObjectType::Block)
        return std::all_of(children->children.begin(), children->children.end(),
                           [](Key64 child) { return child.type() == ObjectType::Region; });
    return key.type() == ObjectType::Region &&
           std::all_of(children->children.begin(), children->children.end(),
                       [](Key64 child) {
                           return child.valid() && child.type() != ObjectType::Region &&
                                  child.type() != ObjectType::Block;
                       });
}

std::vector<uint8_t> ImmutableObject::canonical_payload() const {
    return encode_payload(key.type(), payload);
}

FillRecord ImmutableObject::fill_record() const {
    return {key, content_digest, canonical_payload()};
}

ObjectApplyResult ImmutableObjectStore::apply(const ImmutableObject& object) {
    if (!object.valid_shape()) throw std::invalid_argument("invalid immutable object");
    auto [position, inserted] = objects_.emplace(object.key, object);
    if (inserted) return ObjectApplyResult::Applied;
    if (position->second != object)
        throw std::logic_error("Key64 already names different immutable content");
    return ObjectApplyResult::Duplicate;
}

const ImmutableObject* ImmutableObjectStore::find(Key64 key) const {
    const auto position = objects_.find(key);
    return position == objects_.end() ? nullptr : &position->second;
}

CObjectArena::CObjectArena(CStoreGuid guid, uint16_t generation,
                           uint64_t first_ordinal)
    : guid_(guid), generation_(generation), first_ordinal_(first_ordinal) {
    if (generation > KeyLayoutV1::generation_value_mask)
        throw std::invalid_argument("Key64 generation exceeds KeyLayoutV1");
    if (first_ordinal == 0 || first_ordinal > KeyLayoutV1::ordinal_mask)
        throw std::invalid_argument("Key64 first ordinal exceeds KeyLayoutV1");
    next_ordinal_.fill(first_ordinal_);
}

Key64 CObjectArena::allocate(ObjectType type) {
    if (!Key64::make(type, generation_, 1))
        throw std::invalid_argument("unknown Key64 object type");
    const uint8_t type_value = static_cast<uint8_t>(type);
    uint64_t& next = next_ordinal_[type_value];
    if (next == 0 || next > KeyLayoutV1::ordinal_mask)
        throw std::overflow_error("Key64 ordinal space exhausted");
    const std::optional<Key64> key = Key64::make(type, generation_, next);
    if (!key) throw std::invalid_argument("invalid Key64 arena allocation");
    next = next == KeyLayoutV1::ordinal_mask ? 0 : next + 1;
    return *key;
}

std::optional<Key64> CObjectArena::find_equal(ObjectType type,
                                              const ObjectPayload& payload,
                                              Digest128 digest) const {
    const auto position = content_index_.find(digest);
    if (position == content_index_.end()) return std::nullopt;
    for (Key64 key : position->second) {
        const ImmutableObject& object = this->object(key);
        if (key.type() == type && object.payload == payload) return key;
    }
    return std::nullopt;
}

Key64 CObjectArena::install(ObjectType type, ObjectPayload payload) {
    const Digest128 digest = object_digest(type, payload);
    if (const auto existing = find_equal(type, payload, digest)) return *existing;
    const Key64 key = allocate(type);
    ImmutableObject object{key, std::move(payload), digest};
    objects_.apply(object);
    content_index_[digest].push_back(key);
    return key;
}

Key64 CObjectArena::intern_bytes(ObjectType type,
                                 std::span<const uint8_t> payload) {
    if (!Key64::make(type, generation_, 1))
        throw std::invalid_argument("unknown Key64 object type");
    if (!byte_object(type)) throw std::invalid_argument("object type does not carry bytes");
    return install(type, BytesPayload{{payload.begin(), payload.end()}});
}

Key64 CObjectArena::intern_children(ObjectType type,
                                    std::span<const Key64> payload) {
    const std::optional<Key64> probe = Key64::make(type, generation_, 1);
    if (!probe) throw std::invalid_argument("unknown Key64 object type");
    if (byte_object(type)) throw std::invalid_argument("object type does not carry children");
    ObjectPayload candidate = ChildrenPayload{{payload.begin(), payload.end()}};
    ImmutableObject shape{*probe, candidate, object_digest(type, candidate)};
    if (!shape.valid_shape()) throw std::invalid_argument("invalid child object shape");
    return install(type, std::move(candidate));
}

GenerationAdvanceResult CObjectArena::advance_generation() {
    if (generation_ == KeyLayoutV1::generation_value_mask)
        return GenerationAdvanceResult::GuidFlipRequired;
    ++generation_;
    next_ordinal_.fill(first_ordinal_);
    return GenerationAdvanceResult::Advanced;
}

struct GlobalResourceModel::Object {
    enum class State : uint8_t { Absent, Installing, Present, Pinned };
    State state = State::Absent;
    Digest128 content_digest{};
    uint64_t bytes = 0;
    size_t slot = 0;
    unsigned attempts = 0;
    bool crashed = false;
};

struct GlobalResourceModel::Namespace {
    uint16_t generation = 0;
    bool live = false;
    bool stopped = false;
    bool active = false;
    uint64_t lru = 0;
    std::set<CStoreGuid> guid_history;
    std::map<Key64, Object> objects;
};

namespace {

using GlobalObjectState = GlobalResourceModel::Object::State;

bool checked_add(uint64_t left, uint64_t right, uint64_t limit) {
    return right <= limit && left <= limit - right;
}

}  // namespace

std::string_view global_action_name(GlobalActionType action) {
    switch (action) {
    case GlobalActionType::NAMESPACE_ADMITTED: return "NAMESPACE_ADMITTED";
    case GlobalActionType::NAMESPACE_TOUCHED: return "NAMESPACE_TOUCHED";
    case GlobalActionType::TU_STARTED: return "TU_STARTED";
    case GlobalActionType::TU_FINISHED: return "TU_FINISHED";
    case GlobalActionType::ARENA_INSTALLING: return "ARENA_INSTALLING";
    case GlobalActionType::ARENA_RETRY_INSTALLING: return "ARENA_RETRY_INSTALLING";
    case GlobalActionType::ARENA_PRESENT: return "ARENA_PRESENT";
    case GlobalActionType::ARENA_PINNED: return "ARENA_PINNED";
    case GlobalActionType::ARENA_UNPINNED: return "ARENA_UNPINNED";
    case GlobalActionType::ARENA_RELEASED: return "ARENA_RELEASED";
    case GlobalActionType::INSTALL_CRASHED: return "INSTALL_CRASHED";
    case GlobalActionType::CONTENT_CONFLICT_FATAL: return "CONTENT_CONFLICT_FATAL";
    case GlobalActionType::NAMESPACE_EVICTED: return "NAMESPACE_EVICTED";
    case GlobalActionType::GENERATION_ADVANCED: return "GENERATION_ADVANCED";
    case GlobalActionType::GENERATION_WRAP_STOPPED: return "GENERATION_WRAP_STOPPED";
    case GlobalActionType::C_GUID_FLIPPED: return "C_GUID_FLIPPED";
    }
    throw std::logic_error("unknown Protocol-50 global action");
}

std::string global_action_jsonl(const GlobalActionRecord& record) {
    static constexpr char digits[] = "0123456789abcdef";
    const auto id_hex = [&](const CStoreGuid& id) {
        std::string value;
        value.reserve(id.bytes.size() * 2);
        for (const uint8_t byte : id.bytes) {
            value.push_back(digits[byte >> 4]);
            value.push_back(digits[byte & 0x0f]);
        }
        return value;
    };
    const std::string guid = id_hex(record.c_store_guid);
    std::ostringstream output;
    output << "{\"action\":\"" << global_action_name(record.action)
           << "\",\"c_store_guid\":\"" << guid
           << "\",\"previous_c_store_guid\":\""
           << id_hex(record.previous_c_store_guid)
           << "\",\"generation\":" << record.generation
           << ",\"key64\":" << (record.key.valid() ? record.key.wire_value() : 0)
           << ",\"slot\":" << record.slot
           << ",\"bytes\":" << record.bytes
           << ",\"lru\":" << record.lru
           << ",\"content_digest\":\"" << digest128_hex(record.content_digest)
           << "\"}";
    return output.str();
}

void write_global_trace(const GlobalResourceTrace& trace, const std::string& path) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot open global action trace output");
    for (const GlobalActionRecord& record : trace.records())
        output << global_action_jsonl(record) << '\n';
    if (!output) throw std::runtime_error("cannot write global action trace output");
}

GlobalResourceModel::GlobalResourceModel(GlobalResourceLimits limits,
                                         GlobalResourceFaults faults,
                                         GlobalResourceTrace* trace)
    : limits_(limits), faults_(faults), trace_(trace) {
    if (limits_.max_aggregate_bytes == 0 || limits_.max_namespace_bytes == 0 ||
        limits_.max_staging_bytes == 0 || limits_.max_total_bytes == 0 ||
        limits_.max_staging_slots == 0 ||
        limits_.max_namespace_bytes > limits_.max_aggregate_bytes ||
        limits_.max_aggregate_bytes > limits_.max_total_bytes ||
        limits_.max_generation == 0)
        throw std::invalid_argument("global resource limits are inconsistent");
}

GlobalResourceModel::~GlobalResourceModel() = default;

GlobalResourceModel::Namespace&
GlobalResourceModel::require_namespace(CStoreGuid c_store_guid) {
    const auto position = namespaces_.find(c_store_guid);
    if (position == namespaces_.end()) throw std::logic_error("unknown C namespace");
    return *position->second;
}

const GlobalResourceModel::Namespace&
GlobalResourceModel::require_namespace(CStoreGuid c_store_guid) const {
    const auto position = namespaces_.find(c_store_guid);
    if (position == namespaces_.end()) throw std::logic_error("unknown C namespace");
    return *position->second;
}

void GlobalResourceModel::emit(GlobalActionRecord action) {
    if (trace_) trace_->record(std::move(action));
}

void GlobalResourceModel::admit(CStoreGuid c_store_guid, uint16_t generation) {
    if (c_store_guid == CStoreGuid{})
        throw std::invalid_argument("zero C namespace GUID is reserved");
    auto position = namespaces_.find(c_store_guid);
    if (position == namespaces_.end()) {
        auto inserted = std::make_unique<Namespace>();
        inserted->generation = generation;
        inserted->guid_history.insert(c_store_guid);
        position = namespaces_.emplace(c_store_guid, std::move(inserted)).first;
    }
    Namespace& space = *position->second;
    if (space.live || space.stopped || generation != space.generation ||
        space.generation >= limits_.max_generation)
        throw std::logic_error("namespace admission is not at its current generation");
    space.live = true;
    emit({GlobalActionType::NAMESPACE_ADMITTED, c_store_guid, {}, space.generation});
}

void GlobalResourceModel::touch(CStoreGuid c_store_guid) {
    Namespace& space = require_namespace(c_store_guid);
    if (!space.live) throw std::logic_error("touch of an absent namespace");
    ++clock_;
    space.lru = clock_;
    emit({GlobalActionType::NAMESPACE_TOUCHED, c_store_guid, {}, space.generation,
          {}, 0, 0, space.lru});
}

void GlobalResourceModel::start_tu(CStoreGuid c_store_guid) {
    Namespace& space = require_namespace(c_store_guid);
    if (!space.live || space.active) throw std::logic_error("invalid global TU start");
    space.active = true;
    emit({GlobalActionType::TU_STARTED, c_store_guid, {}, space.generation});
}

void GlobalResourceModel::finish_tu(CStoreGuid c_store_guid) {
    Namespace& space = require_namespace(c_store_guid);
    if (!space.active) throw std::logic_error("invalid global TU finish");
    for (auto& [key, object] : space.objects) {
        if (object.state == GlobalObjectState::Installing)
            throw std::logic_error("cannot finish TU with an installing object");
        if (object.state == GlobalObjectState::Pinned) object.state = GlobalObjectState::Present;
        (void)key;
    }
    space.active = false;
    emit({GlobalActionType::TU_FINISHED, c_store_guid, {}, space.generation});
}

void GlobalResourceModel::begin_install(CStoreGuid c_store_guid, Key64 key,
                                        Digest128 content_digest, uint64_t bytes,
                                        size_t slot, bool retry) {
    Namespace& space = require_namespace(c_store_guid);
    if (!space.live || !space.active || !key.valid() || bytes == 0 ||
        key.generation() != space.generation)
        throw std::logic_error("invalid global install identity");
    auto& object = space.objects[key];
    if (object.state != GlobalObjectState::Absent || (!retry && object.attempts != 0) ||
        (retry && (!object.crashed || object.attempts != 1)))
        throw std::logic_error("global install does not start from the required state");
    const auto owner = slots_.find(slot);
    if (owner != slots_.end() && !faults_.ignore_slot_ownership)
        throw std::logic_error("global staging slot is already owned");
    if (slots_.size() >= limits_.max_staging_slots && owner == slots_.end())
        throw std::length_error("global staging slot pool is exhausted");
    const uint64_t staged = staging_bytes();
    if (!faults_.ignore_staging_cap &&
        (bytes > limits_.max_staging_bytes || staged > limits_.max_staging_bytes - bytes))
        throw std::length_error("global staging byte cap exceeded");
    uint64_t next_staged = 0;
    if (!checked_add(staged, bytes, std::numeric_limits<uint64_t>::max()) ||
        (!faults_.ignore_total_cap &&
         !checked_add(resident_bytes(), next_staged = staged + bytes,
                      limits_.max_total_bytes)))
        throw std::length_error("global total byte cap exceeded");
    object.state = GlobalObjectState::Installing;
    object.content_digest = content_digest;
    object.bytes = bytes;
    object.slot = slot;
    object.attempts = retry ? 2 : 1;
    object.crashed = false;
    slots_[slot] = {c_store_guid, key};
    emit({retry ? GlobalActionType::ARENA_RETRY_INSTALLING : GlobalActionType::ARENA_INSTALLING,
          c_store_guid, {}, space.generation, key, static_cast<uint32_t>(slot), bytes, 0,
          content_digest});
}

void GlobalResourceModel::publish(CStoreGuid c_store_guid, Key64 key, size_t slot,
                                  Digest128 content_digest) {
    Namespace& space = require_namespace(c_store_guid);
    auto object_position = space.objects.find(key);
    if (object_position == space.objects.end() ||
        object_position->second.state != GlobalObjectState::Installing)
        throw std::logic_error("global publish without INSTALLING object");
    Object& object = object_position->second;
    const auto owner = slots_.find(slot);
    if (owner == slots_.end() || owner->second != std::pair{c_store_guid, key})
        throw std::logic_error("global publish lost staging ownership");
    if (object.content_digest != content_digest)
        throw std::logic_error("global staged content changed before publish");
    uint64_t namespace_bytes = 0;
    for (const auto& [unused, candidate] : space.objects)
        if (candidate.state == GlobalObjectState::Present ||
            candidate.state == GlobalObjectState::Pinned)
            namespace_bytes += candidate.bytes;
    if (!faults_.ignore_namespace_cap &&
        (object.bytes > limits_.max_namespace_bytes ||
         namespace_bytes > limits_.max_namespace_bytes - object.bytes))
        throw std::length_error("global namespace byte cap exceeded");
    if (!faults_.ignore_aggregate_cap &&
        (object.bytes > limits_.max_aggregate_bytes ||
         resident_bytes() > limits_.max_aggregate_bytes - object.bytes))
        throw std::length_error("global aggregate byte cap exceeded");
    object.state = GlobalObjectState::Present;
    slots_.erase(owner);
    emit({GlobalActionType::ARENA_PRESENT, c_store_guid, {}, space.generation, key,
          static_cast<uint32_t>(slot), object.bytes, 0, content_digest});
}

void GlobalResourceModel::pin(CStoreGuid c_store_guid, Key64 key) {
    Namespace& space = require_namespace(c_store_guid);
    auto position = space.objects.find(key);
    if (!space.active || position == space.objects.end() ||
        position->second.state != GlobalObjectState::Present)
        throw std::logic_error("global pin requires an active PRESENT object");
    position->second.state = GlobalObjectState::Pinned;
    emit({GlobalActionType::ARENA_PINNED, c_store_guid, {}, space.generation, key, 0,
          position->second.bytes});
}

void GlobalResourceModel::unpin(CStoreGuid c_store_guid, Key64 key) {
    Namespace& space = require_namespace(c_store_guid);
    auto position = space.objects.find(key);
    if (position == space.objects.end() || position->second.state != GlobalObjectState::Pinned)
        throw std::logic_error("global unpin requires a PINNED object");
    position->second.state = GlobalObjectState::Present;
    emit({GlobalActionType::ARENA_UNPINNED, c_store_guid, {}, space.generation, key, 0,
          position->second.bytes});
}

void GlobalResourceModel::crash_install(CStoreGuid c_store_guid, Key64 key, size_t slot) {
    Namespace& space = require_namespace(c_store_guid);
    auto position = space.objects.find(key);
    const auto owner = slots_.find(slot);
    if (position == space.objects.end() || position->second.state != GlobalObjectState::Installing ||
        owner == slots_.end() || owner->second != std::pair{c_store_guid, key})
        throw std::logic_error("global crash does not identify the installing owner");
    const uint64_t bytes = position->second.bytes;
    position->second = Object{};
    position->second.crashed = true;
    position->second.attempts = 1;
    slots_.erase(owner);
    emit({GlobalActionType::INSTALL_CRASHED, c_store_guid, {}, space.generation, key,
          static_cast<uint32_t>(slot), bytes});
}

[[noreturn]] void GlobalResourceModel::conflict(CStoreGuid c_store_guid, Key64 key,
                                                Digest128 content_digest) {
    Namespace& space = require_namespace(c_store_guid);
    const auto position = space.objects.find(key);
    if (position == space.objects.end() ||
        (position->second.state != GlobalObjectState::Present &&
         position->second.state != GlobalObjectState::Pinned) ||
        position->second.content_digest == content_digest)
        throw std::logic_error("global content conflict lacks an immutable existing object");
    emit({GlobalActionType::CONTENT_CONFLICT_FATAL, c_store_guid, {}, space.generation, key,
          0, position->second.bytes, 0, content_digest});
    throw std::logic_error("same Key64 names different immutable content");
}

void GlobalResourceModel::evict(CStoreGuid c_store_guid) {
    Namespace& victim = require_namespace(c_store_guid);
    if (!victim.live || victim.active) throw std::logic_error("global eviction requires idle namespace");
    for (const auto& [key, object] : victim.objects)
        if (object.state == GlobalObjectState::Installing || object.state == GlobalObjectState::Pinned)
            throw std::logic_error("global eviction crossed a namespace lease");
    if (!faults_.ignore_lru) {
        for (const auto& [guid, candidate] : namespaces_) {
            if (guid == c_store_guid || !candidate->live || candidate->active || candidate->lru >= victim.lru)
                continue;
            bool eligible = true;
            for (const auto& [key, object] : candidate->objects)
                eligible = eligible && object.state != GlobalObjectState::Installing &&
                           object.state != GlobalObjectState::Pinned;
            if (eligible)
                throw std::logic_error("global eviction selected a non-LRU namespace");
        }
    }
    last_eviction_lru_ = victim.lru;
    victim.live = false;
    victim.objects.clear();
    emit({GlobalActionType::NAMESPACE_EVICTED, c_store_guid, {}, victim.generation, {}, 0, 0,
          victim.lru});
}

CStoreGuid GlobalResourceModel::evict_oldest() {
    std::optional<std::pair<CStoreGuid, uint64_t>> oldest;
    for (const auto& [guid, space] : namespaces_) {
        if (!space->live || space->active) continue;
        bool eligible = true;
        for (const auto& [key, object] : space->objects)
            eligible = eligible && object.state != GlobalObjectState::Installing &&
                       object.state != GlobalObjectState::Pinned;
        if (eligible && (!oldest || space->lru < oldest->second)) oldest = {guid, space->lru};
    }
    if (!oldest) throw std::logic_error("no eligible global namespace to evict");
    evict(oldest->first);
    return oldest->first;
}

void GlobalResourceModel::advance_generation(CStoreGuid c_store_guid) {
    Namespace& space = require_namespace(c_store_guid);
    if (space.live || space.stopped || space.generation >= limits_.max_generation)
        throw std::logic_error("global generation cannot advance");
    ++space.generation;
    emit({GlobalActionType::GENERATION_ADVANCED, c_store_guid, {}, space.generation});
}

void GlobalResourceModel::stop_generation_wrap(CStoreGuid c_store_guid) {
    Namespace& space = require_namespace(c_store_guid);
    if (space.live || space.stopped || space.generation != limits_.max_generation)
        throw std::logic_error("global generation wrap stop is not admissible");
    space.stopped = true;
    emit({GlobalActionType::GENERATION_WRAP_STOPPED, c_store_guid, {}, space.generation});
}

void GlobalResourceModel::flip_guid(CStoreGuid old_guid, CStoreGuid new_guid) {
    Namespace& old = require_namespace(old_guid);
    if (!old.stopped || new_guid == CStoreGuid{} || namespaces_.contains(new_guid))
        throw std::logic_error("global GUID flip does not establish a fresh namespace");
    auto replacement = std::make_unique<Namespace>();
    replacement->guid_history = old.guid_history;
    replacement->guid_history.insert(new_guid);
    namespaces_.emplace(new_guid, std::move(replacement));
    emit({GlobalActionType::C_GUID_FLIPPED, new_guid, old_guid, 0});
}

std::optional<std::string> GlobalResourceModel::check_invariants() const {
    const uint64_t resident = resident_bytes();
    const uint64_t staging = staging_bytes();
    if (resident > limits_.max_aggregate_bytes) return "aggregate resident byte cap exceeded";
    if (staging > limits_.max_staging_bytes) return "aggregate staging byte cap exceeded";
    if (!checked_add(resident, staging, limits_.max_total_bytes))
        return "total simultaneous byte cap exceeded";
    std::map<size_t, std::pair<CStoreGuid, Key64>> reverse;
    for (const auto& [guid, space] : namespaces_) {
        uint64_t namespace_bytes = 0;
        for (const auto& [key, object] : space->objects) {
            if (object.state == GlobalObjectState::Present || object.state == GlobalObjectState::Pinned)
                namespace_bytes += object.bytes;
            if (object.state == GlobalObjectState::Installing) {
                if (!reverse.emplace(object.slot, std::pair{guid, key}).second)
                    return "multiple INSTALLING objects own one staging slot";
                const auto owner = slots_.find(object.slot);
                if (owner == slots_.end() || owner->second != std::pair{guid, key})
                    return "INSTALLING object does not own its staging slot";
            }
        }
        if (namespace_bytes > limits_.max_namespace_bytes) return "namespace byte cap exceeded";
    }
    if (reverse.size() != slots_.size()) return "staging slot has no INSTALLING owner";
    for (const auto& [slot, owner] : slots_)
        if (!reverse.contains(slot) || reverse.at(slot) != owner)
            return "staging slot reverse ownership mismatch";
    if (last_eviction_lru_)
        for (const auto& [guid, space] : namespaces_)
            if (space->live && !space->active && space->lru < *last_eviction_lru_)
                return "global eviction violated LRU ordering";
    return std::nullopt;
}

uint64_t GlobalResourceModel::resident_bytes() const {
    uint64_t total = 0;
    for (const auto& [guid, space] : namespaces_)
        for (const auto& [key, object] : space->objects)
            if (object.state == GlobalObjectState::Present || object.state == GlobalObjectState::Pinned)
                total += object.bytes;
    return total;
}

uint64_t GlobalResourceModel::staging_bytes() const {
    uint64_t total = 0;
    for (const auto& [guid, space] : namespaces_)
        for (const auto& [key, object] : space->objects)
            if (object.state == GlobalObjectState::Installing) total += object.bytes;
    return total;
}

size_t GlobalResourceModel::live_namespace_count() const {
    size_t count = 0;
    for (const auto& [guid, space] : namespaces_)
        if (space->live) ++count;
    return count;
}

size_t GlobalResourceModel::free_staging_slots() const {
    return limits_.max_staging_slots - slots_.size();
}

std::optional<size_t> GlobalResourceModel::first_free_staging_slot() const {
    for (size_t slot = 0; slot < limits_.max_staging_slots; ++slot)
        if (!slots_.contains(slot)) return slot;
    return std::nullopt;
}

void GlobalResourceModel::release(CStoreGuid c_store_guid, Key64 key) {
    Namespace& space = require_namespace(c_store_guid);
    const auto position = space.objects.find(key);
    // Zero-byte transactions are legal in the endpoint protocol and have no
    // global resident object to release.
    if (position == space.objects.end()) return;
    if (position->second.state != GlobalObjectState::Present &&
        position->second.state != GlobalObjectState::Pinned)
        throw std::logic_error("global release requires a resident object");
    const uint64_t bytes = position->second.bytes;
    space.objects.erase(position);
    emit({GlobalActionType::ARENA_RELEASED, c_store_guid, {}, space.generation,
          key, 0, bytes});
}

const ImmutableObject& CObjectArena::object(Key64 key) const {
    const ImmutableObject* result = objects_.find(key);
    if (!result) throw std::out_of_range("Key64 is absent from C object arena");
    return *result;
}

CAuthority::CAuthority(CStoreGuid guid, p29::OnlineS1::Config config,
                       uint16_t generation, uint64_t first_ordinal)
    : arena_(guid, generation, first_ordinal), s1_config_(config) {}

TuSeq CAuthority::allocate_tu_seq() {
    if (tu_seq_exhausted_) throw std::overflow_error("TU_SEQ space exhausted");
    const TuSeq result{next_tu_seq_};
    if (next_tu_seq_ == std::numeric_limits<uint64_t>::max())
        tu_seq_exhausted_ = true;
    else
        ++next_tu_seq_;
    return result;
}

uint32_t CAuthority::dense_region(Key64 key) {
    if (key.type() != ObjectType::Region || !arena_.objects().contains(key))
        throw std::invalid_argument("PreparedTU root is not a C Region object");
    const auto found = region_to_dense_.find(key);
    if (found != region_to_dense_.end()) return found->second;
    if (dense_to_region_.size() >= std::numeric_limits<uint32_t>::max())
        throw std::overflow_error("dense Region space exceeds P29 u32");
    const uint32_t id = static_cast<uint32_t>(dense_to_region_.size());
    dense_to_region_.push_back(key);
    region_to_dense_.emplace(key, id);
    return id;
}

PreparedTUPtr CAuthority::prepare_tu(std::span<const uint8_t> exact_input,
                                     std::span<const Key64> regions) {
    const std::vector<uint8_t> materialized = materialize(arena_.objects(), regions);
    if (materialized.size() != exact_input.size() ||
        !std::equal(materialized.begin(), materialized.end(), exact_input.begin()))
        throw std::invalid_argument("PreparedTU Regions do not reproduce exact input");
    std::vector<uint32_t> dense;
    dense.reserve(regions.size());
    for (Key64 key : regions) dense.push_back(dense_region(key));
    auto prepared = std::make_shared<PreparedTU>();
    prepared->tu_seq = allocate_tu_seq();
    prepared->raw_bytes = exact_input.size();
    prepared->raw_digest = digest128(exact_input);
    prepared->regions.assign(regions.begin(), regions.end());
    prepared->dense_regions = std::move(dense);
    return prepared;
}

PreparedTUPtr CAuthority::prepare_from_regions(
    std::span<const std::vector<uint8_t>> region_bytes) {
    std::vector<Key64> regions;
    std::vector<uint8_t> exact;
    regions.reserve(region_bytes.size());
    for (const auto& bytes : region_bytes) {
        const Key64 line = arena_.intern_bytes(ObjectType::Line, bytes);
        const std::array<Key64, 1> children{line};
        regions.push_back(arena_.intern_children(ObjectType::Region, children));
        exact.insert(exact.end(), bytes.begin(), bytes.end());
    }
    return prepare_tu(exact, regions);
}

Key64 CAuthority::dense_region_key(uint32_t id) const { return dense_to_region_.at(id); }
Key64 CAuthority::block_key(uint32_t id) const { return block_keys_.at(id); }

void CAuthority::publish_new_p29_blocks() {
    while (block_keys_.size() < block_catalogue_.size()) {
        const p29::Block& block = block_catalogue_.block(
            static_cast<uint32_t>(block_keys_.size()));
        std::vector<Key64> children;
        children.reserve(block.regions.size());
        for (uint32_t id : block.regions) children.push_back(dense_region_key(id));
        block_keys_.push_back(arena_.intern_children(ObjectType::Block, children));
    }
}

std::vector<Key64> CAuthority::transitive_manifest(
    std::span<const Key64> roots) const {
    std::set<Key64> seen;
    std::function<void(Key64)> visit = [&](Key64 key) {
        if (!seen.insert(key).second) return;
        const ImmutableObject& object = arena_.object(key);
        if (const auto* children = std::get_if<ChildrenPayload>(&object.payload))
            for (Key64 child : children->children) visit(child);
    };
    for (Key64 root : roots) visit(root);
    return {seen.begin(), seen.end()};
}

std::vector<uint8_t> CAuthority::materialize(
    const ImmutableObjectStore& objects, std::span<const Key64> roots) const {
    return materialize_objects(objects, roots);
}

CRoute::CRoute(CAuthority& authority, FStoreGuid f_store_guid,
               HistoryNonce history_nonce, ActionTrace* trace)
    : authority_(authority), f_store_guid_(f_store_guid),
      history_nonce_(history_nonce),
      state_digest_(initial_route_digest(authority.guid(), history_nonce)),
      matcher_(std::make_unique<p29::OnlineS1>(authority.s1_config(),
                                               authority.block_catalogue())),
      trace_(trace) {}

CRoute::~CRoute() = default;

std::vector<uint8_t> CRoute::residual_input(const PreparedTUPtr& prepared) const {
    if (!prepared) throw std::invalid_argument("cannot inspect a null PreparedTU");
    std::vector<uint8_t> result;
    append_unacknowledged_line_payload(authority_.arena(), prepared->regions,
                                       acknowledged_objects_, result);
    return result;
}

const CActiveTx& CRoute::begin(const PreparedTUPtr& prepared,
                              P29RootMode root_mode,
                              std::span<const uint8_t> residual,
                              bool residual_body) {
    if (!prepared) throw std::invalid_argument("cannot route a null PreparedTU");
    if (active_) throw std::logic_error("C route already has one ACTIVE_TX");
    if (next_rel_seq_.value == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("REL_SEQ space exhausted");
    try {
        const p29::TuPlan& plan = matcher_->prepare(prepared->dense_regions);
        authority_.publish_new_p29_blocks();
        CActiveTx active;
        active.prepared = prepared;
        if (root_mode == P29RootMode::RouteHistory) {
            active.root.reserve(plan.root.size());
            for (const p29::Ref& ref : plan.root)
                active.root.push_back(ref.kind == p29::RefKind::Region
                                          ? authority_.dense_region_key(ref.id)
                                          : authority_.block_key(ref.id));
        } else if (root_mode == P29RootMode::HistoryIndependent) {
            active.root = prepared->regions;
        } else {
            throw std::invalid_argument("unsupported P29 root mode");
        }
        active.manifest = authority_.transitive_manifest(active.root);
        active.dict = encode_key_vector(active.manifest);
        const std::vector<uint8_t> root_bytes = encode_key_vector(active.root);
        active.body = residual_body
                          ? encode_p29_residual_body(root_bytes, residual)
                          : root_bytes;
        active.region_count = prepared->regions.size();
        active.block_use_count = plan.block_uses.size();
        active.new_block_count = plan.new_blocks.size();
        active.begin.history_nonce = history_nonce_;
        active.begin.rel_seq = next_rel_seq_;
        active.begin.tu_seq = prepared->tu_seq;
        active.begin.profile = ProfileId::P29;
        active.begin.p29_root_mode = root_mode;
        active.begin.pre_state_digest = state_digest_;
        active.begin.dict = describe_component(kP29KeyVectorEncoding, active.dict,
                                               active.manifest.size());
        active.begin.body = describe_component(
            residual_body ? kP29ResidualBodyEncoding : kP29KeyVectorEncoding,
            active.body, active.root.size());
        active.begin.raw_bytes = prepared->raw_bytes;
        active.begin.raw_digest = prepared->raw_digest;
        active.begin.transaction_digest = compute_transaction_digest(
            active.begin, active.dict, active.body);
        active_ = std::move(active);
        record(ActionType::TX_BEGIN, *active_);
    } catch (...) {
        if (matcher_->has_pending()) matcher_->abort();
        active_.reset();
        throw;
    }
    return *active_;
}

std::vector<ImmutableObject> CRoute::build_fill(const Need& need) const {
    if (!active_) throw std::logic_error("C route has no ACTIVE_TX");
    if (need.history_nonce != active_->begin.history_nonce ||
        need.rel_seq != active_->begin.rel_seq || need.tu_seq != active_->begin.tu_seq ||
        need.transaction_digest != active_->begin.transaction_digest)
        throw std::logic_error("Need does not identify C's ACTIVE_TX");
    if (!std::is_sorted(need.missing.begin(), need.missing.end()) ||
        std::adjacent_find(need.missing.begin(), need.missing.end()) != need.missing.end())
        throw std::logic_error("Need is not an exact sorted set");
    std::vector<ImmutableObject> result;
    result.reserve(need.missing.size());
    for (Key64 key : need.missing) {
        if (!std::binary_search(active_->manifest.begin(), active_->manifest.end(), key))
            throw std::logic_error("Need requests a key outside the manifest");
        result.push_back(authority_.arena().object(key));
    }
    return result;
}

void CRoute::record(ActionType action, const CActiveTx& active) {
    if (!trace_) return;
    ActionRecord record;
    record.action = action;
    record.actor = ActorSide::C;
    record.c_store_guid = authority_.guid();
    record.f_store_guid = f_store_guid_;
    record.history_nonce = active.begin.history_nonce;
    record.rel_seq = active.begin.rel_seq;
    record.tu_seq = active.begin.tu_seq;
    record.transaction_digest = active.begin.transaction_digest;
    record.raw_digest = active.begin.raw_digest;
    record.state_digest =
        action == ActionType::COMMIT_ACCEPTED ||
                action == ActionType::LOST_COMMIT_ACCEPTED
            ? compute_post_state_digest(active.begin.pre_state_digest,
                                        active.begin.history_nonce,
                                        active.begin.rel_seq,
                                        active.begin.tu_seq,
                                        active.begin.transaction_digest)
            : active.begin.pre_state_digest;
    trace_->record(std::move(record));
}

void CRoute::accept_commit(const TxCommit& committed, ActionType action) {
    if (!active_) throw std::logic_error("C route has no ACTIVE_TX to commit");
    if (action != ActionType::COMMIT_ACCEPTED &&
        action != ActionType::LOST_COMMIT_ACCEPTED)
        throw std::invalid_argument("invalid commit action");
    if (!same_commit(committed, *active_))
        throw std::logic_error("TX_COMMIT does not close C's ACTIVE_TX");
    acknowledged_objects_.insert(active_->manifest.begin(), active_->manifest.end());
    matcher_->commit();
    state_digest_ = committed.post_state_digest;
    record(action, *active_);
    ++next_rel_seq_.value;
    active_.reset();
}

void CRoute::abandon_active() {
    if (!active_) return;
    record(ActionType::TX_ABORTED, *active_);
    matcher_->abort();
    active_.reset();
}

void CRoute::reset_history(FStoreGuid f_store_guid, HistoryNonce history_nonce) {
    if (history_nonce == history_nonce_)
        throw std::invalid_argument("history reset requires a fresh HISTORY_NONCE");
    abandon_active();
    f_store_guid_ = f_store_guid;
    history_nonce_ = history_nonce;
    next_rel_seq_ = RelSeq{0};
    state_digest_ = initial_route_digest(authority_.guid(), history_nonce_);
    acknowledged_objects_.clear();
    matcher_ = std::make_unique<p29::OnlineS1>(authority_.s1_config(),
                                               authority_.block_catalogue());
}

struct FStore::Namespace {
    struct FPending {
        explicit FPending(TxBegin value) : begin(std::move(value)) {}
        TxBegin begin;
        std::vector<uint8_t> dict;
        std::vector<uint8_t> body;
        std::vector<uint8_t> residual;
        bool dict_complete = false;
        bool body_complete = false;
        std::vector<Key64> manifest;
        std::vector<Key64> root;
        std::set<Key64> acknowledged_before;
        std::set<Key64> requested;
        std::set<Key64> remaining;
        FillStreamDecoder partial_fill;
        std::optional<std::vector<uint8_t>> materialized;
    };

    struct Route {
        HistoryNonce history_nonce{};
        RelSeq next_rel_seq{};
        Digest128 state_digest{};
        std::optional<TxCommit> last_commit;
        std::optional<FPending> pending;
    };

    bool established = false;
    uint64_t active_session_serial = 0;
    ImmutableObjectStore objects;
    std::optional<Route> route;
};

FStore::FStore(FStoreGuid guid, uint64_t first_session_serial, ActionTrace* trace)
    : guid_(guid), next_session_serial_(first_session_serial), trace_(trace) {
    if (first_session_serial == 0)
        throw std::invalid_argument("first session serial must be nonzero");
}

FStore::~FStore() = default;

void FStore::record(ActionType action, SessionHandle session, const TxBegin* begin,
                    std::optional<Key64> key, Digest128 content_digest,
                    uint64_t remaining_need,
                    bool duplicate, std::span<const Key64> need_keys) {
    if (!trace_) return;
    ActionRecord record;
    record.action = action;
    record.actor = ActorSide::F;
    record.c_store_guid = session.c_store_guid;
    record.f_store_guid = guid_;
    record.session_serial = session.serial;
    record.key = key;
    record.content_digest = content_digest;
    record.need_keys.assign(need_keys.begin(), need_keys.end());
    record.remaining_need = remaining_need;
    record.duplicate = duplicate;
    const auto position = namespaces_.find(session.c_store_guid);
    if (position != namespaces_.end() && position->second->route) {
        record.history_nonce = position->second->route->history_nonce;
        record.rel_seq = position->second->route->next_rel_seq;
        record.state_digest = position->second->route->state_digest;
    }
    if (begin) {
        record.history_nonce = begin->history_nonce;
        record.rel_seq = begin->rel_seq;
        record.tu_seq = begin->tu_seq;
        record.transaction_digest = begin->transaction_digest;
        record.raw_digest = begin->raw_digest;
        if (action != ActionType::INPUT_COMMITTED)
            record.state_digest = begin->pre_state_digest;
    }
    trace_->record(std::move(record));
}

SessionHandle FStore::connect(CStoreGuid c_store_guid) {
    if (session_serial_exhausted_)
        throw std::overflow_error("session serial space exhausted until F store reset");
    const uint64_t serial = next_session_serial_;
    if (serial == std::numeric_limits<uint64_t>::max())
        session_serial_exhausted_ = true;
    else
        ++next_session_serial_;
    auto [position, inserted] = namespaces_.try_emplace(c_store_guid);
    if (inserted) position->second = std::make_unique<Namespace>();
    Namespace& space = *position->second;
    const bool replaces = space.active_session_serial != 0;
    if (space.route) space.route->pending.reset();
    space.active_session_serial = serial;
    const SessionHandle session{c_store_guid, guid_, serial};
    record(replaces ? ActionType::SESSION_REPLACED : ActionType::SESSION_OPENED,
           session, nullptr);
    return session;
}

void FStore::disconnect(SessionHandle session) {
    Namespace& space = require_namespace(session);
    if (space.route) space.route->pending.reset();
    record(ActionType::SESSION_DISCONNECTED, session, nullptr);
    space.active_session_serial = 0;
}

SessionState FStore::resume(SessionHandle session) const {
    const Namespace& space = require_namespace(session);
    SessionState result;
    result.f_store_guid = guid_;
    result.namespace_present = space.established;
    result.route_present = space.route.has_value();
    if (space.route) {
        result.history_nonce = space.route->history_nonce;
        result.next_rel_seq = space.route->next_rel_seq;
        result.state_digest = space.route->state_digest;
        result.last_commit = space.route->last_commit;
    }
    return result;
}

void FStore::start_route(SessionHandle session, HistoryNonce history_nonce,
                         Digest128 initial_state_digest) {
    Namespace& space = require_namespace(session);
    if (space.route) throw std::logic_error("F route already exists");
    if (initial_state_digest !=
        initial_route_digest(session.c_store_guid, history_nonce))
        throw std::logic_error("HISTORY_RESET initial digest was not derived from its route");
    space.established = true;
    space.route = Namespace::Route{};
    space.route->history_nonce = history_nonce;
    space.route->state_digest = initial_state_digest;
    record(ActionType::HISTORY_RESET, session, nullptr);
}

void FStore::begin(SessionHandle session, const TxBegin& begin, bool replay) {
    Namespace& space = require_namespace(session);
    if (!space.established || !space.route)
        throw std::logic_error("F route has not been established");
    Namespace::Route& route = *space.route;
    if (begin.history_nonce != route.history_nonce || begin.rel_seq != route.next_rel_seq ||
        begin.pre_state_digest != route.state_digest)
        throw std::logic_error("TX_BEGIN does not match F's route cursor");
    if (begin.profile != ProfileId::P29 ||
        (begin.p29_root_mode != P29RootMode::RouteHistory &&
         begin.p29_root_mode != P29RootMode::HistoryIndependent))
        throw std::invalid_argument("TX_BEGIN profile/root mode is unsupported in M1");
    if (begin.dict.encoding != kP29KeyVectorEncoding ||
        (begin.body.encoding != kP29KeyVectorEncoding &&
         begin.body.encoding != kP29ResidualBodyEncoding))
        throw std::invalid_argument("P29 DICT/BODY encoding is unsupported");
    if (route.next_rel_seq.value == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("F REL_SEQ space exhausted");
    if (route.pending) {
        if (!same_begin(route.pending->begin, begin))
            throw std::logic_error("F route already has a different ACTIVE_TX");
        return;
    }
    route.pending.emplace(begin);
    record(replay ? ActionType::ACTIVE_REPLAYED : ActionType::TX_BEGIN,
           session, &begin);
    if (begin.dict.encoded_bytes == 0)
        append_component(session, true, std::span<const uint8_t>{});
    if (begin.body.encoded_bytes == 0)
        append_component(session, false, std::span<const uint8_t>{});
}

void FStore::append_component(SessionHandle session, bool dict,
                              std::span<const uint8_t> bytes) {
    Namespace& space = require_namespace(session);
    if (!space.route || !space.route->pending)
        throw std::logic_error("component data has no F ACTIVE_TX");
    Namespace::FPending& pending = *space.route->pending;
    std::vector<uint8_t>& target = dict ? pending.dict : pending.body;
    bool& complete = dict ? pending.dict_complete : pending.body_complete;
    const ComponentDescriptor& descriptor = dict ? pending.begin.dict : pending.begin.body;
    if (complete) {
        if (!bytes.empty()) throw std::logic_error("component received bytes after completion");
        return;
    }
    if (bytes.size() > descriptor.encoded_bytes - target.size())
        throw std::length_error("component exceeds its declared byte count");
    target.insert(target.end(), bytes.begin(), bytes.end());
    if (target.size() != descriptor.encoded_bytes) return;
    if (digest128(target) != descriptor.digest)
        throw std::logic_error("component digest does not match TX_BEGIN");
    complete = true;
    if (dict) {
        pending.manifest = decode_key_vector(target);
        if (pending.manifest.size() != descriptor.decoded_bytes)
            throw std::logic_error("DICT decoded count does not match its descriptor");
        if (!std::is_sorted(pending.manifest.begin(), pending.manifest.end()) ||
            std::adjacent_find(pending.manifest.begin(), pending.manifest.end()) !=
                pending.manifest.end())
            throw std::logic_error("DICT manifest is not an exact sorted set");
        for (Key64 key : pending.manifest)
            if (space.objects.contains(key))
                pending.acknowledged_before.insert(key);
            else
                pending.requested.insert(key);
        pending.remaining = pending.requested;
        record(ActionType::DICT_COMPLETE, session, &pending.begin);
        const std::vector<Key64> exact_need(pending.requested.begin(),
                                            pending.requested.end());
        record(ActionType::NEED_RECORDED, session, &pending.begin, std::nullopt,
               {}, pending.remaining.size(), false, exact_need);
    } else {
        std::vector<uint8_t> root_bytes;
        if (descriptor.encoding == kP29ResidualBodyEncoding) {
            const P29ResidualBody composite = decode_p29_residual_body(target);
            root_bytes = composite.root;
            pending.residual = composite.residual;
        } else {
            root_bytes = target;
        }
        pending.root = decode_key_vector(root_bytes);
        if (pending.root.size() != descriptor.decoded_bytes)
            throw std::logic_error("BODY decoded count does not match its descriptor");
        record(ActionType::BODY_COMPLETE, session, &pending.begin);
    }
    if (pending.dict_complete && pending.body_complete) {
        if (compute_transaction_digest(pending.begin, pending.dict, pending.body) !=
            pending.begin.transaction_digest)
            throw std::logic_error("transaction digest does not match its exact components");
        for (Key64 root : pending.root)
            if (!std::binary_search(pending.manifest.begin(), pending.manifest.end(), root))
                throw std::logic_error("BODY root is outside the DICT manifest");
    }
}

void FStore::append_dict(SessionHandle session, std::span<const uint8_t> bytes) {
    append_component(session, true, bytes);
}

void FStore::append_body(SessionHandle session, std::span<const uint8_t> bytes) {
    append_component(session, false, bytes);
}

Need FStore::need(SessionHandle session) const {
    const Namespace& space = require_namespace(session);
    if (!space.route || !space.route->pending || !space.route->pending->dict_complete)
        throw std::logic_error("Need is unavailable before the exact DICT");
    const Namespace::FPending& pending = *space.route->pending;
    return {pending.begin.history_nonce, pending.begin.rel_seq, pending.begin.tu_seq,
            pending.begin.transaction_digest,
            {pending.requested.begin(), pending.requested.end()}};
}

ObjectApplied FStore::apply_object(SessionHandle session,
                                   const ImmutableObject& object) {
    Namespace& space = require_namespace(session);
    if (!space.route || !space.route->pending ||
        !space.route->pending->dict_complete)
        throw std::logic_error("object application has no exact active Need");
    Namespace::FPending& pending = *space.route->pending;
    if (!pending.requested.contains(object.key))
        throw std::logic_error("object was not in F's recorded Need set");
    const bool still_missing = pending.remaining.contains(object.key);
    const ObjectApplyResult result = space.objects.apply(object);
    if (still_missing) pending.remaining.erase(object.key);
    if (!still_missing && result != ObjectApplyResult::Duplicate)
        throw std::logic_error("closed Need key was unexpectedly absent");
    record(ActionType::OBJECT_APPLIED, session, &pending.begin, object.key,
           object.content_digest, pending.remaining.size(),
           result == ObjectApplyResult::Duplicate);
    return {object.key, object.content_digest, result};
}

std::vector<ObjectApplied> FStore::append_fill(SessionHandle session,
                                               const FillMessage& message) {
    Namespace& space = require_namespace(session);
    if (!space.route || !space.route->pending)
        throw std::logic_error("FILL has no F ACTIVE_TX");
    Namespace::FPending& pending = *space.route->pending;
    const std::vector<FillRecord> records =
        pending.partial_fill.push(message);
    std::vector<ObjectApplied> result;
    result.reserve(records.size());
    for (const FillRecord& record : records)
        result.push_back(apply_object(session, ImmutableObject::from_record(record)));
    if (pending.remaining.empty()) pending.partial_fill.finish();
    return result;
}

void FStore::finish_fill(SessionHandle session) const {
    const Namespace& space = require_namespace(session);
    if (!space.route || !space.route->pending)
        throw std::logic_error("FILL has no F ACTIVE_TX");
    space.route->pending->partial_fill.finish();
}

std::vector<uint8_t> FStore::materialize_and_verify(SessionHandle session) {
    Namespace& space = require_namespace(session);
    if (!space.route || !space.route->pending)
        throw std::logic_error("F route has no ACTIVE_TX to materialize");
    Namespace::FPending& pending = *space.route->pending;
    pending.partial_fill.finish();
    if (!pending.dict_complete || !pending.body_complete || !pending.remaining.empty())
        throw std::logic_error("input cannot materialize before DICT, BODY, and Need finish");
    if (pending.materialized)
        throw std::logic_error("input was already materialized for this transaction");
    if (compute_transaction_digest(pending.begin, pending.dict, pending.body) !=
        pending.begin.transaction_digest)
        throw std::logic_error("transaction digest changed before materialization");
    std::set<Key64> reached;
    const std::vector<uint8_t> object_result =
        materialize_objects(space.objects, pending.root, &reached);
    if (!std::equal(reached.begin(), reached.end(), pending.manifest.begin(),
                    pending.manifest.end()))
        throw std::logic_error("DICT is not the exact transitive object closure");
    std::vector<uint8_t> result = object_result;
    if (pending.begin.body.encoding == kP29ResidualBodyEncoding) {
        if (!pending.residual.empty()) {
            residual_group::Codec codec;
            const residual_group::DecodedFrame frame =
                codec.decode(pending.residual.data(), pending.residual.size());
            if (frame.wire_bytes != pending.residual.size())
                throw std::logic_error("P29 residual frame has trailing bytes");
            const std::vector<uint8_t> expected_residual = residual_for_root(
                space.objects, pending.root, pending.acknowledged_before);
            if (frame.raw != expected_residual)
                throw std::logic_error("P29 residual differs from Root/Block line payload");
            result = reconstruct_root_with_residual(
                space.objects, pending.root, pending.acknowledged_before, frame.raw);
        }
        if (pending.residual.empty()) {
            const std::vector<uint8_t> expected_residual = residual_for_root(
                space.objects, pending.root, pending.acknowledged_before);
            if (!expected_residual.empty())
                throw std::logic_error("P29 nonempty line payload has no residual frame");
            result = reconstruct_root_with_residual(
                space.objects, pending.root, pending.acknowledged_before, {});
        }
    }
    if (result.size() != pending.begin.raw_bytes ||
        digest128(result) != pending.begin.raw_digest)
        throw std::logic_error("materialized input does not match TX_BEGIN exactly");
    pending.materialized = result;
    record(ActionType::INPUT_MATERIALIZED, session, &pending.begin);
    return result;
}

TxCommit FStore::commit_input(SessionHandle session) {
    Namespace& space = require_namespace(session);
    if (!space.route || !space.route->pending ||
        !space.route->pending->materialized)
        throw std::logic_error("exact input has not materialized");
    Namespace::Route& route = *space.route;
    const TxBegin begin = route.pending->begin;
    if (route.next_rel_seq.value == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("F REL_SEQ space exhausted");
    TxCommit commit{begin.history_nonce, begin.rel_seq, begin.tu_seq,
                    begin.transaction_digest, begin.raw_digest,
                    compute_post_state_digest(begin.pre_state_digest,
                                              begin.history_nonce, begin.rel_seq,
                                              begin.tu_seq,
                                              begin.transaction_digest)};
    route.state_digest = commit.post_state_digest;
    ++route.next_rel_seq.value;
    route.last_commit = commit;
    record(ActionType::INPUT_COMMITTED, session, &begin);
    route.pending.reset();
    return commit;
}

void FStore::forget_route(SessionHandle session) {
    Namespace& space = require_namespace(session);
    if (space.route && space.route->pending)
        throw std::logic_error("HISTORY_RESET is only legal outside ACTIVE_TX");
    space.route.reset();
    space.established = true;
}

void FStore::destructive_cache_reset(FStoreGuid new_guid) {
    if (new_guid == guid_)
        throw std::invalid_argument("F store reset requires a new F_STORE_GUID");
    namespaces_.clear();
    guid_ = new_guid;
    next_session_serial_ = 1;
    session_serial_exhausted_ = false;
}

size_t FStore::object_count(CStoreGuid c_store_guid) const {
    const auto position = namespaces_.find(c_store_guid);
    return position == namespaces_.end() ? 0 : position->second->objects.size();
}

bool FStore::contains(CStoreGuid c_store_guid, Key64 key) const {
    const auto position = namespaces_.find(c_store_guid);
    return position != namespaces_.end() && position->second->objects.contains(key);
}

FStore::Namespace& FStore::require_namespace(SessionHandle session) {
    const auto position = namespaces_.find(session.c_store_guid);
    if (session.f_store_guid != guid_ || position == namespaces_.end() ||
        session.serial == 0 ||
        position->second->active_session_serial != session.serial)
        throw std::logic_error("stale or unknown F session");
    return *position->second;
}

const FStore::Namespace& FStore::require_namespace(SessionHandle session) const {
    const auto position = namespaces_.find(session.c_store_guid);
    if (session.f_store_guid != guid_ || position == namespaces_.end() ||
        session.serial == 0 ||
        position->second->active_session_serial != session.serial)
        throw std::logic_error("stale or unknown F session");
    return *position->second;
}

ReconnectResult reconnect(CRoute& c_route, FStore& f_store,
                          HistoryNonce fresh_history_nonce) {
    ReconnectResult result;
    result.session = f_store.connect(c_route.c_store_guid());
    const SessionState state = f_store.resume(result.session);
    if (state.f_store_guid != c_route.f_store_guid() || !state.namespace_present) {
        const PreparedTUPtr retry = c_route.active_ ? c_route.active_->prepared : nullptr;
        c_route.reset_history(state.f_store_guid, fresh_history_nonce);
        if (state.route_present) f_store.forget_route(result.session);
        f_store.start_route(result.session, c_route.history_nonce(),
                            c_route.state_digest());
        if (retry) c_route.begin(retry, P29RootMode::HistoryIndependent);
        result.outcome = ReconnectOutcome::ColdFStore;
        result.replay_active = c_route.active().has_value();
        return result;
    }
    if (state.route_present && state.history_nonce == c_route.history_nonce() &&
        state.next_rel_seq == c_route.next_rel_seq() &&
        state.state_digest == c_route.state_digest()) {
        result.outcome = ReconnectOutcome::ExactMatch;
        result.replay_active = c_route.active().has_value();
        return result;
    }
    if (state.route_present && c_route.active() && state.last_commit &&
        state.history_nonce == c_route.history_nonce() &&
        state.next_rel_seq.value == c_route.active()->begin.rel_seq.value + 1 &&
        state.state_digest == state.last_commit->post_state_digest &&
        same_commit(*state.last_commit, *c_route.active())) {
        c_route.accept_commit(*state.last_commit, ActionType::LOST_COMMIT_ACCEPTED);
        result.outcome = ReconnectOutcome::LostFinalAcknowledgement;
        return result;
    }
    const PreparedTUPtr retry = c_route.active_ ? c_route.active_->prepared : nullptr;
    c_route.reset_history(state.f_store_guid, fresh_history_nonce);
    if (state.route_present) f_store.forget_route(result.session);
    f_store.start_route(result.session, c_route.history_nonce(),
                        c_route.state_digest());
    if (retry) c_route.begin(retry, P29RootMode::HistoryIndependent);
    result.outcome = ReconnectOutcome::RouteHistoryReset;
    result.replay_active = c_route.active().has_value();
    return result;
}

}  // namespace icecc::p50
