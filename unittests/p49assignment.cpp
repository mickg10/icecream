/* Protocol-49 production-codec gate.
   Uses MsgChannel and the real message classes; no parallel encoder is used
   for new frames.  The one literal byte fixture is the frozen protocol-48
   UseCS projection inherited from the exact P48 base. */

#include "comm.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <thread>

static int failures = 0;

#define REQUIRE(cond, text) do {                                      \
    if (cond) { std::fprintf(stderr, "ok       - %s\n", text); }      \
    else { std::fprintf(stderr, "FAILED   - %s\n", text); ++failures; } \
} while (0)

struct Pair {
    MsgChannel *left = nullptr;
    MsgChannel *right = nullptr;
    Pair() = default;
    Pair(const Pair &) = delete;
    Pair &operator=(const Pair &) = delete;
    Pair(Pair &&other) noexcept
        : left(other.left), right(other.right)
    {
        other.left = other.right = nullptr;
    }
    ~Pair() { delete left; delete right; }
};

static Pair make_pair()
{
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        std::perror("socketpair");
        std::exit(2);
    }
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    Pair pair;
    std::thread a([&] {
        pair.left = Service::createChannel(
            fds[0], reinterpret_cast<sockaddr *>(&address), sizeof(address));
    });
    std::thread b([&] {
        pair.right = Service::createChannel(
            fds[1], reinterpret_cast<sockaddr *>(&address), sizeof(address));
    });
    a.join();
    b.join();
    if (!pair.left || !pair.right) {
        std::fprintf(stderr, "channel handshake failed\n");
        std::exit(2);
    }
    return pair;
}

template<typename T>
static T *round_trip(Pair &pair, const T &sent, Msg::Value type)
{
    if (!pair.left->send_msg(sent)) {
        return nullptr;
    }
    Msg *wire = pair.right->get_msg(2, true);
    if (!wire || *wire != type) {
        delete wire;
        return nullptr;
    }
    T *typed = dynamic_cast<T *>(wire);
    if (!typed) {
        delete wire;
    }
    return typed;
}

static void test_new_round_trips()
{
    static_assert(Msg::ASSIGN_PREPARE == 0x49f00000);
    static_assert(Msg::ASSIGN_READY == 0x49f00001);
    static_assert(Msg::REVOKE_BEFORE_START == 0x49f00002);
    static_assert(Msg::REVOKE_RESULT == 0x49f00003);
    static_assert(ConfCSMsg::Legacy == 0);
    static_assert(ConfCSMsg::Advisory == 1);
    static_assert(ConfCSMsg::EnforcingCompat == 2);
    static_assert(ConfCSMsg::StrictNonce == 3);
    static_assert(RevokeResultMsg::Revoked == 0);
    static_assert(RevokeResultMsg::ClaimedOrLater == 1);

    Pair pair = make_pair();
    const uint64_t epoch = UINT64_C(0x1122334455667788);
    const uint64_t nonce = UINT64_C(0x8877665544332211);
    const uint32_t wire_id = UINT32_C(0xa1b2c3d4);

    ConfCSMsg *conf = round_trip(
        pair, ConfCSMsg(epoch, ConfCSMsg::EnforcingCompat), Msg::CS_CONF);
    REQUIRE(conf && conf->epoch() == epoch
                && conf->fence_mode == ConfCSMsg::EnforcingCompat,
            "P49 ConfCS round-trips epoch and immutable policy");
    delete conf;

    AssignPrepareMsg *prepare = round_trip(
        pair, AssignPrepareMsg(epoch, wire_id, nonce, 73), Msg::ASSIGN_PREPARE);
    REQUIRE(prepare && prepare->epoch() == epoch && prepare->wire_id == wire_id
                && prepare->nonce() == nonce && prepare->submitter_hostid == 73
                && prepare->flags == 0,
            "PREPARE round-trips the complete assignment identity");
    delete prepare;

    AssignReadyMsg *ready = round_trip(
        pair, AssignReadyMsg(epoch, wire_id, nonce), Msg::ASSIGN_READY);
    REQUIRE(ready && ready->epoch() == epoch && ready->wire_id == wire_id
                && ready->nonce() == nonce,
            "READY round-trips the complete assignment identity");
    delete ready;

    RevokeBeforeStartMsg *revoke = round_trip(
        pair, RevokeBeforeStartMsg(epoch, wire_id, nonce),
        Msg::REVOKE_BEFORE_START);
    REQUIRE(revoke && revoke->epoch() == epoch && revoke->wire_id == wire_id
                && revoke->nonce() == nonce,
            "REVOKE round-trips the complete assignment identity");
    delete revoke;

    RevokeResultMsg *terminal = round_trip(
        pair, RevokeResultMsg(epoch, wire_id, nonce,
                              RevokeResultMsg::ClaimedOrLater),
        Msg::REVOKE_RESULT);
    REQUIRE(terminal && terminal->epoch() == epoch
                && terminal->wire_id == wire_id && terminal->nonce() == nonce
                && terminal->result == RevokeResultMsg::ClaimedOrLater,
            "terminal result round-trips identity and outcome");
    delete terminal;
}

