// p29ccpubench: single-threaded microbenchmark of the C-sidecar per-TX
// source-transfer path (P29V1 profile), built out of the exact production
// entry points and driven on SYNTHETIC transfer units only.
//
// Per TX, the production sidecar (SidecarRuntime::transfer_source_on_owner,
// cache/p50_cache_service.cpp) performs:
//
//   read_source_fd          pread the whole preprocessed TU file into a
//                           fresh std::vector<uint8_t> (a full copy)
//   digest128               XXH3-128 over the whole raw input
//   prepare_source          plan_p29v1 (lock-free canonical split walk +
//                           published-region probes) -> intern under the
//                           C-wide mutex (reserve/intern_planned/commit)
//                           -> verify_p29v1_materialization (full memcmp)
//   install_source          owner-coroutine install of the prepared source
//   prepare_for_route       P29V1: begin_v1 -> P29Serializer::begin_tu
//                           (OnlineS1 matcher prepare + build_body: the
//                           BEGIN/BODY encode)
//   fill (eager, pipelined) fill_p29v1_before_need -> build_fill
//   commit                  authority commit (route accept)
//   release                 release of the prepared handle
//
// The default mode times exactly that sequence on one thread against a
// P50PreparationAuthority built with production caps (P29V1 profile,
// sidecar interner layout, product compression level 3), over a corpus of
// synthetic TUs shaped like preprocessed C++ translation units:
//   - ~30-40 MB each,
//   - assembled from a shared pool of "header region" blobs (90%+ of bytes
//     repeat across TUs, in varying orders/subsets per TU), each region
//     starting with a GNU line-marker-like "# " line the way the canonical
//     split expects,
//   - plus small per-TU unique regions with unique content.
//
// --record/--compare run the SAME corpus through the real loopback client
// and server endpoints (a fresh authority per leg) and hash every C->F wire
// frame (BEGIN/BODY/FILL/...), plus the authority state (route history
// digest/bytes, interner reserved/committed), into a receipt.  The
// differential identity test compares receipts across builds: byte-identical
// wire bytes and authority state are required.  The harness never reads,
// embeds or reproduces any real repository source; the corpus is fully
// synthetic and deterministically seeded.
//
// No real hostnames appear anywhere in this harness.

#include "cache/p50_endpoint.h"
#include "cache/p50_source_view.h"
#include "cache/p50_zstd.h"
#include "cache/protocol50.h"
#include "cache/p50_slice0.h"
#include "services/digest128.h"

#include <boost/asio.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>

namespace {

using icecc::Digest128;
using namespace icecc::p50;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

// SidecarRuntime's production route owner pins the current P50 source path
// to level 3 (cache/sim/p50sim.cpp keeps the same constant).
constexpr int kCurrentProductCompressionLevel = 3;

// ---------------------------------------------------------------------------
// Per-stage thread-CPU + wall clock.
// ---------------------------------------------------------------------------

struct StageClock {
    StageClock() { tick(); }
    void tick() {
        struct timespec cpu {};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);
        cpu_ns = uint64_t(cpu.tv_sec) * 1000000000ULL + uint64_t(cpu.tv_nsec);
        struct timespec wall {};
        clock_gettime(CLOCK_MONOTONIC, &wall);
        wall_ns = uint64_t(wall.tv_sec) * 1000000000ULL + uint64_t(wall.tv_nsec);
    }
    uint64_t cpu_ns = 0;
    uint64_t wall_ns = 0;
};

struct StageSplit {
    std::vector<std::pair<const char*, uint64_t>> cpu;
    std::vector<std::pair<const char*, uint64_t>> wall;
    size_t stage_index(const char* name) {
        for (size_t i = 0; i < cpu.size(); ++i)
            if (std::strcmp(cpu[i].first, name) == 0)
                return i;
        cpu.emplace_back(name, 0);
        wall.emplace_back(name, 0);
        return cpu.size() - 1;
    }
    void add(const char* name, const StageClock& before, const StageClock& after) {
        const size_t index = stage_index(name);
        cpu[index].second += after.cpu_ns - before.cpu_ns;
        wall[index].second += after.wall_ns - before.wall_ns;
    }
    void add_ns(const char* name, uint64_t cpu_delta) {
        const size_t index = stage_index(name);
        cpu[index].second += cpu_delta;
    }
};

// ---------------------------------------------------------------------------
// read_source_fd: the sidecar's source read.  The production function is
// internal linkage in cache/p50_cache_service.cpp and delegates to
// P50SourceView::map_fd (cache/p50_source_view.h); the bench calls the same
// product factory so the benchmark times exactly the product read.
// ---------------------------------------------------------------------------

std::optional<std::shared_ptr<const P50SourceView>> read_source_fd(
    int fd, uint64_t limit) noexcept {
    return P50SourceView::map_fd(fd, limit);
}

