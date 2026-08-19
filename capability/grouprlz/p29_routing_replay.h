// Deterministic, no-socket routing/representation replay for Issue #16 Phase C.
//
// This is deliberately a policy engine, not a second codec.  A representation describes
// exact independently chargeable wire fragments produced by a serializer.  The replay owns
// chronological per-F materialization, route history, egress lanes and compiler slots.  Full
// codec experiments must populate these fragments from the real serializer; estimates must
// use a different input label and may not be reported as physical bytes.
#ifndef P29_ROUTING_REPLAY_H
#define P29_ROUTING_REPLAY_H

#include "p29_event_record.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace p29::routing {

enum class FragmentKind : uint8_t { BlockDefinition = 0, Material = 1 };

struct WireFragment {
    uint32_t ordinal = 0;
    uint64_t bytes = 0;
    FragmentKind kind = FragmentKind::Material;
    ObjectKind object_kind = ObjectKind::Material;
};

// root/control are paid on every use.  A closure fragment is paid only when the selected F
// lacks that exact generation-local ordinal, and is installed on successful completion.
// route_commits_required makes route-local candidates depend only on the selected F's ACKed
// history.  All representations advance that history after success.
struct Representation {
    CandidateKind kind = CandidateKind::Raw;
    uint64_t root_bytes = 0;
    uint64_t control_bytes = 0;
    uint64_t route_commits_required = 0;
    std::vector<WireFragment> closure;
};

struct Tu {
    uint64_t logical = 0;
    Opaque128 source_generation{};
    Opaque128 routing_cohort_key{};
    Opaque128 tu_key{};
    uint64_t raw_bytes = 0;
    uint64_t release_ns = 0;
    uint64_t compile_ns = 0;
    std::vector<Representation> representations;
};

struct FConfig {
    FCacheIdentity identity{};
    uint32_t compiler_slots = 1;
    bool eligible = true;
};

struct ReplayConfig {
    std::vector<FConfig> fs;
    uint32_t requested_slots = 1;
    uint32_t egress_lanes = 1;
    uint64_t link_bits_per_second = 1000000000ULL;

    // R4's online score is bytes + time_weight * completion time - learned installation
    // value.  Both ratios are integer and explicit so a result is reproducible.
    uint64_t time_weight_bytes = 0;
    uint64_t time_weight_ns = 1;
    uint64_t investment_weight_num = 1;
    uint64_t investment_weight_den = 1;
    uint64_t investment_history_scale = 4;
};

enum class Policy : uint8_t {
    R0RoundRobin = 0,
    R0Fastest = 1,
    R1Resident = 2,
    R2Home = 3,
    R3Rendezvous = 4,
    R4StateAware = 5,
};

inline const char* policy_name(Policy policy) {
    static constexpr const char* names[]{"R0_ROUND_ROBIN", "R0_FASTEST", "R1_RESIDENT",
                                          "R2_HOME", "R3_RENDEZVOUS", "R4_STATE_AWARE"};
    const size_t index = static_cast<size_t>(policy);
    return index < sizeof(names) / sizeof(names[0]) ? names[index] : "INVALID";
}

struct PhysicalCost {
    uint64_t root_bytes = 0;
    uint64_t block_definition_bytes = 0;
    uint64_t material_bytes = 0;
    uint64_t control_bytes = 0;
    uint64_t total_bytes = 0;
    std::vector<MissingObjectEvent> missing_objects;
};

struct Action {
    size_t f = 0;
    size_t representation = 0;

    bool operator==(const Action& other) const {
        return f == other.f && representation == other.representation;
    }
    bool operator<(const Action& other) const {
        return f != other.f ? f < other.f : representation < other.representation;
    }
};

struct DecisionRow {
    uint64_t logical = 0;
    Action action{};
    CandidateKind selected = CandidateKind::Raw;
    PhysicalCost cost{};
    uint64_t transfer_start_ns = 0;
    uint64_t transfer_finish_ns = 0;
    uint64_t compile_start_ns = 0;
    uint64_t compile_finish_ns = 0;
    uint64_t route_commits_before = 0;
    uint64_t route_commits_after = 0;
};

