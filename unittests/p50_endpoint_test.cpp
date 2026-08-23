#include "cache/p50_endpoint.h"

#include <zstd.h>

#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using namespace icecc::p50;

[[noreturn]] void fail(std::string_view detail) {
    std::cerr << "p50_endpoint_test: " << detail << '\n';
    std::exit(1);
}

void require(bool value, std::string_view detail) {
    if (!value)
        fail(detail);
}

template <class Exception, class Function>
void require_throws(Function&& function, std::string_view detail) {
    try {
        std::forward<Function>(function)();
    } catch (const Exception&) {
        return;
    } catch (...) {
        fail(std::string(detail) + " (wrong exception type)");
    }
    fail(std::string(detail) + " (no exception)");
}

std::vector<uint8_t> bytes(std::string_view text) { return {text.begin(), text.end()}; }

struct TestClient {
    explicit TestClient(CStoreGuid c_store_guid, EndpointCaps caps = {},
                        HistoryNonce first_history_nonce = HistoryNonce{1},
                        CompletionLog* completions = nullptr, ActionTrace* actions = nullptr)
        : authority(std::make_shared<P50PreparationAuthority>(c_store_guid, caps.zstd)),
          endpoint(authority, caps, first_history_nonce, completions, actions) {}

    operator P50ClientEndpoint&() { return endpoint; }

    PreparedTuHandle prepare(PrepareRequestKey request, std::span<const uint8_t> input) {
        return authority->prepare(request, input);
    }

    [[nodiscard]] CStoreGuid c_store_guid() const { return endpoint.c_store_guid(); }
    [[nodiscard]] bool has_active_transaction() const {
        return endpoint.has_active_transaction();
    }
    [[nodiscard]] bool has_reconciliation_work() const {
        return endpoint.has_reconciliation_work();
    }

    std::shared_ptr<P50PreparationAuthority> authority;
    P50ClientEndpoint endpoint;
};

PreparedTuHandle admit(TestClient& client, std::span<const uint8_t> input) {
    static uint64_t next_request_token = 1;
    return client.prepare(PrepareRequestKey{1, next_request_token++}, input);
}

std::vector<uint8_t> pseudo_random_bytes(size_t count) {
    std::vector<uint8_t> result(count);
    uint32_t state = 0x6d2b79f5U;
    for (uint8_t& byte : result) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        byte = static_cast<uint8_t>(state);
    }
    return result;
}

std::vector<uint8_t> drain_input(InputCursor& cursor, size_t chunk_size = 113) {
    std::vector<uint8_t> result;
    std::vector<uint8_t> chunk(chunk_size);
    while (!cursor.eof()) {
        const size_t count = cursor.read(chunk);
        require(count != 0, "non-EOF InputRecord cursor made no progress");
        result.insert(result.end(), chunk.begin(), chunk.begin() + count);
    }
    return result;
}

std::optional<std::vector<uint8_t>> copy_input(P50ServerEndpoint& server,
                                                CStoreGuid c_store_guid) {
    const std::optional<InputRecordKey> key =
        server.last_committed_input(c_store_guid);
    if (!key)
        return std::nullopt;
    try {
        InputCursor cursor = server.attach_input(*key);
        return drain_input(cursor);
    } catch (const std::logic_error&) {
        return std::nullopt;
    }
}

std::vector<uint8_t> standalone_zstd_frame(std::span<const uint8_t> input) {
    std::vector<uint8_t> result(ZSTD_compressBound(input.size()));
    const size_t encoded =
        ZSTD_compress(result.data(), result.size(), input.data(), input.size(), 1);
    if (ZSTD_isError(encoded))
        fail(std::string("standalone zstd fixture failed: ") + ZSTD_getErrorName(encoded));
    result.resize(encoded);
    return result;
}

bool nonzero(Digest128 digest) {
    return std::any_of(digest.bytes.begin(), digest.bytes.end(),
                       [](uint8_t byte) { return byte != 0; });
}

struct PairResult {
    ClientRunResult client;
    ServerRunResult server;
};

struct CompetingPairResult {
    ClientRunResult first_client;
    ClientRunResult second_client;
    ServerRunResult first_server;
    ServerRunResult second_server;
    P50ServerOwnerUsage usage;
};

PairResult run_pair(P50ClientEndpoint& client, P50ServerEndpoint& server,
                    PreparedTuHandle prepared = {}, EndpointIoControl client_control = {},
                    EndpointIoControl server_control = {}) {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    std::future<ServerRunResult> server_result =
        asio::co_spawn(context, server.accept_one(acceptor, server_control), asio::use_future);
    std::future<ClientRunResult> client_result = asio::co_spawn(
        context, client.run(acceptor.local_endpoint(), prepared, client_control),
        asio::use_future);
    context.run();
    return {client_result.get(), server_result.get()};
}

CompetingPairResult run_competing_pair(P50ServerEndpointConfig config,
                                        uint64_t identity_base,
                                        size_t first_raw_bytes,
                                        size_t second_raw_bytes) {
    P50ServerEndpoint server(Id128::from_u64(identity_base), {}, nullptr, nullptr,
                             std::move(config));
    TestClient first(Id128::from_u64(identity_base + 1));
    TestClient second(Id128::from_u64(identity_base + 2));
    const PreparedTuHandle first_prepared =
        admit(first, pseudo_random_bytes(first_raw_bytes));
    const PreparedTuHandle second_prepared =
        admit(second, pseudo_random_bytes(second_raw_bytes));
    EndpointIoControl first_control;
    first_control.max_write_fragment = 1;
    EndpointIoControl second_control;
    second_control.max_write_fragment = 1;

    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    std::future<ServerRunResult> first_server =
        asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
    std::future<ServerRunResult> second_server =
        asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
    std::future<ClientRunResult> first_client = asio::co_spawn(
        context,
        first.endpoint.run(acceptor.local_endpoint(), first_prepared, first_control),
        asio::use_future);
    std::future<ClientRunResult> second_client = asio::co_spawn(
        context,
        second.endpoint.run(acceptor.local_endpoint(), second_prepared, second_control),
        asio::use_future);
    context.run();
    return {first_client.get(), second_client.get(), first_server.get(),
            second_server.get(), server.owner_usage()};
}

void require_trace(const ActionTrace& trace, std::string_view context) {
    if (const auto error = check_action_trace(trace.records()))
        fail(std::string(context) + ": " + *error);
}

TxBegin make_begin(const ZstdTuEnvelope& prepared, HistoryNonce nonce, Digest128 pre_state,
                   RelSeq rel = RelSeq{0}) {
    TxBegin begin = prepared.begin;
    begin.history_nonce = nonce;
    begin.rel_seq = rel;
    begin.pre_state_digest = pre_state;
    begin.transaction_digest = compute_transaction_digest(
        begin, std::span<const uint8_t>{}, prepared.body);
    return begin;
}

asio::awaitable<void> raw_write(tcp::socket& socket, Message message) {
    const std::vector<uint8_t> frame = encode_frame(message);
    co_await asio::async_write(socket, asio::buffer(frame), asio::use_awaitable);
    co_return;
}

asio::awaitable<void> raw_write_bytes(tcp::socket& socket,
                                      std::span<const uint8_t> bytes_to_write) {
    co_await asio::async_write(socket, asio::buffer(bytes_to_write.data(), bytes_to_write.size()),
                               asio::use_awaitable);
    co_return;
}

asio::awaitable<Frame> raw_read(tcp::socket& socket, uint32_t max_payload) {
    std::array<uint8_t, 4> raw_header{};
    co_await asio::async_read(socket, asio::buffer(raw_header), asio::use_awaitable);
    const FrameHeader header = decode_frame_header(raw_header, max_payload);
    Frame result{header.type, std::vector<uint8_t>(header.payload_bytes)};
    if (!result.payload.empty())
        co_await asio::async_read(socket, asio::buffer(result.payload), asio::use_awaitable);
    co_return result;
}

template <class T> T raw_decode(const Frame& frame) {
    Message decoded = decode_payload(frame.type, frame.payload);
    T* result = std::get_if<T>(&decoded);
    if (!result)
        throw std::invalid_argument("raw peer received an unexpected message");
    return std::move(*result);
}

struct RawRoute {
    SessionHello hello;
    SessionState state;
};

asio::awaitable<RawRoute> raw_open(tcp::socket& socket, const tcp::endpoint& remote,
                                   CStoreGuid c_guid, SessionLimits limits = {});
asio::awaitable<SessionState> raw_reset(tcp::socket& socket, const RawRoute& open,
                                        HistoryNonce nonce);
asio::awaitable<void> raw_wait_for_close(tcp::socket& socket);

struct HelloRaceState {
    bool first_pending = false;
    bool second_finished = false;
};

asio::awaitable<void> raw_pending_then_finish(tcp::endpoint remote, CStoreGuid c_guid,
                                              std::span<const uint8_t> input,
                                              HelloRaceState& coordination) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    RawRoute open = co_await raw_open(socket, remote, c_guid);
    SessionState route = co_await raw_reset(socket, open, HistoryNonce{101});
    const ZstdTuEnvelope prepared = encode_zstd_tu(
        HistoryNonce{1}, RelSeq{0}, TuSeq{11}, Digest128{}, input);
    const TxBegin begin =
        make_begin(prepared, route.history_nonce, route.state_digest, route.next_rel_seq);
    co_await raw_write(socket, begin);
    coordination.first_pending = true;
    asio::steady_timer timer(executor);
    while (!coordination.second_finished) {
        timer.expires_after(std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
    }
    Message body_message = BodyMessage{prepared.body};
    co_await raw_write(socket, std::move(body_message));
    const TxCommit commit =
        raw_decode<TxCommit>(co_await raw_read(socket, route.limits.max_frame_payload));
    if (commit.transaction_digest != begin.transaction_digest ||
        commit.raw_digest != begin.raw_digest)
        throw std::logic_error("pending fixture received a different TX_COMMIT");
    boost::system::error_code ignored;
    socket.close(ignored);
    co_return;
}

asio::awaitable<void> raw_incompatible_hello(tcp::endpoint remote, CStoreGuid c_guid,
                                             HelloRaceState& coordination) {
    const auto executor = co_await asio::this_coro::executor;
    asio::steady_timer timer(executor);
    while (!coordination.first_pending) {
        timer.expires_after(std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
    }
    tcp::socket socket(executor);
    co_await socket.async_connect(remote, asio::use_awaitable);
    SessionHello hello;
    hello.c_store_guid = c_guid;
    hello.supported_profiles = profile_bit(ProfileId::P29);
    co_await raw_write(socket, hello);
    const Frame terminal = co_await raw_read(socket, hello.limits.max_frame_payload);
    coordination.second_finished = true;
    if (terminal.type != MessageType::ERROR)
        throw std::logic_error("incompatible HELLO did not receive terminal ERROR");
    (void)raw_decode<ErrorMessage>(terminal);
    std::array<uint8_t, 1> extra{};
    boost::system::error_code error;
    (void)co_await socket.async_read_some(
        asio::buffer(extra), asio::redirect_error(asio::use_awaitable, error));
    if (error != asio::error::eof && error != asio::error::connection_reset)
        throw std::logic_error("incompatible HELLO connection stayed open");
    co_return;
}

asio::awaitable<void> raw_invalid_first_mutation(tcp::endpoint remote,
                                                 CStoreGuid c_guid,
                                                 HelloRaceState& coordination) {
    const auto executor = co_await asio::this_coro::executor;
    asio::steady_timer timer(executor);
    while (!coordination.first_pending) {
        timer.expires_after(std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
    }
    tcp::socket socket(executor);
    const RawRoute open = co_await raw_open(socket, remote, c_guid);
    Message invalid = BodyMessage{bytes("not a first mutation")};
    co_await raw_write(socket, std::move(invalid));
    const Frame terminal =
        co_await raw_read(socket, open.state.limits.max_frame_payload);
    coordination.second_finished = true;
    if (terminal.type != MessageType::ERROR)
        throw std::logic_error(
            "invalid compatible candidate did not receive terminal ERROR");
    (void)raw_decode<ErrorMessage>(terminal);
    co_await raw_wait_for_close(socket);
}

struct CandidateRevisionRace {
    bool first_staged = false;
    bool route_changed = false;
};

asio::awaitable<void> raw_stale_candidate(tcp::endpoint remote,
                                          CStoreGuid c_guid,
                                          CandidateRevisionRace& coordination) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    const RawRoute open = co_await raw_open(socket, remote, c_guid);
    coordination.first_staged = true;
    asio::steady_timer timer(executor);
    while (!coordination.route_changed) {
        timer.expires_after(std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
    }

    const HistoryNonce replacement{open.state.history_nonce.value + 100};
    Message reset = HistoryReset{
        replacement, initial_route_digest(c_guid, replacement)};
    try {
        co_await raw_write(socket, std::move(reset));
        const Frame reply =
            co_await raw_read(socket, open.state.limits.max_frame_payload);
        if (reply.type == MessageType::ERROR) {
            (void)raw_decode<ErrorMessage>(reply);
            co_return;
        }
        throw std::logic_error("stale candidate received a non-ERROR reply");
    } catch (const boost::system::system_error& error) {
        if (error.code() != asio::error::eof &&
            error.code() != asio::error::connection_reset &&
            error.code() != asio::error::broken_pipe)
            throw;
    }
}

asio::awaitable<void> raw_commit_after_candidate_staged(
    tcp::endpoint remote, CStoreGuid c_guid, std::span<const uint8_t> input,
    CandidateRevisionRace& coordination) {
    const auto executor = co_await asio::this_coro::executor;
    asio::steady_timer timer(executor);
    while (!coordination.first_staged) {
        timer.expires_after(std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
    }
    tcp::socket socket(executor);
    const RawRoute open = co_await raw_open(socket, remote, c_guid);
    if (!open.state.route_present)
        throw std::logic_error("candidate-revision fixture has no established route");
    const ZstdTuEnvelope prepared = encode_zstd_tu(
        HistoryNonce{1}, RelSeq{0}, TuSeq{15}, Digest128{}, input);
    const TxBegin begin = make_begin(prepared, open.state.history_nonce,
                                     open.state.state_digest,
                                     open.state.next_rel_seq);
    co_await raw_write(socket, begin);
    Message body = BodyMessage{prepared.body};
    co_await raw_write(socket, std::move(body));
    const TxCommit commit = raw_decode<TxCommit>(
        co_await raw_read(socket, open.state.limits.max_frame_payload));
    if (commit.transaction_digest != begin.transaction_digest ||
        commit.raw_digest != begin.raw_digest)
        throw std::logic_error("candidate-revision fixture received another commit");
    boost::system::error_code ignored;
    socket.close(ignored);
    coordination.route_changed = true;
}

struct InterruptedRawTu {
    TxBegin begin;
    std::vector<uint8_t> body;
};

enum class TerminalBodyFailure {
    ZeroProgress,
    Excess,
    ComponentDigest,
    ZstdDecode,
    RawDigest,
};

asio::awaitable<InterruptedRawTu> raw_partial_body_disconnect(
    tcp::endpoint remote, CStoreGuid c_guid, std::span<const uint8_t> input) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    RawRoute open = co_await raw_open(socket, remote, c_guid);
    SessionState route = open.state;
    if (!route.route_present)
        route = co_await raw_reset(socket, open, HistoryNonce{121});
    ZstdTuEnvelope prepared = encode_zstd_tu(
        HistoryNonce{1}, RelSeq{0}, TuSeq{21}, Digest128{}, input);
    TxBegin begin = make_begin(prepared, route.history_nonce,
                               route.state_digest, route.next_rel_seq);
    co_await raw_write(socket, begin);
    if (prepared.body.size() < 2)
        throw std::logic_error("partial-BODY fixture encoded fewer than two bytes");
    BodyMessage partial;
    partial.bytes.assign(prepared.body.begin(),
                         prepared.body.begin() + prepared.body.size() / 2);
    Message partial_message = std::move(partial);
    co_await raw_write(socket, std::move(partial_message));
    boost::system::error_code ignored;
    socket.close(ignored);
    co_return InterruptedRawTu{std::move(begin), std::move(prepared.body)};
}

asio::awaitable<InterruptedRawTu> raw_terminal_body_failure(
    tcp::endpoint remote, CStoreGuid c_guid, std::span<const uint8_t> input,
    TerminalBodyFailure scenario, bool receive_terminal = true) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    RawRoute open = co_await raw_open(socket, remote, c_guid);
    SessionState route = open.state;
    if (!route.route_present)
        route = co_await raw_reset(socket, open, HistoryNonce{701});
    ZstdTuEnvelope prepared = encode_zstd_tu(
        HistoryNonce{1}, RelSeq{0}, TuSeq{81}, Digest128{}, input);
    TxBegin begin = make_begin(prepared, route.history_nonce,
                               route.state_digest, route.next_rel_seq);
    std::vector<uint8_t> sent_body = prepared.body;
    if (scenario == TerminalBodyFailure::ZstdDecode) {
        std::fill(sent_body.begin(), sent_body.end(), uint8_t{0});
        begin.body = describe_component(kZstdTuBodyEncoding, sent_body,
                                        begin.raw_bytes);
        begin.transaction_digest = compute_transaction_digest(
            begin, std::span<const uint8_t>{}, sent_body);
        prepared.body = sent_body;
    } else if (scenario == TerminalBodyFailure::RawDigest) {
        begin.raw_digest.bytes[0] ^= 0x80;
        begin.transaction_digest = compute_transaction_digest(
            begin, std::span<const uint8_t>{}, sent_body);
    }

    co_await raw_write(socket, begin);
    switch (scenario) {
    case TerminalBodyFailure::ZeroProgress:
        sent_body.clear();
        break;
    case TerminalBodyFailure::Excess:
        sent_body.push_back(0xff);
        break;
    case TerminalBodyFailure::ComponentDigest:
        if (sent_body.empty())
            throw std::logic_error("component-digest fixture encoded an empty BODY");
        sent_body.front() ^= 0x80;
        break;
    case TerminalBodyFailure::ZstdDecode:
    case TerminalBodyFailure::RawDigest:
        break;
    }
    Message body_message = BodyMessage{std::move(sent_body)};
    co_await raw_write(socket, std::move(body_message));
    if (!receive_terminal) {
        boost::system::error_code ignored;
        socket.close(ignored);
        co_return InterruptedRawTu{std::move(begin), std::move(prepared.body)};
    }
    const Frame terminal =
        co_await raw_read(socket, route.limits.max_frame_payload);
    if (terminal.type != MessageType::ERROR)
        throw std::logic_error("terminal BODY fixture did not receive ERROR");
    (void)raw_decode<ErrorMessage>(terminal);
    co_await raw_wait_for_close(socket);
    co_return InterruptedRawTu{std::move(begin), std::move(prepared.body)};
}

asio::awaitable<void> raw_different_begin_rejected(
    tcp::endpoint remote, CStoreGuid c_guid, const TxBegin& interrupted) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    const RawRoute open = co_await raw_open(socket, remote, c_guid);
    const std::vector<uint8_t> different_input =
        pseudo_random_bytes(4097);
    const ZstdTuEnvelope different = encode_zstd_tu(
        HistoryNonce{1}, RelSeq{0}, TuSeq{interrupted.tu_seq.value + 1},
        Digest128{}, different_input);
    const TxBegin begin = make_begin(different, open.state.history_nonce,
                                     open.state.state_digest,
                                     open.state.next_rel_seq);
    if (begin == interrupted)
        throw std::logic_error("different-begin fixture reproduced the same identity");
    co_await raw_write(socket, begin);
    const Frame terminal =
        co_await raw_read(socket, open.state.limits.max_frame_payload);
    if (terminal.type != MessageType::ERROR)
        throw std::logic_error("different begin did not receive terminal ERROR");
    (void)raw_decode<ErrorMessage>(terminal);
    co_await raw_wait_for_close(socket);
}

