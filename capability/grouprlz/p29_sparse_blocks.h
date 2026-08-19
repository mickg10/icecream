// F's Block store, keyed by CANONICAL id and tolerant of holes.
//
// This header is the ONE definition, used by codec50-sink.cpp and by its test.  It exists as
// a header precisely so the test cannot drift from production: a test that reimplements the
// store stays green while a decoder path regresses, which is a gate guarding a copy of the
// code rather than the code.
//
// Why holes are unavoidable rather than exotic: once a candidate that does not install a
// Block wins, that Block's canonical id is already minted, catalogue ids are monotonic and
// never rewound, and the id is simply never sent.  A store indexed by arrival order cannot
// represent "minted on C, not known on F" at all -- there the unsent id does not merely stay
// unknown, it breaks the NEXT Block's install.
#ifndef P29_SPARSE_BLOCKS_H
#define P29_SPARSE_BLOCKS_H

#include <cstdint>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace p29 {

struct FBlockView {
    size_t offset = 0;      // ABSOLUTE index into the arena, so filling an earlier hole
    uint32_t length = 0;    // later cannot shift a view installed before it
    bool known = false;
};

class SparseBlockStore {
public:
    struct StateMark {
        size_t block_slots = 0;
        size_t child_count = 0;
        size_t known_blocks = 0;
        uint64_t block_digest = 0;
        uint64_t child_digest = 0;

        bool operator==(const StateMark& other) const {
            return block_slots == other.block_slots && child_count == other.child_count &&
                   known_blocks == other.known_blocks && block_digest == other.block_digest &&
                   child_digest == other.child_digest;
        }
        bool operator!=(const StateMark& other) const { return !(*this == other); }
    };

    // The single source of truth for "does F hold this Block".
    bool known(uint64_t id) const { return id < blocks_.size() && blocks_[id].known; }
    size_t size() const { return blocks_.size(); }
    size_t known_count() const { return known_count_; }
    const std::vector<uint32_t>& children() const { return children_; }
    const FBlockView& view(uint64_t id) const { return blocks_[id]; }
    StateMark state_mark() const {
        return {blocks_.size(), children_.size(), known_count_, block_digest_, child_digest_};
    }

    // One low-copy receiver transaction.  Appended children are rolled back with one resize;
    // only installs into holes that predate the transaction need an undo entry.  Extending the
    // sparse id vector is likewise undone by resizing to the checkpoint.  No operation copies
    // the existing store.
    void begin_transaction() {
        if (transaction_active_) throw std::logic_error("SparseBlockStore transaction already active");
        checkpoint_ = state_mark();
        undo_.clear();
        transaction_active_ = true;
    }
    void commit_transaction() {
        require_transaction();
        undo_.clear();
        transaction_active_ = false;
    }
    void abort_transaction() {
        require_transaction();
        for (auto it = undo_.rbegin(); it != undo_.rend(); ++it)
            blocks_[it->id] = it->prior;
        blocks_.resize(checkpoint_.block_slots);
        children_.resize(checkpoint_.child_count);
        known_count_ = checkpoint_.known_blocks;
        block_digest_ = checkpoint_.block_digest;
        child_digest_ = checkpoint_.child_digest;
        undo_.clear();
        transaction_active_ = false;
    }
    bool has_pending_transaction() const { return transaction_active_; }

    // Install from explicit children.  Returns false for a length that cannot be represented
    // or for an id already held -- "already known" is the only identity error that remains.
    bool install_children(uint64_t id, const uint32_t* kids, uint64_t length) {
        if (!representable(length) || known(id)) return false;
        return install(id, uint32_t(length), [&](size_t first) {
            children_.insert(children_.end(), kids, kids + size_t(length));
            update_child_digest(first);
        });
    }

    // Install as a COPY of a slice of that route's own occurrence sequence.  The slice is
    // bounds-checked HERE so every caller gets the check, not just the one that remembered
    // to write it.
    bool install_copy(uint64_t id, const std::vector<uint32_t>& route_occurrences,
                      uint64_t source, uint64_t length) {
        if (!representable(length) || known(id)) return false;
        if (source > route_occurrences.size() ||
            length > route_occurrences.size() - source) return false;
        return install(id, uint32_t(length), [&](size_t first) {
            children_.insert(children_.end(), route_occurrences.begin() + size_t(source),
                             route_occurrences.begin() + size_t(source + length));
            update_child_digest(first);
        });
    }

    // Expand a held Block into its canonical children.
    const uint32_t* begin(uint64_t id) const { return children_.data() + blocks_[id].offset; }
    uint32_t length(uint64_t id) const { return blocks_[id].length; }

private:
    struct Undo { size_t id; FBlockView prior; };

    // Validate the length at FULL width, before it is narrowed to the u32 the view holds.
    static bool representable(uint64_t length) { return length <= 0xffffffffull; }
    static uint64_t mix(uint64_t value) {
        value ^= value >> 30; value *= 0xbf58476d1ce4e5b9ULL;
        value ^= value >> 27; value *= 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
    }
    static uint64_t block_contribution(uint64_t id, const FBlockView& view) {
        return mix(id ^ mix(uint64_t(view.offset)) ^ (uint64_t(view.length) << 17));
    }
    void update_child_digest(size_t first) {
        for (size_t index = first; index < children_.size(); ++index)
            child_digest_ = mix(child_digest_ ^ mix(uint64_t(children_[index]) +
                                                    uint64_t(index) * 0x9e3779b97f4a7c15ULL));
    }
    void require_transaction() const {
        if (!transaction_active_) throw std::logic_error("SparseBlockStore has no active transaction");
    }
    template<class Append>
    bool install(uint64_t id, uint32_t length, Append append) {
        if (id > SIZE_MAX - 1) return false;
        const size_t slot = size_t(id), old_slots = blocks_.size(), old_children = children_.size();
        const uint64_t old_child_digest = child_digest_;
        bool journalled = false;
        try {
            if (transaction_active_ && slot < checkpoint_.block_slots) {
                undo_.push_back({slot, blocks_[slot]});
                journalled = true;
            }
            if (slot >= blocks_.size()) blocks_.resize(slot + 1);
            append(old_children);
            blocks_[slot] = FBlockView{old_children, length, true};
            block_digest_ ^= block_contribution(id, blocks_[slot]);
            ++known_count_;
            return true;
        } catch (...) {
            children_.resize(old_children);
            blocks_.resize(old_slots);
            child_digest_ = old_child_digest;
            if (journalled) undo_.pop_back();
            throw;
        }
    }

    std::vector<FBlockView> blocks_;   // by canonical id; may contain holes
    std::vector<uint32_t> children_;   // append-only arena
    size_t known_count_ = 0;
    uint64_t block_digest_ = 0, child_digest_ = 0;
    bool transaction_active_ = false;
    StateMark checkpoint_{};
    std::vector<Undo> undo_;
};

}  // namespace p29

#endif
