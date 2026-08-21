#include "cache/p50_slice0.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

using namespace icecc::p50;

[[noreturn]] void fail(std::string_view text) {
    std::cerr << "p50_slice0_test: " << text << '\n';
    std::exit(1);
}

void require(bool value, std::string_view text) {
    if (!value) fail(text);
}

template<class Exception, class Callable>
void require_throws(Callable&& callable, std::string_view text) {
    try {
        callable();
    } catch (const Exception&) {
        return;
    } catch (...) {
        fail(std::string(text) + " (wrong exception)");
    }
    fail(std::string(text) + " (no exception)");
}

std::vector<uint8_t> bytes(std::string_view text) {
    return {text.begin(), text.end()};
}

std::vector<std::vector<uint8_t>> regions(
    std::initializer_list<std::string_view> texts) {
    std::vector<std::vector<uint8_t>> result;
    for (std::string_view text : texts) result.push_back(bytes(text));
    return result;
}

struct Pair {
    CAuthority c;
    FStore f;
    CRoute route;
    SessionHandle session{};

    explicit Pair(ActionTrace* trace = nullptr, uint64_t c_id = 100,
                  uint64_t f_id = 200)
        : c(Id128::from_u64(c_id)),
          f(Id128::from_u64(f_id), 1, trace),
          route(c, f.guid(), HistoryNonce{300}, trace) {
        const ReconnectResult connected = reconnect(route, f, HistoryNonce{301});
        require(connected.outcome == ReconnectOutcome::ColdFStore,
                "initial namespace did not take cold reconnect path");
        session = connected.session;
    }
};

Need start(Pair& pair, const CActiveTx& active, bool replay = false) {
    pair.f.begin(pair.session, active.begin, replay);
    pair.f.append_dict(pair.session, active.dict);
    return pair.f.need(pair.session);
}

TxCommit finish(Pair& pair, const CActiveTx& active, bool fill_before_body = false,
                bool acknowledge = true, bool replay = false) {
    const Need need = start(pair, active, replay);
    const std::vector<ImmutableObject> fill = pair.route.build_fill(need);
    if (fill_before_body) {
        for (const ImmutableObject& object : fill)
            pair.f.apply_object(pair.session, object);
        pair.f.append_body(pair.session, active.body);
    } else {
        pair.f.append_body(pair.session, active.body);
        for (const ImmutableObject& object : fill)
            pair.f.apply_object(pair.session, object);
    }
    const std::vector<uint8_t> exact =
        pair.f.materialize_and_verify(pair.session);
    require(exact.size() == active.begin.raw_bytes,
            "materialized byte count differs from TX_BEGIN");
    const TxCommit commit = pair.f.commit_input(pair.session);
    if (acknowledge) pair.route.accept_commit(commit);
    return commit;
}

void test_key_limits_and_mixed_generations() {
    CObjectArena rejecting(Id128::from_u64(2));
    const ObjectType unassigned = static_cast<ObjectType>(8);
    require_throws<std::invalid_argument>(
        [&] { (void)rejecting.intern_bytes(unassigned, bytes("invalid")); },
        "C object arena accepted an unassigned byte-object type");
    require_throws<std::invalid_argument>(
        [&] {
            (void)rejecting.intern_children(unassigned,
                                            std::span<const Key64>{});
        },
        "C object arena accepted an unassigned child-object type");
    require(rejecting.objects().size() == 0,
            "rejected object types changed the C object arena");

    CObjectArena terminal(Id128::from_u64(1),
                          KeyLayoutV1::generation_value_mask,
                          KeyLayoutV1::ordinal_mask);
    const Key64 last = terminal.intern_bytes(ObjectType::Line, bytes("last"));
    require(last.generation() == KeyLayoutV1::generation_value_mask &&
                last.ordinal() == KeyLayoutV1::ordinal_mask,
            "last KeyLayoutV1 value was not allocated");
    require_throws<std::overflow_error>(
        [&] { (void)terminal.intern_bytes(ObjectType::Line, bytes("another")); },
        "ordinal exhaustion was not terminal");
    require(terminal.advance_generation() == GenerationAdvanceResult::GuidFlipRequired,
            "generation exhaustion did not require C_STORE_GUID flip");

    Pair pair;
    const Key64 old_line = pair.c.intern_bytes(ObjectType::Line, bytes("old\n"));
    const std::array<Key64, 1> old_child{old_line};
    const Key64 old_region = pair.c.intern_children(ObjectType::Region, old_child);
    require(old_region.generation() == 0, "C object arena did not start at generation zero");
    require(pair.c.advance_generation() == GenerationAdvanceResult::Advanced,
            "ordinary generation advance failed");
    const Key64 new_line = pair.c.intern_bytes(ObjectType::Line, bytes("new\n"));
    const std::array<Key64, 1> new_child{new_line};
    const Key64 new_region = pair.c.intern_children(ObjectType::Region, new_child);
    const std::array<Key64, 2> roots{old_region, new_region};
    const std::vector<uint8_t> exact = bytes("old\nnew\n");
    const PreparedTUPtr prepared = pair.c.prepare_tu(exact, roots);
    finish(pair, pair.route.begin(prepared));
    require(pair.f.contains(pair.c.guid(), old_region) &&
                pair.f.contains(pair.c.guid(), new_region),
            "old and new generations did not coexist on F");
}