struct ReplayResult {
    Policy policy = Policy::R0RoundRobin;
    std::vector<DecisionRow> rows;
    uint64_t c_to_f_bytes = 0;
    uint64_t makespan_ns = 0;
    double n_eff = 0.0;
    double route_entropy = 0.0;
};

struct GenerationState {
    Opaque128 generation{};
    std::array<std::vector<uint8_t>, 5> known;
};

struct FState {
    FCacheIdentity identity{};
    std::vector<GenerationState> generations;
    std::vector<uint64_t> compiler_ready_ns;
    uint64_t route_commits = 0;
    uint64_t accepted_raw_bytes = 0;
};

struct ReplayState {
    std::vector<FState> fs;
    std::vector<uint64_t> egress_ready_ns;
    std::unordered_map<TypedObjectKey, uint64_t, TypedObjectKeyHash> prior_object_uses;
    uint64_t admitted_tus = 0;
    uint64_t c_to_f_bytes = 0;
    uint64_t makespan_ns = 0;
    uint64_t round_robin_cursor = 0;
};

struct EvaluatedAction {
    Action action{};
    PhysicalCost cost{};
    uint64_t transfer_start_ns = 0;
    uint64_t transfer_finish_ns = 0;
    uint64_t compile_start_ns = 0;
    uint64_t compile_finish_ns = 0;
    uint64_t investment_estimate_bytes = 0;
    size_t egress_lane = 0;
    size_t compiler_slot = 0;
};

struct ParetoOutcome {
    uint64_t c_to_f_bytes = 0;
    uint64_t makespan_ns = 0;
    std::vector<Action> actions;
};

namespace detail {

inline uint64_t checked_add(uint64_t left, uint64_t right, const char* what) {
    if (right > std::numeric_limits<uint64_t>::max() - left)
        throw std::overflow_error(what);
    return left + right;
}

inline uint64_t mul_div_ceil(uint64_t value, uint64_t numerator, uint64_t denominator,
                             const char* what) {
    if (!denominator) throw std::invalid_argument(what);
    if (!value || !numerator) return 0;
    if (value > std::numeric_limits<uint64_t>::max() / numerator)
        throw std::overflow_error(what);
    const uint64_t product = value * numerator;
    return product / denominator + (product % denominator != 0);
}

inline uint64_t mix64(uint64_t value) {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return value;
}

inline uint64_t hash_opaque(const Opaque128& value, uint64_t seed) {
    uint64_t hash = seed;
    for (uint8_t byte : value) hash = mix64(hash ^ byte);
    return hash;
}

inline uint64_t rendezvous_score(const Opaque128& key, const Opaque128& f_id,
                                 uint32_t virtual_slot = 0) {
    uint64_t hash = hash_opaque(key, 0x6a09e667f3bcc909ULL);
    hash = hash_opaque(f_id, hash ^ 0xbb67ae8584caa73bULL);
    return mix64(hash ^ (uint64_t(virtual_slot) * 0x9e3779b97f4a7c15ULL));
}

inline GenerationState* generation(FState& f, const Opaque128& id, bool create) {
    for (GenerationState& state : f.generations)
        if (state.generation == id) return &state;
    if (!create) return nullptr;
    f.generations.push_back({id, {}});
    return &f.generations.back();
}

inline const GenerationState* generation(const FState& f, const Opaque128& id) {
    for (const GenerationState& state : f.generations)
        if (state.generation == id) return &state;
    return nullptr;
}

inline size_t object_kind_index(ObjectKind kind) {
    const size_t index = static_cast<size_t>(kind);
    if (!index || index >= 5) throw std::invalid_argument("invalid typed object kind");
    return index;
}

inline bool known(const FState& f, const Opaque128& generation_id, ObjectKind kind,
                  uint32_t ordinal) {
    const GenerationState* state = generation(f, generation_id);
    if (!state) return false;
    const std::vector<uint8_t>& typed = state->known[object_kind_index(kind)];
    return ordinal < typed.size() && typed[ordinal];
}

inline bool has_generation_state(const FState& f, const Opaque128& generation_id) {
    const GenerationState* state = generation(f, generation_id);
    if (!state) return false;
    for (size_t kind = 1; kind < state->known.size(); ++kind)
        if (std::find(state->known[kind].begin(), state->known[kind].end(), uint8_t{1}) !=
            state->known[kind].end())
            return true;
    return false;
}

inline void install(FState& f, const Opaque128& generation_id, ObjectKind kind,
                    uint32_t ordinal) {
    GenerationState* state = generation(f, generation_id, true);
    std::vector<uint8_t>& typed = state->known[object_kind_index(kind)];
    if (typed.size() <= ordinal) typed.resize(size_t(ordinal) + 1, 0);
    typed[ordinal] = 1;
}

inline size_t earliest(const std::vector<uint64_t>& values) {
    if (values.empty()) throw std::logic_error("empty resource lane set");
    return size_t(std::min_element(values.begin(), values.end()) - values.begin());
}

inline uint64_t stable_fragment_use_count(const ReplayState& state,
                                          const Opaque128& generation_id,
                                          const WireFragment& fragment) {
    const TypedObjectKey key{generation_id, fragment.object_kind, fragment.ordinal};
    const auto found = state.prior_object_uses.find(key);
    return found == state.prior_object_uses.end() ? 0 : found->second;
}

}  // namespace detail

