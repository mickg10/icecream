#include "cache/codec/p29_intern.h"
#include "cache/p50_slice0.h"
#include "cache/codec/p29_online_s1.h"
#include "codec_golden_compare.h"

#include <zstd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using icecc::codec::P29InternAllocation;
using icecc::codec::P29Interner;
using icecc::codec::P29InternLayout;

[[noreturn]] void die(const std::string &reason) {
    throw std::runtime_error(reason);
}

void require(bool condition, const std::string &reason) {
    if (!condition)
        die(reason);
}

// Regression/correctness check for the AVX2/SSE2 extend_match_length() helper in
// p29_online_s1.h (the vectorized replacement for OnlineS1::build()'s inner hash-chain
// match-extension compare). The scalar implementation (extend_match_scalar()) is the
// specification: it is a one-element-at-a-time scan, trivially correct by inspection, so
// every SIMD variant must agree with it EXACTLY for every input, including every boundary
// right at a vector width (4 for SSE2, 8 for AVX2) and every maximum that is not a multiple
// of either width. The dispatcher (extend_match_length()) picks AVX2 or SSE2 depending on
// what the current CPU supports, so this test calls both concrete variants directly (not
// just the dispatcher) to make sure whichever one the bench host lacks is still exercised
// here on whatever host runs this unit test.
void check_match_extend_simd() {
    std::mt19937 rng(0xA5A5u);
    std::uniform_int_distribution<std::uint32_t> value_dist(0, 7);  // small alphabet: frequent
                                                                     // equal runs, like real
                                                                     // region-id occurrences.

    const auto reference_matches = [](const std::vector<std::uint32_t> &a,
                                       const std::vector<std::uint32_t> &b,
                                       std::uint32_t maximum) {
        return p29::match_detail::extend_match_scalar(a.data(), b.data(), maximum);
    };

    // Fixed boundary cases: mismatch at every offset from 0 through well past 2x the AVX2
    // width (8), at totals that are shorter than one vector, exactly one vector, several
    // vectors, and with a remainder -- plus the all-equal (maximum reached) case for each
    // total.
    const std::vector<std::uint32_t> totals = {0, 1, 2, 3, 4, 5, 6, 7,
                                                 8, 9, 15, 16, 17, 23,
                                                 24, 31, 32, 33, 64, 100};
    for (std::uint32_t total : totals) {
        for (std::uint32_t mismatch_at = 0; mismatch_at <= total + 2; ++mismatch_at) {
            std::vector<std::uint32_t> a(total + 8, 1);
            std::vector<std::uint32_t> b(total + 8, 1);
            if (mismatch_at < total) {
                b[mismatch_at] = 2;  // forces a's and b's leading run to end at mismatch_at
            }
            const std::uint32_t expect = reference_matches(a, b, total);
            require(expect == std::min(mismatch_at, total),
                    "extend_match_scalar reference disagrees with its own construction");
            const std::uint32_t got_sse2 =
                p29::match_detail::extend_match_sse2(a.data(), b.data(), total);
            require(got_sse2 == expect,
                    "extend_match_sse2 length differs from scalar reference at total=" +
                        std::to_string(total) +
                        " mismatch_at=" + std::to_string(mismatch_at) + ": got " +
                        std::to_string(got_sse2) + " expected " + std::to_string(expect));
            if (p29::match_detail::avx2_available()) {
                const std::uint32_t got_avx2 =
                    p29::match_detail::extend_match_avx2(a.data(), b.data(), total);
                require(got_avx2 == expect,
                        "extend_match_avx2 length differs from scalar reference at total=" +
                            std::to_string(total) +
                            " mismatch_at=" + std::to_string(mismatch_at) + ": got " +
                            std::to_string(got_avx2) + " expected " + std::to_string(expect));
            }
        }
    }

    // Randomized/adversarial inputs: fully independent random content on both sides (so
    // mismatches land at every possible lane within every vector step), across many random
    // lengths, run many trials.
    std::uniform_int_distribution<std::uint32_t> length_dist(0, 257);
    for (int trial = 0; trial != 20000; ++trial) {
        const std::uint32_t total = length_dist(rng);
        std::vector<std::uint32_t> a(total);
        std::vector<std::uint32_t> b(total);
        for (std::uint32_t index = 0; index != total; ++index) {
            a[index] = value_dist(rng);
            b[index] = value_dist(rng);
        }
        const std::uint32_t expect = reference_matches(a, b, total);
        const std::uint32_t got_sse2 =
            p29::match_detail::extend_match_sse2(a.data(), b.data(), total);
        require(got_sse2 == expect,
                "extend_match_sse2 length differs from scalar reference on random trial " +
                    std::to_string(trial) + " total=" + std::to_string(total));
        if (p29::match_detail::avx2_available()) {
            const std::uint32_t got_avx2 =
                p29::match_detail::extend_match_avx2(a.data(), b.data(), total);
            require(got_avx2 == expect,
                    "extend_match_avx2 length differs from scalar reference on random trial " +
                        std::to_string(trial) + " total=" + std::to_string(total));
        }
        const std::uint32_t got_dispatch =
            p29::match_detail::extend_match_length(a.data(), b.data(), total);
        require(got_dispatch == expect,
                "extend_match_length dispatcher differs from scalar reference on random "
                "trial " +
                    std::to_string(trial) + " total=" + std::to_string(total));
    }

    std::cout << "codec_intern: match_extend_simd check PASS (20000 random trials + "
              << totals.size() << " boundary totals, avx2="
              << (p29::match_detail::avx2_available() ? "yes" : "no") << ")\n";
}

class MmapProvider {
  public:
    explicit MmapProvider(std::size_t budget,
                          std::size_t fail_allocation =
                              std::numeric_limits<std::size_t>::max())
        : budget_(budget), fail_allocation_(fail_allocation) {}