void test_tu_seq_is_not_route_order() {
    Pair pair;
    const PreparedTUPtr tu0 = pair.c.prepare_from_regions(regions({"zero\n"}));
    const PreparedTUPtr tu1 = pair.c.prepare_from_regions(regions({"one\n"}));
    require(tu0->tu_seq.value == 0 && tu1->tu_seq.value == 1,
            "TU_SEQ was not allocated at immutable publication");
    const CActiveTx& first_routed = pair.route.begin(tu1);
    require(first_routed.begin.tu_seq.value == 1 &&
                first_routed.begin.rel_seq.value == 0,
            "first accepted route did not allocate REL_SEQ zero");
    finish(pair, first_routed);
    const CActiveTx& second_routed = pair.route.begin(tu0);
    require(second_routed.begin.tu_seq.value == 0 &&
                second_routed.begin.rel_seq.value == 1,
            "later TU_SEQ incorrectly dictated route order");
    finish(pair, second_routed);
}

void test_separate_preparation_real_interning_and_p29() {
    static_assert(std::is_copy_constructible_v<TxBegin>);
    Pair pair;
    const auto input = regions({"alpha\n", "beta\n", "gamma\n", "delta\n"});
    const PreparedTUPtr first = pair.c.prepare_from_regions(input);
    finish(pair, pair.route.begin(first));

    const PreparedTUPtr second = pair.c.prepare_from_regions(input);
    require(first.get() != second.get() && first->regions == second->regions,
            "separate preparation did not reuse stable interned Region keys");
    const CActiveTx& active = pair.route.begin(second);
    require(active.begin.profile == ProfileId::P29 && active.root.size() == 1 &&
                active.root.front().type() == ObjectType::Block,
            "real OnlineS1 did not produce the repeated-TU Block root");
    const Need need = start(pair, active);
    require(need.missing.size() == 1 &&
                need.missing.front().type() == ObjectType::Block,
            "warm F requested more than the newly interned P29 Block");
    pair.f.append_body(pair.session, active.body);
    for (const ImmutableObject& object : pair.route.build_fill(need))
        pair.f.apply_object(pair.session, object);
    require(pair.f.materialize_and_verify(pair.session) ==
                bytes("alpha\nbeta\ngamma\ndelta\n"),
            "P29 Block root did not reproduce exact bytes");
    pair.route.accept_commit(pair.f.commit_input(pair.session));
}

void test_exact_need_and_duplicate_application() {
    Pair pair;
    const PreparedTUPtr prepared =
        pair.c.prepare_from_regions(regions({"one\n", "two\n", "three\n"}));
    const CActiveTx& active = pair.route.begin(prepared);
    const Need original = start(pair, active);
    const std::vector<ImmutableObject> fill = pair.route.build_fill(original);
    require(fill.size() >= 2, "cold Fill fixture is too small");
    const ObjectApplied applied = pair.f.apply_object(pair.session, fill.front());
    const ObjectApplied duplicate = pair.f.apply_object(pair.session, fill.front());
    require(applied.result == ObjectApplyResult::Applied &&
                duplicate.result == ObjectApplyResult::Duplicate,
            "exact duplicate object was not idempotent");
    require(pair.f.need(pair.session) == original,
            "F did not retain the exact originally recorded Need set");

    const ImmutableObject conflicting =
        ImmutableObject::bytes(fill.front().key, bytes("changed\n"));
    require_throws<std::logic_error>(
        [&] { pair.f.apply_object(pair.session, conflicting); },
        "changed content replaced an installed Key64");
    pair.f.append_body(pair.session, active.body);
    for (size_t i = 1; i != fill.size(); ++i)
        pair.f.apply_object(pair.session, fill[i]);
    pair.f.materialize_and_verify(pair.session);
    pair.route.accept_commit(pair.f.commit_input(pair.session));
}

