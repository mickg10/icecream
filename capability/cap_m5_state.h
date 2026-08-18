#pragma once
// Protocol-50 M5 receiver mirrors, bounded cache policy, and dynamic-state
// snapshots. This module deliberately contains no scheduling or socket logic.
// The coordinator owns global C authority; each F owns one FStore plus
// CacheState; snapshots contain only dynamic generation state (the immutable
// corpus/interner remains the generation's backing store).

#include "cap_codec.h"
#include "cap_identity.h"
#include "cap_protocol.h"
#include "cap_transport.h"
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <utility>

namespace capm5 {

struct ReceiverMirror {
  std::vector<uint8_t> regions;
  std::vector<uint8_t> public_lines{0};
  std::vector<uint8_t> blocks;
  uint32_t path_count = 0;
  bool recovering = false;

  void init(uint32_t nreg, uint32_t nblk) {
    regions.assign(nreg, 0);
    public_lines.assign(1, 0);
    blocks.assign(nblk, 0);
    path_count = 0;
    recovering = false;
  }
  void ensure_dimensions(uint32_t nreg, uint32_t nblk) {
    if (regions.size() < nreg)
      regions.resize(nreg, 0);
    if (blocks.size() < nblk)
      blocks.resize(nblk, 0);
  }
  void reset() {
    std::fill(regions.begin(), regions.end(), 0);
    std::fill(public_lines.begin(), public_lines.end(), 0);
    std::fill(blocks.begin(), blocks.end(), 0);
    path_count = 0;
    recovering = true;
  }
  void apply(const capp::CacheDropsM5 &drops) {
    for (uint32_t id : drops.regions)
      if (id < regions.size())
        regions[id] = 0;
    for (uint32_t id : drops.public_lines)
      if (id < public_lines.size())
        public_lines[id] = 0;
    for (uint32_t id : drops.blocks)
      if (id < blocks.size())
        blocks[id] = 0;
  }
};

// A MixedEncoder contains both global authority and one scratch receiver
// mirror.  This scope swaps a selected mirror in for one materialization, then
// returns its mutations to the owner.
struct MirrorScope {
  capc::MixedEncoder &encoder;
  ReceiverMirror &mirror;
  explicit MirrorScope(capc::MixedEncoder &value, ReceiverMirror &selected)
      : encoder(value), mirror(selected) {
    std::swap(encoder.fknownReg, mirror.regions);
    std::swap(encoder.fknownPublic, mirror.public_lines);
    std::swap(encoder.recovering, mirror.recovering);
  }
  MirrorScope(const MirrorScope &) = delete;
  MirrorScope &operator=(const MirrorScope &) = delete;
  ~MirrorScope() {
    std::swap(encoder.fknownReg, mirror.regions);
    std::swap(encoder.fknownPublic, mirror.public_lines);
    std::swap(encoder.recovering, mirror.recovering);
  }
};

struct CacheLimits {
  uint64_t region_bytes = std::numeric_limits<uint64_t>::max();
  uint64_t public_bytes = std::numeric_limits<uint64_t>::max();
  uint64_t block_bytes = std::numeric_limits<uint64_t>::max();
  uint64_t compact_slack = 64ull << 20;
};

struct CacheTotals {
  uint64_t region_bytes = 0, public_bytes = 0, block_bytes = 0,
           dead_region_bytes = 0;
  uint32_t regions = 0, public_lines = 0, blocks = 0;
  uint64_t region_removals = 0, public_removals = 0, block_removals = 0,
           compactions = 0;
};

struct LruEntry {
  uint64_t tick = 0;
  uint32_t id = 0;
};

// A bounded update-in-place min-heap.  Unlike a lazy heap, repeated touches do
// not append stale entries: policy memory is O(object-ID capacity), and the
// live heap has exactly one entry per resident object.
class IndexedLru {
public:
  void reset(size_t capacity) {
    heap.clear();
    positions.assign(capacity, ABSENT);
  }
  void ensure(size_t capacity) {
    if (positions.size() < capacity)
      positions.resize(capacity, ABSENT);
  }
  void touch(uint32_t id, uint64_t tick) {
    ensure(size_t(id) + 1);
    uint32_t position = positions[id];
    if (position == ABSENT) {
      position = uint32_t(heap.size());
      positions[id] = position;
      heap.push_back({tick, id});
      sift_up(position);
      return;
    }
    LruEntry previous = heap[position];
    heap[position].tick = tick;
    if (earlier(heap[position], previous))
      sift_up(position);
    else
      sift_down(position);
  }
  uint32_t pop() {
    if (heap.empty())
      return UINT32_MAX;
    uint32_t id = heap.front().id;
    positions[id] = ABSENT;
    if (heap.size() == 1) {
      heap.pop_back();
      return id;
    }
    heap.front() = heap.back();
    heap.pop_back();
    positions[heap.front().id] = 0;
    sift_down(0);
    return id;
  }
  uint64_t semantic_bytes() const {
    return heap.size() * sizeof(LruEntry) + positions.size() * sizeof(uint32_t);
  }

private:
  static constexpr uint32_t ABSENT = UINT32_MAX;
  std::vector<LruEntry> heap;
  std::vector<uint32_t> positions;

