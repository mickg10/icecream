#include "p50_endpoint.h"

#include <boost/asio/buffer.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/system_error.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <stdexcept>

namespace icecc::p50 {
namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using PreparedZstdTUPtr = std::shared_ptr<const ZstdTuEnvelope>;

class StaleCompletion final : public std::exception {
public:
    const char* what() const noexcept override { return "stale endpoint completion"; }
};

class PrecommitPublicationFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void validate_caps(const EndpointCaps& caps) {
    if (caps.wire.max_frame_payload < kMandatoryControlFramePayload ||
        caps.wire.max_frame_payload > kInitialMaxFramePayload)
        throw std::invalid_argument("endpoint frame cap is outside the Protocol-50 range");
    if (caps.wire.max_fill_record_bytes < 32)
        throw std::invalid_argument("endpoint FILL-record cap is too small");
    if (caps.zstd.max_encoded_body_bytes == 0 || caps.zstd.max_raw_bytes == 0)
        throw std::invalid_argument("endpoint ZSTD_TU caps must be nonzero");
}

CompletionStamp with_operation(CompletionStamp stamp, AsyncOperationKind operation) {
    stamp.operation = operation;
    return stamp;
}

CompletionStamp completion_for_test(CompletionStamp stamp, EndpointIoControl& control) {
    if (control.wrong_digest_completion == stamp.operation && stamp.transaction_bound) {
        control.wrong_digest_completion.reset();
        stamp.transaction_digest.bytes[0] ^= 0x80;
    }
    return stamp;
}

ErrorMessage bounded_error(uint16_t code, std::string_view detail, uint32_t payload_cap) {
    if (payload_cap < 6)
        throw std::invalid_argument("ERROR frame cap cannot carry its fixed fields");
    ErrorMessage result{code, std::string(detail)};
    const size_t maximum_detail = payload_cap - 6;
    if (result.detail.size() > maximum_detail)
        result.detail.resize(maximum_detail);
    return result;
}

class ClientTerminalResult final {
public:
    ClientTerminalResult(ErrorMessage error, uint32_t payload_cap)
        : error_(std::move(error)) {
        if (encode_payload(Message{error_}).size() > payload_cap)
            throw std::invalid_argument("terminal result exceeds its negotiated frame cap");
    }

    [[nodiscard]] const ErrorMessage& error() const { return error_; }

private:
    ErrorMessage error_;
};

void set_client_terminal_result(ClientRunResult& result, ErrorMessage error) {
    result.status = ClientRunStatus::TerminalError;
    result.whole_new_attempt = true;
    result.terminal_error = std::move(error);
}

template <class Function>
decltype(auto) client_checked(uint32_t payload_cap, Function&& function) {
    try {
        return std::forward<Function>(function)();
    } catch (const ClientTerminalResult&) {
        throw;
    } catch (const std::exception& error) {
        throw ClientTerminalResult(bounded_error(3, error.what(), payload_cap), payload_cap);
    }
}

void record_completion(CompletionLog* log, CompletionStamp stamp, size_t bytes,
                       const boost::system::error_code& error) {
    if (log)
        log->record({stamp, static_cast<uint64_t>(bytes), error.value()});
}

void close_now(tcp::socket& socket) {
    boost::system::error_code ignored;
    socket.cancel(ignored);
    socket.shutdown(tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
}

template <class Verify>
asio::awaitable<void> async_connect(tcp::socket& socket, const tcp::endpoint& remote,
                                    CompletionStamp stamp, CompletionLog* log, Verify verify) {
    boost::system::error_code error;
    co_await socket.async_connect(remote, asio::redirect_error(asio::use_awaitable, error));
    stamp = with_operation(stamp, AsyncOperationKind::Connect);
    record_completion(log, stamp, 0, error);
    verify(stamp);
    if (error)
        throw boost::system::system_error(error);
}

template <class Verify>
asio::awaitable<void> async_accept(tcp::acceptor& acceptor, tcp::socket& socket,
                                   CompletionStamp stamp, CompletionLog* log, Verify verify) {
    boost::system::error_code error;
    co_await acceptor.async_accept(socket, asio::redirect_error(asio::use_awaitable, error));
    stamp = with_operation(stamp, AsyncOperationKind::Accept);
    record_completion(log, stamp, 0, error);
    verify(stamp);
    if (error)
        throw boost::system::system_error(error);
}

template <class Verify>
asio::awaitable<void> async_write_message(tcp::socket& socket, Message message,
                                          uint32_t max_payload, CompletionStamp stamp,
                                          CompletionLog* log, EndpointIoControl& control,
                                          Verify verify) {
    const MessageType type = message_type(message);
    if (control.close_before_write == type) {
        control.close_before_write.reset();
        close_now(socket);
        throw boost::system::system_error(asio::error::operation_aborted);
    }
    if (control.max_write_fragment == 0)
        throw std::invalid_argument("write fragment limit is zero");
    std::vector<uint8_t> frame = encode_frame(message);
    if (frame.size() - 4 > max_payload)
        throw std::length_error("outbound frame exceeds the negotiated cap");
    size_t offset = 0;
    while (offset != frame.size()) {
        const size_t count = std::min(control.max_write_fragment, frame.size() - offset);
        boost::system::error_code error;
        const size_t written =
            co_await asio::async_write(socket, asio::buffer(frame.data() + offset, count),
                                       asio::redirect_error(asio::use_awaitable, error));
        CompletionStamp fragment_stamp =
            with_operation(stamp, AsyncOperationKind::WriteFragment);
        record_completion(log, fragment_stamp, written, error);
        verify(fragment_stamp);
        if (error)
            throw boost::system::system_error(error);
        offset += written;
    }
    if (control.close_after_write == type) {
        control.close_after_write.reset();
        close_now(socket);
        throw boost::system::system_error(asio::error::operation_aborted);
    }
}

template <class Verify>
asio::awaitable<void> async_report_client_terminal(
    tcp::socket& socket, ErrorMessage terminal, uint32_t max_payload,
    CompletionStamp stamp, CompletionLog* log, EndpointIoControl& control,
    Verify verify) {
    try {
        co_await async_write_message(socket, Message{terminal}, max_payload, stamp,
                                     log, control, verify);
    } catch (const boost::system::system_error&) {
        // Reporting to the peer is best effort.  The client already knows this
        // dialogue cannot continue, so a failed ERROR write must not relabel
        // that local result as an uncertain disconnect.
    }
    throw ClientTerminalResult(std::move(terminal), max_payload);
}

template <class Verify>
asio::awaitable<Frame> async_read_frame(tcp::socket& socket, uint32_t max_payload,
                                        CompletionStamp stamp, CompletionLog* log,
                                        Verify verify) {
    std::array<uint8_t, 4> raw_header{};
    boost::system::error_code error;
    const size_t header_bytes = co_await asio::async_read(
        socket, asio::buffer(raw_header), asio::redirect_error(asio::use_awaitable, error));
    CompletionStamp header_stamp = with_operation(stamp, AsyncOperationKind::ReadHeader);
    record_completion(log, header_stamp, header_bytes, error);
    verify(header_stamp);
    if (error)
        throw boost::system::system_error(error);
    const FrameHeader header = decode_frame_header(raw_header, max_payload);
    Frame frame{header.type, std::vector<uint8_t>(header.payload_bytes)};
    if (!frame.payload.empty()) {
        error.clear();
        const size_t payload_bytes = co_await asio::async_read(
            socket, asio::buffer(frame.payload), asio::redirect_error(asio::use_awaitable, error));
        CompletionStamp payload_stamp = with_operation(stamp, AsyncOperationKind::ReadPayload);
        record_completion(log, payload_stamp, payload_bytes, error);
        verify(payload_stamp);
        if (error)
            throw boost::system::system_error(error);
    }
    co_return frame;
}

template <class Verify>
asio::awaitable<void> async_wait_peer_close(tcp::socket& socket, CompletionStamp stamp,
                                            CompletionLog* log, Verify verify) {
    std::array<uint8_t, 1> unexpected{};
    boost::system::error_code error;
    const size_t bytes = co_await socket.async_read_some(
        asio::buffer(unexpected), asio::redirect_error(asio::use_awaitable, error));
    stamp = with_operation(stamp, AsyncOperationKind::WaitPeerClose);
    record_completion(log, stamp, bytes, error);
    verify(stamp);
    if (!error && bytes != 0)
        throw std::invalid_argument("message arrived after final TX_COMMIT");
    if (error != asio::error::eof && error != asio::error::connection_reset)
        throw boost::system::system_error(error);
}

template <class Component, class Verify>
asio::awaitable<void> async_write_component(tcp::socket& socket, std::span<const uint8_t> bytes,
                                            uint32_t max_payload, CompletionStamp stamp,
                                            CompletionLog* log, EndpointIoControl& control,
                                            Verify verify) {
    if (bytes.empty()) {
        co_await async_write_message(socket, Component{}, max_payload, stamp, log, control,
                                     verify);
        co_return;
    }
    for (size_t offset = 0; offset != bytes.size();) {
        const size_t count = std::min<size_t>(max_payload, bytes.size() - offset);
        Component component;
        component.bytes.assign(bytes.begin() + offset, bytes.begin() + offset + count);
        co_await async_write_message(socket, component, max_payload, stamp, log, control, verify);
        offset += count;
    }
}

bool same_commit(const TxCommit& commit, const TxBegin& begin) {
    return commit.history_nonce == begin.history_nonce && commit.rel_seq == begin.rel_seq &&
           commit.tu_seq == begin.tu_seq && commit.transaction_digest == begin.transaction_digest &&
           commit.raw_digest == begin.raw_digest &&
           commit.post_state_digest ==
               compute_post_state_digest(begin.pre_state_digest, begin.history_nonce, begin.rel_seq,
                                         begin.tu_seq, begin.transaction_digest);
}

template <class T> T decode_as(const Frame& frame) {
    Message decoded = decode_payload(frame.type, frame.payload);
    T* value = std::get_if<T>(&decoded);
    if (!value)
        throw std::invalid_argument("unexpected Protocol-50 message type");
    return std::move(*value);
}

ActionRecord action_record(ActionType action, ActorSide actor, CStoreGuid c_guid, FStoreGuid f_guid,
                           uint64_t session_serial, const TxBegin* begin, HistoryNonce nonce,
                           RelSeq rel, Digest128 state_digest) {
    ActionRecord record;
    record.action = action;
    record.actor = actor;
    record.c_store_guid = c_guid;
    record.f_store_guid = f_guid;
    record.session_serial = session_serial;
    record.history_nonce = nonce;
    record.rel_seq = rel;
    record.state_digest = state_digest;
    if (begin) {
        record.history_nonce = begin->history_nonce;
        record.rel_seq = begin->rel_seq;
        record.tu_seq = begin->tu_seq;
        record.transaction_digest = begin->transaction_digest;
        record.raw_digest = begin->raw_digest;
        if (action != ActionType::INPUT_COMMITTED && action != ActionType::COMMIT_ACCEPTED &&
            action != ActionType::LOST_COMMIT_ACCEPTED)
            record.state_digest = begin->pre_state_digest;
    }
    return record;
}

} // namespace

std::string_view async_operation_name(AsyncOperationKind operation) {
    switch (operation) {
    case AsyncOperationKind::Accept:
        return "ACCEPT";
    case AsyncOperationKind::Connect:
        return "CONNECT";
    case AsyncOperationKind::ReadHeader:
        return "READ_HEADER";
    case AsyncOperationKind::ReadPayload:
        return "READ_PAYLOAD";
    case AsyncOperationKind::WriteFragment:
        return "WRITE_FRAGMENT";
    case AsyncOperationKind::WaitPeerClose:
        return "WAIT_PEER_CLOSE";
    }
    throw std::logic_error("unknown asynchronous operation");
}

struct P50PreparationAuthority::Impl {
    struct Entry {
        PrepareRequestKey request{};
        uint64_t raw_bytes = 0;
        Digest128 raw_digest{};
        PreparedZstdTUPtr prepared;
        uint64_t references = 1;
        uint64_t retained_bytes = 0;
    };

