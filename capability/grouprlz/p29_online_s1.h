#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
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

// Recorded for EVERY Block use, not only the ones that mint a new canonical id.  A Block
// can already hold a global id while THIS route has its own acknowledged occurrence, which
// is what makes COPY legal there -- so residency and legality are per route, and a source
// coordinate produced by one matcher is meaningless to another.
struct BlockUse {
    size_t root_index = 0;          // index into TuPlan::root
    uint32_t block_id = 0;          // canonical, from the shared catalogue
    uint32_t source_position = 0;   // MATCHER-LOCAL coordinate; not portable across routes
    uint32_t length = 0;
    bool source_precedes_current_tu = false;   // admission order in THIS matcher only
    bool canonical_was_new = false;            // the catalogue minted the id on this use
};

struct TuPlan {
    uint64_t occurrence_begin = 0;
    uint64_t occurrence_end = 0;
    std::vector<Ref> root;
    std::vector<NewBlock> new_blocks;   // retained for the full-route equivalence audit
    std::vector<BlockUse> block_uses;   // what a selected-F serializer must consume
};

// The canonical Block identity space, shared by every matcher.  It owns ONLY the children,
// the lookup index and the ids; it holds no occurrence stream, so children arrive as a span
// rather than being read out of some matcher's buffer.  It must outlive every matcher that
// references it.
class BlockCatalogue {
public:
    std::pair<uint32_t, bool> intern(const uint32_t* regions, uint32_t length) {
        uint64_t hash = 1469598103934665603ULL ^ (uint64_t(length) * 0x100000001b3ULL);
        for (uint32_t index = 0; index < length; ++index) {
            hash ^= regions[index];
            hash *= 1099511628211ULL;
        }
        auto& candidates = index_[hash];
        for (uint32_t id : candidates) {
            const auto& existing = blocks_[id].regions;
            if (existing.size() == length &&
                std::memcmp(existing.data(), regions, size_t(length) * sizeof(uint32_t)) == 0) {
                return {id, false};
            }
        }
        if (blocks_.size() == std::numeric_limits<uint32_t>::max()) {
            throw std::overflow_error("S1 Block space exceeds u32");
        }
        const uint32_t id = static_cast<uint32_t>(blocks_.size());
        Block block;
        block.regions.assign(regions, regions + length);
        blocks_.push_back(std::move(block));
        candidates.push_back(id);
        return {id, true};
    }

    const Block& block(uint32_t id) const { return blocks_.at(id); }
    size_t size() const { return blocks_.size(); }
    const std::vector<Block>& blocks() const { return blocks_; }

private:
    std::vector<Block> blocks_;
    std::unordered_map<uint64_t, std::vector<uint32_t>> index_;
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

    // Standalone: the matcher owns a private catalogue, which is the single-matcher case
    // the equivalence test exercises.  Shared: several matchers reference one catalogue, so
    // identical children get identical canonical ids across routes.
    // NOT a delegating constructor: delegation would initialise every member of the target,
    // including owned_, destroying the catalogue this one just created and leaving
    // catalogue_ dangling.  Each form initialises its own members and shares only start().
    explicit OnlineS1(Config config)
        : owned_(new BlockCatalogue), catalogue_(owned_.get()), config_(config) { start(); }

    OnlineS1(Config config, BlockCatalogue& catalogue)
        : catalogue_(&catalogue), config_(config) { start(); }

    // prepare / commit / abort.  A prepared TU is APPLIED to the matcher immediately -- the
    // plan has to be built against real state -- and the journal is what makes it reversible.
    // Only one may be pending: a route evaluates one transaction at a time, and the caller
    // either Acks it or discards it before the next TU.
    bool has_pending() const { return pending_; }

    const TuPlan& prepare(const std::vector<uint32_t>& current_regions) {
        if (pending_) throw std::logic_error("S1 prepare while a transaction is already pending");
        journal_.clear();
        journal_.occurrences = occurrences_.size();
        journal_.predecessor_count = predecessors_.size();
        pending_ = true;
        try {
            pending_plan_ = build(current_regions);
        } catch (...) {
            undo();            // a half-applied TU must not survive a failed build
            pending_ = false;
            throw;
        }
        return pending_plan_;
    }

    void commit() {
        if (!pending_) throw std::logic_error("S1 commit without a pending transaction");
        pending_ = false;
        journal_.clear();      // applied state is retained; only the undo log is dropped
    }

    void abort() {
        if (!pending_) throw std::logic_error("S1 abort without a pending transaction");
        undo();
        pending_ = false;
    }

