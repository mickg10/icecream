#include "cache/codec/p29_intern.h"
#include "cache/codec/p29_wire.h"
#include "cache/p50_input_buffer_pool.h"
#include "cache/p50_slice0.h"
#include "codec_golden_compare.h"

#include <algorithm>
#include <array>
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
using icecc::codec::P29Deserializer;
using icecc::codec::P29InternAllocation;
using icecc::codec::P29Interner;
using icecc::codec::P29InternLayout;
using icecc::codec::P29MixedCLineState;
using icecc::codec::P29MixedFLineView;
using icecc::codec::P29ReceiverRouteState;
using icecc::codec::P29SenderRouteState;
using icecc::codec::P29Serializer;
using icecc::codec::P29SourceTextView;
using icecc::codec::P29WireKind;
using icecc::codec::P29WireLimits;
namespace wire = icecc::codec::p29_wire_detail;

[[noreturn]] void die(const std::string &reason) {
  throw std::runtime_error(reason);
}

void require(bool condition, const std::string &reason) {
  if (!condition)
    die(reason);
}

[[nodiscard]] fs::path golden_root() {
  if (const char *top = std::getenv("ICECC_TEST_TOP_SRCDIR"))
    return fs::path(top) / "unittests/codec_golden";
  if (fs::exists("codec_golden"))
    return "codec_golden";
  return "unittests/codec_golden";
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

void write_file(const fs::path &path, std::span<const std::uint8_t> bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output)
    die("cannot write " + path.string());
  if (!bytes.empty())
    output.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  if (!output)
    die("short write to " + path.string());
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
  for (unsigned index = 0; index != 8; ++index)
    result.push_back(root / ("real-0" + std::to_string(index) + ".ii"));
  result.push_back(root / "synthetic-repeat.ii");
  result.push_back(root / "synthetic-unique.ii");
  result.push_back(root / "synthetic-blank-heavy.ii");
  return result;
}

class MmapInternProvider {
public:
  explicit MmapInternProvider(std::size_t budget) : budget_(budget) {}

  [[noreturn]] static void fail(const char *reason) {
    throw std::length_error(reason);
  }

  bool try_reserve_interner(std::size_t bytes) {
    if (bytes > budget_ - reserved_)
      return false;
    reserved_ += bytes;
    return true;
  }

  void *allocate_interner(P29InternAllocation, std::size_t bytes, std::size_t) {
    if (bytes > reserved_ - live_)
      return nullptr;
    void *result = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (result == MAP_FAILED)
      return nullptr;
#ifdef MADV_HUGEPAGE
    (void)madvise(result, bytes, MADV_HUGEPAGE);
#endif
    live_ += bytes;
    return result;
  }

  void deallocate_interner(P29InternAllocation, void *pointer,
                           std::size_t bytes, std::size_t) noexcept {
    if (pointer)
      (void)munmap(pointer, bytes);
    live_ -= bytes;
  }

  void release_interner_reservation(std::size_t bytes) noexcept {
    reserved_ -= bytes;
  }

  void report_interner_usage(std::size_t, std::size_t committed) {
    committed_ = committed;
  }

  void publish_line(std::uint32_t, std::span<const std::uint8_t>) {}
  void publish_region(std::uint32_t, std::span<const std::uint32_t>) {}

  [[nodiscard]] std::size_t committed() const { return committed_; }

private:
  std::size_t budget_ = 0;
  std::size_t reserved_ = 0;
  std::size_t live_ = 0;
  std::size_t committed_ = 0;
};

static_assert(icecc::codec::P29InternProvider<MmapInternProvider>);

struct SourceText {
  bool attempted = false;
  bool available = false;
  std::vector<std::uint8_t> bytes;
  std::vector<std::uint32_t> offsets;
};

template <bool Product> class WireProvider {
public:
  WireProvider()
      : arena_(Product ? std::make_unique<icecc::p50::CObjectArena>(
                             icecc::p50::CStoreGuid::from_u64(0x2901))
                       : nullptr) {
    // The retained research goldens were cut for a homogeneous pair.
    sender_.system_source_reuse = true;
    receiver_.system_source_reuse = true;
  }

  [[noreturn]] static void fail(const char *reason) {
    throw std::invalid_argument(reason);
  }

  P29SenderRouteState &sender_route() { return sender_; }
  P29ReceiverRouteState &receiver_route() { return receiver_; }
  P29WireLimits wire_limits() { return limits_; }
  P29WireLimits &mutable_wire_limits() { return limits_; }

  P29SourceTextView source_text(std::string_view path) {
    const std::string key(path);
    SourceText &source = sources_[key];
    if (!source.attempted) {
      source.attempted = true;
      source.offsets.push_back(0);
      if (icecc::codec::p29_wire_detail::system_source_path(path)) {
        std::error_code error;
        const std::uintmax_t size = fs::file_size(key, error);
        if (!error && size <= std::numeric_limits<std::uint32_t>::max()) {
          try {
            source.bytes = read_file(key);
            for (std::uint32_t index = 0; index < source.bytes.size(); ++index)
              if (source.bytes[index] == '\n')
                source.offsets.push_back(index + 1);
            if (source.offsets.back() != source.bytes.size())
              source.offsets.push_back(
                  static_cast<std::uint32_t>(source.bytes.size()));
            source.available = true;
          } catch (const std::exception &) {
            source.bytes.clear();
            source.offsets.resize(1);
          }
        }
      }
    }
    return {source.available, source.bytes, source.offsets};
  }

  void begin_wire_publish() {
    require(!publishing_, "wire publication already active");
    publishing_ = true;
    staged_regions_.clear();
    staged_blocks_.clear();
    staged_bytes_ = 0;
    staged_region_count_ = 0;
    staged_block_count_ = 0;
  }

  void publish_wire_region(std::uint32_t id,
                           std::span<const std::uint8_t> bytes) {
    require(publishing_, "wire Region publication is not active");
    account(bytes.size());
    ++staged_region_count_;
    if constexpr (Product)
      staged_regions_.push_back(
          {id, std::vector<std::uint8_t>(bytes.begin(), bytes.end())});
  }

  void publish_wire_block(std::uint32_t id,
                          std::span<const std::uint32_t> children) {
    require(publishing_, "wire Block publication is not active");
    account(children.size() * sizeof(std::uint32_t));
    ++staged_block_count_;
    if constexpr (Product)
      staged_blocks_.push_back(
          {id, std::vector<std::uint32_t>(children.begin(), children.end())});
  }

  void commit_wire_publish() {
    require(publishing_, "wire publication is not active");
    if constexpr (Product) {
      for (const PublishedRegion &region : staged_regions_) {
        if (region.id >= region_keys_.size())
          region_keys_.resize(std::size_t(region.id) + 1);
        require(!region_keys_[region.id].valid(),
                "product provider re-published a Region ordinal");
        const icecc::p50::Key64 material = arena_->intern_bytes(
            icecc::p50::ObjectType::Material, region.bytes);
        const std::array<icecc::p50::Key64, 1> children{material};
        region_keys_[region.id] =
            arena_->intern_children(icecc::p50::ObjectType::Region, children);
      }
      for (const PublishedBlock &block : staged_blocks_) {
        std::vector<icecc::p50::Key64> children;
        children.reserve(block.children.size());
        for (std::uint32_t id : block.children) {
          require(id < region_keys_.size() && region_keys_[id].valid(),
                  "product Block names an unpublished Region");
          children.push_back(region_keys_[id]);
        }
        if (block.id >= block_keys_.size())
          block_keys_.resize(std::size_t(block.id) + 1);
        require(!block_keys_[block.id].valid(),
                "product provider re-published a Block ordinal");
        block_keys_[block.id] =
            arena_->intern_children(icecc::p50::ObjectType::Block, children);
      }
    }
    published_regions_ += staged_region_count_;
    published_blocks_ += staged_block_count_;
    publishing_ = false;
    staged_regions_.clear();
    staged_blocks_.clear();
    staged_region_count_ = 0;
    staged_block_count_ = 0;
    staged_bytes_ = 0;
  }

  void abandon_wire_publish() noexcept {
    publishing_ = false;
    staged_regions_.clear();
    staged_blocks_.clear();
    staged_region_count_ = 0;
    staged_block_count_ = 0;
    staged_bytes_ = 0;
  }

  [[nodiscard]] std::size_t published_regions() const {
    return published_regions_;
  }
  [[nodiscard]] std::size_t published_blocks() const {
    return published_blocks_;
  }

  void set_source(std::string path, std::vector<std::uint8_t> bytes) {
    SourceText source;
    source.attempted = true;
    source.available = true;
    source.bytes = std::move(bytes);
    source.offsets.push_back(0);
    for (std::uint32_t index = 0; index < source.bytes.size(); ++index)
      if (source.bytes[index] == '\n')
        source.offsets.push_back(index + 1);
    if (source.offsets.back() != source.bytes.size())
      source.offsets.push_back(static_cast<std::uint32_t>(source.bytes.size()));
    sources_.insert_or_assign(std::move(path), std::move(source));
  }

private:
  struct PublishedRegion {
    std::uint32_t id = 0;
    std::vector<std::uint8_t> bytes;
  };
  struct PublishedBlock {
    std::uint32_t id = 0;
    std::vector<std::uint32_t> children;
  };

  void account(std::size_t bytes) {
    constexpr std::size_t publication_budget = std::size_t{4} << 30;
    require(bytes <= publication_budget - staged_bytes_,
            "wire publication exceeds its bounded staging budget");
    staged_bytes_ += bytes;
  }

  P29WireLimits limits_;
  P29SenderRouteState sender_;
  P29ReceiverRouteState receiver_;
  std::unordered_map<std::string, SourceText> sources_;
  std::unique_ptr<icecc::p50::CObjectArena> arena_;
  std::vector<icecc::p50::Key64> region_keys_;
  std::vector<icecc::p50::Key64> block_keys_;
  std::vector<PublishedRegion> staged_regions_;
  std::vector<PublishedBlock> staged_blocks_;
  std::size_t staged_bytes_ = 0;
  std::size_t staged_region_count_ = 0;
  std::size_t staged_block_count_ = 0;
  std::size_t published_regions_ = 0;
  std::size_t published_blocks_ = 0;
  bool publishing_ = false;
};

