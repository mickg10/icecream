#pragma once

#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <variant>

namespace icecc::codec {

// -1 means that the current product leaves a libzstd parameter at the
// library default.  It is a value in the frozen tuple, not an invitation for
// a later implementation to guess or pin a different value.
inline constexpr int zstd_parameter_default = -1;

struct ZstdTuple {
    std::string_view id;
    int level;
    int window_log;
    bool ldm;
    int ldm_hash_log;
    int ldm_min_match;
    int ldm_bucket_size_log;
    int ldm_hash_rate_log;
    int hash_log;
    int chain_log;
    int search_log;
    int min_match;
    int target_length;
    int strategy;
    int content_size_flag;
    int checksum_flag;
    int dict_id_flag;
    int threads;

    constexpr bool operator==(const ZstdTuple &) const = default;
};

struct GrzTuple {
    std::string_view id;
    std::uint64_t group_bytes;
    std::uint32_t anchor_bytes;
    std::uint32_t anchor_spacing_bits;
    std::uint64_t index_capacity_or_budget;
    std::uint32_t index_bits;
    std::uint32_t index_bits_cap;
    std::uint64_t history_bytes;
    std::uint64_t group_close_tus;
    std::uint64_t group_close_raw_bytes;
    std::uint64_t group_close_add_bytes;
    bool backend_selection;
    std::string_view literal_backend;
    std::string_view token_backend;
    // Empty when backend_selection is false.
    std::string_view selection_candidates;

    constexpr bool operator==(const GrzTuple &) const = default;
};

struct P29Tuple {
    std::string_view id;
    std::uint32_t s1_max_chain;
    bool mo_factor;
    bool mixed_regions;
    bool byte_array_lines;
    bool direct_ordinals;
    bool compressed_blobs;
    int blob_z;
    unsigned blob_workers;
    unsigned blob_job_mib;
    unsigned blob_overlap_log;
    bool stable_root_tags;
    bool literal_ondemand;
    bool literal_group_skip_zstd10;
    std::string_view residual_kind_policy;
    int root_zstd_level;
    bool transactional;

    constexpr bool operator==(const P29Tuple &) const = default;
};

inline constexpr ZstdTuple zstd_route_product_v0{
    "zstd_route_product_v0",
    3,
    27,
    false,
    zstd_parameter_default,
    zstd_parameter_default,
    zstd_parameter_default,
    zstd_parameter_default,
    zstd_parameter_default,
    zstd_parameter_default,
    zstd_parameter_default,
    zstd_parameter_default,
    zstd_parameter_default,
    zstd_parameter_default,
    zstd_parameter_default,
    zstd_parameter_default,
    zstd_parameter_default,
    zstd_parameter_default,
};

inline constexpr ZstdTuple zstd_route_research_thin{
    "zstd_route_research_thin", 3, 27, true, 20, 64, 3, 7, 17, 16, 1, 5, 0, 2, 0, 0, 0, 1,
};

inline constexpr ZstdTuple zstd_route_research_ldm31{
    "zstd_route_research_ldm31", 3, 31, true, 24, 64, 3, 7, 17, 16, 1, 5, 0, 2, 0, 0, 0, 1,
};

inline constexpr GrzTuple grz_product_v0{
    "grz_product_v0",
    std::uint64_t{64} << 10,
    16,
    0,
    std::uint64_t{16} << 10,
    0,
    0,
    std::uint64_t{128} << 20,
    1,
    std::uint64_t{64} << 10,
    0,
    true,
    "residual_group",
    "raw13(copy:u8,value:u64le,length:u32le)",
    "literals:{zstd3,bsc64m,zstd10}",
};

inline constexpr GrzTuple grz_research_g2{
    "grz_research_g2",
    std::uint64_t{8} << 20,
    256,
    6,
    std::uint64_t{2} << 30,
    21,
    26,
    std::uint64_t{1} << 30,
    1,
    std::uint64_t{512} << 20,
    std::uint64_t{128} << 20,
    false,
    "BE_BSC_E0",
    "BE_Z12",
    "",
};

inline constexpr P29Tuple p29_product_v0{
    "p29_product_v0",
    1024,
    false,
    false,
    false,
    false,
    false,
    0,
    0,
    0,
    0,
    false,
    false,
    false,
    "best_of(zstd3,bsc64m,zstd10)",
    0,
    true,
};

inline constexpr P29Tuple p29_wire_v1{
    "p29_wire_v1",
    1024,
    false,
    true,
    false,
    true,
    false,
    0,
    0,
    0,
    0,
    true,
    false,
    false,
    "best_of(zstd3,bsc64m,zstd10)",
    3,
    true,
};

using CodecTuple = std::variant<ZstdTuple, GrzTuple, P29Tuple>;

[[nodiscard]] constexpr CodecTuple tuple_by_id(std::string_view id) {
    if (id == zstd_route_product_v0.id)
        return zstd_route_product_v0;
    if (id == zstd_route_research_thin.id)
        return zstd_route_research_thin;
    if (id == zstd_route_research_ldm31.id)
        return zstd_route_research_ldm31;
    if (id == grz_product_v0.id)
        return grz_product_v0;
    if (id == grz_research_g2.id)
        return grz_research_g2;
    if (id == p29_product_v0.id)
        return p29_product_v0;
    if (id == p29_wire_v1.id)
        return p29_wire_v1;
    throw std::invalid_argument("unknown codec tuple id");
}

} // namespace icecc::codec
