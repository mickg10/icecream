#include "cache/s3_persistence.h"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

using namespace icecc::p50;
using namespace icecc::p50::s3;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "s3_persistence_test: " << message << '\n';
    std::exit(1);
}

void require(bool value, std::string_view message) {
    if (!value) fail(message);
}

template <class Callable>
void require_error(Callable&& callable, ErrorCode expected, std::string_view message) {
    try {
        callable();
    } catch (const PersistenceError& error) {
        if (error.code() != expected) fail(message);
        return;
    } catch (...) {
        fail(message);
    }
    fail(message);
}

std::vector<uint8_t> bytes(std::string_view value) {
    return {value.begin(), value.end()};
}

NamespaceId ns(uint64_t guid, uint16_t generation = 0) {
    return NamespaceId{Id128::from_u64(guid), generation};
}

void state_machine_and_retry_reclaims_staging() {
    PersistenceCore core(Limits{20, 20, 20, 30});
    const NamespaceId id = ns(1);
    core.admit(id);

    const auto payload = bytes("immutable");
    const auto install = core.begin_install(id, "k", payload);
    require(!install.already_present, "first install was treated as a hit");
    require(core.find(id, "k")->state == ObjectState::Installing,
            "ABSENT did not transition to INSTALLING");
    require(core.staging_bytes() == payload.size(), "INSTALLING bytes were not reserved");

    core.crash_install(install.ticket);
    require(core.state(id, "k") == ObjectState::Absent,
            "crash did not return key to ABSENT");
    require(core.staging_bytes() == 0, "crash leaked staged bytes");

    const auto retry = core.begin_install(id, "k", payload);
    core.publish(retry.ticket);
    require_error([&] { core.publish(install.ticket); }, ErrorCode::InvalidInstall,
                  "a crashed install ticket was accepted after retry");
    require(core.find(id, "k")->state == ObjectState::Present,
            "INSTALLING did not transition to PRESENT");
    require(core.resident_bytes() == payload.size(), "publish did not charge resident bytes");
    require(core.staging_bytes() == 0, "publish retained staging bytes");

    core.pin(id, "k");
    require(core.find(id, "k")->state == ObjectState::Pinned,
            "PRESENT did not transition to PINNED");
    core.pin(id, "k");
    core.unpin(id, "k");
    require(core.find(id, "k")->state == ObjectState::Pinned,
            "first lease release cleared a second pin");
    core.unpin(id, "k");
    require(core.find(id, "k")->state == ObjectState::Present,
            "last lease release did not return to PRESENT");
}

void immutable_conflict_is_sticky_and_same_content_is_idempotent() {
    PersistenceCore core;
    const NamespaceId id = ns(2);
    core.admit(id);
    const auto first = bytes("one");
    const auto second = bytes("two");
    core.publish(core.begin_install(id, "same-key", first).ticket);
    const auto duplicate = core.begin_install(id, "same-key", first);
    require(duplicate.already_present, "same immutable content was not idempotent");
    require_error([&] { (void)core.begin_install(id, "same-key", second); },
                  ErrorCode::ContentConflict,
                  "same-key/different-content was not rejected fatally");
    require(core.terminal(), "content conflict did not enter terminal state");
    require_error([&] { core.touch(id); }, ErrorCode::Terminal,
                  "terminal conflict allowed a later mutation");
    const auto view = core.find(id, "same-key");
    require(view && view->payload == first, "conflict modified immutable content");
}

void whole_namespace_global_lru_evicts_only_idle_namespace() {
    PersistenceCore core(Limits{6, 4, 6, 10});
    const NamespaceId old_id = ns(3);
    const NamespaceId new_id = ns(4);
    const NamespaceId incoming_id = ns(5);
    core.admit(old_id);
    core.admit(new_id);
    core.touch(old_id);
    core.touch(new_id);

    const auto old_payload = bytes("old");
    const auto new_payload = bytes("nw");
    core.publish(core.begin_install(old_id, "old", old_payload).ticket);
    core.publish(core.begin_install(new_id, "new", new_payload).ticket);
    core.touch(old_id);  // new_id is now the oldest eligible namespace.

    core.admit(incoming_id);
    const auto incoming = bytes("go");
    core.publish(core.begin_install(incoming_id, "incoming", incoming).ticket);
    require(!core.has_namespace(new_id), "resident cap did not evict whole LRU namespace");
    require(core.has_namespace(old_id) && core.has_namespace(incoming_id),
            "LRU eviction removed the wrong namespace");
    require(core.find(old_id, "old")->payload == old_payload,
            "whole-namespace eviction damaged a surviving namespace");
    require(core.resident_bytes() == old_payload.size() + incoming.size(),
            "resident accounting after namespace eviction is wrong");
    require(core.state(new_id, "new") == ObjectState::Absent,
            "whole-namespace eviction retained an object state");

    // A pinned namespace is never a victim.  With the only unpinned namespace
    // already removed, the request fails closed instead of partial eviction.
    PersistenceCore blocked(Limits{3, 3, 10, 10});
    const NamespaceId resident = ns(6);
    const NamespaceId pinned = ns(7);
    blocked.admit(resident);
    blocked.admit(pinned);
    blocked.publish(blocked.begin_install(resident, "r", bytes("1")).ticket);
    blocked.publish(blocked.begin_install(pinned, "p", bytes("22")).ticket);
    blocked.pin(pinned, "p");
    const auto candidate = blocked.begin_install(resident, "candidate", bytes("33"));
    require_error([&] { blocked.publish(candidate.ticket); }, ErrorCode::CapacityExceeded,
                  "pinned namespace was used as an eviction victim");
    require(blocked.find(pinned, "p")->state == ObjectState::Pinned,
            "failed capacity check changed pinned state");
}

void generation_is_part_of_arena_identity() {
    PersistenceCore core;
    const NamespaceId generation_zero = ns(8, 0);
    const NamespaceId generation_one = ns(8, 1);
    core.admit(generation_zero);
    core.admit(generation_one);
    const auto zero = bytes("generation-zero");
    const auto one = bytes("generation-one");
    core.publish(core.begin_install(generation_zero, "same", zero).ticket);
    core.publish(core.begin_install(generation_one, "same", one).ticket);
    require(core.find(generation_zero, "same")->payload == zero,
            "generation zero arena was aliased");
    require(core.find(generation_one, "same")->payload == one,
            "generation one arena was aliased");
}

}  // namespace

int main() {
    state_machine_and_retry_reclaims_staging();
    immutable_conflict_is_sticky_and_same_content_is_idempotent();
    whole_namespace_global_lru_evicts_only_idle_namespace();
    generation_is_part_of_arena_identity();
}
