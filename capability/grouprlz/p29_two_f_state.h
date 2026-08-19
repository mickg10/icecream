// Deterministic two-F reference state machine for P29 route legality.
//
// The shared authority owns canonical admission, the immutable Block catalogue, and one
// ordered matcher per F/lane.  Each endpoint below represents one independent F cache.  Its
// Block store, committed occurrence material, residency and receiver transaction ledger are
// never shared with another endpoint.  This is a reference seam for Phase B, not the final
// socket integration.
#ifndef P29_TWO_F_STATE_H
#define P29_TWO_F_STATE_H

#include "p29_shared_authority.h"
#include "p29_sparse_blocks.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace p29 {

enum class RepresentationMode : uint8_t { Raw = 0, RouteS1 = 1, GlobalS1 = 2 };
enum class BlockDefinitionMode : uint8_t { Children = 0, RouteCopy = 1 };

struct RouteCopyProof {
    RouteLaneIdentity lane{};
    Opaque128 source_generation{};
    uint64_t source_residency_sequence = 0;
    uint64_t source_position = 0;
    uint32_t length = 0;

    bool operator==(const RouteCopyProof& other) const {
        return lane == other.lane && source_generation == other.source_generation &&
               source_residency_sequence == other.source_residency_sequence &&
               source_position == other.source_position && length == other.length;
    }
};

struct CandidateBlockDefinition {
    uint32_t block_id = 0;
    BlockDefinitionMode mode = BlockDefinitionMode::Children;
    std::vector<uint32_t> children;
    std::optional<RouteCopyProof> copy;

    bool operator==(const CandidateBlockDefinition& other) const {
        return block_id == other.block_id && mode == other.mode &&
               children == other.children && copy == other.copy;
    }
};

struct RouteCandidate {
    RepresentationMode mode = RepresentationMode::Raw;
    RouteLaneIdentity lane{};
    Opaque128 source_generation{};
    uint64_t canonical_admission_sequence = 0;
    uint64_t route_sequence = 0;
    std::vector<Ref> root;
    std::vector<CandidateBlockDefinition> definitions;

    bool operator==(const RouteCandidate& other) const {
        return mode == other.mode && lane == other.lane &&
               source_generation == other.source_generation &&
               canonical_admission_sequence == other.canonical_admission_sequence &&
               route_sequence == other.route_sequence && root == other.root &&
               definitions == other.definitions;
    }
};

// Exact soft identifiers carried into the event record.  Neither participates in object,
// route-transaction, or decoder identity.
struct RoutingHints {
    Opaque128 routing_cohort_key{};
    Opaque128 tu_key{};
};

class FRouteEndpoint {
public:
    struct CommittedSpan {
        uint64_t route_sequence = 0;
        size_t begin = 0;
        size_t end = 0;
        Opaque128 tu_key{};
    };

    struct StateMark {
        SharedCAuthority::RouteHistoryMark c_route{};
        PerFRelationship::StateMark c_mirror{};
        SparseBlockStore::StateMark f_blocks{};
        ReceiverTxnLedger::StateMark f_ledger{};
        uint64_t f_occurrence_count = 0;
        uint64_t f_occurrence_digest = 0;
        uint64_t resident_occurrences = 0;
        uint64_t residency_digest = 0;
        uint64_t source_residency_sequence = 0;
        uint64_t f_apply_count = 0;
        uint64_t committed_spans = 0;
        bool c_pending = false;
        bool f_attempt_active = false;
        bool f_committed_waiting_for_ack = false;

        bool operator==(const StateMark& other) const {
            return c_route == other.c_route && c_mirror == other.c_mirror &&
                   f_blocks == other.f_blocks && f_ledger == other.f_ledger &&
                   f_occurrence_count == other.f_occurrence_count &&
                   f_occurrence_digest == other.f_occurrence_digest &&
                   resident_occurrences == other.resident_occurrences &&
                   residency_digest == other.residency_digest &&
                   source_residency_sequence == other.source_residency_sequence &&
                   f_apply_count == other.f_apply_count &&
                   committed_spans == other.committed_spans &&
                   c_pending == other.c_pending &&
                   f_attempt_active == other.f_attempt_active &&
                   f_committed_waiting_for_ack ==
                       other.f_committed_waiting_for_ack;
        }
    };

