#include "p29_event_record.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "event-record check failed: %s\n", message);
        std::exit(1);
    }
}

p29::Opaque128 opaque(uint64_t seed) {
    p29::Opaque128 value{};
    for (size_t index = 0; index < value.size(); ++index) {
        seed ^= seed >> 12;
        seed ^= seed << 25;
        seed ^= seed >> 27;
        value[index] = static_cast<uint8_t>(seed * 0x2545f4914f6cdd1dULL);
    }
    return value;
}

p29::Digest128 digest(uint64_t seed) {
    return {seed * 0x9e3779b97f4a7c15ULL,
            (seed + 17) * 0xbf58476d1ce4e5b9ULL};
}

size_t candidate_index(p29::CandidateKind kind) {
    return static_cast<size_t>(kind);
}

p29::TuEventRecord full_record() {
    p29::TuEventRecord row;
    row.c_guid = opaque(1);
    row.source_generation = opaque(2);
    row.producer_request_id = opaque(3);
    row.f = {opaque(4), 7};
    row.route_lane_id = 11;
    row.routing_cohort_key = opaque(5);
    row.tu_key = opaque(6);
    row.canonical_admission_sequence = 101;
    row.shared_c_snapshot_version = 203;
    row.logical_ordinal = 17;
    row.physical_ordinal = 9;
    row.raw_bytes = 4096;
    row.output_extent = 4096;
    row.transaction_digest = digest(7);
    row.output_digest = digest(8);
    row.nominal_width = 4;
    row.n_eff = 2.5;
    row.route_entropy = 1.75;

    row.candidates[candidate_index(p29::CandidateKind::Raw)] = {true, true, 120};
    row.candidates[candidate_index(p29::CandidateKind::RouteS1)] = {true, true, 100};
    row.candidates[candidate_index(p29::CandidateKind::GlobalS1)] = {true, true, 105};
    row.selected = p29::CandidateKind::RouteS1;
    row.regret_bytes = 0;

    row.actual_c_to_f_bytes = 100;
    row.root_bytes = 10;
    row.block_definition_bytes = 20;
    row.material_bytes = 30;
    row.control_bytes = 35;
    row.attributed_presend_bytes = 5;
    row.need_bytes = 9;
    row.presend_used_bytes = 3;
    row.presend_unused_bytes = 2;
    row.missing_objects = {{p29::ObjectKind::Region, 12},
                           {p29::ObjectKind::Block, 44},
                           {p29::ObjectKind::Material, 12}};

    row.before = {100, 200, 5, 9, 11, 13, 0x1234};
    row.after = {102, 207, 6, 10, 12, 14, 0x5678};
    row.copy_uses.push_back({{row.f, row.route_lane_id}, row.source_generation,
                             row.before.residency_sequence, 91, 6, 44});
    row.evicted_objects = 1;
    row.evicted_source_bytes = 64;

    row.timestamps = {10, 20, 30, 40, 50, 60};
    row.c_core_ns = 1000;
    row.f_core_ns = 700;
    row.c_rss_bytes = 1U << 20;
    row.f_rss_bytes = 2U << 20;
    row.attempts = 2;
    row.retained_ack_recovery = true;
    row.cold_closure_bytes = 140;
    row.mature_same_route_bytes = 80;
    row.learning_debt_bytes = 20;
    row.state_investment_bytes = 25;
    row.later_bytes_avoided = 41;
    return row;
}

void expect_rejected(const p29::TuEventRecord& source, const char* name,
                     const std::function<void(p29::TuEventRecord&)>& mutate) {
    p29::TuEventRecord changed = source;
    mutate(changed);
    std::string reason;
    if (changed.validate(&reason)) {
        std::fprintf(stderr, "event-record mutation was accepted: %s\n", name);
        std::exit(1);
    }
    check(!reason.empty(), "a rejected record did not explain the failed invariant");
}

}  // namespace

