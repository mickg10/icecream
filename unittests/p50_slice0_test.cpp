#include "cache/p50_slice0.h"

#include <algorithm>
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

namespace icecc::p50 {
struct P50Slice0TestAccess {
    static PreparedTUPtr prepare(CAuthority& authority,
                                 std::span<const uint8_t> exact_input) {
        if (!authority.p29v1_runnable())
            authority.enable_p29v1(UINT64_C(2463121408), UINT64_C(1) << 20);
        const TuSeq sequence = authority.reserve_tu_seq();
        PreparedTUPtr prepared = authority.prepare_p29v1_at_seq(
            exact_input, sequence, icecc::digest128(exact_input));
        authority.commit_tu_seq(sequence);
        return prepared;
    }
};
} // namespace icecc::p50

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

void test_interrupted_install_retry_preserves_exact_identity() {
    const GlobalResourceLimits limits{
        .max_aggregate_bytes = 64,
        .max_namespace_bytes = 64,
        .max_staging_bytes = 64,
        .max_total_bytes = 128,
        .max_generation = 1,
        .max_staging_slots = 2,
    };
    const CStoreGuid c_guid = CStoreGuid::from_u64(12);
    const Key64 key = *Key64::make(ObjectType::Blob, 0, 7);
    const Digest128 digest = tagged_digest(91);
    GlobalResourceModel model(limits);
    model.admit(c_guid);

    // Two interrupted attempts (the initial transfer plus one replay) retain
    // only an identity tombstone: no staging slot or bytes remain charged.
    for (unsigned attempt = 0; attempt != 2; ++attempt) {
        model.start_tu(c_guid);
        const bool retry = model.install_retry_required(c_guid, key);
        require(retry == (attempt != 0),
                "interrupted install retry eligibility was not preserved");
        model.begin_install(c_guid, key, digest, 17, attempt, retry);
        model.crash_install(c_guid, key, attempt);
        model.finish_tu(c_guid);
        require(model.staging_bytes() == 0 && model.resident_bytes() == 0 &&
                    model.free_staging_slots() == limits.max_staging_slots &&
                    !model.check_invariants(),
                "interrupted install leaked global resources");
    }

    model.start_tu(c_guid);
    require_throws<std::logic_error>(
        [&] { model.begin_install(c_guid, key, tagged_digest(92), 17, 0, true); },
        "global retry accepted a different content digest");
    require_throws<std::logic_error>(
        [&] { model.begin_install(c_guid, key, digest, 18, 0, true); },
        "global retry accepted a different byte count");
    require(model.staging_bytes() == 0 && model.free_staging_slots() == 2,
            "rejected retries changed staging accounting");

    // A second interrupted replay is also bounded and exact; the final retry
    // installs/publishes the same object without accumulating stale bytes.
    model.begin_install(c_guid, key, digest, 17, 0, true);
    model.crash_install(c_guid, key, 0);
    model.finish_tu(c_guid);
    require(model.install_retry_required(c_guid, key),
            "second interrupted replay lost its retry tombstone");
    model.start_tu(c_guid);
    model.begin_install(c_guid, key, digest, 17, 0, true);
    model.publish(c_guid, key, 0, digest);
    model.finish_tu(c_guid);
    require(model.resident_bytes() == 17 && model.staging_bytes() == 0 &&
                !model.check_invariants(),
            "exact interrupted install retry did not publish cleanly");
    const Key64 abandoned = *Key64::make(ObjectType::Blob, 0, 8);
    model.start_tu(c_guid);
    model.begin_install(c_guid, abandoned, tagged_digest(93), 9, 1);
    model.crash_install(c_guid, abandoned, 1);
    model.finish_tu(c_guid);
    require(model.discard_crashed_install(c_guid, abandoned) &&
                !model.discard_crashed_install(c_guid, abandoned) &&
                model.resident_bytes() == 17 && model.staging_bytes() == 0 &&
                !model.check_invariants(),
            "terminal tombstone retirement changed resident/staging state");
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

void test_speculative_window_holds_exact_ordered_commit_witnesses() {
    const CStoreGuid c_guid = CStoreGuid::from_u64(70);
    const FStoreGuid f_guid = FStoreGuid::from_u64(71);
    const HistoryNonce nonce{72};
    CAuthority authority(c_guid);
    CRoute route(authority, f_guid, nonce);
    route.configure_speculative_window(30, 1024);

    FStore store(f_guid, 1, nullptr, UINT64_C(1) << 20, false);
    const SessionHandle session = store.connect(c_guid, ProfileId::P29V1);
    store.start_route(session, nonce, initial_route_digest(c_guid, nonce));

    std::vector<TxCommit> commits;
    commits.reserve(30);
    std::vector<PreparedTUPtr> prepared_tus;
    prepared_tus.reserve(30);
    std::vector<std::string> raw_inputs;
    raw_inputs.reserve(30);
    uint64_t raw_total = 0;
    for (uint64_t i = 0; i != 30; ++i) {
        const std::string text = "int value_" + std::to_string(i) + " = " +
                                 std::to_string(i * 17) + ";\n";
        const std::span<const uint8_t> input(
            reinterpret_cast<const uint8_t*>(text.data()), text.size());
        PreparedTUPtr prepared = P50Slice0TestAccess::prepare(authority, input);
        prepared_tus.push_back(prepared);
        raw_inputs.push_back(text);
        const CActiveTx& active = route.begin_v1(prepared, UINT64_C(1) << 20);
        if (i == 0) route.pin_v1_system_source_reuse(false);

        const std::vector<uint8_t> predicted_need = route.predicted_need_v1();
        store.begin(session, active.begin);
        store.append_body(session, active.body);
        const std::vector<uint8_t> f_need = store.p29v1_need_frames(session);
        require(predicted_need == f_need,
                "predicted P29 NEED differs from the real F route reply");
        const std::span<const uint8_t> fill_span =
            route.build_fill_v1(predicted_need);
        const std::vector<uint8_t> fill(fill_span.begin(), fill_span.end());
        store.append_fill_v1(session, fill);
        const std::vector<uint8_t> materialized =
            store.materialize_and_verify(session);
        require(materialized.size() == input.size() &&
                    std::equal(materialized.begin(), materialized.end(),
                               input.begin()),
                "speculative F route materialized different TU bytes");
        commits.push_back(store.commit_input(session));
        route.advance_speculative_v1();
        raw_total += input.size();
    }

    const Digest128 initial = initial_route_digest(c_guid, nonce);
    require(route.next_rel_seq() == RelSeq{0} && route.state_digest() == initial,
            "speculative advancement changed the confirmed C cursor");
    require(route.speculative_next_rel_seq() == RelSeq{30} &&
                route.speculative_tu_count() == 30 &&
                route.speculative_raw_bytes() == raw_total,
            "speculative window did not retain its bounded identity ledger");

    const std::string small_text = "int extra = 1;\n";
    const std::span<const uint8_t> small_input(
        reinterpret_cast<const uint8_t*>(small_text.data()), small_text.size());
    const PreparedTUPtr count_limited =
        P50Slice0TestAccess::prepare(authority, small_input);
    require_throws<std::length_error>(
        [&] { (void)route.begin_v1(count_limited, UINT64_C(1) << 20); },
        "C admitted a TU beyond its count reservation");

    require_throws<std::logic_error>(
        [&] { route.accept_commit(commits[1]); },
        "C accepted an out-of-order speculative commit");
    TxCommit wrong_digest = commits[0];
    wrong_digest.transaction_digest =
        icecc::digest128("wrong speculative transaction");
    require_throws<std::logic_error>(
        [&] { route.accept_commit(wrong_digest); },
        "C accepted a wrong-digest speculative commit");
    require(route.next_rel_seq() == RelSeq{0} &&
                route.speculative_tu_count() == 30,
            "rejected speculative receipts changed ownership or cursor state");

    for (const TxCommit& commit : commits)
        route.accept_commit(commit);
    require(route.next_rel_seq() == RelSeq{30} &&
                route.state_digest() == commits.back().post_state_digest &&
                route.speculative_next_rel_seq() == RelSeq{30} &&
                route.speculative_tu_count() == 0 &&
                route.speculative_raw_bytes() == 0,
            "ordered commit drain did not promote exactly the confirmed prefix");
    require_throws<std::logic_error>(
        [&] { route.accept_commit(commits.front()); },
        "C accepted a duplicate receipt after the speculative ledger drained");
}

void test_reset_rebuilds_only_unsent_speculative_suffix() {
    const CStoreGuid c_guid = CStoreGuid::from_u64(74);
    const FStoreGuid f_guid = FStoreGuid::from_u64(75);
    const HistoryNonce old_nonce{76};
    const HistoryNonce new_nonce{77};
    CAuthority authority(c_guid);
    CRoute route(authority, f_guid, old_nonce);
    route.configure_speculative_window(3, 64);
    FStore store(f_guid, 1, nullptr, UINT64_C(1) << 20, false);
    const SessionHandle session = store.connect(c_guid, ProfileId::P29V1);
    store.start_route(session, old_nonce,
                      initial_route_digest(c_guid, old_nonce));

    std::vector<PreparedTUPtr> prepared_tus;
    prepared_tus.reserve(3);
    std::vector<std::string> inputs;
    inputs.reserve(3);
    std::vector<TxCommit> old_commits;
    old_commits.reserve(3);
    for (unsigned i = 0; i != 3; ++i) {
        inputs.push_back("int staged_" + std::to_string(i) + " = " +
                         std::to_string(101 + i) + ";\n");
        const std::string& input_string = inputs.back();
        const std::span<const uint8_t> input(
            reinterpret_cast<const uint8_t*>(input_string.data()),
            input_string.size());
        prepared_tus.push_back(P50Slice0TestAccess::prepare(authority, input));
    }

    // The first TU is actually committed by F and acknowledged by C.
    const CActiveTx& first = route.begin_v1(prepared_tus[0], UINT64_C(1) << 20);
    route.pin_v1_system_source_reuse(false);
    const std::vector<uint8_t> first_predicted = route.predicted_need_v1();
    store.begin(session, first.begin);
    store.append_body(session, first.body);
    const auto first_need = store.p29v1_need_frames(session);
    require(first_predicted == first_need,
            "initial F NEED differs from its speculative prediction");
    const std::span<const uint8_t> first_fill_span =
        route.build_fill_v1(first_predicted);
    store.append_fill_v1(
        session, std::vector<uint8_t>(first_fill_span.begin(), first_fill_span.end()));
    const auto first_materialized = store.materialize_and_verify(session);
    require(first_materialized.size() == inputs[0].size() &&
                std::equal(first_materialized.begin(), first_materialized.end(),
                           reinterpret_cast<const uint8_t*>(inputs[0].data())),
            "initial acknowledged TU differs from source");
    old_commits.push_back(store.commit_input(session));
    route.advance_speculative_v1();
    route.accept_commit(old_commits.back());

    // The next two TUs are completely staged at C, but their BODY/FILL bundles
    // are not sent to F. They therefore remain the true uncommitted suffix.
    std::array<TxBegin, 2> abandoned_begins{};
    for (std::size_t i = 1; i != 3; ++i) {
        const CActiveTx& active = route.begin_v1(prepared_tus[i], UINT64_C(1) << 20);
        abandoned_begins[i - 1] = active.begin;
        const auto predicted = route.predicted_need_v1();
        (void)route.build_fill_v1(predicted);
        route.advance_speculative_v1();
    }
    require(route.next_rel_seq() == RelSeq{1} &&
                route.speculative_next_rel_seq() == RelSeq{3} &&
                route.speculative_tu_count() == 2 &&
                store.resume(session).next_rel_seq == RelSeq{1},
            "staged unsent suffix was not kept separate from F's committed cursor");

    std::vector<uint8_t> oversized(65, static_cast<uint8_t>('x'));
    const PreparedTUPtr too_large =
        P50Slice0TestAccess::prepare(authority, oversized);
    require_throws<std::length_error>(
        [&] { (void)route.begin_v1(too_large, UINT64_C(1) << 20); },
        "speculative raw-byte cap admitted an oversized suffix TU");

    route.reset_v1_route(f_guid, new_nonce);
    store.forget_route(session);
    store.start_route(session, new_nonce,
                      initial_route_digest(c_guid, new_nonce));
    std::vector<TxCommit> rebuilt_commits;
    rebuilt_commits.reserve(2);
    for (std::size_t i = 1; i != 3; ++i) {
        const CActiveTx& active = route.begin_v1(prepared_tus[i], UINT64_C(1) << 20);
        if (i == 1) route.pin_v1_system_source_reuse(false);
        const auto predicted = route.predicted_need_v1();
        store.begin(session, active.begin);
        store.append_body(session, active.body);
        const auto f_need = store.p29v1_need_frames(session);
        require(predicted == f_need,
                "rebuilt F NEED differs from the canonical prediction");
        const std::span<const uint8_t> fill = route.build_fill_v1(predicted);
        store.append_fill_v1(session,
                             std::vector<uint8_t>(fill.begin(), fill.end()));
        const auto materialized = store.materialize_and_verify(session);
        require(materialized.size() == inputs[i].size() &&
                    std::equal(materialized.begin(), materialized.end(),
                               reinterpret_cast<const uint8_t*>(inputs[i].data())),
                "rebuilt TU with retained identity differs from exact input");
        rebuilt_commits.push_back(store.commit_input(session));
        route.advance_speculative_v1();
    }
    require(route.next_rel_seq() == RelSeq{0} &&
                route.speculative_next_rel_seq() == RelSeq{2} &&
                route.speculative_tu_count() == 2,
            "fresh-history rebuild did not create contiguous replacement ordinals");
    TxCommit stale_receipt{abandoned_begins[0].history_nonce,
                           abandoned_begins[0].rel_seq,
                           abandoned_begins[0].tu_seq,
                           abandoned_begins[0].transaction_digest,
                           abandoned_begins[0].raw_digest,
                           compute_post_state_digest(
                               abandoned_begins[0].pre_state_digest,
                               abandoned_begins[0].history_nonce,
                               abandoned_begins[0].rel_seq,
                               abandoned_begins[0].tu_seq,
                               abandoned_begins[0].transaction_digest)};
    require_throws<std::logic_error>(
        [&] { route.accept_commit(stale_receipt); },
        "C accepted a stale old-history receipt after suffix rebuild");
    for (const TxCommit& commit : rebuilt_commits)
        route.accept_commit(commit);
    require(route.next_rel_seq() == RelSeq{2} &&
                route.state_digest() == rebuilt_commits.back().post_state_digest &&
                route.speculative_next_rel_seq() == RelSeq{2} &&
                route.speculative_tu_count() == 0 &&
                route.speculative_raw_bytes() == 0,
            "ordered rebuilt receipt drain failed");
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
    test_interrupted_install_retry_preserves_exact_identity();
    test_atomic_p29v1_pair_preflight();
    test_global_reverse_invariants_catch_mutants();
    test_f_store_session_and_route_fencing();
    test_f_store_routes_are_isolated_by_profile();
    test_authority_and_route_fail_closed_before_enablement();
    test_speculative_window_holds_exact_ordered_commit_witnesses();
    test_reset_rebuilds_only_unsent_speculative_suffix();
    test_terminal_session_serial();
    return 0;
}
