/* Protocol-50 assignment-identity carrier gate using production codecs. */
#include "comm.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

static int failures;
#define REQUIRE(c, t) do { const bool ok_ = (c); std::fprintf(stderr, "%s - %s\n", ok_ ? "ok    " : "FAILED", t); if (!ok_) ++failures; } while (0)

struct Pair {
    MsgChannel *left = nullptr;
    MsgChannel *right = nullptr;
    Pair() = default;
    Pair(const Pair &) = delete;
    Pair &operator=(const Pair &) = delete;
    Pair(Pair &&o) noexcept : left(o.left), right(o.right) { o.left = o.right = nullptr; }
    ~Pair() { delete left; delete right; }
};

static Pair make_pair(int protocol)
{
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds)) std::exit(2);
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    Pair p;
    std::thread a([&] { p.left = Service::createChannel(fds[0], reinterpret_cast<sockaddr *>(&address), sizeof(address)); });
    std::thread b([&] { p.right = Service::createChannel(fds[1], reinterpret_cast<sockaddr *>(&address), sizeof(address)); });
    a.join(); b.join();
    if (!p.left || !p.right) std::exit(2);
    p.left->protocol = p.right->protocol = protocol;
    return p;
}

template<typename T>
static T *round_trip(Pair &p, const T &sent, Msg::Value type)
{
    if (!p.left->send_msg(sent)) return nullptr;
    Msg *m = p.right->get_msg(2, true);
    if (!m || *m != type) { delete m; return nullptr; }
    T *typed = dynamic_cast<T *>(m);
    if (!typed) delete m;
    return typed;
}

static CompileJob make_job(uint32_t id, uint64_t epoch, uint64_t nonce)
{
    CompileJob j;
    j.setLanguage(CompileJob::Lang_CXX);
    j.setJobID(id);
    j.setAssignmentIdentity(epoch, nonce);
    j.setEnvironmentVersion("env");
    j.setTargetPlatform("x86_64");
    j.setCompilerName("g++");
    j.setInputFile("in.ii");
    j.setWorkingDirectory("/tmp");
    j.setOutputFile("out.o");
    j.appendFlag("-O2", Arg_Remote);
    return j;
}

static CompileInputIdentity fixture_compile_input()
{
    CompileInputIdentity input;
    input.profile = CompileInputIdentity::ZstdTuProfile;
    for (size_t index = 0; index != input.c_store_guid.size(); ++index) {
        input.c_store_guid[index] = static_cast<uint8_t>(0x10u + index);
        input.raw_digest[index] = static_cast<uint8_t>(0xa0u + index);
    }
    input.tu_seq = UINT64_C(0x0102030405060708);
    input.raw_bytes = UINT64_C(0x1112131415161718);
    input.attempt_id = UINT64_C(0x2122232425262728);
    input.request_id = UINT64_C(0x3132333435363738);
    return input;
}

static void test_eight_cells()
{
    const uint32_t wire = UINT32_C(0x12345678);
    const uint64_t epoch = UINT64_C(0x1020304050607080);
    const uint64_t nonce = UINT64_C(0x8877665544332211);
    for (int sn = 0; sn != 2; ++sn) for (int cn = 0; cn != 2; ++cn) for (int fn = 0; fn != 2; ++fn) {
        const int sv = sn ? 50 : 43, cv = cn ? 50 : 43, fv = fn ? 50 : 43;
        const bool all50 = sn && cn && fn;
        const uint64_t c_guid = UINT64_C(0x0102030405060708);
        UseCSMsg scheduled("x86_64", "worker", 10245, wire, true, 7, 9,
                           epoch, nonce, c_guid, UINT64_C(0));
        Pair sd = make_pair(std::min(sv, cv));
        UseCSMsg *at_daemon = round_trip(sd, scheduled, Msg::USE_CS);
        Pair dc = make_pair(cv);
        UseCSMsg *at_client = at_daemon ? round_trip(dc, *at_daemon, Msg::USE_CS) : nullptr;
        delete at_daemon;
        CompileJob claim = make_job(1, 0, 0);
        const bool copied = at_client && at_client->applyAssignmentTo(&claim);
        delete at_client;
        Pair cf = make_pair(std::min(cv, fv));
        CompileFileMsg outbound(&claim);
        CompileFileMsg *received = copied ? round_trip(cf, outbound, Msg::COMPILE_FILE) : nullptr;
        CompileJob *admitted = received ? received->takeJob() : nullptr;
        delete received;
        const bool exact = admitted && admitted->jobID() == wire
            && admitted->assignmentEpoch() == (all50 ? epoch : 0)
            && admitted->assignmentNonce() == (all50 ? nonce : 0)
            && admitted->cGuid() == (all50 ? c_guid : 0)
            && admitted->tuSeq() == 0;
        char label[128];
        std::snprintf(label, sizeof(label), "8-cell S%d/C%d/F%d: one wire id, identity %s", sv, cv, fv, all50 ? "exact" : "absent");
        REQUIRE(copied && exact, label);
        delete admitted;
    }
}