    Impl(CStoreGuid c_store_guid_value, ZstdTuLimits zstd_limits_value,
         PreparationAuthorityLimits authority_limits_value, int compression_level)
        : c_guid(c_store_guid_value), zstd_limits(zstd_limits_value),
          authority_limits(authority_limits_value), codec(compression_level),
          identity(std::make_shared<const uint8_t>(0)) {
        if (c_guid == CStoreGuid{})
            throw std::invalid_argument("C preparation authority GUID zero is reserved");
        if (zstd_limits.max_encoded_body_bytes == 0 ||
            zstd_limits.max_raw_bytes == 0)
            throw std::invalid_argument("preparation ZSTD_TU limits must be nonzero");
        if (authority_limits.max_live_entries == 0 ||
            authority_limits.max_retained_encoded_bytes == 0)
            throw std::invalid_argument("preparation-authority limits must be nonzero");
    }

    TuSeq next_tu_candidate() const {
        if (tu_exhausted)
            throw std::overflow_error("C preparation authority TU_SEQ space exhausted");
        return TuSeq{next_tu};
    }

    void consume_tu() {
        if (next_tu == std::numeric_limits<uint64_t>::max())
            tu_exhausted = true;
        else
            ++next_tu;
    }

    uint64_t next_entry_candidate() const {
        if (entry_exhausted)
            throw std::overflow_error("C preparation handle space exhausted");
        return next_entry;
    }

    void consume_entry() {
        if (next_entry == std::numeric_limits<uint64_t>::max())
            entry_exhausted = true;
        else
            ++next_entry;
    }