// ---------------------------------------------------------------------------
// Synthetic TU corpus: shared "header region" blobs (each begins with a
// linemarker-like "# " line so the P29V1 canonical split cuts at their
// boundary), a per-TU shuffled subset/order of them, plus per-TU unique
// regions with unique content.  90%+ of every TU's bytes are shared.
// ---------------------------------------------------------------------------

struct Rng {
    uint64_t state;
    explicit Rng(uint64_t seed) : state(seed ? seed : 0x9e3779b97f4a7c15ULL) {}
    uint64_t next() {
        state += 0x9e3779b97f4a7c15ULL;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
    uint64_t below(uint64_t bound) { return bound ? next() % bound : 0; }
    uint32_t below32(uint64_t bound) { return static_cast<uint32_t>(below(bound)); }
};

std::string synthetic_cxx_body(Rng& rng, uint32_t lines) {
    // Deterministic pseudo-C++ lines.  Nothing here copies real source: it
    // only needs to be line-shaped and textually repetitive.  Every random
    // value is drawn into a named local BEFORE the snprintf call: the draws
    // feeding one printf must not sit in its argument list, because the
    // evaluation order of function arguments is unspecified and differs
    // across compilations, which would make the corpus bytes depend on the
    // binary rather than only on the seed.  (The wire-identity receipts are
    // compared across separately compiled binaries, so the corpus has to be
    // compilation-stable, not merely process-stable.)
    static constexpr const char* kinds[] = {
        "namespace synthetic_%u { namespace inner_%u { struct Shape_%u { "
        "unsigned width_%u = %u; unsigned height_%u = %u; }; } }\n",
        "template <class T_%u> struct Wrapper_%u { T_%u value_%u{}; "
        "unsigned refcount_%u = %u; bool pinned_%u = %s; };\n",
        "inline unsigned long long fold_%u(unsigned long long left, "
        "unsigned long long right) { return (left ^ 0x%ux) + right * %uULL; }\n",
        "struct Payload_%u_%u { unsigned char bytes_%u[%u]; unsigned tag_%u = "
        "0x%xU; }; static_assert(sizeof(Payload_%u) == %u);\n",
        "static const unsigned table_%u[%u] = { %u, %u, %u, %u, %u, %u, "
        "%u, %u };\n",
        "class Visitor_%u { public: virtual void visit_%u(unsigned stage, "
        "const char* region) = 0; virtual ~Visitor_%u() = default; };\n",
    };
    // Argument counts per kind, in placeholder order.
    static constexpr uint32_t kind_args[] = {7, 8, 3, 8, 10, 3};
    std::string out;
    out.reserve(static_cast<size_t>(lines) * 110);
    char buffer[640];
    for (uint32_t line = 0; line < lines; ++line) {
        const uint32_t kind = static_cast<uint32_t>(rng.below(std::size(kinds)));
        uint32_t args[10] = {};
        bool pinned = false;
        for (uint32_t draw = 0; draw < kind_args[kind]; ++draw) {
            if (kind == 1 && draw == 7)
                pinned = rng.below(2) != 0;
            else if (kind == 4)
                args[draw] = rng.below32(draw == 0 ? 4096 : 9999);
            else if (kind == 3 && draw >= 4)
                args[draw] = rng.below32(9999);
            else
                args[draw] = rng.below32(4096);
        }
        int written = 0;
        switch (kind) {
        case 0:
            written = std::snprintf(buffer, sizeof(buffer), kinds[0], args[0],
                                    args[1], args[2], args[3], args[4],
                                    args[5], args[6]);
            break;
        case 1:
            written = std::snprintf(buffer, sizeof(buffer), kinds[1], args[0],
                                    args[1], args[2], args[3], args[4],
                                    args[5], args[6], pinned ? "true" : "false");
            break;
        case 2:
            written = std::snprintf(buffer, sizeof(buffer), kinds[2], args[0],
                                    args[1], args[2]);
            break;
        case 3:
            written = std::snprintf(buffer, sizeof(buffer), kinds[3], args[0],
                                    args[1], args[2], args[3], args[4],
                                    args[5], args[6], args[7]);
            break;
        case 4:
            written = std::snprintf(buffer, sizeof(buffer), kinds[4], args[0],
                                    args[1], args[2], args[3], args[4],
                                    args[5], args[6], args[7], args[8],
                                    args[9]);
            break;
        default:
            written = std::snprintf(buffer, sizeof(buffer), kinds[5], args[0],
                                    args[1], args[2]);
            break;
        }
        if (written > 0)
            out.append(buffer, static_cast<size_t>(written));
    }
    return out;
}

struct HeaderPool {
    std::vector<std::vector<uint8_t>> blobs;
    std::vector<std::string> paths;
    size_t total_bytes = 0;

