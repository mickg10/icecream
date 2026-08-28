#pragma once

// Outer-loop-owned, nonblocking Protocol-50 control operation.  This file is
// intentionally independent of the synchronous Connection/FdHandoff helpers:
// a daemon poll owner calls begin(), desired_events(), and advance() and keeps
// all waiting and fairness decisions in that outer loop.

#include "p50_control_operation.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace icecc::p50::local {

enum class DaemonControlStatus : uint8_t {
    Idle = 0, InProgress, Complete, InvalidArgument, Timeout, Disconnected,
    IoError, Malformed, CredentialFailure, IdentityMismatch, OperationMismatch,
    Truncated, ControlTruncated, ExtraFd, MissingFd, TrailingData,
};

enum class DaemonControlFdOwnership : uint8_t {
    Borrowed = 0,
    Owned,
};

struct DaemonControlLimits {
    size_t syscalls_per_turn = 4;
    size_t bytes_per_turn = 4096;
};

class DaemonControlOperation {
public:
    DaemonControlOperation() noexcept = default;
    ~DaemonControlOperation();
    DaemonControlOperation(const DaemonControlOperation&) = delete;
    DaemonControlOperation& operator=(const DaemonControlOperation&) = delete;
    DaemonControlOperation(DaemonControlOperation&&) noexcept;
    DaemonControlOperation& operator=(DaemonControlOperation&&) noexcept;

    // Creates an owned SOCK_CLOEXEC|SOCK_NONBLOCK socket.  The path is the
    // only operation that may create a descriptor; no public/shared OFD is
    // ever changed to O_NONBLOCK.
    DaemonControlStatus begin(const std::string& path,
                              const ControlOperation& operation,
                              int transfer_fd,
                              const CredentialExpectation& credentials,
                              std::chrono::steady_clock::time_point deadline,
                              DaemonControlLimits limits = {}) noexcept;

    // Test and daemon seam for an already-connected descriptor.  The caller
    // must provide an already nonblocking descriptor; this method never
    // changes its status flags.
    DaemonControlStatus begin_connected(int nonblocking_fd,
                                        const ControlOperation& operation,
                                        int transfer_fd,
                                        const CredentialExpectation& credentials,
                                        std::chrono::steady_clock::time_point deadline,
                                        DaemonControlLimits limits,
                                        DaemonControlFdOwnership ownership) noexcept;

    // Initializes the source-transfer operation on a descriptor whose daemon
    // HELLO/HELLO_ACK exchange was already completed by the supervising
    // control owner.  The supplied identity is the exact authenticated
    // sidecar incarnation; this entry starts with WriteControl and therefore
    // never emits a second HELLO.
    DaemonControlStatus begin_authenticated(
        int nonblocking_fd, const ControlOperation& operation, int transfer_fd,
        const CredentialExpectation& credentials, Identity authenticated_identity,
        std::chrono::steady_clock::time_point deadline,
        DaemonControlLimits limits,
        DaemonControlFdOwnership ownership) noexcept;

    // Initializes the operation on an already-created, nonblocking socket but
    // deliberately does not call connect(2).  The outer owner can therefore
    // spend one turn creating the descriptor and a later turn performing the
    // single connect action before the normal incremental protocol phases.
    DaemonControlStatus begin_connecting(const std::string& path,
                                         int nonblocking_fd,
                                         const ControlOperation& operation,
                                         int transfer_fd,
                                         const CredentialExpectation& credentials,
                                         std::chrono::steady_clock::time_point deadline,
                                         DaemonControlLimits limits,
                                         DaemonControlFdOwnership ownership) noexcept;

    [[nodiscard]] short desired_events() const noexcept;
    DaemonControlStatus advance(std::chrono::steady_clock::time_point now,
                                short revents) noexcept;

    [[nodiscard]] DaemonControlStatus status() const noexcept { return status_; }
    [[nodiscard]] bool done() const noexcept { return status_ != DaemonControlStatus::InProgress; }
    [[nodiscard]] bool rights_sent() const noexcept { return rights_sent_; }
    [[nodiscard]] bool peer_queried() const noexcept { return peer_queried_; }
    [[nodiscard]] size_t last_advance_syscalls() const noexcept { return last_calls_; }
    [[nodiscard]] size_t last_advance_bytes() const noexcept { return last_bytes_; }
    [[nodiscard]] bool deadline_expired(std::chrono::steady_clock::time_point now) const noexcept {
        return status_ == DaemonControlStatus::InProgress && now >= deadline_;
    }
    [[nodiscard]] std::chrono::steady_clock::time_point deadline() const noexcept {
        return deadline_;
    }
    [[nodiscard]] int native_handle() const noexcept { return fd_; }
    [[nodiscard]] const std::optional<PeerCredential>& peer() const noexcept { return peer_; }
    [[nodiscard]] const std::optional<InputLifecycleApplyStatus>&
    lifecycle_result() const noexcept { return lifecycle_result_; }
    [[nodiscard]] const std::optional<P50SourceTransferResult>&
    source_transfer_result() const noexcept { return source_transfer_result_; }

private:
    enum class Phase : uint8_t { None, ConnectPending, Connecting, WriteHello, ReadHelloAck,
                                 CheckHelloAckTrailing, WriteControl, WriteHandoff,
                                 ReadAck, CheckAckTrailing, ReadLifecycleReply,
                                 CheckLifecycleReplyTrailing, ReadSourceReply,
                                 CheckSourceReplyTrailing, WriteLifecycleGoodbye };
    void fail(DaemonControlStatus status) noexcept;
    void close_fd() noexcept;
    bool query_peer() noexcept;
    bool write_bytes(size_t& offset, const std::vector<uint8_t>& bytes,
                     size_t& calls, size_t& budget) noexcept;
    bool write_handoff(size_t& calls, size_t& budget) noexcept;
    bool read_frame(size_t& calls, size_t& budget) noexcept;
    bool read_ack(size_t& calls, size_t& budget) noexcept;
    bool read_lifecycle_reply(size_t& calls, size_t& budget) noexcept;
    bool read_source_reply(size_t& calls, size_t& budget) noexcept;
    bool write_lifecycle_goodbye(size_t& calls, size_t& budget) noexcept;
    bool check_stream_trailing(Phase next_phase, size_t& calls,
                               size_t& budget) noexcept;
    bool validate_ack() noexcept;

