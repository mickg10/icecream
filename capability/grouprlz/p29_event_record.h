// One exact per-TU event schema for codec, routing, presend, recovery and learning curves.
//
// Fields not measured by a phase stay absent; they are never written as zero.  A later phase
// fills the same row rather than creating a parallel ledger with subtly different identities.
#ifndef P29_EVENT_RECORD_H
#define P29_EVENT_RECORD_H

#include "p29_shared_authority.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace p29 {

enum class CandidateKind : uint8_t {
    Raw = 0,
    RouteS1 = 1,
    GlobalS1 = 2,
    Grz = 3,
    Fi = 4,
    Z3 = 5,
    Count = 6
};

inline const char* candidate_name(CandidateKind kind) {
    static constexpr std::array<const char*, 6> names{
        "RAW", "ROUTE_S1", "GLOBAL_S1", "GRZ", "FI", "Z3"};
    const size_t index = static_cast<size_t>(kind);
    return index < names.size() ? names[index] : "INVALID";
}

struct CandidateMeasurement {
    bool offered = false;
    bool legal = false;
    std::optional<uint64_t> c_to_f_bytes;
};

struct EventState {
    uint64_t catalogue_objects = 0;
    uint64_t route_occurrences = 0;
    uint64_t f_known_objects = 0;
    uint64_t route_commit_sequence = 0;
    uint64_t mirror_sequence = 0;
    uint64_t residency_sequence = 0;
    uint64_t exact_digest = 0;
};

struct CopyUseEvent {
    RouteLaneIdentity lane{};
    Opaque128 source_generation{};
    uint64_t source_residency_sequence = 0;
    uint64_t source_position = 0;
    uint64_t length = 0;
    uint32_t block_id = 0;
};

// SourceGeneration is carried once by the enclosing row.  Object kind is still load-bearing:
// direct ordinal 7 in the Region table and direct ordinal 7 in the Block table are unrelated.
struct MissingObjectEvent {
    ObjectKind kind = ObjectKind::Atom;
    uint32_t ordinal = 0;

    bool operator==(const MissingObjectEvent& other) const {
        return kind == other.kind && ordinal == other.ordinal;
    }
};

struct EventTimestamps {
    std::optional<uint64_t> codec_ready_ns;
    std::optional<uint64_t> handoff_ns;
    std::optional<uint64_t> reconstructed_ns;
    std::optional<uint64_t> compiler_first_ns;
    std::optional<uint64_t> compiler_last_ns;
    std::optional<uint64_t> ack_ns;
};

struct TuEventRecord {
    static constexpr uint32_t kSchemaVersion = 2;

    uint32_t schema_version = kSchemaVersion;
    Opaque128 c_guid{};
    Opaque128 source_generation{};
    Opaque128 producer_request_id{};
    FCacheIdentity f{};
    uint64_t route_lane_id = 0;
    Opaque128 routing_cohort_key{};
    Opaque128 tu_key{};

    uint64_t canonical_admission_sequence = 0;
    uint64_t shared_c_snapshot_version = 0;
    uint64_t logical_ordinal = 0;
    uint64_t physical_ordinal = 0;
    uint64_t raw_bytes = 0;
    uint64_t output_extent = 0;
    Digest128 transaction_digest{};
    Digest128 output_digest{};

    uint32_t nominal_width = 1;
    std::optional<double> n_eff;
    std::optional<double> route_entropy;
    std::array<CandidateMeasurement, static_cast<size_t>(CandidateKind::Count)> candidates{};
    CandidateKind selected = CandidateKind::Raw;
    bool tie = false;
    std::optional<uint64_t> regret_bytes;

    // Exact physical decomposition.  When actual_c_to_f_bytes is present, every C->F part
    // must be present and sum exactly.  Need is F->C and therefore recorded but not scored.
    std::optional<uint64_t> actual_c_to_f_bytes;
    std::optional<uint64_t> root_bytes;
    std::optional<uint64_t> block_definition_bytes;
    std::optional<uint64_t> material_bytes;
    std::optional<uint64_t> control_bytes;
    std::optional<uint64_t> attributed_presend_bytes;
    std::optional<uint64_t> need_bytes;
    std::optional<uint64_t> presend_used_bytes;
    std::optional<uint64_t> presend_unused_bytes;

    std::vector<MissingObjectEvent> missing_objects;
    std::vector<CopyUseEvent> copy_uses;
    EventState before{};
    EventState after{};
    uint64_t evicted_objects = 0;
    uint64_t evicted_source_bytes = 0;

    EventTimestamps timestamps{};
    std::optional<uint64_t> c_core_ns;
    std::optional<uint64_t> f_core_ns;
    std::optional<uint64_t> c_rss_bytes;
    std::optional<uint64_t> f_rss_bytes;