inline void validate(const ReplayConfig& config, const std::vector<Tu>& trace) {
    if (config.fs.empty()) throw std::invalid_argument("routing replay has no F");
    if (!config.requested_slots || !config.egress_lanes || !config.link_bits_per_second)
        throw std::invalid_argument("routing replay has a zero capacity/rate");
    if (!config.time_weight_ns || !config.investment_weight_den ||
        !config.investment_history_scale)
        throw std::invalid_argument("routing replay has a zero policy denominator");
    uint64_t eligible_slots = 0;
    for (const FConfig& f : config.fs) {
        if (!f.compiler_slots) throw std::invalid_argument("F has zero compiler slots");
        if (f.eligible)
            eligible_slots = detail::checked_add(eligible_slots, f.compiler_slots,
                                                 "eligible slot count overflow");
    }
    if (!eligible_slots) throw std::invalid_argument("routing replay has no eligible F");
    std::vector<uint64_t> logical_ids;
    logical_ids.reserve(trace.size());
    std::optional<uint64_t> prior_release;
    for (const Tu& tu : trace) {
        if (prior_release && tu.release_ns < *prior_release)
            throw std::invalid_argument("routing trace release times move backwards");
        prior_release = tu.release_ns;
        if (std::find(logical_ids.begin(), logical_ids.end(), tu.logical) !=
            logical_ids.end())
            throw std::invalid_argument("routing trace repeats a logical TU identity");
        logical_ids.push_back(tu.logical);
        if (tu.representations.empty()) throw std::invalid_argument("TU has no representation");
        for (const Representation& representation : tu.representations) {
            if (static_cast<size_t>(representation.kind) >=
                static_cast<size_t>(CandidateKind::Count))
                throw std::invalid_argument("TU has an invalid representation kind");
            std::vector<std::pair<ObjectKind, uint32_t>> ids;
            ids.reserve(representation.closure.size());
            for (const WireFragment& fragment : representation.closure) {
                (void)detail::object_kind_index(fragment.object_kind);
                if (fragment.kind == FragmentKind::BlockDefinition &&
                    fragment.object_kind != ObjectKind::Block)
                    throw std::invalid_argument(
                        "BlockDefinition fragment does not name a Block object");
                ids.emplace_back(fragment.object_kind, fragment.ordinal);
            }
            std::sort(ids.begin(), ids.end());
            if (std::adjacent_find(ids.begin(), ids.end()) != ids.end())
                throw std::invalid_argument("representation repeats a closure ordinal");
        }
    }
}

