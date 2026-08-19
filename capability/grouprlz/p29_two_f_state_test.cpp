// Phase-B deterministic two-F fixture.
//
//   g++ -std=c++17 -O2 -pthread -Wall -Wextra -Wpedantic -Werror
//       p29_two_f_state_test.cpp -o t && ./t
#include "p29_two_f_state.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <exception>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

p29::Opaque128 opaque(uint8_t base) {
    p29::Opaque128 value{};
    for (size_t index = 0; index < value.size(); ++index)
        value[index] = static_cast<uint8_t>(base + index * 5);
    return value;
}

p29::Digest128 digest(uint64_t value) {
    return {value * 0x9e3779b97f4a7c15ULL,
            (value + 17) * 0xbf58476d1ce4e5b9ULL};
}

struct Fixture {
    explicit Fixture(uint8_t generation_base = 31)
        : authority(opaque(1), opaque(generation_base), {3, 1024, 12}) {
        std::vector<std::string> atoms;
        for (unsigned index = 0; index < 20; ++index)
            atoms.push_back("region-atom-" + std::to_string(index));
        const auto atom_ids = authority.publish_atoms(atoms).ids;
        std::vector<std::vector<uint32_t>> regions;
        for (uint32_t atom : atom_ids) regions.push_back({atom});
        region_ids = authority.publish_regions(regions).ids;
    }

    std::shared_ptr<const p29::SharedCAuthority::AdmittedTu>
    admit(const std::vector<uint32_t>& regions, uint8_t request, uint64_t extent = 4096) {
        p29::SharedCAuthority::AdmissionInput input;
        input.producer_request_id = opaque(request);
        input.source_extent = extent;
        input.output_digest = digest(request);
        input.regions = regions;
        const auto reply = authority.admit(input);
        check(reply.result == p29::SharedCAuthority::AdmissionResult::Created,
              "fixture logical TU was not a new admission");
        return reply.admitted;
    }

    p29::SharedCAuthority authority;
    std::vector<uint32_t> region_ids;
};

p29::TransactionClosure closure_for(
    const p29::SharedCAuthority::AdmittedTu& admitted, uint64_t route_sequence) {
    return {digest(1000 + admitted.canonical_admission_sequence * 31 + route_sequence),
            admitted.output_digest, admitted.source_extent};
}

p29::AckReceipt transfer(p29::FRouteEndpoint& endpoint,
                         const std::shared_ptr<const p29::SharedCAuthority::AdmittedTu>& admitted,
                         p29::RepresentationMode mode, uint8_t key_base,
                         bool recover_lost_ack = false) {
    endpoint.prepare(admitted, {opaque(240), opaque(key_base)});
    const auto closure = closure_for(*admitted, endpoint.candidate(mode).route_sequence);
    endpoint.begin_attempt(mode, closure);
    const p29::AckReceipt first = endpoint.commit_at_f();
    if (recover_lost_ack) {
        const uint64_t applies = endpoint.f_apply_count();
        const auto before = endpoint.state_mark();
        const p29::AckReceipt retained = endpoint.recover_retained_ack();
        check(retained == first, "lost-Ack recovery returned a different receipt");
        check(endpoint.state_mark() == before,
              "asking for a retained Ack changed F or C pending state");
        check(endpoint.f_apply_count() == applies,
              "lost-Ack recovery applied the TU a second time");
        endpoint.deliver_ack(retained);
    } else {
        endpoint.deliver_ack(first);
    }
    const auto committed = endpoint.state_mark();
    check(committed.c_route.committed_sequence == endpoint.committed_spans().size() &&
              committed.c_route.occurrence_count == endpoint.occurrences().size() &&
              committed.c_mirror.mirror_sequence == committed.c_route.committed_sequence,
          "successful representation did not commit its exact C route history");
    return first;
}

size_t count_copy(const p29::RouteCandidate& candidate) {
    return static_cast<size_t>(std::count_if(
        candidate.definitions.begin(), candidate.definitions.end(),
        [](const p29::CandidateBlockDefinition& definition) {
            return definition.mode == p29::BlockDefinitionMode::RouteCopy;
        }));
}

