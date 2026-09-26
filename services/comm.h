/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
    This file is part of Icecream.

    Copyright (c) 2004 Michael Matz <matz@suse.de>
                  2004 Stephan Kulow <coolo@suse.de>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/


#ifndef ICECREAM_COMM_H
#define ICECREAM_COMM_H

#ifdef __linux__
#  include <stdint.h>
#endif
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "job.h"
#include "p50_store_identity_wire.h"
#include <chrono>
#include <array>
#include <compare>
#include <cstdlib>
#include <deque>
#include <optional>
#include <span>
#include <stdint.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// if you increase the PROTOCOL_VERSION, add a macro below and use that
#define PROTOCOL_VERSION 50
// if you increase the MIN_PROTOCOL_VERSION, comment out macros below and clean up the code
#define MIN_PROTOCOL_VERSION 21
#define PROTOCOL_VERSION_JOB_TIMING 47
#define PROTOCOL_VERSION_JOB_LOCAL_FLAGS 48
#define PROTOCOL_VERSION_ASSIGNMENT_FENCE 49
#define PROTOCOL_VERSION_ASSIGNMENT_IDENTITY 50
#define PROTOCOL_VERSION_RESULT_DISPOSITION 50
/* Deliberately shares 50 with PROTOCOL_VERSION_ASSIGNMENT_IDENTITY: owner
   ruling on the d23d9c5d HOLD holds that protocol 50 is an in-development
   draft with no deployed base (43 is the deployed floor; the owner-
   selected final S2 candidate must still converge with the approved S1
   head 0496f50b before either lands -- no parallel endpoint owner, no
   lost deletion gates) -- there is no intra-50 compatibility obligation
   between draft builds, so this tail does not need, and does not get, its
   own version number the way 47/48/49/50 each did for genuinely deployed-
   and-superseded features.  Both LoginMsg's Login-only tail and UseCSMsg's
   S2 assignment-bound cache-handoff tail are MANDATORY (exactly three
   words, never omitted) at this one gate on both hops -- see
   UseCSMsg::fill_from_channel and valid_payload; Login's own decode was
   already strict this way and needed no change. */
#define PROTOCOL_VERSION_CACHE_ADVERTISEMENT 50

#define MAX_SCHEDULER_PONG 3
// MAX_SCHEDULER_PING must be multiple of MAX_SCHEDULER_PONG
#define MAX_SCHEDULER_PING 12 * MAX_SCHEDULER_PONG
// maximum amount of time in seconds a daemon can be busy installing
#define MAX_BUSY_INSTALLING 120

// comparison for protocol version checks
#define IS_PROTOCOL_VERSION(x, c) ((c)->protocol >= (x))

class MsgChannel;
class P50CacheSessionOutcomeMsg;
class P50CacheSessionFdRequestMsg;

enum class P50LegacyWireRole : uint8_t { C, F };

struct P50LegacyWireIdentity {
    uint32_t job_id = 0;
    uint64_t assignment_epoch = 0;
    uint64_t assignment_nonce = 0;
    uint64_t c_guid = 0;
    uint64_t tu_seq = 0;

    [[nodiscard]] bool valid() const noexcept
    {
        const bool assignment_absent = assignment_epoch == 0 &&
                                       assignment_nonce == 0;
        const bool assignment_complete = assignment_epoch != 0 &&
                                         assignment_nonce != 0;
        return job_id != 0 && c_guid != 0 &&
               (assignment_absent || assignment_complete);
    }

    auto operator<=>(const P50LegacyWireIdentity &) const = default;
};

// Terms used:
// S  = scheduler
// C  = client
// CS = daemon

class Msg {
public:
    enum Value: uint32_t {
        // so far unknown
        UNKNOWN = 'A',

        /* When the scheduler didn't get STATS from a CS
           for a specified time (e.g. 10m), then he sends a
           ping */
        PING,

        /* Either the end of file chunks or connection (A<->A) */
        END,

        TIMEOUT, // unused

        // C --> CS
        GET_NATIVE_ENV,
        // CS -> C
        NATIVE_ENV,

        // C --> S
        GET_CS,
        // S --> C
        USE_CS,  // = 'H'
        // C --> CS
        COMPILE_FILE, // = 'I'
        // generic file transfer
        FILE_CHUNK,
        // CS --> C
        COMPILE_RESULT,

        // CS --> S (after the C got the CS from the S, the CS tells the S when the C asks him)
        JOB_BEGIN,
        JOB_DONE,     // = 'M'

        // C --> CS, CS --> S (forwarded from C), _and_ CS -> C as start ping
        JOB_LOCAL_BEGIN, // = 'N'
        JOB_LOCAL_DONE,

        // CS --> S, first message sent
        LOGIN,
        // CS --> S (periodic)
        STATS,

        // messages between monitor and scheduler
        MON_LOGIN,
        MON_GET_CS,
        MON_JOB_BEGIN, // = 'T'
        MON_JOB_DONE,
        MON_LOCAL_JOB_BEGIN,
        MON_STATS,

        TRANFER_ENV, // = 'X'

        TEXT,
        STATUS_TEXT, // = 'Z'
        GET_INTERNALS,

        // S --> CS, answered by LOGIN
        CS_CONF,

        // C --> CS, after installing an environment
        VERIFY_ENV,
        // CS --> C
        VERIFY_ENV_RESULT,
        // C --> CS, CS --> S (forwarded from C), to not use given host for given environment
        BLACKLIST_HOST_ENV,
        // S --> CS
        NO_CS,
        // C --> CS
        JOB_TIMING,

        // Protocol 49 fork-private block, on the persistent scheduler <->
        // worker link only.  Values are explicit so a future upstream append
        // cannot silently alias this vocabulary.
        // S --> CS: install one assignment before it can be exposed to C.
        ASSIGN_PREPARE = 0x49f00000,
        // CS --> S: the matching assignment is installed.
        ASSIGN_READY = 0x49f00001,
        // S --> CS: withdraw an assignment that has not been claimed.
        REVOKE_BEFORE_START = 0x49f00002,
        // CS --> S: the ordered claim/revoke outcome.
        REVOKE_RESULT = 0x49f00003,

        // Protocol-50-private ordinary-link discriminator.  This value is
        // deliberately outside both the historical ASCII vocabulary and the
        // Protocol-49 private block; it is never meaningful below exactly
        // Protocol 50.
        CACHE_SESSION = 0x50f00000,

        // Protocol-50-only result disposition.  0x50f00001 and 0x50f00003 are
        // reserved by CACHE_SESSION_READY_MAGIC and CACHE_SESSION_BUSY_MAGIC
        // (raw handoff witnesses, not Msgs), so this ordinary framed message
        // deliberately uses the value between them.
        RESULT_DISPOSITION = 0x50f00002,

        // Protocol-50 source-arm phase.  These are ordinary framed messages;
        // they are distinct from the empty CACHE_SESSION discriminator and
        // carry all assignment/source/control-launch authority explicitly.
        P50_SOURCE_ARM = 0x50f00010,
        P50_SOURCE_ARMED = 0x50f00011,

        // Positive cache transition.  The claim payload is exactly one
        // canonical bounded P5CL value; the outcome payload is exactly one
        // canonical bounded P5CO value.  The old empty CACHE_SESSION value
        // above remains a negative/mechanism fixture and never authorizes
        // this transition.
        P50_CACHE_SESSION_CLAIM = 0x50f00012,
        P50_CACHE_SESSION_OUTCOME = 0x50f00013,

        // Protocol-50 private ordinary request for the already-authenticated
        // C-cache control descriptor.  The raw SCM_RIGHTS reply is a separate
        // clean-boundary exchange and is never interpreted as a Msg.
        P50_CACHE_SESSION_FD_REQUEST = 0x50f00014
    };

    Msg() = default;
    constexpr Msg(Value value)
        : value_{value}
    {}

    constexpr operator Value() const { return value_; }
    explicit operator bool() = delete;

    /* Payload invariants which cannot be expressed by the frame length alone.
       The default keeps all historical message classes unchanged. */
    virtual bool valid_payload() const { return true; }

    /* Message-specific protocol admission.  Most historical messages are
       valid on every negotiated ordinary link; draft/private messages can
       narrow that rule without allowing send_msg() to compose a frame first. */
    virtual bool valid_for_protocol(int negotiated_protocol) const
    {
        (void)negotiated_protocol;
        return true;
    }

    std::basic_string<char> to_string() const {
        switch (value_) {
            case UNKNOWN:
                return "UNKNOWN";
            case PING:
                return "PING";
            case END:
                return "END";
            case TIMEOUT:
                return "TIMEOUT";
            case GET_NATIVE_ENV:
                return "GET_NATIVE_ENV";
            case NATIVE_ENV:
                return "NATIVE_ENV";
            case GET_CS:
                return "GET_CS";
            case USE_CS:
                return "USE_CS";
            case COMPILE_FILE:
                return "COMPILE_FILE";
            case FILE_CHUNK:
                return "FILE_CHUNK";
            case COMPILE_RESULT:
                return "COMPILE_RESULT";
            case JOB_BEGIN:
                return "JOB_BEGIN";
            case JOB_DONE:
                return "JOB_DONE";
            case JOB_LOCAL_BEGIN:
                return "JOB_LOCAL_BEGIN";
            case JOB_LOCAL_DONE:
                return "JOB_LOCAL_DONE";
            case LOGIN:
                return "LOGIN";
            case STATS:
                return "STATS";
            case MON_LOGIN:
                return "MON_LOGIN";
            case MON_GET_CS:
                return "MON_GET_CS";
            case MON_JOB_BEGIN:
                return "MON_JOB_BEGIN";
            case MON_JOB_DONE:
                return "MON_JOB_DONE";
            case MON_LOCAL_JOB_BEGIN:
                return "MON_LOCAL_JOB_BEGIN";
            case MON_STATS:
                return "MON_STATS";
            case TRANFER_ENV:
                return "TRANFER_ENV";
            case TEXT:
                return "TEXT";
            case STATUS_TEXT:
                return "STATUS_TEXT";
            case GET_INTERNALS:
                return "GET_INTERNALS";
            case CS_CONF:
                return "CS_CONF";
            case VERIFY_ENV:
                return "VERIFY_ENV";
            case VERIFY_ENV_RESULT:
                return "VERIFY_ENV_RESULT";
            case BLACKLIST_HOST_ENV:
                return "BLACKLIST_HOST_ENV";
            case NO_CS:
                return "NO_CS";
            case JOB_TIMING:
                return "JOB_TIMING";
            case ASSIGN_PREPARE:
                return "ASSIGN_PREPARE";
            case ASSIGN_READY:
                return "ASSIGN_READY";
            case REVOKE_BEFORE_START:
                return "REVOKE_BEFORE_START";
            case REVOKE_RESULT:
                return "REVOKE_RESULT";
            case CACHE_SESSION:
                return "CACHE_SESSION";
            case RESULT_DISPOSITION:
                return "RESULT_DISPOSITION";
            case P50_SOURCE_ARM:
                return "P50_SOURCE_ARM";
            case P50_SOURCE_ARMED:
                return "P50_SOURCE_ARMED";
            case P50_CACHE_SESSION_CLAIM:
                return "P50_CACHE_SESSION_CLAIM";
            case P50_CACHE_SESSION_OUTCOME:
                return "P50_CACHE_SESSION_OUTCOME";
            case P50_CACHE_SESSION_FD_REQUEST:
                return "P50_CACHE_SESSION_FD_REQUEST";
        }
        return "UNKNOWN";
    }

    virtual ~Msg() {}
    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

protected:
    Value value_;
};

namespace p50_private_message_registry {
inline constexpr std::array<uint32_t, 7> values{
    Msg::CACHE_SESSION, Msg::RESULT_DISPOSITION, Msg::P50_SOURCE_ARM,
    Msg::P50_SOURCE_ARMED, Msg::P50_CACHE_SESSION_CLAIM,
    Msg::P50_CACHE_SESSION_OUTCOME, Msg::P50_CACHE_SESSION_FD_REQUEST};
inline constexpr bool unique() noexcept
{
    for (size_t i = 0; i != values.size(); ++i) {
        for (size_t j = i + 1; j != values.size(); ++j) {
            if (values[i] == values[j])
                return false;
        }
    }
    return true;
}
static_assert(unique(), "Protocol-50 private ordinary message collision");
} // namespace p50_private_message_registry

/* Opaque one-attempt bearer material created by F from the operating-system
   CSPRNG.  This is deliberately a distinct wire type: it is neither cache
   identity nor a digest, and production diagnostics must never render its
   bytes. */
struct ClaimAttemptCapability128
{
    std::array<uint8_t, 16> bytes{};

    [[nodiscard]] bool valid() const noexcept
    {
        for (const uint8_t byte : bytes) {
            if (byte != 0)
                return true;
        }
        return false;
    }
    auto operator<=>(const ClaimAttemptCapability128 &) const = default;
};

using ClaimAttemptEntropyProvider =
    ssize_t (*)(void *, size_t, unsigned) noexcept;

/* Bounded all-or-nothing capability generation. Short reads, entropy errors,
   persistent zero output, or persistent equality fail closed. */
bool fresh_claim_attempt_capabilities_with_provider(
    ClaimAttemptCapability128 &capability_1,
    ClaimAttemptCapability128 &capability_2,
    ClaimAttemptEntropyProvider provider) noexcept;
bool fresh_claim_attempt_capabilities(
    ClaimAttemptCapability128 &capability_1,
    ClaimAttemptCapability128 &capability_2) noexcept;

/* The decoder may expose a move-only stamp for the immediately preceding
   canonical frame.  A stamp is not descriptor-release authority: only the
   daemon owner can combine a claim stamp with a successful WAIT reservation,
   and only exact ADOPTED validation can combine an outcome stamp with the
   client's retained outbound claim. */
class P50DecodedClaimStamp
{
public:
    P50DecodedClaimStamp() = default;
    P50DecodedClaimStamp(const P50DecodedClaimStamp &) = delete;
    P50DecodedClaimStamp &operator=(const P50DecodedClaimStamp &) = delete;
    P50DecodedClaimStamp(P50DecodedClaimStamp &&other) noexcept;
    P50DecodedClaimStamp &operator=(P50DecodedClaimStamp &&other) noexcept;

