#include "s3_persistence.h"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace icecc::p50::s3 {
namespace {

bool add_overflows(uint64_t left, uint64_t right) {
    return right > std::numeric_limits<uint64_t>::max() - left;
}

}  // namespace

PersistenceCore::PersistenceCore(Limits limits) : limits_(limits) {
    if (limits_.total_resident_bytes < limits_.namespace_resident_bytes &&
        limits_.namespace_resident_bytes != UINT64_MAX)
        throw std::invalid_argument("namespace resident cap exceeds total cap");
}

void PersistenceCore::ensure_mutable() const {
    if (terminal_)
        throw PersistenceError(ErrorCode::Terminal,
                               "S3 persistence core is terminal after content conflict");
}

void PersistenceCore::ensure_key(std::string_view key) const {
    if (key.empty())
        throw PersistenceError(ErrorCode::InvalidObjectKey,
                               "S3 object key must not be empty");
}

PersistenceCore::Namespace& PersistenceCore::namespace_for(NamespaceId namespace_id) {
    auto position = namespaces_.find(namespace_id);
    if (position == namespaces_.end() || !position->second.live)
        throw PersistenceError(ErrorCode::NamespaceAbsent,
                               "S3 namespace is not admitted");
    return position->second;
}

const PersistenceCore::Namespace& PersistenceCore::namespace_for(
    NamespaceId namespace_id) const {
    auto position = namespaces_.find(namespace_id);
    if (position == namespaces_.end() || !position->second.live)
        throw PersistenceError(ErrorCode::NamespaceAbsent,
                               "S3 namespace is not admitted");
    return position->second;
}

PersistenceCore::Object& PersistenceCore::object_for(Namespace& namespace_value,
                                                      std::string_view key) {
    auto position = namespace_value.objects.find(std::string(key));
    if (position == namespace_value.objects.end())
        throw PersistenceError(ErrorCode::InvalidTransition,
                               "S3 object is absent");
    return position->second;
}

const PersistenceCore::Object& PersistenceCore::object_for(
    const Namespace& namespace_value, std::string_view key) const {
    auto position = namespace_value.objects.find(std::string(key));
    if (position == namespace_value.objects.end())
        throw PersistenceError(ErrorCode::InvalidTransition,
                               "S3 object is absent");
    return position->second;
}

void PersistenceCore::admit(NamespaceId namespace_id) {
    ensure_mutable();
    auto [position, inserted] = namespaces_.try_emplace(namespace_id);
    if (!inserted && position->second.live)
        throw PersistenceError(ErrorCode::NamespaceAlreadyAdmitted,
                               "S3 namespace is already admitted");
    Namespace& value = position->second;
    value.id = namespace_id;
    value.last_touch = ++clock_;
    value.resident_bytes = 0;
    value.live = true;
    value.objects.clear();
}

void PersistenceCore::touch(NamespaceId namespace_id) {
    ensure_mutable();
    Namespace& value = namespace_for(namespace_id);
    value.last_touch = ++clock_;
}

void PersistenceCore::content_conflict(std::string_view key) const {
    // Sticky process-wide stop: neither eviction nor another generation may
    // hide an immutable-content violation from its owner.
    auto* self = const_cast<PersistenceCore*>(this);
    self->terminal_ = true;
    throw PersistenceError(
        ErrorCode::ContentConflict,
        "same S3 object key arrived with different immutable content: " +
            std::string(key));
}

bool PersistenceCore::namespace_evictable(const Namespace& value) const {
    if (!value.live) return false;
    for (const auto& [key, object] : value.objects) {
        (void)key;
        if (object.state == ObjectState::Installing || object.state == ObjectState::Pinned)
            return false;
    }
    return true;
}

bool PersistenceCore::evict_one_except(NamespaceId excluded) {
    Namespace* victim = nullptr;
    for (auto& [id, value] : namespaces_) {
        if (id == excluded || !namespace_evictable(value)) continue;
        if (!victim || value.last_touch < victim->last_touch ||
            (value.last_touch == victim->last_touch && id < victim->id))
            victim = &value;
    }
    if (!victim) return false;
    resident_bytes_ -= victim->resident_bytes;
    victim->resident_bytes = 0;
    victim->objects.clear();
    victim->live = false;
    return true;
}

void PersistenceCore::reserve_staging(uint64_t bytes) {
    if (add_overflows(staging_bytes_, bytes) ||
        staging_bytes_ + bytes > limits_.total_staging_bytes)
        throw PersistenceError(ErrorCode::CapacityExceeded,
                               "S3 staging byte cap exceeded");
    if (add_overflows(resident_bytes_, staging_bytes_ + bytes) ||
        resident_bytes_ + staging_bytes_ + bytes > limits_.total_simultaneous_bytes)
        throw PersistenceError(ErrorCode::CapacityExceeded,
                               "S3 simultaneous resident/staging byte cap exceeded");
    staging_bytes_ += bytes;
}