    // Ordinary admission: always-committed, so it does not go through the journal at all.
    // GLOBAL_S1 uses this; the SELECTED ROUTE matcher uses prepare()/commit()/abort(),
    // because a route transaction can be discarded before its Ack.
    //
    // The pending_ guard is load-bearing, not defensive.  Direct admission is a second
    // entrance to the state machine: an admit() landing in the middle of a live route
    // prepare() would mutate heads_ and the occurrence stream WITHOUT journalling them
    // (install_anchor only records while pending_), and the route's later abort() would then
    // roll back to a state that never existed.  Gate 5 tests this rejection.
    //
    // build() is called directly rather than via prepare()+commit(): with pending_ false the
    // journal stays empty instead of recording every head write only to discard it, and the
    // plan is returned by value so nothing is copied.
    //
    // API CONTRACT: this form does NOT roll the matcher back if an allocation throws after
    // mutation has begun.  That is acceptable only because allocation failure terminates the
    // process here, so a partly-built matcher is never reused.  A daemon that wants to CATCH
    // allocation failures and keep using the same matcher needs a different staging design --
    // a parked decision, recorded so it is not rediscovered as a bug.
    TuPlan admit(const std::vector<uint32_t>& current_regions) {
        if (pending_) {
            throw std::logic_error("S1 admit while a transaction is already pending");
        }
        return build(current_regions);
    }

private:
    TuPlan build(const std::vector<uint32_t>& current_regions) {
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
                const auto result = catalogue_->intern(occurrences_.data() + position, best_length);
                plan.block_uses.push_back({plan.root.size(), result.first, best_source,
                                           best_length, best_source + best_length <= begin,
                                           result.second});
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

public:
    const std::vector<uint32_t>& occurrences() const { return occurrences_; }
    const std::vector<Block>& blocks() const { return catalogue_->blocks(); }
    const BlockCatalogue& catalogue() const { return *catalogue_; }

private:
    static constexpr uint32_t kNone = std::numeric_limits<uint32_t>::max();

    void start() {
        if (config_.min_match < 2 || config_.min_match > 16) {
            throw std::invalid_argument("S1 min_match must be in [2,16]");
        }
        if (!config_.max_chain) throw std::invalid_argument("S1 max_chain must be positive");
        if (config_.hash_bits < 4 || config_.hash_bits > 30) {
            throw std::invalid_argument("S1 hash_bits must be in [4,30]");
        }
        heads_.assign(size_t{1} << config_.hash_bits, kNone);
    }

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
        // Predecessor VALUES are deliberately not journalled -- see undo().
        if (pending_) journal_.heads.push_back({slot, heads_[slot]});
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

    // Undo log for one pending transaction.  Capacity is reused between transactions, and
    // the 2^hash_bits head table is never copied -- only the slots actually touched.
    struct Journal {
        size_t occurrences = 0, predecessor_count = 0;
        std::vector<std::pair<uint32_t, uint32_t>> heads;         // {slot, old head}
        void clear() { heads.clear(); }
    };

    void undo() {
        // Heads in REVERSE mutation order: one slot can be written several times in a
        // transaction, and only the reverse walk lands the earliest recorded value last.
        for (size_t i = journal_.heads.size(); i-- > 0;) heads_[journal_.heads[i].first] = journal_.heads[i].second;
        // Predecessor VALUES at positions before this transaction are NOT restored, and do
        // not need to be.  A position touched by install_newly_complete_boundary_anchors was
        // not a published anchor in the committed prefix (its k-gram was still incomplete);
        // once the heads are rolled back in reverse nothing reaches its temporary predecessor
        // value; and any later TU that completes that anchor overwrites predecessors_[p]
        // before publishing p as a head.  So the old value is dead, and a restore loop for it
        // would be a check that can never fail.  Only the SIZES are rolled back.
        occurrences_.resize(journal_.occurrences);
        predecessors_.resize(journal_.predecessor_count);
        journal_.clear();
    }

    bool pending_ = false;
    Journal journal_;
    TuPlan pending_plan_;
    std::unique_ptr<BlockCatalogue> owned_;   // only for the standalone form; declared first
    BlockCatalogue* catalogue_ = nullptr;      // so it outlives catalogue_ and the matcher
    Config config_;
    std::vector<uint32_t> occurrences_;
    std::vector<uint32_t> heads_;
    std::vector<uint32_t> predecessors_;
};

}  // namespace p29