void test_valid_then_invalid_fill_retains_first_object() {
    Pair pair;
    const PreparedTUPtr prepared =
        pair.c.prepare_from_regions(regions({"a\n", "b\n", "c\n"}));
    const CActiveTx& active = pair.route.begin(prepared);
    const Need need = start(pair, active);
    const std::vector<ImmutableObject> fill = pair.route.build_fill(need);
    require(fill.size() >= 2, "FILL fixture needs at least two objects");
    FillRecord first = fill[0].fill_record();
    FillRecord bad = fill[1].fill_record();
    bad.object_bytes.back() ^= 1;
    const std::array<FillRecord, 2> records{first, bad};
    const auto messages = encode_fill_messages(records, kInitialMaxFramePayload);
    require(messages.size() == 1, "valid/invalid fixture unexpectedly split");
    require_throws<std::invalid_argument>(
        [&] { (void)pair.f.append_fill(pair.session, messages.front()); },
        "invalid second FILL object was accepted");
    require(pair.f.contains(pair.c.guid(), first.key),
            "valid object preceding an invalid object was rolled back");

    pair.f.append_body(pair.session, active.body);
    for (const ImmutableObject& object : fill)
        pair.f.apply_object(pair.session, object);
    pair.f.finish_fill(pair.session);
    pair.f.materialize_and_verify(pair.session);
    pair.route.accept_commit(pair.f.commit_input(pair.session));
}

void test_trailing_partial_fill_blocks_commit_and_replays() {
    Pair pair;
    const PreparedTUPtr prepared =
        pair.c.prepare_from_regions(regions({"trailing\n", "partial\n", "fill\n"}));
    const CActiveTx& active = pair.route.begin(prepared);
    const Need need = start(pair, active);
    const std::vector<ImmutableObject> fill = pair.route.build_fill(need);
    require(!fill.empty(), "trailing-partial FILL fixture has no requested objects");

    std::vector<FillRecord> records;
    records.reserve(fill.size());
    for (const ImmutableObject& object : fill)
        records.push_back(object.fill_record());
    const auto complete = encode_fill_messages(records, kInitialMaxFramePayload);
    const std::array<FillRecord, 1> extra{fill.front().fill_record()};
    const auto trailing = encode_fill_messages(extra, kInitialMaxFramePayload);
    require(complete.size() == 1 && trailing.size() == 1 &&
                trailing.front().bytes.size() >= 4,
            "trailing-partial FILL fixture unexpectedly split");

    FillMessage message = complete.front();
    message.bytes.insert(message.bytes.end(), trailing.front().bytes.begin(),
                         trailing.front().bytes.begin() + 4);
    pair.f.append_body(pair.session, active.body);
    require_throws<std::invalid_argument>(
        [&] { (void)pair.f.append_fill(pair.session, message); },
        "trailing partial record was accepted after the final requested object");
    for (const ImmutableObject& object : fill)
        require(pair.f.contains(pair.c.guid(), object.key),
                "complete object preceding trailing partial bytes was rolled back");
    require_throws<std::invalid_argument>(
        [&] { (void)pair.f.materialize_and_verify(pair.session); },
        "transaction materialized with a trailing partial FILL record");
    require_throws<std::logic_error>(
        [&] { (void)pair.f.commit_input(pair.session); },
        "transaction committed after rejected trailing partial FILL bytes");

    pair.f.disconnect(pair.session);
    const ReconnectResult resumed =
        reconnect(pair.route, pair.f, HistoryNonce{350});
    require(resumed.outcome == ReconnectOutcome::ExactMatch && resumed.replay_active,
            "trailing-partial FILL did not take exact replay path");
    pair.session = resumed.session;
    pair.f.begin(pair.session, pair.route.active()->begin, true);
    pair.f.append_dict(pair.session, pair.route.active()->dict);
    require(pair.f.need(pair.session).missing.empty(),
            "replay did not recompute Need from retained complete objects");
    pair.f.append_body(pair.session, pair.route.active()->body);
    pair.f.materialize_and_verify(pair.session);
    pair.route.accept_commit(pair.f.commit_input(pair.session));
}

void test_body_and_fill_complete_in_both_orders() {
    {
        Pair pair;
        const auto prepared = pair.c.prepare_from_regions(regions({"body\n", "first\n"}));
        finish(pair, pair.route.begin(prepared), false);
    }
    {
        Pair pair;
        const auto prepared = pair.c.prepare_from_regions(regions({"fill\n", "first\n"}));
        finish(pair, pair.route.begin(prepared), true);
    }
}