    [[nodiscard]] bool valid() const noexcept { return stamp_nonce_ != 0; }
    [[nodiscard]] std::span<const uint8_t> canonical_claim() const noexcept {
        return canonical_wire_;
    }

private:
    friend class MsgChannel;
    P50DecodedClaimStamp(uint64_t channel_generation, uint64_t mutation_epoch,
                         uint64_t frame_sequence, uint64_t stamp_nonce,
                         std::vector<uint8_t> canonical_wire) noexcept;
    void invalidate() noexcept;

    uint64_t channel_generation_ = 0;
    uint64_t mutation_epoch_ = 0;
    uint64_t frame_sequence_ = 0;
    uint64_t stamp_nonce_ = 0;
    std::vector<uint8_t> canonical_wire_;
};

class P50DecodedOutcomeStamp
{
public:
    P50DecodedOutcomeStamp() = default;
    P50DecodedOutcomeStamp(const P50DecodedOutcomeStamp &) = delete;
    P50DecodedOutcomeStamp &operator=(const P50DecodedOutcomeStamp &) = delete;
    P50DecodedOutcomeStamp(P50DecodedOutcomeStamp &&other) noexcept;
    P50DecodedOutcomeStamp &operator=(P50DecodedOutcomeStamp &&other) noexcept;

    [[nodiscard]] bool valid() const noexcept { return stamp_nonce_ != 0; }
    [[nodiscard]] std::span<const uint8_t> canonical_outcome() const noexcept {
        return canonical_wire_;
    }

private:
    friend class MsgChannel;
    P50DecodedOutcomeStamp(uint64_t channel_generation,
                           uint64_t mutation_epoch, uint64_t frame_sequence,
                           uint64_t stamp_nonce,
                           std::vector<uint8_t> canonical_wire) noexcept;
    void invalidate() noexcept;

    uint64_t channel_generation_ = 0;
    uint64_t mutation_epoch_ = 0;
    uint64_t frame_sequence_ = 0;
    uint64_t stamp_nonce_ = 0;
    std::vector<uint8_t> canonical_wire_;
};

/* Direction-safe F/server authority.  It exists only after canonical claim
   decode plus an owner-atomic Armed -> Reserved transition. */
class P50ServerClaimReleaseTicket
{
public:
    P50ServerClaimReleaseTicket() = default;
    P50ServerClaimReleaseTicket(const P50ServerClaimReleaseTicket &) = delete;
    P50ServerClaimReleaseTicket &
    operator=(const P50ServerClaimReleaseTicket &) = delete;
    P50ServerClaimReleaseTicket(P50ServerClaimReleaseTicket &&other) noexcept;
    P50ServerClaimReleaseTicket &
    operator=(P50ServerClaimReleaseTicket &&other) noexcept;

    [[nodiscard]] bool valid() const noexcept { return release_nonce_ != 0; }
    [[nodiscard]] uint64_t reservation_id() const noexcept {
        return reservation_id_;
    }
    [[nodiscard]] std::span<const uint8_t> canonical_claim() const noexcept {
        return canonical_claim_;
    }

private:
    friend class MsgChannel;
    P50ServerClaimReleaseTicket(uint64_t channel_generation,
                                uint64_t mutation_epoch,
                                uint64_t decoded_frame_sequence,
                                uint64_t reservation_id,
                                ClaimAttemptCapability128 attempt_capability,
                                uint64_t stamp_nonce,
                                uint64_t release_nonce,
                                std::vector<uint8_t> canonical_claim) noexcept;
    void invalidate() noexcept;

    uint64_t channel_generation_ = 0;
    uint64_t mutation_epoch_ = 0;
    uint64_t decoded_frame_sequence_ = 0;
    uint64_t reservation_id_ = 0;
    ClaimAttemptCapability128 attempt_capability_{};
    uint64_t stamp_nonce_ = 0;
    uint64_t release_nonce_ = 0;
    // Zero until the exact echoed ADOPTED P5CO frame has fully flushed.
    uint64_t outcome_frame_sequence_ = 0;
    std::vector<uint8_t> canonical_claim_;
};

/* Direction-safe C/client authority. REFUSED, malformed, EOF, timeout, wrong
   echo/identity/role, or a stale operation can never construct this type. */
class P50ClientAdoptedReleaseTicket
{
public:
    P50ClientAdoptedReleaseTicket() = default;
    P50ClientAdoptedReleaseTicket(
        const P50ClientAdoptedReleaseTicket &) = delete;
    P50ClientAdoptedReleaseTicket &
    operator=(const P50ClientAdoptedReleaseTicket &) = delete;
    P50ClientAdoptedReleaseTicket(
        P50ClientAdoptedReleaseTicket &&other) noexcept;
    P50ClientAdoptedReleaseTicket &
    operator=(P50ClientAdoptedReleaseTicket &&other) noexcept;

    [[nodiscard]] bool valid() const noexcept { return release_nonce_ != 0; }
    [[nodiscard]] std::span<const uint8_t> canonical_claim() const noexcept {
        return canonical_claim_;
    }

private:
    friend class MsgChannel;
    P50ClientAdoptedReleaseTicket(
        uint64_t channel_generation, uint64_t mutation_epoch,
        uint64_t decoded_frame_sequence,
        ClaimAttemptCapability128 attempt_capability,
        uint64_t stamp_nonce, uint64_t release_nonce,
        std::vector<uint8_t> canonical_claim,
        uint64_t f_launch_generation, uint64_t f_launch_attempt,
        std::array<uint8_t, 16> f_store_guid,
        uint64_t operation_sequence) noexcept;
    void invalidate() noexcept;

    uint64_t channel_generation_ = 0;
    uint64_t mutation_epoch_ = 0;
    uint64_t decoded_frame_sequence_ = 0;
    ClaimAttemptCapability128 attempt_capability_{};
    uint64_t stamp_nonce_ = 0;
    uint64_t release_nonce_ = 0;
    std::vector<uint8_t> canonical_claim_;
    uint64_t f_launch_generation_ = 0;
    uint64_t f_launch_attempt_ = 0;
    std::array<uint8_t, 16> f_store_guid_{};
    uint64_t operation_sequence_ = 0;
};

enum Compression {
    C_LZO = 0,
    C_ZSTD = 1
};

// The remote node is capable of unpacking environment compressed as .tar.xz .
const int NODE_FEATURE_ENV_XZ = ( 1 << 0 );
// The remote node is capable of unpacking environment compressed as .tar.zst .
const int NODE_FEATURE_ENV_ZSTD = ( 1 << 1 );

/* CacheWire has a revision space independent of the ordinary Icecream link.
   Revision 1 is the first deployable shape; ordinary peers still negotiate
   Icecream protocol 50 before these fields are present. */
inline constexpr uint32_t CACHE_WIRE_REVISION = 1;

/* Raw four-byte transition witness sent by the F sidecar only after it has
   accepted ownership of the detached ordinary socket.  This is not an
   ordinary framed message and carries no CacheWire identity. */
inline constexpr uint32_t CACHE_SESSION_READY_MAGIC = UINT32_C(0x50f00001);

/* Sent in READY's place when F's sidecar has no session capacity: F then
   closes the socket having touched no CacheWire state, so C may use another
   F.  Like READY it is raw, which is why no framed message uses the value. */
inline constexpr uint32_t CACHE_SESSION_BUSY_MAGIC = UINT32_C(0x50f00003);

/* Fixed raw reply value for the compiler/cache control descriptor.  The
   payload is: magic, version, wire job, assignment epoch, assignment nonce,
   selected profile, the already-authenticated sidecar generation/attempt,
   and the exact sidecar peer uid/gid observed by the daemon, all in network
   byte order. */
inline constexpr uint32_t P50_CACHE_FD_LEASE_MAGIC = UINT32_C(0x5035464c);
inline constexpr uint32_t P50_CACHE_FD_LEASE_VERSION = 3;
inline constexpr size_t P50_CACHE_FD_LEASE_BYTES = 64;

/* Send the exact network-order CACHE_SESSION_READY_MAGIC (or BUSY_MAGIC)
   under one absolute steady-clock deadline.  The caller retains descriptor
   ownership. */
bool send_cache_session_ready(
    int fd, std::chrono::steady_clock::time_point deadline) noexcept;
bool send_cache_session_busy(
    int fd, std::chrono::steady_clock::time_point deadline) noexcept;

/* Registry values are scoped to CACHE_WIRE_REVISION. */
inline constexpr uint32_t CACHE_PROFILE_P29V1 = (UINT32_C(1) << 0);
inline constexpr uint32_t CACHE_PROFILE_ZSTD_TU = (UINT32_C(1) << 1);
inline constexpr uint32_t CACHE_PROFILE_ZSTD_ROUTE = (UINT32_C(1) << 2);
inline constexpr uint32_t CACHE_DECLARED_PROFILE_MASK =
    CACHE_PROFILE_P29V1 | CACHE_PROFILE_ZSTD_TU | CACHE_PROFILE_ZSTD_ROUTE;
inline constexpr uint32_t CACHE_ADVERTISABLE_PROFILE_MASK =
    CACHE_DECLARED_PROFILE_MASK;

/* C->S capability request carried by GET_CS at protocol 50.  This is
   deliberately distinct from F's Login advertisement: C states which
   CacheWire revision/profiles it can consume, and may attach one soft warm
   route hint.  Canonical absence keeps every older/disabled path legacy. */
inline constexpr size_t P50_CACHE_AFFINITY_HOST_MAX = 255;

/* One strict-P50 reassignment may ask S not to return to the ordinary F
   endpoint that just failed.  This is a request-local exclusion, not a
   blacklist and not cache state: it is carried beside the soft warm hint so
   queueing can wait for another compatible F without poisoning later jobs. */
inline bool p50_cache_retry_avoid_is_valid(
    uint32_t port, std::string_view host) noexcept
{
    const bool absent = port == 0 && host.empty();
    const bool present = port != 0 && port <= UINT16_MAX && !host.empty() &&
        host.size() <= P50_CACHE_AFFINITY_HOST_MAX &&
        host.find('\0') == std::string_view::npos;
    return absent || present;
}

inline bool p50_cache_retry_avoid_is_present(
    uint32_t port, std::string_view host) noexcept
{
    return port != 0 && !host.empty() &&
        p50_cache_retry_avoid_is_valid(port, host);
}

struct P50CacheClientCapability
{
    uint32_t protocol = 0;
    uint32_t profile_mask = 0;

    auto operator<=>(const P50CacheClientCapability &) const = default;
};

inline bool p50_cache_client_request_is_wholly_absent(
    uint32_t protocol, uint32_t profile_mask,
    uint32_t affinity_profile_mask, uint32_t affinity_port,
    std::string_view affinity_host) noexcept
{
    return protocol == 0 && profile_mask == 0 &&
           affinity_profile_mask == 0 && affinity_port == 0 &&
           affinity_host.empty();
}

inline bool p50_cache_client_request_is_valid(
    uint32_t protocol, uint32_t profile_mask,
    uint32_t affinity_profile_mask, uint32_t affinity_port,
    std::string_view affinity_host) noexcept
{
    if (p50_cache_client_request_is_wholly_absent(
            protocol, profile_mask, affinity_profile_mask, affinity_port,
            affinity_host))
        return true;
    if (protocol != CACHE_WIRE_REVISION || profile_mask == 0 ||
        (profile_mask & ~CACHE_ADVERTISABLE_PROFILE_MASK) != 0 ||
        (affinity_profile_mask & ~profile_mask) != 0 ||
        affinity_port > UINT16_MAX ||
        affinity_host.size() > P50_CACHE_AFFINITY_HOST_MAX ||
        affinity_host.find('\0') != std::string_view::npos)
        return false;
    const bool affinity_present = affinity_profile_mask != 0;
    return affinity_present == (affinity_port != 0) &&
           affinity_present == !affinity_host.empty();
}

/* The C daemon, not a wrapper-authored GET_CS payload, owns the kill switch.
   Protocol-50 clients run the retained revision-one profiles by default;
   exact `off` is the kill switch.  An explicit value other than `on`/`off`
   is malformed and fails closed rather than guessing operator intent. */
inline P50CacheClientCapability p50_cache_client_capability_from_mode(
    const char *mode, int wrapper_protocol) noexcept
{
    if (wrapper_protocol < PROTOCOL_VERSION_CACHE_ADVERTISEMENT)
        return {};
    if (mode != nullptr && std::string_view(mode) != "on")
        return {};
    return {CACHE_WIRE_REVISION, CACHE_ADVERTISABLE_PROFILE_MASK};
}

inline P50CacheClientCapability p50_cache_client_capability_from_env(
    int wrapper_protocol) noexcept
{
    return p50_cache_client_capability_from_mode(
        std::getenv("ICECC_P50_MODE"), wrapper_protocol);
}

/* An optional scheduler-local request chooses the one source profile carried
   in each assignment. An explicit request never falls back to another profile. */
enum class P50CacheProfileRequest : uint8_t {
    Default,
    P29V1,
    ZSTD_TU,
    ZSTD_ROUTE,
    Off,
    Unsupported,
};

inline constexpr std::array<uint32_t, 3> P50_DEFAULT_PROFILE_ORDER{
    CACHE_PROFILE_P29V1,
    CACHE_PROFILE_ZSTD_TU,
    CACHE_PROFILE_ZSTD_ROUTE,
};

inline P50CacheProfileRequest p50_cache_profile_request_from_env() noexcept
{
    const char *const value = std::getenv("ICECC_P50_PROFILE");
    if (value == nullptr || *value == '\0')
        return P50CacheProfileRequest::Default;
    const std::string_view requested(value);
    if (requested == "ZSTD_TU")
        return P50CacheProfileRequest::ZSTD_TU;
    if (requested == "P29V1")
        return P50CacheProfileRequest::P29V1;
    if (requested == "ZSTD_ROUTE")
        return P50CacheProfileRequest::ZSTD_ROUTE;
    if (requested == "OFF")
        return P50CacheProfileRequest::Off;
    return P50CacheProfileRequest::Unsupported;
}

