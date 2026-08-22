#!/usr/bin/env bash
set -euo pipefail

# Keep this probe cheap enough to run before every package build.  It proves
# that the explicitly installed distro packages expose the language/library
# surface consumed by this product line, and records their actual versions in
# the package manifest.
PROBE_DIR="$(mktemp -d)"
trap 'rm -rf "$PROBE_DIR"' EXIT

cat >"$PROBE_DIR/requirements.cpp" <<'EOF'
#include <archive.h>
#include <utility>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/version.hpp>
#include <compare>
#include <cstdio>
#include <lzo/lzo1x.h>
#include <optional>
#include <span>
#include <variant>
#include <vector>
#include <xxhash.h>
#include <zstd.h>

#if __cplusplus < 202100L
#error a C++23 language mode is required
#endif

#if BOOST_VERSION < 107400
#error Boost 1.74 or newer is required
#endif

#if XXH_VERSION_NUMBER < 801
#error xxHash 0.8.1 or newer is required
#endif

struct OrderedValue {
    unsigned long long value = 0;
    auto operator<=>(const OrderedValue &) const = default;
};

static boost::asio::awaitable<void> asio_coroutine_probe()
{
    co_return;
}

int main()
{
    const std::vector<unsigned long long> values { 1, 2, 3 };
    const std::span<const unsigned long long> view(values);
    const std::optional<OrderedValue> selected { OrderedValue { view.size() } };
    const std::variant<unsigned long long, OrderedValue> tagged { *selected };
    if (std::get<OrderedValue>(tagged).value != 3) {
        return 1;
    }

    XXH3_state_t *xxh = XXH3_createState();
    ZSTD_CCtx *zstd = ZSTD_createCCtx();
    struct archive *archive = archive_read_new();
    if (!xxh || !zstd || !archive || lzo_init() != LZO_E_OK
            || XXH3_128bits_reset(xxh) == XXH_ERROR
            || ZSTD_isError(ZSTD_CCtx_setParameter(
                zstd, ZSTD_c_compressionLevel, 1))) {
        return 2;
    }

    XXH3_freeState(xxh);
    ZSTD_freeCCtx(zstd);
    archive_read_free(archive);
    std::printf(
        "requirements compiler=%s boost=%s xxhash=%d.%d.%d zstd=%s lzo=%s archive=%s\n",
        __VERSION__, BOOST_LIB_VERSION, XXH_VERSION_MAJOR, XXH_VERSION_MINOR,
        XXH_VERSION_RELEASE, ZSTD_versionString(), lzo_version_string(),
        archive_version_string());
    return 0;
}
EOF

"${CXX:-c++}" \
    -std=c++23 \
    -DBOOST_ASIO_HAS_CO_AWAIT=1 \
    -DBOOST_ASIO_HAS_STD_COROUTINE=1 \
    "$PROBE_DIR/requirements.cpp" \
    -o "$PROBE_DIR/requirements" \
    -lxxhash -lzstd -llzo2 -larchive

"$PROBE_DIR/requirements"
