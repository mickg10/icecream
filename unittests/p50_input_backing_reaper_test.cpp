// Unit tests for the off-owner-thread input-backing free path: the
// InputBackingReaper itself, and InputRecordStore's graveyard-deferral
// contract in collect_garbage()/evict_namespace(). No protocol, wire, or
// semantic change is under test here -- only that bulk frees of large
// InputRecord backings can be moved off whatever thread collected them,
// with identical bookkeeping to the inline (pre-fix) behavior.

#include "cache/p50_input_backing_reaper.h"
#include "cache/p50_input_record.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace icecc::p50;
using Clock = std::chrono::steady_clock;

[[noreturn]] void fail(std::string_view text) {
    std::cerr << "p50_input_backing_reaper_test: " << text << '\n';
    std::exit(1);
}

void require(bool value, std::string_view text) {
    if (!value) fail(text);
}

long long elapsed_ms_since(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now() - start)
        .count();
}

// Records which thread destroyed each backing, after an artificial delay --
// the stand-in for a real multi-MB-to-GB input buffer's free cost. Shared via
// shared_ptr so every copy of the deleter functor observes the same log.
struct DeleteLog {
    std::mutex mutex;
    std::vector<std::thread::id> deleter_threads;

    size_t count() {
        std::lock_guard<std::mutex> lock(mutex);
        return deleter_threads.size();
    }

    bool saw_only_thread(std::thread::id expected) {
        std::lock_guard<std::mutex> lock(mutex);
        for (std::thread::id id : deleter_threads) {
            if (id != expected) return false;
        }
        return !deleter_threads.empty();
    }

    bool saw_any_other_thread(std::thread::id excluded) {
        std::lock_guard<std::mutex> lock(mutex);
        for (std::thread::id id : deleter_threads) {
            if (id != excluded) return true;
        }
        return false;
    }
};

struct SlowDeleter {
    std::shared_ptr<DeleteLog> log;
    std::chrono::milliseconds delay{300};

    void operator()(const std::vector<uint8_t>* ptr) const {
        std::this_thread::sleep_for(delay);
        {
            std::lock_guard<std::mutex> lock(log->mutex);
            log->deleter_threads.push_back(std::this_thread::get_id());
        }
        delete ptr;
    }
};

std::shared_ptr<const std::vector<uint8_t>> make_slow_backing(
    size_t bytes, std::shared_ptr<DeleteLog> log,
    std::chrono::milliseconds delay = std::chrono::milliseconds(300)) {
    return std::shared_ptr<const std::vector<uint8_t>>(
        new std::vector<uint8_t>(bytes, 0xAB), SlowDeleter{std::move(log), delay});
}