    explicit HeaderPool(size_t pool_target_bytes, uint64_t seed) {
        Rng rng(seed);
        paths.reserve(64);
        for (uint32_t p = 0; p < 64; ++p)
            paths.push_back("/synthetic/include/pool_" + std::to_string(p) +
                            "/header_" + std::to_string(p) + ".h");
        while (total_bytes < pool_target_bytes) {
            std::string text;
            const std::string& path = paths[rng.below(paths.size())];
            const uint32_t marker_line = rng.below32(100000);
            const uint32_t marker_flag = 1u + rng.below32(3);
            char marker[256];
            const int written = std::snprintf(marker, sizeof(marker),
                                              "# %u \"%s\" %u\n",
                                              marker_line, path.c_str(),
                                              marker_flag);
            text.append(marker, static_cast<size_t>(written > 0 ? written : 0));
            text += synthetic_cxx_body(rng, 60 + rng.below(240));
            total_bytes += text.size();
            blobs.push_back({text.begin(), text.end()});
        }
    }
};

std::vector<uint8_t> assemble_tu(const HeaderPool& pool, Rng& rng,
                                 uint32_t tu_index, size_t tu_bytes,
                                 double shared_fraction) {
    std::vector<uint8_t> out;
    out.reserve(tu_bytes + (tu_bytes >> 4));
    // Deterministic per-TU shuffle of the shared pool.
    std::vector<uint32_t> order(pool.blobs.size());
    for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    for (uint32_t round = 0; round < 3; ++round)
        for (uint32_t i = 0; i + 1 < order.size(); ++i)
            std::swap(order[i], order[i + 1 + rng.below(order.size() - 1 - i)]);
    const size_t shared_budget =
        static_cast<size_t>(double(tu_bytes) * shared_fraction);
    size_t used = 0;
    // Take a shuffled prefix of the pool: every TU includes a different
    // ORDER and a different (large) SUBSET of the shared header pool, the
    // way real TUs include a different subset/order of the tree's headers.
    // The budget cut keeps ~2% of the pool out of each TU at random, so a
    // warm route still has a small per-TX missing set, matching the farm's
    // ~KB C->F wire delta per transfer.
    for (uint32_t i = 0; i < order.size() && used < shared_budget; ++i) {
        const std::vector<uint8_t>& blob = pool.blobs[order[i]];
        if (used + blob.size() > shared_budget) {
            // stop once the budget is full; remaining blobs stay unseen
            continue;
        }
        out.insert(out.end(), blob.begin(), blob.end());
        used += blob.size();
    }
    // Per-TU unique regions, each its own canonical region (own linemarker).
    while (out.size() < tu_bytes) {
        const std::string path =
            "/synthetic/tu/tu_" + std::to_string(tu_index) + "_u" +
            std::to_string(rng.below(64)) + ".h";
        std::string text;
        char marker[256];
        const int written = std::snprintf(marker, sizeof(marker),
                                          "# %u \"%s\" 1\n",
                                          100000u + rng.below32(100000),
                                          path.c_str());
        text.append(marker, static_cast<size_t>(written > 0 ? written : 0));
        text += synthetic_cxx_body(rng, 40 + rng.below(160));
        if (out.size() + text.size() > tu_bytes) {
            const size_t take = std::min(text.size(), tu_bytes - out.size());
            out.insert(out.end(), text.begin(), text.begin() + take);
        } else {
            out.insert(out.end(), text.begin(), text.end());
        }
    }
    return out;
}

struct TuFile {
    std::string path;
    int fd = -1;
    TuFile() = default;
    ~TuFile() {
        if (fd >= 0) ::close(fd);
        if (!path.empty()) ::unlink(path.c_str());
    }
    TuFile(const TuFile&) = delete;
    TuFile& operator=(const TuFile&) = delete;
};

std::string unique_path(const char* base) {
    std::string candidate(base);
    candidate += ".XXXXXX";
    std::vector<char> work(candidate.begin(), candidate.end());
    work.push_back('\0');
    const int fd = ::mkstemp(work.data());
    if (fd < 0)
        throw std::runtime_error("cannot create scratch TU file");
    ::close(fd);
    return std::string(work.data());
}

struct DriveConfig {
    size_t tus = 50;
    size_t tu_mb = 36;
    double shared_fraction = 0.995;
    // Pool oversize over the per-TU shared budget: the fraction of the
    // shuffled pool each TU leaves out (and a later TU's route then has as
    // its per-TX missing set).  1.01 keeps the warm-route FILL at the
    // ~10-20 KB wire size the farm legs record per transfer.
    double pool_oversize = 1.01;
    uint64_t seed = 20261009;
    size_t repeats = 1;
    bool json = false;
    std::string record;
    std::string compare;
    std::string scratch = "/tmp";
};

struct PreparedCorpus {
    std::vector<std::unique_ptr<TuFile>> files;
    std::vector<size_t> sizes;
};

PreparedCorpus stage_corpus(const DriveConfig& config) {
    PreparedCorpus corpus;
    const bool corpus_debug = std::getenv("P29CCPUBENCH_CORPUS_DIGEST") != nullptr;
    // The pool must hold at least a full shared budget of header content so
    // each TU's shuffled prefix can fill its 98% shared budget from a
    // ~1.02x oversized pool (the leftover ~2% is what a warm route is
    // missing each TX).
    const size_t pool_target_bytes =
        size_t(double(config.tu_mb << 20) / std::max(0.5, config.shared_fraction) *
               config.pool_oversize);
    HeaderPool pool(pool_target_bytes, config.seed);
    corpus.files.reserve(config.tus);
    for (size_t t = 0; t < config.tus; ++t) {
        Rng rng(config.seed * 1000003 + t);
        std::vector<uint8_t> bytes =
            assemble_tu(pool, rng, static_cast<uint32_t>(t),
                        config.tu_mb << 20, config.shared_fraction);
        std::unique_ptr<TuFile> file = std::make_unique<TuFile>();
        file->path = unique_path((config.scratch + "/p29ccpubench-").c_str());
        const int fd = ::open(file->path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0)
            throw std::runtime_error("cannot open scratch TU for writing");
        size_t offset = 0;
        while (offset < bytes.size()) {
            const ssize_t written =
                ::write(fd, bytes.data() + offset, bytes.size() - offset);
            if (written <= 0) {
                ::close(fd);
                throw std::runtime_error("short write staging synthetic TU");
            }
            offset += static_cast<size_t>(written);
        }
        ::close(fd);
        file->fd = ::open(file->path.c_str(), O_RDONLY);
        if (file->fd < 0)
            throw std::runtime_error("cannot reopen staged synthetic TU");
        // page-cache warm so every repeat measures the same read cost
        ::posix_fadvise(file->fd, 0, 0, POSIX_FADV_WILLNEED);
        (void)read_source_fd(file->fd, uint64_t{64} << 20);  // fault the pages in
        corpus.files.push_back(std::move(file));
        corpus.sizes.push_back(bytes.size());
        if (corpus_debug) {
            const Digest128 staged = icecc::digest128(bytes);
            static const char* digits = "0123456789abcdef";
            std::string hex;
            for (uint8_t b : staged.bytes) {
                hex += digits[b >> 4];
                hex += digits[b & 15];
            }
            std::fprintf(stdout, "corpus %zu digest=%s size=%zu\n", t,
                          hex.c_str(), bytes.size());
        }
    }
    return corpus;
}

// ---------------------------------------------------------------------------
// Bench mode: the direct per-TX sequence against a fresh authority.
// ---------------------------------------------------------------------------

struct AuthorityIdentity {
    uint64_t route_history_bytes = 0;
    Digest128 route_history_digest{};
    uint64_t interner_reserved = 0;
    uint64_t interner_committed = 0;
    size_t live_entries = 0;
    uint64_t retained_encoded = 0;
};

AuthorityIdentity authority_identity(const P50PreparationAuthority& authority) {
    AuthorityIdentity identity;
    identity.route_history_bytes = authority.route_history_bytes();
    identity.route_history_digest = authority.route_history_digest();
    identity.interner_reserved = authority.p29v1_interner_reserved_bytes();
    identity.interner_committed = authority.p29v1_interner_committed_bytes();
    identity.live_entries = authority.live_entry_count();
    identity.retained_encoded = authority.retained_encoded_bytes();
    return identity;
}

std::string hex_digest(const Digest128& digest) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(2 * digest.bytes.size());
    for (uint8_t b : digest.bytes) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

void drive_leg(P50PreparationAuthority& authority,
               PreparedCorpus& corpus,
               const CStoreGuid& c_guid,
               const FStoreGuid& f_guid,
               StageSplit& split,
               bool timed,
               std::string* receipts,
               size_t receipt_base) {
    const PreparationRouteKey route{c_guid, f_guid, 1, ProfileId::P29V1};
    uint64_t request_token = 0;
    const uint64_t leg_start_cpu_ns = [] {
        struct timespec cpu {};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);
        return uint64_t(cpu.tv_sec) * 1000000000ULL + uint64_t(cpu.tv_nsec);
    }();
    for (size_t t = 0; t < corpus.files.size(); ++t) {
        StageClock mark;
        const PrepareRequestKey request{1, ++request_token};
        std::optional<std::shared_ptr<const P50SourceView>> source =
            read_source_fd(corpus.files[t]->fd, uint64_t{64} << 20);
        StageClock after_read;
        if (timed)
            split.add("read_source_fd(+fused digest)", mark, after_read);
        if (!source.has_value())
            throw std::runtime_error("read_source_fd refused a staged synthetic TU");
        mark = after_read;
        // The digest is fused into the read (P50SourceView); the historical
        // separate whole-input hash pass is gone.
        const Digest128 raw_digest = (*source)->digest();
        StageClock after_digest;
        if (timed)
            split.add("digest128", mark, after_digest);
        mark = after_digest;
        P50PreparationAuthority::PrepareSourceTiming prepare_timing{};
        std::shared_ptr<const P50PreparationAuthority::PreparedSource> prepared =
            authority.prepare_source(request, (*source)->span(), *source,
                                     raw_digest, timed ? &prepare_timing : nullptr);
        StageClock after_prepare;
        if (timed) {
            split.add("prepare_source", mark, after_prepare);
            split.add_ns("prepare_source.plan", prepare_timing.plan_ns);
            split.add_ns("prepare_source.mutex_hold", prepare_timing.mutex_hold_ns);
            split.add_ns("prepare_source.mutex_wait", prepare_timing.mutex_wait_ns);
        }
        if (!prepared)
            throw std::runtime_error("prepare_source refused a synthetic TU");
        mark = after_prepare;
        authority.install_source(request, prepared);
        StageClock after_install;
        if (timed)
            split.add("install_source", mark, after_install);
        mark = after_install;
        const PreparedTuHandle handle =
            authority.prepare_for_route(route, request, (*source)->span());
        StageClock after_route;
        if (timed)
            split.add("prepare_for_route", mark, after_route);
        if (!handle)
            throw std::runtime_error("prepare_for_route refused a synthetic TU");
        mark = after_route;
        // Production pins the route's reuse decision at HISTORY_RESET with
        // the F store's fingerprint before any FILL is built.  The bench runs
        // no fingerprint worker, so the authority's route fingerprint is the
        // zero digest and the pin lands on reuse=false -- the same decision
        // every farm leg with no system-source reuse records
        // (system_source_reuse=false in the source-result trace).
        authority.pin_p29v1_system_source_reuse(handle, Digest128{});
        StageClock after_pin;
        if (timed)
            split.add("pin_reuse", mark, after_pin);
        mark = after_pin;
        const std::optional<std::span<const uint8_t>> fill =
            authority.fill_p29v1_before_need(handle);
        StageClock after_fill;
        if (timed)
            split.add("fill_before_need", mark, after_fill);
        mark = after_fill;
        if (timed && std::getenv("P29CCPUBENCH_DEBUG") != nullptr) {
            const double read_ms = double(after_read.cpu_ns - leg_start_cpu_ns) / 1e6;
            const double digest_ms = double(after_digest.cpu_ns - after_read.cpu_ns) / 1e6;
            const double prep_ms = double(after_prepare.cpu_ns - after_digest.cpu_ns) / 1e6;
            const double route_ms = double(after_route.cpu_ns - after_install.cpu_ns) / 1e6;
            const double fill_ms = double(after_fill.cpu_ns - after_pin.cpu_ns) / 1e6;
            char line[512];
            std::snprintf(line, sizeof(line),
                         "  tx %3zu read=%7.2f digest=%6.2f prep=%7.2f "
                         "(plan %6.2f hold %6.2f) route=%6.2f fill=%7.2f "
                         "fillbytes=%8zu\n",
                         t, read_ms, digest_ms, prep_ms,
                         double(prepare_timing.plan_ns) / 1e6,
                         double(prepare_timing.mutex_hold_ns) / 1e6,
                         route_ms, fill_ms, fill ? fill->size() : size_t{0});
            std::fputs(line, stdout);
        }
        authority.commit(handle);
        StageClock after_commit;
        if (timed)
            split.add("commit", mark, after_commit);
        mark = after_commit;
        (void)authority.release(handle);
        StageClock after_release;
        if (timed)
            split.add("release", mark, after_release);
        mark = after_release;
        authority.release_source(request);
        StageClock after_release_source;
        if (timed)
            split.add("release_source", mark, after_release_source);
        if (receipts != nullptr) {
            char line[512];
            std::snprintf(line, sizeof(line),
                          "tx %zu raw=%zu rawdigest=%s fill=%zu fillframes=%zu\n",
                          receipt_base + t, (*source)->size(),
                          hex_digest(raw_digest).c_str(),
                          fill ? fill->size() : size_t{0}, fill ? size_t{1} : size_t{0});
            *receipts += line;
        }
        (void)fill;
    }
}