void test_zero_components_and_component_boundaries() {
    {
        Pair pair;
        const std::vector<std::vector<uint8_t>> empty_regions;
        const PreparedTUPtr empty = pair.c.prepare_from_regions(empty_regions);
        const CActiveTx& active = pair.route.begin(empty);
        require(active.dict.empty() && active.body.empty() && active.begin.raw_bytes == 0,
                "empty TU did not use legal zero-length components");
        pair.f.begin(pair.session, active.begin);
        require(pair.f.need(pair.session).missing.empty(), "empty TU produced Need keys");
        require(pair.f.materialize_and_verify(pair.session).empty(),
                "empty TU materialized bytes");
        pair.route.accept_commit(pair.f.commit_input(pair.session));
    }
    {
        Pair pair;
        const auto prepared = pair.c.prepare_from_regions(regions({"short\n", "long\n"}));
        const CActiveTx& active = pair.route.begin(prepared);
        pair.f.begin(pair.session, active.begin);
        require(active.dict.size() > 1, "DICT boundary fixture is too short");
        pair.f.append_dict(pair.session,
                           std::span<const uint8_t>(active.dict).first(active.dict.size() - 1));
        require_throws<std::logic_error>([&] { (void)pair.f.need(pair.session); },
                                         "one-byte-short DICT produced Need");
        pair.f.append_dict(pair.session,
                           std::span<const uint8_t>(active.dict).last(1));
        const Need need = pair.f.need(pair.session);
        const std::array<uint8_t, 1> extra{0};
        require_throws<std::logic_error>(
            [&] { pair.f.append_dict(pair.session, extra); },
            "one-byte-long DICT was accepted after completion");
        require_throws<std::length_error>(
            [&] {
                std::vector<uint8_t> too_long = active.body;
                too_long.push_back(0);
                pair.f.append_body(pair.session, too_long);
            },
            "one-byte-long BODY was accepted");
        pair.f.append_body(pair.session, active.body);
        for (const ImmutableObject& object : pair.route.build_fill(need))
            pair.f.apply_object(pair.session, object);
        pair.f.materialize_and_verify(pair.session);
        pair.route.accept_commit(pair.f.commit_input(pair.session));
    }
}

void test_p29_key_vector_encoding_boundary() {
    static_assert(kP29KeyVectorEncoding == 1);
    Pair pair;
    const CActiveTx& active = pair.route.begin(
        pair.c.prepare_from_regions(regions({"encoding\n", "boundary\n"})));

    TxBegin unsupported = active.begin;
    unsupported.dict.encoding = kP29KeyVectorEncoding + 1;
    require_throws<std::invalid_argument>(
        [&] { pair.f.begin(pair.session, unsupported); },
        "F accepted an unsupported P29 DICT encoding");

    unsupported = active.begin;
    unsupported.body.encoding = kP29KeyVectorEncoding + 1;
    require_throws<std::invalid_argument>(
        [&] { pair.f.begin(pair.session, unsupported); },
        "F accepted an unsupported P29 BODY encoding");

    finish(pair, active);
    require(pair.route.next_rel_seq().value == 1,
            "rejected P29 encodings left pending F transaction state");
}

void test_disconnect_replay_every_object_boundary() {
    const auto input = regions({"r0\n", "r1\n", "r2\n", "r3\n"});
    size_t fill_count = 0;
    {
        Pair probe;
        const CActiveTx& active = probe.route.begin(probe.c.prepare_from_regions(input));
        fill_count = probe.route.build_fill(start(probe, active)).size();
    }
    for (size_t delivered = 0; delivered <= fill_count; ++delivered) {
        Pair pair;
        const CActiveTx& active = pair.route.begin(pair.c.prepare_from_regions(input));
        const Need before = start(pair, active);
        const std::vector<ImmutableObject> fill = pair.route.build_fill(before);
        for (size_t i = 0; i != delivered; ++i)
            pair.f.apply_object(pair.session, fill[i]);
        pair.f.disconnect(pair.session);
        const ReconnectResult resumed = reconnect(pair.route, pair.f, HistoryNonce{400 + delivered});
        require(resumed.outcome == ReconnectOutcome::ExactMatch && resumed.replay_active,
                "partial Fill did not take exact replay path");
        pair.session = resumed.session;
        pair.f.begin(pair.session, pair.route.active()->begin, true);
        pair.f.append_dict(pair.session, pair.route.active()->dict);
        const Need after = pair.f.need(pair.session);
        require(after.missing.size() == fill_count - delivered,
                "replay did not retain exactly complete objects");
        pair.f.append_body(pair.session, pair.route.active()->body);
        for (const ImmutableObject& object : pair.route.build_fill(after))
            pair.f.apply_object(pair.session, object);
        pair.f.materialize_and_verify(pair.session);
        pair.route.accept_commit(pair.f.commit_input(pair.session));
        require(pair.route.next_rel_seq().value == 1,
                "replay advanced route more than once");
    }
}

