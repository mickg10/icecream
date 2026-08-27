#include "../cache/p50_adopted_outcome_writer.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

using namespace icecc::p50;
using namespace icecc::p50::daemon;
using namespace icecc::p50::sidecar;

namespace {

void check(bool value, const char* expression) {
    if (!value)
        throw std::runtime_error(expression);
}
#define CHECK(expression) check((expression), #expression)

struct FakeObservations final : MonotonicObservationSource {
    explicit FakeObservations(std::vector<MonotonicObservation> values)
        : values(std::move(values)) {}

    std::optional<MonotonicObservation> observe() noexcept override {
        ++calls;
        if (next == values.size())
            return std::nullopt;
        return values[next++];
    }

    std::vector<MonotonicObservation> values;
    size_t next = 0;
    size_t calls = 0;
};

struct FakeLease final : P5coAdoptedSocketLease {
    struct Reply { P5coWriteKind kind; size_t bytes; };

    explicit FakeLease(P50CacheSessionOutcome expected,
                       AbsoluteMonotonicDeadline expected_deadline)
        : expected(std::move(expected)), expected_deadline(expected_deadline) {}

    ~FakeLease() override {
        if (fence_snapshot)
            *fence_snapshot = fences;
    }

    bool revalidate(const P50CacheSessionOutcome& value,
                    const AbsoluteMonotonicDeadline& deadline) const noexcept override {
        auto* self = const_cast<FakeLease*>(this);
        ++self->revalidations;
        if (value != expected || deadline != expected_deadline)
            return false;
        if (drop_on_second_revalidation && self->revalidations >= 2)
            return false;
        return owned;
    }

    P5coWriteResult send_nonblocking(std::span<const uint8_t> bytes,
                                     uint8_t flags) noexcept override {
        ++sends;
        observed_flags = flags;
        if (replies.empty())
            return {P5coWriteKind::Error, 0};
        const Reply reply = replies.front();
        replies.erase(replies.begin());
        // A real syscall accounts only the bytes it reports.  In particular,
        // a malformed oversize result never contributes bytes to the witness.
        if (reply.kind == P5coWriteKind::Sent && reply.bytes != 0 &&
            reply.bytes <= bytes.size())
            observed.insert(observed.end(), bytes.begin(),
                            bytes.begin() + static_cast<ptrdiff_t>(reply.bytes));
        return {reply.kind, reply.bytes};
    }

    void fence() noexcept override { ++fences; owned = false; }

