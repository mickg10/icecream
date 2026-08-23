#include "p50_loopback.h"

#include <boost/asio/buffer.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace icecc::p50;
using tcp = boost::asio::ip::tcp;

[[noreturn]] void fail(std::string_view text) {
    std::cerr << "p50_loopback_test: " << text << '\n';
    std::exit(1);
}

void require(bool value, std::string_view text) {
    if (!value) fail(text);
}

template<class Exception, class Callable>
void require_throws(Callable&& callable, std::string_view text) {
    try {
        callable();
    } catch (const Exception&) {
        return;
    } catch (...) {
        fail(std::string(text) + " (wrong exception)");
    }
    fail(std::string(text) + " (no exception)");
}

std::vector<uint8_t> input_bytes(size_t count, uint32_t salt) {
    std::vector<uint8_t> result(count);
    for (size_t i = 0; i != result.size(); ++i)
        result[i] = static_cast<uint8_t>((i * 131 + i / 19 + salt) & 0xff);
    return result;
}

void write_fragmented(tcp::socket& socket, const Message& message,
                      size_t quantum = 1) {
    const std::vector<uint8_t> frame = encode_frame(message);
    for (size_t offset = 0; offset < frame.size();) {
        const size_t count = std::min(quantum, frame.size() - offset);
        boost::asio::write(
            socket, boost::asio::buffer(frame.data() + offset, count));
        offset += count;
    }
}

Message read_message(tcp::socket& socket) {
    std::array<uint8_t, 4> header{};
    boost::asio::read(socket, boost::asio::buffer(header));
    const uint32_t word = (uint32_t(header[0]) << 24) |
                          (uint32_t(header[1]) << 16) |
                          (uint32_t(header[2]) << 8) |
                          uint32_t(header[3]);
    const MessageType type = static_cast<MessageType>(word >> 24);
    const uint32_t payload_bytes = word & 0x00ffffffU;
    require(payload_bytes <= kInitialMaxFramePayload,
            "test peer received an oversized frame");
    std::vector<uint8_t> payload(payload_bytes);
    if (!payload.empty())
        boost::asio::read(socket, boost::asio::buffer(payload));
    return decode_payload(type, payload);
}

struct ServerThread {
    explicit ServerThread(ZstdLoopbackServer& server)
        : thread([this, &server] {
              try {
                  server.serve_one_connection();
              } catch (...) {
                  error = std::current_exception();
              }
          }) {}

    ~ServerThread() {
        if (thread.joinable()) thread.join();
    }

    void join() {
        if (thread.joinable()) thread.join();
    }

    void require_success() {
        join();
        if (error) {
            try {
                std::rethrow_exception(error);
            } catch (const std::exception& exception) {
                fail(std::string("server unexpectedly failed: ") + exception.what());
            } catch (...) {
                fail("server unexpectedly failed with a non-standard exception");
            }
        }
    }

    template<class Exception>
    void require_failure() {
        join();
        if (!error) fail("server unexpectedly accepted an invalid connection");
        try {
            std::rethrow_exception(error);
        } catch (const Exception&) {
            return;
        } catch (...) {
            fail("server failed through the wrong exception class");
        }
    }

    std::exception_ptr error;
    std::thread thread;
};

SessionHello hello(CStoreGuid c_guid, SessionLimits limits) {
    SessionHello result;
    result.c_store_guid = c_guid;
    result.supported_profiles = profile_bit(ProfileId::P29) |
                                profile_bit(ProfileId::ZSTD_TU);
    result.limits = limits;
    return result;
}

ZstdTuEnvelope envelope_for(const SessionState& state, TuSeq tu_seq,
                            std::span<const uint8_t> input) {
    return encode_zstd_tu(state.history_nonce, state.next_rel_seq, tu_seq,
                          state.state_digest, input, 1);
}

