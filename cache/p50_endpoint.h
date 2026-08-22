#pragma once

#include "p50_actions.h"
#include "p50_zstd.h"

#include <utility>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace icecc::p50 {

struct EndpointCaps {
    SessionLimits wire{};
    ZstdTuLimits zstd{uint64_t{64} << 20, uint64_t{2} << 30};
    auto operator<=>(const EndpointCaps&) const = default;
};

enum class AsyncOperationKind : uint8_t {
    Accept,
    Connect,
    ReadHeader,
    ReadPayload,
    WriteFragment,
    WaitPeerClose,
};

std::string_view async_operation_name(AsyncOperationKind operation);

struct CompletionStamp {
    ActorSide actor = ActorSide::C;
    AsyncOperationKind operation = AsyncOperationKind::Connect;
    CStoreGuid c_store_guid{};
    FStoreGuid f_store_guid{};
    uint64_t session_serial = 0;
    HistoryNonce history_nonce{};
    RelSeq rel_seq{};
    TuSeq tu_seq{};
    Digest128 transaction_digest{};
    bool transaction_bound = false;
    auto operator<=>(const CompletionStamp&) const = default;
};

struct AsyncCompletion {
    CompletionStamp stamp{};
    uint64_t transferred_bytes = 0;
    int error_value = 0;
    auto operator<=>(const AsyncCompletion&) const = default;
};

class CompletionLog {
public:
    void record(AsyncCompletion completion) { completions_.push_back(std::move(completion)); }
    [[nodiscard]] const std::vector<AsyncCompletion>& completions() const { return completions_; }
    void clear() { completions_.clear(); }

private:
    std::vector<AsyncCompletion> completions_;
};

// This control is a deterministic test seam around message boundaries and write
// fragmentation. Normal product callers use the default value.
struct EndpointIoControl {
    size_t max_write_fragment = std::numeric_limits<size_t>::max();
    std::optional<MessageType> close_before_write;
    std::optional<MessageType> close_after_write;
    std::optional<AsyncOperationKind> wrong_digest_completion;
    std::function<void(const CompletionStamp&)> before_completion_check;
};

struct PrepareRequestKey {
    uint64_t producer_session = 0;
    uint64_t request_token = 0;
    auto operator<=>(const PrepareRequestKey&) const = default;
};

struct PreparationAuthorityLimits {
    size_t max_live_entries = 4096;
    uint64_t max_retained_encoded_bytes = uint64_t{512} << 20;
    auto operator<=>(const PreparationAuthorityLimits&) const = default;
};

class PreparedTuHandle {
public:
    PreparedTuHandle() = default;

    [[nodiscard]] explicit operator bool() const {
        return entry_id_ != 0 && !authority_.expired();
    }

    friend bool operator==(const PreparedTuHandle& left, const PreparedTuHandle& right) {
        const bool same_authority = !left.authority_.owner_before(right.authority_) &&
                                    !right.authority_.owner_before(left.authority_);
        return left.entry_id_ == right.entry_id_ && same_authority;
    }

private:
    PreparedTuHandle(std::weak_ptr<const void> authority, uint64_t entry_id)
        : authority_(std::move(authority)), entry_id_(entry_id) {}

    std::weak_ptr<const void> authority_;
    uint64_t entry_id_ = 0;

    friend class P50PreparationAuthority;
};

class P50PreparationAuthority {
public:
    explicit P50PreparationAuthority(
        CStoreGuid c_store_guid,
        ZstdTuLimits zstd_limits = {uint64_t{64} << 20, uint64_t{2} << 30},
                                     PreparationAuthorityLimits authority_limits = {},
                                     int compression_level = 1);
    ~P50PreparationAuthority();
    P50PreparationAuthority(const P50PreparationAuthority&) = delete;
    P50PreparationAuthority& operator=(const P50PreparationAuthority&) = delete;

    PreparedTuHandle prepare(PrepareRequestKey request,
                             std::span<const uint8_t> exact_input);
    uint64_t retain(PreparedTuHandle handle);
    uint64_t release(PreparedTuHandle handle);

    [[nodiscard]] CStoreGuid c_store_guid() const;
    [[nodiscard]] ZstdTuLimits zstd_limits() const;
    [[nodiscard]] bool contains(PreparedTuHandle handle) const;
    [[nodiscard]] size_t live_entry_count() const;
    [[nodiscard]] uint64_t retained_encoded_bytes() const;

private:
    std::shared_ptr<const ZstdTuEnvelope> resolve(PreparedTuHandle handle) const;
    void validate_begin(const TxBegin& begin) const;

    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend class P50ClientEndpoint;
};

enum class EndpointReconnectOutcome : uint8_t {
    ExactMatch,
    LostFinalAcknowledgement,
    ColdFStore,
    RouteHistoryReset,
};

enum class ClientRunStatus : uint8_t {
    Committed,
    Disconnected,
    TerminalError,
};

struct ClientRunResult {
    ClientRunStatus status = ClientRunStatus::Disconnected;
    EndpointReconnectOutcome reconnect = EndpointReconnectOutcome::ExactMatch;
    bool whole_new_attempt = false;
    std::optional<ErrorMessage> terminal_error;
};

enum class ServerRunStatus : uint8_t {
    Completed,
    Disconnected,
    TerminalError,
};

struct ServerRunResult {
    ServerRunStatus status = ServerRunStatus::Disconnected;
    uint64_t session_serial = 0;
    std::optional<CStoreGuid> c_store_guid;
    std::optional<ErrorMessage> terminal_error;
};

class P50ClientEndpoint {
public:
    explicit P50ClientEndpoint(std::shared_ptr<P50PreparationAuthority> preparation,
                               EndpointCaps caps = {},
                               HistoryNonce first_history_nonce = HistoryNonce{1},
                               CompletionLog* completions = nullptr,
                               ActionTrace* actions = nullptr);
    ~P50ClientEndpoint();
    P50ClientEndpoint(const P50ClientEndpoint&) = delete;
    P50ClientEndpoint& operator=(const P50ClientEndpoint&) = delete;

    boost::asio::awaitable<ClientRunResult> run(boost::asio::ip::tcp::endpoint remote,
                                                PreparedTuHandle prepared = {},
                                                EndpointIoControl control = {});

    [[nodiscard]] CStoreGuid c_store_guid() const;
    [[nodiscard]] std::optional<FStoreGuid> f_store_guid() const;
    [[nodiscard]] bool has_active_transaction() const;
    [[nodiscard]] bool has_reconciliation_work() const;
    [[nodiscard]] RelSeq next_rel_seq() const;
    [[nodiscard]] Digest128 state_digest() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class P50ServerEndpoint {
public:
    explicit P50ServerEndpoint(FStoreGuid f_store_guid, EndpointCaps caps = {},
                               CompletionLog* completions = nullptr,
                               ActionTrace* actions = nullptr);
    ~P50ServerEndpoint();
    P50ServerEndpoint(const P50ServerEndpoint&) = delete;
    P50ServerEndpoint& operator=(const P50ServerEndpoint&) = delete;

    boost::asio::awaitable<ServerRunResult> accept_one(boost::asio::ip::tcp::acceptor& acceptor,
                                                       EndpointIoControl control = {});

    void reset_store(FStoreGuid new_guid);
    [[nodiscard]] FStoreGuid f_store_guid() const;
    [[nodiscard]] size_t namespace_count() const;
    [[nodiscard]] std::optional<std::vector<uint8_t>>
    committed_input(CStoreGuid c_store_guid) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace icecc::p50