asio::awaitable<void> raw_exact_whole_tu_replay(
    tcp::endpoint remote, CStoreGuid c_guid, const InterruptedRawTu& interrupted) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    const RawRoute open = co_await raw_open(socket, remote, c_guid);
    if (open.state.history_nonce != interrupted.begin.history_nonce ||
        open.state.next_rel_seq != interrupted.begin.rel_seq ||
        open.state.state_digest != interrupted.begin.pre_state_digest)
        throw std::logic_error("interrupted route cursor changed before exact replay");
    co_await raw_write(socket, interrupted.begin);
    Message body = BodyMessage{interrupted.body};
    co_await raw_write(socket, std::move(body));
    const TxCommit commit = raw_decode<TxCommit>(
        co_await raw_read(socket, open.state.limits.max_frame_payload));
    if (commit.transaction_digest != interrupted.begin.transaction_digest ||
        commit.raw_digest != interrupted.begin.raw_digest)
        throw std::logic_error("exact replay received a different commit identity");
    boost::system::error_code ignored;
    socket.close(ignored);
}

asio::awaitable<void> raw_exact_whole_tu_rejected(
    tcp::endpoint remote, CStoreGuid c_guid,
    const InterruptedRawTu& interrupted) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    const RawRoute open = co_await raw_open(socket, remote, c_guid);
    if (open.state.history_nonce != interrupted.begin.history_nonce ||
        open.state.next_rel_seq != interrupted.begin.rel_seq ||
        open.state.state_digest != interrupted.begin.pre_state_digest)
        throw std::logic_error(
            "deterministic-failure route cursor changed before exact replay");
    co_await raw_write(socket, interrupted.begin);
    Message body_message = BodyMessage{interrupted.body};
    co_await raw_write(socket, std::move(body_message));
    const Frame terminal =
        co_await raw_read(socket, open.state.limits.max_frame_payload);
    if (terminal.type != MessageType::ERROR)
        throw std::logic_error(
            "deterministic-failure exact replay did not receive ERROR");
    (void)raw_decode<ErrorMessage>(terminal);
    co_await raw_wait_for_close(socket);
}

asio::awaitable<RawRoute> raw_open(tcp::socket& socket, const tcp::endpoint& remote,
                                   CStoreGuid c_guid, SessionLimits limits) {
    co_await socket.async_connect(remote, asio::use_awaitable);
    SessionHello hello;
    hello.c_store_guid = c_guid;
    hello.supported_profiles = profile_bit(ProfileId::ZSTD_TU);
    hello.limits = limits;
    co_await raw_write(socket, hello);
    SessionState state =
        raw_decode<SessionState>(co_await raw_read(socket, limits.max_frame_payload));
    validate_session_state(hello, state);
    co_return RawRoute{hello, state};
}

asio::awaitable<SessionState> raw_reset(tcp::socket& socket, const RawRoute& open,
                                        HistoryNonce nonce) {
    const HistoryReset reset{nonce, initial_route_digest(open.hello.c_store_guid, nonce)};
    co_await raw_write(socket, reset);
    SessionState state =
        raw_decode<SessionState>(co_await raw_read(socket, open.state.limits.max_frame_payload));
    if (!state.route_present || state.history_nonce != nonce || state.next_rel_seq.value != 0 ||
        state.state_digest != reset.initial_state_digest)
        throw std::logic_error("raw HISTORY_RESET acknowledgement differs");
    co_return state;
}

asio::awaitable<void> raw_reset_only(tcp::endpoint remote, CStoreGuid c_guid, HistoryNonce nonce,
                                     SessionLimits limits = {}) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    const RawRoute open = co_await raw_open(socket, remote, c_guid, limits);
    (void)co_await raw_reset(socket, open, nonce);
    boost::system::error_code ignored;
    socket.close(ignored);
    co_return;
}

asio::awaitable<bool> raw_reset_rejected(tcp::endpoint remote, CStoreGuid c_guid,
                                         HistoryNonce nonce, SessionLimits limits = {}) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    const RawRoute open = co_await raw_open(socket, remote, c_guid, limits);
    co_await raw_write(socket,
                       HistoryReset{nonce, initial_route_digest(c_guid, nonce)});
    const Frame reply = co_await raw_read(socket, open.state.limits.max_frame_payload);
    co_return reply.type == MessageType::ERROR;
}

asio::awaitable<void> raw_wait_for_close(tcp::socket& socket) {
    std::array<uint8_t, 1> extra{};
    boost::system::error_code error;
    (void)co_await socket.async_read_some(
        asio::buffer(extra), asio::redirect_error(asio::use_awaitable, error));
    if (error != asio::error::eof && error != asio::error::connection_reset)
        throw std::logic_error("client did not close after its terminal result");
}

enum class ResetAckMutation {
    SelectedProtocol,
    ProfileMask,
    FrameLimit,
    FillLimit,
};

struct ScriptedSessionState {
    FStoreGuid f_guid{};
    bool route_present = false;
    HistoryNonce history_nonce{};
    RelSeq next_rel_seq{};
    Digest128 state_digest{};
};

asio::awaitable<void> raw_bad_reset_ack_peer(tcp::acceptor& acceptor,
                                             ScriptedSessionState scripted,
                                             ResetAckMutation mutation) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    co_await acceptor.async_accept(socket, asio::use_awaitable);
    const SessionHello hello = raw_decode<SessionHello>(
        co_await raw_read(socket, kInitialMaxFramePayload));

    SessionState initial;
    initial.selected_protocol = kProtocolVersion;
    initial.negotiated_profiles = profile_bit(ProfileId::ZSTD_TU);
    initial.limits = hello.limits;
    initial.f_store_guid = scripted.f_guid;
    initial.namespace_present = scripted.route_present;
    initial.route_present = scripted.route_present;
    initial.history_nonce = scripted.history_nonce;
    initial.next_rel_seq = scripted.next_rel_seq;
    initial.state_digest = scripted.state_digest;
    co_await raw_write(socket, initial);

    const HistoryReset reset = raw_decode<HistoryReset>(
        co_await raw_read(socket, initial.limits.max_frame_payload));
    SessionState ack = initial;
    ack.namespace_present = true;
    ack.route_present = true;
    ack.history_nonce = reset.history_nonce;
    ack.next_rel_seq = RelSeq{0};
    ack.state_digest = reset.initial_state_digest;
    if (mutation == ResetAckMutation::ProfileMask) {
        ack.negotiated_profiles = profile_bit(ProfileId::P29);
        co_await raw_write(socket, ack);
    } else if (mutation == ResetAckMutation::FrameLimit) {
        require(ack.limits.max_frame_payload > kMandatoryControlFramePayload,
                "reset-ack frame-limit fixture lacks room to decrease");
        --ack.limits.max_frame_payload;
        co_await raw_write(socket, ack);
    } else if (mutation == ResetAckMutation::FillLimit) {
        require(ack.limits.max_fill_record_bytes > 32,
                "reset-ack FILL-limit fixture lacks room to decrease");
        --ack.limits.max_fill_record_bytes;
        co_await raw_write(socket, ack);
    } else {
        std::vector<uint8_t> frame = encode_frame(ack);
        frame[4] = 0;
        frame[5] = static_cast<uint8_t>(kProtocolVersion + 1);
        co_await raw_write_bytes(socket, frame);
    }
    co_await raw_wait_for_close(socket);
}

asio::awaitable<void> raw_bounded_error_peer(tcp::acceptor& acceptor, uint32_t payload_cap) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    co_await acceptor.async_accept(socket, asio::use_awaitable);
    const SessionHello hello = raw_decode<SessionHello>(
        co_await raw_read(socket, kInitialMaxFramePayload));
    if (hello.limits.max_frame_payload != payload_cap || payload_cap < 6)
        throw std::logic_error("bounded terminal fixture negotiated an unexpected cap");
    Message terminal = ErrorMessage{91, std::string(payload_cap - 6, 'e')};
    co_await raw_write(socket, std::move(terminal));
    co_await raw_wait_for_close(socket);
}

asio::awaitable<void> raw_unknown_frame_peer(tcp::acceptor& acceptor) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    co_await acceptor.async_accept(socket, asio::use_awaitable);
    (void)raw_decode<SessionHello>(
        co_await raw_read(socket, kInitialMaxFramePayload));
    const std::array<uint8_t, 4> unknown_header{0xff, 0, 0, 0};
    co_await raw_write_bytes(socket, unknown_header);
    co_await raw_wait_for_close(socket);
}

asio::awaitable<void> raw_route_mismatch_peer(tcp::acceptor& acceptor,
                                              ScriptedSessionState scripted,
                                              bool expect_error = true) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    co_await acceptor.async_accept(socket, asio::use_awaitable);
    const SessionHello hello = raw_decode<SessionHello>(
        co_await raw_read(socket, kInitialMaxFramePayload));

    SessionState state;
    state.selected_protocol = kProtocolVersion;
    state.negotiated_profiles = profile_bit(ProfileId::ZSTD_TU);
    state.limits = hello.limits;
    state.f_store_guid = scripted.f_guid;
    state.namespace_present = true;
    state.route_present = true;
    state.history_nonce = scripted.history_nonce;
    state.next_rel_seq = scripted.next_rel_seq;
    state.state_digest = scripted.state_digest;
    co_await raw_write(socket, state);
    if (expect_error) {
        const ErrorMessage terminal = raw_decode<ErrorMessage>(
            co_await raw_read(socket, state.limits.max_frame_payload));
        if (terminal.code == 0)
            throw std::logic_error("route mismatch received a zero-code ERROR");
    }
    co_await raw_wait_for_close(socket);
}

asio::awaitable<void> raw_zero_f_store_state_peer(tcp::acceptor& acceptor) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    co_await acceptor.async_accept(socket, asio::use_awaitable);
    const SessionHello hello = raw_decode<SessionHello>(
        co_await raw_read(socket, kInitialMaxFramePayload));

    SessionState state;
    state.selected_protocol = kProtocolVersion;
    state.negotiated_profiles = profile_bit(ProfileId::ZSTD_TU);
    state.limits = hello.limits;
    state.f_store_guid = Id128::from_u64(6000);
    std::vector<uint8_t> frame = encode_frame(Message{state});
    constexpr size_t frame_header_bytes = 4;
    constexpr size_t f_guid_payload_offset = 2 + 4 + 4 + 8;
    std::fill_n(frame.begin() + frame_header_bytes + f_guid_payload_offset,
                16, uint8_t{0});
    co_await raw_write_bytes(socket, frame);
    co_await raw_wait_for_close(socket);
}

asio::awaitable<void> raw_zero_c_store_hello(tcp::endpoint remote) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    co_await socket.async_connect(remote, asio::use_awaitable);
    SessionHello hello;
    hello.c_store_guid = Id128::from_u64(6001);
    hello.supported_profiles = profile_bit(ProfileId::ZSTD_TU);
    std::vector<uint8_t> frame = encode_frame(Message{hello});
    constexpr size_t frame_header_bytes = 4;
    constexpr size_t c_guid_payload_offset = 2 + 2;
    std::fill_n(frame.begin() + frame_header_bytes + c_guid_payload_offset,
                16, uint8_t{0});
    co_await raw_write_bytes(socket, frame);
    const ErrorMessage terminal = raw_decode<ErrorMessage>(
        co_await raw_read(socket, kInitialMaxFramePayload));
    if (terminal.code == 0)
        throw std::logic_error("zero C_STORE_GUID received a zero-code ERROR");
    co_await raw_wait_for_close(socket);
}

asio::awaitable<void> raw_zero_error_code_peer(tcp::acceptor& acceptor) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    co_await acceptor.async_accept(socket, asio::use_awaitable);
    (void)raw_decode<SessionHello>(
        co_await raw_read(socket, kInitialMaxFramePayload));

    std::vector<uint8_t> frame =
        encode_frame(Message{ErrorMessage{1, "reserved-code fixture"}});
    frame[4] = 0;
    frame[5] = 0;
    co_await raw_write_bytes(socket, frame);
    co_await raw_wait_for_close(socket);
}

asio::awaitable<void> raw_zero_history_reset(tcp::endpoint remote,
                                              CStoreGuid c_guid) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    const RawRoute open = co_await raw_open(socket, remote, c_guid);
    const HistoryReset valid{
        HistoryNonce{1}, initial_route_digest(c_guid, HistoryNonce{1})};
    std::vector<uint8_t> frame = encode_frame(Message{valid});
    std::fill_n(frame.begin() + 4, sizeof(uint64_t), uint8_t{0});
    co_await raw_write_bytes(socket, frame);
    const ErrorMessage terminal = raw_decode<ErrorMessage>(
        co_await raw_read(socket, open.state.limits.max_frame_payload));
    if (terminal.code == 0)
        throw std::logic_error("zero HISTORY_NONCE received a zero-code ERROR");
    co_await raw_wait_for_close(socket);
}

template <class Peer>
ClientRunResult run_client_with_raw_peer(P50ClientEndpoint& client,
                                         PreparedTuHandle prepared, Peer peer,
                                         EndpointIoControl control = {}) {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    std::future<void> peer_result =
        asio::co_spawn(context, peer(acceptor), asio::use_future);
    std::future<ClientRunResult> client_result = asio::co_spawn(
        context, client.run(acceptor.local_endpoint(), prepared, control),
        asio::use_future);
    context.run();
    peer_result.get();
    return client_result.get();
}

enum class RawCase {
    ShortBody,
    ExcessBody,
    EmptyBodyProgress,
    DescriptorCap,
    DecodedCap,
    FrameCap,
    ComponentDigest,
    TransactionDigest,
    RawDigest,
    WrongProfile,
    WrongRoot,
    WrongEncoding,
    FrameContentSize,
    TrailingByte,
    AppendedEmptyFrame,
    AppendedNonemptyFrame,
};

struct RawCaseResult {
    bool received_error = false;
    bool closed_after_error = false;
    size_t error_payload_bytes = 0;
};

asio::awaitable<RawCaseResult> run_raw_case(tcp::endpoint remote, CStoreGuid c_guid,
                                            EndpointCaps server_caps, RawCase scenario) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    RawRoute open = co_await raw_open(socket, remote, c_guid, server_caps.wire);
    SessionState route = open.state;
    if (!route.route_present)
        route = co_await raw_reset(socket, open, HistoryNonce{71});

    if (scenario == RawCase::FrameCap) {
        const auto header =
            encode_frame_header(MessageType::TX_BEGIN, server_caps.wire.max_frame_payload + 1);
        co_await raw_write_bytes(socket, header);
    } else {
        const std::vector<uint8_t> raw = pseudo_random_bytes(512);
        const ZstdTuEnvelope prepared = encode_zstd_tu(
            HistoryNonce{1}, RelSeq{0}, TuSeq{9}, Digest128{}, raw);
        TxBegin begin =
            make_begin(prepared, route.history_nonce, route.state_digest, route.next_rel_seq);
        std::vector<uint8_t> body = prepared.body;
        std::vector<uint8_t> raw_begin_frame;
        if (scenario == RawCase::DescriptorCap) {
            body.assign(static_cast<size_t>(server_caps.zstd.max_encoded_body_bytes + 1),
                        uint8_t{0x5a});
            begin.body = describe_component(kZstdTuBodyEncoding, body, 1);
            begin.raw_bytes = 1;
            const std::array<uint8_t, 1> one_raw{0};
            begin.raw_digest = icecc::digest128(one_raw);
            begin.transaction_digest = compute_transaction_digest(
                begin, std::span<const uint8_t>{}, body);
        } else if (scenario == RawCase::DecodedCap) {
            begin.raw_bytes = server_caps.zstd.max_raw_bytes + 1;
            begin.body.decoded_bytes = begin.raw_bytes;
            begin.transaction_digest = compute_transaction_digest(
                begin, std::span<const uint8_t>{}, body);
        } else if (scenario == RawCase::WrongProfile) {
            raw_begin_frame = encode_frame(begin);
            raw_begin_frame[4 + 24] = 0;
            raw_begin_frame[4 + 25] = static_cast<uint8_t>(ProfileId::P29);
            raw_begin_frame[4 + 26] = 0;
            raw_begin_frame[4 + 27] = static_cast<uint8_t>(P29RootMode::HistoryIndependent);
        } else if (scenario == RawCase::WrongRoot) {
            raw_begin_frame = encode_frame(begin);
            raw_begin_frame[4 + 26] = 0;
            raw_begin_frame[4 + 27] = static_cast<uint8_t>(P29RootMode::HistoryIndependent);
        } else if (scenario == RawCase::WrongEncoding) {
            begin.body.encoding = 77;
            begin.transaction_digest = compute_transaction_digest(
                begin, std::span<const uint8_t>{}, body);
        } else if (scenario == RawCase::ComponentDigest) {
            begin.body.digest.bytes[0] ^= 0x80;
        } else if (scenario == RawCase::TransactionDigest) {
            begin.transaction_digest.bytes[0] ^= 0x80;
        } else if (scenario == RawCase::RawDigest) {
            begin.raw_digest.bytes[0] ^= 0x80;
            begin.transaction_digest = compute_transaction_digest(
                begin, std::span<const uint8_t>{}, body);
        } else if (scenario == RawCase::FrameContentSize) {
            ++begin.raw_bytes;
            begin.body.decoded_bytes = begin.raw_bytes;
            begin.transaction_digest = compute_transaction_digest(
                begin, std::span<const uint8_t>{}, body);
        } else if (scenario == RawCase::TrailingByte) {
            body.push_back(0xa5);
            begin.body =
                describe_component(kZstdTuBodyEncoding, body, begin.raw_bytes);
            begin.transaction_digest = compute_transaction_digest(
                begin, std::span<const uint8_t>{}, body);
        } else if (scenario == RawCase::AppendedEmptyFrame) {
            const std::vector<uint8_t> appended = standalone_zstd_frame({});
            body.insert(body.end(), appended.begin(), appended.end());
            begin.body =
                describe_component(kZstdTuBodyEncoding, body, begin.raw_bytes);
            begin.transaction_digest = compute_transaction_digest(
                begin, std::span<const uint8_t>{}, body);
        } else if (scenario == RawCase::AppendedNonemptyFrame) {
            const std::vector<uint8_t> appended =
                standalone_zstd_frame(bytes("second frame"));
            body.insert(body.end(), appended.begin(), appended.end());
            begin.body =
                describe_component(kZstdTuBodyEncoding, body, begin.raw_bytes);
            begin.transaction_digest = compute_transaction_digest(
                begin, std::span<const uint8_t>{}, body);
        }
        if (raw_begin_frame.empty())
            co_await raw_write(socket, begin);
        else
            co_await raw_write_bytes(socket, raw_begin_frame);
        if (scenario == RawCase::DescriptorCap || scenario == RawCase::DecodedCap ||
            scenario == RawCase::WrongProfile || scenario == RawCase::WrongRoot ||
            scenario == RawCase::WrongEncoding) {
            // The descriptor itself is rejected before any component allocation.
        } else {
            if (scenario == RawCase::ShortBody) {
                require(body.size() > 1, "short-body fixture is too small");
                body.pop_back();
                BodyMessage short_body{body};
                co_await raw_write(socket, short_body);
                boost::system::error_code ignored;
                socket.close(ignored);
                co_return RawCaseResult{};
            }
            if (scenario == RawCase::ExcessBody)
                body.push_back(0xff);
            if (scenario == RawCase::EmptyBodyProgress)
                body.clear();
            Message body_message = BodyMessage{std::move(body)};
            co_await raw_write(socket, std::move(body_message));
        }
    }

    RawCaseResult result;
    Frame terminal = co_await raw_read(socket, server_caps.wire.max_frame_payload);
    result.received_error = terminal.type == MessageType::ERROR;
    result.error_payload_bytes = terminal.payload.size();
    if (result.received_error)
        (void)raw_decode<ErrorMessage>(terminal);
    std::array<uint8_t, 4> next{};
    boost::system::error_code error;
    (void)co_await asio::async_read(socket, asio::buffer(next),
                                    asio::redirect_error(asio::use_awaitable, error));
    result.closed_after_error = error == asio::error::eof || error == asio::error::connection_reset;
    co_return result;
}