bool wait_for_count(DeleteLog& log, size_t expected,
                    std::chrono::milliseconds timeout) {
    const Clock::time_point deadline = Clock::now() + timeout;
    while (log.count() < expected) {
        if (Clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

// --- InputBackingReaper direct tests --------------------------------------

void test_submit_returns_fast_and_frees_off_caller_thread() {
    auto log = std::make_shared<DeleteLog>();
    InputBackingReaper reaper;
    const std::thread::id caller = std::this_thread::get_id();

    InputBackingBatch batch;
    batch.push_back(make_slow_backing(1u << 20, log));

    const Clock::time_point start = Clock::now();
    require(reaper.submit(std::move(batch)),
            "reaper rejected a batch with room in the queue");
    const long long submit_ms = elapsed_ms_since(start);
    require(submit_ms < 50,
            "submit() blocked on the backing's slow deleter instead of "
            "handing it to the reaper thread");

    require(wait_for_count(*log, 1, std::chrono::seconds(2)),
            "reaper never ran the deferred deleter");
    require(log->saw_any_other_thread(caller),
            "deferred deleter ran on the submitting (caller) thread, not "
            "the reaper thread");
}

void test_destructor_drains_everything() {
    auto log = std::make_shared<DeleteLog>();
    static constexpr int kBatches = 3;
    {
        InputBackingReaper reaper(/*max_queued_batches=*/8);
        for (int i = 0; i < kBatches; ++i) {
            InputBackingBatch batch;
            batch.push_back(make_slow_backing(
                1u << 16, log, std::chrono::milliseconds(150)));
            require(reaper.submit(std::move(batch)),
                    "reaper rejected a batch under its queue cap");
        }
        // Destructor runs here: must drain the queue and join the worker
        // before returning, so every deleter above has already run by the
        // time this scope exits.
    }
    require(log->count() == static_cast<size_t>(kBatches),
            "reaper destructor returned before draining all queued batches");
}

void test_full_queue_falls_back_to_inline() {
    // The accepted (busy/queued) batches and the rejected (overflow) batch
    // get separate logs. Both batches race against the reaper's worker
    // thread on real wall-clock timing, so there is no way to assert
    // anything about `log`'s contents at a specific moment without a
    // flaky dependency on how far the 400ms/50ms async frees have
    // progressed. `fallback_log` is only ever touched by this thread's own
    // inline overflow_batch.clear(), so it is race-free and gives an exact,
    // deterministic check that the fallback ran synchronously, here, on
    // the caller thread.
    auto log = std::make_shared<DeleteLog>();
    auto fallback_log = std::make_shared<DeleteLog>();
    const std::thread::id caller = std::this_thread::get_id();
    InputBackingReaper reaper(/*max_queued_batches=*/1);

    // Occupy the worker thread with a long-running free so the queue stays
    // observably full for the rest of this test.
    InputBackingBatch busy_batch;
    busy_batch.push_back(
        make_slow_backing(1u << 16, log, std::chrono::milliseconds(400)));
    require(reaper.submit(std::move(busy_batch)),
            "reaper rejected the first batch into an empty queue");
    // Give the worker time to pop it and enter the slow deleter.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    InputBackingBatch queued_batch;
    queued_batch.push_back(
        make_slow_backing(1u << 16, log, std::chrono::milliseconds(50)));
    require(reaper.submit(std::move(queued_batch)),
            "reaper rejected a batch that should still fit under the cap "
            "while the worker is busy");

    InputBackingBatch overflow_batch;
    overflow_batch.push_back(make_slow_backing(
        1u << 16, fallback_log, std::chrono::milliseconds(10)));
    require(!reaper.submit(std::move(overflow_batch)),
            "reaper accepted a batch past its queue cap");
    require(!overflow_batch.empty(),
            "submit() consumed a rejected batch instead of leaving it for "
            "the caller to free inline");

    // Inline fallback: the caller frees it right here, synchronously, on
    // this (the test) thread -- exactly what collect_input_garbage() does
    // when InputBackingReaper::submit() returns false. Checked against its
    // own dedicated log, so this assertion cannot race the worker thread's
    // unrelated frees of busy_batch/queued_batch on `log`.
    const Clock::time_point start = Clock::now();
    overflow_batch.clear();
    (void)elapsed_ms_since(start);  // informational only; inline is allowed to block.
    {
        std::lock_guard<std::mutex> lock(fallback_log->mutex);
        require(fallback_log->deleter_threads.size() == 1 &&
                    fallback_log->deleter_threads.back() == caller,
                "fallback free did not run synchronously on the caller "
                "thread after a full-queue rejection");
    }

    require(wait_for_count(*log, 2, std::chrono::seconds(2)),
            "reaper never finished draining the batches that were accepted");
    // Now that both accepted batches are confirmed freed, the sanity check
    // that at least one of those frees ran off the caller thread is
    // deterministic rather than a race against the worker.
    require(log->saw_only_thread(caller) == false,
            "sanity: the accepted batches should have been freed by the "
            "reaper's worker thread, not the caller");
}

// --- InputRecordStore graveyard-deferral tests ----------------------------

InputRecordKey key_for(uint64_t guid, uint64_t tu) {
    return InputRecordKey{CStoreGuid::from_u64(guid), TuSeq{tu}};
}

#if defined(ICECC_P50_INPUT_RECORD_TEST_HOOKS)

void populate_slow_closed_records(InputRecordStore& store, uint64_t guid,
                                  int count, std::shared_ptr<DeleteLog> log,
                                  size_t bytes_each = 1u << 20) {
    for (int i = 0; i < count; ++i) {
        store.test_only_insert_closed_record(
            key_for(guid, static_cast<uint64_t>(i)), bytes_each,
            make_slow_backing(bytes_each, log));
    }
}

void test_collect_garbage_defers_frees_off_the_calling_thread() {
    static constexpr int kRecords = 5;
    static constexpr size_t kBytesEach = 1u << 20;  // 1 MiB per record

    // "Before": no graveyard supplied -- collect_garbage() frees each
    // backing inline, in place, exactly as it did before this change.
    auto inline_log = std::make_shared<DeleteLog>();
    {
        InputRecordStore store(/*max_records=*/16, /*max_retained_bytes=*/
                               uint64_t{1} << 30);
        populate_slow_closed_records(store, /*guid=*/9001, kRecords,
                                     inline_log, kBytesEach);
        require(store.record_count() == kRecords,
                "setup did not insert all inline-path records");

        const Clock::time_point start = Clock::now();
        store.collect_garbage(/*graveyard=*/nullptr);
        const long long inline_ms = elapsed_ms_since(start);
        std::cout << "p50_input_backing_reaper_test: collect_garbage "
                      "inline (before fix shape), N="
                  << kRecords << ": " << inline_ms << "ms\n";
        require(inline_ms >= 200,
                "inline collect_garbage() returned implausibly fast for "
                "N slow 300ms deleters -- test setup is not exercising the "
                "owner-thread-blocking path it is meant to reproduce");
        require(store.record_count() == 0 && store.retained_bytes() == 0,
                "inline collect_garbage() left accounting inconsistent");
        require(inline_log->count() == static_cast<size_t>(kRecords),
                "inline collect_garbage() did not free every backing");
    }

    // "After": a graveyard is supplied -- collect_garbage() must return
    // quickly, moving the backings out instead of destroying them in place.
    auto deferred_log = std::make_shared<DeleteLog>();
    InputRecordStore store(/*max_records=*/16, /*max_retained_bytes=*/
                           uint64_t{1} << 30);
    populate_slow_closed_records(store, /*guid=*/9002, kRecords, deferred_log,
                                 kBytesEach);
    require(store.record_count() == kRecords,
            "setup did not insert all deferred-path records");

    InputBackingBatch graveyard;
    const Clock::time_point start = Clock::now();
    store.collect_garbage(&graveyard);
    const long long deferred_ms = elapsed_ms_since(start);
    std::cout << "p50_input_backing_reaper_test: collect_garbage "
                  "deferred (after fix), N="
              << kRecords << ": " << deferred_ms << "ms\n";
    require(deferred_ms < 50,
            "collect_garbage(&graveyard) blocked on the owner thread "
            "instead of moving backings into the graveyard");
    require(store.record_count() == 0 && store.retained_bytes() == 0,
            "deferred collect_garbage() left different accounting than the "
            "inline path");
    require(deferred_log->count() == 0,
            "deferred collect_garbage() freed a backing before the "
            "graveyard was destroyed/handed off");
    require(graveyard.size() == kRecords,
            "deferred collect_garbage() did not move every released "
            "backing into the graveyard");

    // Hand the graveyard to a real reaper, exactly as P50ServerEndpoint's
    // collect_input_garbage() does, and confirm the work still happens --
    // just off the calling (owner) thread.
    const std::thread::id caller = std::this_thread::get_id();
    InputBackingReaper reaper;
    require(reaper.submit(std::move(graveyard)),
            "reaper rejected the collected graveyard batch");
    require(wait_for_count(*deferred_log, kRecords, std::chrono::seconds(3)),
            "reaper never freed the deferred graveyard batch");
    require(deferred_log->saw_any_other_thread(caller),
            "graveyard frees ran on the owner thread instead of the "
            "reaper thread");
}

void test_evict_namespace_defers_frees_with_identical_accounting() {
    static constexpr int kPerNamespace = 2;
    static constexpr size_t kBytesEach = 1u << 19;  // 512 KiB per record
    auto log = std::make_shared<DeleteLog>();

    InputRecordStore store(/*max_records=*/16, /*max_retained_bytes=*/
                           uint64_t{1} << 30);
    populate_slow_closed_records(store, /*guid=*/9101, kPerNamespace, log,
                                 kBytesEach);
    populate_slow_closed_records(store, /*guid=*/9102, kPerNamespace, log,
                                 kBytesEach);
    const uint64_t total_before = store.retained_bytes();

    InputBackingBatch graveyard;
    const Clock::time_point start = Clock::now();
    store.evict_namespace(CStoreGuid::from_u64(9101), &graveyard);
    const long long elapsed = elapsed_ms_since(start);
    require(elapsed < 50,
            "evict_namespace(&graveyard) blocked on the owner thread");
    require(store.namespace_record_count(CStoreGuid::from_u64(9101)) == 0,
            "evict_namespace() left records behind in the evicted namespace");
    require(store.namespace_record_count(CStoreGuid::from_u64(9102)) ==
                kPerNamespace,
            "evict_namespace() touched a namespace it was not asked to");
    require(store.retained_bytes() ==
                total_before - kPerNamespace * kBytesEach,
            "evict_namespace(&graveyard) changed retained_bytes_ accounting "
            "relative to the inline contract");
    require(graveyard.size() == kPerNamespace,
            "evict_namespace(&graveyard) did not move every evicted "
            "namespace's backings into the graveyard");
    require(log->count() == 0,
            "evict_namespace(&graveyard) freed a backing before the "
            "graveyard was destroyed");

    graveyard.clear();  // inline drain for this test's own cleanup
    require(log->count() == static_cast<size_t>(kPerNamespace),
            "clearing the graveyard did not free the deferred backings");
}

#endif  // ICECC_P50_INPUT_RECORD_TEST_HOOKS

}  // namespace

int main() {
    test_submit_returns_fast_and_frees_off_caller_thread();
    test_destructor_drains_everything();
    test_full_queue_falls_back_to_inline();
#if defined(ICECC_P50_INPUT_RECORD_TEST_HOOKS)
    test_collect_garbage_defers_frees_off_the_calling_thread();
    test_evict_namespace_defers_frees_with_identical_accounting();
#else
    std::cout << "p50_input_backing_reaper_test: built without "
                 "ICECC_P50_INPUT_RECORD_TEST_HOOKS; skipping "
                 "InputRecordStore-level graveyard tests\n";
#endif
    std::cout << "p50_input_backing_reaper_test: owner-thread input backing "
                 "frees are deferred and account identically to the inline "
                 "path\n";
    return 0;
}
