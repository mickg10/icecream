#include "cache/codec/provider.h"
#include "cache/codec/tuples.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

namespace fs = std::filesystem;

class StubHistory {
  public:
    [[nodiscard]] std::span<const std::uint8_t> view() const { return bytes_; }

    void retain(std::span<const std::uint8_t> committed, std::uint64_t limit) {
        limit_ = limit;
        const std::size_t keep =
            static_cast<std::size_t>(std::min<std::uint64_t>(committed.size(), limit));
        bytes_.assign(committed.end() - keep, committed.end());
    }

    [[nodiscard]] std::uint64_t limit() const { return limit_; }

  private:
    std::vector<std::uint8_t> bytes_;
    std::uint64_t limit_ = 0;
};

class StubAnchors {
  public:
    explicit StubAnchors(std::size_t capacity = 64) : capacity_(capacity) {}

    [[nodiscard]] std::size_t capacity() const { return capacity_; }
    bool insert(std::uint64_t, std::uint64_t position) {
        if (positions_.size() == capacity_)
            return false;
        positions_.push_back(position);
        return true;
    }
    [[nodiscard]] std::span<const std::uint64_t> candidates(std::uint64_t) const {
        return positions_;
    }
    void rebuild(std::span<const std::uint8_t>, unsigned) { positions_.clear(); }
    void begin_provisional() {}
    void commit_provisional() {}
    void rollback_provisional() {}

  private:
    std::size_t capacity_;
    std::vector<std::uint64_t> positions_;
};

class StubObjects {
  public:
    std::uint32_t intern_line(std::span<const std::uint8_t> bytes) {
        lines_.emplace_back(bytes.begin(), bytes.end());
        return static_cast<std::uint32_t>(lines_.size() - 1);
    }
    std::uint32_t intern_region(std::span<const std::uint32_t> children) {
        regions_.emplace_back(children.begin(), children.end());
        return static_cast<std::uint32_t>(regions_.size() - 1);
    }
    [[nodiscard]] std::span<const std::uint8_t> line_bytes(std::uint32_t id) const {
        return lines_.at(id);
    }
    [[nodiscard]] std::span<const std::uint32_t> region_children(std::uint32_t id) const {
        return regions_.at(id);
    }
    [[nodiscard]] std::uint64_t distinct_lines() const { return lines_.size(); }

  private:
    std::vector<std::vector<std::uint8_t>> lines_;
    std::vector<std::vector<std::uint32_t>> regions_;
};

class ResearchLikeProvider {
  public:
    static constexpr unsigned max_threads = 8;
    [[noreturn]] static void fail(const char *reason) { throw std::runtime_error(reason); }
    StubHistory &history() { return history_; }
    StubAnchors &anchors() { return anchors_; }
    StubObjects &objects() { return objects_; }
    residual_group::Codec &residual();

  private:
    StubHistory history_;
    StubAnchors anchors_{1024};
    StubObjects objects_;
};

class ProductLikeProvider {
  public:
    static constexpr unsigned max_threads = 1;
    [[noreturn]] static void fail(const char *reason) { throw std::invalid_argument(reason); }
    StubHistory &history() { return history_; }
    StubAnchors &anchors() { return anchors_; }
    StubObjects &objects() { return objects_; }
    residual_group::Codec &residual();

  private:
    StubHistory history_;
    StubAnchors anchors_{4};
    StubObjects objects_;
};

class MissingRetainHistory {
  public:
    [[nodiscard]] std::span<const std::uint8_t> view() const { return {}; }
    [[nodiscard]] std::uint64_t limit() const { return 0; }
};

class MissingRetainProvider {
  public:
    static constexpr unsigned max_threads = 1;
    static void fail(const char *) {}
    MissingRetainHistory &history() { return history_; }
    StubAnchors &anchors() { return anchors_; }
    StubObjects &objects() { return objects_; }
    residual_group::Codec &residual();

  private:
    MissingRetainHistory history_;
    StubAnchors anchors_;
    StubObjects objects_;
};

static_assert(icecc::codec::ErrorPolicy<ResearchLikeProvider>);
static_assert(icecc::codec::HistoryProvider<StubHistory>);
static_assert(icecc::codec::AnchorIndexProvider<StubAnchors>);
static_assert(icecc::codec::ObjectStoreProvider<StubObjects>);
static_assert(icecc::codec::ResourceProvider<ResearchLikeProvider>);
static_assert(icecc::codec::ResourceProvider<ProductLikeProvider>);
static_assert(!icecc::codec::HistoryProvider<MissingRetainHistory>);

#if defined(ICECC_CODEC_BAD_PROVIDER_CONTROL)
// This assertion is the retained compile-failure control.  Building this file
// with ICECC_CODEC_BAD_PROVIDER_CONTROL must fail because retain() is absent.
static_assert(icecc::codec::ResourceProvider<MissingRetainProvider>);
#else
static_assert(!icecc::codec::ResourceProvider<MissingRetainProvider>);
#endif

template <class T> bool tuple_is(std::string_view id, const T &expected) {
    const icecc::codec::CodecTuple found = icecc::codec::tuple_by_id(id);
    const T *value = std::get_if<T>(&found);
    return value != nullptr && *value == expected;
}