size_t count_children(const p29::RouteCandidate& candidate) {
    return static_cast<size_t>(std::count_if(
        candidate.definitions.begin(), candidate.definitions.end(),
        [](const p29::CandidateBlockDefinition& definition) {
            return definition.mode == p29::BlockDefinitionMode::Children;
        }));
}

std::string candidate_signature(const p29::RouteCandidate& candidate) {
    std::ostringstream out;
    out << unsigned(candidate.mode) << ':';
    for (const p29::Ref& ref : candidate.root)
        out << (ref.kind == p29::RefKind::Region ? 'R' : 'B') << ref.id << ',';
    out << '|';
    for (const auto& definition : candidate.definitions)
        out << definition.block_id <<
            (definition.mode == p29::BlockDefinitionMode::RouteCopy ? 'C' : 'D') << ',';
    return out.str();
}

struct ScheduleResult {
    std::array<std::vector<std::string>, 2> per_f_signatures;
    std::array<std::vector<uint32_t>, 2> per_f_block_ids;
    size_t catalogue_size = 0;
};

class StartGate {
public:
    explicit StartGate(unsigned participants) : participants_(participants) {}
    void arrive_and_wait() {
        ready_.fetch_add(1, std::memory_order_acq_rel);
        while (!go_.load(std::memory_order_acquire)) std::this_thread::yield();
    }
    void release_when_ready() {
        while (ready_.load(std::memory_order_acquire) != participants_)
            std::this_thread::yield();
        go_.store(true, std::memory_order_release);
    }

private:
    const unsigned participants_;
    std::atomic<unsigned> ready_{0};
    std::atomic<bool> go_{false};
};

ScheduleResult run_schedule(const std::string& schedule) {
    Fixture fixture;
    const p29::FCacheIdentity f1{opaque(101), 7}, f2{opaque(141), 7};
    p29::FRouteEndpoint a(fixture.authority, f1, 0), b(fixture.authority, f2, 0);
    std::array<p29::FRouteEndpoint*, 2> endpoints{&a, &b};
    const std::vector<uint32_t> repeated{
        fixture.region_ids[0], fixture.region_ids[1], fixture.region_ids[2],
        fixture.region_ids[3], fixture.region_ids[4], fixture.region_ids[5]};
    ScheduleResult result;
    std::array<size_t, 2> local_ordinal{};
    uint8_t request = 50;
    for (char destination : schedule) {
        const size_t index = destination == 'A' ? 0 : 1;
        const auto admitted = fixture.admit(repeated, request++);
        p29::FRouteEndpoint& endpoint = *endpoints[index];
        endpoint.prepare(admitted, {opaque(230), opaque(uint8_t(180 + local_ordinal[index]))});
        const auto route = endpoint.candidate(p29::RepresentationMode::RouteS1);
        result.per_f_signatures[index].push_back(candidate_signature(route));
        for (const auto& definition : route.definitions)
            result.per_f_block_ids[index].push_back(definition.block_id);

        const auto close = closure_for(*admitted, route.route_sequence);
        endpoint.begin_attempt(p29::RepresentationMode::RouteS1, close);
        const auto ack = endpoint.commit_at_f();
        endpoint.deliver_ack(ack);
        ++local_ordinal[index];
    }
    check(a.committed_spans().size() == 4 && b.committed_spans().size() == 4,
          "schedule did not commit four TUs at each F");
    result.catalogue_size = fixture.authority.block_count();
    return result;
}

}  // namespace