inline ReplayState initial_state(const ReplayConfig& config) {
    ReplayState state;
    state.egress_ready_ns.assign(config.egress_lanes, 0);
    state.fs.reserve(config.fs.size());
    for (const FConfig& f : config.fs) {
        FState runtime;
        runtime.identity = f.identity;
        runtime.compiler_ready_ns.assign(f.compiler_slots, 0);
        state.fs.push_back(std::move(runtime));
    }
    return state;
}

inline void reset_f_epoch(ReplayState& state, size_t f, uint64_t new_epoch) {
    if (f >= state.fs.size()) throw std::out_of_range("F reset index");
    FState& runtime = state.fs[f];
    if (new_epoch == runtime.identity.cache_epoch)
        throw std::invalid_argument("F reset did not change cache epoch");
    runtime.identity.cache_epoch = new_epoch;
    runtime.generations.clear();
    runtime.route_commits = 0;
}

inline void evict_object(ReplayState& state, size_t f, const Opaque128& generation_id,
                         uint32_t ordinal, ObjectKind kind = ObjectKind::Material) {
    if (f >= state.fs.size()) throw std::out_of_range("F eviction index");
    GenerationState* generation = detail::generation(state.fs[f], generation_id, false);
    if (!generation) return;
    std::vector<uint8_t>& typed = generation->known[detail::object_kind_index(kind)];
    if (ordinal < typed.size()) typed[ordinal] = 0;
}

inline bool legal(const FState& f, const Representation& representation) {
    return representation.route_commits_required <= f.route_commits;
}

inline PhysicalCost physical_cost(const FState& f, const Tu& tu,
                                  const Representation& representation) {
    if (!legal(f, representation)) throw std::invalid_argument("route candidate is not legal");
    PhysicalCost cost;
    cost.root_bytes = representation.root_bytes;
    cost.control_bytes = representation.control_bytes;
    for (const WireFragment& fragment : representation.closure) {
        if (detail::known(f, tu.source_generation, fragment.object_kind, fragment.ordinal))
            continue;
        cost.missing_objects.push_back({fragment.object_kind, fragment.ordinal});
        if (fragment.kind == FragmentKind::BlockDefinition)
            cost.block_definition_bytes = detail::checked_add(
                cost.block_definition_bytes, fragment.bytes, "BlockDefinition cost overflow");
        else
            cost.material_bytes = detail::checked_add(
                cost.material_bytes, fragment.bytes, "material cost overflow");
    }
    cost.total_bytes = detail::checked_add(cost.root_bytes, cost.block_definition_bytes,
                                           "physical cost overflow");
    cost.total_bytes = detail::checked_add(cost.total_bytes, cost.material_bytes,
                                           "physical cost overflow");
    cost.total_bytes = detail::checked_add(cost.total_bytes, cost.control_bytes,
                                           "physical cost overflow");
    return cost;
}

