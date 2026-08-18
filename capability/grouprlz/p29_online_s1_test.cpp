#include "p29_online_s1.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using p29::Block;
using p29::NewBlock;
using p29::Ref;
using p29::RefKind;
using p29::TuPlan;

[[noreturn]] void fail(const char* message) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
}

struct ReferenceResult {
    std::vector<TuPlan> plans;
    std::vector<Block> blocks;
};

// Exact typed-reference translation of codec50-sink.cpp's existing full-route S1 loop.
// It intentionally retains the old NS lookahead so the online implementation must prove
// equivalence even for k-grams crossing TU boundaries.
ReferenceResult full_route_reference(const std::vector<std::vector<uint32_t>>& tus,
                                     p29::OnlineS1::Config config) {
    constexpr uint32_t none = std::numeric_limits<uint32_t>::max();
    std::vector<uint32_t> all;
    std::vector<size_t> offsets{0};
    for (const auto& tu : tus) {
        all.insert(all.end(), tu.begin(), tu.end());
        offsets.push_back(all.size());
    }
    if (all.size() > std::numeric_limits<uint32_t>::max()) fail("reference input too large");

    std::vector<uint32_t> heads(size_t{1} << config.hash_bits, none);
    std::vector<uint32_t> previous(all.size(), none);
    std::vector<Block> blocks;
    std::unordered_map<uint64_t, std::vector<uint32_t>> block_index;

    auto sequence_hash = [&](uint32_t position) {
        uint64_t hash = 1469598103934665603ULL;
        for (uint32_t index = 0; index < config.min_match; ++index) {
            hash ^= all[position + index];
            hash *= 1099511628211ULL;
        }
        return hash;
    };
    auto bucket = [&](uint32_t position) {
        return static_cast<uint32_t>(
            (sequence_hash(position) * 0x9E3779B97F4A7C15ULL) >>
            (64 - config.hash_bits));
    };
    auto intern_block = [&](uint32_t position, uint32_t length) {
        uint64_t hash = 1469598103934665603ULL ^
                        (uint64_t(length) * 0x100000001b3ULL);
        for (uint32_t index = 0; index < length; ++index) {
            hash ^= all[position + index];
            hash *= 1099511628211ULL;
        }
        auto& candidates = block_index[hash];
        for (uint32_t id : candidates) {
            const auto& existing = blocks[id].regions;
            if (existing.size() == length &&
                std::equal(existing.begin(), existing.end(), all.begin() + position)) {
                return std::pair<uint32_t, bool>{id, false};
            }
        }
        const uint32_t id = static_cast<uint32_t>(blocks.size());
        blocks.push_back({std::vector<uint32_t>(all.begin() + position,
                                                all.begin() + position + length)});
        candidates.push_back(id);
        return std::pair<uint32_t, bool>{id, true};
    };

    ReferenceResult result;
    for (size_t tu_index = 0; tu_index < tus.size(); ++tu_index) {
        const uint32_t begin = static_cast<uint32_t>(offsets[tu_index]);
        const uint32_t end = static_cast<uint32_t>(offsets[tu_index + 1]);
        TuPlan plan;
        plan.occurrence_begin = begin;
        plan.occurrence_end = end;
        uint32_t position = begin;
        while (position < end) {
            uint32_t best_length = 0;
            uint32_t best_source = 0;
            if (uint64_t(position) + config.min_match <= end) {
                uint32_t candidate = heads[bucket(position)];
                uint32_t chain = 0;
                while (candidate != none && chain < config.max_chain) {
                    if (candidate < position) {
                        uint32_t length = 0;
                        const uint32_t maximum = end - position;
                        while (length < maximum &&
                               all[candidate + length] == all[position + length]) {
                            ++length;
                        }
                        if (length >= config.min_match && length > best_length) {
                            best_length = length;
                            best_source = candidate;
                            if (length == maximum) break;
                        }
                    }
                    candidate = previous[candidate];
                    ++chain;
                }
            }

            uint32_t step = 1;
            if (best_length >= config.min_match) {
                const auto block = intern_block(position, best_length);
                plan.root.push_back({RefKind::Block, block.first});
                if (block.second) {
                    plan.new_blocks.push_back(
                        {block.first, best_source, best_source + best_length <= begin});
                }
                step = best_length;
            } else {
                plan.root.push_back({RefKind::Region, all[position]});
            }

            for (uint32_t at = position; at < position + step; ++at) {
                if (uint64_t(at) + config.min_match <= all.size()) {
                    const uint32_t slot = bucket(at);
                    previous[at] = heads[slot];
                    heads[slot] = at;
                }
            }
            position += step;
        }
        result.plans.push_back(std::move(plan));
    }
    result.blocks = std::move(blocks);
    return result;
}

