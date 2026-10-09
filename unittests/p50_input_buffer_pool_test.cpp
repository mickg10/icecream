// Unit tests for InputBufferPool (cache/p50_input_buffer_pool.h): the
// bounded free list that lets the F-side materialize buffer for one TU's
// InputRecord backing be reused by the next TU instead of being
// malloc'd/freed per transaction. No protocol, wire, or semantic change is
// under test here -- only pool bookkeeping (best fit, byte cap, the
// pooling-disabled no-op path) and that the production custom-deleter
// pattern (cache/p50_input_record.cpp's prepare_validated_publish()) can
// safely return a buffer to the pool from whichever thread drops the last
// reference, not just the thread that allocated it.

#include "cache/p50_input_buffer_pool.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using icecc::p50::InputBufferPool;

[[noreturn]] void fail(std::string_view text) {
    std::cerr << "p50_input_buffer_pool_test: " << text << '\n';
    std::exit(1);
}

void require(bool value, std::string_view text) {
    if (!value) fail(text);
}

// --- best fit --------------------------------------------------------------

void test_get_picks_smallest_fitting_buffer() {
    InputBufferPool pool(1u << 20);

    std::vector<std::uint8_t> small;
    small.reserve(100);
    std::vector<std::uint8_t> medium;
    medium.reserve(500);
    std::vector<std::uint8_t> large;
    large.reserve(2000);
    const std::uint8_t* const medium_data = medium.data();

    pool.put(std::move(small));
    pool.put(std::move(medium));
    pool.put(std::move(large));

    std::vector<std::uint8_t> fit = pool.get(300);
    require(fit.capacity() >= 300,
            "get() returned a buffer smaller than the requested capacity");
    require(fit.data() == medium_data,
            "get() did not pick the smallest free buffer that still fits "
            "the request (best fit)");
    require(fit.empty(),
            "get() returned a buffer with a nonzero size (put() must leave "
            "size() == 0 via clear(), capacity() unchanged)");

    const InputBufferPool::Stats stats = pool.stats();
    require(stats.hits == 1, "a satisfiable get() did not record a hit");
    require(stats.misses == 0,
            "a satisfiable get() incorrectly recorded a miss");

    // The two buffers left behind (small, large) must still be there: a
    // second request too big for "small" and too tight for nothing else
    // must fall through to "large".
    std::vector<std::uint8_t> second_fit = pool.get(2000);
    require(second_fit.capacity() >= 2000,
            "second get() returned an undersized buffer");

    std::vector<std::uint8_t> miss = pool.get(1u << 19);
    require(miss.capacity() == 0 && miss.empty(),
            "get() with no buffer big enough should return an empty vector, "
            "not a reserved one, leaving the caller's own reserve() path "
            "untouched");
    const InputBufferPool::Stats after_miss = pool.stats();
    require(after_miss.misses == 1,
            "an unsatisfiable get() did not record a miss");
}

// --- cap enforcement ---------------------------------------------------------

void test_put_enforces_the_byte_cap() {
    InputBufferPool pool(100);

    std::vector<std::uint8_t> first;
    first.reserve(60);
    pool.put(std::move(first));
    {
        const InputBufferPool::Stats stats = pool.stats();
        require(stats.bytes_held == 60,
                "put() did not retain a buffer that fit under the cap");
        require(stats.puts_dropped == 0,
                "put() dropped a buffer that fit under the cap");
    }

    // 60 (held) + 60 (this one) = 120 > 100 (cap): must be dropped, not
    // added to the free list, and must not perturb bytes_held.
    std::vector<std::uint8_t> second;
    second.reserve(60);
    pool.put(std::move(second));
    {
        const InputBufferPool::Stats stats = pool.stats();
        require(stats.bytes_held == 60,
                "put() exceeded its configured byte cap");
        require(stats.puts_dropped == 1,
                "put() did not count the buffer it dropped for exceeding "
                "the cap");
    }

    // A buffer that fits in the remaining headroom (100 - 60 = 40) must
    // still be accepted.
    std::vector<std::uint8_t> third;
    third.reserve(40);
    pool.put(std::move(third));
    {
        const InputBufferPool::Stats stats = pool.stats();
        require(stats.bytes_held == 100,
                "put() rejected a buffer that fit in the remaining cap "
                "headroom");
        require(stats.puts_dropped == 1,
                "put() dropped a buffer that fit in the remaining cap "
                "headroom");
    }
}

