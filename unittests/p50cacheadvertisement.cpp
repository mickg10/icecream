/* Inert Protocol-50 Login cache-advertisement gate.  All frames use the
   production MsgChannel/LoginMsg codec; old-version hashes are retained from
   the exact pre-advertisement convergence parent. */

#include "comm.h"
#include "cache/protocol50.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

using Bytes = std::vector<unsigned char>;
using namespace icecc::p50;

static int failures = 0;

#define REQUIRE(cond, text) do {                                      \
    if (cond) { std::fprintf(stderr, "ok       - %s\n", text); }      \
    else { std::fprintf(stderr, "FAILED   - %s\n", text); ++failures; } \
} while (0)

struct Pair {
    MsgChannel *left = nullptr;
    MsgChannel *right = nullptr;
    Pair() = default;
    Pair(const Pair&) = delete;
    Pair& operator=(const Pair&) = delete;
    Pair(Pair&& other) noexcept : left(other.left), right(other.right) {
        other.left = other.right = nullptr;
    }
    ~Pair() { delete left; delete right; }
};

static Pair make_pair(int protocol)
{
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) std::exit(2);
    sockaddr_un address {};
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
    if (!pair.left || !pair.right) std::exit(2);
    pair.left->protocol = pair.right->protocol = protocol;
    return pair;
}

static LoginMsg fixture_login()
{
    LoginMsg login(UINT32_C(0x00002805), "cache-node", "x86_64",
                   UINT32_C(0x00000003));
    login.envs.push_back(std::make_pair(std::string("x86_64"),
                                        std::string("cache-env")));
    login.max_kids = UINT32_C(0x01020304);
    login.noremote = false;
    login.chroot_possible = true;
    login.setCacheAdvertisement(UINT32_C(0x0000beef), CACHE_WIRE_PROTOCOL_V1,
                                CACHE_PROFILE_ZSTD_TU);
    return login;
}

static Bytes encode_frame(int protocol, const LoginMsg& login)
{
    Pair pair = make_pair(protocol);
    if (!pair.left->send_msg(login)) return {};
    uint32_t network_length = 0;
    if (recv(pair.right->fd, &network_length, sizeof(network_length), MSG_WAITALL)
            != static_cast<ssize_t>(sizeof(network_length))) {
        return {};
    }
    const uint32_t length = ntohl(network_length);
    Bytes bytes(sizeof(network_length) + length);
    std::memcpy(bytes.data(), &network_length, sizeof(network_length));
    if (recv(pair.right->fd, bytes.data() + sizeof(network_length), length,
             MSG_WAITALL) != static_cast<ssize_t>(length)) {
        return {};
    }
    return bytes;
}

