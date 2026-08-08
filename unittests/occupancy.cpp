/*
    Regression tests for the LEAST_BUSY occupancy comparison
    (scheduler/occupancy.h).

    Each case corresponds to a way the previous implementation chose
    wrongly: it initialised its minimum to zero (unsigned, so it could
    never rise), used a ceiling in one pass and a floor in the other, and
    compared bucketed integer quotients rather than the fractions
    themselves.
*/

#include "../scheduler/occupancy.h"

#include <vector>

struct HostOccupancy { int count; int max_jobs; };

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(bool ok, const char *what)
{
    if (ok) {
        printf("ok       - %s\n", what);
    } else {
        printf("FAILED   - %s\n", what);
        ++failures;
    }
}

/* The SAME selection loop production runs (least_busy_select), here
   instantiated -- as in production -- over a sequence of POINTERS, so the
   handles the loop copies around are cheap and identity-preserving.  */
static std::vector<size_t> pick(const std::vector<HostOccupancy> &hosts)
{
    std::vector<const HostOccupancy *> ptrs;
    for (const HostOccupancy &h : hosts) {
        ptrs.push_back(&h);
    }
    std::vector<const HostOccupancy *> out;
    least_busy_select(ptrs.begin(), ptrs.end(),
                      [](const HostOccupancy *h) { return h->count; },
                      [](const HostOccupancy *h) { return h->max_jobs; },
                      out);
    std::vector<size_t> idx;
    for (const HostOccupancy *p : out) {
        idx.push_back((size_t)(p - &hosts[0]));
    }
    return idx;
}

int main()
{
    /* An idle farm: every host ties at 0, so every host is a candidate and
       the caller distributes among them.  */
    {
        std::vector<HostOccupancy> h{{0, 4}, {0, 8}, {0, 2}};
        check(pick(h).size() == 3, "all-idle hosts tie regardless of capacity");
    }

    /* The plain case: same capacity, different load.  */
    {
        std::vector<HostOccupancy> h{{3, 4}, {1, 4}};
        const std::vector<size_t> got = pick(h);
        check(got.size() == 1 && got[0] == 1, "with equal capacity the emptier host wins");
    }

    /* Unequal denominators: 2/8 is emptier than 1/2 even though it holds
       MORE jobs.  Comparing bucketed quotients (both floor to 0) called
       these equal.  */
    {
        std::vector<HostOccupancy> h{{1, 2}, {2, 8}};
        const std::vector<size_t> got = pick(h);
        check(got.size() == 1 && got[0] == 1,
              "the lower fraction wins although it holds more jobs (2/8 beats 1/2)");
    }

    /* Exact normalized tie across different capacities.  */
    {
        std::vector<HostOccupancy> h{{1, 2}, {2, 4}, {4, 8}};
        check(pick(h).size() == 3, "exact normalized ties are all returned (1/2 == 2/4 == 4/8)");
    }

    /* A host at its limit is still a candidate: it may accept preload
       work.  The previous filter produced an EMPTY candidate set once
       every host reached its limit, and the scheduler then reported that
       no host was suitable while capacity remained.  */
    {
        std::vector<HostOccupancy> h{{4, 4}, {8, 8}};
        check(!pick(h).empty(), "hosts at their job limit remain candidates");
    }
    {
        std::vector<HostOccupancy> h{{6, 4}, {9, 8}};
        const std::vector<size_t> got = pick(h);
        check(!got.empty(), "hosts ABOVE their job limit remain candidates");
        check(got.size() == 1 && got[0] == 1, "and the relatively emptier one is chosen (9/8 < 6/4)");
    }

    /* Hosts with no job limit cannot compile: never candidates.  */
    {
        std::vector<HostOccupancy> h{{0, 0}, {3, 4}};
        const std::vector<size_t> got = pick(h);
        check(got.size() == 1 && got[0] == 1, "a host with no job limit is not a candidate");
    }
    {
        std::vector<HostOccupancy> h{{0, 0}, {0, 0}};
        check(pick(h).empty(), "no candidates when nothing has a job limit");
    }

    /* Large values must not overflow or wrap.  */
    {
        std::vector<HostOccupancy> h{{1000000, 2000000}, {999999, 2000000}};
        const std::vector<size_t> got = pick(h);
        check(got.size() == 1 && got[0] == 1, "large counts compare exactly");
    }
    {
        /* Near the signed-int limit: the products must stay exact in 64 bits.  */
        const int big = 2000000000;
        check(occupancy_less(1, big, 2, big), "1/2e9 < 2/2e9 at the int limit");
        check(occupancy_equal(big, big, 1, 1), "full is full regardless of capacity");
        check(!occupancy_less(big, big, 1, 2), "a full host is not emptier than a half-full one");
    }

    if (failures) {
        printf("RESULT: FAIL (%d)\n", failures);
        return 1;
    }
    printf("RESULT: PASS\n");
    return 0;
}
