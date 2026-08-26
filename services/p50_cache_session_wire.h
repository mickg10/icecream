#pragma once

// Foundational, dependency-downward Protocol-50 cache-session wire DTOs.
// This file is owned by services/libicecc: ordinary MsgChannel framing,
// cache reducers, iceccd and both sidecars all use this one codec.

#include "comm.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace icecc::p50::daemon {

inline constexpr uint16_t kP50CacheSessionWireVersion = 1;
inline constexpr uint16_t kP50CacheSessionClaimKind = 1;
inline constexpr size_t kP50CacheSessionEnvelopeBytes = 12;
inline constexpr size_t kP50CacheSessionClaimMaxWireBytes = 1024;
inline constexpr size_t kP50CacheSessionOutcomeMaxWireBytes = 1152;
inline constexpr uint32_t kP50CacheSessionMaxSourceBudgetMsec =
    P50SourceArmedFields::MaxSourceBudgetMsec;

// Wire identity deliberately does not depend on cache/p50_local_transport.h.
// Higher layers convert to their local Identity only after canonical decode.
struct P50WireLaunchIdentity {
  uint64_t generation = 0;
  uint64_t attempt = 0;

  [[nodiscard]] bool valid() const noexcept {
    return generation != 0 && attempt != 0;
  }
  auto operator<=>(const P50WireLaunchIdentity &) const = default;
};

// Exact facts F acknowledged on the selected ordinary compile connection.
// F-local connection lease/client facts are intentionally absent.
struct P50CacheSessionArmBinding {
  P50SourceArmFields arm{};
  uint64_t f_control_generation = 0;
  uint64_t f_control_attempt = 0;
  uint64_t f_store_generation = 0;
  std::array<uint8_t, 16> f_store_guid{};
  uint64_t f_store_derivation_version = 0;
  uint64_t arm_observation_id = 0;
  uint32_t source_budget_msec = 0;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] P50WireLaunchIdentity c_control_identity() const noexcept {
    return {arm.c_control_generation, arm.c_control_attempt};
  }
  [[nodiscard]] P50WireLaunchIdentity f_control_identity() const noexcept {
    return {f_control_generation, f_control_attempt};
  }
  auto operator<=>(const P50CacheSessionArmBinding &) const = default;
};

struct P50CacheSessionAttemptProof {
  uint8_t ordinal = 0;
  ClaimAttemptCapability128 selected_capability{};

  [[nodiscard]] bool valid() const noexcept {
    return ordinal >= 1 && ordinal <= 2 && selected_capability.valid();
  }
  auto operator<=>(const P50CacheSessionAttemptProof &) const = default;
};

struct P50CacheSessionWireClaim {
  P50CacheSessionArmBinding binding{};
  P50CacheSessionAttemptProof attempt{};

  [[nodiscard]] bool valid() const noexcept;
  auto operator<=>(const P50CacheSessionWireClaim &) const = default;
};

std::vector<uint8_t>
encode_cache_session_wire_claim(const P50CacheSessionWireClaim &claim);
std::optional<P50CacheSessionWireClaim>
decode_cache_session_wire_claim(std::span<const uint8_t> wire);

// Returns the exact self-delimiting P5CL size, or zero for any malformed or
// noncanonical prefix. Extra bytes after that prefix are permitted so P5CO
// can carry the exact claim followed by its kind-specific fields.
size_t canonical_cache_session_claim_prefix_size(
    std::span<const uint8_t> wire) noexcept;

enum class P50CacheSessionOutcomeKind : uint16_t {
  Adopted = 1,
  RefusedPreDetach = 2,
};

enum class P50CacheSessionRefusalReason : uint16_t {
  NoMatchingWait = 1,
  ReservationConflict = 2,
  OwnerChanged = 3,
  Deadline = 4,
  PrivateConnectFailed = 5,
  ReleaseBoundaryDirty = 6,
};

// Numeric value aligns with the existing typed cancellation target, but the
// wire DTO remains dependency-free from cache/p50_control_operation.h.
enum class P50SessionOperationRole : uint16_t {
  FSession = 2,
};

struct P50FSessionOperationId {
  P50WireLaunchIdentity sidecar_launch{};
  P50SessionOperationRole role = P50SessionOperationRole::FSession;
  uint64_t operation_sequence = 0;

  [[nodiscard]] bool valid() const noexcept {
    return sidecar_launch.valid() && role == P50SessionOperationRole::FSession &&
           operation_sequence != 0;
  }
  auto operator<=>(const P50FSessionOperationId &) const = default;
};

// Singular kind-specific outcome. For Adopted, refusal_reason is zero and
// launch/store/operation are present. For RefusedPreDetach, only the exact
// canonical P5CL echo and bounded diagnostic reason are present.
struct P50CacheSessionOutcome {
  P50CacheSessionOutcomeKind kind =
      P50CacheSessionOutcomeKind::RefusedPreDetach;
  std::vector<uint8_t> canonical_claim;
  P50WireLaunchIdentity f_sidecar_launch{};
  std::array<uint8_t, 16> f_store_guid{};
  P50FSessionOperationId operation{};
  std::optional<P50CacheSessionRefusalReason> refusal_reason;

  [[nodiscard]] bool valid() const noexcept;
  auto operator<=>(const P50CacheSessionOutcome &) const = default;
};

std::vector<uint8_t>
encode_cache_session_outcome(const P50CacheSessionOutcome &outcome);
std::optional<P50CacheSessionOutcome>
decode_cache_session_outcome(std::span<const uint8_t> wire);

} // namespace icecc::p50::daemon
