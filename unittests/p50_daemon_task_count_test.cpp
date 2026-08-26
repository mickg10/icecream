#include "daemon/p50_task_count.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <thread>

namespace {

[[noreturn]] void fail(std::string_view detail)
{
    std::cerr << "p50_daemon_task_count_test: " << detail << '\n';
    std::exit(1);
}

void require(bool condition, std::string_view detail)
{
    if (!condition)
        fail(detail);
}

}

int main()
{
#ifdef __linux__
    const auto baseline = iceccd_task_count();
    require(baseline.has_value() && *baseline == 1,
            "single-task process was not counted exactly");

    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::thread hidden_daemon_thread([&] {
        entered.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire))
            std::this_thread::yield();
    });
    while (!entered.load(std::memory_order_acquire))
        std::this_thread::yield();
    const auto with_hidden_thread = iceccd_task_count();
    release.store(true, std::memory_order_release);
    hidden_daemon_thread.join();

    require(with_hidden_thread.has_value() && *with_hidden_thread >= 2,
            "hidden daemon thread mutant was not observable at fork audit");
    require(iceccd_task_count() == baseline,
            "task audit did not return to the single-task baseline");
#else
    require(!iceccd_task_count().has_value(),
            "non-Linux task audit invented an authoritative count");
#endif
    std::cout << "p50_daemon_task_count_test: PASS\n";
    return 0;
}

