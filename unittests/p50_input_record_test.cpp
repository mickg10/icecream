#include "cache/p50_input_record.h"
#include "cache/p50_profile.h"
#include "cache/p50_zstd.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace allocation_probe {

bool enabled = false;
size_t count = 0;

}  // namespace allocation_probe

void* operator new(std::size_t size) {
    void* allocation = std::malloc(size == 0 ? 1 : size);
    if (!allocation) throw std::bad_alloc();
    if (allocation_probe::enabled) ++allocation_probe::count;
    return allocation;
}

void* operator new[](std::size_t size) {
    void* allocation = std::malloc(size == 0 ? 1 : size);
    if (!allocation) throw std::bad_alloc();
    if (allocation_probe::enabled) ++allocation_probe::count;
    return allocation;
}

void operator delete(void* allocation) noexcept {
    std::free(allocation);
}

void operator delete[](void* allocation) noexcept {
    std::free(allocation);
}

void operator delete(void* allocation, std::size_t) noexcept {
    std::free(allocation);
}

void operator delete[](void* allocation, std::size_t) noexcept {
    std::free(allocation);
}

namespace {

using namespace icecc::p50;

[[noreturn]] void fail(std::string_view text) {
    std::cerr << "p50_input_record_test: " << text << '\n';
    std::exit(1);
}

void require(bool value, std::string_view text) {
    if (!value) fail(text);
}

template<class Exception, class Callable>
void require_throws(Callable&& callable, std::string_view text) {
    try {
        callable();
    } catch (const Exception&) {
        return;
    } catch (...) {
        fail(std::string(text) + " (wrong exception)");
    }
    fail(std::string(text) + " (no exception)");
}

std::vector<uint8_t> bytes(size_t count, uint32_t salt) {
    std::vector<uint8_t> result(count);
    for (size_t index = 0; index != result.size(); ++index)
        result[index] = static_cast<uint8_t>(
            (index * 97 + index / 23 + salt) & 0xff);
    return result;
}

struct ExactTransaction {
    TxBegin begin;
    TxCommit commit;
};

ExactTransaction transaction_for(TuSeq tu_seq,
                                 std::span<const uint8_t> exact_input,
                                 RelSeq rel_seq = RelSeq{}) {
    static constexpr std::array<uint8_t, 3> encoded_body{0x50, 0x35, 0x30};

    TxBegin begin;
    begin.history_nonce = HistoryNonce{17};
    begin.rel_seq = rel_seq;
    begin.tu_seq = tu_seq;
    begin.profile = ProfileId::ZSTD_TU;
    begin.pre_state_digest = icecc::digest128("input-record-pre-state");
    begin.body = describe_component(
        static_cast<uint16_t>(ProfileId::ZSTD_TU), encoded_body,
        exact_input.size());
    begin.raw_bytes = exact_input.size();
    begin.raw_digest = icecc::digest128(exact_input);
    begin.transaction_digest = compute_transaction_digest(begin, encoded_body);

    TxCommit commit{
        begin.history_nonce,
        begin.rel_seq,
        begin.tu_seq,
        begin.transaction_digest,
        begin.raw_digest,
        compute_post_state_digest(begin.pre_state_digest,
                                  begin.history_nonce,
                                  begin.rel_seq,
                                  begin.tu_seq,
                                  begin.transaction_digest)};
    return {begin, commit};
}

std::vector<uint8_t> drain(InputCursor& cursor, size_t chunk_size = 31) {
    std::vector<uint8_t> result;
    std::vector<uint8_t> chunk(chunk_size);
    while (!cursor.eof()) {
        const size_t count = cursor.read(chunk);
        require(count != 0, "non-EOF InputCursor made no progress");
        result.insert(result.end(), chunk.begin(), chunk.begin() + count);
    }
    require(cursor.read(chunk) == 0, "EOF InputCursor produced more bytes");
    return result;
}

InputPublishResult commit_without_allocations(
    InputRecordStore& store,
    InputRecordStore::PreparedPublish prepared) {
    const size_t before = allocation_probe::count;
    allocation_probe::enabled = true;
    try {
        const InputPublishResult result =
            store.commit_prepared(std::move(prepared));
        allocation_probe::enabled = false;
        require(allocation_probe::count == before,
                "prepared InputRecord commit allocated at linearization");
        return result;
    } catch (...) {
        allocation_probe::enabled = false;
        throw;
    }
}

TxCommit commit_for(const TxBegin& begin) {
    return {
        begin.history_nonce,
        begin.rel_seq,
        begin.tu_seq,
        begin.transaction_digest,
        begin.raw_digest,
        compute_post_state_digest(begin.pre_state_digest, begin.history_nonce,
                                  begin.rel_seq, begin.tu_seq,
                                  begin.transaction_digest),
    };
}

VerifiedMaterialization verified_zstd_tu(
    const ZstdTuEnvelope& envelope, uint64_t max_raw_bytes) {
    ProfileDialogue dialogue = ProfileDialogue::create(
        ProfileId::ZSTD_TU,
        {
            .negotiated_profiles = profile_bit(ProfileId::ZSTD_TU),
            .c_store_guid = CStoreGuid::from_u64(700),
            .max_encoded_body_bytes = uint64_t{1} << 20,
            .max_raw_bytes = max_raw_bytes,
            .max_window_log = 20,
            .max_history_bytes = uint64_t{1} << 20,
        });
    dialogue.begin(envelope.begin);
    dialogue.append_body(BodyMessage{envelope.body});
    return dialogue.materialize_verified();
}

void test_verified_materialization_binds_begin_commit_and_bytes() {
    static_assert(!std::is_copy_constructible_v<VerifiedMaterialization>);
    static_assert(!std::is_copy_assignable_v<VerifiedMaterialization>);
    static_assert(std::is_nothrow_move_constructible_v<VerifiedMaterialization>);

    const CStoreGuid c_guid = CStoreGuid::from_u64(701);
    const std::vector<uint8_t> input = bytes(64 * 1024, 37);
    const ZstdTuLimits codec_limits{uint64_t{1} << 20,
                                    uint64_t{1} << 20, 20,
                                    uint64_t{1} << 20};
    const ZstdTuEnvelope envelope = encode_zstd_tu(
        HistoryNonce{23}, RelSeq{4}, TuSeq{702},
        icecc::digest128("verified-publish-pre-state"), input, 1,
        codec_limits);
    const TxCommit commit = commit_for(envelope.begin);

    TxBegin wrong_begin = envelope.begin;
    wrong_begin.tu_seq = TuSeq{wrong_begin.tu_seq.value + 1};
    require_throws<std::invalid_argument>(
        [&] {
            (void)InputRecordStore::prepare_verified_publish(
                c_guid, wrong_begin, commit,
                verified_zstd_tu(envelope, codec_limits.max_raw_bytes));
        },
        "verified materialization was accepted for a different TX_BEGIN");

    TxCommit wrong_commit = commit;
    wrong_commit.raw_digest.bytes[0] ^= 0x80;
    require_throws<std::invalid_argument>(
        [&] {
            (void)InputRecordStore::prepare_verified_publish(
                c_guid, envelope.begin, wrong_commit,
                verified_zstd_tu(envelope, codec_limits.max_raw_bytes));
        },
        "verified materialization was accepted for a different TX_COMMIT");

    InputRecordStore store(2, uint64_t{1} << 20);
    InputRecordStore::PreparedPublish prepared =
        InputRecordStore::prepare_verified_publish(
            c_guid, envelope.begin, commit,
            verified_zstd_tu(envelope, codec_limits.max_raw_bytes));
    require(std::ranges::equal(prepared.exact_input(), input),
            "verified publication authority changed reconstructed bytes");
    require(store.commit_prepared(std::move(prepared)) ==
                InputPublishResult::Published,
            "matching verified materialization did not publish");
    InputCursor retained = store.attach({c_guid, envelope.begin.tu_seq});
    require(drain(retained, 997) == input,
            "verified publication retained bytes other than the ZSTD_TU result");
}

void test_prepared_publish_is_exact_one_shot_and_allocation_free() {
    static_assert(!std::is_copy_constructible_v<
                  InputRecordStore::PreparedPublish>);
    static_assert(!std::is_copy_assignable_v<
                  InputRecordStore::PreparedPublish>);
    static_assert(std::is_nothrow_move_constructible_v<
                  InputRecordStore::PreparedPublish>);
    static_assert(std::is_nothrow_move_assignable_v<
                  InputRecordStore::PreparedPublish>);

    const CStoreGuid c_guid = Id128::from_u64(70);
    const std::vector<uint8_t> input = bytes(32 * 1024, 19);
    const ExactTransaction tx = transaction_for(TuSeq{71}, input);
    const InputRecordKey key{c_guid, tx.begin.tu_seq};
    InputRecordStore store(2, 1U << 20);

    InputRecordStore::PreparedPublish prepared =
        InputRecordStore::prepare_publish(c_guid, tx.begin, tx.commit,
                                          std::vector<uint8_t>(input));
    require(prepared.valid() && prepared.key() == key &&
                std::equal(prepared.exact_input().begin(),
                           prepared.exact_input().end(), input.begin()),
            "prepared InputRecord lost its exact identity or bytes");

    InputRecordStore::PreparedPublish moved = std::move(prepared);
    require(!prepared.valid() && moved.valid(),
            "prepared InputRecord move duplicated authority");
    require(commit_without_allocations(store, std::move(moved)) ==
                InputPublishResult::Published,
            "prepared InputRecord was not published");
    require(!moved.valid() && store.contains(key) &&
                store.retained_bytes() == input.size(),
            "prepared InputRecord commit did not consume one-shot authority");
    require_throws<std::invalid_argument>(
        [&] { (void)store.commit_prepared(std::move(moved)); },
        "consumed prepared InputRecord was accepted twice");

    InputRecordStore::PreparedPublish duplicate =
        InputRecordStore::prepare_publish(c_guid, tx.begin, tx.commit,
                                          std::vector<uint8_t>(input));
    require(commit_without_allocations(store, std::move(duplicate)) ==
                InputPublishResult::Existing,
            "exact prepared duplicate was not idempotent");
    require(!duplicate.valid() && store.record_count() == 1 &&
                store.retained_bytes() == input.size(),
            "prepared duplicate changed retained-input accounting");
}

void test_prepared_capacity_failure_is_transactional() {
    const CStoreGuid c_guid = Id128::from_u64(80);
    const std::vector<uint8_t> first = bytes(1024, 23);
    const std::vector<uint8_t> second = bytes(2048, 29);
    const ExactTransaction first_tx = transaction_for(TuSeq{81}, first);
    const ExactTransaction second_tx =
        transaction_for(TuSeq{82}, second, RelSeq{1});
    InputRecordStore store(1, 1U << 20);
    (void)store.publish(c_guid, first_tx.begin, first_tx.commit, first);

    InputRecordStore::PreparedPublish prepared =
        InputRecordStore::prepare_publish(c_guid, second_tx.begin,
                                          second_tx.commit,
                                          std::vector<uint8_t>(second));
    require_throws<std::length_error>(
        [&] { (void)store.commit_prepared(std::move(prepared)); },
        "prepared InputRecord bypassed the record cap");
    require(!prepared.valid() && store.record_count() == 1 &&
                store.retained_bytes() == first.size() &&
                !store.contains({c_guid, second_tx.begin.tu_seq}),
            "prepared record-cap failure partially changed the store");
}

void test_prepared_closed_job_observation_never_reopens() {
    const CStoreGuid c_guid = Id128::from_u64(90);
    const std::vector<uint8_t> input = bytes(4096, 31);
    const ExactTransaction tx = transaction_for(TuSeq{91}, input);
    const InputRecordKey key{c_guid, tx.begin.tu_seq};

    {
        InputRecordStore store(2, 1U << 20);
        InputRecordStore::PreparedPublish prepared =
            InputRecordStore::prepare_publish(c_guid, tx.begin, tx.commit,
                                              std::vector<uint8_t>(input));
        require(store.observe_closed_job_commit(std::move(prepared)) ==
                    InputPublishResult::NotRetainedJobClosed,
                "closed job retained a previously unpublished prepared input");
        require(!prepared.valid() && store.record_count() == 0 &&
                    store.retained_bytes() == 0,
                "closed-job prepared discard recreated input authority");
    }

    {
        InputRecordStore store(2, 1U << 20);
        (void)store.publish(c_guid, tx.begin, tx.commit, input);
        InputCursor authorized = store.attach(key);
        InputRecordStore::PreparedPublish prepared =
            InputRecordStore::prepare_publish(c_guid, tx.begin, tx.commit,
                                              std::vector<uint8_t>(input));
        require(store.observe_closed_job_commit(std::move(prepared)) ==
                    InputPublishResult::Existing,
                "closed-job observation did not match retained exact input");
        require(!prepared.valid() && !store.job_open(key),
                "closed-job prepared observation left attachment open");
        require_throws<std::logic_error>(
            [&] { (void)store.attach(key); },
            "closed-job prepared observation allowed a new attachment");
        require(drain(authorized) == input,
                "closed-job observation invalidated an authorized cursor");
        authorized = InputCursor{};
        store.collect_garbage();
        require(store.record_count() == 0 && store.retained_bytes() == 0,
                "closed prepared record was not reclaimed");
    }
}

void test_publish_and_independent_restart_cursors() {
    const CStoreGuid c_guid = Id128::from_u64(1);
    const std::vector<uint8_t> input = bytes(64 * 1024, 7);
    const ExactTransaction tx = transaction_for(TuSeq{3}, input);
    const InputRecordKey key{c_guid, tx.begin.tu_seq};

    InputRecordStore store(4, 1U << 20);
    require(store.publish(c_guid, tx.begin, tx.commit, input) ==
                InputPublishResult::Published,
            "first exact input was not published");
    require(store.record_count() == 1 &&
                store.retained_bytes() == input.size() &&
                store.job_open(key),
            "published InputRecord accounting is wrong");

    InputCursor attempt_a = store.attach(key);
    InputCursor attempt_b = store.attach(key);
    std::array<uint8_t, 137> prefix_a{};
    std::array<uint8_t, 53> prefix_b{};
    require(attempt_a.read(prefix_a) == prefix_a.size() &&
                std::equal(prefix_a.begin(), prefix_a.end(), input.begin()),
            "attempt A read the wrong prefix");
    require(attempt_b.read(prefix_b) == prefix_b.size() &&
                std::equal(prefix_b.begin(), prefix_b.end(), input.begin()),
            "attempt B did not start at its own byte-zero cursor");

    std::vector<uint8_t> remainder_a = drain(attempt_a, 257);
    std::vector<uint8_t> reconstructed_a(prefix_a.begin(), prefix_a.end());
    reconstructed_a.insert(reconstructed_a.end(), remainder_a.begin(),
                           remainder_a.end());
    require(reconstructed_a == input,
            "attempt A did not reconstruct exact input");

    // Attempt B is independent even though A advanced and reached EOF.
    std::vector<uint8_t> remainder_b = drain(attempt_b, 101);
    std::vector<uint8_t> reconstructed_b(prefix_b.begin(), prefix_b.end());
    reconstructed_b.insert(reconstructed_b.end(), remainder_b.begin(),
                           remainder_b.end());
    require(reconstructed_b == input,
            "attempt B shared attempt A's mutable offset");

    // A later replacement attempt also starts from byte zero without another
    // cache publication or route-history transition.
    InputCursor replacement = store.attach(key);
    require(drain(replacement, 4093) == input,
            "replacement compiler did not reuse exact input from byte zero");
}

void test_duplicate_and_conflicting_publication() {
    const CStoreGuid c_guid = Id128::from_u64(10);
    const std::vector<uint8_t> input = bytes(4096, 2);
    const ExactTransaction tx = transaction_for(TuSeq{11}, input);
    const InputRecordKey key{c_guid, tx.begin.tu_seq};
    InputRecordStore store(3, 1U << 20);

    require(store.publish(c_guid, tx.begin, tx.commit, input) ==
                InputPublishResult::Published,
            "initial publication failed");
    require(store.publish(c_guid, tx.begin, tx.commit, input) ==
                InputPublishResult::Existing,
            "exact duplicate publication was not idempotent");
    require(store.record_count() == 1 &&
                store.retained_bytes() == input.size(),
            "duplicate publication changed accounting");

    std::vector<uint8_t> changed = input;
    changed[changed.size() / 2] ^= 1;
    const ExactTransaction conflict = transaction_for(tx.begin.tu_seq, changed);
    require_throws<std::logic_error>(
        [&] { (void)store.publish(c_guid, conflict.begin,
                                 conflict.commit, changed); },
        "same InputRecord key accepted conflicting exact bytes");

    InputCursor existing = store.attach(key);
    store.close_job(key);
    const size_t closed_record_count = store.record_count();
    const uint64_t closed_retained_bytes = store.retained_bytes();
    require(!store.job_open(key) && closed_record_count == 1 &&
                closed_retained_bytes == input.size(),
            "closed InputRecord accounting is wrong");
    require_throws<std::logic_error>(
        [&] { (void)store.publish(c_guid, tx.begin, tx.commit, input); },
        "late open publication accepted a retained closed InputRecord");
    require(!store.job_open(key) &&
                store.record_count() == closed_record_count &&
                store.retained_bytes() == closed_retained_bytes,
            "rejected closed InputRecord publication changed state or accounting");
    require_throws<std::logic_error>(
        [&] { (void)store.attach(key); },
        "closed logical job accepted a new compiler attachment");
    require(drain(existing, 127) == input,
            "rejected publication changed an existing cursor's exact input");
    existing = InputCursor{};
    store.collect_garbage();
    require(store.record_count() == 0 && store.retained_bytes() == 0,
            "closed duplicate left unreferenced input retained");
}

void test_commit_and_input_validation() {
    const CStoreGuid c_guid = Id128::from_u64(20);
    const std::vector<uint8_t> input = bytes(2048, 5);
    const ExactTransaction tx = transaction_for(TuSeq{21}, input);

    {
        InputRecordStore store(2, 1U << 20);
        TxCommit bad = tx.commit;
        bad.transaction_digest = icecc::digest128("wrong transaction");
        require_throws<std::invalid_argument>(
            [&] { (void)store.publish(c_guid, tx.begin, bad, input); },
            "mismatched TX_COMMIT identity was accepted");
        require(store.record_count() == 0,
                "bad commit left a partial InputRecord");
    }
    {
        InputRecordStore store(2, 1U << 20);
        TxCommit bad = tx.commit;
        bad.post_state_digest = icecc::digest128("wrong post state");
        require_throws<std::invalid_argument>(
            [&] { (void)store.publish(c_guid, tx.begin, bad, input); },
            "wrong post-state digest was accepted");
    }
    {
        InputRecordStore store(2, 1U << 20);
        std::vector<uint8_t> corrupt = input;
        corrupt.front() ^= 1;
        require_throws<std::invalid_argument>(
            [&] { (void)store.publish(c_guid, tx.begin,
                                     tx.commit, corrupt); },
            "wrong exact input bytes were accepted");
    }
    {
        InputRecordStore store(2, 1U << 20);
        TxBegin terminal = tx.begin;
        terminal.rel_seq = RelSeq{std::numeric_limits<uint64_t>::max()};
        TxCommit terminal_commit = tx.commit;
        terminal_commit.rel_seq = terminal.rel_seq;
        terminal_commit.post_state_digest = compute_post_state_digest(
            terminal.pre_state_digest, terminal.history_nonce,
            terminal.rel_seq, terminal.tu_seq,
            terminal.transaction_digest);
        require_throws<std::overflow_error>(
            [&] { (void)store.publish(c_guid, terminal,
                                     terminal_commit, input); },
            "terminal REL_SEQ was published as an InputRecord");
    }
    {
        InputRecordStore store(2, 1U << 20);
        require_throws<std::invalid_argument>(
            [&] { (void)store.publish(CStoreGuid{}, tx.begin,
                                     tx.commit, input); },
            "zero C_STORE_GUID was accepted");
    }
}

void test_capacity_is_transactional() {
    const CStoreGuid c_guid = Id128::from_u64(30);
    const std::vector<uint8_t> first = bytes(1024, 1);
    const std::vector<uint8_t> second = bytes(512, 2);
    const ExactTransaction first_tx = transaction_for(TuSeq{31}, first);
    const ExactTransaction second_tx = transaction_for(TuSeq{32}, second,
                                                        RelSeq{1});

    InputRecordStore one_record(1, 1U << 20);
    (void)one_record.publish(c_guid, first_tx.begin, first_tx.commit, first);
    require_throws<std::length_error>(
        [&] { (void)one_record.publish(c_guid, second_tx.begin,
                                      second_tx.commit, second); },
        "record-count cap was not enforced");
    require(one_record.record_count() == 1 &&
                one_record.retained_bytes() == first.size() &&
                !one_record.contains({c_guid, second_tx.begin.tu_seq}),
            "record-cap failure partially inserted input");

    InputRecordStore byte_limited(2, first.size() - 1);
    require_throws<std::length_error>(
        [&] { (void)byte_limited.publish(c_guid, first_tx.begin,
                                        first_tx.commit, first); },
        "retained-byte cap was not enforced");
    require(byte_limited.record_count() == 0 &&
                byte_limited.retained_bytes() == 0,
            "byte-cap failure changed store accounting");
}

void test_close_waits_for_authorized_reader() {
    const CStoreGuid c_guid = Id128::from_u64(40);
    const std::vector<uint8_t> input = bytes(8192, 8);
    const ExactTransaction tx = transaction_for(TuSeq{41}, input);
    const InputRecordKey key{c_guid, tx.begin.tu_seq};
    InputRecordStore store(2, 1U << 20);
    (void)store.publish(c_guid, tx.begin, tx.commit, input);

    InputCursor running = store.attach(key);
    store.close_job(key);
    store.collect_garbage();
    require(store.record_count() == 1 &&
                store.retained_bytes() == input.size(),
            "job close reclaimed input still owned by an authorized compiler");
    require_throws<std::logic_error>(
        [&] { (void)store.attach(key); },
        "job close allowed a new compiler attachment");
    require(drain(running, 211) == input,
            "authorized compiler lost exact input after job close");

    running = InputCursor{};
    store.collect_garbage();
    require(store.record_count() == 0 && store.retained_bytes() == 0,
            "closed input was not reclaimed after its last reader exited");
}

void test_cursor_outlives_store_owner_object() {
    const CStoreGuid c_guid = Id128::from_u64(50);
    const std::vector<uint8_t> input = bytes(16 * 1024, 12);
    const ExactTransaction tx = transaction_for(TuSeq{51}, input);
    InputCursor authorized;

    {
        InputRecordStore store(2, 1U << 20);
        (void)store.publish(c_guid, tx.begin, tx.commit, input);
        authorized = store.attach({c_guid, tx.begin.tu_seq});
    }

    require(drain(authorized, 509) == input,
            "authorized cursor did not outlive its store owner object");
}

void test_empty_input() {
    const CStoreGuid c_guid = Id128::from_u64(60);
    const std::vector<uint8_t> empty;
    const ExactTransaction tx = transaction_for(TuSeq{61}, empty);
    const InputRecordKey key{c_guid, tx.begin.tu_seq};
    InputRecordStore store(1, 1);

    require(store.publish(c_guid, tx.begin, tx.commit, empty) ==
                InputPublishResult::Published,
            "empty exact input did not publish");
    InputCursor cursor = store.attach(key);
    std::array<uint8_t, 1> byte{};
    require(cursor.eof() && cursor.read(byte) == 0,
            "empty InputRecord produced bytes");
    cursor = InputCursor{};
    store.close_job(key);
    store.collect_garbage();
    require(store.record_count() == 0 && store.retained_bytes() == 0,
            "empty InputRecord did not close cleanly");
}

void test_whole_namespace_eviction_waits_for_jobs_and_cursors() {
    const CStoreGuid first_guid = Id128::from_u64(120);
    const CStoreGuid second_guid = Id128::from_u64(121);
    const std::vector<uint8_t> first = bytes(1024, 41);
    const std::vector<uint8_t> second = bytes(2048, 43);
    const ExactTransaction first_tx = transaction_for(TuSeq{1}, first);
    const ExactTransaction second_tx = transaction_for(TuSeq{2}, second);
    const InputRecordKey first_key{first_guid, first_tx.begin.tu_seq};
    const InputRecordKey second_key{second_guid, second_tx.begin.tu_seq};
    InputRecordStore store(4, 1U << 20);

    (void)store.publish(first_guid, first_tx.begin, first_tx.commit, first);
    (void)store.publish(second_guid, second_tx.begin, second_tx.commit, second);
    require(store.namespace_record_count(first_guid) == 1 &&
                store.namespace_retained_bytes(first_guid) == first.size() &&
                store.namespace_record_count(second_guid) == 1 &&
                store.namespace_retained_bytes(second_guid) == second.size(),
            "per-namespace retained-input accounting is wrong");
    require(!store.namespace_evictable(first_guid),
            "open logical job was declared namespace-evictable");

    InputCursor cursor = store.attach(first_key);
    store.close_job(first_key);
    store.close_job(second_key);
    require(!store.namespace_evictable(first_guid) &&
                store.namespace_evictable(second_guid),
            "cursor ownership did not distinguish namespace eviction state");
    store.evict_namespace(second_guid);
    require(!store.contains(second_key) && store.contains(first_key) &&
                store.record_count() == 1 &&
                store.retained_bytes() == first.size(),
            "whole-namespace eviction removed the wrong retained input");
    require_throws<std::logic_error>(
        [&] { store.evict_namespace(first_guid); },
        "whole-namespace eviction ignored an issued cursor");
    require(drain(cursor) == first,
            "rejected namespace eviction damaged the issued cursor");
    cursor = InputCursor{};
    require(store.namespace_evictable(first_guid),
            "released cursor did not make the closed namespace evictable");
    store.evict_namespace(first_guid);
    require(store.record_count() == 0 && store.retained_bytes() == 0,
            "final whole-namespace eviction leaked retained input");
}

void test_sequential_10000_tu_retention_release_soak() {
    const CStoreGuid c_guid = Id128::from_u64(10000);
    InputRecordStore store(2, 1U << 20);
    for (uint64_t value = 0; value != 10000; ++value) {
        const std::vector<uint8_t> input = bytes(64, static_cast<uint32_t>(value));
        const ExactTransaction tx = transaction_for(TuSeq{value}, input);
        const InputRecordKey key{c_guid, tx.begin.tu_seq};
        require(store.publish(c_guid, tx.begin, tx.commit, input) ==
                    InputPublishResult::Published,
                "sequential soak did not publish the next TU");
        require(store.contains(key) && store.record_count() == 1 &&
                    store.retained_bytes() == input.size(),
                "sequential soak retained the wrong TU footprint");
        store.close_job(key);
        store.collect_garbage();
        require(!store.contains(key) && store.record_count() == 0 &&
                    store.retained_bytes() == 0,
                "sequential soak failed to release a closed TU");
    }
}

void test_guid_flip_capacity_failure_then_retry() {
    const CStoreGuid first_guid = Id128::from_u64(11001);
    const CStoreGuid replacement_guid = Id128::from_u64(11002);
    const std::vector<uint8_t> first = bytes(512, 71);
    const std::vector<uint8_t> replacement = bytes(512, 73);
    const ExactTransaction first_tx = transaction_for(TuSeq{1}, first);
    const ExactTransaction replacement_tx = transaction_for(TuSeq{2}, replacement);
    const InputRecordKey first_key{first_guid, first_tx.begin.tu_seq};
    const InputRecordKey replacement_key{replacement_guid,
                                         replacement_tx.begin.tu_seq};
    InputRecordStore store(1, 1024);
    require(store.publish(first_guid, first_tx.begin, first_tx.commit, first) ==
                InputPublishResult::Published,
            "GUID-flip row failed to publish the original namespace");
    InputRecordStore::PreparedPublish failed =
        InputRecordStore::prepare_publish(replacement_guid, replacement_tx.begin,
                                          replacement_tx.commit,
                                          std::vector<uint8_t>(replacement));
    require_throws<std::length_error>(
        [&] { (void)store.commit_prepared(std::move(failed)); },
        "GUID-flip row did not fail closed at capacity");
    require(!failed.valid() && store.contains(first_key) &&
                !store.contains(replacement_key) && store.record_count() == 1,
            "failed GUID-flip admission changed retained state");
    store.close_job(first_key);
    store.collect_garbage();
    require(store.publish(replacement_guid, replacement_tx.begin,
                          replacement_tx.commit, replacement) ==
                InputPublishResult::Published &&
                store.contains(replacement_key) && store.record_count() == 1,
            "GUID-flip retry did not succeed after release");
}

}  // namespace

int main() {
    test_verified_materialization_binds_begin_commit_and_bytes();
    test_prepared_publish_is_exact_one_shot_and_allocation_free();
    test_prepared_capacity_failure_is_transactional();
    test_prepared_closed_job_observation_never_reopens();
    test_publish_and_independent_restart_cursors();
    test_duplicate_and_conflicting_publication();
    test_commit_and_input_validation();
    test_capacity_is_transactional();
    test_close_waits_for_authorized_reader();
    test_cursor_outlives_store_owner_object();
    test_empty_input();
    test_whole_namespace_eviction_waits_for_jobs_and_cursors();
    test_sequential_10000_tu_retention_release_soak();
    test_guid_flip_capacity_failure_then_retry();
    std::cout << "p50_input_record_test: exact retained-input restart gates passed\n";
    return 0;
}