    int fd_ = -1;
    int transfer_fd_ = -1;
    bool own_fd_ = false;
    bool rights_sent_ = false;
    bool peer_queried_ = false;
    Phase phase_ = Phase::None;
    DaemonControlStatus status_ = DaemonControlStatus::Idle;
    std::chrono::steady_clock::time_point deadline_{};
    DaemonControlLimits limits_{};
    CredentialExpectation credentials_{};
    ControlOperation operation_{};
    std::string connect_path_;
    std::vector<uint8_t> hello_;
    std::vector<uint8_t> control_;
    std::array<uint8_t, 40> handoff_{};
    std::array<uint8_t, 40> ack_{};
    std::vector<uint8_t> frame_read_;
    size_t frame_expected_ = 0;
    size_t offset_ = 0;
    size_t ack_offset_ = 0;
    size_t last_calls_ = 0;
    size_t last_bytes_ = 0;
    std::optional<PeerCredential> peer_;
    bool lifecycle_mode_ = false;
    std::vector<uint8_t> lifecycle_goodbye_;
    std::optional<InputLifecycleApplyStatus> lifecycle_result_;
    std::optional<P50SourceTransferResult> source_transfer_result_;
};

// Receiver seam used by the daemon adapter after it has admitted a connection
// and validated HELLO/control.  It owns every received FD until the complete
// 40-byte key and ancillary set validate; malformed/truncated/extra rights are
// closed immediately and never escape as an accepted descriptor.
class DaemonControlHandoffReceiver {
public:
    DaemonControlHandoffReceiver() noexcept = default;
    ~DaemonControlHandoffReceiver();
    DaemonControlHandoffReceiver(const DaemonControlHandoffReceiver&) = delete;
    DaemonControlHandoffReceiver& operator=(const DaemonControlHandoffReceiver&) = delete;

    DaemonControlStatus begin_connected(int nonblocking_fd,
                                        const ControlOperation& expected,
                                        std::chrono::steady_clock::time_point deadline,
                                        DaemonControlLimits limits,
                                        DaemonControlFdOwnership ownership) noexcept;
    [[nodiscard]] short desired_events() const noexcept;
    DaemonControlStatus advance(std::chrono::steady_clock::time_point now,
                                short revents) noexcept;
    [[nodiscard]] bool done() const noexcept { return status_ != DaemonControlStatus::InProgress; }
    [[nodiscard]] int take_fd() noexcept;
    [[nodiscard]] DaemonControlStatus status() const noexcept { return status_; }
    [[nodiscard]] bool rights_validated() const noexcept { return accepted_fd_ >= 0; }
    [[nodiscard]] size_t last_advance_syscalls() const noexcept { return last_calls_; }
    [[nodiscard]] size_t last_advance_bytes() const noexcept { return last_bytes_; }
    [[nodiscard]] size_t wire_bytes_received() const noexcept { return offset_; }
    [[nodiscard]] bool deadline_expired(std::chrono::steady_clock::time_point now) const noexcept {
        return status_ == DaemonControlStatus::InProgress && now >= deadline_;
    }

private:
    void close_all() noexcept;
    void fail(DaemonControlStatus status) noexcept;
    bool validate_wire() noexcept;
    bool check_trailing(size_t& calls, size_t& budget) noexcept;

    int fd_ = -1;
    int accepted_fd_ = -1;
    bool own_fd_ = false;
    bool have_rights_ = false;
    bool trailing_checked_ = false;
    bool datagram_ = false;
    size_t fd_count_ = 0;
    size_t offset_ = 0;
    size_t ack_offset_ = 0;
    size_t last_calls_ = 0;
    size_t last_bytes_ = 0;
    std::chrono::steady_clock::time_point deadline_{};
    DaemonControlLimits limits_{};
    ControlOperation expected_{};
    std::array<uint8_t, 40> wire_{};
    DaemonControlStatus status_ = DaemonControlStatus::Idle;
};

// A tiny round-robin seam for the sole daemon poll owner.  It bounds work per
// operation and advances each ready operation once before revisiting one.
class DaemonControlPollAdapter {
public:
    using Registration = uint64_t;

    // Registration is explicitly borrowed: the owner must call remove()
    // before destroying the operation.  The adapter never owns or deletes it.
    Registration add(DaemonControlOperation& operation) noexcept;
    bool remove(Registration registration) noexcept;
    [[nodiscard]] size_t size() const noexcept { return operations_.size(); }
    size_t advance_ready(std::chrono::steady_clock::time_point now,
                         const std::vector<short>& revents) noexcept;

private:
    struct Entry {
        Registration registration = 0;
        DaemonControlOperation* operation = nullptr;
    };
    std::vector<Entry> operations_;
    size_t cursor_ = 0;
    Registration next_registration_ = 1;
};

const char* daemon_control_status_name(DaemonControlStatus status) noexcept;

} // namespace icecc::p50::local
