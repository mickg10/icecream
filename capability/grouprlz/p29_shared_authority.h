// One logical shared-C authority for many transient C producers and independent F replicas.
//
// Publication is batched at TU-distinct atom/Region granularity; occurrence streams stay local
// to the producer.  One authority mutex establishes the first exact reference order.  The
// implementation can later shard the tables internally without changing these identities.
#ifndef P29_SHARED_AUTHORITY_H
#define P29_SHARED_AUTHORITY_H

#include "p29_online_s1.h"
#include "p29_transfer_transaction.h"

#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace p29 {

struct Opaque128Hash {
    size_t operator()(const Opaque128& value) const {
        uint64_t lo = 1469598103934665603ULL;
        uint64_t hi = 0x9e3779b97f4a7c15ULL;
        for (uint8_t byte : value) {
            lo = (lo ^ byte) * 1099511628211ULL;
            hi ^= uint64_t(byte) + 0x9e3779b97f4a7c15ULL + (hi << 6) + (hi >> 2);
        }
        return static_cast<size_t>(lo ^ hi);
    }
};

struct U32VectorHash {
    size_t operator()(const std::vector<uint32_t>& values) const {
        uint64_t hash = 1469598103934665603ULL ^ uint64_t(values.size());
        for (uint32_t value : values) {
            hash ^= value;
            hash *= 1099511628211ULL;
        }
        return static_cast<size_t>(hash);
    }
};

template <typename Value, typename Hash = std::hash<Value>>
class CanonicalTable {
public:
    std::pair<uint32_t, bool> intern(const Value& value) {
        const auto found = ids_.find(value);
        if (found != ids_.end()) return {found->second, false};
        if (values_.size() == std::numeric_limits<uint32_t>::max())
            throw std::overflow_error("canonical object space exceeds u32");
        const uint32_t id = static_cast<uint32_t>(values_.size());
        values_.push_back(value);
        try {
            const auto inserted = ids_.emplace(values_.back(), id);
            if (!inserted.second) {
                values_.pop_back();
                return {inserted.first->second, false};
            }
        } catch (...) {
            values_.pop_back();
            throw;
        }
        return {id, true};
    }

    const Value& at(uint32_t id) const { return values_.at(id); }
    size_t size() const { return values_.size(); }

private:
    std::vector<Value> values_;
    std::unordered_map<Value, uint32_t, Hash> ids_;
};

enum class ObjectKind : uint8_t { Atom = 1, Region = 2, Block = 3, Material = 4 };

struct TypedObjectKey {
    Opaque128 source_generation{};
    ObjectKind kind = ObjectKind::Atom;
    uint32_t ordinal = 0;

    bool operator==(const TypedObjectKey& other) const {
        return source_generation == other.source_generation && kind == other.kind &&
               ordinal == other.ordinal;
    }
};

struct TypedObjectKeyHash {
    size_t operator()(const TypedObjectKey& key) const {
        uint64_t hash = Opaque128Hash{}(key.source_generation);
        hash ^= uint64_t(static_cast<uint8_t>(key.kind)) * 0x9e3779b97f4a7c15ULL;
        hash ^= uint64_t(key.ordinal) * 0xbf58476d1ce4e5b9ULL;
        return static_cast<size_t>(hash ^ (hash >> 32));
    }
};

struct FCacheIdentity {
    Opaque128 f_id{};
    uint64_t cache_epoch = 0;

    bool operator==(const FCacheIdentity& other) const {
        return f_id == other.f_id && cache_epoch == other.cache_epoch;
    }
};

struct FCacheIdentityHash {
    size_t operator()(const FCacheIdentity& key) const {
        const uint64_t hash = Opaque128Hash{}(key.f_id);
        return static_cast<size_t>(hash ^ (key.cache_epoch * 0x9e3779b97f4a7c15ULL));
    }
};

struct RouteLaneIdentity {
    FCacheIdentity f{};
    uint64_t lane_id = 0;

