// Gate for the sparse F Block store: F must be able to hold canonical Block ids with HOLES.
//
// The old store was indexed by dense arrival order, so it could not represent "minted on C,
// not known on F" at all.  That state is unavoidable once a candidate that does not install
// a Block wins: the id is minted, catalogue ids are never rewound, and the id is simply
// never sent.  With arrival-order indexing the unsent id did not merely stay unknown -- it
// broke the NEXT Block's install.
//
//   g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror p29_sparse_fblocks_test.cpp -o t && ./t
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

// The store exactly as codec50-sink.cpp holds it.
struct FBlockView { size_t offset = 0; uint32_t length = 0; bool known = false; };
struct FStore {
    std::vector<FBlockView> blocks;
    std::vector<uint32_t> children;
    bool install(uint64_t id, const std::vector<uint32_t>& kids) {
        if (id >= blocks.size()) blocks.resize(size_t(id) + 1);
        if (blocks[id].known) return false;
        const size_t first = children.size();
        children.insert(children.end(), kids.begin(), kids.end());
        blocks[id] = {first, uint32_t(kids.size()), true};
        return true;
    }
    bool known(uint64_t id) const { return id < blocks.size() && blocks[id].known; }
    std::vector<uint32_t> expand(uint64_t id) const {
        const FBlockView& v = blocks[id];
        return std::vector<uint32_t>(children.begin() + v.offset,
                                     children.begin() + v.offset + v.length);
    }
};
}  // namespace

int main() {
    // C mints 0, 1, 2.  RAW wins for the transactions that would have installed 0 and 1, so
    // only 2 is ever sent -- the exact shape local-oracle's gate describes.
    const std::vector<uint32_t> kids0{10, 11, 12}, kids1{20, 21}, kids2{30, 31, 32, 33};

    FStore f;
    // Bail rather than read a view that was never installed: a gate that segfaults reports
    // its finding and then loses it in the crash.
    if (!f.install(2, kids2)) {
        std::fprintf(stderr, "FAIL: installing id 2 into an empty store was refused\n");
        std::printf("P29 sparse F Block store FAIL\n");
        return 1;
    }
    check(!f.known(0) && !f.known(1), "ids 0 and 1 became known without being installed");
    check(f.known(2), "id 2 did not become known");
    check(f.expand(2) == kids2, "id 2 did not reconstruct exactly from a holed store");

    // Later, a winning transaction installs 0.  2's view must be untouched -- the arena is
    // append-only and views are absolute, so an earlier hole filling in must not shift it.
    check(f.install(0, kids0), "installing id 0 after id 2 was refused");
    check(f.expand(2) == kids2, "id 2's view changed when the earlier hole was filled");
    check(f.expand(0) == kids0, "id 0 did not reconstruct after filling the hole");
    check(!f.known(1), "id 1 became known when 0 was installed");

    // Re-installing an id is the ONLY error the store still reports.
    check(!f.install(2, kids2), "a duplicate install was accepted");
    check(f.install(1, kids1), "installing the last hole was refused");
    check(f.expand(1) == kids1 && f.expand(0) == kids0 && f.expand(2) == kids2,
          "a view changed after all holes were filled");

    std::printf("P29 sparse F Block store %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
