/* Focused Protocol-50 ResultDispositionMsg wire and identity tests. */
#include "comm.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
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
    if (!pair.left || !pair.right)
        std::exit(2);
    pair.left->protocol = pair.right->protocol = protocol;
    return pair;
}

using Bytes = std::vector<unsigned char>;

static CompileInputIdentity present_input()
{
    CompileInputIdentity input;
    input.profile = CompileInputIdentity::ZstdTuProfile;
    for (size_t i = 0; i != input.c_store_guid.size(); ++i) {
        input.c_store_guid[i] = static_cast<uint8_t>(0x10 + i);
        input.raw_digest[i] = static_cast<uint8_t>(0xa0 + i);
    }
    input.tu_seq = UINT64_C(0x0102030405060708);
    input.raw_bytes = UINT64_C(0x1112131415161718);
    input.attempt_id = UINT64_C(0x2122232425262728);
    input.request_id = UINT64_C(0x2122232425262728);
    return input;
}

static ResultDispositionMsg present_message(
    ResultDispositionMsg::Disposition disposition = ResultDispositionMsg::Accepted)
{
    return ResultDispositionMsg(UINT32_C(0x01020304),
                                UINT64_C(0x1112131415161718),
                                UINT64_C(0x2122232425262728),
                                present_input(), disposition);
}

static Bytes encode_frame(const Msg &message, int protocol = PROTOCOL_VERSION)
{
    Pair pair = make_pair(protocol);
    if (!pair.left->send_msg(message))
        return {};
    uint32_t net_length = 0;
    if (recv(pair.right->fd, &net_length, sizeof(net_length), MSG_WAITALL)
            != ssize_t(sizeof(net_length)))
        return {};
    const size_t body = ntohl(net_length);
    Bytes frame(sizeof(net_length) + body);
    std::memcpy(frame.data(), &net_length, sizeof(net_length));
    if (recv(pair.right->fd, frame.data() + sizeof(net_length), body,
             MSG_WAITALL) != ssize_t(body))
        return {};
    return frame;
}

static bool rejected_frame(const Bytes &frame, int protocol = PROTOCOL_VERSION)
{
    Pair pair = make_pair(protocol);
    if (send(pair.left->fd, frame.data(), frame.size(), 0)
            != ssize_t(frame.size()))
        return false;
    Msg *decoded = pair.right->get_msg(2, true);
    const bool rejected = decoded == nullptr;
    delete decoded;
    return rejected;
}

static void put_u32(Bytes &bytes, size_t offset, uint32_t value)
{
    const uint32_t network = htonl(value);
    std::memcpy(bytes.data() + offset, &network, sizeof(network));
}

static void test_exact_wire_and_duplicate()
{
    static_assert(Msg::RESULT_DISPOSITION == UINT32_C(0x50f00002));
    static_assert(Msg::RESULT_DISPOSITION != CACHE_SESSION_READY_MAGIC);
    const ResultDispositionMsg expected = present_message();
    const Bytes frame = encode_frame(expected);
    REQUIRE(frame.size() == 4 + 4 + ResultDispositionMsg::FixedPayloadWords * 4,
            "ResultDispositionMsg uses its fixed payload shape");

    /* Header length, message type, then job/assignment and the exact
       CompileInputIdentity fields in the same order as CompileFileMsg. */
    REQUIRE(frame.size() >= 8 && ntohl(*reinterpret_cast<const uint32_t *>(frame.data()))
                == 4 + ResultDispositionMsg::FixedPayloadWords * 4,
            "ResultDispositionMsg frame length is exact");
    uint32_t type = 0;
    std::memcpy(&type, frame.data() + 4, sizeof(type));
    REQUIRE(ntohl(type) == UINT32_C(0x50f00002),
            "ResultDispositionMsg uses the collision-free P50 type");

    Pair pair = make_pair(PROTOCOL_VERSION);
    REQUIRE(pair.left->send_msg(expected) && pair.left->send_msg(expected),
            "duplicate disposition frames are wire-valid and idempotent");
    Msg *first_base = pair.right->get_msg(2, true);
    Msg *second_base = pair.right->get_msg(2, true);
    auto *first = dynamic_cast<ResultDispositionMsg *>(first_base);
    auto *second = dynamic_cast<ResultDispositionMsg *>(second_base);
    REQUIRE(first && second && *first == expected && *second == expected,
            "duplicate disposition preserves every identity field");
    const ResultDispositionMsg conflicting =
        present_message(ResultDispositionMsg::DefinitiveCancel);
    REQUIRE(conflicting.same_identity(expected) && !(conflicting == expected),
            "same-identity conflicting dispositions remain distinguishable");
    delete first_base;
    delete second_base;
}

