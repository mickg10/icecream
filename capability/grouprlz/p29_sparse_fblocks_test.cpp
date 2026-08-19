// Gate for the PRODUCTION sparse F Block store.
//
// This includes p29_sparse_blocks.h -- the same definition codec50-sink.cpp uses.  The
// earlier version of this test reimplemented the store, so it would have stayed green while
// a decoder path regressed: a gate guarding a copy of the code rather than the code.
//
//   g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror p29_sparse_fblocks_test.cpp -o t && ./t
#include "p29_sparse_blocks.h"

#include <cstdio>
#include <stdexcept>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}
std::vector<uint32_t> expand(const p29::SparseBlockStore& s, uint64_t id) {
    return std::vector<uint32_t>(s.begin(id), s.begin(id) + s.length(id));
}
}  // namespace

int main() {
    const std::vector<uint32_t> kids0{10, 11, 12}, kids1{20, 21}, kids2{30, 31, 32, 33};

    // --- holes, through the explicit-children form -------------------------------------
    // C mints 0,1,2; RAW wins for the transactions that would have installed 0 and 1, so
    // only 2 is ever sent.
    {
        p29::SparseBlockStore f;
        if (!f.install_children(2, kids2.data(), kids2.size())) {
            std::fprintf(stderr, "FAIL: installing id 2 into an empty store was refused\n");
            std::printf("P29 sparse F Block store FAIL\n");
            return 1;
        }
        check(!f.known(0) && !f.known(1), "ids 0 and 1 became known without being installed");
        check(f.known(2) && expand(f, 2) == kids2, "id 2 did not reconstruct from a holed store");

        check(f.install_children(0, kids0.data(), kids0.size()), "installing id 0 after id 2 was refused");
        check(expand(f, 2) == kids2, "id 2's view changed when the earlier hole was filled");
        check(expand(f, 0) == kids0, "id 0 did not reconstruct after filling the hole");
        check(!f.known(1), "id 1 became known when 0 was installed");

        check(!f.install_children(2, kids2.data(), kids2.size()), "a duplicate install was accepted");
        check(f.install_children(1, kids1.data(), kids1.size()), "installing the last hole was refused");
        check(expand(f, 1) == kids1 && expand(f, 0) == kids0 && expand(f, 2) == kids2,
              "a view changed after all holes were filled");
    }

    // --- the SAME holed pattern through the route-local COPY form -----------------------
    // ROUTE_S1's candidate uses COPY, so the COPY path has to carry the same guarantees --
    // and its slice bound has to be real, not assumed.
    {
        std::vector<uint32_t> route;                  // a route's own occurrence sequence
        route.insert(route.end(), kids0.begin(), kids0.end());   // [0,3)
        route.insert(route.end(), kids1.begin(), kids1.end());   // [3,5)
        route.insert(route.end(), kids2.begin(), kids2.end());   // [5,9)

        p29::SparseBlockStore f;
        check(f.install_copy(2, route, 5, kids2.size()), "COPY install of id 2 was refused");
        check(!f.known(0) && !f.known(1), "COPY: 0 and 1 became known without being installed");
        check(expand(f, 2) == kids2, "COPY: id 2 did not reconstruct the requested slice");

        check(f.install_copy(0, route, 0, kids0.size()), "COPY install of id 0 was refused");
        check(expand(f, 2) == kids2, "COPY: id 2's view changed when the earlier hole was filled");
        check(expand(f, 0) == kids0, "COPY: id 0 did not reconstruct after filling the hole");

        // both forms must agree on the same children
        p29::SparseBlockStore g;
        check(g.install_children(7, kids2.data(), kids2.size()), "children install of id 7 refused");
        check(expand(g, 7) == expand(f, 2), "COPY and children forms produced different children");

        // the slice bound is the point: past the end, and length overrunning from a valid
        // start, must both be refused rather than reading off the end
        check(!f.install_copy(9, route, route.size(), 1), "COPY past the end was accepted");
        check(!f.install_copy(9, route, 7, 5), "COPY overrunning the end was accepted");
        check(!f.known(9), "a refused COPY still marked the id known");
        check(!f.install_copy(2, route, 5, kids2.size()), "COPY duplicate install was accepted");
    }

    // --- a length that cannot be represented is refused BEFORE it is narrowed ------------
    {
        p29::SparseBlockStore f;
        const uint32_t one = 42;
        check(!f.install_children(3, &one, uint64_t(0xffffffffull) + 1),
              "a length of 2^32 was accepted (it narrows to 0, so this only fails if the "
              "check precedes the narrowing)");
        check(!f.known(3), "a refused oversized install still marked the id known");
    }

    // --- low-copy whole-TU transaction: earlier holes + vector growth + COPY all roll back --
    {
        std::vector<uint32_t> route;
        route.insert(route.end(), kids0.begin(), kids0.end());
        route.insert(route.end(), kids1.begin(), kids1.end());
        route.insert(route.end(), kids2.begin(), kids2.end());
        p29::SparseBlockStore f;
        check(f.install_children(5, kids0.data(), kids0.size()), "transaction fixture install refused");
        const auto before = f.state_mark();
        const auto five = expand(f, 5);

        f.begin_transaction();
        check(f.install_copy(2, route, 3, kids1.size()), "transaction COPY into an old hole refused");
        check(f.install_children(11, kids2.data(), kids2.size()), "transaction sparse growth refused");
        check(f.state_mark() != before, "transaction mutations did not change the state mark");
        f.abort_transaction();
        check(f.state_mark() == before, "abort did not restore the exact state mark");
        check(!f.known(2) && !f.known(11), "abort left a tentative Block known");
        check(f.known(5) && expand(f, 5) == five, "abort changed a pre-existing Block");

        // Retry the identical transaction and commit it.  The first aborted attempt must not
        // perturb arena offsets or the resulting digest.
        f.begin_transaction();
        check(f.install_copy(2, route, 3, kids1.size()), "retry COPY refused");
        check(f.install_children(11, kids2.data(), kids2.size()), "retry sparse growth refused");
        const auto retry = f.state_mark();
        f.commit_transaction();
        check(f.state_mark() == retry, "commit changed the already-installed transaction state");
        check(expand(f, 2) == kids1 && expand(f, 11) == kids2, "committed retry reconstructed differently");

        bool threw = false;
        try { f.begin_transaction(); f.begin_transaction(); }
        catch (const std::logic_error&) { threw = true; }
        check(threw && f.has_pending_transaction(), "a second begin did not preserve the first transaction");
        f.abort_transaction();
        threw = false; try { f.abort_transaction(); } catch (const std::logic_error&) { threw = true; }
        check(threw, "abort without a transaction was accepted");
    }

    std::printf("P29 sparse F Block store %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