void test_lost_commit_acknowledgement() {
    ActionTrace trace;
    Pair pair(&trace);
    const CActiveTx& active = pair.route.begin(
        pair.c.prepare_from_regions(regions({"ack\n", "was\n", "lost\n"})));
    const TxCommit retained = finish(pair, active, false, false);
    require(pair.route.active() && pair.route.next_rel_seq().value == 0,
            "fixture did not retain C ACTIVE_TX after F commit");
    pair.f.disconnect(pair.session);
    const ReconnectResult resumed = reconnect(pair.route, pair.f, HistoryNonce{500});
    require(resumed.outcome == ReconnectOutcome::LostFinalAcknowledgement &&
                !pair.route.active() && pair.route.next_rel_seq().value == 1 &&
                pair.route.state_digest() == retained.post_state_digest,
            "retained complete commit was not accepted exactly once");
    pair.session = resumed.session;
    const auto f_commit = std::find_if(
        trace.records().begin(), trace.records().end(), [](const auto& record) {
            return record.actor == ActorSide::F &&
                   record.action == ActionType::INPUT_COMMITTED;
        });
    const auto lost_accept = std::find_if(
        trace.records().begin(), trace.records().end(), [](const auto& record) {
            return record.actor == ActorSide::C &&
                   record.action == ActionType::LOST_COMMIT_ACCEPTED;
        });
    require(f_commit != trace.records().end() &&
                lost_accept != trace.records().end() && f_commit < lost_accept,
            "trace did not expose the durable-F/lost-C-acceptance window");
    const auto trace_error = check_action_trace(trace.records());
    require(!trace_error,
            trace_error ? *trace_error : "lost-commit trace failed");
}