inline constexpr uint32_t p50_select_cache_profile(
    uint32_t advertised, P50CacheProfileRequest request) noexcept
{
    if ((advertised & ~CACHE_ADVERTISABLE_PROFILE_MASK) != 0)
        return 0;
    switch (request) {
    case P50CacheProfileRequest::Default:
        for (const uint32_t profile : P50_DEFAULT_PROFILE_ORDER)
            if ((advertised & profile) != 0)
                return profile;
        return 0;
    case P50CacheProfileRequest::ZSTD_TU:
        return (advertised & CACHE_PROFILE_ZSTD_TU) != 0
                   ? CACHE_PROFILE_ZSTD_TU
                   : 0;
    case P50CacheProfileRequest::ZSTD_ROUTE:
        return (advertised & CACHE_PROFILE_ZSTD_ROUTE) != 0
                   ? CACHE_PROFILE_ZSTD_ROUTE
                   : 0;
    case P50CacheProfileRequest::P29V1:
        return (advertised & CACHE_PROFILE_P29V1) != 0
                   ? CACHE_PROFILE_P29V1
                   : 0;
    case P50CacheProfileRequest::Off:
    case P50CacheProfileRequest::Unsupported:
        return 0;
    }
    return 0;
}

/* One pair can use a profile only when C and F advertise the same supported
   CacheWire revision.  S's profile request is then applied to their exact
   intersection; an unavailable explicit request never substitutes. */
inline constexpr uint32_t p50_select_pair_cache_profile(
    uint32_t client_protocol, uint32_t client_profiles,
    uint32_t server_protocol, uint32_t server_profiles,
    P50CacheProfileRequest request) noexcept
{
    if (client_protocol != CACHE_WIRE_REVISION ||
        server_protocol != CACHE_WIRE_REVISION ||
        client_profiles == 0 || server_profiles == 0 ||
        (client_profiles & ~CACHE_ADVERTISABLE_PROFILE_MASK) != 0 ||
        (server_profiles & ~CACHE_ADVERTISABLE_PROFILE_MASK) != 0)
        return 0;
    return p50_select_cache_profile(client_profiles & server_profiles,
                                    request);
}

/* Source-arm mode values are deliberately closed to the runnable source
   profiles and are never accepted independently of cache_profile. */
inline constexpr uint32_t P50_SOURCE_MODE_P29V1 = UINT32_C(1);
inline constexpr uint32_t P50_SOURCE_MODE_ZSTD_TU = UINT32_C(2);
inline constexpr uint32_t P50_SOURCE_MODE_ZSTD_ROUTE = UINT32_C(3);

inline constexpr bool p50_source_profile_mode_valid(uint32_t profile,
                                                     uint32_t source_mode) noexcept
{
    return (profile == CACHE_PROFILE_P29V1 &&
            source_mode == P50_SOURCE_MODE_P29V1) ||
           (profile == CACHE_PROFILE_ZSTD_TU &&
            source_mode == P50_SOURCE_MODE_ZSTD_TU) ||
           (profile == CACHE_PROFILE_ZSTD_ROUTE &&
            source_mode == P50_SOURCE_MODE_ZSTD_ROUTE);
}

inline constexpr bool p50_source_profile_selection_valid(uint32_t profiles) noexcept
{
    return profiles == CACHE_PROFILE_P29V1 ||
           profiles == CACHE_PROFILE_ZSTD_TU ||
           profiles == CACHE_PROFILE_ZSTD_ROUTE;
}

/* The one ordinary-link request used by the compiler-side cache seam.  This
   is deliberately smaller than the source-arm record: the authenticated
   relationship already exists, so only the exact assignment and selected
   source profile are echoed across this boundary. */
struct P50CacheSessionFdRequestFields
{
    uint32_t wire_job_id = 0;
    uint64_t assignment_epoch = 0;
    uint64_t assignment_nonce = 0;
    uint32_t profile = 0;

    [[nodiscard]] bool valid() const noexcept
    {
        return wire_job_id != 0 && assignment_epoch != 0 &&
               assignment_nonce != 0 &&
               p50_source_profile_selection_valid(profile);
    }

    auto operator<=>(const P50CacheSessionFdRequestFields &) const = default;
};

/* Identity of the supervised cache-service incarnation whose HELLO/ACK was
   already completed by the local daemon before it passed the descriptor.
   The wrapper uses this exact value on the first post-handshake control frame;
   it never authors or guesses a sidecar incarnation. */
struct P50CacheControlIdentity
{
    uint64_t generation = 0;
    uint64_t attempt = 0;
    uint64_t peer_uid = UINT64_MAX;
    uint64_t peer_gid = UINT64_MAX;

    [[nodiscard]] bool valid() const noexcept
    {
        return generation != 0 && attempt != 0 &&
               peer_uid <= UINT32_MAX && peer_gid <= UINT32_MAX;
    }

    auto operator<=>(const P50CacheControlIdentity &) const = default;
};

/* Move-only authority for one delayed daemon -> wrapper descriptor reply.
   The ordinary event loop probes a drained client socket once after every
   decoded frame; that empty nonblocking read is not a protocol mutation and
   must not destroy a reply that is waiting for supervised sidecar recovery.
   The ticket instead binds the exact channel incarnation, decoded frame,
   outbound frame boundary, and request.  Any real later frame, ordinary send,
   EOF/error, buffered byte, or non-idle socket still makes consumption fail. */
class P50CacheFdReplyTicket
{
public:
    P50CacheFdReplyTicket() = default;
    P50CacheFdReplyTicket(const P50CacheFdReplyTicket &) = delete;
    P50CacheFdReplyTicket &operator=(const P50CacheFdReplyTicket &) = delete;
    P50CacheFdReplyTicket(P50CacheFdReplyTicket &&other) noexcept;
    P50CacheFdReplyTicket &operator=(P50CacheFdReplyTicket &&other) noexcept;

    [[nodiscard]] bool valid() const noexcept
    {
        return channel_generation_ != 0 && request_.valid();
    }

private:
    friend class MsgChannel;
    P50CacheFdReplyTicket(
        uint64_t channel_generation, uint64_t decoded_frame_sequence,
        uint64_t outbound_frame_sequence,
        P50CacheSessionFdRequestFields request) noexcept;
    void invalidate() noexcept;

    uint64_t channel_generation_ = 0;
    uint64_t decoded_frame_sequence_ = 0;
    uint64_t outbound_frame_sequence_ = 0;
    P50CacheSessionFdRequestFields request_{};
};

/* Shared absent-or-present law for a three-word CacheWire advertisement.
   LoginMsg may advertise a runnable capability set.  A UseCS assignment is
   narrower: cache_assignment_is_valid_present additionally requires the one
   concrete profile selected for that assignment. */
inline bool cache_advertisement_is_wholly_absent(uint32_t port, uint32_t protocol,
                                                  uint32_t profile_mask)
{
    return port == 0 && protocol == 0 && profile_mask == 0;
}
inline bool cache_advertisement_is_well_formed_present(
    uint32_t port, uint32_t protocol, uint32_t profile_mask)
{
    return port > 0 && port <= UINT16_MAX
        && protocol > 0 && protocol <= UINT16_MAX
        && profile_mask != 0
        && (profile_mask & ~CACHE_ADVERTISABLE_PROFILE_MASK) == 0;
}
inline bool cache_advertisement_is_valid_present(uint32_t port,
                                                  uint32_t protocol,
                                                  uint32_t profile_mask)
{
    return cache_advertisement_is_well_formed_present(port, protocol,
                                                       profile_mask)
        && protocol == CACHE_WIRE_REVISION;
}

inline bool cache_assignment_is_valid_present(uint32_t port,
                                               uint32_t protocol,
                                               uint32_t profile_mask)
{
    return cache_advertisement_is_valid_present(port, protocol, profile_mask)
        && p50_source_profile_selection_valid(profile_mask);
}

/* WIRE-AUDIT (three-bucket field classification, BigOracle, owner-ruling
   HOLD on d23d9c5d).  Standing per-wire-change review gate: every field
   either cache-tail-bearing message type touches, classified by what
   currently binds it to a real, checkable system-level guarantee.

   BOUND: UseCSMsg's assignment_epoch_hi/lo, assignment_nonce_hi/lo, and
   job_id (the wire id assignmentEpoch()/assignmentNonce() bind to) --
   together with the scheduler's selected-F-derived host/projection
   relationship (hostname/port, matched to job->server() at the moment of
   dispatch).  These carry a real, checkable assignment: any consumer can
   verify a UseCS's identity against the scheduler's own retained state
   for that job.

   WIRE-PLACEHOLDER / DERIVED-GUARD: the canonical (0,0,0) absent encoding
   of cache_endpoint_port/cache_protocol/cache_profile_mask on both
   LoginMsg and UseCSMsg, and each type's decoder-local tail-validity
   bookkeeping (UseCSMsg::cache_tail_valid, LoginMsg::
   cache_advertisement_tail_valid -- both private, never serialized).
   These exist to make absence and malformation distinguishable and
   rejectable on the wire; they are not themselves guarantees about a
   running cache.

   CURRENTLY MODEL-UNREPRESENTED: the cache endpoint port/protocol/profile
   triple itself, when present, on BOTH LoginMsg and UseCSMsg.  045c6ad1's
   CacheWire transaction-trace model (cache/protocol50.h) refines that
   SEPARATE CacheWire protocol's own transactions -- it does not model
   ordinary Login/UseCS delivery, and no Level-2 model claim is made for
   either tail here.  When the M3 assignment->session slice lands,
   assignment identity + selected-F identity + cache-session endpoint
   become jointly model-representable and must be bound together in that
   same change; until then this triple is carried and wire-validated
   (absent-or-valid-present, never partial) but not modeled beyond it. */

// a list of pairs of host platform, filename
typedef std::list<std::pair<std::string, std::string> > Environments;

// CLOCK_MONOTONIC seconds/milliseconds; immune to wall-clock steps.  Used
// for deferred-output deadline accounting (see MsgChannel::deferred_*).
time_t icecream_monotonic_seconds();
uint64_t icecream_monotonic_msec();

// How long undelivered deferred output may wait before its peer is treated
// as dead.  The same budget the historical blocking send granted.
#define ICECC_DEFERRED_SEND_TIMEOUT_MSEC 30000
// TCP transport failure must not pre-empt the scheduler's 30-second deferred
// output bound or its 36-second ping/pong liveness owner.  Application paths
// with longer exact deadlines (for example a remote compile result) extend
// this per-channel value explicitly after connection admission.
#define ICECC_TCP_USER_TIMEOUT_MSEC 60000
// Historical accepted-channel protocol negotiation allowed one peer this much
// time. Daemons preserve that per-peer budget without blocking their shared
// event loop (see Service::createChannelAccepted()).
#define ICECC_PROTOCOL_HANDSHAKE_TIMEOUT_MSEC 15000

// MsgChannel supports backpressure-tolerant sends (SendDeferrable,
// has_pending_write(), flush_pending()).
#define ICECC_MSGCHANNEL_HAS_DEFERRED_SEND 1

class MsgChannel
{
public:
    enum class ProtocolAdmissionState : uint8_t {
        Pending = 0,
        Ready,
        Failed,
    };

    enum SendFlags {
        SendBlocking = 1 << 0,
        SendNonBlocking = 1 << 1,
        SendBulkOnly = 1 << 2,
        // Tolerate backpressure instead of failing the channel: if the peer's
        // receive buffer is full (EAGAIN, or the poll timeout expires when
        // combined with SendBlocking), keep the unsent bytes queued in the
        // write buffer and report success.  The caller must later call
        // flush_pending() when the socket becomes writable again (e.g. from a
        // POLLOUT event; see has_pending_write()).  Queued bytes are flushed
        // in order, so the byte stream stays intact even if a message was
        // partially transmitted when the buffer filled up.
        SendDeferrable = 1 << 3
    };

    virtual ~MsgChannel();

    void setBulkTransfer();

    // Move Linux TCP_USER_TIMEOUT just past an application-owned absolute
    // deadline.  On platforms without that socket option, merely validate
    // that the deadline is still live.  This changes no poll/get_msg bound;
    // it only prevents the kernel transport timer from firing first.
    bool setTcpUserTimeoutUntil(
        std::chrono::steady_clock::time_point deadline) noexcept;

    std::string dump() const;
    // NULL  <--> channel closed or timeout
    // Will warn in log if EOF and !eofAllowed.
    Msg *get_msg(int timeout = 10, bool eofAllowed = false);
    // Partial frames and EINTR consume the same absolute receive budget.
    Msg *get_msg_until(std::chrono::steady_clock::time_point deadline,
                       bool eofAllowed = false);

    // A malformed P50_SOURCE_ARM is never returned as a message, but its
    // first exact assignment triple is retained long enough for the daemon's
    // event loop to settle only that named scheduler owner. Endpoint, store,
    // and control fields from an invalid frame are never exposed.
    bool take_invalid_p50_source_arm_identity(uint32_t *wire_id,
                                              uint64_t *epoch,
                                              uint64_t *nonce) noexcept;

    // Move out the unforgeable stamp for the immediately preceding exact
    // canonical P5CL/P5CO decode. Each stamp is available once.
    P50DecodedClaimStamp take_p50_decoded_claim_stamp() noexcept;
    P50DecodedOutcomeStamp take_p50_decoded_outcome_stamp() noexcept;

    // F owner call after the exact WAIT row was atomically reserved. The
    // attempt token must be the token carried by the decoded P5CL.
    P50ServerClaimReleaseTicket issue_p50_server_claim_release_ticket(
        P50DecodedClaimStamp &&stamp, uint64_t reservation_id,
        ClaimAttemptCapability128 attempt_capability) noexcept;

    // The only legal F-side ordinary send after a reserved P5CL. The outcome
    // must echo the ticket's exact claim. ADOPTED rebinds the same ticket only
    // after the complete P5CO frame flushes; REFUSED consumes it without ever
    // creating descriptor-release authority.
    bool send_p50_cache_session_outcome(
        P50ServerClaimReleaseTicket &ticket,
        const P50CacheSessionOutcomeMsg &outcome) noexcept;

    // C owner call after receiving P5CO. Validation is performed here against
    // the exact retained outbound P5CL and the expected accepted F identity.
    // Only ADOPTED can return a valid ticket.
    P50ClientAdoptedReleaseTicket issue_p50_client_adopted_release_ticket(
        P50DecodedOutcomeStamp &&stamp, uint64_t expected_f_launch_generation,
        uint64_t expected_f_launch_attempt,
        std::array<uint8_t, 16> expected_f_store_guid,
        uint64_t expected_operation_sequence) noexcept;

    // Opposite descriptor transitions deliberately accept different types.
    int release_fd_after_p50_server_claim(
        P50ServerClaimReleaseTicket &&ticket) noexcept;
    int release_fd_after_p50_client_adopted(
        P50ClientAdoptedReleaseTicket &&ticket) noexcept;