    uint32_t attempts = 1;
    bool retained_ack_recovery = false;
    std::optional<uint64_t> cold_closure_bytes;
    std::optional<uint64_t> mature_same_route_bytes;
    std::optional<int64_t> learning_debt_bytes;
    std::optional<uint64_t> state_investment_bytes;
    std::optional<uint64_t> later_bytes_avoided;

    bool validate(const char** reason = nullptr) const {
        const auto reject = [&](const char* message) {
            if (reason) *reason = message;
            return false;
        };
        if (schema_version != kSchemaVersion) return reject("unknown event schema version");
        if (!attempts) return reject("event has zero transfer attempts");
        if (!nominal_width) return reject("event has zero nominal width");
        if (static_cast<size_t>(selected) >= candidates.size())
            return reject("selected candidate kind is invalid");
        const CandidateMeasurement& chosen = candidates[static_cast<size_t>(selected)];
        for (const CandidateMeasurement& candidate : candidates)
            if (candidate.legal && !candidate.offered)
                return reject("a legal candidate was not offered");
        if (!chosen.offered || !chosen.legal)
            return reject("selected candidate was not offered and legal");
        if (actual_c_to_f_bytes && chosen.c_to_f_bytes &&
            *actual_c_to_f_bytes != *chosen.c_to_f_bytes)
            return reject("selected candidate bytes differ from actual C-to-F bytes");

        const bool has_physical_part =
            root_bytes || block_definition_bytes || material_bytes || control_bytes ||
            attributed_presend_bytes;
        if (has_physical_part && !actual_c_to_f_bytes)
            return reject("physical decomposition exists without actual C-to-F bytes");
        if (actual_c_to_f_bytes) {
            if (!root_bytes || !block_definition_bytes || !material_bytes || !control_bytes ||
                !attributed_presend_bytes)
                return reject("actual C-to-F bytes lack a complete physical decomposition");
            const uint64_t parts[] = {*root_bytes, *block_definition_bytes, *material_bytes,
                                      *control_bytes, *attributed_presend_bytes};
            uint64_t sum = 0;
            for (uint64_t part : parts) {
                if (part > std::numeric_limits<uint64_t>::max() - sum)
                    return reject("physical byte decomposition overflows");
                sum += part;
            }
            if (sum != *actual_c_to_f_bytes)
                return reject("physical byte decomposition does not equal actual C-to-F bytes");
        }
        if (presend_used_bytes && attributed_presend_bytes &&
            *presend_used_bytes > *attributed_presend_bytes)
            return reject("used presend bytes exceed attributed presend bytes");
        if (presend_unused_bytes && attributed_presend_bytes &&
            *presend_unused_bytes > *attributed_presend_bytes)
            return reject("unused presend bytes exceed attributed presend bytes");

        if (after.catalogue_objects < before.catalogue_objects)
            return reject("canonical catalogue shrank");
        if (after.route_occurrences < before.route_occurrences)
            return reject("route occurrence history shrank");
        if (before.route_commit_sequence == std::numeric_limits<uint64_t>::max() ||
            after.route_commit_sequence != before.route_commit_sequence + 1)
            return reject("successful TU did not advance route commit sequence exactly once");
        if (before.mirror_sequence == std::numeric_limits<uint64_t>::max() ||
            after.mirror_sequence != before.mirror_sequence + 1)
            return reject("successful TU did not advance mirror sequence exactly once");
        if (before.residency_sequence == std::numeric_limits<uint64_t>::max() ||
            after.residency_sequence != before.residency_sequence + 1)
            return reject("successful TU did not advance F source residency exactly once");

        // The event is validated on the Ack/commit boundary.  Keep validation allocation-free
        // there: missing lists are normally tiny, and a quadratic duplicate check avoids a
        // hash-table allocation after the state transition has become visible.
        for (size_t left = 0; left < missing_objects.size(); ++left) {
            if (!valid_object_kind(missing_objects[left].kind))
                return reject("missing object has an invalid kind");
            for (size_t right = left + 1; right < missing_objects.size(); ++right)
                if (missing_objects[left] == missing_objects[right])
                    return reject("typed missing object list contains a duplicate");
        }

        if (selected == CandidateKind::Raw && !copy_uses.empty())
            return reject("RAW event contains a COPY use");
        const RouteLaneIdentity event_lane{f, route_lane_id};
        for (size_t index = 0; index < copy_uses.size(); ++index) {
            const CopyUseEvent& copy = copy_uses[index];
            if (copy.lane != event_lane || copy.source_generation != source_generation ||
                copy.source_residency_sequence != before.residency_sequence || !copy.length)
                return reject("COPY evidence is not local to the event's pre-TU F state");
            bool names_missing_block = false;
            for (const MissingObjectEvent& missing : missing_objects)
                if (missing.kind == ObjectKind::Block && missing.ordinal == copy.block_id) {
                    names_missing_block = true;
                    break;
                }
            if (!names_missing_block)
                return reject("COPY evidence does not name a missing Block install");
            for (size_t prior = 0; prior < index; ++prior)
                if (copy_uses[prior].block_id == copy.block_id)
                    return reject("COPY evidence repeats a Block install");
        }

        std::optional<uint64_t> previous;
        const std::optional<uint64_t> ordered_times[] = {
            timestamps.codec_ready_ns, timestamps.handoff_ns,
            timestamps.reconstructed_ns, timestamps.compiler_first_ns,
            timestamps.compiler_last_ns, timestamps.ack_ns};
        for (const auto& timestamp : ordered_times) {
            if (!timestamp) continue;
            if (previous && *timestamp < *previous)
                return reject("event timestamps are not monotone");
            previous = timestamp;
        }

        if (mature_same_route_bytes && actual_c_to_f_bytes && learning_debt_bytes) {
            int64_t expected = 0;
            if (*actual_c_to_f_bytes >= *mature_same_route_bytes) {
                const uint64_t difference =
                    *actual_c_to_f_bytes - *mature_same_route_bytes;
                if (difference > uint64_t(std::numeric_limits<int64_t>::max()))
                    return reject("learning debt exceeds signed range");
                expected = static_cast<int64_t>(difference);
            } else {
                const uint64_t difference =
                    *mature_same_route_bytes - *actual_c_to_f_bytes;
                const uint64_t minimum_magnitude = uint64_t{1} << 63;
                if (difference > minimum_magnitude)
                    return reject("learning debt exceeds signed range");
                expected = difference == minimum_magnitude
                               ? std::numeric_limits<int64_t>::min()
                               : -static_cast<int64_t>(difference);
            }
            if (*learning_debt_bytes != expected)
                return reject("learning debt differs from exact mature-route counterfactual");
        }
        return true;
    }

