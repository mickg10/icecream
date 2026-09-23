// Test-only reference component; not part of the live P50 implementation.
#pragma once

// Event-loop-owned Protocol-50 join between an exact F WAITP50INPUT owner and
// a later public CACHE_SESSION claim.  Authority is registered when F admits
// P50_SOURCE_ARM; a network claim can reserve an existing registration but can
// never manufacture one.

#include "daemon/connection_provenance.h"
#include "cache/p50_local_transport.h"
#include "cache/p50_source_identity.h"
#include "services/comm.h"
#include "services/p50_cache_session_wire.h"
#include "services/p50_store_identity_wire.h"

#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace icecc::p50::daemon {

// The only public wire-message conversion consumes the actual decoded
// message, including its private trailing/truncation validity latch.
std::optional<P50CacheSessionArmBinding>
cache_session_binding_from_armed(const P50SourceArmedMsg &message) noexcept;

struct P50CacheSessionOwnerContext {
  ConnectionLeaseId connection_lease{};
  uint64_t client_id = 0;

  [[nodiscard]] bool valid() const noexcept {
    return connection_lease.valid() && client_id != 0;
  }
  auto operator<=>(const P50CacheSessionOwnerContext &) const = default;
};

enum class P50CacheSessionOwnerAuthorityState : uint8_t {
  Unknown = 0,
  Live,
  Retired,
};

struct P50CurrentFIncarnation;

// Production implements this on the daemon event-loop owner using
// ConnectionLeaseRegistry::revalidate plus the exact Client/channel/peer and
// cache-eligibility checks.  It must also compare the supplied structured-
// READY incarnation to the currently published listener.  A nonzero value
// record alone is never authority.
class P50CacheSessionOwnerAuthority {
public:
  virtual ~P50CacheSessionOwnerAuthority() = default;
  [[nodiscard]] virtual bool
  current(const P50CurrentFIncarnation &current_f,
          const P50CacheSessionArmBinding &binding,
          P50CacheSessionOwnerContext owner) const noexcept = 0;
  [[nodiscard]] virtual P50CacheSessionOwnerAuthorityState
  state(const P50CurrentFIncarnation &current_f,
        P50CacheSessionOwnerContext owner) const noexcept = 0;
};

struct P50CurrentFIncarnation {
  uint64_t daemon_generation = 0;
  local::Identity control_launch{};
  FStoreGuid store_guid{};
  uint64_t store_generation = 0;
  uint64_t store_derivation_version = 0;
  // Local structured-READY lease observation. It never crosses the public
  // claim wire and rotates with listener replacement.
  uint64_t ready_lease_observation_id = 0;

  [[nodiscard]] bool valid() const noexcept;
  auto operator<=>(const P50CurrentFIncarnation &) const = default;
};

// The authority owns at most two capabilities for each exact
// (C-control-launch, arm-observation) scope. Capabilities are consumed by the
// F reducer; a nonzero ordinal alone is never sufficient.
class P50CacheSessionAttemptAuthority {
public:
  explicit P50CacheSessionAttemptAuthority(size_t max_scopes = 1024);
  P50CacheSessionAttemptAuthority(const P50CacheSessionAttemptAuthority &) =
      delete;
  P50CacheSessionAttemptAuthority &
  operator=(const P50CacheSessionAttemptAuthority &) = delete;

  [[nodiscard]] std::optional<P50CacheSessionAttemptProof>
  burn(const P50CacheSessionArmBinding &binding) noexcept;
  [[nodiscard]] bool consume(const P50CacheSessionAttemptProof &proof,
                              const P50CacheSessionArmBinding &binding) noexcept;

private:
  struct Scope {
    P50WireLaunchIdentity c_control_launch{};
    uint64_t arm_observation_id = 0;
    std::array<ClaimAttemptCapability128, 2> capabilities{};
    uint8_t burned = 0;
    uint8_t consumed = 0;
  };

  size_t max_scopes_ = 0;
  uint64_t next_capability_ = 1;
  std::vector<Scope> scopes_;
};

