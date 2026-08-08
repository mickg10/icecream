/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
    This file is part of Icecream.

    Occupancy comparison for the LEAST_BUSY scheduler algorithm, kept
    separate from the scheduler so the arithmetic can be tested directly.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#ifndef ICECREAM_SCHEDULER_OCCUPANCY_H
#define ICECREAM_SCHEDULER_OCCUPANCY_H

#include <stdint.h>
#include <stddef.h>

/* Is occupancy a/a_max strictly lower than b/b_max?  Compared exactly by
   64-bit cross multiplication: no division, so no bucketing of distinct
   fractions into the same integer quotient, and no rounding direction to
   get wrong.  For eligible servers the counts and limits are
   signed-int-range values with a positive denominator, so the products fit
   comfortably in uint64_t.

   Callers must exclude hosts with no job limit (max_jobs <= 0); such a
   host cannot compile and has no meaningful occupancy.  */
static inline bool occupancy_less(int a_count, int a_max, int b_count, int b_max)
{
    const uint64_t an = (uint64_t)(a_count > 0 ? a_count : 0);
    const uint64_t bn = (uint64_t)(b_count > 0 ? b_count : 0);
    return an * (uint64_t)b_max < bn * (uint64_t)a_max;
}

/* Exactly equal occupancy -- the tie the caller distributes among.  */
static inline bool occupancy_equal(int a_count, int a_max, int b_count, int b_max)
{
    const uint64_t an = (uint64_t)(a_count > 0 ? a_count : 0);
    const uint64_t bn = (uint64_t)(b_count > 0 ? b_count : 0);
    return an * (uint64_t)b_max == bn * (uint64_t)a_max;
}

/* One-pass least-busy selection over [begin, end): appends the exact-tie
   set of lowest-occupancy candidates to `out` (cleared first).  Candidates
   with no job limit are skipped -- they cannot compile.  This is THE
   selection loop: the scheduler's pick_server_least_busy() and the
   regression test both instantiate it, so the test cannot silently drift
   from what production runs.  */
template <typename Iter, typename GetCount, typename GetMax, typename Out>
inline void least_busy_select(Iter begin, Iter end,
                              GetCount get_count, GetMax get_max, Out &out)
{
    out.clear();
    int best_count = 0;
    int best_max = 0;
    for (Iter it = begin; it != end; ++it) {
        const int mj = get_max(*it);
        if (mj <= 0) {
            continue;
        }
        const int cnt = get_count(*it);
        if (out.empty() || occupancy_less(cnt, mj, best_count, best_max)) {
            out.clear();
            out.push_back(*it);
            best_count = cnt;
            best_max = mj;
        } else if (occupancy_equal(cnt, mj, best_count, best_max)) {
            out.push_back(*it);
        }
    }
}

#endif