    [[noreturn]] static void fail(const char *reason) {
        throw std::length_error(reason);
    }

    bool try_reserve_interner(std::size_t bytes) {
        ++reservation_calls_;
        if (bytes > budget_ - reserved_)
            return false;
        reserved_ += bytes;
        return true;
    }

    void *allocate_interner(P29InternAllocation, std::size_t bytes,
                            std::size_t) {
        if (allocation_calls_++ == fail_allocation_)
            return nullptr;
        if (bytes > reserved_ - live_bytes_)
            return nullptr;
        void *result = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (result == MAP_FAILED)
            return nullptr;
#ifdef MADV_HUGEPAGE
        (void)madvise(result, bytes, MADV_HUGEPAGE);
#endif
        live_bytes_ += bytes;
        return result;
    }

    void deallocate_interner(P29InternAllocation, void *pointer,
                             std::size_t bytes, std::size_t) noexcept {
        if (pointer)
            (void)munmap(pointer, bytes);
        live_bytes_ -= bytes;
        ++deallocation_calls_;
    }

    void release_interner_reservation(std::size_t bytes) noexcept {
        if (bytes <= reserved_)
            reserved_ -= bytes;
        else
            accounting_error_ = true;
        ++release_calls_;
    }

    void report_interner_usage(std::size_t reserved, std::size_t committed) {
        reported_reserved_ = reserved;
        reported_committed_ = committed;
        ++report_calls_;
    }

    void publish_line(std::uint32_t, std::span<const std::uint8_t>) {}
    void publish_region(std::uint32_t, std::span<const std::uint32_t>) {}

    [[nodiscard]] std::size_t reserved() const { return reserved_; }
    [[nodiscard]] std::size_t live_bytes() const { return live_bytes_; }
    [[nodiscard]] std::size_t allocation_calls() const {
        return allocation_calls_;
    }
    [[nodiscard]] std::size_t deallocation_calls() const {
        return deallocation_calls_;
    }
    [[nodiscard]] std::size_t release_calls() const { return release_calls_; }
    [[nodiscard]] std::size_t reported_reserved() const {
        return reported_reserved_;
    }
    [[nodiscard]] std::size_t reported_committed() const {
        return reported_committed_;
    }
    [[nodiscard]] std::size_t report_calls() const { return report_calls_; }
    [[nodiscard]] bool accounting_error() const { return accounting_error_; }

  private:
    std::size_t budget_ = 0;
    std::size_t reserved_ = 0;
    std::size_t live_bytes_ = 0;
    std::size_t fail_allocation_ = 0;
    std::size_t reservation_calls_ = 0;
    std::size_t allocation_calls_ = 0;
    std::size_t deallocation_calls_ = 0;
    std::size_t release_calls_ = 0;
    std::size_t reported_reserved_ = 0;
    std::size_t reported_committed_ = 0;
    std::size_t report_calls_ = 0;
    bool accounting_error_ = false;
};

class ProductTestProvider : public MmapProvider {
  public:
    explicit ProductTestProvider(std::size_t budget)
        : MmapProvider(budget),
          arena_(icecc::p50::CStoreGuid::from_u64(0x29)) {
        line_keys_.push_back({});
    }

    void publish_line(std::uint32_t id,
                      std::span<const std::uint8_t> bytes) {
        require(id == line_keys_.size(), "product Line ordinals are not dense");
        line_keys_.push_back(
            arena_.intern_bytes(icecc::p50::ObjectType::Line, bytes));
    }

    void publish_region(std::uint32_t id,
                        std::span<const std::uint32_t> children) {
        require(id == region_keys_.size(),
                "product Region ordinals are not dense");
        std::vector<icecc::p50::Key64> keys;
        keys.reserve(children.size());
        for (std::uint32_t child : children) {
            require(child != 0 && child < line_keys_.size(),
                    "product Region names an absent Line ordinal");
            keys.push_back(line_keys_[child]);
        }
        region_keys_.push_back(
            arena_.intern_children(icecc::p50::ObjectType::Region, keys));
    }

    [[nodiscard]] const icecc::p50::CObjectArena &arena() const {
        return arena_;
    }
    [[nodiscard]] const std::vector<icecc::p50::Key64> &line_keys() const {
        return line_keys_;
    }
    [[nodiscard]] const std::vector<icecc::p50::Key64> &region_keys() const {
        return region_keys_;
    }

  private:
    icecc::p50::CObjectArena arena_;
    std::vector<icecc::p50::Key64> line_keys_;
    std::vector<icecc::p50::Key64> region_keys_;
};

static_assert(icecc::codec::P29InternProvider<MmapProvider>);
static_assert(icecc::codec::P29InternProvider<ProductTestProvider>);

[[nodiscard]] P29InternLayout small_layout(std::size_t line_capacity = 32) {
    P29InternLayout layout;
    layout.tiny_capacity = 16;
    layout.short_capacity = 16;
    layout.line_capacity = line_capacity;
    layout.region_index_capacity = 16;
    layout.region_capacity = 16;
    layout.line_reference_capacity = 64;
    layout.line_bytes_capacity = 4096;
    layout.region_bytes_capacity = 4096;
    layout.region_line_id_capacity = 128;
    return layout;
}

// Intentionally independent from p29_interner_reservation(): these are the
// frozen layout widths whose sum a provider must reserve before allocation 1.
[[nodiscard]] std::size_t independent_reservation(const P29InternLayout &layout) {
    return layout.tiny_capacity * 16 + layout.short_capacity * 24 +
           layout.line_capacity * 24 + layout.region_index_capacity * 4 +
           layout.region_capacity * 32 + layout.line_reference_capacity * 8 +
           layout.line_bytes_capacity + layout.region_bytes_capacity +
           layout.region_line_id_capacity * 4;
}

