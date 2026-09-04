#include "../cache/p50_cache_session_join.h"
#include "../cache/p50_incarnation_identity.h"

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace icecc::p50;
using namespace icecc::p50::daemon;
using Clock = P50CacheSessionJoinTable::Clock;

int failures = 0;
P50CacheSessionAttemptAuthority attempts(256);

#define CHECK(expression, message)                                             \
  do {                                                                         \
    if (!(expression)) {                                                       \
      std::fprintf(stderr, "FAILED - %s\n", message);                          \
      ++failures;                                                              \
    }                                                                          \
  } while (false)

std::vector<uint8_t> hex_fixture(const char *hex) {
  const std::string text(hex);
  if (text.size() % 2 != 0)
    throw std::runtime_error("odd fixed fixture");
  std::vector<uint8_t> bytes;
  bytes.reserve(text.size() / 2);
  const auto nibble = [](char value) -> uint8_t {
    if (value >= '0' && value <= '9')
      return static_cast<uint8_t>(value - '0');
    if (value >= 'a' && value <= 'f')
      return static_cast<uint8_t>(value - 'a' + 10);
    if (value >= 'A' && value <= 'F')
      return static_cast<uint8_t>(value - 'A' + 10);
    throw std::runtime_error("invalid fixed fixture");
  };
  for (size_t i = 0; i != text.size(); i += 2)
    bytes.push_back(static_cast<uint8_t>((nibble(text[i]) << 4) |
                                         nibble(text[i + 1])));
  return bytes;
}

CStoreGuid c_guid() {
  CStoreGuid guid{};
  for (size_t i = 0; i != guid.bytes.size(); ++i)
    guid.bytes[i] = static_cast<uint8_t>(i + 1);
  guid.bytes[kStoreIdentityRoleByte] &=
      static_cast<uint8_t>(~kStoreIdentityRoleMask);
  return guid;
}

FStoreGuid f_guid() {
  FStoreGuid guid{};
  for (size_t i = 0; i != guid.bytes.size(); ++i)
    guid.bytes[i] = static_cast<uint8_t>(i + 33);
  guid.bytes[kStoreIdentityRoleByte] |= kStoreIdentityRoleMask;
  return guid;
}

ClaimAttemptCapability128 capability(uint8_t seed) {
  ClaimAttemptCapability128 value;
  for (size_t i = 0; i != value.bytes.size(); ++i)
    value.bytes[i] = static_cast<uint8_t>(seed + i);
  return value;
}

P50CurrentFIncarnation incarnation() {
  return P50CurrentFIncarnation{
      71, {81, 2}, f_guid(), 91, kStoreIdentityDerivationVersion, 7001};
}

P50CacheSessionOwnerContext owner(uint64_t sequence, uint64_t client) {
  return P50CacheSessionOwnerContext{{71, sequence}, client};
}

P50CacheSessionArmBinding binding(uint64_t observation, uint32_t wire_job_id) {
  P50SourceArmFields arm;
  arm.wire_job_id = wire_job_id;
  arm.assignment_epoch = 1000 + wire_job_id;
  arm.assignment_nonce = 2000 + wire_job_id;
  arm.selected_f_host = "f.example.test";
  arm.selected_f_ordinary_port = 10250;
  arm.selected_f_cache_port = 10251;
  arm.cache_protocol = CACHE_WIRE_REVISION;
  arm.cache_profile = CACHE_PROFILE_ZSTD_TU;
  arm.logical_job = 3000 + wire_job_id;
  arm.compiler_attempt = 4000 + wire_job_id;
  arm.c_store_generation = 61;
  arm.c_store_derivation_version = kStoreIdentityDerivationVersion;
  arm.c_store_guid = c_guid().bytes;
  arm.source_request_id = 5000 + wire_job_id;
  arm.source_mode = P50_SOURCE_MODE_ZSTD_TU;
  arm.c_control_generation = 80;
  arm.c_control_attempt = 1;
  const auto current = incarnation();
  P50SourceArmedMsg message(
      std::move(arm), current.control_launch.generation,
      current.control_launch.attempt, current.store_generation,
      current.store_guid.bytes, current.store_derivation_version,
      observation, 5000, capability(0x51), capability(0x91));
  const auto value = cache_session_binding_from_armed(message);
  if (!value)
    throw std::runtime_error("canonical source-armed binding conversion failed");
  return *value;
}

