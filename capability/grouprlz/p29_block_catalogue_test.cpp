// Gates 2-5 for the shared BlockCatalogue (gate 1 is p29_online_s1_test.cpp, which stays
// unmodified so it remains an independent guard; gate 6 is that file's mutation checks).
//
// The property under test is the one that makes per-route matching safe: Block IDENTITY is
// global and shared, while every source COORDINATE is matcher-local and means nothing on
// another route.  Conflating the two is exactly how a COPY against a source the selected F
// never received would get emitted.
//
//   g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror p29_block_catalogue_test.cpp -o t && ./t
#include "p29_online_s1.h"

#include <cstdio>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

std::vector<p29::BlockUse> block_uses(const p29::TuPlan& plan) { return plan.block_uses; }

}  // namespace

int main() {
    const p29::OnlineS1::Config config{3, 1024, 12};

    // A repeated Region run so the matcher has something to intern, plus padding either
    // side so the two routes see it at DIFFERENT occurrence positions.
    const std::vector<uint32_t> shared_run{41, 42, 43, 44, 45};
    std::vector<uint32_t> route_a_first{7, 8};
    route_a_first.insert(route_a_first.end(), shared_run.begin(), shared_run.end());
    std::vector<uint32_t> route_b_first{9, 10, 11, 12, 13, 14, 15};
    route_b_first.insert(route_b_first.end(), shared_run.begin(), shared_run.end());

    p29::BlockCatalogue catalogue;
    p29::OnlineS1 route_a(config, catalogue), route_b(config, catalogue);

    // Each route admits its padded copy once, then the run again so the SECOND admission
    // matches against its own history and interns a Block.
    route_a.admit(route_a_first);
    const p29::TuPlan a_plan = route_a.admit(shared_run);
    route_b.admit(route_b_first);
    const p29::TuPlan b_plan = route_b.admit(shared_run);

    const auto a_uses = block_uses(a_plan);
    const auto b_uses = block_uses(b_plan);
    check(!a_uses.empty(), "route A recorded no Block use");
    check(!b_uses.empty(), "route B recorded no Block use");
    if (a_uses.empty() || b_uses.empty()) {
        std::fprintf(stderr, "shared-catalogue gates: FAIL\n");
        return 1;
    }

    // GATE 2: identical children -> identical canonical id, across independent matchers.
    check(a_uses.front().block_id == b_uses.front().block_id,
          "gate 2: the same Region sequence got different Block ids on two routes");

    // GATE 5: the second matcher must REUSE that id, not mint another.
    check(!b_uses.front().canonical_was_new,
          "gate 5: route B minted a new canonical id for children route A already interned");
    const size_t after_both = catalogue.size();
    p29::OnlineS1 route_c(config, catalogue);
    route_c.admit(route_b_first);
    route_c.admit(shared_run);
    check(catalogue.size() == after_both,
          "gate 5: catalogue grew when a third matcher interned identical children");

    // GATE 3: source coordinates are matcher-local, so the two routes -- which saw the run
    // at different offsets -- must report different positions for the SAME Block.
    check(a_uses.front().source_position != b_uses.front().source_position,
          "gate 3: source positions coincided, so this fixture cannot show they are local");
    check(a_uses.front().length == b_uses.front().length, "gate 3: lengths disagree");

    // GATE 4: a sequence only route A has seen is not matchable on route B until B admits
    // it.  B is asked for a run it has never seen; it must emit Regions, not a Block.
    const std::vector<uint32_t> a_only{201, 202, 203, 204};
    p29::BlockCatalogue fresh;
    p29::OnlineS1 only_a(config, fresh), only_b(config, fresh);
    only_a.admit(a_only);
    only_a.admit(a_only);                       // A now has it interned as a Block
    const p29::TuPlan b_first_sight = only_b.admit(a_only);
    check(b_first_sight.block_uses.empty(),
          "gate 4: route B matched a sequence it had never admitted");
    bool all_regions = true;
    for (const p29::Ref& ref : b_first_sight.root) {
        if (ref.kind != p29::RefKind::Region) all_regions = false;
    }
    check(all_regions, "gate 4: route B emitted a Block reference on first sight");
    const p29::TuPlan b_second_sight = only_b.admit(a_only);
    check(!b_second_sight.block_uses.empty(),
          "gate 4: route B still did not match after admitting the sequence itself");
    if (!b_second_sight.block_uses.empty() && !only_a.blocks().empty()) {
        check(!b_second_sight.block_uses.front().canonical_was_new,
              "gate 4: route B minted a second id for children route A had already interned");
    }

    std::printf("P29 shared BlockCatalogue gates 2-5 %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