[[nodiscard]] std::span<const std::uint8_t> bytes(std::string_view value) {
    return {reinterpret_cast<const std::uint8_t *>(value.data()), value.size()};
}

void allocation_controls() {
    const P29InternLayout layout = small_layout();
    const std::size_t reservation = independent_reservation(layout);
    require(reservation == icecc::codec::p29_interner_reservation(layout),
            "independent reservation computation differs");

    MmapProvider short_budget(reservation - 1);
    bool rejected = false;
    try {
        P29Interner<MmapProvider> interner(short_budget, layout);
        (void)interner;
    } catch (const std::length_error &) {
        rejected = true;
    }
    require(rejected, "one-byte-short budget was accepted");
    require(short_budget.allocation_calls() == 0 && short_budget.reserved() == 0 &&
                short_budget.live_bytes() == 0 &&
                short_budget.reported_reserved() == 0,
            "one-byte-short budget allocated or accounted memory");

    MmapProvider injected(reservation, 3);
    rejected = false;
    try {
        P29Interner<MmapProvider> interner(injected, layout);
        (void)interner;
    } catch (const std::length_error &) {
        rejected = true;
    }
    require(rejected, "injected allocation failure was accepted");
    require(injected.live_bytes() == 0 && injected.reserved() == 0 &&
                injected.deallocation_calls() == 3 &&
                injected.release_calls() == 1 && !injected.accounting_error(),
            "partial construction did not release every allocation/reservation");
}

[[nodiscard]] std::size_t accounting_control_once() {
    const P29InternLayout layout = small_layout();
    MmapProvider provider(independent_reservation(layout));
    {
        P29Interner<MmapProvider> interner(provider, layout);
        std::vector<std::uint32_t> regions;
        interner.process(bytes("# 1 \"a\"\nalpha beta gamma\n# 2 \"b\"\ndelta\n"),
                         regions);
        require(regions.size() == 2, "accounting corpus Region split differs");
        require(provider.reported_reserved() == independent_reservation(layout),
                "provider reported reservation differs");
        require(provider.reported_committed() == interner.committed_bytes(),
                "provider committed-byte report differs");
    }
    require(provider.reserved() == 0 && provider.live_bytes() == 0,
            "successful interner destruction leaked accounting");
    return provider.reported_committed();
}

void mutation_and_capacity_controls() {
    const P29InternLayout layout = small_layout();
    MmapProvider provider(independent_reservation(layout));
    P29Interner<MmapProvider> interner(provider, layout);
    const std::string original = "abcdefghijklmnopq\n";
    const std::string unrelated = "qrstuvwxyzabcdefgh\n";
    std::string changed = original;
    changed[7] ^= 1;
    const std::uint32_t original_id = interner.intern_line(bytes(original));
    const std::uint32_t unrelated_id = interner.intern_line(bytes(unrelated));
    const std::uint32_t changed_id = interner.intern_line(bytes(changed));
    require(original_id != changed_id,
            "one-byte Line mutation retained the original id");
    require(interner.intern_line(bytes(original)) == original_id &&
                interner.intern_line(bytes(unrelated)) == unrelated_id &&
                interner.intern_line(bytes(changed)) == changed_id,
            "one-byte Line mutation disturbed an unrelated identity");

    const P29InternLayout full_layout = small_layout(2);
    MmapProvider full_provider(independent_reservation(full_layout));
    P29Interner<MmapProvider> full(full_provider, full_layout);
    const std::string first = "line-table-entry-number-one\n";
    const std::string second = "line-table-entry-number-two\n";
    const std::string third = "line-table-entry-number-three\n";
    const std::uint32_t first_id = full.intern_line(bytes(first));
    const std::uint32_t second_id = full.intern_line(bytes(second));
    bool rejected = false;
    try {
        (void)full.intern_line(bytes(third));
    } catch (const std::length_error &) {
        rejected = true;
    }
    require(rejected, "undersized Line table wrapped instead of failing");
    require(full.distinct_lines() == 2 &&
                full.intern_line(bytes(first)) == first_id &&
                full.intern_line(bytes(second)) == second_id,
            "Line-table failure overwrote an occupied slot");
}

// With budget beyond the layout, full hash tables add levels instead of
// failing, and no id moves.
void growth_controls() {
    P29InternLayout layout = small_layout(4);
    layout.region_capacity = 512;
    layout.line_reference_capacity = 2048;
    layout.line_bytes_capacity = 1 << 16;
    layout.region_bytes_capacity = 1 << 16;
    layout.region_line_id_capacity = 1024;
    MmapProvider provider(independent_reservation(layout) + (1 << 20));
    P29Interner<MmapProvider> interner(provider, layout);
    std::vector<std::uint32_t> ids;
    std::string corpus;
    for (int index = 0; index != 300; ++index) {
        const std::string name = std::to_string(index);
        ids.push_back(interner.intern_line(bytes("growing-line-table-" + name + "\n")));
        ids.push_back(interner.intern_line(bytes("s" + name + "\n")));
        corpus += "# " + name + " \"r\"\nregion-" + name + "\n";
    }
    std::vector<std::uint32_t> regions;
    interner.process(bytes(corpus), regions);
    for (int index = 0; index != 300; ++index) {
        const std::string name = std::to_string(index);
        require(interner.intern_line(bytes("growing-line-table-" + name + "\n")) ==
                        ids[2 * index] &&
                    interner.intern_line(bytes("s" + name + "\n")) == ids[2 * index + 1],
                "a Line moved when its table grew");
    }
    std::vector<std::uint32_t> again;
    interner.process(bytes(corpus), again);
    const P29InternLayout grown = interner.capacity();
    require(regions.size() == 300 && again == regions &&
                grown.line_capacity > layout.line_capacity &&
                grown.short_capacity > layout.short_capacity &&
                grown.region_index_capacity > layout.region_index_capacity &&
                provider.reserved() == interner.reserved_bytes() &&
                provider.reported_reserved() == interner.reserved_bytes(),
            "hash tables did not grow within the provider budget");
}

