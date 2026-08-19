#include "p29_routing_replay.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

p29::Opaque128 opaque(uint8_t base) {
    p29::Opaque128 value{};
    for (size_t index = 0; index < value.size(); ++index)
        value[index] = uint8_t(base + index * 7);
    return value;
}

p29::routing::Representation rep(p29::CandidateKind kind, uint64_t root,
                                 std::vector<p29::routing::WireFragment> closure = {},
                                 uint64_t history = 0, uint64_t control = 0) {
    return {kind, root, control, history, std::move(closure)};
}

p29::routing::Tu tu(uint64_t logical, uint8_t key, uint64_t compile_ns,
                    std::vector<p29::routing::Representation> representations,
                    uint64_t raw = 1000) {
    p29::routing::Tu value;
    value.logical = logical;
    value.source_generation = opaque(10);
    value.routing_cohort_key = opaque(30);
    value.tu_key = opaque(key);
    value.raw_bytes = raw;
    value.compile_ns = compile_ns;
    value.representations = std::move(representations);
    return value;
}

p29::routing::ReplayConfig config(std::vector<uint32_t> slots, uint32_t wanted = 1) {
    p29::routing::ReplayConfig value;
    value.requested_slots = wanted;
    value.link_bits_per_second = 8000000000ULL;  // one byte/ns
    value.egress_lanes = uint32_t(slots.size());
    for (size_t index = 0; index < slots.size(); ++index)
        value.fs.push_back({{opaque(uint8_t(80 + index * 20)), 1}, slots[index], true});
    return value;
}

}  // namespace