// ---------------------------------------------------------------------------
// Identity mode: the same corpus through the real loopback endpoints, with
// every C->F frame hashed.  Fresh authority per leg, exactly like the bench
// leg, so the interner warms the same way.
// ---------------------------------------------------------------------------

struct WireDigest {
    icecc::Digest128 value{};
    uint64_t bytes = 0;
    uint64_t frames = 0;
    std::vector<uint8_t> collector;
    void observe(const Message& message) {
        const std::vector<uint8_t> frame = encode_frame(message);
        collector.insert(collector.end(), frame.begin(), frame.end());
        bytes += frame.size();
        ++frames;
    }
    void finish() {
        value = icecc::digest128(collector);
    }
};

std::string loopback_identity(const DriveConfig& config,
                              PreparedCorpus& corpus,
                              const CStoreGuid& c_guid,
                              const FStoreGuid& f_guid,
                              HistoryNonce history_nonce) {
    EndpointCaps caps{};
    caps.profile = ProfileId::P29V1;
    caps.supported_profiles = profile_bit(ProfileId::P29V1);
    PreparationAuthorityLimits limits{};
    auto authority = std::make_shared<P50PreparationAuthority>(
        c_guid, caps.zstd, limits, kCurrentProductCompressionLevel,
        ProfileId::P29V1);
    ActionTrace actions(1024);
    CompletionLog completions(4096);
    P50ServerEndpointConfig server_config;
    server_config.input_job_state = [](CStoreGuid, const TxBegin&,
                                       const TxCommit&, std::span<const uint8_t>) {
        return InputJobState::Open;
    };
    P50ServerEndpoint server(f_guid, caps, &completions, &actions,
                              std::move(server_config));
    P50ClientEndpoint client(authority, caps, history_nonce, &completions,
                              &actions, std::nullopt,
                              std::function<void(EndpointCancelPermit)>{},
                              std::function<void(EndpointCancelPermit,
                                                 EndpointTerminalResult)>{});
    WireDigest c_to_f;
    WireDigest f_to_c;
    const auto observe_c = [&](ActorSide, const Message& message) {
        c_to_f.observe(message);
    };
    const auto observe_f = [&](ActorSide, const Message& message) {
        f_to_c.observe(message);
    };

    const PreparationRouteKey route{c_guid, f_guid, 1, ProfileId::P29V1};
    std::string receipts;
    uint64_t request_token = 0;
    asio::io_context context;
    tcp::acceptor acceptor(context,
                           tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const tcp::endpoint endpoint = acceptor.local_endpoint();
    for (size_t t = 0; t < corpus.files.size(); ++t) {
        const PrepareRequestKey request{1, ++request_token};
        std::optional<std::shared_ptr<const P50SourceView>> source =
            read_source_fd(corpus.files[t]->fd, uint64_t{64} << 20);
        if (!source.has_value())
            throw std::runtime_error("read_source_fd refused a staged synthetic TU");
        const Digest128 raw_digest = (*source)->digest();
        auto prepared_source = authority->prepare_source(request, (*source)->span(),
                                                         *source, raw_digest);
        if (!prepared_source)
            throw std::runtime_error("prepare_source refused a synthetic TU");
        authority->install_source(request, prepared_source);
        const PreparedTuHandle handle =
            authority->prepare_for_route(route, request, (*source)->span());
        if (!handle)
            throw std::runtime_error("prepare_for_route refused a synthetic TU");
        // The endpoint run consumes the handle: it encodes BEGIN/BODY,
        // answers NEED with FILL, and commits.
        EndpointIoControl client_control;
        EndpointIoControl server_control;
        client_control.outbound_message_observer = observe_c;
        server_control.outbound_message_observer = observe_f;
        context.restart();
        auto server_future = asio::co_spawn(
            context, server.accept_one(acceptor, std::move(server_control)),
            asio::use_future);
        auto client_future = asio::co_spawn(
            context, client.run(endpoint, handle, std::move(client_control)),
            asio::use_future);
        context.run();
        const ServerRunResult server_result = server_future.get();
        const ClientRunResult client_result = client_future.get();
        if (client_result.status != ClientRunStatus::Committed ||
            server_result.status != ServerRunStatus::Completed)
            throw std::runtime_error(
                "loopback identity TU did not commit (client status " +
                std::to_string(int(client_result.status)) + ", server status " +
                std::to_string(int(server_result.status)) + ")");
        char line[512];
        std::snprintf(line, sizeof(line),
                      "tx %zu raw=%zu rawdigest=%s c2fbytes=%zu\n",
                      t, (*source)->size(), hex_digest(raw_digest).c_str(),
                      c_to_f.bytes);
        receipts += line;
        c_to_f.finish();
        receipts += "tx " + std::to_string(t) + " c2fdigest=" +
                    hex_digest(c_to_f.value) + "\n";
        // Server input reclamation, exactly like the batch sim.
        if (server_result.committed_input)
            server.close_input_job(*server_result.committed_input);
        server.collect_input_garbage();
        (void)authority->release(handle);
        authority->release_source(request);
    }
    const AuthorityIdentity identity = authority_identity(*authority);
    receipts += "route_history_bytes " +
                std::to_string(identity.route_history_bytes) + "\n";
    receipts += "route_history_digest " +
                hex_digest(identity.route_history_digest) + "\n";
    receipts += "interner_reserved " +
                std::to_string(identity.interner_reserved) + "\n";
    receipts += "interner_committed " +
                std::to_string(identity.interner_committed) + "\n";
    receipts += "live_entries " + std::to_string(identity.live_entries) + "\n";
    receipts += "retained_encoded " +
                std::to_string(identity.retained_encoded) + "\n";
    receipts += "f2cframes " + std::to_string(f_to_c.frames) + "\n";
    return receipts;
}

#if defined(ICECC_P50SIM_STAGE_TIMERS)
// Accumulates the product's own per-stage timers (see
// cache/codec/p29_wire.h P29StageTimerEntry) so a measurement build of this
// bench (-DICECC_P50SIM_STAGE_TIMERS on the bench AND libprotocol50) can
// attribute the per-TX CPU to the product's named stages.
std::array<std::atomic<std::uint64_t>, 16> g_stage_ns{};
const char* k_stage_names[16] = {
    "ReceiverReceiveFill",      "ReceiverTakeMaterialized",
    "ReceiverMaterializedDigest", "ReceiverTransactionDigest",
    "ReceiverPendingSegmentDigest", "SenderInternAndMatch",
    "SenderEncode",             "ReceiverMaterializeCopyback",
    "SenderPrepareReserve",      "SenderMatchPrepare",
    "SenderBuildRoot",           "SenderEncodeRoot",
    "SenderEncodeBlocks",        "", "", "",
};
#endif

}  // namespace