    // Exact bounded payload helpers for the two new ordinary messages. They
    // consume/append bytes only inside the current ordinary frame.
    bool read_current_message_payload(std::vector<uint8_t> &payload,
                                      size_t min_bytes, size_t max_bytes);
    void write_message_payload(std::span<const uint8_t> payload);

    /* Transfer the still-owned ordinary-link descriptor after the one
       immediately preceding decode returned CACHE_SESSION.  A successful
       transfer returns the descriptor and sets fd to -1; all refusal paths
       return -1 without reading or dropping input. */
    int release_fd_if_input_empty();

    /* After a successfully flushed Protocol-50 CACHE_SESSION, wait under the
       caller's unchanged absolute deadline for the exact raw sidecar READY
       witness.  Only then transfer the client descriptor.  Every call consumes
       the one-shot send arm, including refusal and timeout paths.  A BUSY
       witness returns -1 and sets *busy. */
    int release_fd_after_cache_session_ready(
        std::chrono::steady_clock::time_point deadline, bool *busy = nullptr);

    /* Protocol-50 compiler/cache control seam.  The request must have been
       completely decoded on this channel and the reply is a one-shot raw
       fixed lease plus one descriptor.  Every call consumes transfer_fd
       (success and failure); the receiver owns the returned descriptor on
       success. */
    P50CacheFdReplyTicket take_p50_cache_fd_reply_ticket(
        const P50CacheSessionFdRequestMsg &request) noexcept;
    bool send_p50_cache_fd_reply(
        P50CacheFdReplyTicket &&ticket,
        P50CacheControlIdentity control_identity, int transfer_fd,
        std::chrono::steady_clock::time_point deadline) noexcept;
    /* Immediate compatibility helper: mint and consume the ticket in the same
       call.  Delayed owners must retain the explicit move-only ticket. */
    bool send_p50_cache_fd_reply(
        const P50CacheSessionFdRequestMsg &request,
        P50CacheControlIdentity control_identity, int transfer_fd,
        std::chrono::steady_clock::time_point deadline) noexcept;
    int receive_p50_cache_fd_reply(
        const P50CacheSessionFdRequestFields &expected,
        P50CacheControlIdentity &control_identity,
        std::chrono::steady_clock::time_point deadline) noexcept;

    /* Bytes which remain inside the frame currently being decoded.  This is
       deliberately frame-bounded rather than based on buffered input: a
       single read may already contain the following frame. */
    size_t current_message_bytes_remaining(void) const
    {
        return current_message_end >= intogo ? current_message_end - intogo : 0;
    }

    // false <--> error (msg not send)
    bool send_msg(const Msg &, int SendFlags = SendBlocking);

    /* The legacy FileChunk stream has no input selector carrying TU identity.
       The product supplies this identity after assignment and before source
       bytes are transferred.  Counters then follow complete ordinary frames
       at the channel's send/read boundaries. */
    void set_p50_legacy_wire_role(P50LegacyWireRole role) noexcept
    {
        p50_legacy_wire_role = role;
    }
    bool set_p50_legacy_wire_identity(
        const P50LegacyWireIdentity &identity) noexcept;
    bool p50_legacy_wire_prepare_trace() noexcept;
    bool p50_legacy_wire_complete() noexcept;

    // Consume the one terminal STATUS_TEXT, if any, that set_error() fetched
    // while the channel was still readable.  Presence is independent of the
    // text being nonempty.  This moves a bounded value out of the channel,
    // clears the slot, and never reads from or changes the ERROR channel.
    std::optional<std::string> take_error_status();

    // True if a previous send left bytes queued in the write buffer (a
    // SendDeferrable send that ran into backpressure, or a message so far only
    // collected by SendBulkOnly).
    bool has_pending_write(void) const
    {
        return msgtogo > 0;
    }

    // Bytes currently queued for the peer (deferred + bulk-collected).
    size_t pending_bytes(void) const
    {
        return msgtogo;
    }

    // Message-boundary delivery tracking.  framesQueued() advances once per
    // fully composed frame; framesFlushed() advances once the LAST byte of
    // that frame has left the send buffer.  "send_msg returned true" means
    // queued, not delivered -- callers that need delivery (e.g. a request
    // whose reply must not be matched before the request was even on the
    // wire) compare their recorded framesQueued() against framesFlushed().
    uint64_t framesQueued(void) const
    {
        return frames_queued_seq;
    }
    uint64_t framesFlushed(void) const
    {
        return frames_queued_seq - pending_frame_ends.size();
    }

    // Test-only (armed by the daemon under ICECC_TEST_USECS_CUT_AT): force the
    // next flush to send exactly `n` more bytes and then fail as if the peer
    // vanished with the frame incomplete.  This makes the four exact UseCS
    // injection positions -- before byte 1 (n == 0), partial header, partial
    // body, final-byte-short -- deterministically reproducible without a
    // sleep, for the prerequisite usecs-cut-* witnesses.  One-shot and a pure
    // no-op unless explicitly armed; it never runs on the production path.
    void testCutNextFlushAfter(size_t n)
    {
        test_cut_bytes = n;
        test_cut_armed = true;
    }

    // True while a deferrable send has left output undelivered.  An explicit
    // flag rather than a timestamp test: an age of zero is ambiguous during
    // the first second of a backlog, and owners must distinguish "not armed"
    // from "armed, just now".
    bool deferred_output_armed(void) const
    {
        return pending_write_armed;
    }

    // Absolute CLOCK_MONOTONIC millisecond deadline for the current backlog
    // (meaningful only while deferred_output_armed()).  Owners enforce it:
    // the kernel TCP_USER_TIMEOUT bound is #ifdef'd and SO_KEEPALIVE does
    // not cover a peer whose TCP keeps ACKing while the process never reads.
    uint64_t deferred_output_deadline_msec(void) const
    {
        return pending_write_deadline_msec;
    }

    // Try to write queued output without blocking.  A still-full peer buffer
    // just leaves the remaining bytes queued and returns true; false is
    // returned only if the connection hit a real error (the channel is in the
    // error state / at_eof() afterwards).
    bool flush_pending(void);

    // Event-loop admission support. An accepted channel is not an ordinary
    // client until the complete two-way protocol exchange, including queued
    // handshake output, has finished. finish_protocol_admission() disables
    // the handshake-only silent/nonblocking error path before promotion.
    ProtocolAdmissionState protocol_admission_state(void) const noexcept;
    bool finish_protocol_admission(void) noexcept;

    bool has_msg(void) const
    {
        return eof || instate == HAS_MSG;
    }

    // Returns ture if there were no errors filling inbuf.
    bool read_a_bit(void);

    bool at_eof(void) const
    {
        return instate != HAS_MSG && eof;
    }

    bool is_text_based(void) const
    {
        return text_based;
    }

    void readcompressed(unsigned char **buf, size_t &_uclen, size_t &_clen);
    void writecompressed(const unsigned char *in_buf,
                         size_t _in_len, size_t &_out_len);
    void write_environments(const Environments &envs);
    void read_environments(Environments &envs);
    void read_line(std::string &line);
    void write_line(const std::string &line);

    bool eq_ip(const MsgChannel &s) const;

    MsgChannel &operator>>(uint32_t &);
    MsgChannel &operator>>(std::string &);
    MsgChannel &operator>>(std::list<std::string> &);

    // Bounded string reader for new strict wire messages.  Unlike the legacy
    // operator>>, this requires a length-delimited NUL terminator and never
    // constructs a string from an untrusted unterminated buffer.
    bool read_bounded_string(std::string &, size_t max_length);

    MsgChannel &operator<<(uint32_t);
    MsgChannel &operator<<(const std::string &);
    MsgChannel &operator<<(const std::list<std::string> &);

    // our filedesc
    int fd;

    // the minimum protocol version between me and him
    int protocol;
    // the actual maximum protocol the remote supports
    int maximum_remote_protocol;

    std::string name;
    time_t last_talk;

protected:
    MsgChannel(int _fd, struct sockaddr *, socklen_t, bool text = false);
    MsgChannel(int _fd, struct sockaddr *, socklen_t, bool text,
               bool nonblocking_protocol_handshake);

    bool wait_for_protocol();
    bool wait_for_protocol_until(
        std::chrono::steady_clock::time_point deadline);
    // returns false if there was an error sending something; send_flags is a
    // combination of SendFlags bits (SendBlocking / SendDeferrable matter here)
    bool flush_writebuf(int send_flags);
    void writefull(const void *_buf, size_t count);
    // returns false if there was an error in the protocol setup
    bool update_state(void);
    void chop_input(void);
    void chop_output(void);
    bool wait_for_msg(int timeout);
    void set_error(bool silent = false);

    char *msgbuf;
    size_t msgbuflen;
    size_t msgofs;
    size_t msgtogo;
    uint64_t total_appended = 0;
    uint64_t total_drained = 0;
    uint64_t frames_queued_seq = 0;
    std::deque<uint64_t> pending_frame_ends;   // append offsets of frame ends
    struct P50LegacyPendingFrame {
        uint64_t begin = 0;
        uint64_t end = 0;
    };
    struct P50LegacyPendingCompileFile {
        P50LegacyWireIdentity identity{};
        uint64_t frame_bytes = 0;
    };
    std::deque<P50LegacyPendingFrame> p50_legacy_pending_frames;
    std::optional<P50LegacyPendingCompileFile> p50_legacy_pending_compile_file;
    // test-only one-shot mid-frame cut; see testCutNextFlushAfter()
    size_t test_cut_bytes = 0;
    bool test_cut_armed = false;
    // deferred-output deadline state; see deferred_output_armed()
    bool pending_write_armed;
    uint64_t pending_write_deadline_msec;
    P50LegacyWireRole p50_legacy_wire_role = P50LegacyWireRole::C;
    P50LegacyWireIdentity p50_legacy_wire_identity{};
    bool p50_legacy_wire_identity_set = false;
    bool p50_legacy_wire_completed = false;
    int p50_legacy_wire_trace_fd = -1;
    uint64_t p50_legacy_c_to_f_sent = 0;
    uint64_t p50_legacy_c_to_f_received = 0;
    uint64_t p50_legacy_f_to_c_sent = 0;
    uint64_t p50_legacy_f_to_c_received = 0;
    char *inbuf;
    size_t inbuflen;
    size_t inofs;
    size_t intogo;
    size_t current_message_end;

    enum {
        NEED_PROTO,
        NEED_LEN,
        FILL_BUF,
        HAS_MSG,
        ERROR
    } instate;

    uint32_t inmsglen;
    bool eof;
    bool text_based;
    bool nonblocking_protocol_handshake;
    // Armed only by a successfully decoded CACHE_SESSION.  It is cleared by
    // any subsequent decode or ordinary send attempt; there is no generic
    // clean-boundary escape.
    bool cache_session_release_armed;
    // Armed only by a fully flushed outbound CACHE_SESSION with no earlier
    // queued frame. Any later send/receive clears it permanently.
    bool cache_session_send_release_armed;

    // P50 positive-bridge channel authority. Generation is globally unique
    // within the process; mutation_epoch advances on every relevant channel
    // use and invalidates any previously minted stamp/ticket.
    uint64_t p50_channel_generation = 0;
    uint64_t p50_mutation_epoch = 1;
    uint64_t p50_decoded_frame_sequence = 0;
    Msg::Value p50_last_decoded_type = Msg::UNKNOWN;
    uint64_t p50_last_stamp_nonce = 0;
    bool p50_last_stamp_taken = false;
    std::vector<uint8_t> p50_last_canonical_payload;
    uint64_t p50_active_server_release_nonce = 0;
    uint64_t p50_active_server_claim_stamp_nonce = 0;
    uint64_t p50_active_client_release_nonce = 0;
    bool p50_server_outcome_send_armed = false;
    bool p50_fd_reply_arm_consumed = false;
    bool p50_fd_request_ready = false;
    P50CacheSessionFdRequestFields p50_last_fd_request{};
    bool p50_fd_receive_arm = false;
    uint64_t p50_fd_receive_frame_sequence = 0;
    P50CacheSessionFdRequestFields p50_armed_fd_request{};
    bool p50_fd_request_pending = false;
    uint64_t p50_fd_pending_request_frame = 0;
    P50CacheSessionFdRequestFields p50_pending_fd_request{};

    // One exact outbound claim may await one first outcome on this fresh
    // connection. A queued claim is promoted only after its frame fully
    // flushes; any other send or unexpected decoded frame clears it.
    uint64_t p50_queued_claim_frame = 0;
    std::vector<uint8_t> p50_queued_claim;
    ClaimAttemptCapability128 p50_queued_claim_attempt_capability{};
    std::vector<uint8_t> p50_outbound_claim;
    ClaimAttemptCapability128 p50_outbound_claim_attempt_capability{};

    uint32_t invalid_p50_source_arm_wire_id = 0;
    uint64_t invalid_p50_source_arm_epoch = 0;
    uint64_t invalid_p50_source_arm_nonce = 0;

private:
    friend class Service;

    // deep copied
    struct sockaddr *addr;
    socklen_t addr_len;
    bool set_error_recursion;
    bool deadline_receive_active = false;
    std::optional<std::string> error_status;

    void p50_note_channel_mutation() noexcept;
    void begin_receive() noexcept;
    void p50_clear_decoded_stamp() noexcept;
    void p50_clear_outbound_claim() noexcept;
    void p50_promote_flushed_claim() noexcept;
    void p50_promote_flushed_fd_request() noexcept;
    void p50_legacy_note_received(Msg::Value type, size_t frame_bytes) noexcept;
    void p50_legacy_note_frame_queued(Msg::Value type,
                                      uint64_t begin, uint64_t end) noexcept;
    void p50_legacy_note_drained(uint64_t begin, uint64_t end) noexcept;
    bool p50_clean_release_boundary() const noexcept;
    int p50_checked_release_fd() noexcept;
};

