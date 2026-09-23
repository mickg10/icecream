#include "unittests/support/p50_cache_session_join.h"

#include <algorithm>
#include <stdexcept>
#include <type_traits>

namespace icecc::p50::daemon {
namespace {

bool role_guid(Id128 guid, uint8_t expected_role) noexcept {
  return store_identity_guid_valid_for_role(guid.bytes, expected_role);
}

bool same_identity(P50WireLaunchIdentity wire,
                   local::Identity local) noexcept {
  return wire.generation == local.generation && wire.attempt == local.attempt;
}

ClaimAttemptCapability128 temporary_attempt_capability(
    uint64_t observation, uint64_t sequence) noexcept {
  ClaimAttemptCapability128 capability;
  for (unsigned index = 0; index != 8; ++index) {
    const unsigned shift = 56 - 8 * index;
    capability.bytes[index] =
        static_cast<uint8_t>(observation >> shift);
    capability.bytes[index + 8] =
        static_cast<uint8_t>(sequence >> shift);
  }
  return capability;
}

} // namespace

std::optional<P50CacheSessionArmBinding>
cache_session_binding_from_armed(const P50SourceArmedMsg &message) noexcept {
  if (!message.valid_payload())
    return std::nullopt;
  try {
    P50CacheSessionArmBinding binding;
    binding.arm = message.arm;
    binding.f_control_generation = message.f_control_generation;
    binding.f_control_attempt = message.f_control_attempt;
    binding.f_store_generation = message.f_store_generation;
    binding.f_store_guid = message.f_store_guid;
    binding.f_store_derivation_version =
        message.f_store_derivation_version;
    binding.arm_observation_id = message.arm_observation_id;
    binding.source_budget_msec = message.source_budget_msec;
    if (!binding.valid())
      return std::nullopt;
    return binding;
  } catch (...) {
    return std::nullopt;
  }
}

bool P50CurrentFIncarnation::valid() const noexcept {
  return daemon_generation != 0 && control_launch.generation != 0 &&
         control_launch.attempt != 0 && store_generation != 0 &&
         store_derivation_version == kStoreIdentityDerivationVersion &&
         ready_lease_observation_id != 0 &&
         role_guid(store_guid, kStoreIdentityFileRole);
}

P50CacheSessionAttemptAuthority::P50CacheSessionAttemptAuthority(
    size_t max_scopes)
    : max_scopes_(max_scopes) {
  if (max_scopes_ == 0)
    throw std::invalid_argument("P50 attempt authority scope limit is zero");
  scopes_.reserve(max_scopes_);
}

std::optional<P50CacheSessionAttemptProof>
P50CacheSessionAttemptAuthority::burn(
    const P50CacheSessionArmBinding &binding) noexcept {
  if (!binding.valid())
    return std::nullopt;
  const P50WireLaunchIdentity c_control_launch =
      binding.c_control_identity();
  const uint64_t arm_observation_id = binding.arm_observation_id;
  auto it = std::find_if(scopes_.begin(), scopes_.end(),
                         [&](const Scope &scope) {
                           return scope.c_control_launch == c_control_launch &&
                                  scope.arm_observation_id == arm_observation_id;
                         });
  if (it == scopes_.end()) {
    if (scopes_.size() >= max_scopes_ ||
        next_capability_ == 0 || next_capability_ == UINT64_MAX)
      return std::nullopt;
    const ClaimAttemptCapability128 capability_1 =
        temporary_attempt_capability(arm_observation_id,
                                     next_capability_++);
    const ClaimAttemptCapability128 capability_2 =
        temporary_attempt_capability(arm_observation_id,
                                     next_capability_++);
    try {
      scopes_.push_back(Scope{
          c_control_launch, arm_observation_id,
          {capability_1, capability_2}});
    } catch (...) {
      return std::nullopt;
    }
    it = std::prev(scopes_.end());
  }
  if (it->burned >= it->capabilities.size())
    return std::nullopt;
  const uint8_t ordinal = static_cast<uint8_t>(it->burned + 1);
  const ClaimAttemptCapability128 capability =
      it->capabilities[it->burned++];
  return P50CacheSessionAttemptProof{ordinal, capability};
}

bool P50CacheSessionAttemptAuthority::consume(
    const P50CacheSessionAttemptProof &proof,
    const P50CacheSessionArmBinding &binding) noexcept {
  if (!proof.valid() || !binding.valid())
    return false;
  auto it = std::find_if(scopes_.begin(), scopes_.end(),
                         [&](const Scope &scope) {
                           return scope.c_control_launch ==
                                      binding.c_control_identity() &&
                                  scope.arm_observation_id ==
                                      binding.arm_observation_id;
                         });
  if (it == scopes_.end() || proof.ordinal > it->burned)
    return false;
  const size_t index = proof.ordinal - 1;
  if (it->capabilities[index] != proof.selected_capability ||
      (it->consumed & static_cast<uint8_t>(1u << index)) != 0)
    return false;
  it->consumed = static_cast<uint8_t>(
      it->consumed | static_cast<uint8_t>(1u << index));
  return true;
}

P50CacheSessionOwnerAuthorityAdapter::P50CacheSessionOwnerAuthorityAdapter(
    const ConnectionLeaseRegistry &registry, OwnerResolver owner_resolver,
    ReadyProvider ready_provider) noexcept
    : registry_(&registry), owner_resolver_(owner_resolver),
      ready_provider_(ready_provider) {}

bool P50CacheSessionOwnerAuthorityAdapter::current(
    const P50CurrentFIncarnation &current_f,
    const P50CacheSessionArmBinding &binding,
    P50CacheSessionOwnerContext owner) const noexcept {
  if (registry_ == nullptr || owner_resolver_ == nullptr ||
      ready_provider_ == nullptr || !current_f.valid() || !binding.valid() ||
      !owner.valid() || owner.connection_lease.daemon_generation !=
                             current_f.daemon_generation)
    return false;
  P50CurrentFIncarnation published_ready{};
  if (!ready_provider_(published_ready) || published_ready != current_f ||
      !same_identity(binding.f_control_identity(),
                     current_f.control_launch) ||
      binding.f_store_guid != current_f.store_guid.bytes ||
      binding.f_store_generation != current_f.store_generation ||
      binding.f_store_derivation_version !=
          current_f.store_derivation_version)
    return false;
  Client *client = nullptr;
  MsgChannel *channel = nullptr;
  PeerCredentials peer{};
  if (owner_resolver_(owner.client_id, client, channel, peer) !=
          P50CacheSessionOwnerAuthorityState::Live ||
      client == nullptr || channel == nullptr || !peer.complete())
    return false;
  const auto record = registry_->revalidate(owner.connection_lease, client,
                                             channel, peer);
  return record.has_value() && record->provenance.cache_eligible();
}

P50CacheSessionOwnerAuthorityState
P50CacheSessionOwnerAuthorityAdapter::state(
    const P50CurrentFIncarnation &current_f,
    P50CacheSessionOwnerContext owner) const noexcept {
  if (registry_ == nullptr || owner_resolver_ == nullptr ||
      ready_provider_ == nullptr || !current_f.valid() || !owner.valid() ||
      owner.connection_lease.daemon_generation != current_f.daemon_generation)
    return P50CacheSessionOwnerAuthorityState::Unknown;
  P50CurrentFIncarnation published_ready{};
  if (!ready_provider_(published_ready) || published_ready != current_f)
    return P50CacheSessionOwnerAuthorityState::Unknown;
  Client *client = nullptr;
  MsgChannel *channel = nullptr;
  PeerCredentials peer{};
  const auto result = owner_resolver_(owner.client_id, client, channel, peer);
  if (result == P50CacheSessionOwnerAuthorityState::Retired)
    return registry_->lookup(owner.connection_lease).has_value()
               ? P50CacheSessionOwnerAuthorityState::Unknown
               : P50CacheSessionOwnerAuthorityState::Retired;
  if (result != P50CacheSessionOwnerAuthorityState::Live ||
      client == nullptr || channel == nullptr || !peer.complete() ||
      !registry_->revalidate(owner.connection_lease, client, channel, peer))
    return P50CacheSessionOwnerAuthorityState::Unknown;
  return P50CacheSessionOwnerAuthorityState::Live;
}

P50CacheSessionJoinTable::P50CacheSessionJoinTable(
    size_t capacity, P50CurrentFIncarnation current_f,
    const P50CacheSessionOwnerAuthority &owner_authority,
    P50CacheSessionAttemptAuthority &attempt_authority,
    size_t retirement_fence_capacity)
    : capacity_(capacity), current_f_(current_f),
      owner_authority_(&owner_authority), attempt_authority_(&attempt_authority),
      retirement_fence_capacity_(retirement_fence_capacity == 0
                                     ? capacity
                                     : retirement_fence_capacity) {
  if (capacity_ == 0 || !current_f_.valid())
    throw std::invalid_argument("P50 join capacity/incarnation is invalid");
  if (retirement_fence_capacity_ == 0)
    throw std::invalid_argument("P50 retirement fence limit is zero");
  rows_.reserve(capacity_);
  retired_owners_.reserve(retirement_fence_capacity_);
  static_assert(std::is_nothrow_move_assignable_v<Row>);
  static_assert(std::is_nothrow_move_assignable_v<P50CacheSessionOwnerContext>);
}

void P50CacheSessionJoinTable::reclaim_expired_pre_detach(
    TimePoint now) noexcept {
  for (size_t i = 0; i != rows_.size();) {
    const auto state = rows_[i].state;
    if (now >= rows_[i].deadline &&
        (state == P50CacheSessionJoinState::Armed ||
         state == P50CacheSessionJoinState::AttemptReserved)) {
      rows_.erase(rows_.begin() + static_cast<ptrdiff_t>(i));
      continue;
    }
    ++i;
  }
}

bool P50CacheSessionJoinTable::current_binding(
    const P50CacheSessionArmBinding &binding) const noexcept {
  return binding.valid() &&
         same_identity(binding.f_control_identity(),
                       current_f_.control_launch) &&
         binding.f_store_guid == current_f_.store_guid.bytes &&
         binding.f_store_generation == current_f_.store_generation &&
         binding.f_store_derivation_version ==
             current_f_.store_derivation_version;
}

bool P50CacheSessionJoinTable::current_owner(
    const P50CacheSessionArmBinding &binding,
    P50CacheSessionOwnerContext owner) const noexcept {
  return !owner_fenced(owner) && owner.valid() &&
         owner.connection_lease.daemon_generation ==
             current_f_.daemon_generation &&
         owner_authority_ != nullptr &&
         owner_authority_->current(current_f_, binding, owner);
}

P50CacheSessionJoinTable::Row *
P50CacheSessionJoinTable::row_for_observation(uint64_t observation) noexcept {
  for (auto &row : rows_) {
    if (row.binding.arm_observation_id == observation)
      return &row;
  }
  return nullptr;
}

const P50CacheSessionJoinTable::Row *
P50CacheSessionJoinTable::row_for_observation(
    uint64_t observation) const noexcept {
  for (const auto &row : rows_) {
    if (row.binding.arm_observation_id == observation)
      return &row;
  }
  return nullptr;
}

P50CacheSessionJoinTable::Row *P50CacheSessionJoinTable::exact_row(
    const P50CacheSessionArmBinding &binding,
    P50CacheSessionOwnerContext owner) noexcept {
  auto *row = row_for_observation(binding.arm_observation_id);
  return row != nullptr && row->binding == binding && row->owner == owner
             ? row
             : nullptr;
}

const P50CacheSessionJoinTable::Row *P50CacheSessionJoinTable::exact_row(
    const P50CacheSessionArmBinding &binding,
    P50CacheSessionOwnerContext owner) const noexcept {
  const auto *row = row_for_observation(binding.arm_observation_id);
  return row != nullptr && row->binding == binding && row->owner == owner
             ? row
             : nullptr;
}

P50CacheSessionJoinTable::Row *P50CacheSessionJoinTable::exact_claim(
    const P50CacheSessionWireClaim &claim,
    P50CacheSessionOwnerContext owner) noexcept {
  if (!claim.valid())
    return nullptr;
  auto *row = exact_row(claim.binding, owner);
  return row != nullptr &&
                 row->active_attempt == claim.attempt.selected_capability
             ? row
             : nullptr;
}

const P50CacheSessionJoinTable::Row *P50CacheSessionJoinTable::exact_claim(
    const P50CacheSessionWireClaim &claim,
    P50CacheSessionOwnerContext owner) const noexcept {
  if (!claim.valid())
    return nullptr;
  const auto *row = exact_row(claim.binding, owner);
  return row != nullptr &&
                 row->active_attempt == claim.attempt.selected_capability
             ? row
             : nullptr;
}

bool P50CacheSessionJoinTable::owner_fenced(
    P50CacheSessionOwnerContext owner) const noexcept {
  return std::any_of(retired_owners_.begin(), retired_owners_.end(),
                     [&](const P50CacheSessionOwnerContext &fenced) {
                       return fenced == owner;
                     });
}

void P50CacheSessionJoinTable::erase_row(Row *row) noexcept {
  if (row == nullptr)
    return;
  const auto it = std::find_if(rows_.begin(), rows_.end(),
                               [&](const Row &candidate) {
                                 return &candidate == row;
                               });
  if (it != rows_.end())
    rows_.erase(it);
}

bool P50CacheSessionJoinTable::terminal(
    P50CacheSessionJoinState state) noexcept {
  return state == P50CacheSessionJoinState::CommittedInput ||
         state == P50CacheSessionJoinState::CommittedSuppressed ||
         state == P50CacheSessionJoinState::ClosedPrecommit ||
         state == P50CacheSessionJoinState::CancelledPrecommit;
}

P50CacheSessionJoinDecision P50CacheSessionJoinTable::state_decision(
    P50CacheSessionJoinState state) noexcept {
  switch (state) {
  case P50CacheSessionJoinState::Armed:
    return P50CacheSessionJoinDecision::Registered;
  case P50CacheSessionJoinState::AttemptReserved:
    return P50CacheSessionJoinDecision::Reserved;
  case P50CacheSessionJoinState::PublicFdDetached:
    return P50CacheSessionJoinDecision::PublicFdDetached;
  case P50CacheSessionJoinState::EndpointInFlight:
    return P50CacheSessionJoinDecision::EndpointInFlight;
  case P50CacheSessionJoinState::ReconcileRequired:
    return P50CacheSessionJoinDecision::ReconcileRequired;
  case P50CacheSessionJoinState::CommittedInput:
    return P50CacheSessionJoinDecision::CommittedInput;
  case P50CacheSessionJoinState::CommittedSuppressed:
    return P50CacheSessionJoinDecision::CommittedSuppressed;
  case P50CacheSessionJoinState::ClosedPrecommit:
    return P50CacheSessionJoinDecision::ClosedPrecommit;
  case P50CacheSessionJoinState::CancelledPrecommit:
    return P50CacheSessionJoinDecision::CancelledPrecommit;
  }
  return P50CacheSessionJoinDecision::WrongState;
}

P50CacheSessionJoinDecision P50CacheSessionJoinTable::register_wait(
    const P50CacheSessionArmBinding &binding, P50CacheSessionOwnerContext owner,
    TimePoint now) {
  reclaim_expired_pre_detach(now);
  if (owner_fenced(owner))
    return P50CacheSessionJoinDecision::RetiredOwner;
  if (!current_binding(binding) || !current_owner(binding, owner))
    return P50CacheSessionJoinDecision::Invalid;
  const auto budget = std::chrono::milliseconds(binding.source_budget_msec);
  if (now > TimePoint::max() - budget)
    return P50CacheSessionJoinDecision::Invalid;
  const auto deadline = now + budget;
  if (binding.arm_observation_id <= registered_observation_high_water_)
    return P50CacheSessionJoinDecision::RetiredObservation;
  if (rows_.size() >= capacity_)
    return P50CacheSessionJoinDecision::CapacityExceeded;
  for (const auto &row : rows_) {
    if (row.owner == owner)
      return P50CacheSessionJoinDecision::Busy;
  }
  try {
    rows_.push_back(Row{binding, owner, deadline});
  } catch (...) {
    return P50CacheSessionJoinDecision::CapacityExceeded;
  }
  registered_observation_high_water_ = binding.arm_observation_id;
  return P50CacheSessionJoinDecision::Registered;
}

P50CacheSessionJoinDecision
P50CacheSessionJoinTable::reserve_claim(const P50CacheSessionWireClaim &claim,
                                        P50CacheSessionOwnerContext owner,
                                        TimePoint now) {
  if (owner_fenced(owner))
    return P50CacheSessionJoinDecision::RetiredOwner;
  if (!claim.valid() || !current_owner(claim.binding, owner))
    return P50CacheSessionJoinDecision::Invalid;
  auto *row = row_for_observation(claim.binding.arm_observation_id);
  if (row == nullptr) {
    return claim.binding.arm_observation_id <=
                   registered_observation_high_water_
               ? P50CacheSessionJoinDecision::RetiredObservation
               : P50CacheSessionJoinDecision::NoOwner;
  }
  if (row->binding != claim.binding || row->owner != owner)
    return P50CacheSessionJoinDecision::WrongClaim;
  if (terminal(row->state))
    return state_decision(row->state);
  if (now >= row->deadline) {
    row->owner_closed = true;
    if (row->state == P50CacheSessionJoinState::Armed ||
        row->state == P50CacheSessionJoinState::AttemptReserved) {
      erase_row(row);
      return P50CacheSessionJoinDecision::Expired;
    }
    row->state = P50CacheSessionJoinState::ReconcileRequired;
    return P50CacheSessionJoinDecision::ReconcileRequired;
  }
  for (size_t i = 0; i != row->attempt_count; ++i) {
    if (row->attempts[i] == claim.attempt.selected_capability)
      return P50CacheSessionJoinDecision::DuplicateAttempt;
  }
  if (row->state != P50CacheSessionJoinState::Armed)
    return P50CacheSessionJoinDecision::Busy;
  if (row->attempt_count == row->attempts.size())
    return P50CacheSessionJoinDecision::RetryExhausted;
  if (attempt_authority_ == nullptr ||
      !attempt_authority_->consume(claim.attempt, claim.binding))
    return P50CacheSessionJoinDecision::Invalid;
  row->attempts[row->attempt_count++] =
      claim.attempt.selected_capability;
  row->active_attempt = claim.attempt.selected_capability;
  row->state = P50CacheSessionJoinState::AttemptReserved;
  return P50CacheSessionJoinDecision::Reserved;
}

P50CacheSessionJoinDecision
P50CacheSessionJoinTable::release_pre_detach_for_retry(
    const P50CacheSessionWireClaim &claim, P50CacheSessionOwnerContext owner,
    TimePoint now) {
  auto *row = exact_claim(claim, owner);
  if (row == nullptr)
    return P50CacheSessionJoinDecision::WrongClaim;
  if (row->state != P50CacheSessionJoinState::AttemptReserved)
    return P50CacheSessionJoinDecision::WrongState;
  if (!current_owner(claim.binding, owner)) {
    erase_row(row);
    return P50CacheSessionJoinDecision::CancelledPrecommit;
  }
  if (now >= row->deadline) {
    erase_row(row);
    return P50CacheSessionJoinDecision::Expired;
  }
  if (row->attempt_count >= row->attempts.size()) {
    erase_row(row);
    return P50CacheSessionJoinDecision::RetryExhausted;
  }
  row->active_attempt = {};
  row->state = P50CacheSessionJoinState::Armed;
  return P50CacheSessionJoinDecision::RetryAllowed;
}

P50CacheSessionJoinDecision
P50CacheSessionJoinTable::revalidate_before_private_connect(
    const P50CacheSessionWireClaim &claim, P50CacheSessionOwnerContext owner,
    TimePoint now) const noexcept {
  if (!claim.valid() || !current_owner(claim.binding, owner))
    return P50CacheSessionJoinDecision::Invalid;
  const auto *row = exact_claim(claim, owner);
  if (row == nullptr)
    return P50CacheSessionJoinDecision::WrongClaim;
  if (now >= row->deadline)
    return P50CacheSessionJoinDecision::Expired;
  return row->state == P50CacheSessionJoinState::AttemptReserved
             ? P50CacheSessionJoinDecision::Reserved
             : state_decision(row->state);
}

P50CacheSessionJoinDecision
P50CacheSessionJoinTable::revalidate_before_fd_release(
    const P50CacheSessionWireClaim &claim, P50CacheSessionOwnerContext owner,
    TimePoint now) const noexcept {
  return revalidate_before_private_connect(claim, owner, now);
}

P50CacheSessionJoinDecision P50CacheSessionJoinTable::mark_public_fd_detached(
    const P50CacheSessionWireClaim &claim, P50CacheSessionOwnerContext owner,
    TimePoint now) noexcept {
  auto *row = exact_claim(claim, owner);
  if (row == nullptr)
    return P50CacheSessionJoinDecision::WrongClaim;
  if (row->state != P50CacheSessionJoinState::AttemptReserved)
    return state_decision(row->state);
  if (!current_owner(claim.binding, owner)) {
    erase_row(row);
    return P50CacheSessionJoinDecision::CancelledPrecommit;
  }
  if (now >= row->deadline) {
    erase_row(row);
    return P50CacheSessionJoinDecision::Expired;
  }
  row->state = P50CacheSessionJoinState::PublicFdDetached;
  return P50CacheSessionJoinDecision::PublicFdDetached;
}

P50CacheSessionJoinDecision P50CacheSessionJoinTable::mark_endpoint_inflight(
    const P50CacheSessionWireClaim &claim, P50CacheSessionOwnerContext owner,
    TimePoint now) noexcept {
  auto *row = exact_claim(claim, owner);
  if (row == nullptr)
    return P50CacheSessionJoinDecision::WrongClaim;
  if (row->state != P50CacheSessionJoinState::PublicFdDetached)
    return state_decision(row->state);
  if (!current_owner(claim.binding, owner)) {
    row->owner_closed = true;
    row->state = P50CacheSessionJoinState::ReconcileRequired;
    return P50CacheSessionJoinDecision::ReconcileRequired;
  }
  if (now >= row->deadline) {
    row->owner_closed = true;
    row->state = P50CacheSessionJoinState::ReconcileRequired;
    return P50CacheSessionJoinDecision::ReconcileRequired;
  }
  row->state = P50CacheSessionJoinState::EndpointInFlight;
  return P50CacheSessionJoinDecision::EndpointInFlight;
}

P50CacheSessionJoinDecision
P50CacheSessionJoinTable::cancel_wait(const P50CacheSessionArmBinding &binding,
                                      P50CacheSessionOwnerContext owner,
                                      TimePoint) noexcept {
  auto *row = exact_row(binding, owner);
  if (row == nullptr)
    return P50CacheSessionJoinDecision::WrongClaim;
  if (terminal(row->state))
    return state_decision(row->state);
  row->owner_closed = true;
  if (row->state == P50CacheSessionJoinState::Armed ||
      row->state == P50CacheSessionJoinState::AttemptReserved) {
    erase_row(row);
    return P50CacheSessionJoinDecision::CancelledPrecommit;
  }
  row->state = P50CacheSessionJoinState::ReconcileRequired;
  return P50CacheSessionJoinDecision::ReconcileRequired;
}

P50CacheSessionJoinDecision
P50CacheSessionJoinTable::expire_wait(const P50CacheSessionArmBinding &binding,
                                      P50CacheSessionOwnerContext owner,
                                      TimePoint now) noexcept {
  auto *row = exact_row(binding, owner);
  if (row == nullptr)
    return P50CacheSessionJoinDecision::WrongClaim;
  if (terminal(row->state))
    return state_decision(row->state);
  if (now < row->deadline)
    return P50CacheSessionJoinDecision::NotExpired;
  row->owner_closed = true;
  if (row->state == P50CacheSessionJoinState::Armed ||
      row->state == P50CacheSessionJoinState::AttemptReserved) {
    erase_row(row);
    return P50CacheSessionJoinDecision::ClosedPrecommit;
  }
  row->state = P50CacheSessionJoinState::ReconcileRequired;
  return P50CacheSessionJoinDecision::ReconcileRequired;
}

P50CacheSessionJoinDecision P50CacheSessionJoinTable::settle_endpoint(
    const P50CacheSessionWireClaim &claim, P50CacheSessionOwnerContext owner,
    P50EndpointSettlement settlement, TimePoint now) noexcept {
  if (settlement != P50EndpointSettlement::Unresolved &&
      settlement != P50EndpointSettlement::ProvedNoCommit &&
      settlement != P50EndpointSettlement::CommittedInput)
    return P50CacheSessionJoinDecision::InvalidSettlement;
  auto *row = exact_claim(claim, owner);
  if (row == nullptr)
    return P50CacheSessionJoinDecision::WrongClaim;
  if (terminal(row->state))
    return state_decision(row->state);
  if (row->state != P50CacheSessionJoinState::PublicFdDetached &&
      row->state != P50CacheSessionJoinState::EndpointInFlight &&
      row->state != P50CacheSessionJoinState::ReconcileRequired)
    return P50CacheSessionJoinDecision::WrongState;
  if (now >= row->deadline)
    row->owner_closed = true;
  if (settlement == P50EndpointSettlement::Unresolved) {
    row->state = P50CacheSessionJoinState::ReconcileRequired;
    return P50CacheSessionJoinDecision::ReconcileRequired;
  }
  if (settlement == P50EndpointSettlement::CommittedInput) {
    row->state = row->owner_closed
                     ? P50CacheSessionJoinState::CommittedSuppressed
                     : P50CacheSessionJoinState::CommittedInput;
    return state_decision(row->state);
  }
  row->state = row->owner_closed ? P50CacheSessionJoinState::CancelledPrecommit
                                 : P50CacheSessionJoinState::ClosedPrecommit;
  return state_decision(row->state);
}

P50CacheSessionJoinDecision
P50CacheSessionJoinTable::retire_wait(const P50CacheSessionArmBinding &binding,
                                      P50CacheSessionOwnerContext owner) {
  auto it = std::find_if(rows_.begin(), rows_.end(), [&](const Row &row) {
    return row.binding == binding && row.owner == owner;
  });
  if (it == rows_.end())
    return binding.arm_observation_id <= registered_observation_high_water_
               ? P50CacheSessionJoinDecision::RetiredObservation
               : P50CacheSessionJoinDecision::NoOwner;
  if (!terminal(it->state))
    return P50CacheSessionJoinDecision::WrongState;
  rows_.erase(it);
  return P50CacheSessionJoinDecision::RetiredObservation;
}

P50CacheSessionJoinDecision
P50CacheSessionJoinTable::retire_unseen_owner(
    P50CacheSessionOwnerContext owner) {
  if (!owner.valid() ||
      owner.connection_lease.daemon_generation != current_f_.daemon_generation)
    return P50CacheSessionJoinDecision::Invalid;
  if (owner_fenced(owner))
    return P50CacheSessionJoinDecision::RetiredOwner;
  for (const auto &row : rows_) {
    if (row.owner == owner)
      return P50CacheSessionJoinDecision::WrongState;
  }
  if (owner_authority_ == nullptr ||
      owner_authority_->state(current_f_, owner) !=
          P50CacheSessionOwnerAuthorityState::Retired)
    return P50CacheSessionJoinDecision::Invalid;
  if (retired_owners_.size() >= retirement_fence_capacity_)
    return P50CacheSessionJoinDecision::RetirementFenceCapacity;
  try {
    retired_owners_.push_back(owner);
  } catch (...) {
    return P50CacheSessionJoinDecision::RetirementFenceCapacity;
  }
  return P50CacheSessionJoinDecision::RetiredOwner;
}

std::optional<P50CacheSessionJoinState> P50CacheSessionJoinTable::state(
    const P50CacheSessionArmBinding &binding,
    P50CacheSessionOwnerContext owner) const noexcept {
  const auto *row = exact_row(binding, owner);
  return row == nullptr ? std::nullopt
                        : std::optional<P50CacheSessionJoinState>(row->state);
}

} // namespace icecc::p50::daemon
