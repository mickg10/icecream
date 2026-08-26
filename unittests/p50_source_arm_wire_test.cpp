/* Focused Protocol-50 source-arm / exact armed-ACK wire tests.

   WIRE-AUDIT three-bucket classification:
   - BOUND: the discriminator; wire_job_id, assignment_epoch/nonce,
     selected_f_host/ordinary/cache ports, cache_protocol/profile,
     logical_job/compiler_attempt, C store generation/GUID, source request
     id/mode, separate C control generation/attempt; and on ACK the exact arm
     echo, F control generation/attempt, F StoreIdentity GUID/version, and
     nonzero arm observation all have strict validators and stable fixtures;
   - WIRE-PLACEHOLDER / DERIVED-GUARD: the fixed F role bit and derivation
     version are the only ordinary-wire projection of the later
     CSPRNG-backed StoreIdentity derivation; raw entropy/root is deliberately
     neither sent nor modelled on this ACK;
   - CURRENTLY MODEL-UNREPRESENTED: WAITP50INPUT ownership, CACHE_SESSION /
     private attachment, InputRecord/reverse-FD, cancellation, and lifecycle
     transitions have no fields or production caller in this bounded slice. */
#include "comm.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;
#define REQUIRE(condition, message) do { \
    const bool ok_ = (condition); \
    std::fprintf(stderr, "%s - %s\n", ok_ ? "ok    " : "FAILED", message); \
    if (!ok_) ++failures; \
} while (0)

struct Pair {
    MsgChannel *left = nullptr;
    MsgChannel *right = nullptr;
    Pair() = default;
    Pair(const Pair &) = delete;
    Pair &operator=(const Pair &) = delete;
    Pair(Pair &&other) noexcept : left(other.left), right(other.right)
    {
        other.left = other.right = nullptr;
    }
    ~Pair() { delete left; delete right; }
};

Pair make_pair(int protocol)
{
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
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
    if (!pair.left || !pair.right)
        std::exit(2);
    pair.left->protocol = pair.right->protocol = protocol;
    return pair;
}

using Bytes = std::vector<unsigned char>;

Bytes hex_fixture(const char *hex)
{
    Bytes bytes;
    const std::string text(hex);
    if (text.size() % 2 != 0)
        std::exit(2);
    bytes.reserve(text.size() / 2);
    for (size_t i = 0; i != text.size(); i += 2) {
        const auto nibble = [](char c) -> unsigned char {
            if (c >= '0' && c <= '9') return static_cast<unsigned char>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<unsigned char>(c - 'a' + 10);
            if (c >= 'A' && c <= 'F') return static_cast<unsigned char>(c - 'A' + 10);
            std::exit(2);
        };
        bytes.push_back(static_cast<unsigned char>((nibble(text[i]) << 4) |
                                                   nibble(text[i + 1])));
    }
    return bytes;
}

P50SourceArmFields arm()
{
    P50SourceArmFields value;
    value.wire_job_id = 7;
    value.assignment_epoch = UINT64_C(0x0102030405060708);
    value.assignment_nonce = UINT64_C(0x1112131415161718);
    value.selected_f_host = "worker.example";
    value.selected_f_ordinary_port = 10245;
    value.selected_f_cache_port = 10246;
    value.cache_protocol = CACHE_WIRE_PROTOCOL_V1;
    value.cache_profile = CACHE_PROFILE_ZSTD_TU;
    value.logical_job = 19;
    value.compiler_attempt = UINT64_C(0x2122232425262728);
    value.c_store_generation = UINT64_C(0x3132333435363738);
    value.c_store_derivation_version = icecc::p50::kStoreIdentityDerivationVersion;
    for (size_t i = 0; i != value.c_store_guid.size(); ++i)
        value.c_store_guid[i] = static_cast<uint8_t>(0x40 + i);
    value.source_request_id = UINT64_C(0x5152535455565758);
    value.source_mode = P50_SOURCE_MODE_ZSTD_TU;
    value.c_control_generation = UINT64_C(0x6162636465666768);
    value.c_control_attempt = UINT64_C(0x7172737475767778);
    return value;
}

P50SourceArmedMsg armed(const P50SourceArmFields &source_arm)
{
    std::array<uint8_t, 16> f_guid{};
    for (size_t i = 0; i != f_guid.size(); ++i)
        f_guid[i] = static_cast<uint8_t>(0xd0 + i);
    f_guid[icecc::p50::kStoreIdentityRoleByte] |=
        icecc::p50::kStoreIdentityFileRole;
    return P50SourceArmedMsg(source_arm, UINT64_C(0x8182838485868788),
                             UINT64_C(0x9192939495969798), f_guid,
                             icecc::p50::kStoreIdentityDerivationVersion,
                             UINT64_C(0xa1a2a3a4a5a6a7a8));
}

