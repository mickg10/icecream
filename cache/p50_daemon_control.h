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
                              std::chrono::steady_clock::time_point deadline,
                              DaemonControlLimits limits = {}) noexcept;

    // Test and daemon seam for an already-connected descriptor.  The caller
    // must provide an already nonblocking descriptor; this method never
    // changes its status flags.
    DaemonControlStatus begin_connected(int nonblocking_fd,
                                        const ControlOperation& operation,
                                        int transfer_fd,
                                        std::chrono::steady_clock::time_point deadline,
                                        DaemonControlLimits limits = {}) noexcept;

    [[nodiscard]] short desired_events() const noexcept;
    DaemonControlStatus advance(std::chrono::steady_clock::time_point now,
                                short revents) noexcept;

    [[nodiscard]] DaemonControlStatus status() const noexcept { return status_; }
    [[nodiscard]] bool done() const noexcept { return status_ != DaemonControlStatus::InProgress; }
    [[nodiscard]] bool rights_sent() const noexcept { return rights_sent_; }
    [[nodiscard]] bool peer_queried() const noexcept { return peer_queried_; }
    [[nodiscard]] int native_handle() const noexcept { return fd_; }
    [[nodiscard]] const std::optional<PeerCredential>& peer() const noexcept { return peer_; }

private:
    enum class Phase : uint8_t { None, Connecting, WriteHello, ReadHelloAck,
                                 WriteControl, WriteHandoff, ReadAck };
    void fail(DaemonControlStatus status) noexcept;
    void close_fd() noexcept;
    bool query_peer() noexcept;
    bool write_bytes(size_t& offset, const std::vector<uint8_t>& bytes,
                     size_t& calls, size_t& budget) noexcept;
    bool write_handoff(size_t& calls, size_t& budget) noexcept;
    bool read_frame(size_t& calls, size_t& budget) noexcept;
    bool read_ack(size_t& calls, size_t& budget) noexcept;
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
    ControlOperation operation_{};
    std::vector<uint8_t> hello_;
    std::vector<uint8_t> control_;
    std::array<uint8_t, 40> handoff_{};
    std::array<uint8_t, 40> ack_{};
    std::vector<uint8_t> frame_read_;
    size_t frame_expected_ = 0;
    size_t offset_ = 0;
    size_t ack_offset_ = 0;
    std::optional<PeerCredential> peer_;
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
                                        DaemonControlLimits limits = {}) noexcept;
    [[nodiscard]] short desired_events() const noexcept;
    DaemonControlStatus advance(std::chrono::steady_clock::time_point now,
                                short revents) noexcept;
    [[nodiscard]] bool done() const noexcept { return status_ != DaemonControlStatus::InProgress; }
    [[nodiscard]] int take_fd() noexcept;
    [[nodiscard]] DaemonControlStatus status() const noexcept { return status_; }
    [[nodiscard]] bool rights_validated() const noexcept { return accepted_fd_ >= 0; }

private:
    void close_all() noexcept;
    void fail(DaemonControlStatus status) noexcept;
    bool validate_wire() noexcept;

    int fd_ = -1;
    int accepted_fd_ = -1;
    bool own_fd_ = false;
    bool have_rights_ = false;
    size_t fd_count_ = 0;
    size_t offset_ = 0;
    size_t ack_offset_ = 0;
    size_t calls_ = 0;
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
    void add(DaemonControlOperation* operation) noexcept;
    [[nodiscard]] size_t size() const noexcept { return operations_.size(); }
    size_t advance_ready(std::chrono::steady_clock::time_point now,
                         const std::vector<short>& revents) noexcept;

private:
    std::vector<DaemonControlOperation*> operations_;
    size_t cursor_ = 0;
};

const char* daemon_control_status_name(DaemonControlStatus status) noexcept;

} // namespace icecc::p50::local