std::pair<RawCaseResult, ServerRunResult> run_raw_server_case(EndpointCaps caps, RawCase scenario,
                                                              uint64_t c_id) {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    P50ServerEndpoint server(Id128::from_u64(900 + c_id), caps);
    std::future<ServerRunResult> server_future =
        asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
    std::future<RawCaseResult> peer_future = asio::co_spawn(
        context, run_raw_case(acceptor.local_endpoint(), Id128::from_u64(c_id), caps, scenario),
        asio::use_future);
    context.run();
    return {peer_future.get(), server_future.get()};
}

ServerRunResult force_route_reset(P50ServerEndpoint& server, CStoreGuid c_guid,
                                  HistoryNonce nonce) {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    std::future<ServerRunResult> server_future =
        asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
    std::future<void> peer_future = asio::co_spawn(
        context, raw_reset_only(acceptor.local_endpoint(), c_guid, nonce), asio::use_future);
    context.run();
    peer_future.get();
    return server_future.get();
}


bool force_rejected_route_reset(P50ServerEndpoint& server, CStoreGuid c_guid,
                                HistoryNonce nonce) {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    std::future<ServerRunResult> server_future =
        asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
    std::future<bool> peer_future = asio::co_spawn(
        context, raw_reset_rejected(acceptor.local_endpoint(), c_guid, nonce), asio::use_future);
    context.run();
    const bool rejected = peer_future.get();
    require(server_future.get().status == ServerRunStatus::TerminalError,
            "invalid route reset did not terminate the F session");
    return rejected;
}

std::pair<InterruptedRawTu, ServerRunResult> force_terminal_body_failure(
    P50ServerEndpoint& server, CStoreGuid c_guid,
    std::span<const uint8_t> input, TerminalBodyFailure scenario,
    bool receive_terminal = true) {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    std::future<ServerRunResult> server_future =
        asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
    std::future<InterruptedRawTu> peer_future = asio::co_spawn(
        context,
        raw_terminal_body_failure(acceptor.local_endpoint(), c_guid, input,
                                  scenario, receive_terminal),
        asio::use_future);
    context.run();
    InterruptedRawTu interrupted = peer_future.get();
    return {std::move(interrupted), server_future.get()};
}

ServerRunResult force_different_begin_rejected(
    P50ServerEndpoint& server, CStoreGuid c_guid, const TxBegin& interrupted) {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    std::future<ServerRunResult> server_future =
        asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
    std::future<void> peer_future = asio::co_spawn(
        context,
        raw_different_begin_rejected(acceptor.local_endpoint(), c_guid,
                                     interrupted),
        asio::use_future);
    context.run();
    peer_future.get();
    return server_future.get();
}

ServerRunResult force_exact_whole_tu_replay(
    P50ServerEndpoint& server, CStoreGuid c_guid,
    const InterruptedRawTu& interrupted, bool expect_commit) {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    std::future<ServerRunResult> server_future =
        asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
    std::future<void> peer_future;
    if (expect_commit) {
        peer_future = asio::co_spawn(
            context,
            raw_exact_whole_tu_replay(acceptor.local_endpoint(), c_guid,
                                      interrupted),
            asio::use_future);
    } else {
        peer_future = asio::co_spawn(
            context,
            raw_exact_whole_tu_rejected(acceptor.local_endpoint(), c_guid,
                                        interrupted),
            asio::use_future);
    }
    context.run();
    peer_future.get();
    return server_future.get();
}

void test_normal_zero_and_completion_stamps() {
    CompletionLog completions;
    ActionTrace actions;
    P50ServerEndpoint server(Id128::from_u64(200), {}, &completions, &actions);
    TestClient client(Id128::from_u64(100), {}, HistoryNonce{10}, &completions, &actions);
    const std::vector<uint8_t> input = pseudo_random_bytes(4096);
    const PairResult first = run_pair(client, server, admit(client, input));
    require(first.client.status == ClientRunStatus::Committed &&
                first.server.status == ServerRunStatus::Completed,
            "normal loopback did not complete");
    require(first.client.reconnect == EndpointReconnectOutcome::ColdFStore,
            "first loopback did not take the explicit cold-F path");
    require(copy_input(server, client.c_store_guid()) == input,
            "normal loopback did not retain exact input");

    const std::vector<uint8_t> empty;
    const PairResult second = run_pair(client, server, admit(client, empty));
    require(second.client.status == ClientRunStatus::Committed &&
                second.client.reconnect == EndpointReconnectOutcome::ExactMatch,
            "zero-length component transaction did not complete on the exact route");
    require(copy_input(server, client.c_store_guid()) == empty,
            "zero-length ZSTD_TU did not materialize exactly");

    size_t bound_c = 0;
    size_t bound_f = 0;
    for (const AsyncCompletion& completion : completions.completions()) {
        require(completion.stamp.session_serial != 0,
                "async completion omitted its session serial");
        (void)async_operation_name(completion.stamp.operation);
        if (completion.stamp.transaction_bound) {
            require(completion.stamp.history_nonce.value != 0 &&
                        nonzero(completion.stamp.transaction_digest),
                    "transaction-bound completion omitted route identity");
            if (completion.stamp.actor == ActorSide::C)
                ++bound_c;
            else
                ++bound_f;
        }
    }
    require(bound_c != 0 && bound_f != 0,
            "loopback did not capture full transaction identity on both sides");
    require_trace(actions, "normal/zero completion trace");
}

enum class CompletionStampField {
    Actor,
    Operation,
    CStoreGuid,
    FStoreGuid,
    SessionSerial,
    HistoryNonce,
    RelSeq,
    TuSeq,
    TransactionDigest,
    RawDigest,
    TransactionBound,
};

void change_completion_field(CompletionStamp& stamp, CompletionStampField field) {
    switch (field) {
    case CompletionStampField::Actor:
        stamp.actor = stamp.actor == ActorSide::C ? ActorSide::F : ActorSide::C;
        break;
    case CompletionStampField::Operation:
        stamp.operation = stamp.operation == AsyncOperationKind::WriteFragment
                              ? AsyncOperationKind::ReadPayload
                              : AsyncOperationKind::WriteFragment;
        break;
    case CompletionStampField::CStoreGuid:
        stamp.c_store_guid.bytes[0] ^= 0x80;
        break;
    case CompletionStampField::FStoreGuid:
        stamp.f_store_guid.bytes[0] ^= 0x80;
        break;
    case CompletionStampField::SessionSerial:
        ++stamp.session_serial;
        break;
    case CompletionStampField::HistoryNonce:
        ++stamp.history_nonce.value;
        break;
    case CompletionStampField::RelSeq:
        ++stamp.rel_seq.value;
        break;
    case CompletionStampField::TuSeq:
        ++stamp.tu_seq.value;
        break;
    case CompletionStampField::TransactionDigest:
        stamp.transaction_digest.bytes[0] ^= 0x80;
        break;
    case CompletionStampField::RawDigest:
        stamp.raw_digest.bytes[0] ^= 0x80;
        break;
    case CompletionStampField::TransactionBound:
        stamp.transaction_bound = !stamp.transaction_bound;
        break;
    }
}

void change_live_identity_field(CompletionLiveIdentity& identity,
                                CompletionStampField field) {
    switch (field) {
    case CompletionStampField::Actor:
        identity.actor = identity.actor == ActorSide::C ? ActorSide::F : ActorSide::C;
        break;
    case CompletionStampField::Operation:
        fail("operation is not part of the live endpoint identity");
    case CompletionStampField::CStoreGuid:
        identity.c_store_guid.bytes[0] ^= 0x80;
        break;
    case CompletionStampField::FStoreGuid:
        identity.f_store_guid.bytes[0] ^= 0x80;
        break;
    case CompletionStampField::SessionSerial:
        ++identity.session_serial;
        break;
    case CompletionStampField::HistoryNonce:
        ++identity.history_nonce.value;
        break;
    case CompletionStampField::RelSeq:
        ++identity.rel_seq.value;
        break;
    case CompletionStampField::TuSeq:
        ++identity.tu_seq.value;
        break;
    case CompletionStampField::TransactionDigest:
        identity.transaction_digest.bytes[0] ^= 0x80;
        break;
    case CompletionStampField::RawDigest:
        identity.raw_digest.bytes[0] ^= 0x80;
        break;
    case CompletionStampField::TransactionBound:
        identity.transaction_bound = !identity.transaction_bound;
        break;
    }
}

void test_completion_stamp_correspondence() {
    const std::array fields{
        std::pair{CompletionStampField::Actor, "actor"},
        std::pair{CompletionStampField::Operation, "operation"},
        std::pair{CompletionStampField::CStoreGuid, "C_STORE_GUID"},
        std::pair{CompletionStampField::FStoreGuid, "F_STORE_GUID"},
        std::pair{CompletionStampField::SessionSerial, "session serial"},
        std::pair{CompletionStampField::HistoryNonce, "HISTORY_NONCE"},
        std::pair{CompletionStampField::RelSeq, "REL_SEQ"},
        std::pair{CompletionStampField::TuSeq, "TU_SEQ"},
        std::pair{CompletionStampField::TransactionDigest, "transaction digest"},
        std::pair{CompletionStampField::RawDigest, "raw digest"},
        std::pair{CompletionStampField::TransactionBound, "transaction-bound flag"},
    };

    uint64_t identity = 3000;
    for (const ActorSide actor : {ActorSide::C, ActorSide::F}) {
        const AsyncOperationKind target = actor == ActorSide::C
                                              ? AsyncOperationKind::WriteFragment
                                              : AsyncOperationKind::ReadPayload;
        for (const auto& [field, name] : fields) {
            const CompletionStampField selected_field = field;
            const std::string_view field_name = name;
            P50ServerEndpoint server(Id128::from_u64(identity++));
            TestClient client(Id128::from_u64(identity++));
            const std::vector<uint8_t> input = pseudo_random_bytes(4096);
            bool changed = false;
            EndpointIoControl client_control;
            EndpointIoControl server_control;
            EndpointIoControl& selected =
                actor == ActorSide::C ? client_control : server_control;
            selected.before_completion_check = [&](CompletionStamp& observed) {
                if (changed || observed.actor != actor ||
                    observed.operation != target || !observed.transaction_bound)
                    return;
                change_completion_field(observed, selected_field);
                changed = true;
            };

            const PairResult rejected = run_pair(client, server, admit(client, input),
                                                 client_control, server_control);
            std::string context = actor == ActorSide::C ? "C " : "F ";
            context.append(field_name);
            require(changed, context + " completion mutation did not run");
            require(rejected.client.status == ClientRunStatus::Disconnected &&
                        rejected.server.status == ServerRunStatus::Disconnected &&
                        client.has_active_transaction() &&
                        !copy_input(server, client.c_store_guid()),
                    context + " completion mutation was not rejected exactly");

            const PairResult replayed = run_pair(client, server);
            require(replayed.client.status == ClientRunStatus::Committed &&
                        replayed.server.status == ServerRunStatus::Completed &&
                        replayed.client.reconnect == EndpointReconnectOutcome::ExactMatch &&
                        !client.has_active_transaction() &&
                        copy_input(server, client.c_store_guid()) == input,
                    context + " completion rejection did not permit exact follow-up");
        }
    }

    P50ServerEndpoint server(Id128::from_u64(identity++));
    TestClient client(Id128::from_u64(identity++));
    bool saw_unbound_c = false;
    bool saw_unbound_f = false;
    EndpointIoControl client_control;
    EndpointIoControl server_control;
    client_control.before_completion_check = [&](CompletionStamp& observed) {
        saw_unbound_c = saw_unbound_c ||
                        (observed.actor == ActorSide::C && !observed.transaction_bound);
    };
    server_control.before_completion_check = [&](CompletionStamp& observed) {
        saw_unbound_f = saw_unbound_f ||
                        (observed.actor == ActorSide::F && !observed.transaction_bound);
    };
    const std::vector<uint8_t> first_input = pseudo_random_bytes(1024);
    const PairResult first = run_pair(client, server, admit(client, first_input),
                                      client_control, server_control);
    require(first.client.status == ClientRunStatus::Committed && saw_unbound_c && saw_unbound_f,
            "legal non-transaction completions did not complete on both endpoints");
    const std::vector<uint8_t> second_input = pseudo_random_bytes(1536);
    const PairResult follow_up = run_pair(client, server, admit(client, second_input));
    require(follow_up.client.status == ClientRunStatus::Committed &&
                follow_up.server.status == ServerRunStatus::Completed &&
                follow_up.client.reconnect == EndpointReconnectOutcome::ExactMatch &&
                copy_input(server, client.c_store_guid()) == second_input,
            "legal non-transaction completions did not preserve exact follow-up");
}

void test_completion_live_identity_correspondence() {
    const std::array fields{
        std::pair{CompletionStampField::Actor, "actor"},
        std::pair{CompletionStampField::CStoreGuid, "C_STORE_GUID"},
        std::pair{CompletionStampField::FStoreGuid, "F_STORE_GUID"},
        std::pair{CompletionStampField::SessionSerial, "session serial"},
        std::pair{CompletionStampField::HistoryNonce, "HISTORY_NONCE"},
        std::pair{CompletionStampField::RelSeq, "REL_SEQ"},
        std::pair{CompletionStampField::TuSeq, "TU_SEQ"},
        std::pair{CompletionStampField::TransactionDigest, "transaction digest"},
        std::pair{CompletionStampField::RawDigest, "raw digest"},
        std::pair{CompletionStampField::TransactionBound, "transaction-bound state"},
    };

    uint64_t identity = 4000;
    size_t rejection_gaps = 0;
    for (const ActorSide actor : {ActorSide::C, ActorSide::F}) {
        const AsyncOperationKind target = actor == ActorSide::C
                                              ? AsyncOperationKind::WriteFragment
                                              : AsyncOperationKind::ReadPayload;
        for (const auto& [field, name] : fields) {
            const CompletionStampField selected_field = field;
            const std::string_view field_name = name;
            P50ServerEndpoint server(Id128::from_u64(identity++));
            TestClient client(Id128::from_u64(identity++));
            const std::vector<uint8_t> input = pseudo_random_bytes(4096);
            bool changed = false;
            EndpointIoControl client_control;
            EndpointIoControl server_control;
            EndpointIoControl& selected =
                actor == ActorSide::C ? client_control : server_control;
            selected.before_live_identity_check =
                [&](const CompletionStamp& expected, CompletionLiveIdentity& live) {
                    if (changed || expected.actor != actor ||
                        expected.operation != target || !expected.transaction_bound)
                        return;
                    change_live_identity_field(live, selected_field);
                    changed = true;
                };

            const PairResult rejected = run_pair(client, server, admit(client, input),
                                                 client_control, server_control);
            std::string context = actor == ActorSide::C ? "C live " : "F live ";
            context.append(field_name);
            require(changed, context + " mutation did not run");
            const bool rejected_exactly =
                rejected.client.status == ClientRunStatus::Disconnected &&
                rejected.server.status == ServerRunStatus::Disconnected &&
                client.has_active_transaction() &&
                !copy_input(server, client.c_store_guid());
            if (!rejected_exactly) {
                std::cerr << "p50_endpoint_test: " << context
                          << " mutation was not rejected exactly\n";
                ++rejection_gaps;
                continue;
            }

            const PairResult replayed = run_pair(client, server);
            require(replayed.client.status == ClientRunStatus::Committed &&
                        replayed.server.status == ServerRunStatus::Completed &&
                        replayed.client.reconnect == EndpointReconnectOutcome::ExactMatch &&
                        !client.has_active_transaction() &&
                        copy_input(server, client.c_store_guid()) == input,
                    context + " rejection did not permit exact follow-up");
        }
    }
    require(rejection_gaps == 0,
            "one or more live identity mutations were not rejected exactly");

    P50ServerEndpoint server(Id128::from_u64(identity++));
    TestClient client(Id128::from_u64(identity++));
    bool saw_unbound_c = false;
    bool saw_unbound_f = false;
    EndpointIoControl client_control;
    EndpointIoControl server_control;
    client_control.before_live_identity_check =
        [&](const CompletionStamp& expected, CompletionLiveIdentity& live) {
            saw_unbound_c = saw_unbound_c ||
                            (expected.actor == ActorSide::C &&
                             !expected.transaction_bound && !live.transaction_bound);
        };
    server_control.before_live_identity_check =
        [&](const CompletionStamp& expected, CompletionLiveIdentity& live) {
            saw_unbound_f = saw_unbound_f ||
                            (expected.actor == ActorSide::F &&
                             !expected.transaction_bound && !live.transaction_bound);
        };
    const std::vector<uint8_t> first_input = pseudo_random_bytes(1024);
    const PairResult first = run_pair(client, server, admit(client, first_input),
                                      client_control, server_control);
    require(first.client.status == ClientRunStatus::Committed && saw_unbound_c && saw_unbound_f,
            "legal unbound live identities did not complete on both endpoints");
    const std::vector<uint8_t> second_input = pseudo_random_bytes(1536);
    const PairResult follow_up = run_pair(client, server, admit(client, second_input));
    require(follow_up.client.status == ClientRunStatus::Committed &&
                follow_up.server.status == ServerRunStatus::Completed &&
                follow_up.client.reconnect == EndpointReconnectOutcome::ExactMatch &&
                copy_input(server, client.c_store_guid()) == second_input,
            "legal unbound live identities did not preserve exact follow-up");
}

