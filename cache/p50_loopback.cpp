#include "p50_loopback.h"

#include <boost/asio/buffer.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace icecc::p50 {
namespace {

using tcp = boost::asio::ip::tcp;

bool known_message_type(MessageType type) {
    const uint8_t value = static_cast<uint8_t>(type);
    return value >= static_cast<uint8_t>(MessageType::SESSION_HELLO) &&
           value <= static_cast<uint8_t>(MessageType::TX_COMMIT);
}

std::optional<Message> read_message(tcp::socket& socket, uint32_t max_payload) {
    std::array<uint8_t, 4> header{};
    boost::system::error_code error;
    const size_t header_bytes = boost::asio::read(
        socket, boost::asio::buffer(header), error);
    if (error == boost::asio::error::eof && header_bytes == 0)
        return std::nullopt;
    if (error) throw boost::system::system_error(error);
    if (header_bytes != header.size())
        throw std::invalid_argument("CacheWire stream ended inside a frame header");

    const uint32_t word = (uint32_t(header[0]) << 24) |
                          (uint32_t(header[1]) << 16) |
                          (uint32_t(header[2]) << 8) |
                          uint32_t(header[3]);
    const MessageType type = static_cast<MessageType>(word >> 24);
    const uint32_t payload_bytes = word & 0x00ffffffU;
    if (!known_message_type(type))
        throw std::invalid_argument("CacheWire frame has an unknown message type");
    if (payload_bytes > max_payload)
        throw std::length_error("CacheWire frame exceeds the negotiated payload cap");

    std::vector<uint8_t> payload(payload_bytes);
    if (payload_bytes != 0) {
        const size_t read_bytes = boost::asio::read(
            socket, boost::asio::buffer(payload), error);
        if (error) throw boost::system::system_error(error);
        if (read_bytes != payload.size())
            throw std::invalid_argument("CacheWire stream ended inside a frame payload");
    }
    return decode_payload(type, payload);
}

Message require_message(tcp::socket& socket, uint32_t max_payload,
                        const char* context) {
    std::optional<Message> message = read_message(socket, max_payload);
    if (!message)
        throw std::invalid_argument(std::string(context) +
                                    " ended before the required message");
    if (const auto* remote_error = std::get_if<ErrorMessage>(&*message))
        throw std::runtime_error("remote CacheWire error: " +
                                 remote_error->detail);
    return std::move(*message);
}

void write_message(tcp::socket& socket, const Message& message,
                   size_t fragment_bytes = 0) {
    const std::vector<uint8_t> frame = encode_frame(message);
    const size_t quantum = fragment_bytes == 0 ? frame.size() : fragment_bytes;
    if (quantum == 0) throw std::logic_error("empty encoded CacheWire frame");
    for (size_t offset = 0; offset < frame.size();) {
        const size_t count = std::min(quantum, frame.size() - offset);
        boost::asio::write(
            socket, boost::asio::buffer(frame.data() + offset, count));
        offset += count;
    }
}

void best_effort_error(tcp::socket& socket, uint16_t code,
                       const std::string& detail) noexcept {
    try {
        write_message(socket, Message{ErrorMessage{code, detail}});
    } catch (...) {
    }
}

void close_socket(tcp::socket& socket) noexcept {
    boost::system::error_code ignored;
    socket.shutdown(tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
}

bool same_commit_identity(const TxCommit& commit, const TxBegin& begin) {
    return commit.history_nonce == begin.history_nonce &&
           commit.rel_seq == begin.rel_seq &&
           commit.tu_seq == begin.tu_seq &&
           commit.transaction_digest == begin.transaction_digest &&
           commit.raw_digest == begin.raw_digest &&
           commit.post_state_digest == compute_post_state_digest(
               begin.pre_state_digest, begin.history_nonce, begin.rel_seq,
               begin.tu_seq, begin.transaction_digest);
}

}  // namespace

CandidateSessionGate::CandidateSessionGate(
    uint32_t server_profiles, SessionLimits server_limits,
    size_t max_staged_candidates, uint64_t first_candidate_id,
    uint64_t first_session_serial)
    : server_profiles_(server_profiles),
      server_limits_(server_limits),
      max_staged_candidates_(max_staged_candidates),
      next_candidate_id_(first_candidate_id),
      next_session_serial_(first_session_serial) {
    if (server_profiles_ == 0)
        throw std::invalid_argument("candidate gate has no server profile");
    if (max_staged_candidates_ == 0)
        throw std::invalid_argument("candidate gate cannot stage zero candidates");
    if (next_candidate_id_ == 0 || next_session_serial_ == 0)
        throw std::invalid_argument("candidate/session serial zero is reserved");
    SessionHello probe;
    probe.supported_profiles = server_profiles_;
    probe.limits = server_limits_;
    (void)negotiate_session(probe, kProtocolVersion, kProtocolVersion,
                            server_profiles_, server_limits_);
}

CandidateSession CandidateSessionGate::stage(const SessionHello& hello) {
    if (staged_.size() >= max_staged_candidates_)
        throw std::length_error("too many staged CacheWire candidates");
    if (candidate_id_exhausted_)
        throw std::overflow_error("candidate id space exhausted");

    const SessionSelection selection = negotiate_session(
        hello, kProtocolVersion, kProtocolVersion, server_profiles_,
        server_limits_);
    const uint64_t id = next_candidate_id_;
    if (id == std::numeric_limits<uint64_t>::max())
        candidate_id_exhausted_ = true;
    else
        ++next_candidate_id_;

    CandidateSession candidate{id, state_revision_, selection};
    staged_.emplace(id, candidate);
    return candidate;
}

uint64_t CandidateSessionGate::activate(uint64_t candidate_id) {
    const auto position = staged_.find(candidate_id);
    if (position == staged_.end())
        throw std::logic_error("unknown or already-consumed CacheWire candidate");
    if (position->second.state_revision != state_revision_)
        throw std::logic_error("CacheWire candidate snapshot became stale");
    if (session_serial_exhausted_)
        throw std::overflow_error("session serial space exhausted");

    const uint64_t serial = next_session_serial_;
    if (serial == std::numeric_limits<uint64_t>::max())
        session_serial_exhausted_ = true;
    else
        ++next_session_serial_;

    current_session_serial_ = serial;
    staged_.clear();
    advance_revision();
    return serial;
}

void CandidateSessionGate::reject(uint64_t candidate_id) {
    staged_.erase(candidate_id);
}

void CandidateSessionGate::note_state_change() {
    advance_revision();
}

void CandidateSessionGate::disconnect(uint64_t session_serial) {
    if (session_serial == 0 || session_serial != current_session_serial_)
        return;
    current_session_serial_ = 0;
    advance_revision();
}

void CandidateSessionGate::advance_revision() {
    if (state_revision_exhausted_)
        throw std::overflow_error("candidate state revision space exhausted");
    if (state_revision_ == std::numeric_limits<uint64_t>::max()) {
        state_revision_exhausted_ = true;
        throw std::overflow_error("candidate state revision requires F GUID replacement");
    }
    ++state_revision_;
}

ZstdLoopbackServer::ZstdLoopbackServer(
    boost::asio::io_context& context, ZstdLoopbackConfig config,
    CStoreGuid expected_c_store_guid,
    PublishExactInput publish_exact_input)
    : context_(context),
      config_(std::move(config)),
      expected_c_store_guid_(expected_c_store_guid),
      publish_exact_input_(std::move(publish_exact_input)),
      acceptor_(context_, tcp::endpoint(
          boost::asio::ip::address_v4::loopback(), 0)),
      gate_(config_.supported_profiles, config_.session_limits,
            config_.max_staged_candidates) {
    if (config_.supported_profiles != profile_bit(ProfileId::ZSTD_TU))
        throw std::invalid_argument(
            "the bounded loopback endpoint supports only ZSTD_TU");
    if (!publish_exact_input_)
        throw std::invalid_argument("loopback endpoint has no publish callback");
}

uint16_t ZstdLoopbackServer::port() const {
    return acceptor_.local_endpoint().port();
}

SessionState ZstdLoopbackServer::session_state_for(
    const SessionSelection& selection) const {
    SessionState result;
    result.selected_protocol = selection.protocol;
    result.selected_profile = selection.profile;
    result.limits = selection.limits;
    result.f_store_guid = config_.f_store_guid;
    result.namespace_present = namespace_present_;
    result.route_present = route_present_;
    if (route_present_) {
        result.history_nonce = history_nonce_;
        result.next_rel_seq = next_rel_seq_;
        result.state_digest = state_digest_;
        result.last_commit = last_commit_;
    }
    return result;
}

SessionState ZstdLoopbackServer::route_snapshot() const {
    SessionHello probe;
    probe.supported_profiles = config_.supported_profiles;
    probe.limits = config_.session_limits;
    return session_state_for(negotiate_session(
        probe, kProtocolVersion, kProtocolVersion,
        config_.supported_profiles, config_.session_limits));
}

void ZstdLoopbackServer::validate_history_reset(
    const HistoryReset& reset) const {
    if (route_present_ || namespace_present_)
        throw std::logic_error("initial HISTORY_RESET arrived after route establishment");
    if (reset.history_nonce.value == 0 ||
        reset.history_nonce.value <= history_nonce_high_water_)
        throw std::logic_error("HISTORY_NONCE was zero, reused, or decreasing");
    if (reset.initial_state_digest != initial_route_digest(
            expected_c_store_guid_, reset.history_nonce))
        throw std::logic_error("HISTORY_RESET initial digest is not canonical");
}

void ZstdLoopbackServer::apply_history_reset(
    const HistoryReset& reset) {
    namespace_present_ = true;
    route_present_ = true;
    history_nonce_ = reset.history_nonce;
    next_rel_seq_ = RelSeq{};
    state_digest_ = reset.initial_state_digest;
    last_commit_.reset();
    history_nonce_high_water_ = reset.history_nonce.value;
    gate_.note_state_change();
}

void ZstdLoopbackServer::validate_begin_cursor(
    const TxBegin& begin, const SessionSelection& selection) const {
    if (!namespace_present_ || !route_present_)
        throw std::logic_error("TX_BEGIN arrived before route establishment");
    if (begin.profile != selection.profile ||
        (profile_bit(begin.profile) & config_.supported_profiles) == 0)
        throw std::invalid_argument("TX_BEGIN selected the wrong session profile");
    if (begin.history_nonce != history_nonce_ ||
        begin.rel_seq != next_rel_seq_ ||
        begin.pre_state_digest != state_digest_)
        throw std::logic_error("TX_BEGIN does not match the endpoint route cursor");
}

void ZstdLoopbackServer::publish_commit(
    ZstdTuDialogue& dialogue, tcp::socket& socket,
    uint64_t session_serial, const SessionSelection& selection) {
    if (session_serial == 0 ||
        gate_.current_session_serial() != session_serial)
        throw std::logic_error("stale session attempted to publish exact input");
    if (!dialogue.active_begin())
        throw std::logic_error("materialized dialogue lost its TX_BEGIN");

    const TxBegin begin = *dialogue.active_begin();
    if (begin.profile != selection.profile)
        throw std::logic_error("dialogue profile differs from installed session");
    std::vector<uint8_t> exact_input = dialogue.materialize();
    const TxCommit commit{
        begin.history_nonce,
        begin.rel_seq,
        begin.tu_seq,
        begin.transaction_digest,
        begin.raw_digest,
        compute_post_state_digest(begin.pre_state_digest, begin.history_nonce,
                                  begin.rel_seq, begin.tu_seq,
                                  begin.transaction_digest)};

    // The callback is the atomic InputRecord + route publication boundary. If
    // it throws, no CacheWire commit is made visible and the session terminates.
    publish_exact_input_(begin, commit, std::move(exact_input));

    history_nonce_ = commit.history_nonce;
    next_rel_seq_ = RelSeq{commit.rel_seq.value + 1};
    state_digest_ = commit.post_state_digest;
    last_commit_ = commit;
    gate_.note_state_change();
    dialogue.commit_visible();

    // Route state is already durable here. A write failure is therefore a lost
    // final acknowledgement, and the retained last_commit_ remains reconcilable.
    write_message(socket, Message{commit});
}

void ZstdLoopbackServer::serve_one_connection() {
    tcp::socket socket(context_);
    acceptor_.accept(socket);

    std::optional<uint64_t> candidate_id;
    uint64_t session_serial = 0;
    try {
        Message hello_message = require_message(
            socket, kInitialMaxFramePayload, "CacheWire handshake");
        const auto* hello = std::get_if<SessionHello>(&hello_message);
        if (!hello)
            throw std::invalid_argument("first CacheWire message was not SESSION_HELLO");
        if (hello->c_store_guid != expected_c_store_guid_)
            throw std::invalid_argument("SESSION_HELLO named the wrong C_STORE_GUID");

        const CandidateSession candidate = gate_.stage(*hello);
        candidate_id = candidate.id;
        write_message(socket, Message{session_state_for(candidate.selection)});

        Message first_mutation = require_message(
            socket, candidate.selection.limits.max_frame_payload,
            "staged CacheWire candidate");
        std::optional<ZstdTuDialogue> dialogue;

        if (const auto* reset = std::get_if<HistoryReset>(&first_mutation)) {
            validate_history_reset(*reset);
            session_serial = gate_.activate(candidate.id);
            candidate_id.reset();
            apply_history_reset(*reset);
            write_message(socket,
                          Message{session_state_for(candidate.selection)});
            dialogue.emplace(profile_bit(candidate.selection.profile),
                             config_.zstd_limits);
        } else if (const auto* begin = std::get_if<TxBegin>(&first_mutation)) {
            validate_begin_cursor(*begin, candidate.selection);
            dialogue.emplace(profile_bit(candidate.selection.profile),
                             config_.zstd_limits);
            dialogue->begin(*begin);
            session_serial = gate_.activate(candidate.id);
            candidate_id.reset();
        } else {
            throw std::invalid_argument(
                "candidate did not begin with HISTORY_RESET or TX_BEGIN");
        }

        while (true) {
            std::optional<Message> incoming = read_message(
                socket, candidate.selection.limits.max_frame_payload);
            if (!incoming) {
                if (dialogue && dialogue->state() != ZstdTuDialogue::State::Idle)
                    throw std::invalid_argument(
                        "CacheWire session disconnected with an active transaction");
                break;
            }

            if (const auto* begin = std::get_if<TxBegin>(&*incoming)) {
                validate_begin_cursor(*begin, candidate.selection);
                dialogue->begin(*begin);
            } else if (const auto* body = std::get_if<BodyMessage>(&*incoming)) {
                dialogue->append_body(*body);
                if (dialogue->state() == ZstdTuDialogue::State::BodyClosed)
                    publish_commit(*dialogue, socket, session_serial,
                                   candidate.selection);
            } else if (const auto* dict = std::get_if<DictMessage>(&*incoming)) {
                dialogue->append_dict(*dict);
            } else if (const auto* need = std::get_if<NeedMessage>(&*incoming)) {
                dialogue->receive_need(*need);
            } else if (const auto* fill = std::get_if<FillMessage>(&*incoming)) {
                dialogue->receive_fill(*fill);
            } else if (const auto* remote_error =
                           std::get_if<ErrorMessage>(&*incoming)) {
                throw std::runtime_error("remote CacheWire error: " +
                                         remote_error->detail);
            } else {
                throw std::invalid_argument(
                    "message type is invalid after session activation");
            }
        }

        gate_.disconnect(session_serial);
        close_socket(socket);
    } catch (const std::exception& error) {
        if (candidate_id) gate_.reject(*candidate_id);
        if (session_serial != 0) gate_.disconnect(session_serial);
        best_effort_error(socket, config_.protocol_error_code, error.what());
        close_socket(socket);
        throw;
    } catch (...) {
        if (candidate_id) gate_.reject(*candidate_id);
        if (session_serial != 0) gate_.disconnect(session_serial);
        best_effort_error(socket, config_.protocol_error_code,
                          "unknown CacheWire endpoint failure");
        close_socket(socket);
        throw;
    }
}

ZstdLoopbackClient::ZstdLoopbackClient(
    boost::asio::io_context& context, const tcp::endpoint& endpoint,
    SessionHello hello, size_t wire_fragment_bytes)
    : context_(context),
      socket_(context_),
      hello_(std::move(hello)),
      wire_fragment_bytes_(wire_fragment_bytes) {
    socket_.connect(endpoint);
    write_message(socket_, Message{hello_}, wire_fragment_bytes_);
    Message response = require_message(
        socket_, kInitialMaxFramePayload, "SESSION_STATE response");
    const auto* state = std::get_if<SessionState>(&response);
    if (!state)
        throw std::invalid_argument("CacheWire server did not return SESSION_STATE");
    validate_session_state(hello_, *state);
    state_ = *state;
}

ZstdLoopbackClient::~ZstdLoopbackClient() {
    close();
}

void ZstdLoopbackClient::establish_initial_route(
    HistoryNonce history_nonce) {
    if (state_.namespace_present || state_.route_present)
        throw std::logic_error("client route is already established");
    if (history_nonce.value == 0)
        throw std::invalid_argument("initial HISTORY_NONCE cannot be zero");

    const HistoryReset reset{
        history_nonce,
        initial_route_digest(hello_.c_store_guid, history_nonce)};
    write_message(socket_, Message{reset}, wire_fragment_bytes_);
    Message response = require_message(
        socket_, state_.limits.max_frame_payload,
        "HISTORY_RESET acknowledgement");
    const auto* acknowledged = std::get_if<SessionState>(&response);
    if (!acknowledged)
        throw std::invalid_argument("HISTORY_RESET was not acknowledged by SESSION_STATE");
    validate_session_state(hello_, *acknowledged);
    if (!acknowledged->namespace_present || !acknowledged->route_present ||
        acknowledged->history_nonce != history_nonce ||
        acknowledged->next_rel_seq.value != 0 ||
        acknowledged->state_digest != reset.initial_state_digest ||
        acknowledged->last_commit)
        throw std::logic_error("HISTORY_RESET acknowledgement differs from the new route");
    state_ = *acknowledged;
}

TxCommit ZstdLoopbackClient::transfer(
    const ZstdTuEnvelope& envelope, size_t body_message_bytes) {
    if (!state_.namespace_present || !state_.route_present)
        throw std::logic_error("client cannot transfer before route establishment");
    if (envelope.begin.profile != state_.selected_profile ||
        envelope.begin.history_nonce != state_.history_nonce ||
        envelope.begin.rel_seq != state_.next_rel_seq ||
        envelope.begin.pre_state_digest != state_.state_digest)
        throw std::logic_error("client envelope does not match SESSION_STATE cursor");
    if (body_message_bytes == 0)
        body_message_bytes = state_.limits.max_frame_payload;
    if (body_message_bytes == 0 ||
        body_message_bytes > state_.limits.max_frame_payload)
        throw std::invalid_argument("BODY message size exceeds the negotiated frame cap");

    write_message(socket_, Message{envelope.begin}, wire_fragment_bytes_);
    for (size_t offset = 0; offset < envelope.body.size();) {
        const size_t count = std::min(body_message_bytes,
                                      envelope.body.size() - offset);
        BodyMessage body{{envelope.body.begin() + offset,
                          envelope.body.begin() + offset + count}};
        write_message(socket_, Message{std::move(body)}, wire_fragment_bytes_);
        offset += count;
    }

    Message response = require_message(
        socket_, state_.limits.max_frame_payload, "TX_COMMIT response");
    const auto* committed = std::get_if<TxCommit>(&response);
    if (!committed)
        throw std::invalid_argument("CacheWire server did not return TX_COMMIT");
    if (!same_commit_identity(*committed, envelope.begin))
        throw std::logic_error("TX_COMMIT does not match the exact transaction");

    state_.history_nonce = committed->history_nonce;
    state_.next_rel_seq = RelSeq{committed->rel_seq.value + 1};
    state_.state_digest = committed->post_state_digest;
    state_.last_commit = *committed;
    return *committed;
}

void ZstdLoopbackClient::close() {
    close_socket(socket_);
}

}  // namespace icecc::p50
