#include "cache/p50_slice0.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace icecc::p50;

namespace {

using RouteBeginV1 = decltype(&CRoute::begin_v1);
static_assert(std::is_invocable_r_v<const CActiveTx&, RouteBeginV1, CRoute&,
                                    const PreparedTUPtr&, uint64_t>);

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

template <class Exception = std::exception, class Function>
void require_throws(Function&& function, const char* message) {
    try {
        function();
    } catch (const Exception&) {
        return;
    }
    require(false, message);
}

std::vector<uint8_t> bytes(std::string_view value) {
    return {value.begin(), value.end()};
}

Digest128 tagged_digest(uint8_t tag) {
    Digest128 result{};
    result.bytes.front() = tag;
    return result;
}

void test_object_arena_and_canonical_records() {
    CObjectArena arena(CStoreGuid::from_u64(1));
    const auto alpha = bytes("alpha\n");
    const Key64 line = arena.intern_bytes(ObjectType::Line, alpha);
    require(arena.intern_bytes(ObjectType::Line, alpha) == line,
            "C object arena did not intern equal byte objects");

    const std::array<Key64, 1> children{line};
    const Key64 region = arena.intern_children(ObjectType::Region, children);
    require(region.type() == ObjectType::Region && region.generation() == 0,
            "C object arena assigned the wrong region identity");

    const ImmutableObject& object = arena.object(region);
    const FillRecord record = object.fill_record();
    require(ImmutableObject::from_record(record) == object,
            "canonical immutable-object record did not round trip");

    ImmutableObjectStore store;
    require(store.apply(object) == ObjectApplyResult::Applied &&
                store.apply(object) == ObjectApplyResult::Duplicate &&
                store.size() == 1,
            "immutable object store did not distinguish apply from duplicate");
    require_throws<std::logic_error>(
        [&] {
            store.apply(ImmutableObject::children(
                region, std::span<const Key64>{}));
        },
        "immutable object store accepted one key for different content");

    require(arena.advance_generation() == GenerationAdvanceResult::Advanced &&
                arena.generation() == 1,
            "ordinary object generation did not advance");
    const Key64 next = arena.intern_bytes(ObjectType::Line, bytes("next\n"));
    require(next.generation() == 1 && next != line,
            "generation advance reused an old Key64");

    CObjectArena terminal(CStoreGuid::from_u64(2),
                          KeyLayoutV1::generation_value_mask,
                          KeyLayoutV1::ordinal_mask);
    (void)terminal.intern_bytes(ObjectType::Line, bytes("last"));
    require_throws<std::overflow_error>(
        [&] { (void)terminal.intern_bytes(ObjectType::Line, bytes("overflow")); },
        "object ordinal exhaustion wrapped");
    require(terminal.advance_generation() ==
                GenerationAdvanceResult::GuidFlipRequired,
            "terminal generation did not require a GUID flip");
}

void test_global_resource_owner() {
    const GlobalResourceLimits limits{
        .max_aggregate_bytes = 8,
        .max_namespace_bytes = 8,
        .max_staging_bytes = 8,
        .max_total_bytes = 16,
        .max_generation = 1,
        .max_staging_slots = 2,
    };
    GlobalResourceTrace trace;
    GlobalResourceModel model(limits, {}, &trace);
    const CStoreGuid first = CStoreGuid::from_u64(10);
    const CStoreGuid second = CStoreGuid::from_u64(11);
    const Key64 first_key = *Key64::make(ObjectType::Line, 0, 1);
    const Key64 second_key = *Key64::make(ObjectType::Line, 0, 2);

    model.admit(first);
    model.admit(second);
    model.touch(first);
    model.touch(second);
    model.start_tu(first);
    model.start_tu(second);
    model.begin_install(first, first_key, tagged_digest(1), 3, 0);
    model.begin_install(second, second_key, tagged_digest(2), 2, 1);
    require(model.staging_bytes() == 5 && model.free_staging_slots() == 0,
            "global owner did not account shared staging resources");
    model.publish(first, first_key, 0, tagged_digest(1));
    model.publish(second, second_key, 1, tagged_digest(2));
    model.pin(first, first_key);
    model.finish_tu(first);
    model.finish_tu(second);
    require(model.resident_bytes() == 5 && !model.check_invariants(),
            "global owner rejected valid resident state");
    require(model.evict_oldest() == first,
            "global owner did not evict the least-recently-used namespace");
    require(model.resident_bytes() == 2 && !trace.records().empty(),
            "global eviction did not update accounting or trace");

    model.release(second, second_key);
    require(model.resident_bytes() == 0 && !model.check_invariants(),
            "global release left resident bytes behind");
}

void test_atomic_p29v1_pair_preflight() {
    const CStoreGuid c_guid = CStoreGuid::from_u64(20);
    const Key64 blob = *Key64::make(ObjectType::Blob, 0, 7);
    const Key64 segment = *Key64::make(ObjectType::P29Segment, 0, 7);
    const Digest128 blob_digest = tagged_digest(3);
    const Digest128 segment_digest = tagged_digest(4);

    GlobalResourceLimits limits{
        .max_aggregate_bytes = 7,
        .max_namespace_bytes = 7,
        .max_staging_bytes = 8,
        .max_total_bytes = 15,
        .max_generation = 1,
        .max_staging_slots = 2,
    };
    GlobalResourceModel rejected(limits);
    rejected.admit(c_guid);
    rejected.start_tu(c_guid);
    rejected.begin_install(c_guid, blob, blob_digest, 4, 0);
    rejected.begin_install(c_guid, segment, segment_digest, 4, 1);
    require_throws<std::length_error>(
        [&] {
            rejected.preflight_publish_pair(
                c_guid, segment, 1, segment_digest, blob, 0, blob_digest);
        },
        "P29V1 pair preflight admitted a partial-publication capacity failure");
    require(rejected.resident_bytes() == 0 && rejected.staging_bytes() == 8 &&
                !rejected.check_invariants(),
            "failed P29V1 pair preflight changed visible state");

    limits.max_aggregate_bytes = 8;
    limits.max_namespace_bytes = 8;
    GlobalResourceModel accepted(limits);
    accepted.admit(c_guid);
    accepted.start_tu(c_guid);
    accepted.begin_install(c_guid, blob, blob_digest, 4, 0);
    accepted.begin_install(c_guid, segment, segment_digest, 4, 1);
    accepted.preflight_publish_pair(
        c_guid, segment, 1, segment_digest, blob, 0, blob_digest);
    accepted.publish(c_guid, segment, 1, segment_digest);
    accepted.publish(c_guid, blob, 0, blob_digest);
    accepted.finish_tu(c_guid);
    accepted.release(c_guid, blob);
    require(accepted.resident_bytes() == 4 && !accepted.check_invariants(),
            "P29V1 route segment did not survive collected input release");
    accepted.release(c_guid, segment);
}

void test_global_reverse_invariants_catch_mutants() {
    const GlobalResourceLimits limits{
        .max_aggregate_bytes = 8,
        .max_namespace_bytes = 8,
        .max_staging_bytes = 8,
        .max_total_bytes = 16,
        .max_generation = 1,
        .max_staging_slots = 2,
    };
    const CStoreGuid first = CStoreGuid::from_u64(30);
    const CStoreGuid second = CStoreGuid::from_u64(31);
    const Key64 first_key = *Key64::make(ObjectType::Line, 0, 1);
    const Key64 second_key = *Key64::make(ObjectType::Line, 0, 2);

    GlobalResourceFaults faults;
    faults.ignore_slot_ownership = true;
    GlobalResourceModel mutant(limits, faults);
    mutant.admit(first);
    mutant.admit(second);
    mutant.start_tu(first);
    mutant.start_tu(second);
    mutant.begin_install(first, first_key, tagged_digest(5), 2, 0);
    mutant.begin_install(second, second_key, tagged_digest(6), 2, 0);
    const auto violation = mutant.check_invariants();
    require(violation && violation->find("staging slot") != std::string::npos,
            "reverse invariant missed duplicate staging-slot ownership");
}

void test_f_store_session_and_route_fencing() {
    ActionTrace trace;
    const CStoreGuid c_guid = CStoreGuid::from_u64(40);
    FStore store(FStoreGuid::from_u64(41), 1, &trace, 4096, true);
    const SessionHandle first = store.connect(c_guid);
    require(!store.resume(first).namespace_present &&
                !store.resume(first).route_present,
            "cold F namespace was not canonical absent state");

    const HistoryNonce nonce{42};
    require_throws<std::logic_error>(
        [&] { store.start_route(first, nonce, Digest128{}); },
        "F accepted a HISTORY_RESET with a non-derived digest");
    store.start_route(first, nonce, initial_route_digest(c_guid, nonce));
    const SessionState established = store.resume(first);
    require(established.namespace_present && established.route_present &&
                established.history_nonce == nonce &&
                established.next_rel_seq == RelSeq{0},
            "F route did not expose its established cursor");

    TxBegin wrong_profile;
    wrong_profile.history_nonce = nonce;
    wrong_profile.profile = ProfileId::ZSTD_TU;
    wrong_profile.pre_state_digest = established.state_digest;
    require_throws<std::invalid_argument>(
        [&] { store.begin(first, wrong_profile); },
        "P29V1 F store admitted another profile");

    const SessionHandle replacement = store.connect(c_guid);
    require_throws<std::logic_error>([&] { (void)store.resume(first); },
                                     "replaced F session remained live");
    require(store.resume(replacement).route_present,
            "session replacement discarded committed route state");
    store.forget_route(replacement);
    require(store.resume(replacement).namespace_present &&
                !store.resume(replacement).route_present,
            "route forget discarded the C namespace");

    store.destructive_cache_reset(FStoreGuid::from_u64(43));
    require_throws<std::logic_error>(
        [&] { (void)store.resume(replacement); },
        "old session survived F_STORE_GUID reset");
    require(trace.records().size() >= 3,
            "F session transitions did not emit canonical actions");
}

void test_authority_and_route_fail_closed_before_enablement() {
    CAuthority authority(CStoreGuid::from_u64(50));
    require(authority.guid() == CStoreGuid::from_u64(50) &&
                !authority.p29v1_runnable() &&
                authority.p29v1_interner_reserved_bytes() == 0,
            "bare C authority unexpectedly enabled P29V1");

    CRoute route(authority, FStoreGuid::from_u64(51), HistoryNonce{52});
    require(route.next_rel_seq() == RelSeq{0} && !route.active() &&
                route.state_digest() ==
                    initial_route_digest(authority.guid(), HistoryNonce{52}),
            "new C route did not use the canonical cursor");
    require_throws<std::invalid_argument>(
        [&] { (void)route.begin_v1({}, 4096); },
        "C route accepted a null PreparedTU");
    require_throws<std::logic_error>(
        [&] { route.pin_v1_system_source_reuse(true); },
        "C route pinned reuse before its P29V1 codec existed");
}

void test_terminal_session_serial() {
    FStore store(FStoreGuid::from_u64(60),
                 std::numeric_limits<uint64_t>::max());
    const CStoreGuid c_guid = CStoreGuid::from_u64(61);
    const SessionHandle last = store.connect(c_guid);
    require(last.serial == std::numeric_limits<uint64_t>::max(),
            "last F session serial was not allocated");
    require_throws<std::overflow_error>(
        [&] { (void)store.connect(c_guid); },
        "F session serial wrapped after exhaustion");
    store.destructive_cache_reset(FStoreGuid::from_u64(62));
    require(store.connect(c_guid).serial == 1,
            "new F store incarnation did not reset session serial space");
}

}  // namespace

int main() {
    test_object_arena_and_canonical_records();
    test_global_resource_owner();
    test_atomic_p29v1_pair_preflight();
    test_global_reverse_invariants_catch_mutants();
    test_f_store_session_and_route_fencing();
    test_authority_and_route_fail_closed_before_enablement();
    test_terminal_session_serial();
    return 0;
}
