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

struct HelloRaceState {
    bool first_pending = false;
    bool incompatible_finished = false;
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
    while (!coordination.incompatible_finished) {
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
    coordination.incompatible_finished = true;
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
    require(server.committed_input(client.c_store_guid()) == input,
            "normal loopback did not retain exact input");

    const std::vector<uint8_t> empty;
    const PairResult second = run_pair(client, server, admit(client, empty));
    require(second.client.status == ClientRunStatus::Committed &&
                second.client.reconnect == EndpointReconnectOutcome::ExactMatch,
            "zero-length component transaction did not complete on the exact route");
    require(server.committed_input(client.c_store_guid()) == empty,
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

void test_idempotent_prepare_admission() {
    TestClient client(Id128::from_u64(150));
    const PrepareRequestKey request{7, 91};
    const std::vector<uint8_t> input = bytes("one canonical local request\n");
    const PreparedTuHandle first = client.prepare(request, input);
    const PreparedTuHandle replay = client.prepare(request, input);
    require(first == replay && client.authority->live_entry_count() == 1 &&
                client.authority->retained_encoded_bytes() != 0,
            "same prepare key/input did not return one retained authority entry");
    require_throws<std::invalid_argument>(
        [&] { (void)client.prepare(request, bytes("different bytes\n")); },
        "prepare key was rebound to different input");

    auto other_authority =
        std::make_shared<P50PreparationAuthority>(Id128::from_u64(149));
    require_throws<std::invalid_argument>([&] { (void)other_authority->retain(first); },
                                          "foreign preparation handle was accepted");
    require(client.authority->release(replay) == 1 &&
                client.authority->release(first) == 0 &&
                client.authority->live_entry_count() == 0 &&
                client.authority->retained_encoded_bytes() == 0,
            "explicit preparation release did not reach exact zero");
    require_throws<std::invalid_argument>([&] { (void)client.authority->release(first); },
                                          "zero-reference handle was released twice");

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
    require(bounded->release(bounded_replay) == 1 && bounded->release(bounded_first) == 0,
            "bounded replay references did not release to zero");

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
        require(server.committed_input(client.c_store_guid()) == input,
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
        require(server.committed_input(client.c_store_guid()) == input,
                "F did not retain its completed input before final-message loss");
        const PairResult reconciled = run_pair(client, server);
        require(reconciled.client.status == ClientRunStatus::Committed &&
                    reconciled.client.reconnect ==
                        EndpointReconnectOutcome::LostFinalAcknowledgement &&
                    !client.has_active_transaction(),
                "lost-final reconciliation did not accept the exact retained commit");
        require_trace(actions, "lost-final trace");
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
                    !server.committed_input(client.c_store_guid()),
                "wrong-digest completion changed endpoint state");
        const PairResult replayed = run_pair(client, server);
        require(replayed.client.status == ClientRunStatus::Committed &&
                    server.committed_input(client.c_store_guid()) == input,
                "wrong-digest completion was not recoverable by exact replay");
        require_trace(actions, "wrong-digest completion trace");
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
                    !server.committed_input(client.c_store_guid()),
                "F reset did not fence its old in-flight completion");
        const PairResult replaced = run_pair(client, server);
        require(replaced.client.status == ClientRunStatus::Committed &&
                    replaced.client.reconnect == EndpointReconnectOutcome::ColdFStore &&
                    replaced.client.whole_new_attempt &&
                    replaced.server.session_serial > invalidated.server.session_serial &&
                    server.committed_input(client.c_store_guid()) == input,
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
                    server.committed_input(client.c_store_guid()) == input,
                "postcommit/pre-ack replacement fixture did not reach its durable window");
        server.reset_store(Id128::from_u64(472));
        const PairResult replaced = run_pair(client, server);
        require(replaced.client.status == ClientRunStatus::Committed &&
                    replaced.client.reconnect == EndpointReconnectOutcome::ColdFStore &&
                    replaced.client.whole_new_attempt &&
                    server.committed_input(client.c_store_guid()) == input,
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
        require(server.committed_input(client.c_store_guid()) == input,
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
                    good.committed_input(client.c_store_guid()) == input,
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
                    good.committed_input(client.c_store_guid()) == input,
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
                    good.committed_input(client.c_store_guid()) == input,
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
                    established.committed_input(client.c_store_guid()) == input,
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
                    server.committed_input(c_guid) == input,
                "incompatible HELLO bound or changed the live namespace");
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
                    !inconsistent.committed_input(client.c_store_guid()),
                "same GUID with an absent established namespace was treated as cold");

        const PairResult recovered = run_pair(client, established);
        require(recovered.client.status == ClientRunStatus::Committed &&
                    established.committed_input(client.c_store_guid()) == input,
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
                    server.committed_input(client.c_store_guid()) == input,
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
                    server.committed_input(client.c_store_guid()) == input,
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
                    server.committed_input(client.c_store_guid()) == input,
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
                    server.committed_input(client.c_store_guid()) == input,
                "asymmetric exact-152-byte negotiation did not complete");
    }
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

int main(int argc, char** argv) {
    const bool performance_gate = argc == 2 && std::string_view(argv[1]) == "--performance";
    if (argc > 2 || (argc == 2 && !performance_gate))
        fail("usage: p50endpoint [--performance]");
    test_normal_zero_and_completion_stamps();
    test_idempotent_prepare_admission();
    test_fragmentation_at_every_control_and_body_boundary();
    test_exact_replay_and_lost_final();
    test_completion_identity_and_store_replacement();
    test_same_f_route_reset();
    test_reset_ack_equality_and_terminal_result();
    test_handshake_binding_and_namespace_rules();
    test_disconnect_at_each_message_boundary();
    test_component_and_allocation_caps();
    report_zstd1_metrics(performance_gate);
    std::cout << "p50_endpoint_test: PASS\n";
    return 0;
}
