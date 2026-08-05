/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
    This file is part of Icecream.

    Scheduler request-selection primitives, shared with the policy unit
    tests: the production comparator and deadline rule live HERE and only
    here, so a test that goes green while production regresses is
    impossible for these rules (the divergence review's requirement).

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#ifndef ICECREAM_SCHEDULER_SELECTION_H
#define ICECREAM_SCHEDULER_SELECTION_H

#include <stdint.h>

/* Maximum time a request may wait behind higher-scoring work at the same
   niceness before it is promoted unconditionally.  Aging as a numeric bonus
   (however large) only guarantees eventual promotion; a hard rule makes the
   worst case explicit and testable, which is what an operator can reason
   about.  */
static const int64_t selection_max_queue_wait_promotion_msec = 60 * 1000;

/* Time-dependent score: estimate + age/2, in msec.  The age bonus is
   deliberately UNBOUNDED: with a cap of one estimate a short job (estimate
   S, max score 2S) is starved forever by a sustained stream of jobs whose
   estimate exceeds 2S.  Unbounded aging guarantees every queued request
   eventually outranks any fixed-estimate newcomer.  Niceness remains the
   first-order key (group ordering, outside these primitives).  */
static inline uint64_t selection_score(uint64_t estimate_snapshot_msec,
                                       uint64_t enqueue_mono_msec,
                                       uint64_t now_mono_msec)
{
    const uint64_t age_msec = now_mono_msec > enqueue_mono_msec
                              ? now_mono_msec - enqueue_mono_msec : 0;
    return estimate_snapshot_msec + age_msec / 2;
}

/* Static selection key: score(t) differences carry no time term, so the
   order at ANY t is fully decided by K = estimate - enqueue/2.  Signed
   64-bit: the monotonic clock and any plausible estimate are far below
   2^62, so the subtraction cannot wrap.  */
static inline int64_t selection_static_key(uint64_t estimate_snapshot_msec,
                                           uint64_t enqueue_mono_msec)
{
    return (int64_t)estimate_snapshot_msec - (int64_t)(enqueue_mono_msec / 2);
}

/* Deadline rule: overdue requests outrank all normal scoring.  */
static inline bool selection_overdue(uint64_t enqueue_mono_msec,
                                     uint64_t now_mono_msec)
{
    return now_mono_msec >= enqueue_mono_msec
        && now_mono_msec - enqueue_mono_msec
               >= (uint64_t)selection_max_queue_wait_promotion_msec;
}

/* Comparator: does candidate a win over candidate b?  Higher key first;
   equal keys resolve to the smaller job id (deterministic FIFO-ish tie).  */
static inline bool selection_prefers(int64_t key_a, unsigned id_a,
                                     int64_t key_b, unsigned id_b)
{
    if (key_a != key_b) {
        return key_a > key_b;
    }
    return id_a < id_b;
}

#endif