using ResearchProvider = WireProvider<false>;
using ProductProvider = WireProvider<true>;

static_assert(icecc::codec::P29SenderProvider<ResearchProvider>);
static_assert(icecc::codec::P29ReceiverProvider<ResearchProvider>);
static_assert(icecc::codec::P29SenderProvider<ProductProvider>);
static_assert(icecc::codec::P29ReceiverProvider<ProductProvider>);

// Adds the optional get_pooled_materialize_buffer() hook (see the SFINAE
// concept icecc::codec::p29_wire_detail::P29ReceiverProvidesPooledMaterializeBuffer
// in cache/codec/p29_wire.h) on top of ResearchProvider, so receive_fill()'s
// pool-consultation branch has something to call. Owns its own
// InputBufferPool instance -- never the process-wide
// InputBufferPool::instance() singleton -- so tests stay isolated from each
// other and from any other pool user in this binary.
class PooledResearchProvider : public ResearchProvider {
public:
  explicit PooledResearchProvider(std::uint64_t cap_bytes)
      : pool_(std::make_shared<icecc::p50::InputBufferPool>(cap_bytes)) {}

  [[nodiscard]] std::vector<std::uint8_t>
  get_pooled_materialize_buffer(std::size_t min_capacity) {
    return pool_->get(min_capacity);
  }

  [[nodiscard]] const std::shared_ptr<icecc::p50::InputBufferPool> &
  pool() const {
    return pool_;
  }

private:
  std::shared_ptr<icecc::p50::InputBufferPool> pool_;
};

static_assert(icecc::codec::P29ReceiverProvider<PooledResearchProvider>);
static_assert(icecc::codec::p29_wire_detail::
                  P29ReceiverProvidesPooledMaterializeBuffer<
                      PooledResearchProvider>);
static_assert(!icecc::codec::p29_wire_detail::
                   P29ReceiverProvidesPooledMaterializeBuffer<
                       ResearchProvider>);

struct LogicalTu {
  fs::path path;
  std::uint64_t raw_size = 0;
  std::shared_ptr<const std::vector<std::uint32_t>> regions;
};

struct Corpus {
  std::vector<LogicalTu> tus;
  std::uint64_t raw_bytes = 0;
  std::uint64_t unique_raw_bytes = 0;
  std::size_t unique_tus = 0;
};

Corpus load_and_intern(P29Interner<MmapInternProvider> &interner,
                       std::span<const fs::path> paths) {
  struct Cached {
    std::uint64_t raw_size = 0;
    std::shared_ptr<const std::vector<std::uint32_t>> regions;
  };
  Corpus result;
  result.tus.reserve(paths.size());
  std::unordered_map<std::string, Cached> cache;
  for (const fs::path &path : paths) {
    const std::string key = path.string();
    auto found = cache.find(key);
    if (found == cache.end()) {
      const std::vector<std::uint8_t> bytes = read_file(path);
      Cached cached;
      cached.raw_size = bytes.size();
      auto regions = std::make_shared<std::vector<std::uint32_t>>();
      interner.process(bytes, *regions);
      cached.regions = std::move(regions);
      result.unique_raw_bytes += bytes.size();
      ++result.unique_tus;
      found = cache.emplace(key, std::move(cached)).first;
    }
    result.raw_bytes += found->second.raw_size;
    result.tus.push_back({path, found->second.raw_size, found->second.regions});
  }
  return result;
}

template <class SenderProvider, class ReceiverProvider> struct DialogueResult {
  std::vector<std::uint8_t> cf;
  std::vector<std::uint8_t> fc;
  double sender_seconds = 0;
  double receiver_seconds = 0;
  double receiver_body_seconds = 0;
  double receiver_fill_seconds = 0;
  double receiver_verify_seconds = 0;
  double receiver_commit_seconds = 0;
  std::size_t published_regions = 0;
  std::size_t published_blocks = 0;
};

template <class SenderProvider, class ReceiverProvider>
DialogueResult<SenderProvider, ReceiverProvider>
run_dialogue(const Corpus &corpus,
             const P29Interner<MmapInternProvider> &interner) {
  SenderProvider sender_provider;
  ReceiverProvider receiver_provider;
  P29Serializer<SenderProvider, P29Interner<MmapInternProvider>> serializer(
      sender_provider, interner);
  P29Deserializer<ReceiverProvider> deserializer(receiver_provider);
  DialogueResult<SenderProvider, ReceiverProvider> result;
  for (std::size_t index = 0; index < corpus.tus.size(); ++index) {
    const bool close = index + 1 == corpus.tus.size();
    const LogicalTu &tu = corpus.tus[index];
    const std::vector<std::uint8_t> expected = read_file(tu.path);
    require(expected.size() == tu.raw_size,
            "P29 input size changed after interning at index " +
                std::to_string(index));
    auto start = Clock::now();
    const std::vector<std::uint8_t> body = serializer.begin_tu(*tu.regions);
    result.sender_seconds +=
        std::chrono::duration<double>(Clock::now() - start).count();

    start = Clock::now();
    const std::vector<std::uint8_t> need = deserializer.receive_body(body);
    const double body_seconds =
        std::chrono::duration<double>(Clock::now() - start).count();
    result.receiver_seconds += body_seconds;
    result.receiver_body_seconds += body_seconds;

    start = Clock::now();
    const std::vector<std::uint8_t> fill = serializer.answer_need(need, close);
    result.sender_seconds +=
        std::chrono::duration<double>(Clock::now() - start).count();

    start = Clock::now();
    (void)deserializer.receive_fill(fill, close);
    const std::vector<std::uint8_t> materialized =
        deserializer.take_materialized();
    const double fill_seconds =
        std::chrono::duration<double>(Clock::now() - start).count();
    result.receiver_fill_seconds += fill_seconds;
    start = Clock::now();
    require(materialized.size() == expected.size() &&
                std::equal(materialized.begin(), materialized.end(),
                           expected.begin()),
            "P29 materialized TU differs at index " + std::to_string(index));
    const double verify_seconds =
        std::chrono::duration<double>(Clock::now() - start).count();
    result.receiver_verify_seconds += verify_seconds;
    const std::vector<std::uint8_t> reverse_close =
        deserializer.captured_close();
    start = Clock::now();
    deserializer.commit();
    const double commit_seconds =
        std::chrono::duration<double>(Clock::now() - start).count();
    result.receiver_commit_seconds += commit_seconds;
    result.receiver_seconds += fill_seconds + verify_seconds + commit_seconds;
    serializer.commit();

    result.cf.insert(result.cf.end(), body.begin(), body.end());
    result.cf.insert(result.cf.end(), fill.begin(), fill.end());
    result.fc.insert(result.fc.end(), need.begin(), need.end());
    result.fc.insert(result.fc.end(), reverse_close.begin(),
                     reverse_close.end());

    const P29SenderRouteState &sender = sender_provider.sender_route();
    const P29ReceiverRouteState &receiver = receiver_provider.receiver_route();
    require(sender.paths == receiver.paths,
            "P29 C/F Path mirror differs after TU " + std::to_string(index));
    require(sender.next_public == receiver.public_lines.size(),
            "P29 C/F public-Line mirror differs after TU " +
                std::to_string(index));
    require(sender.known_region_count == receiver.known_region_count,
            "P29 C/F known-Region count differs after TU " +
                std::to_string(index));
    require(sender.known_block_count == receiver.known_block_count,
            "P29 C/F known-Block count differs after TU " +
                std::to_string(index));
    require(sender.revision == receiver.revision,
            "P29 C/F revision differs after TU " + std::to_string(index));
  }
  const P29SenderRouteState &sender = sender_provider.sender_route();
  const P29ReceiverRouteState &receiver = receiver_provider.receiver_route();
  for (std::size_t id = 0;
       id < std::max(sender.fknown_regions.size(), receiver.regions.size());
       ++id) {
    const bool c =
        id < sender.fknown_regions.size() && sender.fknown_regions[id];
    const bool f = id < receiver.regions.size() && receiver.regions[id].known;
    require(c == f, "P29 final C/F Region mirror differs");
  }
  for (std::size_t id = 0;
       id < std::max(sender.fknown_blocks.size(), receiver.blocks.size());
       ++id) {
    const bool c = id < sender.fknown_blocks.size() && sender.fknown_blocks[id];
    const bool f = id < receiver.blocks.size() && receiver.blocks[id].known;
    require(c == f, "P29 final C/F Block mirror differs");
  }
  for (std::uint32_t line = 1; line < sender.mixed_lines.size(); ++line) {
    const P29MixedCLineState &c = sender.mixed_lines[line];
    if (!c.public_id)
      continue;
    require(c.public_id < receiver.public_lines.size(),
            "P29 C public Line is absent on F");
    const P29MixedFLineView &f = receiver.public_lines[c.public_id];
    require(f.source_region == c.source_region &&
                f.source_offset == c.source_offset &&
                f.length == interner.line(line).size(),
            "P29 final C/F public-Line view differs");
  }
  result.published_regions = receiver_provider.published_regions();
  result.published_blocks = receiver_provider.published_blocks();
  return result;
}

