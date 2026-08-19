// Deterministic A2 fixture for one shared C authority and independent F replicas.
//
//   g++ -std=c++17 -O2 -pthread -Wall -Wextra -Wpedantic -Werror
//       p29_shared_authority_test.cpp -o t && ./t
#include "p29_shared_authority.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <exception>
#include <thread>
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
        value[index] = static_cast<uint8_t>(base + index * 3);
    return value;
}

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

struct ProducerResult {
    p29::SharedCAuthority::PublishedBatch atoms;
    p29::SharedCAuthority::PublishedBatch regions;
    std::pair<uint32_t, bool> block{};
    p29::SharedCAuthority::AdmissionReply admission;
    std::exception_ptr error;
};

}  // namespace

int main() {
    const p29::Opaque128 c_guid = opaque(1), generation = opaque(51);
    p29::SharedCAuthority authority(c_guid, generation, {3, 1024, 12});
    const p29::Opaque128 logical_request = opaque(101);
    const p29::Digest128 output{0x1111222233334444ULL, 0xaaaabbbbccccddddULL};

    // C1 and C2 publish overlapping TU-distinct content and submit the SAME logical request
    // concurrently.  Publication IDs must converge and exactly one canonical admission wins.
    std::array<ProducerResult, 2> same_request;
    StartGate first_gate(2);
    std::array<std::thread, 2> first_threads;
    for (size_t producer = 0; producer < first_threads.size(); ++producer) {
        first_threads[producer] = std::thread([&, producer] {
            try {
                first_gate.arrive_and_wait();
                auto& result = same_request[producer];
                result.atoms = authority.publish_atoms({"line-A", "line-B", "line-A"});
                result.regions = authority.publish_regions(
                    {{result.atoms.ids[0], result.atoms.ids[1]},
                     {result.atoms.ids[0], result.atoms.ids[1]},
                     {result.atoms.ids[1], result.atoms.ids[0]}});
                result.block = authority.publish_block(
                    {result.regions.ids[0], result.regions.ids[2],
                     result.regions.ids[0], result.regions.ids[2]});
                p29::SharedCAuthority::AdmissionInput input;
                input.producer_request_id = logical_request;
                input.source_extent = 4096;
                input.output_digest = output;
                input.regions = {result.regions.ids[0], result.regions.ids[2],
                                 result.regions.ids[0], result.regions.ids[2]};
                result.admission = authority.admit(input);
            } catch (...) {
                same_request[producer].error = std::current_exception();
            }
        });
    }
    first_gate.release_when_ready();
    for (auto& thread : first_threads) thread.join();
    for (const auto& result : same_request) check(!result.error, "concurrent producer threw");
    if (same_request[0].error || same_request[1].error) return 1;

    check(same_request[0].atoms.ids == same_request[1].atoms.ids,
          "same atoms received different canonical IDs");
    check(same_request[0].regions.ids == same_request[1].regions.ids,
          "same Regions received different canonical IDs");
    check(same_request[0].block.first == same_request[1].block.first,
          "same Block received different canonical IDs");
    check(same_request[0].atoms.created + same_request[1].atoms.created == 2,
          "same atoms were not published exactly once");
    check(same_request[0].regions.created + same_request[1].regions.created == 2,
          "same Regions were not published exactly once");
    check(unsigned(same_request[0].block.second) + unsigned(same_request[1].block.second) == 1,
          "same Block was not published exactly once");

    const auto created_count =
        unsigned(same_request[0].admission.result ==
                 p29::SharedCAuthority::AdmissionResult::Created) +
        unsigned(same_request[1].admission.result ==
                 p29::SharedCAuthority::AdmissionResult::Created);
    const auto existing_count =
        unsigned(same_request[0].admission.result ==
                 p29::SharedCAuthority::AdmissionResult::Existing) +
        unsigned(same_request[1].admission.result ==
                 p29::SharedCAuthority::AdmissionResult::Existing);
    check(created_count == 1 && existing_count == 1,
          "logical retry did not produce one Created plus one Existing admission");
    check(same_request[0].admission.admitted == same_request[1].admission.admitted,
          "logical retry did not return the same immutable AdmittedTu");
    check(authority.admission_count() == 1, "logical retry appended two canonical admissions");
    check(authority.global_occurrence_count() == 4,
          "logical retry appended the TU twice to global history");
    check(authority.atom_count() == 2 && authority.region_count() == 2,
          "canonical atom/Region counts differ");
    check(authority.c_guid() == c_guid && authority.source_generation() == generation,
          "shared authority changed exact identity");

    const auto first_admitted = same_request[0].admission.admitted;
    check(first_admitted && first_admitted->canonical_admission_sequence == 0 &&
              first_admitted->source_extent == 4096 &&
              first_admitted->output_digest == output &&
              first_admitted->shared_c_snapshot_version == authority.snapshot_version(),
          "first AdmittedTu closure differs");

    // Same request identity with changed exact input must not mutate any shared state.
    const size_t before_mismatch_admissions = authority.admission_count();
    const size_t before_mismatch_occurrences = authority.global_occurrence_count();
    const uint64_t before_mismatch_snapshot = authority.snapshot_version();
    p29::SharedCAuthority::AdmissionInput mismatched{logical_request, 4097, output,
                                                     first_admitted->regions};
    const auto mismatch_reply = authority.admit(mismatched);
    check(mismatch_reply.result == p29::SharedCAuthority::AdmissionResult::RequestMismatch &&
              mismatch_reply.admitted == first_admitted,
          "changed retry extent was not identified");
    mismatched.source_extent = first_admitted->source_extent;
    mismatched.output_digest.lo ^= 1;
    check(authority.admit(mismatched).result ==
              p29::SharedCAuthority::AdmissionResult::RequestMismatch,
          "changed retry output digest was not identified");
    mismatched.output_digest = first_admitted->output_digest;
    ++mismatched.regions.back();
    check(authority.admit(mismatched).result ==
              p29::SharedCAuthority::AdmissionResult::RequestMismatch,
          "changed retry Region sequence was not identified");
    check(authority.admission_count() == before_mismatch_admissions &&
              authority.global_occurrence_count() == before_mismatch_occurrences &&
              authority.snapshot_version() == before_mismatch_snapshot,
          "changed retry mutated shared-C state");

    // Publication/admission cannot name an object that this authority has not published.
    bool bad_region_refused = false, bad_block_refused = false, bad_admission_refused = false;
    try {
        authority.publish_regions({{999999}});
    } catch (const std::out_of_range&) {
        bad_region_refused = true;
    }
    try {
        authority.publish_block({999999});
    } catch (const std::out_of_range&) {
        bad_block_refused = true;
    }
    try {
        p29::SharedCAuthority::AdmissionInput invalid{opaque(143), 1, {1, 2}, {999999}};
        static_cast<void>(authority.admit(invalid));
    } catch (const std::out_of_range&) {
        bad_admission_refused = true;
    }
    check(bad_region_refused && bad_block_refused && bad_admission_refused,
          "an unpublished canonical child was accepted");
    check(authority.admission_count() == before_mismatch_admissions &&
              authority.global_occurrence_count() == before_mismatch_occurrences &&
              authority.snapshot_version() == before_mismatch_snapshot,
          "refused unpublished child mutated shared-C state");

    // Identical bytes belonging to two independent logical requests are two admissions.  The
    // order may be either producer, but sequences must be consecutive and the records distinct.
    std::array<p29::SharedCAuthority::AdmissionReply, 2> independent;
    std::array<std::exception_ptr, 2> independent_errors;
    StartGate second_gate(2);
    std::array<std::thread, 2> second_threads;
    for (size_t producer = 0; producer < second_threads.size(); ++producer) {
        second_threads[producer] = std::thread([&, producer] {
            try {
                second_gate.arrive_and_wait();
                p29::SharedCAuthority::AdmissionInput input;
                input.producer_request_id = opaque(static_cast<uint8_t>(151 + producer * 17));
                input.source_extent = first_admitted->source_extent;
                input.output_digest = first_admitted->output_digest;
                input.regions = first_admitted->regions;
                independent[producer] = authority.admit(input);
            } catch (...) {
                independent_errors[producer] = std::current_exception();
            }
        });
    }
    second_gate.release_when_ready();
    for (auto& thread : second_threads) thread.join();
    for (const auto& error : independent_errors)
        check(!error, "independent logical producer threw");
    if (independent_errors[0] || independent_errors[1]) return 1;
    check(independent[0].result == p29::SharedCAuthority::AdmissionResult::Created &&
              independent[1].result == p29::SharedCAuthority::AdmissionResult::Created,
          "independent identical logical TUs were deduplicated");
    check(independent[0].admitted != independent[1].admitted,
          "independent logical TUs share one AdmittedTu object");
    std::array<uint64_t, 2> sequences{independent[0].admitted->canonical_admission_sequence,
                                      independent[1].admitted->canonical_admission_sequence};
    std::sort(sequences.begin(), sequences.end());
    check(sequences == std::array<uint64_t, 2>{1, 2},
          "independent logical TUs did not receive consecutive admission sequences");
    check(authority.admission_count() == 3 && authority.global_occurrence_count() == 12,
          "independent logical admissions did not both advance global history");

    // Two concurrent frontends demand one object at F1.  Exactly one owns publication and the
    // other joins the same token.  F2 and a new F1 epoch remain independent replicas.
    const p29::FCacheIdentity f1{opaque(201), 7}, f2{opaque(221), 4}, f1_new_epoch{f1.f_id, 8};
    auto relation1a = authority.relationship(f1);
    auto relation1b = authority.relationship(f1);
    check(relation1a == relation1b, "two frontends received different F1 relationship owners");
    const p29::TypedObjectKey object{generation, p29::ObjectKind::Block,
                                      same_request[0].block.first};
    std::array<p29::PerFRelationship::Reservation, 2> reservations;
    StartGate install_gate(2);
    std::array<std::thread, 2> install_threads;
    for (size_t producer = 0; producer < install_threads.size(); ++producer) {
        install_threads[producer] = std::thread([&, producer] {
            install_gate.arrive_and_wait();
            const auto& relation = producer ? relation1b : relation1a;
            reservations[producer] = relation->reserve_install(object);
        });
    }
    install_gate.release_when_ready();
    for (auto& thread : install_threads) thread.join();
    const unsigned owners =
        unsigned(reservations[0].result == p29::PerFRelationship::ReserveResult::Owner) +
        unsigned(reservations[1].result == p29::PerFRelationship::ReserveResult::Owner);
    const unsigned joiners =
        unsigned(reservations[0].result == p29::PerFRelationship::ReserveResult::Joined) +
        unsigned(reservations[1].result == p29::PerFRelationship::ReserveResult::Joined);
    check(owners == 1 && joiners == 1 && reservations[0].token == reservations[1].token,
          "concurrent F1 demand did not single-flight");
    const uint64_t owner_token = reservations[0].token;
    check(relation1a->commit_install(object, owner_token) ==
              p29::PerFRelationship::CommitResult::Committed,
          "F1 owner could not commit installation");
    check(relation1a->known(object) &&
              relation1a->state_mark() == p29::PerFRelationship::StateMark{1, 1, 0},
          "F1 committed mirror differs");
    check(relation1b->reserve_install(object).result ==
              p29::PerFRelationship::ReserveResult::Known,
          "later F1 demand did not observe committed state");
    check(relation1b->commit_install(object, owner_token) ==
              p29::PerFRelationship::CommitResult::AlreadyKnown,
          "repeat F1 install completion was not idempotent");

    auto relation2 = authority.relationship(f2);
    auto relation1epoch8 = authority.relationship(f1_new_epoch);
    check(!relation2->known(object) && !relation1epoch8->known(object),
          "F1 state leaked into another F or cache epoch");
    p29::TypedObjectKey other_generation = object;
    other_generation.source_generation = opaque(52);
    check(!relation1a->known(other_generation),
          "one SourceGeneration authorized another generation's ordinal");
    const auto f2_reservation = relation2->reserve_install(object);
    check(f2_reservation.result == p29::PerFRelationship::ReserveResult::Owner,
          "independent F2 could not open its own installation");
    check(relation2->abort_install(object, f2_reservation.token) &&
              !relation2->known(object) && relation1a->known(object),
          "F2 abort changed F1 or committed F2 state");

    // Joined demand survives only through the shared relationship.  When the owner aborts,
    // a later frontend obtains a new token and can commit it.
    const p29::TypedObjectKey second_object{generation, p29::ObjectKind::Region,
                                             same_request[0].regions.ids[0]};
    const auto first_owner = relation1a->reserve_install(second_object);
    const auto first_joiner = relation1b->reserve_install(second_object);
    check(first_owner.result == p29::PerFRelationship::ReserveResult::Owner &&
              first_joiner.result == p29::PerFRelationship::ReserveResult::Joined &&
              first_owner.token == first_joiner.token,
          "second F1 object did not single-flight");
    check(!relation1a->abort_install(second_object, first_owner.token + 1) &&
              relation1a->abort_install(second_object, first_owner.token),
          "F1 pending install accepted a non-owner token or refused its owner");
    const auto replacement = relation1b->reserve_install(second_object);
    check(replacement.result == p29::PerFRelationship::ReserveResult::Owner &&
              replacement.token != first_owner.token,
          "post-abort frontend did not receive a fresh ownership token");
    check(relation1b->commit_install(second_object, replacement.token) ==
              p29::PerFRelationship::CommitResult::Committed,
          "replacement F1 owner could not commit");

    // Frontend-local handles disappear here; the shared authority still owns both the
    // canonical admission and the per-F relationship state.
    relation1a.reset();
    relation1b.reset();
    auto after_frontend_exit = authority.relationship(f1);
    p29::SharedCAuthority::AdmissionInput retry{logical_request, 4096, output,
                                                first_admitted->regions};
    const auto after_exit_retry = authority.admit(retry);
    check(after_exit_retry.result == p29::SharedCAuthority::AdmissionResult::Existing &&
              after_exit_retry.admitted == first_admitted,
          "frontend exit lost shared-C admission identity");
    check(after_frontend_exit->known(object) && after_frontend_exit->known(second_object) &&
              after_frontend_exit->state_mark() ==
                  p29::PerFRelationship::StateMark{2, 2, 0},
          "frontend exit lost per-F relationship state");

    std::printf("P29 shared-C multi-producer/single-flight/independent-F A2 %s\n",
                failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