P50CacheSessionWireClaim claim(P50CacheSessionArmBinding value,
                               uint64_t attempt) {
  (void)attempt;
  const auto proof = attempts.burn(value);
  return P50CacheSessionWireClaim{std::move(value),
                                   proof.value_or(P50CacheSessionAttemptProof{})};
}

class OwnerAuthority final : public P50CacheSessionOwnerAuthority {
public:
  bool
  current(const P50CurrentFIncarnation &current_f,
          const P50CacheSessionArmBinding &value,
          P50CacheSessionOwnerContext value_owner) const noexcept override {
    return allow && value.valid() && value_owner.valid() &&
           (!expected_incarnation.has_value() ||
            *expected_incarnation == current_f) &&
           (!retired.has_value() || *retired != value_owner);
  }

  P50CacheSessionOwnerAuthorityState
  state(const P50CurrentFIncarnation &,
        P50CacheSessionOwnerContext value_owner) const noexcept override {
    if (retired.has_value() && *retired == value_owner)
      return P50CacheSessionOwnerAuthorityState::Retired;
    return allow ? P50CacheSessionOwnerAuthorityState::Live
                 : P50CacheSessionOwnerAuthorityState::Unknown;
  }

  bool allow = true;
  std::optional<P50CurrentFIncarnation> expected_incarnation;
  std::optional<P50CacheSessionOwnerContext> retired;
};

void wire_roundtrip_and_bounds() {
  const auto exact = claim(binding(100, 1), 7);
  CHECK(exact.valid(), "exact public-session claim validates");
  const P50SourceArmedMsg exact_ack(
      exact.binding.arm, exact.binding.f_control_generation,
      exact.binding.f_control_attempt, exact.binding.f_store_generation,
      exact.binding.f_store_guid, exact.binding.f_store_derivation_version,
      exact.binding.arm_observation_id, exact.binding.source_budget_msec,
      capability(0x51), capability(0x91));
  const auto extracted = cache_session_binding_from_armed(exact_ack);
  CHECK(extracted && *extracted == exact.binding,
        "claim binding is extracted losslessly from the actual full ACK");
  const auto wire = encode_cache_session_wire_claim(exact);
  CHECK(wire == hex_fixture(
      "5035434c00010001000000db0000000100000000000003e900000000000007d1"
      "0000000f662e6578616d706c652e74657374000000280a0000280b0000000100"
      "0000020000000000000bb90000000000000fa1000000000000003d0000000000"
      "0000010102030405060708090a0b0c0d0e0f1000000000000013890000000200"
      "0000000000005000000000000000010000000000000051000000000000000200"
      "0000000000005ba122232425262728292a2b2c2d2e2f30000000000000000100"
      "00000000000064000013880000000001000000000000000000000000000064"
      "0000000000000001"),
        "canonical full-ACK claim has one stable byte representation");
  const auto decoded = decode_cache_session_wire_claim(wire);
  CHECK(!wire.empty() && wire.size() <= kP50CacheSessionClaimMaxWireBytes &&
            decoded && *decoded == exact,
        "bounded exact claim round-trips");

  bool every_truncation_rejected = true;
  for (size_t size = 0; size != wire.size(); ++size) {
    if (decode_cache_session_wire_claim(
            std::span<const uint8_t>(wire.data(), size))) {
      every_truncation_rejected = false;
      break;
    }
  }
  CHECK(every_truncation_rejected,
        "every truncated public-session claim is rejected");
  auto trailing = wire;
  trailing.push_back(0);
  CHECK(!decode_cache_session_wire_claim(trailing),
        "trailing bytes cannot create an alternate claim encoding");

  auto malformed = wire;
  malformed[malformed.size() - 8] ^= 1;
  const auto changed = decode_cache_session_wire_claim(malformed);
  CHECK(changed && *changed != exact,
        "field mutation changes identity rather than preserving equality");
  malformed = wire;
  malformed[6] = 1;
  CHECK(!decode_cache_session_wire_claim(malformed),
        "nonzero reserved header is rejected");
  malformed = wire;
  malformed[malformed.size() - 28] = 1;
  CHECK(!decode_cache_session_wire_claim(malformed),
        "nonzero reserved full-ACK word is rejected");

  auto bad = exact;
  bad.binding.arm.cache_profile = CACHE_PROFILE_P29V1;
  CHECK(!bad.valid() && encode_cache_session_wire_claim(bad).empty(),
        "unsupported profile is rejected before composing wire");
  bad = exact;
  bad.binding.f_store_guid = bad.binding.arm.c_store_guid;
  CHECK(!bad.valid(), "C/F StoreIdentity alias is rejected");
  bad = exact;
  bad.binding.f_store_guid = bad.binding.arm.c_store_guid;
  bad.binding.f_store_guid[kStoreIdentityRoleByte] |=
      kStoreIdentityRoleMask;
  CHECK(!bad.valid(),
        "C/F role-tagged GUIDs must still have independent 127-bit roots");
  bad = exact;
  bad.binding.f_store_guid = {};
  bad.binding.f_store_guid[kStoreIdentityRoleByte] =
      kStoreIdentityRoleMask;
  CHECK(!bad.valid(), "role-tag-only F StoreIdentity root is rejected");
  bad = exact;
  bad.binding.arm.c_store_guid = {};
  CHECK(!bad.valid(), "role-tag-only C StoreIdentity root is rejected");
  bad = exact;
  bad.attempt = P50CacheSessionAttemptProof{};
  CHECK(!bad.valid(), "zero attempt is rejected");

  const StoreIdentityRoot zero_root{};
  CHECK(!zero_root.valid() && f_store_guid_for_root(zero_root) == FStoreGuid{} &&
            c_store_guid_for_root(zero_root) == CStoreGuid{},
        "StoreIdentity factories reject a zero 127-bit root");
}