  static bool earlier(const LruEntry &left, const LruEntry &right) {
    return left.tick != right.tick ? left.tick < right.tick
                                   : left.id < right.id;
  }
  void swap_entries(uint32_t left, uint32_t right) {
    std::swap(heap[left], heap[right]);
    positions[heap[left].id] = left;
    positions[heap[right].id] = right;
  }
  void sift_up(uint32_t position) {
    while (position) {
      uint32_t parent = (position - 1) / 2;
      if (!earlier(heap[position], heap[parent]))
        break;
      swap_entries(position, parent);
      position = parent;
    }
  }
  void sift_down(uint32_t position) {
    for (;;) {
      uint32_t left = position * 2 + 1;
      if (left >= heap.size())
        return;
      uint32_t right = left + 1;
      uint32_t child = right < heap.size() && earlier(heap[right], heap[left])
                           ? right
                           : left;
      if (!earlier(heap[child], heap[position]))
        return;
      swap_entries(position, child);
      position = child;
    }
  }
};

class CacheState {
public:
  CacheLimits limits;
  CacheTotals totals;

  void init(uint32_t nreg, uint32_t nblk, CacheLimits configured) {
    limits = configured;
    region_last.assign(nreg, 0);
    region_accounted.assign(nreg, 0);
    block_last.assign(nblk, 0);
    block_accounted.assign(nblk, 0);
    public_accounted.assign(1, 0);
    region_heap.reset(nreg);
    public_heap.reset(1);
    block_heap.reset(nblk);
    totals = CacheTotals{};
  }

  void ensure_dimensions(uint32_t nreg, uint32_t nblk) {
    if (region_last.size() < nreg) {
      region_last.resize(nreg, 0);
      region_accounted.resize(nreg, 0);
      region_heap.ensure(nreg);
    }
    if (block_last.size() < nblk) {
      block_last.resize(nblk, 0);
      block_accounted.resize(nblk, 0);
      block_heap.ensure(nblk);
    }
  }

  void account_preloaded_region(const capc::FStore &store, uint32_t id,
                                uint64_t tick = 1) {
    if (id >= store.FmixedRegions.size() || !store.FmixedRegions[id].known ||
        region_accounted[id])
      return;
    region_accounted[id] = 1;
    region_last[id] = tick;
    totals.region_bytes += store.FmixedRegions[id].length;
    ++totals.regions;
    region_heap.touch(id, tick);
  }