void test_store_reset_history_reset_and_retry_another_f() {
    {
        const CStoreGuid c_guid = Id128::from_u64(80);
        FStore f(Id128::from_u64(81));
        const SessionHandle session = f.connect(c_guid);
        require_throws<std::logic_error>(
            [&] { f.start_route(session, HistoryNonce{82}, Digest128{}); },
            "HISTORY_RESET trusted a non-derived initial digest");
        require(!f.resume(session).route_present,
                "rejected HISTORY_RESET created route state");
        f.start_route(session, HistoryNonce{82},
                      initial_route_digest(c_guid, HistoryNonce{82}));
    }
    {
        Pair pair;
        require(pair.f.object_count(pair.c.guid()) == 0,
                "empty history-reset fixture unexpectedly has objects");
        pair.f.forget_route(pair.session);
        pair.f.disconnect(pair.session);
        require_throws<std::invalid_argument>(
            [&] {
                (void)reconnect(pair.route, pair.f, pair.route.history_nonce());
            },
            "route reset reused its HISTORY_NONCE");
        const ReconnectResult reset = reconnect(pair.route, pair.f, HistoryNonce{550});
        require(reset.outcome == ReconnectOutcome::RouteHistoryReset &&
                    !reset.replay_active &&
                    pair.f.object_count(pair.c.guid()) == 0,
                "empty route reset did not preserve an empty object namespace");
        pair.session = reset.session;
    }
    {
        Pair pair;
        const auto first = pair.c.prepare_from_regions(regions({"keep\n", "objects\n"}));
        finish(pair, pair.route.begin(first));
        const auto second = pair.c.prepare_from_regions(regions({"cold\n", "again\n"}));
        pair.route.begin(second);
        pair.f.destructive_cache_reset(Id128::from_u64(201));
        const ReconnectResult cold = reconnect(pair.route, pair.f, HistoryNonce{600});
        require(cold.outcome == ReconnectOutcome::ColdFStore && cold.replay_active &&
                    pair.route.active()->begin.profile == ProfileId::P29 &&
                    pair.route.active()->begin.p29_root_mode ==
                        P29RootMode::HistoryIndependent,
                "F store reset did not re-encode active TU independently");
        pair.session = cold.session;
        require(!start(pair, *pair.route.active(), true).missing.empty(),
                "cold F store did not request repopulation");
        pair.f.append_body(pair.session, pair.route.active()->body);
        for (const auto& object : pair.route.build_fill(pair.f.need(pair.session)))
            pair.f.apply_object(pair.session, object);
        pair.f.materialize_and_verify(pair.session);
        pair.route.accept_commit(pair.f.commit_input(pair.session));
    }
    {
        Pair pair;
        const auto input = pair.c.prepare_from_regions(regions({"route\n", "reset\n"}));
        finish(pair, pair.route.begin(input));
        const size_t retained = pair.f.object_count(pair.c.guid());
        pair.route.begin(input);
        pair.f.forget_route(pair.session);
        pair.f.disconnect(pair.session);
        const ReconnectResult reset = reconnect(pair.route, pair.f, HistoryNonce{700});
        require(reset.outcome == ReconnectOutcome::RouteHistoryReset &&
                    reset.replay_active &&
                    pair.f.object_count(pair.c.guid()) == retained,
                "route reset discarded reusable objects or active TU");
        pair.session = reset.session;
        require(start(pair, *pair.route.active(), true).missing.empty(),
                "route reset requested retained objects");
        pair.f.append_body(pair.session, pair.route.active()->body);
        pair.f.materialize_and_verify(pair.session);
        pair.route.accept_commit(pair.f.commit_input(pair.session));
    }
    {
        ActionTrace trace;
        Pair pair(&trace);
        const auto input = pair.c.prepare_from_regions(regions({"retry\n", "F2\n"}));
        const CActiveTx& active = pair.route.begin(input);
        const Need need = start(pair, active);
        const auto fill = pair.route.build_fill(need);
        pair.f.apply_object(pair.session, fill.front());
        pair.f.disconnect(pair.session);
        FStore second(Id128::from_u64(900), 1, &trace);
        const ReconnectResult moved = reconnect(pair.route, second, HistoryNonce{901});
        require(moved.outcome == ReconnectOutcome::ColdFStore && moved.replay_active &&
                    pair.route.active()->begin.profile == ProfileId::P29 &&
                    pair.route.active()->begin.p29_root_mode ==
                        P29RootMode::HistoryIndependent,
                "retry to another F did not use cold independent encoding");
        const SessionHandle second_session = moved.session;
        second.begin(second_session, pair.route.active()->begin, true);
        second.append_dict(second_session, pair.route.active()->dict);
        const Need second_need = second.need(second_session);
        require(second_need.missing.size() == pair.route.active()->manifest.size(),
                "second F inherited the first F's object state");
        second.append_body(second_session, pair.route.active()->body);
        for (const auto& object : pair.route.build_fill(second_need))
            second.apply_object(second_session, object);
        second.materialize_and_verify(second_session);
        pair.route.accept_commit(second.commit_input(second_session));
        const auto trace_error = check_action_trace(trace.records());
        require(!trace_error,
                trace_error ? *trace_error : "multi-F trace failed");
    }
    {
        ActionTrace trace;
        CAuthority c(Id128::from_u64(1300));
        FStore first(Id128::from_u64(1301), 1, &trace);
        FStore second(Id128::from_u64(1302), 1, &trace);
        CRoute first_route(c, first.guid(), HistoryNonce{1303}, &trace);
        CRoute second_route(c, second.guid(), HistoryNonce{1304}, &trace);
        SessionHandle first_session =
            reconnect(first_route, first, HistoryNonce{1305}).session;
        SessionHandle second_session =
            reconnect(second_route, second, HistoryNonce{1306}).session;
        const PreparedTUPtr prepared =
            c.prepare_from_regions(regions({"already\n", "at F2\n"}));

        const CActiveTx& warm = second_route.begin(prepared);
        second.begin(second_session, warm.begin);
        second.append_dict(second_session, warm.dict);
        const Need warm_need = second.need(second_session);
        second.append_body(second_session, warm.body);
        for (const auto& object : second_route.build_fill(warm_need))
            second.apply_object(second_session, object);
        second.materialize_and_verify(second_session);
        second_route.accept_commit(second.commit_input(second_session));
        const size_t retained = second.object_count(c.guid());
        second.disconnect(second_session);

        const CActiveTx& in_flight = first_route.begin(prepared);
        first.begin(first_session, in_flight.begin);
        first.append_dict(first_session, in_flight.dict);
        first.disconnect(first_session);
        const ReconnectResult moved =
            reconnect(first_route, second, HistoryNonce{1307});
        require(moved.outcome == ReconnectOutcome::ColdFStore &&
                    moved.replay_active && second.object_count(c.guid()) == retained,
                "retry to a previously used F discarded its C namespace");
        second_session = moved.session;
        second.begin(second_session, first_route.active()->begin, true);
        second.append_dict(second_session, first_route.active()->dict);
        const Need retained_need = second.need(second_session);
        require(retained_need.missing.empty(),
                "retry to a warm F requested already retained immutable objects");
        second.append_body(second_session, first_route.active()->body);
        second.materialize_and_verify(second_session);
        first_route.accept_commit(second.commit_input(second_session));
        const auto trace_error = check_action_trace(trace.records());
        require(!trace_error,
                trace_error ? *trace_error : "warm retry-to-F trace failed");
    }
}

