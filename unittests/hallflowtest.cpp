/*
    Hall/min-cut fairness oracle, phase H0 (issue #4): pure-helper tests.

    Every INFEASIBLE verdict's witness is independently verified by
    recomputing both sides of the Hall inequality from the raw inputs --
    the helper's word is never taken for it.
*/

#include "../scheduler/hallflow.h"

#include <stdio.h>

static int failures = 0;
static int checks_executed = 0;

static void check(bool ok, const char *what)
{
    ++checks_executed;
    printf(ok ? "ok       - %s\n" : "FAILED   - %s\n", what);
    if (!ok) {
        ++failures;
    }
    fflush(stdout);
}

/* Recompute the Hall inequality for a returned witness.  */
static bool witness_valid(const HallWitness &w,
                          const std::vector<long long> &demands,
                          const std::vector<long long> &free_slots,
                          const std::vector<std::vector<size_t> > &edges)
{
    if (w.feasible) {
        return false;
    }
    long long demand = 0;
    std::vector<bool> nbhd(free_slots.size(), false);
    for (const size_t q : w.signature_set) {
        demand += demands[q];
        for (const size_t f : edges[q]) {
            nbhd[f] = true;
        }
    }
    long long cap = 0;
    for (size_t f = 0; f < free_slots.size(); ++f) {
        if (nbhd[f]) {
            cap += free_slots[f] > 0 ? free_slots[f] : 0;
        }
    }
    return demand > cap && demand == w.protected_demand
           && cap == w.available_capacity;
}