    FRouteEndpoint(SharedCAuthority& authority, FCacheIdentity f, uint64_t lane_id)
        : authority_(authority), lane_{std::move(f), lane_id},
          relation_(authority_.relationship(lane_.f)),
          receiver_(RouteScope{authority_.c_guid(), authority_.source_generation(),
                               lane_.f.cache_epoch}) {}
    FRouteEndpoint(const FRouteEndpoint&) = delete;
    FRouteEndpoint& operator=(const FRouteEndpoint&) = delete;
    FRouteEndpoint(FRouteEndpoint&&) = delete;
    FRouteEndpoint& operator=(FRouteEndpoint&&) = delete;

    const RouteLaneIdentity& lane() const { return lane_; }
    uint64_t source_residency_sequence() const { return source_residency_sequence_; }
    uint64_t f_apply_count() const { return f_apply_count_; }
    const SparseBlockStore& blocks() const { return blocks_; }
    const std::vector<uint32_t>& occurrences() const { return occurrences_; }
    const std::vector<CommittedSpan>& committed_spans() const { return spans_; }
    bool has_pending() const { return pending_.has_value(); }

    StateMark state_mark() const {
        StateMark mark;
        mark.c_route = authority_.route_history_mark(lane_);
        mark.c_mirror = relation_->state_mark();
        mark.f_blocks = blocks_.state_mark();
        mark.f_ledger = receiver_.state_mark();
        mark.f_occurrence_count = occurrences_.size();
        mark.f_occurrence_digest = digest_occurrences();
        for (size_t index = 0; index < resident_.size(); ++index) {
            if (resident_[index]) ++mark.resident_occurrences;
            mark.residency_digest = mix(mark.residency_digest ^
                                        mix(uint64_t(index) * 2 + resident_[index]));
        }
        mark.source_residency_sequence = source_residency_sequence_;
        mark.f_apply_count = f_apply_count_;
        mark.committed_spans = spans_.size();
        mark.c_pending = pending_.has_value();
        if (pending_) {
            mark.f_attempt_active = pending_->attempt_active;
            mark.f_committed_waiting_for_ack = pending_->f_committed;
        }
        return mark;
    }

    void prepare(const std::shared_ptr<const SharedCAuthority::AdmittedTu>& admitted,
                 RoutingHints hints = {}) {
        if (pending_) throw std::logic_error("endpoint prepare while a TU is pending");
        SharedCAuthority::RoutePreparation route = authority_.prepare_route(lane_, admitted);
        try {
            if (route.plan.occurrence_begin != occurrences_.size())
                throw std::logic_error("C route history and F occurrence history diverged");
            Pending pending;
            pending.admitted = admitted;
            pending.hints = hints;
            pending.route = std::move(route);
            pending.candidates[mode_index(RepresentationMode::Raw)] =
                build_candidate(RepresentationMode::Raw, pending);
            pending.candidates[mode_index(RepresentationMode::RouteS1)] =
                build_candidate(RepresentationMode::RouteS1, pending);
            pending.candidates[mode_index(RepresentationMode::GlobalS1)] =
                build_candidate(RepresentationMode::GlobalS1, pending);
            pending_ = std::move(pending);
            for (const RouteCandidate& candidate : pending_->candidates)
                if (!candidate_legal_for(candidate))
                    throw std::logic_error("authority constructed an illegal route candidate");
        } catch (...) {
            // If pending_ was installed, abort through its exact ticket.  Otherwise the local
            // route variable still owns the ticket.
            if (pending_) {
                authority_.abort_route(pending_->route);
                pending_.reset();
            } else {
                authority_.abort_route(route);
            }
            throw;
        }
    }

    const RouteCandidate& candidate(RepresentationMode mode) const {
        require_pending();
        return pending_->candidates[mode_index(mode)];
    }