void product_provider_control() {
    const P29InternLayout layout = small_layout();
    ProductTestProvider provider(independent_reservation(layout));
    {
        P29Interner<ProductTestProvider> interner(provider, layout);
        std::vector<std::uint32_t> regions;
        const std::string input =
            "# 1 \"one\"\nfirst line\n# 2 \"two\"\nsecond line\n";
        interner.process(bytes(input), regions);
        require(provider.line_keys().size() == interner.distinct_lines() + 1,
                "CObjectArena provider Line map differs");
        require(provider.region_keys().size() == interner.distinct_regions(),
                "CObjectArena provider Region map differs");
        require(provider.arena().objects().size() ==
                    interner.distinct_lines() + interner.distinct_regions(),
                "CObjectArena provider did not publish every distinct object");
    }
}

[[nodiscard]] fs::path golden_root() {
    if (const char *source = std::getenv("ICECC_TEST_TOP_SRCDIR"))
        return fs::path(source) / "unittests/codec_golden";
    return fs::path("codec_golden");
}

[[nodiscard]] std::vector<std::uint8_t> read_file(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        die("cannot read " + path.string());
    input.seekg(0, std::ios::end);
    const std::streamoff length = input.tellg();
    if (length < 0)
        die("cannot size " + path.string());
    input.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> result(static_cast<std::size_t>(length));
    if (!result.empty() &&
        !input.read(reinterpret_cast<char *>(result.data()), length))
        die("short read from " + path.string());
    return result;
}

[[nodiscard]] std::vector<fs::path> read_manifest(const fs::path &path) {
    std::ifstream input(path);
    if (!input)
        die("cannot read manifest " + path.string());
    std::vector<fs::path> result;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty())
            result.emplace_back(line);
    }
    return result;
}

[[nodiscard]] std::vector<fs::path> phase0_inputs() {
    const fs::path root = golden_root() / "inputs";
    std::vector<fs::path> result;
    for (unsigned index = 0; index != 8; ++index) {
        std::string name = "real-0" + std::to_string(index) + ".ii";
        result.push_back(root / name);
    }
    result.push_back(root / "synthetic-repeat.ii");
    result.push_back(root / "synthetic-unique.ii");
    result.push_back(root / "synthetic-blank-heavy.ii");
    return result;
}

void put_varint(std::vector<std::uint8_t> &output, std::uint64_t value) {
    while (value >= 0x80) {
        output.push_back(static_cast<std::uint8_t>(value) | 0x80);
        value >>= 7;
    }
    output.push_back(static_cast<std::uint8_t>(value));
}

void append_root_frames(std::span<const std::uint32_t> occurrences,
                        std::span<const std::size_t> offsets,
                        std::vector<std::uint8_t> &wire) {
    std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> context(
        ZSTD_createCCtx(), ZSTD_freeCCtx);
    require(context != nullptr, "cannot allocate zstd C context");
    p29::OnlineS1 matcher({3, 1024, 22});
    std::vector<std::uint8_t> root;
    std::vector<std::uint8_t> encoded;
    for (std::size_t tu = 0; tu + 1 < offsets.size(); ++tu) {
        const auto begin = occurrences.begin() + offsets[tu];
        const auto end = occurrences.begin() + offsets[tu + 1];
        const std::vector<std::uint32_t> current(begin, end);
        const p29::TuPlan plan = matcher.admit(current);
        root.clear();
        for (const p29::Ref &reference : plan.root) {
            require(reference.id < (std::uint64_t{1} << 31),
                    "S1 reference exceeds typed-tag space");
            const std::uint32_t tag =
                (reference.id << 1) |
                (reference.kind == p29::RefKind::Block ? 1U : 0U);
            put_varint(root, tag);
        }
        require(!ZSTD_isError(
                    ZSTD_CCtx_reset(context.get(), ZSTD_reset_session_and_parameters)),
                "zstd context reset failed");
        require(!ZSTD_isError(ZSTD_CCtx_setParameter(
                    context.get(), ZSTD_c_compressionLevel, 3)),
                "zstd level setup failed");
        encoded.resize(ZSTD_compressBound(root.size()));
        const std::size_t encoded_size = ZSTD_compress2(
            context.get(), encoded.data(), encoded.size(),
            root.empty() ? static_cast<const void *>("") : root.data(),
            root.size());
        require(!ZSTD_isError(encoded_size),
                std::string("zstd ROOT compression failed: ") +
                    ZSTD_getErrorName(encoded_size));
        require(encoded_size <= std::numeric_limits<std::uint32_t>::max(),
                "ROOT frame exceeds u32 length");
        wire.push_back(1);
        for (unsigned byte = 0; byte != 4; ++byte)
            wire.push_back(static_cast<std::uint8_t>(encoded_size >> (8 * byte)));
        wire.insert(wire.end(), encoded.begin(), encoded.begin() + encoded_size);
    }
}

void write_file(const fs::path &path, std::span<const std::uint8_t> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        die("cannot write " + path.string());
    if (!bytes.empty())
        output.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    if (!output)
        die("short write to " + path.string());
}

