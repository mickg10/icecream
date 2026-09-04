#include "cache/p50_input_attachment.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
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
    begin.pre_state_digest = icecc::digest128("attachment-pre-state");
    begin.body = describe_component(
        static_cast<uint16_t>(ProfileId::ZSTD_TU), body, input.size());
    begin.raw_bytes = input.size();
    begin.raw_digest = icecc::digest128(input);
    begin.transaction_digest = compute_transaction_digest(begin, body);
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
    InputAttachmentReply forged_ack_state = reply;
    forged_ack_state.acknowledged = true;
    require(core.acknowledge(req, forged_ack_state) ==
                InputAttachmentAckResult::Rejected,
            "forged acknowledgement state was accepted");
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
    const InputAttachmentReply acknowledged_replay = core.request(req);
    require(acknowledged_replay.status == InputAttachmentStatus::Ready &&
                acknowledged_replay.event_id == reply.event_id &&
                acknowledged_replay.acknowledged,
            "exact request replay changed its canonical ACK state");

    // A new request after ACK gets a replay observation, not a second event.
    const InputAttachmentRequest replay_req{req.key, first, 12};
    const InputAttachmentReply replay = core.request(replay_req);
    require(replay.status == InputAttachmentStatus::ReadyReplay &&
                replay.event_id == reply.event_id &&
                replay.raw_bytes == reply.raw_bytes &&
                replay.raw_digest == reply.raw_digest,
            "post-ACK request lost the canonical ready event identity");
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
    require(core.owner_count() == 0,
            "already-observed committed owner was not reclaimed");
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
    require(core.acknowledge(old_req, old_reply) ==
                InputAttachmentAckResult::Accepted,
            "old attempt ready acknowledgement was rejected");
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
    require(new_reply.status == InputAttachmentStatus::ReadyReplay &&
                new_reply.event_id == old_reply.event_id &&
                new_reply.raw_bytes == old_reply.raw_bytes &&
                new_reply.raw_digest == old_reply.raw_digest,
            "replacement did not reuse exact ready event");
    InputCursor cursor = core.attach(new_req, new_reply);
    require(drain(cursor) == input, "replacement cursor did not start at byte zero");
    core.cancel(replacement, key);
}

