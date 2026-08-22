#pragma once

#include "p50_zstd.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace icecc::p50 {

// TX_BEGIN is the largest fixed V1 control payload (152 bytes). A negotiated
// frame cap smaller than this can never carry a transaction and is rejected at
// candidate staging instead of failing after session installation.
constexpr uint32_t kM2MinimumControlPayload = 152;

struct CandidateSession {
    uint64_t id = 0;
    uint64_t state_revision = 0;
    SessionSelection selection{};
    auto operator<=>(const CandidateSession&) const = default;
};

// Candidate sockets are staged against a route-state revision. A candidate
// becomes current only after its first mutating message has been validated.
// Activating or disconnecting a session advances the revision so that another
// candidate built from an older snapshot cannot replace current state.
class CandidateSessionGate {
public:
    CandidateSessionGate(uint32_t server_profiles, SessionLimits server_limits,
                         size_t max_staged_candidates = 8,
                         uint64_t first_candidate_id = 1,
                         uint64_t first_session_serial = 1);

    CandidateSession stage(const SessionHello& hello);
    uint64_t activate(uint64_t candidate_id);
    void reject(uint64_t candidate_id);
    void note_state_change();
    void disconnect(uint64_t session_serial) noexcept;

    [[nodiscard]] uint64_t state_revision() const { return state_revision_; }
    [[nodiscard]] uint64_t current_session_serial() const {
        return current_session_serial_;
    }
    [[nodiscard]] size_t staged_candidates() const { return staged_.size(); }

private:
    void advance_revision();

    uint32_t server_profiles_ = 0;
    SessionLimits server_limits_{};
    size_t max_staged_candidates_ = 0;
    uint64_t next_candidate_id_ = 1;
    uint64_t next_session_serial_ = 1;
    bool candidate_id_exhausted_ = false;
    bool session_serial_exhausted_ = false;
    uint64_t state_revision_ = 0;
    bool state_revision_exhausted_ = false;
    uint64_t current_session_serial_ = 0;
    std::map<uint64_t, CandidateSession> staged_;
};

struct ZstdLoopbackConfig {
    FStoreGuid f_store_guid{};
    uint32_t supported_profiles = profile_bit(ProfileId::ZSTD_TU);
    SessionLimits session_limits{};
    ZstdTuLimits zstd_limits{64U << 20, 256U << 20};
    size_t max_staged_candidates = 8;
    uint16_t protocol_error_code = 1;
};

// This is a private M2 precommit staging hook, not yet the job-visible
// InputRecord store. It may retain the exact bytes privately or throw to inject
// an allocation failure. The endpoint advances its route only after this hook
// returns. M3 replaces this seam with one owner transition that publishes the
// restartable InputRecord and route commit atomically.
using PublishExactInput = std::function<void(
    const TxBegin&, const TxCommit&, std::vector<uint8_t>)>;

// A bounded C1F1 transport slice. It intentionally serves one TCP connection
// at a time; daemon accept-loop concurrency and scheduler integration remain
// outside M2. A connection may carry multiple sequential ZSTD_TU transactions.
class ZstdLoopbackServer {
public:
    using tcp = boost::asio::ip::tcp;

    ZstdLoopbackServer(boost::asio::io_context& context,
                       ZstdLoopbackConfig config,
                       CStoreGuid expected_c_store_guid,
                       PublishExactInput publish_exact_input);

    [[nodiscard]] uint16_t port() const;
    [[nodiscard]] SessionState route_snapshot() const;
    [[nodiscard]] const CandidateSessionGate& candidate_gate() const {
        return gate_;
    }

    // Blocks until one accepted connection closes or fails. Protocol failures
    // are reported to the peer best-effort, close only that connection, and are
    // rethrown to the caller for a discriminating test/result path.
    void serve_one_connection();

private:
    SessionState session_state_for(const SessionSelection& selection) const;
    void validate_history_reset(const HistoryReset& reset) const;
    void apply_history_reset(const HistoryReset& reset);
    void validate_begin_cursor(const TxBegin& begin,
                               const SessionSelection& selection) const;
    void publish_commit(ZstdTuDialogue& dialogue, tcp::socket& socket,
                        uint64_t session_serial,
                        const SessionSelection& selection);

    boost::asio::io_context& context_;
    ZstdLoopbackConfig config_;
    CStoreGuid expected_c_store_guid_{};
    PublishExactInput publish_exact_input_;
    tcp::acceptor acceptor_;
    CandidateSessionGate gate_;

    bool namespace_present_ = false;
    bool route_present_ = false;
    HistoryNonce history_nonce_{};
    RelSeq next_rel_seq_{};
    Digest128 state_digest_{};
    std::optional<TxCommit> last_commit_;
    uint64_t history_nonce_high_water_ = 0;
};

class ZstdLoopbackClient {
public:
    using tcp = boost::asio::ip::tcp;

    ZstdLoopbackClient(boost::asio::io_context& context,
                       const tcp::endpoint& endpoint,
                       SessionHello hello,
                       size_t wire_fragment_bytes = 0);
    ~ZstdLoopbackClient();

    ZstdLoopbackClient(const ZstdLoopbackClient&) = delete;
    ZstdLoopbackClient& operator=(const ZstdLoopbackClient&) = delete;

    [[nodiscard]] const SessionState& state() const { return state_; }
    void establish_initial_route(HistoryNonce history_nonce);
    TxCommit transfer(const ZstdTuEnvelope& envelope,
                      size_t body_message_bytes = 0);
    void close();

private:
    tcp::socket socket_;
    SessionHello hello_{};
    SessionState state_{};
    size_t wire_fragment_bytes_ = 0;
};

}  // namespace icecc::p50