fs::path golden_root() {
    if (const char *source = std::getenv("ICECC_TEST_TOP_SRCDIR"))
        return fs::path(source) / "unittests/codec_golden";
    return fs::path("codec_golden");
}

std::vector<std::string> grz_golden_options() {
    const fs::path path = golden_root() / "research/tuples.json";
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("cannot read " + path.string());
    const std::string json(std::istreambuf_iterator<char>(input), {});
    const std::size_t key = json.find("\"grz\"");
    const std::size_t begin = json.find('[', key);
    const std::size_t end = json.find(']', begin);
    if (key == std::string::npos || begin == std::string::npos ||
        end == std::string::npos)
        throw std::runtime_error(
            "research/tuples.json lacks the grz option array");

    std::vector<std::string> result;
    std::size_t position = begin + 1;
    while (position < end) {
        const std::size_t first = json.find('"', position);
        if (first == std::string::npos || first >= end)
            break;
        const std::size_t last = json.find('"', first + 1);
        if (last == std::string::npos || last > end)
            throw std::runtime_error(
                "research grz option is not a JSON string");
        result.push_back(json.substr(first + 1, last - first - 1));
        position = last + 1;
    }
    if (result.empty() || result.size() % 2 != 0)
        throw std::runtime_error(
            "research grz options are not name/value pairs");
    return result;
}

const std::string &option_value(const std::vector<std::string> &options,
                                std::string_view name) {
    for (std::size_t i = 0; i != options.size(); i += 2)
        if (options[i] == name)
            return options[i + 1];
    throw std::runtime_error("research grz options lack " + std::string(name));
}

bool has_enabled_option(const std::vector<std::string> &options,
                        std::string_view name) {
    for (std::size_t i = 0; i != options.size(); i += 2)
        if (options[i] == name)
            return std::stoull(options[i + 1]) != 0;
    return false;
}

std::uint64_t option_number(const std::vector<std::string> &options,
                            std::string_view name) {
    return std::stoull(option_value(options, name));
}

std::string_view grz_backend_name(std::uint64_t backend) {
    switch (backend) {
    case 4:
        return "BE_BSC_E0";
    case 5:
        return "BE_Z12";
    default:
        throw std::runtime_error(
            "research grz golden uses an unrecorded backend id");
    }
}

bool grz_tuple_matches_golden_options() {
    const std::vector<std::string> options = grz_golden_options();
    const icecc::codec::GrzTuple &tuple = icecc::codec::grz_research_g2;
    constexpr std::uint64_t mib = std::uint64_t{1} << 20;
    return option_value(options, "-m") == "g2" &&
           tuple.anchor_bytes == option_number(options, "-K") &&
           tuple.anchor_spacing_bits == option_number(options, "-s") &&
           tuple.index_capacity_or_budget == (std::uint64_t{2} << 30) &&
           tuple.index_bits == option_number(options, "-t") &&
           tuple.index_bits_cap == 26 &&
           tuple.group_bytes == option_number(options, "-b") * mib &&
           tuple.history_bytes == option_number(options, "--hist") * mib &&
           tuple.group_close_tus == option_number(options, "--gtu") &&
           tuple.group_close_raw_bytes ==
               option_number(options, "--graw") * mib &&
           tuple.group_close_add_bytes ==
               option_number(options, "--gadd") * mib &&
           tuple.backend_selection == has_enabled_option(options, "--select") &&
           tuple.literal_backend ==
               grz_backend_name(option_number(options, "-l")) &&
           tuple.token_backend ==
               grz_backend_name(option_number(options, "-k")) &&
           tuple.selection_candidates.empty();
}

} // namespace

int main() {
    bool ok = true;
    ok &= tuple_is(icecc::codec::zstd_route_product_v0.id, icecc::codec::zstd_route_product_v0);
    ok &=
        tuple_is(icecc::codec::zstd_route_research_thin.id, icecc::codec::zstd_route_research_thin);
    ok &= tuple_is(icecc::codec::zstd_route_research_ldm31.id,
                   icecc::codec::zstd_route_research_ldm31);
    ok &= tuple_is(icecc::codec::grz_product_v0.id, icecc::codec::grz_product_v0);
    ok &= tuple_is(icecc::codec::grz_research_g2.id, icecc::codec::grz_research_g2);
    ok &= tuple_is(icecc::codec::p29_product_v0.id, icecc::codec::p29_product_v0);
    ok &= tuple_is(icecc::codec::p29_wire_v1.id, icecc::codec::p29_wire_v1);

    try {
        (void)icecc::codec::tuple_by_id("no-such-tuple");
        std::cerr << "tuple_by_id accepted an unknown tuple\n";
        ok = false;
    } catch (const std::invalid_argument &) {
    }

    try {
        ProductLikeProvider::fail("product failure control");
    } catch (const std::invalid_argument &) {
    }

    try {
        if (!grz_tuple_matches_golden_options()) {
            std::cerr << "grz_research_g2 differs from research/tuples.json\n";
            ok = false;
        }
    } catch (const std::exception &error) {
        std::cerr << "grz_research_g2 golden control: " << error.what() << '\n';
        ok = false;
    }

    if (!ok)
        return 1;
    std::cout
        << "codec_provider: provider concepts, 7 tuple ids, and G2 golden "
           "tuple PASS\n";
    return 0;
}
