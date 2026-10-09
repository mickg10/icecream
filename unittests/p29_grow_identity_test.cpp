// Identity + latency check for the GrowBuffer-backed OnlineS1 matcher
// (cache/codec/p29_online_s1.h) against a faithful vector-based reference
// implementation of the SAME algorithm -- the exact pre-change shape: the
// old matcher was this code with std::vector occurrence/predecessor streams.
//
// WHAT IS CHECKED
//   1. Plan identity: over a long seeded synthetic route (thousands of TUs,
//      with region-id repeats so genuine Block matches occur), every TuPlan
//      field the serializer consumes must be equal, element for element:
//      occurrence_begin/end, root, new_blocks, block_uses.  Same catalogue
//      ids, same matcher-local source coordinates.
//   2. Stream identity: the committed occurrence stream after the whole
//      route must be byte-equal between the two implementations.
//   3. Max single-TU prepare latency, reported for BOTH implementations
//      over the same route: the vector reference shows the capacity-doubling
//      copy spikes; the GrowBuffer matcher must not.  (Both numbers are
//      printed; the assertion is only that plans match -- the latency is a
//      reported measurement, not a hard gate, since a loaded CI host can
//      make any absolute number noisy.)
//
// The route sizes are chosen so the vector reference crosses the SAME
// doublings the farm stall was attributed to (two generations late in the
// route), at a runtime that still fits `make check` (~seconds).

#include "cache/codec/p29_online_s1.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using p29::BlockCatalogue;
using p29::BlockUse;
using p29::NewBlock;
using p29::Ref;
using p29::RefKind;
using p29::TuPlan;
using Clock = std::chrono::steady_clock;

[[noreturn]] void die(const std::string &reason) {
    throw std::runtime_error(reason);
}

void require(bool condition, const std::string &reason) {
    if (!condition)
        die(reason);
}

// ---------------------------------------------------------------------------
// Reference matcher: the pre-change algorithm with std::vector streams.
// Kept in lockstep with OnlineS1::build() (same hashes, same chain order,
// same boundary-anchor installation); any divergence is a bug in one of the
// two, and this test is what catches it.
// ---------------------------------------------------------------------------
class VectorOnlineS1 {
public:
    explicit VectorOnlineS1(p29::OnlineS1::Config config)
        : catalogue_(std::make_unique<BlockCatalogue>()),
          config_(config) {
        if (config_.min_match < 2 || config_.min_match > 16)
            die("reference min_match out of range");
        heads_.assign(std::size_t{1} << config_.hash_bits, kNone);
    }

    const std::vector<std::uint32_t> &occurrences() const { return occurrences_; }

    TuPlan admit(const std::vector<std::uint32_t> &current_regions) {
        return build(current_regions);
    }

    const BlockCatalogue &catalogue() const { return *catalogue_; }

private:
    static constexpr std::uint32_t kNone =
        std::numeric_limits<std::uint32_t>::max();

