#include "cache/codec/p29_intern.h"
#include "cache/p50_slice0.h"
#include "capability/grouprlz/p29_online_s1.h"

#include <zstd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
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

[[nodiscard]] std::vector<std::uint8_t>
frames_of_kind(std::span<const std::uint8_t> wire, std::uint8_t wanted) {
    std::vector<std::uint8_t> result;
    std::size_t position = 0;
    while (position < wire.size()) {
        require(wire.size() - position >= 5, "wire stream has truncated header");
        const std::size_t frame_begin = position;
        const std::uint8_t kind = wire[position++];
        std::uint32_t length = 0;
        for (unsigned byte = 0; byte != 4; ++byte)
            length |= std::uint32_t(wire[position++]) << (8 * byte);
        require(length <= wire.size() - position,
                "wire stream has truncated payload");
        position += length;
        if (kind == wanted)
            result.insert(result.end(), wire.begin() + frame_begin,
                          wire.begin() + position);
    }
    return result;
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
            else if (selected == "firefox")
                result.layout = P29InternLayout::firefox();
            else
                die("layout must be probe or firefox");
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

} // namespace

int main(int argc, char **argv) {
    try {
        allocation_controls();
        const std::size_t committed_first = accounting_control_once();
        const std::size_t committed_second = accounting_control_once();
        require(committed_first == committed_second,
                "committed high-water is nondeterministic");
        mutation_and_capacity_controls();
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
            require(frames_of_kind(roots, 1) == frames_of_kind(reference, 1),
                    "ROOT frames differ from retained research stream");
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
