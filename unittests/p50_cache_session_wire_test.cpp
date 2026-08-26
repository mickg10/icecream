#include "../services/comm.h"
#include "../services/p50_cache_session_wire.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace icecc::p50;
using namespace icecc::p50::daemon;

int failures = 0;

enum class EntropyMode {
  Distinct,
  Zero,
  Equal,
  Short,
  InterruptedThenDistinct,
};

EntropyMode entropy_mode = EntropyMode::Distinct;
unsigned entropy_calls = 0;

ssize_t fixture_entropy(void *buffer, size_t size, unsigned) noexcept {
  ++entropy_calls;
  auto *bytes = static_cast<uint8_t *>(buffer);
  if (entropy_mode == EntropyMode::Short) {
    // Vary the bytes across calls so a mutant that accepts a positive short
    // read cannot hide behind the independent equality guard.
    std::memset(bytes, static_cast<int>(0x70 + entropy_calls), size);
    return static_cast<ssize_t>(size - 1);
  }
  if (entropy_mode == EntropyMode::InterruptedThenDistinct &&
      entropy_calls == 1) {
    errno = EINTR;
    return -1;
  }
  const uint8_t fill =
      entropy_mode == EntropyMode::Zero
          ? 0
          : entropy_mode == EntropyMode::Equal
                ? 0x61
                : static_cast<uint8_t>(0x20 + entropy_calls);
  std::memset(bytes, fill, size);
  return static_cast<ssize_t>(size);
}

template <size_t Size>
size_t occurrence_count(const std::vector<uint8_t> &wire,
                        const std::array<uint8_t, Size> &needle) {
  size_t count = 0;
  auto begin = wire.begin();
  while (begin != wire.end()) {
    const auto found =
        std::search(begin, wire.end(), needle.begin(), needle.end());
    if (found == wire.end())
      break;
    ++count;
    begin = std::next(found);
  }
  return count;
}

#define CHECK(expression, message)                                             \
  do {                                                                         \
    if (!(expression)) {                                                       \
      std::fprintf(stderr, "FAILED - %s\n", message);                         \
      ++failures;                                                              \
    }                                                                          \
  } while (false)

struct Pair {
  MsgChannel *left = nullptr;
  MsgChannel *right = nullptr;
  Pair() = default;
  Pair(const Pair &) = delete;
  Pair &operator=(const Pair &) = delete;
  Pair(Pair &&other) noexcept : left(other.left), right(other.right) {
    other.left = other.right = nullptr;
  }
  ~Pair() {
    delete left;
    delete right;
  }
};