struct Options {
    std::vector<fs::path> inputs;
    P29InternLayout layout = P29InternLayout::probe();
    std::uint64_t expected_regions = 126016;
    std::uint64_t expected_occurrences = 166258;
    std::uint64_t expected_lines = 303181;
    fs::path root_reference;
    fs::path root_output;
    double minimum_mib_per_second = 0;
    double maximum_seconds = 0;
};

[[nodiscard]] std::uint64_t number(const char *text, const char *name) {
    char *end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (!end || *end)
        die(std::string("bad ") + name);
    return value;
}

[[nodiscard]] Options options(int argc, char **argv) {
    Options result;
    result.inputs = phase0_inputs();
    result.root_reference = golden_root() / "research/phase0-11-cf.bin";
    for (int index = 1; index < argc; ++index) {
        const std::string_view option = argv[index];
        auto value = [&]() -> const char * {
            if (++index >= argc)
                die(std::string(option) + " needs a value");
            return argv[index];
        };
        if (option == "--manifest") {
            result.inputs = read_manifest(value());
            result.root_reference.clear();
        } else if (option == "--layout") {
            const std::string_view selected = value();
            if (selected == "probe")
                result.layout = P29InternLayout::probe();
            else if (selected == "sidecar")
                result.layout = P29InternLayout::sidecar();
            else
                die("layout must be probe or sidecar");
        } else if (option == "--expected-regions") {
            result.expected_regions = number(value(), "Region count");
        } else if (option == "--expected-occurrences") {
            result.expected_occurrences = number(value(), "occurrence count");
        } else if (option == "--expected-lines") {
            result.expected_lines = number(value(), "Line count");
        } else if (option == "--root-reference") {
            result.root_reference = value();
        } else if (option == "--root-output") {
            result.root_output = value();
        } else if (option == "--min-mib-s") {
            result.minimum_mib_per_second = std::stod(value());
        } else if (option == "--max-seconds") {
            result.maximum_seconds = std::stod(value());
        } else {
            die("unknown option " + std::string(option));
        }
    }
    require(!result.inputs.empty(), "input manifest is empty");
    return result;
}

struct CorpusResult {
    std::uint64_t raw_bytes = 0;
    std::uint64_t unique_raw_bytes = 0;
    std::size_t unique_tus = 0;
    std::vector<std::uint32_t> occurrences;
    std::vector<std::size_t> offsets{0};
};

CorpusResult intern_corpus(P29Interner<MmapProvider> &interner,
                           std::span<const fs::path> inputs) {
    CorpusResult result;
    std::unordered_map<std::string, std::size_t> by_path;
    std::vector<std::vector<std::uint32_t>> cached_regions;
    for (const fs::path &path : inputs) {
        const std::string key = path.string();
        auto found = by_path.find(key);
        if (found == by_path.end()) {
            const std::vector<std::uint8_t> input = read_file(path);
            const std::size_t cache_id = cached_regions.size();
            found = by_path.emplace(key, cache_id).first;
            cached_regions.emplace_back();
            interner.process(input, cached_regions.back());
            result.unique_raw_bytes += input.size();
            ++result.unique_tus;
        }
        const std::vector<std::uint32_t> &regions =
            cached_regions[found->second];
        const std::uintmax_t file_size = fs::file_size(path);
        result.raw_bytes += file_size;
        result.occurrences.insert(result.occurrences.end(), regions.begin(),
                                  regions.end());
        result.offsets.push_back(result.occurrences.size());
    }
    return result;
}

// The test-local executable specification for the region-boundary scan:
// the byte-at-a-time loop p29_intern.h's next_region() replaced with a
// memchr-driven scan.  Every randomized and edge-case input below must
// give the byte-identical result for both.
[[nodiscard]] const std::uint8_t *next_region_reference(
    const std::uint8_t *position, const std::uint8_t *end) {
    const std::uint8_t *scan = position + 1;
    while (scan + 1 < end) {
        if (*scan == '#' && scan[-1] == '\n' && scan[1] == ' ')
            return scan;
        ++scan;
    }
    return end;
}

void check_next_region_differential() {
    using icecc::codec::p29_intern_detail::next_region;
    // Fixed edge cases: no boundary, boundary at every offset from the
    // scan start, boundaries adjacent to both ends of the window (the
    // scan+1 < end cutoff), '#' with wrong neighbours ('#' without the
    // preceding newline, '#' followed by something else, "# " at the very
    // start), input too short for any candidate at all, and position at
    // the very end of the buffer.
    const std::vector<std::string> edges = {
        "",
        "#",
        "\n",
        "# ",
        "\n#",
        "\n# ",
        "x\n# ",
        "\n# x",
        "\n#\n",
        "a\n# b",
        "a\n# b\n# c",
        "# # # # ",
        "\n\n\n\n",
        "abc\n# def\nghi\n# jkl\n",
        "abc\n#def\nghi\n# jkl\n",
        "\n# \n# \n# ",
        "x\nx# y",
        "x# y",
    };
    for (const std::string &text : edges) {
        const std::uint8_t *data =
            reinterpret_cast<const std::uint8_t *>(text.data());
        const std::uint8_t *end = data + text.size();
        for (std::size_t start = 0; start <= text.size(); ++start) {
            const std::uint8_t *expect =
                next_region_reference(data + start, end);
            const std::uint8_t *got = next_region(data + start, end);
            require(expect == got,
                    "next_region edge case differs from the scalar reference: "
                    "input=\"" + text + "\" start=" + std::to_string(start) +
                    " expected=" + (expect == end ? std::string("end")
                                                  : std::to_string(expect - data)) +
                    " got=" + (got == end ? std::string("end")
                                          : std::to_string(got - data)));
        }
    }

    // Randomized inputs over a boundary-heavy alphabet, and every start
    // offset inside each: boundaries land at every possible window offset
    // including both edges.
    std::mt19937 rng(0xC0FFEEu);
    std::uniform_int_distribution<int> alphabet(0, 5);
    std::uniform_int_distribution<std::size_t> length_dist(0, 300);
    for (int trial = 0; trial != 20000; ++trial) {
        const std::size_t length = length_dist(rng);
        std::string text;
        text.reserve(length);
        for (std::size_t index = 0; index != length; ++index) {
            static constexpr char tokens[6] = {'\n', '#', ' ', 'x', 'y', 'z'};
            text.push_back(tokens[alphabet(rng)]);
        }
        const std::uint8_t *data =
            reinterpret_cast<const std::uint8_t *>(text.data());
        const std::uint8_t *end = data + text.size();
        std::uniform_int_distribution<std::size_t> start_dist(0, text.size());
        const std::size_t start = start_dist(rng);
        const std::uint8_t *expect = next_region_reference(data + start, end);
        const std::uint8_t *got = next_region(data + start, end);
        require(expect == got,
                "next_region random trial differs from the scalar reference: "
                "trial=" + std::to_string(trial) + " length=" +
                std::to_string(length) + " start=" + std::to_string(start));
    }

    std::cout << "codec_intern: next_region differential check PASS ("
              << edges.size() << " edge inputs x all starts + 20000 random "
              << "trials)\n";
}