void reverse_arrival_and_stale_fence() {
  const auto now = Clock::now();
  OwnerAuthority authority;
  P50CacheSessionJoinTable table(2, incarnation(), authority, attempts);
  const auto first_binding = binding(100, 10);
  const auto second_binding = binding(101, 11);
  const auto first_owner = owner(1, 101);
  const auto second_owner = owner(2, 102);
  CHECK(table.register_wait(first_binding, first_owner, now) ==
            P50CacheSessionJoinDecision::Registered,
        "first F WAIT registration is installed in arm order");
  CHECK(table.register_wait(second_binding, second_owner, now) ==
            P50CacheSessionJoinDecision::Registered,
        "second F WAIT registration is installed in arm order");

  // C burns attempt 1 for the older arm and attempt 2 for the newer arm.
  // The network deliberately delivers the newer public connection first.
  const auto first_claim = claim(first_binding, 1);
  const auto second_claim = claim(second_binding, 2);
  CHECK(table.reserve_claim(second_claim, second_owner, now) ==
            P50CacheSessionJoinDecision::Reserved,
        "newer live claim reserves first");
  CHECK(table.reserve_claim(first_claim, first_owner, now) ==
            P50CacheSessionJoinDecision::Reserved,
        "older live claim still reserves after reverse-order arrival");

  CHECK(table.cancel_wait(first_binding, first_owner, now) ==
                P50CacheSessionJoinDecision::CancelledPrecommit &&
            table.retire_wait(first_binding, first_owner) ==
                P50CacheSessionJoinDecision::RetiredObservation,
        "older exact WAIT cancels and retires");
  CHECK(table.cancel_wait(second_binding, second_owner, now) ==
                P50CacheSessionJoinDecision::CancelledPrecommit &&
            table.retire_wait(second_binding, second_owner) ==
                P50CacheSessionJoinDecision::RetiredObservation,
        "newer exact WAIT cancels and retires");
  CHECK(table.reserve_claim(first_claim, first_owner, now) ==
            P50CacheSessionJoinDecision::RetiredObservation,
        "delayed retired observation is a lookup-only refusal");
  CHECK(table.register_wait(first_binding, first_owner, now) ==
            P50CacheSessionJoinDecision::RetiredObservation,
        "observation cannot be re-registered after retirement");
}