[[nodiscard]] std::vector<std::uint8_t>
make_body(std::span<const std::uint8_t> root_raw,
          std::span<const std::uint8_t> block_raw = {}) {
  wire::MessageCodec messages;
  std::vector<std::uint8_t> result;
  const std::vector<std::uint8_t> root = messages.encode(root_raw);
  wire::append_frame(result, P29WireKind::Root, root);
  if (!block_raw.empty()) {
    const std::vector<std::uint8_t> blocks = messages.encode(block_raw);
    wire::append_frame(result, P29WireKind::BlockDefinition, blocks);
  }
  return result;
}

[[nodiscard]] std::vector<std::uint8_t>
single_region_body(std::uint32_t id = 0) {
  std::vector<std::uint8_t> root;
  wire::put_varint(root, wire::make_tag(id, false));
  return make_body(root);
}

// Same as make_fill(), but the two zstd streams stay OPEN across calls so a
// single P29Deserializer can consume several of these fills in sequence --
// exactly the production FILL-per-TU shape (only the route's last fill closes
// entropy). make_fill() itself ends each frame on every call, which is only
// consumable with close_entropy=true and therefore only usable for a
// deserializer's final TU.
class ContinuingFillBuilder {
public:
  [[nodiscard]] std::vector<std::uint8_t>
  make(std::span<const std::uint8_t> control_raw,
       std::span<const std::uint8_t> literal_raw, bool close) {
    const std::vector<std::uint8_t> control_encoded =
        control_.encode(control_raw, close);
    const std::vector<std::uint8_t> literal_encoded =
        literal_.encode(literal_raw, close);
    std::vector<std::uint8_t> result;
    std::uint8_t mask = 0;
    if (!control_encoded.empty()) {
      wire::append_frame(result, P29WireKind::FillControl, control_encoded);
      mask |= 1;
    }
    if (!literal_encoded.empty()) {
      wire::append_frame(result, P29WireKind::FillLiteral, literal_encoded);
      mask |= 2;
    }
    wire::append_frame(result, P29WireKind::TuEnd,
                       std::span<const std::uint8_t>(&mask, 1));
    return result;
  }

private:
  wire::ContinuingEncoder control_;
  wire::ContinuingEncoder literal_;
};

[[nodiscard]] std::vector<std::uint8_t>
make_fill(std::span<const std::uint8_t> control_raw,
          std::span<const std::uint8_t> literal_raw,
          std::span<const std::string> paths = {}) {
  wire::ContinuingEncoder control;
  wire::ContinuingEncoder literal;
  wire::MessageCodec messages;
  const std::vector<std::uint8_t> control_encoded =
      control.encode(control_raw, true);
  const std::vector<std::uint8_t> literal_encoded =
      literal.encode(literal_raw, true);
  std::vector<std::uint8_t> result;
  std::uint8_t mask = 0;
  if (!control_encoded.empty()) {
    wire::append_frame(result, P29WireKind::FillControl, control_encoded);
    mask |= 1;
  }
  if (!literal_encoded.empty()) {
    wire::append_frame(result, P29WireKind::FillLiteral, literal_encoded);
    mask |= 2;
  }
  if (!paths.empty()) {
    std::vector<std::uint8_t> path_raw;
    for (const std::string &path : paths) {
      wire::put_varint(path_raw, path.size());
      path_raw.insert(path_raw.end(), path.begin(), path.end());
    }
    const std::vector<std::uint8_t> encoded = messages.encode(path_raw);
    wire::append_frame(result, P29WireKind::PathDefinition, encoded);
  }
  wire::append_frame(result, P29WireKind::TuEnd,
                     std::span<const std::uint8_t>(&mask, 1));
  return result;
}

template <class Mutator>
[[nodiscard]] std::vector<std::uint8_t>
mutate_frame(std::span<const std::uint8_t> input, P29WireKind target,
             Mutator mutate) {
  const std::vector<wire::FrameView> frames = wire::parse_frames(input);
  std::vector<std::uint8_t> result;
  bool found = false;
  for (const wire::FrameView &frame : frames) {
    std::vector<std::uint8_t> payload(frame.payload.begin(),
                                      frame.payload.end());
    if (!found && frame.kind == target) {
      mutate(payload);
      found = true;
    }
    wire::append_frame(result, frame.kind, payload);
  }
  require(found, "wire mutation target frame is absent");
  return result;
}

[[nodiscard]] std::vector<std::uint8_t>
reorder_fill(std::span<const std::uint8_t> input) {
  const std::vector<wire::FrameView> frames = wire::parse_frames(input);
  require(frames.size() >= 3 && frames[0].kind == P29WireKind::FillControl &&
              frames[1].kind == P29WireKind::FillLiteral,
          "reorder control needs CTRL then LIT");
  std::vector<std::uint8_t> result;
  wire::append_frame(result, frames[1].kind, frames[1].payload);
  wire::append_frame(result, frames[0].kind, frames[0].payload);
  for (std::size_t index = 2; index < frames.size(); ++index)
    wire::append_frame(result, frames[index].kind, frames[index].payload);
  return result;
}

template <class Setup>
void expect_fill_failure(const std::vector<std::uint8_t> &body,
                         const std::vector<std::uint8_t> &fill,
                         std::string_view label, Setup setup) {
  ResearchProvider provider;
  setup(provider);
  const P29ReceiverRouteState before = provider.receiver_route();
  const std::size_t regions_before = provider.published_regions();
  const std::size_t blocks_before = provider.published_blocks();
  P29Deserializer<ResearchProvider> deserializer(provider);
  (void)deserializer.receive_body(body);
  bool rejected = false;
  try {
    (void)deserializer.receive_fill(fill, true);
  } catch (const std::exception &) {
    rejected = true;
  }
  require(rejected, std::string(label) + " was accepted");
  require(provider.receiver_route() == before,
          std::string(label) + " changed receiver state before abandon");
  if (deserializer.has_pending())
    deserializer.abandon();
  require(provider.receiver_route() == before &&
              provider.published_regions() == regions_before &&
              provider.published_blocks() == blocks_before,
          std::string(label) + " changed receiver state");
}

void expect_body_failure(const std::vector<std::uint8_t> &body,
                         std::string_view label) {
  ResearchProvider provider;
  const P29ReceiverRouteState before = provider.receiver_route();
  P29Deserializer<ResearchProvider> deserializer(provider);
  bool rejected = false;
  try {
    (void)deserializer.receive_body(body);
  } catch (const std::exception &) {
    rejected = true;
  }
  require(rejected, std::string(label) + " was accepted");
  require(!deserializer.has_pending() && provider.receiver_route() == before,
          std::string(label) + " changed receiver state");
}

[[nodiscard]] std::vector<std::uint8_t>
literal_control(std::uint64_t region_count, std::uint64_t raw_length,
                std::uint64_t literal_length) {
  std::vector<std::uint8_t> control;
  wire::put_varint(control, region_count);
  wire::put_varint(control, raw_length);
  control.push_back(0);
  wire::put_varint(control, literal_length);
  return control;
}

[[nodiscard]] std::vector<std::uint8_t>
source_control(std::uint8_t opcode, std::uint64_t raw_length,
               std::uint64_t source_line = 0) {
  std::vector<std::uint8_t> control;
  wire::put_varint(control, 1);
  wire::put_varint(control, raw_length);
  control.push_back(opcode);
  wire::put_varint(control, 0); // Path 0.
  wire::put_varint(control, source_line);
  if (opcode == 6) {
    wire::put_varint(control, 0);          // Prefix.
    wire::put_varint(control, 0);          // Suffix.
    wire::put_varint(control, raw_length); // Literal middle.
  }
  return control;
}