  void account_fill(const capc::FStore &store,
                    const std::vector<uint32_t> &regions,
                    const capc::FStore::BlockTransaction &blocks,
                    const capc::FStore::FillTransaction &fill, uint64_t tick) {
    ensure_public(store.FpublicPresent.size());
    for (uint32_t id : regions)
      if (id < store.FmixedRegions.size() && store.FmixedRegions[id].known) {
        if (!region_accounted[id]) {
          region_accounted[id] = 1;
          totals.region_bytes += store.FmixedRegions[id].length;
          ++totals.regions;
        }
        touch_region(id, tick);
      }
    for (const auto &binding : blocks.bindings)
      if (binding.install) {
        uint32_t id = binding.id;
        if (!block_accounted[id]) {
          block_accounted[id] = 1;
          totals.block_bytes +=
              store.FblkChildren[id].size() * sizeof(uint32_t);
          ++totals.blocks;
        }
      }
    for (uint32_t id : fill.public_touches)
      if (id < store.FpublicPresent.size() && store.FpublicPresent[id]) {
        if (!public_accounted[id]) {
          public_accounted[id] = 1;
          totals.public_bytes += store.FpublicBytes[id].size();
          ++totals.public_lines;
        }
        public_heap.touch(id, store.FpublicLastUse[id]);
      }
  }

  void touch_committed(const capc::FStore &store,
                       const std::vector<uint32_t> &regions,
                       const std::vector<uint32_t> &blocks, uint64_t tick) {
    for (uint32_t id : regions)
      if (id < region_accounted.size() && store.FmixedRegions[id].known)
        touch_region(id, tick);
    for (uint32_t id : blocks)
      if (id < block_accounted.size() && store.FknownBlk[id]) {
        block_last[id] = tick;
        block_heap.touch(id, tick);
      }
  }

  capp::CacheDropsM5 evict(capc::FStore &store) {
    capp::CacheDropsM5 drops;
    while (totals.region_bytes > limits.region_bytes) {
      uint32_t id = pop_region(store);
      if (id == UINT32_MAX)
        break;
      auto &view = store.FmixedRegions[id];
      totals.region_bytes -= view.length;
      totals.dead_region_bytes += view.length;
      view.known = false;
      region_accounted[id] = 0;
      --totals.regions;
      ++totals.region_removals;
      drops.regions.push_back(id);
    }
    while (totals.public_bytes > limits.public_bytes) {
      uint32_t id = pop_public(store);
      if (id == UINT32_MAX)
        break;
      totals.public_bytes -= store.FpublicBytes[id].size();
      store.FpublicPresent[id] = 0;
      std::vector<uint8_t>().swap(store.FpublicBytes[id]);
      public_accounted[id] = 0;
      --store.publicHeld;
      --totals.public_lines;
      ++totals.public_removals;
      drops.public_lines.push_back(id);
    }
    while (totals.block_bytes > limits.block_bytes) {
      uint32_t id = pop_block(store);
      if (id == UINT32_MAX)
        break;
      totals.block_bytes -= store.FblkChildren[id].size() * sizeof(uint32_t);
      store.FknownBlk[id] = 0;
      std::vector<uint32_t>().swap(store.FblkChildren[id]);
      block_accounted[id] = 0;
      --totals.blocks;
      ++totals.block_removals;
      drops.blocks.push_back(id);
    }
    maybe_compact(store);
    return drops;
  }

  void rebuild(const capc::FStore &store,
               const std::vector<uint64_t> &savedRegionLast,
               const std::vector<uint64_t> &savedBlockLast) {
    region_heap.reset(store.NREG);
    public_heap.reset(store.FpublicPresent.size());
    block_heap.reset(store.NBLK);
    totals = CacheTotals{};
    region_last = savedRegionLast;
    region_last.resize(store.NREG, 0);
    region_accounted.assign(store.NREG, 0);
    block_last = savedBlockLast;
    block_last.resize(store.NBLK, 0);
    block_accounted.assign(store.NBLK, 0);
    public_accounted.assign(store.FpublicPresent.size(), 0);
    for (uint32_t id = 0; id < store.NREG; ++id)
      if (store.FmixedRegions[id].known) {
        region_accounted[id] = 1;
        totals.region_bytes += store.FmixedRegions[id].length;
        ++totals.regions;
        uint64_t tick = region_last[id] ? region_last[id] : 1;
        region_last[id] = tick;
        region_heap.touch(id, tick);
      }
    for (uint32_t id = 1; id < store.FpublicPresent.size(); ++id)
      if (store.FpublicPresent[id]) {
        public_accounted[id] = 1;
        totals.public_bytes += store.FpublicBytes[id].size();
        ++totals.public_lines;
        uint64_t tick = store.FpublicLastUse[id] ? store.FpublicLastUse[id] : 1;
        public_heap.touch(id, tick);
      }
    for (uint32_t id = 0; id < store.NBLK; ++id)
      if (store.FknownBlk[id]) {
        block_accounted[id] = 1;
        totals.block_bytes += store.FblkChildren[id].size() * sizeof(uint32_t);
        ++totals.blocks;
        uint64_t tick = block_last[id] ? block_last[id] : 1;
        block_last[id] = tick;
        block_heap.touch(id, tick);
      }
  }