// just convenient functions to create MsgChannels
class Service
{
public:
    static MsgChannel *createChannel(const std::string &host, unsigned short p, int timeout);
    // Absolute-deadline variant used by the P50 CACHE_SESSION factory.  The
    // connect and ordinary protocol negotiation share one unchanged budget;
    // on TCP, the kernel user timeout is moved just past that budget so it
    // cannot pre-empt the application owner.
    static MsgChannel *createChannelUntil(
        const std::string &host, unsigned short p,
        std::chrono::steady_clock::time_point deadline);
    // Retry the same endpoint only when an individual connection/protocol
    // attempt consumes its complete slice.  Immediate definitive failures
    // remain immediate, and every attempt shares one unchanged outer
    // deadline.  No application message is sent by this factory.
    // RemainingAfterFirst permits one fresh connection after a stalled first
    // slice, then lets a consistently slow peer use the remaining budget.
    // HedgeAfterFirst retains the first live handshake while starting at most
    // one additional connection to the same resolved endpoint. Both share the
    // original deadline; only the selected winner may carry application data.
    enum class ChannelRetryPolicy { FixedSlices, RemainingAfterFirst, HedgeAfterFirst };
    static MsgChannel *createChannelRetryUntil(
        const std::string &host, unsigned short p,
        std::chrono::steady_clock::time_point deadline,
        std::chrono::milliseconds attempt_budget);
    static MsgChannel *createChannelRetryUntil(
        const std::string &host, unsigned short p,
        std::chrono::steady_clock::time_point deadline,
        std::chrono::milliseconds attempt_budget,
        ChannelRetryPolicy policy);
    static MsgChannel *createChannel(const std::string &domain_socket);
    static MsgChannel *createChannel(int remote_fd, struct sockaddr *, socklen_t);
    // Daemon accept-side factory: construct and emit the initial protocol
    // bytes without waiting for the peer. The owning event loop must poll,
    // progress, deadline, and call finish_protocol_admission() before exposing
    // the channel to ordinary message handlers.
    static MsgChannel *createChannelAccepted(
        int remote_fd, struct sockaddr *, socklen_t);
};

class Broadcasts
{
public:
    // Broadcasts a message about this scheduler and its information.
    static void broadcastSchedulerVersion(int scheduler_port, const char* netname, time_t starttime);
    // Checks if the data received is a scheduler version broadcast.
    static bool isSchedulerVersion(const char* buf, int buflen);
    // Reads data from a scheduler version broadcast.
    static void getSchedulerVersionData( const char* buf, int* protocol, time_t* time, std::string* netname );
    /// Broadcasts the given data on the given port.
    static const int BROAD_BUFLEN = 268;
private:
    static void broadcastData(int port, const char* buf, int size);
};

// --------------------------------------------------------------------------
// this class is also used by icecream-monitor
class DiscoverSched
{
public:
    /* Connect to a scheduler waiting max. TIMEOUT seconds.
       schedname can be the hostname of a box running a scheduler, to avoid
       broadcasting, port can be specified explicitly */
    DiscoverSched(const std::string &_netname = std::string(),
                  int _timeout = 2,
                  const std::string &_schedname = std::string(),
                  int port = 0);
    ~DiscoverSched();

    bool timed_out();

    int listen_fd() const
    {
        return schedname.empty() ? ask_fd : -1;
    }

    int connect_fd() const
    {
        return schedname.empty() ? -1 : ask_fd;
    }

    // compat for icecream monitor
    int get_fd() const
    {
        return listen_fd();
    }

    /* Attempt to get a conenction to the scheduler.

       Continue to call this while it returns NULL and timed_out()
       returns false. If this returns NULL you should wait for either
       more data on listen_fd() (use select), or a timeout of your own.
       */
    MsgChannel *try_get_scheduler();

    // Returns the hostname of the scheduler - set by constructor or by try_get_scheduler
    std::string schedulerName() const
    {
        return schedname;
    }

    // Returns the network name of the scheduler - set by constructor or by try_get_scheduler
    std::string networkName() const
    {
        return netname;
    }

    /* Return a list of all reachable netnames.  We wait max. WAITTIME
       milliseconds for answers.  */
    static std::list<std::string> getNetnames(int waittime = 2000, int port = 8765);

    // Checks if the data is from a scheduler discovery broadcast, returns version of the sending
    // daemon is yes.
    static bool isSchedulerDiscovery(const char* buf, int buflen, int* daemon_version);
    // Prepares data for sending a reply to a scheduler discovery broadcast.
    static int prepareBroadcastReply(char* buf, const char* netname, time_t starttime);

private:
    struct sockaddr_in remote_addr;
    std::string netname;
    std::string schedname;
    int timeout;
    int ask_fd;
    int ask_second_fd; // for debugging
    time_t time0;
    unsigned int sport;
    int best_version;
    time_t best_start_time;
    std::string best_schedname;
    int best_port;
    bool multiple;

    void attempt_scheduler_connect();
    void sendSchedulerDiscovery( int version );
    static bool get_broad_answer(int ask_fd, int timeout, char *buf2, struct sockaddr_in *remote_addr,
                 socklen_t *remote_len);
    static void get_broad_data(const char* buf, const char** name, int* version, time_t* start_time);
};
// --------------------------------------------------------------------------

/* Return a list of all reachable netnames.  We wait max. WAITTIME
   milliseconds for answers.  */
std::list<std::string> get_netnames(int waittime = 2000, int port = 8765);

class PingMsg : public Msg
{
public:
    PingMsg()
        : Msg(Msg::PING) {}
};

class EndMsg : public Msg
{
public:
    EndMsg()
        : Msg(Msg::END) {}
};

/* Empty-payload ordinary-link discriminator for the same-fd CacheWire
   handoff.  CacheWire's own SessionHello follows on the transferred fd and
   binds C_GUID there; this ordinary message carries no CacheWire fields. */
class CacheSessionMsg : public Msg
{
public:
    CacheSessionMsg()
        : Msg(Msg::CACHE_SESSION) {}

    bool valid_for_protocol(int negotiated_protocol) const override
    {
        return negotiated_protocol == PROTOCOL_VERSION;
    }
};

class P50CacheSessionClaimMsg : public Msg
{
public:
    static constexpr size_t MaxPayloadBytes = 1024;

    P50CacheSessionClaimMsg()
        : Msg(Msg::P50_CACHE_SESSION_CLAIM) {}
    explicit P50CacheSessionClaimMsg(std::vector<uint8_t> canonical_wire)
        : Msg(Msg::P50_CACHE_SESSION_CLAIM), wire(std::move(canonical_wire)) {}

    void fill_from_channel(MsgChannel *c) override;
    void send_to_channel(MsgChannel *c) const override;
    bool valid_payload() const override;
    bool valid_for_protocol(int negotiated_protocol) const override
    {
        return negotiated_protocol == PROTOCOL_VERSION;
    }

    std::vector<uint8_t> wire;

private:
    bool wire_payload_valid = true;
};

class P50CacheSessionOutcomeMsg : public Msg
{
public:
    static constexpr size_t MaxPayloadBytes = 1152;

    P50CacheSessionOutcomeMsg()
        : Msg(Msg::P50_CACHE_SESSION_OUTCOME) {}
    explicit P50CacheSessionOutcomeMsg(std::vector<uint8_t> canonical_wire)
        : Msg(Msg::P50_CACHE_SESSION_OUTCOME), wire(std::move(canonical_wire)) {}

    void fill_from_channel(MsgChannel *c) override;
    void send_to_channel(MsgChannel *c) const override;
    bool valid_payload() const override;
    bool valid_for_protocol(int negotiated_protocol) const override
    {
        return negotiated_protocol == PROTOCOL_VERSION;
    }

    std::vector<uint8_t> wire;

private:
    bool wire_payload_valid = true;
};

/*
 * Explicit Protocol-50 source-arm authority.  This is an ordinary-wire
 * admission message, not a CACHE_SESSION payload.  Every field is carried in
 * the same frame so a receiver never has to join assignment, endpoint, store,
 * and control-launch values from separate messages.
 */
struct P50SourceArmFields {
    uint32_t wire_job_id = 0;
    uint64_t assignment_epoch = 0;
    uint64_t assignment_nonce = 0;
    std::string selected_f_host;
    uint32_t selected_f_ordinary_port = 0;
    uint32_t selected_f_cache_port = 0;
    uint32_t cache_protocol = 0;
    uint32_t cache_profile = 0;
    uint64_t logical_job = 0;
    uint64_t compiler_attempt = 0;
    uint64_t c_store_generation = 0;
    uint64_t c_store_derivation_version = 0;
    std::array<uint8_t, 16> c_store_guid{};
    uint64_t source_request_id = 0;
    uint32_t source_mode = 0;
    uint64_t c_control_generation = 0;
    uint64_t c_control_attempt = 0;

    [[nodiscard]] bool valid() const noexcept
    {
        return wire_job_id != 0 && assignment_epoch != 0 &&
               assignment_nonce != 0 && !selected_f_host.empty() &&
               selected_f_host.size() <= 255 &&
               selected_f_host.find('\0') == std::string::npos &&
               selected_f_ordinary_port != 0 &&
               selected_f_ordinary_port <= UINT16_MAX &&
               selected_f_cache_port != 0 &&
               selected_f_cache_port <= UINT16_MAX &&
               cache_protocol == CACHE_WIRE_REVISION &&
               p50_source_profile_mode_valid(cache_profile, source_mode) &&
               logical_job != 0 &&
               compiler_attempt != 0 && c_store_generation != 0 &&
               c_store_derivation_version ==
                   icecc::p50::kStoreIdentityDerivationVersion &&
               icecc::p50::store_identity_guid_valid_for_role(
                   c_store_guid, icecc::p50::kStoreIdentityClientRole) &&
               source_request_id != 0 &&
               c_control_generation != 0 && c_control_attempt != 0;
    }
    auto operator<=>(const P50SourceArmFields&) const = default;
};

class P50SourceArmMsg : public Msg
{
public:
    static constexpr size_t MaxPayloadBytes = 512;

    P50SourceArmMsg()
        : Msg(Msg::P50_SOURCE_ARM) {}
    explicit P50SourceArmMsg(P50SourceArmFields fields)
        : Msg(Msg::P50_SOURCE_ARM), arm(std::move(fields)) {}

    void fill_from_channel(MsgChannel *c) override;
    void send_to_channel(MsgChannel *c) const override;
    bool valid_payload() const override;
    bool valid_for_protocol(int negotiated_protocol) const override
    {
        return negotiated_protocol == PROTOCOL_VERSION;
    }

    P50SourceArmFields arm;

private:
    bool wire_payload_valid = true;
};

/* Exact private request for a raw control-fd lease.  The request is ordinary
   framed MsgChannel traffic; the reply deliberately is not another Msg so
   that the descriptor and its fixed lease bytes are received together. */
class P50CacheSessionFdRequestMsg : public Msg
{
public:
    static constexpr size_t PayloadBytes = 4 + 8 + 8 + 4;

    P50CacheSessionFdRequestMsg()
        : Msg(Msg::P50_CACHE_SESSION_FD_REQUEST) {}
    explicit P50CacheSessionFdRequestMsg(P50CacheSessionFdRequestFields fields)
        : Msg(Msg::P50_CACHE_SESSION_FD_REQUEST), request(std::move(fields)) {}

    void fill_from_channel(MsgChannel *c) override;
    void send_to_channel(MsgChannel *c) const override;
    bool valid_payload() const override;
    bool valid_for_protocol(int negotiated_protocol) const override
    {
        return negotiated_protocol == PROTOCOL_VERSION;
    }

    P50CacheSessionFdRequestFields request;

private:
    bool wire_payload_valid = true;
};

/* Canonical semantic value for the exact F acknowledgement.  Reducers retain
 * this complete value without retaining MsgChannel framing state. */
struct P50SourceArmedFields {
    static constexpr size_t MaxPayloadBytes = 1024;
    static constexpr uint32_t MaxSourceBudgetMsec = 60000;

    [[nodiscard]] bool semantic_valid() const noexcept
    {
        return arm.valid() && f_control_generation != 0 &&
               f_control_attempt != 0 && f_store_generation != 0 &&
               icecc::p50::store_identity_guid_valid_for_role(
                   f_store_guid, icecc::p50::kStoreIdentityFileRole) &&
               f_store_derivation_version ==
                   icecc::p50::kStoreIdentityDerivationVersion &&
               arm_observation_id != 0 && source_budget_msec != 0 &&
               source_budget_msec <= MaxSourceBudgetMsec &&
               attempt_capability_1.valid() &&
               attempt_capability_2.valid() &&
               attempt_capability_1 != attempt_capability_2 &&
               !icecc::p50::store_identity_file_guid_matches_client(
                   arm.c_store_guid, f_store_guid);
    }

    P50SourceArmFields arm{};
    uint64_t f_control_generation = 0;
    uint64_t f_control_attempt = 0;
    uint64_t f_store_generation = 0;
    std::array<uint8_t, 16> f_store_guid{};
    uint64_t f_store_derivation_version = 0;
    uint64_t arm_observation_id = 0;
    uint32_t source_budget_msec = 0;
    ClaimAttemptCapability128 attempt_capability_1{};
    ClaimAttemptCapability128 attempt_capability_2{};

    auto operator<=>(const P50SourceArmedFields&) const = default;
};

/* Exact F acknowledgement.  It echoes every arm field and adds the current
 * F control launch, StoreIdentity generation/GUID/version, and a fresh
 * nonzero observation.  C and F are independent supervised sidecar
 * incarnations: each GUID must have its own fixed role bit, but the ACK must
 * never pretend that F's CSPRNG root is derived from C's root.  Raw roots are
 * deliberately never sent or logged. */
class P50SourceArmedMsg : public Msg, public P50SourceArmedFields
{
public:
    P50SourceArmedMsg()
        : Msg(Msg::P50_SOURCE_ARMED) {}

    P50SourceArmedMsg(P50SourceArmFields fields,
                      uint64_t f_generation, uint64_t f_attempt,
                      uint64_t f_store_generation,
                      std::array<uint8_t, 16> f_guid,
                      uint64_t derivation_version,
                      uint64_t observation,
                      uint32_t source_budget_msec,
                      ClaimAttemptCapability128 capability_1,
                      ClaimAttemptCapability128 capability_2)
        : Msg(Msg::P50_SOURCE_ARMED),
          P50SourceArmedFields{std::move(fields), f_generation, f_attempt,
                               f_store_generation, f_guid,
                               derivation_version, observation,
                               source_budget_msec, capability_1,
                               capability_2} {}

    void fill_from_channel(MsgChannel *c) override;
    void send_to_channel(MsgChannel *c) const override;
    bool valid_payload() const override;
    bool valid_for_protocol(int negotiated_protocol) const override
    {
        return negotiated_protocol == PROTOCOL_VERSION;
    }