void test_session_replacement_and_terminal_serial() {
    Pair pair;
    const CActiveTx& active = pair.route.begin(
        pair.c.prepare_from_regions(regions({"replace\n", "session\n"})));
    (void)start(pair, active);
    const SessionHandle stale = pair.session;
    const SessionHandle replacement = pair.f.connect(pair.c.guid());
    require_throws<std::logic_error>(
        [&] { pair.f.append_body(stale, active.body); },
        "stale session completed work after replacement");
    pair.session = replacement;
    finish(pair, active, false, true, true);

    FStore terminal(Id128::from_u64(1000), std::numeric_limits<uint64_t>::max());
    const CStoreGuid c_guid = Id128::from_u64(1001);
    const SessionHandle last = terminal.connect(c_guid);
    require(last.serial == std::numeric_limits<uint64_t>::max(),
            "last session serial was not allocated");
    require_throws<std::overflow_error>([&] { (void)terminal.connect(c_guid); },
                                        "session serial wrapped after exhaustion");
    require(!terminal.resume(last).namespace_present,
            "failed allocation mutated the current session");
    require_throws<std::overflow_error>([&] { (void)terminal.connect(c_guid); },
                                        "session exhaustion was not terminal");
    terminal.destructive_cache_reset(Id128::from_u64(1002));
    const SessionHandle after_reset = terminal.connect(c_guid);
    require(after_reset.serial == 1,
            "new F store incarnation did not reset session serial space");
    require_throws<std::logic_error>([&] { (void)terminal.resume(last); },
                                     "old F-store handle survived GUID reset");

    FStore another(Id128::from_u64(1003));
    const SessionHandle coincident = another.connect(c_guid);
    require(coincident.serial == after_reset.serial,
            "cross-F fencing fixture did not produce equal serials");
    require_throws<std::logic_error>([&] { (void)another.resume(after_reset); },
                                     "session handle was accepted by another F store");
}

void test_two_c_namespaces_with_equal_key_values() {
    ActionTrace trace;
    FStore f(Id128::from_u64(1100), 1, &trace);
    CAuthority c1(Id128::from_u64(1101));
    CAuthority c2(Id128::from_u64(1102));
    CRoute r1(c1, f.guid(), HistoryNonce{11}, &trace);
    CRoute r2(c2, f.guid(), HistoryNonce{12}, &trace);
    const SessionHandle s1 = reconnect(r1, f, HistoryNonce{13}).session;
    const SessionHandle s2 = reconnect(r2, f, HistoryNonce{14}).session;
    const auto p1 = c1.prepare_from_regions(regions({"first C\n"}));
    const auto p2 = c2.prepare_from_regions(regions({"second C\n"}));
    require(p1->regions.front().wire_value() == p2->regions.front().wire_value(),
            "fixture did not produce equal C-local Key64 values");
    const auto drive = [&](CAuthority&, CRoute& route, SessionHandle session,
                           const PreparedTUPtr& prepared) {
        const CActiveTx& active = route.begin(prepared);
        f.begin(session, active.begin);
        f.append_dict(session, active.dict);
        const Need need = f.need(session);
        f.append_body(session, active.body);
        for (const auto& object : route.build_fill(need)) f.apply_object(session, object);
        f.materialize_and_verify(session);
        route.accept_commit(f.commit_input(session));
    };
    drive(c1, r1, s1, p1);
    drive(c2, r2, s2, p2);
    require(f.contains(c1.guid(), p1->regions.front()) &&
                f.contains(c2.guid(), p2->regions.front()),
            "equal Key64 values crossed C_STORE_GUID namespaces");
    const auto trace_error = check_action_trace(trace.records());
    require(!trace_error,
            trace_error ? *trace_error : "C2F1 trace shared checker state");
}