int main() {
    using namespace p29::routing;

    // Malformed inputs must not become apparently valid replay evidence.
    {
        ReplayConfig c = config({1});
        std::vector<Tu> trace{tu(0, 40, 1,
                                 {rep(p29::CandidateKind::Fi, 1,
                                      {{7, 2, FragmentKind::Material},
                                       {7, 3, FragmentKind::Material}})})};
        bool rejected = false;
        try {
            (void)replay(c, trace, Policy::R0RoundRobin);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected, "duplicate closure ordinal was accepted");
        trace = {tu(0, 40, 1,
                    {rep(p29::CandidateKind::Fi, 1,
                         {{8, 3, FragmentKind::BlockDefinition,
                           p29::ObjectKind::Region}})})};
        rejected = false;
        try {
            (void)replay(c, trace, Policy::R0RoundRobin);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected, "BlockDefinition fragment with Region identity was accepted");
        // Direct ordinals are typed: Region 7 and Block 7 are distinct and both must be paid.
        trace = {tu(0, 40, 1,
                    {rep(p29::CandidateKind::Fi, 1,
                         {{7, 2, FragmentKind::Material, p29::ObjectKind::Region},
                          {7, 3, FragmentKind::BlockDefinition,
                           p29::ObjectKind::Block}})})};
        const ReplayResult typed = replay(c, trace, Policy::R0RoundRobin);
        check(typed.c_to_f_bytes == 6 && typed.rows[0].cost.missing_objects.size() == 2,
              "typed ordinals sharing a number were collapsed");
        c = config({1});
        c.egress_lanes = 0;
        rejected = false;
        try {
            (void)replay(c, {tu(0, 40, 1, {rep(p29::CandidateKind::Raw, 1)})},
                         Policy::R0RoundRobin);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected, "zero egress capacity was accepted");
    }

    // Route history advances after every successful representation.  A route-local candidate
    // that requires one committed TU therefore becomes legal after RAW, not only after a route
    // representation wins.
    {
        ReplayConfig c = config({1});
        std::vector<Tu> trace{
            tu(0, 41, 0, {rep(p29::CandidateKind::Raw, 5),
                          rep(p29::CandidateKind::RouteS1, 1, {}, 1)}),
            tu(1, 42, 0, {rep(p29::CandidateKind::Raw, 5),
                          rep(p29::CandidateKind::RouteS1, 1, {}, 1)})};
        ReplayResult result = replay(c, trace, Policy::R0RoundRobin);
        check(result.rows.size() == 2 && result.rows[0].selected == p29::CandidateKind::Raw &&
                  result.rows[1].selected == p29::CandidateKind::RouteS1 &&
                  result.rows[0].route_commits_before == 0 &&
                  result.rows[0].route_commits_after == 1 &&
                  result.rows[1].route_commits_before == 1 &&
                  result.rows[1].route_commits_after == 2,
              "representation-independent route history did not advance");
    }

    // R5 sees the state consequence of a representation.  Current-TU greedy RAW costs 60+60;
    // installing the shared object costs 80 now and 10 later, so the exact two-TU byte optimum
    // selects FI on the first TU.
    {
        ReplayConfig c = config({1});
        const WireFragment object{3, 70, FragmentKind::Material};
        std::vector<Tu> trace{
            tu(0, 43, 0, {rep(p29::CandidateKind::Raw, 60),
                          rep(p29::CandidateKind::Fi, 10, {object})}),
            tu(1, 44, 0, {rep(p29::CandidateKind::Raw, 60),
                          rep(p29::CandidateKind::Fi, 10, {object})})};
        ReplayResult greedy = replay(c, trace, Policy::R0RoundRobin);
        check(greedy.c_to_f_bytes == 120 &&
                  greedy.rows[0].selected == p29::CandidateKind::Raw,
              "greedy control did not retain its expected deferral cost");
        const auto frontier = exact_r5_frontier(c, trace);
        check(frontier.size() == 1 && frontier[0].c_to_f_bytes == 90 &&
                  frontier[0].actions.size() == 2 &&
                  frontier[0].actions[0].representation == 1 &&
                  frontier[0].actions[1].representation == 1,
              "R5 did not value future installed state");
    }

    // The online investment learner observes semantic TU use, not the number of candidate
    // encodings that happen to mention the same object.
    {
        ReplayConfig c = config({1});
        const WireFragment object{4, 20, FragmentKind::Material};
        const Tu value = tu(0, 44, 0,
                            {rep(p29::CandidateKind::Fi, 1, {object}),
                             rep(p29::CandidateKind::GlobalS1, 2, {object})});
        ReplayState state = initial_state(c);
        apply(c, state, value, evaluate(c, state, value, {0, 0}));
        const p29::TypedObjectKey key{value.source_generation,
                                      p29::ObjectKind::Material, object.ordinal};
        check(state.prior_object_uses.at(key) == 1,
              "one TU was counted once per representation by the online learner");
    }

    // One warm F and one cold F create a real byte/makespan tradeoff.  Keeping both TUs on the
    // same F installs once but serializes compilation; spreading duplicates the installation
    // and compiles in parallel.  Both points must survive instead of being collapsed into an
    // unexplained scalar "oracle" result.
    {
        ReplayConfig c = config({1, 1}, 2);
        c.egress_lanes = 2;
        c.link_bits_per_second = std::numeric_limits<uint64_t>::max();
        const WireFragment object{9, 90, FragmentKind::Material};
        std::vector<Tu> trace{
            tu(0, 45, 1000, {rep(p29::CandidateKind::Fi, 10, {object})}),
            tu(1, 46, 1000, {rep(p29::CandidateKind::Fi, 10, {object})})};
        const auto frontier = exact_r5_frontier(c, trace);
        check(frontier.size() == 2, "R5 did not retain both byte/makespan Pareto points");
        check(frontier[0].c_to_f_bytes == 110 && frontier[0].makespan_ns > 1900 &&
                  frontier[1].c_to_f_bytes == 200 && frontier[1].makespan_ns < 1100,
              "R5 Pareto costs differ from exact chronological state");
    }

    // R2 opens the fewest capacity-sufficient Fs.  An 8-slot F alone satisfies six requested
    // slots, so lower-capacity alternatives may not enter this cohort's home set.
    {
        ReplayConfig c = config({8, 4, 2}, 6);
        const auto homes = home_set(c, opaque(30));
        check(homes.size() == 1 && homes[0] == 0,
              "R2 did not choose the minimum capacity-sufficient home set");
    }

    // Independent Fs never receive one another's installed-object credit.  Round-robin sends
    // the same closure to F0 and F1, and both pay it once.
    {
        ReplayConfig c = config({1, 1}, 2);
        const WireFragment object{11, 90, FragmentKind::Material};
        std::vector<Tu> trace{
            tu(0, 47, 0, {rep(p29::CandidateKind::Fi, 10, {object})}),
            tu(1, 48, 0, {rep(p29::CandidateKind::Fi, 10, {object})})};
        ReplayResult result = replay(c, trace, Policy::R0RoundRobin);
        check(result.c_to_f_bytes == 200 && result.rows[0].action.f != result.rows[1].action.f,
              "one F's materialization was credited to another F");
        check(std::abs(result.n_eff - 2.0) < 1e-12 &&
                  std::abs(result.route_entropy - 1.0) < 1e-12,
              "effective width/route entropy is not raw-byte weighted");
    }

    // Epoch reset discards only the selected F's usable state and route history.
    {
        ReplayConfig c = config({1});
        ReplayState state = initial_state(c);
        const WireFragment object{12, 90, FragmentKind::Material};
        Tu first = tu(0, 49, 0, {rep(p29::CandidateKind::Fi, 10, {object})});
        apply(c, state, first, evaluate(c, state, first, {0, 0}));
        check(physical_cost(state.fs[0], first, first.representations[0]).total_bytes == 10,
              "warm F did not retain its object");
        reset_f_epoch(state, 0, 2);
        check(state.fs[0].route_commits == 0 &&
                  physical_cost(state.fs[0], first, first.representations[0]).total_bytes == 100,
              "new FCacheEpoch retained stale usable state");
    }

    // Policies R0-R4 are prefix-causal: appending an unavailable suffix cannot alter prior
    // decisions or bytes.  R5 is intentionally separate and receives an explicit horizon.
    {
        ReplayConfig c = config({2, 1}, 2);
        c.time_weight_bytes = 1;
        c.time_weight_ns = 100;
        const WireFragment a{20, 25, FragmentKind::Material};
        std::vector<Tu> prefix{
            tu(0, 50, 300, {rep(p29::CandidateKind::Raw, 40),
                            rep(p29::CandidateKind::Fi, 10, {a})}),
            tu(1, 51, 200, {rep(p29::CandidateKind::Raw, 40),
                            rep(p29::CandidateKind::Fi, 10, {a})})};
        std::vector<Tu> extended = prefix;
        extended.push_back(tu(2, 52, 100, {rep(p29::CandidateKind::Raw, 1)}));
        for (Policy policy : {Policy::R0RoundRobin, Policy::R0Fastest, Policy::R1Resident,
                              Policy::R2Home, Policy::R3Rendezvous, Policy::R4StateAware}) {
            const ReplayResult short_run = replay(c, prefix, policy);
            const ReplayResult long_run = replay(c, extended, policy);
            check(short_run.rows.size() == 2 && long_run.rows.size() == 3,
                  "prefix fixture row count changed");
            for (size_t index = 0; index < prefix.size(); ++index)
                check(short_run.rows[index].action == long_run.rows[index].action &&
                          short_run.rows[index].cost.total_bytes ==
                              long_run.rows[index].cost.total_bytes,
                      "R0-R4 decision observed an unavailable suffix");
        }
    }

    // Exact enumeration must stop loudly rather than relabel a truncated search as R5 exact.
    {
        ReplayConfig c = config({1, 1}, 2);
        std::vector<Tu> trace{
            tu(0, 53, 0, {rep(p29::CandidateKind::Raw, 1)}),
            tu(1, 54, 0, {rep(p29::CandidateKind::Raw, 1)})};
        bool stopped = false;
        try {
            (void)exact_r5_frontier(c, trace, 1);
        } catch (const std::runtime_error&) {
            stopped = true;
        }
        check(stopped, "truncated R5 search was accepted as exact");
    }

    std::puts("P29 Phase-C R0-R5 chronological routing replay PASS");
    return 0;
}