void run_wire_controls(const Corpus &corpus,
                       const P29Interner<MmapInternProvider> &interner) {
  require(!corpus.tus.empty() && !corpus.tus.front().regions->empty(),
          "wire controls need one Region");
  const auto no_setup = [](ResearchProvider &) {};
  const std::vector<std::uint8_t> body = single_region_body();
  const std::vector<std::uint8_t> control = literal_control(1, 3, 3);
  const std::array<std::uint8_t, 3> abc{'a', 'b', 'c'};
  const std::vector<std::uint8_t> literal_fill = make_fill(control, abc);

  const std::vector<std::uint8_t> bad_root = mutate_frame(
      body, P29WireKind::Root, [](std::vector<std::uint8_t> &payload) {
        require(!payload.empty(), "ROOT payload is empty");
        payload.front() ^= 0xff;
      });
  expect_body_failure(bad_root, "corrupt ROOT byte");
  const std::array<std::uint8_t, 2> nonminimal_root{0x80, 0x00};
  expect_body_failure(make_body(nonminimal_root), "non-minimal ROOT varint");
  {
    ResearchProvider provider;
    provider.mutable_wire_limits().max_tu_bytes = 0;
    const P29ReceiverRouteState before = provider.receiver_route();
    P29Deserializer<ResearchProvider> deserializer(provider);
    bool rejected = false;
    try {
      (void)deserializer.receive_body(body);
    } catch (const std::exception &) {
      rejected = true;
    }
    require(rejected && !deserializer.has_pending() &&
                provider.receiver_route() == before,
            "decompressed-message bound did not fail closed");
  }

  const auto truncate = [](std::vector<std::uint8_t> &payload) {
    require(!payload.empty(), "compressed mutation payload is empty");
    payload.pop_back();
  };
  expect_fill_failure(
      body, mutate_frame(literal_fill, P29WireKind::FillControl, truncate),
      "truncated FILL_CTRL", no_setup);
  expect_fill_failure(
      body, mutate_frame(literal_fill, P29WireKind::FillLiteral, truncate),
      "truncated FILL_LIT", no_setup);
  expect_fill_failure(body, reorder_fill(literal_fill), "reordered FILL frames",
                      no_setup);
  expect_fill_failure(
      body,
      mutate_frame(literal_fill, P29WireKind::FillControl,
                   [](std::vector<std::uint8_t> &payload) {
                     const std::vector<std::uint8_t> duplicate = payload;
                     payload.insert(payload.end(), duplicate.begin(),
                                    duplicate.end());
                   }),
      "concatenated continuing frames", no_setup);
  {
    ResearchProvider provider;
    const P29ReceiverRouteState before = provider.receiver_route();
    P29Deserializer<ResearchProvider> deserializer(provider);
    (void)deserializer.receive_body(body);
    bool rejected = false;
    try {
      (void)deserializer.receive_fill(literal_fill, false);
    } catch (const std::exception &) {
      rejected = true;
    }
    require(rejected && provider.receiver_route() == before,
            "premature continuing-stream close changed receiver state");
    deserializer.abandon();
  }
  {
    std::vector<std::uint8_t> excessive = body;
    wire::append_frame(excessive, P29WireKind::BlockDefinition, {});
    wire::append_frame(excessive, P29WireKind::Root, {});
    expect_body_failure(excessive, "excess BODY frame count");
  }
  {
    ResearchProvider provider;
    P29Deserializer<ResearchProvider> deserializer(provider);
    (void)deserializer.receive_body(body);
    (void)deserializer.receive_fill(literal_fill, true);
    require(deserializer.pending_segment_digest() == icecc::digest128(abc),
            "streamed Region-segment digest differs from exact bytes");
    deserializer.commit();
    const P29ReceiverRouteState committed = provider.receiver_route();
    bool rejected = false;
    try {
      (void)deserializer.receive_body(body);
    } catch (const std::exception &) {
      rejected = true;
    }
    require(rejected && !deserializer.has_pending() &&
                provider.receiver_route() == committed,
            "receiver accepted BODY after route entropy close");
  }
  {
    ResearchProvider provider;
    P29Deserializer<ResearchProvider> deserializer(provider);
    (void)deserializer.receive_body(body);
    std::vector<std::uint8_t> empty_control;
    wire::put_varint(empty_control, 1);
    wire::put_varint(empty_control, 0);
    const std::vector<std::uint8_t> empty_fill = make_fill(empty_control, {});
    require(deserializer.receive_fill(empty_fill, true).empty(),
            "empty Region materialized bytes");
    require(deserializer.pending_segment_digest() == icecc::Digest128{},
            "empty Region segment has a nonzero digest");
    deserializer.commit();
    require(provider.receiver_route().known_region_count == 1,
            "empty Region did not commit");
  }

  std::vector<std::uint8_t> opcode3;
  wire::put_varint(opcode3, 1);
  wire::put_varint(opcode3, 1);
  opcode3.push_back(3);
  expect_fill_failure(body, make_fill(opcode3, {}), "opcode 3", no_setup);

  const std::array<std::string, 1> relative_path{"relative/p29.h"};
  expect_fill_failure(body, make_fill(source_control(5, 1), {}, relative_path),
                      "opcode 5 non-system Path", no_setup);
  const std::array<std::uint8_t, 1> x{'x'};
  expect_fill_failure(body, make_fill(source_control(6, 1), x, relative_path),
                      "opcode 6 non-system Path", no_setup);

  const std::string missing_path =
      "/usr/include/p29-wire-v1-definitely-absent.h";
  const std::array<std::string, 1> missing_paths{missing_path};
  expect_fill_failure(body, make_fill(source_control(5, 6), {}, missing_paths),
                      "absent system source", no_setup);

  const std::string system_path = "/usr/include/p29-wire-v1-control.h";
  const std::array<std::string, 1> system_paths{system_path};
  const std::array<std::uint8_t, 6> expected_source{'r', 'i', 'g',
                                                    'h', 't', '\n'};
  const std::array<std::uint8_t, 6> altered_source{'w', 'r', 'o',
                                                   'n', 'g', '\n'};
  {
    ResearchProvider provider;
    provider.set_source(system_path,
                        {altered_source.begin(), altered_source.end()});
    const P29ReceiverRouteState before = provider.receiver_route();
    P29Deserializer<ResearchProvider> deserializer(provider);
    (void)deserializer.receive_body(body);
    const std::vector<std::uint8_t> source_fill =
        make_fill(source_control(5, expected_source.size()), {}, system_paths);
    const std::span<const std::uint8_t> materialized =
        deserializer.receive_fill(source_fill, true);
    require(!std::equal(materialized.begin(), materialized.end(),
                        expected_source.begin(), expected_source.end()),
            "altered system source accidentally matched expected output");
    // Pair-level raw-digest verification rejects this successful decode;
    // abandoning must leave the provider exactly unchanged.
    deserializer.abandon();
    require(provider.receiver_route() == before &&
                provider.published_regions() == 0,
            "altered system-source rejection changed receiver state");
  }
  {
    ResearchProvider provider;
    provider.set_source(system_path,
                        {expected_source.begin(), expected_source.end()});
    P29Deserializer<ResearchProvider> deserializer(provider);
    (void)deserializer.receive_body(body);
    const std::vector<std::uint8_t> source_fill =
        make_fill(source_control(5, expected_source.size()), {}, system_paths);
    const std::span<const std::uint8_t> materialized =
        deserializer.receive_fill(source_fill, true);
    require(materialized.size() == expected_source.size() &&
                std::equal(materialized.begin(), materialized.end(),
                           expected_source.begin()),
            "valid system-source Line did not materialize exactly");
    deserializer.commit();
    require(provider.receiver_route().known_region_count == 1,
            "valid system-source Line did not commit");
  }
  const auto valid_source_setup = [&](ResearchProvider &provider) {
    provider.set_source(system_path,
                        std::vector<std::uint8_t>(expected_source.begin(),
                                                  expected_source.end()));
  };
  expect_fill_failure(
      body, make_fill(source_control(5, expected_source.size()), {}, system_paths),
      "system-source Line without negotiated reuse",
      [&](ResearchProvider &provider) {
        valid_source_setup(provider);
        provider.receiver_route().system_source_reuse = false;
      });
  expect_fill_failure(body,
                      make_fill(source_control(5, 1, 99), {}, system_paths),
                      "out-of-range system-source Line", valid_source_setup);
  expect_fill_failure(body,
                      make_fill(source_control(6, 1, 99), x, system_paths),
                      "out-of-range system-source patch", valid_source_setup);

  expect_fill_failure(body, make_fill({}, {}),
                      "undefined Root Region at TU_END", no_setup);
  expect_fill_failure(body, make_fill(literal_control(2, 3, 3), abc),
                      "FILL for an unrequested Region", no_setup);

  std::vector<std::uint8_t> block_root;
  wire::put_varint(block_root, wire::make_tag(0, true));
  std::vector<std::uint8_t> block_definition;
  wire::put_varint(block_definition, 1); // Definition count.
  wire::put_varint(block_definition, 0); // Block 0.
  block_definition.push_back(0);         // Explicit child list.
  wire::put_varint(block_definition, 1);
  wire::put_varint(block_definition, 1); // Unknown Region 1.
  const std::vector<std::uint8_t> block_body =
      make_body(block_root, block_definition);
  expect_fill_failure(block_body, make_fill({}, {}),
                      "BLOCKDEF unknown child at TU_END", no_setup);
  {
    ResearchProvider provider;
    P29Deserializer<ResearchProvider> first(provider);
    (void)first.receive_body(block_body);
    const std::span<const std::uint8_t> materialized =
        first.receive_fill(literal_fill, true);
    require(
        materialized.size() == abc.size() &&
            std::equal(materialized.begin(), materialized.end(), abc.begin()),
        "manifest-resend control setup did not materialize");
    first.commit();
    const P29ReceiverRouteState committed = provider.receiver_route();
    P29Deserializer<ResearchProvider> replay(provider);
    bool rejected = false;
    try {
      (void)replay.receive_body(block_body);
    } catch (const std::exception &) {
      rejected = true;
    }
    require(rejected && !replay.has_pending() &&
                provider.receiver_route() == committed,
            "known BLOCKDEF manifest resend was not rejected");
  }

  const std::uint32_t region_id = corpus.tus.front().regions->front();
  const std::array<std::uint32_t, 1> one_region{region_id};
  const std::span<const std::uint8_t> expected_region =
      interner.region_bytes(region_id);

  // A fully decoded attempt may be abandoned.  Both route states remain
  // byte-for-byte unchanged and the resend uses fresh, identical zstd frames.
  {
    ResearchProvider sender_provider;
    ResearchProvider receiver_provider;
    const P29SenderRouteState sender_before = sender_provider.sender_route();
    const P29ReceiverRouteState receiver_before =
        receiver_provider.receiver_route();
    P29Serializer<ResearchProvider, P29Interner<MmapInternProvider>> serializer(
        sender_provider, interner);
    P29Deserializer<ResearchProvider> deserializer(receiver_provider);

    const std::vector<std::uint8_t> body1 = serializer.begin_tu(one_region);
    const std::vector<std::uint8_t> need1 = deserializer.receive_body(body1);
    const std::vector<std::uint8_t> fill1 = serializer.answer_need(need1, true);
    const std::span<const std::uint8_t> first =
        deserializer.receive_fill(fill1, true);
    require(first.size() == expected_region.size() &&
                std::equal(first.begin(), first.end(), expected_region.begin()),
            "abandon control first materialization differs");
    deserializer.abandon();
    serializer.abandon();
    require(sender_provider.sender_route() == sender_before &&
                receiver_provider.receiver_route() == receiver_before,
            "abandon changed a provider route state");

    const std::vector<std::uint8_t> body2 = serializer.begin_tu(one_region);
    const std::vector<std::uint8_t> need2 = deserializer.receive_body(body2);
    const std::vector<std::uint8_t> fill2 = serializer.answer_need(need2, true);
    const std::span<const std::uint8_t> second =
        deserializer.receive_fill(fill2, true);
    require(body1 == body2 && need1 == need2 && fill1 == fill2,
            "abandon/resend did not reproduce fresh attempt frames");
    require(
        second.size() == expected_region.size() &&
            std::equal(second.begin(), second.end(), expected_region.begin()),
        "abandon/resend materialization differs");
    deserializer.commit();
    serializer.commit();
    require(sender_provider.sender_route().revision == 1 &&
                receiver_provider.receiver_route().revision == 1 &&
                sender_provider.sender_route().known_region_count ==
                    receiver_provider.receiver_route().known_region_count,
            "abandon/resend commit mirror differs");
  }

  // Reset is also correct after committed entropy history: the abandoned
  // segment used that history, while the retry starts a fresh zstd frame on
  // both sides and retains the already committed dictionary state.
  {
    std::uint32_t next_region = region_id;
    for (const LogicalTu &tu : corpus.tus) {
      const auto found = std::find_if(
          tu.regions->begin(), tu.regions->end(),
          [&](std::uint32_t candidate) { return candidate != region_id; });
      if (found != tu.regions->end()) {
        next_region = *found;
        break;
      }
    }
    require(next_region != region_id,
            "history-abandon control needs two Regions");
    const std::array<std::uint32_t, 1> next_one_region{next_region};
    const std::span<const std::uint8_t> next_expected =
        interner.region_bytes(next_region);
    ResearchProvider sender_provider;
    ResearchProvider receiver_provider;
    P29Serializer<ResearchProvider, P29Interner<MmapInternProvider>> serializer(
        sender_provider, interner);
    P29Deserializer<ResearchProvider> deserializer(receiver_provider);

    const std::vector<std::uint8_t> initial_body =
        serializer.begin_tu(one_region);
    const std::vector<std::uint8_t> initial_need =
        deserializer.receive_body(initial_body);
    const std::vector<std::uint8_t> initial_fill =
        serializer.answer_need(initial_need, false);
    (void)deserializer.receive_fill(initial_fill, false);
    deserializer.commit();
    serializer.commit();
    const P29SenderRouteState sender_committed = sender_provider.sender_route();
    const P29ReceiverRouteState receiver_committed =
        receiver_provider.receiver_route();

    const std::vector<std::uint8_t> body1 =
        serializer.begin_tu(next_one_region);
    const std::vector<std::uint8_t> need1 = deserializer.receive_body(body1);
    const std::vector<std::uint8_t> fill1 = serializer.answer_need(need1, true);
    const std::span<const std::uint8_t> first =
        deserializer.receive_fill(fill1, true);
    require(first.size() == next_expected.size() &&
                std::equal(first.begin(), first.end(), next_expected.begin()),
            "history-abandon first materialization differs");
    deserializer.abandon();
    serializer.abandon();
    require(sender_provider.sender_route() == sender_committed &&
                receiver_provider.receiver_route() == receiver_committed,
            "history-abandon changed committed provider state");

    const std::vector<std::uint8_t> body2 =
        serializer.begin_tu(next_one_region);
    const std::vector<std::uint8_t> need2 = deserializer.receive_body(body2);
    const std::vector<std::uint8_t> fill2 = serializer.answer_need(need2, true);
    const std::span<const std::uint8_t> second =
        deserializer.receive_fill(fill2, true);
    require(body1 == body2 && need1 == need2,
            "history-abandon changed independent retry frames");
    require(second.size() == next_expected.size() &&
                std::equal(second.begin(), second.end(), next_expected.begin()),
            "history-abandon retry materialization differs");
    deserializer.commit();
    serializer.commit();
    require(sender_provider.sender_route().revision == 2 &&
                receiver_provider.receiver_route().revision == 2 &&
                sender_provider.sender_route().known_region_count ==
                    receiver_provider.receiver_route().known_region_count,
            "history-abandon retry commit mirror differs");
  }

  // Both sides refuse a commit if their provider-owned redo base changes.
  {
    ResearchProvider sender_provider;
    ResearchProvider receiver_provider;
    const P29SenderRouteState sender_before = sender_provider.sender_route();
    const P29ReceiverRouteState receiver_before =
        receiver_provider.receiver_route();
    P29Serializer<ResearchProvider, P29Interner<MmapInternProvider>> serializer(
        sender_provider, interner);
    P29Deserializer<ResearchProvider> deserializer(receiver_provider);
    const std::vector<std::uint8_t> attempt_body =
        serializer.begin_tu(one_region);
    const std::vector<std::uint8_t> attempt_need =
        deserializer.receive_body(attempt_body);
    const std::vector<std::uint8_t> attempt_fill =
        serializer.answer_need(attempt_need, true);
    (void)deserializer.receive_fill(attempt_fill, true);

    ++receiver_provider.receiver_route().revision;
    bool receiver_rejected = false;
    try {
      deserializer.commit();
    } catch (const std::exception &) {
      receiver_rejected = true;
    }
    --receiver_provider.receiver_route().revision;
    require(receiver_rejected &&
                receiver_provider.receiver_route() == receiver_before,
            "receiver accepted a mismatched redo base");
    deserializer.abandon();

    ++sender_provider.sender_route().revision;
    bool sender_rejected = false;
    try {
      serializer.commit();
    } catch (const std::exception &) {
      sender_rejected = true;
    }
    --sender_provider.sender_route().revision;
    require(sender_rejected && sender_provider.sender_route() == sender_before,
            "sender accepted a mismatched redo base");
    serializer.abandon();
  }

  // Once a Region is known, a one-Region TU requests no fill.  Any unsolicited
  // NEED must be rejected without moving either committed route state.
  {
    ResearchProvider sender_provider;
    ResearchProvider receiver_provider;
    P29Serializer<ResearchProvider, P29Interner<MmapInternProvider>> serializer(
        sender_provider, interner);
    P29Deserializer<ResearchProvider> deserializer(receiver_provider);
    const std::vector<std::uint8_t> first_body =
        serializer.begin_tu(one_region);
    const std::vector<std::uint8_t> first_need =
        deserializer.receive_body(first_body);
    const std::vector<std::uint8_t> first_fill =
        serializer.answer_need(first_need, false);
    (void)deserializer.receive_fill(first_fill, false);
    deserializer.commit();
    serializer.commit();

    const P29SenderRouteState sender_before = sender_provider.sender_route();
    const P29ReceiverRouteState receiver_before =
        receiver_provider.receiver_route();
    const std::vector<std::uint8_t> second_body =
        serializer.begin_tu(one_region);
    const std::vector<std::uint8_t> second_need =
        deserializer.receive_body(second_body);
    require(second_need.empty(), "known one-Region TU unexpectedly needs data");
    const std::array<std::uint8_t, 1> unsolicited{0};
    bool rejected = false;
    try {
      (void)serializer.answer_need(unsolicited, false);
    } catch (const std::exception &) {
      rejected = true;
    }
    require(rejected && sender_provider.sender_route() == sender_before &&
                receiver_provider.receiver_route() == receiver_before,
            "unsolicited NEED changed provider state");
    deserializer.abandon();
    serializer.abandon();
  }

  // A FILL built before NEED is the FILL that answering NEED builds, and the
  // NEED it is later confirmed against must still match exactly.
  {
    ResearchProvider early_sender;
    ResearchProvider early_receiver;
    ResearchProvider answered_sender;
    ResearchProvider answered_receiver;
    P29Serializer<ResearchProvider, P29Interner<MmapInternProvider>> early(
        early_sender, interner);
    P29Serializer<ResearchProvider, P29Interner<MmapInternProvider>> answered(
        answered_sender, interner);
    P29Deserializer<ResearchProvider> early_f(early_receiver);
    P29Deserializer<ResearchProvider> answered_f(answered_receiver);
    const std::vector<std::uint8_t> early_need =
        early_f.receive_body(early.begin_tu(one_region));
    const std::vector<std::uint8_t> answered_need =
        answered_f.receive_body(answered.begin_tu(one_region));
    const std::vector<std::uint8_t> early_fill = early.fill_before_need();
    require(!early_need.empty() &&
                early_fill == answered.answer_need(answered_need, false),
            "FILL built before NEED differs from the answered FILL");
    bool rejected = false;
    try {
      early.confirm_need({});
    } catch (const std::exception &) {
      rejected = true;
    }
    require(rejected, "sender confirmed a NEED that differs from its own");
    early.confirm_need(early_need);
    (void)early_f.receive_fill(early_fill, false);
    early_f.commit();
    early.commit();
    answered_f.abandon();
    answered.abandon();
  }

  // Errata (2026-10-08, P50_ROUTE_PIPELINING.md Step 2): P29Serializer stays
  // scalar.  A second begin_tu() before the first TU's commit()/abandon()
  // must still fail closed, and the still-pending TU must remain
  // completable afterward, untouched by the rejected re-entrant attempt.
  {
    ResearchProvider sender_provider;
    ResearchProvider receiver_provider;
    const P29SenderRouteState sender_before = sender_provider.sender_route();
    P29Serializer<ResearchProvider, P29Interner<MmapInternProvider>> serializer(
        sender_provider, interner);
    P29Deserializer<ResearchProvider> deserializer(receiver_provider);

    const std::vector<std::uint8_t> pending_body =
        serializer.begin_tu(one_region);
    bool rejected = false;
    std::string reason;
    try {
      (void)serializer.begin_tu(one_region);
    } catch (const std::exception &error) {
      rejected = true;
      reason = error.what();
    }
    require(rejected && reason == "P29 serializer already has a pending TU",
            "re-entrant begin_tu did not fail closed with the expected "
            "reason");
    require(serializer.has_pending() &&
                sender_provider.sender_route() == sender_before,
            "rejected re-entrant begin_tu disturbed the still-pending TU");

    const std::vector<std::uint8_t> pending_need =
        deserializer.receive_body(pending_body);
    const std::vector<std::uint8_t> pending_fill =
        serializer.answer_need(pending_need, false);
    const std::span<const std::uint8_t> materialized =
        deserializer.receive_fill(pending_fill, false);
    require(materialized.size() == expected_region.size() &&
                std::equal(materialized.begin(), materialized.end(),
                           expected_region.begin()),
            "TU pending across a rejected re-entrant begin_tu did not "
            "complete normally");
    deserializer.commit();
    serializer.commit();
  }

  // Errata (2026-10-08, P50_ROUTE_PIPELINING.md Step 2/3): under K>1,
  // P29Serializer::commit() moves to run locally right after the FILL write
  // completes ("eager"), instead of waiting for the receiver's own commit()
  // ("deferred", today's K=1 ack-gated timing).  Invariant 3 claims this is
  // only a change in *when* commit() runs, never in *what* it computes: the
  // same TU sequence must produce byte-identical wire frames and an
  // identical post-commit sender route state under either timing.
  {
    std::vector<std::uint32_t> sequence;
    for (const LogicalTu &tu : corpus.tus) {
      for (std::uint32_t region : *tu.regions) {
        if (sequence.empty() || sequence.back() != region)
          sequence.push_back(region);
        if (sequence.size() >= 4)
          break;
      }
      if (sequence.size() >= 4)
        break;
    }
    require(sequence.size() >= 2,
            "commit-timing equivalence needs at least two distinct Regions");

    const auto run_sequence =
        [&](bool eager, std::vector<std::uint8_t> &trace,
            P29SenderRouteState &final_sender_state) {
          ResearchProvider sender_provider;
          ResearchProvider receiver_provider;
          P29Serializer<ResearchProvider, P29Interner<MmapInternProvider>>
              seq_serializer(sender_provider, interner);
          P29Deserializer<ResearchProvider> seq_deserializer(
              receiver_provider);
          for (std::uint32_t region : sequence) {
            const std::array<std::uint32_t, 1> one{region};
            const std::vector<std::uint8_t> tu_body =
                seq_serializer.begin_tu(one);
            const std::vector<std::uint8_t> tu_need =
                seq_deserializer.receive_body(tu_body);
            const std::vector<std::uint8_t> tu_fill =
                seq_serializer.answer_need(tu_need, false);
            const std::span<const std::uint8_t> tu_materialized =
                seq_deserializer.receive_fill(tu_fill, false);
            trace.insert(trace.end(), tu_body.begin(), tu_body.end());
            trace.insert(trace.end(), tu_need.begin(), tu_need.end());
            trace.insert(trace.end(), tu_fill.begin(), tu_fill.end());
            trace.insert(trace.end(), tu_materialized.begin(),
                         tu_materialized.end());
            if (eager) {
              // K>1 timing: local commit right after the FILL write,
              // feeding predicted_cursor_ ahead of the receiver's ack.
              seq_serializer.commit();
              seq_deserializer.commit();
            } else {
              // K=1 timing: unchanged, commit only after the
              // ack-equivalent point (the receiver's own commit()).
              seq_deserializer.commit();
              seq_serializer.commit();
            }
          }
          final_sender_state = sender_provider.sender_route();
        };

    std::vector<std::uint8_t> deferred_trace;
    P29SenderRouteState deferred_state;
    run_sequence(false, deferred_trace, deferred_state);
    std::vector<std::uint8_t> eager_trace;
    P29SenderRouteState eager_state;
    run_sequence(true, eager_trace, eager_state);
    require(deferred_trace == eager_trace,
            "eager commit timing produced different wire frames");
    require(deferred_state == eager_state,
            "eager commit timing produced a different post-commit sender "
            "route state");
  }
}

