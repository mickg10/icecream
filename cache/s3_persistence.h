#pragma once

#include "protocol50.h"

#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace icecc::p50::s3 {

// Product-neutral S3 seam. S2 adapts its own key/transaction types here.
using ObjectKey = std::string;

struct NamespaceId {
    CStoreGuid c_guid{};
    uint16_t generation = 0;
    auto operator<=>(const NamespaceId&) const = default;
};

struct NamespaceIdHash {
    size_t operator()(const NamespaceId& value) const noexcept {
        const size_t guid = Id128Hash{}(value.c_guid);
        return guid ^ (static_cast<size_t>(value.generation) *
                      static_cast<size_t>(0x9e3779b9U));
    }
};

enum class ObjectState { Absent, Installing, Present, Pinned };

enum class ErrorCode {
    NamespaceAbsent,
    NamespaceAlreadyAdmitted,
    InvalidObjectKey,
    InvalidTransition,
    InvalidInstall,
    CapacityExceeded,
    ContentConflict,
    Terminal,
};

class PersistenceError : public std::runtime_error {
public:
    PersistenceError(ErrorCode code, std::string message)
        : std::runtime_error(std::move(message)), code_(code) {}
    [[nodiscard]] ErrorCode code() const noexcept { return code_; }

private:
    ErrorCode code_;
};

struct Limits {
    uint64_t total_resident_bytes = UINT64_MAX;
    uint64_t namespace_resident_bytes = UINT64_MAX;
    uint64_t total_staging_bytes = UINT64_MAX;
    uint64_t total_simultaneous_bytes = UINT64_MAX;
};

struct InstallTicket {
    NamespaceId namespace_id{};
    ObjectKey key;
    uint64_t serial = 0;
    auto operator<=>(const InstallTicket&) const = default;
};

struct BeginInstallResult {
    // Same-key/same-content is an idempotent hit; no ticket is returned.
    bool already_present = false;
    InstallTicket ticket{};
};

struct ObjectView {
    ObjectState state = ObjectState::Absent;
    Digest128 content_digest{};
    std::vector<uint8_t> payload;
};

class PersistenceCore {
public:
    explicit PersistenceCore(Limits limits = Limits{});

    // One immutable arena is identified by the exact (C_GUID,generation) pair.
    void admit(NamespaceId namespace_id);
    void touch(NamespaceId namespace_id);

    // Ownership starts at INSTALLING; payload is copied before the ticket is
    // returned, so callers cannot mutate staged content behind the core.
    BeginInstallResult begin_install(NamespaceId namespace_id,
                                     std::string_view key,
                                     std::span<const uint8_t> payload);
    void publish(const InstallTicket& ticket);       // INSTALLING -> PRESENT
    void crash_install(const InstallTicket& ticket); // INSTALLING -> ABSENT

    // Lease-counted PRESENT <-> PINNED transition.
    void pin(NamespaceId namespace_id, std::string_view key);
    void unpin(NamespaceId namespace_id, std::string_view key);

    // Whole-namespace global-LRU eviction. New publishes invoke this when
    // resident capacity is needed; no individual object is evicted.
    [[nodiscard]] std::optional<NamespaceId> evict_lru();

    [[nodiscard]] std::optional<ObjectView> find(NamespaceId namespace_id,
                                                  std::string_view key) const;
    [[nodiscard]] ObjectState state(NamespaceId namespace_id,
                                    std::string_view key) const;
    [[nodiscard]] bool has_namespace(NamespaceId namespace_id) const;
    [[nodiscard]] bool terminal() const noexcept { return terminal_; }
    [[nodiscard]] uint64_t resident_bytes() const noexcept { return resident_bytes_; }
    [[nodiscard]] uint64_t staging_bytes() const noexcept { return staging_bytes_; }
    [[nodiscard]] size_t live_namespace_count() const noexcept;
    [[nodiscard]] uint64_t namespace_resident_bytes(NamespaceId namespace_id) const;

private:
    struct Object {
        ObjectState state = ObjectState::Absent;
        Digest128 content_digest{};
        std::vector<uint8_t> payload;
        uint64_t ticket_serial = 0;
        uint64_t pin_count = 0;
    };

    struct Namespace {
        NamespaceId id{};
        uint64_t last_touch = 0;
        uint64_t resident_bytes = 0;
        bool live = false;
        std::unordered_map<ObjectKey, Object> objects;
    };

    [[nodiscard]] Namespace& namespace_for(NamespaceId namespace_id);
    [[nodiscard]] const Namespace& namespace_for(NamespaceId namespace_id) const;
    void ensure_mutable() const;
    void ensure_key(std::string_view key) const;
    void reserve_staging(uint64_t bytes);
    void reserve_resident(NamespaceId namespace_id, uint64_t bytes);
    [[nodiscard]] bool evict_one_except(NamespaceId excluded);
    [[nodiscard]] bool namespace_evictable(const Namespace& value) const;
    [[nodiscard]] Object& object_for(Namespace& namespace_value,
                                     std::string_view key);
    [[nodiscard]] const Object& object_for(const Namespace& namespace_value,
                                           std::string_view key) const;
    [[nodiscard]] Object& ticket_object(const InstallTicket& ticket);
    void content_conflict(std::string_view key) const;
    [[nodiscard]] uint64_t next_clock();
    [[nodiscard]] uint64_t allocate_ticket_serial();

    Limits limits_;
    uint64_t clock_ = 0;
    uint64_t next_ticket_serial_ = 1;
    uint64_t resident_bytes_ = 0;
    uint64_t staging_bytes_ = 0;
    bool terminal_ = false;
    std::unordered_map<NamespaceId, Namespace, NamespaceIdHash> namespaces_;
};

}  // namespace icecc::p50::s3
