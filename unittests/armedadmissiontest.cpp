/*
    ArmedAdmission (cache/p50_armed_admission.h): order, bounded head start,
    leaving the queue, and the slot bound under concurrency.
*/

#include "../cache/p50_armed_admission.h"

#include <atomic>
#include <memory>
#include <stdio.h>
#include <thread>
#include <vector>

using icecc::p50::service::ArmedAdmission;
using namespace std::chrono_literals;

static int failures = 0;

static void check(bool ok, const char *what)
{
    printf(ok ? "ok       - %s\n" : "FAILED   - %s\n", what);
    if (!ok) {
        ++failures;
    }
    fflush(stdout);
}

int main()
{
    const auto t0 = ArmedAdmission::Clock::now();
    const auto soon = [] { return ArmedAdmission::Clock::now() + 20ms; };
    const uint64_t big = ArmedAdmission::kHeadStartBytes;

    {
        ArmedAdmission gate(1);
        auto holder = std::make_unique<ArmedAdmission::Wait>(gate, 0, t0);
        check(holder->acquire_until(soon()), "a free slot is taken at once");
        ArmedAdmission::Wait small1(gate, 1000, t0 + 1s);
        ArmedAdmission::Wait large(gate, big, t0 + 2s);
        ArmedAdmission::Wait small2(gate, 1000, t0 + 3s);
        check(!large.acquire_until(soon()), "no slot while the only one is held");
        holder.reset();
        check(!small1.acquire_until(soon()), "an earlier small source waits for a later large one");
        check(large.acquire_until(soon()), "the large source takes the freed slot first");
        check(!small2.acquire_until(soon()), "a slot is held by one waiter at a time");
    }
    {
        ArmedAdmission gate(1);
        auto holder = std::make_unique<ArmedAdmission::Wait>(gate, 0, t0);
        holder->acquire_until(soon());
        ArmedAdmission::Wait early(gate, 1000, t0);
        ArmedAdmission::Wait late(gate, big, t0 + ArmedAdmission::kMaxHeadStart + 1s);
        holder.reset();
        check(!late.acquire_until(soon()), "the head start is bounded: a waiter is not overtaken by a much later one");
        check(early.acquire_until(soon()), "the earlier waiter goes first");
    }
    {
        ArmedAdmission gate(1);
        auto holder = std::make_unique<ArmedAdmission::Wait>(gate, 0, t0);
        holder->acquire_until(soon());
        ArmedAdmission::Wait first(gate, 5000, t0 + 1s);
        ArmedAdmission::Wait second(gate, 5000, t0 + 1s);
        auto quitter = std::make_unique<ArmedAdmission::Wait>(gate, big, t0);
        holder.reset();
        quitter.reset();
        check(!second.acquire_until(soon()), "equal keys keep arrival order");
        check(first.acquire_until(soon()), "a head that leaves the queue lets the next one in");
    }
    {
        ArmedAdmission gate(3);
        std::atomic<int> held{0}, peak{0}, done{0};
        std::vector<std::thread> threads;
        for (int i = 0; i < 40; ++i) {
            threads.emplace_back([&, i] {
                ArmedAdmission::Wait wait(gate, uint64_t(i) << 20, ArmedAdmission::Clock::now());
                while (!wait.acquire_until(ArmedAdmission::Clock::now() + 50ms)) {
                }
                const int now = ++held;
                for (int p = peak; now > p && !peak.compare_exchange_weak(p, now);) {
                }
                std::this_thread::sleep_for(2ms);
                --held;
                ++done;
            });
        }
        for (auto &thread : threads) {
            thread.join();
        }
        check(done == 40 && peak <= 3 && peak >= 1, "40 concurrent uploads all pass, at most 3 at once");
    }
    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