void test_idempotent_prepare_admission() {
    TestClient client(Id128::from_u64(150));
    const PrepareRequestKey request{7, 91};
    const std::vector<uint8_t> input = bytes("one canonical local request\n");
    const PreparedTuHandle first = client.prepare(request, input);
    const PreparedTuHandle replay = client.prepare(request, input);
    require(first == replay && client.authority->live_entry_count() == 1 &&
                client.authority->retained_encoded_bytes() != 0,
            "same prepare key/input did not return one retained authority entry");
    std::vector<uint8_t> same_length_different = input;
    same_length_different[same_length_different.size() / 2] ^= 1;
    require_throws<std::invalid_argument>(
        [&] { (void)client.prepare(request, same_length_different); },
        "prepare key was rebound to different same-length input");

    auto other_authority =
        std::make_shared<P50PreparationAuthority>(Id128::from_u64(149));
    require_throws<std::invalid_argument>([&] { (void)other_authority->retain(first); },
                                          "foreign preparation handle was accepted");
    require(client.authority->release(replay) == 0 &&
                client.authority->live_entry_count() == 0 &&
                client.authority->retained_encoded_bytes() == 0,
            "idempotent replay acquired an additional reference");
    require_throws<std::invalid_argument>([&] { (void)client.authority->release(first); },
                                          "zero-reference handle was released twice");

    const PreparedTuHandle retained = client.prepare({7, 92}, input);
    require(client.authority->retain(retained) == 2 &&
                client.authority->release(retained) == 1 &&
                client.authority->release(retained) == 0,
            "explicit retain did not represent one additional owner");

    PreparationAuthorityLimits one_entry;
    one_entry.max_live_entries = 1;
    auto bounded = std::make_shared<P50PreparationAuthority>(
        Id128::from_u64(148), EndpointCaps{}.zstd, one_entry);
    const PreparedTuHandle bounded_first =
        bounded->prepare(PrepareRequestKey{9, 1}, bytes("bounded first\n"));
    const PreparedTuHandle bounded_replay =
        bounded->prepare(PrepareRequestKey{9, 1}, bytes("bounded first\n"));
    require(bounded_first == bounded_replay && bounded->live_entry_count() == 1,
            "idempotent replay consumed another bounded entry");
    require_throws<std::length_error>(
        [&] { (void)bounded->prepare(PrepareRequestKey{9, 2}, bytes("bounded second\n")); },
        "preparation authority exceeded its live-entry bound");
    require(bounded->release(bounded_replay) == 0,
            "bounded replay acquired an additional reference");
    require_throws<std::invalid_argument>(
        [&] { (void)bounded->release(bounded_first); },
        "bounded replay retained an invisible owner");

    PreparationAuthorityLimits one_byte;
    one_byte.max_retained_encoded_bytes = 1;
    P50PreparationAuthority byte_bounded(Id128::from_u64(147), EndpointCaps{}.zstd,
                                         one_byte);
    require_throws<std::length_error>(
        [&] {
            (void)byte_bounded.prepare(PrepareRequestKey{10, 1},
                                       bytes("retained bytes exceed one"));
        },
        "preparation authority exceeded its retained-byte bound");
    require(byte_bounded.live_entry_count() == 0 &&
                byte_bounded.retained_encoded_bytes() == 0,
            "failed retained-byte admission published a partial entry");

    EndpointCaps tight;
    tight.zstd.max_raw_bytes = 4;
    CompletionLog completions;
    TestClient capped(Id128::from_u64(151), tight, HistoryNonce{1}, &completions);
    const PrepareRequestKey retried{8, 1};
    require_throws<std::length_error>(
        [&] { (void)capped.prepare(retried, bytes("too long")); },
        "failed preparation ignored its decoded-input cap");
    const PreparedTuHandle admitted = capped.prepare(retried, bytes("okay"));
    P50ServerEndpoint server(Id128::from_u64(152), tight, &completions);
    require(run_pair(capped, server, admitted).client.status == ClientRunStatus::Committed,
            "admitted preparation did not complete after a bounded failure");
    require(std::any_of(completions.completions().begin(), completions.completions().end(),
                        [](const AsyncCompletion& completion) {
                            return completion.stamp.actor == ActorSide::C &&
                                   completion.stamp.transaction_bound &&
                                   completion.stamp.tu_seq.value == 0;
                        }),
            "failed preparation consumed the next TU_SEQ");
    require(capped.authority->release(admitted) == 0,
            "completed preparation did not release explicitly to zero");

    require_throws<std::invalid_argument>(
        [&] { P50PreparationAuthority rejected(CStoreGuid{}); },
        "zero C_STORE_GUID was accepted by the preparation authority");
    require_throws<std::invalid_argument>(
        [&] { (void)client.prepare({0, 1}, input); },
        "zero producer session was accepted by the preparation authority");
    require_throws<std::invalid_argument>(
        [&] { (void)client.prepare({1, 0}, input); },
        "zero request token was accepted by the preparation authority");
    require_throws<std::invalid_argument>(
        [&] { P50ServerEndpoint rejected(FStoreGuid{}); },
        "zero F_STORE_GUID was accepted by the endpoint");
    P50ServerEndpointConfig zero_error;
    zero_error.protocol_error_code = 0;
    require_throws<std::invalid_argument>(
        [&] {
            P50ServerEndpoint rejected(Id128::from_u64(153), {}, nullptr,
                                       nullptr, zero_error);
        },
        "zero protocol ERROR code was accepted by the endpoint");
    P50ServerEndpoint reset_target(Id128::from_u64(154));
    require_throws<std::invalid_argument>(
        [&] { reset_target.reset_store(FStoreGuid{}); },
        "zero F_STORE_GUID was accepted as an incarnation replacement");

    for (const int invalid_window_log : {9, 32}) {
        EndpointCaps invalid;
        invalid.zstd.max_window_log = invalid_window_log;
        require_throws<std::invalid_argument>(
            [&] {
                P50PreparationAuthority rejected(Id128::from_u64(155),
                                                 invalid.zstd);
            },
            "preparation authority accepted an out-of-range Zstd window log");

        auto valid_authority = std::make_shared<P50PreparationAuthority>(
            Id128::from_u64(156));
        require_throws<std::invalid_argument>(
            [&] { P50ClientEndpoint rejected(valid_authority, invalid); },
            "client endpoint accepted an out-of-range Zstd window log");
        require_throws<std::invalid_argument>(
            [&] { P50ServerEndpoint rejected(Id128::from_u64(157), invalid); },
            "server endpoint accepted an out-of-range Zstd window log");
    }
}

void test_candidate_stage_has_no_revision_residue() {
    P50ServerEndpoint server(Id128::from_u64(154));
    for (uint64_t index = 1; index != 17; ++index) {
        TestClient candidate(Id128::from_u64(154 + index));
        EndpointIoControl stop_after_hello;
        stop_after_hello.close_after_write = MessageType::SESSION_HELLO;
        const PairResult stopped = run_pair(
            candidate, server, admit(candidate, bytes("candidate-only input\n")),
            stop_after_hello);
        require(stopped.client.status == ClientRunStatus::Disconnected &&
                    stopped.server.status == ServerRunStatus::Disconnected &&
                    server.namespace_count() == 0 && server.revision_count() == 0 &&
                    server.live_session_count() == 0,
                "unactivated candidate retained namespace, revision, or live-session state");
    }

    TestClient active(Id128::from_u64(171));
    const PairResult committed =
        run_pair(active, server, admit(active, bytes("activated input\n")));
    require(committed.client.status == ClientRunStatus::Committed &&
                committed.server.status == ServerRunStatus::Completed &&
                server.namespace_count() == 1 && server.revision_count() == 1 &&
                server.live_session_count() == 0,
            "activated namespace did not acquire exactly one revision owner");
}

void test_input_record_owner_and_aggregate_limits() {
    {
        P50ServerEndpoint server(Id128::from_u64(172));
        TestClient client(Id128::from_u64(173));
        const std::vector<uint8_t> input = pseudo_random_bytes(32 * 1024);
        const PairResult completed = run_pair(client, server, admit(client, input));
        require(completed.client.status == ClientRunStatus::Committed &&
                    completed.server.status == ServerRunStatus::Completed &&
                    completed.server.committed_input.has_value() &&
                    completed.server.committed_input ==
                        server.last_committed_input(client.c_store_guid()) &&
                    server.owner_usage().retained_input_records == 1 &&
                    server.owner_usage().retained_input_bytes == input.size(),
                "route commit did not atomically publish one InputRecord key");

        InputCursor authorized = server.attach_input(*completed.server.committed_input);
        std::array<uint8_t, 257> prefix{};
        require(authorized.read(prefix) == prefix.size() &&
                    std::equal(prefix.begin(), prefix.end(), input.begin()),
                "authorized InputRecord cursor read the wrong prefix");
        server.close_input_job(*completed.server.committed_input);
        require_throws<std::logic_error>(
            [&] { (void)server.attach_input(*completed.server.committed_input); },
            "closed logical job accepted a new InputRecord attachment");
        server.collect_input_garbage();
        require(server.owner_usage().retained_input_records == 1,
                "job close reclaimed input owned by an authorized cursor");
        std::vector<uint8_t> reconstructed(prefix.begin(), prefix.end());
        std::vector<uint8_t> remainder = drain_input(authorized);
        reconstructed.insert(reconstructed.end(), remainder.begin(), remainder.end());
        require(reconstructed == input,
                "authorized cursor lost exact input after logical-job close");
        authorized = InputCursor{};
        server.collect_input_garbage();
        require(server.owner_usage().retained_input_records == 0 &&
                    server.owner_usage().retained_input_bytes == 0,
                "closed InputRecord did not collect after its final cursor");
    }

    {
        P50ServerEndpointConfig config;
        config.input_job_state =
            [](CStoreGuid, const TxBegin&, const TxCommit&, std::span<const uint8_t>) {
                return InputJobState::Closed;
            };
        P50ServerEndpoint server(Id128::from_u64(174), {}, nullptr, nullptr,
                                 std::move(config));
        TestClient client(Id128::from_u64(175));
        const PairResult completed =
            run_pair(client, server, admit(client, bytes("late closed job input\n")));
        require(completed.client.status == ClientRunStatus::Committed &&
                    completed.server.status == ServerRunStatus::Completed &&
                    !completed.server.committed_input &&
                    server.owner_usage().retained_input_records == 0 &&
                    server.owner_usage().retained_input_bytes == 0,
                "closed-before-commit job created a compiler-visible lease");
    }

    {
        P50ServerEndpoint server(Id128::from_u64(188));
        TestClient client(Id128::from_u64(189));
        const std::vector<uint8_t> input = pseudo_random_bytes(8192);
        const PairResult completed = run_pair(client, server, admit(client, input));
        require(completed.server.committed_input.has_value(),
                "store-reset fixture did not publish an InputRecord");
        InputCursor authorized = server.attach_input(*completed.server.committed_input);
        server.reset_store(Id128::from_u64(190));
        const P50ServerOwnerUsage reset_usage = server.owner_usage();
        require(reset_usage.live_sessions == 0 && reset_usage.namespaces == 0 &&
                    reset_usage.revisions == 0 && reset_usage.pending_encoded_bytes == 0 &&
                    reset_usage.pending_raw_bytes == 0 &&
                    reset_usage.decoder_window_bytes == 0 &&
                    reset_usage.retained_input_records == 0 &&
                    reset_usage.retained_input_bytes == 0 &&
                    drain_input(authorized) == input,
                "F-store replacement lost an authorized cursor or retained owner state");
    }

    {
        P50ServerEndpointConfig config;
        config.owner_limits.max_namespaces = 1;
        P50ServerEndpoint server(Id128::from_u64(176), {}, nullptr, nullptr,
                                 std::move(config));
        TestClient first(Id128::from_u64(177));
        TestClient second(Id128::from_u64(178));
        require(run_pair(first, server, admit(first, bytes("first namespace\n")))
                        .client.status == ClientRunStatus::Committed,
                "first namespace did not fit its aggregate bound");
        const PairResult rejected =
            run_pair(second, server, admit(second, bytes("second namespace\n")));
        require(rejected.client.status == ClientRunStatus::TerminalError &&
                    rejected.server.status == ServerRunStatus::TerminalError &&
                    server.owner_usage().namespaces == 1 &&
                    server.owner_usage().revisions == 1,
                "namespace-cap failure changed aggregate owner state");
    }

    {
        P50ServerEndpointConfig config;
        config.owner_limits.max_pending_encoded_bytes = 1;
        P50ServerEndpoint server(Id128::from_u64(179), {}, nullptr, nullptr,
                                 std::move(config));
        TestClient client(Id128::from_u64(180));
        const PairResult rejected =
            run_pair(client, server, admit(client, pseudo_random_bytes(4096)));
        const P50ServerOwnerUsage usage = server.owner_usage();
        require(rejected.client.status == ClientRunStatus::TerminalError &&
                    rejected.server.status == ServerRunStatus::TerminalError &&
                    client.has_active_transaction() &&
                    usage.pending_encoded_bytes == 0 && usage.pending_raw_bytes == 0 &&
                    usage.decoder_window_bytes == 0 && usage.retained_input_records == 0,
                "pending-input cap failure leaked an aggregate reservation");
    }

    {
        P50ServerEndpointConfig config;
        config.owner_limits.max_pending_encoded_bytes = 4096;
        config.owner_limits.max_decoder_window_bytes =
            uint64_t{2} << EndpointCaps{}.zstd.max_window_log;
        const CompetingPairResult result =
            run_competing_pair(std::move(config), 196, 3000, 3001);
        const size_t committed =
            static_cast<size_t>(result.first_client.status ==
                                ClientRunStatus::Committed) +
            static_cast<size_t>(result.second_client.status ==
                                ClientRunStatus::Committed);
        const size_t completed =
            static_cast<size_t>(result.first_server.status ==
                                ServerRunStatus::Completed) +
            static_cast<size_t>(result.second_server.status ==
                                ServerRunStatus::Completed);
        const size_t terminal =
            static_cast<size_t>(result.first_server.status ==
                                ServerRunStatus::TerminalError) +
            static_cast<size_t>(result.second_server.status ==
                                ServerRunStatus::TerminalError);
        require(committed == 1 && completed == 1 && terminal == 1 &&
                    result.usage.pending_encoded_bytes == 0 &&
                    result.usage.pending_raw_bytes == 0 &&
                    result.usage.decoder_window_bytes == 0 &&
                    result.usage.retained_input_records == 1,
                "aggregate encoded-byte budget admitted overlapping dialogues");
    }

    {
        P50ServerEndpointConfig config;
        config.owner_limits.max_pending_encoded_bytes = 8192;
        config.owner_limits.max_pending_raw_bytes = 4096;
        config.owner_limits.max_decoder_window_bytes =
            uint64_t{2} << EndpointCaps{}.zstd.max_window_log;
        const CompetingPairResult result =
            run_competing_pair(std::move(config), 199, 3000, 3001);
        const size_t committed =
            static_cast<size_t>(result.first_client.status ==
                                ClientRunStatus::Committed) +
            static_cast<size_t>(result.second_client.status ==
                                ClientRunStatus::Committed);
        const size_t completed =
            static_cast<size_t>(result.first_server.status ==
                                ServerRunStatus::Completed) +
            static_cast<size_t>(result.second_server.status ==
                                ServerRunStatus::Completed);
        const size_t terminal =
            static_cast<size_t>(result.first_server.status ==
                                ServerRunStatus::TerminalError) +
            static_cast<size_t>(result.second_server.status ==
                                ServerRunStatus::TerminalError);
        require(committed == 1 && completed == 1 && terminal == 1 &&
                    result.usage.pending_encoded_bytes == 0 &&
                    result.usage.pending_raw_bytes == 0 &&
                    result.usage.decoder_window_bytes == 0 &&
                    result.usage.retained_input_records == 1,
                "aggregate raw-byte budget admitted overlapping dialogues");
    }

    {
        P50ServerEndpointConfig config;
        config.owner_limits.max_retained_input_bytes = 8;
        P50ServerEndpoint server(Id128::from_u64(181), {}, nullptr, nullptr,
                                 std::move(config));
        TestClient client(Id128::from_u64(182));
        const PairResult rejected =
            run_pair(client, server, admit(client, bytes("larger than eight bytes")));
        const P50ServerOwnerUsage usage = server.owner_usage();
        require(rejected.client.status == ClientRunStatus::TerminalError &&
                    rejected.server.status == ServerRunStatus::TerminalError &&
                    client.has_active_transaction() && usage.retained_input_records == 0 &&
                    usage.retained_input_bytes == 0 && usage.pending_encoded_bytes == 0 &&
                    usage.pending_raw_bytes == 0 && usage.decoder_window_bytes == 0,
                "InputRecord-cap failure committed or leaked owner resources");
    }

    {
        P50ServerEndpointConfig config;
        config.owner_limits.max_retained_input_records = 1;
        P50ServerEndpoint server(Id128::from_u64(202), {}, nullptr, nullptr,
                                 std::move(config));
        TestClient client(Id128::from_u64(203));
        const PairResult first =
            run_pair(client, server, admit(client, bytes("first retained input\n")));
        const PairResult second =
            run_pair(client, server, admit(client, bytes("second retained input\n")));
        const P50ServerOwnerUsage usage = server.owner_usage();
        require(first.client.status == ClientRunStatus::Committed &&
                    first.server.status == ServerRunStatus::Completed &&
                    second.client.status == ClientRunStatus::TerminalError &&
                    second.server.status == ServerRunStatus::TerminalError &&
                    usage.retained_input_records == 1 &&
                    usage.pending_encoded_bytes == 0 &&
                    usage.pending_raw_bytes == 0 &&
                    usage.decoder_window_bytes == 0,
                "InputRecord-count bound admitted a second retained input");
    }

    {
        CompletionLog completions(0);
        ActionTrace actions(0);
        P50ServerEndpoint server(Id128::from_u64(183), {}, &completions, &actions);
        TestClient client(Id128::from_u64(184), {}, HistoryNonce{1}, &completions,
                          &actions);
        const PairResult completed =
            run_pair(client, server, admit(client, bytes("diagnostic loss is not state\n")));
        require(completed.client.status == ClientRunStatus::Committed &&
                    completed.server.status == ServerRunStatus::Completed &&
                    !completions.valid() && completions.completions().empty() &&
                    !actions.valid() && actions.records().empty() &&
                    server.owner_usage().retained_input_records == 1,
                "bounded diagnostic loss changed the durable protocol result");
    }

    {
        P50ServerEndpointConfig config;
        config.owner_limits.max_live_sessions = 1;
        P50ServerEndpoint server(Id128::from_u64(191), {}, nullptr, nullptr,
                                 std::move(config));
        TestClient client(Id128::from_u64(192));
        const PreparedTuHandle prepared =
            admit(client, bytes("one accepted live session\n"));
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> accepted =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<ServerRunResult> excess =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<ClientRunResult> client_result = asio::co_spawn(
            context, client.endpoint.run(acceptor.local_endpoint(), prepared),
            asio::use_future);
        context.run();
        require(client_result.get().status == ClientRunStatus::Committed &&
                    accepted.get().status == ServerRunStatus::Completed,
                "live-session bound rejected its admitted dialogue");
        require_throws<std::length_error>(
            [&] { (void)excess.get(); },
            "live-session aggregate bound admitted an extra dialogue");
        require(server.owner_usage().live_sessions == 0,
                "live-session bound retained a completed session");
    }

    {
        P50ServerEndpointConfig config;
        config.owner_limits.max_decoder_window_bytes =
            uint64_t{1} << EndpointCaps{}.zstd.max_window_log;
        P50ServerEndpoint server(Id128::from_u64(193), {}, nullptr, nullptr,
                                 std::move(config));
        TestClient first(Id128::from_u64(194));
        TestClient second(Id128::from_u64(195));
        const PreparedTuHandle first_prepared =
            admit(first, pseudo_random_bytes(4096));
        const PreparedTuHandle second_prepared =
            admit(second, pseudo_random_bytes(4097));
        EndpointIoControl fragment_first;
        fragment_first.max_write_fragment = 1;
        EndpointIoControl fragment_second;
        fragment_second.max_write_fragment = 1;
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> first_server =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<ServerRunResult> second_server =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<ClientRunResult> first_client = asio::co_spawn(
            context,
            first.endpoint.run(acceptor.local_endpoint(), first_prepared,
                               fragment_first),
            asio::use_future);
        std::future<ClientRunResult> second_client = asio::co_spawn(
            context,
            second.endpoint.run(acceptor.local_endpoint(), second_prepared,
                                fragment_second),
            asio::use_future);
        context.run();
        const ClientRunResult first_client_result = first_client.get();
        const ClientRunResult second_client_result = second_client.get();
        const ServerRunResult first_server_result = first_server.get();
        const ServerRunResult second_server_result = second_server.get();
        const size_t committed_clients =
            static_cast<size_t>(first_client_result.status == ClientRunStatus::Committed) +
            static_cast<size_t>(second_client_result.status == ClientRunStatus::Committed);
        const size_t completed_servers =
            static_cast<size_t>(first_server_result.status == ServerRunStatus::Completed) +
            static_cast<size_t>(second_server_result.status == ServerRunStatus::Completed);
        const size_t terminal_servers =
            static_cast<size_t>(first_server_result.status == ServerRunStatus::TerminalError) +
            static_cast<size_t>(second_server_result.status == ServerRunStatus::TerminalError);
        const P50ServerOwnerUsage usage = server.owner_usage();
        require(committed_clients == 1 && completed_servers == 1 && terminal_servers == 1 &&
                    usage.pending_encoded_bytes == 0 && usage.pending_raw_bytes == 0 &&
                    usage.decoder_window_bytes == 0 &&
                    usage.retained_input_records == 1,
                "aggregate decoder-window budget admitted overlapping dialogues");
    }

    {
        auto authority =
            std::make_shared<P50PreparationAuthority>(Id128::from_u64(185));
        const PreparedTuHandle prepared =
            authority->prepare({1, 1}, bytes("single owner thread\n"));
        bool rejected = false;
        std::thread other([&] {
            try {
                (void)authority->retain(prepared);
            } catch (const std::logic_error&) {
                rejected = true;
            }
        });
        other.join();
        require(rejected && authority->release(prepared) == 0,
                "mutable preparation state crossed its established owner thread");
    }

    {
        P50ServerEndpoint server(Id128::from_u64(204));
        TestClient client(Id128::from_u64(205));
        require(run_pair(client, server,
                         admit(client, bytes("endpoint owner thread\n")))
                        .client.status == ClientRunStatus::Committed,
                "endpoint thread-owner fixture did not commit");
        bool client_rejected = false;
        bool server_rejected = false;
        std::thread other([&] {
            try {
                (void)client.endpoint.next_rel_seq();
            } catch (const std::logic_error&) {
                client_rejected = true;
            }
            try {
                server.collect_input_garbage();
            } catch (const std::logic_error&) {
                server_rejected = true;
            }
        });
        other.join();
        require(client_rejected && server_rejected,
                "client or server mutable state crossed its established owner thread");
    }

    {
        P50ServerEndpointConfig invalid;
        invalid.owner_limits.max_live_sessions = 0;
        require_throws<std::invalid_argument>(
            [&] {
                P50ServerEndpoint rejected(Id128::from_u64(186), {}, nullptr, nullptr,
                                           invalid);
            },
            "zero aggregate live-session bound was accepted");
        invalid = {};
        invalid.owner_limits.max_decoder_window_bytes =
            (uint64_t{1} << EndpointCaps{}.zstd.max_window_log) - 1;
        require_throws<std::invalid_argument>(
            [&] {
                P50ServerEndpoint rejected(Id128::from_u64(187), {}, nullptr, nullptr,
                                           invalid);
            },
            "decoder-window aggregate bound admitted no complete dialogue");
    }
}

