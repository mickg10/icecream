/*
    JobIdAllocator: pure-class tests (issue #4 allocator ruling), covering
    the ruled deterministic gate on the tiny {1,2,3} domain plus wrap,
    exactly-once release, and the issued counter.
*/

#include "../scheduler/jobidallocator.h"

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

int main()
{
    /* The ruled tiny-domain gate.  */
    {
        JobIdAllocator a(3);
        const uint32_t i1 = a.allocate();
        const uint32_t i2 = a.allocate();
        const uint32_t i3 = a.allocate();
        check(i1 == 1 && i2 == 2 && i3 == 3,
              "the {1,2,3} domain allocates each id once, in order");
        check(a.allocate() == 0,
              "the fourth allocation reports explicit exhaustion");
        check(a.liveCount() == 3, "all three ids are live");
        check(a.release(2), "id 2 releases");
        const uint32_t r = a.allocate();
        check(r == 2, "only the released id becomes reusable");
        check(a.allocate() == 0, "the domain is exhausted again");
        check(!a.release(9), "releasing an id that was never issued fails");
        check(a.release(2) && !a.release(2),
              "release succeeds exactly once; the double release fails");
        check(a.issuedTotal() == 4,
              "the issued counter counts allocations, not live ids");
    }

    /* Zero is never issued, wrap goes max -> 1.  */
    {
        JobIdAllocator a(5);
        for (int i = 0; i < 5; ++i) {
            check(a.allocate() != 0, "allocation in an open domain is nonzero");
        }
        check(a.release(1) && a.release(2), "two releases");
        const uint32_t w1 = a.allocate();
        const uint32_t w2 = a.allocate();
        check(w1 == 1 && w2 == 2,
              "the cursor wraps past max_id to 1 and skips live ids");
    }

    /* The scan bound: a nearly-full large domain still allocates the one
       free id promptly (live_count + 1 candidates at most).  */
    {
        JobIdAllocator a(100000);
        for (int i = 0; i < 100000; ++i) {
            a.allocate();
        }
        check(a.allocate() == 0, "full 100k domain is exhausted");
        check(a.release(70000), "release one");
        check(a.allocate() == 70000,
              "the single free id in a 100k-live domain is found");
    }

    /* contains() reflects liveness.  */
    {
        JobIdAllocator a(4);
        const uint32_t id = a.allocate();
        check(a.contains(id), "an allocated id is contained");
        a.release(id);
        check(!a.contains(id), "a released id is not contained");
    }

    if (failures) {
        printf("RESULT: FAIL (%d)\n", failures);
        return 1;
    }
    if (checks_executed == 0) {
        printf("RESULT: FAIL (no assertions executed)\n");
        return 1;
    }
    printf("# jobidalloc: %d assertions executed\n", checks_executed);
    printf("RESULT: PASS\n");
    return 0;
}
