/*
    Deterministic scheduler-policy tests (no scheduler process required).

    The scoring, static-key, promotion and tie-break rules are the
    PRODUCTION functions from scheduler/selection.h, compiled into this
    test -- production cannot regress while this stays green, because
    there is exactly one statement of the rules.  Only the loop shape
    (walk groups, promotion first) is restated here, in the minimal form
    get_first_job_request() uses:

      - aging: a short job eventually outranks a stream of long ones;
      - hard promotion: a job past the promotion interval wins outright,
        oldest first, regardless of estimates;
      - priority: niceness dominates both of the above;
      - time invariance: the static key selects the same winner as the
        time-dependent score at any probe time (the property the indexed
        selector rests on).
*/

#include "../scheduler/selection.h"

#include <cstdio>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

static int failures = 0;

#define REQUIRE(cond, what)                                             \
    do {                                                                \
        if (cond) {                                                     \
            fprintf(stderr, "ok       - %s\n", what);                   \
        } else {                                                        \
            fprintf(stderr, "FAILED   - %s (at %s:%d)\n", what,         \
                    __FILE__, __LINE__);                                \
            ++failures;                                                 \
        }                                                               \
    } while (0)

// --- production rules from scheduler/selection.h, loop shape restated ----

static const time_t max_queue_wait_promotion_s =
    selection_max_queue_wait_promotion_msec / 1000;

struct Req {
    unsigned int id;
    int niceness;
    uint64_t estimate_msec;
    time_t enqueue;       // seconds on the test's abstract monotonic clock
};

static uint64_t score(const Req &r, time_t now)
{
    return selection_score(r.estimate_msec, uint64_t(r.enqueue) * 1000ULL,
                           uint64_t(now) * 1000ULL);
}

// The loop shape of get_first_job_request(): niceness first, then hard
// promotion by oldest enqueue, then the static-key comparator.
static const Req *select(const std::vector<Req> &reqs, time_t now)
{
    if (reqs.empty()) {
        return nullptr;
    }
    int best_niceness = reqs[0].niceness;
    for (const Req &r : reqs) {
        if (r.niceness < best_niceness) {
            best_niceness = r.niceness;
        }
    }

    const Req *overdue = nullptr;
    const Req *best = nullptr;
    int64_t best_key = 0;
    for (const Req &r : reqs) {
        if (r.niceness != best_niceness) {
            continue;
        }
        if (selection_overdue(uint64_t(r.enqueue) * 1000ULL,
                              uint64_t(now) * 1000ULL)) {
            if (!overdue || r.enqueue < overdue->enqueue
                    || (r.enqueue == overdue->enqueue && r.id < overdue->id)) {
                overdue = &r;
            }
            continue;
        }
        const int64_t key = selection_static_key(r.estimate_msec,
                                                 uint64_t(r.enqueue) * 1000ULL);
        if (!best || selection_prefers(key, r.id, best_key, best->id)) {
            best = &r;
            best_key = key;
        }
    }
    return overdue ? overdue : best;
}

// The indexed selector rests on this: the static key must pick the same
// winner as the time-dependent score, whatever the probe time.
static void test_time_invariance()
{
    /* All ages stay below the promotion window at every probe: promotion
       is deliberately out of scope here (it has its own tests) -- this
       property is about score-vs-key agreement.  */
    std::vector<Req> reqs;
    reqs.push_back({1, 0, 40000, 1000});
    reqs.push_back({2, 0, 10000, 995});
    reqs.push_back({3, 0, 25000, 998});
    reqs.push_back({4, 0, 25000, 998});   // exact tie with 3 -> id order
    for (time_t now : { time_t(1001), time_t(1020), time_t(1050) }) {
        const Req *by_key = select(reqs, now);
        const Req *by_score = nullptr;
        uint64_t best_s = 0;
        for (const Req &r : reqs) {
            const uint64_t s = score(r, now);
            if (!by_score || s > best_s || (s == best_s && r.id < by_score->id)) {
                by_score = &r;
                best_s = s;
            }
        }
        REQUIRE(by_key && by_score && by_key->id == by_score->id,
                "static-key winner equals score winner at every probe time");
    }
}

// --- tests ---------------------------------------------------------------

// A short job must not be starved by a continuous stream of long jobs.
static void test_no_starvation()
{
    const time_t t0 = 1000000;
    std::vector<Req> reqs;
    reqs.push_back({1, 0, 1000, t0});          // short job, waits

    bool short_selected = false;
    time_t now = t0;
    for (int round = 0; round < 200 && !short_selected; ++round) {
        now += 1;
        // a fresh long job arrives every second
        reqs.push_back({unsigned(100 + round), 0, 60000, now});
        const Req *sel = select(reqs, now);
        if (sel && sel->id == 1) {
            short_selected = true;
            fprintf(stderr, "# short job selected after %lds of waiting\n",
                    long(now - t0));
        }
    }
    REQUIRE(short_selected, "short job is selected despite a stream of long jobs");
    REQUIRE(short_selected && (now - t0) <= max_queue_wait_promotion_s + 1,
            "promotion happens within the promotion interval");
}

// Among overdue jobs the oldest wins, and estimates do not matter.
static void test_promotion_is_fifo()
{
    const time_t t0 = 1000000;
    const time_t now = t0 + max_queue_wait_promotion_s + 10;
    std::vector<Req> reqs;
    reqs.push_back({1, 0, 1000, t0});          // oldest, tiny estimate
    reqs.push_back({2, 0, 900000, t0 + 5});    // newer, huge estimate
    const Req *sel = select(reqs, now);
    REQUIRE(sel && sel->id == 1, "oldest overdue request wins over a larger estimate");
}

// Niceness dominates promotion and score.
static void test_niceness_first()
{
    const time_t t0 = 1000000;
    const time_t now = t0 + max_queue_wait_promotion_s + 10;
    std::vector<Req> reqs;
    reqs.push_back({1, 5, 1000, t0});            // overdue, but nicer (lower prio)
    reqs.push_back({2, 0, 1000, now - 1});       // fresh, top priority
    const Req *sel = select(reqs, now);
    REQUIRE(sel && sel->id == 2, "niceness outranks an overdue lower-priority request");
}

// Below the promotion interval, larger estimates go first (LPT).
static void test_lpt_below_promotion()
{
    const time_t t0 = 1000000;
    const time_t now = t0 + 5;
    std::vector<Req> reqs;
    reqs.push_back({1, 0, 1000, t0});
    reqs.push_back({2, 0, 50000, t0});
    const Req *sel = select(reqs, now);
    REQUIRE(sel && sel->id == 2, "longest-processing-time first while nothing is overdue");
}

int main()
{
    fprintf(stderr, "=== scheduler policy rules ===\n");
    test_no_starvation();
    test_time_invariance();
    test_promotion_is_fifo();
    test_niceness_first();
    test_lpt_below_promotion();

    if (failures) {
        fprintf(stderr, "RESULT: FAIL (%d)\n", failures);
        return 1;
    }
    fprintf(stderr, "RESULT: PASS\n");
    return 0;
}