#if defined(ICECC_P50SIM_STAGE_TIMERS)
extern "C" void icecc_p50sim_p29_stage_timer_add(
    unsigned entry, std::uint64_t nanoseconds) noexcept {
    if (entry < 16)
        g_stage_ns[entry].fetch_add(nanoseconds, std::memory_order_relaxed);
}
#endif

int main(int argc, char** argv) {
    try {
        DriveConfig config;
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument(argv[index]);
            const auto value = [&]() -> std::string_view {
                if (index + 1 >= argc)
                    throw std::invalid_argument("option lacks a value");
                return std::string_view(argv[++index]);
            };
            if (argument == "--tus")
                config.tus = std::stoul(std::string(value()));
            else if (argument == "--tu-mb")
                config.tu_mb = std::stoul(std::string(value()));
            else if (argument == "--shared-fraction")
                config.shared_fraction = std::stod(std::string(value()));
            else if (argument == "--seed")
                config.seed = std::stoull(std::string(value()));
            else if (argument == "--pool-oversize")
                config.pool_oversize = std::stod(std::string(value()));
            else if (argument == "--repeats")
                config.repeats = std::stoul(std::string(value()));
            else if (argument == "--json")
                config.json = true;
            else if (argument == "--record")
                config.record = std::string(value());
            else if (argument == "--compare")
                config.compare = std::string(value());
            else if (argument == "--scratch")
                config.scratch = std::string(value());
            else
                throw std::invalid_argument("unknown option: " +
                                            std::string(argument));
        }

        const CStoreGuid c_guid = CStoreGuid::from_u64(0x43435550324e3250ULL);
        const FStoreGuid f_guid = FStoreGuid::from_u64(0x46464646504f3250ULL);
        const HistoryNonce history_nonce{1};

        if (!config.record.empty() || !config.compare.empty()) {
            PreparedCorpus corpus = stage_corpus(config);
            std::string receipts =
                loopback_identity(config, corpus, c_guid, f_guid, history_nonce);
            if (!config.record.empty()) {
                FILE* out = std::fopen(config.record.c_str(), "w");
                if (!out)
                    throw std::runtime_error("cannot open --record output");
                std::fwrite(receipts.data(), 1, receipts.size(), out);
                std::fclose(out);
                std::fprintf(stdout, "recorded identity receipts (%zu bytes)\n",
                             receipts.size());
                return 0;
            }
            FILE* reference = std::fopen(config.compare.c_str(), "r");
            if (!reference)
                throw std::runtime_error("cannot open --compare reference");
            std::string expected;
            char buffer[4096];
            size_t read_bytes = 0;
            while ((read_bytes = std::fread(buffer, 1, sizeof(buffer), reference)) != 0)
                expected.append(buffer, buffer + read_bytes);
            std::fclose(reference);
            if (expected == receipts) {
                std::fprintf(stdout,
                             "p29ccpubench differential identity: MATCH "
                             "(%zu bytes, corpus=%zu tx @ %zu MB)\n",
                             receipts.size(), config.tus, config.tu_mb);
                return 0;
            }
            std::fprintf(stderr, "p29ccpubench differential identity: DIFFER\n");
            size_t line = 1;
            size_t at = 0;
            while (at < expected.size() && at < receipts.size() &&
                   expected[at] == receipts[at]) {
                if (expected[at] == '\n')
                    ++line;
                ++at;
            }
            std::fprintf(stderr, "first difference at line %zu offset %zu\n", line, at);
            const size_t from = at > 160 ? at - 160 : 0;
            std::fprintf(stderr, "expected: ...%.*s\n",
                          static_cast<int>(std::min<size_t>(
                              320, expected.size() - from)),
                          expected.data() + from);
            std::fprintf(stderr, "recorded: ...%.*s\n",
                          static_cast<int>(std::min<size_t>(
                              320, receipts.size() - from)),
                          receipts.data() + from);
            return 1;
        }

        // Benchmark mode: one fresh authority per repeat; report the mean.
        PreparedCorpus corpus = stage_corpus(config);
        struct LegResult {
            double per_tx_ms;
            std::vector<std::pair<const char*, double>> stages;
        };
        std::vector<LegResult> legs;
        for (size_t repeat = 0; repeat < std::max<size_t>(1, config.repeats);
             ++repeat) {
            EndpointCaps caps{};
            caps.profile = ProfileId::P29V1;
            caps.supported_profiles = profile_bit(ProfileId::P29V1);
            PreparationAuthorityLimits limits{};
            auto authority = std::make_shared<P50PreparationAuthority>(
                c_guid, caps.zstd, limits, kCurrentProductCompressionLevel,
                ProfileId::P29V1);
            StageSplit split;
            drive_leg(*authority, corpus, c_guid, f_guid, split, true, nullptr, 0);
            uint64_t total_cpu = 0;
            for (const auto& [name, ns] : split.cpu)
                total_cpu += ns;
            LegResult leg;
            leg.per_tx_ms = double(total_cpu) / 1e6 / double(corpus.files.size());
            for (const auto& [name, ns] : split.cpu) {
                leg.stages.emplace_back(
                    name, double(ns) / 1e6 / double(corpus.files.size()));
            }
            std::sort(leg.stages.begin(), leg.stages.end(),
                      [](const auto& left, const auto& right) {
                          return left.second > right.second;
                      });
            legs.push_back(std::move(leg));
        }
        const LegResult& report = legs.back();
        double sum = 0;
        for (const LegResult& leg : legs)
            sum += leg.per_tx_ms;
        const double mean_per_tx_ms = sum / double(legs.size());
        if (config.json) {
            std::printf(
                "{\"schema\":\"p29ccpubench-v1\",\"transfers\":%zu,"
                "\"tu_mb\":%zu,\"shared_fraction\":%.2f,\"repeats\":%zu,"
                "\"per_tx_cpu_ms_mean\":%.3f,\"per_tx_cpu_ms_last\":%.3f,"
                "\"stages_ms_mean_per_tx\":{",
                corpus.files.size(), config.tu_mb, config.shared_fraction,
                legs.size(), mean_per_tx_ms, report.per_tx_ms);
            bool first = true;
            for (const auto& [name, ms] : report.stages) {
                std::printf("%s\"%s\":%.4f", first ? "" : ",", name, ms);
                first = false;
            }
            std::printf("}}\n");
        } else {
            std::printf(
                "p29ccpubench: %zu transfers x %zu MB (shared %.1f%%, "
                "pool %.3fx), %zu repeat(s): %.3f ms CPU per TX (mean), "
                "%.3f (last)\n",
                corpus.files.size(), config.tu_mb,
                config.shared_fraction * 100.0, config.pool_oversize,
                legs.size(), mean_per_tx_ms, report.per_tx_ms);
            std::printf("  %-44s %14s %10s\n", "stage", "ms/TX", "share");
            for (const auto& [name, ms] : report.stages)
                std::printf("  %-44s %14.4f %9.2f%%\n", name, ms,
                            100.0 * ms / report.per_tx_ms);
        }
#if defined(ICECC_P50SIM_STAGE_TIMERS)
        for (size_t i = 0; i < 16; ++i)
            if (k_stage_names[i][0] != '\0' && g_stage_ns[i].load() != 0)
                std::fprintf(stdout, "  product-stage %-30s %10.3f ms/TX\n",
                             k_stage_names[i],
                             double(g_stage_ns[i].load()) / 1e6 /
                                 double(corpus.files.size()) /
                                 double(std::max<size_t>(1, config.repeats)));
#endif
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "p29ccpubench: %s\n", error.what());
        return 1;
    }
}