    bool candidate_legal_for(const RouteCandidate& candidate) const {
        if (!pending_ || candidate.lane != lane_ ||
            candidate.source_generation != authority_.source_generation() ||
            candidate.canonical_admission_sequence !=
                pending_->admitted->canonical_admission_sequence ||
            candidate.route_sequence != pending_->route.route_sequence)
            return false;

        std::unordered_set<uint32_t> definition_ids;
        for (const CandidateBlockDefinition& definition : candidate.definitions) {
            if (!definition_ids.insert(definition.block_id).second) return false;
            const std::vector<uint32_t> canonical =
                authority_.canonical_block_children(definition.block_id);
            if (definition.children != canonical) return false;
            if (blocks_.known(definition.block_id) ||
                relation_->known(block_key(definition.block_id)))
                return false;
            if (definition.mode == BlockDefinitionMode::Children) {
                if (definition.copy) return false;
            } else {
                if (!definition.copy || !copy_legal(*definition.copy, canonical)) return false;
            }
        }

        std::vector<uint32_t> expanded;
        for (const Ref& ref : candidate.root) {
            if (ref.kind == RefKind::Region) {
                expanded.push_back(ref.id);
                continue;
            }
            const bool known = blocks_.known(ref.id);
            const bool defined = definition_ids.find(ref.id) != definition_ids.end();
            if (known == defined) return false;  // exactly one source: committed or this TU
            const std::vector<uint32_t> children = authority_.canonical_block_children(ref.id);
            expanded.insert(expanded.end(), children.begin(), children.end());
        }
        if (candidate.mode == RepresentationMode::Raw && !candidate.definitions.empty())
            return false;
        return expanded == pending_->admitted->regions;
    }

    void begin_attempt(RepresentationMode mode, TransactionClosure closure) {
        require_pending();
        begin_attempt(candidate(mode), closure);
    }

    // This overload exists so the fixture can prove that a candidate or COPY proof from one
    // F cannot be replayed at another F.  Product callers normally select by mode.
    void begin_attempt(const RouteCandidate& selected, TransactionClosure closure) {
        require_pending();
        if (pending_->attempt_active || pending_->f_committed)
            throw std::logic_error("endpoint already has an active F attempt");
        const RouteCandidate& expected =
            pending_->candidates[mode_index(selected.mode)];
        if (!(selected == expected) || !candidate_legal_for(selected))
            throw std::invalid_argument("candidate is not legal for this F route");

        const TransferTxnId id{receiver_scope(), pending_->route.route_sequence};
        if (receiver_.begin(id, closure) != BeginResult::Ready)
            throw std::logic_error("receiver refused a new route attempt");
        if (source_residency_sequence_ == std::numeric_limits<uint64_t>::max() ||
            f_apply_count_ == std::numeric_limits<uint64_t>::max()) {
            receiver_.abort();
            throw std::overflow_error("F route state clock exhausted");
        }
        pending_->closure = closure;
        pending_->selected = selected.mode;
        pending_->attempt_active = true;

        try {
            occurrences_.reserve(occurrences_.size() + pending_->admitted->regions.size());
            resident_.reserve(resident_.size() + pending_->admitted->regions.size());
            spans_.reserve(spans_.size() + 1);
            pending_->claims.reserve(selected.definitions.size());
            blocks_.begin_transaction();
            for (const CandidateBlockDefinition& definition : selected.definitions) {
                const TypedObjectKey key = block_key(definition.block_id);
                const PerFRelationship::Reservation reservation = relation_->reserve_install(key);
                if (reservation.result != PerFRelationship::ReserveResult::Owner)
                    throw std::logic_error("route install was not the single-flight owner");
                pending_->claims.push_back({key, reservation.token});

                bool installed = false;
                if (definition.mode == BlockDefinitionMode::Children) {
                    installed = blocks_.install_children(definition.block_id,
                                                         definition.children.data(),
                                                         definition.children.size());
                } else {
                    if (!definition.copy ||
                        !copy_legal(*definition.copy, definition.children))
                        throw std::logic_error("COPY proof changed before F apply");
                    installed = blocks_.install_copy(definition.block_id, occurrences_,
                                                     definition.copy->source_position,
                                                     definition.copy->length);
                }
                if (!installed) throw std::logic_error("F refused a planned Block install");
            }

            const std::vector<uint32_t> reconstructed = expand_at_f(selected.root);
            if (reconstructed != pending_->admitted->regions)
                throw std::logic_error("F reconstructed a different Region sequence");
        } catch (...) {
            rollback_attempt_only();
            throw;
        }
    }

    // Reject before TU close.  Both the F overlay and the C route transaction are discarded;
    // canonical IDs remain global, while the same AdmittedTu can be prepared again.
    void reject_attempt() {
        require_pending();
        if (!pending_->attempt_active || pending_->f_committed)
            throw std::logic_error("no rejectable F attempt");
        rollback_attempt_only();
        authority_.abort_route(pending_->route);
        pending_.reset();
    }