    P50CacheSessionOutcome expected;
    AbsoluteMonotonicDeadline expected_deadline;
    std::vector<Reply> replies;
    std::vector<uint8_t> observed;
    uint8_t observed_flags = 0;
    size_t revalidations = 0;
    size_t sends = 0;
    size_t fences = 0;
    bool owned = true;
    bool drop_on_second_revalidation = false;
    std::shared_ptr<size_t> fence_snapshot = std::make_shared<size_t>(0);
};

std::array<uint8_t, 16> c_guid() {
    std::array<uint8_t, 16> result{};
    for (size_t i = 0; i != result.size(); ++i)
        result[i] = static_cast<uint8_t>(i + 1);
    result[kStoreIdentityRoleByte] &= static_cast<uint8_t>(~kStoreIdentityRoleMask);
    return result;
}

std::array<uint8_t, 16> f_guid() {
    std::array<uint8_t, 16> result{};
    for (size_t i = 0; i != result.size(); ++i)
        result[i] = static_cast<uint8_t>(i + 41);
    result[kStoreIdentityRoleByte] |= kStoreIdentityRoleMask;
    return result;
}

ClaimAttemptCapability128 capability(uint8_t seed) {
    ClaimAttemptCapability128 result;
    for (size_t i = 0; i != result.bytes.size(); ++i)
        result.bytes[i] = static_cast<uint8_t>(seed + i);
    return result;
}

P50CacheSessionOutcome adopted(uint64_t operation_sequence = 91) {
    P50CacheSessionWireClaim claim;
    auto& arm = claim.binding.arm;
    arm.wire_job_id = 17;
    arm.assignment_epoch = 18;
    arm.assignment_nonce = 19;
    arm.selected_f_host = "f.example.test";
    arm.selected_f_ordinary_port = 10250;
    arm.selected_f_cache_port = 10251;
    arm.cache_protocol = CACHE_WIRE_PROTOCOL_V1;
    arm.cache_profile = CACHE_PROFILE_ZSTD_TU;
    arm.logical_job = 20;
    arm.compiler_attempt = 21;
    arm.c_store_generation = 22;
    arm.c_store_derivation_version = kStoreIdentityDerivationVersion;
    arm.c_store_guid = c_guid();
    arm.source_request_id = 23;
    arm.source_mode = P50_SOURCE_MODE_ZSTD_TU;
    arm.c_control_generation = 24;
    arm.c_control_attempt = 25;
    claim.binding.f_control_generation = 31;
    claim.binding.f_control_attempt = 32;
    claim.binding.f_store_generation = 33;
    claim.binding.f_store_guid = f_guid();
    claim.binding.f_store_derivation_version = kStoreIdentityDerivationVersion;
    claim.binding.arm_observation_id = 34;
    claim.binding.source_budget_msec = 5000;
    claim.attempt = {1, capability(51)};
    CHECK(claim.valid());

    P50CacheSessionOutcome outcome;
    outcome.kind = P50CacheSessionOutcomeKind::Adopted;
    outcome.canonical_claim = encode_cache_session_wire_claim(claim);
    outcome.f_sidecar_launch = claim.binding.f_control_identity();
    outcome.f_store_guid = claim.binding.f_store_guid;
    outcome.operation = {outcome.f_sidecar_launch,
                         P50SessionOperationRole::FSession,
                         operation_sequence};
    CHECK(outcome.valid());
    return outcome;
}

constexpr MonotonicClockIdentity kClock{3, 9};
constexpr AbsoluteMonotonicDeadline kDeadline{1000, 3, 9};

std::unique_ptr<FakeLease> lease_for(const P50CacheSessionOutcome& outcome,
                                     FakeLease*& raw) {
    auto lease = std::make_unique<FakeLease>(outcome, kDeadline);
    raw = lease.get();
    return lease;
}

std::unique_ptr<FakeObservations> observations(
    std::initializer_list<int64_t> values, FakeObservations*& raw) {
    std::vector<MonotonicObservation> result;
    for (const int64_t value : values)
        result.push_back({value, kClock});
    auto source = std::make_unique<FakeObservations>(std::move(result));
    raw = source.get();
    return source;
}

AdoptedOutcomeWriter make_writer(
    std::unique_ptr<P5coAdoptedSocketLease> lease,
    std::unique_ptr<MonotonicObservationSource> source,
    AbsoluteMonotonicDeadline deadline = kDeadline,
    size_t max_bytes = 4096,
    uint64_t operation_sequence = 91) {
    return AdoptedOutcomeWriter(std::move(lease), adopted(operation_sequence),
                                std::move(source), deadline, {max_bytes});
}

void constructor_rejects_arbitrary_bytes_and_bare_identity() {
    // The constructor is typed around P50CacheSessionOutcome; this row also
    // ensures a refusal DTO cannot become a P5CO writer.
    auto refused = adopted();
    refused.kind = P50CacheSessionOutcomeKind::RefusedPreDetach;
    refused.refusal_reason = P50CacheSessionRefusalReason::Deadline;
    FakeLease* raw_lease = nullptr;
    FakeObservations* raw_source = nullptr;
    auto lease = std::make_unique<FakeLease>(refused, kDeadline);
    raw_lease = lease.get();
    auto value = AdoptedOutcomeWriter(
        std::move(lease), std::move(refused),
        observations({900}, raw_source), kDeadline);
    CHECK(value.state() == P5coWriterState::FailedAfterDetach);
    CHECK(value.canonical_frame().empty());
    CHECK(raw_lease->fences == 0);
}

void immediate_expiry() {
    const auto outcome = adopted();
    FakeLease* raw = nullptr;
    FakeObservations* source = nullptr;
    auto value = make_writer(lease_for(outcome, raw), observations({1000}, source));
    CHECK(value.advance(POLLOUT) == P5coWriterState::FailedAfterDetach);
    CHECK(value.failure() == P5coFailure::Expired && value.offset() == 0);
    CHECK(source->calls == 1 && raw->sends == 0 && raw->fences == 1);
}

void partial_then_expiry_and_no_later_byte() {
    const auto outcome = adopted();
    FakeLease* raw = nullptr;
    FakeObservations* source = nullptr;
    auto lease = lease_for(outcome, raw);
    raw->replies.push_back({P5coWriteKind::Sent, 2});
    auto value = make_writer(std::move(lease), observations({900, 900, 1000}, source),
                             kDeadline, 2);
    CHECK(value.advance(POLLOUT) == P5coWriterState::Writing);
    CHECK(value.offset() == 2 && raw->observed == std::vector<uint8_t>({0x50, 0x35}));
    CHECK(value.advance(POLLOUT) == P5coWriterState::FailedAfterDetach);
    CHECK(value.failure() == P5coFailure::Expired && value.offset() == 2);
    CHECK(value.advance(POLLOUT) == P5coWriterState::FailedAfterDetach);
    CHECK(raw->sends == 1 && raw->fences == 1);
}

void stale_first_vs_second_observations() {
    const auto outcome = adopted();
    FakeLease* raw = nullptr;
    FakeObservations* source = nullptr;
    auto lease = lease_for(outcome, raw);
    raw->replies.push_back({P5coWriteKind::Sent,
                            encode_cache_session_outcome(outcome).size()});
    auto value = make_writer(std::move(lease), observations({999, 1000}, source));
    CHECK(value.advance(POLLOUT) == P5coWriterState::FailedAfterDetach);
    CHECK(value.failure() == P5coFailure::Expired && value.offset() == 0);
    CHECK(raw->sends == 0 && raw->fences == 1 && source->calls == 2);
}

void pollout_just_before_expiry_and_canonical_frame() {
    const auto outcome = adopted();
    const auto expected = encode_cache_session_outcome(outcome);
    FakeLease* raw = nullptr;
    FakeObservations* source = nullptr;
    auto lease = lease_for(outcome, raw);
    raw->replies.push_back({P5coWriteKind::Sent, expected.size()});
    auto value = make_writer(std::move(lease), observations({999, 999}, source));
    CHECK(value.advance(POLLOUT) == P5coWriterState::FullyFlushed);
    CHECK(std::equal(value.canonical_frame().begin(), value.canonical_frame().end(),
                     expected.begin(), expected.end()) && raw->observed == expected);
    CHECK(raw->observed_flags == (P5coSendFlag::DontWait | P5coSendFlag::NoSignal));
    CHECK(raw->fences == 0);
}

void full_flush_then_expiry_before_endpoint() {
    const auto outcome = adopted();
    FakeLease* raw = nullptr;
    FakeObservations* source = nullptr;
    auto lease = lease_for(outcome, raw);
    raw->replies.push_back({P5coWriteKind::Sent,
                            encode_cache_session_outcome(outcome).size()});
    auto value = make_writer(std::move(lease), observations({900, 900, 1000}, source));
    CHECK(value.advance(POLLOUT) == P5coWriterState::FullyFlushed);
    CHECK(value.take_for_endpoint() == nullptr);
    CHECK(value.failure() == P5coFailure::EndpointStartExpired);
    CHECK(raw->fences == 1 && source->calls == 3);
}

void full_flush_then_fresh_endpoint_transfer() {
    const auto outcome = adopted();
    FakeLease* raw = nullptr;
    FakeObservations* source = nullptr;
    auto lease = lease_for(outcome, raw);
    raw->replies.push_back({P5coWriteKind::Sent,
                            encode_cache_session_outcome(outcome).size()});
    auto value = make_writer(std::move(lease), observations({999, 999, 999}, source));
    CHECK(value.advance(POLLOUT) == P5coWriterState::FullyFlushed);
    auto transferred = value.take_for_endpoint();
    CHECK(transferred != nullptr && value.state() == P5coWriterState::FullyFlushed);
    CHECK(raw->revalidations == 3 && source->calls == 3 && raw->fences == 0);
}

void deadline_renewal_mutant_is_caught() {
    const auto outcome = adopted();
    FakeLease* raw = nullptr;
    FakeObservations* source = nullptr;
    auto lease = lease_for(outcome, raw);
    raw->replies.push_back({P5coWriteKind::Sent, 2});
    raw->replies.push_back({P5coWriteKind::Sent, 2});
    auto sample_source = observations({900, 900, 999, 999, 1000}, source);
    auto value = make_writer(std::move(lease), std::move(sample_source),
                             kDeadline, 2);
    CHECK(value.advance(POLLOUT) == P5coWriterState::Writing);
    CHECK(value.advance(POLLOUT) == P5coWriterState::Writing);
    CHECK(value.advance(POLLOUT) == P5coWriterState::FailedAfterDetach);
    CHECK(value.offset() == 4 && raw->sends == 2);
}

void dropped_prewrite_identity_check_is_caught() {
    const auto outcome = adopted();
    FakeLease* raw = nullptr;
    FakeObservations* source = nullptr;
    auto lease = lease_for(outcome, raw);
    raw->drop_on_second_revalidation = true;
    raw->replies.push_back({P5coWriteKind::Sent,
                            encode_cache_session_outcome(outcome).size()});
    auto value = make_writer(std::move(lease), observations({999, 999}, source));
    CHECK(value.advance(POLLOUT) == P5coWriterState::FailedAfterDetach);
    CHECK(value.failure() == P5coFailure::OwnershipLost && raw->sends == 0);
    CHECK(raw->revalidations == 2);
}

void exact_outcome_mismatch_is_caught() {
    const auto expected = adopted(91);
    const auto different = adopted(92);
    FakeLease* raw = nullptr;
    FakeObservations* source = nullptr;
    auto lease = lease_for(expected, raw);
    auto value = AdoptedOutcomeWriter(std::move(lease), different,
                                      observations({999}, source), kDeadline);
    CHECK(value.advance(POLLOUT) == P5coWriterState::FailedAfterDetach);
    CHECK(value.failure() == P5coFailure::OwnershipLost && raw->sends == 0);
}

void eagain_eintr_zero_oversize_and_error() {
    for (const auto kind : {P5coWriteKind::WouldBlock, P5coWriteKind::Interrupted}) {
        const auto outcome = adopted();
        FakeLease* raw = nullptr;
        FakeObservations* source = nullptr;
        auto lease = lease_for(outcome, raw);
        raw->replies.push_back({kind, 0});
        auto value = make_writer(std::move(lease), observations({999, 999}, source));
        CHECK(value.advance(POLLOUT) == P5coWriterState::Writing);
        CHECK(value.offset() == 0 && raw->sends == 1 && raw->fences == 0);
    }
    for (const auto reply : std::array<FakeLease::Reply, 3>{
             FakeLease::Reply{P5coWriteKind::Sent, 0},
             FakeLease::Reply{P5coWriteKind::Sent, 5000},
             FakeLease::Reply{P5coWriteKind::Error, 0}}) {
        const auto outcome = adopted();
        FakeLease* raw = nullptr;
        FakeObservations* source = nullptr;
        auto lease = lease_for(outcome, raw);
        raw->replies.push_back(reply);
        auto value = make_writer(std::move(lease), observations({999, 999}, source));
        CHECK(value.advance(POLLOUT) == P5coWriterState::FailedAfterDetach);
        CHECK(value.failure() == P5coFailure::SendError && value.offset() == 0);
        CHECK(raw->sends == 1 && raw->fences == 1);
    }
}

void terminal_precedence_and_ownership_loss() {
    {
        const auto outcome = adopted();
        FakeLease* raw = nullptr;
        FakeObservations* source = nullptr;
        auto value = make_writer(lease_for(outcome, raw), observations({1000}, source));
        CHECK(value.advance(POLLOUT | POLLHUP) == P5coWriterState::FailedAfterDetach);
        CHECK(value.failure() == P5coFailure::TerminalEvent && raw->sends == 0);
    }
    {
        const auto outcome = adopted();
        FakeLease* raw = nullptr;
        FakeObservations* source = nullptr;
        auto lease = lease_for(outcome, raw);
        raw->owned = false;
        auto value = make_writer(std::move(lease), observations({999}, source));
        CHECK(value.advance(POLLOUT) == P5coWriterState::FailedAfterDetach);
        CHECK(value.failure() == P5coFailure::OwnershipLost && raw->sends == 0);
    }
}

void move_and_destruction_fence_exactly_once() {
    const auto outcome = adopted();
    {
        FakeLease* invalid_raw = nullptr;
        FakeObservations* source = nullptr;
        auto invalid = outcome;
        invalid.kind = P50CacheSessionOutcomeKind::RefusedPreDetach;
        invalid.refusal_reason = P50CacheSessionRefusalReason::Deadline;
        auto lease = std::make_unique<FakeLease>(invalid, kDeadline);
        invalid_raw = lease.get();
        const auto fence_snapshot = invalid_raw->fence_snapshot;
        { AdoptedOutcomeWriter value(std::move(lease), std::move(invalid),
                                      observations({999}, source), kDeadline); }
        CHECK(*fence_snapshot == 1);
    }
    {
        FakeLease* raw = nullptr;
        FakeObservations* source = nullptr;
        auto value_lease = lease_for(outcome, raw);
        const auto fence_snapshot = raw->fence_snapshot;
        { auto value = make_writer(std::move(value_lease), observations({999}, source)); }
        CHECK(*fence_snapshot == 1);
    }
    {
        FakeLease* raw = nullptr;
        FakeObservations* source = nullptr;
        auto lease = lease_for(outcome, raw);
        const auto fence_snapshot = raw->fence_snapshot;
        raw->replies.push_back({P5coWriteKind::Sent, 1});
        { auto value = make_writer(std::move(lease), observations({999, 999}, source),
                                   kDeadline, 1); CHECK(value.advance(POLLOUT) ==
                                                         P5coWriterState::Writing); }
        CHECK(*fence_snapshot == 1);
    }
    {
        FakeLease* raw = nullptr;
        FakeObservations* source = nullptr;
        auto lease = lease_for(outcome, raw);
        const auto fence_snapshot = raw->fence_snapshot;
        raw->replies.push_back({P5coWriteKind::Sent,
                                encode_cache_session_outcome(outcome).size()});
        { auto value = make_writer(std::move(lease), observations({999, 999}, source));
          CHECK(value.advance(POLLOUT) == P5coWriterState::FullyFlushed); }
        CHECK(*fence_snapshot == 1);
    }
    {
        FakeLease* first = nullptr;
        FakeLease* second = nullptr;
        FakeObservations* source = nullptr;
        auto left = make_writer(lease_for(outcome, first), observations({999}, source));
        auto right = make_writer(lease_for(outcome, second), observations({999}, source));
        const auto second_fence_snapshot = second->fence_snapshot;
        right = std::move(left);
        CHECK(first->fences == 0 && *second_fence_snapshot == 1);
        CHECK(right.state() == P5coWriterState::Ready);
    }
    {
        FakeLease* raw = nullptr;
        FakeObservations* source = nullptr;
        auto value = make_writer(lease_for(outcome, raw), observations({999}, source));
        auto transferred = std::move(value);
        CHECK(raw->fences == 0 && value.state() == P5coWriterState::FailedAfterDetach);
        CHECK(transferred.state() == P5coWriterState::Ready);
    }
}

} // namespace

int main() {
    constructor_rejects_arbitrary_bytes_and_bare_identity();
    immediate_expiry();
    partial_then_expiry_and_no_later_byte();
    stale_first_vs_second_observations();
    pollout_just_before_expiry_and_canonical_frame();
    full_flush_then_expiry_before_endpoint();
    full_flush_then_fresh_endpoint_transfer();
    deadline_renewal_mutant_is_caught();
    dropped_prewrite_identity_check_is_caught();
    exact_outcome_mismatch_is_caught();
    eagain_eintr_zero_oversize_and_error();
    terminal_precedence_and_ownership_loss();
    move_and_destruction_fence_exactly_once();
    return 0;
}