// Differential identity for the two-phase intern: process() (the locked
// single-pass path) vs plan_regions() + process_planned() (the split path
// the client now takes).  On a FRESH interner pair fed the same TU
// sequence, every TU must produce the identical dense_regions, and the
// two interners must end with identical state (region count, distinct
// lines, occurrences, committed bytes).  Includes the stale-plan case
// (plan TU2/TU3 against a snapshot taken BEFORE TU1 commits) and TUs
// that repeat a brand-new region internally.
void check_process_planned_differential() {
    const auto make_interner = [] {
        P29InternLayout layout = small_layout(64);
        layout.region_capacity = 4096;
        layout.line_reference_capacity = 16384;
        layout.line_bytes_capacity = 1 << 18;
        layout.region_bytes_capacity = 1 << 18;
        layout.region_line_id_capacity = 1 << 14;
        auto provider = std::make_unique<MmapProvider>(
            independent_reservation(layout) + (1 << 20));
        auto interner = std::make_unique<P29Interner<MmapProvider>>(
            *provider, layout);
        return std::pair{std::move(interner), std::move(provider)};
    };

    // Synthetic linemarker-heavy corpus: shared regions across TUs, TUs
    // that repeat a new region internally, a TU with a trailing fragment,
    // and a TU with no boundary at all.
    const std::vector<std::string> tus = {
        "# 1 \"a\"\nalpha\n# 2 \"b\"\nbeta beta\n# 3 \"c\"\ngamma\n",
        "# 1 \"a\"\nalpha\n# 2 \"b\"\nbeta beta\n# 4 \"d\"\ndelta\n",
        // Repeats a brand-new region twice internally.
        "# 5 \"e\"\nepsilon\n# 5 \"e\"\nepsilon\n# 6 \"f\"\nzeta\n",
        // No linemarker at all: one whole-TU region.
        "just one region, no boundary inside\n",
        // Trailing fragment after the last boundary (region end == TU end).
        "# 7 \"g\"\neta tail without a final boundary marker\n",
        // A '#' that is not a boundary ('#' without preceding newline).
        "no newline before # here\n# 8 \"h\"\ntheta\n",
    };

    auto [plain_interner, plain_provider] = make_interner();
    auto [planned_interner, planned_provider] = make_interner();

    auto interned_state = [](P29Interner<MmapProvider> &interner) {
        std::string state;
        state += "regions=" + std::to_string(interner.distinct_regions());
        state += " lines=" + std::to_string(interner.distinct_lines());
        state += " occurrences=" +
                 std::to_string(interner.region_occurrences());
        state += " committed=" + std::to_string(interner.committed_bytes());
        return state;
    };

    for (const std::string &text : tus) {
        std::vector<std::uint32_t> plain_regions;
        plain_interner->process(bytes(text), plain_regions);
        // Fresh-plan the SAME input on the other interner: the plan is
        // built against the state before this TU is interned there.
        const std::vector<icecc::codec::P29RegionPlanEntry> plan =
            planned_interner->plan_regions(bytes(text));
        std::vector<std::uint32_t> planned_regions;
        planned_interner->process_planned(bytes(text), plan, planned_regions);
        require(plain_regions == planned_regions,
                "process_planned dense_regions differ from process() for "
                "input: \"" + text + "\"");
        require(interned_state(*plain_interner) ==
                    interned_state(*planned_interner),
                "interner state diverged between process() and "
                "process_planned() for input: \"" + text + "\" (" +
                    interned_state(*plain_interner) + " vs " +
                    interned_state(*planned_interner) + ")");
    }

    // Stale-plan case: plan TU2 and TU3 against a snapshot taken BEFORE
    // TU1 commits, then commit in order.  TU2's plan may say "miss" for
    // a region TU1 added meanwhile (the re-find under the writer lock
    // must then hit), and TU3's plan may say "hit" for a region that was
    // already published (still valid: records never move).
    auto [stale_interner, stale_provider] = make_interner();
    std::vector<std::uint32_t> stale_regions1;
    stale_interner->process(bytes(tus[0]), stale_regions1);
    // Snapshot plans for TU1(again) and TU2 against this state.
    const std::vector<icecc::codec::P29RegionPlanEntry> stale_plan_tu1 =
        stale_interner->plan_regions(bytes(tus[0]));
    const std::vector<icecc::codec::P29RegionPlanEntry> stale_plan_tu2 =
        stale_interner->plan_regions(bytes(tus[1]));
    // A DIFFERENT interner interns TU0..TU2 the plain way for reference.
    auto [ref_interner, ref_provider] = make_interner();
    std::vector<std::uint32_t> ref1, ref2;
    ref_interner->process(bytes(tus[0]), ref1);
    ref_interner->process(bytes(tus[1]), ref2);
    // Now consume the stale plans IN ORDER on the stale interner.
    std::vector<std::uint32_t> stale1, stale2;
    stale_interner->process_planned(bytes(tus[0]), stale_plan_tu1, stale1);
    stale_interner->process_planned(bytes(tus[1]), stale_plan_tu2, stale2);
    require(stale1 == ref1 && stale2 == ref2,
            "stale-plan process_planned output differs from the process() "
            "reference sequence");

    // The SAME TU re-fed twice through the split path (warm-hit plan).
    std::vector<std::uint32_t> warm1, warm2;
    const std::vector<icecc::codec::P29RegionPlanEntry> warm_plan =
        stale_interner->plan_regions(bytes(tus[2]));
    stale_interner->process_planned(bytes(tus[2]), warm_plan, warm1);
    const std::vector<icecc::codec::P29RegionPlanEntry> warm_plan2 =
        stale_interner->plan_regions(bytes(tus[2]));
    stale_interner->process_planned(bytes(tus[2]), warm_plan2, warm2);
    require(warm1 == warm2 && !warm1.empty(),
            "warm re-plan of the same TU differs");

    // The same differential over the REAL corpus inputs (when the source
    // tree is known): a fresh interner pair per arm, each TU processed by
    // process() on one and plan+process_planned on the other, both
    // interning the same sequence so the second TU onward exercises
    // cross-TU shared regions.  Only the differential is asserted here;
    // the frozen corpus counts are gated in main() as before.
    if (std::getenv("ICECC_TEST_TOP_SRCDIR") != nullptr) {
        const P29InternLayout real_layout = P29InternLayout::probe();
        for (const fs::path &input : phase0_inputs()) {
            if (!fs::exists(input))
                continue;
            auto real_plain_provider = std::make_unique<MmapProvider>(
                independent_reservation(real_layout));
            auto real_plain_interner =
                std::make_unique<P29Interner<MmapProvider>>(
                    *real_plain_provider, real_layout);
            auto real_planned_provider = std::make_unique<MmapProvider>(
                independent_reservation(real_layout));
            auto real_planned_interner =
                std::make_unique<P29Interner<MmapProvider>>(
                    *real_planned_provider, real_layout);
            const std::vector<std::uint8_t> corpus_input = read_file(input);
            std::vector<std::uint32_t> real_plain;
            real_plain_interner->process(corpus_input, real_plain);
            const std::vector<icecc::codec::P29RegionPlanEntry> real_plan =
                real_planned_interner->plan_regions(corpus_input);
            std::vector<std::uint32_t> real_planned;
            real_planned_interner->process_planned(corpus_input, real_plan,
                                                   real_planned);
            require(real_plain == real_planned,
                    "process_planned differs from process() on corpus input "
                    "with " + std::to_string(corpus_input.size()) +
                    " bytes");
        }
    }

    std::cout << "codec_intern: process_planned differential check PASS\n";
}