void test_fragmentation_at_every_control_and_body_boundary() {
    const std::vector<uint8_t> input = pseudo_random_bytes(96);
    const ZstdTuEnvelope encoded = encode_zstd_tu(
        HistoryNonce{1}, RelSeq{0}, TuSeq{0}, Digest128{}, input);
    const size_t frame_bytes =
        std::max(4 + encoded.body.size(), size_t{4 + kMandatoryControlFramePayload});
    require(frame_bytes > 8, "fragmentation fixture is too small");
    for (size_t split = 1; split != frame_bytes; ++split) {
        P50ServerEndpoint server(Id128::from_u64(1000 + split));
        TestClient client(Id128::from_u64(2000 + split));
        EndpointIoControl client_control;
        client_control.max_write_fragment = split;
        EndpointIoControl server_control;
        server_control.max_write_fragment = split;
        const PairResult result =
            run_pair(client, server, admit(client, input), client_control, server_control);
        if (result.client.status != ClientRunStatus::Committed ||
            result.server.status != ServerRunStatus::Completed)
            fail("fragmentation failed at split " + std::to_string(split));
    }
}

void test_exact_replay_and_lost_final() {
    {
        ActionTrace actions;
        P50ServerEndpoint server(Id128::from_u64(300), {}, nullptr, &actions);
        TestClient client(Id128::from_u64(301), {}, HistoryNonce{20}, nullptr, &actions);
        EndpointIoControl stop;
        stop.close_after_write = MessageType::TX_BEGIN;
        const std::vector<uint8_t> input = bytes("exact replay input\n");
        const PairResult interrupted = run_pair(client, server, admit(client, input), stop);
        require(interrupted.client.status == ClientRunStatus::Disconnected &&
                    client.has_active_transaction(),
                "uncertain disconnect erased C's active transaction");
        const PairResult replayed = run_pair(client, server);
        require(replayed.client.status == ClientRunStatus::Committed &&
                    replayed.client.reconnect == EndpointReconnectOutcome::ExactMatch,
                "exact replay did not commit on the unchanged route");
        require(copy_input(server, client.c_store_guid()) == input,
                "exact replay materialized different bytes");
        require(std::any_of(actions.records().begin(), actions.records().end(),
                            [](const ActionRecord& record) {
                                return record.action == ActionType::ACTIVE_REPLAYED;
                            }),
                "exact replay omitted ACTIVE_REPLAYED correspondence");
        require_trace(actions, "exact replay trace");
    }

    {
        ActionTrace actions;
        P50ServerEndpoint server(Id128::from_u64(400), {}, nullptr, &actions);
        TestClient client(Id128::from_u64(401), {}, HistoryNonce{30}, nullptr, &actions);
        EndpointIoControl lose_final;
        lose_final.close_before_write = MessageType::TX_COMMIT;
        const std::vector<uint8_t> input = bytes("lost final acknowledgement\n");
        const PairResult interrupted =
            run_pair(client, server, admit(client, input), {}, lose_final);
        require(interrupted.client.status == ClientRunStatus::Disconnected &&
                    client.has_active_transaction(),
                "lost-final window did not retain C active state");
        require(copy_input(server, client.c_store_guid()) == input,
                "F did not retain its completed input before final-message loss");
        const PairResult reconciled = run_pair(client, server);
        require(reconciled.client.status == ClientRunStatus::Committed &&
                    reconciled.client.reconnect ==
                        EndpointReconnectOutcome::LostFinalAcknowledgement &&
                    !client.has_active_transaction(),
                "lost-final reconciliation did not accept the exact retained commit");
        require_trace(actions, "lost-final trace");
    }

    {
        ActionTrace actions;
        size_t publication_attempts = 0;
        const std::vector<uint8_t> input =
            bytes("publication must precede route commit\n");
        P50ServerEndpointConfig config;
        config.input_job_state =
            [&](CStoreGuid published_guid, const TxBegin& begin,
                const TxCommit& commit, std::span<const uint8_t> exact) {
                ++publication_attempts;
                require(published_guid == Id128::from_u64(411) &&
                            commit.history_nonce == begin.history_nonce &&
                            commit.rel_seq == begin.rel_seq &&
                            commit.tu_seq == begin.tu_seq &&
                            commit.transaction_digest == begin.transaction_digest &&
                            commit.raw_digest == begin.raw_digest &&
                            std::ranges::equal(exact, input),
                        "precommit publisher received the wrong exact tuple or bytes");
                if (publication_attempts == 1)
                    throw std::bad_alloc();
                return InputJobState::Open;
            };
        P50ServerEndpoint server(Id128::from_u64(410), {}, nullptr, &actions,
                                 std::move(config));
        TestClient client(Id128::from_u64(411), {}, HistoryNonce{31}, nullptr,
                          &actions);
        const PairResult rejected =
            run_pair(client, server, admit(client, input));
        require(rejected.client.status == ClientRunStatus::TerminalError &&
                    rejected.client.whole_new_attempt &&
                    rejected.server.status == ServerRunStatus::TerminalError &&
                    client.has_active_transaction() &&
                    !copy_input(server, client.c_store_guid()) &&
                    publication_attempts == 1,
                "failed precommit publication advanced or discarded the transaction");
        const PairResult replayed = run_pair(client, server);
        require(replayed.client.status == ClientRunStatus::Committed &&
                    replayed.client.reconnect == EndpointReconnectOutcome::ExactMatch &&
                    copy_input(server, client.c_store_guid()) == input &&
                    publication_attempts == 2,
                "allocation-failure transaction did not replay and publish exactly");
        require(std::any_of(actions.records().begin(), actions.records().end(),
                            [](const ActionRecord& record) {
                                return record.actor == ActorSide::F &&
                                       record.action == ActionType::ACTIVE_REPLAYED;
                            }),
                "allocation-failure replay omitted ACTIVE_REPLAYED");
        require_trace(actions, "precommit publication replay trace");
    }
}

void test_completion_identity_and_store_replacement() {
    {
        ActionTrace actions;
        P50ServerEndpoint server(Id128::from_u64(450), {}, nullptr, &actions);
        TestClient client(Id128::from_u64(451), {}, HistoryNonce{31}, nullptr, &actions);
        EndpointIoControl wrong;
        wrong.wrong_digest_completion = AsyncOperationKind::WriteFragment;
        const std::vector<uint8_t> input = pseudo_random_bytes(2048);
        const PairResult rejected = run_pair(client, server, admit(client, input), wrong);
        require(rejected.client.status == ClientRunStatus::Disconnected &&
                    client.has_active_transaction() &&
                    !copy_input(server, client.c_store_guid()),
                "wrong-digest completion changed endpoint state");
        const PairResult replayed = run_pair(client, server);
        require(replayed.client.status == ClientRunStatus::Committed &&
                    copy_input(server, client.c_store_guid()) == input,
                "wrong-digest completion was not recoverable by exact replay");
        require_trace(actions, "wrong-digest completion trace");
    }

    for (bool alter_raw_digest : {false, true}) {
        P50ServerEndpoint server(Id128::from_u64(alter_raw_digest ? 455 : 453));
        TestClient client(Id128::from_u64(alter_raw_digest ? 456 : 454));
        EndpointIoControl wrong;
        if (alter_raw_digest)
            wrong.wrong_raw_digest_completion = AsyncOperationKind::ReadPayload;
        else
            wrong.wrong_digest_completion = AsyncOperationKind::ReadPayload;
        const std::vector<uint8_t> input = pseudo_random_bytes(3072);
        const PairResult rejected =
            run_pair(client, server, admit(client, input), {}, wrong);
        require(rejected.server.status == ServerRunStatus::Disconnected &&
                    rejected.client.status == ClientRunStatus::Disconnected &&
                    client.has_active_transaction() &&
                    !copy_input(server, client.c_store_guid()),
                alter_raw_digest
                    ? "late raw-digest completion changed endpoint state"
                    : "late transaction-digest completion changed endpoint state");
        const PairResult replayed = run_pair(client, server);
        require(replayed.client.status == ClientRunStatus::Committed &&
                    copy_input(server, client.c_store_guid()) == input,
                alter_raw_digest
                    ? "late raw-digest completion did not permit exact replay"
                    : "late transaction-digest completion did not permit exact replay");
    }

    {
        ActionTrace actions;
        CompletionLog completions;
        const FStoreGuid first_guid = Id128::from_u64(460);
        const FStoreGuid second_guid = Id128::from_u64(461);
        P50ServerEndpoint server(first_guid, {}, &completions, &actions);
        TestClient client(Id128::from_u64(462), {}, HistoryNonce{32}, &completions,
                                 &actions);
        bool reset_done = false;
        EndpointIoControl reset_during_body;
        reset_during_body.before_completion_check = [&](const CompletionStamp& stamp) {
            if (!reset_done && stamp.actor == ActorSide::F && stamp.transaction_bound &&
                stamp.operation == AsyncOperationKind::ReadPayload) {
                reset_done = true;
                server.reset_store(second_guid);
            }
        };
        const std::vector<uint8_t> input = pseudo_random_bytes(4096);
        const PairResult invalidated =
            run_pair(client, server, admit(client, input), {}, reset_during_body);
        require(reset_done && invalidated.server.status == ServerRunStatus::Disconnected &&
                    invalidated.client.status == ClientRunStatus::Disconnected &&
                    client.has_active_transaction() && server.f_store_guid() == second_guid &&
                    !copy_input(server, client.c_store_guid()),
                "F reset did not fence its old in-flight completion");
        const PairResult replaced = run_pair(client, server);
        require(replaced.client.status == ClientRunStatus::Committed &&
                    replaced.client.reconnect == EndpointReconnectOutcome::ColdFStore &&
                    replaced.client.whole_new_attempt &&
                    replaced.server.session_serial > invalidated.server.session_serial &&
                    copy_input(server, client.c_store_guid()) == input,
                "precommit F-incarnation replacement did not preserve exact prepared work");
        require(std::any_of(actions.records().begin(), actions.records().end(),
                            [](const ActionRecord& record) {
                                return record.action == ActionType::F_STORE_INCAR_REPLACED;
                            }),
                "precommit F replacement omitted its explicit action");
        require_trace(actions, "precommit F replacement trace");

        bool saw_old_completion = false;
        bool saw_new_completion = false;
        for (const AsyncCompletion& completion : completions.completions()) {
            if (completion.stamp.actor != ActorSide::F)
                continue;
            if (completion.stamp.f_store_guid == first_guid)
                saw_old_completion = true;
            if (completion.stamp.f_store_guid == second_guid)
                saw_new_completion = true;
        }
        require(saw_old_completion && saw_new_completion,
                "completion log did not retain distinct old/new F identities");
    }

    {
        ActionTrace actions;
        P50ServerEndpoint server(Id128::from_u64(470), {}, nullptr, &actions);
        TestClient client(Id128::from_u64(471), {}, HistoryNonce{33}, nullptr, &actions);
        EndpointIoControl lose_final;
        lose_final.close_before_write = MessageType::TX_COMMIT;
        const std::vector<uint8_t> input = bytes("durable before F reset\n");
        const PairResult durable =
            run_pair(client, server, admit(client, input), {}, lose_final);
        require(durable.client.status == ClientRunStatus::Disconnected &&
                    client.has_active_transaction() &&
                    copy_input(server, client.c_store_guid()) == input,
                "postcommit/pre-ack replacement fixture did not reach its durable window");
        server.reset_store(Id128::from_u64(472));
        const PairResult replaced = run_pair(client, server);
        require(replaced.client.status == ClientRunStatus::Committed &&
                    replaced.client.reconnect == EndpointReconnectOutcome::ColdFStore &&
                    replaced.client.whole_new_attempt &&
                    copy_input(server, client.c_store_guid()) == input,
                "postcommit/pre-ack F replacement did not replay exact prepared work");
        require_trace(actions, "postcommit/pre-ack F replacement trace");
    }
}

void test_same_f_route_reset() {
    {
        ActionTrace actions;
        P50ServerEndpoint server(Id128::from_u64(550), {}, nullptr, &actions);
        TestClient client(Id128::from_u64(551), {}, HistoryNonce{50}, nullptr, &actions);
        require(
            run_pair(client, server, admit(client, bytes("route before reset\n"))).client.status ==
                ClientRunStatus::Committed,
            "route-reset fixture did not establish its first route");
        const ServerRunResult forced =
            force_route_reset(server, client.c_store_guid(), HistoryNonce{999});
        require(forced.status == ServerRunStatus::Disconnected &&
                    forced.c_store_guid == client.c_store_guid(),
                "independent route reset did not close as expected");
        const std::vector<uint8_t> input = bytes("route after reset\n");
        const PairResult repaired = run_pair(client, server, admit(client, input));
        require(repaired.client.status == ClientRunStatus::Committed &&
                    repaired.client.reconnect == EndpointReconnectOutcome::RouteHistoryReset,
                "same-F route mismatch did not take the history-reset outcome");
        require(copy_input(server, client.c_store_guid()) == input,
                "route-reset retry did not materialize exact input");
        require_trace(actions, "route-history-reset trace");
    }
}

void test_reset_ack_equality_and_terminal_result() {
    const FStoreGuid f_guid = Id128::from_u64(570);
    for (const ResetAckMutation mutation :
         {ResetAckMutation::SelectedProtocol, ResetAckMutation::ProfileMask,
          ResetAckMutation::FrameLimit, ResetAckMutation::FillLimit}) {
        TestClient client(Id128::from_u64(571 + static_cast<uint8_t>(mutation)));
        const std::vector<uint8_t> input = bytes("reset acknowledgement equality\n");
        const PreparedTuHandle prepared = admit(client, input);
        const ClientRunResult terminal = run_client_with_raw_peer(
            client, prepared, [&](tcp::acceptor& acceptor) {
                return raw_bad_reset_ack_peer(
                    acceptor, ScriptedSessionState{.f_guid = f_guid}, mutation);
            });
        require(terminal.status == ClientRunStatus::TerminalError &&
                    terminal.whole_new_attempt && client.has_reconciliation_work() &&
                    !client.has_active_transaction(),
                "bad HISTORY_RESET acknowledgement lost queued reconciliation work");

        P50ServerEndpoint good(f_guid);
        const PairResult recovered = run_pair(client, good);
        require(recovered.client.status == ClientRunStatus::Committed &&
                    copy_input(good, client.c_store_guid()) == input,
                "bad HISTORY_RESET acknowledgement changed the queued PreparedTU");
    }

    {
        EndpointCaps exact_cap;
        exact_cap.wire.max_frame_payload = kMandatoryControlFramePayload;
        TestClient client(Id128::from_u64(580), exact_cap);
        const std::vector<uint8_t> input = bytes("bounded peer terminal result\n");
        const PreparedTuHandle prepared = admit(client, input);
        const ClientRunResult terminal = run_client_with_raw_peer(
            client, prepared, [&](tcp::acceptor& acceptor) {
                return raw_bounded_error_peer(acceptor, kMandatoryControlFramePayload);
            });
        require(terminal.status == ClientRunStatus::TerminalError &&
                    terminal.whole_new_attempt && terminal.terminal_error &&
                    encode_payload(Message{*terminal.terminal_error}).size() ==
                        kMandatoryControlFramePayload &&
                    client.has_reconciliation_work(),
                "cap-sized peer ERROR did not use the bounded terminal-result path");
        P50ServerEndpoint good(Id128::from_u64(581), exact_cap);
        require(run_pair(client, good).client.status == ClientRunStatus::Committed &&
                    copy_input(good, client.c_store_guid()) == input,
                "terminal peer ERROR changed the queued PreparedTU");
    }

    {
        TestClient client(Id128::from_u64(585));
        const std::vector<uint8_t> input = bytes("client framing terminal result\n");
        const PreparedTuHandle prepared = admit(client, input);
        const ClientRunResult terminal = run_client_with_raw_peer(
            client, prepared,
            [&](tcp::acceptor& acceptor) { return raw_unknown_frame_peer(acceptor); });
        require(terminal.status == ClientRunStatus::TerminalError &&
                    terminal.whole_new_attempt && terminal.terminal_error &&
                    client.has_reconciliation_work() && !client.has_active_transaction(),
                "client framing failure bypassed the bounded terminal-result path");
        P50ServerEndpoint good(Id128::from_u64(586));
        require(run_pair(client, good).client.status == ClientRunStatus::Committed &&
                    copy_input(good, client.c_store_guid()) == input,
                "client framing failure changed the queued PreparedTU");
    }

    {
        ActionTrace actions;
        P50ServerEndpoint established(f_guid, {}, nullptr, &actions);
        TestClient client(Id128::from_u64(590), {}, HistoryNonce{1}, nullptr, &actions);
        const std::vector<uint8_t> input = bytes("active identity survives route mismatch\n");
        EndpointIoControl stop;
        stop.close_after_write = MessageType::TX_BEGIN;
        require(run_pair(client, established, admit(client, input), stop).client.status ==
                        ClientRunStatus::Disconnected &&
                    client.has_active_transaction(),
                "active terminal fixture did not retain its original transaction");
        const auto initial_position = std::find_if(
            actions.records().begin(), actions.records().end(),
            [](const ActionRecord& record) {
                return record.actor == ActorSide::C && record.action == ActionType::TX_BEGIN;
            });
        require(initial_position != actions.records().end(),
                "active terminal fixture omitted its canonical C TX_BEGIN");
        const ActionRecord initial_identity = *initial_position;
        const RelSeq initial_next_rel = client.endpoint.next_rel_seq();
        const Digest128 initial_state = client.endpoint.state_digest();
        const size_t action_count_before_mismatch = actions.records().size();

        const ClientRunResult terminal = run_client_with_raw_peer(
            client, {}, [&](tcp::acceptor& acceptor) {
                return raw_route_mismatch_peer(
                    acceptor,
                    ScriptedSessionState{.f_guid = f_guid,
                                         .route_present = true,
                                         .history_nonce = HistoryNonce{991},
                                         .next_rel_seq = RelSeq{0},
                                         .state_digest = icecc::digest128("other route")});
            });
        require(terminal.status == ClientRunStatus::TerminalError &&
                    terminal.reconnect == EndpointReconnectOutcome::RouteHistoryReset &&
                    terminal.whole_new_attempt && client.has_active_transaction() &&
                    client.has_reconciliation_work() &&
                    client.endpoint.next_rel_seq() == initial_next_rel &&
                    client.endpoint.state_digest() == initial_state &&
                    actions.records().size() == action_count_before_mismatch,
                "active same-F route mismatch erased reconciliation identity");
        const PairResult recovered = run_pair(client, established);
        require(recovered.client.status == ClientRunStatus::Committed &&
                    recovered.client.reconnect == EndpointReconnectOutcome::ExactMatch &&
                    copy_input(established, client.c_store_guid()) == input,
                "terminal route-ack error changed exact replay identity");
        const auto replay_position = std::find_if(
            actions.records().begin(), actions.records().end(),
            [](const ActionRecord& record) {
                return record.actor == ActorSide::F &&
                       record.action == ActionType::ACTIVE_REPLAYED;
            });
        require(replay_position != actions.records().end() &&
                    replay_position->c_store_guid == initial_identity.c_store_guid &&
                    replay_position->f_store_guid == initial_identity.f_store_guid &&
                    replay_position->history_nonce == initial_identity.history_nonce &&
                    replay_position->rel_seq == initial_identity.rel_seq &&
                    replay_position->tu_seq == initial_identity.tu_seq &&
                    replay_position->transaction_digest == initial_identity.transaction_digest &&
                    replay_position->raw_digest == initial_identity.raw_digest &&
                    replay_position->state_digest == initial_identity.state_digest,
                "same-F mismatch changed the digest-bound active tuple before exact replay");
        require_trace(actions, "same-F active route-mismatch trace");

        EndpointIoControl stop_terminal_report;
        stop_terminal_report.close_before_write = MessageType::ERROR;
        require(run_pair(client, established, admit(client, input), stop).client.status ==
                        ClientRunStatus::Disconnected &&
                    client.has_active_transaction(),
                "terminal-report fixture did not retain a second active transaction");
        const ClientRunResult local_terminal = run_client_with_raw_peer(
            client, {},
            [&](tcp::acceptor& acceptor) {
                return raw_route_mismatch_peer(
                    acceptor,
                    ScriptedSessionState{.f_guid = f_guid,
                                         .route_present = true,
                                         .history_nonce = HistoryNonce{992},
                                         .next_rel_seq = RelSeq{0},
                                         .state_digest = icecc::digest128("third route")},
                    false);
            },
            stop_terminal_report);
        require(local_terminal.status == ClientRunStatus::TerminalError &&
                    local_terminal.whole_new_attempt &&
                    client.has_active_transaction(),
                "failed terminal-report write relabeled a known route mismatch");
    }
}

