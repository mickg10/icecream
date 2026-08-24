#include "cache/p50_input_attachment.h"

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

[[noreturn]] void fail(std::string_view text) {
    std::cerr << "p50_input_attachment_test: " << text << '\n';
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

std::vector<uint8_t> bytes(size_t count, uint8_t salt) {
    std::vector<uint8_t> result(count);
    for (size_t index = 0; index != result.size(); ++index)
        result[index] = static_cast<uint8_t>((index * 37 + salt) & 0xff);
    return result;
}

struct ExactTransaction {
    TxBegin begin;
    TxCommit commit;
};

ExactTransaction transaction_for(TuSeq tu_seq,
                                 std::span<const uint8_t> input,
                                 RelSeq rel_seq = RelSeq{}) {
    static constexpr std::array<uint8_t, 3> body{0x50, 0x35, 0x30};
    TxBegin begin;
    begin.history_nonce = HistoryNonce{71};
    begin.rel_seq = rel_seq;
    begin.tu_seq = tu_seq;
    begin.profile = ProfileId::ZSTD_TU;
    begin.p29_root_mode = P29RootMode::NotApplicable;
    begin.pre_state_digest = icecc::digest128("attachment-pre-state");
    begin.dict = describe_component(0, std::span<const uint8_t>{}, 0);
    begin.body = describe_component(1, body, input.size());
    begin.raw_bytes = input.size();
    begin.raw_digest = icecc::digest128(input);
    begin.transaction_digest =
        compute_transaction_digest(begin, std::span<const uint8_t>{}, body);
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
    std::array<uint8_t, 97> chunk{};
    while (!cursor.eof()) {
        const size_t count = cursor.read(chunk);
        require(count != 0, "attachment cursor made no progress");
        result.insert(result.end(), chunk.begin(), chunk.begin() + count);
    }
    return result;
}

void test_commit_ready_ack_replay_and_reclaim() {
    const CStoreGuid guid = Id128::from_u64(1);
    const InputAttempt first{101, 1};
    const InputAttachmentRequest req{{guid, TuSeq{0}}, first, 11};
    const std::vector<uint8_t> input = bytes(4096, 7);
    const ExactTransaction tx = transaction_for(req.key.tu_seq, input);
    InputAttachmentCore core(4, 1U << 20, 4);

    require(core.commit_open(first, guid, tx.begin, tx.commit, input) ==
                InputPublishResult::Published,
            "open commit was not published");
    require(core.commit_open(first, guid, tx.begin, tx.commit, input) ==
                InputPublishResult::Existing,
            "exact duplicate Open commit was not idempotent");
    require(core.pending_ready_count() == 1,
            "one committed Open did not create one ready event");
    const InputAttachmentReply reply = core.request(req);
    require(reply.status == InputAttachmentStatus::Ready && reply.event_id != 0,
            "ready request returned the wrong identity/status");
    const InputAttachmentReply repeated = core.request(req);
    require(repeated == reply, "exact request replay changed its reply");
    InputCursor cursor = core.attach(req, reply);
    require(drain(cursor) == input, "attachment cursor did not read exact bytes");
    require(core.acknowledge(req, reply) == InputAttachmentAckResult::Accepted,
            "ready acknowledgement was rejected");
    require(core.acknowledge(req, reply) ==
                InputAttachmentAckResult::AlreadyAccepted,
            "duplicate acknowledgement was not idempotent");
    require(core.pending_ready_count() == 0,
            "acknowledgement did not retire pending ready event");

    // A new request after ACK gets a replay observation, not a second event.
    const InputAttachmentRequest replay_req{req.key, first, 12};
    const InputAttachmentReply replay = core.request(replay_req);
    require(replay.status == InputAttachmentStatus::ReadyReplay &&
                replay.event_id == 0,
            "post-ACK request created a second ready event");
    require(core.acknowledge(replay_req, replay) ==
                InputAttachmentAckResult::Accepted,
            "replay acknowledgement was rejected");

    core.cancel(first, req.key);
    require(!core.job_open(req.key), "cancel did not close logical job");
    require(core.request({req.key, first, 13}).status ==
                InputAttachmentStatus::Closed,
            "request after close was not classified closed");
    require_throws<std::logic_error>([&] { (void)core.attach(req, reply); },
                                     "attach after close was accepted");
    cursor = InputCursor{};
    core.collect_garbage();
    require(core.record_count() == 0 && core.retained_bytes() == 0,
            "closed input was not reclaimed after cursor release");
}

void test_owner_replacement_and_stale_identity() {
    const CStoreGuid guid = Id128::from_u64(2);
    const InputRecordKey key{guid, TuSeq{2}};
    const InputAttempt first{202, 1};
    const InputAttempt replacement{202, 2};
    const std::vector<uint8_t> input = bytes(128, 9);
    const ExactTransaction tx = transaction_for(key.tu_seq, input);
    InputAttachmentCore core(2, 4096, 2);
    (void)core.commit_open(first, guid, tx.begin, tx.commit, input);
    const InputAttachmentRequest old_req{key, first, 20};
    const InputAttachmentReply old_reply = core.request(old_req);
    core.retry(first, replacement.attempt_id, key);
    require(core.request(old_req).status == InputAttachmentStatus::StaleAttempt,
            "old attempt request replay was not stale");
    require(core.acknowledge(old_req, old_reply) ==
                InputAttachmentAckResult::Rejected,
            "old attempt acknowledgement crossed replacement boundary");
    require_throws<std::logic_error>([&] { core.cancel(first, key); },
                                     "old attempt cancelled replacement");

    const InputAttachmentRequest new_req{key, replacement, 21};
    const InputAttachmentReply new_reply = core.request(new_req);
    require(new_reply.status == InputAttachmentStatus::Ready &&
                new_reply.event_id == old_reply.event_id,
            "replacement did not reuse exact ready event");
    InputCursor cursor = core.attach(new_req, new_reply);
    require(drain(cursor) == input, "replacement cursor did not start at byte zero");
    core.cancel(replacement, key);
}

void test_closed_before_commit_and_table_exhaustion() {
    const CStoreGuid guid = Id128::from_u64(3);
    const InputAttempt owner{303, 1};
    const InputRecordKey closed_key{guid, TuSeq{3}};
    const std::vector<uint8_t> input = bytes(64, 4);
    const ExactTransaction closed_tx = transaction_for(closed_key.tu_seq, input);
    InputAttachmentCore core(4, 4096, 1);
    core.cancel(owner, closed_key);
    require(core.commit_open(owner, guid, closed_tx.begin, closed_tx.commit, input) ==
                InputPublishResult::NotRetainedJobClosed,
            "closed-before-commit retained input or emitted ready");
    require(core.pending_ready_count() == 0 && core.record_count() == 0,
            "closed-before-commit changed attachment state");

    const InputAttempt open_a{304, 1};
    const InputRecordKey key_a{guid, TuSeq{4}};
    const std::vector<uint8_t> input_a = bytes(32, 5);
    const ExactTransaction tx_a = transaction_for(key_a.tu_seq, input_a);
    (void)core.commit_open(open_a, guid, tx_a.begin, tx_a.commit, input_a);
    const InputAttempt open_b{305, 1};
    const InputRecordKey key_b{guid, TuSeq{5}};
    const std::vector<uint8_t> input_b = bytes(32, 6);
    const ExactTransaction tx_b = transaction_for(key_b.tu_seq, input_b, RelSeq{1});
    require_throws<std::length_error>(
        [&] { (void)core.commit_open(open_b, guid, tx_b.begin, tx_b.commit, input_b); },
        "pending-ready table exhaustion was not bounded");
    require(!core.contains(key_b), "table exhaustion partially retained record");
}

void test_wrong_guid_tu_owner_and_perturbation() {
    const CStoreGuid guid = Id128::from_u64(6);
    const InputAttempt owner{606, 1};
    const InputRecordKey key{guid, TuSeq{6}};
    const std::vector<uint8_t> input = bytes(80, 8);
    const ExactTransaction tx = transaction_for(key.tu_seq, input);
    InputAttachmentCore core(2, 1024, 2);
    (void)core.commit_open(owner, guid, tx.begin, tx.commit, input);
    const InputAttachmentRequest req{key, owner, 61};
    const InputAttachmentReply reply = core.request(req);
    const InputRecordKey wrong_tu{guid, TuSeq{99}};
    require(core.request({wrong_tu, owner, 62}).status ==
                InputAttachmentStatus::Unknown,
            "wrong TU_SEQ was attached");
    require(core.request({{Id128::from_u64(7), key.tu_seq}, owner, 63}).status ==
                InputAttachmentStatus::Unknown,
            "wrong C_STORE_GUID was attached");
    const InputAttempt wrong_owner{607, 1};
    require(core.request({key, wrong_owner, 64}).status ==
                InputAttachmentStatus::Unknown,
            "cross-owner request was accepted");
    InputAttachmentReply perturbed = reply;
    perturbed.event_id++;
    require(core.acknowledge(req, perturbed) == InputAttachmentAckResult::Rejected,
            "perturbed replay acknowledgement was accepted");
    core.cancel(owner, key);
}

}  // namespace

int main() {
    test_commit_ready_ack_replay_and_reclaim();
    test_owner_replacement_and_stale_identity();
    test_closed_before_commit_and_table_exhaustion();
    test_wrong_guid_tu_owner_and_perturbation();
    std::cout << "p50_input_attachment_test: ownership, ready, replay, and reclamation gates passed\n";
    return 0;
}