static void test_frozen_p48_usecs_bytes()
{
    Pair pair = make_pair();
    pair.left->protocol = 48;
    pair.right->protocol = 48;

    UseCSMsg old("p", "h", 10245, UINT32_C(0x01020304), true,
                 UINT32_C(0xa0b0c0d0), UINT32_C(0x0a0b0c0d));
    REQUIRE(pair.left->send_msg(old), "P48 UseCS frame was emitted");

    const std::array<unsigned char, 40> expected {{
        0x00,0x00,0x00,0x24, 0x00,0x00,0x00,0x48,
        0x01,0x02,0x03,0x04, 0x00,0x00,0x28,0x05,
        0x00,0x00,0x00,0x02, 0x68,0x00,
        0x00,0x00,0x00,0x02, 0x70,0x00,
        0x00,0x00,0x00,0x01, 0xa0,0xb0,0xc0,0xd0,
        0x0a,0x0b,0x0c,0x0d
    }};
    std::array<unsigned char, 40> actual {};
    size_t have = 0;
    while (have < actual.size()) {
        const ssize_t n = recv(pair.right->fd, actual.data() + have,
                               actual.size() - have, 0);
        if (n <= 0) {
            break;
        }
        have += static_cast<size_t>(n);
    }
    REQUIRE(have == actual.size() && actual == expected,
            "protocol-48 UseCS bytes remain frozen");
}

static void test_frozen_p48_confcs_bytes()
{
    Pair pair = make_pair();
    pair.left->protocol = 48;
    pair.right->protocol = 48;

    /* Deliberately populate the P49-only fields: the production encoder must
       omit them completely on a negotiated P48 link. */
    ConfCSMsg config(UINT64_C(0x1122334455667788),
                     ConfCSMsg::StrictNonce);
    REQUIRE(pair.left->send_msg(config), "P48 ConfCS frame was emitted");

    const std::array<unsigned char, 21> expected {{
        0x00,0x00,0x00,0x11, 0x00,0x00,0x00,0x5c,
        0x00,0x00,0x00,0x03, 0x00,0x00,0x00,0x24,
        0x00,0x00,0x00,0x01, 0x00
    }};
    std::array<unsigned char, 21> actual {};
    size_t have = 0;
    while (have < actual.size()) {
        const ssize_t n = recv(pair.right->fd, actual.data() + have,
                               actual.size() - have, 0);
        if (n <= 0) {
            break;
        }
        have += static_cast<size_t>(n);
    }
    REQUIRE(have == actual.size() && actual == expected,
            "protocol-48 ConfCS bytes remain frozen");
}

static void test_old_decoder_rejects_p49_control()
{
    Pair pair = make_pair();
    pair.left->protocol = 48;
    pair.right->protocol = 48;
    REQUIRE(pair.left->send_msg(AssignPrepareMsg(7, 11, 13, 17)),
            "test control frame reached the old-version boundary");
    Msg *msg = pair.right->get_msg(2, true);
    REQUIRE(msg == nullptr,
            "protocol-48 decoder refuses protocol-49 control vocabulary");
    delete msg;
}

int main()
{
    test_new_round_trips();
    test_frozen_p48_usecs_bytes();
    test_frozen_p48_confcs_bytes();
    test_old_decoder_rejects_p49_control();
    std::fprintf(stderr, "%s: %d failure(s)\n",
                 failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