void test_candidate_staging_and_stale_close_fence() {
    const SessionLimits session_limits{4096, 1U << 20};
    CandidateSessionGate gate(profile_bit(ProfileId::ZSTD_TU),
                              session_limits, 2, 10, 20);
    const SessionHello valid = hello(Id128::from_u64(1), session_limits);

    const CandidateSession first = gate.stage(valid);
    require(first.id == 10 && gate.current_session_serial() == 0,
            "staging a candidate installed it prematurely");
    const uint64_t serial1 = gate.activate(first.id);
    require(serial1 == 20 && gate.current_session_serial() == serial1,
            "first candidate did not install the expected session serial");

    const CandidateSession stale = gate.stage(valid);
    gate.note_state_change();
    require_throws<std::logic_error>([&] { (void)gate.activate(stale.id); },
                                     "stale candidate replaced current state");
    gate.reject(stale.id);
    require(gate.current_session_serial() == serial1,
            "stale candidate failure changed the current session");

    const CandidateSession replacement = gate.stage(valid);
    const uint64_t serial2 = gate.activate(replacement.id);
    require(serial2 == 21 && serial2 != serial1,
            "replacement reused a session serial");
    gate.disconnect(serial1);
    require(gate.current_session_serial() == serial2,
            "late close from the old session tore down its replacement");
    gate.disconnect(serial2);
    require(gate.current_session_serial() == 0,
            "current session close did not clear the installed session");

    SessionHello wrong_profile = valid;
    wrong_profile.supported_profiles = profile_bit(ProfileId::P29);
    require_throws<std::invalid_argument>([&] { (void)gate.stage(wrong_profile); },
                                          "candidate with no common profile staged");

    SessionHello undersized = valid;
    undersized.limits.max_frame_payload = kM2MinimumControlPayload - 1;
    require_throws<std::invalid_argument>([&] { (void)gate.stage(undersized); },
                                          "candidate unable to carry TX_BEGIN staged");
}

void test_persistent_cold_then_warm_transfer() {
    boost::asio::io_context server_context;
    boost::asio::io_context client_context;
    const CStoreGuid c_guid = Id128::from_u64(100);
    const FStoreGuid f_guid = Id128::from_u64(200);
    const SessionLimits session_limits{4096, 1U << 20};
    ZstdLoopbackConfig config;
    config.f_store_guid = f_guid;
    config.session_limits = session_limits;
    config.zstd_limits = {1U << 20, 1U << 20};

    std::vector<std::vector<uint8_t>> published;
    ZstdLoopbackServer server(
        server_context, config, c_guid,
        [&](const TxBegin& begin, const TxCommit& commit,
            std::vector<uint8_t> exact) {
            require(commit.history_nonce == begin.history_nonce &&
                        commit.rel_seq == begin.rel_seq &&
                        commit.tu_seq == begin.tu_seq &&
                        commit.raw_digest == begin.raw_digest,
                    "publish callback received a mismatched commit");
            published.push_back(std::move(exact));
        });
    ServerThread running(server);

    ZstdLoopbackClient client(
        client_context,
        tcp::endpoint(boost::asio::ip::address_v4::loopback(), server.port()),
        hello(c_guid, session_limits), 1);
    require(client.state().negotiated_profiles == profile_bit(ProfileId::ZSTD_TU) &&
                !client.state().namespace_present && !client.state().route_present,
            "cold handshake selected the wrong profile or route state");
    client.establish_initial_route(HistoryNonce{9});

    const std::vector<uint8_t> first = input_bytes(256 * 1024, 17);
    const ZstdTuEnvelope first_tx =
        envelope_for(client.state(), TuSeq{1}, first);
    const TxCommit first_commit = client.transfer(first_tx, 37);
    require(first_commit.rel_seq.value == 0 &&
                client.state().next_rel_seq.value == 1,
            "first transfer did not advance REL_SEQ once");

    const std::vector<uint8_t> second;
    const ZstdTuEnvelope second_tx =
        envelope_for(client.state(), TuSeq{2}, second);
    const TxCommit second_commit = client.transfer(second_tx, 19);
    require(second_commit.rel_seq.value == 1 &&
                client.state().next_rel_seq.value == 2,
            "second transfer did not reuse the persistent session cursor");

    client.close();
    running.require_success();
    require(published.size() == 2 && published[0] == first &&
                published[1] == second,
            "loopback endpoint did not publish exact cold/warm inputs");
    const SessionState snapshot = server.route_snapshot();
    require(snapshot.route_present && snapshot.next_rel_seq.value == 2 &&
                snapshot.last_commit == second_commit,
            "server did not retain the final route/commit witness");
}