void run_golden_comparison_controls() {
  wire::MessageCodec messages;
  wire::ContinuingEncoder control;
  const std::array<std::uint8_t, 3> root_raw{1, 2, 3};
  const std::array<std::uint8_t, 2> control_first{4, 5};
  const std::array<std::uint8_t, 2> control_last{6, 7};
  std::vector<std::uint8_t> original;
  wire::append_frame(original, P29WireKind::Root, messages.encode(root_raw));
  wire::append_frame(original, P29WireKind::FillControl,
                     control.encode(control_first, false));
  wire::append_frame(original, P29WireKind::FillControl,
                     control.encode(control_last, true));
  wire::append_frame(original, P29WireKind::TuEnd, {});
  require(icecc::codec::test_golden::equivalent(original, original),
          "golden semantic comparison rejected identical frames");

  auto rebuild = [](const std::vector<wire::FrameView> &frames) {
    std::vector<std::uint8_t> bytes;
    for (const wire::FrameView &frame : frames)
      wire::append_frame(bytes, frame.kind, frame.payload);
    return bytes;
  };
  const auto frames = wire::parse_frames(original);
  {
    std::vector<wire::FrameView> changed = frames;
    changed[0].kind = P29WireKind::BlockDefinition;
    require(!icecc::codec::test_golden::equivalent(original, rebuild(changed)),
            "golden semantic comparison accepted altered frame kind");
  }
  {
    std::vector<wire::FrameView> changed = frames;
    std::swap(changed[0], changed[1]);
    require(!icecc::codec::test_golden::equivalent(original, rebuild(changed)),
            "golden semantic comparison accepted reordered frames");
  }
  {
    std::vector<std::uint8_t> changed;
    auto raw = messages.decode(frames[0].payload);
    raw[0] ^= 0xff;
    wire::append_frame(changed, P29WireKind::Root, messages.encode(raw));
    for (std::size_t i = 1; i < frames.size(); ++i)
      wire::append_frame(changed, frames[i].kind, frames[i].payload);
    require(!icecc::codec::test_golden::equivalent(original, changed),
            "golden semantic comparison accepted changed decoded byte");
  }
  {
    std::vector<std::uint8_t> changed;
    wire::append_frame(changed, P29WireKind::Root, frames[0].payload);
    std::vector<std::uint8_t> merged(frames[1].payload.begin(),
                                     frames[1].payload.end());
    merged.insert(merged.end(), frames[2].payload.begin(), frames[2].payload.end());
    wire::append_frame(changed, P29WireKind::FillControl, merged);
    wire::append_frame(changed, P29WireKind::TuEnd, {});
    require(!icecc::codec::test_golden::equivalent(original, changed),
            "golden semantic comparison accepted changed frame boundaries");
  }
  {
    std::vector<std::uint8_t> truncated = original;
    truncated.pop_back();
    require(!icecc::codec::test_golden::equivalent(original, truncated),
            "golden semantic comparison accepted truncation");
  }
  {
    std::vector<std::uint8_t> changed;
    std::vector<std::uint8_t> trailing(frames[0].payload.begin(),
                                       frames[0].payload.end());
    trailing.push_back(0);
    wire::append_frame(changed, P29WireKind::Root, trailing);
    for (std::size_t i = 1; i < frames.size(); ++i)
      wire::append_frame(changed, frames[i].kind, frames[i].payload);
    require(!icecc::codec::test_golden::equivalent(original, changed),
            "golden semantic comparison accepted trailing compressed data");
  }
}