inline EvaluatedAction evaluate(const ReplayConfig& config, const ReplayState& state,
                                const Tu& tu, Action action) {
    if (action.f >= config.fs.size() || action.f >= state.fs.size() ||
        action.representation >= tu.representations.size())
        throw std::out_of_range("routing action index");
    if (!config.fs[action.f].eligible)
        throw std::invalid_argument("routing action selected an ineligible F");
    const FState& f = state.fs[action.f];
    const Representation& representation = tu.representations[action.representation];
    EvaluatedAction result;
    result.action = action;
    result.cost = physical_cost(f, tu, representation);
    result.egress_lane = detail::earliest(state.egress_ready_ns);
    result.transfer_start_ns =
        std::max(tu.release_ns, state.egress_ready_ns[result.egress_lane]);
    const uint64_t transfer_ns = detail::mul_div_ceil(
        result.cost.total_bytes, 8000000000ULL, config.link_bits_per_second,
        "transfer duration overflow");
    result.transfer_finish_ns = detail::checked_add(
        result.transfer_start_ns, transfer_ns, "transfer finish overflow");
    result.compiler_slot = detail::earliest(f.compiler_ready_ns);
    result.compile_start_ns =
        std::max(result.transfer_finish_ns, f.compiler_ready_ns[result.compiler_slot]);
    result.compile_finish_ns =
        detail::checked_add(result.compile_start_ns, tu.compile_ns, "compile finish overflow");

    uint64_t investment = 0;
    for (const WireFragment& fragment : representation.closure) {
        if (detail::known(f, tu.source_generation, fragment.object_kind, fragment.ordinal))
            continue;
        const uint64_t uses = std::min(detail::stable_fragment_use_count(
                                           state, tu.source_generation, fragment),
                                       config.investment_history_scale);
        const uint64_t value = detail::mul_div_ceil(
            fragment.bytes, uses, config.investment_history_scale,
            "investment estimate overflow");
        investment = detail::checked_add(investment, value,
                                         "investment estimate overflow");
    }
    result.investment_estimate_bytes = investment;
    return result;
}