void compare(const std::vector<std::vector<uint32_t>>& tus,
             p29::OnlineS1::Config config) {
    const ReferenceResult expected = full_route_reference(tus, config);
    p29::OnlineS1 online(config);
    std::vector<TuPlan> actual;
    for (const auto& tu : tus) actual.push_back(online.admit(tu));

    if (actual.size() != expected.plans.size()) fail("plan count differs");
    for (size_t index = 0; index < actual.size(); ++index) {
        const auto& left = actual[index];
        const auto& right = expected.plans[index];
        if (left.occurrence_begin != right.occurrence_begin ||
            left.occurrence_end != right.occurrence_end || left.root != right.root) {
            fail("Root plan differs from full-route reference");
        }
        if (left.new_blocks.size() != right.new_blocks.size()) {
            fail("new Block count differs from full-route reference");
        }
        for (size_t block = 0; block < left.new_blocks.size(); ++block) {
            const NewBlock& a = left.new_blocks[block];
            const NewBlock& b = right.new_blocks[block];
            if (a.id != b.id || a.first_source_position != b.first_source_position ||
                a.source_precedes_current_tu != b.source_precedes_current_tu) {
                fail("new Block metadata differs from full-route reference");
            }
        }
    }
    if (online.blocks().size() != expected.blocks.size()) fail("Block table size differs");
    for (size_t id = 0; id < expected.blocks.size(); ++id) {
        if (online.blocks()[id].regions != expected.blocks[id].regions) {
            fail("canonical Block differs from full-route reference");
        }
    }
}

uint64_t next_random(uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

}  // namespace

int main() {
    const p29::OnlineS1::Config config{3, 1024, 12};

    // The first useful anchor is [10,11,12], which crosses from TU0 into TU1.
    // A full-route encoder installs it while finishing TU0. The online encoder may only
    // install it after TU1 arrives; both must then choose the same Block later in TU1.
    const std::vector<std::vector<uint32_t>> boundary_case{
        {10, 11}, {12, 90, 91, 10, 11, 12, 92}, {10, 11, 12, 92}};
    compare(boundary_case, config);
    p29::OnlineS1 boundary_online(config);
    boundary_online.admit(boundary_case[0]);
    const TuPlan boundary_plan = boundary_online.admit(boundary_case[1]);
    if (std::none_of(boundary_plan.root.begin(), boundary_plan.root.end(),
                     [](const Ref& ref) { return ref.kind == RefKind::Block; })) {
        fail("boundary fixture did not exercise a deferred cross-TU anchor");
    }

    // Empty and one-Region TUs force a boundary k-gram to remain pending across calls.
    compare({{1}, {}, {2}, {}, {3, 8, 1, 2, 3}, {8, 1, 2, 3}}, config);

    // A larger deterministic mix exercises repeated Blocks, overlapping matches, hash
    // chains, and many TU boundaries.
    std::vector<std::vector<uint32_t>> tus;
    uint64_t random = 0x9e3779b97f4a7c15ULL;
    std::vector<uint32_t> vocabulary;
    for (uint32_t value = 1; value <= 64; ++value) vocabulary.push_back(value);
    for (size_t tu = 0; tu < 200; ++tu) {
        std::vector<uint32_t> current;
        const size_t length = 1 + next_random(random) % 90;
        current.reserve(length);
        while (current.size() < length) {
            if (tu && (next_random(random) & 3) != 0) {
                const auto& prior = tus[next_random(random) % tus.size()];
                if (!prior.empty()) {
                    size_t begin = next_random(random) % prior.size();
                    size_t count = std::min<size_t>(1 + next_random(random) % 12,
                                                    prior.size() - begin);
                    count = std::min(count, length - current.size());
                    current.insert(current.end(), prior.begin() + begin,
                                   prior.begin() + begin + count);
                    continue;
                }
            }
            current.push_back(vocabulary[next_random(random) % vocabulary.size()]);
        }
        tus.push_back(std::move(current));
    }
    compare(tus, config);

    // Prefix state cannot depend on a suffix because the online API has not received it.
    p29::OnlineS1 left(config), right(config);
    const std::vector<uint32_t> prefix{7, 8, 9, 7, 8, 9};
    const TuPlan left_prefix = left.admit(prefix);
    const TuPlan right_prefix = right.admit(prefix);
    if (left_prefix.root != right_prefix.root || left.blocks().size() != right.blocks().size()) {
        fail("identical current TU produced unequal prefix state");
    }
    left.admit({1, 2, 3});
    right.admit({400, 401, 402, 403});
    if (left_prefix.root != right_prefix.root) fail("saved prefix changed after suffix admission");

    std::puts("P29 online S1 T_current/full-route equivalence PASS");
    return 0;
}
