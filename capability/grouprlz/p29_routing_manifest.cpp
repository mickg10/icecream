// Manifest-to-routing adapter for Issue #16 Phase C.
//
// It uses the same Region interner as the protocol-50 capability harness, then runs the
// deterministic R0-R4 policy engine over typed Region closures.  Its byte model is explicitly
// an independent-Region zstd-3 estimate; the emitted assignment must be passed to the M5
// physical runner before any result is reported as C-to-F wire bytes.
#include "p29_routing_replay.h"
#include "../cap_codec.h"

#include <zstd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct Options {
    const char* manifest = nullptr;
    const char* assignment_out = nullptr;
    const char* curve_out = nullptr;
    size_t max_files = SIZE_MAX;
    uint32_t repetitions = 1;
    uint32_t workers = 1;
    std::vector<uint32_t> slots;
    uint32_t requested_slots = 0;
    uint32_t egress_lanes = 1;
    uint64_t link_bits_per_second = 1000000000ULL;
    uint64_t compiler_bytes_per_second = 500000000ULL;
    uint64_t time_weight_bytes = 0;
    uint64_t time_weight_ns = 1;
    uint64_t investment_weight_num = 1;
    uint64_t investment_weight_den = 1;
    uint64_t investment_history_scale = 4;
    p29::routing::Policy policy = p29::routing::Policy::R0RoundRobin;
};

bool parse_u64(const char* text, uint64_t& value) {
    if (!text || !*text || *text == '-') return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (errno || !end || *end) return false;
    value = parsed;
    return true;
}

bool parse_u32(const char* text, uint32_t& value) {
    uint64_t wide = 0;
    if (!parse_u64(text, wide) || wide > std::numeric_limits<uint32_t>::max())
        return false;
    value = static_cast<uint32_t>(wide);
    return true;
}

bool parse_slots(const char* text, std::vector<uint32_t>& slots) {
    std::stringstream input(text ? text : "");
    std::string token;
    slots.clear();
    while (std::getline(input, token, ',')) {
        uint32_t value = 0;
        if (!parse_u32(token.c_str(), value) || !value) return false;
        slots.push_back(value);
    }
    return !slots.empty();
}

bool parse_policy(const char* text, p29::routing::Policy& policy) {
    struct Entry {
        const char* name;
        p29::routing::Policy policy;
    };
    static constexpr Entry entries[]{
        {"r0-roundrobin", p29::routing::Policy::R0RoundRobin},
        {"r0-fastest", p29::routing::Policy::R0Fastest},
        {"r1-resident", p29::routing::Policy::R1Resident},
        {"r2-home", p29::routing::Policy::R2Home},
        {"r3-rendezvous", p29::routing::Policy::R3Rendezvous},
        {"r4-state", p29::routing::Policy::R4StateAware},
    };
    for (const Entry& entry : entries)
        if (!std::strcmp(text, entry.name)) {
            policy = entry.policy;
            return true;
        }
    return false;
}

p29::Opaque128 opaque(uint64_t seed) {
    p29::Opaque128 result{};
    for (uint8_t& byte : result) {
        seed = capc::mix64(seed + 0x9e3779b97f4a7c15ULL);
        byte = static_cast<uint8_t>(seed);
    }
    return result;
}

uint64_t compressed_size(ZSTD_CCtx* context, const void* data, size_t size,
                         std::vector<uint8_t>& scratch) {
    const size_t bound = ZSTD_compressBound(size);
    if (scratch.size() < bound) scratch.resize(bound);
    const size_t encoded = ZSTD_compressCCtx(context, scratch.data(), scratch.size(), data,
                                             size, 3);
    if (ZSTD_isError(encoded))
        throw std::runtime_error(std::string("zstd: ") + ZSTD_getErrorName(encoded));
    return encoded;
}

uint64_t checked_add(uint64_t left, uint64_t right, const char* message) {
    if (right > std::numeric_limits<uint64_t>::max() - left)
        throw std::overflow_error(message);
    return left + right;
}