void PersistenceCore::reserve_resident(NamespaceId namespace_id, uint64_t bytes) {
    Namespace& target = namespace_for(namespace_id);
    if (add_overflows(target.resident_bytes, bytes) ||
        target.resident_bytes + bytes > limits_.namespace_resident_bytes)
        throw PersistenceError(ErrorCode::CapacityExceeded,
                               "S3 namespace resident byte cap exceeded");

    if (add_overflows(resident_bytes_, bytes) ||
        resident_bytes_ + bytes > limits_.total_resident_bytes) {
        std::vector<NamespaceId> candidates;
        for (const auto& [id, value] : namespaces_) {
            if (id == namespace_id || !namespace_evictable(value)) continue;
            candidates.push_back(id);
        }
        std::sort(candidates.begin(), candidates.end(), [&](NamespaceId left,
                                                             NamespaceId right) {
            const Namespace& lhs = namespaces_.at(left);
            const Namespace& rhs = namespaces_.at(right);
            return lhs.last_touch < rhs.last_touch ||
                   (lhs.last_touch == rhs.last_touch && left < right);
        });
        const uint64_t required =
            add_overflows(resident_bytes_, bytes)
                ? UINT64_MAX
                : resident_bytes_ + bytes - limits_.total_resident_bytes;
        uint64_t planned = 0;
        size_t victim_count = 0;
        for (NamespaceId id : candidates) {
            if (planned >= required) break;
            const uint64_t charge = namespaces_.at(id).resident_bytes;
            if (!add_overflows(planned, charge)) planned += charge;
            ++victim_count;
        }
        if (planned < required)
            throw PersistenceError(ErrorCode::CapacityExceeded,
                                   "S3 total resident byte cap has no eligible namespace");
        // Preflight above makes this loop atomic with respect to a rejected
        // request: every selected victim is known before the first is cleared.
        for (size_t index = 0; index != victim_count; ++index)
            if (!evict_one_except(namespace_id))
                throw std::logic_error("S3 eviction preflight lost its victim");
    }
    target.resident_bytes += bytes;
    resident_bytes_ += bytes;
}

BeginInstallResult PersistenceCore::begin_install(
    NamespaceId namespace_id, std::string_view key,
    std::span<const uint8_t> payload) {
    ensure_mutable();
    ensure_key(key);
    Namespace& namespace_value = namespace_for(namespace_id);
    const Digest128 digest = digest128(payload);
    auto position = namespace_value.objects.find(std::string(key));
    if (position != namespace_value.objects.end()) {
        Object& existing = position->second;
        if (existing.state == ObjectState::Present ||
            existing.state == ObjectState::Pinned) {
            if (existing.content_digest == digest && existing.payload.size() == payload.size() &&
                std::equal(existing.payload.begin(), existing.payload.end(), payload.begin()))
                return BeginInstallResult{true, {}};
            content_conflict(key);
        }
        if (existing.state == ObjectState::Installing) {
            if (existing.content_digest != digest) content_conflict(key);
            throw PersistenceError(ErrorCode::InvalidTransition,
                                   "S3 object install is already in progress");
        }
    }

    if constexpr (sizeof(size_t) > sizeof(uint64_t)) {
        if (payload.size() > std::numeric_limits<uint64_t>::max())
            throw PersistenceError(ErrorCode::CapacityExceeded,
                                   "S3 object payload exceeds addressable byte count");
    }
    const uint64_t bytes = static_cast<uint64_t>(payload.size());
    if (add_overflows(namespace_value.resident_bytes, bytes) ||
        namespace_value.resident_bytes + bytes > limits_.namespace_resident_bytes)
        throw PersistenceError(ErrorCode::CapacityExceeded,
                               "S3 namespace resident byte cap exceeded");
    reserve_staging(bytes);

    Object& object = namespace_value.objects[std::string(key)];
    object.state = ObjectState::Installing;
    object.content_digest = digest;
    object.payload.assign(payload.begin(), payload.end());
    object.ticket_serial = next_ticket_serial_++;
    object.pin_count = 0;
    return BeginInstallResult{
        false, InstallTicket{namespace_id, std::string(key), object.ticket_serial}};
}

