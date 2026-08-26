#include "p50_cache_session_wire.h"

#include <algorithm>
#include <array>
#include <limits>

namespace icecc::p50::daemon {
namespace {

constexpr std::array<uint8_t, 4> kClaimMagic{'P', '5', 'C', 'L'};
constexpr std::array<uint8_t, 4> kOutcomeMagic{'P', '5', 'C', 'O'};

void put_u8(std::vector<uint8_t> &out, uint8_t value) { out.push_back(value); }

void put_u16(std::vector<uint8_t> &out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value));
}

void put_u32(std::vector<uint8_t> &out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value >> 24));
  out.push_back(static_cast<uint8_t>(value >> 16));
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value));
}

void put_u64(std::vector<uint8_t> &out, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8)
    out.push_back(static_cast<uint8_t>(value >> shift));
}

void put_bytes(std::vector<uint8_t> &out, std::span<const uint8_t> bytes) {
  out.insert(out.end(), bytes.begin(), bytes.end());
}

void put_string32_nul(std::vector<uint8_t> &out, const std::string &value) {
  put_u32(out, static_cast<uint32_t>(value.size() + 1));
  put_bytes(out, std::span<const uint8_t>(
                     reinterpret_cast<const uint8_t *>(value.data()),
                     value.size()));
  put_u8(out, 0);
}

class Reader {
public:
  explicit Reader(std::span<const uint8_t> bytes) : bytes_(bytes) {}

  bool take_u8(uint8_t &value) {
    if (remaining() < 1)
      return false;
    value = bytes_[offset_++];
    return true;
  }
  bool take_u16(uint16_t &value) {
    if (remaining() < 2)
      return false;
    value = static_cast<uint16_t>(bytes_[offset_]) << 8 |
            static_cast<uint16_t>(bytes_[offset_ + 1]);
    offset_ += 2;
    return true;
  }
  bool take_u32(uint32_t &value) {
    if (remaining() < 4)
      return false;
    value = static_cast<uint32_t>(bytes_[offset_]) << 24 |
            static_cast<uint32_t>(bytes_[offset_ + 1]) << 16 |
            static_cast<uint32_t>(bytes_[offset_ + 2]) << 8 |
            static_cast<uint32_t>(bytes_[offset_ + 3]);
    offset_ += 4;
    return true;
  }
  bool take_u64(uint64_t &value) {
    if (remaining() < 8)
      return false;
    value = 0;
    for (unsigned i = 0; i != 8; ++i)
      value = (value << 8) | bytes_[offset_ + i];
    offset_ += 8;
    return true;
  }
  bool take_array(std::array<uint8_t, 16> &value) {
    if (remaining() < value.size())
      return false;
    std::copy_n(bytes_.begin() + static_cast<ptrdiff_t>(offset_), value.size(),
                value.begin());
    offset_ += value.size();
    return true;
  }
  bool take_string32_nul(std::string &value) {
    uint32_t wire_size = 0;
    if (!take_u32(wire_size) || wire_size == 0 || wire_size > 256 ||
        remaining() < wire_size || bytes_[offset_ + wire_size - 1] != 0)
      return false;
    const auto begin = bytes_.begin() + static_cast<ptrdiff_t>(offset_);
    const auto terminator = begin + static_cast<ptrdiff_t>(wire_size - 1);
    if (std::find(begin, terminator, uint8_t{0}) != terminator)
      return false;
    try {
      value.assign(reinterpret_cast<const char *>(&*begin), wire_size - 1);
    } catch (...) {
      return false;
    }
    offset_ += wire_size;
    return true;
  }
  bool take_zeroes(size_t count) {
    if (remaining() < count)
      return false;
    for (size_t i = 0; i != count; ++i) {
      if (bytes_[offset_ + i] != 0)
        return false;
    }
    offset_ += count;
    return true;
  }
  bool skip(size_t count) {
    if (count > remaining())
      return false;
    offset_ += count;
    return true;
  }
  [[nodiscard]] size_t remaining() const noexcept {
    return bytes_.size() - offset_;
  }

private:
  std::span<const uint8_t> bytes_;
  size_t offset_ = 0;
};

void put_launch(std::vector<uint8_t> &out, P50WireLaunchIdentity identity) {
  put_u64(out, identity.generation);
  put_u64(out, identity.attempt);
}

bool take_launch(Reader &reader, P50WireLaunchIdentity &identity) {
  return reader.take_u64(identity.generation) &&
         reader.take_u64(identity.attempt);
}