// Confirms the ICECC_P29V1_MATERIALIZE_THP runtime switch (opt-5; see
// research/reports/P50_PERF_OPT_REPORT.md) is honored: forcing it on makes
// a reserve-crossing FILL take the madvise() path, forcing it off does
// not, with everything else about the FILL (materialized bytes, digest)
// unaffected either way -- madvise() is a best-effort kernel hint only, so
// toggling it can never change wire-visible state. Uses the test-only
// override slot rather than setenv(), since p29v1_materialize_thp_enabled()
// caches its environment read for the lifetime of the process.
void run_materialize_thp_switch_controls() {
  // Large enough that reserve()'s 2-MiB-aligned interior is non-empty
  // regardless of where glibc's mmap happens to land this allocation (head
  // and tail waste are each under 2 MiB, so an 8 MiB reservation leaves at
  // least ~4 MiB of aligned interior for madvise() to cover).
  constexpr std::size_t kLargeFillBytes = 8u * 1024 * 1024;
  const std::vector<std::uint8_t> large_payload(kLargeFillBytes, 'a');
  const std::vector<std::uint8_t> large_control =
      literal_control(1, kLargeFillBytes, kLargeFillBytes);
  const std::vector<std::uint8_t> large_fill =
      make_fill(large_control, large_payload);
  const std::vector<std::uint8_t> body = single_region_body();

  const struct RestoreOverride {
    ~RestoreOverride() {
      wire::p29v1_materialize_thp_test_override(
          wire::P29MaterializeThpOverride::Unset);
    }
  } restore_override;

  {
    wire::p29v1_materialize_thp_test_override(
        wire::P29MaterializeThpOverride::ForceOn);
    const std::uint64_t calls_before =
        wire::p29v1_materialize_thp_madvise_calls();
    ResearchProvider provider;
    P29Deserializer<ResearchProvider> deserializer(provider);
    (void)deserializer.receive_body(body);
    const std::span<const std::uint8_t> materialized = deserializer.receive_fill(
        large_fill, true, kLargeFillBytes);
    require(materialized.size() == kLargeFillBytes,
            "THP switch test: materialized size differs from the forced FILL");
    require(wire::p29v1_materialize_thp_madvise_calls() == calls_before + 1,
            "THP switch forced ON did not take the madvise path");
    deserializer.commit();
  }
  {
    wire::p29v1_materialize_thp_test_override(
        wire::P29MaterializeThpOverride::ForceOff);
    const std::uint64_t calls_before =
        wire::p29v1_materialize_thp_madvise_calls();
    ResearchProvider provider;
    P29Deserializer<ResearchProvider> deserializer(provider);
    (void)deserializer.receive_body(body);
    const std::span<const std::uint8_t> materialized = deserializer.receive_fill(
        large_fill, true, kLargeFillBytes);
    require(materialized.size() == kLargeFillBytes,
            "THP switch test: materialized size differs from the forced FILL");
    require(wire::p29v1_materialize_thp_madvise_calls() == calls_before,
            "THP switch forced OFF unexpectedly took the madvise path");
    deserializer.commit();
  }
}