    std::uint64_t sequence_hash(std::uint32_t position) const {
        std::uint64_t hash = 1469598103934665603ULL;
        for (std::uint32_t index = 0; index < config_.min_match; ++index) {
            hash ^= occurrences_[position + index];
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    std::uint32_t bucket(std::uint32_t position) const {
        const std::uint64_t mixed = sequence_hash(position) * 0x9E3779B97F4A7C15ULL;
        return static_cast<std::uint32_t>(mixed >> (64 - config_.hash_bits));
    }

    void install_anchor(std::uint32_t position) {
        const std::uint32_t slot = bucket(position);
        predecessors_[position] = heads_[slot];
        heads_[slot] = position;
    }

    void install_newly_complete_boundary_anchors(std::uint32_t begin,
                                                 std::uint32_t end) {
        const std::uint32_t reach = config_.min_match - 1;
        const std::uint32_t first = begin > reach ? begin - reach : 0;
        for (std::uint32_t position = first; position < begin; ++position) {
            if (std::uint64_t(position) + config_.min_match > begin &&
                std::uint64_t(position) + config_.min_match <= end)
                install_anchor(position);
        }
    }

    TuPlan build(const std::vector<std::uint32_t> &current_regions) {
        const std::size_t u32_max = std::numeric_limits<std::uint32_t>::max();
        if (current_regions.size() > u32_max ||
            occurrences_.size() > u32_max - current_regions.size())
            throw std::overflow_error("S1 occurrence space exceeds u32");

        const std::uint32_t begin = static_cast<std::uint32_t>(occurrences_.size());
        occurrences_.insert(occurrences_.end(), current_regions.begin(),
                            current_regions.end());
        const std::uint32_t end = static_cast<std::uint32_t>(occurrences_.size());
        predecessors_.resize(occurrences_.size(), kNone);
        install_newly_complete_boundary_anchors(begin, end);

        TuPlan plan;
        plan.occurrence_begin = begin;
        plan.occurrence_end = end;
        std::uint32_t position = begin;
        while (position < end) {
            std::uint32_t best_length = 0;
            std::uint32_t best_source = 0;
            if (std::uint64_t(position) + config_.min_match <= end) {
                std::uint32_t candidate = heads_[bucket(position)];
                std::uint32_t chain = 0;
                while (candidate != kNone && chain < config_.max_chain) {
                    const std::uint32_t next_candidate = predecessors_[candidate];
                    if (candidate < position) {
                        const std::uint32_t maximum = end - position;
                        const std::uint32_t length = p29::match_detail::extend_match_scalar(
                            occurrences_.data() + candidate,
                            occurrences_.data() + position, maximum);
                        if (length >= config_.min_match && length > best_length) {
                            best_length = length;
                            best_source = candidate;
                            if (length == maximum)
                                break;
                        }
                    }
                    candidate = next_candidate;
                    ++chain;
                }
            }
            std::uint32_t step = 1;
            if (best_length >= config_.min_match) {
                const auto result = catalogue_->intern(occurrences_.data() + position,
                                                       best_length);
                plan.block_uses.push_back({plan.root.size(), result.first, best_source,
                                           best_length,
                                           best_source + best_length <= begin,
                                           result.second});
                plan.root.push_back({RefKind::Block, result.first});
                if (result.second) {
                    plan.new_blocks.push_back(
                        {result.first, best_source,
                         best_source + best_length <= begin});
                }
                step = best_length;
            } else {
                plan.root.push_back({RefKind::Region, occurrences_[position]});
            }
            for (std::uint32_t at = position; at < position + step; ++at) {
                if (std::uint64_t(at) + config_.min_match <= end)
                    install_anchor(at);
            }
            position += step;
        }
        return plan;
    }

    std::unique_ptr<BlockCatalogue> catalogue_;
    p29::OnlineS1::Config config_;
    std::vector<std::uint32_t> heads_;
    std::vector<std::uint32_t> occurrences_;
    std::vector<std::uint32_t> predecessors_;
};

// ---------------------------------------------------------------------------
// Route generation: seeded, with repeats so genuine matches occur.
// ---------------------------------------------------------------------------
struct Route {
    std::vector<std::vector<std::uint32_t>> tus;
};

[[nodiscard]] Route make_route(std::uint32_t tu_count, std::uint32_t tu_width,
                               std::uint32_t alphabet) {
    // Realistic region-id shape AND guaranteed matches:
    //  - a large alphabet (the real corpus interns >100k distinct regions)
    //    so hash_bits=22 buckets stay sparse and the chain walk stays short
    //    (a small alphabet saturates max_chain with collisions and drowns
    //    the growth signal in chain-walk time);
    //  - a fixed pool of contiguous RUNS (3..12 regions) that is reused
    //    across TUs, so genuine >=min_match (3) matches recur and the matcher
    //    mints Blocks -- random ids over a wide alphabet never repeat a
    //    3-gram by chance;
    //  - fresh singleton regions per TU so Region refs occur too.
    // Deterministic seed: same route every run.
    std::mt19937 rng(0x5EED0000u);
    std::uniform_int_distribution<std::uint32_t> id_dist(0, alphabet);
    std::uniform_int_distribution<std::uint32_t> run_length_dist(3, 12);
    std::vector<std::vector<std::uint32_t>> pool(64);
    for (auto &run : pool) {
        const std::uint32_t length = run_length_dist(rng);
        for (std::uint32_t index = 0; index != length; ++index)
            run.push_back(id_dist(rng));
    }
    Route route;
    route.tus.reserve(tu_count);
    for (std::uint32_t tu = 0; tu != tu_count; ++tu) {
        std::vector<std::uint32_t> regions;
        regions.reserve(tu_width);
        while (regions.size() < tu_width) {
            const std::vector<std::uint32_t> &run = pool[rng() % pool.size()];
            const std::uint32_t fresh = id_dist(rng);
            // Alternate: half the time append a reused run (match fodder),
            // half the time a fresh singleton (region fodder).
            if (rng() % 2 == 0 && regions.size() + run.size() <= tu_width)
                regions.insert(regions.end(), run.begin(), run.end());
            else if (regions.size() + 1 <= tu_width)
                regions.push_back(fresh);
        }
        route.tus.push_back(std::move(regions));
    }
    return route;
}

void require_plans_equal(const TuPlan &left, const TuPlan &right,
                         std::uint32_t tu) {
    const std::string tag = " at TU " + std::to_string(tu);
    require(left.occurrence_begin == right.occurrence_begin,
            "occurrence_begin differs" + tag);
    require(left.occurrence_end == right.occurrence_end,
            "occurrence_end differs" + tag);
    require(left.root.size() == right.root.size(), "root size differs" + tag);
    for (std::size_t index = 0; index != left.root.size(); ++index) {
        require(left.root[index].kind == right.root[index].kind &&
                     left.root[index].id == right.root[index].id,
                "root ref differs at " + std::to_string(index) + tag);
    }
    require(left.new_blocks.size() == right.new_blocks.size(),
            "new_blocks size differs" + tag);
    for (std::size_t index = 0; index != left.new_blocks.size(); ++index) {
        require(left.new_blocks[index].id == right.new_blocks[index].id &&
                    left.new_blocks[index].first_source_position ==
                        right.new_blocks[index].first_source_position &&
                    left.new_blocks[index].source_precedes_current_tu ==
                        right.new_blocks[index].source_precedes_current_tu,
                "new_block differs at " + std::to_string(index) + tag);
    }
    require(left.block_uses.size() == right.block_uses.size(),
            "block_uses size differs" + tag);
    for (std::size_t index = 0; index != left.block_uses.size(); ++index) {
        const BlockUse &a = left.block_uses[index];
        const BlockUse &b = right.block_uses[index];
        require(a.root_index == b.root_index && a.block_id == b.block_id &&
                    a.source_position == b.source_position &&
                    a.length == b.length &&
                    a.source_precedes_current_tu ==
                        b.source_precedes_current_tu &&
                    a.canonical_was_new == b.canonical_was_new,
                "block_use differs at " + std::to_string(index) + tag);
    }
}

struct RouteStats {
    std::uint64_t max_admit_ns = 0;
    std::uint64_t total_admit_ns = 0;
    std::size_t max_admit_tu = 0;
    // Top outliers by admit time, for attributing the max: a growth spike
    // lands at a capacity-crossing TU, anything else is workload.
    std::vector<std::pair<std::uint64_t, std::uint32_t>> slowest;
    std::size_t block_refs = 0;
    std::size_t region_refs = 0;
    std::size_t new_blocks = 0;
    std::vector<TuPlan> plans;
};

// Drive any matcher-shaped object (real p29::OnlineS1 or the vector
// reference) over the route, timing each admit() call.
template <class Matcher>
[[nodiscard]] RouteStats run_route(Matcher &matcher, const Route &route,
                                   bool keep_plans) {
    RouteStats stats;
    if (keep_plans)
        stats.plans.reserve(route.tus.size());
    for (std::uint32_t tu = 0; tu != route.tus.size(); ++tu) {
        const auto started = Clock::now();
        TuPlan plan = matcher.admit(route.tus[tu]);
        const std::uint64_t elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                                  started)
                .count();
        if (elapsed > stats.max_admit_ns) {
            stats.max_admit_ns = elapsed;
            stats.max_admit_tu = tu;
        }
        stats.slowest.emplace_back(elapsed, tu);
        stats.total_admit_ns += elapsed;
        for (const Ref &reference : plan.root)
            reference.kind == RefKind::Block ? ++stats.block_refs
                                             : ++stats.region_refs;
        stats.new_blocks += plan.new_blocks.size();
        if (keep_plans)
            stats.plans.push_back(std::move(plan));
    }
    // Keep the 8 slowest admits for the report.
    std::partial_sort(stats.slowest.begin(), stats.slowest.begin() + 8,
                       stats.slowest.end(),
                       [](const auto &left, const auto &right) {
                           return left.first > right.first;
                       });
    stats.slowest.resize(std::min<std::size_t>(8, stats.slowest.size()));
    return stats;
}

} // namespace