void test_invalid_candidate_does_not_install() {
    boost::asio::io_context server_context;
    boost::asio::io_context client_context;
    const CStoreGuid c_guid = Id128::from_u64(300);
    const SessionLimits session_limits{4096, 1U << 20};
    ZstdLoopbackConfig config;
    config.f_store_guid = Id128::from_u64(301);
    config.session_limits = session_limits;
    config.zstd_limits = {1U << 20, 1U << 20};

    ZstdLoopbackServer server(
        server_context, config, c_guid,
        [](const TxBegin&, const TxCommit&, std::vector<uint8_t>) {});
    ServerThread running(server);

    tcp::socket socket(client_context);
    socket.connect(tcp::endpoint(boost::asio::ip::address_v4::loopback(),
                                 server.port()));
    write_fragmented(socket, Message{hello(c_guid, session_limits)});
    require(std::holds_alternative<SessionState>(read_message(socket)),
            "valid candidate did not receive SESSION_STATE");
    write_fragmented(socket, Message{BodyMessage{{1, 2, 3}}});
    const Message response = read_message(socket);
    require(std::holds_alternative<ErrorMessage>(response),
            "invalid first mutation did not produce terminal ERROR");
    socket.close();
    running.require_failure<std::invalid_argument>();
    require(server.candidate_gate().current_session_serial() == 0 &&
                server.candidate_gate().staged_candidates() == 0 &&
                !server.route_snapshot().route_present,
            "invalid candidate changed the live relationship");
}

void test_mid_body_disconnect_replays_whole_transaction() {
    boost::asio::io_context server_context;
    boost::asio::io_context raw_client_context;
    const CStoreGuid c_guid = Id128::from_u64(400);
    const SessionLimits session_limits{4096, 1U << 20};
    ZstdLoopbackConfig config;
    config.f_store_guid = Id128::from_u64(401);
    config.session_limits = session_limits;
    config.zstd_limits = {1U << 20, 1U << 20};

    std::vector<std::vector<uint8_t>> published;
    ZstdLoopbackServer server(
        server_context, config, c_guid,
        [&](const TxBegin&, const TxCommit&, std::vector<uint8_t> exact) {
            published.push_back(std::move(exact));
        });

    const std::vector<uint8_t> input = input_bytes(192 * 1024, 99);
    ZstdTuEnvelope transaction;
    {
        ServerThread first_server(server);
        tcp::socket socket(raw_client_context);
        socket.connect(tcp::endpoint(boost::asio::ip::address_v4::loopback(),
                                     server.port()));
        write_fragmented(socket, Message{hello(c_guid, session_limits)});
        SessionState state = std::get<SessionState>(read_message(socket));
        const HistoryNonce nonce{77};
        write_fragmented(socket, Message{HistoryReset{
                                     nonce,
                                     initial_route_digest(c_guid, nonce)}});
        state = std::get<SessionState>(read_message(socket));
        transaction = envelope_for(state, TuSeq{10}, input);
        write_fragmented(socket, Message{transaction.begin});
        const size_t partial = transaction.body.size() / 2;
        write_fragmented(socket, Message{BodyMessage{{
                                     transaction.body.begin(),
                                     transaction.body.begin() + partial}}});
        socket.close();
        first_server.require_failure<std::exception>();
    }

    require(published.empty(),
            "mid-BODY disconnect published an incomplete exact input");
    const SessionState after_disconnect = server.route_snapshot();
    require(after_disconnect.route_present &&
                after_disconnect.next_rel_seq.value == 0 &&
                !after_disconnect.last_commit,
            "mid-BODY disconnect advanced or erased the route");

    boost::asio::io_context retry_context;
    ServerThread retry_server(server);
    ZstdLoopbackClient retry(
        retry_context,
        tcp::endpoint(boost::asio::ip::address_v4::loopback(), server.port()),
        hello(c_guid, session_limits), 3);
    require(retry.state().history_nonce == transaction.begin.history_nonce &&
                retry.state().next_rel_seq == transaction.begin.rel_seq &&
                retry.state().state_digest == transaction.begin.pre_state_digest,
            "replacement session did not report the replay cursor");
    const TxCommit committed = retry.transfer(transaction, 53);
    retry.close();
    retry_server.require_success();
    require(published.size() == 1 && published.front() == input &&
                committed.rel_seq.value == 0,
            "whole-transaction replay did not publish exactly once");
}