// Pool-path regression for the InputBufferPool hook (cache/p50_input_buffer_
// pool.h, cache/codec/p29_wire.h's receive_fill()): a Provider that offers
// get_pooled_materialize_buffer() must have receive_fill() actually reuse a
// returned buffer -- proven here by pointer equality of the materialized
// span's data() across two TUs on the same deserializer, with the first
// TU's buffer handed back to the pool exactly as the production custom
// deleter does (cache/p50_input_record.cpp's prepare_validated_publish()) --
// and reuse must not perturb a single materialized byte relative to an
// unpooled baseline over the same input.
//
// Protocol shape: one deserializer consumes a two-TU route where only the
// SECOND fill closes entropy -- the production FILL-per-TU shape (see
// run_dialogue(): close only on the last TU). Ending the zstd stream on the
// first fill would leave the deserializer's entropy decoders closed, and the
// route's own control test ("receiver accepted BODY after route entropy
// close") pins that a subsequent receive_body() must then fail; the original
// version of this test did exactly that and failed codec_wire.
void run_input_buffer_pool_reuse_and_parity() {
  constexpr std::size_t kFillBytes = 256u * 1024;
  const std::vector<std::uint8_t> payload(kFillBytes, std::uint8_t{'p'});
  const std::vector<std::uint8_t> control =
      literal_control(1, kFillBytes, kFillBytes);
  const std::vector<std::uint8_t> body0 = single_region_body(/*id=*/0);
  const std::vector<std::uint8_t> body1 = single_region_body(/*id=*/1);

  struct RunResult {
    std::vector<std::uint8_t> first_bytes;
    std::vector<std::uint8_t> second_bytes;
    const std::uint8_t *first_data = nullptr;
    const std::uint8_t *second_data = nullptr;
    icecc::p50::InputBufferPool::Stats stats{};
  };

  // Identical two-TU dialogue for both runs; only the pool cap differs
  // (cap_bytes=0 disables pooling entirely -- get()/put() no-ops, exactly
  // today's pre-pool behavior).
  const auto run_sequence = [&](std::uint64_t cap_bytes) {
    PooledResearchProvider provider(cap_bytes);
    P29Deserializer<PooledResearchProvider> deserializer(provider);
    ContinuingFillBuilder fills;
    RunResult result;

    // TU 1: stage Region 0, keep the entropy streams open.
    (void)deserializer.receive_body(body0);
    const std::span<const std::uint8_t> first_materialized =
        deserializer.receive_fill(fills.make(control, payload, /*close=*/false),
                                  /*close_entropy=*/false, kFillBytes);
    require(first_materialized.size() == kFillBytes,
            "pool reuse test: first materialized size differs from the FILL");
    result.first_bytes.assign(first_materialized.begin(),
                              first_materialized.end());
    result.first_data = first_materialized.data();
    wire::MaterializedBuffer taken = deserializer.take_materialized();
    deserializer.commit();
    require(taken.data() == result.first_data,
            "pool reuse test: take_materialized() returned different storage "
            "than receive_fill() handed back");
    // Models the production custom deleter in prepare_validated_publish():
    // once the last InputRecord/InputCursor reference to the backing drops,
    // the deleter does nothing but move the vector into pool.put().
    provider.pool()->put(std::move(taken));

    // TU 2: stage Region 1 (Region 0 is committed-known now) and close the
    // route's entropy streams on this final fill.
    (void)deserializer.receive_body(body1);
    const std::span<const std::uint8_t> second_materialized =
        deserializer.receive_fill(fills.make(control, payload, /*close=*/true),
                                  /*close_entropy=*/true, kFillBytes);
    require(second_materialized.size() == kFillBytes,
            "pool reuse test: second materialized size differs from the FILL");
    result.second_bytes.assign(second_materialized.begin(),
                               second_materialized.end());
    result.second_data = second_materialized.data();
    deserializer.commit();
    result.stats = provider.pool()->stats();
    return result;
  };

  const RunResult baseline = run_sequence(/*cap_bytes=*/0);
  const RunResult pooled = run_sequence(/*cap_bytes=*/16u * 1024 * 1024);

  require(baseline.first_data != nullptr &&
              baseline.second_data != nullptr,
          "pool reuse test: unpooled baseline materialized nothing");
  require(pooled.first_bytes == baseline.first_bytes &&
              pooled.second_bytes == baseline.second_bytes,
          "pool reuse test: pooled-run materialized bytes differ from the "
          "unpooled baseline");
  require(pooled.first_data != nullptr && pooled.second_data != nullptr,
          "pool reuse test: pooled run materialized nothing");
  require(pooled.second_data == pooled.first_data,
          "pool reuse test: second receive_fill() did not reuse the pooled "
          "buffer (data() pointer differs)");
  require(pooled.stats.hits >= 1,
          "pool reuse test: pool recorded no hits");

  std::cout << "input buffer pool reuse + parity: OK (" << kFillBytes
            << " bytes per TU, pool hits=" << pooled.stats.hits << ")\n";
}

