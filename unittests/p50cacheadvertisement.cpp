/* Inert Protocol-50 Login cache-advertisement gate.  All frames use the
   production MsgChannel/LoginMsg codec; old-version hashes are retained from
   the exact pre-advertisement convergence parent. */

#include "comm.h"
#include "cache/protocol50.h"
#include "daemon/p50_cache_recovery_policy.h"

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
    login.setCacheAdvertisement(UINT32_C(0x0000beef), CACHE_WIRE_REVISION,
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

// Sets a 64-bit tail field (assignment epoch or nonce) addressed by the
// word-count-from-end of its HIGH word; the low word is one position closer
// to the end. Used to hand-craft raw-frame identity-binding rows.
static void set_tail_word64(Bytes& bytes, size_t hi_words_from_end, uint64_t value)
{
    set_tail_word(bytes, hi_words_from_end, static_cast<uint32_t>(value >> 32));
    set_tail_word(bytes, hi_words_from_end - 1, static_cast<uint32_t>(value));
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

// Inverse of remove_tail_words: appends one word to a framed message and
// grows its length prefix to match, simulating a tail that started
// transmitting (a torn/cut-off frame) rather than one that was never sent.
static Bytes append_word(Bytes bytes, uint32_t value)
{
    if (bytes.size() < sizeof(uint32_t)) return {};
    uint32_t network_length = 0;
    std::memcpy(&network_length, bytes.data(), sizeof(network_length));
    const uint32_t length = ntohl(network_length) + sizeof(uint32_t);
    network_length = htonl(length);
    std::memcpy(bytes.data(), &network_length, sizeof(network_length));
    value = htonl(value);
    const size_t old_size = bytes.size();
    bytes.resize(old_size + sizeof(value));
    std::memcpy(bytes.data() + old_size, &value, sizeof(value));
    return bytes;
}

static Bytes remove_tail_bytes(Bytes bytes, size_t removed)
{
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
    const Bytes p51 = encode_frame(51, login);
    REQUIRE(p50.size() == p49.size() + 3 * sizeof(uint32_t)
                && std::equal(p49.begin() + sizeof(uint32_t), p49.end(),
                              p50.begin() + sizeof(uint32_t)),
            "P50 Login appends exactly three words to the unchanged P49 body");
    REQUIRE(p51 == p50,
            "selected R1 cache advertisement keeps exact Login bytes on Protocol 51");
    Pair pair = make_pair(50);
    REQUIRE(pair.left->send_msg(login), "P50 Login advertises a valid endpoint");
    Msg *wire = pair.right->get_msg(2, true);
    LoginMsg *decoded = dynamic_cast<LoginMsg *>(wire);
    REQUIRE(decoded && decoded->cache_endpoint_port == UINT32_C(0x0000beef)
                && decoded->cache_protocol == CACHE_WIRE_REVISION
                && decoded->cache_profile_mask == CACHE_PROFILE_ZSTD_TU,
            "P50 Login round-trips the exact three-word advertisement");
    delete wire;

    Pair p51_pair = make_pair(51);
    REQUIRE(p51_pair.left->send_msg(login),
            "Protocol-50 Login advertisement remains selectable on Protocol 51");
    wire = p51_pair.right->get_msg(2, true);
    decoded = dynamic_cast<LoginMsg *>(wire);
    REQUIRE(decoded && decoded->cache_endpoint_port == UINT32_C(0x0000beef)
                && decoded->cache_protocol == CACHE_WIRE_REVISION
                && decoded->cache_profile_mask == CACHE_PROFILE_ZSTD_TU,
            "Protocol-51 decoder accepts the unchanged R1 advertisement");
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
        {0, CACHE_WIRE_REVISION, CACHE_PROFILE_ZSTD_TU,
         "zero port with positive capability"},
        {70000, CACHE_WIRE_REVISION, CACHE_PROFILE_ZSTD_TU,
         "port outside TCP range"},
        {10245, 0, CACHE_PROFILE_ZSTD_TU, "missing cache protocol"},
        {10245, CACHE_WIRE_REVISION, 0, "missing cache profile"},
        {10245, CACHE_WIRE_REVISION, UINT32_C(0x00000008),
         "removed revision-one profile bit"},
        {10245, CACHE_WIRE_REVISION, UINT32_C(0x80000000),
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
    set_tail_word(malformed, 1, CACHE_PROFILE_ZSTD_TU | CACHE_PROFILE_ZSTD_ROUTE);
    REQUIRE(!decoder_rejects(malformed),
            "decoder accepts the implemented TU+ROUTE capability advertisement");
    malformed = valid;
    set_tail_word(malformed, 1, UINT32_C(0x80000000));
    REQUIRE(decoder_rejects(malformed),
            "decoder rejects unknown profile advertisement");
}

static void test_p51_login_and_opt_in_selection_matrix()
{
    // The Login envelope stays the same size at protocol 50 and 51. Revision
    // two is syntactically accepted there, but scheduler assignment must still
    // require protocol 51 at both ordinary peers.
    LoginMsg r2_login = fixture_login();
    r2_login.setCacheAdvertisement(UINT32_C(0x0000beef),
                                  CACHE_WIRE_REVISION_R2,
                                  CACHE_ADVERTISABLE_PROFILE_MASK);
    const Bytes r2_p50 = encode_frame(50, r2_login);
    const Bytes r2_p51 = encode_frame(51, r2_login);
    REQUIRE(!r2_p50.empty() && r2_p50 == r2_p51,
            "R2 Login advertisement has identical protocol-50/51 bytes");
    for (const int protocol : {50, 51}) {
        Pair pair = make_pair(protocol);
        REQUIRE(pair.left->send_msg(r2_login),
                "Login revision two is syntactically accepted on protocol 50/51");
        Msg *wire = pair.right->get_msg(2, true);
        LoginMsg *decoded = dynamic_cast<LoginMsg *>(wire);
        REQUIRE(decoded && decoded->cache_protocol == CACHE_WIRE_REVISION_R2 &&
                    decoded->cache_profile_mask == CACHE_ADVERTISABLE_PROFILE_MASK,
                "Login revision-two advertisement round-trips on protocol 50/51");
        delete wire;
    }

    struct EnvironmentRestore {
        const char *name;
        bool was_set;
        std::string old_value;
        explicit EnvironmentRestore(const char *variable)
            : name(variable), was_set(std::getenv(variable) != nullptr),
              old_value(was_set ? std::getenv(variable) : "") {}
        ~EnvironmentRestore()
        {
            if (was_set)
                (void)setenv(name, old_value.c_str(), 1);
            else
                (void)unsetenv(name);
        }
        bool set(const char *value)
        {
            return value != nullptr ? setenv(name, value, 1) == 0
                                    : unsetenv(name) == 0;
        }
    } p50_mode("ICECC_P50_MODE"), p51_mode("ICECC_P51_MODE");
    REQUIRE(p50_mode.set("on"), "set P50 wrapper opt-in for selector matrix");

    struct ModeRow {
        int protocol;
        const char *mode;
        uint32_t expected_revision;
        const char *label;
    };
    const ModeRow rows[] = {
        {50, nullptr, CACHE_WIRE_REVISION_R1, "P50 mode unset"},
        {50, "off", CACHE_WIRE_REVISION_R1, "P50 mode off"},
        {50, "on", 0, "P50 cannot opt into R2"},
        {50, "malformed", 0, "P50 malformed mode"},
        {51, nullptr, CACHE_WIRE_REVISION_R1, "P51 mode unset defaults to R1"},
        {51, "off", CACHE_WIRE_REVISION_R1, "P51 mode off keeps R1"},
        {51, "on", CACHE_WIRE_REVISION_R2, "P51 explicit opt-in selects R2"},
        {51, "malformed", 0, "P51 malformed mode fails closed"},
    };
    for (const ModeRow &row : rows) {
        REQUIRE(p51_mode.set(row.mode), "set P51 mode row");
        const uint32_t revision =
            p50_cache_revision_from_environment(row.protocol);
        const P50CacheClientCapability capability =
            p50_cache_client_capability_from_env(row.protocol);
        char label[180];
        std::snprintf(label, sizeof(label),
                      "%s (ordinary=%d) chooses revision %u", row.label,
                      row.protocol, row.expected_revision);
        REQUIRE(revision == row.expected_revision, label);
        REQUIRE(capability.protocol == row.expected_revision &&
                    capability.profile_mask ==
                        (row.expected_revision != 0
                             ? CACHE_ADVERTISABLE_PROFILE_MASK : 0),
                "client capability projection matches the mode/version row");
    }
    REQUIRE(p51_mode.set("on"), "enable P51 for ordinary-peer matrix");
    REQUIRE(p50_cache_pair_ordinary_protocols_compatible(
                CACHE_WIRE_REVISION_R1, 50, 50) &&
                p50_cache_pair_ordinary_protocols_compatible(
                    CACHE_WIRE_REVISION_R1, 50, 51) &&
                p50_cache_pair_ordinary_protocols_compatible(
                    CACHE_WIRE_REVISION_R1, 51, 50) &&
                p50_cache_pair_ordinary_protocols_compatible(
                    CACHE_WIRE_REVISION_R1, 51, 51),
            "R1 remains selectable for all protocol-50/51 peer pairs");
    REQUIRE(p50_cache_pair_ordinary_protocols_compatible(
                CACHE_WIRE_REVISION_R2, 51, 51) &&
                !p50_cache_pair_ordinary_protocols_compatible(
                    CACHE_WIRE_REVISION_R2, 50, 51) &&
                !p50_cache_pair_ordinary_protocols_compatible(
                    CACHE_WIRE_REVISION_R2, 51, 50) &&
                !p50_cache_pair_ordinary_protocols_compatible(
                    CACHE_WIRE_REVISION_R2, 50, 50),
            "R2 assignment requires protocol 51 at both ordinary peers");
    REQUIRE(!p50_cache_pair_ordinary_protocols_compatible(
                CACHE_WIRE_REVISION_R2 + 1, 51, 51),
            "unknown CacheWire revision never selects for protocol-51 peers");
    REQUIRE(p51_mode.set("off") &&
                p50_cache_revision_from_environment(51) ==
                    CACHE_WIRE_REVISION_R1 &&
                p50_cache_client_capability_from_env(51).protocol ==
                    CACHE_WIRE_REVISION_R1,
            "explicitly disabled R2 retains the R1 path");
    REQUIRE(p50_mode.set("off") && p51_mode.set("on") &&
                p50_cache_client_capability_from_env(51) ==
                    P50CacheClientCapability{},
            "P50 kill switch suppresses even an R2 P51 capability request");
}

static GetCSMsg fixture_getcs()
{
    Environments environments;
    environments.push_back(std::make_pair(std::string("x86_64"),
                                           std::string("cache-env")));
    GetCSMsg request(environments, "/build/cache-input.cpp",
                     CompileJob::Lang_CXX, UINT32_C(1), "x86_64",
                     UINT32_C(0), "", 50, UINT32_C(0), 0);
    request.client_id = UINT32_C(17);
    request.cache_protocol = CACHE_WIRE_REVISION;
    request.cache_profile_mask = CACHE_ADVERTISABLE_PROFILE_MASK;
    request.cache_affinity_profile_mask = CACHE_PROFILE_P29V1;
    request.cache_affinity_port = UINT32_C(10245);
    request.cache_affinity_host = "warm-cache-worker";
    return request;
}

static Bytes encode_getcs_frame(int protocol, const GetCSMsg& request)
{
    Pair pair = make_pair(protocol);
    if (!pair.left->send_msg(request)) return {};
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

static bool getcs_decoder_rejects(const Bytes& bytes)
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

static void test_getcs_cache_request_wire_and_laws()
{
    GetCSMsg request = fixture_getcs();
    GetCSMsg absent = request;
    absent.cache_protocol = 0;
    absent.cache_profile_mask = 0;
    absent.cache_affinity_profile_mask = 0;
    absent.cache_affinity_port = 0;
    absent.cache_affinity_host.clear();

    const Bytes p49_present_object = encode_getcs_frame(49, request);
    const Bytes p49_absent_object = encode_getcs_frame(49, absent);
    REQUIRE(!p49_present_object.empty()
                && p49_present_object == p49_absent_object,
            "P49 GetCS bytes are unchanged by protocol-50 client capability state");

    Pair old_pair = make_pair(49);
    REQUIRE(old_pair.left->send_msg(request),
            "populated client capability object emits on a P49 link");
    Msg *wire = old_pair.right->get_msg(2, true);
    GetCSMsg *decoded = dynamic_cast<GetCSMsg *>(wire);
    REQUIRE(decoded && decoded->cache_protocol == 0
                && decoded->cache_profile_mask == 0
                && decoded->cache_affinity_profile_mask == 0
                && decoded->cache_affinity_port == 0
                && decoded->cache_affinity_host.empty()
                && decoded->cache_retry_avoid_port == 0
                && decoded->cache_retry_avoid_host.empty()
                && decoded->remote_required == 0,
            "P49 GetCS decoder receives canonical client capability absence");
    delete wire;

    const Bytes p50_absent = encode_getcs_frame(50, absent);
    REQUIRE(p50_absent.size() == p49_absent_object.size()
                + 8 * sizeof(uint32_t) + 2,
            "P50 GetCS appends its six fields and two bounded empty strings");

    Pair pair = make_pair(50);
    REQUIRE(pair.left->send_msg(request),
            "P50 GetCS carries a valid client capability and warm hint");
    wire = pair.right->get_msg(2, true);
    decoded = dynamic_cast<GetCSMsg *>(wire);
    REQUIRE(decoded && decoded->cache_protocol == CACHE_WIRE_REVISION
                && decoded->cache_profile_mask == CACHE_ADVERTISABLE_PROFILE_MASK
                && decoded->cache_affinity_profile_mask == CACHE_PROFILE_P29V1
                && decoded->cache_affinity_port == UINT32_C(10245)
                && decoded->cache_affinity_host == "warm-cache-worker"
                && decoded->cache_retry_avoid_port == 0
                && decoded->cache_retry_avoid_host.empty()
                && decoded->remote_required == 0,
            "P50 GetCS round-trips the exact capability and warm hint");
    delete wire;

    GetCSMsg retry_avoid = request;
    retry_avoid.cache_affinity_profile_mask = 0;
    retry_avoid.cache_affinity_port = 0;
    retry_avoid.cache_affinity_host.clear();
    retry_avoid.cache_retry_avoid_port = UINT32_C(10246);
    retry_avoid.cache_retry_avoid_host = "failed-cache-worker";
    Pair retry_pair = make_pair(50);
    REQUIRE(retry_pair.left->send_msg(retry_avoid),
            "P50 GetCS carries one exact request-local retry exclusion");
    wire = retry_pair.right->get_msg(2, true);
    decoded = dynamic_cast<GetCSMsg *>(wire);
    REQUIRE(decoded && decoded->cache_protocol == CACHE_WIRE_REVISION
                && decoded->cache_profile_mask == CACHE_ADVERTISABLE_PROFILE_MASK
                && decoded->cache_affinity_profile_mask == 0
                && decoded->cache_affinity_port == 0
                && decoded->cache_affinity_host.empty()
                && decoded->cache_retry_avoid_port == UINT32_C(10246)
                && decoded->cache_retry_avoid_host == "failed-cache-worker"
                && decoded->remote_required == 0,
            "P50 GetCS round-trips retry exclusion independently of warm affinity");
    delete wire;

    Pair absent_pair = make_pair(50);
    REQUIRE(absent_pair.left->send_msg(absent),
            "P50 GetCS accepts canonical all-zero client capability absence");
    wire = absent_pair.right->get_msg(2, true);
    decoded = dynamic_cast<GetCSMsg *>(wire);
    REQUIRE(decoded && decoded->cache_protocol == 0
                && decoded->cache_profile_mask == 0
                && decoded->cache_affinity_profile_mask == 0
                && decoded->cache_affinity_port == 0
                && decoded->cache_affinity_host.empty()
                && decoded->cache_retry_avoid_port == 0
                && decoded->cache_retry_avoid_host.empty()
                && decoded->remote_required == 0,
            "P50 absent client capability remains wholly canonical");
    delete wire;

    GetCSMsg remote_required = absent;
    remote_required.remote_required = 1;
    Pair remote_required_pair = make_pair(50);
    REQUIRE(remote_required_pair.left->send_msg(remote_required),
            "P50 GetCS carries the explicit remote-required bit");
    wire = remote_required_pair.right->get_msg(2, true);
    decoded = dynamic_cast<GetCSMsg *>(wire);
    REQUIRE(decoded && decoded->remote_required == 1,
            "P50 GetCS round-trips the exact remote-required policy");
    delete wire;

    GetCSMsg invalid_remote_required = absent;
    invalid_remote_required.remote_required = 2;
    REQUIRE(!make_pair(50).left->send_msg(invalid_remote_required),
            "GetCS encoder rejects a non-boolean remote-required value");

    const struct Invalid {
        uint32_t protocol;
        uint32_t profiles;
        uint32_t affinity_profiles;
        uint32_t affinity_port;
        const char *affinity_host;
        const char *name;
    } invalid[] = {
        {0, CACHE_PROFILE_P29V1, 0, 0, "", "profile without revision"},
        {CACHE_WIRE_REVISION, 0, 0, 0, "", "revision without profiles"},
        {CACHE_WIRE_REVISION, UINT32_C(0x80000000), 0, 0, "",
         "unknown client profile"},
        {CACHE_WIRE_REVISION, CACHE_PROFILE_P29V1,
         CACHE_PROFILE_ZSTD_TU, 10245, "warm-cache-worker",
         "affinity outside client capabilities"},
        {CACHE_WIRE_REVISION, CACHE_PROFILE_P29V1,
         CACHE_PROFILE_P29V1, 10245, "", "affinity profile without host"},
        {CACHE_WIRE_REVISION, CACHE_PROFILE_P29V1,
         CACHE_PROFILE_P29V1, 0, "warm-cache-worker",
         "affinity host without port"},
        {CACHE_WIRE_REVISION, CACHE_PROFILE_P29V1,
         0, 10245, "warm-cache-worker", "affinity host without profile"},
        {CACHE_WIRE_REVISION, CACHE_PROFILE_P29V1,
         CACHE_PROFILE_P29V1, 70000, "warm-cache-worker",
         "affinity port outside TCP range"},
    };
    for (const Invalid& value : invalid) {
        GetCSMsg malformed = absent;
        malformed.cache_protocol = value.protocol;
        malformed.cache_profile_mask = value.profiles;
        malformed.cache_affinity_profile_mask = value.affinity_profiles;
        malformed.cache_affinity_port = value.affinity_port;
        malformed.cache_affinity_host = value.affinity_host;
        Pair send_pair = make_pair(50);
        char label[176];
        std::snprintf(label, sizeof(label),
                      "GetCS encoder rejects %s", value.name);
        REQUIRE(!send_pair.left->send_msg(malformed), label);
    }

    GetCSMsg malformed_retry = absent;
    malformed_retry.cache_protocol = CACHE_WIRE_REVISION;
    malformed_retry.cache_profile_mask = CACHE_ADVERTISABLE_PROFILE_MASK;
    malformed_retry.cache_retry_avoid_port = UINT32_C(10246);
    REQUIRE(!make_pair(50).left->send_msg(malformed_retry),
            "GetCS encoder rejects retry-exclusion port without host");
    malformed_retry.cache_retry_avoid_port = 0;
    malformed_retry.cache_retry_avoid_host = "failed-cache-worker";
    REQUIRE(!make_pair(50).left->send_msg(malformed_retry),
            "GetCS encoder rejects retry-exclusion host without port");
    malformed_retry.cache_retry_avoid_port = UINT32_C(70000);
    REQUIRE(!make_pair(50).left->send_msg(malformed_retry),
            "GetCS encoder rejects retry-exclusion port outside TCP range");

    GetCSMsg retry_without_capability = absent;
    retry_without_capability.cache_retry_avoid_port = UINT32_C(10246);
    retry_without_capability.cache_retry_avoid_host = "failed-cache-worker";
    REQUIRE(!make_pair(50).left->send_msg(retry_without_capability),
            "GetCS encoder rejects retry exclusion without P50 capability");

    REQUIRE(getcs_decoder_rejects(remove_tail_bytes(p50_absent, 34)),
            "P50 GetCS decoder rejects a wholly omitted capability tail");
    REQUIRE(getcs_decoder_rejects(remove_tail_bytes(p50_absent, 1)),
            "P50 GetCS decoder rejects a truncated retry-exclusion string");
    REQUIRE(getcs_decoder_rejects(append_word(p50_absent, UINT32_C(0))),
            "P50 GetCS decoder rejects bytes after the exact request tail");

    const P50CacheClientCapability enabled =
        p50_cache_client_capability_from_mode("on", 50);
    REQUIRE(enabled.protocol == CACHE_WIRE_REVISION
                && enabled.profile_mask == CACHE_ADVERTISABLE_PROFILE_MASK,
            "C mode on advertises exactly the retained revision-one profiles");
    REQUIRE(p50_cache_client_capability_from_mode(nullptr, 50) == enabled,
            "C protocol-50 mode defaults on when the kill-switch variable is absent");
    REQUIRE(p50_cache_client_capability_from_mode("off", 50) ==
                P50CacheClientCapability{}
                && p50_cache_client_capability_from_mode("invalid", 50) ==
                    P50CacheClientCapability{}
                && p50_cache_client_capability_from_mode("on", 49) ==
                    P50CacheClientCapability{},
            "C mode off, invalid, and old-wrapper paths fail closed");

    REQUIRE(p50_select_pair_cache_profile(
                CACHE_WIRE_REVISION,
                CACHE_PROFILE_P29V1 | CACHE_PROFILE_ZSTD_TU,
                CACHE_WIRE_REVISION,
                CACHE_PROFILE_P29V1 | CACHE_PROFILE_ZSTD_ROUTE,
                P50CacheProfileRequest::Default) == CACHE_PROFILE_P29V1,
            "pair selection applies the default order to the exact intersection");
    REQUIRE(p50_select_pair_cache_profile(
                CACHE_WIRE_REVISION, CACHE_ADVERTISABLE_PROFILE_MASK,
                CACHE_WIRE_REVISION,
                CACHE_PROFILE_P29V1 | CACHE_PROFILE_ZSTD_ROUTE,
                P50CacheProfileRequest::ZSTD_ROUTE) == CACHE_PROFILE_ZSTD_ROUTE,
            "pair selection honors an available exact scheduler request");
    REQUIRE(p50_select_pair_cache_profile(
                CACHE_WIRE_REVISION, CACHE_PROFILE_P29V1,
                CACHE_WIRE_REVISION, CACHE_PROFILE_ZSTD_TU,
                P50CacheProfileRequest::Default) == 0
                && p50_select_pair_cache_profile(
                    CACHE_WIRE_REVISION, CACHE_PROFILE_P29V1,
                    CACHE_WIRE_REVISION, CACHE_PROFILE_P29V1,
                    P50CacheProfileRequest::ZSTD_ROUTE) == 0
                && p50_select_pair_cache_profile(
                    CACHE_WIRE_REVISION + 1, CACHE_PROFILE_P29V1,
                    CACHE_WIRE_REVISION, CACHE_PROFILE_P29V1,
                    P50CacheProfileRequest::Default) == 0,
            "disjoint, unavailable-exact, and revision-skew pairs remain legacy");
}

static UseCSMsg fixture_usecs()
{
    UseCSMsg use("x86_64", "cache-worker", UINT32_C(0x00002805),
                 UINT32_C(0x0000beef), true, UINT32_C(7), UINT32_C(0),
                 UINT64_C(0x1020304050607080), UINT64_C(0x8877665544332211),
                 UINT32_C(0x0000cafe), CACHE_WIRE_REVISION,
                 CACHE_PROFILE_ZSTD_TU);
    return use;
}

/* Byte-frozen SUPERSEDED-DRAFT PROVENANCE fixture -- NOT a compatibility
   guarantee.  Captured once from an isolated build of the pre-branch
   (a862, "Merge accepted R6 input seam into corrected foundations")
   services/comm.{h,cpp} -- the actual bytes a protocol-50 UseCS peer built
   before this cache-handoff tail existed puts on the wire -- rather than
   derived by trimming today's encoder output (which would only prove the
   current build agrees with itself, not with a real old binary).  Owner
   ruling (d23d9c5d HOLD): protocol 50 is an in-development draft with no
   deployed base, so this frame's absent tail (a862's binary never wrote
   the three cache words at all, so `remaining` is exactly 0 bytes at the
   point the decoder checks) is now REJECTED like any other malformed
   50-frame -- the mandatory-tail law has exactly one legal encoding of
   absence (three zero-valued words), not "tail omitted entirely".  This
   fixture now proves the decoder DETECTS a pre-final-50 frame and refuses
   it, which is the deployment-safety property that actually matters here;
   it is deliberately kept, relabeled, rather than deleted, so that
   property stays under test.  Source values: UseCSMsg("x86_64",
   "cache-worker", 0x00002805, 0x0000beef, true, 7, 0,
   0x1020304050607080, 0x8877665544332211) sent at protocol 50 through the
   a862 MsgChannel.  Independently roundtripped back through the a862
   decoder before freezing: job_id/port/hostname/host_platform/got_env/
   client_id/matched_job_id/assignmentEpoch/assignmentNonce all matched the
   source values exactly. */
static const unsigned char kOldPeerUseCSFrame[] = {
    0x00, 0x00, 0x00, 0x44, 0x00, 0x00, 0x00, 0x48, 0x00, 0x00, 0xbe, 0xef,
    0x00, 0x00, 0x28, 0x05, 0x00, 0x00, 0x00, 0x0d, 0x63, 0x61, 0x63, 0x68,
    0x65, 0x2d, 0x77, 0x6f, 0x72, 0x6b, 0x65, 0x72, 0x00, 0x00, 0x00, 0x00,
    0x07, 0x78, 0x38, 0x36, 0x5f, 0x36, 0x34, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x10, 0x20, 0x30, 0x40,
    0x50, 0x60, 0x70, 0x80, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
};

static Bytes encode_usecs_frame(int protocol, const UseCSMsg& use)
{
    Pair pair = make_pair(protocol);
    if (!pair.left->send_msg(use)) return {};
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

static void test_usecs_legacy_bytes()
{
    const UseCSMsg use = fixture_usecs();
    const Bytes p49 = encode_usecs_frame(49, use);
    const Bytes p50 = encode_usecs_frame(50, use);
    REQUIRE(!p49.empty() && !p50.empty(), "UseCS encodes at both P49 and P50");
    /* PROTOCOL_VERSION_ASSIGNMENT_IDENTITY and PROTOCOL_VERSION_CACHE_ADVERTISEMENT
       are the same value (50): P50 UseCS carries both the eight-word assignment
       identity (epoch, nonce, C GUID, TU sequence) and this three-word S2
       cache-handoff tail, so it is eleven words larger than P49 -- not three --
       with an unchanged prefix. */
    REQUIRE(p50.size() == p49.size() + 11 * sizeof(uint32_t)
                && std::equal(p49.begin() + sizeof(uint32_t), p49.end(),
                              p50.begin() + sizeof(uint32_t)),
            "P50 UseCS appends exactly eleven words to the unchanged P49 body");

    Pair old_pair = make_pair(49);
    REQUIRE(old_pair.left->send_msg(use),
            "populated cache-handoff object emits on a P49 link");
    Msg *wire = old_pair.right->get_msg(2, true);
    UseCSMsg *decoded = dynamic_cast<UseCSMsg *>(wire);
    REQUIRE(decoded && !decoded->hasCacheAdvertisement()
                && decoded->cache_protocol == 0
                && decoded->cache_profile_mask == 0,
            "P49 decoder receives canonical cache-handoff absence");
    delete wire;
}

static void test_usecs_p50_round_trip_and_validation()
{
    const UseCSMsg use = fixture_usecs();
    Pair pair = make_pair(50);
    REQUIRE(pair.left->send_msg(use),
            "P50 UseCS carries a valid assignment-bound cache endpoint");
    Msg *wire = pair.right->get_msg(2, true);
    UseCSMsg *decoded = dynamic_cast<UseCSMsg *>(wire);
    REQUIRE(decoded && decoded->cache_endpoint_port == UINT32_C(0x0000cafe)
                && decoded->cache_protocol == CACHE_WIRE_REVISION
                && decoded->cache_profile_mask == CACHE_PROFILE_ZSTD_TU
                && decoded->hostname == "cache-worker"
                && decoded->assignmentEpoch() == UINT64_C(0x1020304050607080)
                && decoded->assignmentNonce() == UINT64_C(0x8877665544332211),
            "P50 UseCS round-trips the exact cache endpoint bound to its "
            "assignment identity");
    delete wire;

    UseCSMsg absent = fixture_usecs();
    absent.cache_endpoint_port = 0;
    absent.cache_protocol = 0;
    absent.cache_profile_mask = 0;
    Pair absent_pair = make_pair(50);
    REQUIRE(absent_pair.left->send_msg(absent),
            "P50 UseCS accepts canonical all-zero cache-handoff absence");
    wire = absent_pair.right->get_msg(2, true);
    decoded = dynamic_cast<UseCSMsg *>(wire);
    REQUIRE(decoded && !decoded->hasCacheAdvertisement()
                && decoded->cache_protocol == 0
                && decoded->cache_profile_mask == 0,
            "P50 absent cache handoff remains wholly zero");
    delete wire;

    const struct Invalid {
        uint32_t port;
        uint32_t protocol;
        uint32_t profiles;
        const char *name;
    } invalid[] = {
        {0, CACHE_WIRE_REVISION, CACHE_PROFILE_ZSTD_TU,
         "zero port with positive capability"},
        {70000, CACHE_WIRE_REVISION, CACHE_PROFILE_ZSTD_TU,
         "port outside TCP range"},
        {10245, 0, CACHE_PROFILE_ZSTD_TU, "missing cache protocol"},
        {10245, UINT32_C(49), CACHE_PROFILE_ZSTD_TU,
         "stale pre-CacheWire protocol number"},
        {10245, CACHE_WIRE_REVISION, 0, "missing cache profile"},
        {10245, CACHE_WIRE_REVISION,
         CACHE_PROFILE_ZSTD_TU | CACHE_PROFILE_ZSTD_ROUTE,
         "multiple profiles in one assignment"},
        {10245, CACHE_WIRE_REVISION, UINT32_C(0x00000008),
         "removed revision-one profile bit"},
        {10245, CACHE_WIRE_REVISION, UINT32_C(0x80000000),
         "unknown profile bit"},
    };
    for (const Invalid& value : invalid) {
        UseCSMsg malformed = fixture_usecs();
        malformed.cache_endpoint_port = value.port;
        malformed.cache_protocol = value.protocol;
        malformed.cache_profile_mask = value.profiles;
        Pair send_pair = make_pair(50);
        char label[160];
        std::snprintf(label, sizeof(label),
                      "encoder rejects %s (UseCS cache-handoff tail)", value.name);
        REQUIRE(!send_pair.left->send_msg(malformed), label);
    }

    {
        /* Fixture A (REJECT row -- superseded-draft provenance, not a
           compat guarantee, see kOldPeerUseCSFrame's comment above): the
           byte-frozen genuine old-peer frame carries NO cache tail at all
           (remaining == 0 at the check point).  Under the mandatory-tail
           law that is indistinguishable from any other malformed 50-frame
           and must be rejected outright -- proving the decoder detects and
           refuses a pre-final-50 peer rather than silently downgrading to
           an "absent" projection for it. */
        Pair frozen_pair = make_pair(50);
        const bool wrote = send(frozen_pair.left->fd, kOldPeerUseCSFrame,
                                 sizeof(kOldPeerUseCSFrame), 0)
            == static_cast<ssize_t>(sizeof(kOldPeerUseCSFrame));
        Msg *wire = wrote ? frozen_pair.right->get_msg(2, true) : nullptr;
        REQUIRE(wrote && !wire,
                "P50 UseCS decoder REJECTS the byte-frozen superseded-draft "
                "old-peer frame (no tail at all) -- provenance/reject row, "
                "not a compatibility guarantee");
        delete wire;
    }
    {
        /* Fixture B: the same frozen old-peer bytes, plus one more word as
           if a tail had started transmitting and been cut off (or
           corrupted) partway.  remaining == 4 != 12 and must stay
           rejected, exactly like fixture A now -- both are malformed
           50-frames under the mandatory-tail law, just via different
           byte shapes. */
        const Bytes frozen(kOldPeerUseCSFrame,
                            kOldPeerUseCSFrame + sizeof(kOldPeerUseCSFrame));
        REQUIRE(decoder_rejects(append_word(frozen, UINT32_C(0x0000cafe))),
                "P50 UseCS decoder rejects the frozen old-peer frame plus a "
                "torn one-word start of a cache-handoff tail");
    }

    const Bytes valid = encode_usecs_frame(50, use);
    REQUIRE(decoder_rejects(remove_tail_words(valid, 3)),
            "P50 UseCS decoder rejects a wholly omitted cache-handoff tail "
            "(mandatory tail: absence is value-encoded 0/0/0, never "
            "omission)");
    REQUIRE(decoder_rejects(remove_tail_words(valid, 2)),
            "P50 UseCS decoder rejects a one-word cache-handoff tail");
    REQUIRE(decoder_rejects(remove_tail_words(valid, 1)),
            "P50 UseCS decoder rejects a two-word cache-handoff tail");
    REQUIRE(decoder_rejects(append_word(valid, UINT32_C(0xdeadbeef))),
            "P50 UseCS decoder rejects an over-length tail (valid frame "
            "plus trailing bytes) -- caught directly by the exact "
            "remaining==12 check, with MsgChannel's own exact-consumption "
            "law as an independent second backstop");
    Bytes malformed_wire = valid;
    set_tail_word(malformed_wire, 3, 0);
    REQUIRE(decoder_rejects(malformed_wire),
            "decoder rejects zero cache port with positive capability");
    malformed_wire = valid;
    set_tail_word(malformed_wire, 2, 0);
    REQUIRE(decoder_rejects(malformed_wire),
            "decoder rejects partial cache-protocol absence");
    malformed_wire = valid;
    set_tail_word(malformed_wire, 1,
                  CACHE_PROFILE_ZSTD_TU | CACHE_PROFILE_ZSTD_ROUTE);
    REQUIRE(decoder_rejects(malformed_wire),
            "UseCS decoder rejects multiple profiles in one assignment");
    malformed_wire = valid;
    set_tail_word(malformed_wire, 1, UINT32_C(0x80000000));
    REQUIRE(decoder_rejects(malformed_wire),
            "decoder rejects unknown cache-profile advertisement");
}

// BigOracle's identity-binding law (d23d9c5d HOLD): a valid-PRESENT cache
// triple additionally requires a COMPLETE nonzero assignment identity
// {job_id, epoch, nonce}; a partial identity is rejected by the
// pre-existing assignment law alone, independent of cache state.  Each row
// is exercised three ways -- direct-object (valid_payload() called
// directly), encoder (send_msg), and raw-frame (hand-crafted wire bytes,
// independent of both the object and the encoder) -- since each layer has
// its own chance to regress.
static void test_usecs_identity_binding_law()
{
    const uint64_t epoch = UINT64_C(0x1020304050607080);
    const uint64_t nonce = UINT64_C(0x8877665544332211);
    const uint32_t job_id = UINT32_C(0x0000beef);
    const uint32_t cache_port = UINT32_C(0x0000cafe);

    const struct Row {
        uint32_t job_id;
        uint64_t epoch;
        uint64_t nonce;
        uint32_t cache_port;
        uint32_t cache_protocol;
        uint32_t cache_mask;
        bool expect_valid;
        const char *name;
    } rows[] = {
        // Partial identity, cache absent: caught by the pre-existing
        // assignment law alone (assignment_absent || assignment_complete)
        // -- these three rows stay red even if the NEW cache-binding
        // clause were deleted entirely; they do not discriminate it.
        {job_id, 0, nonce, 0, 0, 0, false, "missing epoch (cache absent)"},
        {job_id, epoch, 0, 0, 0, 0, false, "missing nonce (cache absent)"},
        {0, epoch, nonce, 0, 0, 0, false, "zero wire job_id (cache absent)"},
        // Baseline: assignment wholly absent + cache wholly absent is the
        // OTHER legal combination the law allows -- not just "complete
        // identity + valid cache".
        {job_id, 0, 0, 0, 0, 0, true,
         "assignment absent, cache absent (baseline)"},
        // THE deletion-sensitive discriminator (BigOracle's exact trap):
        // both identity words absent together is a state the pre-existing
        // assignment law deliberately ACCEPTS on its own (assignment_
        // absent).  Only the NEW clause (!cache_present ||
        // assignment_complete) catches it once a valid-present cache
        // triple is added on top.  Neutralizing that one clause must flip
        // exactly THIS row green, while the three partial rows above stay
        // red for their own, unrelated reason.
        {job_id, 0, 0, cache_port, CACHE_WIRE_REVISION, CACHE_PROFILE_ZSTD_TU,
         false,
         "both identity words absent, cache VALID-PRESENT (discriminator)"},
        // Valid complete baseline (also covered by the round-trip test
        // above; repeated here so every identity-law row sits together).
        {job_id, epoch, nonce, cache_port, CACHE_WIRE_REVISION,
         CACHE_PROFILE_ZSTD_TU, true,
         "complete identity, cache VALID-PRESENT (baseline)"},
    };

    for (const Row& row : rows) {
        char label[192];

        const UseCSMsg direct("x86_64", "cache-worker", UINT32_C(0x00002805),
                              row.job_id, true, UINT32_C(7), UINT32_C(0),
                              row.epoch, row.nonce, row.cache_port,
                              row.cache_protocol, row.cache_mask);
        std::snprintf(label, sizeof(label),
                      "direct-object valid_payload(): %s", row.name);
        REQUIRE(direct.valid_payload() == row.expect_valid, label);

        Pair send_pair = make_pair(50);
        std::snprintf(label, sizeof(label), "encoder %s %s",
                      row.expect_valid ? "accepts" : "rejects", row.name);
        REQUIRE(send_pair.left->send_msg(direct) == row.expect_valid, label);

        // Raw-frame: patch a known-valid frame's tail (and its job_id,
        // which lives just before the tail, right after the 4-byte length
        // prefix and 4-byte message-type header) directly on the wire.
        Bytes frame = encode_usecs_frame(50, fixture_usecs());
        if (frame.size() >= 12) {
            uint32_t network_job_id = htonl(row.job_id);
            std::memcpy(frame.data() + 8, &network_job_id, sizeof(network_job_id));
        }
        // The three cache words are followed by TU sequence (2), C GUID (4),
        // nonce (2), and epoch (2) identity words from the frame end.
        set_tail_word64(frame, 11, row.epoch);
        set_tail_word64(frame, 9, row.nonce);
        set_tail_word(frame, 3, row.cache_port);
        set_tail_word(frame, 2, row.cache_protocol);
        set_tail_word(frame, 1, row.cache_mask);
        std::snprintf(label, sizeof(label), "raw-frame decoder %s %s",
                      row.expect_valid ? "accepts" : "rejects", row.name);
        if (row.expect_valid) {
            Pair raw_pair = make_pair(50);
            const bool wrote = !frame.empty()
                && send(raw_pair.left->fd, frame.data(), frame.size(), 0)
                    == static_cast<ssize_t>(frame.size());
            Msg *wire = wrote ? raw_pair.right->get_msg(2, true) : nullptr;
            REQUIRE(wrote && wire != nullptr, label);
            delete wire;
        } else {
            REQUIRE(decoder_rejects(frame), label);
        }
    }
}

// BigOracle's fixture-trap guidance (d23d9c5d HOLD): UseCSMsg::valid_payload
// now refuses to let a present-cache/absent-identity UseCSMsg exist as a
// successfully decoded (or successfully sent) object at all, so no live
// wire path can any longer hand Daemon::scheduler_use_cs the one input its
// own defensive re-check (usecs_cache_handoff_admissible, services/comm.h)
// exists to catch.  Rather than let that defense-in-depth become silently
// unexercisable, its logic was factored out into that small, pure,
// independently callable helper; this exercises it DIRECTLY with hand-
// constructed objects the wire itself would now refuse to carry, proving
// the daemon-side check still stands on its own regardless of whether a
// real socket can reach it today.
static void test_daemon_cache_handoff_admissible_helper()
{
    const uint64_t epoch = UINT64_C(0x1020304050607080);
    const uint64_t nonce = UINT64_C(0x8877665544332211);
    const uint32_t job_id = UINT32_C(0x0000beef);
    const uint32_t cache_port = UINT32_C(0x0000cafe);

    const struct Row {
        uint32_t job_id;
        uint64_t epoch;
        uint64_t nonce;
        uint32_t cache_port;
        uint32_t cache_protocol;
        uint32_t cache_mask;
        bool expect_admissible;
        const char *name;
    } rows[] = {
        {job_id, epoch, nonce, cache_port, CACHE_WIRE_REVISION,
         CACHE_PROFILE_ZSTD_TU, true, "complete identity, cache present (baseline)"},
        {job_id, epoch, nonce, cache_port, CACHE_WIRE_REVISION,
         CACHE_PROFILE_ZSTD_TU | CACHE_PROFILE_ZSTD_ROUTE, false,
         "multiple profiles in one assignment"},
        {0, epoch, nonce, cache_port, CACHE_WIRE_REVISION,
         CACHE_PROFILE_ZSTD_TU, false,
         "zero job_id, epoch+nonce present, cache present -- the row "
         "job_id!=0's omission let through"},
        {job_id, 0, nonce, cache_port, CACHE_WIRE_REVISION,
         CACHE_PROFILE_ZSTD_TU, false, "zero epoch, cache present"},
        {job_id, epoch, 0, cache_port, CACHE_WIRE_REVISION,
         CACHE_PROFILE_ZSTD_TU, false, "zero nonce, cache present"},
        {job_id, 0, 0, 0, 0, 0, false,
         "wholly absent cache triple (nothing to retain either way)"},
    };
    for (const Row& row : rows) {
        const UseCSMsg direct("x86_64", "cache-worker", UINT32_C(0x00002805),
                              row.job_id, true, UINT32_C(7), UINT32_C(0),
                              row.epoch, row.nonce, row.cache_port,
                              row.cache_protocol, row.cache_mask);
        char label[192];
        std::snprintf(label, sizeof(label), "daemon helper %s: %s",
                      row.expect_admissible ? "admits" : "refuses", row.name);
        REQUIRE(usecs_cache_handoff_admissible(direct) == row.expect_admissible,
                label);
    }
}

static void test_cache_handoff_completion_binding()
{
    const uint32_t job_id = UINT32_C(0x5201);
    const uint64_t epoch = UINT64_C(0x5200000000000001);
    const uint64_t nonce = UINT64_C(0x1122334455667788);
    const uint64_t c_guid = UINT64_C(0x8877665544332211);
    const uint64_t tu_seq = UINT64_C(0x19);
    const uint32_t base_flags =
        static_cast<uint32_t>(JobDoneMsg::FROM_SUBMITTER) |
        static_cast<uint32_t>(JobDoneMsg::P50CacheRouteObservation);
    const JobDoneMsg exact(
        job_id, 0, base_flags, 0, epoch, nonce, c_guid, tu_seq);
    REQUIRE(p50_cache_handoff_completion_matches(
                exact, job_id, epoch, nonce, c_guid, tu_seq,
                CACHE_PROFILE_P29V1),
            "exact successful submitter completion may advance the warm hint");

    const JobDoneMsg wrong_job(job_id + 1, 0, base_flags, 0, epoch, nonce,
                               c_guid, tu_seq);
    const JobDoneMsg wrong_epoch(job_id, 0, base_flags, 0, epoch + 1, nonce,
                                 c_guid, tu_seq);
    const JobDoneMsg wrong_nonce(job_id, 0, base_flags, 0, epoch, nonce + 1,
                                 c_guid, tu_seq);
    const JobDoneMsg wrong_c_guid(job_id, 0, base_flags, 0, epoch, nonce,
                                  c_guid + 1, tu_seq);
    const JobDoneMsg wrong_tu(job_id, 0, base_flags, 0, epoch, nonce,
                              c_guid, tu_seq + 1);
    const JobDoneMsg failed(job_id, 1, base_flags, 0, epoch, nonce,
                            c_guid, tu_seq);
    const JobDoneMsg from_server(job_id, 0, JobDoneMsg::FROM_SERVER, 0,
                                 epoch, nonce, c_guid, tu_seq);
    const JobDoneMsg ordinary_submitter(
        job_id, 0, JobDoneMsg::FROM_SUBMITTER, 0, epoch, nonce,
        c_guid, tu_seq);
    const JobDoneMsg observation_with_unknown_flag(
        job_id, 0,
        base_flags |
            static_cast<uint32_t>(JobDoneMsg::UnknownJobId),
        0, epoch, nonce, c_guid, tu_seq);
    const JobDoneMsg permanent(
        job_id, 106,
        base_flags |
            static_cast<uint32_t>(
                JobDoneMsg::P50PermanentLocalCapabilityFailure),
        0, epoch, nonce, c_guid, tu_seq);
    const JobDoneMsg replacement(
        job_id, 106,
        base_flags |
            static_cast<uint32_t>(
                JobDoneMsg::P50LocalSidecarReplacementRequired),
        0, epoch, nonce, c_guid, tu_seq);
    const auto matches = [&](const JobDoneMsg& value) {
        return p50_cache_handoff_completion_matches(
            value, job_id, epoch, nonce, c_guid, tu_seq,
            CACHE_PROFILE_P29V1);
    };
    REQUIRE(!matches(wrong_job) && !matches(wrong_epoch) &&
                !matches(wrong_nonce) && !matches(wrong_c_guid) &&
                !matches(wrong_tu) && !matches(failed) &&
                !matches(from_server) && !matches(ordinary_submitter) &&
                !matches(observation_with_unknown_flag) &&
                !p50_cache_handoff_completion_matches(
                    exact, 0, epoch, nonce, c_guid, tu_seq,
                    CACHE_PROFILE_P29V1) &&
                !p50_cache_handoff_completion_matches(
                    exact, job_id, 0, nonce, c_guid, tu_seq,
                    CACHE_PROFILE_P29V1) &&
                !p50_cache_handoff_completion_matches(
                    exact, job_id, epoch, 0, c_guid, tu_seq,
                    CACHE_PROFILE_P29V1),
            "wrong job, epoch, nonce, result, origin, message kind, flags, or absent binding cannot advance the warm hint");
    REQUIRE(p50_cache_route_observation_kind(
                failed, job_id, epoch, nonce, c_guid, tu_seq,
                CACHE_PROFILE_P29V1) ==
                P50CacheRouteObservationKind::Failure &&
                p50_cache_route_observation_kind(
                    permanent, job_id, epoch, nonce, c_guid, tu_seq,
                    CACHE_PROFILE_P29V1) ==
                    P50CacheRouteObservationKind::PermanentLocalProfileFailure &&
                p50_cache_route_observation_kind(
                    permanent, job_id, epoch, nonce, c_guid, tu_seq,
                    CACHE_PROFILE_ZSTD_TU) ==
                    P50CacheRouteObservationKind::Invalid &&
                p50_cache_route_observation_kind(
                    replacement, job_id, epoch, nonce, c_guid, tu_seq,
                    CACHE_PROFILE_ZSTD_ROUTE) ==
                    P50CacheRouteObservationKind::
                        LocalSidecarReplacementRequired,
            "generic, permanent-profile, and whole-sidecar replacement observations remain distinct");
}

static void test_cache_advertisement_predicate_matches_projection_law()
{
    REQUIRE(cache_advertisement_is_valid_present(
                10245, CACHE_WIRE_REVISION,
                CACHE_PROFILE_ZSTD_TU | CACHE_PROFILE_ZSTD_ROUTE) &&
                !cache_assignment_is_valid_present(
                    10245, CACHE_WIRE_REVISION,
                    CACHE_PROFILE_ZSTD_TU | CACHE_PROFILE_ZSTD_ROUTE),
            "Login capability sets and singleton UseCS assignments were conflated");
    /* scheduler/scheduler.cpp's project_cache_handoff and UseCSMsg's own
       wire validator both resolve a retained/received snapshot through
       exactly this pair of predicates.  A live CompileServer can never
       actually observe an inconsistent ("mixed") combination through the
       wire -- LoginMsg::valid_payload and UseCSMsg::valid_payload above
       both already enforce the identical law on receipt, on every path,
       for every sender -- so this exercises the defensive branch directly:
       every combination the wire refuses to carry also PROJECTS as wholly
       absent rather than partially or incorrectly advertised, should it
       ever be reached by any future, non-wire caller. */
    const struct Mixed {
        uint32_t port;
        uint32_t protocol;
        uint32_t profiles;
        const char *name;
    } mixed[] = {
        {0, CACHE_WIRE_REVISION, CACHE_PROFILE_ZSTD_TU,
         "zero port, live protocol+mask"},
        {10245, 0, CACHE_PROFILE_ZSTD_TU, "live port+mask, zero protocol"},
        {10245, CACHE_WIRE_REVISION, 0, "live port+protocol, zero mask"},
        {10245, 49, CACHE_PROFILE_ZSTD_TU,
         "live port+mask, stale pre-CacheWire protocol"},
        {10245, CACHE_WIRE_REVISION, UINT32_C(0x00000008),
         "live port+protocol, removed profile bit"},
        {70000, CACHE_WIRE_REVISION, CACHE_PROFILE_ZSTD_TU,
         "out-of-range port, live protocol+mask"},
    };
    for (const Mixed& value : mixed) {
        const bool present = cache_advertisement_is_valid_present(
            value.port, value.protocol, value.profiles);
        const bool absent = cache_advertisement_is_wholly_absent(
            value.port, value.protocol, value.profiles);
        char label[176];
        std::snprintf(label, sizeof(label),
                      "mixed retained snapshot (%s) is neither valid-present "
                      "nor wholly-absent -- projects absent", value.name);
        REQUIRE(!present && !absent, label);
    }
}

static void test_revision_one_profile_registry()
{
    static_assert(static_cast<uint16_t>(ProfileId::P29V1) == 1);
    static_assert(static_cast<uint16_t>(ProfileId::ZSTD_TU) == 2);
    static_assert(static_cast<uint16_t>(ProfileId::ZSTD_ROUTE) == 3);
    static_assert(profile_bit(ProfileId::P29V1) == CACHE_PROFILE_P29V1);
    static_assert(profile_bit(ProfileId::ZSTD_TU) == CACHE_PROFILE_ZSTD_TU);
    static_assert(profile_bit(ProfileId::ZSTD_ROUTE)
                  == CACHE_PROFILE_ZSTD_ROUTE);
    static_assert(kKnownProfileMask == UINT32_C(0x00000007));
    static_assert(CACHE_ADVERTISABLE_PROFILE_MASK == kKnownProfileMask);
    REQUIRE(profile_name(ProfileId::P29V1) == "p29_v1"
                && profile_name(ProfileId::ZSTD_TU) == "zstd_tu"
                && profile_name(ProfileId::ZSTD_ROUTE) == "zstd_route",
            "revision-one profile IDs and labels are exact");

    {
        SessionHello hello;
        hello.c_store_guid = Id128::from_u64(91);
        hello.supported_profiles = profile_bit(ProfileId::ZSTD_ROUTE);
        const SessionSelection selected = negotiate_session(
            hello, kP50WireRevision, profile_bit(ProfileId::ZSTD_ROUTE));
        REQUIRE(selected.negotiated_profiles == profile_bit(ProfileId::ZSTD_ROUTE),
                "revision-one ZSTD_ROUTE profile negotiates");
    }

    {
        SessionHello hello;
        hello.c_store_guid = Id128::from_u64(92);
        hello.supported_profiles = UINT32_C(0x00000008);
        bool negotiation_rejected = false;
        try {
            (void)negotiate_session(hello, kP50WireRevision,
                                    UINT32_C(0x00000008));
        } catch (const std::invalid_argument&) {
            negotiation_rejected = true;
        }
        REQUIRE(negotiation_rejected,
                "a removed revision-one profile bit cannot negotiate");

        TxBegin begin;
        begin.history_nonce = HistoryNonce{1};
        begin.profile = static_cast<ProfileId>(4);
        bool transaction_rejected = false;
        try {
            (void)compute_transaction_digest(begin, {});
        } catch (const std::invalid_argument&) {
            transaction_rejected = true;
        }
        REQUIRE(transaction_rejected,
                "a removed revision-one profile cannot enter a transaction");
    }
}

static void test_daemon_cache_recovery_deferral_policy()
{
    using icecc::p50::daemon::should_defer_cache_capable_getcs;

    REQUIRE(should_defer_cache_capable_getcs(
                1, CACHE_PROFILE_P29V1, false, true),
            "strict P50 cache offer waits for an in-progress successor lease "
            "without depending on the separate remote-required policy");
    REQUIRE(!should_defer_cache_capable_getcs(
                2, CACHE_PROFILE_P29V1, false, true) &&
                !should_defer_cache_capable_getcs(1, 0, false, true) &&
                !should_defer_cache_capable_getcs(
                    1, CACHE_PROFILE_P29V1, true, true) &&
                !should_defer_cache_capable_getcs(
                    1, CACHE_PROFILE_P29V1, false, false),
            "batch, cache-absent, already-ready, and terminal requests are "
            "never retained by the successor-lease deferral policy");
}

int main()
{
    test_legacy_bytes();
    test_p50_round_trip_and_validation();
    test_p51_login_and_opt_in_selection_matrix();
    test_getcs_cache_request_wire_and_laws();
    test_usecs_legacy_bytes();
    test_usecs_p50_round_trip_and_validation();
    test_usecs_identity_binding_law();
    test_daemon_cache_handoff_admissible_helper();
    test_cache_handoff_completion_binding();
    test_cache_advertisement_predicate_matches_projection_law();
    test_revision_one_profile_registry();
    test_daemon_cache_recovery_deferral_policy();
    std::fprintf(stderr, "%s: %d failure(s)\n",
                 failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