void test_handshake_binding_and_namespace_rules() {
    {
        P50ServerEndpoint server(Id128::from_u64(610));
        const CStoreGuid c_guid = Id128::from_u64(611);
        const std::vector<uint8_t> input = bytes("pending survives incompatible HELLO\n");
        HelloRaceState coordination;
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> first_server =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<ServerRunResult> second_server =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<void> first_peer = asio::co_spawn(
            context, raw_pending_then_finish(acceptor.local_endpoint(), c_guid, input, coordination),
            asio::use_future);
        std::future<void> incompatible_peer = asio::co_spawn(
            context, raw_incompatible_hello(acceptor.local_endpoint(), c_guid, coordination),
            asio::use_future);
        context.run();
        first_peer.get();
        incompatible_peer.get();
        const ServerRunResult first = first_server.get();
        const ServerRunResult second = second_server.get();
        require(first.status == ServerRunStatus::Completed &&
                    second.status == ServerRunStatus::TerminalError &&
                    copy_input(server, c_guid) == input,
                "incompatible HELLO bound or changed the live namespace");
    }

    {
        P50ServerEndpoint server(Id128::from_u64(612));
        const CStoreGuid c_guid = Id128::from_u64(613);
        const std::vector<uint8_t> input =
            bytes("pending survives invalid compatible candidate\n");
        HelloRaceState coordination;
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> first_server =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<ServerRunResult> second_server =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<void> first_peer = asio::co_spawn(
            context, raw_pending_then_finish(acceptor.local_endpoint(), c_guid, input, coordination),
            asio::use_future);
        std::future<void> invalid_peer = asio::co_spawn(
            context, raw_invalid_first_mutation(acceptor.local_endpoint(), c_guid, coordination),
            asio::use_future);
        context.run();
        first_peer.get();
        invalid_peer.get();
        const ServerRunResult first = first_server.get();
        const ServerRunResult second = second_server.get();
        require(first.status == ServerRunStatus::Completed &&
                    second.status == ServerRunStatus::TerminalError &&
                    server.namespace_count() == 1 &&
                    copy_input(server, c_guid) == input,
                "invalid compatible candidate installed or replaced the live namespace");
    }

    {
        P50ServerEndpoint server(Id128::from_u64(614));
        const CStoreGuid c_guid = Id128::from_u64(615);
        HelloRaceState coordination{.first_pending = true};
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> server_result =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<void> peer_result = asio::co_spawn(
            context, raw_invalid_first_mutation(acceptor.local_endpoint(), c_guid, coordination),
            asio::use_future);
        context.run();
        peer_result.get();
        require(server_result.get().status == ServerRunStatus::TerminalError &&
                    server.namespace_count() == 0 && !copy_input(server, c_guid),
                "cold compatible candidate installed a namespace before validation");
    }

    {
        P50ServerEndpoint server(Id128::from_u64(616));
        const CStoreGuid c_guid = Id128::from_u64(617);
        require(force_route_reset(server, c_guid, HistoryNonce{110}).status ==
                    ServerRunStatus::Disconnected,
                "candidate-revision fixture did not establish its route");
        const std::vector<uint8_t> input =
            bytes("newly activated session invalidates an older snapshot\n");
        CandidateRevisionRace coordination;
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> first_server =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<ServerRunResult> second_server =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<void> stale_peer = asio::co_spawn(
            context, raw_stale_candidate(acceptor.local_endpoint(), c_guid, coordination),
            asio::use_future);
        std::future<void> current_peer = asio::co_spawn(
            context, raw_commit_after_candidate_staged(
                         acceptor.local_endpoint(), c_guid, input, coordination),
            asio::use_future);
        context.run();
        stale_peer.get();
        current_peer.get();
        require(first_server.get().status == ServerRunStatus::Disconnected &&
                    second_server.get().status == ServerRunStatus::Completed &&
                    server.namespace_count() == 1 &&
                    copy_input(server, c_guid) == input,
                "stale candidate replaced state from the newer activated session");
    }

    {
        const FStoreGuid guid = Id128::from_u64(620);
        P50ServerEndpoint established(guid);
        TestClient client(Id128::from_u64(621));
        EndpointIoControl stop;
        stop.close_after_write = MessageType::TX_BEGIN;
        const std::vector<uint8_t> input = bytes("same GUID missing namespace\n");
        const PairResult interrupted =
            run_pair(client, established, admit(client, input), stop);
        require(interrupted.client.status == ClientRunStatus::Disconnected &&
                    client.has_active_transaction(),
                "same-GUID inconsistency fixture lost its active transaction");

        P50ServerEndpoint inconsistent(guid);
        const PairResult refused = run_pair(client, inconsistent);
        require(refused.client.status == ClientRunStatus::TerminalError &&
                    client.has_active_transaction() &&
                    !copy_input(inconsistent, client.c_store_guid()),
                "same GUID with an absent established namespace was treated as cold");

        const PairResult recovered = run_pair(client, established);
        require(recovered.client.status == ClientRunStatus::Committed &&
                    copy_input(established, client.c_store_guid()) == input,
                "same-GUID inconsistency changed the retained retry identity");
    }

    {
        P50ServerEndpoint server(Id128::from_u64(630));
        const CStoreGuid c_guid = Id128::from_u64(631);
        require(force_route_reset(server, c_guid, HistoryNonce{500}).status ==
                    ServerRunStatus::Disconnected,
                "monotonic nonce fixture did not establish its route");
        require(force_rejected_route_reset(server, c_guid, HistoryNonce{500}),
                "reused HISTORY_NONCE was accepted");
        require(force_rejected_route_reset(server, c_guid, HistoryNonce{499}),
                "decreasing HISTORY_NONCE was accepted");
        require(force_route_reset(server, c_guid, HistoryNonce{501}).status ==
                    ServerRunStatus::Disconnected,
                "strictly increasing HISTORY_NONCE was rejected");
    }
}


// Port of the transplant lane's unique mask-law assertions onto the accepted
// production endpoint (bigoracle S1 transplant ruling, bounded convergence):
// a TX_BEGIN whose profile is intrinsically valid on the wire (GRZ carries
// its canonical NotApplicable root mode) but outside the session's negotiated
// mask must be rejected by the endpoint's own negotiated-mask law -- bound by
// EXACT detail text, because the ZSTD_TU shape validator behind it rejects
// the same frame with a different text ("transaction is not a ZSTD_TU
// profile"), and std::invalid_argument IS-A std::logic_error so exception
// classes cannot separate the layers. The rejection must not move the route:
// a second connection then completes a transaction at the untouched cursor.
asio::awaitable<void> raw_unnegotiated_begin_rejected(tcp::endpoint remote,
                                                      CStoreGuid c_guid,
                                                      std::span<const uint8_t> input) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    RawRoute open = co_await raw_open(socket, remote, c_guid);
    if ((open.state.negotiated_profiles & profile_bit(ProfileId::GRZ)) != 0)
        throw std::logic_error("fixture requires GRZ outside the negotiated mask");
    SessionState route = co_await raw_reset(socket, open, HistoryNonce{771});
    const ZstdTuEnvelope prepared = encode_zstd_tu(
        HistoryNonce{1}, RelSeq{0}, TuSeq{41}, Digest128{}, input);
    TxBegin begin = make_begin(prepared, route.history_nonce, route.state_digest,
                               route.next_rel_seq);
    begin.profile = ProfileId::GRZ;
    co_await raw_write(socket, begin);
    // No further traffic: a server that wrongly admits this begin sees EOF
    // with an open transaction and fails through a DIFFERENT detail text, so
    // the exact-text assertion below stays the discriminator without a hang.
    boost::system::error_code shutdown_error;
    socket.shutdown(tcp::socket::shutdown_send, shutdown_error);
    const Frame terminal = co_await raw_read(socket, route.limits.max_frame_payload);
    if (terminal.type != MessageType::ERROR)
        throw std::logic_error("unnegotiated TX_BEGIN did not receive terminal ERROR");
    const ErrorMessage rejection = raw_decode<ErrorMessage>(terminal);
    if (rejection.detail != "TX_BEGIN profile was not negotiated")
        throw std::logic_error(
            "unnegotiated TX_BEGIN was not rejected by the endpoint mask law: " +
            rejection.detail);
    boost::system::error_code ignored;
    socket.close(ignored);
    co_return;
}

asio::awaitable<void> raw_complete_at_preserved_cursor(tcp::endpoint remote,
                                                       CStoreGuid c_guid,
                                                       std::span<const uint8_t> input) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    RawRoute open = co_await raw_open(socket, remote, c_guid);
    if (!open.state.route_present || open.state.history_nonce != HistoryNonce{771} ||
        open.state.next_rel_seq != RelSeq{0})
        throw std::logic_error("rejected unnegotiated TX_BEGIN moved the endpoint route");
    const ZstdTuEnvelope prepared = encode_zstd_tu(
        HistoryNonce{1}, RelSeq{0}, TuSeq{42}, Digest128{}, input);
    const TxBegin begin = make_begin(prepared, open.state.history_nonce,
                                     open.state.state_digest, open.state.next_rel_seq);
    co_await raw_write(socket, begin);
    Message body_message = BodyMessage{prepared.body};
    co_await raw_write(socket, std::move(body_message));
    const TxCommit commit = raw_decode<TxCommit>(
        co_await raw_read(socket, open.state.limits.max_frame_payload));
    if (commit.transaction_digest != begin.transaction_digest ||
        commit.raw_digest != begin.raw_digest)
        throw std::logic_error("preserved-cursor completion received a different TX_COMMIT");
    boost::system::error_code ignored;
    socket.close(ignored);
    co_return;
}

void test_unnegotiated_begin_rejected_and_route_preserved() {
    P50ServerEndpoint server(Id128::from_u64(770));
    const CStoreGuid c_guid = Id128::from_u64(771);
    const std::vector<uint8_t> input = bytes("route survives unnegotiated TX_BEGIN\n");
    {
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> serving =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<void> peer = asio::co_spawn(
            context,
            raw_unnegotiated_begin_rejected(acceptor.local_endpoint(), c_guid, input),
            asio::use_future);
        context.run();
        peer.get();
        require(serving.get().status == ServerRunStatus::TerminalError,
                "unnegotiated TX_BEGIN left the server run non-terminal");
    }
    {
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> serving =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<void> peer = asio::co_spawn(
            context,
            raw_complete_at_preserved_cursor(acceptor.local_endpoint(), c_guid, input),
            asio::use_future);
        context.run();
        peer.get();
        require(serving.get().status == ServerRunStatus::Completed &&
                    copy_input(server, c_guid) == input,
                "post-rejection completion at the preserved cursor failed");
    }
}