void test_replacement_before_commit_publishes_ready() {
    const CStoreGuid guid = Id128::from_u64(20);
    const InputRecordKey key{guid, TuSeq{20}};
    const InputAttempt first{2001, 1};
    const InputAttempt replacement{2001, 2};
    const std::vector<uint8_t> input = bytes(48, 20);
    const ExactTransaction tx = transaction_for(key.tu_seq, input);
    InputAttachmentCore core(2, 4096, 2);

    core.retry(first, replacement.attempt_id, key);
    require(core.owner_count() == 1 && core.record_count() == 0,
            "pre-commit replacement created bytes or duplicate owners");
    require_throws<std::logic_error>(
        [&] { (void)core.commit_open(first, guid, tx.begin, tx.commit, input); },
        "replaced attempt committed input after ownership moved");
    require(core.commit_open(replacement, guid, tx.begin, tx.commit, input) ==
                InputPublishResult::Published,
            "replacement-before-commit did not publish exact input");
    require(core.pending_ready_count() == 1,
            "replacement-before-commit retained compiler-invisible bytes");
    const InputAttachmentRequest request{key, replacement, 201};
    const InputAttachmentReply reply = core.request(request);
    require(reply.status == InputAttachmentStatus::Ready && reply.event_id != 0,
            "replacement-before-commit did not publish canonical Ready");
    InputCursor cursor = core.attach(request, reply);
    require(drain(cursor) == input,
            "replacement-before-commit cursor lost exact bytes");
    core.cancel(replacement, key);
    cursor = InputCursor{};
    core.collect_garbage();
    require(core.owner_count() == 0,
            "replacement-before-commit owner did not reclaim");
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
    require(core.owner_count() == 0,
            "observed cancel-before-commit tombstone was not reclaimed");

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
    core.cancel(open_a, key_a);
    core.collect_garbage();
    require(core.commit_open(open_b, guid, tx_b.begin, tx_b.commit, input_b) ==
                InputPublishResult::Published,
            "pending-ready capacity did not recover after close/reclaim");
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

void test_same_key_conflict_and_duplicate_attachment() {
    const CStoreGuid guid = Id128::from_u64(8);
    const InputAttempt owner{808, 1};
    const InputRecordKey key{guid, TuSeq{8}};
    const std::vector<uint8_t> input = bytes(96, 1);
    const std::vector<uint8_t> conflicting = bytes(96, 2);
    const ExactTransaction tx = transaction_for(key.tu_seq, input);
    const ExactTransaction conflicting_tx =
        transaction_for(key.tu_seq, conflicting);
    InputAttachmentCore core(4, 4096, 4, 4);
    (void)core.commit_open(owner, guid, tx.begin, tx.commit, input);
    require_throws<std::logic_error>(
        [&] {
            (void)core.commit_open(owner, guid, conflicting_tx.begin,
                                   conflicting_tx.commit, conflicting);
        },
        "same key accepted different exact bytes");
    const ExactTransaction metadata_conflict =
        transaction_for(key.tu_seq, input, RelSeq{9});
    require_throws<std::logic_error>(
        [&] {
            (void)core.commit_open(owner, guid, metadata_conflict.begin,
                                   metadata_conflict.commit, input);
        },
        "same key and bytes accepted different transaction metadata");

    const InputAttachmentRequest first{key, owner, 81};
    const InputAttachmentReply first_reply = core.request(first);
    require(core.request(first) == first_reply,
            "exact duplicate request changed its reply");
    const InputAttachmentRequest second{key, owner, 82};
    const InputAttachmentReply second_reply = core.request(second);
    require(second_reply.event_id == first_reply.event_id,
            "distinct request ID changed canonical event identity");
    InputCursor cursor = core.attach(first, first_reply);
    require(drain(cursor) == input, "canonical attachment lost exact bytes");
    require_throws<std::logic_error>(
        [&] { (void)core.attach(first, first_reply); },
        "duplicate attach admitted a second cursor");
    require_throws<std::logic_error>(
        [&] { (void)core.attach(second, second_reply); },
        "noncanonical duplicate request admitted a second cursor");
    core.cancel(owner, key);

    const InputRecordKey invalid_key{guid, TuSeq{80}};
    const ExactTransaction invalid_tx = transaction_for(invalid_key.tu_seq, input);
    TxCommit invalid_commit = invalid_tx.commit;
    invalid_commit.raw_digest = {};
    const InputAttempt invalid_owner{809, 1};
    require_throws<std::invalid_argument>(
        [&] {
            (void)core.commit_open(invalid_owner, guid, invalid_tx.begin,
                                   invalid_commit, input);
        },
        "invalid commit was not rejected");
    const InputAttempt valid_owner{810, 1};
    require(core.commit_open(valid_owner, guid, invalid_tx.begin,
                             invalid_tx.commit, input) ==
                InputPublishResult::Published,
            "invalid commit poisoned a later valid owner");
    core.cancel(valid_owner, invalid_key);
}

void test_cancel_and_replacement_replay_recovery() {
    const CStoreGuid guid = Id128::from_u64(9);
    const std::vector<uint8_t> input = bytes(32, 9);
    InputAttachmentCore cancel_core(4, 4096, 2, 1);
    const InputAttempt cancel_owner{901, 1};
    const InputRecordKey cancel_key{guid, TuSeq{9}};
    const ExactTransaction cancel_tx =
        transaction_for(cancel_key.tu_seq, input);
    (void)cancel_core.commit_open(cancel_owner, guid, cancel_tx.begin,
                                  cancel_tx.commit, input);
    (void)cancel_core.request({cancel_key, cancel_owner, 91});
    cancel_core.cancel(cancel_owner, cancel_key);

    const InputAttempt next_owner{902, 1};
    const InputRecordKey next_key{guid, TuSeq{10}};
    const ExactTransaction next_tx = transaction_for(next_key.tu_seq, input);
    (void)cancel_core.commit_open(next_owner, guid, next_tx.begin,
                                  next_tx.commit, input);
    require(cancel_core.request({next_key, next_owner, 92}).status ==
                InputAttachmentStatus::Ready,
            "cancelled unacknowledged reply exhausted replay capacity");

    InputAttachmentCore replacement_core(4, 4096, 2, 1);
    const InputAttempt old_owner{903, 1};
    const InputAttempt new_owner{903, 2};
    const InputRecordKey replacement_key{guid, TuSeq{11}};
    const ExactTransaction replacement_tx =
        transaction_for(replacement_key.tu_seq, input);
    (void)replacement_core.commit_open(old_owner, guid, replacement_tx.begin,
                                       replacement_tx.commit, input);
    require_throws<std::invalid_argument>(
        [&] { replacement_core.retry(old_owner, old_owner.attempt_id,
                                     replacement_key); },
        "replacement reused ATTEMPT_ID");
    (void)replacement_core.request({replacement_key, old_owner, 93});
    replacement_core.retry(old_owner, new_owner.attempt_id, replacement_key);
    require(replacement_core.request({replacement_key, new_owner, 94}).status ==
                InputAttachmentStatus::Ready,
            "replacement stale reply exhausted replay capacity");
    replacement_core.cancel(new_owner, replacement_key);
}

void test_ack_pruning_and_owner_bound() {
    const CStoreGuid guid = Id128::from_u64(10);
    const InputAttempt owner{1001, 1};
    const InputRecordKey key{guid, TuSeq{12}};
    const std::vector<uint8_t> input = bytes(32, 10);
    const ExactTransaction tx = transaction_for(key.tu_seq, input);
    InputAttachmentCore core(4, 4096, 2, 1, 2);
    (void)core.commit_open(owner, guid, tx.begin, tx.commit, input);
    const InputAttachmentRequest first{key, owner, 101};
    const InputAttachmentReply reply = core.request(first);
    require(core.acknowledge(first, reply) == InputAttachmentAckResult::Accepted,
            "canonical ACK was rejected");
    (void)core.request({key, owner, 102});
    InputCursor cursor = core.attach(first, reply);
    require(drain(cursor) == input,
            "ACK pruning invalidated the canonical late attachment");
    core.cancel(owner, key);
    cursor = InputCursor{};
    core.collect_garbage();
    require(core.owner_count() == 0,
            "committed closed owner required a redundant explicit release");

    const InputAttempt closed_a{1002, 1};
    const InputAttempt closed_b{1003, 1};
    const InputRecordKey key_a{guid, TuSeq{13}};
    const InputRecordKey key_b{guid, TuSeq{14}};
    core.cancel(closed_a, key_a);
    core.cancel(closed_b, key_b);
    require(core.owner_count() == 2, "closed owner tombstone was lost early");
    require_throws<std::length_error>(
        [&] { core.cancel({1004, 1}, {guid, TuSeq{15}}); },
        "owner table exceeded its explicit bound");
    core.release_closed_owner(closed_a, key_a);
    core.cancel({1004, 1}, {guid, TuSeq{15}});
}

void test_cross_request_ack_prunes_all_observations() {
    const CStoreGuid guid = Id128::from_u64(21);
    const InputAttempt owner{2101, 1};
    const InputRecordKey key{guid, TuSeq{21}};
    const std::vector<uint8_t> input = bytes(40, 21);
    const ExactTransaction tx = transaction_for(key.tu_seq, input);
    InputAttachmentCore core(2, 4096, 2, 2);
    (void)core.commit_open(owner, guid, tx.begin, tx.commit, input);

    const InputAttachmentRequest first{key, owner, 211};
    const InputAttachmentRequest second{key, owner, 212};
    const InputAttachmentReply first_reply = core.request(first);
    const InputAttachmentReply second_reply = core.request(second);
    require(first_reply.event_id == second_reply.event_id &&
                core.replay_entry_count() == 2,
            "two request observations did not fill the replay table");
    require(core.acknowledge(second, second_reply) ==
                InputAttachmentAckResult::Accepted,
            "noncanonical exact request ACK was rejected");
    require(core.request(first).acknowledged,
            "one event ACK did not update its older canonical observation");

    const InputAttachmentRequest third{key, owner, 213};
    const InputAttachmentReply third_reply = core.request(third);
    require(third_reply.status == InputAttachmentStatus::ReadyReplay &&
                core.replay_entry_count() == 1,
            "ACKed observations were not pruned before the next request");
    InputCursor cursor = core.attach(first, core.request(first));
    require(drain(cursor) == input,
            "pruning invalidated canonical late attachment authorization");
    core.cancel(owner, key);
}

void test_unknown_closed_callbacks_do_not_create_owners() {
    const CStoreGuid guid = Id128::from_u64(22);
    const InputAttempt owner{2201, 1};
    const InputRecordKey key{guid, TuSeq{22}};
    const std::vector<uint8_t> input = bytes(24, 22);
    const ExactTransaction tx = transaction_for(key.tu_seq, input);
    InputAttachmentCore core(2, 4096, 2, 2, 1);

    require_throws<std::out_of_range>(
        [&] {
            (void)core.observe_closed_commit(owner, guid, tx.begin,
                                             tx.commit, input);
        },
        "unknown closed commit callback was accepted");
    require(core.owner_count() == 0,
            "unknown closed commit callback poisoned owner capacity");
    require_throws<std::out_of_range>(
        [&] { core.release_closed_owner(owner, key); },
        "unknown closed owner release was accepted");
    require(core.owner_count() == 0,
            "unknown release callback poisoned owner capacity");

    const InputAttempt valid{2202, 1};
    core.cancel(valid, key);
    require(core.owner_count() == 1,
            "invalid callbacks exhausted the bounded owner table");
    core.release_closed_owner(valid, key);
    require(core.owner_count() == 0,
            "valid closed owner did not release after invalid callbacks");
}

void test_generation_and_late_closed_commit() {
    const CStoreGuid guid = Id128::from_u64(11);
    const InputAttempt owner{1101, 1};
    const InputRecordKey key{guid, TuSeq{16}};
    const std::vector<uint8_t> input = bytes(24, 11);
    const ExactTransaction tx = transaction_for(key.tu_seq, input);
    InputAttachmentCore core(4, 4096, 2);
    core.cancel(owner, key);
    require(core.observe_closed_commit(owner, guid, tx.begin, tx.commit, input) ==
                InputPublishResult::NotRetainedJobClosed,
            "late closed commit was not validated through its tombstone");
    require(core.owner_count() == 0,
            "observed closed owner was not reclaimable");

    const InputAttempt open_owner{1102, 1};
    const InputRecordKey open_key{guid, TuSeq{17}};
    const ExactTransaction open_tx = transaction_for(open_key.tu_seq, input);
    (void)core.commit_open(open_owner, guid, open_tx.begin, open_tx.commit,
                           input);
    InputCursor old_cursor = core.attach(
        {open_key, open_owner, 111}, core.request({open_key, open_owner, 111}));
    core.clear();
    require(core.store_generation() == 2, "clear did not advance store generation");
    require_throws<std::logic_error>(
        [&] { (void)core.commit_open(open_owner, guid, open_tx.begin,
                                     open_tx.commit, input); },
        "pre-clear commit crossed the generation fence");
    require_throws<std::logic_error>(
        [&] { (void)core.request({open_key, open_owner, 112}); },
        "pre-clear request crossed the generation fence");
    require(drain(old_cursor) == input,
            "store clear invalidated a previously issued cursor");
    const InputAttempt replacement{open_owner.logical_job,
                                   open_owner.attempt_id + 1,
                                   core.store_generation()};
    const ExactTransaction replacement_tx =
        transaction_for(open_key.tu_seq, bytes(24, 12));
    const std::vector<uint8_t> replacement_input = bytes(24, 12);
    (void)core.commit_open(replacement, guid, replacement_tx.begin,
                           replacement_tx.commit, replacement_input);
    old_cursor = InputCursor{};
    core.cancel(replacement, open_key);
}

#ifdef P50_ATTACHMENT_TEST_SEAMS
void test_transactional_failure_seams_and_event_overflow() {
    const CStoreGuid guid = Id128::from_u64(12);
    const InputAttempt owner{1201, 1};
    const InputRecordKey key{guid, TuSeq{18}};
    const std::vector<uint8_t> input = bytes(16, 12);
    const ExactTransaction tx = transaction_for(key.tu_seq, input);

    InputAttachmentCore lifecycle_failure(4, 4096, 2);
    lifecycle_failure.test_fail_next_lifecycle_insert();
    require_throws<std::bad_alloc>(
        [&] {
            (void)lifecycle_failure.commit_open(owner, guid, tx.begin,
                                                tx.commit, input);
        },
        "lifecycle insertion failure did not propagate");
    require(lifecycle_failure.owner_count() == 0 &&
                lifecycle_failure.record_count() == 0 &&
                lifecycle_failure.pending_ready_count() == 0,
            "lifecycle insertion failure left partial state");
    (void)lifecycle_failure.commit_open(owner, guid, tx.begin, tx.commit,
                                       input);

    InputAttachmentCore reply_failure(4, 4096, 2);
    (void)reply_failure.commit_open(owner, guid, tx.begin, tx.commit, input);
    reply_failure.test_fail_next_reply_insert();
    require_throws<std::bad_alloc>(
        [&] { (void)reply_failure.request({key, owner, 121}); },
        "reply insertion failure did not propagate");
    require(reply_failure.pending_ready_count() == 1 &&
                reply_failure.replay_entry_count() == 0,
            "reply insertion failure changed canonical state");
    (void)reply_failure.request({key, owner, 121});

    InputAttachmentCore overflow(4, 4096, 2);
    overflow.test_set_next_event_id(std::numeric_limits<uint64_t>::max());
    (void)overflow.commit_open(owner, guid, tx.begin, tx.commit, input);
    const InputRecordKey second_key{guid, TuSeq{19}};
    const ExactTransaction second_tx = transaction_for(second_key.tu_seq, input);
    require_throws<std::overflow_error>(
        [&] {
            (void)overflow.commit_open(owner, guid, second_tx.begin,
                                       second_tx.commit, input);
        },
        "event ID exhaustion was not rejected transactionally");
    require(!overflow.contains(second_key) && overflow.owner_count() == 1,
            "event ID exhaustion left partial state");
}
#endif

}  // namespace

int main() {
    test_commit_ready_ack_replay_and_reclaim();
    test_owner_replacement_and_stale_identity();
    test_replacement_before_commit_publishes_ready();
    test_closed_before_commit_and_table_exhaustion();
    test_wrong_guid_tu_owner_and_perturbation();
    test_same_key_conflict_and_duplicate_attachment();
    test_cancel_and_replacement_replay_recovery();
    test_ack_pruning_and_owner_bound();
    test_cross_request_ack_prunes_all_observations();
    test_unknown_closed_callbacks_do_not_create_owners();
    test_generation_and_late_closed_commit();
#ifdef P50_ATTACHMENT_TEST_SEAMS
    test_transactional_failure_seams_and_event_overflow();
#endif
    std::cout << "p50_input_attachment_test: ownership, ready, replay, and reclamation gates passed\n";
    return 0;
}