    AckReceipt commit_at_f() {
        require_pending();
        if (!pending_->attempt_active || pending_->f_committed ||
            !blocks_.has_pending_transaction() || !receiver_.has_pending())
            throw std::logic_error("F commit without a complete pending attempt");

        // Capacity was reserved before F mutation.  These appends are therefore non-allocating
        // for trivial element types, and the ledger commit cannot fail after the precheck.
        blocks_.commit_transaction();
        const size_t begin = occurrences_.size();
        occurrences_.insert(occurrences_.end(), pending_->admitted->regions.begin(),
                            pending_->admitted->regions.end());
        resident_.insert(resident_.end(), pending_->admitted->regions.size(), uint8_t{1});
        spans_.push_back({pending_->route.route_sequence, begin, occurrences_.size(),
                          pending_->hints.tu_key});
        ++source_residency_sequence_;
        ++f_apply_count_;

        AckReceipt ack;
        if (!receiver_.commit(ack)) std::terminate();
        pending_->ack = ack;
        pending_->attempt_active = false;
        pending_->f_committed = true;
        return ack;
    }

    void deliver_ack(const AckReceipt& ack) {
        require_pending();
        if (!pending_->f_committed || !pending_->ack || !(ack == *pending_->ack))
            throw std::invalid_argument("Ack does not close the pending F transaction");
        authority_.commit_route_and_installs(pending_->route, pending_->claims);
        pending_.reset();
    }

    AckReceipt recover_retained_ack() {
        require_pending();
        if (!pending_->f_committed || !pending_->ack)
            throw std::logic_error("no committed F transaction awaiting Ack recovery");
        AckReceipt retained;
        const TransferTxnId id{receiver_scope(), pending_->route.route_sequence};
        const BeginResult result = receiver_.begin(id, pending_->closure, &retained);
        if (result != BeginResult::DuplicateCommitted || !(retained == *pending_->ack))
            throw std::logic_error("receiver did not return the retained Ack");
        return retained;
    }

    // Evict one committed TU's raw/Region source material.  Matcher history and installed
    // canonical Blocks remain, but no later definition may COPY from any evicted position.
    bool evict_source(uint64_t route_sequence) {
        if (pending_) throw std::logic_error("source eviction while a TU is pending");
        for (const CommittedSpan& span : spans_) {
            if (span.route_sequence != route_sequence) continue;
            bool changed = false;
            for (size_t index = span.begin; index < span.end; ++index) {
                changed |= resident_[index] != 0;
                resident_[index] = 0;
            }
            if (changed) ++source_residency_sequence_;
            return changed;
        }
        return false;
    }

private:
    struct Pending {
        std::shared_ptr<const SharedCAuthority::AdmittedTu> admitted;
        RoutingHints hints{};
        SharedCAuthority::RoutePreparation route;
        std::array<RouteCandidate, 3> candidates;
        std::optional<RepresentationMode> selected;
        TransactionClosure closure{};
        std::vector<PerFRelationship::InstallClaim> claims;
        std::optional<AckReceipt> ack;
        bool attempt_active = false;
        bool f_committed = false;
    };

    static size_t mode_index(RepresentationMode mode) {
        return static_cast<size_t>(mode);
    }

    void require_pending() const {
        if (!pending_) throw std::logic_error("endpoint has no pending TU");
    }

    RouteScope receiver_scope() const {
        return {authority_.c_guid(), authority_.source_generation(), lane_.f.cache_epoch};
    }

    TypedObjectKey block_key(uint32_t id) const {
        return {authority_.source_generation(), ObjectKind::Block, id};
    }