inline DecisionRow apply(const ReplayConfig& config, ReplayState& state, const Tu& tu,
                         const EvaluatedAction& evaluated, bool update_online_history = true) {
    (void)config;
    FState& f = state.fs[evaluated.action.f];
    const Representation& representation = tu.representations[evaluated.action.representation];
    const uint64_t commits_before = f.route_commits;
    state.egress_ready_ns[evaluated.egress_lane] = evaluated.transfer_finish_ns;
    f.compiler_ready_ns[evaluated.compiler_slot] = evaluated.compile_finish_ns;
    for (const WireFragment& fragment : representation.closure)
        detail::install(f, tu.source_generation, fragment.object_kind, fragment.ordinal);
    if (f.route_commits == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("route commit sequence overflow");
    ++f.route_commits;
    f.accepted_raw_bytes =
        detail::checked_add(f.accepted_raw_bytes, tu.raw_bytes, "per-F raw total overflow");
    state.c_to_f_bytes = detail::checked_add(
        state.c_to_f_bytes, evaluated.cost.total_bytes, "replay C-to-F total overflow");
    state.makespan_ns = std::max(state.makespan_ns, evaluated.compile_finish_ns);
    if (update_online_history) {
        std::vector<TypedObjectKey> seen;
        for (const Representation& candidate : tu.representations) {
            for (const WireFragment& fragment : candidate.closure) {
                const TypedObjectKey key{tu.source_generation, fragment.object_kind,
                                         fragment.ordinal};
                if (std::find(seen.begin(), seen.end(), key) == seen.end())
                    seen.push_back(key);
            }
        }
        for (const TypedObjectKey& key : seen) {
            uint64_t& count = state.prior_object_uses[key];
            if (count != std::numeric_limits<uint64_t>::max()) ++count;
        }
        if (state.admitted_tus != std::numeric_limits<uint64_t>::max()) ++state.admitted_tus;
    }
    return {tu.logical,
            evaluated.action,
            representation.kind,
            evaluated.cost,
            evaluated.transfer_start_ns,
            evaluated.transfer_finish_ns,
            evaluated.compile_start_ns,
            evaluated.compile_finish_ns,
            commits_before,
            f.route_commits};
}

inline std::vector<size_t> eligible_fs(const ReplayConfig& config) {
    std::vector<size_t> result;
    for (size_t index = 0; index < config.fs.size(); ++index)
        if (config.fs[index].eligible) result.push_back(index);
    return result;
}

// Minimum cardinality first, then maximum capacity, then a cohort-stable tie order.  This
// keeps R2 a soft locality choice: it never changes object identity or candidate legality.
inline std::vector<size_t> home_set(const ReplayConfig& config,
                                    const Opaque128& routing_cohort_key) {
    std::vector<size_t> candidates = eligible_fs(config);
    std::sort(candidates.begin(), candidates.end(), [&](size_t left, size_t right) {
        const uint32_t ls = config.fs[left].compiler_slots;
        const uint32_t rs = config.fs[right].compiler_slots;
        if (ls != rs) return ls > rs;
        const uint64_t lh = detail::rendezvous_score(
            routing_cohort_key, config.fs[left].identity.f_id);
        const uint64_t rh = detail::rendezvous_score(
            routing_cohort_key, config.fs[right].identity.f_id);
        return lh != rh ? lh > rh : left < right;
    });
    uint64_t slots = 0;
    size_t keep = 0;
    while (keep < candidates.size() && slots < config.requested_slots) {
        slots = detail::checked_add(slots, config.fs[candidates[keep]].compiler_slots,
                                    "home-set capacity overflow");
        ++keep;
    }
    if (slots < config.requested_slots)
        throw std::invalid_argument("requested slots exceed eligible capacity");
    candidates.resize(keep);
    std::sort(candidates.begin(), candidates.end());
    return candidates;
}

inline size_t rendezvous_f(const ReplayConfig& config, const Tu& tu,
                           const std::vector<size_t>& candidates) {
    if (candidates.empty()) throw std::logic_error("empty rendezvous candidate set");
    size_t best = candidates.front();
    uint64_t best_score = 0;
    bool have = false;
    for (size_t f : candidates) {
        // Weighted rendezvous via one deterministic virtual node per compiler slot.
        uint64_t score = 0;
        for (uint32_t slot = 0; slot < config.fs[f].compiler_slots; ++slot)
            score = std::max(score, detail::rendezvous_score(
                                        tu.tu_key, config.fs[f].identity.f_id, slot));
        if (!have || score > best_score || (score == best_score && f < best)) {
            best = f;
            best_score = score;
            have = true;
        }
    }
    return best;
}

inline std::vector<EvaluatedAction> actions_for(const ReplayConfig& config,
                                                const ReplayState& state, const Tu& tu,
                                                const std::vector<size_t>& fs) {
    std::vector<EvaluatedAction> actions;
    for (size_t f : fs)
        for (size_t representation = 0; representation < tu.representations.size();
             ++representation)
            if (legal(state.fs[f], tu.representations[representation]))
                actions.push_back(evaluate(config, state, tu, {f, representation}));
    if (actions.empty()) throw std::logic_error("TU has no legal action in selected F set");
    return actions;
}

inline EvaluatedAction min_bytes_action(const ReplayConfig& config, const ReplayState& state,
                                        const Tu& tu, const std::vector<size_t>& fs) {
    std::vector<EvaluatedAction> actions = actions_for(config, state, tu, fs);
    return *std::min_element(actions.begin(), actions.end(), [&](const auto& left,
                                                                 const auto& right) {
        if (left.cost.total_bytes != right.cost.total_bytes)
            return left.cost.total_bytes < right.cost.total_bytes;
        if (left.compile_finish_ns != right.compile_finish_ns)
            return left.compile_finish_ns < right.compile_finish_ns;
        return left.action < right.action;
    });
}

inline EvaluatedAction fastest_action(const ReplayConfig& config, const ReplayState& state,
                                      const Tu& tu, const std::vector<size_t>& fs) {
    std::vector<EvaluatedAction> actions = actions_for(config, state, tu, fs);
    return *std::min_element(actions.begin(), actions.end(), [&](const auto& left,
                                                                 const auto& right) {
        if (left.compile_finish_ns != right.compile_finish_ns)
            return left.compile_finish_ns < right.compile_finish_ns;
        if (left.cost.total_bytes != right.cost.total_bytes)
            return left.cost.total_bytes < right.cost.total_bytes;
        return left.action < right.action;
    });
}

inline EvaluatedAction select_action(const ReplayConfig& config, const ReplayState& state,
                                     const Tu& tu, Policy policy) {
    const std::vector<size_t> all = eligible_fs(config);
    if (policy == Policy::R0RoundRobin) {
        const size_t f = all[size_t(state.round_robin_cursor % all.size())];
        return min_bytes_action(config, state, tu, {f});
    }
    if (policy == Policy::R0Fastest) return fastest_action(config, state, tu, all);
    if (policy == Policy::R1Resident) {
        std::vector<size_t> resident;
        for (size_t f : all)
            if (detail::has_generation_state(state.fs[f], tu.source_generation))
                resident.push_back(f);
        return fastest_action(config, state, tu, resident.empty() ? all : resident);
    }
    const std::vector<size_t> homes = home_set(config, tu.routing_cohort_key);
    if (policy == Policy::R2Home) return fastest_action(config, state, tu, homes);
    if (policy == Policy::R3Rendezvous) {
        const size_t f = rendezvous_f(config, tu, homes);
        return min_bytes_action(config, state, tu, {f});
    }
    if (policy != Policy::R4StateAware) throw std::invalid_argument("unknown routing policy");

    std::vector<EvaluatedAction> actions = actions_for(config, state, tu, homes);
    const auto score = [&](const EvaluatedAction& action) {
        const uint64_t time = detail::mul_div_ceil(
            action.compile_finish_ns, config.time_weight_bytes, config.time_weight_ns,
            "R4 time score overflow");
        uint64_t positive = detail::checked_add(action.cost.total_bytes, time,
                                                "R4 positive score overflow");
        const uint64_t investment = detail::mul_div_ceil(
            action.investment_estimate_bytes, config.investment_weight_num,
            config.investment_weight_den, "R4 investment score overflow");
        return positive > investment ? positive - investment : uint64_t{0};
    };
    return *std::min_element(actions.begin(), actions.end(), [&](const auto& left,
                                                                 const auto& right) {
        const uint64_t ls = score(left), rs = score(right);
        if (ls != rs) return ls < rs;
        if (left.cost.total_bytes != right.cost.total_bytes)
            return left.cost.total_bytes < right.cost.total_bytes;
        if (left.compile_finish_ns != right.compile_finish_ns)
            return left.compile_finish_ns < right.compile_finish_ns;
        return left.action < right.action;
    });
}

inline void finish_metrics(ReplayResult& result, const ReplayState& state) {
    long double total = 0.0L;
    for (const FState& f : state.fs) total += f.accepted_raw_bytes;
    if (!total) return;
    long double squares = 0.0L, entropy = 0.0L;
    for (const FState& f : state.fs) {
        if (!f.accepted_raw_bytes) continue;
        const long double p = f.accepted_raw_bytes / total;
        squares += p * p;
        entropy -= p * std::log2(p);
    }
    result.n_eff = squares ? double(1.0L / squares) : 0.0;
    result.route_entropy = double(entropy);
}

inline ReplayResult replay(const ReplayConfig& config, const std::vector<Tu>& trace,
                           Policy policy, ReplayState state) {
    validate(config, trace);
    if (state.fs.size() != config.fs.size() ||
        state.egress_ready_ns.size() != config.egress_lanes)
        throw std::invalid_argument("initial replay state dimensions differ from config");
    for (size_t index = 0; index < state.fs.size(); ++index)
        if (state.fs[index].identity.f_id != config.fs[index].identity.f_id ||
            state.fs[index].compiler_ready_ns.size() != config.fs[index].compiler_slots)
            throw std::invalid_argument("initial replay F identity/capacity differs from config");
    ReplayResult result;
    result.policy = policy;
    result.rows.reserve(trace.size());
    for (const Tu& tu : trace) {
        const EvaluatedAction selected = select_action(config, state, tu, policy);
        result.rows.push_back(apply(config, state, tu, selected));
        if (policy == Policy::R0RoundRobin) ++state.round_robin_cursor;
    }
    result.c_to_f_bytes = state.c_to_f_bytes;
    result.makespan_ns = state.makespan_ns;
    finish_metrics(result, state);
    return result;
}

inline ReplayResult replay(const ReplayConfig& config, const std::vector<Tu>& trace,
                           Policy policy) {
    return replay(config, trace, policy, initial_state(config));
}

namespace detail {

inline bool dominates(const ParetoOutcome& left, const ParetoOutcome& right) {
    return left.c_to_f_bytes <= right.c_to_f_bytes && left.makespan_ns <= right.makespan_ns &&
           (left.c_to_f_bytes < right.c_to_f_bytes || left.makespan_ns < right.makespan_ns);
}

inline void exact_walk(const ReplayConfig& config, const std::vector<Tu>& trace, size_t end,
                       size_t index, ReplayState state, std::vector<Action>& actions,
                       std::vector<ParetoOutcome>& leaves, uint64_t& nodes,
                       uint64_t node_limit) {
    if (++nodes > node_limit) throw std::runtime_error("exact R5 node limit exceeded");
    if (index == end) {
        leaves.push_back({state.c_to_f_bytes, state.makespan_ns, actions});
        return;
    }
    const Tu& tu = trace[index];
    std::vector<EvaluatedAction> candidates = actions_for(config, state, tu,
                                                          eligible_fs(config));
    std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
        return left.action < right.action;
    });
    for (const EvaluatedAction& candidate : candidates) {
        ReplayState next = state;
        apply(config, next, tu, candidate, false);
        actions.push_back(candidate.action);
        exact_walk(config, trace, end, index + 1, std::move(next), actions, leaves, nodes,
                   node_limit);
        actions.pop_back();
    }
}

}  // namespace detail