void test_canonical_action_trace() {
    ActionTrace trace;
    Pair pair(&trace);
    const auto input = pair.c.prepare_from_regions(regions({"trace\n", "bridge\n"}));
    finish(pair, pair.route.begin(input));
    const auto f_commit = std::find_if(
        trace.records().begin(), trace.records().end(), [](const auto& record) {
            return record.actor == ActorSide::F &&
                   record.action == ActionType::INPUT_COMMITTED;
        });
    const auto c_accept = std::find_if(
        trace.records().begin(), trace.records().end(), [](const auto& record) {
            return record.actor == ActorSide::C &&
                   record.action == ActionType::COMMIT_ACCEPTED;
        });
    require(f_commit != trace.records().end() &&
                c_accept != trace.records().end() && f_commit < c_accept,
            "normal trace conflated F durability with C acceptance");
    const auto error = check_action_trace(trace.records());
    require(!error, error ? *error : "canonical trace failed");
    if (const char* trace_path = std::getenv("P50_TRACE_PATH"))
        write_action_trace(trace, trace_path);

    const auto c_begin = std::find_if(
        trace.records().begin(), trace.records().end(), [](const auto& record) {
            return record.actor == ActorSide::C &&
                   record.action == ActionType::TX_BEGIN;
        });
    const auto f_begin = std::find_if(
        trace.records().begin(), trace.records().end(), [](const auto& record) {
            return record.actor == ActorSide::F &&
                   record.action == ActionType::TX_BEGIN;
        });
    require(c_begin != trace.records().end() && f_begin != trace.records().end(),
            "trace fixture lacks C/F TX_BEGIN");
    ActionRecord abort = *c_begin;
    abort.action = ActionType::TX_ABORTED;

    std::vector<ActionRecord> changed(trace.records().begin(), f_begin + 1);
    changed.push_back(abort);
    require(check_action_trace(changed).has_value(),
            "trace checker accepted abort while F had a pending transaction");

    changed.assign(trace.records().begin(), f_commit + 1);
    changed.push_back(abort);
    require(check_action_trace(changed).has_value(),
            "trace checker accepted abort after F commit but before C acceptance");

    constexpr std::array stale_session_actions{
        ActionType::HISTORY_RESET, ActionType::TX_BEGIN,
        ActionType::DICT_COMPLETE, ActionType::BODY_COMPLETE,
        ActionType::NEED_RECORDED, ActionType::OBJECT_APPLIED,
        ActionType::INPUT_MATERIALIZED, ActionType::INPUT_COMMITTED,
    };
    for (ActionType action : stale_session_actions) {
        changed = trace.records();
        const auto position = std::find_if(
            changed.begin(), changed.end(), [action](const auto& record) {
                return record.actor == ActorSide::F && record.action == action;
            });
        require(position != changed.end(),
                "trace fixture lacks an F action for current-session checking");
        ++position->session_serial;
        require(check_action_trace(changed).has_value(),
                "trace checker accepted an F action from a stale session");
    }

    changed = trace.records();
    const auto dict = std::find_if(changed.begin(), changed.end(), [](const auto& record) {
        return record.action == ActionType::DICT_COMPLETE;
    });
    const auto need = std::find_if(changed.begin(), changed.end(), [](const auto& record) {
        return record.action == ActionType::NEED_RECORDED;
    });
    require(dict != changed.end() && need != changed.end(), "trace fixture lacks DICT/NEED");
    std::iter_swap(dict, need);
    require(check_action_trace(changed).has_value(),
            "trace checker accepted NEED before DICT completion");

    changed = trace.records();
    const auto object = std::find_if(
        changed.begin(), changed.end(), [](const auto& record) {
            return record.action == ActionType::OBJECT_APPLIED;
        });
    require(object != changed.end(), "trace fixture lacks OBJECT_APPLIED");
    auto repeat = *object;
    repeat.duplicate = true;
    repeat.content_digest.bytes.front() ^= 1;
    changed.insert(object + 1, repeat);
    require(check_action_trace(changed).has_value(),
            "trace checker accepted changed content for an immutable Key64");

    ActionTrace replay_trace;
    Pair replay(&replay_trace, 1200, 1201);
    const CActiveTx& active = replay.route.begin(
        replay.c.prepare_from_regions(regions({"disconnect\n", "replay\n"})));
    const Need before = start(replay, active);
    const auto fill = replay.route.build_fill(before);
    replay.f.apply_object(replay.session, fill.front());
    replay.f.disconnect(replay.session);
    const ReconnectResult resumed = reconnect(replay.route, replay.f, HistoryNonce{1202});
    replay.session = resumed.session;
    finish(replay, *replay.route.active(), false, true, true);
    const auto replay_error = check_action_trace(replay_trace.records());
    require(!replay_error, replay_error ? *replay_error : "replay trace failed");
    changed = replay_trace.records();
    const auto replay_action = std::find_if(
        changed.begin(), changed.end(), [](const auto& record) {
            return record.actor == ActorSide::F &&
                   record.action == ActionType::ACTIVE_REPLAYED;
        });
    require(replay_action != changed.end(), "replay trace lacks ACTIVE_REPLAYED");
    ++replay_action->session_serial;
    require(check_action_trace(changed).has_value(),
            "trace checker accepted replay from a stale session");
}

}  // namespace

int main() {
    test_key_limits_and_mixed_generations();
    test_tu_seq_is_not_route_order();
    test_separate_preparation_real_interning_and_p29();
    test_exact_need_and_duplicate_application();
    test_valid_then_invalid_fill_retains_first_object();
    test_trailing_partial_fill_blocks_commit_and_replays();
    test_body_and_fill_complete_in_both_orders();
    test_zero_components_and_component_boundaries();
    test_p29_key_vector_encoding_boundary();
    test_disconnect_replay_every_object_boundary();
    test_lost_commit_acknowledgement();
    test_store_reset_history_reset_and_retry_another_f();
    test_session_replacement_and_terminal_serial();
    test_two_c_namespaces_with_equal_key_values();
    test_canonical_action_trace();
    std::cout << "p50_slice0_test: all corrected M1 gates passed\n";
    return 0;
}
