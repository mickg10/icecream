/*
    FASTEST stale-refresh policy: pure-function tests (issue #4, the
    confirmed dead-code finding).

    The old expression computed its threshold fraction in uint8_t integer
    arithmetic -- (255 - STATS_UPDATE_WEIGHT) / 255 == 0 -- so the
    stale-host refresh branch could never execute.  It is kept here,
    verbatim, as the red negative control: over the whole sampled domain
    it must never select a stale host, while the replacement satisfies
    every documented monotonicity.
*/

#include "../scheduler/fastest.h"

#include <limits>
#include <stdint.h>
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

/* The replaced expression, verbatim semantics.  */
static bool old_expression_refreshes(unsigned int job_id,
                                     unsigned int last_picked_id,
                                     unsigned int weight,
                                     size_t eligible)
{
    uint8_t weight_limit = std::numeric_limits<uint8_t>::max() - weight;
    uint8_t weight_factor = weight_limit / std::numeric_limits<uint8_t>::max();
    return weight_factor > 0
           && (!last_picked_id
               || ((job_id - last_picked_id) > (weight_factor * eligible)));
}

int main()
{
    const unsigned int kWeight = 120;   /* STATS_UPDATE_WEIGHT */

    /* NEGATIVE CONTROL: the old expression never refreshes -- not for a
       never-picked host, not at any distance, any eligible size, any
       documented weight.  */
    {
        bool ever = false;
        for (unsigned int w = 0; w <= 254 && !ever; ++w) {
            for (unsigned long long d = 0; d <= 100000 && !ever; d += 997) {
                for (size_t e = 1; e <= 800 && !ever; e = e * 2 + 1) {
                    if (old_expression_refreshes(1000000 + (unsigned int)d,
                                                 1000000, w, e)
                        || old_expression_refreshes(42, 0, w, e)) {
                        ever = true;
                    }
                }
            }
        }
        check(!ever, "negative control: the replaced expression never"
                     " selects a stale host anywhere in the sampled domain");
    }

    /* Monotonicity 1: higher weight -> refreshes never more often, and
       strictly rarer somewhere.  */
    {
        bool monotone = true;
        bool strictly_rarer_somewhere = false;
        for (unsigned long long d = 1; d <= 4096; d *= 2) {
            for (unsigned long long e = 1; e <= 1024; e *= 4) {
                bool prev = fastest_should_refresh(d, 0, e);
                for (unsigned int w = 1; w <= 255; ++w) {
                    const bool cur = fastest_should_refresh(d, w, e);
                    if (cur && !prev) {
                        monotone = false;
                    }
                    if (prev && !cur) {
                        strictly_rarer_somewhere = true;
                    }
                    prev = cur;
                }
            }
        }
        check(monotone, "higher weight never makes a refresh MORE likely");
        check(strictly_rarer_somewhere,
              "higher weight makes refreshes strictly rarer somewhere");
    }

    /* Monotonicity 2: distance eventually forces a refresh (no permanent
       under-sampling) for any weight < 255 and finite eligible set.  */
    {
        bool forced = true;
        for (unsigned int w = 0; w <= 254; w += 2) {
            for (unsigned long long e = 1; e <= 800; e = e * 3 + 1) {
                /* the inequality flips no later than w*e/255 + 1 */
                const unsigned long long bound = (unsigned long long)w * e / 255ULL + 1;
                if (!fastest_should_refresh(bound, w, e)) {
                    forced = false;
                }
            }
        }
        check(forced, "a stale host is refreshed after at most"
                      " weight*eligible/255 + 1 picks");
    }

    /* Monotonicity 3: a larger eligible set spaces refreshes wider.  */
    {
        bool wider = true;
        for (unsigned long long d = 1; d <= 4096; d *= 2) {
            bool prev = fastest_should_refresh(d, kWeight, 1);
            for (unsigned long long e = 2; e <= 2048; e *= 2) {
                const bool cur = fastest_should_refresh(d, kWeight, e);
                if (cur && !prev) {
                    wider = false;
                }
                prev = cur;
            }
        }
        check(wider, "a larger eligible set never makes a refresh MORE likely");
    }

    /* Boundary values.  */
    check(fastest_should_refresh(1, 0, 800),
          "weight 0: any nonzero distance refreshes");
    check(!fastest_should_refresh(0, 0, 1),
          "weight 0: zero distance does not refresh");
    check(!fastest_should_refresh(5, 128, 10)
              && fastest_should_refresh(6, 128, 10),
          "weight 128: the threshold sits at about half the eligible set");
    check(fastest_should_refresh((unsigned long long)1 << 40, 254, 800),
          "64-bit distances do not overflow the cross-multiplication");

    /* A never-picked host (distance = whole pick history).  */
    check(fastest_should_refresh(100000, kWeight, 13),
          "a never-picked host with real history refreshes immediately");

    /* The production weight actually refreshes: the exact regression that
       was dead code.  */
    check(fastest_should_refresh(400, kWeight, 13),
          "at the production weight a stale host among 13 workers is"
          " refreshed (the replaced expression never did this)");

    if (failures) {
        printf("RESULT: FAIL (%d)\n", failures);
        return 1;
    }
    if (checks_executed == 0) {
        printf("RESULT: FAIL (no assertions executed)\n");
        return 1;
    }
    printf("# fastest: %d assertions executed\n", checks_executed);
    printf("RESULT: PASS\n");
    return 0;
}