void exact_owner_and_incarnation() {
  const auto now = Clock::now();
  {
    OwnerAuthority current_authority;
    P50CacheSessionJoinTable current_table(1, incarnation(), current_authority,
                                           attempts);
    auto stale_f = binding(199, 19);
    ++stale_f.f_store_generation;
    CHECK(current_table.register_wait(stale_f, owner(9, 109), now) ==
              P50CacheSessionJoinDecision::Invalid,
          "WAIT registration refuses a non-current F StoreIdentity");
    auto role_only = binding(198, 18);
    role_only.f_store_guid = {};
    role_only.f_store_guid[kStoreIdentityRoleByte] =
        kStoreIdentityRoleMask;
    auto role_only_current = incarnation();
    role_only_current.store_guid.bytes = role_only.f_store_guid;
    CHECK(!role_only.valid() && !role_only_current.valid(),
          "role-tag-only F root is invalid in binding and current incarnation");
  }
  OwnerAuthority authority;
  P50CacheSessionJoinTable table(1, incarnation(), authority, attempts);
  const auto exact_binding = binding(200, 20);
  const auto exact_owner = owner(3, 103);
  CHECK(table.register_wait(exact_binding, exact_owner, now) ==
            P50CacheSessionJoinDecision::Registered,
        "exact current F incarnation registers");

  const auto admitted = claim(exact_binding, 4);

  auto wrong = exact_binding;
  ++wrong.f_store_generation;
  CHECK(table.reserve_claim(P50CacheSessionWireClaim{wrong, admitted.attempt},
                            exact_owner, now) ==
            P50CacheSessionJoinDecision::WrongClaim,
        "mutated F StoreIdentity cannot join live WAIT");
  wrong = exact_binding;
  ++wrong.arm_observation_id;
  CHECK(table.reserve_claim(claim(wrong, 3), exact_owner, now) ==
            P50CacheSessionJoinDecision::NoOwner,
        "invented F observation cannot manufacture a WAIT owner");
  CHECK(table.reserve_claim(admitted, owner(3, 104), now) ==
            P50CacheSessionJoinDecision::WrongClaim,
        "same connection sequence with a different client cannot alias");
  CHECK(table.reserve_claim(admitted, owner(4, 103), now) ==
            P50CacheSessionJoinDecision::WrongClaim,
        "different connection lease cannot alias the live owner");

  CHECK(table.reserve_claim(admitted, exact_owner, now) ==
            P50CacheSessionJoinDecision::Reserved,
        "current authoritative owner reserves exact claim");
  authority.retired = exact_owner;
  CHECK(table.revalidate_before_private_connect(admitted, exact_owner, now) ==
            P50CacheSessionJoinDecision::Invalid,
        "owner retirement invalidates private-connect revalidation");
  CHECK(table.mark_public_fd_detached(admitted, exact_owner, now) ==
            P50CacheSessionJoinDecision::CancelledPrecommit,
        "owner retirement before release prevents descriptor detach");

  OwnerAuthority replaced_ready_authority;
  replaced_ready_authority.expected_incarnation = incarnation();
  P50CacheSessionJoinTable replaced_ready_table(1, incarnation(),
                                                replaced_ready_authority,
                                                attempts);
  const auto replaced_binding = binding(202, 22);
  const auto replaced_owner = owner(11, 111);
  const auto replaced_claim = claim(replaced_binding, 5);
  CHECK(replaced_ready_table.register_wait(replaced_binding, replaced_owner,
                                           now) ==
            P50CacheSessionJoinDecision::Registered,
        "WAIT registers against the current structured-READY incarnation");
  ++replaced_ready_authority.expected_incarnation->ready_lease_observation_id;
  CHECK(
      replaced_ready_table.reserve_claim(replaced_claim, replaced_owner, now) ==
          P50CacheSessionJoinDecision::Invalid,
      "F listener replacement invalidates the old READY-bound owner");

  OwnerAuthority retired_authority;
  retired_authority.retired = owner(10, 110);
  P50CacheSessionJoinTable retired_table(1, incarnation(), retired_authority,
                                         attempts);
  CHECK(retired_table.register_wait(binding(201, 21),
                                    *retired_authority.retired, now) ==
            P50CacheSessionJoinDecision::Invalid,
        "unseen retired owner cannot register a future WAIT");
}