// Exact over precisely [begin,end), under the declared resource model.  It returns the full
// byte/makespan Pareto set because without a declared tradeoff there is no single honest R5
// optimum.  Full-corpus searches that hit node_limit must be labelled bounded, never exact.
inline std::vector<ParetoOutcome> exact_r5_frontier(
    const ReplayConfig& config, const std::vector<Tu>& trace, size_t begin, size_t end,
    const ReplayState& state, uint64_t node_limit = 1000000) {
    validate(config, trace);
    if (begin > end || end > trace.size()) throw std::out_of_range("R5 horizon bounds");
    std::vector<ParetoOutcome> leaves;
    std::vector<Action> actions;
    uint64_t nodes = 0;
    detail::exact_walk(config, trace, end, begin, state, actions, leaves, nodes, node_limit);
    std::sort(leaves.begin(), leaves.end(), [](const auto& left, const auto& right) {
        if (left.c_to_f_bytes != right.c_to_f_bytes)
            return left.c_to_f_bytes < right.c_to_f_bytes;
        if (left.makespan_ns != right.makespan_ns)
            return left.makespan_ns < right.makespan_ns;
        return left.actions < right.actions;
    });
    std::vector<ParetoOutcome> frontier;
    for (const ParetoOutcome& candidate : leaves) {
        bool dominated = false;
        for (const ParetoOutcome& retained : frontier)
            if (detail::dominates(retained, candidate) ||
                (retained.c_to_f_bytes == candidate.c_to_f_bytes &&
                 retained.makespan_ns == candidate.makespan_ns)) {
                dominated = true;
                break;
            }
        if (dominated) continue;
        frontier.erase(std::remove_if(frontier.begin(), frontier.end(), [&](const auto& prior) {
                           return detail::dominates(candidate, prior);
                       }),
                       frontier.end());
        frontier.push_back(candidate);
    }
    std::sort(frontier.begin(), frontier.end(), [](const auto& left, const auto& right) {
        return left.c_to_f_bytes != right.c_to_f_bytes
                   ? left.c_to_f_bytes < right.c_to_f_bytes
                   : left.makespan_ns < right.makespan_ns;
    });
    return frontier;
}

inline std::vector<ParetoOutcome> exact_r5_frontier(
    const ReplayConfig& config, const std::vector<Tu>& trace,
    uint64_t node_limit = 1000000) {
    return exact_r5_frontier(config, trace, 0, trace.size(), initial_state(config),
                             node_limit);
}

}  // namespace p29::routing

#endif  // P29_ROUTING_REPLAY_H