    bool operator==(const RouteLaneIdentity& other) const {
        return f == other.f && lane_id == other.lane_id;
    }
    bool operator!=(const RouteLaneIdentity& other) const { return !(*this == other); }
};

struct RouteLaneIdentityHash {
    size_t operator()(const RouteLaneIdentity& key) const {
        const uint64_t hash = FCacheIdentityHash{}(key.f);
        return static_cast<size_t>(hash ^
                                   (key.lane_id * 0xbf58476d1ce4e5b9ULL));
    }
};

class PerFRelationship {
public:
    enum class ReserveResult { Owner, Joined, Known };
    enum class CommitResult { Committed, AlreadyKnown, NotOwner };

    struct Reservation {
        ReserveResult result = ReserveResult::Known;
        uint64_t token = 0;
    };

    struct InstallClaim {
        TypedObjectKey key{};
        uint64_t token = 0;
    };

    struct StateMark {
        uint64_t residency_sequence = 0;
        size_t known_objects = 0;
        size_t pending_installs = 0;
        uint64_t mirror_sequence = 0;  // one step per ACKed TU closure, including no-install
        bool operator==(const StateMark& other) const {
            return residency_sequence == other.residency_sequence &&
                   known_objects == other.known_objects &&
                   pending_installs == other.pending_installs &&
                   mirror_sequence == other.mirror_sequence;
        }
    };

    explicit PerFRelationship(FCacheIdentity identity) : identity_(std::move(identity)) {}

    Reservation reserve_install(const TypedObjectKey& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (known_.find(key) != known_.end()) return {ReserveResult::Known, 0};
        const auto pending = pending_.find(key);
        if (pending != pending_.end()) return {ReserveResult::Joined, pending->second};
        if (next_token_ == std::numeric_limits<uint64_t>::max())
            throw std::overflow_error("per-F install token space exhausted");
        const uint64_t token = next_token_++;
        pending_.emplace(key, token);
        return {ReserveResult::Owner, token};
    }

    CommitResult commit_install(const TypedObjectKey& key, uint64_t token) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto pending = pending_.find(key);
        if (pending == pending_.end())
            return known_.find(key) != known_.end() ? CommitResult::AlreadyKnown
                                                    : CommitResult::NotOwner;
        if (pending->second != token) return CommitResult::NotOwner;
        if (residency_sequence_ == std::numeric_limits<uint64_t>::max())
            throw std::overflow_error("per-F residency sequence exhausted");
        known_.insert(key);
        pending_.erase(pending);
        ++residency_sequence_;
        return CommitResult::Committed;
    }

    // Commit the object closure of one ACKed TU as one C-mirror transition.  Every claim is
    // validated, duplicate keys are refused, and hash-table runway is allocated before the
    // first visible mutation.  Once insertion starts, allocation failure follows the same
    // process-fatal policy as the canonical matcher; a partly advanced mirror is never reused.
    bool commit_installs(const std::vector<InstallClaim>& claims) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (claims.size() > std::numeric_limits<uint64_t>::max() - residency_sequence_ ||
            mirror_sequence_ == std::numeric_limits<uint64_t>::max())
            throw std::overflow_error("per-F relationship sequence exhausted");
        std::unordered_set<TypedObjectKey, TypedObjectKeyHash> distinct;
        distinct.reserve(claims.size());
        for (const InstallClaim& claim : claims) {
            if (!distinct.insert(claim.key).second) return false;
            const auto pending = pending_.find(claim.key);
            if (pending == pending_.end() || pending->second != claim.token ||
                known_.find(claim.key) != known_.end())
                return false;
        }
        known_.reserve(known_.size() + claims.size());
        bool mutation_started = false;
        try {
            for (const InstallClaim& claim : claims) {
                mutation_started = true;
                known_.insert(claim.key);
                pending_.erase(claim.key);
                ++residency_sequence_;
            }
        } catch (...) {
            if (mutation_started) std::terminate();
            throw;
        }
        ++mirror_sequence_;
        return true;
    }

    bool abort_install(const TypedObjectKey& key, uint64_t token) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto pending = pending_.find(key);
        if (pending == pending_.end() || pending->second != token) return false;
        pending_.erase(pending);
        return true;
    }

    bool known(const TypedObjectKey& key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return known_.find(key) != known_.end();
    }

    StateMark state_mark() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {residency_sequence_, known_.size(), pending_.size(), mirror_sequence_};
    }

    const FCacheIdentity& identity() const { return identity_; }

