/*
    Deterministic scheduler-policy tests (no scheduler process required).

    These exercise the selection rules directly through a small replica of
    the production ordering logic, so the every-commit tier covers fairness
    and traversal semantics that the integration harness can only observe
    indirectly:

      - aging: a short job eventually outranks a stream of long ones;
      - hard promotion: a job past the promotion interval wins outright,
        oldest first, regardless of estimates;
      - priority: niceness dominates both of the above.

    The replica is intentionally tiny and mirrors
    scheduler/scheduler.cpp's estimate_job_queue_score() and the promotion
    rule in get_first_job_request(); if those change, this file must change
    with them, which is the point -- the rules are then stated twice, in
    executable form.
*/

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

// --- replica of the production rules -------------------------------------

static const time_t max_queue_wait_promotion_s = 60;

struct Req {
    unsigned int id;
    int niceness;
    uint64_t estimate_msec;
    time_t enqueue;
};

static uint64_t score(const Req &r, time_t now)
{
    time_t age = now - r.enqueue;
    if (age < 0) {
        age = 0;
    }
    return r.estimate_msec + uint64_t(age) * 1000ULL / 2;
}

// Mirrors get_first_job_request(): niceness first, then hard promotion by
// oldest enqueue, then estimate-weighted score.
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
    uint64_t best_score = 0;
    for (const Req &r : reqs) {
        if (r.niceness != best_niceness) {
            continue;
        }
        if (now - r.enqueue >= max_queue_wait_promotion_s) {
            if (!overdue || r.enqueue < overdue->enqueue
                    || (r.enqueue == overdue->enqueue && r.id < overdue->id)) {
                overdue = &r;
            }
            continue;
        }
        const uint64_t s = score(r, now);
        if (!best || s > best_score || (s == best_score && r.id < best->id)) {
            best = &r;
            best_score = s;
        }
    }
    return overdue ? overdue : best;
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
