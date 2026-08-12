/*
    Direct unit test for the shared local-client selection comparator
    (daemon/clientselect.h), issue #4 fairness gate.

    Exercises the EXACT pure function the daemon selection path
    (Daemon::Clients::get_earliest_client) uses, over the required rows:

      1. later client id + better niceness  -> the better-niceness (later id) wins
      2. equal niceness + lower client id   -> the lower id wins
      3. lower client id + worse niceness   -> the worse niceness does NOT win
      4. old-conjunction control            -> the historical rule gets the
                                               crossed-priority row wrong

    RED against the former conjunction (client_outranks_conjunction), GREEN with the
    corrected lexicographic rule (client_outranks).
*/
#include <cstdint>
#include <cstdio>
#include <vector>

#include "clientselect.h"

namespace {

/* Historical (buggy) conjunction rule -- test-local, reproduced only for the negative
   control (it no longer ships in the product header): the former code replaced the best
   only when the candidate improved on BOTH id AND niceness. */
bool client_outranks_conjunction(uint32_t cand_niceness, int cand_id,
                                 bool have_best,
                                 uint32_t best_niceness, int best_id)
{
    if (!have_best) {
        return true;
    }
    return cand_id < best_id && cand_niceness < best_niceness;
}

struct Cand { uint32_t niceness; int id; };

/* Pick the winning client id from candidates in the given order, mirroring
   get_earliest_client's fold, using the supplied comparator. */
int pick(const std::vector<Cand> &cs,
         bool (*outranks)(uint32_t, int, bool, uint32_t, int))
{
    bool have = false;
    uint32_t best_n = 0;
    int best_id = -1;
    for (const Cand &c : cs) {
        if (outranks(c.niceness, c.id, have, best_n, best_id)) {
            best_n = c.niceness;
            best_id = c.id;
            have = true;
        }
    }
    return best_id;
}

int failures = 0;
void check(bool ok, const char *what)
{
    fprintf(stderr, "%s - %s\n", ok ? "ok      " : "FAILED  ", what);
    if (!ok) {
        ++failures;
    }
}

} // namespace

int main()
{
    /* 1. later client id + better niceness -> the better-niceness (later id) wins. */
    check(pick({{5, 1}, {3, 2}}, client_outranks) == 2,
          "later id with better (lower) niceness wins");

    /* order-independence of the corrected rule: reversing the input is unchanged. */
    check(pick({{3, 2}, {5, 1}}, client_outranks) == 2,
          "same winner regardless of iteration order");

    /* 2. equal niceness + lower client id -> the lower id wins. */
    check(pick({{4, 7}, {4, 3}}, client_outranks) == 3,
          "equal niceness -> lower client id wins");

    /* 3. lower client id + worse niceness -> the worse niceness does NOT win. */
    check(pick({{2, 9}, {8, 1}}, client_outranks) == 9,
          "lower id with worse (higher) niceness does not win");

    /* 4. old-conjunction control: on the crossed-priority row the historical rule
          keeps the lower-priority id=1 instead of the higher-priority id=2, and
          depends on iteration order; the corrected rule is right and stable. */
    check(pick({{5, 1}, {3, 2}}, client_outranks_conjunction) == 1,
          "CONTROL: conjunction rule gets the crossed-priority row wrong (keeps id=1)");
    check(pick({{3, 2}, {5, 1}}, client_outranks_conjunction) == 2,
          "CONTROL: conjunction rule is iteration-order dependent (reversed -> id=2)");
    check(pick({{5, 1}, {3, 2}}, client_outranks) == 2,
          "corrected rule fixes the crossed-priority row (id=2 wins)");

    if (failures) {
        fprintf(stderr, "RESULT: FAIL [clientselect] (%d failures)\n", failures);
        return 1;
    }
    fprintf(stderr, "RESULT: PASS [clientselect] (0 failures)\n");
    return 0;
}