private:
    FCacheIdentity identity_{};
    mutable std::mutex mutex_;
    uint64_t next_token_ = 1;
    uint64_t residency_sequence_ = 0;
    uint64_t mirror_sequence_ = 0;
    std::unordered_set<TypedObjectKey, TypedObjectKeyHash> known_;
    std::unordered_map<TypedObjectKey, uint64_t, TypedObjectKeyHash> pending_;
};

class SharedCAuthority {
public:
    struct PublishedBatch {
        std::vector<uint32_t> ids;
        size_t created = 0;
    };

    struct AdmissionInput {
        Opaque128 producer_request_id{};
        uint64_t source_extent = 0;
        Digest128 output_digest{};
        std::vector<uint32_t> regions;
    };

    struct AdmittedTu {
        Opaque128 producer_request_id{};
        uint64_t canonical_admission_sequence = 0;
        uint64_t shared_c_snapshot_version = 0;
        uint64_t source_extent = 0;
        Digest128 output_digest{};
        std::vector<uint32_t> regions;
        TuPlan global_plan;
    };

    enum class AdmissionResult { Created, Existing, RequestMismatch };

    struct AdmissionReply {
        AdmissionResult result = AdmissionResult::RequestMismatch;
        std::shared_ptr<const AdmittedTu> admitted;
    };

    // A route plan is a ticket, not merely a TuPlan.  The internal token prevents an old
    // caller from committing a later prepare that happens to reuse the same route sequence
    // and AdmittedTu after an abort.  The token is authority-local and never appears on the
    // wire; route_sequence is the durable per-lane clock.
    struct RoutePreparation {
        RouteLaneIdentity lane{};
        uint64_t route_sequence = 0;
        uint64_t prepare_token = 0;
        uint64_t canonical_admission_sequence = 0;
        TuPlan plan;
    };

    struct RouteHistoryMark {
        uint64_t committed_sequence = 0;
        uint64_t occurrence_count = 0;
        bool pending = false;
        uint64_t pending_prepare_token = 0;
        uint64_t pending_admission_sequence = 0;

        bool operator==(const RouteHistoryMark& other) const {
            return committed_sequence == other.committed_sequence &&
                   occurrence_count == other.occurrence_count && pending == other.pending &&
                   pending_prepare_token == other.pending_prepare_token &&
                   pending_admission_sequence == other.pending_admission_sequence;
        }
    };

    SharedCAuthority(Opaque128 c_guid, Opaque128 source_generation,
                     OnlineS1::Config config = OnlineS1::Config{})
        : c_guid_(std::move(c_guid)), source_generation_(std::move(source_generation)),
          global_(config, blocks_), route_config_(config) {}

    PublishedBatch publish_atoms(const std::vector<std::string>& distinct_atoms) {
        std::lock_guard<std::mutex> lock(mutex_);
        require_snapshot_runway(distinct_atoms.size());
        PublishedBatch batch;
        batch.ids.reserve(distinct_atoms.size());
        for (const std::string& atom : distinct_atoms) {
            const auto result = atoms_.intern(atom);
            batch.ids.push_back(result.first);
            if (result.second) {
                ++batch.created;
                advance_snapshot(1);
            }
        }
        return batch;
    }

