#include "cache/p50_input_record.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace icecc::p50;

[[noreturn]] void fail(std::string_view detail) {
    std::cerr << "p50_input_record_closed_reopen_test: " << detail << '\n';
    std::exit(1);
}

void require(bool value, std::string_view detail) {
    if (!value) fail(detail);
}

template<class Exception, class Function>
void require_throws(Function&& function, std::string_view detail) {
    try {
        function();
    } catch (const Exception&) {
        return;
    } catch (...) {
        fail(std::string(detail) + " (wrong exception)");
    }
    fail(std::string(detail) + " (no exception)");
}

struct ExactTransaction {
    TxBegin begin;
    TxCommit commit;
};

ExactTransaction transaction_for(TuSeq tu_seq,
                                 RelSeq rel_seq,
                                 Digest128 pre_state,
                                 std::span<const uint8_t> exact_input) {
    static constexpr std::array<uint8_t, 4> encoded_body{
        0x50, 0x35, 0x30, 0x52};

    TxBegin begin;
    begin.history_nonce = HistoryNonce{73};
    begin.rel_seq = rel_seq;
    begin.tu_seq = tu_seq;
    begin.profile = ProfileId::ZSTD_TU;
    begin.p29_root_mode = P29RootMode::NotApplicable;
    begin.pre_state_digest = pre_state;
    begin.dict = describe_component(0, std::span<const uint8_t>{}, 0);
    begin.body = describe_component(1, encoded_body, exact_input.size());
    begin.raw_bytes = exact_input.size();
    begin.raw_digest = icecc::digest128(exact_input);
    begin.transaction_digest = compute_transaction_digest(
        begin, std::span<const uint8_t>{}, encoded_body);

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
    std::array<uint8_t, 113> chunk{};
    while (!cursor.eof()) {
        const size_t count = cursor.read(chunk);
        require(count != 0, "non-EOF cursor made no progress");
        result.insert(result.end(), chunk.begin(), chunk.begin() + count);
    }
    return result;
}

void test_open_publication_cannot_reuse_closed_lease() {
    const CStoreGuid c_guid = Id128::from_u64(9001);
    const TuSeq tu_seq{0};
    const InputRecordKey key{c_guid, tu_seq};
    const std::vector<uint8_t> input{
        'e', 'x', 'a', 'c', 't', ' ', 'r', 'e', 's', 't', 'a', 'r', 't'};

    const ExactTransaction first = transaction_for(
        tu_seq, RelSeq{0}, icecc::digest128("route-zero"), input);
    const ExactTransaction later = transaction_for(
        tu_seq, RelSeq{1}, first.commit.post_state_digest, input);

    InputRecordStore store(2, 1U << 20);
    require(store.publish(c_guid, first.begin, first.commit, input) ==
                InputPublishResult::Published,
            "initial open job did not publish its InputRecord");
    InputCursor authorized = store.attach(key);
    store.close_job(key);

    require_throws<std::logic_error>(
        [&] {
            (void)store.publish(c_guid, first.begin, first.commit, input);
        },
        "exact open-job replay accepted an already-closed InputRecord");

    require(store.contains(key) && !store.job_open(key) &&
                store.record_count() == 1 &&
                store.retained_bytes() == input.size(),
            "rejected reopen changed the closed InputRecord lease");
    require(drain(authorized) == input,
            "rejected reopen invalidated an authorized compiler cursor");

    require_throws<std::logic_error>(
        [&] {
            (void)store.observe_closed_job_commit(
                c_guid, later.begin, later.commit, input);
        },
        "closed-job replay accepted conflicting transaction metadata");
    require(store.contains(key) && !store.job_open(key) &&
                store.record_count() == 1 &&
                store.retained_bytes() == input.size(),
            "rejected closed-job metadata conflict changed the lease");

    require(store.observe_closed_job_commit(
                c_guid, first.begin, first.commit, input) ==
                InputPublishResult::Existing &&
                !store.job_open(key),
            "exact closed-job route completion was not idempotently closed");
}

}  // namespace

int main() {
    test_open_publication_cannot_reuse_closed_lease();
    std::cout <<
        "p50_input_record_closed_reopen_test: closed lease stayed closed\n";
    return 0;
}