    RouteCandidate build_candidate(RepresentationMode mode, const Pending& pending) const {
        RouteCandidate candidate;
        candidate.mode = mode;
        candidate.lane = lane_;
        candidate.source_generation = authority_.source_generation();
        candidate.canonical_admission_sequence =
            pending.admitted->canonical_admission_sequence;
        candidate.route_sequence = pending.route.route_sequence;

        if (mode == RepresentationMode::Raw) {
            candidate.root.reserve(pending.admitted->regions.size());
            for (uint32_t region : pending.admitted->regions)
                candidate.root.push_back({RefKind::Region, region});
            return candidate;
        }

        const TuPlan& plan = mode == RepresentationMode::RouteS1
                                 ? pending.route.plan
                                 : pending.admitted->global_plan;
        candidate.root = plan.root;
        std::unordered_set<uint32_t> defined;
        for (size_t root_index = 0; root_index < candidate.root.size(); ++root_index) {
            const Ref& ref = candidate.root[root_index];
            if (ref.kind != RefKind::Block || !defined.insert(ref.id).second) continue;
            const bool f_known = blocks_.known(ref.id);
            const bool c_known = relation_->known(block_key(ref.id));
            if (f_known != c_known)
                throw std::logic_error("C mirror and F Block state diverged before prepare");
            if (f_known) continue;

            CandidateBlockDefinition definition;
            definition.block_id = ref.id;
            definition.children = authority_.canonical_block_children(ref.id);
            if (mode == RepresentationMode::RouteS1) {
                for (const BlockUse& use : plan.block_uses) {
                    if (use.block_id != ref.id || !use.source_precedes_current_tu) continue;
                    RouteCopyProof proof{lane_, authority_.source_generation(),
                                         source_residency_sequence_, use.source_position,
                                         use.length};
                    if (copy_legal(proof, definition.children)) {
                        definition.mode = BlockDefinitionMode::RouteCopy;
                        definition.copy = proof;
                        break;
                    }
                }
            }
            candidate.definitions.push_back(std::move(definition));
        }
        return candidate;
    }

    bool copy_legal(const RouteCopyProof& proof,
                    const std::vector<uint32_t>& canonical) const {
        if (proof.lane != lane_ ||
            proof.source_generation != authority_.source_generation() ||
            proof.source_residency_sequence != source_residency_sequence_ ||
            proof.length != canonical.size() ||
            proof.source_position > occurrences_.size() ||
            proof.length > occurrences_.size() - proof.source_position)
            return false;
        const size_t begin = static_cast<size_t>(proof.source_position);
        const size_t end = begin + proof.length;
        if (!std::all_of(resident_.begin() + begin, resident_.begin() + end,
                         [](uint8_t value) { return value != 0; }))
            return false;
        return std::equal(canonical.begin(), canonical.end(), occurrences_.begin() + begin);
    }

    std::vector<uint32_t> expand_at_f(const std::vector<Ref>& root) const {
        std::vector<uint32_t> expanded;
        for (const Ref& ref : root) {
            if (ref.kind == RefKind::Region) {
                expanded.push_back(ref.id);
            } else {
                if (!blocks_.known(ref.id))
                    throw std::logic_error("Root names a Block absent at F");
                const uint32_t* children = blocks_.begin(ref.id);
                expanded.insert(expanded.end(), children, children + blocks_.length(ref.id));
            }
        }
        return expanded;
    }

    void rollback_attempt_only() {
        if (!pending_) return;
        if (blocks_.has_pending_transaction()) blocks_.abort_transaction();
        if (receiver_.has_pending()) receiver_.abort();
        for (const PerFRelationship::InstallClaim& claim : pending_->claims)
            if (!relation_->abort_install(claim.key, claim.token))
                throw std::logic_error("could not abort the C install reservation");
        pending_->claims.clear();
        pending_->attempt_active = false;
        pending_->selected.reset();
    }

    static uint64_t mix(uint64_t value) {
        value ^= value >> 30;
        value *= 0xbf58476d1ce4e5b9ULL;
        value ^= value >> 27;
        value *= 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
    }

    uint64_t digest_occurrences() const {
        uint64_t digest = 0;
        for (size_t index = 0; index < occurrences_.size(); ++index)
            digest = mix(digest ^ mix(uint64_t(occurrences_[index]) +
                                      uint64_t(index) * 0x9e3779b97f4a7c15ULL));
        return digest;
    }

    SharedCAuthority& authority_;
    RouteLaneIdentity lane_{};
    std::shared_ptr<PerFRelationship> relation_;
    ReceiverTxnLedger receiver_;
    SparseBlockStore blocks_;
    std::vector<uint32_t> occurrences_;
    std::vector<uint8_t> resident_;
    std::vector<CommittedSpan> spans_;
    uint64_t source_residency_sequence_ = 0;
    uint64_t f_apply_count_ = 0;
    std::optional<Pending> pending_;
};

}  // namespace p29

#endif