// Concurrency stress for the two-phase intern: the plan phase is lock-free
// and runs concurrently with the single writer's process_planned under an
// explicit mutex (exactly the production shape: plan outside the lock,
// write inside it).  N threads (>= 8) process distinct and OVERLAPPING TUs
// concurrently; every produced region id must materialize the exact input
// bytes through region_bytes() (the concurrent-read-safe accessor), and
// the whole run must leave the interner in a state whose region count is
// exactly the distinct regions of the corpus (same as a single-threaded
// process() of the same TUs, which the second interner computes).
void check_process_planned_concurrent() {
    const P29InternLayout layout = P29InternLayout::probe();
    MmapProvider provider(independent_reservation(layout));
    P29Interner<MmapProvider> interner(provider, layout);
    MmapProvider reference_provider(independent_reservation(layout));
    P29Interner<MmapProvider> reference_interner(reference_provider, layout);

    // Overlapping corpus: TUs 0..7 are distinct, and every thread ALSO
    // processes the SHARED TU (tus[0] reused per thread) plus its own
    // distinct TU, so plans overlap and the lock-free probes race with
    // adds of the very same content.
    std::vector<std::vector<std::uint8_t>> inputs;
    for (int index = 0; index != 12; ++index) {
        const std::string name = std::to_string(index);
        // Linemarker-heavy: two "# <n> \"x\"" boundaries per TU, with
        // per-TU region content so plans overlap across workers while
        // each TU still adds its own new regions.
        const std::string text =
            "# " + name + " \"r\"\nregion-" + name + "-aaaaaaaa\n" +
            "# " + name + " \"s\"\nshared-" + name + "\n";
        inputs.push_back(std::vector<uint8_t>(text.begin(), text.end()));
    }
    // Every thread also re-processes TU 0 (the overlap).
    inputs.push_back(inputs[0]);

    constexpr int kThreads = 8;
    std::mutex write_mutex;
    std::vector<std::thread> workers;
    std::atomic<int> failures{0};
    for (int worker = 0; worker != kThreads; ++worker) {
        workers.emplace_back([&, worker] {
            try {
                for (int round = 0; round != 8; ++round) {
                    const std::vector<std::uint8_t> &input =
                        inputs[(worker + round) % inputs.size()];
                    // Plan OUTSIDE the lock (concurrent with every other
                    // thread's writer phase).
                    const std::vector<icecc::codec::P29RegionPlanEntry> plan =
                        interner.plan_regions(input);
                    std::vector<std::uint32_t> regions;
                    {
                        std::lock_guard<std::mutex> lock(write_mutex);
                        interner.process_planned(input, plan, regions);
                    }
                    // Every produced TU must materialize exactly, through
                    // the concurrent-read-safe accessor, while other
                    // threads keep interning.
                    size_t offset = 0;
                    for (const std::uint32_t id : regions) {
                        const std::span<const std::uint8_t> region =
                            interner.region_bytes(id);
                        if (region.size() > input.size() - offset ||
                            std::memcmp(region.data(), input.data() + offset,
                                        region.size()) != 0)
                            throw std::runtime_error(
                                "concurrent region does not materialize");
                        offset += region.size();
                    }
                    if (offset != input.size())
                        throw std::runtime_error(
                            "concurrent materialization length differs");
                }
            } catch (const std::exception&) {
                ++failures;
            }
        });
    }

    // The single-threaded reference over the same corpus.
    std::vector<std::uint32_t> reference_regions;
    {
        std::vector<std::uint32_t> unused;
        for (const std::vector<std::uint8_t> &input : inputs)
            reference_interner.process(input, unused);
        (void)unused;
    }

    for (std::thread &worker : workers)
        worker.join();
    require(failures.load() == 0, "concurrent workers reported failures");
    require(interner.distinct_regions() > 0 &&
                interner.distinct_regions() ==
                    reference_interner.distinct_regions(),
            "concurrent interner region count differs from the "
            "single-threaded reference: " +
                std::to_string(interner.distinct_regions()) + " vs " +
                std::to_string(reference_interner.distinct_regions()));
    require(interner.distinct_lines() ==
                reference_interner.distinct_lines(),
            "concurrent interner line count differs from the "
            "single-threaded reference");
    std::cout << "codec_intern: process_planned concurrent check PASS ("
              << kThreads << " threads, " << inputs.size()
              << " overlapping TUs x 8 rounds)\n";
}


} // namespace

