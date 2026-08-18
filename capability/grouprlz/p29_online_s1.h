#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace p29 {

enum class RefKind : uint8_t { Region = 0, Block = 1 };

struct Ref {
    RefKind kind = RefKind::Region;
    uint32_t id = 0;

    friend bool operator==(const Ref& left, const Ref& right) {
        return left.kind == right.kind && left.id == right.id;
    }
    friend bool operator!=(const Ref& left, const Ref& right) { return !(left == right); }
};

struct Block {
    std::vector<uint32_t> regions;
};

struct NewBlock {
    uint32_t id = 0;
    uint32_t first_source_position = 0;
    // This describes global C admission order only. It does not assert that a selected F
    // owns this source; the per-F transport must independently choose COPY or raw children.
    bool source_precedes_current_tu = false;
};

struct TuPlan {
    uint64_t occurrence_begin = 0;
    uint64_t occurrence_end = 0;
    std::vector<Ref> root;
    std::vector<NewBlock> new_blocks;
};

// Online S1 longest-previous-factor state.
//
// admit() receives exactly one complete current TU. The implementation may inspect all of
// that TU, plus state committed by earlier calls, but it has no API through which it could
// inspect a later TU. Region and Block references have separate native namespaces; no final
// Region count is required to distinguish them.
class OnlineS1 {
public:
    struct Config {
        uint32_t min_match = 3;
        uint32_t max_chain = 1024;
        uint32_t hash_bits = 22;
    };

    OnlineS1() : OnlineS1(Config{}) {}

    explicit OnlineS1(Config config) : config_(config) {
        if (config_.min_match < 2 || config_.min_match > 16) {
            throw std::invalid_argument("S1 min_match must be in [2,16]");
        }
        if (!config_.max_chain) throw std::invalid_argument("S1 max_chain must be positive");
        if (config_.hash_bits < 4 || config_.hash_bits > 30) {
            throw std::invalid_argument("S1 hash_bits must be in [4,30]");
        }
        heads_.assign(size_t{1} << config_.hash_bits, kNone);
    }

    TuPlan admit(const std::vector<uint32_t>& current_regions) {
        const size_t u32_max = std::numeric_limits<uint32_t>::max();
        if (current_regions.size() > u32_max ||
            occurrences_.size() > u32_max - current_regions.size()) {
            throw std::overflow_error("S1 occurrence space exceeds u32");
        }

        const uint32_t begin = static_cast<uint32_t>(occurrences_.size());
        occurrences_.insert(occurrences_.end(), current_regions.begin(), current_regions.end());
        const uint32_t end = static_cast<uint32_t>(occurrences_.size());
        predecessors_.resize(occurrences_.size(), kNone);

        // Positions at the tail of the preceding committed prefix did not have a complete
        // k-gram then. The current TU supplies the missing lookahead. Install those anchors
        // now, before matching this TU, in the same order as the old full-route loop.
        install_newly_complete_boundary_anchors(begin, end);

        TuPlan plan;
        plan.occurrence_begin = begin;
        plan.occurrence_end = end;
        uint32_t position = begin;
        while (position < end) {
            uint32_t best_length = 0;
            uint32_t best_source = 0;
            if (uint64_t(position) + config_.min_match <= end) {
                uint32_t candidate = heads_[bucket(position)];
                uint32_t chain = 0;
                while (candidate != kNone && chain < config_.max_chain) {
                    if (candidate < position) {
                        uint32_t length = 0;
                        const uint32_t maximum = end - position;
                        while (length < maximum &&
                               occurrences_[candidate + length] == occurrences_[position + length]) {
                            ++length;
                        }
                        if (length >= config_.min_match && length > best_length) {
                            best_length = length;
                            best_source = candidate;
                            if (length == maximum) break;
                        }
                    }
                    candidate = predecessors_[candidate];
                    ++chain;
                }
            }

            uint32_t step = 1;
            if (best_length >= config_.min_match) {
                const auto result = intern_block(position, best_length);
                plan.root.push_back({RefKind::Block, result.first});
                if (result.second) {
                    plan.new_blocks.push_back(
                        {result.first, best_source, best_source + best_length <= begin});
                }
                step = best_length;
            } else {
                plan.root.push_back({RefKind::Region, occurrences_[position]});
            }

            // Whole-current-TU lookahead is allowed. Anchors that would extend into the next
            // TU are deliberately left pending and installed by the next admit() call.
            for (uint32_t at = position; at < position + step; ++at) {
                if (uint64_t(at) + config_.min_match <= end) install_anchor(at);
            }
            position += step;
        }
        return plan;
    }

    const std::vector<uint32_t>& occurrences() const { return occurrences_; }
    const std::vector<Block>& blocks() const { return blocks_; }

private:
    static constexpr uint32_t kNone = std::numeric_limits<uint32_t>::max();

    uint64_t sequence_hash(uint32_t position) const {
        uint64_t hash = 1469598103934665603ULL;
        for (uint32_t index = 0; index < config_.min_match; ++index) {
            hash ^= occurrences_[position + index];
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    uint32_t bucket(uint32_t position) const {
        const uint64_t mixed = sequence_hash(position) * 0x9E3779B97F4A7C15ULL;
        return static_cast<uint32_t>(mixed >> (64 - config_.hash_bits));
    }

    void install_anchor(uint32_t position) {
        const uint32_t slot = bucket(position);
        predecessors_[position] = heads_[slot];
        heads_[slot] = position;
    }

    void install_newly_complete_boundary_anchors(uint32_t begin, uint32_t end) {
        const uint32_t reach = config_.min_match - 1;
        const uint32_t first = begin > reach ? begin - reach : 0;
        for (uint32_t position = first; position < begin; ++position) {
            if (uint64_t(position) + config_.min_match > begin &&
                uint64_t(position) + config_.min_match <= end) {
                install_anchor(position);
            }
        }
    }

    std::pair<uint32_t, bool> intern_block(uint32_t position, uint32_t length) {
        uint64_t hash = 1469598103934665603ULL ^
                        (uint64_t(length) * 0x100000001b3ULL);
        for (uint32_t index = 0; index < length; ++index) {
            hash ^= occurrences_[position + index];
            hash *= 1099511628211ULL;
        }

        auto& candidates = block_index_[hash];
        for (uint32_t id : candidates) {
            const auto& existing = blocks_[id].regions;
            if (existing.size() == length &&
                std::memcmp(existing.data(), occurrences_.data() + position,
                            size_t(length) * sizeof(uint32_t)) == 0) {
                return {id, false};
            }
        }

        if (blocks_.size() == std::numeric_limits<uint32_t>::max()) {
            throw std::overflow_error("S1 Block space exceeds u32");
        }
        const uint32_t id = static_cast<uint32_t>(blocks_.size());
        Block block;
        block.regions.assign(occurrences_.begin() + position,
                             occurrences_.begin() + position + length);
        blocks_.push_back(std::move(block));
        candidates.push_back(id);
        return {id, true};
    }

    Config config_;
    std::vector<uint32_t> occurrences_;
    std::vector<uint32_t> heads_;
    std::vector<uint32_t> predecessors_;
    std::vector<Block> blocks_;
    std::unordered_map<uint64_t, std::vector<uint32_t>> block_index_;
};

}  // namespace p29