    [[nodiscard]] bool acknowledges(const P50SourceArmMsg &request) const noexcept
    {
        return valid_payload() && request.valid_payload() && arm == request.arm;
    }

private:
    bool wire_payload_valid = true;
};

class GetCSMsg : public Msg
{
public:
    GetCSMsg()
        : Msg(Msg::GET_CS)
        , count(1)
        , arg_flags(0)
        , client_id(0)
        , client_count(0)
        , niceness(0)
        , cache_protocol(0)
        , cache_profile_mask(0)
        , cache_affinity_profile_mask(0)
        , cache_affinity_port(0)
        , cache_retry_avoid_port(0)
        , remote_required(0)
        , cache_request_tail_valid(true)
        {}

    GetCSMsg(const Environments &envs, const std::string &f,
             CompileJob::Language _lang, unsigned int _count,
             std::string _target, unsigned int _arg_flags,
             const std::string &host, int _minimal_host_version,
             unsigned int _required_features,
             int _niceness,
             unsigned int _client_count = 0,
             const std::string &_command_summary = "");

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;
    bool valid_payload() const override;

    Environments versions;
    std::string filename;
    CompileJob::Language lang;
    uint32_t count; // the number of UseCS messages to answer with - usually 1
    std::string target;
    uint32_t arg_flags;
    uint32_t client_id;
    std::string preferred_host;
    int minimal_host_version;
    uint32_t required_features;
    uint32_t client_count; // number of CS -> C connections at the moment
    uint32_t niceness; // nice priority (0-20)
    std::string command_summary;
    uint32_t cache_protocol; // C-supported CacheWire revision, or 0
    uint32_t cache_profile_mask; // all profiles C can consume, or 0
    uint32_t cache_affinity_profile_mask; // warm profiles at the hinted F
    uint32_t cache_affinity_port; // exact ordinary F port; zero iff no hint
    std::string cache_affinity_host; // exact F address; empty iff no hint
    uint32_t cache_retry_avoid_port; // failed ordinary F port; zero iff absent
    std::string cache_retry_avoid_host; // failed exact F address; empty iff absent
    uint32_t remote_required; // one iff submitter-local selection is forbidden

private:
    bool cache_request_tail_valid;
};

class UseCSMsg : public Msg
{
public:
    UseCSMsg()
        : Msg(Msg::USE_CS)
        , job_id(0)
        , port(0)
        , got_env(0)
        , client_id(0)
        , matched_job_id(0)
        , assignment_epoch_hi(0)
        , assignment_epoch_lo(0)
        , assignment_nonce_hi(0)
        , assignment_nonce_lo(0)
        , c_guid_hi(0)
        , c_guid_lo(0)
        , tu_seq_hi(0)
        , tu_seq_lo(0)
        , cache_endpoint_port(0)
        , cache_protocol(0)
        , cache_profile_mask(0)
        , cache_tail_valid(true) {}
    UseCSMsg(std::string platform, std::string host, unsigned int p, unsigned int id, bool gotit,
             unsigned int _client_id, unsigned int matched_host_jobs,
             uint64_t assignment_epoch = 0, uint64_t assignment_nonce = 0,
             uint32_t cache_port = 0, uint32_t cache_proto = 0,
             uint32_t cache_mask = 0)
        : Msg(Msg::USE_CS),
          job_id(id),
          hostname(host),
          port(p),
          host_platform(platform),
          got_env(gotit),
          client_id(_client_id),
          matched_job_id(matched_host_jobs),
          assignment_epoch_hi(uint32_t(assignment_epoch >> 32)),
          assignment_epoch_lo(uint32_t(assignment_epoch)),
          assignment_nonce_hi(uint32_t(assignment_nonce >> 32)),
          assignment_nonce_lo(uint32_t(assignment_nonce)),
          c_guid_hi(0),
          c_guid_lo(0),
          tu_seq_hi(0),
          tu_seq_lo(0),
          cache_endpoint_port(cache_port),
          cache_protocol(cache_proto),
          cache_profile_mask(cache_mask),
          cache_tail_valid(true) {}

    /* C_GUID/TU_SEQ-only form used when no cache advertisement is present.
       Keep the historical cache-tail constructor above source-compatible. */
    UseCSMsg(std::string platform, std::string host, unsigned int p, unsigned int id, bool gotit,
             unsigned int _client_id, unsigned int matched_host_jobs,
             uint64_t assignment_epoch, uint64_t assignment_nonce,
             uint64_t c_guid, uint64_t tu_seq)
        : UseCSMsg(std::move(platform), std::move(host), p, id, gotit,
                   _client_id, matched_host_jobs, assignment_epoch,
                   assignment_nonce, 0, 0, 0)
    {
        setCompileIdentity(c_guid, tu_seq);
    }

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;
    virtual bool valid_payload() const;

    uint64_t assignmentEpoch() const
    {
        return (uint64_t(assignment_epoch_hi) << 32) | assignment_epoch_lo;
    }
    uint64_t assignmentNonce() const
    {
        return (uint64_t(assignment_nonce_hi) << 32) | assignment_nonce_lo;
    }

    void setCompileIdentity(uint64_t c_guid, uint64_t tu_seq)
    {
        c_guid_hi = uint32_t(c_guid >> 32);
        c_guid_lo = uint32_t(c_guid);
        tu_seq_hi = uint32_t(tu_seq >> 32);
        tu_seq_lo = uint32_t(tu_seq);
    }
    uint64_t cGuid() const
    {
        return (uint64_t(c_guid_hi) << 32) | c_guid_lo;
    }
    uint64_t tuSeq() const
    {
        return (uint64_t(tu_seq_hi) << 32) | tu_seq_lo;
    }
    bool compileIdentityValid() const
    {
        return cGuid() != 0 || tuSeq() == 0;
    }

    bool hasAssignmentIdentity() const
    {
        return assignmentEpoch() != 0 && assignmentNonce() != 0;
    }
    bool applyAssignmentTo(CompileJob *job) const;
    bool hasCacheAdvertisement() const { return cache_endpoint_port != 0; }

    uint32_t job_id;
    std::string hostname;
    uint32_t port;
    std::string host_platform;
    uint32_t got_env;
    uint32_t client_id;
    uint32_t matched_job_id;
    /* Protocol 50 appends exactly these four words.  job_id above remains the
       sole serialized wire id. */
    uint32_t assignment_epoch_hi;
    uint32_t assignment_epoch_lo;
    uint32_t assignment_nonce_hi;
    uint32_t assignment_nonce_lo;
    uint32_t c_guid_hi;
    uint32_t c_guid_lo;
    uint32_t tu_seq_hi;
    uint32_t tu_seq_lo;
    /* S2: protocol 50 also appends this three-word assignment-bound S->C
       cache-endpoint handoff tail, gated and shaped exactly like LoginMsg's
       Login-only advertisement tail (see cache_advertisement_is_wholly_absent
       / cache_advertisement_is_valid_present).  hostname/port above already
       carry the selected F's compile endpoint; the F's cache port is a
       separate listener on the same host. */
    uint32_t cache_endpoint_port;
    uint32_t cache_protocol;
    uint32_t cache_profile_mask;

private:
    bool cache_tail_valid;
};

/* Daemon-side defensive re-check (BigOracle, d23d9c5d HOLD), factored out
   of Daemon::scheduler_use_cs into a small, pure, independently testable
   helper: a present cache triple is retained only when it is both fully
   valid on its own (never merely non-empty) AND bound to a COMPLETE,
   nonzero assignment identity {job_id, epoch, nonce} -- matching
   UseCSMsg::valid_payload's own assignment_complete definition exactly.
   hasAssignmentIdentity() alone checks only epoch+nonce, not job_id (see
   its own comment on UseCSMsg): a hand-constructed message with job_id==0
   and nonzero epoch/nonce would otherwise be wrongly admitted here, even
   though valid_payload() itself would already refuse it as a partial
   identity paired with a present cache triple. UseCSMsg::valid_payload()
   already enforces the full law at the wire (see MsgChannel::get_msg), so
   an invalid combination can no longer legitimately reach this function
   through any real socket -- but the daemon must not trust that channel-
   layer gate implicitly, so this stays as defense in depth. Because a
   live wire path can no longer construct the malformed input, this is
   exercised directly with a hand-constructed UseCSMsg in
   unittests/p50cacheadvertisement.cpp rather than through any end-to-end
   integration test. */
inline bool usecs_cache_handoff_admissible(const UseCSMsg &msg)
{
    return msg.job_id != 0 && msg.hasAssignmentIdentity()
        && msg.hasCacheAdvertisement()
        && cache_assignment_is_valid_present(
               msg.cache_endpoint_port, msg.cache_protocol,
               msg.cache_profile_mask);
}

class NoCSMsg : public Msg
{
public:
    NoCSMsg()
        : Msg(Msg::NO_CS), job_id(0), client_id(0), assignment_epoch_hi(0),
          assignment_epoch_lo(0), assignment_nonce_hi(0), assignment_nonce_lo(0),
          c_guid_hi(0), c_guid_lo(0), tu_seq_hi(0), tu_seq_lo(0) {}
    NoCSMsg(unsigned int id, unsigned int _client_id,
            uint64_t assignment_epoch = 0, uint64_t assignment_nonce = 0,
            uint64_t c_guid = 0, uint64_t tu_seq = 0)
        : Msg(Msg::NO_CS),
          job_id(id),
          client_id(_client_id),
          assignment_epoch_hi(uint32_t(assignment_epoch >> 32)),
          assignment_epoch_lo(uint32_t(assignment_epoch)),
          assignment_nonce_hi(uint32_t(assignment_nonce >> 32)),
          assignment_nonce_lo(uint32_t(assignment_nonce)),
          c_guid_hi(uint32_t(c_guid >> 32)), c_guid_lo(uint32_t(c_guid)),
          tu_seq_hi(uint32_t(tu_seq >> 32)), tu_seq_lo(uint32_t(tu_seq)) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    uint32_t job_id;
    uint32_t client_id;
    uint32_t assignment_epoch_hi;
    uint32_t assignment_epoch_lo;
    uint32_t assignment_nonce_hi;
    uint32_t assignment_nonce_lo;
    uint32_t c_guid_hi;
    uint32_t c_guid_lo;
    uint32_t tu_seq_hi;
    uint32_t tu_seq_lo;
    uint64_t assignmentEpoch() const
    { return (uint64_t(assignment_epoch_hi) << 32) | assignment_epoch_lo; }
    uint64_t assignmentNonce() const
    { return (uint64_t(assignment_nonce_hi) << 32) | assignment_nonce_lo; }
    uint64_t cGuid() const { return (uint64_t(c_guid_hi) << 32) | c_guid_lo; }
    uint64_t tuSeq() const { return (uint64_t(tu_seq_hi) << 32) | tu_seq_lo; }
    bool compileIdentityValid() const { return cGuid() != 0 || tuSeq() == 0; }
};

class GetNativeEnvMsg : public Msg
{
public:
    GetNativeEnvMsg()
        : Msg(Msg::GET_NATIVE_ENV) {}

    GetNativeEnvMsg(const std::string &c, const std::list<std::string> &e,
        const std::string &comp)
        : Msg(Msg::GET_NATIVE_ENV)
        , compiler(c)
        , extrafiles(e)
        , compression(comp)
        {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    std::string compiler; // "gcc", "clang" or the actual binary
    std::list<std::string> extrafiles;
    std::string compression; // "" (=default), "none", "gzip", "xz", etc.
};

class UseNativeEnvMsg : public Msg
{
public:
    UseNativeEnvMsg()
        : Msg(Msg::NATIVE_ENV) {}

    UseNativeEnvMsg(std::string _native)
        : Msg(Msg::NATIVE_ENV)
        , nativeVersion(_native) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    std::string nativeVersion;
};

class CompileFileMsg : public Msg
{
public:
    CompileFileMsg(CompileJob *j, bool delete_job = false)
        : Msg(Msg::COMPILE_FILE)
        , deleteit(delete_job)
        , job(j)
        , p50_input_tail_valid(true) {}

    ~CompileFileMsg()
    {
        if (deleteit) {
            delete job;
        }
    }

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;
    virtual bool valid_for_protocol(int negotiated_protocol) const
    {
        return job == nullptr || !job->usesP50Input()
            || negotiated_protocol >= PROTOCOL_VERSION_CACHE_ADVERTISEMENT;
    }
    virtual bool valid_payload() const
    {
        return job != nullptr && p50_input_tail_valid
            && job->assignmentIdentityValid()
            && job->compileIdentityValid()
            && job->compileInputIdentityValid();
    }
    bool legacy_wire_identity(P50LegacyWireIdentity &identity) const noexcept
    {
        if (job == nullptr)
            return false;
        identity = {job->jobID(), job->assignmentEpoch(),
                    job->assignmentNonce(), job->cGuid(), job->tuSeq()};
        return identity.valid();
    }
    CompileJob *takeJob();

private:
    std::string remote_compiler_name() const;

    bool deleteit;
    CompileJob *job;
    /* Protocol 50 is an unshipped draft, so its fixed compiler-input tail is
       mandatory even for legacy-mode jobs (canonical all-zero value).  Keep
       decoder shape validity separate from semantic identity validity. */
    bool p50_input_tail_valid;
};

class FileChunkMsg : public Msg
{
public:
    FileChunkMsg(unsigned char *_buffer, size_t _len)
        : Msg(Msg::FILE_CHUNK)
        , buffer(_buffer)
        , len(_len)
        , del_buf(false) {}

    FileChunkMsg()
        : Msg(Msg::FILE_CHUNK)
        , buffer(0)
        , len(0)
        , del_buf(true) {}