// Production-shaped adapter. The resolver must return the exact Client,
// MsgChannel, and peer credentials for the logical client id. The READY
// provider must return the currently published structured READY incarnation;
// equality therefore consumes ready_lease_observation_id as well as the F
// launch/store identity. Actual daemon call-site wiring remains HOLD.
class P50CacheSessionOwnerAuthorityAdapter final
    : public P50CacheSessionOwnerAuthority {
public:
  using OwnerResolver = P50CacheSessionOwnerAuthorityState (*)(
      uint64_t client_id, Client *&client, MsgChannel *&channel,
      PeerCredentials &peer) noexcept;
  using ReadyProvider = bool (*)(P50CurrentFIncarnation &current) noexcept;

  P50CacheSessionOwnerAuthorityAdapter(const ConnectionLeaseRegistry &registry,
                                       OwnerResolver owner_resolver,
                                       ReadyProvider ready_provider) noexcept;

  [[nodiscard]] bool
  current(const P50CurrentFIncarnation &current_f,
          const P50CacheSessionArmBinding &binding,
          P50CacheSessionOwnerContext owner) const noexcept override;
  [[nodiscard]] P50CacheSessionOwnerAuthorityState
  state(const P50CurrentFIncarnation &current_f,
        P50CacheSessionOwnerContext owner) const noexcept override;

private:
  const ConnectionLeaseRegistry *registry_ = nullptr;
  OwnerResolver owner_resolver_ = nullptr;
  ReadyProvider ready_provider_ = nullptr;
};

enum class P50CacheSessionJoinState : uint8_t {
  Armed = 0,
  AttemptReserved,
  PublicFdDetached,
  EndpointInFlight,
  ReconcileRequired,
  CommittedInput,
  CommittedSuppressed,
  ClosedPrecommit,
  CancelledPrecommit,
};

enum class P50EndpointSettlement : uint8_t {
  Unresolved = 0,
  ProvedNoCommit,
  CommittedInput,
};

enum class P50CacheSessionJoinDecision : uint8_t {
  Invalid = 0,
  Registered,
  Reserved,
  DuplicateAttempt,
  Busy,
  RetryAllowed,
  RetryExhausted,
  PublicFdDetached,
  EndpointInFlight,
  ReconcileRequired,
  CommittedInput,
  CommittedSuppressed,
  ClosedPrecommit,
  CancelledPrecommit,
  NoOwner,
  WrongClaim,
  WrongState,
  NotExpired,
  Expired,
  RetiredObservation,
  RetiredOwner,
  RetirementFenceCapacity,
  InvalidSettlement,
  CapacityExceeded,
};

// Single-threaded/event-loop-owned reducer.  Observation monotonicity is
// consumed at register_wait(), which runs in F arm order, never at public
// connection arrival.  Consequently two live claims may arrive in reverse
// order without weakening stale-observation fencing.
class P50CacheSessionJoinTable {
public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;

  explicit P50CacheSessionJoinTable(
      size_t capacity, P50CurrentFIncarnation current_f,
      const P50CacheSessionOwnerAuthority &owner_authority,
      P50CacheSessionAttemptAuthority &attempt_authority,
      size_t retirement_fence_capacity = 0);
  P50CacheSessionJoinTable(const P50CacheSessionJoinTable &) = delete;
  P50CacheSessionJoinTable &
  operator=(const P50CacheSessionJoinTable &) = delete;

  P50CacheSessionJoinDecision
  register_wait(const P50CacheSessionArmBinding &binding,
                P50CacheSessionOwnerContext owner, TimePoint now);

  P50CacheSessionJoinDecision
  reserve_claim(const P50CacheSessionWireClaim &claim,
                P50CacheSessionOwnerContext owner, TimePoint now);

  // A failure before descriptor detach may reopen the same immutable WAIT
  // once.  The old attempt remains burned and the absolute deadline remains
  // unchanged.
  P50CacheSessionJoinDecision
  release_pre_detach_for_retry(const P50CacheSessionWireClaim &claim,
                               P50CacheSessionOwnerContext owner,
                               TimePoint now);

  [[nodiscard]] P50CacheSessionJoinDecision
  revalidate_before_private_connect(const P50CacheSessionWireClaim &claim,
                                    P50CacheSessionOwnerContext owner,
                                    TimePoint now) const noexcept;
  [[nodiscard]] P50CacheSessionJoinDecision
  revalidate_before_fd_release(const P50CacheSessionWireClaim &claim,
                               P50CacheSessionOwnerContext owner,
                               TimePoint now) const noexcept;

  P50CacheSessionJoinDecision
  mark_public_fd_detached(const P50CacheSessionWireClaim &claim,
                          P50CacheSessionOwnerContext owner,
                          TimePoint now) noexcept;
  P50CacheSessionJoinDecision
  mark_endpoint_inflight(const P50CacheSessionWireClaim &claim,
                         P50CacheSessionOwnerContext owner,
                         TimePoint now) noexcept;

