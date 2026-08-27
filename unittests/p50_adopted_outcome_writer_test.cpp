#include "../cache/p50_adopted_outcome_writer.h"

#include <stdexcept>
#include <utility>

using namespace icecc::p50;
using namespace icecc::p50::sidecar;

namespace {

void check(bool value, const char* expression) {
    if (!value)
        throw std::runtime_error(expression);
}
#define CHECK(expression) check((expression), #expression)

struct FakeLease final : P5coAdoptedSocketLease {
    struct Reply { P5coWriteKind kind; size_t bytes; };
    std::vector<Reply> replies;
    std::vector<uint8_t> observed;
    uint8_t observed_flags = 0;
    size_t revalidations = 0;
    size_t sends = 0;
    size_t fences = 0;
    bool owned = true;
    bool drop_on_second_revalidation = false;

    bool revalidate(const local::Identity& identity) const noexcept override {
        auto* self = const_cast<FakeLease*>(this);
        ++self->revalidations;
        if (identity.generation != 41 || identity.attempt != 7)
            return false;
        if (drop_on_second_revalidation && self->revalidations >= 2)
            self->owned = false;
        return self->owned;
    }

    P5coWriteResult send_nonblocking(std::span<const uint8_t> bytes,
                                     uint8_t flags) noexcept override {
        ++sends;
        observed_flags = flags;
        observed.insert(observed.end(), bytes.begin(), bytes.end());
        if (replies.empty())
            return {P5coWriteKind::Error, 0};
        const Reply reply = replies.front();
        replies.erase(replies.begin());
        return {reply.kind, reply.bytes};
    }

    void fence() noexcept override { ++fences; owned = false; }
};

constexpr local::Identity kOperation{41, 7};
constexpr MonotonicClockIdentity kClock{3, 9};
constexpr AbsoluteMonotonicDeadline kDeadline{1000, 3, 9};

std::unique_ptr<FakeLease> fake(FakeLease*& raw) {
    auto lease = std::make_unique<FakeLease>();
    raw = lease.get();
    return lease;
}

AdoptedOutcomeWriter writer(std::unique_ptr<P5coAdoptedSocketLease> lease,
                            size_t max_bytes = 4096) {
    return AdoptedOutcomeWriter(std::move(lease), {1, 2, 3, 4, 5}, kOperation,
                                 kDeadline, {max_bytes});
}

void immediate_expiry() {
    FakeLease* raw = nullptr;
    auto value = writer(fake(raw));
    CHECK(value.advance(1000, kClock, POLLOUT) == P5coWriterState::FailedAfterDetach);
    CHECK(value.failure() == P5coFailure::Expired && value.offset() == 0);
    CHECK(raw->sends == 0 && raw->fences == 1);
}

void partial_then_expiry() {
    FakeLease* raw = nullptr;
    auto lease = fake(raw);
    raw->replies.push_back({P5coWriteKind::Sent, 2});
    auto value = writer(std::move(lease), 2);
    CHECK(value.advance(999, kClock, POLLOUT) == P5coWriterState::Writing);
    CHECK(value.offset() == 2 && raw->sends == 1);
    CHECK(value.advance(1000, kClock, POLLOUT) == P5coWriterState::FailedAfterDetach);
    CHECK(value.failure() == P5coFailure::Expired && raw->sends == 1);
}

void pollout_just_before_expiry() {
    FakeLease* raw = nullptr;
    auto lease = fake(raw);
    raw->replies.push_back({P5coWriteKind::Sent, 5});
    auto value = writer(std::move(lease));
    CHECK(value.advance(999, kClock, POLLOUT) == P5coWriterState::FullyFlushed);
    CHECK(value.offset() == 5 && raw->observed_flags ==
          (P5coSendFlag::DontWait | P5coSendFlag::NoSignal));
    CHECK(raw->fences == 0);
}

void full_flush_then_expiry_before_hello() {
    FakeLease* raw = nullptr;
    auto lease = fake(raw);
    raw->replies.push_back({P5coWriteKind::Sent, 5});
    auto value = writer(std::move(lease));
    CHECK(value.advance(999, kClock, POLLOUT) == P5coWriterState::FullyFlushed);
    CHECK(value.take_for_endpoint(1000, kClock) == nullptr);
    CHECK(value.failure() == P5coFailure::EndpointStartExpired && raw->fences == 1);
}

void deadline_is_not_renewed() {
    FakeLease* raw = nullptr;
    auto lease = fake(raw);
    raw->replies.push_back({P5coWriteKind::Sent, 2});
    raw->replies.push_back({P5coWriteKind::Sent, 2});
    auto value = writer(std::move(lease), 2);
    CHECK(value.advance(900, kClock, POLLOUT) == P5coWriterState::Writing);
    CHECK(value.advance(999, kClock, POLLOUT) == P5coWriterState::Writing);
    CHECK(value.advance(1000, kClock, POLLOUT) == P5coWriterState::FailedAfterDetach);
    CHECK(value.offset() == 4 && raw->sends == 2);
}

void dropped_prewrite_check_is_caught() {
    FakeLease* raw = nullptr;
    auto lease = fake(raw);
    raw->drop_on_second_revalidation = true;
    raw->replies.push_back({P5coWriteKind::Sent, 5});
    auto value = writer(std::move(lease));
    CHECK(value.advance(999, kClock, POLLOUT) == P5coWriterState::FailedAfterDetach);
    CHECK(value.failure() == P5coFailure::OwnershipLost && raw->sends == 0);
}

} // namespace

int main() {
    immediate_expiry();
    partial_then_expiry();
    pollout_just_before_expiry();
    full_flush_then_expiry_before_hello();
    deadline_is_not_renewed();
    dropped_prewrite_check_is_caught();
    return 0;
}