using Bytes = std::vector<unsigned char>;
template<typename T>
static Bytes encode_frame(int protocol, const T &msg)
{
    Pair p = make_pair(protocol);
    if (!p.left->send_msg(msg)) return {};
    uint32_t netlen;
    if (recv(p.right->fd, &netlen, 4, MSG_WAITALL) != 4) return {};
    const uint32_t len = ntohl(netlen);
    Bytes bytes(4 + len);
    std::memcpy(bytes.data(), &netlen, 4);
    if (recv(p.right->fd, bytes.data() + 4, len, MSG_WAITALL) != ssize_t(len)) return {};
    return bytes;
}

static Bytes encoded_usecs(int protocol)
{
    UseCSMsg m("p", "h", 10245, UINT32_C(0x01020304), true,
               UINT32_C(0xa0b0c0d0), UINT32_C(0x0a0b0c0d),
               UINT64_C(0x1122334455667788), UINT64_C(0x8877665544332211));
    return encode_frame(protocol, m);
}

static Bytes encoded_compile(int protocol)
{
    CompileJob j = make_job(UINT32_C(0x01020304), UINT64_C(0x1122334455667788), UINT64_C(0x8877665544332211));
    CompileFileMsg m(&j);
    return encode_frame(protocol, m);
}

static Bytes encoded_compile_with_input(int protocol)
{
    CompileJob j = make_job(UINT32_C(0x01020304),
                            UINT64_C(0x1122334455667788),
                            UINT64_C(0x8877665544332211));
    j.setCompileInputIdentity(fixture_compile_input());
    CompileFileMsg m(&j);
    return encode_frame(protocol, m);
}

static bool appended_word_count(const Bytes &oldf, const Bytes &newf, size_t words)
{
    if (newf.size() != oldf.size() + 4 * words || oldf.size() < 4 + 16)
        return false;
    /* The new identity words follow the existing assignment words. */
    const size_t prefix_end = oldf.size() - 16;
    return std::equal(oldf.begin() + 4, oldf.begin() + prefix_end,
                      newf.begin() + 4)
        && std::equal(oldf.begin() + prefix_end, oldf.end(),
                      newf.begin() + prefix_end);
}