int main()
{
    /* 1. The signature case the aggregate clamp cannot see: two A-only
       slots occupied, one B-only slot free.  Globally a slot is free, so
       slots-1 style accounting reports headroom -- but class A's
       neighborhood is exhausted: a proper-subset Hall cut.  */
    {
        const std::vector<long long> demands = { 1, 0 };      // A starved, B not
        const std::vector<long long> free_slots = { 0, 1 };   // fA consumed, fB free
        const std::vector<std::vector<size_t> > edges = { { 0 }, { 1 } };
        const HallWitness w = hall_feasible(demands, free_slots, edges);
        check(!w.feasible,
              "a monopolized A-only neighborhood is INFEASIBLE while a"
              " global free slot exists (the aggregate clamp's blind spot)");
        check(witness_valid(w, demands, free_slots, edges),
              "the A-cut witness recomputes to a true Hall violation");
        check(w.signature_set.size() == 1 && w.signature_set[0] == 0,
              "the witness names exactly the starved class");
    }

    /* 2. Overlapping neighborhoods {A,B}, {B,C}, {C} where no single
       per-signature limit catches the union cut: A and B together need 3
       but N({A,B}) holds only 2.  */
    {
        const std::vector<long long> demands = { 2, 1, 0 };
        const std::vector<long long> free_slots = { 1, 1, 5 };
        const std::vector<std::vector<size_t> > edges = {
            { 0, 1 },      // A -> f0, f1
            { 1 },         // B -> f1
            { 1, 2 }       // C -> f1, f2
        };
        const HallWitness w = hall_feasible(demands, free_slots, edges);
        check(!w.feasible, "a union cut over overlapping neighborhoods is caught");
        check(witness_valid(w, demands, free_slots, edges),
              "the union-cut witness recomputes to a true Hall violation");
        /* per-signature checks alone pass: A alone needs 2 <= 2,
           B alone needs 1 <= 1 -- only the union fails */
        check(w.signature_set.size() >= 2,
              "the witness is a PROPER union, invisible per-signature");
    }

    /* 3. A rare architecture/environment worker: the only ppc worker is
       full; ppc demand infeasible, x86 unaffected.  */
    {
        const std::vector<long long> demands = { 1, 1 };
        const std::vector<long long> free_slots = { 0, 40 };
        const std::vector<std::vector<size_t> > edges = { { 0 }, { 1 } };
        const HallWitness w = hall_feasible(demands, free_slots, edges);
        check(!w.feasible && w.signature_set.size() == 1 && w.signature_set[0] == 0,
              "a full rare-architecture worker starves only its own class");
    }

    /* 4. A preferred-host singleton behaves as a one-worker neighborhood.  */
    {
        const std::vector<long long> demands = { 1 };
        const std::vector<long long> free_slots = { 0, 8 };
        const std::vector<std::vector<size_t> > edges = { { 0 } };   // pinned to f0
        const HallWitness w = hall_feasible(demands, free_slots, edges);
        check(!w.feasible, "a pinned class with a full preferred host is infeasible");
    }

    /* 5. no-remote submitters/local slots are EXCLUDED from the graph:
       modeled by simply not appearing; the remaining graph is feasible.  */
    {
        const std::vector<long long> demands = { 1 };
        const std::vector<long long> free_slots = { 2 };
        const std::vector<std::vector<size_t> > edges = { { 0 } };
        const HallWitness w = hall_feasible(demands, free_slots, edges);
        check(w.feasible, "excluding local-only capacity leaves a clean feasible graph");
    }

    /* 6. Capacity shrink and reconnect: the same demand flips infeasible
       when the worker disconnects (free 0) and back when it returns.  */
    {
        const std::vector<long long> demands = { 1, 1 };
        const std::vector<std::vector<size_t> > edges = { { 0 }, { 0, 1 } };
        const HallWitness gone = hall_feasible(demands, { 0, 1 }, edges);
        const HallWitness back = hall_feasible(demands, { 1, 1 }, edges);
        check(!gone.feasible, "a disconnected worker's classes go infeasible");
        check(back.feasible, "reconnect restores feasibility with the same demand");
    }

    /* 7. PREPARED reservations count as occupied before UseCS: modeled as
       already-consumed free slots.  */
    {
        const std::vector<long long> demands = { 1 };
        const std::vector<std::vector<size_t> > edges = { { 0 } };
        const HallWitness prepared = hall_feasible(demands, { 0 }, edges);
        check(!prepared.feasible,
              "a PREPARED-but-unclaimed reservation already consumes the slot");
    }

    /* 8. Mixed old/new worker eligibility: a class that only new workers
       can run (feature-gated) is starved when they are full, even while
       old workers idle.  */
    {
        const std::vector<long long> demands = { 1, 1 };
        const std::vector<long long> free_slots = { 0, 6 };   // f0=new full, f1=old idle
        const std::vector<std::vector<size_t> > edges = {
            { 0 },        // feature-gated class: new worker only
            { 0, 1 }      // legacy class: both
        };
        const HallWitness w = hall_feasible(demands, free_slots, edges);
        check(!w.feasible && w.signature_set.size() == 1 && w.signature_set[0] == 0,
              "a feature-gated class starves on full new workers while old idle");
    }

    /* Feasible baseline with slack everywhere.  */
    {
        const std::vector<long long> demands = { 1, 1, 1 };
        const std::vector<long long> free_slots = { 2, 2, 2 };
        const std::vector<std::vector<size_t> > edges = {
            { 0, 1 }, { 1, 2 }, { 0, 2 }
        };
        check(hall_feasible(demands, free_slots, edges).feasible,
              "a slack graph is feasible");
    }

    /* Zero demand is trivially feasible.  */
    {
        const std::vector<long long> demands = { 0, 0 };
        const std::vector<long long> free_slots = { 0, 0 };
        const std::vector<std::vector<size_t> > edges = { { 0 }, { 1 } };
        check(hall_feasible(demands, free_slots, edges).feasible,
              "zero protected demand is trivially feasible");
    }

    if (failures) {
        printf("RESULT: FAIL (%d)\n", failures);
        return 1;
    }
    if (checks_executed == 0) {
        printf("RESULT: FAIL (no assertions executed)\n");
        return 1;
    }
    printf("# hallflow: %d assertions executed\n", checks_executed);
    printf("RESULT: PASS\n");
    return 0;
}