// --- 0 disables pooling ------------------------------------------------------

void test_zero_cap_disables_pooling() {
    InputBufferPool pool(0);
    require(!pool.enabled(), "cap_bytes=0 did not disable the pool");

    std::vector<std::uint8_t> buffer;
    buffer.reserve(4096);
    pool.put(std::move(buffer));  // disabled: freed normally, no bookkeeping

    std::vector<std::uint8_t> fetched = pool.get(1);
    require(fetched.capacity() == 0 && fetched.empty(),
            "a disabled pool returned a non-empty buffer from get(), "
            "instead of leaving the caller to reserve() as today");

    const InputBufferPool::Stats stats = pool.stats();
    require(stats.hits == 0 && stats.misses == 0 && stats.puts_dropped == 0 &&
                stats.bytes_held == 0,
            "a disabled pool recorded stats instead of behaving as a pure "
            "no-op on both get() and put()");
}

// --- cross-thread deleter return --------------------------------------------

// Models cache/p50_input_record.cpp's prepare_validated_publish() custom
// deleter exactly: the deleter captures a shared_ptr<InputBufferPool> and,
// when it runs, does nothing but move the backing vector into pool.put().
// It must run correctly -- and safely hand the buffer back to the pool --
// on whichever thread happens to drop the shared_ptr<InputRecord backing>'s
// last reference, which in production is usually InputBackingReaper's
// background thread, not the thread that published the record.
void test_deleter_returns_buffer_to_pool_from_another_thread() {
    auto pool = std::make_shared<InputBufferPool>(16u * 1024 * 1024);
    constexpr std::size_t kBytes = 4u * 1024 * 1024;

    std::mutex thread_mutex;
    std::thread::id deleter_thread{};
    std::atomic<bool> deleter_ran{false};

    auto* const raw_backing =
        new std::vector<std::uint8_t>(kBytes, std::uint8_t{0x5a});
    std::shared_ptr<std::vector<std::uint8_t>> backing(
        raw_backing,
        [pool, &thread_mutex, &deleter_thread,
         &deleter_ran](std::vector<std::uint8_t>* ptr) noexcept {
            std::unique_ptr<std::vector<std::uint8_t>> owned(ptr);
            {
                std::lock_guard<std::mutex> lock(thread_mutex);
                deleter_thread = std::this_thread::get_id();
            }
            pool->put(std::move(*owned));
            deleter_ran.store(true, std::memory_order_release);
        });

    const std::thread::id main_thread = std::this_thread::get_id();
    std::thread worker([thread_backing = std::move(backing)]() mutable {
        thread_backing.reset();  // last reference drops here
    });
    worker.join();

    require(deleter_ran.load(std::memory_order_acquire),
            "the custom deleter never ran after the backing's last "
            "reference dropped");
    {
        std::lock_guard<std::mutex> lock(thread_mutex);
        require(deleter_thread != main_thread,
                "the deleter ran on the main thread instead of the worker "
                "thread that actually dropped the last reference");
    }

    const InputBufferPool::Stats stats = pool->stats();
    require(stats.bytes_held == kBytes,
            "the cross-thread deleter did not hand the buffer back to the "
            "pool's free list");
    require(stats.puts_dropped == 0,
            "the cross-thread put() was unexpectedly dropped (cap too "
            "small for this test's buffer)");

    std::vector<std::uint8_t> reused = pool->get(kBytes);
    require(reused.capacity() >= kBytes,
            "the pool did not return the cross-thread-released buffer on a "
            "subsequent get()");
}

}  // namespace

int main() {
    test_get_picks_smallest_fitting_buffer();
    test_put_enforces_the_byte_cap();
    test_zero_cap_disables_pooling();
    test_deleter_returns_buffer_to_pool_from_another_thread();
    std::cout << "p50_input_buffer_pool_test: best-fit reuse, byte-cap "
                 "enforcement, pooling-disabled no-op, and cross-thread "
                 "deleter return all match the InputBufferPool contract\n";
    return 0;
}