PersistenceCore::Object& PersistenceCore::ticket_object(const InstallTicket& ticket) {
    Namespace& namespace_value = namespace_for(ticket.namespace_id);
    if (ticket.key.empty() || ticket.serial == 0)
        throw PersistenceError(ErrorCode::InvalidInstall,
                               "invalid S3 install ticket");
    Object& object = object_for(namespace_value, ticket.key);
    if (object.state != ObjectState::Installing || object.ticket_serial != ticket.serial)
        throw PersistenceError(ErrorCode::InvalidInstall,
                               "stale or already-consumed S3 install ticket");
    return object;
}

void PersistenceCore::publish(const InstallTicket& ticket) {
    ensure_mutable();
    Object& object = ticket_object(ticket);
    const uint64_t bytes = static_cast<uint64_t>(object.payload.size());
    Namespace& namespace_value = namespace_for(ticket.namespace_id);
    (void)namespace_value;
    // reserve_resident may evict another namespace, but never the installing
    // target. It runs before the state transition.
    reserve_resident(ticket.namespace_id, bytes);
    staging_bytes_ -= bytes;
    object.state = ObjectState::Present;
    object.ticket_serial = 0;
}

void PersistenceCore::crash_install(const InstallTicket& ticket) {
    ensure_mutable();
    Object& object = ticket_object(ticket);
    staging_bytes_ -= static_cast<uint64_t>(object.payload.size());
    Namespace& namespace_value = namespace_for(ticket.namespace_id);
    namespace_value.objects.erase(ticket.key);
}

void PersistenceCore::pin(NamespaceId namespace_id, std::string_view key) {
    ensure_mutable();
    ensure_key(key);
    Object& object = object_for(namespace_for(namespace_id), key);
    if (object.state == ObjectState::Installing || object.state == ObjectState::Absent)
        throw PersistenceError(ErrorCode::InvalidTransition,
                               "S3 PINNED requires PRESENT object");
    if (object.pin_count == UINT64_MAX)
        throw PersistenceError(ErrorCode::CapacityExceeded, "S3 pin count overflow");
    ++object.pin_count;
    object.state = ObjectState::Pinned;
}

void PersistenceCore::unpin(NamespaceId namespace_id, std::string_view key) {
    ensure_mutable();
    ensure_key(key);
    Object& object = object_for(namespace_for(namespace_id), key);
    if (object.state != ObjectState::Pinned || object.pin_count == 0)
        throw PersistenceError(ErrorCode::InvalidTransition,
                               "S3 UNPIN requires PINNED object");
    --object.pin_count;
    if (object.pin_count == 0) object.state = ObjectState::Present;
}

std::optional<NamespaceId> PersistenceCore::evict_lru() {
    ensure_mutable();
    Namespace* victim = nullptr;
    for (auto& [id, value] : namespaces_) {
        (void)id;
        if (!namespace_evictable(value)) continue;
        if (!victim || value.last_touch < victim->last_touch ||
            (value.last_touch == victim->last_touch && id < victim->id))
            victim = &value;
    }
    if (!victim) return std::nullopt;
    const NamespaceId id = victim->id;
    resident_bytes_ -= victim->resident_bytes;
    victim->resident_bytes = 0;
    victim->objects.clear();
    victim->live = false;
    return id;
}

std::optional<ObjectView> PersistenceCore::find(NamespaceId namespace_id,
                                                 std::string_view key) const {
    ensure_key(key);
    auto namespace_position = namespaces_.find(namespace_id);
    if (namespace_position == namespaces_.end() || !namespace_position->second.live)
        return std::nullopt;
    const Namespace& namespace_value = namespace_position->second;
    auto position = namespace_value.objects.find(std::string(key));
    if (position == namespace_value.objects.end()) return std::nullopt;
    const Object& object = position->second;
    return ObjectView{object.state, object.content_digest, object.payload};
}

ObjectState PersistenceCore::state(NamespaceId namespace_id,
                                   std::string_view key) const {
    ensure_key(key);
    auto namespace_position = namespaces_.find(namespace_id);
    if (namespace_position == namespaces_.end() || !namespace_position->second.live)
        return ObjectState::Absent;
    const Namespace& namespace_value = namespace_position->second;
    auto position = namespace_value.objects.find(std::string(key));
    return position == namespace_value.objects.end() ? ObjectState::Absent
                                                      : position->second.state;
}

bool PersistenceCore::has_namespace(NamespaceId namespace_id) const {
    auto position = namespaces_.find(namespace_id);
    return position != namespaces_.end() && position->second.live;
}

size_t PersistenceCore::live_namespace_count() const noexcept {
    size_t count = 0;
    for (const auto& [id, value] : namespaces_) {
        (void)id;
        if (value.live) ++count;
    }
    return count;
}

uint64_t PersistenceCore::namespace_resident_bytes(NamespaceId namespace_id) const {
    return namespace_for(namespace_id).resident_bytes;
}

}  // namespace icecc::p50::s3