    CStoreGuid c_guid{};
    ZstdTuLimits zstd_limits{};
    PreparationAuthorityLimits authority_limits{};
    ZstdTuCodec codec;
    std::shared_ptr<const void> identity;
    uint64_t next_tu = 0;
    bool tu_exhausted = false;
    uint64_t next_entry = 1;
    bool entry_exhausted = false;
    uint64_t retained_bytes = 0;
    std::map<PrepareRequestKey, uint64_t> requests;
    std::map<uint64_t, Entry> entries;
};

P50PreparationAuthority::P50PreparationAuthority(
    CStoreGuid c_store_guid, ZstdTuLimits zstd_limits,
    PreparationAuthorityLimits authority_limits, int compression_level)
    : impl_(std::make_unique<Impl>(c_store_guid, zstd_limits, authority_limits,
                                   compression_level)) {}

P50PreparationAuthority::~P50PreparationAuthority() = default;

PreparedTuHandle P50PreparationAuthority::prepare(PrepareRequestKey request,
                                                   std::span<const uint8_t> exact_input) {
    if (exact_input.size() > impl_->zstd_limits.max_raw_bytes)
        throw std::length_error("ZSTD_TU raw input exceeds the local cap");
    const Digest128 raw_digest = digest128(exact_input);
    if (const auto request_position = impl_->requests.find(request);
        request_position != impl_->requests.end()) {
        const auto entry_position = impl_->entries.find(request_position->second);
        if (entry_position == impl_->entries.end())
            throw std::logic_error("preparation request index lost its retained entry");
        Impl::Entry& entry = entry_position->second;
        if (entry.raw_bytes != exact_input.size() || entry.raw_digest != raw_digest)
            throw std::invalid_argument("PrepareRequestKey was reused for different input");
        if (entry.references == std::numeric_limits<uint64_t>::max())
            throw std::overflow_error("prepared-TU reference count exhausted");
        ++entry.references;
        return PreparedTuHandle(impl_->identity, entry_position->first);
    }

    if (impl_->entries.size() >= impl_->authority_limits.max_live_entries)
        throw std::length_error("C preparation authority reached its live-entry bound");
    const uint64_t retained_room =
        impl_->authority_limits.max_retained_encoded_bytes - impl_->retained_bytes;
    if (retained_room == 0)
        throw std::length_error("C preparation authority reached its retained-byte bound");

    const TuSeq tu_seq = impl_->next_tu_candidate();
    const uint64_t entry_id = impl_->next_entry_candidate();
    ZstdTuLimits admission_limits = impl_->zstd_limits;
    admission_limits.max_encoded_body_bytes =
        std::min(admission_limits.max_encoded_body_bytes, retained_room);
    PreparedZstdTUPtr prepared = std::make_shared<const ZstdTuEnvelope>(impl_->codec.encode(
        HistoryNonce{1}, RelSeq{0}, tu_seq, Digest128{}, exact_input, admission_limits));
    const uint64_t retained = static_cast<uint64_t>(prepared->body.size());
    if (retained > impl_->authority_limits.max_retained_encoded_bytes ||
        impl_->retained_bytes > impl_->authority_limits.max_retained_encoded_bytes - retained)
        throw std::length_error("C preparation authority reached its retained-byte bound");

    Impl::Entry entry{request, static_cast<uint64_t>(exact_input.size()), raw_digest, prepared, 1,
                      retained};
    const auto [entry_position, entry_inserted] =
        impl_->entries.emplace(entry_id, std::move(entry));
    if (!entry_inserted)
        throw std::logic_error("C preparation authority reused an entry identifier");
    try {
        const auto [request_position, request_inserted] = impl_->requests.emplace(request, entry_id);
        (void)request_position;
        if (!request_inserted)
            throw std::logic_error("C preparation request was admitted twice");
    } catch (...) {
        impl_->entries.erase(entry_position);
        throw;
    }
    impl_->retained_bytes += retained;
    impl_->consume_tu();
    impl_->consume_entry();
    return PreparedTuHandle(impl_->identity, entry_id);
}

uint64_t P50PreparationAuthority::retain(PreparedTuHandle handle) {
    if (handle.authority_.lock() != impl_->identity || handle.entry_id_ == 0)
        throw std::invalid_argument("prepared-TU handle does not belong to this C authority");
    const auto position = impl_->entries.find(handle.entry_id_);
    if (position == impl_->entries.end())
        throw std::invalid_argument("prepared-TU handle has been released");
    if (position->second.references == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("prepared-TU reference count exhausted");
    return ++position->second.references;
}

uint64_t P50PreparationAuthority::release(PreparedTuHandle handle) {
    if (handle.authority_.lock() != impl_->identity || handle.entry_id_ == 0)
        throw std::invalid_argument("prepared-TU handle does not belong to this C authority");
    const auto position = impl_->entries.find(handle.entry_id_);
    if (position == impl_->entries.end())
        throw std::invalid_argument("prepared-TU handle has already reached zero references");
    Impl::Entry& entry = position->second;
    if (--entry.references != 0)
        return entry.references;
    if (impl_->requests.erase(entry.request) != 1)
        throw std::logic_error("preparation request index lost its release target");
    impl_->retained_bytes -= entry.retained_bytes;
    impl_->entries.erase(position);
    return 0;
}

CStoreGuid P50PreparationAuthority::c_store_guid() const { return impl_->c_guid; }

ZstdTuLimits P50PreparationAuthority::zstd_limits() const { return impl_->zstd_limits; }

bool P50PreparationAuthority::contains(PreparedTuHandle handle) const {
    return handle.authority_.lock() == impl_->identity && handle.entry_id_ != 0 &&
           impl_->entries.contains(handle.entry_id_);
}

size_t P50PreparationAuthority::live_entry_count() const { return impl_->entries.size(); }

uint64_t P50PreparationAuthority::retained_encoded_bytes() const {
    return impl_->retained_bytes;
}

std::shared_ptr<const ZstdTuEnvelope>
P50PreparationAuthority::resolve(PreparedTuHandle handle) const {
    if (handle.authority_.lock() != impl_->identity || handle.entry_id_ == 0)
        throw std::invalid_argument("prepared-TU handle does not belong to this C authority");
    const auto position = impl_->entries.find(handle.entry_id_);
    if (position == impl_->entries.end())
        throw std::invalid_argument("prepared-TU handle has been released");
    return position->second.prepared;
}

void P50PreparationAuthority::validate_begin(const TxBegin& begin) const {
    validate_zstd_tu_begin(begin, impl_->zstd_limits);
}

struct P50ClientEndpoint::Impl {
    struct Session {
        CStoreGuid c_guid{};
        std::optional<FStoreGuid> f_guid;
        uint64_t serial = 0;
        std::optional<HistoryNonce> provisional_nonce;
        RelSeq provisional_rel{};
    };

    struct Active {
        PreparedZstdTUPtr prepared;
        TxBegin begin;
    };

    Impl(std::shared_ptr<P50PreparationAuthority> preparation_value, EndpointCaps cap_value,
         HistoryNonce first_nonce,
         CompletionLog* completion_log, ActionTrace* action_trace)
        : preparation(std::move(preparation_value)), caps(cap_value),
          next_nonce(first_nonce.value),
          completions(completion_log), actions(action_trace) {
        if (!preparation)
            throw std::invalid_argument("C endpoint requires its preparation authority");
        c_guid = preparation->c_store_guid();
        validate_caps(caps);
        if (caps.zstd != preparation->zstd_limits())
            throw std::invalid_argument(
                "C endpoint and preparation authority use different ZSTD_TU caps");
        if (first_nonce.value == 0)
            throw std::invalid_argument("first endpoint HISTORY_NONCE must be nonzero");
    }

    uint64_t allocate_session() {
        if (session_exhausted)
            throw std::overflow_error("C endpoint session serial space exhausted");
        const uint64_t result = next_session;
        if (next_session == std::numeric_limits<uint64_t>::max())
            session_exhausted = true;
        else
            ++next_session;
        return result;
    }

    HistoryNonce allocate_nonce() {
        if (nonce_exhausted)
            throw std::overflow_error("C endpoint HISTORY_NONCE space exhausted");
        const HistoryNonce result{next_nonce};
        if (next_nonce == std::numeric_limits<uint64_t>::max())
            nonce_exhausted = true;
        else
            ++next_nonce;
        return result;
    }

    void advance_nonce_past(HistoryNonce observed) {
        if (nonce_exhausted || next_nonce > observed.value)
            return;
        if (observed.value == std::numeric_limits<uint64_t>::max()) {
            nonce_exhausted = true;
            return;
        }
        next_nonce = observed.value + 1;
    }

    CompletionStamp stamp(const Session& session, AsyncOperationKind operation) const {
        CompletionStamp result;
        result.actor = ActorSide::C;
        result.operation = operation;
        result.c_store_guid = session.c_guid;
        result.f_store_guid = session.f_guid.value_or(FStoreGuid{});
        result.session_serial = session.serial;
        if (session.provisional_nonce) {
            result.history_nonce = *session.provisional_nonce;
            result.rel_seq = session.provisional_rel;
        } else if (active) {
            result.history_nonce = active->begin.history_nonce;
            result.rel_seq = active->begin.rel_seq;
            result.tu_seq = active->begin.tu_seq;
            result.transaction_digest = active->begin.transaction_digest;
            result.transaction_bound = true;
        } else if (route_known) {
            result.history_nonce = history_nonce;
            result.rel_seq = next_rel;
        }
        return result;
    }

    void require_completion(const Session& session, const CompletionStamp& completion) const {
        if (completion.actor != ActorSide::C || completion.c_store_guid != c_guid ||
            active_session != completion.session_serial ||
            completion.session_serial != session.serial)
            throw StaleCompletion();
        if (completion.f_store_guid != session.f_guid.value_or(FStoreGuid{}))
            throw StaleCompletion();
        if (completion.transaction_bound) {
            if (!active || completion.history_nonce != active->begin.history_nonce ||
                completion.rel_seq != active->begin.rel_seq ||
                completion.tu_seq != active->begin.tu_seq ||
                completion.transaction_digest != active->begin.transaction_digest)
                throw StaleCompletion();
        } else if (completion.history_nonce.value != 0) {
            if (session.provisional_nonce) {
                if (completion.history_nonce != *session.provisional_nonce ||
                    completion.rel_seq != session.provisional_rel)
                    throw StaleCompletion();
            } else if (!route_known || completion.history_nonce != history_nonce ||
                       completion.rel_seq != next_rel) {
                throw StaleCompletion();
            }
        }
    }

    void record(ActionType action, const TxBegin& begin, uint64_t serial,
                Digest128 state_digest) {
        if (!actions || !f_guid)
            return;
        actions->record(action_record(action, ActorSide::C, c_guid, *f_guid, serial, &begin,
                                      begin.history_nonce, begin.rel_seq, state_digest));
    }

    void record_incarnation_replaced(FStoreGuid previous, FStoreGuid replacement,
                                     const TxBegin& retained, uint64_t serial) {
        if (!actions)
            return;
        ActionRecord value = action_record(ActionType::F_STORE_INCAR_REPLACED, ActorSide::C,
                                           c_guid, replacement, serial, &retained,
                                           retained.history_nonce, retained.rel_seq,
                                           retained.pre_state_digest);
        value.previous_f_store_guid = previous;
        actions->record(std::move(value));
    }

    void start_active(const PreparedZstdTUPtr& prepared, uint64_t serial) {
        if (!route_known || !f_guid)
            throw std::logic_error("cannot begin before a route is known");
        if (next_rel.value == std::numeric_limits<uint64_t>::max())
            throw std::overflow_error("C endpoint REL_SEQ space exhausted");
        Active value;
        value.prepared = prepared;
        value.begin.history_nonce = history_nonce;
        value.begin.rel_seq = next_rel;
        value.begin.tu_seq = prepared->begin.tu_seq;
        value.begin.profile = ProfileId::ZSTD_TU;
        value.begin.p29_root_mode = P29RootMode::NotApplicable;
        value.begin.pre_state_digest = state;
        value.begin.dict = prepared->begin.dict;
        value.begin.body = prepared->begin.body;
        value.begin.raw_bytes = prepared->begin.raw_bytes;
        value.begin.raw_digest = prepared->begin.raw_digest;
        value.begin.transaction_digest =
            compute_transaction_digest(value.begin, std::span<const uint8_t>{}, prepared->body);
        preparation->validate_begin(value.begin);
        active = std::move(value);
        record(ActionType::TX_BEGIN, active->begin, serial, active->begin.pre_state_digest);
    }

    void accept(const TxCommit& commit, ActionType action, uint64_t serial) {
        if (!active || !same_commit(commit, active->begin))
            throw std::logic_error("TX_COMMIT does not close C's active transaction");
        record(action, active->begin, serial, commit.post_state_digest);
        state = commit.post_state_digest;
        if (next_rel.value == std::numeric_limits<uint64_t>::max())
            throw std::overflow_error("C endpoint REL_SEQ space exhausted");
        ++next_rel.value;
        active.reset();
        queued.reset();
    }

    CStoreGuid c_guid{};
    std::shared_ptr<P50PreparationAuthority> preparation;
    EndpointCaps caps{};
    uint64_t next_session = 1;
    bool session_exhausted = false;
    uint64_t active_session = 0;
    uint64_t next_nonce = 1;
    bool nonce_exhausted = false;
    std::optional<FStoreGuid> f_guid;
    bool route_known = false;
    HistoryNonce history_nonce{};
    RelSeq next_rel{};
    Digest128 state{};
    std::optional<Active> active;
    PreparedZstdTUPtr queued;
    CompletionLog* completions = nullptr;
    ActionTrace* actions = nullptr;
};

struct P50ServerEndpoint::Impl {
    struct Pending {
        TxBegin begin;
        std::unique_ptr<ZstdTuDialogue> dialogue;
    };

    struct PreparedBegin {
        Pending pending;
        bool replay = false;
    };

    struct Route {
        HistoryNonce nonce{};
        RelSeq next_rel{};
        Digest128 state{};
        std::optional<TxCommit> last_commit;
        std::optional<TxBegin> interrupted;
        std::optional<Pending> pending;
    };

    struct Namespace {
        bool established = false;
        uint64_t active_session = 0;
        std::optional<HistoryNonce> nonce_high_water;
        std::optional<Route> route;
        std::optional<std::vector<uint8_t>> committed;
    };

    struct Revision {
        uint64_t value = 0;
        bool exhausted = false;
    };

    struct LiveSession {
        FStoreGuid f_guid{};
        std::optional<CStoreGuid> c_guid;
        uint64_t candidate_revision = 0;
        HistoryNonce candidate_nonce{};
        RelSeq candidate_rel{};
        bool activated = false;
    };

    struct Session {
        uint64_t serial = 0;
        FStoreGuid f_guid{};
        std::optional<CStoreGuid> c_guid;
        uint64_t candidate_revision = 0;
        std::optional<SessionState> candidate_state;
        bool activated = false;
    };

    Impl(FStoreGuid f_guid_value, EndpointCaps cap_value, CompletionLog* completion_log,
         ActionTrace* action_trace, P50ServerEndpointConfig config_value)
        : f_guid(f_guid_value), caps(cap_value), completions(completion_log),
          actions(action_trace), config(std::move(config_value)) {
        if (f_guid == FStoreGuid{})
            throw std::invalid_argument("F endpoint GUID zero is reserved");
        if (config.protocol_error_code == 0)
            throw std::invalid_argument("F endpoint ERROR code zero is reserved");
        validate_caps(caps);
    }

    Session allocate_session() {
        if (session_exhausted)
            throw std::overflow_error("F endpoint session serial space exhausted");
        const uint64_t result = next_session;
        if (next_session == std::numeric_limits<uint64_t>::max())
            session_exhausted = true;
        else
            ++next_session;
        const auto inserted = live_sessions.emplace(
            result, LiveSession{.f_guid = f_guid,
                                .c_guid = std::nullopt,
                                .candidate_revision = 0,
                                .candidate_nonce = HistoryNonce{},
                                .candidate_rel = RelSeq{},
                                .activated = false});
        if (!inserted.second)
            throw std::logic_error("F endpoint reused a live session serial");
        return Session{.serial = result,
                       .f_guid = f_guid,
                       .c_guid = std::nullopt,
                       .candidate_revision = 0,
                       .candidate_state = std::nullopt,
                       .activated = false};
    }

    void require_incarnation(const Session& session) const {
        const auto position = live_sessions.find(session.serial);
        if (session.serial == 0 || session.f_guid != f_guid ||
            position == live_sessions.end() || position->second.f_guid != session.f_guid)
            throw StaleCompletion();
    }

    void release_session(const Session& session) {
        const auto position = live_sessions.find(session.serial);
        if (position != live_sessions.end() && position->second.f_guid == session.f_guid)
            live_sessions.erase(position);
    }

    Namespace& require(const Session& session) {
        require_incarnation(session);
        if (!session.c_guid || !session.activated)
            throw StaleCompletion();
        auto position = namespaces.find(*session.c_guid);
        if (position == namespaces.end() || position->second.active_session != session.serial)
            throw StaleCompletion();
        return position->second;
    }

    const Namespace& require(const Session& session) const {
        require_incarnation(session);
        if (!session.c_guid || !session.activated)
            throw StaleCompletion();
        const auto position = namespaces.find(*session.c_guid);
        if (position == namespaces.end() || position->second.active_session != session.serial)
            throw StaleCompletion();
        return position->second;
    }

    CompletionStamp stamp(const Session& session, AsyncOperationKind operation,
                          const TxBegin* explicit_begin = nullptr) const {
        CompletionStamp result;
        result.actor = ActorSide::F;
        result.operation = operation;
        result.c_store_guid = session.c_guid.value_or(CStoreGuid{});
        result.f_store_guid = session.f_guid;
        result.session_serial = session.serial;
        const TxBegin* begin = explicit_begin;
        if (!begin && session.activated && session.c_guid) {
            const auto position = namespaces.find(*session.c_guid);
            if (position != namespaces.end() && position->second.route) {
                const Route& route = *position->second.route;
                result.history_nonce = route.nonce;
                result.rel_seq = route.next_rel;
                if (route.pending)
                    begin = &route.pending->begin;
            }
        } else if (!begin && session.candidate_state &&
                   session.candidate_state->route_present) {
            result.history_nonce = session.candidate_state->history_nonce;
            result.rel_seq = session.candidate_state->next_rel_seq;
        }
        if (begin) {
            result.history_nonce = begin->history_nonce;
            result.rel_seq = begin->rel_seq;
            result.tu_seq = begin->tu_seq;
            result.transaction_digest = begin->transaction_digest;
            result.transaction_bound = true;
        }
        return result;
    }

    void require_completion(const CompletionStamp& completion) const {
        if (completion.actor != ActorSide::F || completion.f_store_guid != f_guid)
            throw StaleCompletion();
        const auto live = live_sessions.find(completion.session_serial);
        if (live == live_sessions.end() || live->second.f_guid != completion.f_store_guid)
            throw StaleCompletion();
        const LiveSession& current = live->second;
        if (completion.c_store_guid == CStoreGuid{}) {
            if (current.c_guid || completion.transaction_bound ||
                completion.history_nonce.value != 0)
                throw StaleCompletion();
            return;
        }
        if (!current.c_guid || completion.c_store_guid != *current.c_guid)
            throw StaleCompletion();
        if (!current.activated) {
            const auto revision = revisions.find(completion.c_store_guid);
            if (revision == revisions.end() || revision->second.exhausted ||
                revision->second.value != current.candidate_revision ||
                completion.transaction_bound ||
                completion.history_nonce != current.candidate_nonce ||
                completion.rel_seq != current.candidate_rel)
                throw StaleCompletion();
            return;
        }
        const auto position = namespaces.find(completion.c_store_guid);
        if (position == namespaces.end() ||
            position->second.active_session != completion.session_serial)
            throw StaleCompletion();
        const Namespace& space = position->second;
        if (completion.transaction_bound) {
            const TxBegin* begin = nullptr;
            if (space.route && space.route->pending)
                begin = &space.route->pending->begin;
            if (!begin && space.route && space.route->last_commit) {
                const TxCommit& commit = *space.route->last_commit;
                if (commit.history_nonce == completion.history_nonce &&
                    commit.rel_seq == completion.rel_seq && commit.tu_seq == completion.tu_seq &&
                    commit.transaction_digest == completion.transaction_digest)
                    return;
            }
            if (!begin || begin->history_nonce != completion.history_nonce ||
                begin->rel_seq != completion.rel_seq || begin->tu_seq != completion.tu_seq ||
                begin->transaction_digest != completion.transaction_digest)
                throw StaleCompletion();
        } else if (completion.history_nonce.value != 0 &&
                   (!space.route || space.route->nonce != completion.history_nonce ||
                    space.route->next_rel != completion.rel_seq)) {
            throw StaleCompletion();
        }
    }

    void record(ActionType action, const Session& session, const TxBegin* begin = nullptr,
                Digest128 state_override = {}) {
        if (!actions || !session.c_guid)
            return;
        HistoryNonce nonce{};
        RelSeq rel{};
        Digest128 state_value = state_override;
        const auto position = namespaces.find(*session.c_guid);
        if (position != namespaces.end() && position->second.route) {
            nonce = position->second.route->nonce;
            rel = position->second.route->next_rel;
            if (state_value == Digest128{})
                state_value = position->second.route->state;
        }
        ActionRecord value = action_record(action, ActorSide::F, *session.c_guid, f_guid,
                                           session.serial, begin, nonce, rel, state_value);
        if (action == ActionType::NEED_RECORDED) {
            value.need_keys.clear();
            value.remaining_need = 0;
        }
        actions->record(std::move(value));
    }

    uint64_t current_revision(CStoreGuid c_guid) {
        Revision& revision = revisions[c_guid];
        if (revision.exhausted)
            throw std::overflow_error(
                "candidate revision requires F_STORE_GUID replacement");
        return revision.value;
    }

    Revision& require_revision_advance(CStoreGuid c_guid) {
        Revision& revision = revisions[c_guid];
        if (revision.exhausted ||
            revision.value == std::numeric_limits<uint64_t>::max()) {
            revision.exhausted = true;
            throw std::overflow_error(
                "candidate revision requires F_STORE_GUID replacement");
        }
        return revision;
    }

    static void advance_revision(Revision& revision) noexcept {
        ++revision.value;
        if (revision.value == std::numeric_limits<uint64_t>::max())
            revision.exhausted = true;
    }

    void advance_revision(CStoreGuid c_guid) {
        Revision& revision = require_revision_advance(c_guid);
        advance_revision(revision);
    }

    void advance_revision_on_disconnect(CStoreGuid c_guid) noexcept {
        Revision& revision = revisions[c_guid];
        if (revision.exhausted)
            return;
        if (revision.value == std::numeric_limits<uint64_t>::max())
            revision.exhausted = true;
        else
            ++revision.value;
    }

    SessionState snapshot(CStoreGuid c_guid, SessionSelection selection) const {
        SessionState result;
        result.selected_protocol = selection.protocol;
        result.negotiated_profiles = selection.negotiated_profiles;
        result.limits = selection.limits;
        result.f_store_guid = f_guid;
        const auto position = namespaces.find(c_guid);
        if (position == namespaces.end())
            return result;
        const Namespace& space = position->second;
        result.namespace_present = space.established;
        result.route_present = space.route.has_value();
        if (space.route) {
            result.history_nonce = space.route->nonce;
            result.next_rel_seq = space.route->next_rel;
            result.state_digest = space.route->state;
            result.last_commit = space.route->last_commit;
        }
        return result;
    }

    SessionState stage(Session& session, CStoreGuid c_guid,
                       SessionSelection selection) {
        require_incarnation(session);
        if (c_guid == CStoreGuid{})
            throw std::invalid_argument("SESSION_HELLO C_STORE_GUID zero is reserved");
        const uint64_t revision = current_revision(c_guid);
        SessionState state = snapshot(c_guid, selection);
        session.c_guid = c_guid;
        session.candidate_revision = revision;
        session.candidate_state = state;
        LiveSession& live = live_sessions.at(session.serial);
        live.c_guid = c_guid;
        live.candidate_revision = revision;
        if (state.route_present) {
            live.candidate_nonce = state.history_nonce;
            live.candidate_rel = state.next_rel_seq;
        }
        return state;
    }

    void activate(Session& session) {
        require_incarnation(session);
        if (!session.c_guid || !session.candidate_state || session.activated)
            throw std::logic_error("F endpoint candidate cannot activate");
        LiveSession& live = live_sessions.at(session.serial);
        const auto revision = revisions.find(*session.c_guid);
        if (!live.c_guid || *live.c_guid != *session.c_guid || live.activated ||
            revision == revisions.end() || revision->second.exhausted ||
            revision->second.value != session.candidate_revision)
            throw StaleCompletion();
        advance_revision(*session.c_guid);

        auto [position, inserted] = namespaces.try_emplace(*session.c_guid);
        Namespace& space = position->second;
        const bool replaced = !inserted && space.active_session != 0;
        if (space.route && space.route->pending) {
            space.route->interrupted = space.route->pending->begin;
            space.route->pending.reset();
        }
        space.active_session = session.serial;
        session.activated = true;
        live.activated = true;
        session.candidate_state.reset();
        record(replaced ? ActionType::SESSION_REPLACED : ActionType::SESSION_OPENED, session);
    }

    void disconnect(const Session& session, bool retain_interrupted) {
        if (!session.c_guid)
            return;
        auto position = namespaces.find(*session.c_guid);
        if (position == namespaces.end() || position->second.active_session != session.serial)
            return;
        Namespace& space = position->second;
        if (space.route && space.route->pending) {
            if (retain_interrupted)
                space.route->interrupted = space.route->pending->begin;
            else
                space.route->interrupted.reset();
            space.route->pending.reset();
        }
        record(ActionType::SESSION_DISCONNECTED, session);
        space.active_session = 0;
        advance_revision_on_disconnect(*session.c_guid);
    }

    SessionState session_state(const Session& session, SessionSelection selection) const {
        const Namespace& space = require(session);
        SessionState result;
        result.selected_protocol = selection.protocol;
        result.negotiated_profiles = selection.negotiated_profiles;
        result.limits = selection.limits;
        result.f_store_guid = f_guid;
        result.namespace_present = space.established;
        result.route_present = space.route.has_value();
        if (space.route) {
            result.history_nonce = space.route->nonce;
            result.next_rel_seq = space.route->next_rel;
            result.state_digest = space.route->state;
            result.last_commit = space.route->last_commit;
        }
        return result;
    }

    void validate_history_reset(CStoreGuid c_guid, const Namespace* space,
                                const HistoryReset& reset) const {
        if (space && space->route &&
            (space->route->pending || space->route->interrupted))
            throw std::logic_error(
                "HISTORY_RESET arrived while F retained transaction identity");
        if (reset.initial_state_digest !=
            initial_route_digest(c_guid, reset.history_nonce))
            throw std::invalid_argument("HISTORY_RESET digest was not derived locally");
        if (space && space->nonce_high_water &&
            reset.history_nonce.value <= space->nonce_high_water->value)
            throw std::invalid_argument("HISTORY_RESET nonce did not advance monotonically");
    }

    void validate_history_reset_candidate(const Session& session,
                                          const HistoryReset& reset) const {
        require_incarnation(session);
        if (!session.c_guid || session.activated)
            throw StaleCompletion();
        const auto position = namespaces.find(*session.c_guid);
        validate_history_reset(*session.c_guid,
                               position == namespaces.end() ? nullptr : &position->second,
                               reset);
    }

    void reset_history(const Session& session, const HistoryReset& reset) {
        Namespace& space = require(session);
        validate_history_reset(*session.c_guid, &space, reset);
        space.established = true;
        space.nonce_high_water = reset.history_nonce;
        space.route = Route{.nonce = reset.history_nonce,
                            .next_rel = RelSeq{0},
                            .state = reset.initial_state_digest,
                            .last_commit = std::nullopt,
                            .interrupted = std::nullopt,
                            .pending = std::nullopt};
        record(ActionType::HISTORY_RESET, session);
    }

    PreparedBegin prepare_begin(const Namespace& space, const TxBegin& begin,
                                uint32_t negotiated_profiles) const {
        if (!space.route)
            throw std::logic_error("TX_BEGIN arrived before HISTORY_RESET");
        const Route& route = *space.route;
        if (begin.history_nonce != route.nonce || begin.rel_seq != route.next_rel ||
            begin.pre_state_digest != route.state)
            throw std::logic_error("TX_BEGIN differs from the F route cursor");
        if (route.next_rel.value == std::numeric_limits<uint64_t>::max())
            throw std::overflow_error("F endpoint REL_SEQ space exhausted");
        const uint32_t transaction_profile_bit = profile_bit(begin.profile);
        if (transaction_profile_bit == 0 ||
            (transaction_profile_bit & negotiated_profiles) != transaction_profile_bit)
            throw std::invalid_argument("TX_BEGIN profile was not negotiated");
        if (route.pending)
            throw std::logic_error("F endpoint already has one active transaction");
        if (route.interrupted && *route.interrupted != begin)
            throw std::logic_error(
                "TX_BEGIN differs from the interrupted transaction identity");
        PreparedBegin result;
        result.replay = route.interrupted.has_value();
        result.pending.begin = begin;
        result.pending.dialogue =
            std::make_unique<ZstdTuDialogue>(negotiated_profiles, caps.zstd);
        result.pending.dialogue->begin(begin);
        return result;
    }

    PreparedBegin prepare_begin_candidate(const Session& session, const TxBegin& begin,
                                          uint32_t negotiated_profiles) const {
        require_incarnation(session);
        if (!session.c_guid || session.activated)
            throw StaleCompletion();
        const auto position = namespaces.find(*session.c_guid);
        if (position == namespaces.end())
            throw std::logic_error("TX_BEGIN arrived before HISTORY_RESET");
        return prepare_begin(position->second, begin, negotiated_profiles);
    }

    bool install_begin(const Session& session, PreparedBegin prepared) {
        Namespace& space = require(session);
        Route& route = *space.route;
        if (route.pending)
            throw std::logic_error("F endpoint already has one active transaction");
        if (route.interrupted && route.interrupted != prepared.pending.begin)
            throw StaleCompletion();
        const TxBegin begin = prepared.pending.begin;
        const bool replay = prepared.replay;
        route.interrupted.reset();
        route.pending = std::move(prepared.pending);
        record(replay ? ActionType::ACTIVE_REPLAYED : ActionType::TX_BEGIN, session, &begin);
        record(ActionType::DICT_COMPLETE, session, &begin);
        record(ActionType::NEED_RECORDED, session, &begin);
        return replay;
    }

    bool begin(const Session& session, const TxBegin& begin,
               uint32_t negotiated_profiles) {
        const Namespace& space = require(session);
        return install_begin(session, prepare_begin(space, begin, negotiated_profiles));
    }

    void append_body(const Session& session, BodyMessage message) {
        Namespace& space = require(session);
        if (!space.route || !space.route->pending)
            throw std::logic_error("BODY has no F active transaction");
        Pending& pending = *space.route->pending;
        pending.dialogue->append_body(message);
        if (pending.dialogue->state() == ZstdTuDialogue::State::BodyClosed)
            record(ActionType::BODY_COMPLETE, session, &pending.begin);
    }

    bool body_complete(const Session& session) const {
        const Namespace& space = require(session);
        if (!space.route || !space.route->pending)
            throw std::logic_error("BODY has no F active transaction");
        return space.route->pending->dialogue->state() ==
               ZstdTuDialogue::State::BodyClosed;
    }

    TxCommit materialize_and_commit(const Session& session, TxBegin& committed_begin) {
        Namespace& space = require(session);
        if (!space.route || !space.route->pending)
            throw std::logic_error("F endpoint has no active transaction");
        Route& route = *space.route;
        Pending& pending = *route.pending;
        if (pending.dialogue->state() != ZstdTuDialogue::State::BodyClosed)
            throw std::logic_error("input cannot materialize before BODY closure");
        std::vector<uint8_t> exact = pending.dialogue->materialize();
        record(ActionType::INPUT_MATERIALIZED, session, &pending.begin);
        committed_begin = pending.begin;
        TxCommit commit{pending.begin.history_nonce,
                        pending.begin.rel_seq,
                        pending.begin.tu_seq,
                        pending.begin.transaction_digest,
                        pending.begin.raw_digest,
                        compute_post_state_digest(pending.begin.pre_state_digest,
                                                  pending.begin.history_nonce,
                                                  pending.begin.rel_seq, pending.begin.tu_seq,
                                                  pending.begin.transaction_digest)};
        Revision& revision = require_revision_advance(*session.c_guid);
        if (config.precommit_publish) {
            try {
                config.precommit_publish(*session.c_guid, pending.begin, commit, exact);
            } catch (...) {
                throw PrecommitPublicationFailure(
                    "exact input publication failed before route commit");
            }
        }
        space.committed = std::move(exact);
        advance_revision(revision);
        route.state = commit.post_state_digest;
        ++route.next_rel.value;
        route.last_commit = commit;
        route.interrupted.reset();
        pending.dialogue->commit_visible();
        route.pending.reset();
        record(ActionType::INPUT_COMMITTED, session, &committed_begin, commit.post_state_digest);
        return commit;
    }

    FStoreGuid f_guid{};
    EndpointCaps caps{};
    uint64_t next_session = 1;
    bool session_exhausted = false;
    std::map<uint64_t, LiveSession> live_sessions;
    std::map<CStoreGuid, Namespace> namespaces;
    std::map<CStoreGuid, Revision> revisions;
    CompletionLog* completions = nullptr;
    ActionTrace* actions = nullptr;
    P50ServerEndpointConfig config{};
};

P50ClientEndpoint::P50ClientEndpoint(std::shared_ptr<P50PreparationAuthority> preparation,
                                     EndpointCaps caps, HistoryNonce first_history_nonce,
                                     CompletionLog* completions, ActionTrace* actions)
    : impl_(std::make_unique<Impl>(std::move(preparation), caps, first_history_nonce, completions,
                                   actions)) {}

P50ClientEndpoint::~P50ClientEndpoint() = default;

boost::asio::awaitable<ClientRunResult> P50ClientEndpoint::run(tcp::endpoint remote,
                                                               PreparedTuHandle prepared,
                                                               EndpointIoControl control) {
    if (impl_->active_session != 0)
        throw std::logic_error("C endpoint already has one active dialogue");
    PreparedZstdTUPtr admitted;
    if (prepared)
        admitted = impl_->preparation->resolve(prepared);
    if (impl_->active) {
        if (admitted && admitted != impl_->active->prepared)
            throw std::invalid_argument("retry supplied a different PreparedZstdTU");
    } else if (admitted) {
        if (impl_->queued && admitted != impl_->queued)
            throw std::invalid_argument("C endpoint already has different queued work");
        impl_->queued = std::move(admitted);
    }
    if (!impl_->active && !impl_->queued)
        throw std::invalid_argument("C endpoint run has no prepared transaction");

    Impl::Session session{impl_->c_guid, impl_->f_guid, impl_->allocate_session(), std::nullopt,
                          RelSeq{}};
    const uint64_t serial = session.serial;
    impl_->active_session = session.serial;
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    ClientRunResult result;
    uint32_t terminal_cap = impl_->caps.wire.max_frame_payload;
    const auto verify = [&](CompletionStamp completion) {
        if (control.before_completion_check)
            control.before_completion_check(completion);
        impl_->require_completion(session, completion_for_test(completion, control));
    };
    try {
        co_await async_connect(socket, remote, impl_->stamp(session, AsyncOperationKind::Connect),
                               impl_->completions, verify);

        SessionHello hello;
        hello.c_store_guid = impl_->c_guid;
        hello.supported_profiles = profile_bit(ProfileId::ZSTD_TU);
        hello.limits = impl_->caps.wire;
        co_await async_write_message(socket, hello, impl_->caps.wire.max_frame_payload,
                                     impl_->stamp(session, AsyncOperationKind::WriteFragment),
                                     impl_->completions, control, verify);

        Frame state_frame = co_await async_read_frame(
            socket, impl_->caps.wire.max_frame_payload,
            impl_->stamp(session, AsyncOperationKind::ReadHeader), impl_->completions, verify);
        SessionState peer = client_checked(impl_->caps.wire.max_frame_payload, [&] {
            if (state_frame.type == MessageType::ERROR)
                throw ClientTerminalResult(decode_as<ErrorMessage>(state_frame),
                                           impl_->caps.wire.max_frame_payload);
            SessionState received = decode_as<SessionState>(state_frame);
            validate_session_state(hello, received);
            if ((received.negotiated_profiles & profile_bit(ProfileId::ZSTD_TU)) == 0)
                throw std::invalid_argument("peer did not negotiate the ZSTD_TU profile");
            return received;
        });
        terminal_cap = peer.limits.max_frame_payload;

        const bool knew_f = impl_->f_guid.has_value();
        const bool same_f = knew_f && *impl_->f_guid == peer.f_store_guid;
        const bool established_relationship = knew_f && impl_->route_known;
        session.f_guid = peer.f_store_guid;
        if (same_f && established_relationship && !peer.namespace_present) {
            const ErrorMessage terminal = bounded_error(
                2, "established F_STORE_GUID reported a missing C namespace",
                peer.limits.max_frame_payload);
            co_await async_report_client_terminal(
                socket, terminal, peer.limits.max_frame_payload,
                impl_->stamp(session, AsyncOperationKind::WriteFragment), impl_->completions,
                control, verify);
        }
        const bool exact = same_f && peer.namespace_present && peer.route_present &&
                           impl_->route_known && peer.history_nonce == impl_->history_nonce &&
                           peer.next_rel_seq == impl_->next_rel &&
                           peer.state_digest == impl_->state;
        if (same_f && impl_->active && peer.last_commit && peer.namespace_present &&
            peer.route_present && same_commit(*peer.last_commit, impl_->active->begin) &&
            peer.history_nonce == impl_->history_nonce &&
            peer.next_rel_seq.value == impl_->active->begin.rel_seq.value + 1 &&
            peer.state_digest == peer.last_commit->post_state_digest) {
            impl_->accept(*peer.last_commit, ActionType::LOST_COMMIT_ACCEPTED, serial);
            result.status = ClientRunStatus::Committed;
            result.reconnect = EndpointReconnectOutcome::LostFinalAcknowledgement;
            close_now(socket);
            impl_->active_session = 0;
            co_return result;
        }
        if (same_f && impl_->active && !exact) {
            result.reconnect = EndpointReconnectOutcome::RouteHistoryReset;
            const ErrorMessage terminal = bounded_error(
                2, "established F route differs while C retains an active transaction",
                peer.limits.max_frame_payload);
            co_await async_report_client_terminal(
                socket, terminal, peer.limits.max_frame_payload,
                impl_->stamp(session, AsyncOperationKind::WriteFragment), impl_->completions,
                control, verify);
        }

        if (exact) {
            result.reconnect = EndpointReconnectOutcome::ExactMatch;
        } else {
            result.reconnect = same_f ? EndpointReconnectOutcome::RouteHistoryReset
                                      : EndpointReconnectOutcome::ColdFStore;
            result.whole_new_attempt = impl_->active.has_value();
            PreparedZstdTUPtr retry = impl_->active ? impl_->active->prepared : impl_->queued;
            if (same_f && peer.route_present)
                impl_->advance_nonce_past(peer.history_nonce);
            const HistoryNonce replacement_nonce = impl_->allocate_nonce();
            const RelSeq replacement_rel{0};
            const Digest128 replacement_state =
                initial_route_digest(impl_->c_guid, replacement_nonce);
            session.provisional_nonce = replacement_nonce;
            session.provisional_rel = replacement_rel;
            HistoryReset reset{replacement_nonce, replacement_state};
            co_await async_write_message(socket, reset, peer.limits.max_frame_payload,
                                         impl_->stamp(session, AsyncOperationKind::WriteFragment),
                                         impl_->completions, control, verify);
            Frame reset_ack_frame = co_await async_read_frame(
                socket, peer.limits.max_frame_payload,
                impl_->stamp(session, AsyncOperationKind::ReadHeader), impl_->completions, verify);
            const SessionState reset_ack = client_checked(peer.limits.max_frame_payload, [&] {
                if (reset_ack_frame.type == MessageType::ERROR)
                    throw ClientTerminalResult(decode_as<ErrorMessage>(reset_ack_frame),
                                               peer.limits.max_frame_payload);
                SessionState received = decode_as<SessionState>(reset_ack_frame);
                validate_session_state(hello, received);
                if (received.selected_protocol != peer.selected_protocol ||
                    received.negotiated_profiles != peer.negotiated_profiles ||
                    received.limits != peer.limits || !received.namespace_present ||
                    !received.route_present || received.f_store_guid != peer.f_store_guid ||
                    received.history_nonce != replacement_nonce ||
                    received.next_rel_seq != replacement_rel ||
                    received.state_digest != replacement_state || received.last_commit)
                    throw std::invalid_argument(
                        "HISTORY_RESET acknowledgement differs from the new route");
                return received;
            });
            (void)reset_ack;
            if (impl_->active) {
                if (same_f) {
                    impl_->record(ActionType::TX_ABORTED, impl_->active->begin, serial,
                                  impl_->active->begin.pre_state_digest);
                } else if (established_relationship) {
                    impl_->record_incarnation_replaced(*impl_->f_guid, peer.f_store_guid,
                                                       impl_->active->begin, serial);
                }
            }
            impl_->active.reset();
            impl_->queued = retry;
            impl_->f_guid = peer.f_store_guid;
            impl_->history_nonce = replacement_nonce;
            impl_->next_rel = replacement_rel;
            impl_->state = replacement_state;
            impl_->route_known = true;
            session.provisional_nonce.reset();
        }

        if (!impl_->active) {
            if (!impl_->queued)
                throw std::logic_error("route reconciliation lost queued work");
            const PreparedZstdTUPtr queued = impl_->queued;
            impl_->start_active(queued, serial);
            impl_->queued.reset();
        }
        const TxBegin begin = impl_->active->begin;
        if ((peer.negotiated_profiles & profile_bit(begin.profile)) == 0)
            throw std::logic_error("C selected a profile outside the negotiated mask");
        const uint32_t frame_cap = peer.limits.max_frame_payload;
        co_await async_write_message(socket, begin, frame_cap,
                                     impl_->stamp(session, AsyncOperationKind::WriteFragment),
                                     impl_->completions, control, verify);

        co_await async_write_component<BodyMessage>(
            socket, impl_->active->prepared->body, frame_cap,
            impl_->stamp(session, AsyncOperationKind::WriteFragment), impl_->completions, control,
            verify);
        Frame commit_frame = co_await async_read_frame(
            socket, frame_cap, impl_->stamp(session, AsyncOperationKind::ReadHeader),
            impl_->completions, verify);
        client_checked(frame_cap, [&] {
            if (commit_frame.type == MessageType::ERROR)
                throw ClientTerminalResult(decode_as<ErrorMessage>(commit_frame), frame_cap);
            const TxCommit commit = decode_as<TxCommit>(commit_frame);
            impl_->accept(commit, ActionType::COMMIT_ACCEPTED, serial);
        });
        result.status = ClientRunStatus::Committed;
        close_now(socket);
    } catch (const ClientTerminalResult& terminal) {
        close_now(socket);
        set_client_terminal_result(result, terminal.error());
    } catch (const StaleCompletion&) {
        close_now(socket);
        result.status = ClientRunStatus::Disconnected;
    } catch (const boost::system::system_error&) {
        close_now(socket);
        result.status = ClientRunStatus::Disconnected;
    } catch (const std::bad_alloc&) {
        close_now(socket);
        if (impl_->active_session == serial)
            impl_->active_session = 0;
        throw;
    } catch (const std::exception& error) {
        close_now(socket);
        set_client_terminal_result(result, bounded_error(3, error.what(), terminal_cap));
    } catch (...) {
        close_now(socket);
        if (impl_->active_session == serial)
            impl_->active_session = 0;
        throw;
    }
    if (impl_->active_session == serial)
        impl_->active_session = 0;
    co_return result;
}

CStoreGuid P50ClientEndpoint::c_store_guid() const { return impl_->c_guid; }

std::optional<FStoreGuid> P50ClientEndpoint::f_store_guid() const { return impl_->f_guid; }

bool P50ClientEndpoint::has_active_transaction() const { return impl_->active.has_value(); }

bool P50ClientEndpoint::has_reconciliation_work() const {
    return impl_->active.has_value() || static_cast<bool>(impl_->queued);
}

RelSeq P50ClientEndpoint::next_rel_seq() const { return impl_->next_rel; }

Digest128 P50ClientEndpoint::state_digest() const { return impl_->state; }

P50ServerEndpoint::P50ServerEndpoint(FStoreGuid f_store_guid, EndpointCaps caps,
                                     CompletionLog* completions, ActionTrace* actions,
                                     P50ServerEndpointConfig config)
    : impl_(std::make_unique<Impl>(f_store_guid, caps, completions, actions,
                                   std::move(config))) {}

P50ServerEndpoint::~P50ServerEndpoint() = default;

boost::asio::awaitable<ServerRunResult> P50ServerEndpoint::accept_one(tcp::acceptor& acceptor,
                                                                      EndpointIoControl control) {
    Impl::Session session = impl_->allocate_session();
    ServerRunResult result;
    result.session_serial = session.serial;
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    uint32_t reply_cap = impl_->caps.wire.max_frame_payload;
    const auto verify = [&](CompletionStamp completion) {
        if (control.before_completion_check)
            control.before_completion_check(completion);
        impl_->require_completion(completion_for_test(completion, control));
    };
    std::optional<ErrorMessage> terminal_error;
    bool retain_interrupted_on_terminal = false;
    try {
        co_await async_accept(acceptor, socket, impl_->stamp(session, AsyncOperationKind::Accept),
                              impl_->completions, verify);
        Frame hello_frame = co_await async_read_frame(
            socket, impl_->caps.wire.max_frame_payload,
            impl_->stamp(session, AsyncOperationKind::ReadHeader), impl_->completions, verify);
        SessionHello hello = decode_as<SessionHello>(hello_frame);
        result.c_store_guid = hello.c_store_guid;
        reply_cap = std::min(reply_cap, hello.limits.max_frame_payload);
        const SessionSelection selection =
            negotiate_session(hello, kProtocolVersion, kProtocolVersion,
                              profile_bit(ProfileId::ZSTD_TU), impl_->caps.wire);
        SessionState state = impl_->stage(session, hello.c_store_guid, selection);
        co_await async_write_message(socket, state, selection.limits.max_frame_payload,
                                     impl_->stamp(session, AsyncOperationKind::WriteFragment),
                                     impl_->completions, control, verify);

        Frame next = co_await async_read_frame(
            socket, selection.limits.max_frame_payload,
            impl_->stamp(session, AsyncOperationKind::ReadHeader), impl_->completions, verify);
        if (next.type == MessageType::HISTORY_RESET) {
            const HistoryReset reset = decode_as<HistoryReset>(next);
            impl_->validate_history_reset_candidate(session, reset);
            impl_->activate(session);
            impl_->reset_history(session, reset);
            SessionState reset_ack = impl_->session_state(session, selection);
            co_await async_write_message(socket, reset_ack, selection.limits.max_frame_payload,
                                         impl_->stamp(session, AsyncOperationKind::WriteFragment),
                                         impl_->completions, control, verify);
            next = co_await async_read_frame(socket, selection.limits.max_frame_payload,
                                             impl_->stamp(session, AsyncOperationKind::ReadHeader),
                                             impl_->completions, verify);
            const TxBegin begin = decode_as<TxBegin>(next);
            impl_->begin(session, begin, selection.negotiated_profiles);
        } else {
            const TxBegin begin = decode_as<TxBegin>(next);
            Impl::PreparedBegin prepared = impl_->prepare_begin_candidate(
                session, begin, selection.negotiated_profiles);
            impl_->activate(session);
            impl_->install_begin(session, std::move(prepared));
        }

        do {
            Frame component = co_await async_read_frame(
                socket, selection.limits.max_frame_payload,
                impl_->stamp(session, AsyncOperationKind::ReadHeader), impl_->completions, verify);
            impl_->append_body(session, decode_as<BodyMessage>(component));
        } while (!impl_->body_complete(session));

        TxBegin committed_begin;
        const TxCommit commit = impl_->materialize_and_commit(session, committed_begin);
        co_await async_write_message(
            socket, commit, selection.limits.max_frame_payload,
            impl_->stamp(session, AsyncOperationKind::WriteFragment, &committed_begin),
            impl_->completions, control, verify);
        co_await async_wait_peer_close(
            socket, impl_->stamp(session, AsyncOperationKind::WaitPeerClose, &committed_begin),
            impl_->completions, verify);
        impl_->disconnect(session, false);
        impl_->release_session(session);
        result.status = ServerRunStatus::Completed;
        close_now(socket);
        co_return result;
    } catch (const StaleCompletion&) {
        close_now(socket);
        impl_->disconnect(session, true);
        impl_->release_session(session);
        result.status = ServerRunStatus::Disconnected;
        co_return result;
    } catch (const boost::system::system_error&) {
        close_now(socket);
        impl_->disconnect(session, true);
        impl_->release_session(session);
        result.status = ServerRunStatus::Disconnected;
        co_return result;
    } catch (const PrecommitPublicationFailure& error) {
        retain_interrupted_on_terminal = true;
        terminal_error = bounded_error(impl_->config.protocol_error_code,
                                       error.what(), reply_cap);
    } catch (const std::exception& error) {
        terminal_error = bounded_error(impl_->config.protocol_error_code,
                                       error.what(), reply_cap);
    }

    // C++ forbids a coroutine suspension directly inside an exception handler.
    // Preserve the bounded reply there and send it on the ordinary coroutine path.
    try {
        co_await async_write_message(socket, *terminal_error, reply_cap,
                                     impl_->stamp(session, AsyncOperationKind::WriteFragment),
                                     impl_->completions, control, verify);
    } catch (...) {
    }
    close_now(socket);
    impl_->disconnect(session, retain_interrupted_on_terminal);
    impl_->release_session(session);
    result.status = ServerRunStatus::TerminalError;
    result.terminal_error = std::move(*terminal_error);
    co_return result;
}

void P50ServerEndpoint::reset_store(FStoreGuid new_guid) {
    if (new_guid == FStoreGuid{})
        throw std::invalid_argument("F store reset GUID zero is reserved");
    if (new_guid == impl_->f_guid)
        throw std::invalid_argument("F store reset requires a fresh GUID");
    for (const auto& [guid, space] : impl_->namespaces) {
        if (space.active_session != 0) {
            Impl::Session invalidated{.serial = space.active_session,
                                      .f_guid = impl_->f_guid,
                                      .c_guid = guid,
                                      .candidate_revision = 0,
                                      .candidate_state = std::nullopt,
                                      .activated = true};
            impl_->record(ActionType::SESSION_DISCONNECTED, invalidated);
        }
    }
    impl_->live_sessions.clear();
    impl_->namespaces.clear();
    impl_->revisions.clear();
    impl_->f_guid = new_guid;
}

FStoreGuid P50ServerEndpoint::f_store_guid() const { return impl_->f_guid; }

size_t P50ServerEndpoint::namespace_count() const { return impl_->namespaces.size(); }

std::optional<std::vector<uint8_t>>
P50ServerEndpoint::committed_input(CStoreGuid c_store_guid) const {
    const auto position = impl_->namespaces.find(c_store_guid);
    return position == impl_->namespaces.end() ? std::nullopt : position->second.committed;
}

} // namespace icecc::p50