    bool validate(std::string* reason) const {
        const char* message = nullptr;
        const bool valid = validate(&message);
        if (reason) *reason = message ? message : "";
        return valid;
    }

    static std::string tsv_header() {
        std::ostringstream out;
        out << "schema_version\tc_guid\tsource_generation\tproducer_request_id\tf_id"
               "\tf_cache_epoch\troute_lane_id\trouting_cohort_key\ttu_key"
               "\tcanonical_admission_sequence\tshared_c_snapshot_version"
               "\tlogical_ordinal\tphysical_ordinal\traw_bytes\toutput_extent"
               "\ttransaction_digest\toutput_digest\tnominal_width\tn_eff\troute_entropy";
        for (size_t index = 0; index < static_cast<size_t>(CandidateKind::Count); ++index) {
            const char* name = candidate_name(static_cast<CandidateKind>(index));
            out << '\t' << name << "_offered\t" << name << "_legal\t" << name << "_cf_bytes";
        }
        out << "\tselected\ttie\tregret_bytes\tactual_c_to_f_bytes\troot_bytes"
               "\tblock_definition_bytes\tmaterial_bytes\tcontrol_bytes"
               "\tattributed_presend_bytes\tneed_bytes\tpresend_used_bytes"
               "\tpresend_unused_bytes\tmissing_objects\tcopy_uses"
               "\tbefore_catalogue\tafter_catalogue\tbefore_route_occurrences"
               "\tafter_route_occurrences\tbefore_f_known\tafter_f_known"
               "\tbefore_route_commit\tafter_route_commit\tbefore_mirror\tafter_mirror"
               "\tbefore_residency\tafter_residency\tbefore_state_digest"
               "\tafter_state_digest\tevicted_objects\tevicted_source_bytes"
               "\tcodec_ready_ns\thandoff_ns\treconstructed_ns\tcompiler_first_ns"
               "\tcompiler_last_ns\tack_ns\tc_core_ns\tf_core_ns\tc_rss_bytes"
               "\tf_rss_bytes\tattempts\tretained_ack_recovery\tcold_closure_bytes"
               "\tmature_same_route_bytes\tlearning_debt_bytes\tstate_investment_bytes"
               "\tlater_bytes_avoided";
        return out.str();
    }