void bounded_retry_and_duplicate_race() {
  const auto now = Clock::now();
  OwnerAuthority authority;
  P50CacheSessionJoinTable table(1, incarnation(), authority, attempts);
  const auto value = binding(300, 30);
  const auto exact_owner = owner(5, 105);
  CHECK(table.register_wait(value, exact_owner, now) ==
            P50CacheSessionJoinDecision::Registered,
        "retry row registers");
  const auto first = claim(value, 10);
  const auto concurrent = claim(value, 11);
  CHECK(table.reserve_claim(first, exact_owner, now) ==
            P50CacheSessionJoinDecision::Reserved,
        "first concurrent claimant wins");
  CHECK(table.reserve_claim(first, exact_owner, now) ==
            P50CacheSessionJoinDecision::DuplicateAttempt,
        "exact duplicate cannot execute twice");
  CHECK(table.reserve_claim(concurrent, exact_owner, now) ==
            P50CacheSessionJoinDecision::Busy,
        "different concurrent attempt cannot steal reservation");
  CHECK(table.release_pre_detach_for_retry(first, exact_owner, now) ==
            P50CacheSessionJoinDecision::RetryAllowed,
        "one pre-detach failure reopens original WAIT");
  CHECK(table.reserve_claim(first, exact_owner, now) ==
            P50CacheSessionJoinDecision::DuplicateAttempt,
        "failed attempt remains burned");
  CHECK(table.reserve_claim(concurrent, exact_owner, now) ==
            P50CacheSessionJoinDecision::Reserved,
        "fresh second attempt reserves");
  CHECK(table.release_pre_detach_for_retry(concurrent, exact_owner, now) ==
                P50CacheSessionJoinDecision::RetryExhausted &&
            !table.state(value, exact_owner).has_value() &&
            table.active_size() == 0,
        "second pre-detach failure terminalizes and immediately reclaims row");
}

void reconciliation_is_nonterminal() {
  const auto now = Clock::now();
  OwnerAuthority authority;
  P50CacheSessionJoinTable table(1, incarnation(), authority, attempts);
  const auto value = binding(400, 40);
  const auto exact_owner = owner(6, 106);
  auto expiring_value = value;
  expiring_value.source_budget_msec = 1;
  const auto exact_claim = claim(expiring_value, 20);
  CHECK(table.register_wait(expiring_value, exact_owner, now) ==
                P50CacheSessionJoinDecision::Registered &&
            table.reserve_claim(exact_claim, exact_owner, now) ==
                P50CacheSessionJoinDecision::Reserved &&
            table.mark_public_fd_detached(exact_claim, exact_owner, now) ==
                P50CacheSessionJoinDecision::PublicFdDetached &&
            table.mark_endpoint_inflight(exact_claim, exact_owner, now) ==
                P50CacheSessionJoinDecision::EndpointInFlight,
        "post-detach endpoint becomes live");

  const auto late = now + std::chrono::seconds(2);
  CHECK(table.expire_wait(expiring_value, exact_owner, late) ==
            P50CacheSessionJoinDecision::ReconcileRequired,
        "post-detach expiry enters reconciliation");
  CHECK(table.retire_wait(expiring_value, exact_owner) ==
            P50CacheSessionJoinDecision::WrongState,
        "unresolved reconciliation cannot retire");
  CHECK(table.settle_endpoint(exact_claim, exact_owner,
                              P50EndpointSettlement::Unresolved, late) ==
            P50CacheSessionJoinDecision::ReconcileRequired,
        "uncertain endpoint result remains nonterminal");
  CHECK(table.retire_wait(expiring_value, exact_owner) ==
            P50CacheSessionJoinDecision::WrongState,
        "uncertain result still cannot retire");
  CHECK(table.settle_endpoint(exact_claim, exact_owner,
                              P50EndpointSettlement::CommittedInput, late) ==
            P50CacheSessionJoinDecision::CommittedSuppressed,
        "late canonical committed_input remains authoritative but delivery is "
        "suppressed");
  CHECK(table.retire_wait(expiring_value, exact_owner) ==
                P50CacheSessionJoinDecision::RetiredObservation &&
            table.active_size() == 0,
        "resolved committed-after-close row reclaims capacity");
}

