/*
    Shared pure comparator for choosing which waiting/queued local client next
    receives a freed compile slot (issue #4 fairness gate).

    The intended order is lexicographic: strictly lower niceness (higher priority)
    wins; on equal niceness the lower client id wins.  This is the SAME rule the
    LINKJOB/scheduler ordering uses.  It is factored out here as a pure function so the
    daemon selection path (Daemon::Clients::get_earliest_client) and a direct unit test
    exercise the EXACT same logic.

    The historical selection used a conjunction -- replace the best only when the
    candidate had BOTH a lower id AND a lower niceness -- so a higher-priority
    (lower-niceness) client with a higher id never won, and crossed priorities resolved
    by hash-map iteration order.  That broken rule is reproduced as a test-local
    function in unittests/clientselect.cpp solely to demonstrate the crossed-priority
    row it gets wrong; only the corrected comparator lives in the product header.
*/
#ifndef ICECREAM_CLIENTSELECT_H
#define ICECREAM_CLIENTSELECT_H

#include <cstdint>

/* Correct order: candidate replaces the current best iff there is no best yet, or the
   candidate has strictly lower niceness, or equal niceness and a strictly lower id. */
static inline bool client_outranks(uint32_t cand_niceness, int cand_id,
                                   bool have_best,
                                   uint32_t best_niceness, int best_id)
{
    if (!have_best) {
        return true;
    }
    if (cand_niceness != best_niceness) {
        return cand_niceness < best_niceness;
    }
    return cand_id < best_id;
}

#endif /* ICECREAM_CLIENTSELECT_H */
