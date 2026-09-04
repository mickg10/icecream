#include "cache/p50_input_record.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace icecc::p50;

[[noreturn]] void fail(std::string_view text) {
    std::cerr << "p50_input_record_late_commit_test: " << text << '\n';
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
            (index * 61 + index / 17 + salt) & 0xff);
    return result;
}

struct ExactTransaction {
    TxBegin begin;
    TxCommit commit;
};

ExactTransaction transaction_for(TuSeq tu_seq,
                                 std::span<const uint8_t> exact_input,
                                 RelSeq rel_seq = RelSeq{}) {
    static constexpr std::array<uint8_t, 4> encoded_body{
        0x50, 0x35, 0x30, 0x4a};

    TxBegin begin;
    begin.history_nonce = HistoryNonce{29};
    begin.rel_seq = rel_seq;
    begin.tu_seq = tu_seq;
    begin.profile = ProfileId::ZSTD_TU;
    begin.pre_state_digest = icecc::digest128("late-commit-pre-state");
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

std::vector<uint8_t> drain(InputCursor& cursor) {
    std::vector<uint8_t> result;
    std::array<uint8_t, 257> chunk{};
    while (!cursor.eof()) {
        const size_t count = cursor.read(chunk);
        require(count != 0, "non-EOF cursor made no progress");
        result.insert(result.end(), chunk.begin(), chunk.begin() + count);
    }
    return result;
}

void test_closed_before_commit_does_not_create_a_lease() {
    const CStoreGuid c_guid = Id128::from_u64(100);
    const std::vector<uint8_t> input = bytes(32 * 1024, 1);
    const ExactTransaction tx = transaction_for(TuSeq{101}, input);
    const InputRecordKey key{c_guid, tx.begin.tu_seq};

    // The retention cap is intentionally smaller than this exact input. A
    // closed-job cache commit validates bytes but does not allocate a record.
    InputRecordStore store(1, 1024);
    require(store.observe_closed_job_commit(
                c_guid, tx.begin, tx.commit, input) ==
                InputPublishResult::NotRetainedJobClosed,
            "closed-job cache commit unexpectedly retained input");
    require(store.record_count() == 0 && store.retained_bytes() == 0 &&
                !store.contains(key) && !store.job_open(key),
            "closed-before-commit path created a compiler-visible lease");
    require_throws<std::out_of_range>(
        [&] { (void)store.attach(key); },
        "closed-before-commit path allowed compiler attachment");
}

void test_late_commit_closes_retained_input_but_preserves_reader() {
    const CStoreGuid c_guid = Id128::from_u64(200);
    const std::vector<uint8_t> input = bytes(24 * 1024, 2);
    const ExactTransaction tx = transaction_for(TuSeq{201}, input);
    const InputRecordKey key{c_guid, tx.begin.tu_seq};

    InputRecordStore store(2, 1U << 20);
    require(store.publish(c_guid, tx.begin, tx.commit, input) ==
                InputPublishResult::Published,
            "open job input did not publish");
    InputCursor authorized = store.attach(key);

    require(store.observe_closed_job_commit(
                c_guid, tx.begin, tx.commit, input) ==
                InputPublishResult::Existing,
            "late route commit did not match retained exact input");
    require(!store.job_open(key),
            "late route commit reopened or preserved an open job lease");
    require_throws<std::logic_error>(
        [&] { (void)store.attach(key); },
        "closed job accepted a replacement compiler attachment");

    store.collect_garbage();
    require(store.record_count() == 1,
            "closed job reclaimed input still owned by an authorized compiler");
    require(drain(authorized) == input,
            "authorized compiler lost input when the logical job closed");

    authorized = InputCursor{};
    store.collect_garbage();
    require(store.record_count() == 0 && store.retained_bytes() == 0,
            "closed late-commit record was not reclaimed after its reader");
}

void test_conflicting_late_commit_is_fail_closed() {
    const CStoreGuid c_guid = Id128::from_u64(300);
    const std::vector<uint8_t> input = bytes(4096, 3);
    const ExactTransaction tx = transaction_for(TuSeq{301}, input);
    const InputRecordKey key{c_guid, tx.begin.tu_seq};

    InputRecordStore store(2, 1U << 20);
    (void)store.publish(c_guid, tx.begin, tx.commit, input);

    std::vector<uint8_t> changed = input;
    changed[changed.size() / 2] ^= 1;
    const ExactTransaction conflict = transaction_for(tx.begin.tu_seq, changed);
    require_throws<std::logic_error>(
        [&] {
            (void)store.observe_closed_job_commit(
                c_guid, conflict.begin, conflict.commit, changed);
        },
        "conflicting late commit changed one InputRecord identity");
    require(store.job_open(key),
            "conflicting late commit mutated the existing job lease");
}

void test_closed_observation_is_idempotent_while_retained() {
    const CStoreGuid c_guid = Id128::from_u64(400);
    const std::vector<uint8_t> input = bytes(2048, 4);
    const ExactTransaction tx = transaction_for(TuSeq{401}, input);
    const InputRecordKey key{c_guid, tx.begin.tu_seq};

    InputRecordStore store(2, 1U << 20);
    (void)store.publish(c_guid, tx.begin, tx.commit, input);
    require(store.observe_closed_job_commit(
                c_guid, tx.begin, tx.commit, input) ==
                InputPublishResult::Existing,
            "first closed observation failed");
    require(store.observe_closed_job_commit(
                c_guid, tx.begin, tx.commit, input) ==
                InputPublishResult::Existing,
            "duplicate closed observation was not idempotent");
    require(!store.job_open(key),
            "duplicate closed observation reopened the job");
}

void test_closed_observation_validates_commit_identity() {
    const CStoreGuid c_guid = Id128::from_u64(500);
    const std::vector<uint8_t> input = bytes(1024, 5);
    const ExactTransaction tx = transaction_for(TuSeq{501}, input);
    InputRecordStore store(1, 1U << 20);

    TxCommit bad = tx.commit;
    bad.post_state_digest = icecc::digest128("not-the-post-state");
    require_throws<std::invalid_argument>(
        [&] {
            (void)store.observe_closed_job_commit(
                c_guid, tx.begin, bad, input);
        },
        "closed-job path accepted the wrong commit identity");
    require(store.record_count() == 0 && store.retained_bytes() == 0,
            "invalid closed-job commit changed store state");

    TxBegin terminal = tx.begin;
    terminal.rel_seq = RelSeq{std::numeric_limits<uint64_t>::max()};
    TxCommit terminal_commit = tx.commit;
    terminal_commit.rel_seq = terminal.rel_seq;
    terminal_commit.post_state_digest = compute_post_state_digest(
        terminal.pre_state_digest, terminal.history_nonce,
        terminal.rel_seq, terminal.tu_seq, terminal.transaction_digest);
    require_throws<std::overflow_error>(
        [&] {
            (void)store.observe_closed_job_commit(
                c_guid, terminal, terminal_commit, input);
        },
        "closed-job path accepted terminal REL_SEQ");
}

}  // namespace

int main() {
    test_closed_before_commit_does_not_create_a_lease();
    test_late_commit_closes_retained_input_but_preserves_reader();
    test_conflicting_late_commit_is_fail_closed();
    test_closed_observation_is_idempotent_while_retained();
    test_closed_observation_validates_commit_identity();
    std::cout <<
        "p50_input_record_late_commit_test: late cache/job races passed\n";
    return 0;
}
