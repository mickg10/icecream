#include "../cache/p50_adopted_outcome_writer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

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
    explicit FakeObservations(std::vector<MonotonicObservation> values,
                              std::vector<char>* events = nullptr)
        : values(std::move(values)), events(events) {}

    std::optional<MonotonicObservation> observe() noexcept override {
        ++calls;
        if (events != nullptr)
            events->push_back('O');
        if (after_observe)
            after_observe();
        if (next == values.size())
            return std::nullopt;
        return values[next++];
    }

    std::vector<MonotonicObservation> values;
    std::vector<char>* events = nullptr;
    std::function<void()> after_observe;
    size_t next = 0;
    size_t calls = 0;
};

struct FakeLease final : P5coAdoptedSocketLease {
    struct Reply {
        P5coWriteKind kind;
        size_t bytes;
    };

    explicit FakeLease(P50CacheSessionOutcome expected,
                       AbsoluteMonotonicDeadline expected_deadline,
                       std::vector<char>* events = nullptr)
        : expected(std::move(expected)), expected_deadline(expected_deadline),
          events(events) {}

    ~FakeLease() override {
        if (fence_snapshot)
            *fence_snapshot = fences;
    }

    bool revalidate(const P50CacheSessionOutcome& value,
                    const AbsoluteMonotonicDeadline& deadline) const noexcept override {
        auto* self = const_cast<FakeLease*>(this);
        ++self->revalidations;
        if (self->events != nullptr)
            self->events->push_back('R');
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
        if (events != nullptr)
            events->push_back('S');
        if (replies.empty())
            return {P5coWriteKind::Error, 0};
        const Reply reply = replies.front();
        replies.erase(replies.begin());
        // Count only the bytes the fake syscall reports, and only for a
        // bounded successful result.  This makes a later-byte witness exact.
        if (reply.kind == P5coWriteKind::Sent && reply.bytes != 0 &&
            reply.bytes <= bytes.size())
            observed.insert(observed.end(), bytes.begin(),
                            bytes.begin() + static_cast<std::ptrdiff_t>(reply.bytes));
        return {reply.kind, reply.bytes};
    }

    void fence() noexcept override {
        ++fences;
        owned = false;
        if (events != nullptr)
            events->push_back('F');
    }

    P50CacheSessionOutcome expected;
    AbsoluteMonotonicDeadline expected_deadline;
    std::vector<char>* events = nullptr;
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
                                     FakeLease*& raw,
                                     std::vector<char>* events = nullptr) {
    auto lease = std::make_unique<FakeLease>(outcome, kDeadline, events);
    raw = lease.get();
    return lease;
}