    PublishedBatch publish_regions(const std::vector<std::vector<uint32_t>>& distinct_regions) {
        std::lock_guard<std::mutex> lock(mutex_);
        require_snapshot_runway(distinct_regions.size());
        for (const auto& region : distinct_regions)
            for (uint32_t atom : region)
                if (atom >= atoms_.size())
                    throw std::out_of_range("Region names an unpublished Atom");
        PublishedBatch batch;
        batch.ids.reserve(distinct_regions.size());
        for (const auto& region : distinct_regions) {
            const auto result = regions_.intern(region);
            batch.ids.push_back(result.first);
            if (result.second) {
                ++batch.created;
                advance_snapshot(1);
            }
        }
        return batch;
    }

    std::pair<uint32_t, bool> publish_block(const std::vector<uint32_t>& region_ids) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (region_ids.empty()) throw std::invalid_argument("cannot publish an empty Block");
        if (region_ids.size() > std::numeric_limits<uint32_t>::max())
            throw std::overflow_error("Block child count exceeds u32");
        for (uint32_t region : region_ids)
            if (region >= regions_.size())
                throw std::out_of_range("Block names an unpublished Region");
        require_snapshot_runway(1);
        std::pair<uint32_t, bool> result;
        try {
            result = blocks_.intern(region_ids.data(), static_cast<uint32_t>(region_ids.size()));
        } catch (...) {
            // BlockCatalogue publication follows OnlineS1's process-fatal allocation policy.
            std::terminate();
        }
        if (result.second) advance_snapshot(1);
        return result;
    }

    AdmissionReply admit(const AdmissionInput& input) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto existing = requests_.find(input.producer_request_id);
        if (existing != requests_.end()) {
            const auto& admitted = existing->second;
            const bool same = admitted->source_extent == input.source_extent &&
                              admitted->output_digest == input.output_digest &&
                              admitted->regions == input.regions;
            return {same ? AdmissionResult::Existing : AdmissionResult::RequestMismatch,
                    admitted};
        }
        for (uint32_t region : input.regions)
            if (region >= regions_.size())
                throw std::out_of_range("AdmittedTu names an unpublished Region");

        const uint64_t snapshot_runway =
            std::numeric_limits<uint64_t>::max() - snapshot_version_;
        if (!snapshot_runway || input.regions.size() > snapshot_runway - 1)
            throw std::overflow_error("shared-C snapshot version has no TU runway");
        if (admissions_.size() == std::numeric_limits<uint64_t>::max())
            throw std::overflow_error("canonical admission sequence exhausted");

        // Allocate and populate every fallible log/index object before mutating OnlineS1.
        // OnlineS1::admit has the documented terminate-on-allocation-failure contract; after
        // it starts, an exception ends this process rather than exposing a partly-mutated
        // shared matcher to a later producer.
        auto admitted = std::make_shared<AdmittedTu>();
        admitted->producer_request_id = input.producer_request_id;
        admitted->canonical_admission_sequence = admissions_.size();
        admitted->source_extent = input.source_extent;
        admitted->output_digest = input.output_digest;
        admitted->regions = input.regions;
        admissions_.reserve(admissions_.size() + 1);
        const auto request_slot = requests_.emplace(admitted->producer_request_id, admitted);
        if (!request_slot.second) throw std::logic_error("shared-C request index changed under lock");

        const size_t blocks_before = blocks_.size();
        TuPlan plan;
        try {
            plan = global_.admit(input.regions);
        } catch (...) {
            std::terminate();
        }
        advance_snapshot(blocks_.size() - blocks_before);
        advance_snapshot(1);  // the immutable AdmittedTu log entry itself

        admitted->shared_c_snapshot_version = snapshot_version_;
        admitted->global_plan = std::move(plan);
        admissions_.push_back(admitted);
        return {AdmissionResult::Created, std::move(admitted)};
    }

    std::shared_ptr<PerFRelationship> relationship(const FCacheIdentity& identity) {
        std::lock_guard<std::mutex> lock(mutex_);
        return relationship_locked(identity);
    }

    // Prepare one current TU against exactly one F/lane's committed semantic history.  All
    // route matchers share the authority's immutable Block catalogue, so this operation is
    // serialized with global publication.  A route-specific match may discover a canonical
    // Block the global matcher did not; that ID remains global even if this route aborts.
    RoutePreparation prepare_route(const RouteLaneIdentity& identity,
                                   const std::shared_ptr<const AdmittedTu>& admitted) {
        std::lock_guard<std::mutex> lock(mutex_);
        require_own_admission(admitted);
        RouteLaneState& lane = route_lane_locked(identity);
        if (lane.pending) throw std::logic_error("route prepare while a transaction is pending");
        if (lane.next_prepare_token == std::numeric_limits<uint64_t>::max())
            throw std::overflow_error("route prepare token space exhausted");
        if (lane.committed_sequence == std::numeric_limits<uint64_t>::max())
            throw std::overflow_error("route sequence exhausted");
        // A plan can create no more Blocks than it has Region occurrences.  Reserve that
        // snapshot runway before the shared catalogue can change.
        require_snapshot_runway(admitted->regions.size());

        const size_t blocks_before = blocks_.size();
        try {
            const TuPlan& plan = lane.matcher.prepare(admitted->regions);
            RoutePreparation prepared;
            prepared.lane = identity;
            prepared.route_sequence = lane.committed_sequence;
            prepared.prepare_token = lane.next_prepare_token++;
            prepared.canonical_admission_sequence =
                admitted->canonical_admission_sequence;
            prepared.plan = plan;
            advance_snapshot(blocks_.size() - blocks_before);
            lane.pending = RouteLaneState::Pending{prepared.prepare_token,
                                                    prepared.canonical_admission_sequence};
            return prepared;
        } catch (...) {
            // Block IDs are monotonic and deliberately survive an aborted/failed route
            // prepare.  Account for any IDs minted before restoring the route matcher.
            advance_snapshot(blocks_.size() - blocks_before);
            if (lane.matcher.has_pending()) lane.matcher.abort();
            throw;
        }
    }

    void commit_route_and_installs(
        const RoutePreparation& prepared,
        const std::vector<PerFRelationship::InstallClaim>& claims) {
        std::lock_guard<std::mutex> lock(mutex_);
        RouteLaneState& lane = existing_route_lane_locked(prepared.lane);
        require_route_ticket(lane, prepared);
        const auto relation = relationship_locked(prepared.lane.f);
        if (!relation->commit_installs(claims))
            throw std::logic_error("route Ack carries an invalid install closure");
        lane.matcher.commit();
        ++lane.committed_sequence;
        lane.pending.reset();
    }

    void abort_route(const RoutePreparation& prepared) {
        std::lock_guard<std::mutex> lock(mutex_);
        RouteLaneState& lane = existing_route_lane_locked(prepared.lane);
        require_route_ticket(lane, prepared);
        lane.matcher.abort();
        lane.pending.reset();
    }

    RouteHistoryMark route_history_mark(const RouteLaneIdentity& identity) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = route_lanes_.find(identity);
        if (found == route_lanes_.end()) return {};
        const RouteLaneState& lane = *found->second;
        RouteHistoryMark mark;
        mark.committed_sequence = lane.committed_sequence;
        mark.occurrence_count = lane.matcher.occurrences().size();
        mark.pending = lane.pending.has_value();
        if (lane.pending) {
            mark.pending_prepare_token = lane.pending->prepare_token;
            mark.pending_admission_sequence = lane.pending->canonical_admission_sequence;
        }
        return mark;
    }

    std::vector<uint32_t> canonical_block_children(uint32_t block_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return blocks_.block(block_id).regions;
    }

    size_t admission_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return admissions_.size();
    }
    size_t global_occurrence_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return global_.occurrences().size();
    }
    size_t atom_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return atoms_.size();
    }
    size_t region_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return regions_.size();
    }
    size_t block_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return blocks_.size();
    }
    uint64_t snapshot_version() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_version_;
    }
    const Opaque128& c_guid() const { return c_guid_; }
    const Opaque128& source_generation() const { return source_generation_; }