static uint64_t fnv1a(const Bytes &bytes)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned char byte : bytes) {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void test_bytes()
{
    const Bytes u43 = encoded_usecs(43), u48 = encoded_usecs(48), u49 = encoded_usecs(49), u50 = encoded_usecs(50);
    REQUIRE(!u43.empty() && fnv1a(u43) == UINT64_C(0xe556229af116ee8d),
            "P43 UseCS matches its retained byte fixture");
    REQUIRE(!u48.empty() && fnv1a(u48) == UINT64_C(0xe556229af116ee8d),
            "P48 UseCS matches its retained byte fixture");
    REQUIRE(!u49.empty() && fnv1a(u49) == UINT64_C(0xe556229af116ee8d),
            "P49 UseCS matches its retained byte fixture");
    /* Protocol 50 appends four assignment words, four C_GUID/TU_SEQ words,
       and the three-word cache-handoff tail. */
    REQUIRE(appended_word_count(u49, u50, 11),
            "P50 UseCS appends assignment and C_GUID/TU_SEQ words plus the cache tail");
    const Bytes f43 = encoded_compile(43), f48 = encoded_compile(48), f49 = encoded_compile(49), f50 = encoded_compile(50);
    REQUIRE(!f43.empty() && fnv1a(f43) == UINT64_C(0x1d040038613f174d),
            "P43 CompileFile matches its retained byte fixture");
    REQUIRE(!f48.empty() && fnv1a(f48) == UINT64_C(0x1d040038613f174d),
            "P48 CompileFile matches its retained byte fixture");
    REQUIRE(!f49.empty() && fnv1a(f49) == UINT64_C(0x1d040038613f174d),
            "P49 CompileFile matches its retained byte fixture");
    REQUIRE(appended_word_count(f49, f50, 25),
            "P50 CompileFile appends assignment, C_GUID/TU_SEQ, and the mandatory input selector");
}

static void test_compile_input_identity()
{
    const CompileInputIdentity expected = fixture_compile_input();
    CompileJob source = make_job(UINT32_C(0x01020304),
                                 UINT64_C(0x1122334455667788),
                                 UINT64_C(0x8877665544332211));
    source.setCompileInputIdentity(expected);
    Pair pair = make_pair(50);
    CompileFileMsg outbound(&source);
    CompileFileMsg *received = round_trip(pair, outbound, Msg::COMPILE_FILE);
    CompileJob *job = received ? received->takeJob() : nullptr;
    delete received;
    const CompileInputIdentity actual = job ? job->compileInputIdentity()
                                            : CompileInputIdentity{};
    REQUIRE(job && job->usesP50Input()
                && actual.profile == expected.profile
                && actual.c_store_guid == expected.c_store_guid
                && actual.tu_seq == expected.tu_seq
                && actual.raw_bytes == expected.raw_bytes
                && actual.raw_digest == expected.raw_digest
                && actual.attempt_id == expected.attempt_id
                && actual.request_id == expected.request_id,
            "P50 CompileFile round-trips the exact InputRecord/compiler attachment identity");
    delete job;

    Pair old = make_pair(49);
    CompileJob old_job = make_job(7, 3, 5);
    old_job.setCompileInputIdentity(expected);
    CompileFileMsg old_message(&old_job);
    REQUIRE(!old.left->send_msg(old_message),
            "P50 compiler input is never erased onto a pre-50 link");
}

static void test_invalid()
{
    { Pair p = make_pair(50); UseCSMsg m("p", "h", 1, 7, true, 1, 0); m.assignment_epoch_lo = 1;
      REQUIRE(!p.left->send_msg(m), "UseCS refuses partial identity"); }
    { Pair p = make_pair(50); UseCSMsg m("p", "h", 1, 0, true, 1, 0, 3, 5);
      REQUIRE(!p.left->send_msg(m), "UseCS refuses full identity with zero wire id"); }
    { Pair p = make_pair(50); CompileJob j = make_job(7, 3, 0); CompileFileMsg m(&j);
      REQUIRE(!p.left->send_msg(m), "CompileFile refuses partial identity"); }
    { Pair p = make_pair(50); CompileJob j = make_job(0, 3, 5); CompileFileMsg m(&j);
      REQUIRE(!p.left->send_msg(m), "CompileFile refuses full identity with zero wire id"); }

    auto rejected_by_decoder = [](const Bytes &bytes) {
        Pair p = make_pair(50);
        const bool wrote = !bytes.empty()
            && send(p.left->fd, bytes.data(), bytes.size(), 0)
                == ssize_t(bytes.size());
        Msg *decoded = wrote ? p.right->get_msg(2, true) : nullptr;
        const bool rejected = wrote && !decoded;
        delete decoded;
        return rejected;
    };

    /* The P50 tail carries assignment, C_GUID/TU_SEQ, and the three cache
       words.  encoded_usecs(50) leaves the cache words absent; zero the
       complete nonce pair to produce a partial assignment. */
    Bytes use_partial = encoded_usecs(50);
    if (use_partial.size() >= 36) {
        std::fill(use_partial.end() - 36, use_partial.end() - 28, 0);
    }
    REQUIRE(rejected_by_decoder(use_partial),
            "production decoder rejects partial UseCS identity");
    Bytes use_zero_wire = encoded_usecs(50);
    if (use_zero_wire.size() >= 12) std::fill(use_zero_wire.begin() + 8, use_zero_wire.begin() + 12, 0);
    REQUIRE(rejected_by_decoder(use_zero_wire),
            "production decoder rejects full UseCS identity with zero wire id");

    Bytes compile_partial = encoded_compile(50);
    if (compile_partial.size() >= 92)
        std::fill(compile_partial.end() - 92, compile_partial.end() - 84, 0);
    REQUIRE(rejected_by_decoder(compile_partial),
            "production decoder rejects partial CompileFile identity");
    Bytes compile_zero_wire = encoded_compile(50);
    if (compile_zero_wire.size() >= 16) std::fill(compile_zero_wire.begin() + 12, compile_zero_wire.begin() + 16, 0);
    REQUIRE(rejected_by_decoder(compile_zero_wire),
            "production decoder rejects full CompileFile identity with zero wire id");

    { Pair p = make_pair(50); CompileJob j = make_job(7, 0, 0);
      j.setCompileInputIdentity(fixture_compile_input()); CompileFileMsg m(&j);
      REQUIRE(!p.left->send_msg(m),
              "CompileFile refuses P50 input without a complete assignment identity"); }
    { Pair p = make_pair(50); CompileJob j = make_job(7, 3, 5);
      CompileInputIdentity partial; partial.profile = CompileInputIdentity::ZstdTuProfile;
      j.setCompileInputIdentity(partial); CompileFileMsg m(&j);
      REQUIRE(!p.left->send_msg(m),
              "CompileFile refuses a partial P50 compiler-input selector"); }

    Bytes short_tail = encoded_compile(50);
    if (short_tail.size() >= 8) {
        short_tail.resize(short_tail.size() - 4);
        const uint32_t payload = htonl(static_cast<uint32_t>(short_tail.size() - 4));
        std::memcpy(short_tail.data(), &payload, sizeof(payload));
    }
    REQUIRE(rejected_by_decoder(short_tail),
            "production decoder rejects a truncated mandatory compiler-input tail");

    Bytes long_tail = encoded_compile(50);
    if (!long_tail.empty()) {
        long_tail.insert(long_tail.end(), 4, 0);
        const uint32_t payload = htonl(static_cast<uint32_t>(long_tail.size() - 4));
        std::memcpy(long_tail.data(), &payload, sizeof(payload));
    }
    REQUIRE(rejected_by_decoder(long_tail),
            "production decoder rejects an extended compiler-input tail");

    Bytes zero_guid = encoded_compile_with_input(50);
    if (zero_guid.size() >= 64)
        std::fill(zero_guid.end() - 64, zero_guid.end() - 48, 0);
    REQUIRE(rejected_by_decoder(zero_guid),
            "production decoder rejects P50 input with a zero C-store GUID");

    Bytes zero_attempt = encoded_compile_with_input(50);
    if (zero_attempt.size() >= 16)
        std::fill(zero_attempt.end() - 16, zero_attempt.end() - 8, 0);
    REQUIRE(rejected_by_decoder(zero_attempt),
            "production decoder rejects P50 input with a zero ATTEMPT_ID");

    Bytes partial_absence = encoded_compile(50);
    if (partial_absence.size() >= 48)
        partial_absence[partial_absence.size() - 48] = 1;
    REQUIRE(rejected_by_decoder(partial_absence),
            "production decoder rejects a noncanonical partial legacy selector");
}

int main()
{
    test_eight_cells();
    test_bytes();
    test_compile_input_identity();
    test_invalid();
    return failures ? 1 : 0;
}