int main(int argc, char **argv) {
    try {
        // Sizing: 3000 TUs x 2000 occurrences = 6M positions (24 MB per
        // uint32 stream).  The vector reference crosses capacity doublings
        // at powers of two (4M/8M elements), i.e. the same late-route
        // generations the farm stall was attributed to, at a runtime that
        // fits `make check`.  An optional --big flag runs the full
        // farm-shaped route (2600 x 50k = 130M positions) for a closer
        // stall replica.
        const bool big = argc > 1 && std::string_view(argv[1]) == "--big";
        const std::uint32_t tu_count = big ? 2600 : 3000;
        const std::uint32_t tu_width = big ? 50000 : 2000;
        // Alphabet sized so buckets stay sparse (hash_bits=22): the real
        // corpus interns >100k distinct regions per route.
        const Route route = make_route(tu_count, tu_width, 120000);

        const p29::OnlineS1::Config config{3, 1024, 22};

        // --- identity: run BOTH over the SAME route, plan by plan -------
        p29::OnlineS1 matcher(config);
        VectorOnlineS1 reference(config);
        const RouteStats grow = run_route(matcher, route, true);
        const RouteStats vector = run_route(reference, route, true);
        require(grow.plans.size() == vector.plans.size() &&
                    grow.plans.size() == route.tus.size(),
                "plan counts differ");
        for (std::uint32_t tu = 0; tu != route.tus.size(); ++tu)
            require_plans_equal(grow.plans[tu], vector.plans[tu], tu);
        require(grow.block_refs == vector.block_refs &&
                    grow.region_refs == vector.region_refs &&
                    grow.new_blocks == vector.new_blocks,
                "route aggregate ref counts differ");
        require(matcher.occurrences().size() == reference.occurrences().size(),
                "committed occurrence stream sizes differ");
        require(std::memcmp(matcher.occurrences().data(),
                            reference.occurrences().data(),
                            matcher.occurrences().size() *
                                sizeof(std::uint32_t)) == 0,
                "committed occurrence stream differs from the vector mirror");
        require(matcher.catalogue().size() == reference.catalogue().size(),
                "catalogue sizes differ");
        require(matcher.catalogue().size() > 0,
                "synthetic route produced no Blocks; test is vacuous");
        require(grow.block_refs > 0 && grow.region_refs > 0,
                "synthetic route produced only one ref kind; identity check "
                "is weaker than intended");

        // --- latency report -------------------------------------------------
        std::cout << "p29_grow_identity: PASS"
                  << " tus=" << route.tus.size()
                  << " width=" << tu_width
                  << " positions="
                  << static_cast<std::uint64_t>(route.tus.size()) * tu_width
                  << " block_refs=" << grow.block_refs
                  << " region_refs=" << grow.region_refs
                  << " new_blocks=" << grow.new_blocks
                  << " catalogue=" << matcher.catalogue().size()
                  << "\n";
        std::cout << "p29_grow_identity: per-admit latency:"
                  << " vector_max_ns=" << vector.max_admit_ns
                  << " vector_total_ms="
                  << vector.total_admit_ns / 1000000
                  << " growbuffer_max_ns=" << grow.max_admit_ns
                  << " growbuffer_total_ms="
                  << grow.total_admit_ns / 1000000
                  << "\n";
        auto report_outliers = [](const char *tag, const RouteStats &stats) {
            std::cout << "p29_grow_identity: " << tag << " slowest admits:";
            for (const auto &[nanoseconds, tu] : stats.slowest)
                std::cout << " [" << tu << "]"
                           << nanoseconds / 1000000 << "ms";
            std::cout << "\n";
        };
        report_outliers("vector", vector);
        report_outliers("growbuffer", grow);
        // Sanity bound: identical algorithm, only storage differs.  A
        // blow-up here means a growth-ladder bug, not host noise (the bound
        // is generous: 4x the vector total plus one second).
        require(grow.total_admit_ns <
                     vector.total_admit_ns * 4 + std::uint64_t{1000000000},
                "GrowBuffer matcher is pathologically slower than the vector"
                " reference");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "p29_grow_identity: FAIL: " << error.what() << "\n";
        return 1;
    }
}
