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
#include <vector>

namespace p29 {

struct FBlockView {
    size_t offset = 0;      // ABSOLUTE index into the arena, so filling an earlier hole
    uint32_t length = 0;    // later cannot shift a view installed before it
    bool known = false;
};

class SparseBlockStore {
public:
    // The single source of truth for "does F hold this Block".
    bool known(uint64_t id) const { return id < blocks_.size() && blocks_[id].known; }
    size_t size() const { return blocks_.size(); }
    const std::vector<uint32_t>& children() const { return children_; }
    const FBlockView& view(uint64_t id) const { return blocks_[id]; }

    // Install from explicit children.  Returns false for a length that cannot be represented
    // or for an id already held -- "already known" is the only identity error that remains.
    bool install_children(uint64_t id, const uint32_t* kids, uint64_t length) {
        if (!representable(length) || known(id)) return false;
        const size_t first = children_.size();
        children_.insert(children_.end(), kids, kids + size_t(length));
        place(id, first, uint32_t(length));
        return true;
    }

    // Install as a COPY of a slice of that route's own occurrence sequence.  The slice is
    // bounds-checked HERE so every caller gets the check, not just the one that remembered
    // to write it.
    bool install_copy(uint64_t id, const std::vector<uint32_t>& route_occurrences,
                      uint64_t source, uint64_t length) {
        if (!representable(length) || known(id)) return false;
        if (source > route_occurrences.size() ||
            length > route_occurrences.size() - source) return false;
        const size_t first = children_.size();
        children_.insert(children_.end(), route_occurrences.begin() + size_t(source),
                         route_occurrences.begin() + size_t(source + length));
        place(id, first, uint32_t(length));
        return true;
    }

    // Expand a held Block into its canonical children.
    const uint32_t* begin(uint64_t id) const { return children_.data() + blocks_[id].offset; }
    uint32_t length(uint64_t id) const { return blocks_[id].length; }

private:
    // Validate the length at FULL width, before it is narrowed to the u32 the view holds.
    static bool representable(uint64_t length) { return length <= 0xffffffffull; }
    void place(uint64_t id, size_t first, uint32_t length) {
        if (id >= blocks_.size()) blocks_.resize(size_t(id) + 1);
        blocks_[id] = FBlockView{first, length, true};
    }

    std::vector<FBlockView> blocks_;   // by canonical id; may contain holes
    std::vector<uint32_t> children_;   // append-only arena
};

}  // namespace p29

#endif