private:
    struct RouteLaneState {
        struct Pending {
            uint64_t prepare_token = 0;
            uint64_t canonical_admission_sequence = 0;
        };

        RouteLaneState(OnlineS1::Config config, BlockCatalogue& catalogue)
            : matcher(config, catalogue) {}

        OnlineS1 matcher;
        uint64_t committed_sequence = 0;
        uint64_t next_prepare_token = 1;
        std::optional<Pending> pending;
    };

    std::shared_ptr<PerFRelationship> relationship_locked(const FCacheIdentity& identity) {
        const auto found = relationships_.find(identity);
        if (found != relationships_.end()) return found->second;
        auto relation = std::make_shared<PerFRelationship>(identity);
        relationships_.emplace(identity, relation);
        return relation;
    }

    RouteLaneState& route_lane_locked(const RouteLaneIdentity& identity) {
        // Creating a lane also establishes the durable per-F relationship owner.  The
        // returned handle is intentionally discarded here; the authority retains it.
        static_cast<void>(relationship_locked(identity.f));
        const auto found = route_lanes_.find(identity);
        if (found != route_lanes_.end()) return *found->second;
        auto lane = std::make_unique<RouteLaneState>(route_config_, blocks_);
        RouteLaneState* result = lane.get();
        route_lanes_.emplace(identity, std::move(lane));
        return *result;
    }

    RouteLaneState& existing_route_lane_locked(const RouteLaneIdentity& identity) {
        const auto found = route_lanes_.find(identity);
        if (found == route_lanes_.end())
            throw std::logic_error("route ticket names an unknown lane");
        return *found->second;
    }

    void require_own_admission(const std::shared_ptr<const AdmittedTu>& admitted) const {
        if (!admitted || admitted->canonical_admission_sequence >= admissions_.size() ||
            admissions_[admitted->canonical_admission_sequence] != admitted)
            throw std::invalid_argument("route prepare names a foreign AdmittedTu");
    }

    static void require_route_ticket(const RouteLaneState& lane,
                                     const RoutePreparation& prepared) {
        if (!lane.pending ||
            lane.pending->prepare_token != prepared.prepare_token ||
            lane.pending->canonical_admission_sequence !=
                prepared.canonical_admission_sequence ||
            lane.committed_sequence != prepared.route_sequence)
            throw std::logic_error("stale or mismatched route ticket");
    }

    void advance_snapshot(size_t count) {
        if (count > std::numeric_limits<uint64_t>::max() - snapshot_version_)
            throw std::overflow_error("shared-C snapshot version exhausted");
        snapshot_version_ += static_cast<uint64_t>(count);
    }
    void require_snapshot_runway(size_t count) const {
        if (count > std::numeric_limits<uint64_t>::max() - snapshot_version_)
            throw std::overflow_error("shared-C snapshot version exhausted");
    }

    Opaque128 c_guid_{};
    Opaque128 source_generation_{};
    mutable std::mutex mutex_;
    uint64_t snapshot_version_ = 0;
    CanonicalTable<std::string> atoms_;
    CanonicalTable<std::vector<uint32_t>, U32VectorHash> regions_;
    BlockCatalogue blocks_;
    OnlineS1 global_;
    OnlineS1::Config route_config_;
    std::vector<std::shared_ptr<const AdmittedTu>> admissions_;
    std::unordered_map<Opaque128, std::shared_ptr<const AdmittedTu>, Opaque128Hash> requests_;
    std::unordered_map<FCacheIdentity, std::shared_ptr<PerFRelationship>, FCacheIdentityHash>
        relationships_;
    std::unordered_map<RouteLaneIdentity, std::unique_ptr<RouteLaneState>, RouteLaneIdentityHash>
        route_lanes_;
};

}  // namespace p29

#endif
