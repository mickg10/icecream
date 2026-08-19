#include "p29_transfer_transaction.h"

#include <cstdio>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}
p29::Opaque128 opaque(uint8_t base) {
    p29::Opaque128 value{};
    for (size_t i = 0; i < value.size(); ++i) value[i] = uint8_t(base + i);
    return value;
}
}  // namespace

int main() {
    const p29::RouteScope scope{opaque(1), opaque(31), 7};
    p29::ReceiverTxnLedger ledger(scope);
    const p29::TransferTxnId first{scope, 0}, second{scope, 1}, gap{scope, 2};
    const p29::Digest128 tx0{10, 11}, tx0wrong{12, 13}, out0{20, 21};
    const p29::Digest128 tx1{30, 31}, out1{40, 41};

    const auto empty = ledger.state_mark();
    check(ledger.begin(first, tx0) == p29::BeginResult::Ready, "first transaction not accepted");
    check(ledger.state_mark().pending_sequence == 0 &&
              ledger.state_mark().pending_transaction_digest == tx0,
          "state mark does not identify the pending transaction");
    check(ledger.begin(first, tx0) == p29::BeginResult::PendingDuplicate,
          "same in-progress transaction was not identified");
    check(ledger.begin(first, tx0wrong) == p29::BeginResult::DigestMismatch,
          "same sequence with different transaction digest was accepted");
    check(ledger.begin(second, tx1) == p29::BeginResult::Busy,
          "a second transaction was accepted while one was pending");
    check(ledger.abort(), "pending transaction did not abort");
    check(ledger.state_mark() == empty, "abort changed the receiver ledger");

    check(ledger.begin(first, tx0) == p29::BeginResult::Ready,
          "identical retry after abort was refused");
    p29::AckReceipt ack0;
    check(ledger.commit(out0, 1234, ack0), "retry did not commit");
    check(ack0.id == first && ack0.transaction_digest == tx0 &&
              ack0.output_digest == out0 && ack0.output_extent == 1234,
          "Ack did not retain exact identity/digests/extent");

    const auto committed = ledger.state_mark();
    p29::AckReceipt duplicate;
    check(ledger.begin(first, tx0, &duplicate) == p29::BeginResult::DuplicateCommitted,
          "committed retry did not return the retained Ack");
    check(duplicate == ack0, "duplicate delivery returned a different Ack");
    check(ledger.state_mark() == committed, "duplicate delivery changed receiver state");
    check(ledger.begin(first, tx0wrong) == p29::BeginResult::DigestMismatch,
          "committed sequence accepted a different transaction digest");
    check(ledger.begin(gap, tx1) == p29::BeginResult::SequenceMismatch,
          "a sequence gap was accepted");

    check(ledger.begin(second, tx1) == p29::BeginResult::Ready,
          "next sequence was refused");
    p29::AckReceipt ack1;
    check(ledger.commit(out1, 5678, ack1), "next sequence did not commit");
    check(ledger.begin(first, tx0) == p29::BeginResult::SequenceMismatch,
          "an old transaction beyond the retained-Ack window was accepted");

    p29::RouteScope other = scope; ++other.f_cache_epoch;
    check(ledger.begin({other, 2}, {50, 51}) == p29::BeginResult::ScopeMismatch,
          "a different F cache epoch entered this route ledger");
    check(!ledger.abort(), "abort without a pending transaction was accepted");

    std::printf("P29 transfer transaction identity/retry/Ack %s\n",
                failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
