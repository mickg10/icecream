/*
    This file is part of Icecream.

    Copyright (c) 2026 the Icecream maintainers

    Pure FASTEST stale-refresh policy, unit-tested in
    unittests/fastesttest.cpp.

    Icecream is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#ifndef ICECREAM_FASTEST_H
#define ICECREAM_FASTEST_H

/* Should this pick REFRESH a host whose statistics have gone stale,
   instead of exploiting the currently-fastest host?  Dimensionless
   integer inequality, 64-bit cross-multiplied (no float, no overflow for
   any realistic inputs):

       picks_since_last * 255  >  staleness_weight * eligible_count

   i.e. refresh once a host's picks-since-last-selection exceeds
   (weight/255) picks per eligible host.  Monotonicities, each covered by
   a test:

   - higher staleness_weight  -> refreshes strictly rarer (more
     exploitation of fresh statistics);
   - larger picks_since_last  -> eventually forces a refresh for any
     weight < 255 and finite eligible set (no host stays permanently
     under-sampled);
   - larger eligible set      -> refreshes spaced proportionally wider;
   - weight 0                 -> any nonzero distance refreshes;
   - weight 128               -> refresh once the distance exceeds about
     half the eligible-set size;
   - a never-picked host passes the full pick history as its distance and
     refreshes immediately.

   The distance clock is a dedicated monotonic 64-bit PICK SEQUENCE (one
   increment per assignment recorded), NOT the wire job id: job ids can
   wrap or restart, so id distance is not a clock.

   The expression this replaces computed the threshold fraction in a
   uint8_t ((255 - weight) / 255 == 0 in integer math), so the refresh
   branch could never execute and a cold or formerly-slow host stayed
   under-sampled forever; unittests/fastesttest.cpp keeps that expression
   as a red negative control.  */
static inline bool fastest_should_refresh(unsigned long long picks_since_last,
                                          unsigned int staleness_weight,
                                          unsigned long long eligible_count)
{
    return picks_since_last * 255ULL
           > (unsigned long long)staleness_weight * eligible_count;
}

#endif