  const std::vector<uint64_t> &region_ticks() const { return region_last; }
  const std::vector<uint64_t> &block_ticks() const { return block_last; }
  uint64_t semantic_bytes() const {
    return totals.region_bytes + totals.public_bytes + totals.block_bytes +
           region_last.size() * sizeof(uint64_t) +
           block_last.size() * sizeof(uint64_t) + region_heap.semantic_bytes() +
           public_heap.semantic_bytes() + block_heap.semantic_bytes();
  }

private:
  std::vector<uint64_t> region_last, block_last;
  std::vector<uint8_t> region_accounted, public_accounted, block_accounted;
  IndexedLru region_heap, public_heap, block_heap;
  void ensure_public(size_t size) {
    if (public_accounted.size() < size) {
      public_accounted.resize(size, 0);
      public_heap.ensure(size);
    }
  }
  void touch_region(uint32_t id, uint64_t tick) {
    region_last[id] = tick;
    region_heap.touch(id, tick);
  }
  uint32_t pop_region(const capc::FStore &store) {
    for (;;) {
      uint32_t id = region_heap.pop();
      if (id == UINT32_MAX)
        return id;
      if (id < region_accounted.size() && region_accounted[id] &&
          store.FmixedRegions[id].known)
        return id;
    }
  }
  uint32_t pop_public(const capc::FStore &store) {
    for (;;) {
      uint32_t id = public_heap.pop();
      if (id == UINT32_MAX)
        return id;
      if (id < public_accounted.size() && public_accounted[id] &&
          id < store.FpublicPresent.size() && store.FpublicPresent[id])
        return id;
    }
  }
  uint32_t pop_block(const capc::FStore &store) {
    for (;;) {
      uint32_t id = block_heap.pop();
      if (id == UINT32_MAX)
        return id;
      if (id < block_accounted.size() && block_accounted[id] &&
          store.FknownBlk[id])
        return id;
    }
  }
  void maybe_compact(capc::FStore &store) {
    if (!totals.dead_region_bytes)
      return;
    if (totals.dead_region_bytes <= limits.compact_slack &&
        totals.dead_region_bytes <= totals.region_bytes)
      return;
    std::vector<uint8_t> compact;
    compact.reserve(size_t(totals.region_bytes));
    for (auto &view : store.FmixedRegions)
      if (view.known) {
        if (view.offset > store.FmixedRegionData.size() ||
            view.length > store.FmixedRegionData.size() - view.offset) {
          fprintf(stderr, "M5 compact: invalid Region view\n");
          abort();
        }
        size_t next = compact.size();
        compact.insert(
            compact.end(), store.FmixedRegionData.begin() + view.offset,
            store.FmixedRegionData.begin() + view.offset + view.length);
        view.offset = next;
      }
    store.FmixedRegionData.swap(compact);
    totals.dead_region_bytes = 0;
    ++totals.compactions;
  }
};

// Direct installation is used only to construct the two CACHE50 starting
// states.  It is independent of the wire grammar: the receiver begins with
// immutable exact Region bytes.
static inline bool install_raw_region(capc::FStore &store,
                                      const capc::Interner &dict, uint32_t id) {
  if (id >= store.FmixedRegions.size())
    return false;
  const char *data = dict.region_data(id);
  uint32_t length = dict.region_raw_len(id);
  auto &view = store.FmixedRegions[id];
  if (view.known)
    return view.length == length &&
           (!length ||
            !memcmp(store.FmixedRegionData.data() + view.offset, data, length));
  size_t offset = store.FmixedRegionData.size();
  store.FmixedRegionData.insert(store.FmixedRegionData.end(), data,
                                data + length);
  view = {offset, length, true};
  return true;
}

class SnapshotWriter {
public:
  explicit SnapshotWriter(const std::string &path)
      : final_path(path), temporary(path + ".tmp." + std::to_string(getpid())),
        out(temporary, std::ios::binary | std::ios::trunc) {}
  bool good() const { return out.good(); }
  void u8(uint8_t value) { out.put(char(value)); }
  void u32(uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
      u8(uint8_t(value >> (8 * i)));
  }
  void u64(uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
      u8(uint8_t(value >> (8 * i)));
  }
  void raw(const void *data, size_t size) {
    if (size)
      out.write(static_cast<const char *>(data), std::streamsize(size));
  }
  void bytes(const std::vector<uint8_t> &value) {
    u64(value.size());
    raw(value.data(), value.size());
  }
  void string(const std::string &value) {
    u64(value.size());
    raw(value.data(), value.size());
  }
  bool finish() {
    out.flush();
    bool ok = out.good();
    out.close();
    if (ok) {
      std::error_code error;
      std::filesystem::rename(temporary, final_path, error);
      ok = !error;
    }
    if (!ok) {
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
    }
    return ok;
  }

private:
  std::string final_path, temporary;
  std::ofstream out;
};

class SnapshotReader {
public:
  explicit SnapshotReader(const std::string &path)
      : in(path, std::ios::binary) {}
  bool good() const { return in.good(); }
  bool u8(uint8_t &value) {
    char byte = 0;
    if (!in.get(byte))
      return false;
    value = uint8_t(byte);
    return true;
  }
  bool u32(uint32_t &value) {
    value = 0;
    for (unsigned i = 0; i < 4; ++i) {
      uint8_t byte = 0;
      if (!u8(byte))
        return false;
      value |= uint32_t(byte) << (8 * i);
    }
    return true;
  }
  bool u64(uint64_t &value) {
    value = 0;
    for (unsigned i = 0; i < 8; ++i) {
      uint8_t byte = 0;
      if (!u8(byte))
        return false;
      value |= uint64_t(byte) << (8 * i);
    }
    return true;
  }
  bool raw(void *data, size_t size) {
    return !size ||
           bool(in.read(static_cast<char *>(data), std::streamsize(size)));
  }
  bool bytes(std::vector<uint8_t> &value, uint64_t maximum = 1ull << 40) {
    uint64_t size = 0;
    if (!u64(size) || size > maximum || size > SIZE_MAX)
      return false;
    value.resize(size_t(size));
    return raw(value.data(), value.size());
  }
  bool string(std::string &value, uint64_t maximum = 1ull << 30) {
    uint64_t size = 0;
    if (!u64(size) || size > maximum || size > SIZE_MAX)
      return false;
    value.resize(size_t(size));
    return raw(value.data(), value.size());
  }
  bool end() {
    char byte = 0;
    return !in.get(byte) && in.eof();
  }

private:
  std::ifstream in;
};

static constexpr char SNAP_MAGIC[8] = {'C', 'A', 'P', 'M', '5', 'S', '1', '\0'};

static inline void write_u8_vector(SnapshotWriter &out,
                                   const std::vector<uint8_t> &values) {
  out.bytes(values);
}
static inline bool read_u8_vector(SnapshotReader &in,
                                  std::vector<uint8_t> &values,
                                  uint64_t maximum) {
  return in.bytes(values, maximum);
}
static inline void write_u64_vector(SnapshotWriter &out,
                                    const std::vector<uint64_t> &values) {
  out.u64(values.size());
  for (uint64_t value : values)
    out.u64(value);
}
static inline bool read_u64_vector(SnapshotReader &in,
                                   std::vector<uint64_t> &values,
                                   uint64_t maximum) {
  uint64_t count = 0;
  if (!in.u64(count) || count > maximum || count > SIZE_MAX)
    return false;
  values.resize(size_t(count));
  for (uint64_t &value : values)
    if (!in.u64(value))
      return false;
  return true;
}

static inline bool save_c_snapshot(const std::string &path,
                                   const cap::SourceGeneration &generation,
                                   const capc::MixedEncoder &encoder,
                                   const std::vector<ReceiverMirror> &mirrors) {
  SnapshotWriter out(path);
  if (!out.good())
    return false;
  out.raw(SNAP_MAGIC, sizeof SNAP_MAGIC);
  out.u8('C');
  out.u32(capp::CAP_PROTOCOL_VERSION);
  out.raw(generation.data(), generation.size());
  out.u64(encoder.mixedCLine.size());
  for (const auto &line : encoder.mixedCLine) {
    out.u32(line.source_region);
    out.u32(line.source_offset);
    out.u32(line.public_id);
  }
  out.u32(encoder.nextMixedPublic);
  out.u64(encoder.paths.size());
  for (const auto &pathValue : encoder.paths)
    out.string(pathValue);
  for (uint64_t value : encoder.mixedOps)
    out.u64(value);
  for (uint64_t value :
       {encoder.op7_count, encoder.op8_count, encoder.op9_count,
        encoder.op7_wire, encoder.op8_wire, encoder.op9_wire,
        encoder.mixedLiteralRaw, encoder.mixedArrayValues, encoder.n_marker,
        encoder.n_literal})
    out.u64(value);
  out.u64(mirrors.size());
  for (const auto &mirror : mirrors) {
    write_u8_vector(out, mirror.regions);
    write_u8_vector(out, mirror.public_lines);
    write_u8_vector(out, mirror.blocks);
    out.u32(mirror.path_count);
    out.u8(mirror.recovering ? 1 : 0);
  }
  return out.finish();
}

static inline bool load_c_snapshot(const std::string &path,
                                   const cap::SourceGeneration &expected,
                                   capc::MixedEncoder &encoder,
                                   std::vector<ReceiverMirror> &mirrors,
                                   uint32_t nreg, uint32_t nblk,
                                   uint32_t distinctLines) {
  SnapshotReader in(path);
  char magic[sizeof SNAP_MAGIC];
  uint8_t kind = 0, recovering = 0;
  uint32_t version = 0;
  cap::SourceGeneration generation{};
  uint64_t count = 0;
  if (!in.good() || !in.raw(magic, sizeof magic) ||
      memcmp(magic, SNAP_MAGIC, sizeof magic) || !in.u8(kind) || kind != 'C' ||
      !in.u32(version) || version != capp::CAP_PROTOCOL_VERSION ||
      !in.raw(generation.data(), generation.size()) || generation != expected ||
      !in.u64(count) || count != uint64_t(distinctLines) + 1)
    return false;
  encoder.init(distinctLines, nreg);
  for (auto &line : encoder.mixedCLine)
    if (!in.u32(line.source_region) || !in.u32(line.source_offset) ||
        !in.u32(line.public_id))
      return false;
  if (!in.u32(encoder.nextMixedPublic) || !encoder.nextMixedPublic ||
      !in.u64(count) || count > UINT32_MAX)
    return false;
  encoder.paths.resize(size_t(count));
  encoder.pathid.clear();
  for (uint32_t i = 0; i < count; ++i) {
    if (!in.string(encoder.paths[i]))
      return false;
    if (!encoder.pathid.emplace(encoder.paths[i], i).second)
      return false;
  }
  for (uint64_t &value : encoder.mixedOps)
    if (!in.u64(value))
      return false;
  uint64_t *fields[] = {&encoder.op7_count,       &encoder.op8_count,
                        &encoder.op9_count,       &encoder.op7_wire,
                        &encoder.op8_wire,        &encoder.op9_wire,
                        &encoder.mixedLiteralRaw, &encoder.mixedArrayValues,
                        &encoder.n_marker,        &encoder.n_literal};
  for (uint64_t *field : fields)
    if (!in.u64(*field))
      return false;
  if (!in.u64(count) || count > 1024)
    return false;
  mirrors.resize(size_t(count));
  for (auto &mirror : mirrors) {
    if (!read_u8_vector(in, mirror.regions, nreg) ||
        mirror.regions.size() != nreg ||
        !read_u8_vector(in, mirror.public_lines, UINT32_MAX) ||
        mirror.public_lines.empty() ||
        !read_u8_vector(in, mirror.blocks, nblk) ||
        mirror.blocks.size() != nblk || !in.u32(mirror.path_count) ||
        mirror.path_count > encoder.paths.size() || !in.u8(recovering) ||
        recovering > 1)
      return false;
    mirror.recovering = recovering != 0;
  }
  return in.end();
}

static inline bool save_f_snapshot(const std::string &path,
                                   const cap::SourceGeneration &generation,
                                   const capc::FStore &store,
                                   const CacheState &cache) {
  SnapshotWriter out(path);
  if (!out.good())
    return false;
  out.raw(SNAP_MAGIC, sizeof SNAP_MAGIC);
  out.u8('F');
  out.u32(capp::CAP_PROTOCOL_VERSION);
  out.raw(generation.data(), generation.size());
  out.u32(store.NREG);
  out.u32(store.NBLK);
  uint64_t known = 0;
  for (const auto &view : store.FmixedRegions)
    known += view.known ? 1 : 0;
  out.u64(known);
  const auto &regionTicks = cache.region_ticks();
  for (uint32_t id = 0; id < store.NREG; ++id)
    if (store.FmixedRegions[id].known) {
      const auto &view = store.FmixedRegions[id];
      if (view.offset > store.FmixedRegionData.size() ||
          view.length > store.FmixedRegionData.size() - view.offset)
        return false;
      out.u32(id);
      out.u64(regionTicks[id]);
      out.u32(view.length);
      out.raw(store.FmixedRegionData.data() + view.offset, view.length);
    }
  out.u32(store.Fpublic_next);
  out.u32(store.publicHeld);
  for (uint32_t id = 1; id < store.FpublicPresent.size(); ++id)
    if (store.FpublicPresent[id]) {
      out.u32(id);
      out.u32(store.FpublicLastUse[id]);
      out.bytes(store.FpublicBytes[id]);
    }
  out.u64(store.Fpaths.size());
  for (const auto &value : store.Fpaths)
    out.string(value);
  known = 0;
  for (uint8_t value : store.FknownBlk)
    known += value ? 1 : 0;
  out.u64(known);
  const auto &blockTicks = cache.block_ticks();
  for (uint32_t id = 0; id < store.NBLK; ++id)
    if (store.FknownBlk[id]) {
      out.u32(id);
      out.u64(blockTicks[id]);
      out.u64(store.FblkChildren[id].size());
      for (uint32_t child : store.FblkChildren[id])
        out.u32(child);
    }
  out.u64(cache.limits.region_bytes);
  out.u64(cache.limits.public_bytes);
  out.u64(cache.limits.block_bytes);
  out.u64(cache.limits.compact_slack);
  out.u64(cache.totals.region_removals);
  out.u64(cache.totals.public_removals);
  out.u64(cache.totals.block_removals);
  out.u64(cache.totals.compactions);
  return out.finish();
}

static inline bool load_f_snapshot(const std::string &path,
                                   const cap::SourceGeneration &expected,
                                   capc::FStore &store, CacheState &cache) {
  SnapshotReader in(path);
  char magic[sizeof SNAP_MAGIC];
  uint8_t kind = 0;
  uint32_t version = 0, nreg = 0, nblk = 0, id = 0, length = 0, last32 = 0;
  cap::SourceGeneration generation{};
  uint64_t count = 0, tick = 0, children = 0;
  std::vector<uint64_t> regionTicks, blockTicks;
  if (!in.good() || !in.raw(magic, sizeof magic) ||
      memcmp(magic, SNAP_MAGIC, sizeof magic) || !in.u8(kind) || kind != 'F' ||
      !in.u32(version) || version != capp::CAP_PROTOCOL_VERSION ||
      !in.raw(generation.data(), generation.size()) || generation != expected ||
      !in.u32(nreg) || !in.u32(nblk) || !nreg)
    return false;
  store = capc::FStore{};
  store.init(nreg, nblk);
  regionTicks.assign(nreg, 0);
  blockTicks.assign(nblk, 0);
  if (!in.u64(count) || count > nreg)
    return false;
  for (uint64_t i = 0; i < count; ++i) {
    if (!in.u32(id) || id >= nreg || store.FmixedRegions[id].known ||
        !in.u64(tick) || !in.u32(length))
      return false;
    size_t offset = store.FmixedRegionData.size();
    if (length > cap::MAX_PAYLOAD)
      return false;
    store.FmixedRegionData.resize(offset + length);
    if (!in.raw(store.FmixedRegionData.data() + offset, length))
      return false;
    store.FmixedRegions[id] = {offset, length, true};
    regionTicks[id] = tick;
  }
  uint32_t publicNext = 0, publicHeld = 0;
  if (!in.u32(publicNext) || !publicNext || !in.u32(publicHeld) ||
      publicHeld > publicNext)
    return false;
  store.Fpublic_next = publicNext;
  store.publicHeld = publicHeld;
  store.FpublicBytes.resize(publicNext);
  store.FpublicPresent.resize(publicNext, 0);
  store.FpublicLastUse.resize(publicNext, 0);
  for (uint32_t i = 0; i < publicHeld; ++i) {
    std::vector<uint8_t> bytes;
    if (!in.u32(id) || !id || id >= publicNext || store.FpublicPresent[id] ||
        !in.u32(last32) || !in.bytes(bytes, cap::MAX_PAYLOAD))
      return false;
    store.FpublicPresent[id] = 1;
    store.FpublicLastUse[id] = last32;
    store.FpublicBytes[id] = std::move(bytes);
  }
  if (!in.u64(count) || count > UINT32_MAX)
    return false;
  store.Fpaths.resize(size_t(count));
  for (auto &value : store.Fpaths)
    if (!in.string(value))
      return false;
  if (!in.u64(count) || count > nblk)
    return false;
  for (uint64_t i = 0; i < count; ++i) {
    if (!in.u32(id) || id >= nblk || store.FknownBlk[id] || !in.u64(tick) ||
        !in.u64(children) || children > nreg)
      return false;
    store.FknownBlk[id] = 1;
    store.FblkChildren[id].resize(size_t(children));
    for (uint32_t &child : store.FblkChildren[id])
      if (!in.u32(child) || child >= nreg)
        return false;
    blockTicks[id] = tick;
  }
  CacheLimits limits;
  uint64_t regionRemovals = 0, publicRemovals = 0, blockRemovals = 0,
           compactions = 0;
  if (!in.u64(limits.region_bytes) || !in.u64(limits.public_bytes) ||
      !in.u64(limits.block_bytes) || !in.u64(limits.compact_slack) ||
      !in.u64(regionRemovals) || !in.u64(publicRemovals) ||
      !in.u64(blockRemovals) || !in.u64(compactions) || !in.end())
    return false;
  cache.init(nreg, nblk, limits);
  cache.rebuild(store, regionTicks, blockTicks);
  cache.totals.region_removals = regionRemovals;
  cache.totals.public_removals = publicRemovals;
  cache.totals.block_removals = blockRemovals;
  cache.totals.compactions = compactions;
  return true;
}

} // namespace capm5