void test_publish_failure_does_not_advance_route() {
    boost::asio::io_context server_context;
    const CStoreGuid c_guid = Id128::from_u64(500);
    const SessionLimits session_limits{4096, 1U << 20};
    ZstdLoopbackConfig config;
    config.f_store_guid = Id128::from_u64(501);
    config.session_limits = session_limits;
    config.zstd_limits = {1U << 20, 1U << 20};

    size_t publish_attempts = 0;
    std::vector<std::vector<uint8_t>> published;
    ZstdLoopbackServer server(
        server_context, config, c_guid,
        [&](const TxBegin&, const TxCommit&, std::vector<uint8_t> exact) {
            ++publish_attempts;
            if (publish_attempts == 1) throw std::bad_alloc();
            published.push_back(std::move(exact));
        });

    const std::vector<uint8_t> input = input_bytes(96 * 1024, 123);
    ZstdTuEnvelope transaction;
    {
        boost::asio::io_context first_context;
        ServerThread first_server(server);
        ZstdLoopbackClient first(
            first_context,
            tcp::endpoint(boost::asio::ip::address_v4::loopback(), server.port()),
            hello(c_guid, session_limits), 2);
        first.establish_initial_route(HistoryNonce{91});
        transaction = envelope_for(first.state(), TuSeq{12}, input);
        require_throws<std::runtime_error>(
            [&] { (void)first.transfer(transaction, 41); },
            "failed InputRecord publication was exposed as TX_COMMIT");
        first.close();
        first_server.require_failure<std::bad_alloc>();
    }

    const SessionState failed = server.route_snapshot();
    require(publish_attempts == 1 && published.empty() &&
                failed.route_present && failed.next_rel_seq.value == 0 &&
                !failed.last_commit,
            "failed publication advanced the route or retained input");

    boost::asio::io_context retry_context;
    ServerThread retry_server(server);
    ZstdLoopbackClient retry(
        retry_context,
        tcp::endpoint(boost::asio::ip::address_v4::loopback(), server.port()),
        hello(c_guid, session_limits), 5);
    require(retry.state().history_nonce == transaction.begin.history_nonce &&
                retry.state().next_rel_seq == transaction.begin.rel_seq &&
                retry.state().state_digest == transaction.begin.pre_state_digest,
            "publication failure did not retain the exact retry cursor");
    const TxCommit committed = retry.transfer(transaction, 47);
    retry.close();
    retry_server.require_success();
    require(publish_attempts == 2 && published.size() == 1 &&
                published.front() == input && committed.rel_seq.value == 0,
            "retry after publication failure did not commit exactly once");
}

}  // namespace


// The three tests below bind the endpoint's warm-route validation clauses so
// that neutralizing any of them (cursor law, negotiated-mask law on either
// side) is caught here rather than surviving as a reachable-but-untested
// branch. The publish-time stale-serial backstop in publish_commit is not
// externally expressible through the one-connection-at-a-time serve loop and
// remains defense-in-depth behind the gate tests above.