// Regression test for a decode-side bug found while building the THP
// switch test above (see research/reports/P50_PERF_OPT_REPORT.md): a
// payload whose decoded size is an exact multiple of the decoder's
// internal zstd output buffer (ZSTD_DStreamOutSize()) can finish its
// frame (remaining == 0) on the very ZSTD_decompressStream() call that
// also fills that buffer completely. ContinuingDecoder::decode()'s loop
// used to continue solely on output.pos == output.size, so it spun one
// extra iteration past an already-finished frame with no input left;
// zstd then reported a nonzero byte count for a nonexistent next frame,
// which read as "P29 zstd stream did not close". This test is deliberately
// minimal and decoupled from the THP-switch feature: it drives
// ContinuingEncoder/ContinuingDecoder directly with a single-chunk
// payload, independent of any materialize/FILL/THP machinery, so a
// regression here cannot be masked by -- or confused with -- unrelated
// feature changes.
void run_continuing_decoder_exact_multiple_regression() {
  const std::size_t chunk_bytes = ZSTD_DStreamOutSize();
  require(chunk_bytes > 0,
          "exact-multiple regression: ZSTD_DStreamOutSize() returned 0");
  // Highly compressible content keeps the whole payload inside a single
  // zstd block, so a one-shot decode() call is the one that both finishes
  // the frame and fills the output buffer -- the exact edge case.
  const std::vector<std::uint8_t> raw(chunk_bytes, std::uint8_t{0x5a});

  wire::ContinuingEncoder encoder;
  const std::vector<std::uint8_t> encoded = encoder.encode(raw, /*close=*/true);

  wire::ContinuingDecoder decoder;
  const std::vector<std::uint8_t> decoded =
      decoder.decode(encoded, /*close=*/true);

  require(decoded.size() == raw.size(),
          "exact-multiple regression: decoded size differs from the "
          "exact-multiple payload");
  require(decoded == raw,
          "exact-multiple regression: decoded bytes differ from the "
          "exact-multiple payload");
  std::cout << "continuing decoder exact-multiple regression: OK ("
            << chunk_bytes << " bytes)\n";
}

struct Options {
  std::vector<fs::path> inputs = phase0_inputs();
  P29InternLayout layout = P29InternLayout::probe();
  fs::path cf_reference = golden_root() / "research/phase0-11-cf.bin";
  fs::path fc_reference = golden_root() / "research/phase0-11-fc.bin";
  fs::path cf_output;
  fs::path fc_output;
  std::uint64_t expected_regions = 126016;
  std::uint64_t expected_occurrences = 166258;
  std::uint64_t expected_lines = 303181;
  bool cross_provider = true;
};

[[nodiscard]] std::uint64_t number(const char *text, const char *name) {
  char *end = nullptr;
  const unsigned long long value = std::strtoull(text, &end, 10);
  if (!end || *end)
    die(std::string("bad ") + name);
  return value;
}

Options options(int argc, char **argv) {
  Options result;
  for (int index = 1; index < argc; ++index) {
    const std::string_view option = argv[index];
    auto value = [&]() -> const char * {
      if (++index >= argc)
        die(std::string(option) + " needs a value");
      return argv[index];
    };
    if (option == "--manifest") {
      result.inputs = read_manifest(value());
      result.cf_reference.clear();
      result.fc_reference.clear();
    } else if (option == "--layout") {
      const std::string_view layout = value();
      if (layout == "probe")
        result.layout = P29InternLayout::probe();
      else if (layout == "sidecar")
        result.layout = P29InternLayout::sidecar();
      else
        die("layout must be probe or sidecar");
    } else if (option == "--cf-reference") {
      result.cf_reference = value();
    } else if (option == "--fc-reference") {
      result.fc_reference = value();
    } else if (option == "--cf-output") {
      result.cf_output = value();
    } else if (option == "--fc-output") {
      result.fc_output = value();
    } else if (option == "--expected-regions") {
      result.expected_regions = number(value(), "Region count");
    } else if (option == "--expected-occurrences") {
      result.expected_occurrences = number(value(), "occurrence count");
    } else if (option == "--expected-lines") {
      result.expected_lines = number(value(), "Line count");
    } else if (option == "--single-provider") {
      result.cross_provider = false;
    } else {
      die("unknown option " + std::string(option));
    }
  }
  require(!result.inputs.empty(), "wire input set is empty");
  return result;
}

} // namespace

int main(int argc, char **argv) {
  try {
    const Options config = options(argc, argv);
    const std::size_t reservation =
        icecc::codec::p29_interner_reservation(config.layout);
    MmapInternProvider intern_provider(reservation);
    const auto intern_start = Clock::now();
    P29Interner<MmapInternProvider> interner(intern_provider, config.layout);
    const Corpus corpus = load_and_intern(interner, config.inputs);
    const double intern_seconds =
        std::chrono::duration<double>(Clock::now() - intern_start).count();
    std::uint64_t occurrences = 0;
    for (const LogicalTu &tu : corpus.tus)
      occurrences += tu.regions->size();
    require(interner.distinct_regions() == config.expected_regions,
            "wire distinct Region count differs");
    require(occurrences == config.expected_occurrences,
            "wire Region occurrence count differs");
    require(interner.distinct_lines() == config.expected_lines,
            "wire distinct Line count differs");
    run_wire_controls(corpus, interner);
    run_golden_comparison_controls();
    run_materialize_thp_switch_controls();
    run_input_buffer_pool_reuse_and_parity();
    run_continuing_decoder_exact_multiple_regression();

    const auto total_start = Clock::now();
    auto research =
        run_dialogue<ResearchProvider, ResearchProvider>(corpus, interner);
    const double research_wall =
        std::chrono::duration<double>(Clock::now() - total_start).count();
    if (!config.cf_reference.empty())
      require(icecc::codec::test_golden::equivalent(
                  research.cf, read_file(config.cf_reference)),
              "C-to-F decoded frames differ from retained research golden");
    if (!config.fc_reference.empty())
      require(icecc::codec::test_golden::equivalent(
                  research.fc, read_file(config.fc_reference)),
              "F-to-C decoded frames differ from retained research golden");
    if (!config.cf_output.empty())
      write_file(config.cf_output, research.cf);
    if (!config.fc_output.empty())
      write_file(config.fc_output, research.fc);

    double product_wall = 0;
    if (config.cross_provider) {
      const auto product_start = Clock::now();
      auto product =
          run_dialogue<ProductProvider, ProductProvider>(corpus, interner);
      auto research_to_product =
          run_dialogue<ResearchProvider, ProductProvider>(corpus, interner);
      auto product_to_research =
          run_dialogue<ProductProvider, ResearchProvider>(corpus, interner);
      auto research_again =
          run_dialogue<ResearchProvider, ResearchProvider>(corpus, interner);
      product_wall =
          std::chrono::duration<double>(Clock::now() - product_start).count();
      require(product.cf == research.cf && product.fc == research.fc &&
                  research_to_product.cf == research.cf &&
                  research_to_product.fc == research.fc &&
                  product_to_research.cf == research.cf &&
                  product_to_research.fc == research.fc &&
                  research_again.cf == research.cf &&
                  research_again.fc == research.fc,
              "cross-provider or deterministic wire streams differ");
      require(product.published_regions == interner.distinct_regions(),
              "product provider Region publication count differs");
      require(research_to_product.published_regions ==
                      interner.distinct_regions() &&
                  product_to_research.published_regions ==
                      interner.distinct_regions(),
              "cross-provider Region publication count differs");
    }

    rusage usage{};
    require(getrusage(RUSAGE_SELF, &usage) == 0, "getrusage failed");
    const double mib = double(corpus.raw_bytes) / double(std::size_t{1} << 20);
    std::cout << "codec_wire: PASS"
              << " controls=PASS"
              << " tus=" << corpus.tus.size()
              << " unique_tus=" << corpus.unique_tus
              << " raw_bytes=" << corpus.raw_bytes
              << " unique_raw_bytes=" << corpus.unique_raw_bytes
              << " regions=" << interner.distinct_regions()
              << " region_occurrences=" << occurrences
              << " distinct_lines=" << interner.distinct_lines()
              << " cf_bytes=" << research.cf.size()
              << " fc_bytes=" << research.fc.size()
              << " intern_seconds=" << intern_seconds
              << " sender_seconds=" << research.sender_seconds
              << " receiver_seconds=" << research.receiver_seconds
              << " receiver_body_seconds=" << research.receiver_body_seconds
              << " receiver_fill_seconds=" << research.receiver_fill_seconds
              << " receiver_verify_seconds=" << research.receiver_verify_seconds
              << " receiver_commit_seconds=" << research.receiver_commit_seconds
              << " sender_MiB_s=" << (mib / research.sender_seconds)
              << " receiver_MiB_s=" << (mib / research.receiver_seconds)
              << " research_wall_seconds=" << research_wall
              << " product_wall_seconds=" << product_wall
              << " max_rss_kib=" << usage.ru_maxrss << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "codec_wire: FAIL: " << error.what() << '\n';
    return 1;
  }
}