int main() {
    // An aborted prepare can reuse the durable route sequence, but never its internal ticket.
    // The stale ticket must not resolve a later prepare of the same AdmittedTu.
    {
        Fixture fixture;
        const std::vector<uint32_t> x{fixture.region_ids[0], fixture.region_ids[1],
                                      fixture.region_ids[2], fixture.region_ids[3]};
        const auto admitted = fixture.admit(x, 40);
        const p29::RouteLaneIdentity lane{{opaque(101), 7}, 0};
        const auto first = fixture.authority.prepare_route(lane, admitted);
        fixture.authority.abort_route(first);
        const auto second = fixture.authority.prepare_route(lane, admitted);
        const auto before_stale = fixture.authority.route_history_mark(lane);
        bool stale_refused = false;
        try {
            fixture.authority.abort_route(first);
        } catch (const std::logic_error&) {
            stale_refused = true;
        }
        check(stale_refused &&
                  fixture.authority.route_history_mark(lane) == before_stale,
              "stale route ticket was accepted for a later prepare");
        if (fixture.authority.route_history_mark(lane).pending)
            fixture.authority.abort_route(second);
    }

    // Independent F endpoints may prepare and close concurrently.  The shared authority
    // serializes canonical publication while neither F's receiver/store state is shared.
    {
        Fixture fixture;
        const std::vector<uint32_t> x{fixture.region_ids[0], fixture.region_ids[1],
                                      fixture.region_ids[2], fixture.region_ids[3],
                                      fixture.region_ids[4], fixture.region_ids[5]};
        p29::FRouteEndpoint f1(fixture.authority, {opaque(101), 7}, 0);
        p29::FRouteEndpoint f2(fixture.authority, {opaque(141), 7}, 0);
        transfer(f1, fixture.admit(x, 41), p29::RepresentationMode::Raw, 141);
        transfer(f2, fixture.admit(x, 42), p29::RepresentationMode::Raw, 142);
        const std::array<std::shared_ptr<const p29::SharedCAuthority::AdmittedTu>, 2> admitted{
            fixture.admit(x, 43), fixture.admit(x, 44)};
        std::array<p29::FRouteEndpoint*, 2> endpoints{&f1, &f2};
        std::array<std::exception_ptr, 2> errors;
        StartGate gate(2);
        std::array<std::thread, 2> workers;
        for (size_t index = 0; index < workers.size(); ++index) {
            workers[index] = std::thread([&, index] {
                try {
                    gate.arrive_and_wait();
                    p29::FRouteEndpoint& endpoint = *endpoints[index];
                    endpoint.prepare(admitted[index],
                                     {opaque(235), opaque(uint8_t(143 + index))});
                    const auto route =
                        endpoint.candidate(p29::RepresentationMode::RouteS1);
                    endpoint.begin_attempt(
                        route, closure_for(*admitted[index], route.route_sequence));
                    const auto ack = endpoint.commit_at_f();
                    endpoint.deliver_ack(ack);
                } catch (...) {
                    errors[index] = std::current_exception();
                }
            });
        }
        gate.release_when_ready();
        for (auto& worker : workers) worker.join();
        check(!errors[0] && !errors[1], "concurrent independent-F transaction threw");
        check(fixture.authority.route_history_mark(f1.lane()).committed_sequence == 2 &&
                  fixture.authority.route_history_mark(f2.lane()).committed_sequence == 2,
              "concurrent independent-F route commits diverged");
    }

    // AAAA/BBBB concentration, ABAB alternation and AABB switching must produce the same
    // per-route semantic history.  Global admission interleaving changes neither route-local
    // source coordinates nor canonical identity.
    {
        const ScheduleResult concentrated = run_schedule("AAAABBBB");
        const ScheduleResult alternating = run_schedule("ABABABAB");
        const ScheduleResult switching = run_schedule("AABBAABB");
        check(concentrated.per_f_signatures == alternating.per_f_signatures &&
                  concentrated.per_f_signatures == switching.per_f_signatures,
              "AAAA/BBBB, ABAB and AABB changed a per-F route plan");
        check(concentrated.catalogue_size == alternating.catalogue_size &&
                  concentrated.catalogue_size == switching.catalogue_size,
              "routing order changed global catalogue size");
        check(!concentrated.per_f_block_ids[0].empty() &&
                  concentrated.per_f_block_ids[0] == concentrated.per_f_block_ids[1],
              "two Fs did not reuse the same canonical Block IDs");
    }

    // Repeated RAW wins must still advance route history while leaving Block holes.  The
    // later ROUTE_S1 transaction may then install the already-canonical Block from its own
    // retained route source.
    {
        Fixture fixture;
        const std::vector<uint32_t> x{fixture.region_ids[0], fixture.region_ids[1],
                                      fixture.region_ids[2], fixture.region_ids[3],
                                      fixture.region_ids[4], fixture.region_ids[5]};
        p29::FRouteEndpoint f(fixture.authority, {opaque(101), 7}, 0);
        for (uint8_t request = 70; request < 73; ++request) {
            const auto admitted = fixture.admit(x, request);
            f.prepare(admitted, {opaque(230), opaque(request)});
            const auto route = f.candidate(p29::RepresentationMode::RouteS1);
            if (request > 70)
                check(!route.definitions.empty() && count_copy(route) > 0,
                      "route history did not learn while RAW kept winning");
            const auto close = closure_for(*admitted, route.route_sequence);
            f.begin_attempt(p29::RepresentationMode::Raw, close);
            const auto ack = f.commit_at_f();
            f.deliver_ack(ack);
            check(f.blocks().known_count() == 0,
                  "a RAW transaction installed a route candidate Block");
        }
        const auto admitted = fixture.admit(x, 73);
        f.prepare(admitted, {opaque(230), opaque(73)});
        const auto route = f.candidate(p29::RepresentationMode::RouteS1);
        check(count_copy(route) > 0, "fourth route TU had no legal local COPY definition");
        const auto close = closure_for(*admitted, route.route_sequence);
        f.begin_attempt(route, close);
        const auto ack = f.commit_at_f();
        f.deliver_ack(ack);
        check(f.blocks().known_count() > 0,
              "selected ROUTE_S1 transaction did not fill the earlier Block hole");
        const auto mark = fixture.authority.route_history_mark(f.lane());
        check(mark.committed_sequence == 4 && mark.occurrence_count == x.size() * 4,
              "route commit did not advance for every successful representation");
    }

    // A COPY candidate is meaningful only in the matcher/F pair that produced its source
    // coordinate.  Give both Fs the same committed content, then present F1's real COPY
    // candidate to F2 while both have a pending second sight.
    {
        Fixture fixture;
        const std::vector<uint32_t> x{fixture.region_ids[0], fixture.region_ids[1],
                                      fixture.region_ids[2], fixture.region_ids[3],
                                      fixture.region_ids[4], fixture.region_ids[5]};
        p29::FRouteEndpoint f1(fixture.authority, {opaque(101), 7}, 0);
        p29::FRouteEndpoint f2(fixture.authority, {opaque(141), 7}, 0);
        transfer(f1, fixture.admit(x, 74), p29::RepresentationMode::Raw, 174);
        check(fixture.authority.route_history_mark(f2.lane()).committed_sequence == 0,
              "F1 route history appeared in F2 before F2's first TU");
        transfer(f2, fixture.admit(x, 75), p29::RepresentationMode::Raw, 175);
        const auto second1 = fixture.admit(x, 76);
        const auto second2 = fixture.admit(x, 77);
        f1.prepare(second1, {opaque(230), opaque(176)});
        f2.prepare(second2, {opaque(230), opaque(177)});
        const auto copy1 = f1.candidate(p29::RepresentationMode::RouteS1);
        const auto copy2 = f2.candidate(p29::RepresentationMode::RouteS1);
        check(count_copy(copy1) > 0 && count_copy(copy2) > 0,
              "cross-route fixture produced no real COPY candidates");
        check(!f2.candidate_legal_for(copy1) && !f1.candidate_legal_for(copy2),
              "one F accepted another F's COPY candidate/source coordinate");
        auto mixed_proof = copy2;
        const auto source1 = std::find_if(
            copy1.definitions.begin(), copy1.definitions.end(),
            [](const p29::CandidateBlockDefinition& definition) {
                return definition.copy.has_value();
            });
        auto target2 = std::find_if(
            mixed_proof.definitions.begin(), mixed_proof.definitions.end(),
            [](const p29::CandidateBlockDefinition& definition) {
                return definition.copy.has_value();
            });
        if (source1 != copy1.definitions.end() && target2 != mixed_proof.definitions.end())
            target2->copy = source1->copy;
        check(!f2.candidate_legal_for(mixed_proof),
              "F2 accepted a COPY proof whose outer candidate was relabelled for F2");
        const auto close1 = closure_for(*second1, copy1.route_sequence);
        const auto close2 = closure_for(*second2, copy2.route_sequence);
        f1.begin_attempt(copy1, close1);
        f2.begin_attempt(copy2, close2);
        const auto ack1 = f1.commit_at_f();
        const auto ack2 = f2.commit_at_f();
        f1.deliver_ack(ack1);
        f2.deliver_ack(ack2);
    }

    // Admit one seed without routing it.  The next global plan has a Block which is absent
    // from both route histories.  F1 and then F2 must each receive explicit children for the
    // same canonical ID; F1's installation cannot authorize F2.
    {
        Fixture fixture;
        const std::vector<uint32_t> x{fixture.region_ids[6], fixture.region_ids[7],
                                      fixture.region_ids[8], fixture.region_ids[9],
                                      fixture.region_ids[10], fixture.region_ids[11]};
        static_cast<void>(fixture.admit(x, 80));  // global-only seed
        p29::FRouteEndpoint f1(fixture.authority, {opaque(101), 7}, 0);
        p29::FRouteEndpoint f2(fixture.authority, {opaque(141), 7}, 0);

        const auto first = fixture.admit(x, 81);
        f1.prepare(first, {opaque(231), opaque(181)});
        const auto f1_route = f1.candidate(p29::RepresentationMode::RouteS1);
        const auto f1_global = f1.candidate(p29::RepresentationMode::GlobalS1);
        check(f1_route.definitions.empty() && !f1_global.definitions.empty() &&
                  count_children(f1_global) == f1_global.definitions.size(),
              "F1 fixture did not expose a GLOBAL-only explicit Block");
        const uint32_t global_block = f1_global.definitions.front().block_id;
        const auto close1 = closure_for(*first, f1_global.route_sequence);
        f1.begin_attempt(f1_global, close1);
        const auto ack1 = f1.commit_at_f();
        f1.deliver_ack(ack1);

        const auto second = fixture.admit(x, 82);
        f2.prepare(second, {opaque(231), opaque(182)});
        const auto f2_global = f2.candidate(p29::RepresentationMode::GlobalS1);
        check(!f2.blocks().known(global_block) && !f2_global.definitions.empty() &&
                  f2_global.definitions.front().block_id == global_block &&
                  count_children(f2_global) == f2_global.definitions.size(),
              "F1 GLOBAL installation leaked into F2 or changed canonical identity");
        check(!f2.candidate_legal_for(f1_global),
              "F1's candidate was accepted as an F2 candidate");
        const auto close2 = closure_for(*second, f2_global.route_sequence);
        f2.begin_attempt(f2_global, close2);
        const auto ack2 = f2.commit_at_f();
        f2.deliver_ack(ack2);
        check(f1.blocks().known(global_block) && f2.blocks().known(global_block),
              "the same canonical Block was not materialized independently at both Fs");
    }

    // Mid-transaction rejection restores every visible plane.  Retry reuses the same
    // AdmittedTu and candidate.  A later lost Ack returns the retained receipt without a
    // second F application.
    {
        Fixture fixture;
        const std::vector<uint32_t> x{fixture.region_ids[0], fixture.region_ids[1],
                                      fixture.region_ids[2], fixture.region_ids[3],
                                      fixture.region_ids[4], fixture.region_ids[5]};
        p29::FRouteEndpoint f(fixture.authority, {opaque(101), 7}, 0);
        transfer(f, fixture.admit(x, 90), p29::RepresentationMode::Raw, 190);
        const auto retry_admitted = fixture.admit(x, 91);
        const auto before = f.state_mark();
        const size_t catalogue_before = fixture.authority.block_count();
        const uint64_t snapshot_before = fixture.authority.snapshot_version();
        f.prepare(retry_admitted, {opaque(232), opaque(191)});
        const auto first_candidate = f.candidate(p29::RepresentationMode::RouteS1);
        const auto close = closure_for(*retry_admitted, first_candidate.route_sequence);
        f.begin_attempt(first_candidate, close);
        f.reject_attempt();
        check(f.state_mark() == before,
              "mid-transaction rejection did not restore the two-F endpoint state");
        check(fixture.authority.block_count() == catalogue_before &&
                  fixture.authority.snapshot_version() == snapshot_before,
              "rejected retry changed an already-canonical catalogue");

        f.prepare(retry_admitted, {opaque(232), opaque(191)});
        const auto retry_candidate = f.candidate(p29::RepresentationMode::RouteS1);
        check(retry_candidate == first_candidate,
              "identical retry produced a different route candidate");
        f.begin_attempt(retry_candidate, close);
        const auto retry_ack = f.commit_at_f();
        f.deliver_ack(retry_ack);
        check(f.f_apply_count() == 2, "retry did not apply exactly once after the seed");

        const auto lost = fixture.admit(x, 92);
        transfer(f, lost, p29::RepresentationMode::Raw, 192, true);
        check(f.f_apply_count() == 3,
              "lost-Ack transaction changed the exact F application count");
    }

    // COPY source eviction leaves semantic route history intact but forces the next unknown
    // Block definition to explicit children.  A stale pre-eviction candidate is not reusable.
    {
        Fixture fixture;
        const std::vector<uint32_t> x{fixture.region_ids[0], fixture.region_ids[1],
                                      fixture.region_ids[2], fixture.region_ids[3],
                                      fixture.region_ids[4], fixture.region_ids[5]};
        p29::FRouteEndpoint f(fixture.authority, {opaque(101), 7}, 0);
        transfer(f, fixture.admit(x, 100), p29::RepresentationMode::Raw, 200);
        const auto second = fixture.admit(x, 101);
        f.prepare(second, {opaque(233), opaque(201)});
        const auto before_eviction = f.candidate(p29::RepresentationMode::RouteS1);
        check(count_copy(before_eviction) > 0,
              "pre-eviction route candidate had no COPY source");
        const auto close = closure_for(*second, before_eviction.route_sequence);
        f.begin_attempt(p29::RepresentationMode::Raw, close);
        f.reject_attempt();
        check(f.evict_source(0), "committed COPY source was not evicted");
        f.prepare(second, {opaque(233), opaque(201)});
        const auto after_eviction = f.candidate(p29::RepresentationMode::RouteS1);
        check(count_copy(after_eviction) == 0 && count_children(after_eviction) > 0,
              "source eviction did not force an explicit Block definition");
        f.begin_attempt(after_eviction, close);
        const auto ack = f.commit_at_f();
        f.deliver_ack(ack);
    }

    // A new F cache epoch and a new SourceGeneration are independent exact domains even
    // when the physical F identity and semantic content stay the same.
    {
        Fixture fixture;
        const std::vector<uint32_t> x{fixture.region_ids[0], fixture.region_ids[1],
                                      fixture.region_ids[2], fixture.region_ids[3],
                                      fixture.region_ids[4], fixture.region_ids[5]};
        const p29::FCacheIdentity old_identity{opaque(101), 7};
        p29::FRouteEndpoint old_f(fixture.authority, old_identity, 0);
        transfer(old_f, fixture.admit(x, 110), p29::RepresentationMode::Raw, 210);
        transfer(old_f, fixture.admit(x, 111), p29::RepresentationMode::GlobalS1, 211);
        check(old_f.blocks().known_count() > 0, "old epoch fixture installed no Block");

        p29::FRouteEndpoint new_epoch(fixture.authority, {old_identity.f_id, 8}, 0);
        const auto epoch_tu = fixture.admit(x, 112);
        new_epoch.prepare(epoch_tu, {opaque(234), opaque(212)});
        const auto epoch_global = new_epoch.candidate(p29::RepresentationMode::GlobalS1);
        check(new_epoch.blocks().known_count() == 0 && !epoch_global.definitions.empty(),
              "new FCacheEpoch inherited the old epoch's Block state");
        const auto epoch_close = closure_for(*epoch_tu, epoch_global.route_sequence);
        new_epoch.begin_attempt(epoch_global, epoch_close);
        const auto epoch_ack = new_epoch.commit_at_f();
        new_epoch.deliver_ack(epoch_ack);

        Fixture next_generation(32);
        const std::vector<uint32_t> next_x{next_generation.region_ids[0],
                                           next_generation.region_ids[1],
                                           next_generation.region_ids[2],
                                           next_generation.region_ids[3],
                                           next_generation.region_ids[4],
                                           next_generation.region_ids[5]};
        static_cast<void>(next_generation.admit(next_x, 120));
        p29::FRouteEndpoint generation_f(next_generation.authority, old_identity, 0);
        const auto generation_tu = next_generation.admit(next_x, 121);
        generation_f.prepare(generation_tu, {opaque(234), opaque(221)});
        const auto generation_global =
            generation_f.candidate(p29::RepresentationMode::GlobalS1);
        check(generation_f.blocks().known_count() == 0 &&
                  !generation_global.definitions.empty() &&
                  generation_global.source_generation != fixture.authority.source_generation(),
              "new SourceGeneration inherited an old typed ordinal");
        const auto generation_close =
            closure_for(*generation_tu, generation_global.route_sequence);
        generation_f.begin_attempt(generation_global, generation_close);
        const auto generation_ack = generation_f.commit_at_f();
        generation_f.deliver_ack(generation_ack);
    }

    // Stable TUKey is a routing hint only.  Reordering two lineages and editing then reverting
    // one lineage preserve canonical object meaning while route sequence follows ACK order.
    {
        Fixture fixture;
        const std::vector<uint32_t> x{fixture.region_ids[0], fixture.region_ids[1],
                                      fixture.region_ids[2], fixture.region_ids[3]};
        const std::vector<uint32_t> y{fixture.region_ids[4], fixture.region_ids[5],
                                      fixture.region_ids[6], fixture.region_ids[7]};
        const std::vector<uint32_t> edited{fixture.region_ids[0], fixture.region_ids[8],
                                           fixture.region_ids[2], fixture.region_ids[3]};
        p29::FRouteEndpoint f1(fixture.authority, {opaque(101), 7}, 0);
        p29::FRouteEndpoint f2(fixture.authority, {opaque(141), 7}, 0);
        const p29::Opaque128 key_x = opaque(11), key_y = opaque(21);

        transfer(f1, fixture.admit(x, 130), p29::RepresentationMode::Raw, 11);
        transfer(f2, fixture.admit(y, 131), p29::RepresentationMode::Raw, 21);
        transfer(f2, fixture.admit(y, 132), p29::RepresentationMode::Raw, 21);  // reordered first
        transfer(f1, fixture.admit(x, 133), p29::RepresentationMode::Raw, 11);
        std::vector<std::vector<uint32_t>> catalogue_before_edit;
        for (uint32_t id = 0; id < fixture.authority.block_count(); ++id)
            catalogue_before_edit.push_back(fixture.authority.canonical_block_children(id));
        transfer(f1, fixture.admit(edited, 134), p29::RepresentationMode::Raw, 11);
        transfer(f1, fixture.admit(x, 135), p29::RepresentationMode::Raw, 11);
        bool catalogue_prefix_unchanged =
            fixture.authority.block_count() >= catalogue_before_edit.size();
        for (uint32_t id = 0; id < catalogue_before_edit.size(); ++id)
            catalogue_prefix_unchanged &=
                fixture.authority.canonical_block_children(id) == catalogue_before_edit[id];
        check(catalogue_prefix_unchanged,
              "edit/revert rebound an existing canonical Block ID");
        check(f1.committed_spans().size() == 4 && f2.committed_spans().size() == 2,
              "reorder/revert fixture committed the wrong route sequence");
        check(f1.committed_spans()[0].tu_key == key_x &&
                  f1.committed_spans()[3].tu_key == key_x &&
                  f2.committed_spans()[0].tu_key == key_y,
              "stable TUKey did not survive reorder/edit/revert as a soft hint");
    }

    std::printf("P29 deterministic two-F route/state/eviction/retry Phase B %s\n",
                failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