void test_reserved_zero_endpoint_values() {
    {
        P50ServerEndpoint server(Id128::from_u64(628));
        asio::io_context context;
        tcp::acceptor acceptor(context,
                               {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> server_result =
            asio::co_spawn(context, server.accept_one(acceptor),
                           asio::use_future);
        std::future<void> peer_result = asio::co_spawn(
            context,
            raw_zero_c_store_hello(acceptor.local_endpoint()),
            asio::use_future);
        context.run();
        peer_result.get();
        require(server_result.get().status == ServerRunStatus::TerminalError &&
                    server.namespace_count() == 0,
                "zero C_STORE_GUID left an F namespace or route");

        TestClient recovered(Id128::from_u64(629));
        const std::vector<uint8_t> input =
            bytes("valid relationship after reserved zero C GUID\n");
        require(run_pair(recovered, server, admit(recovered, input))
                            .client.status == ClientRunStatus::Committed &&
                    copy_input(server, recovered.c_store_guid()) == input,
                "zero C_STORE_GUID changed the subsequent cold relationship");
    }

    {
        P50ServerEndpoint server(Id128::from_u64(632));
        const CStoreGuid c_guid = Id128::from_u64(633);
        asio::io_context context;
        tcp::acceptor acceptor(context,
                               {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> server_result =
            asio::co_spawn(context, server.accept_one(acceptor),
                           asio::use_future);
        std::future<void> peer_result = asio::co_spawn(
            context,
            raw_zero_history_reset(acceptor.local_endpoint(), c_guid),
            asio::use_future);
        context.run();
        peer_result.get();
        require(server_result.get().status == ServerRunStatus::TerminalError &&
                    server.namespace_count() == 0 &&
                    !copy_input(server, c_guid),
                "zero HISTORY_NONCE left an F namespace or route");

        TestClient recovered(c_guid);
        const std::vector<uint8_t> input =
            bytes("valid reset after reserved zero nonce\n");
        require(run_pair(recovered, server, admit(recovered, input))
                            .client.status == ClientRunStatus::Committed &&
                    copy_input(server, c_guid) == input,
                "zero HISTORY_NONCE changed the subsequent cold relationship");
    }

    {
        TestClient client(Id128::from_u64(634));
        const std::vector<uint8_t> input =
            bytes("reserved zero F_STORE_GUID\n");
        const ClientRunResult terminal = run_client_with_raw_peer(
            client, admit(client, input), [](tcp::acceptor& acceptor) {
                return raw_zero_f_store_state_peer(acceptor);
            });
        require(terminal.status == ClientRunStatus::TerminalError &&
                    terminal.whole_new_attempt &&
                    client.has_reconciliation_work() &&
                    !client.has_active_transaction() &&
                    !client.endpoint.f_store_guid() &&
                    client.endpoint.next_rel_seq() == RelSeq{0} &&
                    client.endpoint.state_digest() == Digest128{},
                "zero F_STORE_GUID left C relationship state");
        P50ServerEndpoint good(Id128::from_u64(635));
        require(run_pair(client, good).client.status ==
                        ClientRunStatus::Committed &&
                    copy_input(good, client.c_store_guid()) == input,
                "zero F_STORE_GUID changed queued retry work");
    }

    {
        TestClient client(Id128::from_u64(636));
        const std::vector<uint8_t> input = bytes("reserved zero ERROR code\n");
        const ClientRunResult terminal = run_client_with_raw_peer(
            client, admit(client, input), [](tcp::acceptor& acceptor) {
                return raw_zero_error_code_peer(acceptor);
            });
        require(terminal.status == ClientRunStatus::TerminalError &&
                    terminal.whole_new_attempt && terminal.terminal_error &&
                    terminal.terminal_error->code != 0 &&
                    client.has_reconciliation_work() &&
                    !client.has_active_transaction() &&
                    !client.endpoint.f_store_guid() &&
                    client.endpoint.next_rel_seq() == RelSeq{0} &&
                    client.endpoint.state_digest() == Digest128{},
                "zero ERROR code left C relationship state");
        P50ServerEndpoint good(Id128::from_u64(637));
        require(run_pair(client, good).client.status ==
                        ClientRunStatus::Committed &&
                    copy_input(good, client.c_store_guid()) == input,
                "zero ERROR code changed queued retry work");
    }
}

void test_interrupted_begin_identity() {
    ActionTrace actions;
    P50ServerEndpoint server(Id128::from_u64(640), {}, nullptr, &actions);
    const CStoreGuid c_guid = Id128::from_u64(641);
    const std::vector<uint8_t> input = pseudo_random_bytes(8192);

    InterruptedRawTu interrupted;
    {
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> server_result =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<InterruptedRawTu> peer_result = asio::co_spawn(
            context,
            raw_partial_body_disconnect(acceptor.local_endpoint(), c_guid, input),
            asio::use_future);
        context.run();
        interrupted = peer_result.get();
        require(server_result.get().status == ServerRunStatus::Disconnected &&
                    !copy_input(server, c_guid),
                "partial BODY disconnect materialized or committed input");
    }

    {
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> server_result =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<void> peer_result = asio::co_spawn(
            context,
            raw_different_begin_rejected(acceptor.local_endpoint(), c_guid,
                                         interrupted.begin),
            asio::use_future);
        context.run();
        peer_result.get();
        require(server_result.get().status == ServerRunStatus::TerminalError &&
                    !copy_input(server, c_guid),
                "different begin at the interrupted cursor replaced retained identity");
    }

    {
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> server_result =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<void> peer_result = asio::co_spawn(
            context,
            raw_exact_whole_tu_replay(acceptor.local_endpoint(), c_guid, interrupted),
            asio::use_future);
        context.run();
        peer_result.get();
        require(server_result.get().status == ServerRunStatus::Completed &&
                    copy_input(server, c_guid) == input,
                "exact whole-TU replay did not commit after partial BODY disconnect");
    }

    require(std::any_of(actions.records().begin(), actions.records().end(),
                        [&](const ActionRecord& record) {
                            return record.actor == ActorSide::F &&
                                   record.action == ActionType::ACTIVE_REPLAYED &&
                                   record.transaction_digest ==
                                       interrupted.begin.transaction_digest;
                        }),
            "exact whole-TU replay omitted the retained transaction identity");
}

void test_terminal_body_failure_identity() {
    const std::vector<uint8_t> input = pseudo_random_bytes(8192);
    uint64_t identity = 650;
    for (const auto& [scenario, label] :
         std::array{std::pair{TerminalBodyFailure::ZeroProgress,
                              "zero-progress BODY"},
                    std::pair{TerminalBodyFailure::Excess,
                              "excess BODY"},
                    std::pair{TerminalBodyFailure::ComponentDigest,
                              "component digest"}}) {
        const FStoreGuid f_guid = Id128::from_u64(identity++);
        CompletionLog completions;
        P50ServerEndpoint server(f_guid, {}, &completions);
        const CStoreGuid c_guid = Id128::from_u64(identity++);
        auto failure = force_terminal_body_failure(server, c_guid, input, scenario);
        InterruptedRawTu interrupted = std::move(failure.first);
        ServerRunResult terminal = std::move(failure.second);
        require(terminal.status == ServerRunStatus::TerminalError &&
                    !copy_input(server, c_guid),
                std::string(label) +
                    " failure materialized or committed input");
        require(std::any_of(
                    completions.completions().begin(),
                    completions.completions().end(),
                    [&](const AsyncCompletion& completion) {
                        const CompletionStamp& stamp = completion.stamp;
                        return stamp.actor == ActorSide::F &&
                               stamp.operation == AsyncOperationKind::WriteFragment &&
                               stamp.c_store_guid == c_guid &&
                               stamp.f_store_guid == f_guid &&
                               stamp.session_serial == terminal.session_serial &&
                               stamp.history_nonce == interrupted.begin.history_nonce &&
                               stamp.rel_seq == interrupted.begin.rel_seq &&
                               stamp.tu_seq == interrupted.begin.tu_seq &&
                               stamp.transaction_digest ==
                                   interrupted.begin.transaction_digest &&
                               stamp.raw_digest == interrupted.begin.raw_digest &&
                               stamp.transaction_bound;
                    }),
                std::string(label) +
                    " terminal completion lost the installed operation identity");
        require(force_different_begin_rejected(server, c_guid,
                                               interrupted.begin)
                        .status == ServerRunStatus::TerminalError &&
                    !copy_input(server, c_guid),
                std::string(label) +
                    " failure did not retain the exact TX_BEGIN identity");
        require(force_exact_whole_tu_replay(server, c_guid, interrupted, true)
                            .status == ServerRunStatus::Completed &&
                    copy_input(server, c_guid) == input,
                std::string(label) +
                    " failure did not accept exact whole-TU replay");
    }

    {
        P50ServerEndpoint server(Id128::from_u64(identity++));
        const CStoreGuid c_guid = Id128::from_u64(identity++);
        const auto [interrupted, terminal] = force_terminal_body_failure(
            server, c_guid, input, TerminalBodyFailure::ZeroProgress, false);
        require(terminal.status == ServerRunStatus::TerminalError &&
                    !copy_input(server, c_guid),
                "lost terminal report changed the interrupted transaction");
        require(force_exact_whole_tu_replay(server, c_guid, interrupted, true)
                            .status == ServerRunStatus::Completed &&
                    copy_input(server, c_guid) == input,
                "lost terminal report did not permit exact whole-TU replay");
    }

    for (const auto& [scenario, label] :
         std::array{std::pair{TerminalBodyFailure::ZstdDecode,
                              "Zstd decode"},
                    std::pair{TerminalBodyFailure::RawDigest,
                              "raw digest"}}) {
        P50ServerEndpoint server(Id128::from_u64(identity++));
        const CStoreGuid c_guid = Id128::from_u64(identity++);
        const auto [interrupted, terminal] = force_terminal_body_failure(
            server, c_guid, input, scenario);
        require(terminal.status == ServerRunStatus::TerminalError &&
                    !copy_input(server, c_guid),
                std::string(label) +
                    " failure materialized or committed input");
        require(force_different_begin_rejected(server, c_guid,
                                               interrupted.begin)
                        .status == ServerRunStatus::TerminalError,
                std::string(label) +
                    " failure accepted a different TX_BEGIN at the retained cursor");
        require(force_exact_whole_tu_replay(server, c_guid, interrupted, false)
                            .status == ServerRunStatus::TerminalError &&
                    !copy_input(server, c_guid),
                std::string(label) +
                    " failure did not retain its deterministic exact identity");
    }
}

void test_disconnect_at_each_message_boundary() {
    const std::array<MessageType, 4> client_messages{
        MessageType::SESSION_HELLO, MessageType::HISTORY_RESET, MessageType::TX_BEGIN,
        MessageType::BODY};
    for (size_t index = 0; index != client_messages.size(); ++index) {
        P50ServerEndpoint server(Id128::from_u64(700 + index));
        TestClient client(Id128::from_u64(710 + index));
        EndpointIoControl stop;
        stop.close_after_write = client_messages[index];
        const std::vector<uint8_t> input = bytes("boundary disconnect\n");
        const PairResult result = run_pair(client, server, admit(client, input), stop);
        require(result.client.status == ClientRunStatus::Disconnected,
                "client message-boundary close did not stop the dialogue");
        if (client_messages[index] == MessageType::TX_BEGIN ||
            client_messages[index] == MessageType::BODY)
            require(client.has_active_transaction(),
                    "uncertain client boundary erased active identity");
        const PairResult recovered = run_pair(client, server);
        require(recovered.client.status == ClientRunStatus::Committed &&
                    copy_input(server, client.c_store_guid()) == input,
                "client message-boundary reconnect did not commit exact input");
    }

    const std::array<MessageType, 1> server_messages{MessageType::SESSION_STATE};
    for (size_t index = 0; index != server_messages.size(); ++index) {
        P50ServerEndpoint server(Id128::from_u64(720 + index));
        TestClient client(Id128::from_u64(730 + index));
        EndpointIoControl stop;
        stop.close_after_write = server_messages[index];
        const std::vector<uint8_t> input = bytes("server boundary\n");
        const PairResult result = run_pair(client, server, admit(client, input), {}, stop);
        require(result.client.status == ClientRunStatus::Disconnected,
                "server message-boundary close did not stop the dialogue");
        const PairResult recovered = run_pair(client, server);
        require(recovered.client.status == ClientRunStatus::Committed &&
                    copy_input(server, client.c_store_guid()) == input,
                "server message-boundary reconnect did not commit exact input");
    }

    {
        P50ServerEndpoint server(Id128::from_u64(740));
        TestClient client(Id128::from_u64(741));
        EndpointIoControl stop;
        stop.close_after_write = MessageType::TX_COMMIT;
        const std::vector<uint8_t> input = bytes("close after final commit frame\n");
        const PairResult result = run_pair(client, server, admit(client, input), {}, stop);
        require(result.client.status == ClientRunStatus::Committed &&
                    copy_input(server, client.c_store_guid()) == input,
                "close after the complete TX_COMMIT changed the committed result");
    }
}

void test_component_and_allocation_caps() {
    EndpointCaps ordinary;
    {
        const auto [peer, server] = run_raw_server_case(ordinary, RawCase::ShortBody, 800);
        require(!peer.received_error && server.status == ServerRunStatus::Disconnected,
                "short component was allowed to materialize or commit");
    }
    {
        const auto [peer, server] = run_raw_server_case(ordinary, RawCase::ExcessBody, 801);
        require(peer.received_error && peer.closed_after_error &&
                    server.status == ServerRunStatus::TerminalError,
                "excess component did not terminate the message session");
    }
    for (const auto& [scenario, name, id] :
         std::array{std::tuple{RawCase::EmptyBodyProgress, "zero-progress BODY", uint64_t{805}},
                    std::tuple{RawCase::ComponentDigest, "component digest", uint64_t{806}},
                    std::tuple{RawCase::TransactionDigest, "transaction digest", uint64_t{807}},
                    std::tuple{RawCase::RawDigest, "raw digest", uint64_t{808}},
                    std::tuple{RawCase::WrongProfile, "profile", uint64_t{809}},
                    std::tuple{RawCase::WrongRoot, "root mode", uint64_t{810}},
                    std::tuple{RawCase::WrongEncoding, "encoding", uint64_t{811}},
                    std::tuple{RawCase::FrameContentSize, "zstd frame size", uint64_t{812}},
                    std::tuple{RawCase::TrailingByte, "trailing zstd byte", uint64_t{813}},
                    std::tuple{RawCase::AppendedEmptyFrame, "appended empty zstd frame",
                               uint64_t{814}},
                    std::tuple{RawCase::AppendedNonemptyFrame, "appended nonempty zstd frame",
                               uint64_t{815}}}) {
        const auto [peer, server] = run_raw_server_case(ordinary, scenario, id);
        require(peer.received_error && peer.closed_after_error &&
                    server.status == ServerRunStatus::TerminalError,
                std::string("invalid ") + name + " did not close terminally");
    }

    EndpointCaps tight;
    tight.zstd.max_encoded_body_bytes = 32;
    tight.zstd.max_raw_bytes = 128;
    {
        const auto [peer, server] = run_raw_server_case(tight, RawCase::DescriptorCap, 802);
        require(peer.received_error && peer.closed_after_error &&
                    server.status == ServerRunStatus::TerminalError,
                "encoded-component descriptor cap was not enforced before data");
    }
    {
        const auto [peer, server] = run_raw_server_case(tight, RawCase::DecodedCap, 803);
        require(peer.received_error && peer.closed_after_error &&
                    server.status == ServerRunStatus::TerminalError,
                "decoded-input cap was not enforced before expansion");
    }

    EndpointCaps frame_tight;
    frame_tight.wire.max_frame_payload = kMandatoryControlFramePayload;
    {
        const auto [peer, server] = run_raw_server_case(frame_tight, RawCase::FrameCap, 804);
        require(peer.received_error && peer.closed_after_error &&
                    peer.error_payload_bytes <= kMandatoryControlFramePayload &&
                    server.status == ServerRunStatus::TerminalError,
                "declared frame cap did not reject before payload allocation");
    }


    EndpointCaps below_control;
    below_control.wire.max_frame_payload = kMandatoryControlFramePayload - 1;
    require_throws<std::invalid_argument>(
        [&] { TestClient rejected(Id128::from_u64(820), below_control); },
        "151-byte client frame cap was accepted");
    require_throws<std::invalid_argument>(
        [&] { P50ServerEndpoint rejected(Id128::from_u64(821), below_control); },
        "151-byte server frame cap was accepted");

    EndpointCaps exact_control;
    exact_control.wire.max_frame_payload = kMandatoryControlFramePayload;
    for (bool client_is_tight : {false, true}) {
        const EndpointCaps client_caps = client_is_tight ? exact_control : ordinary;
        const EndpointCaps server_caps = client_is_tight ? ordinary : exact_control;
        TestClient client(Id128::from_u64(830 + client_is_tight), client_caps);
        P50ServerEndpoint server(Id128::from_u64(840 + client_is_tight), server_caps);
        const std::vector<uint8_t> input = pseudo_random_bytes(4096);
        const PairResult result = run_pair(client, server, admit(client, input));
        require(result.client.status == ClientRunStatus::Committed &&
                    copy_input(server, client.c_store_guid()) == input,
                "asymmetric exact-152-byte negotiation did not complete");
    }
}

void test_two_client_one_server_isolation() {
    ActionTrace actions;
    CompletionLog completions;
    P50ServerEndpoint server(Id128::from_u64(860), {}, &completions, &actions);
    TestClient first(Id128::from_u64(861), {}, HistoryNonce{201},
                     &completions, &actions);
    TestClient second(Id128::from_u64(862), {}, HistoryNonce{301},
                      &completions, &actions);
    const std::vector<uint8_t> first_input =
        bytes("C-one exact input on shared F\n");
    const std::vector<uint8_t> second_input =
        bytes("C-two independent exact input on shared F\n");
    const PreparedTuHandle first_prepared = admit(first, first_input);
    const PreparedTuHandle second_prepared = admit(second, second_input);

    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    std::future<ServerRunResult> first_server =
        asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
    std::future<ServerRunResult> second_server =
        asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
    std::future<ClientRunResult> first_client = asio::co_spawn(
        context, first.endpoint.run(acceptor.local_endpoint(), first_prepared),
        asio::use_future);
    std::future<ClientRunResult> second_client = asio::co_spawn(
        context, second.endpoint.run(acceptor.local_endpoint(), second_prepared),
        asio::use_future);
    context.run();

    require(first_client.get().status == ClientRunStatus::Committed &&
                second_client.get().status == ClientRunStatus::Committed &&
                first_server.get().status == ServerRunStatus::Completed &&
                second_server.get().status == ServerRunStatus::Completed &&
                server.namespace_count() == 2 &&
                copy_input(server, first.c_store_guid()) == first_input &&
                copy_input(server, second.c_store_guid()) == second_input &&
                first.endpoint.next_rel_seq() == RelSeq{1} &&
                second.endpoint.next_rel_seq() == RelSeq{1},
            "concurrent C2F1 dialogues crossed namespace or route state");

    const std::vector<uint8_t> first_followup =
        bytes("C-one advances without changing C-two\n");
    const PairResult advanced =
        run_pair(first, server, admit(first, first_followup));
    require(advanced.client.status == ClientRunStatus::Committed &&
                server.namespace_count() == 2 &&
                copy_input(server, first.c_store_guid()) == first_followup &&
                copy_input(server, second.c_store_guid()) == second_input &&
                first.endpoint.next_rel_seq() == RelSeq{2} &&
                second.endpoint.next_rel_seq() == RelSeq{1},
            "one C route advance changed the other C namespace");

    bool first_f_completion = false;
    bool second_f_completion = false;
    for (const AsyncCompletion& completion : completions.completions()) {
        if (completion.stamp.actor != ActorSide::F)
            continue;
        first_f_completion = first_f_completion ||
                             completion.stamp.c_store_guid == first.c_store_guid();
        second_f_completion = second_f_completion ||
                              completion.stamp.c_store_guid == second.c_store_guid();
    }
    require(first_f_completion && second_f_completion,
            "C2F1 completion identities did not retain both C namespaces");
    require_trace(actions, "C2F1 endpoint-isolation trace");
}

void report_zstd1_metrics(bool enforce_performance_floor) {
    constexpr size_t input_bytes = size_t{16} << 20;
    constexpr unsigned iterations = 4;
    std::vector<uint8_t> input(input_bytes);
    static constexpr std::string_view line =
        "template<class T> inline T p50_value(T value) { return value + 1; }\n";
    for (size_t offset = 0; offset != input.size();) {
        const size_t count = std::min(line.size(), input.size() - offset);
        std::copy_n(line.begin(), count, input.begin() + offset);
        offset += count;
    }
    for (size_t offset = 4096; offset < input.size(); offset += 4096)
        input[offset] ^= static_cast<uint8_t>(offset >> 12);

    ZstdTuCodec codec(1);
    ZstdTuEnvelope prepared;
    const auto encode_start = std::chrono::steady_clock::now();
    for (unsigned iteration = 0; iteration != iterations; ++iteration)
        prepared = codec.encode(HistoryNonce{1}, RelSeq{0}, TuSeq{iteration}, Digest128{},
                                input, {uint64_t{64} << 20, uint64_t{64} << 20});
    const auto encode_stop = std::chrono::steady_clock::now();
    TxBegin begin = make_begin(prepared, HistoryNonce{1},
                               initial_route_digest(Id128::from_u64(1), HistoryNonce{1}));
    std::vector<uint8_t> decoded;
    const auto decode_start = std::chrono::steady_clock::now();
    for (unsigned iteration = 0; iteration != iterations; ++iteration)
        decoded = codec.decode(begin, prepared.body,
                               {uint64_t{64} << 20, uint64_t{64} << 20});
    const auto decode_stop = std::chrono::steady_clock::now();
    require(decoded == input, "Zstd1 metric loop did not decode exactly");

    const double encoded_seconds =
        std::chrono::duration<double>(encode_stop - encode_start).count();
    const double decoded_seconds =
        std::chrono::duration<double>(decode_stop - decode_start).count();
    const double total_gb = static_cast<double>(input.size()) * iterations / 1'000'000'000.0;
    const double encode_gbps = total_gb / encoded_seconds;
    const double decode_gbps = total_gb / decoded_seconds;
    std::cout << "p50_endpoint_test: zstd1 raw_bytes=" << input.size()
              << " compressed_bytes=" << prepared.body.size() << " encode_GBps=" << encode_gbps
              << " decode_GBps=" << decode_gbps << '\n';
    if (enforce_performance_floor)
        require(encode_gbps >= 0.5 && decode_gbps >= 0.5,
                "quietbox Zstd1 throughput is below 0.5 GB/s");
}

} // namespace


// ===== same_commit negative coverage: ordinary commit path =====
// local-oracle ruling (#16, 2026-08-23T16:54Z): a well-formed fake F must not be able to
// close C's active transaction with a TxCommit differing from the live TxBegin in any of the
// six identity fields.  Each mismatch must be rejected exactly, leave the active transaction
// unchanged, and permit a later exact retry to commit.  The baseline (exact commit) proves the
// fake F speaks the protocol; without it a broken fake would fake-pass the matrix.

enum class CommitMismatchField {
    None,
    HistoryNonce,
    RelSeq,
    TuSeq,
    TransactionDigest,
    RawDigest,
    PostStateDigest,
};

TxCommit correct_commit_for(const TxBegin& begin) {
    TxCommit commit;
    commit.history_nonce = begin.history_nonce;
    commit.rel_seq = begin.rel_seq;
    commit.tu_seq = begin.tu_seq;
    commit.transaction_digest = begin.transaction_digest;
    commit.raw_digest = begin.raw_digest;
    commit.post_state_digest =
        compute_post_state_digest(begin.pre_state_digest, begin.history_nonce, begin.rel_seq,
                                  begin.tu_seq, begin.transaction_digest);
    return commit;
}

void mutate_commit(TxCommit& commit, CommitMismatchField field) {
    switch (field) {
    case CommitMismatchField::None:
        break;
    case CommitMismatchField::HistoryNonce:
        commit.history_nonce.value += 1;
        break;
    case CommitMismatchField::RelSeq:
        commit.rel_seq.value += 1;
        break;
    case CommitMismatchField::TuSeq:
        commit.tu_seq.value += 1;
        break;
    case CommitMismatchField::TransactionDigest:
        commit.transaction_digest.bytes[0] ^= 0x80;
        break;
    case CommitMismatchField::RawDigest:
        commit.raw_digest.bytes[0] ^= 0x80;
        break;
    case CommitMismatchField::PostStateDigest:
        commit.post_state_digest.bytes[0] ^= 0x80;
        break;
    }
}

// A raw fake F: accepts one real-client connection, performs the exact fresh-route dance
// (HELLO -> fresh SESSION_STATE -> HISTORY_RESET -> bound acknowledgement), consumes the
// TX_BEGIN and BODY components, then answers with a commit derived from the received begin
// with exactly one field mutated (or none for the baseline), and waits for the peer to close.
asio::awaitable<void> raw_f_serve_and_commit(tcp::acceptor& acceptor, FStoreGuid f_guid,
                                             CommitMismatchField field, TxBegin& out_begin,
                                             std::vector<uint8_t>* out_body = nullptr) {
    tcp::socket socket = co_await acceptor.async_accept(asio::use_awaitable);
    const SessionHello hello =
        raw_decode<SessionHello>(co_await raw_read(socket, kInitialMaxFramePayload));
    const SessionSelection selection =
        negotiate_session(hello, kProtocolVersion, kProtocolVersion, kKnownProfileMask);
    const uint32_t cap = selection.limits.max_frame_payload;
    SessionState fresh;
    fresh.selected_protocol = selection.protocol;
    fresh.negotiated_profiles = selection.negotiated_profiles;
    fresh.limits = selection.limits;
    fresh.f_store_guid = f_guid;
    co_await raw_write(socket, Message{fresh});
    Frame next = co_await raw_read(socket, cap);
    TxBegin& begin = out_begin;
    if (next.type == MessageType::HISTORY_RESET) {
        const HistoryReset reset = raw_decode<HistoryReset>(next);
        SessionState ack = fresh;
        ack.namespace_present = true;
        ack.route_present = true;
        ack.history_nonce = reset.history_nonce;
        ack.next_rel_seq = RelSeq{0};
        ack.state_digest = reset.initial_state_digest;
        co_await raw_write(socket, Message{ack});
        begin = raw_decode<TxBegin>(co_await raw_read(socket, cap));
    } else {
        begin = raw_decode<TxBegin>(next);
    }
    uint64_t body_bytes = 0;
    while (body_bytes < begin.body.encoded_bytes) {
        const BodyMessage body = raw_decode<BodyMessage>(co_await raw_read(socket, cap));
        if (body.bytes.empty())
            break;
        body_bytes += body.bytes.size();
        if (out_body)
            out_body->insert(out_body->end(), body.bytes.begin(), body.bytes.end());
    }
    TxCommit commit = correct_commit_for(begin);
    mutate_commit(commit, field);
    co_await raw_write(socket, Message{commit});
    co_await raw_wait_for_close(socket);
    co_return;
}


// Ordinary same-F exact retry (bigoracle endpoint-HOLD shape B): after a mismatch refusal the
// SAME F presents the unchanged retained route; the client must replay the IDENTICAL TxBegin
// and BODY and commit with reconnect == ExactMatch.  The fake itself throws if C resets,
// rebinds, or reconstructs any begin/body identity.
asio::awaitable<void> raw_f_exact_retry(tcp::acceptor& acceptor, FStoreGuid same_f_guid,
                                        TxBegin captured_begin,
                                        std::vector<uint8_t> captured_body) {
    tcp::socket socket = co_await acceptor.async_accept(asio::use_awaitable);
    const SessionHello hello =
        raw_decode<SessionHello>(co_await raw_read(socket, kInitialMaxFramePayload));
    const SessionSelection selection =
        negotiate_session(hello, kProtocolVersion, kProtocolVersion, kKnownProfileMask);
    SessionState peer;
    peer.selected_protocol = selection.protocol;
    peer.negotiated_profiles = selection.negotiated_profiles;
    peer.limits = selection.limits;
    peer.f_store_guid = same_f_guid;
    peer.namespace_present = true;
    peer.route_present = true;
    peer.history_nonce = captured_begin.history_nonce;
    peer.next_rel_seq = captured_begin.rel_seq;
    peer.state_digest = captured_begin.pre_state_digest;
    co_await raw_write(socket, Message{peer});
    const TxBegin replayed =
        raw_decode<TxBegin>(co_await raw_read(socket, peer.limits.max_frame_payload));
    if (replayed != captured_begin)
        throw std::logic_error("ordinary retry changed retained TxBegin identity");
    std::vector<uint8_t> replay_body;
    while (replay_body.size() < replayed.body.encoded_bytes) {
        BodyMessage part = raw_decode<BodyMessage>(
            co_await raw_read(socket, peer.limits.max_frame_payload));
        if (part.bytes.empty())
            break;
        replay_body.insert(replay_body.end(), part.bytes.begin(), part.bytes.end());
    }
    if (replay_body != captured_body)
        throw std::logic_error("ordinary retry changed retained BODY bytes");
    co_await raw_write(socket, Message{correct_commit_for(captured_begin)});
    co_await raw_wait_for_close(socket);
    co_return;
}

void test_commit_identity_negative_matrix() {
    const std::array cases{
        std::pair{CommitMismatchField::HistoryNonce, "history nonce"},
        std::pair{CommitMismatchField::RelSeq, "REL_SEQ"},
        std::pair{CommitMismatchField::TuSeq, "TU_SEQ"},
        std::pair{CommitMismatchField::TransactionDigest, "transaction digest"},
        std::pair{CommitMismatchField::RawDigest, "raw digest"},
        std::pair{CommitMismatchField::PostStateDigest, "post-state digest"},
    };
    uint64_t identity = 7000;

    {
        TestClient client(Id128::from_u64(identity++));
        const std::vector<uint8_t> input = pseudo_random_bytes(4096);
        const PreparedTuHandle prepared = admit(client, input);
        TxBegin baseline_begin;
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<void> fake = asio::co_spawn(
            context,
            raw_f_serve_and_commit(acceptor, Id128::from_u64(identity++),
                                   CommitMismatchField::None, baseline_begin),
            asio::use_future);
        std::future<ClientRunResult> run = asio::co_spawn(
            context, client.endpoint.run(acceptor.local_endpoint(), prepared, {}),
            asio::use_future);
        context.run();
        fake.get();
        const ClientRunResult baseline = run.get();
        require(baseline.status == ClientRunStatus::Committed && !client.has_active_transaction(),
                "raw fake F baseline exact commit was not accepted");
    }

    for (const auto& [field, name] : cases) {
        // Client A: refusal -> SAME-F exact retry (the REQUIRED exact-retry close: identical
        // TxBegin and BODY replayed on the unchanged route, commit with ExactMatch).
        {
            TestClient client(Id128::from_u64(identity++));
            const std::vector<uint8_t> input = pseudo_random_bytes(4096);
            const PreparedTuHandle prepared = admit(client, input);
            TxBegin observed_begin;
            std::vector<uint8_t> observed_body;
            const FStoreGuid fake_f = Id128::from_u64(identity++);
            {
                asio::io_context context;
                tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
                std::future<void> fake = asio::co_spawn(
                    context,
                    raw_f_serve_and_commit(acceptor, fake_f, field, observed_begin,
                                           &observed_body),
                    asio::use_future);
                std::future<ClientRunResult> run = asio::co_spawn(
                    context, client.endpoint.run(acceptor.local_endpoint(), prepared, {}),
                    asio::use_future);
                context.run();
                fake.get();
                const ClientRunResult rejected = run.get();
                require(rejected.status == ClientRunStatus::TerminalError &&
                            client.has_active_transaction() &&
                            client.endpoint.next_rel_seq() == observed_begin.rel_seq &&
                            client.endpoint.state_digest() ==
                                observed_begin.pre_state_digest &&
                            client.endpoint.f_store_guid() ==
                                std::optional<FStoreGuid>(fake_f),
                        std::string(name) +
                            " commit mismatch rejection did not preserve the transaction cursor");
            }
            {
                asio::io_context context;
                tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
                std::future<void> fake = asio::co_spawn(
                    context,
                    raw_f_exact_retry(acceptor, fake_f, observed_begin, observed_body),
                    asio::use_future);
                std::future<ClientRunResult> run = asio::co_spawn(
                    context, client.endpoint.run(acceptor.local_endpoint(), {}, {}),
                    asio::use_future);
                context.run();
                fake.get();
                const ClientRunResult retried = run.get();
                require(retried.status == ClientRunStatus::Committed &&
                            retried.reconnect == EndpointReconnectOutcome::ExactMatch &&
                            !client.has_active_transaction(),
                        std::string(name) +
                            " refusal did not permit the same-F exact retry to commit");
            }
        }
        // Client B: refusal -> different-F ColdFStore row, retained as a DISTINCT control:
        // prepared work survives rejection across an incarnation replacement.
        {
            TestClient client(Id128::from_u64(identity++));
            const std::vector<uint8_t> input = pseudo_random_bytes(4096);
            const PreparedTuHandle prepared = admit(client, input);
            TxBegin observed_begin;
            const FStoreGuid fake_f = Id128::from_u64(identity++);
            asio::io_context context;
            tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
            std::future<void> fake = asio::co_spawn(
                context,
                raw_f_serve_and_commit(acceptor, fake_f, field, observed_begin),
                asio::use_future);
            std::future<ClientRunResult> run = asio::co_spawn(
                context, client.endpoint.run(acceptor.local_endpoint(), prepared, {}),
                asio::use_future);
            context.run();
            fake.get();
            require(run.get().status == ClientRunStatus::TerminalError &&
                        client.has_active_transaction(),
                    std::string(name) + " (cold-F row) mismatch was not refused");
            P50ServerEndpoint real_server(Id128::from_u64(identity++));
            const PairResult retried = run_pair(client, real_server);
            require(retried.client.status == ClientRunStatus::Committed &&
                        retried.client.reconnect == EndpointReconnectOutcome::ColdFStore &&
                        !client.has_active_transaction() &&
                        copy_input(real_server, client.c_store_guid()) == input,
                    std::string(name) +
                        " prepared work did not survive incarnation replacement");
        }
    }
}


// ===== same_commit negative coverage: lost-final reconnect path =====
// Ruling part 2: after a lost final commit, a fake F presenting a well-formed
// SessionState.last_commit differing from the active TxBegin in any identity field must be
// refused (no LostFinalAcknowledgement), must leave the active transaction in place, and an
// exact subsequent reconciliation must commit.  The presented peer state is kept
// self-consistent (state_digest == last_commit.post_state_digest, next_rel_seq == begin+1,
// route nonce echoed) so only the same_commit comparison against the live begin can object.

asio::awaitable<void> raw_f_receive_then_drop(tcp::acceptor& acceptor, FStoreGuid f_guid,
                                              TxBegin& out_begin) {
    tcp::socket socket = co_await acceptor.async_accept(asio::use_awaitable);
    const SessionHello hello =
        raw_decode<SessionHello>(co_await raw_read(socket, kInitialMaxFramePayload));
    const SessionSelection selection =
        negotiate_session(hello, kProtocolVersion, kProtocolVersion, kKnownProfileMask);
    const uint32_t cap = selection.limits.max_frame_payload;
    SessionState fresh;
    fresh.selected_protocol = selection.protocol;
    fresh.negotiated_profiles = selection.negotiated_profiles;
    fresh.limits = selection.limits;
    fresh.f_store_guid = f_guid;
    co_await raw_write(socket, Message{fresh});
    Frame next = co_await raw_read(socket, cap);
    if (next.type == MessageType::HISTORY_RESET) {
        const HistoryReset reset = raw_decode<HistoryReset>(next);
        SessionState ack = fresh;
        ack.namespace_present = true;
        ack.route_present = true;
        ack.history_nonce = reset.history_nonce;
        ack.next_rel_seq = RelSeq{0};
        ack.state_digest = reset.initial_state_digest;
        co_await raw_write(socket, Message{ack});
        next = co_await raw_read(socket, cap);
    }
    out_begin = raw_decode<TxBegin>(next);
    uint64_t body_bytes = 0;
    while (body_bytes < out_begin.body.encoded_bytes) {
        const BodyMessage body = raw_decode<BodyMessage>(co_await raw_read(socket, cap));
        if (body.bytes.empty())
            break;
        body_bytes += body.bytes.size();
    }
    // Lost final: the F "commits" but the acknowledgement never reaches C.
    boost::system::error_code ignored;
    socket.shutdown(tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
    co_return;
}

asio::awaitable<void> raw_f_present_last_commit(tcp::acceptor& acceptor, FStoreGuid f_guid,
                                                TxBegin begin, CommitMismatchField field) {
    tcp::socket socket = co_await acceptor.async_accept(asio::use_awaitable);
    const SessionHello hello =
        raw_decode<SessionHello>(co_await raw_read(socket, kInitialMaxFramePayload));
    const SessionSelection selection =
        negotiate_session(hello, kProtocolVersion, kProtocolVersion, kKnownProfileMask);
    SessionState peer;
    peer.selected_protocol = selection.protocol;
    peer.negotiated_profiles = selection.negotiated_profiles;
    peer.limits = selection.limits;
    peer.f_store_guid = f_guid;
    peer.namespace_present = true;
    peer.route_present = true;
    TxCommit last = correct_commit_for(begin);
    mutate_commit(last, field);
    // Wire rule: a retained commit must be internally consistent with its route
    // (nonce equal, next_rel = commit.rel + 1, state digest = commit post-state), so the
    // presented state derives from the (possibly mutated) commit.  Nonce/REL_SEQ mismatches
    // are then refused by the client's route checks; TU_SEQ and the digests are decided by
    // same_commit alone.
    peer.history_nonce = last.history_nonce;
    peer.next_rel_seq = RelSeq{last.rel_seq.value + 1};
    peer.state_digest = last.post_state_digest;
    peer.last_commit = last;
    co_await raw_write(socket, Message{peer});
    // Acceptance closes silently; refusal first sends a terminal ERROR frame.  Consume
    // whatever arrives until the peer closes.
    try {
        for (;;)
            (void)co_await raw_read(socket, peer.limits.max_frame_payload);
    } catch (const boost::system::system_error&) {
        // peer closed
    }
    co_return;
}

void test_lost_final_commit_identity_negative_matrix() {
    const std::array cases{
        std::pair{CommitMismatchField::HistoryNonce, "history nonce"},
        std::pair{CommitMismatchField::RelSeq, "REL_SEQ"},
        std::pair{CommitMismatchField::TuSeq, "TU_SEQ"},
        std::pair{CommitMismatchField::TransactionDigest, "transaction digest"},
        std::pair{CommitMismatchField::RawDigest, "raw digest"},
        std::pair{CommitMismatchField::PostStateDigest, "post-state digest"},
    };
    uint64_t identity = 7100;

    // Control: the exact last_commit closes the lost-final window.
    {
        TestClient client(Id128::from_u64(identity++));
        const FStoreGuid f_guid = Id128::from_u64(identity++);
        const std::vector<uint8_t> input = pseudo_random_bytes(4096);
        const PreparedTuHandle prepared = admit(client, input);
        TxBegin begin;
        {
            asio::io_context context;
            tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
            std::future<void> fake = asio::co_spawn(
                context, raw_f_receive_then_drop(acceptor, f_guid, begin), asio::use_future);
            std::future<ClientRunResult> run = asio::co_spawn(
                context, client.endpoint.run(acceptor.local_endpoint(), prepared, {}),
                asio::use_future);
            context.run();
            fake.get();
            const ClientRunResult dropped = run.get();
            require(dropped.status == ClientRunStatus::Disconnected &&
                        client.has_active_transaction(),
                    "lost-final setup did not leave an active transaction");
        }
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<void> fake = asio::co_spawn(
            context,
            raw_f_present_last_commit(acceptor, f_guid, begin, CommitMismatchField::None),
            asio::use_future);
        std::future<ClientRunResult> run = asio::co_spawn(
            context, client.endpoint.run(acceptor.local_endpoint(), {}, {}), asio::use_future);
        context.run();
        fake.get();
        const ClientRunResult accepted = run.get();
        require(accepted.status == ClientRunStatus::Committed &&
                    accepted.reconnect == EndpointReconnectOutcome::LostFinalAcknowledgement &&
                    !client.has_active_transaction(),
                "exact lost-final acknowledgement was not accepted");
    }

    for (const auto& [field, name] : cases) {
        TestClient client(Id128::from_u64(identity++));
        const FStoreGuid f_guid = Id128::from_u64(identity++);
        const std::vector<uint8_t> input = pseudo_random_bytes(4096);
        const PreparedTuHandle prepared = admit(client, input);
        TxBegin begin;
        {
            asio::io_context context;
            tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
            std::future<void> fake = asio::co_spawn(
                context, raw_f_receive_then_drop(acceptor, f_guid, begin), asio::use_future);
            std::future<ClientRunResult> run = asio::co_spawn(
                context, client.endpoint.run(acceptor.local_endpoint(), prepared, {}),
                asio::use_future);
            context.run();
            fake.get();
            require(run.get().status == ClientRunStatus::Disconnected &&
                        client.has_active_transaction(),
                    std::string(name) + " lost-final setup did not retain the transaction");
        }
        {
            const std::optional<FStoreGuid> f_before = client.endpoint.f_store_guid();
            asio::io_context context;
            tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
            std::future<void> fake = asio::co_spawn(
                context, raw_f_present_last_commit(acceptor, f_guid, begin, field),
                asio::use_future);
            std::future<ClientRunResult> run = asio::co_spawn(
                context, client.endpoint.run(acceptor.local_endpoint(), {}, {}),
                asio::use_future);
            context.run();
            fake.get();
            const ClientRunResult refused = run.get();
            // RouteHistoryReset is the SITE's refusal shape (:1508); a bypassed site would
            // fall through to accept()'s internal check, which throws with the default
            // reconnect outcome instead -- so this assertion proves the call site itself.
            // Cursor preservation: the refusal must leave rel/state/route untouched.
            require(refused.status == ClientRunStatus::TerminalError &&
                        refused.reconnect == EndpointReconnectOutcome::RouteHistoryReset &&
                        client.has_active_transaction() &&
                        client.endpoint.next_rel_seq() == begin.rel_seq &&
                        client.endpoint.state_digest() == begin.pre_state_digest &&
                        client.endpoint.f_store_guid() == f_before,
                    std::string(name) +
                        " mismatched last_commit refusal did not preserve the cursor");
        }
        {
            asio::io_context context;
            tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
            std::future<void> fake = asio::co_spawn(
                context,
                raw_f_present_last_commit(acceptor, f_guid, begin, CommitMismatchField::None),
                asio::use_future);
            std::future<ClientRunResult> run = asio::co_spawn(
                context, client.endpoint.run(acceptor.local_endpoint(), {}, {}),
                asio::use_future);
            context.run();
            fake.get();
            const ClientRunResult reconciled = run.get();
            require(reconciled.status == ClientRunStatus::Committed &&
                        reconciled.reconnect ==
                            EndpointReconnectOutcome::LostFinalAcknowledgement &&
                        !client.has_active_transaction(),
                    std::string(name) + " refusal blocked the exact reconciliation");
        }
    }
}

int main(int argc, char** argv) {
    const bool performance_gate = argc == 2 && std::string_view(argv[1]) == "--performance";
    if (argc > 2 || (argc == 2 && !performance_gate))
        fail("usage: p50endpoint [--performance]");
    test_normal_zero_and_completion_stamps();
    test_completion_stamp_correspondence();
    test_completion_live_identity_correspondence();
    test_commit_identity_negative_matrix();
    test_lost_final_commit_identity_negative_matrix();
    test_idempotent_prepare_admission();
    test_candidate_stage_has_no_revision_residue();
    test_input_record_owner_and_aggregate_limits();
    test_fragmentation_at_every_control_and_body_boundary();
    test_exact_replay_and_lost_final();
    test_completion_identity_and_store_replacement();
    test_same_f_route_reset();
    test_reset_ack_equality_and_terminal_result();
    test_handshake_binding_and_namespace_rules();
    test_unnegotiated_begin_rejected_and_route_preserved();
    test_reserved_zero_endpoint_values();
    test_interrupted_begin_identity();
    test_terminal_body_failure_identity();
    test_disconnect_at_each_message_boundary();
    test_component_and_allocation_caps();
    test_two_client_one_server_isolation();
    report_zstd1_metrics(performance_gate);
    std::cout << "p50_endpoint_test: PASS\n";
    return 0;
}