std::unique_ptr<FakeObservations> observations(
    std::initializer_list<int64_t> values, FakeObservations*& raw,
    std::vector<char>* events = nullptr) {
    std::vector<MonotonicObservation> result;
    for (const int64_t value : values)
        result.push_back({value, kClock});
    auto source = std::make_unique<FakeObservations>(std::move(result), events);
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

void constructor_rejects_and_fences_immediately() {
    {
        // A valid refusal is still not an accepted post-detach P5CO writer.
        auto refused = adopted();
        refused.kind = P50CacheSessionOutcomeKind::RefusedPreDetach;
        refused.f_sidecar_launch = {};
        refused.f_store_guid = {};
        refused.operation = {};
        refused.refusal_reason = P50CacheSessionRefusalReason::Deadline;
        CHECK(refused.valid());
        FakeLease* raw = nullptr;
        FakeObservations* source = nullptr;
        auto lease = std::make_unique<FakeLease>(refused, kDeadline);
        raw = lease.get();
        const auto fence_snapshot = raw->fence_snapshot;
        {
            auto value = AdoptedOutcomeWriter(std::move(lease), std::move(refused),
                                              observations({900}, source), kDeadline);
            CHECK(value.state() == P5coWriterState::FailedAfterDetach);
            CHECK(value.failure() == P5coFailure::InvalidInput);
            CHECK(value.canonical_frame().empty() && raw->fences == 1);
            auto moved = std::move(value);
            CHECK(raw->fences == 1 &&
                  moved.state() == P5coWriterState::FailedAfterDetach);
        }
        CHECK(*fence_snapshot == 1);
    }

    // A malformed typed outcome, absent observation source, invalid absolute
    // deadline, and zero write bound all reject while the lease is still live.
    {
        auto malformed = adopted();
        malformed.canonical_claim.clear();
        FakeLease* raw = nullptr;
        auto lease = std::make_unique<FakeLease>(malformed, kDeadline);
        raw = lease.get();
        FakeObservations* source = nullptr;
        auto value = AdoptedOutcomeWriter(std::move(lease), std::move(malformed),
                                          observations({}, source),
                                          kDeadline);
        CHECK(value.state() == P5coWriterState::FailedAfterDetach && raw->fences == 1);
    }
    {
        const auto outcome = adopted();
        FakeLease* raw = nullptr;
        auto lease = lease_for(outcome, raw);
        auto value = AdoptedOutcomeWriter(std::move(lease), outcome, nullptr,
                                          kDeadline);
        CHECK(value.state() == P5coWriterState::FailedAfterDetach && raw->fences == 1);
    }
    {
        const auto outcome = adopted();
        FakeLease* raw = nullptr;
        FakeObservations* source = nullptr;
        auto value = make_writer(lease_for(outcome, raw), observations({900}, source),
                                 AbsoluteMonotonicDeadline{0, 3, 9});
        CHECK(value.state() == P5coWriterState::FailedAfterDetach && raw->fences == 1);
    }
    {
        const auto outcome = adopted();
        FakeLease* raw = nullptr;
        FakeObservations* source = nullptr;
        auto value = make_writer(lease_for(outcome, raw), observations({900}, source),
                                 kDeadline, 0);
        CHECK(value.state() == P5coWriterState::FailedAfterDetach && raw->fences == 1);
    }
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
    CHECK(value.take_for_endpoint() == std::nullopt);
    CHECK(value.state() == P5coWriterState::Writing);
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
    CHECK(!value.take_for_endpoint().has_value());
    CHECK(value.failure() == P5coFailure::EndpointStartExpired);
    CHECK(raw->fences == 1 && source->calls == 3);
}

void full_flush_then_typed_endpoint_transfer_and_order() {
    const auto outcome = adopted();
    std::vector<char> events;
    FakeLease* raw = nullptr;
    FakeObservations* source = nullptr;
    auto lease = lease_for(outcome, raw, &events);
    raw->replies.push_back({P5coWriteKind::Sent,
                            encode_cache_session_outcome(outcome).size()});
    auto value = make_writer(std::move(lease), observations({999, 999, 999}, source,
                                                              &events));
    CHECK(value.advance(POLLOUT) == P5coWriterState::FullyFlushed);
    events.clear();

    auto handoff = value.take_for_endpoint();
    CHECK(handoff.has_value() && handoff->valid());
    CHECK(handoff->outcome() == outcome && handoff->deadline() == kDeadline);
    CHECK(events == std::vector<char>({'O', 'R'}));
    CHECK(raw->revalidations == 3 && source->calls == 3 && raw->fences == 0);

    auto transferred = handoff->take_lease();
    CHECK(transferred != nullptr && !handoff->valid());
    CHECK(handoff->take_lease() == nullptr);
    CHECK(value.state() == P5coWriterState::FullyFlushed);
    CHECK(!value.take_for_endpoint().has_value());
    transferred.reset();
}

void endpoint_order_and_ownership_loss_after_observation() {
    const auto outcome = adopted();
    std::vector<char> events;
    FakeLease* raw = nullptr;
    FakeObservations* source = nullptr;
    auto lease = lease_for(outcome, raw, &events);
    raw->replies.push_back({P5coWriteKind::Sent,
                            encode_cache_session_outcome(outcome).size()});
    auto value = make_writer(std::move(lease), observations({999, 999, 999}, source,
                                                              &events));
    CHECK(value.advance(POLLOUT) == P5coWriterState::FullyFlushed);
    events.clear();
    source->after_observe = [&] { raw->owned = false; };
    CHECK(!value.take_for_endpoint().has_value());
    CHECK(value.failure() == P5coFailure::OwnershipLost);
    CHECK(events == std::vector<char>({'O', 'R', 'F'}));
    CHECK(raw->fences == 1 && source->calls == 3);
}

void deadline_is_never_renewed() {
    const auto outcome = adopted();
    FakeLease* raw = nullptr;
    FakeObservations* source = nullptr;
    auto lease = lease_for(outcome, raw);
    raw->replies.push_back({P5coWriteKind::Sent, 2});
    raw->replies.push_back({P5coWriteKind::Sent, 2});
    auto value = make_writer(std::move(lease), observations({900, 900, 999, 999, 1000}, source),
                             kDeadline, 2);
    CHECK(value.advance(POLLOUT) == P5coWriterState::Writing);
    CHECK(value.advance(POLLOUT) == P5coWriterState::Writing);
    CHECK(value.advance(POLLOUT) == P5coWriterState::FailedAfterDetach);
    CHECK(value.offset() == 4 && raw->sends == 2);
}

void exact_identity_and_prewrite_revalidation() {
    {
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
    {
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
}

void would_block_and_interrupt_preserve_state_and_prefix() {
    for (const auto kind : {P5coWriteKind::WouldBlock, P5coWriteKind::Interrupted}) {
        const auto outcome = adopted();
        FakeLease* raw = nullptr;
        FakeObservations* source = nullptr;
        auto lease = lease_for(outcome, raw);
        raw->replies.push_back({kind, 0});
        auto value = make_writer(std::move(lease), observations({999, 999}, source));
        CHECK(value.state() == P5coWriterState::Ready);
        CHECK(value.advance(POLLOUT) == P5coWriterState::Ready);
        CHECK(value.offset() == 0 && value.last_bytes() == 0 &&
              raw->sends == 1 && raw->fences == 0);
    }

    for (const auto kind : {P5coWriteKind::WouldBlock, P5coWriteKind::Interrupted}) {
        const auto outcome = adopted();
        FakeLease* raw = nullptr;
        FakeObservations* source = nullptr;
        auto lease = lease_for(outcome, raw);
        raw->replies.push_back({P5coWriteKind::Sent, 1});
        raw->replies.push_back({kind, 0});
        auto value = make_writer(std::move(lease), observations({999, 999, 999, 999}, source),
                                 kDeadline, 1);
        CHECK(value.advance(POLLOUT) == P5coWriterState::Writing);
        CHECK(value.offset() == 1);
        CHECK(value.advance(POLLOUT) == P5coWriterState::Writing);
        CHECK(value.offset() == 1 && value.last_bytes() == 0 && raw->sends == 2);
    }
}

void malformed_send_returns_are_terminal() {
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

void terminal_readiness_precedes_observation_and_expiry() {
    const std::array<short, 4> flags = {
        static_cast<short>(POLLOUT | POLLERR),
        static_cast<short>(POLLOUT | POLLHUP),
        static_cast<short>(POLLOUT | POLLNVAL),
#ifdef POLLRDHUP
        static_cast<short>(POLLOUT | POLLRDHUP),
#else
        static_cast<short>(POLLOUT | POLLERR),
#endif
    };
    for (const short revents : flags) {
        const auto outcome = adopted();
        FakeLease* raw = nullptr;
        FakeObservations* source = nullptr;
        auto value = make_writer(lease_for(outcome, raw), observations({}, source));
        CHECK(value.advance(revents) == P5coWriterState::FailedAfterDetach);
        CHECK(value.failure() == P5coFailure::TerminalEvent && source->calls == 0 &&
              raw->sends == 0 && raw->fences == 1);
    }
    {
        const auto outcome = adopted();
        FakeLease* raw = nullptr;
        FakeObservations* source = nullptr;
        auto value = make_writer(lease_for(outcome, raw), observations({1000}, source));
        CHECK(value.advance(static_cast<short>(POLLOUT | POLLHUP)) ==
              P5coWriterState::FailedAfterDetach);
        CHECK(value.failure() == P5coFailure::TerminalEvent && source->calls == 0);
    }
}

void ownership_loss_is_terminal_without_send() {
    const auto outcome = adopted();
    FakeLease* raw = nullptr;
    FakeObservations* source = nullptr;
    auto lease = lease_for(outcome, raw);
    raw->owned = false;
    auto value = make_writer(std::move(lease), observations({999}, source));
    CHECK(value.advance(POLLOUT) == P5coWriterState::FailedAfterDetach);
    CHECK(value.failure() == P5coFailure::OwnershipLost && raw->sends == 0 &&
          raw->fences == 1);
}

void move_and_destruction_fence_exactly_once() {
    const auto outcome = adopted();
    {
        FakeLease* raw = nullptr;
        FakeObservations* source = nullptr;
        auto lease = lease_for(outcome, raw);
        const auto snapshot = raw->fence_snapshot;
        { auto value = make_writer(std::move(lease), observations({999}, source)); }
        CHECK(*snapshot == 1);
    }
    {
        FakeLease* raw = nullptr;
        FakeObservations* source = nullptr;
        auto lease = lease_for(outcome, raw);
        const auto snapshot = raw->fence_snapshot;
        raw->replies.push_back({P5coWriteKind::Sent, 1});
        { auto value = make_writer(std::move(lease), observations({999, 999}, source),
                                   kDeadline, 1);
          CHECK(value.advance(POLLOUT) == P5coWriterState::Writing); }
        CHECK(*snapshot == 1);
    }
    {
        FakeLease* raw = nullptr;
        FakeObservations* source = nullptr;
        auto lease = lease_for(outcome, raw);
        const auto snapshot = raw->fence_snapshot;
        raw->replies.push_back({P5coWriteKind::Sent,
                                encode_cache_session_outcome(outcome).size()});
        { auto value = make_writer(std::move(lease), observations({999, 999}, source));
          CHECK(value.advance(POLLOUT) == P5coWriterState::FullyFlushed); }
        CHECK(*snapshot == 1);
    }
    {
        FakeLease* first = nullptr;
        FakeLease* second = nullptr;
        FakeObservations* first_source = nullptr;
        FakeObservations* second_source = nullptr;
        auto left = make_writer(lease_for(outcome, first),
                                observations({999}, first_source));
        auto right = make_writer(lease_for(outcome, second),
                                 observations({999}, second_source));
        const auto second_snapshot = second->fence_snapshot;
        right = std::move(left);
        CHECK(first->fences == 0 && *second_snapshot == 1 &&
              right.state() == P5coWriterState::Ready);
    }
    {
        FakeLease* raw = nullptr;
        FakeObservations* source = nullptr;
        auto value = make_writer(lease_for(outcome, raw), observations({999}, source));
        auto moved = std::move(value);
        CHECK(raw->fences == 0 && value.state() == P5coWriterState::FailedAfterDetach &&
              moved.state() == P5coWriterState::Ready);
    }
}

void endpoint_handoff_move_and_fence() {
    const auto outcome = adopted();
    FakeLease* raw = nullptr;
    FakeObservations* source = nullptr;
    auto lease = lease_for(outcome, raw);
    raw->replies.push_back({P5coWriteKind::Sent,
                            encode_cache_session_outcome(outcome).size()});
    auto value = make_writer(std::move(lease), observations({999, 999, 999}, source));
    CHECK(value.advance(POLLOUT) == P5coWriterState::FullyFlushed);
    const auto snapshot = raw->fence_snapshot;
    auto handoff = value.take_for_endpoint();
    CHECK(handoff.has_value());
    FakeLease* second_raw = nullptr;
    FakeObservations* second_source = nullptr;
    auto second_lease = lease_for(outcome, second_raw);
    second_raw->replies.push_back({P5coWriteKind::Sent,
                                   encode_cache_session_outcome(outcome).size()});
    auto second_writer = make_writer(std::move(second_lease),
                                     observations({999, 999, 999}, second_source));
    CHECK(second_writer.advance(POLLOUT) == P5coWriterState::FullyFlushed);
    auto second_handoff = second_writer.take_for_endpoint();
    CHECK(second_handoff.has_value());
    const auto second_snapshot = second_raw->fence_snapshot;
    {
        auto moved = std::move(*handoff);
        CHECK(!handoff->valid() && moved.valid());
        CHECK(*snapshot == 0);
        moved = std::move(*second_handoff);
        CHECK(*snapshot == 1 && *second_snapshot == 0 && moved.valid());
    }
    CHECK(*second_snapshot == 1);
}

} // namespace

int main() {
    constructor_rejects_and_fences_immediately();
    immediate_expiry();
    partial_then_expiry_and_no_later_byte();
    stale_first_vs_second_observations();
    pollout_just_before_expiry_and_canonical_frame();
    full_flush_then_expiry_before_endpoint();
    full_flush_then_typed_endpoint_transfer_and_order();
    endpoint_order_and_ownership_loss_after_observation();
    deadline_is_never_renewed();
    exact_identity_and_prewrite_revalidation();
    would_block_and_interrupt_preserve_state_and_prefix();
    malformed_send_returns_are_terminal();
    terminal_readiness_precedes_observation_and_expiry();
    ownership_loss_is_terminal_without_send();
    move_and_destruction_fence_exactly_once();
    endpoint_handoff_move_and_fence();
    return 0;
}