int main(int argc, char **argv) {
    try {
        check_match_extend_simd();
        allocation_controls();
        check_next_region_differential();
        check_process_planned_differential();
        check_process_planned_concurrent();
        const std::size_t committed_first = accounting_control_once();
        const std::size_t committed_second = accounting_control_once();
        require(committed_first == committed_second,
                "committed high-water is nondeterministic");
        mutation_and_capacity_controls();
        growth_controls();
        product_provider_control();

        const Options config = options(argc, argv);
        const std::size_t reservation = independent_reservation(config.layout);
        require(reservation ==
                    icecc::codec::p29_interner_reservation(config.layout),
                "corpus reservation differs from independent computation");
        MmapProvider provider(reservation);
        const auto start = Clock::now();
        P29Interner<MmapProvider> interner(provider, config.layout);
        CorpusResult corpus = intern_corpus(interner, config.inputs);
        const double seconds =
            std::chrono::duration<double>(Clock::now() - start).count();
        const double mib =
            double(corpus.raw_bytes) / double(std::size_t{1} << 20);

        require(config.minimum_mib_per_second == 0 ||
                    mib / seconds >= config.minimum_mib_per_second,
                "load+intern throughput is below the requested floor");
        require(config.maximum_seconds == 0 ||
                    seconds <= config.maximum_seconds,
                "load+intern time exceeds the requested ceiling");

        require(interner.distinct_regions() == config.expected_regions,
                "distinct Region count differs: got " +
                    std::to_string(interner.distinct_regions()));
        // The research loader interns each immutable pathname once, then
        // appends its cached Region sequence for every logical manifest row.
        // Thus the gate's occurrence count is the logical flattened stream;
        // interner.region_occurrences() is the separately reported physical
        // parse count when a manifest repeats paths.
        require(corpus.occurrences.size() == config.expected_occurrences,
                "logical Region occurrence count differs: got " +
                    std::to_string(corpus.occurrences.size()));
        require(interner.distinct_lines() == config.expected_lines,
                "distinct Line count differs: got " +
                    std::to_string(interner.distinct_lines()));
        require(provider.reported_reserved() == reservation &&
                    provider.reported_committed() == interner.committed_bytes(),
                "corpus provider accounting differs");

        rusage usage{};
        require(getrusage(RUSAGE_SELF, &usage) == 0, "getrusage failed");
        const std::uint64_t rss_bytes =
            static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
        require(interner.committed_bytes() <= rss_bytes,
                "interner committed accounting exceeds process max RSS");

        std::vector<std::uint8_t> roots;
        append_root_frames(corpus.occurrences, corpus.offsets, roots);
        if (!config.root_reference.empty()) {
            const std::vector<std::uint8_t> reference =
                read_file(config.root_reference);
            require(icecc::codec::test_golden::equivalent(
                        roots, reference, icecc::codec::P29WireKind::Root),
                    "ROOT decoded frames differ from retained research stream");
        }
        if (!config.root_output.empty())
            write_file(config.root_output, roots);

        std::cout << "codec_intern: PASS"
                  << " tus=" << config.inputs.size()
                  << " unique_tus=" << corpus.unique_tus
                  << " raw_bytes=" << corpus.raw_bytes
                  << " unique_raw_bytes=" << corpus.unique_raw_bytes
                  << " regions=" << interner.distinct_regions()
                  << " region_occurrences=" << corpus.occurrences.size()
                  << " physical_region_occurrences="
                  << interner.region_occurrences()
                  << " distinct_lines=" << interner.distinct_lines()
                  << " seconds=" << seconds << " MiB_s=" << (mib / seconds)
                  << " reserved_bytes=" << reservation
                  << " committed_bytes=" << interner.committed_bytes()
                  << " max_rss_bytes=" << rss_bytes
                  << " root_bytes=" << roots.size() << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "codec_intern: FAIL: " << error.what() << '\n';
        return 1;
    }
}