static void test_absent_present_and_mutations()
{
    ResultDispositionMsg absent(UINT32_C(7), UINT64_C(8), UINT64_C(9),
                                CompileInputIdentity{},
                                ResultDispositionMsg::DefinitiveCancel);
    Pair pair = make_pair(PROTOCOL_VERSION);
    REQUIRE(!pair.left->send_msg(absent),
            "wholly absent compiler input is refused before framing");

    CompileInputIdentity partial = present_input();
    partial.request_id = 0;
    ResultDispositionMsg partial_msg(UINT32_C(7), UINT64_C(8), UINT64_C(9),
                                     partial, ResultDispositionMsg::Accepted);
    REQUIRE(!make_pair(PROTOCOL_VERSION).left->send_msg(partial_msg),
            "partial compiler input is refused before framing");

    const ResultDispositionMsg original = present_message();
    const auto check_mutation = [&](const std::function<void(ResultDispositionMsg &)> &mutate,
                                    const char *label) {
        ResultDispositionMsg changed = original;
        mutate(changed);
        REQUIRE(changed.valid_payload() && !changed.same_identity(original), label);
    };
    check_mutation([](ResultDispositionMsg &m) { ++m.job_id; },
                   "jobID mutation changes disposition identity");
    check_mutation([](ResultDispositionMsg &m) { ++m.assignment_epoch_lo; },
                   "assignment epoch mutation changes disposition identity");
    {
        ResultDispositionMsg changed = original;
        ++changed.assignment_nonce_lo;
        REQUIRE(!changed.valid_payload() && !changed.same_identity(original),
                "assignment nonce mutation breaks input binding");
    }
    {
        ResultDispositionMsg changed = original;
        changed.compile_input.profile =
            CompileInputIdentity::ZstdRouteProfile + 1;
        REQUIRE(!changed.valid_payload() && !changed.same_identity(original),
                "profile mutation is rejected and changes disposition identity");
    }
    check_mutation([](ResultDispositionMsg &m) { ++m.compile_input.c_store_guid[0]; },
                   "C-store GUID mutation changes disposition identity");
    check_mutation([](ResultDispositionMsg &m) { ++m.compile_input.tu_seq; },
                   "TU sequence mutation changes disposition identity");
    check_mutation([](ResultDispositionMsg &m) { ++m.compile_input.raw_bytes; },
                   "raw byte-count mutation changes disposition identity");
    check_mutation([](ResultDispositionMsg &m) { ++m.compile_input.raw_digest[0]; },
                   "raw digest mutation changes disposition identity");
    {
        ResultDispositionMsg changed = original;
        ++changed.compile_input.attempt_id;
        REQUIRE(!changed.valid_payload() && !changed.same_identity(original),
                "attempt ID mutation breaks assignment-nonce binding");
    }
    {
        ResultDispositionMsg changed = original;
        ++changed.compile_input.request_id;
        REQUIRE(!changed.valid_payload() && !changed.same_identity(original),
                "request ID mutation breaks assignment-nonce binding");
    }
}

static void test_rejections_and_protocol_gates()
{
    const Bytes valid = encode_frame(present_message());
    REQUIRE(!valid.empty(), "valid disposition frame available for mutants");

    Bytes truncated = valid;
    truncated.resize(truncated.size() - 4);
    put_u32(truncated, 0, static_cast<uint32_t>(truncated.size() - 4));
    REQUIRE(rejected_frame(truncated), "truncated disposition body is rejected");

    Bytes trailing = valid;
    trailing.insert(trailing.end(), 4, 0);
    put_u32(trailing, 0, static_cast<uint32_t>(trailing.size() - 4));
    REQUIRE(rejected_frame(trailing), "trailing disposition body is rejected");

    Bytes unknown = valid;
    put_u32(unknown, unknown.size() - 4, 99);
    REQUIRE(rejected_frame(unknown), "unknown disposition value is rejected");

    ResultDispositionMsg bad = present_message();
    bad.disposition = 99;
    REQUIRE(!make_pair(PROTOCOL_VERSION).left->send_msg(bad),
            "unknown disposition is refused before framing");

    ResultDispositionMsg partial_assignment = present_message();
    partial_assignment.assignment_nonce_hi = 0;
    partial_assignment.assignment_nonce_lo = 0;
    REQUIRE(!make_pair(PROTOCOL_VERSION).left->send_msg(partial_assignment),
            "partial assignment identity is refused before framing");

    for (const int protocol : {43, 48, 49, 51}) {
        Pair pair = make_pair(protocol);
        REQUIRE(!pair.left->send_msg(present_message()),
                "ResultDispositionMsg is refused outside exact protocol 50");
        unsigned char byte = 0;
        REQUIRE(recv(pair.right->fd, &byte, sizeof(byte), MSG_DONTWAIT) < 0
                    && (errno == EAGAIN || errno == EWOULDBLOCK),
                "pre-P50/non-P50 refusal emits no frame bytes");
    }
}

static void test_compile_result_bytes_unchanged()
{
    CompileResultMsg expected;
    expected.err = "err";
    expected.out = "out";
    expected.status = 17;
    expected.was_out_of_memory = true;
    expected.have_dwo_file = true;
    const Bytes p43 = encode_frame(expected, 43);
    const Bytes p48 = encode_frame(expected, 48);
    REQUIRE(!p43.empty() && p43 == p48,
            "existing CompileResultMsg P43/P48 bytes remain identical");

    Pair pair = make_pair(48);
    REQUIRE(pair.left->send_msg(expected), "existing P48 CompileResultMsg still sends");
    Msg *decoded = pair.right->get_msg(2, true);
    auto *result = dynamic_cast<CompileResultMsg *>(decoded);
    REQUIRE(result && result->err == expected.err && result->out == expected.out
                && result->status == expected.status
                && result->was_out_of_memory == expected.was_out_of_memory
                && result->have_dwo_file == expected.have_dwo_file,
            "existing CompileResultMsg still decodes unchanged");
    delete decoded;
}

} // namespace

int main()
{
    test_exact_wire_and_duplicate();
    test_absent_present_and_mutations();
    test_rejections_and_protocol_gates();
    test_compile_result_bytes_unchanged();
    return failures ? 1 : 0;
}