void pre_detach_expiry_and_capacity() {
  const auto now = Clock::now();
  OwnerAuthority authority;
  P50CacheSessionJoinTable table(1, incarnation(), authority, attempts);
  const auto first = binding(500, 50);
  const auto second = binding(501, 51);
  const auto first_owner = owner(7, 107);
  const auto second_owner = owner(8, 108);
  auto first_with_budget = first;
  first_with_budget.source_budget_msec = 1;
  CHECK(table.register_wait(first_with_budget, first_owner, now) ==
            P50CacheSessionJoinDecision::Registered,
        "capacity row registers");
  CHECK(table.register_wait(second, second_owner, now) ==
            P50CacheSessionJoinDecision::CapacityExceeded,
        "bounded capacity refuses a second live WAIT");
  CHECK(table.register_wait(second, second_owner,
                            now + std::chrono::milliseconds(2)) ==
            P50CacheSessionJoinDecision::Registered,
        "expired pre-detach WAIT is swept before capacity admission");

  OwnerAuthority reserved_authority;
  P50CacheSessionJoinTable reserved_table(1, incarnation(),
                                           reserved_authority, attempts);
  auto reserved_binding = binding(504, 54);
  reserved_binding.source_budget_msec = 1;
  const auto reserved_owner = owner(11, 111);
  const auto reserved_claim = claim(reserved_binding, 54);
  CHECK(reserved_table.register_wait(reserved_binding, reserved_owner, now) ==
                P50CacheSessionJoinDecision::Registered &&
            reserved_table.reserve_claim(reserved_claim, reserved_owner, now) ==
                P50CacheSessionJoinDecision::Reserved &&
            reserved_table.register_wait(binding(505, 55), owner(12, 112),
                                         now + std::chrono::milliseconds(2)) ==
                P50CacheSessionJoinDecision::Registered,
        "expired pre-detach AttemptReserved WAIT is swept before admission");

  OwnerAuthority detached_authority;
  P50CacheSessionJoinTable detached_table(1, incarnation(), detached_authority,
                                          attempts);
  const auto detached_binding = binding(502, 52);
  const auto detached_owner = owner(9, 109);
  const auto detached_claim = claim(detached_binding, 52);
  CHECK(detached_table.register_wait(detached_binding, detached_owner, now) ==
                P50CacheSessionJoinDecision::Registered &&
            detached_table.reserve_claim(detached_claim, detached_owner, now) ==
                P50CacheSessionJoinDecision::Reserved &&
            detached_table.mark_public_fd_detached(detached_claim,
                                                   detached_owner, now) ==
                P50CacheSessionJoinDecision::PublicFdDetached &&
            detached_table.mark_endpoint_inflight(detached_claim,
                                                  detached_owner, now) ==
                P50CacheSessionJoinDecision::EndpointInFlight &&
            detached_table.register_wait(binding(503, 53), owner(10, 110),
                                         now + std::chrono::minutes(2)) ==
                P50CacheSessionJoinDecision::CapacityExceeded &&
            detached_table.state(detached_binding, detached_owner) ==
                P50CacheSessionJoinState::EndpointInFlight,
        "detached/in-flight ownership is never swept by pre-detach expiry");
}

void attempt_authority_requires_burn_and_scope() {
  const auto now = Clock::now();
  OwnerAuthority owner_authority;
  P50CacheSessionAttemptAuthority c_authority(4);
  P50CacheSessionJoinTable table(1, incarnation(), owner_authority,
                                 c_authority);
  const auto value = binding(600, 60);
  const auto exact_owner = owner(20, 120);
  CHECK(table.register_wait(value, exact_owner, now) ==
            P50CacheSessionJoinDecision::Registered,
        "attempt authority test WAIT registers");

  const P50CacheSessionWireClaim forged{
      value,
      P50CacheSessionAttemptProof{1, capability(0xd1)}};
  CHECK(table.reserve_claim(forged, exact_owner, now) ==
            P50CacheSessionJoinDecision::Invalid,
        "forged non-burned capability cannot reserve a live WAIT");

  const auto first = c_authority.burn(value);
  const auto second = c_authority.burn(value);
  CHECK(first.has_value() && second.has_value(),
        "exact C arm burns both bounded attempt capabilities");
  if (first && second) {
    const P50CacheSessionWireClaim first_claim{value, *first};
    CHECK(table.reserve_claim(first_claim, exact_owner, now) ==
              P50CacheSessionJoinDecision::Reserved,
          "first burned capability reserves the exact live WAIT");
    auto invalid_ordinal = first_claim;
    invalid_ordinal.attempt.ordinal = 0;
    CHECK(!invalid_ordinal.valid() &&
              table.reserve_claim(invalid_ordinal, exact_owner, now) ==
                  P50CacheSessionJoinDecision::Invalid,
          "invalid claim is rejected before duplicate-capability lookup");
    CHECK(table.release_pre_detach_for_retry(first_claim, exact_owner, now) ==
                  P50CacheSessionJoinDecision::RetryAllowed &&
              table.reserve_claim(P50CacheSessionWireClaim{value, *second},
                                  exact_owner, now) ==
                  P50CacheSessionJoinDecision::Reserved &&
              !c_authority.burn(value),
          "only two burned ordinals are consumable per exact C arm");
  }
}