int main() {
    const p29::TuEventRecord full = full_record();
    std::string reason;
    check(full.validate(&reason), reason.c_str());

    const std::string header = p29::TuEventRecord::tsv_header();
    const std::string row = full.to_tsv();
    check(std::count(header.begin(), header.end(), '\t') ==
              std::count(row.begin(), row.end(), '\t'),
          "TSV header and full row have different column counts");
    check(header == p29::TuEventRecord::tsv_header() && row == full.to_tsv(),
          "TSV serialization is not deterministic");
    check(row.find("R:12,B:44,M:12") != std::string::npos,
          "typed missing objects were not serialized distinctly");

    p29::TuEventRecord partial;
    partial.c_guid = opaque(20);
    partial.source_generation = opaque(21);
    partial.producer_request_id = opaque(22);
    partial.f = {opaque(23), 2};
    partial.route_lane_id = 3;
    partial.candidates[candidate_index(p29::CandidateKind::Raw)] = {true, true, std::nullopt};
    partial.selected = p29::CandidateKind::Raw;
    partial.before.route_commit_sequence = 4;
    partial.after.route_commit_sequence = 5;
    partial.before.mirror_sequence = 6;
    partial.after.mirror_sequence = 7;
    partial.before.residency_sequence = 8;
    partial.after.residency_sequence = 9;
    check(partial.validate(&reason), reason.c_str());
    const std::string partial_row = partial.to_tsv();
    check(std::count(header.begin(), header.end(), '\t') ==
              std::count(partial_row.begin(), partial_row.end(), '\t'),
          "TSV header and partial row have different column counts");
    check(partial_row.find("\t\t") != std::string::npos,
          "an absent measurement was serialized as a value");

    expect_rejected(full, "legal but not offered", [](auto& value) {
        value.candidates[candidate_index(p29::CandidateKind::Grz)].legal = true;
    });
    expect_rejected(full, "selected not offered", [](auto& value) {
        value.candidates[candidate_index(p29::CandidateKind::RouteS1)].offered = false;
        value.candidates[candidate_index(p29::CandidateKind::RouteS1)].legal = false;
    });
    expect_rejected(full, "selected byte mismatch", [](auto& value) {
        value.candidates[candidate_index(p29::CandidateKind::RouteS1)].c_to_f_bytes = 99;
    });
    expect_rejected(full, "physical sum", [](auto& value) { value.root_bytes = 11; });
    expect_rejected(partial, "orphan physical part", [](auto& value) {
        value.root_bytes = 1;
    });
    expect_rejected(full, "route sequence", [](auto& value) {
        value.after.route_commit_sequence = value.before.route_commit_sequence;
    });
    expect_rejected(full, "mirror sequence", [](auto& value) {
        value.after.mirror_sequence = value.before.mirror_sequence;
    });
    expect_rejected(full, "source residency sequence", [](auto& value) {
        value.after.residency_sequence = value.before.residency_sequence;
    });
    expect_rejected(full, "duplicate typed missing object", [](auto& value) {
        value.missing_objects.push_back(value.missing_objects.front());
    });
    expect_rejected(full, "invalid missing object kind", [](auto& value) {
        value.missing_objects.front().kind = static_cast<p29::ObjectKind>(0);
    });
    expect_rejected(full, "RAW with COPY", [](auto& value) {
        value.selected = p29::CandidateKind::Raw;
    });
    expect_rejected(full, "cross-route COPY", [](auto& value) {
        value.copy_uses.front().lane.lane_id++;
    });
    expect_rejected(full, "stale COPY residency", [](auto& value) {
        value.copy_uses.front().source_residency_sequence++;
    });
    expect_rejected(full, "zero COPY length", [](auto& value) {
        value.copy_uses.front().length = 0;
    });
    expect_rejected(full, "COPY without missing Block", [](auto& value) {
        value.copy_uses.front().block_id++;
    });
    expect_rejected(full, "duplicate COPY Block", [](auto& value) {
        value.copy_uses.push_back(value.copy_uses.front());
    });
    expect_rejected(full, "timestamp inversion", [](auto& value) {
        value.timestamps.reconstructed_ns = 19;
    });
    expect_rejected(full, "learning debt", [](auto& value) {
        value.learning_debt_bytes = 19;
    });

    p29::TuEventRecord negative_debt = full;
    negative_debt.actual_c_to_f_bytes = 50;
    negative_debt.candidates[candidate_index(p29::CandidateKind::RouteS1)].c_to_f_bytes = 50;
    negative_debt.root_bytes = 5;
    negative_debt.block_definition_bytes = 10;
    negative_debt.material_bytes = 15;
    negative_debt.control_bytes = 15;
    negative_debt.attributed_presend_bytes = 5;
    negative_debt.mature_same_route_bytes = 80;
    negative_debt.learning_debt_bytes = -30;
    check(negative_debt.validate(&reason), reason.c_str());

    std::printf("P29 unified per-TU event schema/serialization PASS\n");
    return 0;
}
