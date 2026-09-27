/*
    ArmedAdmission (cache/p50_armed_admission.h): order, bounded head start,
    leaving the queue, expiry and stop with every slot held, and the slot bound
    under concurrency.
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
        ArmedAdmission gate(1);
        auto holder = std::make_unique<ArmedAdmission::Wait>(gate, 0, t0);
        holder->acquire_until(soon());
        auto expiring = std::make_unique<ArmedAdmission::Wait>(gate, big, t0);
        const auto start = ArmedAdmission::Clock::now();
        const bool got = expiring->acquire_until(start + 100ms);
        const auto waited = ArmedAdmission::Clock::now() - start;
        check(!got && waited >= 100ms && waited < 1s, "a wait expires at its limit while every slot stays held");
        ArmedAdmission::Wait later(gate, 0, t0 + 1s);
        expiring.reset();
        holder.reset();
        check(later.acquire_until(soon()), "an expired waiter that leaves does not keep the next one out");
    }
    {
        ArmedAdmission gate(1);
        auto holder = std::make_unique<ArmedAdmission::Wait>(gate, 0, t0);
        holder->acquire_until(soon());
        auto head = std::make_unique<ArmedAdmission::Wait>(gate, 0, t0);
        std::atomic<bool> acquired{false};
        ArmedAdmission::Clock::time_point acquired_at{};
        std::thread successor([&] {
            ArmedAdmission::Wait wait(gate, 0, t0 + 1s);
            if (wait.acquire_until(ArmedAdmission::Clock::now() + 5s)) {
                acquired_at = ArmedAdmission::Clock::now();
                acquired = true;
            }
        });
        std::this_thread::sleep_for(50ms);
        // The freed slot is the head's, and the head is not waiting for it.
        holder.reset();
        std::this_thread::sleep_for(50ms);
        const bool held_back = !acquired;
        const auto left_at = ArmedAdmission::Clock::now();
        head.reset();
        successor.join();
        check(held_back && acquired && acquired_at - left_at < 500ms,
              "a head that leaves wakes the successor blocked on the freed slot");
    }
    {
        ArmedAdmission gate(2);
        auto first = std::make_unique<ArmedAdmission::Wait>(gate, 0, t0);
        auto second = std::make_unique<ArmedAdmission::Wait>(gate, 0, t0);
        first->acquire_until(soon());
        second->acquire_until(soon());
        std::atomic<bool> stop{false};
        std::atomic<int> left{0};
        std::vector<std::thread> queued;
        for (int i = 0; i < 10; ++i) {
            queued.emplace_back([&, i] {
                ArmedAdmission::Wait wait(gate, uint64_t(i) << 20, ArmedAdmission::Clock::now());
                // The service's loop: turns of 50 ms, each checking for stop.
                while (!stop && !wait.acquire_until(ArmedAdmission::Clock::now() + 50ms)) {
                }
                ++left;
            });
        }
        std::this_thread::sleep_for(50ms);
        const auto stopped_at = ArmedAdmission::Clock::now();
        stop = true;
        for (auto &thread : queued) {
            thread.join();
        }
        check(left == 10 && ArmedAdmission::Clock::now() - stopped_at < 500ms,
              "stopping ends every queued wait within one turn");
        first.reset();
        second.reset();
        ArmedAdmission::Wait a(gate, 0, ArmedAdmission::Clock::now());
        ArmedAdmission::Wait b(gate, 0, ArmedAdmission::Clock::now());
        check(a.acquire_until(soon()) && b.acquire_until(soon()),
              "after a stop the gate has every slot back and nobody queued");
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