Pair make_pair(int protocol = PROTOCOL_VERSION) {
  int fds[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
    std::exit(2);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  Pair pair;
  std::thread left([&] {
    pair.left = Service::createChannel(
        fds[0], reinterpret_cast<sockaddr *>(&address), sizeof(address));
  });
  std::thread right([&] {
    pair.right = Service::createChannel(
        fds[1], reinterpret_cast<sockaddr *>(&address), sizeof(address));
  });
  left.join();
  right.join();
  if (pair.left == nullptr || pair.right == nullptr)
    std::exit(2);
  pair.left->protocol = pair.right->protocol = protocol;
  return pair;
}

std::array<uint8_t, 16> c_guid() {
  std::array<uint8_t, 16> result{};
  for (size_t i = 0; i != result.size(); ++i)
    result[i] = static_cast<uint8_t>(i + 1);
  result[kStoreIdentityRoleByte] &=
      static_cast<uint8_t>(~kStoreIdentityRoleMask);
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

void capability_entropy_matrix() {
  static_assert(sizeof(ClaimAttemptCapability128) == 16);
  ClaimAttemptCapability128 first;
  ClaimAttemptCapability128 second;

  entropy_mode = EntropyMode::Distinct;
  entropy_calls = 0;
  CHECK(fresh_claim_attempt_capabilities_with_provider(
            first, second, fixture_entropy) &&
            first.valid() && second.valid() && first != second,
        "OS-provider seam creates two distinct typed 128-bit capabilities");

  entropy_mode = EntropyMode::InterruptedThenDistinct;
  entropy_calls = 0;
  CHECK(fresh_claim_attempt_capabilities_with_provider(
            first, second, fixture_entropy) && first != second,
        "bounded EINTR restarts preserve all-or-nothing generation");

  for (const EntropyMode mode : {EntropyMode::Zero, EntropyMode::Equal,
                                 EntropyMode::Short}) {
    entropy_mode = mode;
    entropy_calls = 0;
    first = capability(1);
    second = capability(2);
    CHECK(!fresh_claim_attempt_capabilities_with_provider(
              first, second, fixture_entropy) &&
              !first.valid() && !second.valid(),
          "zero, repeated-equal, and short entropy fail closed and clear");
  }
}

P50CacheSessionWireClaim exact_claim() {
  P50CacheSessionWireClaim claim;
  auto &arm = claim.binding.arm;
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
  claim.binding.f_store_derivation_version =
      kStoreIdentityDerivationVersion;
  claim.binding.arm_observation_id = 34;
  claim.binding.source_budget_msec = 5000;
  claim.attempt = {1, capability(51)};
  return claim;
}

P50CacheSessionOutcome adopted(const std::vector<uint8_t> &claim_wire,
                               uint64_t operation_sequence = 91) {
  const auto claim = decode_cache_session_wire_claim(claim_wire);
  if (!claim)
    std::exit(2);
  P50CacheSessionOutcome outcome;
  outcome.kind = P50CacheSessionOutcomeKind::Adopted;
  outcome.canonical_claim = claim_wire;
  outcome.f_sidecar_launch = claim->binding.f_control_identity();
  outcome.f_store_guid = claim->binding.f_store_guid;
  outcome.operation = {outcome.f_sidecar_launch,
                       P50SessionOperationRole::FSession,
                       operation_sequence};
  return outcome;
}

P50CacheSessionOutcome refused(const std::vector<uint8_t> &claim_wire) {
  P50CacheSessionOutcome outcome;
  outcome.kind = P50CacheSessionOutcomeKind::RefusedPreDetach;
  outcome.canonical_claim = claim_wire;
  outcome.refusal_reason =
      P50CacheSessionRefusalReason::ReservationConflict;
  return outcome;
}

void send_raw(int fd, std::span<const uint8_t> bytes) {
  size_t offset = 0;
  while (offset != bytes.size()) {
    const ssize_t result =
        send(fd, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
    if (result > 0) {
      offset += static_cast<size_t>(result);
      continue;
    }
    std::exit(2);
  }
}

void send_raw_ordinary(int fd, Msg::Value type,
                       std::span<const uint8_t> payload) {
  std::vector<uint8_t> frame(8 + payload.size());
  const uint32_t length = htonl(static_cast<uint32_t>(4 + payload.size()));
  const uint32_t raw_type = htonl(static_cast<uint32_t>(type));
  std::memcpy(frame.data(), &length, sizeof(length));
  std::memcpy(frame.data() + 4, &raw_type, sizeof(raw_type));
  std::copy(payload.begin(), payload.end(), frame.begin() + 8);
  send_raw(fd, frame);
}

P50ServerClaimReleaseTicket reserve_server_claim(
    MsgChannel &server, const P50CacheSessionWireClaim &claim,
    uint64_t reservation_id) {
  std::unique_ptr<Msg> decoded(server.get_msg(2, true));
  if (!decoded || *decoded != Msg::P50_CACHE_SESSION_CLAIM)
    return {};
  auto stamp = server.take_p50_decoded_claim_stamp();
  return server.issue_p50_server_claim_release_ticket(
      std::move(stamp), reservation_id, claim.attempt.selected_capability);
}

void codec_matrix() {
  static_assert(Msg::P50_CACHE_SESSION_CLAIM == UINT32_C(0x50f00012));
  static_assert(Msg::P50_CACHE_SESSION_OUTCOME == UINT32_C(0x50f00013));
  static_assert(p50_private_message_registry::unique());
  static_assert(!std::is_invocable_r_v<
                int,
                decltype(&MsgChannel::release_fd_after_p50_client_adopted),
                MsgChannel *, P50ServerClaimReleaseTicket &&>);
  static_assert(!std::is_invocable_r_v<
                int,
                decltype(&MsgChannel::release_fd_after_p50_server_claim),
                MsgChannel *, P50ClientAdoptedReleaseTicket &&>);

  const auto claim = exact_claim();
  const auto wire = encode_cache_session_wire_claim(claim);
  CHECK(wire.size() > kP50CacheSessionEnvelopeBytes && wire[0] == 'P' &&
            wire[1] == '5' && wire[2] == 'C' && wire[3] == 'L' &&
            wire[4] == 0 && wire[5] == 1 && wire[6] == 0 && wire[7] == 1,
        "P5CL v1 CLAIM=1 canonical envelope");
  CHECK(decode_cache_session_wire_claim(wire) == claim,
        "canonical P5CL round trip");
  CHECK(canonical_cache_session_claim_prefix_size(wire) == wire.size(),
        "P5CL self-delimiting prefix size");
  CHECK(occurrence_count(wire, claim.attempt.selected_capability.bytes) == 1 &&
            occurrence_count(wire, capability(91).bytes) == 0,
        "one P5CL exposes only its selected 128-bit capability");

  for (size_t offset : {size_t{0}, size_t{5}, size_t{7}, size_t{11},
                        wire.size() - 20}) {
    auto changed = wire;
    changed[offset] ^= 1;
    CHECK(!decode_cache_session_wire_claim(changed),
          "every authoritative P5CL envelope/body mutation rejects");
  }
  auto changed_capability = wire;
  changed_capability[changed_capability.size() - 16] ^= 1;
  const auto changed_capability_claim =
      decode_cache_session_wire_claim(changed_capability);
  CHECK(changed_capability_claim.has_value() &&
            changed_capability_claim->attempt.selected_capability !=
                claim.attempt.selected_capability,
        "selected capability bytes are semantic canonical claim material");
  auto trailing = wire;
  trailing.push_back(0);
  CHECK(!decode_cache_session_wire_claim(trailing),
        "P5CL trailing byte rejects");
  for (size_t size = 0; size != wire.size(); ++size) {
    CHECK(!decode_cache_session_wire_claim(
              std::span<const uint8_t>(wire.data(), size)),
          "every truncated P5CL prefix rejects");
  }
  std::vector<uint8_t> oversized_claim(kP50CacheSessionClaimMaxWireBytes + 1,
                                       0);
  CHECK(!decode_cache_session_wire_claim(oversized_claim),
        "oversized P5CL rejects before allocation-driven parsing");
  auto embedded_nul_claim = wire;
  const auto host_begin =
      std::search(embedded_nul_claim.begin(), embedded_nul_claim.end(),
                  claim.binding.arm.selected_f_host.begin(),
                  claim.binding.arm.selected_f_host.end());
  CHECK(host_begin != embedded_nul_claim.end(),
        "fixture host exists in canonical P5CL");
  if (host_begin != embedded_nul_claim.end()) {
    *(host_begin + 1) = 0;
    CHECK(!decode_cache_session_wire_claim(embedded_nul_claim),
          "embedded NUL in bounded P5CL host rejects");
  }

  const auto adopted_value = adopted(wire);
  const auto adopted_wire = encode_cache_session_outcome(adopted_value);
  CHECK(adopted_wire.size() > wire.size() && adopted_wire[0] == 'P' &&
            adopted_wire[1] == '5' && adopted_wire[2] == 'C' &&
            adopted_wire[3] == 'O' && adopted_wire[6] == 0 &&
            adopted_wire[7] == 1,
        "P5CO v1 ADOPTED=1 canonical envelope");
  CHECK(decode_cache_session_outcome(adopted_wire) == adopted_value,
        "canonical ADOPTED round trip");
  auto adopted_trailing = adopted_wire;
  adopted_trailing.push_back(0);
  CHECK(!decode_cache_session_outcome(adopted_trailing),
        "P5CO trailing byte rejects");
  for (size_t size = 0; size != adopted_wire.size(); ++size) {
    CHECK(!decode_cache_session_outcome(
              std::span<const uint8_t>(adopted_wire.data(), size)),
          "every truncated P5CO prefix rejects");
  }
  std::vector<uint8_t> oversized_outcome(
      kP50CacheSessionOutcomeMaxWireBytes + 1, 0);
  CHECK(!decode_cache_session_outcome(oversized_outcome),
        "oversized P5CO rejects before embedded claim parsing");
  auto adopted_reserved = adopted_wire;
  adopted_reserved[adopted_reserved.size() - 10] = 1;
  CHECK(!decode_cache_session_outcome(adopted_reserved),
        "P5CO nonzero reserved word rejects");

  const auto refused_value = refused(wire);
  const auto refused_wire = encode_cache_session_outcome(refused_value);
  CHECK(refused_wire[6] == 0 && refused_wire[7] == 2 &&
            decode_cache_session_outcome(refused_wire) == refused_value,
        "canonical singular REFUSED_PRE_DETACH round trip");
  auto invalid_refusal = refused_value;
  invalid_refusal.refusal_reason =
      static_cast<P50CacheSessionRefusalReason>(0xffff);
  CHECK(encode_cache_session_outcome(invalid_refusal).empty(),
        "out-of-range refusal reason cannot encode");

  auto noncanonical_embedded_claim = adopted_wire;
  noncanonical_embedded_claim[kP50CacheSessionEnvelopeBytes +
                              wire.size() - 20] ^= 1;
  CHECK(!decode_cache_session_outcome(noncanonical_embedded_claim),
        "noncanonical embedded P5CL rejects outcome");
  auto wrong_role = adopted_value;
  wrong_role.operation.role = static_cast<P50SessionOperationRole>(1);
  CHECK(encode_cache_session_outcome(wrong_role).empty(),
        "wrong operation role cannot encode ADOPTED");
  auto duplicated_launch_mismatch = adopted_value;
  ++duplicated_launch_mismatch.operation.sidecar_launch.attempt;
  CHECK(encode_cache_session_outcome(duplicated_launch_mismatch).empty(),
        "operation launch must exactly equal sidecar launch");
  auto claim_launch_mismatch = adopted_value;
  ++claim_launch_mismatch.f_sidecar_launch.attempt;
  claim_launch_mismatch.operation.sidecar_launch =
      claim_launch_mismatch.f_sidecar_launch;
  CHECK(encode_cache_session_outcome(claim_launch_mismatch).empty(),
        "ADOPTED launch must equal the launch bound by echoed P5CL");
  auto claim_guid_mismatch = adopted_value;
  claim_guid_mismatch.f_store_guid[0] ^= 1;
  CHECK(encode_cache_session_outcome(claim_guid_mismatch).empty(),
        "ADOPTED F StoreIdentity must equal the GUID bound by echoed P5CL");
  auto refused_with_identity = refused_value;
  refused_with_identity.f_sidecar_launch = {1, 1};
  CHECK(encode_cache_session_outcome(refused_with_identity).empty(),
        "REFUSED has no fabricated sidecar identity");
  auto refused_with_partial_launch = refused_value;
  refused_with_partial_launch.f_sidecar_launch = {1, 0};
  CHECK(encode_cache_session_outcome(refused_with_partial_launch).empty(),
        "REFUSED rejects a partially populated invalid launch identity");
  auto refused_with_partial_operation = refused_value;
  refused_with_partial_operation.operation.sidecar_launch = {1, 0};
  CHECK(encode_cache_session_outcome(refused_with_partial_operation).empty(),
        "REFUSED rejects a partially populated invalid operation identity");
}

void ordinary_message_and_server_ticket() {
  const auto claim = exact_claim();
  const auto claim_wire = encode_cache_session_wire_claim(claim);
  Pair pair = make_pair();
  CHECK(pair.left->send_msg(P50CacheSessionClaimMsg(claim_wire)),
        "exact P50 claim ordinary frame sends");
  std::unique_ptr<Msg> decoded(pair.right->get_msg(2, true));
  auto *claim_message =
      dynamic_cast<P50CacheSessionClaimMsg *>(decoded.get());
  CHECK(claim_message != nullptr && claim_message->wire == claim_wire,
        "exact P50 claim ordinary frame decodes");
  auto stamp = pair.right->take_p50_decoded_claim_stamp();
  CHECK(stamp.valid() && !pair.right->take_p50_decoded_claim_stamp().valid(),
        "decoded claim stamp is unforgeable and one-shot");
  auto ticket = pair.right->issue_p50_server_claim_release_ticket(
      std::move(stamp), 1001, claim.attempt.selected_capability);
  CHECK(ticket.valid() && ticket.reservation_id() == 1001,
        "F owner combines exact decoded stamp with winning reservation");
  const auto outcome_wire =
      encode_cache_session_outcome(adopted(claim_wire, 9001));
  CHECK(pair.right->send_p50_cache_session_outcome(
            ticket, P50CacheSessionOutcomeMsg(outcome_wire)) &&
            ticket.valid(),
        "exact ADOPTED P5CO barrier fully flushes before server release");
  const int released =
      pair.right->release_fd_after_p50_server_claim(std::move(ticket));
  CHECK(released >= 0 && pair.right->fd == -1,
        "server ticket releases exact descriptor once");
  const std::array<uint8_t, 3> continuity{'C', 'W', 1};
  send_raw(pair.left->fd, continuity);
  std::array<uint8_t, 3> received{};
  CHECK(recv(released, received.data(), received.size(), 0) ==
                static_cast<ssize_t>(received.size()) &&
            received == continuity,
        "server-released descriptor continuity survives parser destruction");
  close(released);
}

void server_ticket_stale_and_dirty_rows() {
  const auto claim = exact_claim();
  const auto claim_wire = encode_cache_session_wire_claim(claim);
  {
    Pair pair = make_pair();
    CHECK(pair.left->send_msg(P50CacheSessionClaimMsg(claim_wire)),
          "zero-reservation claim sends");
    std::unique_ptr<Msg> decoded(pair.right->get_msg(2, true));
    auto stamp = pair.right->take_p50_decoded_claim_stamp();
    auto ticket = pair.right->issue_p50_server_claim_release_ticket(
        std::move(stamp), 0, claim.attempt.selected_capability);
    CHECK(!ticket.valid() && pair.right->fd >= 0,
          "zero reservation cannot mint a server release ticket");
  }
  {
    Pair pair = make_pair();
    CHECK(pair.left->send_msg(P50CacheSessionClaimMsg(claim_wire)),
          "wrong-capability claim sends");
    std::unique_ptr<Msg> decoded(pair.right->get_msg(2, true));
    auto stamp = pair.right->take_p50_decoded_claim_stamp();
    auto ticket = pair.right->issue_p50_server_claim_release_ticket(
        std::move(stamp), 1004, capability(0xe1));
    CHECK(!ticket.valid() && pair.right->fd >= 0,
          "wrong selected capability cannot mint a server release ticket");
  }
  {
    Pair pair = make_pair();
    CHECK(pair.left->send_msg(P50CacheSessionClaimMsg(claim_wire)),
          "pre-barrier release row sends claim");
    auto ticket = reserve_server_claim(*pair.right, claim, 1005);
    const int owned = pair.right->fd;
    CHECK(ticket.valid() &&
              pair.right->release_fd_after_p50_server_claim(
                  std::move(ticket)) == -1 &&
              pair.right->fd == owned,
          "server reservation ticket cannot release before P5CO flush");
  }
  {
    Pair pair = make_pair();
    CHECK(pair.left->send_msg(P50CacheSessionClaimMsg(claim_wire)),
          "stale-ticket claim sends");
    std::unique_ptr<Msg> decoded(pair.right->get_msg(2, true));
    auto stamp = pair.right->take_p50_decoded_claim_stamp();
    auto ticket = pair.right->issue_p50_server_claim_release_ticket(
        std::move(stamp), 1002, claim.attempt.selected_capability);
    CHECK(ticket.valid() && !pair.right->send_msg(PingMsg()),
          "server refuses an ordinary frame between P5CL and P5CO");
    const int owned = pair.right->fd;
    CHECK(pair.right->release_fd_after_p50_server_claim(std::move(ticket)) ==
                  -1 &&
              pair.right->fd == owned,
          "intervening send invalidates stale server ticket");
  }
  {
    Pair pair = make_pair();
    CHECK(pair.left->send_msg(P50CacheSessionClaimMsg(claim_wire)),
          "server wrong-echo row sends claim");
    auto ticket = reserve_server_claim(*pair.right, claim, 1006);
    auto other_claim = claim;
    ++other_claim.binding.arm.source_request_id;
    const auto other_wire = encode_cache_session_wire_claim(other_claim);
    CHECK(ticket.valid() &&
              !pair.right->send_p50_cache_session_outcome(
                  ticket, P50CacheSessionOutcomeMsg(
                              encode_cache_session_outcome(
                                  adopted(other_wire, 9006)))) &&
              !ticket.valid(),
          "server barrier rejects a different canonical P5CL echo");
  }
  {
    Pair pair = make_pair();
    CHECK(pair.left->send_msg(P50CacheSessionClaimMsg(claim_wire)),
          "dirty-boundary claim sends");
    std::unique_ptr<Msg> decoded(pair.right->get_msg(2, true));
    auto stamp = pair.right->take_p50_decoded_claim_stamp();
    auto ticket = pair.right->issue_p50_server_claim_release_ticket(
        std::move(stamp), 1003, claim.attempt.selected_capability);
    CHECK(pair.right->send_p50_cache_session_outcome(
              ticket, P50CacheSessionOutcomeMsg(
                          encode_cache_session_outcome(
                              adopted(claim_wire, 9003)))),
          "dirty-boundary row first flushes the exact outcome barrier");
    const uint8_t early = 0x43;
    send_raw(pair.left->fd, std::span<const uint8_t>(&early, 1));
    const int owned = pair.right->fd;
    CHECK(pair.right->release_fd_after_p50_server_claim(std::move(ticket)) ==
                  -1 &&
              pair.right->fd == owned,
          "kernel-queued byte refuses server release without consuming it");
  }
  {
    Pair pair = make_pair();
    CHECK(pair.left->send_msg(CacheSessionMsg()),
          "legacy empty discriminator remains decodable fixture");
    std::unique_ptr<Msg> decoded(pair.right->get_msg(2, true));
    CHECK(decoded && *decoded == Msg::CACHE_SESSION &&
              !pair.right->take_p50_decoded_claim_stamp().valid() &&
              !pair.right->take_p50_decoded_outcome_stamp().valid(),
          "empty CACHE_SESSION creates zero positive claim/outcome stamp");
  }
}

void client_claim_singularity_rows() {
  const auto claim_wire =
      encode_cache_session_wire_claim(exact_claim());
  {
    Pair pair = make_pair();
    CHECK(pair.left->send_msg(P50CacheSessionClaimMsg(claim_wire),
                              MsgChannel::SendBulkOnly) &&
              pair.left->has_pending_write(),
          "bulk-only P5CL remains queued for singularity test");
    CHECK(!pair.left->send_msg(PingMsg()),
          "queued P5CL refuses a second ordinary frame");
  }
  {
    Pair pair = make_pair();
    CHECK(pair.left->send_msg(P50CacheSessionClaimMsg(claim_wire)),
          "fully flushed P5CL arms singularity test");
    CHECK(!pair.left->send_msg(PingMsg()),
          "flushed P5CL awaiting P5CO refuses a second ordinary frame");
  }
}

void client_adopted_ticket() {
  const auto claim = exact_claim();
  const auto claim_wire = encode_cache_session_wire_claim(claim);
  const uint64_t operation_sequence = 9101;
  const auto outcome = adopted(claim_wire, operation_sequence);
  const auto outcome_wire = encode_cache_session_outcome(outcome);
  Pair pair = make_pair();
  CHECK(pair.left->send_msg(P50CacheSessionClaimMsg(claim_wire)),
        "client sends one fresh canonical claim");
  auto server_ticket = reserve_server_claim(*pair.right, claim, 2001);
  CHECK(server_ticket.valid() &&
            pair.right->send_p50_cache_session_outcome(
                server_ticket, P50CacheSessionOutcomeMsg(outcome_wire)),
        "test peer returns one exact canonical ADOPTED outcome");
  std::unique_ptr<Msg> client_outcome(pair.left->get_msg(2, true));
  CHECK(client_outcome &&
            *client_outcome == Msg::P50_CACHE_SESSION_OUTCOME,
        "client decodes first complete outcome on same connection");
  auto stamp = pair.left->take_p50_decoded_outcome_stamp();
  auto ticket = pair.left->issue_p50_client_adopted_release_ticket(
      std::move(stamp), outcome.f_sidecar_launch.generation,
      outcome.f_sidecar_launch.attempt, outcome.f_store_guid,
      operation_sequence);
  CHECK(ticket.valid(),
        "exact ADOPTED plus retained outbound P5CL mints client ticket");
  const int released =
      pair.left->release_fd_after_p50_client_adopted(std::move(ticket));
  CHECK(released >= 0 && pair.left->fd == -1,
        "client ADOPTED ticket releases the opposite descriptor direction");
  const std::array<uint8_t, 3> continuity{'S', 'H', 1};
  send_raw(pair.right->fd, continuity);
  std::array<uint8_t, 3> received{};
  CHECK(recv(released, received.data(), received.size(), 0) ==
                static_cast<ssize_t>(received.size()) &&
            received == continuity,
        "client-released descriptor begins byte-silent CacheWire boundary");
  close(released);
}

void client_refused_and_wrong_identity_rows() {
  const auto claim = exact_claim();
  const auto claim_wire = encode_cache_session_wire_claim(claim);
  {
    Pair pair = make_pair();
    CHECK(pair.left->send_msg(P50CacheSessionClaimMsg(claim_wire)),
          "refusal row sends claim");
    auto server_ticket = reserve_server_claim(*pair.right, claim, 2002);
    CHECK(server_ticket.valid() &&
              pair.right->send_p50_cache_session_outcome(
                  server_ticket, P50CacheSessionOutcomeMsg(
                                     encode_cache_session_outcome(
                                         refused(claim_wire)))) &&
              !server_ticket.valid(),
          "F daemon emits singular REFUSED_PRE_DETACH");
    std::unique_ptr<Msg> client_outcome(pair.left->get_msg(2, true));
    auto stamp = pair.left->take_p50_decoded_outcome_stamp();
    auto ticket = pair.left->issue_p50_client_adopted_release_ticket(
        std::move(stamp), claim.binding.f_control_generation,
        claim.binding.f_control_attempt, claim.binding.f_store_guid, 91);
    CHECK(!ticket.valid() && pair.left->fd >= 0,
          "REFUSED never creates a client descriptor-release ticket");
  }
  {
    Pair pair = make_pair();
    const auto outcome = adopted(claim_wire, 92);
    CHECK(pair.left->send_msg(P50CacheSessionClaimMsg(claim_wire)),
          "wrong-identity row sends claim");
    auto server_ticket = reserve_server_claim(*pair.right, claim, 2003);
    CHECK(server_ticket.valid() &&
              pair.right->send_p50_cache_session_outcome(
                  server_ticket, P50CacheSessionOutcomeMsg(
                                     encode_cache_session_outcome(outcome))),
          "wrong-identity row sends valid outcome");
    std::unique_ptr<Msg> client_outcome(pair.left->get_msg(2, true));
    auto stamp = pair.left->take_p50_decoded_outcome_stamp();
    auto ticket = pair.left->issue_p50_client_adopted_release_ticket(
        std::move(stamp), outcome.f_sidecar_launch.generation,
        outcome.f_sidecar_launch.attempt + 1, outcome.f_store_guid, 92);
    CHECK(!ticket.valid() && pair.left->fd >= 0,
          "wrong accepted-F identity refuses client release");
  }
  {
    Pair pair = make_pair();
    auto other_claim = claim;
    ++other_claim.binding.arm.source_request_id;
    const auto other_claim_wire =
        encode_cache_session_wire_claim(other_claim);
    const auto other_outcome = adopted(other_claim_wire, 93);
    CHECK(pair.left->send_msg(P50CacheSessionClaimMsg(claim_wire)),
          "wrong-echo row sends original claim");
    std::unique_ptr<Msg> server_claim(pair.right->get_msg(2, true));
    send_raw_ordinary(pair.right->fd, Msg::P50_CACHE_SESSION_OUTCOME,
                      encode_cache_session_outcome(other_outcome));
    CHECK(server_claim && *server_claim == Msg::P50_CACHE_SESSION_CLAIM,
          "hostile raw peer sends a different canonical claim echo");
    std::unique_ptr<Msg> client_outcome(pair.left->get_msg(2, true));
    auto stamp = pair.left->take_p50_decoded_outcome_stamp();
    auto ticket = pair.left->issue_p50_client_adopted_release_ticket(
        std::move(stamp), claim.binding.f_control_generation,
        claim.binding.f_control_attempt, claim.binding.f_store_guid, 93);
    CHECK(!ticket.valid() && pair.left->fd >= 0,
          "different but canonical P5CL echo cannot release the client fd");
  }
  {
    Pair pair = make_pair();
    const auto outcome = adopted(claim_wire, 95);
    CHECK(pair.left->send_msg(P50CacheSessionClaimMsg(claim_wire)),
          "wrong-operation row sends claim");
    auto server_ticket = reserve_server_claim(*pair.right, claim, 2005);
    CHECK(server_ticket.valid() &&
              pair.right->send_p50_cache_session_outcome(
                  server_ticket, P50CacheSessionOutcomeMsg(
                                     encode_cache_session_outcome(outcome))),
          "wrong-operation row sends valid outcome");
    std::unique_ptr<Msg> client_outcome(pair.left->get_msg(2, true));
    auto stamp = pair.left->take_p50_decoded_outcome_stamp();
    auto ticket = pair.left->issue_p50_client_adopted_release_ticket(
        std::move(stamp), outcome.f_sidecar_launch.generation,
        outcome.f_sidecar_launch.attempt, outcome.f_store_guid, 96);
    CHECK(!ticket.valid() && pair.left->fd >= 0,
          "wrong F-session operation sequence refuses client release");
  }
  {
    Pair pair = make_pair();
    const auto outcome = adopted(claim_wire, 96);
    CHECK(pair.left->send_msg(P50CacheSessionClaimMsg(claim_wire)),
          "client dirty-boundary row sends claim");
    auto server_ticket = reserve_server_claim(*pair.right, claim, 2006);
    CHECK(server_ticket.valid() &&
              pair.right->send_p50_cache_session_outcome(
                  server_ticket, P50CacheSessionOutcomeMsg(
                                     encode_cache_session_outcome(outcome))),
          "client dirty-boundary row sends exact outcome");
    std::unique_ptr<Msg> client_outcome(pair.left->get_msg(2, true));
    auto stamp = pair.left->take_p50_decoded_outcome_stamp();
    auto ticket = pair.left->issue_p50_client_adopted_release_ticket(
        std::move(stamp), outcome.f_sidecar_launch.generation,
        outcome.f_sidecar_launch.attempt, outcome.f_store_guid, 96);
    const uint8_t early = 0x53;
    send_raw(pair.right->fd, std::span<const uint8_t>(&early, 1));
    const int owned = pair.left->fd;
    CHECK(ticket.valid() &&
              pair.left->release_fd_after_p50_client_adopted(
                  std::move(ticket)) == -1 &&
              pair.left->fd == owned,
          "kernel-queued byte refuses client ADOPTED release");
  }
  {
    Pair pair = make_pair();
    const auto outcome = adopted(claim_wire, 94);
    CHECK(pair.left->send_msg(P50CacheSessionClaimMsg(claim_wire)),
          "client stale-ticket row sends claim");
    auto server_ticket = reserve_server_claim(*pair.right, claim, 2004);
    CHECK(server_ticket.valid() &&
              pair.right->send_p50_cache_session_outcome(
                  server_ticket, P50CacheSessionOutcomeMsg(
                                     encode_cache_session_outcome(outcome))),
          "client stale-ticket row sends outcome");
    std::unique_ptr<Msg> client_outcome(pair.left->get_msg(2, true));
    auto stamp = pair.left->take_p50_decoded_outcome_stamp();
    auto ticket = pair.left->issue_p50_client_adopted_release_ticket(
        std::move(stamp), outcome.f_sidecar_launch.generation,
        outcome.f_sidecar_launch.attempt, outcome.f_store_guid, 94);
    CHECK(ticket.valid() && !pair.left->send_msg(PingMsg()),
          "client refuses an ordinary send after ADOPTED ticket mint");
    const int owned = pair.left->fd;
    CHECK(pair.left->release_fd_after_p50_client_adopted(
              std::move(ticket)) == -1 &&
              pair.left->fd == owned,
          "intervening client send invalidates ADOPTED release ticket");
  }
}

void exact_protocol_gate() {
  const auto wire = encode_cache_session_wire_claim(exact_claim());
  const auto outcome_wire =
      encode_cache_session_outcome(adopted(wire, 501));
  const P50CacheSessionClaimMsg claim_message(wire);
  const P50CacheSessionOutcomeMsg outcome_message(outcome_wire);
  CHECK(claim_message.valid_for_protocol(50) &&
            !claim_message.valid_for_protocol(49) &&
            !claim_message.valid_for_protocol(51),
        "claim message is valid for exactly Protocol 50");
  CHECK(outcome_message.valid_for_protocol(50) &&
            !outcome_message.valid_for_protocol(49) &&
            !outcome_message.valid_for_protocol(51),
        "outcome message is valid for exactly Protocol 50");
  {
    Pair pair = make_pair();
    const int owned = pair.left->fd;
    CHECK(!pair.left->send_msg(outcome_message) && pair.left->fd == owned,
          "unissued P5CO cannot send on a clean Protocol-50 channel");
  }
  for (int protocol : {49, 51}) {
    Pair pair = make_pair(protocol);
    const int owned = pair.left->fd;
    CHECK(!pair.left->send_msg(P50CacheSessionClaimMsg(wire)) &&
              pair.left->fd == owned && pair.left->send_msg(PingMsg()),
          "claim refuses every protocol except exactly 50 before framing");
    Pair outcome_pair = make_pair(protocol);
    const int outcome_owned = outcome_pair.left->fd;
    CHECK(!outcome_pair.left->send_msg(
              P50CacheSessionOutcomeMsg(outcome_wire)) &&
              outcome_pair.left->fd == outcome_owned &&
              outcome_pair.left->send_msg(PingMsg()),
          "outcome refuses every protocol except exactly 50 before framing");
  }
}

} // namespace

int main() {
  capability_entropy_matrix();
  codec_matrix();
  ordinary_message_and_server_ticket();
  server_ticket_stale_and_dirty_rows();
  client_claim_singularity_rows();
  client_adopted_ticket();
  client_refused_and_wrong_identity_rows();
  exact_protocol_gate();
  if (failures != 0)
    std::fprintf(stderr, "%d P50 claim/outcome failures\n", failures);
  return failures == 0 ? 0 : 1;
}