  // Owner loss/expiry before detach is terminal.  After detach it merely
  // suppresses delivery and enters ReconcileRequired until the endpoint
  // owner reports an authoritative settlement.
  P50CacheSessionJoinDecision
  cancel_wait(const P50CacheSessionArmBinding &binding,
              P50CacheSessionOwnerContext owner, TimePoint now) noexcept;
  P50CacheSessionJoinDecision
  expire_wait(const P50CacheSessionArmBinding &binding,
              P50CacheSessionOwnerContext owner, TimePoint now) noexcept;

  // This method deliberately ignores the local deadline: a late canonical
  // committed_input witness remains authoritative.  Unresolved is not a
  // terminal result and can never authorize retirement.
  P50CacheSessionJoinDecision
  settle_endpoint(const P50CacheSessionWireClaim &claim,
                  P50CacheSessionOwnerContext owner,
                  P50EndpointSettlement settlement, TimePoint now) noexcept;

  P50CacheSessionJoinDecision
  retire_wait(const P50CacheSessionArmBinding &binding,
              P50CacheSessionOwnerContext owner);

  // The caller must have observed the owner as Retired in the authoritative
  // registry. This installs an exact-owner fence only; lower live connection
  // sequences remain admissible. Fence exhaustion fails closed.
  P50CacheSessionJoinDecision
  retire_unseen_owner(P50CacheSessionOwnerContext owner);

  [[nodiscard]] std::optional<P50CacheSessionJoinState>
  state(const P50CacheSessionArmBinding &binding,
        P50CacheSessionOwnerContext owner) const noexcept;
  [[nodiscard]] size_t active_size() const noexcept { return rows_.size(); }
  [[nodiscard]] size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] size_t retirement_fence_size() const noexcept {
    return retired_owners_.size();
  }
  [[nodiscard]] uint64_t registered_observation_high_water() const noexcept {
    return registered_observation_high_water_;
  }

private:
  static constexpr size_t kMaxAttemptsPerWait = 2;

  struct Row {
    P50CacheSessionArmBinding binding{};
    P50CacheSessionOwnerContext owner{};
    TimePoint deadline{};
    P50CacheSessionJoinState state = P50CacheSessionJoinState::Armed;
    std::array<ClaimAttemptCapability128, kMaxAttemptsPerWait> attempts{};
    size_t attempt_count = 0;
    ClaimAttemptCapability128 active_attempt{};
    bool owner_closed = false;
  };

  [[nodiscard]] bool
  current_binding(const P50CacheSessionArmBinding &binding) const noexcept;
  [[nodiscard]] bool
  current_owner(const P50CacheSessionArmBinding &binding,
                P50CacheSessionOwnerContext owner) const noexcept;
  [[nodiscard]] Row *row_for_observation(uint64_t observation) noexcept;
  [[nodiscard]] const Row *
  row_for_observation(uint64_t observation) const noexcept;
  [[nodiscard]] Row *exact_row(const P50CacheSessionArmBinding &binding,
                               P50CacheSessionOwnerContext owner) noexcept;
  [[nodiscard]] const Row *
  exact_row(const P50CacheSessionArmBinding &binding,
            P50CacheSessionOwnerContext owner) const noexcept;
  [[nodiscard]] Row *exact_claim(const P50CacheSessionWireClaim &claim,
                                 P50CacheSessionOwnerContext owner) noexcept;
  [[nodiscard]] const Row *
  exact_claim(const P50CacheSessionWireClaim &claim,
              P50CacheSessionOwnerContext owner) const noexcept;
  [[nodiscard]] bool owner_fenced(P50CacheSessionOwnerContext owner) const noexcept;
  void reclaim_expired_pre_detach(TimePoint now) noexcept;
  void erase_row(Row *row) noexcept;
  [[nodiscard]] static bool terminal(P50CacheSessionJoinState state) noexcept;
  [[nodiscard]] static P50CacheSessionJoinDecision
  state_decision(P50CacheSessionJoinState state) noexcept;

  size_t capacity_;
  P50CurrentFIncarnation current_f_{};
  const P50CacheSessionOwnerAuthority *owner_authority_ = nullptr;
  P50CacheSessionAttemptAuthority *attempt_authority_ = nullptr;
  size_t retirement_fence_capacity_ = 0;
  std::vector<Row> rows_;
  std::vector<P50CacheSessionOwnerContext> retired_owners_;
  uint64_t registered_observation_high_water_ = 0;
};

} // namespace icecc::p50::daemon