Bytes encode_frame(const Msg &message, int protocol = PROTOCOL_VERSION)
{
    Pair pair = make_pair(protocol);
    if (!pair.left->send_msg(message))
        return {};
    uint32_t network_length = 0;
    if (::recv(pair.right->fd, &network_length, sizeof(network_length), MSG_WAITALL)
            != ssize_t(sizeof(network_length)))
        return {};
    const size_t body = ntohl(network_length);
    Bytes frame(sizeof(network_length) + body);
    std::memcpy(frame.data(), &network_length, sizeof(network_length));
    if (::recv(pair.right->fd, frame.data() + sizeof(network_length), body,
               MSG_WAITALL) != ssize_t(body))
        return {};
    return frame;
}

Msg *decode_frame(const Bytes &frame, int protocol = PROTOCOL_VERSION)
{
    Pair pair = make_pair(protocol);
    if (::send(pair.left->fd, frame.data(), frame.size(), 0)
            != ssize_t(frame.size()))
        return nullptr;
    return pair.right->get_msg(2, true);
}

void put_u32(Bytes &bytes, size_t offset, uint32_t value)
{
    const uint32_t network = htonl(value);
    std::memcpy(bytes.data() + offset, &network, sizeof(network));
}

void test_roundtrip_and_exact_echo()
{
    static_assert(Msg::P50_SOURCE_ARM == UINT32_C(0x50f00010));
    static_assert(Msg::P50_SOURCE_ARMED == UINT32_C(0x50f00011));

    const P50SourceArmMsg request(arm());
    const Bytes request_wire = encode_frame(request);
    REQUIRE(!request_wire.empty(), "source-arm message encodes on Protocol 50");
    REQUIRE(request_wire == hex_fixture(
                "0000008750f000100000000701020304050607081112131415161718"
                "0000000f776f726b65722e6578616d706c650000002805000028060000"
                "003200000002000000000000001321222324252627283132333435363738"
                "0000000000000001404142434445464748494a4b4c4d4e4f515253545556"
                "575800000001"
                "61626364656667687172737475767778"),
            "source-arm stable Protocol-50 fixture bytes remain unchanged");
    Msg *request_base = decode_frame(request_wire);
    auto *decoded_request = dynamic_cast<P50SourceArmMsg *>(request_base);
    REQUIRE(decoded_request && decoded_request->arm == request.arm,
            "source-arm round-trip preserves every field");

    const P50SourceArmedMsg reply = armed(request.arm);
    const Bytes reply_wire = encode_frame(reply);
    REQUIRE(reply_wire == hex_fixture(
                "000000b750f000110000000701020304050607081112131415161718"
                "0000000f776f726b65722e6578616d706c650000002805000028060000"
                "003200000002000000000000001321222324252627283132333435363738"
                "0000000000000001404142434445464748494a4b4c4d4e4f515253545556"
                "575800000001"
                "616263646566676871727374757677788182838485868788919293949596"
                "9798d0d1d2d3d4d5d6d7d8d9dadbdcdddedf0000000000000001"
                "a1a2a3a4a5a6a7a8"),
            "armed ACK stable Protocol-50 fixture bytes remain unchanged");
    Msg *reply_base = decode_frame(reply_wire);
    auto *decoded_reply = dynamic_cast<P50SourceArmedMsg *>(reply_base);
    REQUIRE(decoded_reply && decoded_reply->arm == request.arm,
            "armed ACK echoes the complete source arm");
    REQUIRE(decoded_reply && decoded_reply->acknowledges(request),
            "armed ACK validates against the exact original request");
    REQUIRE(decoded_reply && decoded_reply->arm_observation_id != 0 &&
                decoded_reply->f_control_generation != 0 &&
                decoded_reply->f_control_attempt != 0,
            "armed ACK carries fresh observation and F control launch");
    delete request_base;
    delete reply_base;
}