void put_arm(std::vector<uint8_t> &out, const P50SourceArmFields &arm) {
  put_u32(out, arm.wire_job_id);
  put_u64(out, arm.assignment_epoch);
  put_u64(out, arm.assignment_nonce);
  put_string32_nul(out, arm.selected_f_host);
  put_u32(out, arm.selected_f_ordinary_port);
  put_u32(out, arm.selected_f_cache_port);
  put_u32(out, arm.cache_protocol);
  put_u32(out, arm.cache_profile);
  put_u64(out, arm.logical_job);
  put_u64(out, arm.compiler_attempt);
  put_u64(out, arm.c_store_generation);
  put_u64(out, arm.c_store_derivation_version);
  put_bytes(out, arm.c_store_guid);
  put_u64(out, arm.source_request_id);
  put_u32(out, arm.source_mode);
  put_u64(out, arm.c_control_generation);
  put_u64(out, arm.c_control_attempt);
}

bool take_arm(Reader &reader, P50SourceArmFields &arm) {
  return reader.take_u32(arm.wire_job_id) &&
         reader.take_u64(arm.assignment_epoch) &&
         reader.take_u64(arm.assignment_nonce) &&
         reader.take_string32_nul(arm.selected_f_host) &&
         reader.take_u32(arm.selected_f_ordinary_port) &&
         reader.take_u32(arm.selected_f_cache_port) &&
         reader.take_u32(arm.cache_protocol) &&
         reader.take_u32(arm.cache_profile) &&
         reader.take_u64(arm.logical_job) &&
         reader.take_u64(arm.compiler_attempt) &&
         reader.take_u64(arm.c_store_generation) &&
         reader.take_u64(arm.c_store_derivation_version) &&
         reader.take_array(arm.c_store_guid) &&
         reader.take_u64(arm.source_request_id) &&
         reader.take_u32(arm.source_mode) &&
         reader.take_u64(arm.c_control_generation) &&
         reader.take_u64(arm.c_control_attempt);
}

void put_binding(std::vector<uint8_t> &out,
                 const P50CacheSessionArmBinding &binding) {
  put_arm(out, binding.arm);
  put_u64(out, binding.f_control_generation);
  put_u64(out, binding.f_control_attempt);
  put_u64(out, binding.f_store_generation);
  put_bytes(out, binding.f_store_guid);
  put_u64(out, binding.f_store_derivation_version);
  put_u64(out, binding.arm_observation_id);
  put_u32(out, binding.source_budget_msec);
  put_u32(out, 0);
}

bool take_binding(Reader &reader, P50CacheSessionArmBinding &binding) {
  return take_arm(reader, binding.arm) &&
         reader.take_u64(binding.f_control_generation) &&
         reader.take_u64(binding.f_control_attempt) &&
         reader.take_u64(binding.f_store_generation) &&
         reader.take_array(binding.f_store_guid) &&
         reader.take_u64(binding.f_store_derivation_version) &&
         reader.take_u64(binding.arm_observation_id) &&
         reader.take_u32(binding.source_budget_msec) && reader.take_zeroes(4);
}

std::vector<uint8_t> finish_wire(std::array<uint8_t, 4> magic,
                                 uint16_t kind,
                                 std::vector<uint8_t> body,
                                 size_t max_wire_bytes) {
  if (kind == 0 || body.empty() ||
      body.size() > std::numeric_limits<uint32_t>::max() ||
      body.size() > max_wire_bytes - kP50CacheSessionEnvelopeBytes)
    return {};
  std::vector<uint8_t> wire;
  wire.reserve(kP50CacheSessionEnvelopeBytes + body.size());
  wire.insert(wire.end(), magic.begin(), magic.end());
  put_u16(wire, kP50CacheSessionWireVersion);
  put_u16(wire, kind);
  put_u32(wire, static_cast<uint32_t>(body.size()));
  wire.insert(wire.end(), body.begin(), body.end());
  return wire;
}

bool start_wire(Reader &reader, std::span<const uint8_t> wire,
                std::array<uint8_t, 4> magic, size_t max_wire_bytes,
                uint16_t &kind, uint32_t &body_size) {
  if (wire.size() < kP50CacheSessionEnvelopeBytes ||
      wire.size() > max_wire_bytes ||
      !std::equal(magic.begin(), magic.end(), wire.begin()) ||
      !reader.skip(magic.size()))
    return false;
  uint16_t version = 0;
  return reader.take_u16(version) && reader.take_u16(kind) &&
         reader.take_u32(body_size) &&
         version == kP50CacheSessionWireVersion && kind != 0 &&
         body_size == wire.size() - kP50CacheSessionEnvelopeBytes;
}