void test_warm_route_rejects_cursor_divergent_begin() {
    boost::asio::io_context server_context;
    boost::asio::io_context client_context;
    const CStoreGuid c_guid = Id128::from_u64(600);
    const SessionLimits session_limits{4096, 1U << 20};
    ZstdLoopbackConfig config;
    config.f_store_guid = Id128::from_u64(601);
    config.session_limits = session_limits;
    config.zstd_limits = {1U << 20, 1U << 20};

    ZstdLoopbackServer server(
        server_context, config, c_guid,
        [](const TxBegin&, const TxCommit&, std::vector<uint8_t>) {});
    ServerThread running(server);

    tcp::socket socket(client_context);
    socket.connect(tcp::endpoint(boost::asio::ip::address_v4::loopback(),
                                 server.port()));
    write_fragmented(socket, Message{hello(c_guid, session_limits)});
    const Message cold = read_message(socket);
    const auto* cold_state = std::get_if<SessionState>(&cold);
    require(cold_state != nullptr && !cold_state->route_present,
            "cursor-divergence probe did not receive the cold SESSION_STATE");

    const HistoryNonce nonce{77};
    write_fragmented(
        socket,
        Message{HistoryReset{nonce, initial_route_digest(c_guid, nonce)}});
    const Message established = read_message(socket);
    const auto* route = std::get_if<SessionState>(&established);
    require(route != nullptr && route->route_present &&
                route->next_rel_seq.value == 0,
            "cursor-divergence probe did not establish the initial route");

    SessionState divergent = *route;
    divergent.next_rel_seq.value += 1;  // one ahead of the endpoint cursor
    const std::vector<uint8_t> payload = input_bytes(2048, 23);
    const ZstdTuEnvelope crafted = envelope_for(divergent, TuSeq{1}, payload);
    write_fragmented(socket, Message{crafted.begin});
    // No further traffic: a server that wrongly accepts this TX_BEGIN sees
    // EOF with an active transaction and fails through a DIFFERENT detail
    // text, so the exact-text assertion below is the discriminator
    // (std::invalid_argument IS-A std::logic_error, classes cannot separate
    // the two rejection layers).
    socket.shutdown(tcp::socket::shutdown_send);
    const Message rejection = read_message(socket);
    const auto* rejection_error = std::get_if<ErrorMessage>(&rejection);
    require(rejection_error != nullptr &&
                rejection_error->detail ==
                    "TX_BEGIN does not match the endpoint route cursor",
            "cursor-divergent TX_BEGIN was not rejected by the route cursor law");
    socket.close();
    running.require_failure<std::logic_error>();

    const SessionState snapshot = server.route_snapshot();
    require(snapshot.route_present && snapshot.history_nonce == nonce &&
                snapshot.next_rel_seq.value == 0 &&
                snapshot.state_digest == initial_route_digest(c_guid, nonce),
            "cursor-divergent TX_BEGIN moved the endpoint route");
}