void test_rejects_malformed_and_legacy()
{
    P50SourceArmMsg request(arm());
    P50SourceArmFields oversized = request.arm;
    oversized.selected_f_host.assign(256, 'x');
    REQUIRE(!make_pair(PROTOCOL_VERSION).left->send_msg(
                P50SourceArmMsg(oversized)),
            "oversized selected-F host is refused before framing");

    Bytes trailing = encode_frame(request);
    REQUIRE(!trailing.empty(), "valid source-arm fixture exists for mutation");
    const uint32_t old_body = ntohl(*reinterpret_cast<const uint32_t *>(trailing.data()));
    trailing.push_back(0);
    put_u32(trailing, 0, old_body + 1);
    Msg *trailing_decoded = decode_frame(trailing);
    REQUIRE(trailing_decoded == nullptr,
            "source-arm decoder rejects trailing payload bytes");
    delete trailing_decoded;

    Bytes unterminated = encode_frame(request);
    /* Frame length (4), message type (4), fixed prefix through the host
       length (20), then the host bytes including its terminating NUL. */
    const size_t host_terminator = 4 + 4 + 4 + 8 + 8 + 4 +
                                   request.arm.selected_f_host.size();
    unterminated[host_terminator] = 'X';
    Msg *unterminated_decoded = decode_frame(unterminated);
    REQUIRE(unterminated_decoded == nullptr,
            "source-arm decoder requires a bounded NUL-terminated host");
    delete unterminated_decoded;

    P50SourceArmedMsg bad_role = armed(request.arm);
    bad_role.f_store_guid[icecc::p50::kStoreIdentityRoleByte] &=
        static_cast<uint8_t>(~icecc::p50::kStoreIdentityRoleMask);
    REQUIRE(!make_pair(PROTOCOL_VERSION).left->send_msg(bad_role),
            "missing F role bit is refused before framing");
    P50SourceArmedMsg bad_version = armed(request.arm);
    bad_version.f_store_derivation_version = 2;
    REQUIRE(!make_pair(PROTOCOL_VERSION).left->send_msg(bad_version),
            "unknown StoreIdentity derivation version is refused");
    P50SourceArmFields bad_c_version = request.arm;
    bad_c_version.c_store_derivation_version =
        icecc::p50::kStoreIdentityDerivationVersion + 1;
    REQUIRE(!make_pair(PROTOCOL_VERSION).left->send_msg(
                P50SourceArmMsg(bad_c_version)),
            "unknown C StoreIdentity derivation version is refused");
    P50SourceArmedMsg no_observation = armed(request.arm);
    no_observation.arm_observation_id = 0;
    REQUIRE(!make_pair(PROTOCOL_VERSION).left->send_msg(no_observation),
            "zero arm observation is refused before framing");

    P50SourceArmFields unknown_profile = request.arm;
    unknown_profile.cache_profile = CACHE_PROFILE_P29;
    REQUIRE(!make_pair(PROTOCOL_VERSION).left->send_msg(
                P50SourceArmMsg(unknown_profile)),
            "non-advertisable cache profile is refused before framing");
    P50SourceArmFields unknown_mode = request.arm;
    unknown_mode.source_mode = P50_SOURCE_MODE_ZSTD_TU + 1;
    REQUIRE(!make_pair(PROTOCOL_VERSION).left->send_msg(
                P50SourceArmMsg(unknown_mode)),
            "unknown source mode is refused before framing");

    P50SourceArmFields c_role_alias = request.arm;
    c_role_alias.c_store_guid[icecc::p50::kStoreIdentityRoleByte] |=
        icecc::p50::kStoreIdentityRoleMask;
    REQUIRE(!make_pair(PROTOCOL_VERSION).left->send_msg(
                P50SourceArmMsg(c_role_alias)),
            "C StoreIdentity with the F role bit is refused");

    REQUIRE(!make_pair(PROTOCOL_VERSION - 1).left->send_msg(request),
            "source-arm is refused on legacy Protocol 49");
    REQUIRE(!make_pair(PROTOCOL_VERSION - 1).left->send_msg(armed(request.arm)),
            "armed ACK is refused on legacy Protocol 49");
}

void test_ack_conflict()
{
    const P50SourceArmMsg request(arm());
    P50SourceArmedMsg wrong = armed(request.arm);
    wrong.arm.source_request_id++;
    REQUIRE(!wrong.acknowledges(request),
            "ACK with a mutated source request cannot authorize the arm");
    wrong = armed(request.arm);
    wrong.f_store_guid = request.arm.c_store_guid;
    REQUIRE(!wrong.acknowledges(request),
            "ACK with a C/F GUID alias cannot authorize the arm");
    wrong = armed(request.arm);
    wrong.f_store_guid[1] ^= 1;
    REQUIRE(wrong.acknowledges(request),
            "independent F sidecar StoreIdentity root is accepted");
}

} // namespace

int main()
{
    test_roundtrip_and_exact_echo();
    test_rejects_malformed_and_legacy();
    test_ack_conflict();
    return failures == 0 ? 0 : 1;
}