void exact_retirement_fence_is_bounded() {
  const auto now = Clock::now();
  OwnerAuthority owner_authority;
  P50CacheSessionAttemptAuthority c_authority(8);
  P50CacheSessionJoinTable table(2, incarnation(), owner_authority, c_authority,
                                 1);
  const auto high_owner = owner(31, 131);
  const auto low_owner = owner(30, 130);
  owner_authority.retired = high_owner;
  CHECK(table.retire_unseen_owner(high_owner) ==
            P50CacheSessionJoinDecision::RetiredOwner &&
            table.retirement_fence_size() == 1,
        "authoritative unseen retirement installs one exact-owner fence");

  const auto low_binding = binding(700, 70);
  CHECK(table.register_wait(low_binding, low_owner, now) ==
            P50CacheSessionJoinDecision::Registered,
        "exact unseen fence does not reject a lower still-live owner");
  const auto fenced_binding = binding(701, 71);
  CHECK(table.register_wait(fenced_binding, high_owner, now) ==
            P50CacheSessionJoinDecision::RetiredOwner,
        "fenced owner cannot create a future WAIT");

  owner_authority.retired = owner(32, 132);
  CHECK(table.retire_unseen_owner(owner(32, 132)) ==
            P50CacheSessionJoinDecision::RetirementFenceCapacity &&
            table.retirement_fence_size() == 1,
        "fence capacity exhaustion fails closed without forgetting a fence");
}

void deadline_and_invalid_settlement_fail_closed() {
  const auto now = Clock::now();
  OwnerAuthority owner_authority;
  P50CacheSessionAttemptAuthority c_authority(8);
  P50CacheSessionJoinTable table(1, incarnation(), owner_authority, c_authority);
  auto value = binding(800, 80);
  value.source_budget_msec = 1;
  const auto exact_owner = owner(40, 140);
  const auto exact_claim = claim(value, 1);
  CHECK(table.register_wait(value, exact_owner, now) ==
            P50CacheSessionJoinDecision::Registered &&
            table.reserve_claim(exact_claim, exact_owner,
                                now + std::chrono::milliseconds(2)) ==
                P50CacheSessionJoinDecision::Expired &&
            table.active_size() == 0,
        "source budget mechanically expires and reclaims pre-detach row");

  auto overflow = binding(801, 81);
  overflow.source_budget_msec = kP50CacheSessionMaxSourceBudgetMsec;
  CHECK(table.register_wait(overflow, owner(41, 141),
                            Clock::time_point::max() -
                                std::chrono::milliseconds(1)) ==
            P50CacheSessionJoinDecision::Invalid,
        "deadline addition overflow fails closed");

  auto live = binding(802, 82);
  live.source_budget_msec = 5000;
  const auto live_owner = owner(42, 142);
  const auto d1 = table.register_wait(live, live_owner, now);
  const auto live_proof = c_authority.burn(live);
  const P50CacheSessionWireClaim live_claim{
      live, live_proof.value_or(P50CacheSessionAttemptProof{})};
  const auto d2 = table.reserve_claim(live_claim, live_owner, now);
  auto invalid_live_claim = live_claim;
  invalid_live_claim.attempt.selected_capability.bytes[0] ^= 1;
  const auto invalid_d3 =
      table.mark_public_fd_detached(invalid_live_claim, live_owner, now);
  const auto d3 = table.mark_public_fd_detached(live_claim, live_owner, now);
  const auto d4 = table.mark_endpoint_inflight(live_claim, live_owner, now);
  const auto d5 = table.settle_endpoint(
      live_claim, live_owner, static_cast<P50EndpointSettlement>(99), now);
  CHECK(d1 == P50CacheSessionJoinDecision::Registered && live_proof &&
            d2 == P50CacheSessionJoinDecision::Reserved &&
            invalid_d3 == P50CacheSessionJoinDecision::WrongClaim &&
            d3 == P50CacheSessionJoinDecision::PublicFdDetached &&
            d4 == P50CacheSessionJoinDecision::EndpointInFlight &&
            d5 == P50CacheSessionJoinDecision::InvalidSettlement &&
            table.state(live, live_owner) ==
                P50CacheSessionJoinState::EndpointInFlight,
        "invalid future endpoint settlement cannot mutate reducer state");
}