void test_unnegotiated_profile_begin_rejected() {
    boost::asio::io_context server_context;
    boost::asio::io_context client_context;
    const CStoreGuid c_guid = Id128::from_u64(700);
    const SessionLimits session_limits{4096, 1U << 20};
    ZstdLoopbackConfig config;
    config.f_store_guid = Id128::from_u64(701);
    config.session_limits = session_limits;
    config.zstd_limits = {1U << 20, 1U << 20};

    ZstdLoopbackServer server(
        server_context, config, c_guid,
        [](const TxBegin&, const TxCommit&, std::vector<uint8_t>) {});
    ServerThread running(server);

    tcp::socket socket(client_context);
    socket.connect(tcp::endpoint(boost::asio::ip::address_v4::loopback(),
                                 server.port()));
    write_fragmented(socket, Message{hello(c_guid, session_limits)});
    require(std::holds_alternative<SessionState>(read_message(socket)),
            "mask probe did not receive the cold SESSION_STATE");
    const HistoryNonce nonce{88};
    write_fragmented(
        socket,
        Message{HistoryReset{nonce, initial_route_digest(c_guid, nonce)}});
    const Message established = read_message(socket);
    const auto* route = std::get_if<SessionState>(&established);
    require(route != nullptr && route->route_present,
            "mask probe did not establish the initial route");

    const std::vector<uint8_t> payload = input_bytes(2048, 29);
    const ZstdTuEnvelope good = envelope_for(*route, TuSeq{1}, payload);
    TxBegin bad = good.begin;
    // Intrinsically valid (GRZ carries NotApplicable) but outside the
    // ZSTD_TU-only negotiated mask, with a correct route cursor: only the
    // negotiated-mask clause can reject it.
    bad.profile = ProfileId::GRZ;
    write_fragmented(socket, Message{bad});
    socket.shutdown(tcp::socket::shutdown_send);
    const Message rejection = read_message(socket);
    const auto* rejection_error = std::get_if<ErrorMessage>(&rejection);
    // Exact text: the endpoint's own negotiated-mask clause must reject this,
    // not the dialogue layer behind it ("TX_BEGIN selected an unnegotiated
    // profile") and not the disconnect backstop.
    require(rejection_error != nullptr &&
                rejection_error->detail == "TX_BEGIN profile was not negotiated",
            "unnegotiated-profile TX_BEGIN was not rejected by the endpoint mask law");
    socket.close();
    running.require_failure<std::invalid_argument>();

    const SessionState snapshot = server.route_snapshot();
    require(snapshot.route_present && snapshot.history_nonce == nonce &&
                snapshot.next_rel_seq.value == 0,
            "unnegotiated-profile TX_BEGIN moved the endpoint route");
}

void test_client_rejects_unnegotiated_envelope() {
    boost::asio::io_context server_context;
    boost::asio::io_context client_context;
    const CStoreGuid c_guid = Id128::from_u64(800);
    const SessionLimits session_limits{4096, 1U << 20};
    ZstdLoopbackConfig config;
    config.f_store_guid = Id128::from_u64(801);
    config.session_limits = session_limits;
    config.zstd_limits = {1U << 20, 1U << 20};

    std::vector<std::vector<uint8_t>> published;
    ZstdLoopbackServer server(
        server_context, config, c_guid,
        [&](const TxBegin&, const TxCommit&, std::vector<uint8_t> exact) {
            published.push_back(std::move(exact));
        });
    ServerThread running(server);

    ZstdLoopbackClient client(
        client_context,
        tcp::endpoint(boost::asio::ip::address_v4::loopback(), server.port()),
        hello(c_guid, session_limits), 64);
    client.establish_initial_route(HistoryNonce{99});

    const std::vector<uint8_t> payload = input_bytes(4096, 41);
    ZstdTuEnvelope crafted = envelope_for(client.state(), TuSeq{1}, payload);
    crafted.begin.profile = ProfileId::GRZ;
    bool locally_rejected = false;
    try {
        (void)client.transfer(crafted, 33);
    } catch (const std::logic_error& error) {
        locally_rejected =
            std::string_view(error.what()) ==
            "client envelope does not match SESSION_STATE cursor";
    }
    require(locally_rejected,
            "client did not reject the unnegotiated envelope through its own "
            "session-cursor law");

    const ZstdTuEnvelope good = envelope_for(client.state(), TuSeq{1}, payload);
    const TxCommit commit = client.transfer(good, 33);
    require(commit.rel_seq.value == 0 && client.state().next_rel_seq.value == 1,
            "client cursor was disturbed by the rejected local envelope");
    client.close();
    running.require_success();
    require(published.size() == 1 && published[0] == payload,
            "server observed anything besides the single good transfer");
}

int main() {
    test_candidate_staging_and_stale_close_fence();
    test_persistent_cold_then_warm_transfer();
    test_invalid_candidate_does_not_install();
    test_mid_body_disconnect_replays_whole_transaction();
    test_publish_failure_does_not_advance_route();
    test_warm_route_rejects_cursor_divergent_begin();
    test_unnegotiated_profile_begin_rejected();
    test_client_rejects_unnegotiated_envelope();
    std::cout << "p50_loopback_test: all staged loopback endpoint gates passed\n";
    return 0;
}