bool refusal_reason_valid(P50CacheSessionRefusalReason reason) noexcept {
  return reason >= P50CacheSessionRefusalReason::NoMatchingWait &&
         reason <= P50CacheSessionRefusalReason::ReleaseBoundaryDirty;
}

bool guid_is_f_role(const std::array<uint8_t, 16> &guid) noexcept {
  return icecc::p50::store_identity_guid_valid_for_role(
      guid, icecc::p50::kStoreIdentityFileRole);
}

bool all_zero(const std::array<uint8_t, 16> &value) noexcept {
  return std::all_of(value.begin(), value.end(),
                     [](uint8_t byte) { return byte == 0; });
}

} // namespace

bool P50CacheSessionArmBinding::valid() const noexcept {
  return arm.valid() && f_control_generation != 0 &&
         f_control_attempt != 0 && f_store_generation != 0 &&
         guid_is_f_role(f_store_guid) &&
         f_store_derivation_version == kStoreIdentityDerivationVersion &&
         arm_observation_id != 0 && source_budget_msec != 0 &&
         source_budget_msec <= P50SourceArmedFields::MaxSourceBudgetMsec &&
         !store_identity_file_guid_matches_client(arm.c_store_guid,
                                                  f_store_guid);
}

bool P50CacheSessionWireClaim::valid() const noexcept {
  return binding.valid() && attempt.valid();
}

std::vector<uint8_t>
encode_cache_session_wire_claim(const P50CacheSessionWireClaim &claim) {
  if (!claim.valid())
    return {};
  try {
    std::vector<uint8_t> body;
    body.reserve(256 + claim.binding.arm.selected_f_host.size());
    put_binding(body, claim.binding);
    put_u8(body, claim.attempt.ordinal);
    for (unsigned i = 0; i != 7; ++i)
      put_u8(body, 0);
    put_bytes(body, claim.attempt.selected_capability.bytes);
    return finish_wire(kClaimMagic, kP50CacheSessionClaimKind,
                       std::move(body), kP50CacheSessionClaimMaxWireBytes);
  } catch (...) {
    return {};
  }
}

std::optional<P50CacheSessionWireClaim>
decode_cache_session_wire_claim(std::span<const uint8_t> wire) {
  Reader reader(wire);
  uint16_t kind = 0;
  uint32_t body_size = 0;
  if (!start_wire(reader, wire, kClaimMagic,
                  kP50CacheSessionClaimMaxWireBytes, kind, body_size) ||
      kind != kP50CacheSessionClaimKind || body_size == 0)
    return std::nullopt;
  try {
    P50CacheSessionWireClaim claim;
    if (!take_binding(reader, claim.binding) ||
        !reader.take_u8(claim.attempt.ordinal) || !reader.take_zeroes(7) ||
        !reader.take_array(claim.attempt.selected_capability.bytes) ||
        reader.remaining() != 0 || !claim.valid())
      return std::nullopt;
    const auto canonical = encode_cache_session_wire_claim(claim);
    if (canonical.size() != wire.size() ||
        !std::equal(canonical.begin(), canonical.end(), wire.begin()))
      return std::nullopt;
    return claim;
  } catch (...) {
    return std::nullopt;
  }
}

size_t canonical_cache_session_claim_prefix_size(
    std::span<const uint8_t> wire) noexcept {
  if (wire.size() < kP50CacheSessionEnvelopeBytes ||
      !std::equal(kClaimMagic.begin(), kClaimMagic.end(), wire.begin()))
    return 0;
  const uint16_t version = static_cast<uint16_t>(wire[4]) << 8 | wire[5];
  const uint16_t kind = static_cast<uint16_t>(wire[6]) << 8 | wire[7];
  const uint32_t body_size = static_cast<uint32_t>(wire[8]) << 24 |
                             static_cast<uint32_t>(wire[9]) << 16 |
                             static_cast<uint32_t>(wire[10]) << 8 | wire[11];
  if (version != kP50CacheSessionWireVersion ||
      kind != kP50CacheSessionClaimKind ||
      body_size > kP50CacheSessionClaimMaxWireBytes -
                      kP50CacheSessionEnvelopeBytes)
    return 0;
  const size_t total = kP50CacheSessionEnvelopeBytes + body_size;
  if (total > wire.size())
    return 0;
  return decode_cache_session_wire_claim(wire.first(total)).has_value()
             ? total
             : 0;
}

