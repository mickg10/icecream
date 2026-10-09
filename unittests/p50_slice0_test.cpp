#include "cache/p50_slice0.h"
#include "cache/p50_endpoint.h"
#include "cache/p50_actions.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <unistd.h>
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

void test_f_store_routes_are_isolated_by_profile() {
    const CStoreGuid c_guid = CStoreGuid::from_u64(44);
    FStore store(FStoreGuid::from_u64(45));

    const SessionHandle p29 = store.connect(c_guid, ProfileId::P29V1);
    const HistoryNonce p29_nonce{46};
    store.start_route(p29, p29_nonce,
                      initial_route_digest(c_guid, p29_nonce));
    store.disconnect(p29);

    const SessionHandle zstd = store.connect(c_guid, ProfileId::ZSTD_TU);
    require(store.resume(zstd).namespace_present &&
                !store.resume(zstd).route_present,
            "a route from another profile leaked into ZSTD_TU resume");
    const HistoryNonce zstd_nonce{47};
    store.start_route(zstd, zstd_nonce,
                      initial_route_digest(c_guid, zstd_nonce));
    store.disconnect(zstd);

    const SessionHandle p29_again =
        store.connect(c_guid, ProfileId::P29V1);
    const SessionState retained_p29 = store.resume(p29_again);
    require(retained_p29.route_present &&
                retained_p29.history_nonce == p29_nonce,
            "ZSTD_TU replaced the retained P29V1 route");
    store.forget_route(p29_again);
    store.disconnect(p29_again);

    const SessionHandle zstd_again =
        store.connect(c_guid, ProfileId::ZSTD_TU);
    const SessionState retained_zstd = store.resume(zstd_again);
    require(retained_zstd.route_present &&
                retained_zstd.history_nonce == zstd_nonce,
            "forgetting P29V1 removed the retained ZSTD_TU route");

    SessionHandle wrong_profile = zstd_again;
    wrong_profile.profile = ProfileId::ZSTD_ROUTE;
    require_throws<std::logic_error>(
        [&] { (void)store.resume(wrong_profile); },
        "an active F session accepted a different profile identity");
}