    ~FileChunkMsg();

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    unsigned char *buffer;
    size_t len;
    mutable size_t compressed;
    bool del_buf;

private:
    FileChunkMsg(const FileChunkMsg &);
    FileChunkMsg &operator=(const FileChunkMsg &);
};

class CompileResultMsg : public Msg
{
public:
    CompileResultMsg()
        : Msg(Msg::COMPILE_RESULT)
        , status(0)
        , was_out_of_memory(false)
        , have_dwo_file(false)
        , assignment_epoch_hi(0), assignment_epoch_lo(0)
        , assignment_nonce_hi(0), assignment_nonce_lo(0)
        , c_guid_hi(0), c_guid_lo(0), tu_seq_hi(0), tu_seq_lo(0) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    void setAssignmentIdentity(uint64_t epoch, uint64_t nonce)
    {
        assignment_epoch_hi = uint32_t(epoch >> 32);
        assignment_epoch_lo = uint32_t(epoch);
        assignment_nonce_hi = uint32_t(nonce >> 32);
        assignment_nonce_lo = uint32_t(nonce);
    }
    void setCompileIdentity(uint64_t c_guid, uint64_t tu_seq)
    {
        c_guid_hi = uint32_t(c_guid >> 32);
        c_guid_lo = uint32_t(c_guid);
        tu_seq_hi = uint32_t(tu_seq >> 32);
        tu_seq_lo = uint32_t(tu_seq);
    }
    uint64_t assignmentEpoch() const
    { return (uint64_t(assignment_epoch_hi) << 32) | assignment_epoch_lo; }
    uint64_t assignmentNonce() const
    { return (uint64_t(assignment_nonce_hi) << 32) | assignment_nonce_lo; }
    uint64_t cGuid() const { return (uint64_t(c_guid_hi) << 32) | c_guid_lo; }
    uint64_t tuSeq() const { return (uint64_t(tu_seq_hi) << 32) | tu_seq_lo; }
    bool compileIdentityMatches(const CompileJob &job) const
    {
        return job.assignmentIdentityValid() && job.compileIdentityValid()
            && assignmentEpoch() == job.assignmentEpoch()
            && assignmentNonce() == job.assignmentNonce()
            && cGuid() == job.cGuid() && tuSeq() == job.tuSeq();
    }

    int status;
    std::string out;
    std::string err;
    bool was_out_of_memory;
    bool have_dwo_file;
    uint32_t assignment_epoch_hi, assignment_epoch_lo;
    uint32_t assignment_nonce_hi, assignment_nonce_lo;
    uint32_t c_guid_hi, c_guid_lo;
    uint32_t tu_seq_hi, tu_seq_lo;
};

/*
 * P50 result disposition carried on the ordinary framed link.
 *
 * This is intentionally a wire-only acknowledgement/disposition carrier;
 * it does not alter CompileResultMsg.  Its identity is exactly the identity
 * already carried by the P50 assignment and CompileFile messages:
 * job_id, assignment epoch/nonce, and CompileInputIdentity.  In particular,
 * no run id, selected-F id, F-store id, endpoint generation, or protocol-51
 * extension is implied here.
 *
 * The body after the message type is fixed at 23 words:
 *   job_id (1), epoch/nonce (4), CompileInputIdentity (17), disposition (1).
 * This terminal message exists only for a present P50 input selector.  It
 * requires validPresent() and binds both input attempt_id and request_id to
 * the exact assignment nonce.  An absent/partial selector or a disposition
 * without a complete assignment fence is never useful to lifecycle closure
 * and is refused before framing.
 */
class ResultDispositionMsg : public Msg
{
public:
    enum Disposition : uint32_t {
        Accepted = 1,
        DefinitiveCancel = 2,
        // Descriptive alias for callers that use result terminology.
        Rejected = DefinitiveCancel
    };

    static constexpr size_t FixedPayloadWords = 23;

    ResultDispositionMsg()
        : Msg(Msg::RESULT_DISPOSITION)
        , job_id(0)
        , assignment_epoch_hi(0)
        , assignment_epoch_lo(0)
        , assignment_nonce_hi(0)
        , assignment_nonce_lo(0)
        , compile_input()
        , disposition(DefinitiveCancel)
        , wire_payload_valid(true) {}

    ResultDispositionMsg(uint32_t id, uint64_t epoch, uint64_t nonce,
                         const CompileInputIdentity &input,
                         Disposition result)
        : Msg(Msg::RESULT_DISPOSITION)
        , job_id(id)
        , assignment_epoch_hi(uint32_t(epoch >> 32))
        , assignment_epoch_lo(uint32_t(epoch))
        , assignment_nonce_hi(uint32_t(nonce >> 32))
        , assignment_nonce_lo(uint32_t(nonce))
        , compile_input(input)
        , disposition(result)
        , wire_payload_valid(true) {}

    ResultDispositionMsg(const CompileJob &job, Disposition result)
        : ResultDispositionMsg(job.jobID(), job.assignmentEpoch(),
                               job.assignmentNonce(),
                               job.compileInputIdentity(), result) {}

    void fill_from_channel(MsgChannel *c) override;
    void send_to_channel(MsgChannel *c) const override;
    bool valid_payload() const override;
    bool valid_for_protocol(int negotiated_protocol) const override
    {
        return negotiated_protocol == PROTOCOL_VERSION_RESULT_DISPOSITION;
    }

    uint64_t assignmentEpoch() const
    {
        return (uint64_t(assignment_epoch_hi) << 32) | assignment_epoch_lo;
    }
    uint64_t assignmentNonce() const
    {
        return (uint64_t(assignment_nonce_hi) << 32) | assignment_nonce_lo;
    }

    CompileInputIdentity &compileInputIdentity() { return compile_input; }
    const CompileInputIdentity &compileInputIdentity() const { return compile_input; }

    /* Equality intentionally includes every identity dimension.  Ordinary
       duplicate frames are valid and compare equal; deduplication belongs to
       the result owner, not this stateless wire codec. */
    bool same_identity(const ResultDispositionMsg &other) const
    {
        return job_id == other.job_id
            && assignmentEpoch() == other.assignmentEpoch()
            && assignmentNonce() == other.assignmentNonce()
            && compile_input.profile == other.compile_input.profile
            && compile_input.c_store_guid == other.compile_input.c_store_guid
            && compile_input.tu_seq == other.compile_input.tu_seq
            && compile_input.raw_bytes == other.compile_input.raw_bytes
            && compile_input.raw_digest == other.compile_input.raw_digest
            && compile_input.attempt_id == other.compile_input.attempt_id
            && compile_input.request_id == other.compile_input.request_id;
    }

    bool operator==(const ResultDispositionMsg &other) const
    {
        return same_identity(other) && disposition == other.disposition;
    }

    uint32_t job_id;
    uint32_t assignment_epoch_hi;
    uint32_t assignment_epoch_lo;
    uint32_t assignment_nonce_hi;
    uint32_t assignment_nonce_lo;
    CompileInputIdentity compile_input;
    uint32_t disposition;

private:
    bool wire_payload_valid;
};

class JobBeginMsg : public Msg
{
public:
    JobBeginMsg()
        : Msg(Msg::JOB_BEGIN)
        , client_count(0) {}

    JobBeginMsg(unsigned int j, unsigned int _client_count)
        : Msg(Msg::JOB_BEGIN)
        , job_id(j)
        , stime(time(0))
        , client_count(_client_count) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    uint32_t job_id;
    uint32_t stime;
    uint32_t client_count; // number of CS -> C connections at the moment
};

class JobDoneMsg : public Msg
{
public:
    /* FROM_SERVER: this message was generated by the daemon responsible
          for remotely compiling the job (i.e. job->server).
       FROM_SUBMITTER: this message was generated by the daemon connected
          to the submitting client.  */
    enum from_type {
        FROM_SERVER = 0,
        FROM_SUBMITTER = 1
    };

    // other flags
    enum {
        UnknownJobId = (1 << 1),
        /* Local protocol-50 C-wrapper -> C-daemon observation only.  The
           daemon consumes this message and never forwards it to S: the F
           daemon remains the sole scheduler completion authority for a
           remotely compiled job. */
        P50CacheRouteObservation = (1 << 2),
        /* Set only on a failed local cache-route observation when the C
           sidecar proved that the selected P29V1 implementation cannot be
           initialized for this exact READY lease.  This is deliberately not
           a generic transfer-error bit: the daemon may suppress P29V1 until
           lease replacement only for this typed condition. */
        P50PermanentLocalCapabilityFailure = (1 << 3),
        /* The authenticated C sidecar completed the local control reply but
           its retained C/F route is no longer safe for a distinct request.
           The C daemon consumes this local-only fact and requests a cold
           sidecar replacement after the reply/Goodbye exchange. */
        P50LocalSidecarReplacementRequired = (1 << 4)
    };

    JobDoneMsg(int job_id = 0, int exitcode = -1, unsigned int flags = FROM_SERVER,
               unsigned int _client_count = 0, uint64_t assignment_epoch = 0,
               uint64_t assignment_nonce = 0, uint64_t c_guid = 0,
               uint64_t tu_seq = 0);

    void set_from(from_type from)
    {
        flags |= (uint32_t)from;
    }

    bool is_from_server() const
    {
        return (flags & FROM_SUBMITTER) == 0;
    }

    bool is_p50_cache_route_observation() const
    {
        return (flags & P50CacheRouteObservation) != 0;
    }

    void set_unknown_job_client_id( uint32_t clientId );
    uint32_t unknown_job_client_id() const;
    void set_job_id( uint32_t jobId );

    void setAssignmentIdentity(uint64_t epoch, uint64_t nonce)
    {
        assignment_epoch_hi = uint32_t(epoch >> 32);
        assignment_epoch_lo = uint32_t(epoch);
        assignment_nonce_hi = uint32_t(nonce >> 32);
        assignment_nonce_lo = uint32_t(nonce);
    }
    void setCompileIdentity(uint64_t c_guid, uint64_t tu_seq)
    {
        c_guid_hi = uint32_t(c_guid >> 32);
        c_guid_lo = uint32_t(c_guid);
        tu_seq_hi = uint32_t(tu_seq >> 32);
        tu_seq_lo = uint32_t(tu_seq);
    }
    uint64_t assignmentEpoch() const
    { return (uint64_t(assignment_epoch_hi) << 32) | assignment_epoch_lo; }
    uint64_t assignmentNonce() const
    { return (uint64_t(assignment_nonce_hi) << 32) | assignment_nonce_lo; }
    uint64_t cGuid() const { return (uint64_t(c_guid_hi) << 32) | c_guid_lo; }
    uint64_t tuSeq() const { return (uint64_t(tu_seq_hi) << 32) | tu_seq_lo; }

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    uint32_t real_msec; /* real time it used */
    uint32_t user_msec; /* user time used */
    uint32_t sys_msec; /* system time used */
    uint32_t pfaults; /* page faults */

    int exitcode; /* exit code */

    uint32_t flags;

    uint32_t in_compressed;
    uint32_t in_uncompressed;
    uint32_t out_compressed;
    uint32_t out_uncompressed;

    uint32_t job_id;
    uint32_t client_count; // number of CS -> C connections at the moment
    uint32_t assignment_epoch_hi, assignment_epoch_lo;
    uint32_t assignment_nonce_hi, assignment_nonce_lo;
    uint32_t c_guid_hi, c_guid_lo;
    uint32_t tu_seq_hi, tu_seq_lo;
};

enum class P50CacheRouteObservationKind : uint8_t {
    Invalid = 0,
    Success,
    Failure,
    PermanentLocalProfileFailure,
    LocalSidecarReplacementRequired,
};

/* Classify only the wrapper's local-only observation for one exact UseCS.
   Ordinary JobDone settlement is never accepted here.  All five assignment
   identity components are checked; TU sequence zero remains a valid first
   sequence.  A permanent observation is meaningful only for the singleton
   P29V1 profile and a nonzero failure exit code. */
inline P50CacheRouteObservationKind p50_cache_route_observation_kind(
    const JobDoneMsg& completion, uint32_t expected_job_id,
    uint64_t expected_epoch, uint64_t expected_nonce,
    uint64_t expected_c_guid, uint64_t expected_tu_seq,
    uint32_t expected_profile_mask) noexcept
{
    const uint32_t base_flags =
        static_cast<uint32_t>(JobDoneMsg::FROM_SUBMITTER) |
        static_cast<uint32_t>(JobDoneMsg::P50CacheRouteObservation);
    const uint32_t permanent_flags =
        base_flags |
        static_cast<uint32_t>(
            JobDoneMsg::P50PermanentLocalCapabilityFailure);
    const uint32_t replacement_flags =
        base_flags |
        static_cast<uint32_t>(
            JobDoneMsg::P50LocalSidecarReplacementRequired);
    if (expected_job_id == 0 || expected_epoch == 0 ||
        expected_nonce == 0 || expected_c_guid == 0 ||
        completion.job_id != expected_job_id ||
        completion.assignmentEpoch() != expected_epoch ||
        completion.assignmentNonce() != expected_nonce ||
        completion.cGuid() != expected_c_guid ||
        completion.tuSeq() != expected_tu_seq)
        return P50CacheRouteObservationKind::Invalid;
    if (completion.flags == base_flags) {
        return completion.exitcode == 0
            ? P50CacheRouteObservationKind::Success
            : P50CacheRouteObservationKind::Failure;
    }
    if (completion.flags == permanent_flags && completion.exitcode != 0 &&
        expected_profile_mask == CACHE_PROFILE_P29V1)
        return P50CacheRouteObservationKind::PermanentLocalProfileFailure;
    if (completion.flags == replacement_flags && completion.exitcode != 0)
        return P50CacheRouteObservationKind::LocalSidecarReplacementRequired;
    return P50CacheRouteObservationKind::Invalid;
}

inline bool p50_cache_handoff_completion_matches(
    const JobDoneMsg& completion, uint32_t expected_job_id,
    uint64_t expected_epoch, uint64_t expected_nonce,
    uint64_t expected_c_guid, uint64_t expected_tu_seq,
    uint32_t expected_profile_mask) noexcept
{
    return p50_cache_route_observation_kind(
               completion, expected_job_id, expected_epoch, expected_nonce,
               expected_c_guid, expected_tu_seq, expected_profile_mask) ==
           P50CacheRouteObservationKind::Success;
}

class JobLocalBeginMsg : public Msg
{
public:
    enum LocalFlags {
        LocalFlagNone = 0,
        LocalFlagPreprocessOnly = 1 << 0
    };

    JobLocalBeginMsg(int job_id = 0, const std::string &file = "", bool full = false,
                     const std::string &reason = "", const std::string &_cmdline = "",
                     uint32_t _local_flags = LocalFlagNone)
        : Msg(Msg::JOB_LOCAL_BEGIN)
        , outfile(file)
        , stime(time(0))
        , id(job_id)
        , fulljob(full)
        , local_reason(reason)
        , cmdline(_cmdline)
        , local_flags(_local_flags) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    std::string outfile;
    uint32_t stime;
    uint32_t id;
    bool fulljob;
    std::string local_reason;
    std::string cmdline;
    uint32_t local_flags;
};

class JobLocalDoneMsg : public Msg
{
public:
    JobLocalDoneMsg(unsigned int id = 0)
        : Msg(Msg::JOB_LOCAL_DONE)
        , job_id(id) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    uint32_t job_id;
};

class JobTimingMsg : public Msg
{
public:
    JobTimingMsg()
        : Msg(Msg::JOB_TIMING)
        , submit_ts(0)
        , enqueue_msec(0)
        , start_msec(0)
        , finish_msec(0)
        , waitforcs_msec(0)
        , local_queue_msec(0)
        , exec_msec(0)
        , scheduler_job_id(0)
        , compile_job_id(0)
        , exitcode(0) {}