    std::string to_tsv() const {
        std::ostringstream out;
        out << std::setprecision(17);
        out << schema_version << '\t' << hex(c_guid) << '\t' << hex(source_generation) << '\t'
            << hex(producer_request_id) << '\t' << hex(f.f_id) << '\t' << f.cache_epoch << '\t'
            << route_lane_id << '\t' << hex(routing_cohort_key) << '\t' << hex(tu_key) << '\t'
            << canonical_admission_sequence << '\t' << shared_c_snapshot_version << '\t'
            << logical_ordinal << '\t' << physical_ordinal << '\t' << raw_bytes << '\t'
            << output_extent << '\t' << hex(transaction_digest) << '\t' << hex(output_digest)
            << '\t' << nominal_width << '\t';
        append_optional(out, n_eff); out << '\t'; append_optional(out, route_entropy);
        for (const CandidateMeasurement& candidate : candidates) {
            out << '\t' << unsigned(candidate.offered) << '\t' << unsigned(candidate.legal)
                << '\t';
            append_optional(out, candidate.c_to_f_bytes);
        }
        out << '\t' << candidate_name(selected) << '\t' << unsigned(tie) << '\t';
        append_optional(out, regret_bytes); out << '\t'; append_optional(out, actual_c_to_f_bytes);
        out << '\t'; append_optional(out, root_bytes);
        out << '\t'; append_optional(out, block_definition_bytes);
        out << '\t'; append_optional(out, material_bytes);
        out << '\t'; append_optional(out, control_bytes);
        out << '\t'; append_optional(out, attributed_presend_bytes);
        out << '\t'; append_optional(out, need_bytes);
        out << '\t'; append_optional(out, presend_used_bytes);
        out << '\t'; append_optional(out, presend_unused_bytes);
        out << '\t' << join_missing(missing_objects) << '\t' << join_copies(copy_uses)
            << '\t' << before.catalogue_objects << '\t' << after.catalogue_objects
            << '\t' << before.route_occurrences << '\t' << after.route_occurrences
            << '\t' << before.f_known_objects << '\t' << after.f_known_objects
            << '\t' << before.route_commit_sequence << '\t' << after.route_commit_sequence
            << '\t' << before.mirror_sequence << '\t' << after.mirror_sequence
            << '\t' << before.residency_sequence << '\t' << after.residency_sequence
            << '\t' << before.exact_digest << '\t' << after.exact_digest
            << '\t' << evicted_objects << '\t' << evicted_source_bytes << '\t';
        append_optional(out, timestamps.codec_ready_ns); out << '\t';
        append_optional(out, timestamps.handoff_ns); out << '\t';
        append_optional(out, timestamps.reconstructed_ns); out << '\t';
        append_optional(out, timestamps.compiler_first_ns); out << '\t';
        append_optional(out, timestamps.compiler_last_ns); out << '\t';
        append_optional(out, timestamps.ack_ns); out << '\t';
        append_optional(out, c_core_ns); out << '\t'; append_optional(out, f_core_ns); out << '\t';
        append_optional(out, c_rss_bytes); out << '\t'; append_optional(out, f_rss_bytes);
        out << '\t' << attempts << '\t' << unsigned(retained_ack_recovery) << '\t';
        append_optional(out, cold_closure_bytes); out << '\t';
        append_optional(out, mature_same_route_bytes); out << '\t';
        append_optional(out, learning_debt_bytes); out << '\t';
        append_optional(out, state_investment_bytes); out << '\t';
        append_optional(out, later_bytes_avoided);
        return out.str();
    }

private:
    template <typename Value>
    static void append_optional(std::ostringstream& out, const std::optional<Value>& value) {
        if (value) out << *value;
    }

    static std::string hex(const Opaque128& value) {
        std::ostringstream out;
        out << std::hex << std::setfill('0');
        for (uint8_t byte : value) out << std::setw(2) << unsigned(byte);
        return out.str();
    }

    static std::string hex(const Digest128& value) {
        std::ostringstream out;
        out << std::hex << std::setfill('0') << std::setw(16) << value.hi
            << std::setw(16) << value.lo;
        return out.str();
    }

    static bool valid_object_kind(ObjectKind kind) {
        return kind == ObjectKind::Atom || kind == ObjectKind::Region ||
               kind == ObjectKind::Block || kind == ObjectKind::Material;
    }

    static char object_kind_token(ObjectKind kind) {
        switch (kind) {
            case ObjectKind::Atom: return 'A';
            case ObjectKind::Region: return 'R';
            case ObjectKind::Block: return 'B';
            case ObjectKind::Material: return 'M';
        }
        return '?';
    }

    static std::string join_missing(const std::vector<MissingObjectEvent>& values) {
        std::ostringstream out;
        for (size_t index = 0; index < values.size(); ++index) {
            if (index) out << ',';
            out << object_kind_token(values[index].kind) << ':' << values[index].ordinal;
        }
        return out.str();
    }

    static std::string join_copies(const std::vector<CopyUseEvent>& copies) {
        std::ostringstream out;
        for (size_t index = 0; index < copies.size(); ++index) {
            if (index) out << ';';
            const CopyUseEvent& copy = copies[index];
            out << hex(copy.lane.f.f_id) << ':' << copy.lane.f.cache_epoch << ':'
                << copy.lane.lane_id << ':' << hex(copy.source_generation) << ':'
                << copy.source_residency_sequence << ':' << copy.source_position << ':'
                << copy.length << ':' << copy.block_id;
        }
        return out.str();
    }
};

}  // namespace p29

#endif