static uint64_t fnv1a(const Bytes& bytes)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned char byte : bytes) {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void set_tail_word(Bytes& bytes, size_t words_from_end, uint32_t value)
{
    if (bytes.size() < words_from_end * 4) return;
    value = htonl(value);
    std::memcpy(bytes.data() + bytes.size() - words_from_end * 4,
                &value, sizeof(value));
}

static Bytes remove_tail_words(Bytes bytes, size_t words)
{
    const size_t removed = words * sizeof(uint32_t);
    if (bytes.size() < sizeof(uint32_t) + removed) return {};
    uint32_t network_length = 0;
    std::memcpy(&network_length, bytes.data(), sizeof(network_length));
    const uint32_t length = ntohl(network_length);
    if (length < removed) return {};
    bytes.resize(bytes.size() - removed);
    network_length = htonl(length - static_cast<uint32_t>(removed));
    std::memcpy(bytes.data(), &network_length, sizeof(network_length));
    return bytes;
}

static bool decoder_rejects(const Bytes& bytes)
{
    Pair pair = make_pair(50);
    const bool wrote = !bytes.empty()
        && send(pair.left->fd, bytes.data(), bytes.size(), 0)
            == static_cast<ssize_t>(bytes.size());
    Msg *decoded = wrote ? pair.right->get_msg(2, true) : nullptr;
    const bool rejected = wrote && !decoded;
    delete decoded;
    return rejected;
}

static void test_legacy_bytes()
{
    const LoginMsg login = fixture_login();
    const Bytes p43 = encode_frame(43, login);
    const Bytes p48 = encode_frame(48, login);
    const Bytes p49 = encode_frame(49, login);
    const uint64_t h43 = fnv1a(p43);
    const uint64_t h48 = fnv1a(p48);
    const uint64_t h49 = fnv1a(p49);
    std::fprintf(stderr, "Login fixture hashes: P43=%016llx P48=%016llx P49=%016llx\n",
                 static_cast<unsigned long long>(h43),
                 static_cast<unsigned long long>(h48),
                 static_cast<unsigned long long>(h49));
    REQUIRE(!p43.empty() && h43 == UINT64_C(0x807c97a21d147a7c),
            "P43 Login matches its retained pre-advertisement fixture");
    REQUIRE(!p48.empty() && h48 == UINT64_C(0x807c97a21d147a7c),
            "P48 Login matches its retained pre-advertisement fixture");
    REQUIRE(!p49.empty() && h49 == UINT64_C(0x807c97a21d147a7c),
            "P49 Login matches its retained pre-advertisement fixture");

    Pair old_pair = make_pair(49);
    REQUIRE(old_pair.left->send_msg(login),
            "populated advertisement object emits on a P49 link");
    Msg *wire = old_pair.right->get_msg(2, true);
    LoginMsg *decoded = dynamic_cast<LoginMsg *>(wire);
    REQUIRE(decoded && !decoded->hasCacheAdvertisement()
                && decoded->cache_protocol == 0
                && decoded->cache_profile_mask == 0,
            "P49 decoder receives canonical cache absence");
    delete wire;
}

static void test_p50_round_trip_and_validation()
{
    LoginMsg login = fixture_login();
    const Bytes p49 = encode_frame(49, login);
    const Bytes p50 = encode_frame(50, login);
    REQUIRE(p50.size() == p49.size() + 3 * sizeof(uint32_t)
                && std::equal(p49.begin() + sizeof(uint32_t), p49.end(),
                              p50.begin() + sizeof(uint32_t)),
            "P50 Login appends exactly three words to the unchanged P49 body");
    Pair pair = make_pair(50);
    REQUIRE(pair.left->send_msg(login), "P50 Login advertises a valid endpoint");
    Msg *wire = pair.right->get_msg(2, true);
    LoginMsg *decoded = dynamic_cast<LoginMsg *>(wire);
    REQUIRE(decoded && decoded->cache_endpoint_port == UINT32_C(0x0000beef)
                && decoded->cache_protocol == CACHE_WIRE_PROTOCOL_V1
                && decoded->cache_profile_mask == CACHE_PROFILE_ZSTD_TU,
            "P50 Login round-trips the exact three-word advertisement");
    delete wire;

    LoginMsg absent = fixture_login();
    absent.setCacheAdvertisement(0, 0, 0);
    Pair absent_pair = make_pair(50);
    REQUIRE(absent_pair.left->send_msg(absent),
            "P50 Login accepts canonical all-zero absence");
    wire = absent_pair.right->get_msg(2, true);
    decoded = dynamic_cast<LoginMsg *>(wire);
    REQUIRE(decoded && !decoded->hasCacheAdvertisement()
                && decoded->cache_protocol == 0
                && decoded->cache_profile_mask == 0,
            "P50 absent advertisement remains wholly zero");
    delete wire;

    const struct Invalid {
        uint32_t port;
        uint32_t protocol;
        uint32_t profiles;
        const char *name;
    } invalid[] = {
        {0, CACHE_WIRE_PROTOCOL_V1, CACHE_PROFILE_ZSTD_TU,
         "zero port with positive capability"},
        {70000, CACHE_WIRE_PROTOCOL_V1, CACHE_PROFILE_ZSTD_TU,
         "port outside TCP range"},
        {10245, 0, CACHE_PROFILE_ZSTD_TU, "missing cache protocol"},
        {10245, CACHE_WIRE_PROTOCOL_V1, 0, "missing cache profile"},
        {10245, CACHE_WIRE_PROTOCOL_V1, CACHE_PROFILE_P29,
         "non-runnable P29 endpoint"},
        {10245, CACHE_WIRE_PROTOCOL_V1, CACHE_PROFILE_Z3_LONG,
         "declared-only z3_long endpoint"},
        {10245, CACHE_WIRE_PROTOCOL_V1, CACHE_PROFILE_Z3_SHARED_LONG,
         "declared-only z3_shared_long endpoint"},
        {10245, CACHE_WIRE_PROTOCOL_V1, UINT32_C(0x80000000),
         "unknown profile bit"},
    };
    for (const Invalid& value : invalid) {
        LoginMsg malformed = fixture_login();
        malformed.setCacheAdvertisement(value.port, value.protocol, value.profiles);
        Pair send_pair = make_pair(50);
        char label[160];
        std::snprintf(label, sizeof(label), "encoder rejects %s", value.name);
        REQUIRE(!send_pair.left->send_msg(malformed), label);
    }

    const Bytes valid = p50;
    REQUIRE(decoder_rejects(remove_tail_words(valid, 3)),
            "P50 decoder rejects a wholly omitted advertisement tail");
    REQUIRE(decoder_rejects(remove_tail_words(valid, 2)),
            "P50 decoder rejects a one-word advertisement tail");
    REQUIRE(decoder_rejects(remove_tail_words(valid, 1)),
            "P50 decoder rejects a two-word advertisement tail");
    Bytes malformed = valid;
    set_tail_word(malformed, 3, 0);
    REQUIRE(decoder_rejects(malformed),
            "decoder rejects zero port with positive capability");
    malformed = valid;
    set_tail_word(malformed, 2, 0);
    REQUIRE(decoder_rejects(malformed),
            "decoder rejects partial protocol absence");
    malformed = valid;
    set_tail_word(malformed, 1, CACHE_PROFILE_Z3_LONG);
    REQUIRE(decoder_rejects(malformed),
            "decoder rejects declared-only profile advertisement");
    malformed = valid;
    set_tail_word(malformed, 1, UINT32_C(0x80000000));
    REQUIRE(decoder_rejects(malformed),
            "decoder rejects unknown profile advertisement");
}

static void test_declared_profiles_are_inert()
{
    static_assert(static_cast<uint16_t>(ProfileId::P29) == 1);
    static_assert(static_cast<uint16_t>(ProfileId::ZSTD_TU) == 2);
    static_assert(static_cast<uint16_t>(ProfileId::GRZ) == 3);
    static_assert(static_cast<uint16_t>(ProfileId::Z3_LONG) == 4);
    static_assert(static_cast<uint16_t>(ProfileId::Z3_SHARED_LONG) == 5);
    static_assert(profile_bit(ProfileId::P29) == CACHE_PROFILE_P29);
    static_assert(profile_bit(ProfileId::ZSTD_TU) == CACHE_PROFILE_ZSTD_TU);
    static_assert(profile_bit(ProfileId::GRZ) == CACHE_PROFILE_GRZ);
    static_assert(profile_bit(ProfileId::Z3_LONG) == CACHE_PROFILE_Z3_LONG);
    static_assert(profile_bit(ProfileId::Z3_SHARED_LONG)
                  == CACHE_PROFILE_Z3_SHARED_LONG);
    static_assert((kKnownProfileMask & profile_bit(ProfileId::Z3_LONG)) == 0);
    static_assert((kKnownProfileMask & profile_bit(ProfileId::Z3_SHARED_LONG)) == 0);
    static_assert((CACHE_ADVERTISABLE_PROFILE_MASK
                   & (CACHE_PROFILE_Z3_LONG | CACHE_PROFILE_Z3_SHARED_LONG)) == 0);
    REQUIRE(profile_name(ProfileId::Z3_LONG) == "z3_long"
                && profile_name(ProfileId::Z3_SHARED_LONG) == "z3_shared_long",
            "streaming profile IDs have stable declared labels");

    for (ProfileId profile : {ProfileId::Z3_LONG, ProfileId::Z3_SHARED_LONG}) {
        SessionHello hello;
        hello.c_store_guid = Id128::from_u64(91);
        hello.supported_profiles = profile_bit(profile);
        bool negotiation_rejected = false;
        try {
            (void)negotiate_session(hello, kProtocolVersion, kProtocolVersion,
                                    kKnownProfileMask);
        } catch (const std::invalid_argument&) {
            negotiation_rejected = true;
        }
        REQUIRE(negotiation_rejected,
                "declared streaming profile cannot negotiate without a codec");

        TxBegin begin;
        begin.history_nonce = HistoryNonce{1};
        begin.profile = profile;
        begin.p29_root_mode = P29RootMode::NotApplicable;
        bool transaction_rejected = false;
        try {
            (void)compute_transaction_digest(begin, {}, {});
        } catch (const std::invalid_argument&) {
            transaction_rejected = true;
        }
        REQUIRE(transaction_rejected,
                "declared streaming profile cannot enter a transaction");
    }
}

int main()
{
    test_legacy_bytes();
    test_p50_round_trip_and_validation();
    test_declared_profiles_are_inert();
    std::fprintf(stderr, "%s: %d failure(s)\n",
                 failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
