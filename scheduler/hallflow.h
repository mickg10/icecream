/*
    This file is part of Icecream.

    Phase H0 of the eligibility-aware fairness oracle (issue #4): a pure,
    deterministic max-flow feasibility check over a capacitated bipartite
    eligibility graph, unit-tested in unittests/hallflowtest.cpp.

    The aggregate dispatch-credit clamp checks only TOTAL remote slots --
    the Hall cut X = L.  It cannot see a proper subset of eligibility
    classes whose entire worker neighborhood is monopolized while
    unrelated capacity stays free (the model's eligibility-blind-credit
    counterexample).  This helper answers the exact question: after a
    candidate reservation consumes one slot on its chosen worker, does a
    declared protected demand (typically one dispatch opportunity per
    starved class) remain feasible?  If not, it returns the residual
    min-cut as a concrete Hall witness:

        sum_{q in X} p(q)  >  sum_{f in N(X)} free_after_candidate(f)

    Pure integers, no scheduler types: left classes are hard eligibility
    signatures (platform/environment/features/policy -- never transient
    load or speed), right nodes are remote-capable workers with their
    free slot counts.  Observe-only in phase H1; policy only in H2.

    Icecream is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#ifndef ICECREAM_HALLFLOW_H
#define ICECREAM_HALLFLOW_H

#include <stddef.h>
#include <functional>
#include <vector>

struct HallWitness {
    bool feasible;
    /* INFEASIBLE only: the violating class set X, its worker
       neighborhood N(X), and both sides of the Hall inequality.  */
    std::vector<size_t> signature_set;      // indices into the left side
    std::vector<size_t> worker_neighborhood; // indices into the right side
    long long protected_demand;              // sum p(q), q in X
    long long available_capacity;            // sum free(f), f in N(X)
};

/* Feasibility of the protected demand vector over the eligibility graph.

   demands   : p(q) >= 0 per left class (a bounded quantum, NOT a backlog)
   free_slots: free capacity per worker AFTER the candidate reservation
   edges     : edges[q] = worker indices that can EVER run class q

   Dinic max flow on source -> q (cap p(q)), q -> f (cap unbounded),
   f -> sink (cap free(f)); feasible iff maxflow == sum p(q).  On
   infeasibility, the source-side residual cut yields the witness, whose
   inequality the caller (and the unit test) can recompute independently.
   Deterministic: iteration order is index order; no randomness, no
   floating point.  */
inline HallWitness hall_feasible(const std::vector<long long> &demands,
                                 const std::vector<long long> &free_slots,
                                 const std::vector<std::vector<size_t> > &edges)
{
    const size_t nq = demands.size();
    const size_t nf = free_slots.size();
    /* nodes: 0 = source, 1..nq = classes, nq+1..nq+nf = workers, last = sink */
    const size_t S = 0, T = nq + nf + 1, N = nq + nf + 2;
    struct Edge { size_t to; long long cap; size_t rev; };
    std::vector<std::vector<Edge> > g(N);
    auto add_edge = [&](size_t a, size_t b, long long cap) {
        g[a].push_back(Edge{b, cap, g[b].size()});
        g[b].push_back(Edge{a, 0, g[a].size() - 1});
    };
    long long want = 0;
    for (size_t q = 0; q < nq; ++q) {
        if (demands[q] > 0) {
            add_edge(S, 1 + q, demands[q]);
            want += demands[q];
        }
    }
    const long long kInf = 1LL << 60;
    for (size_t q = 0; q < nq; ++q) {
        for (const size_t f : edges[q]) {
            if (f < nf) {
                add_edge(1 + q, 1 + nq + f, kInf);
            }
        }
    }
    for (size_t f = 0; f < nf; ++f) {
        if (free_slots[f] > 0) {
            add_edge(1 + nq + f, T, free_slots[f]);
        }
    }

    /* Dinic */
    std::vector<int> level(N);
    std::vector<size_t> iter(N);
    auto bfs = [&]() -> bool {
        for (size_t i = 0; i < N; ++i) {
            level[i] = -1;
        }
        std::vector<size_t> queue;
        queue.push_back(S);
        level[S] = 0;
        for (size_t h = 0; h < queue.size(); ++h) {
            const size_t v = queue[h];
            for (const Edge &e : g[v]) {
                if (e.cap > 0 && level[e.to] < 0) {
                    level[e.to] = level[v] + 1;
                    queue.push_back(e.to);
                }
            }
        }
        return level[T] >= 0;
    };
    std::function<long long(size_t, long long)> dfs =
        [&](size_t v, long long f) -> long long {
        if (v == T) {
            return f;
        }
        for (size_t &i = iter[v]; i < g[v].size(); ++i) {
            Edge &e = g[v][i];
            if (e.cap > 0 && level[v] < level[e.to]) {
                const long long d = dfs(e.to, f < e.cap ? f : e.cap);
                if (d > 0) {
                    e.cap -= d;
                    g[e.to][e.rev].cap += d;
                    return d;
                }
            }
        }
        return 0;
    };
    long long flow = 0;
    while (bfs()) {
        for (size_t i = 0; i < N; ++i) {
            iter[i] = 0;
        }
        long long f;
        while ((f = dfs(S, kInf)) > 0) {
            flow += f;
        }
    }

    HallWitness w;
    w.feasible = (flow == want);
    w.protected_demand = 0;
    w.available_capacity = 0;
    if (!w.feasible) {
        /* Residual reachability from the source gives the min cut; the
           reachable left classes with positive demand form X, and their
           FULL eligibility neighborhoods form N(X).  */
        std::vector<bool> seen(N, false);
        std::vector<size_t> queue;
        queue.push_back(S);
        seen[S] = true;
        for (size_t h = 0; h < queue.size(); ++h) {
            const size_t v = queue[h];
            for (const Edge &e : g[v]) {
                if (e.cap > 0 && !seen[e.to]) {
                    seen[e.to] = true;
                    queue.push_back(e.to);
                }
            }
        }
        std::vector<bool> in_nbhd(nf, false);
        for (size_t q = 0; q < nq; ++q) {
            if (seen[1 + q] && demands[q] > 0) {
                w.signature_set.push_back(q);
                w.protected_demand += demands[q];
                for (const size_t f : edges[q]) {
                    if (f < nf && !in_nbhd[f]) {
                        in_nbhd[f] = true;
                    }
                }
            }
        }
        for (size_t f = 0; f < nf; ++f) {
            if (in_nbhd[f]) {
                w.worker_neighborhood.push_back(f);
                w.available_capacity += free_slots[f] > 0 ? free_slots[f] : 0;
            }
        }
    }
    return w;
}

#endif
