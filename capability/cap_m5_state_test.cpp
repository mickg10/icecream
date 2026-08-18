// Focused M5 state tests: per-F mirror isolation, sparse ordinal state, bounded
// Region/public-Line/Block removal, arena compaction, and C/F snapshot round
// trips.
#include "cap_m5_state.h"
#include "cap_m5_metrics.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

using namespace capc;
using namespace capm5;

static int failures = 0;
static void check(bool condition, const char *message) {
  fprintf(stderr, "  %s: %s\n", condition ? "ok" : "FAIL", message);
  if (!condition)
    ++failures;
}

static bool write_partial_copy(const std::string &source,
                               const std::string &target) {
  std::ifstream input(source, std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
  if (input.bad() || bytes.size() < 2)
    return false;
  bytes.resize(bytes.size() / 2);
  std::ofstream output(target, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), std::streamsize(bytes.size()));
  return output.good();
}

int main() {
  fprintf(stderr, "[1] receiver mirror scope:\n");
  MixedEncoder encoder;
  encoder.init(4, 3);
  ReceiverMirror first, second;
  first.init(3, 2);
  second.init(3, 2);
  first.regions[1] = 1;
  second.regions[2] = 1;
  {
    MirrorScope scope(encoder, first);
    check(encoder.fknownReg[1] && !encoder.fknownReg[2],
          "first mirror selected");
    encoder.fknownReg[0] = 1;
  }
  {
    MirrorScope scope(encoder, second);
    check(!encoder.fknownReg[1] && encoder.fknownReg[2],
          "second mirror isolated");
  }
  check(first.regions[0] && first.regions[1] && !second.regions[0],
        "mutations return only to selected mirror");

  fprintf(stderr, "[1b] grow-only typed dimensions:\n");
  MixedEncoder growingEncoder;
  growingEncoder.init(2, 2);
  growingEncoder.mixedCLine[1].public_id = 7;
  growingEncoder.fknownReg[1] = 1;
  growingEncoder.ensure_dimensions(9, 11);
  check(growingEncoder.mixedCLine.size() == 10 &&
            growingEncoder.fknownReg.size() == 11 &&
            growingEncoder.mixedCLine[1].public_id == 7 &&
            growingEncoder.fknownReg[1],
        "C authority grows without changing prior bindings");
  ReceiverMirror growingMirror;
  growingMirror.init(2, 2);
  growingMirror.regions[1] = 1;
  growingMirror.blocks[0] = 1;
  growingMirror.ensure_dimensions(11, 7);
  check(growingMirror.regions.size() == 11 &&
            growingMirror.blocks.size() == 7 && growingMirror.regions[1] &&
            growingMirror.blocks[0] && !growingMirror.regions[10] &&
            !growingMirror.blocks[6],
        "receiver mirror grows with unknown appended ordinals");
  FStore growingStore;
  growingStore.init(2, 2);
  growingStore.FmixedRegions[1].known = true;
  growingStore.FknownBlk[0] = 1;
  growingStore.ensure_dimensions(11, 7);
  check(growingStore.NREG == 11 && growingStore.NBLK == 7 &&
            growingStore.FmixedRegions[1].known &&
            growingStore.FknownBlk[0] &&
            !growingStore.FmixedRegions[10].known &&
            !growingStore.FknownBlk[6],
        "F stores grow without changing resident objects");
  CacheState growingCache;
  growingCache.init(2, 2, CacheLimits{});
  growingCache.ensure_dimensions(11, 7);
  check(growingCache.semantic_bytes() >=
            11 * (sizeof(uint64_t) + sizeof(uint32_t)) +
                7 * (sizeof(uint64_t) + sizeof(uint32_t)),
        "cache policy indexes grow to cover typed stores");

  fprintf(stderr, "[1c] typed Root closure and expansion:\n");
  FStore typedStore;
  typedStore.init(2, 1);
  typedStore.FmixedRegionData = {'a', 'b', 'c'};
  typedStore.FmixedRegions[0] = {0, 1, true};
  typedStore.FmixedRegions[1] = {1, 2, true};
  std::vector<uint8_t> blockDefinition;
  put_varint(blockDefinition, 1);
  put_varint(blockDefinition, 0);
  blockDefinition.push_back(0);
  put_varint(blockDefinition, 2);
  put_varint(blockDefinition, 0);
  put_varint(blockDefinition, 1);
  FStore::BlockTransaction typedBlocks;
  check(typedStore.stage_blocks(blockDefinition, typedBlocks),
        "typed Block staged");
  std::vector<uint8_t> typedRoot;
  put_varint(typedRoot, uint64_t(1) << 1);
  put_varint(typedRoot, (uint64_t(0) << 1) | 1);
  std::vector<uint32_t> typedRegions, typedRequiredBlocks;
  check(typedStore.typed_requirements(typedRoot, &typedBlocks, typedRegions,
                                      typedRequiredBlocks) &&
            typedRegions == std::vector<uint32_t>({0, 1}) &&
            typedRequiredBlocks == std::vector<uint32_t>({0}),
        "typed Root derives complete Region/Block closure");
  std::vector<uint8_t> typedOutput;
  std::vector<uint32_t> typedOccurrences;
  check(typedStore.reconstruct_typed_staged(
            typedRoot, &typedBlocks, typedOutput, typedOccurrences) &&
            typedOutput == std::vector<uint8_t>({'b', 'c', 'a', 'b', 'c'}) &&
            typedOccurrences == std::vector<uint32_t>({1, 0, 1}),
        "typed Root expands exact Region and Block order");
  std::vector<uint8_t> invalidTypedRoot;
  put_varint(invalidTypedRoot, uint64_t(2) << 1);
  check(!typedStore.typed_requirements(invalidTypedRoot, &typedBlocks,
                                       typedRegions, typedRequiredBlocks),
        "typed Root rejects an undefined ordinal");

  fprintf(stderr, "[1d] raw-weighted learning gates:\n");
  struct MetricRow {
    uint64_t raw = 0, wire = 0;
  };
  std::vector<MetricRow> learningCurve(20, {1000, 4});
  for (size_t index = 0; index < 4; ++index)
    learningCurve[index].wire = 10;
  learningCurve[5].wire = 10;
  auto learning = summarize_curve(learningCurve);
  check(learning.c50_tu == 10 && learning.c50 > 142.8 &&
            learning.c50 < 142.9 && learning.second_half == 250.0,
        "C50 and second-half use raw-byte boundary");
  check(learning.h200_tu == 7 && learning.h200 == 0.35,
        "H200 rejects a transient hit and requires following 10% persistence");
  for (auto &row : learningCurve)
    row.wire = 10;
  learning = summarize_curve(learningCurve);
  check(learning.h200_tu == UINT32_MAX && learning.h200 < 0,
        "H200 remains absent when the trailing 5% never reaches 200x");

  fprintf(stderr, "[2] bounded stores and compaction:\n");
  const std::string source = "# 1 \"a.hpp\"\nalpha\n# 2 \"b.hpp\"\nbeta beta\n";
  Interner dict;
  std::vector<uint32_t> ids(source.size() + 1), regions;
  size_t idCount = 0;
  uint64_t hits = 0;
  dict.process(source.data(), source.data() + source.size(), ids.data(),
               idCount, hits, true, &regions);
  check(regions.size() == 2, "two Region fixture");
  FStore store;
  store.init(uint32_t(dict.region_count()), 2);
  check(install_raw_region(store, dict, regions[0]) &&
            install_raw_region(store, dict, regions[1]),
        "raw cache preload exact");
  store.Fpublic_next = 3;
  store.FpublicBytes.resize(3);
  store.FpublicPresent.resize(3, 0);
  store.FpublicLastUse.resize(3, 0);
  store.FpublicBytes[1] = {'a', 'a', 'a'};
  store.FpublicBytes[2] = {'b', 'b', 'b'};
  store.FpublicPresent[1] = store.FpublicPresent[2] = 1;
  store.FpublicLastUse[1] = 1;
  store.FpublicLastUse[2] = 2;
  store.publicHeld = 2;
  store.FknownBlk[0] = store.FknownBlk[1] = 1;
  store.FblkChildren[0] = {regions[0]};
  store.FblkChildren[1] = {regions[1]};
  CacheLimits limits;
  limits.region_bytes = dict.region_raw_len(regions[1]);
  limits.public_bytes = 3;
  limits.block_bytes = 4;
  limits.compact_slack = 0;
  CacheState cache;
  cache.init(store.NREG, store.NBLK, limits);
  std::vector<uint64_t> regionTicks(store.NREG), blockTicks(store.NBLK);
  regionTicks[regions[0]] = 1;
  regionTicks[regions[1]] = 2;
  blockTicks[0] = 1;
  blockTicks[1] = 2;
  cache.rebuild(store, regionTicks, blockTicks);
  uint64_t boundedPolicyBytes = cache.semantic_bytes();
  FStore::BlockTransaction noBlocks;
  FStore::FillTransaction publicTouch;
  publicTouch.public_touches = {1, 2};
  for (uint64_t tick = 3; tick < 1003; ++tick) {
    store.FpublicLastUse[1] = store.FpublicLastUse[2] = uint32_t(tick);
    cache.account_fill(store, {}, noBlocks, publicTouch, tick);
    cache.touch_committed(store, regions, {0, 1}, tick);
  }
  check(cache.semantic_bytes() == boundedPolicyBytes,
        "repeated touches do not grow policy memory");
  auto drops = cache.evict(store);
  check(drops.regions.size() == 1 && drops.regions[0] == regions[0] &&
            !store.FmixedRegions[regions[0]].known,
        "oldest Region removed");
  check(drops.public_lines == std::vector<uint32_t>{1} &&
            !store.FpublicPresent[1],
        "oldest public Line removed");
  check(drops.blocks == std::vector<uint32_t>{0} && !store.FknownBlk[0],
        "oldest Block removed");
  check(cache.totals.dead_region_bytes == 0 && cache.totals.compactions == 1,
        "dead Region arena compacted");
  check(store.FmixedRegions[regions[1]].offset == 0,
        "live Region view rewritten by compaction");

  fprintf(stderr, "[3] C snapshot round trip:\n");
  cap::SourceGeneration generation{};
  for (size_t i = 0; i < generation.size(); ++i)
    generation[i] = uint8_t(i + 1);
  encoder.mixedCLine[1] = {2, 7, 19};
  encoder.nextMixedPublic = 23;
  encoder.paths = {"a.hpp", "b.hpp"};
  encoder.pathid = {{"a.hpp", 0}, {"b.hpp", 1}};
  encoder.mixedOps[2] = 11;
  first.path_count = 2;
  first.public_lines.resize(23);
  first.public_lines[19] = 1;
  std::vector<ReceiverMirror> mirrors{first, second};
  std::string base = "/tmp/cap-m5-state-test-" + std::to_string(getpid());
  check(save_c_snapshot(base + ".c", generation, encoder, mirrors),
        "C snapshot written atomically");
  MixedEncoder loadedEncoder;
  std::vector<ReceiverMirror> loadedMirrors;
  check(load_c_snapshot(base + ".c", generation, loadedEncoder, loadedMirrors,
                        3, 2, 4),
        "C snapshot loaded");
  check(loadedEncoder.nextMixedPublic == 23 &&
            loadedEncoder.mixedCLine[1].public_id == 19 &&
            loadedEncoder.paths == encoder.paths,
        "C authority restored");
  check(loadedMirrors.size() == 2 && loadedMirrors[0].public_lines[19] &&
            loadedMirrors[1].regions[2],
        "per-F mirrors restored");
  auto wrongGeneration = generation;
  wrongGeneration[0] ^= 0xff;
  check(!load_c_snapshot(base + ".c", wrongGeneration, loadedEncoder,
                         loadedMirrors, 3, 2, 4),
        "C snapshot from another generation rejected");
  check(write_partial_copy(base + ".c", base + ".c.partial") &&
            !load_c_snapshot(base + ".c.partial", generation, loadedEncoder,
                             loadedMirrors, 3, 2, 4),
        "partial C snapshot rejected");

  fprintf(stderr, "[4] F snapshot round trip:\n");
  CacheLimits unlimited;
  CacheState savedCache;
  savedCache.init(store.NREG, store.NBLK, unlimited);
  regionTicks.assign(store.NREG, 0);
  blockTicks.assign(store.NBLK, 0);
  regionTicks[regions[1]] = 9;
  blockTicks[1] = 10;
  savedCache.rebuild(store, regionTicks, blockTicks);
  savedCache.totals.region_removals = 17;
  savedCache.totals.public_removals = 19;
  savedCache.totals.block_removals = 23;
  savedCache.totals.compactions = 29;
  store.Fpaths = {"a.hpp", "b.hpp"};
  check(save_f_snapshot(base + ".f", generation, store, savedCache),
        "F snapshot written atomically");
  FStore loadedStore;
  CacheState loadedCache;
  check(load_f_snapshot(base + ".f", generation, loadedStore, loadedCache),
        "F snapshot loaded");
  check(loadedStore.FmixedRegions[regions[1]].known &&
            loadedStore.FpublicPresent[2] && loadedStore.FknownBlk[1],
        "F typed stores restored");
  const auto &view = loadedStore.FmixedRegions[regions[1]];
  check(view.length == dict.region_raw_len(regions[1]) &&
            !memcmp(loadedStore.FmixedRegionData.data() + view.offset,
                    dict.region_data(regions[1]), view.length),
        "F Region bytes exact after reload");
  check(loadedCache.totals.regions == 1 &&
            loadedCache.totals.public_lines == 1 &&
            loadedCache.totals.blocks == 1 &&
            loadedCache.totals.region_removals == 17 &&
            loadedCache.totals.public_removals == 19 &&
            loadedCache.totals.block_removals == 23 &&
            loadedCache.totals.compactions == 29,
        "F cache accounting rebuilt");
  check(!load_f_snapshot(base + ".f", wrongGeneration, loadedStore,
                         loadedCache),
        "F snapshot from another generation rejected");
  check(write_partial_copy(base + ".f", base + ".f.partial") &&
            !load_f_snapshot(base + ".f.partial", generation, loadedStore,
                             loadedCache),
        "partial F snapshot rejected");

  unlink((base + ".c").c_str());
  unlink((base + ".c.partial").c_str());
  unlink((base + ".f").c_str());
  unlink((base + ".f.partial").c_str());
  printf("cap_m5_state_test: %s\n", failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}