P50CurrentFIncarnation adapter_ready{};
Client *adapter_client = reinterpret_cast<Client *>(static_cast<uintptr_t>(1));
MsgChannel *adapter_channel =
    reinterpret_cast<MsgChannel *>(static_cast<uintptr_t>(2));
PeerCredentials adapter_peer{1000, 1000, 4242, true};
bool adapter_retired = false;

P50CacheSessionOwnerAuthorityState adapter_resolver(
    uint64_t client_id, Client *&client, MsgChannel *&channel,
    PeerCredentials &peer) noexcept {
  if (client_id != 9001)
    return P50CacheSessionOwnerAuthorityState::Unknown;
  if (adapter_retired)
    return P50CacheSessionOwnerAuthorityState::Retired;
  client = adapter_client;
  channel = adapter_channel;
  peer = adapter_peer;
  return P50CacheSessionOwnerAuthorityState::Live;
}

bool adapter_ready_provider(P50CurrentFIncarnation &current) noexcept {
  current = adapter_ready;
  return true;
}

void production_shaped_owner_adapter() {
  ConnectionLeaseRegistry registry(71);
  const auto provenance = registry.allocate(ListenerKind::UnixLocal,
                                            adapter_peer);
  CHECK(provenance.has_value(), "adapter test allocates a local lease");
  CHECK(provenance &&
            registry.bind(provenance->lease, adapter_client, adapter_channel),
        "adapter test binds exact client and channel");
  adapter_ready = incarnation();
  adapter_retired = false;
  P50CacheSessionOwnerAuthorityAdapter authority(
      registry, adapter_resolver, adapter_ready_provider);
  P50CacheSessionAttemptAuthority c_authority(4);
  P50CacheSessionJoinTable table(1, adapter_ready, authority, c_authority);
  const auto value = binding(900, 90);
  const P50CacheSessionOwnerContext exact_owner{provenance->lease, 9001};
  const auto proof = c_authority.burn(value);
  CHECK(proof && table.register_wait(value, exact_owner, Clock::now()) ==
                    P50CacheSessionJoinDecision::Registered &&
            table.reserve_claim(P50CacheSessionWireClaim{value, *proof},
                                exact_owner, Clock::now()) ==
                P50CacheSessionJoinDecision::Reserved,
        "production-shaped adapter consumes registry and READY authority");
  adapter_ready.ready_lease_observation_id++;
  CHECK(table.revalidate_before_private_connect(
            P50CacheSessionWireClaim{value, *proof}, exact_owner,
            Clock::now()) == P50CacheSessionJoinDecision::Invalid,
        "adapter rejects replaced structured READY observation");
  adapter_ready = incarnation();
  adapter_peer.pid++;
  CHECK(table.revalidate_before_private_connect(
            P50CacheSessionWireClaim{value, *proof}, exact_owner,
            Clock::now()) == P50CacheSessionJoinDecision::Invalid,
        "adapter rejects changed peer credentials through lease registry");
  adapter_peer.pid--;
  adapter_retired = true;
  CHECK(registry.cancel(provenance->lease) &&
            table.cancel_wait(value, exact_owner, Clock::now()) ==
                P50CacheSessionJoinDecision::CancelledPrecommit &&
            table.retire_unseen_owner(exact_owner) ==
                P50CacheSessionJoinDecision::RetiredOwner,
        "adapter exposes authoritative owner retirement for exact fencing");
}

void constructor_refuses_implicit_generation() {
  bool threw = false;
  OwnerAuthority authority;
  try {
    auto invalid = incarnation();
    invalid.daemon_generation = 0;
    P50CacheSessionJoinTable table(1, invalid, authority, attempts);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  CHECK(threw, "zero F daemon generation is never trust-on-first-use");
}

} // namespace

int main() {
  wire_roundtrip_and_bounds();
  reverse_arrival_and_stale_fence();
  exact_owner_and_incarnation();
  bounded_retry_and_duplicate_race();
  reconciliation_is_nonterminal();
  pre_detach_expiry_and_capacity();
  attempt_authority_requires_burn_and_scope();
  exact_retirement_fence_is_bounded();
  deadline_and_invalid_settlement_fail_closed();
  production_shaped_owner_adapter();
  constructor_refuses_implicit_generation();
  return failures == 0 ? 0 : 1;
}