uint64_t scale_time(uint64_t bytes, uint64_t bytes_per_second) {
    if (!bytes_per_second) throw std::invalid_argument("zero compiler rate");
    if (bytes > std::numeric_limits<uint64_t>::max() / 1000000000ULL)
        throw std::overflow_error("compiler duration overflow");
    const uint64_t product = bytes * 1000000000ULL;
    return product / bytes_per_second + (product % bytes_per_second != 0);
}

void put_varint(std::vector<uint8_t>& output, uint64_t value) {
    while (value >= 0x80) {
        output.push_back(static_cast<uint8_t>(value) | 0x80);
        value >>= 7;
    }
    output.push_back(static_cast<uint8_t>(value));
}

void usage(const char* program) {
    std::fprintf(
        stderr,
        "usage: %s --manifest FILE --assignment-out FILE --curve-out FILE "
        "--policy r0-roundrobin|r0-fastest|r1-resident|r2-home|r3-rendezvous|r4-state "
        "[--workers N] [--slots N,N,...] [--requested-slots N] [--repetitions N] "
        "[--max-files N] [--egress-lanes N] [--link-bps N] [--compiler-bps N] "
        "[--time-weight NUM DEN] [--investment-weight NUM DEN] "
        "[--investment-history-scale N]\n",
        program);
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        if (!std::strcmp(argv[index], "--manifest") && index + 1 < argc)
            options.manifest = argv[++index];
        else if (!std::strcmp(argv[index], "--assignment-out") && index + 1 < argc)
            options.assignment_out = argv[++index];
        else if (!std::strcmp(argv[index], "--curve-out") && index + 1 < argc)
            options.curve_out = argv[++index];
        else if (!std::strcmp(argv[index], "--policy") && index + 1 < argc) {
            if (!parse_policy(argv[++index], options.policy)) return 2;
        } else if (!std::strcmp(argv[index], "--workers") && index + 1 < argc) {
            if (!parse_u32(argv[++index], options.workers) || !options.workers ||
                options.workers > 64)
                return 2;
        } else if (!std::strcmp(argv[index], "--slots") && index + 1 < argc) {
            if (!parse_slots(argv[++index], options.slots)) return 2;
        } else if (!std::strcmp(argv[index], "--requested-slots") && index + 1 < argc) {
            if (!parse_u32(argv[++index], options.requested_slots) ||
                !options.requested_slots)
                return 2;
        } else if (!std::strcmp(argv[index], "--repetitions") && index + 1 < argc) {
            if (!parse_u32(argv[++index], options.repetitions) || !options.repetitions)
                return 2;
        } else if (!std::strcmp(argv[index], "--max-files") && index + 1 < argc) {
            uint64_t value = 0;
            if (!parse_u64(argv[++index], value) || !value || value > SIZE_MAX) return 2;
            options.max_files = static_cast<size_t>(value);
        } else if (!std::strcmp(argv[index], "--egress-lanes") && index + 1 < argc) {
            if (!parse_u32(argv[++index], options.egress_lanes) || !options.egress_lanes)
                return 2;
        } else if (!std::strcmp(argv[index], "--link-bps") && index + 1 < argc) {
            if (!parse_u64(argv[++index], options.link_bits_per_second) ||
                !options.link_bits_per_second)
                return 2;
        } else if (!std::strcmp(argv[index], "--compiler-bps") && index + 1 < argc) {
            if (!parse_u64(argv[++index], options.compiler_bytes_per_second) ||
                !options.compiler_bytes_per_second)
                return 2;
        } else if (!std::strcmp(argv[index], "--time-weight") && index + 2 < argc) {
            if (!parse_u64(argv[++index], options.time_weight_bytes) ||
                !parse_u64(argv[++index], options.time_weight_ns) ||
                !options.time_weight_ns)
                return 2;
        } else if (!std::strcmp(argv[index], "--investment-weight") &&
                   index + 2 < argc) {
            if (!parse_u64(argv[++index], options.investment_weight_num) ||
                !parse_u64(argv[++index], options.investment_weight_den) ||
                !options.investment_weight_den)
                return 2;
        } else if (!std::strcmp(argv[index], "--investment-history-scale") &&
                   index + 1 < argc) {
            if (!parse_u64(argv[++index], options.investment_history_scale) ||
                !options.investment_history_scale)
                return 2;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (!options.manifest || !options.assignment_out || !options.curve_out) {
        usage(argv[0]);
        return 2;
    }
    if (options.slots.empty()) options.slots.assign(options.workers, 1);
    if (options.slots.size() != options.workers) {
        std::fprintf(stderr, "--slots count must equal --workers\n");
        return 2;
    }
    if (!options.requested_slots) {
        uint64_t total = 0;
        for (uint32_t slots : options.slots) total += slots;
        if (!total || total > std::numeric_limits<uint32_t>::max()) {
            std::fprintf(stderr, "total compiler slots exceed u32\n");
            return 2;
        }
        options.requested_slots = static_cast<uint32_t>(total);
    }

    try {
        capc::Corpus corpus = capc::load_corpus(options.manifest, options.max_files);
        if (corpus.files.empty()) throw std::runtime_error("manifest has no TUs");
        if (corpus.files.size() > SIZE_MAX / options.repetitions)
            throw std::overflow_error("repeated TU count overflow");

        capc::Interner interner;
        uint32_t max_length = 0;
        for (const capc::FileSpan& file : corpus.files)
            max_length = std::max(max_length, file.len);
        std::vector<uint32_t> line_ids(size_t(max_length) + 1);
        std::vector<std::vector<uint32_t>> regions(corpus.files.size());
        uint64_t hits = 0;
        for (size_t index = 0; index < corpus.files.size(); ++index) {
            const capc::FileSpan& file = corpus.files[index];
            size_t count = 0;
            const char* begin = corpus.bytes.data() + file.off;
            interner.process(begin, begin + file.len, line_ids.data(), count, hits, true,
                             &regions[index]);
        }
        if (interner.region_count() > std::numeric_limits<uint32_t>::max())
            throw std::overflow_error("Region space exceeds u32");

        using ZstdContext = std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)>;
        ZstdContext zstd(ZSTD_createCCtx(), &ZSTD_freeCCtx);
        if (!zstd) throw std::runtime_error("cannot allocate zstd context");
        std::vector<uint8_t> scratch, root_raw;
        std::vector<uint64_t> region_wire(size_t(interner.region_count()));
        for (uint32_t region = 0; region < region_wire.size(); ++region) {
            const uint64_t payload = compressed_size(
                zstd.get(), interner.region_data(region), interner.region_raw_len(region),
                scratch);
            // Four-byte frame plus typed ordinal and raw-length varints.  This is an
            // independently framed estimate, not the M5 Fill bundle measurement.
            region_wire[region] = checked_add(
                payload, 4 + capc::varint_size(region) +
                             capc::varint_size(interner.region_raw_len(region)),
                "Region estimate overflow");
        }

        p29::routing::ReplayConfig replay_config;
        replay_config.requested_slots = options.requested_slots;
        replay_config.egress_lanes = options.egress_lanes;
        replay_config.link_bits_per_second = options.link_bits_per_second;
        replay_config.time_weight_bytes = options.time_weight_bytes;
        replay_config.time_weight_ns = options.time_weight_ns;
        replay_config.investment_weight_num = options.investment_weight_num;
        replay_config.investment_weight_den = options.investment_weight_den;
        replay_config.investment_history_scale = options.investment_history_scale;
        for (uint32_t f = 0; f < options.workers; ++f)
            replay_config.fs.push_back({{opaque(0xf0000000ULL + f), 1},
                                        options.slots[f], true});

        const p29::Opaque128 source_generation = opaque(0x5100);
        const p29::Opaque128 cohort = opaque(0xc040);
        std::vector<p29::routing::Tu> trace;
        trace.reserve(corpus.files.size() * options.repetitions);
        std::vector<uint32_t> region_seen(size_t(interner.region_count()), 0);
        uint32_t region_stamp = 0;
        uint64_t logical = 0;
        for (uint32_t repetition = 0; repetition < options.repetitions; ++repetition) {
            for (size_t physical = 0; physical < corpus.files.size(); ++physical) {
                const capc::FileSpan& file = corpus.files[physical];
                p29::routing::Tu item;
                item.logical = logical++;
                item.source_generation = source_generation;
                item.routing_cohort_key = cohort;
                // Repetitions deliberately retain the same TUKey.
                item.tu_key = opaque(0x70000000ULL + physical);
                item.raw_bytes = file.len;
                item.compile_ns = scale_time(file.len, options.compiler_bytes_per_second);

                p29::routing::Representation fi;
                fi.kind = p29::CandidateKind::Fi;
                root_raw.clear();
                for (uint32_t region : regions[physical])
                    put_varint(root_raw, uint64_t(region) << 1);
                fi.root_bytes = checked_add(
                    4, compressed_size(zstd.get(), root_raw.data(), root_raw.size(), scratch),
                    "Root estimate overflow");
                fi.control_bytes = 16;
                if (++region_stamp == 0) {
                    std::fill(region_seen.begin(), region_seen.end(), 0);
                    ++region_stamp;
                }
                for (uint32_t region : regions[physical]) {
                    if (region_seen[region] == region_stamp) continue;
                    region_seen[region] = region_stamp;
                    fi.closure.push_back({region, region_wire[region],
                                          p29::routing::FragmentKind::Material,
                                          p29::ObjectKind::Region});
                }
                item.representations.push_back(std::move(fi));
                trace.push_back(std::move(item));
            }
        }

        const p29::routing::ReplayResult result =
            p29::routing::replay(replay_config, trace, options.policy);
        std::ofstream assignments(options.assignment_out);
        if (!assignments) throw std::runtime_error("cannot open assignment output");
        assignments << "routing-assignment-v1\n";
        for (size_t index = 0; index < result.rows.size(); ++index)
            assignments << index << ' ' << result.rows[index].action.f << '\n';
        if (!assignments) throw std::runtime_error("cannot write assignment output");

        std::ofstream curve(options.curve_out);
        if (!curve) throw std::runtime_error("cannot open estimator curve output");
        curve << "logical\tworker\traw\testimated_c_to_f\tmissing_objects"
                 "\ttransfer_start_ns\ttransfer_finish_ns\tcompile_start_ns"
                 "\tcompile_finish_ns\n";
        for (const p29::routing::DecisionRow& row : result.rows)
            curve << row.logical << '\t' << row.action.f << '\t'
                  << trace[size_t(row.logical)].raw_bytes << '\t' << row.cost.total_bytes
                  << '\t' << row.cost.missing_objects.size() << '\t'
                  << row.transfer_start_ns << '\t' << row.transfer_finish_ns << '\t'
                  << row.compile_start_ns << '\t' << row.compile_finish_ns << '\n';
        if (!curve) throw std::runtime_error("cannot write estimator curve output");

        std::printf(
            "ROUTING_ESTIMATE schema=independent-region-zstd3-v1 policy=%s tus=%zu "
            "workers=%u requested_slots=%u estimated_c_to_f=%llu makespan_ns=%llu "
            "N_eff=%.9f H_route=%.9f assignment=%s curve=%s\n",
            p29::routing::policy_name(options.policy), trace.size(), options.workers,
            options.requested_slots,
            static_cast<unsigned long long>(result.c_to_f_bytes),
            static_cast<unsigned long long>(result.makespan_ns), result.n_eff,
            result.route_entropy, options.assignment_out, options.curve_out);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "routing manifest adapter: %s\n", error.what());
        return 2;
    }
}
