#pragma once

#include <stdint.h>

/* Stable public bit assignments for the CacheWire profile registry.  Keep
   these independent of the service/message definitions so client-only code
   can inspect a profile mask without depending on the full comm layer. */
inline constexpr uint32_t CACHE_PROFILE_P29V1 = (UINT32_C(1) << 0);
inline constexpr uint32_t CACHE_PROFILE_ZSTD_TU = (UINT32_C(1) << 1);
inline constexpr uint32_t CACHE_PROFILE_ZSTD_ROUTE = (UINT32_C(1) << 2);
inline constexpr uint32_t CACHE_DECLARED_PROFILE_MASK =
    CACHE_PROFILE_P29V1 | CACHE_PROFILE_ZSTD_TU | CACHE_PROFILE_ZSTD_ROUTE;
inline constexpr uint32_t CACHE_ADVERTISABLE_PROFILE_MASK =
    CACHE_DECLARED_PROFILE_MASK;