    JobTimingMsg(uint32_t _submit_ts, uint32_t _enqueue_msec, uint32_t _start_msec, uint32_t _finish_msec,
                 uint32_t _waitforcs_msec, uint32_t _local_queue_msec, uint32_t _exec_msec,
                 uint32_t _scheduler_job_id, uint32_t _compile_job_id, int _exitcode,
                 const std::string &_mode)
        : Msg(Msg::JOB_TIMING)
        , submit_ts(_submit_ts)
        , enqueue_msec(_enqueue_msec)
        , start_msec(_start_msec)
        , finish_msec(_finish_msec)
        , waitforcs_msec(_waitforcs_msec)
        , local_queue_msec(_local_queue_msec)
        , exec_msec(_exec_msec)
        , scheduler_job_id(_scheduler_job_id)
        , compile_job_id(_compile_job_id)
        , exitcode(_exitcode)
        , mode(_mode) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    uint32_t submit_ts;
    uint32_t enqueue_msec;
    uint32_t start_msec;
    uint32_t finish_msec;
    uint32_t waitforcs_msec;
    uint32_t local_queue_msec;
    uint32_t exec_msec;
    uint32_t scheduler_job_id;
    uint32_t compile_job_id;
    int exitcode;
    std::string mode;
};

class LoginMsg : public Msg
{
public:
    LoginMsg(unsigned int myport, const std::string &_nodename, const std::string &_host_platform,
             unsigned int my_features);
    LoginMsg()
        : Msg(Msg::LOGIN)
        , port(0)
        , cache_endpoint_port(0)
        , cache_protocol(0)
        , cache_profile_mask(0)
        , cache_advertisement_tail_valid(true) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;
    bool valid_payload() const override;

    void setCacheAdvertisement(uint32_t endpoint_port, uint32_t protocol,
                               uint32_t profiles)
    {
        cache_endpoint_port = endpoint_port;
        cache_protocol = protocol;
        cache_profile_mask = profiles;
        cache_advertisement_tail_valid = true;
    }
    bool hasCacheAdvertisement() const { return cache_endpoint_port != 0; }

    uint32_t port;
    Environments envs;
    uint32_t max_kids;
    bool noremote;
    bool chroot_possible;
    std::string nodename;
    std::string host_platform;
    uint32_t supported_features; // bitmask of various features the node supports
    /* Protocol-50 Login tail.  Host identity is the scheduler-observed peer
       address; store GUID and session limits remain CacheWire handshake data. */
    uint32_t cache_endpoint_port;
    uint32_t cache_protocol;
    uint32_t cache_profile_mask;

private:
    bool cache_advertisement_tail_valid;
};

class ConfCSMsg : public Msg
{
public:
    enum FenceMode : uint32_t {
        Legacy = 0,
        Advisory = 1,
        EnforcingCompat = 2,
        StrictNonce = 3
    };

    ConfCSMsg()
        : Msg(Msg::CS_CONF)
        , max_scheduler_pong(MAX_SCHEDULER_PONG)
        , max_scheduler_ping(MAX_SCHEDULER_PING)
        , epoch_hi(0)
        , epoch_lo(0)
        , fence_mode(Legacy) {}

    ConfCSMsg(uint64_t epoch, FenceMode mode)
        : Msg(Msg::CS_CONF)
        , max_scheduler_pong(MAX_SCHEDULER_PONG)
        , max_scheduler_ping(MAX_SCHEDULER_PING)
        , epoch_hi(uint32_t(epoch >> 32))
        , epoch_lo(uint32_t(epoch))
        , fence_mode(mode) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    uint32_t max_scheduler_pong;
    uint32_t max_scheduler_ping;
    uint32_t epoch_hi;
    uint32_t epoch_lo;
    uint32_t fence_mode;

    uint64_t epoch() const
    {
        return (uint64_t(epoch_hi) << 32) | epoch_lo;
    }
};

class AssignPrepareMsg : public Msg
{
public:
    AssignPrepareMsg()
        : Msg(Msg::ASSIGN_PREPARE)
        , epoch_hi(0), epoch_lo(0), wire_id(0)
        , nonce_hi(0), nonce_lo(0), submitter_hostid(0), flags(0) {}
    AssignPrepareMsg(uint64_t epoch, uint32_t id, uint64_t nonce,
                     uint32_t submitter, uint32_t assignment_flags = 0)
        : Msg(Msg::ASSIGN_PREPARE)
        , epoch_hi(uint32_t(epoch >> 32)), epoch_lo(uint32_t(epoch))
        , wire_id(id)
        , nonce_hi(uint32_t(nonce >> 32)), nonce_lo(uint32_t(nonce))
        , submitter_hostid(submitter), flags(assignment_flags) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    uint64_t epoch() const { return (uint64_t(epoch_hi) << 32) | epoch_lo; }
    uint64_t nonce() const { return (uint64_t(nonce_hi) << 32) | nonce_lo; }

    uint32_t epoch_hi, epoch_lo;
    uint32_t wire_id;
    uint32_t nonce_hi, nonce_lo;
    uint32_t submitter_hostid;
    uint32_t flags;
};

class AssignReadyMsg : public Msg
{
public:
    AssignReadyMsg()
        : Msg(Msg::ASSIGN_READY), epoch_hi(0), epoch_lo(0), wire_id(0)
        , nonce_hi(0), nonce_lo(0) {}
    AssignReadyMsg(uint64_t epoch, uint32_t id, uint64_t nonce)
        : Msg(Msg::ASSIGN_READY), epoch_hi(uint32_t(epoch >> 32))
        , epoch_lo(uint32_t(epoch)), wire_id(id)
        , nonce_hi(uint32_t(nonce >> 32)), nonce_lo(uint32_t(nonce)) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;
    uint64_t epoch() const { return (uint64_t(epoch_hi) << 32) | epoch_lo; }
    uint64_t nonce() const { return (uint64_t(nonce_hi) << 32) | nonce_lo; }

    uint32_t epoch_hi, epoch_lo;
    uint32_t wire_id;
    uint32_t nonce_hi, nonce_lo;
};

class RevokeBeforeStartMsg : public Msg
{
public:
    RevokeBeforeStartMsg()
        : Msg(Msg::REVOKE_BEFORE_START), epoch_hi(0), epoch_lo(0), wire_id(0)
        , nonce_hi(0), nonce_lo(0) {}
    RevokeBeforeStartMsg(uint64_t epoch, uint32_t id, uint64_t nonce)
        : Msg(Msg::REVOKE_BEFORE_START), epoch_hi(uint32_t(epoch >> 32))
        , epoch_lo(uint32_t(epoch)), wire_id(id)
        , nonce_hi(uint32_t(nonce >> 32)), nonce_lo(uint32_t(nonce)) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;
    uint64_t epoch() const { return (uint64_t(epoch_hi) << 32) | epoch_lo; }
    uint64_t nonce() const { return (uint64_t(nonce_hi) << 32) | nonce_lo; }

    uint32_t epoch_hi, epoch_lo;
    uint32_t wire_id;
    uint32_t nonce_hi, nonce_lo;
};

class RevokeResultMsg : public Msg
{
public:
    enum Result : uint32_t {
        Revoked = 0,
        ClaimedOrLater = 1
    };

    RevokeResultMsg()
        : Msg(Msg::REVOKE_RESULT), epoch_hi(0), epoch_lo(0), wire_id(0)
        , nonce_hi(0), nonce_lo(0), result(Revoked) {}
    RevokeResultMsg(uint64_t epoch, uint32_t id, uint64_t nonce,
                    Result assignment_result)
        : Msg(Msg::REVOKE_RESULT), epoch_hi(uint32_t(epoch >> 32))
        , epoch_lo(uint32_t(epoch)), wire_id(id)
        , nonce_hi(uint32_t(nonce >> 32)), nonce_lo(uint32_t(nonce))
        , result(assignment_result) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;
    uint64_t epoch() const { return (uint64_t(epoch_hi) << 32) | epoch_lo; }
    uint64_t nonce() const { return (uint64_t(nonce_hi) << 32) | nonce_lo; }
    bool validResult() const
    {
        return result == Revoked || result == ClaimedOrLater;
    }

    uint32_t epoch_hi, epoch_lo;
    uint32_t wire_id;
    uint32_t nonce_hi, nonce_lo;
    uint32_t result;
};

class StatsMsg : public Msg
{
public:
    StatsMsg()
        : Msg(Msg::STATS)
        , load(0)
        , client_count(0)
    {
    }

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    /**
     * For now the only load measure we have is the
     * load from 0-1000.
     * This is defined to be a daemon defined value
     * on how busy the machine is. The higher the load
     * is, the slower a given job will compile (preferably
     * linear scale). Load of 1000 means to not schedule
     * another job under no circumstances.
     */
    uint32_t load;

    uint32_t loadAvg1;
    uint32_t loadAvg5;
    uint32_t loadAvg10;
    uint32_t freeMem;

    uint32_t client_count; // number of CS -> C connections at the moment
};

class EnvTransferMsg : public Msg
{
public:
    EnvTransferMsg()
        : Msg(Msg::TRANFER_ENV) {}

    EnvTransferMsg(const std::string &_target, const std::string &_name)
        : Msg(Msg::TRANFER_ENV)
        , name(_name)
        , target(_target) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    std::string name;
    std::string target;
};

class GetInternalStatus : public Msg
{
public:
    GetInternalStatus()
        : Msg(Msg::GET_INTERNALS) {}

    GetInternalStatus(const GetInternalStatus &)
        : Msg(Msg::GET_INTERNALS) {}
};

class MonLoginMsg : public Msg
{
public:
    MonLoginMsg()
        : Msg(Msg::MON_LOGIN) {}
};

class MonGetCSMsg : public GetCSMsg
{
public:
    MonGetCSMsg()
        : GetCSMsg()
    { // overwrite
        value_ = MON_GET_CS;
        clientid = job_id = 0;
    }

    MonGetCSMsg(int jobid, int hostid, const GetCSMsg *m)
        : GetCSMsg(Environments(), m->filename, m->lang, 1, m->target, 0, std::string(), false, m->client_count, m->niceness)
        , job_id(jobid)
        , clientid(hostid)
    {
        value_ = MON_GET_CS;
    }

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    uint32_t job_id;
    uint32_t clientid;
};

class MonJobBeginMsg : public Msg
{
public:
    MonJobBeginMsg()
        : Msg(Msg::MON_JOB_BEGIN)
        , job_id(0)
        , stime(0)
        , hostid(0) {}

    MonJobBeginMsg(unsigned int id, unsigned int time, int _hostid)
        : Msg(Msg::MON_JOB_BEGIN)
        , job_id(id)
        , stime(time)
        , hostid(_hostid) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    uint32_t job_id;
    uint32_t stime;
    uint32_t hostid;
};

class MonJobDoneMsg : public JobDoneMsg
{
public:
    MonJobDoneMsg()
        : JobDoneMsg()
    {
        value_ = MON_JOB_DONE;
    }

    MonJobDoneMsg(const JobDoneMsg &o)
        : JobDoneMsg(o)
    {
        value_ = MON_JOB_DONE;
    }
};

class MonLocalJobBeginMsg : public Msg
{
public:
    MonLocalJobBeginMsg()
        : Msg(Msg::MON_LOCAL_JOB_BEGIN) {}

    MonLocalJobBeginMsg(unsigned int id, const std::string &_file, unsigned int time, int _hostid)
        : Msg(Msg::MON_LOCAL_JOB_BEGIN)
        , job_id(id)
        , stime(time)
        , hostid(_hostid)
        , file(_file) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    uint32_t job_id;
    uint32_t stime;
    uint32_t hostid;
    std::string file;
};

class MonStatsMsg : public Msg
{
public:
    MonStatsMsg()
        : Msg(Msg::MON_STATS) {}

    MonStatsMsg(int id, const std::string &_statmsg)
        : Msg(Msg::MON_STATS)
        , hostid(id)
        , statmsg(_statmsg) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    uint32_t hostid;
    std::string statmsg;
};

class TextMsg : public Msg
{
public:
    TextMsg()
        : Msg(Msg::TEXT) {}

    TextMsg(const std::string &_text)
        : Msg(Msg::TEXT)
        , text(_text) {}

    TextMsg(const TextMsg &m)
        : Msg(Msg::TEXT)
        , text(m.text) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    std::string text;
};

class StatusTextMsg : public Msg
{
public:
    StatusTextMsg()
        : Msg(Msg::STATUS_TEXT) {}

    StatusTextMsg(const std::string &_text)
        : Msg(Msg::STATUS_TEXT)
        , text(_text) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    std::string text;
};

class VerifyEnvMsg : public Msg
{
public:
    VerifyEnvMsg()
        : Msg(Msg::VERIFY_ENV) {}

    VerifyEnvMsg(const std::string &_target, const std::string &_environment)
        : Msg(Msg::VERIFY_ENV)
        , environment(_environment)
        , target(_target) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    std::string environment;
    std::string target;
};

class VerifyEnvResultMsg : public Msg
{
public:
    VerifyEnvResultMsg()
        : Msg(Msg::VERIFY_ENV_RESULT) {}

    VerifyEnvResultMsg(bool _ok)
        : Msg(Msg::VERIFY_ENV_RESULT)
        , ok(_ok) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    bool ok;
};

class BlacklistHostEnvMsg : public Msg
{
public:
    BlacklistHostEnvMsg()
        : Msg(Msg::BLACKLIST_HOST_ENV) {}

    BlacklistHostEnvMsg(const std::string &_target, const std::string &_environment, const std::string &_hostname)
        : Msg(Msg::BLACKLIST_HOST_ENV)
        , environment(_environment)
        , target(_target)
        , hostname(_hostname) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    std::string environment;
    std::string target;
    std::string hostname;
};

#endif