bool P50CacheSessionOutcome::valid() const noexcept {
  const auto claim = decode_cache_session_wire_claim(canonical_claim);
  if (!claim.has_value())
    return false;
  if (kind == P50CacheSessionOutcomeKind::Adopted)
    return !refusal_reason.has_value() && f_sidecar_launch.valid() &&
           guid_is_f_role(f_store_guid) && operation.valid() &&
           operation.sidecar_launch == f_sidecar_launch &&
           f_sidecar_launch == claim->binding.f_control_identity() &&
           f_store_guid == claim->binding.f_store_guid;
  if (kind == P50CacheSessionOutcomeKind::RefusedPreDetach)
    return refusal_reason.has_value() &&
           refusal_reason_valid(*refusal_reason) &&
           f_sidecar_launch == P50WireLaunchIdentity{} &&
           all_zero(f_store_guid) && operation == P50FSessionOperationId{};
  return false;
}

std::vector<uint8_t>
encode_cache_session_outcome(const P50CacheSessionOutcome &outcome) {
  if (!outcome.valid())
    return {};
  try {
    std::vector<uint8_t> body;
    body.reserve(outcome.canonical_claim.size() + 48);
    put_bytes(body, outcome.canonical_claim);
    if (outcome.kind == P50CacheSessionOutcomeKind::Adopted) {
      put_launch(body, outcome.f_sidecar_launch);
      put_bytes(body, outcome.f_store_guid);
      put_u16(body, static_cast<uint16_t>(outcome.operation.role));
      put_u16(body, 0);
      put_u64(body, outcome.operation.operation_sequence);
    } else {
      put_u16(body, static_cast<uint16_t>(*outcome.refusal_reason));
      put_u16(body, 0);
    }
    return finish_wire(kOutcomeMagic, static_cast<uint16_t>(outcome.kind),
                       std::move(body), kP50CacheSessionOutcomeMaxWireBytes);
  } catch (...) {
    return {};
  }
}

std::optional<P50CacheSessionOutcome>
decode_cache_session_outcome(std::span<const uint8_t> wire) {
  Reader reader(wire);
  uint16_t raw_kind = 0;
  uint32_t body_size = 0;
  if (!start_wire(reader, wire, kOutcomeMagic,
                  kP50CacheSessionOutcomeMaxWireBytes, raw_kind, body_size) ||
      body_size == 0)
    return std::nullopt;
  const auto kind = static_cast<P50CacheSessionOutcomeKind>(raw_kind);
  if (kind != P50CacheSessionOutcomeKind::Adopted &&
      kind != P50CacheSessionOutcomeKind::RefusedPreDetach)
    return std::nullopt;

  const auto body = wire.subspan(kP50CacheSessionEnvelopeBytes);
  const size_t claim_size = canonical_cache_session_claim_prefix_size(body);
  if (claim_size == 0 || !reader.skip(claim_size))
    return std::nullopt;

  try {
    P50CacheSessionOutcome outcome;
    outcome.kind = kind;
    outcome.canonical_claim.assign(body.begin(),
                                   body.begin() +
                                       static_cast<ptrdiff_t>(claim_size));
    if (kind == P50CacheSessionOutcomeKind::Adopted) {
      uint16_t role = 0;
      if (!take_launch(reader, outcome.f_sidecar_launch) ||
          !reader.take_array(outcome.f_store_guid) ||
          !reader.take_u16(role) || !reader.take_zeroes(2) ||
          !reader.take_u64(outcome.operation.operation_sequence))
        return std::nullopt;
      outcome.operation.sidecar_launch = outcome.f_sidecar_launch;
      outcome.operation.role = static_cast<P50SessionOperationRole>(role);
    } else {
      uint16_t reason = 0;
      if (!reader.take_u16(reason) || !reader.take_zeroes(2))
        return std::nullopt;
      outcome.refusal_reason =
          static_cast<P50CacheSessionRefusalReason>(reason);
    }
    if (reader.remaining() != 0 || !outcome.valid())
      return std::nullopt;
    const auto canonical = encode_cache_session_outcome(outcome);
    if (canonical.size() != wire.size() ||
        !std::equal(canonical.begin(), canonical.end(), wire.begin()))
      return std::nullopt;
    return outcome;
  } catch (...) {
    return std::nullopt;
  }
}

} // namespace icecc::p50::daemon