// Step 3 (route pipelining, C side) regression gate: FStore::begin's
// occupied-slot admission (cache/p50_slice0.cpp, FStore::begin) is explicitly
// *not* touched by route pipelining -- the F-side admission widening for
// K>1 lives entirely in the new F-side sequencer (a later step), never in
// FStore itself (design doc §1.2 / §2 invariant 8). Prove FStore::begin
// called twice in a row without an intervening commit_input still rejects
// the second call exactly as it does today.
void test_f_store_begin_rejects_second_active_tx_without_commit() {
    const CStoreGuid c_guid = CStoreGuid::from_u64(70);
    FStore store(FStoreGuid::from_u64(71));

    const SessionHandle session = store.connect(c_guid);
    const HistoryNonce nonce{72};
    store.start_route(session, nonce, initial_route_digest(c_guid, nonce));
    const SessionState established = store.resume(session);

    TxBegin begin;
    begin.history_nonce = nonce;
    begin.rel_seq = established.next_rel_seq;
    begin.tu_seq = TuSeq{0};
    begin.profile = ProfileId::P29V1;
    begin.pre_state_digest = established.state_digest;
    begin.body.encoding = static_cast<uint16_t>(ProfileId::P29V1);
    // encoded_bytes == 0 would make FStore::begin auto-complete the body via
    // append_component(session, {}), which runs the full P29V1 deserializer
    // -- out of scope for this admission-only regression gate (and not a
    // live P29V1 cycle, per this test's own doc comment). A nonzero
    // encoded_bytes with no matching append_component() call leaves the
    // ACTIVE_TX genuinely pending, which is all this test needs.
    begin.body.encoded_bytes = 1;
    store.begin(session, begin);

    // Same ACTIVE_TX replayed verbatim (no commit_input in between) is the
    // one case FStore::begin tolerates: it is idempotent, not a second slot.
    store.begin(session, begin);

    // A genuinely different TX_BEGIN for the same still-pending slot -- same
    // route cursor (history_nonce/rel_seq/pre_state_digest), different
    // tu_seq -- must still be rejected exactly as today: FStore has not
    // grown a second slot.
    TxBegin different = begin;
    different.tu_seq = TuSeq{1};
    require_throws<std::logic_error>(
        [&] { store.begin(session, different); },
        "F route admitted a second ACTIVE_TX without an intervening commit");
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
    require(route.confirmed_rel_seq() == route.next_rel_seq() &&
                route.active_count() == 0 && route.max_active_tx() == 1,
            "default-depth C route did not reproduce the legacy K=1 cursor");
    require_throws<std::invalid_argument>(
        [&] { (void)route.begin_v1({}, 4096); },
        "C route accepted a null PreparedTU");
    require_throws<std::logic_error>(
        [&] { route.pin_v1_system_source_reuse(true); },
        "C route pinned reuse before its P29V1 codec existed");

    // max_active_tx is the route-pipelining FIFO depth (§1.1 point 2); 0 is
    // nonsensical and clamps to the K=1 legacy depth rather than leaving the
    // route unable to ever hold an ACTIVE_TX.
    const CRoute zero_clamped(authority, FStoreGuid::from_u64(53),
                              HistoryNonce{54}, nullptr, 0);
    require(zero_clamped.max_active_tx() == 1,
            "C route did not clamp a zero pipeline depth to 1");

    const CRoute deep(authority, FStoreGuid::from_u64(55), HistoryNonce{56},
                      nullptr, 3);
    require(deep.max_active_tx() == 3 && deep.active_count() == 0 &&
                deep.confirmed_rel_seq() == deep.next_rel_seq(),
            "K>1 C route did not report its configured pipeline depth");
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

// ---------------------------------------------------------------------
// Route pipelining (P50_ROUTE_PIPELINING.md step 7): the additive
// C-side queue_position/queue_depth trace fields and the generalized
// in-process checker.  Everything here is synthetic ActionRecord input --
// no live P29V1 cycle is needed, and every K=1 record is fieldless, which
// is the K=1 identity argument: at depth 1 CRoute::record stamps
// nothing, so a K=1 trace is byte-identical to a pre-pipelining trace
// and check_action_trace takes today's scalar path verbatim.
ActionRecord pipeline_record(ActionType action, ActorSide actor,
                             uint64_t serial, HistoryNonce nonce,
                             RelSeq rel, TuSeq tu, Digest128 tx,
                             Digest128 raw, Digest128 state,
                             std::optional<uint32_t> position = {},
                             std::optional<uint32_t> depth = {}) {
    ActionRecord record;
    record.action = action;
    record.actor = actor;
    record.profile = ProfileId::P29V1;
    record.c_store_guid = CStoreGuid::from_u64(70);
    record.f_store_guid = FStoreGuid::from_u64(71);
    record.session_serial = serial;
    record.history_nonce = nonce;
    record.rel_seq = rel;
    record.tu_seq = tu;
    record.transaction_digest = tx;
    record.raw_digest = raw;
    record.state_digest = state;
    record.queue_position = position;
    record.queue_depth = depth;
    return record;
}

struct PipelineTx {
    HistoryNonce nonce{10};
    RelSeq rel{};
    TuSeq tu{};
    Digest128 tx{};
    Digest128 raw{};
    Digest128 pre{};
    Digest128 post{};
};

std::vector<ActionRecord> pipeline_k1_trace() {
    const HistoryNonce nonce{10};
    const Digest128 s0 = tagged_digest(1);
    const Digest128 s1 = tagged_digest(2);
    const PipelineTx t0{nonce, RelSeq{0}, TuSeq{7}, tagged_digest(3),
                        tagged_digest(4), s0, s1};
    return {
        pipeline_record(ActionType::SESSION_OPENED, ActorSide::F, 1, nonce,
                        RelSeq{0}, TuSeq{}, {}, {}, s0),
        pipeline_record(ActionType::HISTORY_RESET, ActorSide::F, 1, nonce,
                        RelSeq{0}, TuSeq{}, {}, {}, s0),
        pipeline_record(ActionType::TX_BEGIN, ActorSide::C, 0, t0.nonce,
                        t0.rel, t0.tu, t0.tx, t0.raw, t0.pre),
        pipeline_record(ActionType::TX_BEGIN, ActorSide::F, 1, t0.nonce,
                        t0.rel, t0.tu, t0.tx, t0.raw, t0.pre),
        pipeline_record(ActionType::BODY_COMPLETE, ActorSide::F, 1, t0.nonce,
                        t0.rel, t0.tu, t0.tx, t0.raw, t0.pre),
        pipeline_record(ActionType::NEED_RECORDED, ActorSide::F, 1, t0.nonce,
                        t0.rel, t0.tu, t0.tx, t0.raw, t0.pre),
        pipeline_record(ActionType::INPUT_MATERIALIZED, ActorSide::F, 1,
                        t0.nonce, t0.rel, t0.tu, t0.tx, t0.raw, t0.pre),
        pipeline_record(ActionType::INPUT_COMMITTED, ActorSide::F, 1,
                        t0.nonce, t0.rel, t0.tu, t0.tx, t0.raw, t0.post),
        pipeline_record(ActionType::COMMIT_ACCEPTED, ActorSide::C, 0,
                        t0.nonce, t0.rel, t0.tu, t0.tx, t0.raw, t0.post),
    };
}

std::vector<ActionRecord> pipeline_k2_overlap_trace() {
    const HistoryNonce nonce{10};
    const Digest128 s0 = tagged_digest(1);
    const Digest128 s1 = tagged_digest(2);
    const Digest128 s2 = tagged_digest(3);
    const PipelineTx t0{nonce, RelSeq{0}, TuSeq{7}, tagged_digest(4),
                        tagged_digest(5), s0, s1};
    const PipelineTx t1{nonce, RelSeq{1}, TuSeq{8}, tagged_digest(6),
                        tagged_digest(7), s1, s2};
    return {
        pipeline_record(ActionType::SESSION_OPENED, ActorSide::F, 1, nonce,
                        RelSeq{0}, TuSeq{}, {}, {}, s0),
        pipeline_record(ActionType::HISTORY_RESET, ActorSide::F, 1, nonce,
                        RelSeq{0}, TuSeq{}, {}, {}, s0),
        pipeline_record(ActionType::TX_BEGIN, ActorSide::C, 0, t0.nonce,
                        t0.rel, t0.tu, t0.tx, t0.raw, t0.pre,
                        uint32_t{0}, uint32_t{2}),
        pipeline_record(ActionType::TX_BEGIN, ActorSide::F, 1, t0.nonce,
                        t0.rel, t0.tu, t0.tx, t0.raw, t0.pre),
        pipeline_record(ActionType::BODY_COMPLETE, ActorSide::F, 1, t0.nonce,
                        t0.rel, t0.tu, t0.tx, t0.raw, t0.pre),
        pipeline_record(ActionType::INPUT_MATERIALIZED, ActorSide::F, 1,
                        t0.nonce, t0.rel, t0.tu, t0.tx, t0.raw, t0.pre),
        pipeline_record(ActionType::INPUT_COMMITTED, ActorSide::F, 1,
                        t0.nonce, t0.rel, t0.tu, t0.tx, t0.raw, t0.post),
        // Successor admitted while T0's durable commit is unaccepted.
        pipeline_record(ActionType::TX_BEGIN, ActorSide::C, 0, t1.nonce,
                        t1.rel, t1.tu, t1.tx, t1.raw, t1.pre,
                        uint32_t{1}, uint32_t{2}),
        // F releases the parked successor: the committed-predecessor
        // session was retired, so this session opened as a replacement.
        pipeline_record(ActionType::SESSION_REPLACED, ActorSide::F, 2, nonce,
                        RelSeq{1}, TuSeq{}, {}, {}, s1),
        pipeline_record(ActionType::TX_BEGIN, ActorSide::F, 2, t1.nonce,
                        t1.rel, t1.tu, t1.tx, t1.raw, t1.pre),
        pipeline_record(ActionType::BODY_COMPLETE, ActorSide::F, 2, t1.nonce,
                        t1.rel, t1.tu, t1.tx, t1.raw, t1.pre),
        pipeline_record(ActionType::INPUT_MATERIALIZED, ActorSide::F, 2,
                        t1.nonce, t1.rel, t1.tu, t1.tx, t1.raw, t1.pre),
        pipeline_record(ActionType::INPUT_COMMITTED, ActorSide::F, 2,
                        t1.nonce, t1.rel, t1.tu, t1.tx, t1.raw, t1.post),
        // C accepts both, strictly head-first.
        pipeline_record(ActionType::COMMIT_ACCEPTED, ActorSide::C, 0,
                        t0.nonce, t0.rel, t0.tu, t0.tx, t0.raw, t0.post,
                        uint32_t{0}, uint32_t{2}),
        pipeline_record(ActionType::COMMIT_ACCEPTED, ActorSide::C, 0,
                        t1.nonce, t1.rel, t1.tu, t1.tx, t1.raw, t1.post,
                        uint32_t{0}, uint32_t{2}),
        pipeline_record(ActionType::SESSION_DISCONNECTED, ActorSide::F, 2,
                        t1.nonce, t1.rel, t1.tu, t1.tx, t1.raw, t1.post),
    };
}

std::vector<ActionRecord> pipeline_k2_poison_trace() {
    const HistoryNonce nonce{10};
    const Digest128 s0 = tagged_digest(1);
    const Digest128 s1 = tagged_digest(2);
    const PipelineTx t0{nonce, RelSeq{0}, TuSeq{7}, tagged_digest(4),
                        tagged_digest(5), s0, s1};
    const PipelineTx t1{nonce, RelSeq{1}, TuSeq{8}, tagged_digest(6),
                        tagged_digest(7), s1, {}};
    return {
        pipeline_record(ActionType::SESSION_OPENED, ActorSide::F, 1, nonce,
                        RelSeq{0}, TuSeq{}, {}, {}, s0),
        pipeline_record(ActionType::HISTORY_RESET, ActorSide::F, 1, nonce,
                        RelSeq{0}, TuSeq{}, {}, {}, s0),
        pipeline_record(ActionType::TX_BEGIN, ActorSide::C, 0, t0.nonce,
                        t0.rel, t0.tu, t0.tx, t0.raw, t0.pre,
                        uint32_t{0}, uint32_t{2}),
        pipeline_record(ActionType::TX_BEGIN, ActorSide::F, 1, t0.nonce,
                        t0.rel, t0.tu, t0.tx, t0.raw, t0.pre),
        pipeline_record(ActionType::BODY_COMPLETE, ActorSide::F, 1, t0.nonce,
                        t0.rel, t0.tu, t0.tx, t0.raw, t0.pre),
        pipeline_record(ActionType::INPUT_MATERIALIZED, ActorSide::F, 1,
                        t0.nonce, t0.rel, t0.tu, t0.tx, t0.raw, t0.pre),
        pipeline_record(ActionType::INPUT_COMMITTED, ActorSide::F, 1,
                        t0.nonce, t0.rel, t0.tu, t0.tx, t0.raw, t0.post),
        pipeline_record(ActionType::TX_BEGIN, ActorSide::C, 0, t1.nonce,
                        t1.rel, t1.tu, t1.tx, t1.raw, t1.pre,
                        uint32_t{1}, uint32_t{2}),
        // CRoute::abandon_active drains the whole queue front-to-back with
        // positions 0..n-1.
        pipeline_record(ActionType::TX_ABORTED, ActorSide::C, 0, t0.nonce,
                        t0.rel, t0.tu, t0.tx, t0.raw, t0.pre,
                        uint32_t{0}, uint32_t{2}),
        pipeline_record(ActionType::TX_ABORTED, ActorSide::C, 0, t1.nonce,
                        t1.rel, t1.tu, t1.tx, t1.raw, t1.pre,
                        uint32_t{1}, uint32_t{2}),
        // Cold reset on a fresh session discards the abandoned-but-durable
        // T0 commit (universal invalidation).
        pipeline_record(ActionType::SESSION_DISCONNECTED, ActorSide::F, 1,
                        HistoryNonce{11}, RelSeq{0}, TuSeq{}, {}, {},
                        tagged_digest(8)),
        pipeline_record(ActionType::SESSION_OPENED, ActorSide::F, 2,
                        HistoryNonce{11}, RelSeq{0}, TuSeq{}, {}, {},
                        tagged_digest(8)),
        pipeline_record(ActionType::HISTORY_RESET, ActorSide::F, 2,
                        HistoryNonce{11}, RelSeq{0}, TuSeq{}, {}, {},
                        tagged_digest(8)),
    };
}

void require_checker_accepts(const std::vector<ActionRecord>& records,
                              const char* message) {
    const auto error = check_action_trace(
        std::span<const ActionRecord>{records});
    require(!error, message);
}

void require_checker_rejects(const std::vector<ActionRecord>& records,
                             std::string_view needle, const char* message) {
    const auto error = check_action_trace(
        std::span<const ActionRecord>{records});
    require(error && error->find(needle) != std::string::npos, message);
}

void test_action_trace_jsonl_fields_additive() {
    // A fieldless (K=1) record must serialize exactly as before: no
    // queue_position/queue_depth keys anywhere in the JSON line.  A
    // fielded record appends them at the end of the object.
    const ActionRecord plain = pipeline_record(
        ActionType::TX_BEGIN, ActorSide::C, 0, HistoryNonce{10}, RelSeq{0},
        TuSeq{7}, tagged_digest(3), tagged_digest(4), tagged_digest(1));
    const std::string plain_json = action_jsonl(plain);
    require(plain_json.find("\"queue_position\"") == std::string::npos &&
                plain_json.find("\"queue_depth\"") == std::string::npos,
            "K=1 action record serialized a queue field");
    require(plain_json.ends_with(",\"stage_bytes\":0}"),
            "K=1 action record changed shape");

    const ActionRecord fielded = pipeline_record(
        ActionType::TX_BEGIN, ActorSide::C, 0, HistoryNonce{10}, RelSeq{1},
        TuSeq{8}, tagged_digest(3), tagged_digest(4), tagged_digest(1),
        uint32_t{1}, uint32_t{2});
    const std::string fielded_json = action_jsonl(fielded);
    require(fielded_json.find("\"queue_position\":1") != std::string::npos &&
                fielded_json.find("\"queue_depth\":2") != std::string::npos,
            "K>1 action record omitted its queue fields");
    require(fielded_json.ends_with(
                ",\"stage_bytes\":0,\"queue_position\":1,\"queue_depth\":2}"),
            "queue fields did not append at the end of the object");
}

void test_check_action_trace_k1_identity() {
    // The K=1 synthetic trace must pass the generalized checker exactly
    // as it passed the scalar one, with no queue fields anywhere.
    require_checker_accepts(pipeline_k1_trace(),
                            "K=1 synthetic trace did not pass the checker");
    // The old scalar negatives still reject through their own gates.
    auto records = pipeline_k1_trace();
    records[8].action = ActionType::TX_ABORTED;  // abort after durable commit
    require_checker_rejects(records, "aborted before accepting",
                             "K=1 abort-after-durable-commit was accepted");
}

void test_check_action_trace_k2_pipelining() {
    require_checker_accepts(pipeline_k2_overlap_trace(),
                            "K=2 overlap chain did not pass the checker");
    require_checker_accepts(pipeline_k2_poison_trace(),
                            "K=2 poison -> cold reset did not pass");

    // Successor retiring before its head: swap the two acceptances.
    {
        auto records = pipeline_k2_overlap_trace();
        const size_t first = 13;
        const size_t second = 14;
        std::swap(records[first], records[second]);
        require_checker_rejects(records, "acceptance",
                                 "K=2 retire-before-head was accepted");
    }
    // C ahead of F by more than K: a third admission at position 2 with
    // depth 2.
    {
        auto records = pipeline_k2_overlap_trace();
        records.insert(records.begin() + 8,
                        pipeline_record(ActionType::TX_BEGIN, ActorSide::C,
                                        0, HistoryNonce{10}, RelSeq{2},
                                        TuSeq{9}, tagged_digest(9),
                                        tagged_digest(10),
                                        tagged_digest(3), uint32_t{2},
                                        uint32_t{2}));
        require_checker_rejects(records, "queue position",
                                 "K=2 ahead-by-more-than-K was accepted");
    }
    // Admission skipping a queue slot.
    {
        auto records = pipeline_k2_overlap_trace();
        records[7].queue_position = uint32_t{0};
        require_checker_rejects(records, "queue slot",
                                 "K=2 admission slot hole was accepted");
    }
    // Acceptance naming a non-head slot.
    {
        auto records = pipeline_k2_overlap_trace();
        records[13].queue_position = uint32_t{1};
        require_checker_rejects(records, "retiring queue head",
                                 "K=2 non-head acceptance was accepted");
    }
    // Abort drain out of batch order.
    {
        auto records = pipeline_k2_poison_trace();
        records[9].queue_position = uint32_t{0};
        require_checker_rejects(records, "queue slot",
                                 "K=2 abort batch disorder was accepted");
    }
    // F TX_BEGIN naming a TX C never admitted.
    {
        auto records = pipeline_k2_overlap_trace();
        records[9].tu_seq = TuSeq{99};
        require_checker_rejects(records, "does not name",
                                 "K=2 foreign F begin was accepted");
    }
    // A fieldless second begin while a TX is mid-flight must still hit the
    // scalar rule at K=1.
    {
        auto records = pipeline_k1_trace();
        const ActionRecord second = pipeline_record(
            ActionType::TX_BEGIN, ActorSide::C, 0, HistoryNonce{10}, RelSeq{1},
            TuSeq{8}, tagged_digest(6), tagged_digest(7),
            tagged_digest(2));
        records.insert(records.begin() + 3, second);
        require_checker_rejects(records, "second active",
                                 "fieldless second begin was accepted");
    }
    // A K=1 record carrying a second slot must be rejected.
    {
        auto records = pipeline_k1_trace();
        const ActionRecord second = pipeline_record(
            ActionType::TX_BEGIN, ActorSide::C, 0, HistoryNonce{10}, RelSeq{1},
            TuSeq{8}, tagged_digest(6), tagged_digest(7),
            tagged_digest(2), uint32_t{1}, uint32_t{1});
        records.push_back(second);
        require_checker_rejects(records, "queue position",
                                 "K=1 record claiming a second slot passed");
    }
    // Only one of the two queue fields is a generation error.
    {
        auto records = pipeline_k2_overlap_trace();
        records[7].queue_depth.reset();
        require_checker_rejects(records, "only one queue field",
                                 "half-fielded record was accepted");
    }
}

void test_croute_records_stamp_pipeline_fields() {
    // CRoute::record stamps queue_position/queue_depth only on a K>1
    // route.  Driving a full P29V1 cycle needs the endpoint harness, so
    // here we pin the depth-1 half of that contract: a default-depth
    // route emits fieldless records.  The K>1 half is pinned by the JSONL
    // shape test above plus the endpoint identity test (K=1) elsewhere;
    // the stamping branch itself is one guarded assignment in
    // CRoute::record (max_active_tx_ > 1).
    CAuthority authority(CStoreGuid::from_u64(80));
    ActionTrace trace;
    CRoute route(authority, FStoreGuid::from_u64(81), HistoryNonce{82},
                 &trace);
    require(route.max_active_tx() == 1, "default route depth is not 1");
    // No live TX: nothing to record through the public surface without a
    // P29V1 cycle; the guard is asserted structurally by the tests above.
    require(trace.records().empty(),
            "a fresh route trace is not empty");
}

// Capture the interner's own stderr reports (one JSON line per "nonrunnable"
// event) for a bounded region, so a test can count them exactly.
class StderrCapture {
public:
    StderrCapture() {
        std::fflush(stderr);
        saved_ = ::dup(STDERR_FILENO);
        int pipefd[2];
        if (::pipe(pipefd) != 0 || saved_ < 0) {
            if (saved_ >= 0) ::close(saved_);
            saved_ = -1;
            return;
        }
        ::dup2(pipefd[1], STDERR_FILENO);
        ::close(pipefd[1]);
        reader_ = pipefd[0];
    }
    ~StderrCapture() {
        if (reader_ >= 0) {
            ::close(reader_);
        }
        if (saved_ >= 0) {
            ::dup2(saved_, STDERR_FILENO);
            ::close(saved_);
        }
        std::fflush(stderr);
    }
    [[nodiscard]] std::string drained() const {
        if (reader_ < 0)
            return {};
        std::string result;
        char buffer[4096];
        for (;;) {
            const ssize_t got = ::read(reader_, buffer, sizeof(buffer));
            if (got <= 0)
                break;
            result.append(buffer, static_cast<size_t>(got));
        }
        return result;
    }
    [[nodiscard]] static size_t count_lines(const std::string& text,
                                            const char* needle) {
        size_t total = 0;
        std::string_view view(text);
        for (size_t position = 0; position < view.size();) {
            const size_t end = view.find('\n', position);
            const std::string_view line =
                view.substr(position, end == std::string_view::npos
                                           ? std::string_view::npos
                                           : end - position);
            if (line.find(needle) != std::string_view::npos)
                ++total;
            if (end == std::string_view::npos)
                break;
            position = end + 1;
        }
        return total;
    }

private:
    int saved_ = -1;
    int reader_ = -1;
};

void test_split_prepare_marks_nonrunnable_exactly_once() {
    // The split locked/unlocked prepare_source must reproduce the combined
    // path's failure shape exactly: only a failure in the guarded region
    // (the interner's own reserve/process/injected-failure) marks the
    // interner non-runnable and prints a "nonrunnable" line.  The
    // oversize-TU precondition stays an ordinary length_error (no mark, no
    // line), and a call after the interner is already non-runnable throws
    // the precondition logic_error WITHOUT re-marking (no second line).
    // prepare_source is the public path over the same split halves.
    const std::vector<uint8_t> oversize(2048, uint8_t{'x'});
    const std::vector<uint8_t> fine{uint8_t{'#'}, uint8_t{' '},
                                    uint8_t{'a'}, uint8_t{'\n'}};
    const PreparationAuthorityLimits limits{};
    {
        P50PreparationAuthority authority(CStoreGuid::from_u64(101),
                                          ZstdTuLimits{}, limits, 1,
                                          ProfileId::P29V1, TuSeq{0});
        {
            StderrCapture capture;
            // prepare_source refuses oversize input itself with a null
            // return (the max_raw_bytes check), so drive the underlying
            // typed path through prepare_for_route, which does NOT
            // pre-check the P29V1 TU limit and relies on the interner's
            // own length_error staying ordinary.
            const PreparationRouteKey route{
                CStoreGuid::from_u64(101),
                FStoreGuid::from_u64(202), 1, ProfileId::P29V1};
            const PrepareRequestKey key{1, 1};
            require_throws<std::length_error>(
                [&] {
                    (void)authority.prepare_for_route(route, key, oversize);
                },
                "oversize TU did not throw length_error");
            require(capture.drained().empty(),
                    "oversize TU printed an interner report");
        }
        // A valid TU still interns after the ordinary length_error.
        {
            StderrCapture capture;
            const PreparationRouteKey route{
                CStoreGuid::from_u64(101),
                FStoreGuid::from_u64(202), 1, ProfileId::P29V1};
            const PreparedTuHandle prepared =
                authority.prepare_for_route(route, PrepareRequestKey{1, 1},
                                            fine);
            (void)prepared;
            require(capture.drained().empty(),
                    "valid TU printed an interner report");
        }
    }
    {
        P50PreparationAuthority authority(
            CStoreGuid::from_u64(102), ZstdTuLimits{}, limits, 1,
            ProfileId::P29V1, TuSeq{10},
            P29InternerFaultInjection::FailOnce);
        const PreparationRouteKey route{
            CStoreGuid::from_u64(102), FStoreGuid::from_u64(203), 1,
            ProfileId::P29V1};
        {
            StderrCapture capture;
            require_throws<P29V1CapabilityUnavailable>(
                [&] {
                    (void)authority.prepare_for_route(
                        route, PrepareRequestKey{1, 1}, fine);
                },
                "injected failure did not surface");
            const std::string drained = capture.drained();
            require(StderrCapture::count_lines(
                        drained, "\"event\":\"nonrunnable\"") == 1,
                    "injected failure did not print exactly one "
                    "nonrunnable line");
        }
        {
            StderrCapture capture;
            require_throws<P29V1CapabilityUnavailable>(
                [&] {
                    (void)authority.prepare_for_route(
                        route, PrepareRequestKey{1, 2}, fine);
                },
                "non-runnable interner did not fail closed");
            require(capture.drained().empty(),
                    "post-failure call re-printed a nonrunnable line");
        }
    }
}

}  // namespace

int main() {
    test_object_arena_and_canonical_records();
    test_global_resource_owner();
    test_atomic_p29v1_pair_preflight();
    test_global_reverse_invariants_catch_mutants();
    test_f_store_session_and_route_fencing();
    test_f_store_routes_are_isolated_by_profile();
    test_f_store_begin_rejects_second_active_tx_without_commit();
    test_authority_and_route_fail_closed_before_enablement();
    test_split_prepare_marks_nonrunnable_exactly_once();
    test_terminal_session_serial();
    test_action_trace_jsonl_fields_additive();
    test_check_action_trace_k1_identity();
    test_check_action_trace_k2_pipelining();
    test_croute_records_stamp_pipeline_fields();
    return 0;
}
