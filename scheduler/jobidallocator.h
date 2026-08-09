/*
    This file is part of Icecream.

    Checked scheduler wire-id allocator (issue #4 ruling), unit-tested in
    unittests/jobidalloctest.cpp.

    One instance serves BOTH ownership classes that consume
    scheduler-visible ids -- remote jobs and local monitor records -- so
    neither class can be issued an id the other still holds.  The
    replaced scheme was ++counter with a debug-only assert against the
    remote map alone: release builds could silently replace a live remote
    job on wrap (the ABA family), local ids were invisible to the guard,
    and id 0 (the protocol sentinel) was reachable after wrap.

    Icecream is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#ifndef ICECREAM_JOBIDALLOCATOR_H
#define ICECREAM_JOBIDALLOCATOR_H

#include <set>
#include <stddef.h>
#include <stdint.h>

class JobIdAllocator
{
public:
    explicit JobIdAllocator(uint32_t max_id = 0xffffffffu)
        : m_maxId(max_id ? max_id : 1)
    {
    }

    /* A reserved id in 1..max_id not currently live, or 0 for explicit
       exhaustion (0 itself is never a valid id).  The caller publishes no
       object or message before this returns: reservation precedes
       publication.  Scans at most live_count + 1 distinct candidates --
       among that many distinct nonzero cyclic candidates at most
       live_count can be live -- so a full domain never loops.  */
    uint32_t allocate()
    {
        if (m_live.size() >= m_maxId) {
            return 0;   /* every id in the domain is live */
        }
        uint32_t candidate = m_cursor;
        for (;;) {
            ++candidate;
            if (candidate > m_maxId || candidate == 0) {
                candidate = 1;   /* wrap max_id -> 1; 0 never considered */
            }
            if (m_live.insert(candidate).second) {
                m_cursor = candidate;
                ++m_issuedTotal;
                return candidate;
            }
        }
    }

    /* Exactly once per allocation: releasing an id that is not live is an
       invariant failure reported to the caller.  */
    bool release(uint32_t id)
    {
        return m_live.erase(id) == 1;
    }

    bool contains(uint32_t id) const
    {
        return m_live.find(id) != m_live.end();
    }

    size_t liveCount() const
    {
        return m_live.size();
    }

    size_t freeCount() const
    {
        return (size_t)m_maxId - m_live.size();
    }

    /* Cumulative successful allocations: 64-bit, monotonic, and never
       itself used as an id -- the diagnostics counter the wrapping
       cursor can no longer provide.  */
    uint64_t issuedTotal() const
    {
        return m_issuedTotal;
    }

private:
    uint32_t m_maxId;
    uint32_t m_cursor = 0;
    std::set<uint32_t> m_live;
    uint64_t m_issuedTotal = 0;
};

#endif
