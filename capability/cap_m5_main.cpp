// icecream #16 — protocol-50 M5 scenario/mesh capability harness.
//
// M4's component bytes and transaction boundary are retained.  M5 adds one
// global C authority, independent persistent F stores, wave-pipelined actual
// sockets, explicit post-TU cache-removal feedback, chronological curves, order
// controls, CACHE50, worker restart/late join/failover, and bounded
// Region/public-Line/Block stores.
#include "cap_codec.h"
#include "cap_identity.h"
#include "cap_m5_metrics.h"
#include "cap_m5_state.h"
#include "cap_protocol.h"
#include "cap_transport.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <numeric>
#include <queue>
#include <random>
#include <set>
#include <string>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace capc;
using Clock = std::chrono::steady_clock;
static double seconds_since(Clock::time_point begin) {
  return std::chrono::duration<double>(Clock::now() - begin).count();
}

enum class OrderMode { Standard, Reverse, Shuffle, Novelty };
enum class AssignmentMode { RoundRobin, Sticky, Random, Failover };
enum ComponentKind {
  CK_ROOT = 0,
  CK_BLOCK = 1,
  CK_NEED = 2,
  CK_PATH = 3,
  CK_CONTROL = 4,
  CK_LITERAL = 5,
  CK_ARRAY_CONTROL = 6,
  CK_ARRAY_VALUES = 7,
  CK_COUNT = 8
};
static const char *const COMPONENT_NAMES[CK_COUNT] = {
    "root",           "block",   "need",          "path",
    "region_control", "raw_run", "array_control", "array_values"};

struct Options {
  const char *manifest = nullptr;
  size_t max_files = SIZE_MAX;
  uint32_t repetitions = 1, workers = 1, wave = 1;
  capp::CodecPolicy policy = capp::CodecPolicy::Zstd1;
  OrderMode order = OrderMode::Standard;
  AssignmentMode assignment = AssignmentMode::RoundRobin;
  uint64_t seed = 1;
  int cache50 = -1;
  capm5::CacheLimits cache_limits;
  uint32_t restart_at = UINT32_MAX, latejoin_at = UINT32_MAX,
           failover_at = UINT32_MAX, corrupt_tu = UINT32_MAX;
  bool real_pipes = false;
  const char *curve_out = nullptr;
  std::string snapshot_prefix;
};

struct FrameLedger {
  std::array<uint64_t, 7> bytes{}, count{};
  void note(cap::Frame frame, size_t payload) {
    size_t i = size_t(frame);
    bytes[i] += 4 + payload;
    ++count[i];
  }
  uint64_t total() const {
    return std::accumulate(bytes.begin(), bytes.end(), uint64_t(0));
  }
};
struct ComponentLedger {
  uint64_t raw = 0, selected = 0, z1_candidates = 0, z3_candidates = 0,
           raw_selected = 0, z1_selected = 0, z3_selected = 0, count = 0;
  void note(const capp::EncodedComponent &component) {
    raw += component.raw_size;
    selected += component.wire.size();
    ++count;
    if (component.z1_size != SIZE_MAX)
      z1_candidates += component.z1_size;
    if (component.z3_size != SIZE_MAX)
      z3_candidates += component.z3_size;
    if (component.codec == capp::ComponentCodec::Raw)
      ++raw_selected;
    else if (component.codec == capp::ComponentCodec::Zstd1)
      ++z1_selected;
    else
      ++z3_selected;
  }
  void received(const std::vector<uint8_t> &wire, size_t rawSize) {
    raw += rawSize;
    selected += wire.size();
    ++count;
    if (!wire.empty() && wire[0] == uint8_t(capp::ComponentCodec::Raw))
      ++raw_selected;
    else if (!wire.empty() && wire[0] == uint8_t(capp::ComponentCodec::Zstd1))
      ++z1_selected;
    else
      ++z3_selected;
  }
};
struct CodecContexts {
  ZSTD_CCtx *z1 = ZSTD_createCCtx(), *z3 = ZSTD_createCCtx();
  ZSTD_DCtx *decoder = ZSTD_createDCtx();
  ~CodecContexts() {
    ZSTD_freeCCtx(z1);
    ZSTD_freeCCtx(z3);
    ZSTD_freeDCtx(decoder);
  }
};
static bool send_counted(int fd, cap::Frame frame,
                         const std::vector<uint8_t> &payload,
                         FrameLedger &ledger) {
  if (!cap::send_frame(fd, frame, payload))
    return false;
  ledger.note(frame, payload.size());
  return true;
}
static bool recv_counted(int fd, cap::Frame &frame,
                         std::vector<uint8_t> &payload, FrameLedger &ledger) {
  if (!cap::recv_frame(fd, frame, payload))
    return false;
  ledger.note(frame, payload.size());
  return true;
}
static capp::EncodedComponent
encode_named(const std::vector<uint8_t> &raw, capp::CodecPolicy policy,
             CodecContexts &codec, ComponentLedger &ledger, bool blob = true) {
  auto value = capp::encode_component(raw, policy, codec.z1, codec.z3, blob);
  ledger.note(value);
  return value;
}
static cap::SourceGeneration make_generation() {
  cap::SourceGeneration result{};
  std::random_device source;
  for (uint8_t &value : result)
    value = uint8_t(source());
  if (std::all_of(result.begin(), result.end(),
                  [](uint8_t value) { return value == 0; }))
    result[0] = 1;
  return result;
}
static void enlarge_pipe(int fd) {
#ifdef F_SETPIPE_SZ
  (void)fcntl(fd, F_SETPIPE_SZ, 1 << 20);
#else
  (void)fd;
#endif
}

struct PreparationTiming {
  double manifest = 0, source_pipe = 0, interning = 0, ordering = 0,
         factorization = 0;
};

// Feed the preprocessed byte stream through a real pipe from a separate
// producer process.  TU sizes come from the job metadata, just as they do on a
// real submitter connection; the C side receives no direct file mapping.
static bool load_corpus_via_pipe(const char *manifest, size_t maxFiles,
                                 Corpus &corpus, Interner &dict,
                                 std::vector<std::vector<uint32_t>> &regions,
                                 PreparationTiming &timing) {
  auto manifestBegin = Clock::now();
  FILE *input = fopen(manifest, "r");
  if (!input) {
    perror(manifest);
    return false;
  }
  std::vector<std::string> paths;
  std::vector<uint32_t> lengths;
  uint64_t total = 0;
  char path[8192];
  while (fgets(path, sizeof path, input)) {
    size_t length = strlen(path);
    while (length && (path[length - 1] == '\n' || path[length - 1] == '\r'))
      path[--length] = 0;
    if (!length)
      continue;
    struct stat status {};
    if (stat(path, &status) != 0 || status.st_size < 0 ||
        uint64_t(status.st_size) > UINT32_MAX) {
      perror(path);
      fclose(input);
      return false;
    }
    paths.emplace_back(path);
    lengths.push_back(uint32_t(status.st_size));
    total += uint64_t(status.st_size);
    if (paths.size() == maxFiles)
      break;
  }
  fclose(input);
  timing.manifest += seconds_since(manifestBegin);
  int descriptors[2];
  if (pipe(descriptors) != 0) {
    perror("pipe");
    return false;
  }
  enlarge_pipe(descriptors[1]);
  pid_t producer = fork();
  if (producer < 0) {
    perror("fork");
    close(descriptors[0]);
    close(descriptors[1]);
    return false;
  }
  if (producer == 0) {
    close(descriptors[0]);
    std::vector<uint8_t> buffer(1 << 20);
    for (const auto &source : paths) {
      FILE *file = fopen(source.c_str(), "rb");
      if (!file)
        _exit(2);
      for (;;) {
        size_t count = fread(buffer.data(), 1, buffer.size(), file);
        if (count && !cap::write_all(descriptors[1], buffer.data(), count))
          _exit(2);
        if (count != buffer.size()) {
          if (ferror(file))
            _exit(2);
          break;
        }
      }
      fclose(file);
    }
    close(descriptors[1]);
    _exit(0);
  }
  close(descriptors[1]);
  corpus = Corpus{};
  corpus.bytes.resize(size_t(total) + 64);
  corpus.files.reserve(paths.size());
  regions.resize(paths.size());
  uint32_t maximumLength =
      lengths.empty() ? 0 : *std::max_element(lengths.begin(), lengths.end());
  std::vector<uint32_t> lineIds(size_t(maximumLength) + 1);
  uint64_t hits = 0;
  uint64_t offset = 0;
  bool exact = true;
  for (size_t ordinal = 0; ordinal < lengths.size(); ++ordinal) {
    uint32_t length = lengths[ordinal];
    auto pipeBegin = Clock::now();
    if (length &&
        !cap::read_all(descriptors[0], corpus.bytes.data() + offset, length)) {
      exact = false;
      break;
    }
    timing.source_pipe += seconds_since(pipeBegin);
    corpus.files.push_back({offset, length});
    size_t lineCount = 0;
    const char *begin = corpus.bytes.data() + offset;
    auto internBegin = Clock::now();
    dict.process(begin, begin + length, lineIds.data(), lineCount, hits, true,
                 &regions[ordinal]);
    timing.interning += seconds_since(internBegin);
    offset += length;
  }
  close(descriptors[0]);
  int status = 0;
  waitpid(producer, &status, 0);
  corpus.raw = offset;
  return exact && offset == total && WIFEXITED(status) &&
         WEXITSTATUS(status) == 0;
}
static const char *policy_name(capp::CodecPolicy value) {
  switch (value) {
  case capp::CodecPolicy::Raw:
    return "raw";
  case capp::CodecPolicy::Zstd1:
    return "z1";
  case capp::CodecPolicy::Zstd3:
    return "z3";
  case capp::CodecPolicy::Best:
    return "best";
  }
  return "unknown";
}
static const char *order_name(OrderMode value) {
  switch (value) {
  case OrderMode::Standard:
    return "standard";
  case OrderMode::Reverse:
    return "reverse";
  case OrderMode::Shuffle:
    return "shuffle";
  case OrderMode::Novelty:
    return "novelty-max";
  }
  return "unknown";
}
static const char *assignment_name(AssignmentMode value) {
  switch (value) {
  case AssignmentMode::RoundRobin:
    return "roundrobin";
  case AssignmentMode::Sticky:
    return "sticky";
  case AssignmentMode::Random:
    return "random";
  case AssignmentMode::Failover:
    return "failover";
  }
  return "unknown";
}

static std::vector<uint8_t> build_need(const std::vector<uint32_t> &missing) {
  std::vector<uint8_t> raw;
  put_varint(raw, missing.size());
  for (uint32_t id : missing)
    put_varint(raw, id);
  put_varint(raw, 0);
  return raw;
}
static uint64_t region_token(uint32_t id) { return uint64_t(id) << 1; }
static uint64_t block_token(uint32_t id) {
  return (uint64_t(id) << 1) | 1;
}
static bool parse_need(const std::vector<uint8_t> &raw, uint32_t nreg,
                       std::vector<uint32_t> &missing) {
  const uint8_t *p = raw.data(), *e = p + raw.size();
  uint64_t count = 0;
  if (!get_varint_bounded(p, e, count) || count > nreg)
    return false;
  missing.clear();
  missing.reserve(size_t(count));
  std::vector<uint8_t> seen(nreg, 0);
  for (uint64_t i = 0; i < count; ++i) {
    uint32_t id = 0;
    if (!get_u32_bounded(p, e, id) || id >= nreg || seen[id])
      return false;
    seen[id] = 1;
    missing.push_back(id);
  }
  return get_varint_bounded(p, e, count) && count == 0 && p == e;
}
static bool derive_requirements(FStore &store, const std::vector<uint8_t> &root,
                                const FStore::BlockTransaction &blocks,
                                std::vector<uint32_t> &regions,
                                std::vector<uint32_t> &requiredBlocks) {
  return store.typed_requirements(root, &blocks, regions, requiredBlocks);
}

struct Factorized {
  std::vector<uint64_t> tokens;
  std::vector<uint32_t> block_children;
  std::vector<size_t> token_offsets{0}, block_offsets{0};
};
static Factorized
factor_sequence(const std::vector<std::vector<uint32_t>> &tuRegions,
                const std::vector<uint32_t> &sequence) {
  Factorized result;
  std::vector<uint32_t> stream;
  std::vector<size_t> tuOffsets{0};
  for (uint32_t physical : sequence) {
    const auto &values = tuRegions[physical];
    stream.insert(stream.end(), values.begin(), values.end());
    tuOffsets.push_back(stream.size());
  }
  constexpr uint32_t MIN_MATCH = 3, MAX_CHAIN = 64, HASH_BITS = 22;
  std::vector<uint32_t> head(size_t(1) << HASH_BITS, UINT32_MAX),
      previous(stream.size(), UINT32_MAX);
  std::unordered_map<uint64_t, uint32_t> dictionary;
  auto kgram = [&](size_t position) {
    uint64_t hash = 1469598103934665603ull;
    for (uint32_t j = 0; j < MIN_MATCH; ++j) {
      hash ^= stream[position + j];
      hash *= 1099511628211ull;
    }
    return (hash * 0x9e3779b97f4a7c15ull) >> (64 - HASH_BITS);
  };
  auto block = [&](const uint32_t *values, size_t length) {
    uint64_t hash = 1469598103934665603ull ^ (length * 0x100000001b3ull);
    for (size_t i = 0; i < length; ++i) {
      hash ^= values[i];
      hash *= 1099511628211ull;
    }
    auto found = dictionary.find(hash);
    if (found != dictionary.end()) {
      uint32_t id = found->second;
      if (result.block_offsets[id + 1] - result.block_offsets[id] == length &&
          !memcmp(result.block_children.data() + result.block_offsets[id],
                  values, length * sizeof(uint32_t)))
        return block_token(id);
    }
    uint32_t id = uint32_t(result.block_offsets.size() - 1);
    result.block_children.insert(result.block_children.end(), values,
                                 values + length);
    result.block_offsets.push_back(result.block_children.size());
    if (found == dictionary.end())
      dictionary.emplace(hash, id);
    return block_token(id);
  };
  for (size_t logical = 0; logical < sequence.size(); ++logical) {
    size_t begin = tuOffsets[logical], end = tuOffsets[logical + 1],
           position = begin;
    while (position < end) {
      size_t bestLength = 0;
      if (position + MIN_MATCH <= end) {
        uint32_t candidate = head[kgram(position)], chain = 0;
        while (candidate != UINT32_MAX && chain < MAX_CHAIN) {
          if (candidate < position) {
            size_t length = 0, maximum = std::min(end - position,
                                                  position - size_t(candidate));
            while (length < maximum &&
                   stream[candidate + length] == stream[position + length])
              ++length;
            if (length >= MIN_MATCH && length > bestLength) {
              bestLength = length;
              if (length == maximum)
                break;
            }
          }
          candidate = previous[candidate];
          ++chain;
        }
      }
      size_t step = 1;
      if (bestLength >= MIN_MATCH) {
        result.tokens.push_back(block(stream.data() + position, bestLength));
        step = bestLength;
      } else
        result.tokens.push_back(region_token(stream[position]));
      // Keep the scenario factorizer causal like the one-pass factorizer: a
      // k-gram may use bytes from this complete TU, but never the first
      // Regions of a future TU that only the batch preload can see.
      for (size_t i = position; i < position + step; ++i)
        if (i + MIN_MATCH <= end) {
          uint64_t hash = kgram(i);
          previous[i] = head[hash];
          head[hash] = uint32_t(i);
        }
      position += step;
    }
    result.token_offsets.push_back(result.tokens.size());
  }
  return result;
}

static std::vector<uint32_t>
make_order(OrderMode mode, uint64_t seed,
           const std::vector<std::vector<uint32_t>> &regions,
           const Interner &dict) {
  std::vector<uint32_t> order(regions.size());
  std::iota(order.begin(), order.end(), 0);
  if (mode == OrderMode::Reverse)
    std::reverse(order.begin(), order.end());
  else if (mode == OrderMode::Shuffle) {
    std::mt19937_64 random(seed);
    std::shuffle(order.begin(), order.end(), random);
  } else if (mode == OrderMode::Novelty) {
    std::vector<uint8_t> selected(order.size(), 0),
        seen(dict.region_count(), 0);
    std::vector<uint64_t> bound(order.size());
    using Candidate = std::pair<uint64_t, uint32_t>;
    std::priority_queue<Candidate> queue;
    for (uint32_t tu = 0; tu < regions.size(); ++tu) {
      std::set<uint32_t> unique(regions[tu].begin(), regions[tu].end());
      for (uint32_t id : unique)
        bound[tu] += dict.region_raw_len(id);
      queue.push({bound[tu], tu});
    }
    order.clear();
    order.reserve(regions.size());
    while (order.size() < regions.size()) {
      auto [upper, tu] = queue.top();
      queue.pop();
      if (selected[tu])
        continue;
      uint64_t gain = 0;
      std::set<uint32_t> unique(regions[tu].begin(), regions[tu].end());
      for (uint32_t id : unique)
        if (!seen[id])
          gain += dict.region_raw_len(id);
      if (queue.empty() || gain >= queue.top().first) {
        selected[tu] = 1;
        order.push_back(tu);
        for (uint32_t id : unique)
          seen[id] = 1;
      } else
        queue.push({gain, tu});
    }
  }
  return order;
}

struct WorkerSummary {
  bool exact = false;
  uint64_t verified = 0, raw = 0, decode_ns = 0, path_ns = 0, failures = 0,
           wire_before_ack = 0, peak_rss_kib = 0, compiler_pipe_bytes = 0,
           compiler_pipe_tus = 0, compiler_pipe_ns = 0;
  capm5::CacheTotals cache;
};
static std::vector<uint8_t> pack_worker_summary(const WorkerSummary &value) {
  std::vector<uint8_t> out;
  out.push_back(value.exact ? 1 : 0);
  const std::array<uint64_t, 20> fields = {
      value.verified,
      value.raw,
      value.decode_ns,
      value.path_ns,
      value.failures,
      value.wire_before_ack,
      value.peak_rss_kib,
      value.compiler_pipe_bytes,
      value.compiler_pipe_tus,
      value.compiler_pipe_ns,
      value.cache.region_bytes,
      value.cache.public_bytes,
      value.cache.block_bytes,
      uint64_t(value.cache.regions),
      uint64_t(value.cache.public_lines),
      uint64_t(value.cache.blocks),
      value.cache.region_removals,
      value.cache.public_removals,
      value.cache.block_removals,
      value.cache.compactions,
  };
  for (uint64_t field : fields)
    put_u64le(out, field);
  return out;
}
static bool unpack_worker_summary(const std::vector<uint8_t> &payload,
                                  WorkerSummary &value) {
  constexpr size_t FIELD_COUNT = 20;
  if (payload.size() != 1 + FIELD_COUNT * sizeof(uint64_t) || payload[0] > 1)
    return false;
  value.exact = payload[0] != 0;
  const uint8_t *p = payload.data() + 1, *e = payload.data() + payload.size();
  std::array<uint64_t, FIELD_COUNT> fields{};
  for (uint64_t &field : fields) {
    if (size_t(e - p) < sizeof(uint64_t))
      return false;
    for (unsigned byte = 0; byte < sizeof(uint64_t); ++byte)
      field |= uint64_t(*p++) << (8 * byte);
  }
  if (fields[13] > UINT32_MAX || fields[14] > UINT32_MAX ||
      fields[15] > UINT32_MAX || p != e)
    return false;
  value.verified = fields[0];
  value.raw = fields[1];
  value.decode_ns = fields[2];
  value.path_ns = fields[3];
  value.failures = fields[4];
  value.wire_before_ack = fields[5];
  value.peak_rss_kib = fields[6];
  value.compiler_pipe_bytes = fields[7];
  value.compiler_pipe_tus = fields[8];
  value.compiler_pipe_ns = fields[9];
  value.cache.region_bytes = fields[10];
  value.cache.public_bytes = fields[11];
  value.cache.block_bytes = fields[12];
  value.cache.regions = uint32_t(fields[13]);
  value.cache.public_lines = uint32_t(fields[14]);
  value.cache.blocks = uint32_t(fields[15]);
  value.cache.region_removals = fields[16];
  value.cache.public_removals = fields[17];
  value.cache.block_removals = fields[18];
  value.cache.compactions = fields[19];
  return true;
}

class CompilerPipe {
public:
  CompilerPipe() = default;
  CompilerPipe(const CompilerPipe &) = delete;
  CompilerPipe &operator=(const CompilerPipe &) = delete;
  ~CompilerPipe() { abandon(); }

  bool start(int relationshipFd, const Corpus &corpus,
             const std::vector<uint32_t> &sequence) {
    int descriptors[2], acknowledgements[2];
    if (pipe(descriptors) != 0)
      return false;
    if (pipe(acknowledgements) != 0) {
      close(descriptors[0]);
      close(descriptors[1]);
      return false;
    }
    enlarge_pipe(descriptors[1]);
    pid = fork();
    if (pid < 0) {
      close(descriptors[0]);
      close(descriptors[1]);
      close(acknowledgements[0]);
      close(acknowledgements[1]);
      return false;
    }
    if (pid == 0) {
      close(descriptors[1]);
      close(acknowledgements[0]);
      close(relationshipFd);
      for (;;) {
        uint8_t header[8];
        if (!cap::read_all(descriptors[0], header, sizeof header))
          _exit(2);
        uint32_t logical = 0, length = 0;
        for (unsigned i = 0; i < 4; ++i) {
          logical |= uint32_t(header[i]) << (8 * i);
          length |= uint32_t(header[4 + i]) << (8 * i);
        }
        if (logical == UINT32_MAX && length == 0)
          break;
        if (logical >= sequence.size())
          _exit(2);
        const auto &file = corpus.files[sequence[logical]];
        if (length != file.len)
          _exit(2);
        std::vector<uint8_t> bytes(std::min<uint32_t>(length, 1u << 20));
        uint32_t consumed = 0;
        while (consumed < length) {
          uint32_t chunk =
              std::min<uint32_t>(length - consumed, uint32_t(bytes.size()));
          if (!cap::read_all(descriptors[0], bytes.data(), chunk) ||
              memcmp(bytes.data(), corpus.bytes.data() + file.off + consumed,
                     chunk))
            _exit(2);
          consumed += chunk;
        }
        const uint8_t accepted = 1;
        if (!cap::write_all(acknowledgements[1], &accepted, 1))
          _exit(2);
      }
      close(descriptors[0]);
      close(acknowledgements[1]);
      _exit(0);
    }
    close(descriptors[0]);
    close(acknowledgements[1]);
    fd = descriptors[1];
    ackFd = acknowledgements[0];
    return true;
  }

  bool emit(uint32_t logical, const std::vector<uint8_t> &bytes) {
    if (fd < 0 || ackFd < 0 || bytes.size() > UINT32_MAX)
      return false;
    uint8_t header[8];
    uint32_t length = uint32_t(bytes.size());
    for (unsigned i = 0; i < 4; ++i) {
      header[i] = uint8_t(logical >> (8 * i));
      header[4 + i] = uint8_t(length >> (8 * i));
    }
    if (!cap::write_all(fd, header, sizeof header) ||
        (length && !cap::write_all(fd, bytes.data(), length)))
      return false;
    uint8_t accepted = 0;
    if (!cap::read_all(ackFd, &accepted, 1) || accepted != 1)
      return false;
    transferred += length;
    ++tus;
    return true;
  }

  bool finish() {
    if (fd < 0 || pid < 0)
      return false;
    uint8_t header[8];
    memset(header, 0, sizeof header);
    for (unsigned i = 0; i < 4; ++i)
      header[i] = 0xff;
    bool written = cap::write_all(fd, header, sizeof header);
    close(fd);
    fd = -1;
    close(ackFd);
    ackFd = -1;
    int status = 0;
    waitpid(pid, &status, 0);
    pid = -1;
    return written && WIFEXITED(status) && WEXITSTATUS(status) == 0;
  }

  uint64_t transferred = 0, tus = 0;

private:
  int fd = -1, ackFd = -1;
  pid_t pid = -1;
  void abandon() {
    if (fd >= 0) {
      close(fd);
      fd = -1;
    }
    if (ackFd >= 0) {
      close(ackFd);
      ackFd = -1;
    }
    if (pid >= 0) {
      int status = 0;
      waitpid(pid, &status, 0);
      pid = -1;
    }
  }
};

static int worker_loop(int fd, uint32_t workerId, const Corpus &corpus,
                       const Interner &dict,
                       const std::vector<uint32_t> &sequence,
                       const Options &options, bool preloadCache,
                       bool resumeSnapshot, const std::string &snapshotPath) {
  (void)workerId;
  CodecContexts codec;
  FrameLedger frames;
  cap::Frame frame;
  std::vector<uint8_t> payload;
  if (!recv_counted(fd, frame, payload, frames) || frame != cap::Frame::Hello)
    return 2;
  cap::SourceGeneration generation{};
  uint32_t helloNreg = 0, helloNblk = 0, physicalTus = 0, repetitions = 0;
  if (!capp::try_unpack_hello_m4(payload, generation, helloNreg, helloNblk,
                                 physicalTus, repetitions) ||
      helloNreg != 0 || helloNblk != 0 ||
      physicalTus != corpus.files.size() || repetitions != options.repetitions ||
      dict.region_count() > UINT32_MAX)
    return 2;
  const uint32_t availableNreg = uint32_t(dict.region_count());
  FStore store;
  capm5::CacheState cache;
  if (resumeSnapshot) {
    if (snapshotPath.empty() ||
        !capm5::load_f_snapshot(snapshotPath, generation, store, cache))
      return 2;
  } else {
    store.init(0, 0);
    cache.init(0, 0, options.cache_limits);
  }
  if (!resumeSnapshot && preloadCache && options.cache50 >= 0) {
    store.ensure_dimensions(availableNreg, 0);
    cache.ensure_dimensions(availableNreg, 0);
    for (uint32_t id = 0; id < availableNreg; ++id)
      if (int(id & 1) == options.cache50) {
        if (!capm5::install_raw_region(store, dict, id))
          return 2;
        cache.account_preloaded_region(store, id);
      }
  }
  CompilerPipe compilerPipe;
  if (options.real_pipes && !compilerPipe.start(fd, corpus, sequence))
    return 2;
  uint64_t verified = 0, verifiedRaw = 0, failures = 0;
  double decodeSeconds = 0, pathSeconds = 0, compilerPipeSeconds = 0;
  bool exact = true;
  std::vector<uint8_t> reconstructed;
  std::vector<uint32_t> occurrences;
  for (;;) {
    if (!recv_counted(fd, frame, payload, frames))
      return 2;
    if (frame == cap::Frame::Done) {
      if (payload.size() > 1 || (!payload.empty() && payload[0] != 1))
        return 2;
      if (!payload.empty() &&
          (snapshotPath.empty() ||
           !capm5::save_f_snapshot(snapshotPath, generation, store, cache)))
        exact = false;
      if (options.real_pipes && !compilerPipe.finish())
        exact = false;
      struct rusage usage {};
      getrusage(RUSAGE_SELF, &usage);
      WorkerSummary summary;
      summary.exact = exact;
      summary.verified = verified;
      summary.raw = verifiedRaw;
      summary.decode_ns = uint64_t(decodeSeconds * 1e9);
      summary.path_ns = uint64_t(pathSeconds * 1e9);
      summary.failures = failures;
      summary.wire_before_ack = frames.total();
      summary.peak_rss_kib = uint64_t(usage.ru_maxrss);
      summary.compiler_pipe_bytes = compilerPipe.transferred;
      summary.compiler_pipe_tus = compilerPipe.tus;
      summary.compiler_pipe_ns = uint64_t(compilerPipeSeconds * 1e9);
      summary.cache = cache.totals;
      return send_counted(fd, cap::Frame::Ack, pack_worker_summary(summary),
                          frames)
                 ? (exact ? 0 : 1)
                 : 2;
    }
    if (frame == cap::Frame::Rejoin) {
      uint32_t resume = 0;
      if (!capp::try_unpack_rejoin(payload, resume))
        return 2;
      (void)resume;
      store.reset_store(1);
      cache.init(store.NREG, store.NBLK, options.cache_limits);
      capp::CacheDropsM5 none;
      if (!send_counted(fd, cap::Frame::Ack,
                        capp::pack_tu_ack_m5(resume, true, none), frames))
        return 2;
      continue;
    }
    if (frame != cap::Frame::Root)
      return 2;
    auto pathBegin = Clock::now();
    uint32_t logical = 0;
    std::vector<uint8_t> rootWire, blockWire, rootRaw, blockRaw;
    auto decodeBegin = Clock::now();
    auto reject = [&](FStore::FillTransaction *fill,
                      const char *reason) -> bool {
      if (fill && fill->active)
        store.rollback_fill(*fill);
      decodeSeconds += seconds_since(decodeBegin);
      pathSeconds += seconds_since(pathBegin);
      ++failures;
      fprintf(stderr, "F%u M5 reject TU %u (%s)\n", workerId, logical, reason);
      capp::CacheDropsM5 none;
      return send_counted(fd, cap::Frame::Ack,
                          capp::pack_tu_ack_m5(logical, false, none), frames);
    };
    if (!capp::try_unpack_root_m4(payload, logical, rootWire, blockWire) ||
        logical >= sequence.size() ||
        !capp::decode_component(rootWire, codec.decoder, rootRaw) ||
        !capp::decode_component(blockWire, codec.decoder, blockRaw)) {
      if (!reject(nullptr, "Root/component"))
        return 2;
      continue;
    }
    uint32_t requiredNreg = 0, requiredNblk = 0;
    if (!capp::try_scan_typed_dimensions(rootRaw, blockRaw, requiredNreg,
                                         requiredNblk)) {
      if (!reject(nullptr, "typed dimensions"))
        return 2;
      continue;
    }
    store.ensure_dimensions(requiredNreg, requiredNblk);
    cache.ensure_dimensions(requiredNreg, requiredNblk);
    FStore::BlockTransaction blockTx;
    if (!store.stage_blocks(blockRaw, blockTx)) {
      if (!reject(nullptr, "Block"))
        return 2;
      continue;
    }
    std::vector<uint32_t> requiredRegions, requiredBlocks, missing;
    if (!derive_requirements(store, rootRaw, blockTx, requiredRegions,
                             requiredBlocks)) {
      if (!reject(nullptr, "closure"))
        return 2;
      continue;
    }
    for (uint32_t id : requiredRegions)
      if (!store.FmixedRegions[id].known)
        missing.push_back(id);
    decodeSeconds += seconds_since(decodeBegin);
    auto need = capp::encode_component(build_need(missing), options.policy,
                                       codec.z1, codec.z3, true);
    if (!send_counted(fd, cap::Frame::Need,
                      capp::pack_need_m4(logical, need.wire), frames))
      return 2;
    if (!recv_counted(fd, frame, payload, frames) || frame != cap::Frame::Fill)
      return 2;
    decodeBegin = Clock::now();
    uint32_t fillTU = 0, pathBase = 0, publicBase = 0;
    std::vector<uint8_t> pathWire, pathRaw;
    std::array<std::vector<uint8_t>, 6> mixedWire, mixedRaw;
    bool components =
        capp::try_unpack_fill_m4(payload, fillTU, pathBase, publicBase,
                                 pathWire, mixedWire, 4) &&
        fillTU == logical &&
        capp::decode_component(pathWire, codec.decoder, pathRaw);
    if (components)
      for (size_t part = 0; part < 4; ++part)
        if (!capp::decode_component(mixedWire[part], codec.decoder,
                                    mixedRaw[part])) {
          components = false;
          break;
        }
    if (!components) {
      if (!reject(nullptr, "Fill/component"))
        return 2;
      continue;
    }
    FStore::FillTransaction fillTx;
    if (!store.stage_fill(mixedRaw, missing, pathRaw, pathBase, publicBase,
                          logical, fillTx)) {
      if (!reject(nullptr, "Fill"))
        return 2;
      continue;
    }
    const auto &file = corpus.files[sequence[logical]];
    reconstructed.reserve(file.len);
    bool expanded = store.reconstruct_typed_staged(
        rootRaw, &blockTx, reconstructed, occurrences);
    const char *original = corpus.bytes.data() + file.off;
    if (!expanded || reconstructed.size() != file.len ||
        (!options.real_pipes && file.len &&
         memcmp(reconstructed.data(), original, file.len))) {
      if (!reject(&fillTx, "exact expansion"))
        return 2;
      continue;
    }
    // A successful decode is only PREPARED here.  C may still have to abort
    // this TU when an earlier authority transaction in the same wave fails.
    // Do not commit cache state or expose bytes to the compiler pipe until C
    // sends the ordered transaction decision.
    decodeSeconds += seconds_since(decodeBegin);
    capp::CacheDropsM5 none;
    if (!send_counted(fd, cap::Frame::Ack,
                      capp::pack_tu_ack_m5(logical, true, none), frames))
      return 2;
    bool commit = false;
    uint32_t decisionTU = 0;
    if (!recv_counted(fd, frame, payload, frames) || frame != cap::Frame::Ack ||
        !capp::try_unpack_tu_ack(payload, decisionTU, commit) ||
        decisionTU != logical)
      return 2;
    if (!commit) {
      store.rollback_fill(fillTx);
      pathSeconds += seconds_since(pathBegin);
      if (!send_counted(fd, cap::Frame::Ack,
                        capp::pack_tu_ack_m5(logical, false, none), frames))
        return 2;
      continue;
    }
    if (options.real_pipes) {
      auto pipeBegin = Clock::now();
      if (!compilerPipe.emit(logical, reconstructed))
        return 2;
      compilerPipeSeconds += seconds_since(pipeBegin);
    }
    // Keep decoder/cache CPU distinct from time blocked on the downstream
    // compiler pipe.  The pipe has its own clock and gate below.
    auto commitBegin = Clock::now();
    store.commit_fill(fillTx);
    store.commit_blocks(blockTx);
    cache.account_fill(store, missing, blockTx, fillTx, uint64_t(logical) + 1);
    cache.touch_committed(store, occurrences, requiredBlocks,
                          uint64_t(logical) + 1);
    auto drops = cache.evict(store);
    decodeSeconds += seconds_since(commitBegin);
    pathSeconds += seconds_since(pathBegin);
    if (!send_counted(fd, cap::Frame::Ack,
                      capp::pack_tu_ack_m5(logical, true, drops), frames))
      return 2;
    ++verified;
    verifiedRaw += file.len;
  }
}

struct Worker {
  pid_t pid = -1;
  int fd = -1;
  bool active = false;
  bool summary_lost = false;
  bool session_cache_cumulative = false;
  uint32_t completed_sessions = 0;
  uint64_t session_start = 0, pending_curve_bytes = 0, accepted = 0,
           accepted_raw = 0;
  std::string snapshot_path;
  FrameLedger frames;
  capm5::ReceiverMirror mirror;
  WorkerSummary accumulated;
};

static bool spawn_worker(Worker &worker, uint32_t id, std::vector<Worker> &all,
                         const Corpus &corpus, const Interner &dict,
                         const std::vector<uint32_t> &sequence, uint32_t nreg,
                         uint32_t nblk, const Options &options,
                         const cap::SourceGeneration &generation, bool preload,
                         bool resumeSnapshot = false) {
  int sockets[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
    perror("socketpair");
    return false;
  }
  pid_t child = fork();
  if (child < 0) {
    perror("fork");
    close(sockets[0]);
    close(sockets[1]);
    return false;
  }
  if (child == 0) {
    for (const auto &other : all)
      if (other.fd >= 0)
        close(other.fd);
    close(sockets[0]);
    int result = worker_loop(sockets[1], id, corpus, dict, sequence, options,
                             preload, resumeSnapshot, worker.snapshot_path);
    close(sockets[1]);
    _exit(result);
  }
  close(sockets[1]);
  worker.pid = child;
  worker.fd = sockets[0];
  worker.active = true;
  worker.session_cache_cumulative = resumeSnapshot;
  worker.session_start = worker.frames.total();
  if (!resumeSnapshot)
    worker.mirror.init(nreg, nblk);
  if (!resumeSnapshot && preload && options.cache50 >= 0)
    for (uint32_t region = 0; region < nreg; ++region)
      if (int(region & 1) == options.cache50)
        worker.mirror.regions[region] = 1;
  uint64_t before = worker.frames.total();
  auto hello = capp::pack_hello_m4(generation, 0, 0,
                                   uint32_t(corpus.files.size()),
                                   options.repetitions);
  if (!send_counted(worker.fd, cap::Frame::Hello, hello, worker.frames))
    return false;
  worker.pending_curve_bytes += worker.frames.total() - before;
  return true;
}

static bool finish_worker(Worker &worker, bool saveSnapshot = false) {
  if (!worker.active)
    return true;
  const uint64_t controlStart = worker.frames.total();
  const std::vector<uint8_t> done =
      saveSnapshot ? std::vector<uint8_t>{1} : std::vector<uint8_t>{};
  if (!send_counted(worker.fd, cap::Frame::Done, done, worker.frames))
    return false;
  uint64_t beforeAck = worker.frames.total() - worker.session_start;
  cap::Frame frame;
  std::vector<uint8_t> payload;
  WorkerSummary summary;
  if (!recv_counted(worker.fd, frame, payload, worker.frames) ||
      frame != cap::Frame::Ack || !unpack_worker_summary(payload, summary) ||
      summary.wire_before_ack != beforeAck)
    return false;
  if (worker.completed_sessions == 0)
    worker.accumulated.exact = summary.exact;
  else
    worker.accumulated.exact = worker.accumulated.exact && summary.exact;
  ++worker.completed_sessions;
  worker.accumulated.verified += summary.verified;
  worker.accumulated.raw += summary.raw;
  worker.accumulated.decode_ns += summary.decode_ns;
  worker.accumulated.path_ns += summary.path_ns;
  worker.accumulated.failures += summary.failures;
  worker.accumulated.compiler_pipe_bytes += summary.compiler_pipe_bytes;
  worker.accumulated.compiler_pipe_tus += summary.compiler_pipe_tus;
  worker.accumulated.compiler_pipe_ns += summary.compiler_pipe_ns;
  worker.accumulated.peak_rss_kib =
      std::max(worker.accumulated.peak_rss_kib, summary.peak_rss_kib);
  const uint64_t priorRegionRemovals = worker.accumulated.cache.region_removals;
  const uint64_t priorPublicRemovals = worker.accumulated.cache.public_removals;
  const uint64_t priorBlockRemovals = worker.accumulated.cache.block_removals;
  const uint64_t priorCompactions = worker.accumulated.cache.compactions;
  worker.accumulated.cache = summary.cache;
  // A resumed F snapshot already contains the preceding session's cache
  // counters.  A newly initialized/restarted F does not.
  if (!worker.session_cache_cumulative) {
    worker.accumulated.cache.region_removals += priorRegionRemovals;
    worker.accumulated.cache.public_removals += priorPublicRemovals;
    worker.accumulated.cache.block_removals += priorBlockRemovals;
    worker.accumulated.cache.compactions += priorCompactions;
  }
  close(worker.fd);
  worker.fd = -1;
  int status = 0;
  waitpid(worker.pid, &status, 0);
  worker.pid = -1;
  worker.active = false;
  worker.pending_curve_bytes += worker.frames.total() - controlStart;
  return summary.exact && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool force_stop_worker(Worker &worker) {
  if (!worker.active)
    return true;
  kill(worker.pid, SIGTERM);
  int status = 0;
  waitpid(worker.pid, &status, 0);
  close(worker.fd);
  worker.fd = -1;
  worker.pid = -1;
  worker.active = false;
  worker.summary_lost = true;
  return true;
}

static bool resync_worker(Worker &worker, uint32_t logical) {
  if (!send_counted(worker.fd, cap::Frame::Rejoin, capp::pack_rejoin(logical),
                    worker.frames))
    return false;
  cap::Frame frame;
  std::vector<uint8_t> payload;
  capp::CacheDropsM5 drops;
  uint32_t reply = 0;
  bool accepted = false;
  return recv_counted(worker.fd, frame, payload, worker.frames) &&
         frame == cap::Frame::Ack &&
         capp::try_unpack_tu_ack_m5(payload, reply, accepted, drops,
                                    uint32_t(worker.mirror.regions.size()),
                                    uint32_t(worker.mirror.blocks.size())) &&
         reply == logical && accepted;
}

struct CurveRow {
  uint32_t logical = 0, physical = 0, worker = 0;
  uint64_t raw = 0, wire = 0, latency_ns = 0;
};
struct Pending {
  uint32_t logical = 0, worker = 0, physical = 0;
  uint64_t wire_start = 0;
  std::vector<uint32_t> missing;
  MixedEncoder::AuthorityTransaction authority;
  bool authority_started = false;
  Clock::time_point started;
};

static std::vector<uint32_t>
make_assignments(const Options &options,
                 const std::vector<uint32_t> &sequence) {
  std::vector<uint32_t> result(sequence.size());
  uint32_t workers = options.workers;
  std::mt19937_64 random(options.seed ^ 0x9e3779b97f4a7c15ull);
  if (options.assignment == AssignmentMode::Random) {
    std::vector<uint32_t> permutation(workers);
    std::iota(permutation.begin(), permutation.end(), 0);
    for (size_t base = 0; base < result.size(); base += workers) {
      std::shuffle(permutation.begin(), permutation.end(), random);
      for (size_t i = 0; i < workers && base + i < result.size(); ++i)
        result[base + i] = permutation[i];
    }
  } else
    for (size_t i = 0; i < result.size(); ++i) {
      if (options.assignment == AssignmentMode::RoundRobin)
        result[i] = uint32_t(i % workers);
      else
        result[i] = uint32_t(
            mix64(uint64_t(sequence[i]) + 0xa0761d6478bd642full) % workers);
    }
  if (options.assignment == AssignmentMode::Failover && workers > 1) {
    uint32_t point = options.failover_at == UINT32_MAX
                         ? uint32_t(result.size() / 2)
                         : options.failover_at;
    for (size_t i = point; i < result.size(); ++i)
      if (result[i] == 0)
        result[i] = 1;
  }
  if (options.latejoin_at != UINT32_MAX)
    for (size_t i = 0; i < std::min<size_t>(options.latejoin_at, result.size());
         ++i)
      result[i] = 0;
  return result;
}

static int run_coordinator(const Corpus &corpus, const Interner &dict,
                           const std::vector<uint32_t> &sequence,
                           const Factorized &factor, const Options &options,
                           Clock::time_point completeStart,
                           double preparationSeconds) {
  uint32_t nreg = uint32_t(dict.region_count()),
           nblk = uint32_t(factor.block_offsets.size() - 1);
  cap::SourceGeneration generation = make_generation();
  MixedEncoder encoder;
  encoder.init(dict.distinct(), nreg);
  CodecContexts codec;
  std::array<ComponentLedger, CK_COUNT> components{};
  std::vector<Worker> workers(options.workers);
  for (uint32_t id = 0; id < workers.size(); ++id) {
    workers[id].mirror.init(nreg, nblk);
    if (!options.snapshot_prefix.empty())
      workers[id].snapshot_path =
          options.snapshot_prefix + ".f" + std::to_string(id) + ".bin";
  }
  uint64_t expectedRaw = 0;
  for (uint32_t physical : sequence)
    expectedRaw += corpus.files[physical].len;
  size_t snapshotBoundary = SIZE_MAX;
  uint64_t snapshotRaw = 0;
  if (!options.snapshot_prefix.empty()) {
    const uint64_t halfRaw = (expectedRaw + 1) / 2;
    for (size_t i = 0; i < sequence.size(); ++i) {
      snapshotRaw += corpus.files[sequence[i]].len;
      if (snapshotRaw >= halfRaw) {
        snapshotBoundary = i + 1;
        break;
      }
    }
  }
  uint32_t initial = options.latejoin_at == UINT32_MAX ? options.workers : 1;
  for (uint32_t id = 0; id < initial; ++id)
    if (!spawn_worker(workers[id], id, workers, corpus, dict, sequence, nreg,
                      nblk, options, generation, true))
      return 2;
  auto assignments = make_assignments(options, sequence);
  std::vector<CurveRow> curve;
  curve.reserve(sequence.size());
  std::vector<uint8_t> fillAttempts(sequence.size(), 0);
  std::vector<Clock::time_point> logicalStarted(sequence.size());
  uint64_t rawTotal = 0, socketFailures = 0, preparedAccepted = 0,
           decodeRejected = 0, transactionCommitted = 0,
           transactionAborted = 0;
  double encodeSeconds = 0;
  bool exact = true, lateJoined = initial == options.workers, restarted = false,
       failedOver = false, snapshotted = options.snapshot_prefix.empty();
  auto wallStart = Clock::now();
  size_t cursor = 0;
  while (cursor < sequence.size() || !snapshotted) {
    if (!snapshotted && cursor == snapshotBoundary) {
      std::vector<uint8_t> wasActive(workers.size(), 0);
      for (uint32_t id = 0; id < workers.size(); ++id) {
        wasActive[id] = workers[id].active ? 1 : 0;
        if (workers[id].active && !finish_worker(workers[id], true))
          return 2;
      }
      std::vector<capm5::ReceiverMirror> mirrors;
      mirrors.reserve(workers.size());
      for (const auto &worker : workers)
        mirrors.push_back(worker.mirror);
      const std::string cSnapshot = options.snapshot_prefix + ".c.bin";
      if (!capm5::save_c_snapshot(cSnapshot, generation, encoder, mirrors))
        return 2;
      MixedEncoder restoredEncoder;
      std::vector<capm5::ReceiverMirror> restoredMirrors;
      if (!capm5::load_c_snapshot(cSnapshot, generation, restoredEncoder,
                                  restoredMirrors, nreg, nblk,
                                  dict.distinct()) ||
          restoredMirrors.size() != workers.size())
        return 2;
      encoder = std::move(restoredEncoder);
      for (uint32_t id = 0; id < workers.size(); ++id) {
        workers[id].mirror = std::move(restoredMirrors[id]);
        if (wasActive[id] &&
            !spawn_worker(workers[id], id, workers, corpus, dict, sequence,
                          nreg, nblk, options, generation, false, true))
          return 2;
      }
      snapshotted = true;
      fprintf(stderr,
              "M5 snapshot/resume boundary=%zu raw=%llu fraction=%.6f\n",
              snapshotBoundary, (unsigned long long)snapshotRaw,
              expectedRaw ? double(snapshotRaw) / expectedRaw : 0.0);
    }
    if (cursor >= sequence.size())
      break;
    if (!lateJoined && cursor >= options.latejoin_at) {
      for (uint32_t id = 1; id < options.workers; ++id)
        if (!spawn_worker(workers[id], id, workers, corpus, dict, sequence,
                          nreg, nblk, options, generation, false))
          return 2;
      lateJoined = true;
    }
    if (!restarted && options.restart_at != UINT32_MAX &&
        cursor >= options.restart_at) {
      if (!finish_worker(workers[0]))
        return 2;
      workers[0].mirror.reset();
      if (!spawn_worker(workers[0], 0, workers, corpus, dict, sequence, nreg,
                        nblk, options, generation, false))
        return 2;
      restarted = true;
    }
    uint32_t failPoint = options.failover_at == UINT32_MAX
                             ? uint32_t(sequence.size() / 2)
                             : options.failover_at;
    if (options.assignment == AssignmentMode::Failover && !failedOver &&
        cursor >= failPoint) {
      force_stop_worker(workers[0]);
      workers[0].mirror.reset();
      failedOver = true;
    }

    std::vector<Pending> pending;
    std::set<uint32_t> used;
    size_t next = cursor;
    while (next < sequence.size() &&
           pending.size() < std::min<uint32_t>(options.wave, options.workers)) {
      if ((!lateJoined && next >= options.latejoin_at) ||
          (!snapshotted && next >= snapshotBoundary) ||
          (!restarted && options.restart_at != UINT32_MAX &&
           next >= options.restart_at) ||
          (options.assignment == AssignmentMode::Failover && !failedOver &&
           next >= failPoint))
        break;
      uint32_t worker = assignments[next];
      if (worker >= workers.size() || !workers[worker].active ||
          used.count(worker))
        break;
      used.insert(worker);
      pending.push_back({uint32_t(next),
                         worker,
                         sequence[next],
                         workers[worker].frames.total(),
                         {},
                         {},
                         false,
                         {}});
      ++next;
    }
    if (pending.empty())
      continue;
    auto send_root = [&](Pending &job) {
      auto &worker = workers[job.worker];
      auto &firstStarted = logicalStarted[job.logical];
      if (firstStarted == Clock::time_point{})
        firstStarted = Clock::now();
      job.started = firstStarted;
      auto encodeBegin = Clock::now();
      const uint64_t *tokens =
          factor.tokens.data() + factor.token_offsets[job.logical];
      size_t count = factor.token_offsets[job.logical + 1] -
                     factor.token_offsets[job.logical];
      std::vector<uint8_t> rootRaw;
      rootRaw.reserve(count * 2);
      std::vector<uint32_t> requiredBlocks;
      for (size_t i = 0; i < count; ++i) {
        put_varint(rootRaw, tokens[i]);
        if (tokens[i] & 1)
          requiredBlocks.push_back(uint32_t(tokens[i] >> 1));
      }
      std::sort(requiredBlocks.begin(), requiredBlocks.end());
      requiredBlocks.erase(
          std::unique(requiredBlocks.begin(), requiredBlocks.end()),
          requiredBlocks.end());
      std::vector<uint8_t> blockRaw;
      size_t definitionCount = 0;
      for (uint32_t block : requiredBlocks)
        definitionCount += worker.mirror.blocks[block] ? 0 : 1;
      if (definitionCount) {
        put_varint(blockRaw, definitionCount);
        for (uint32_t block : requiredBlocks)
          if (!worker.mirror.blocks[block]) {
            put_varint(blockRaw, block);
            blockRaw.push_back(0);
            size_t length =
                factor.block_offsets[block + 1] - factor.block_offsets[block];
            put_varint(blockRaw, length);
            for (size_t i = factor.block_offsets[block];
                 i < factor.block_offsets[block + 1]; ++i)
              put_varint(blockRaw, factor.block_children[i]);
            worker.mirror.blocks[block] = 1;
          }
      }
      auto root =
          encode_named(rootRaw, options.policy, codec, components[CK_ROOT]);
      auto blocks =
          encode_named(blockRaw, options.policy, codec, components[CK_BLOCK]);
      encodeSeconds += seconds_since(encodeBegin);
      return send_counted(
          worker.fd, cap::Frame::Root,
          capp::pack_root_m4(job.logical, root.wire, blocks.wire),
          worker.frames);
    };
    auto receive_need = [&](Pending &job, bool &ready) {
      auto &worker = workers[job.worker];
      cap::Frame frame;
      std::vector<uint8_t> payload, wire, raw;
      uint32_t logical = 0;
      if (!recv_counted(worker.fd, frame, payload, worker.frames))
        return false;
      if (frame == cap::Frame::Ack) {
        capp::CacheDropsM5 drops;
        bool accepted = true;
        if (!capp::try_unpack_tu_ack_m5(payload, logical, accepted, drops, nreg,
                                        nblk) ||
            logical != job.logical || accepted || !drops.regions.empty() ||
            !drops.public_lines.empty() || !drops.blocks.empty())
          return false;
        ready = false;
        return true;
      }
      if (frame != cap::Frame::Need ||
          !capp::try_unpack_need_m4(payload, logical, wire) ||
          logical != job.logical ||
          !capp::decode_component(wire, codec.decoder, raw) ||
          !parse_need(raw, nreg, job.missing))
        return false;
      components[CK_NEED].received(wire, raw.size());
      ready = true;
      return true;
    };
    auto establish_need = [&](Pending &job, bool rootAlreadySent) {
      for (unsigned attempt = 0; attempt < 3; ++attempt) {
        if (!rootAlreadySent && !send_root(job))
          return false;
        bool ready = false;
        if (!receive_need(job, ready))
          return false;
        if (ready)
          return true;
        ++socketFailures;
        auto &worker = workers[job.worker];
        worker.mirror.reset();
        if (!resync_worker(worker, job.logical))
          return false;
        rootAlreadySent = false;
      }
      return false;
    };

    // Pipeline Root/NEED over one job per F.  Fill transactions below are
    // deliberately independent so one rejected F cannot cause an accepted
    // peer TU to be replayed.
    for (auto &job : pending)
      if (!send_root(job))
        return 2;
    for (auto &job : pending)
      if (!establish_need(job, true))
        return 2;

    // Stack one authority transaction per Fill and put the complete wave on
    // independent F sockets before waiting for replies. Transactions form an
    // ordered tentative prefix: if every F accepts, commit in order. If one F
    // rejects, commit only the accepted prefix, roll the suffix back in
    // reverse, discard those F sessions, and retry only that suffix. Thus no
    // accepted peer TU is replayed and no F keeps state derived from
    // rolled-back C state.
    struct PreparedFill {
      uint32_t path_base = 0, public_base = 0;
      std::vector<uint8_t> paths;
      std::array<std::vector<uint8_t>, 6> mixed_raw;
      capp::EncodedComponent path;
      std::array<capp::EncodedComponent, 4> mixed;
      bool sent = false;
    };
    std::vector<PreparedFill> prepared(pending.size());
    auto fillWaveBegin = Clock::now();
    for (size_t pendingIndex = 0; pendingIndex < pending.size();
         ++pendingIndex) {
      auto &job = pending[pendingIndex];
      auto &worker = workers[job.worker];
      auto &fill = prepared[pendingIndex];
      if (++fillAttempts[job.logical] > 3)
        return 2;
      encoder.begin_authority_transaction(job.authority);
      job.authority_started = true;
      for (uint32_t region : job.missing)
        worker.mirror.regions[region] = 0;
      fill.path_base = worker.mirror.path_count;
      {
        capm5::MirrorScope scope(encoder, worker.mirror);
        if (!job.missing.empty())
          encoder.materialize(dict, job.missing, job.logical, &job.authority);
        else {
          for (auto &part : encoder.mixedRaw)
            part.clear();
          encoder.fill_paths.clear();
          encoder.fill_public_base = encoder.nextMixedPublic;
        }
      }
      fill.public_base = encoder.fill_public_base;
      if (fill.path_base > encoder.paths.size())
        return 2;
      for (size_t i = fill.path_base; i < encoder.paths.size(); ++i) {
        put_varint(fill.paths, encoder.paths[i].size());
        fill.paths.insert(fill.paths.end(), encoder.paths[i].begin(),
                          encoder.paths[i].end());
      }
      worker.mirror.path_count = uint32_t(encoder.paths.size());
      for (size_t part = 0; part < 4; ++part)
        fill.mixed_raw[part] = std::move(encoder.mixedRaw[part]);
    }
    std::vector<std::thread> fillThreads;
    fillThreads.reserve(pending.size());
    for (size_t pendingIndex = 0; pendingIndex < pending.size(); ++pendingIndex)
      fillThreads.emplace_back([&, pendingIndex] {
        auto &job = pending[pendingIndex];
        auto &worker = workers[job.worker];
        auto &fill = prepared[pendingIndex];
        CodecContexts localCodec;
        fill.path = capp::encode_component(fill.paths, options.policy,
                                           localCodec.z1, localCodec.z3, true);
        std::array<std::vector<uint8_t>, 6> mixedWire;
        for (size_t part = 0; part < 4; ++part) {
          fill.mixed[part] =
              capp::encode_component(fill.mixed_raw[part], options.policy,
                                     localCodec.z1, localCodec.z3, true);
          mixedWire[part] = fill.mixed[part].wire;
        }
        if (job.logical == options.corrupt_tu && fillAttempts[job.logical] == 1)
          mixedWire[0].push_back(0xff);
        fill.sent = send_counted(
            worker.fd, cap::Frame::Fill,
            capp::pack_fill_m4(job.logical, fill.path_base, fill.public_base,
                               fill.path.wire, mixedWire, 4),
            worker.frames);
      });
    for (auto &thread : fillThreads)
      thread.join();
    encodeSeconds += seconds_since(fillWaveBegin);
    for (const auto &fill : prepared) {
      if (!fill.sent)
        return 2;
      components[CK_PATH].note(fill.path);
      for (size_t part = 0; part < 4; ++part)
        components[CK_CONTROL + part].note(fill.mixed[part]);
    }

    struct PreparedReply {
      bool accepted = false;
    };
    std::vector<PreparedReply> preparedReplies(pending.size());
    size_t firstRejected = pending.size();
    for (size_t i = 0; i < pending.size(); ++i) {
      auto &job = pending[i];
      auto &worker = workers[job.worker];
      cap::Frame frame;
      std::vector<uint8_t> payload;
      capp::CacheDropsM5 drops;
      uint32_t logical = 0;
      if (!recv_counted(worker.fd, frame, payload, worker.frames) ||
          frame != cap::Frame::Ack ||
          !capp::try_unpack_tu_ack_m5(payload, logical,
                                      preparedReplies[i].accepted, drops,
                                      nreg, nblk) ||
          logical != job.logical || !drops.regions.empty() ||
          !drops.public_lines.empty() || !drops.blocks.empty())
        return 2;
      if (!preparedReplies[i].accepted) {
        firstRejected = std::min(firstRejected, i);
        ++decodeRejected;
        ++socketFailures;
      } else
        ++preparedAccepted;
    }

    // Complete the receiver-side transaction only after the ordered C prefix
    // is known.  Accepted jobs before the first rejection commit; any prepared
    // suffix jobs abort.  A job that rejected during decode has no staged
    // transaction and receives no decision.
    struct FinalReply {
      bool accepted = false;
      capp::CacheDropsM5 drops;
      Clock::time_point received;
    };
    std::vector<FinalReply> finalReplies(pending.size());
    for (size_t i = 0; i < pending.size(); ++i) {
      if (!preparedReplies[i].accepted)
        continue;
      bool commit = i < firstRejected;
      auto &job = pending[i];
      auto &worker = workers[job.worker];
      if (!send_counted(worker.fd, cap::Frame::Ack,
                        capp::pack_tu_ack(job.logical, commit), worker.frames))
        return 2;
    }
    for (size_t i = 0; i < pending.size(); ++i) {
      if (!preparedReplies[i].accepted)
        continue;
      bool expectedCommit = i < firstRejected;
      auto &job = pending[i];
      auto &worker = workers[job.worker];
      cap::Frame frame;
      std::vector<uint8_t> payload;
      uint32_t logical = 0;
      if (!recv_counted(worker.fd, frame, payload, worker.frames) ||
          frame != cap::Frame::Ack ||
          !capp::try_unpack_tu_ack_m5(payload, logical,
                                      finalReplies[i].accepted,
                                      finalReplies[i].drops, nreg, nblk) ||
          logical != job.logical ||
          finalReplies[i].accepted != expectedCommit ||
          (!expectedCommit && (!finalReplies[i].drops.regions.empty() ||
                               !finalReplies[i].drops.public_lines.empty() ||
                               !finalReplies[i].drops.blocks.empty())))
        return 2;
      finalReplies[i].received = Clock::now();
      if (expectedCommit)
        ++transactionCommitted;
      else
        ++transactionAborted;
    }

    auto commit_job = [&](size_t i) {
      auto &job = pending[i];
      auto &worker = workers[job.worker];
      encoder.commit_authority_transaction(job.authority);
      job.authority_started = false;
      worker.mirror.apply(finalReplies[i].drops);
      const auto &file = corpus.files[job.physical];
      uint64_t wire =
          worker.frames.total() - job.wire_start + worker.pending_curve_bytes;
      worker.pending_curve_bytes = 0;
      uint64_t latencyNs =
          uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                       finalReplies[i].received - job.started)
                       .count());
      curve.push_back(
          {job.logical, job.physical, job.worker, file.len, wire, latencyNs});
      rawTotal += file.len;
      ++worker.accepted;
      worker.accepted_raw += file.len;
    };
    for (size_t i = 0; i < firstRejected; ++i) {
      if (!preparedReplies[i].accepted || !finalReplies[i].accepted)
        return 2;
      commit_job(i);
    }
    if (firstRejected == pending.size()) {
      cursor = next;
      continue;
    }

    for (size_t i = pending.size(); i-- > firstRejected;) {
      if (!pending[i].authority_started)
        return 2;
      encoder.rollback_authority_transaction(pending[i].authority);
      pending[i].authority_started = false;
    }
    // The explicit abort decision leaves every prepared suffix receiver at a
    // clean TU boundary.  Close each session normally so earlier committed
    // compiler-pipe work and cache counters remain provable, then reset its C
    // mirror and retry the suffix on a fresh receiver.
    for (size_t i = firstRejected; i < pending.size(); ++i) {
      auto &job = pending[i];
      auto &worker = workers[job.worker];
      worker.pending_curve_bytes += worker.frames.total() - job.wire_start;
      if (!finish_worker(worker))
        return 2;
      worker.mirror.reset();
    }
    for (size_t i = firstRejected; i < pending.size(); ++i) {
      auto &job = pending[i];
      if (!spawn_worker(workers[job.worker], job.worker, workers, corpus, dict,
                        sequence, nreg, nblk, options, generation, false))
        return 2;
    }
    cursor += firstRejected;
  }
  double relationshipSeconds = seconds_since(wallStart);
  for (auto &worker : workers)
    if (worker.active && !finish_worker(worker))
      exact = false;
  std::sort(curve.begin(), curve.end(),
            [](const CurveRow &a, const CurveRow &b) {
              return a.logical < b.logical;
            });
  // Charge final Done/Ack control to the last accepted TU on that
  // relationship.  Hello bytes were charged to the first subsequently
  // accepted TU above.  An entirely idle relationship is closed by the global
  // residual below.
  for (uint32_t id = 0; id < workers.size(); ++id) {
    if (!workers[id].pending_curve_bytes)
      continue;
    auto last =
        std::find_if(curve.rbegin(), curve.rend(),
                     [&](const CurveRow &row) { return row.worker == id; });
    if (last != curve.rend()) {
      last->wire += workers[id].pending_curve_bytes;
      workers[id].pending_curve_bytes = 0;
    }
  }
  uint64_t socketBytes = 0, fDecodeNs = 0, fPathNs = 0, compilerPipeBytes = 0,
           compilerPipeMeasuredBytes = 0, compilerPipeSummaryBytes = 0,
           compilerPipeNs = 0, compilerPipeTus = 0,
           compilerPipeSummaryTus = 0, summaryLostWorkers = 0, peakRss = 0;
  capm5::CacheTotals cacheTotals;
  for (const auto &worker : workers) {
    socketBytes += worker.frames.total();
    fDecodeNs += worker.accumulated.decode_ns;
    fPathNs += worker.accumulated.path_ns;
    // accepted/accepted_raw are advanced only after C receives F's final Ack;
    // F sends that Ack only after the verifier process consumed and accepted
    // the complete TU.  This event ledger therefore survives a later worker
    // stop even when the optional aggregate worker summary does not.
    compilerPipeBytes += worker.accepted_raw;
    compilerPipeMeasuredBytes += worker.accepted_raw;
    compilerPipeTus += worker.accepted;
    compilerPipeSummaryBytes += worker.accumulated.compiler_pipe_bytes;
    compilerPipeSummaryTus += worker.accumulated.compiler_pipe_tus;
    compilerPipeNs += worker.accumulated.compiler_pipe_ns;
    peakRss = std::max(peakRss, worker.accumulated.peak_rss_kib);
    cacheTotals.region_bytes += worker.accumulated.cache.region_bytes;
    cacheTotals.public_bytes += worker.accumulated.cache.public_bytes;
    cacheTotals.block_bytes += worker.accumulated.cache.block_bytes;
    cacheTotals.regions += worker.accumulated.cache.regions;
    cacheTotals.public_lines += worker.accumulated.cache.public_lines;
    cacheTotals.blocks += worker.accumulated.cache.blocks;
    cacheTotals.region_removals += worker.accumulated.cache.region_removals;
    cacheTotals.public_removals += worker.accumulated.cache.public_removals;
    cacheTotals.block_removals += worker.accumulated.cache.block_removals;
    cacheTotals.compactions += worker.accumulated.cache.compactions;
    if (options.real_pipes && worker.summary_lost) {
      ++summaryLostWorkers;
      if (worker.accumulated.compiler_pipe_bytes > worker.accepted_raw ||
          worker.accumulated.compiler_pipe_tus > worker.accepted) {
        exact = false;
      }
    }
  }
  uint64_t curveWire = 0;
  for (const auto &row : curve)
    curveWire += row.wire;
  if (!curve.empty() && curveWire < socketBytes)
    curve.back().wire += socketBytes - curveWire;
  else if (curveWire != socketBytes)
    exact = false;
  auto summary = capm5::summarize_curve(curve);
  if (options.curve_out) {
    std::ofstream out(options.curve_out);
    out << "logical\tphysical\tworker\traw\twire\tlatency_ns\tcumulative_raw\t"
           "cumulative_wire\n";
    uint64_t cr = 0, cw = 0;
    for (const auto &row : curve) {
      cr += row.raw;
      cw += row.wire;
      out << row.logical << '\t' << row.physical << '\t' << row.worker << '\t'
          << row.raw << '\t' << row.wire << '\t' << row.latency_ns << '\t' << cr
          << '\t' << cw << '\n';
    }
    if (!out.good())
      exact = false;
  }
  uint64_t frameClosure = 0;
  std::array<uint64_t, 7> frameBytes{}, frameCount{};
  for (const auto &worker : workers)
    for (size_t i = 0; i < frameBytes.size(); ++i) {
      frameBytes[i] += worker.frames.bytes[i];
      frameCount[i] += worker.frames.count[i];
      frameClosure += worker.frames.bytes[i];
    }
  if (frameClosure != socketBytes || curve.size() != sequence.size() ||
      rawTotal != expectedRaw ||
      (options.real_pipes &&
       (compilerPipeBytes != rawTotal || compilerPipeMeasuredBytes != rawTotal ||
        compilerPipeTus != curve.size())) ||
      preparedAccepted != transactionCommitted + transactionAborted ||
      transactionCommitted != curve.size())
    exact = false;
  for (uint32_t id = 0; id < workers.size(); ++id) {
    const auto &worker = workers[id];
    if (!worker.summary_lost &&
        (!worker.accumulated.exact ||
         worker.accumulated.verified != worker.accepted ||
         worker.accumulated.raw != worker.accepted_raw ||
         (options.real_pipes &&
          (worker.accumulated.compiler_pipe_bytes != worker.accepted_raw ||
           worker.accumulated.compiler_pipe_tus != worker.accepted)))) {
      fprintf(stderr,
              "M5 worker-summary mismatch F%u exact=%u verified=%llu/%llu "
              "raw=%llu/%llu\n",
              id, worker.accumulated.exact ? 1u : 0u,
              (unsigned long long)worker.accumulated.verified,
              (unsigned long long)worker.accepted,
              (unsigned long long)worker.accumulated.raw,
              (unsigned long long)worker.accepted_raw);
      exact = false;
    }
  }
  static const char *const frameNames[] = {"Hello", "Root", "Need",  "Fill",
                                           "Done",  "Ack",  "Rejoin"};
  printf("FRAME_LEDGER");
  for (size_t i = 0; i < frameBytes.size(); ++i)
    printf(" %s=%llu/%llu", frameNames[i], (unsigned long long)frameBytes[i],
           (unsigned long long)frameCount[i]);
  printf(" total=%llu closure=%s\n", (unsigned long long)frameClosure,
         frameClosure == socketBytes ? "OK" : "FAIL");
  printf("RESULT exact=%s codec=%s order=%s assignment=%s workers=%u wave=%u "
         "tus=%zu raw=%llu actual_socket=%llu ratio=%.3f failures=%llu "
         "snapshot=%s\n",
         exact ? "OK" : "FAIL", policy_name(options.policy),
         order_name(options.order), assignment_name(options.assignment),
         options.workers, options.wave, curve.size(),
         (unsigned long long)rawTotal, (unsigned long long)socketBytes,
         socketBytes ? double(rawTotal) / socketBytes : 0.0,
         (unsigned long long)socketFailures,
         options.snapshot_prefix.empty() ? "none" : "raw50-stop-resume");
  printf("TRANSACTIONS prepared_accepted=%llu decode_rejected=%llu "
         "committed=%llu aborted=%llu closure=%s\n",
         (unsigned long long)preparedAccepted,
         (unsigned long long)decodeRejected,
         (unsigned long long)transactionCommitted,
         (unsigned long long)transactionAborted,
         preparedAccepted == transactionCommitted + transactionAborted &&
                 transactionCommitted == curve.size()
             ? "OK"
             : "FAIL");
  printf("CURVE_SUMMARY C50=%.3f C50_TU=%u H200=", summary.c50, summary.c50_tu);
  if (summary.h200 < 0)
    printf("none");
  else
    printf("%.6f", summary.h200);
  printf(" H200_TU=");
  if (summary.h200_tu == UINT32_MAX)
    printf("none");
  else
    printf("%u", summary.h200_tu);
  printf(" SECOND_HALF=%.3f\n", summary.second_half);
  printf("CACHE regions=%u/%llu public=%u/%llu blocks=%u/%llu "
         "removals=%llu/%llu/%llu compactions=%llu\n",
         cacheTotals.regions, (unsigned long long)cacheTotals.region_bytes,
         cacheTotals.public_lines, (unsigned long long)cacheTotals.public_bytes,
         cacheTotals.blocks, (unsigned long long)cacheTotals.block_bytes,
         (unsigned long long)cacheTotals.region_removals,
         (unsigned long long)cacheTotals.public_removals,
         (unsigned long long)cacheTotals.block_removals,
         (unsigned long long)cacheTotals.compactions);
  printf("THROUGHPUT C_transform=%.3f GB/s F_decode_cpu=%.3f GB/s "
         "relationship=%.3f GB/s wall=%.6fs F_peak=%.1fMiB\n",
         encodeSeconds ? rawTotal / encodeSeconds / 1e9 : 0.0,
         fDecodeNs ? rawTotal / (fDecodeNs / 1e9) / 1e9 : 0.0,
         relationshipSeconds ? rawTotal / relationshipSeconds / 1e9 : 0.0,
         relationshipSeconds, peakRss / 1024.0);
  const double completeSeconds = seconds_since(completeStart);
  printf("PIPE_PATH mode=%s C_pipe_to_wire=%.3f GB/s "
         "F_wire_to_compiler_pipe=%.3f GB/s F_aggregate=%.3f GB/s "
         "F_pipe_write=%.3f GB/s "
         "complete=%.3f GB/s "
         "prepare=%.6fs compiler_bytes=%llu compiler_tus=%llu "
         "compiler_measured_bytes=%llu compiler_summary_bytes=%llu "
         "compiler_summary_tus=%llu summary_lost_workers=%llu\n",
         options.real_pipes ? "real" : "memory",
         options.real_pipes && preparationSeconds + relationshipSeconds > 0
             ? rawTotal / (preparationSeconds + relationshipSeconds) / 1e9
             : 0.0,
         options.real_pipes && fPathNs
             ? rawTotal / (double(fPathNs) / 1e9) / 1e9
             : 0.0,
         options.real_pipes && relationshipSeconds > 0
             ? rawTotal / relationshipSeconds / 1e9
             : 0.0,
         options.real_pipes && compilerPipeNs
             ? compilerPipeSummaryBytes / (double(compilerPipeNs) / 1e9) /
                   1e9
             : 0.0,
         options.real_pipes && completeSeconds > 0
             ? rawTotal / completeSeconds / 1e9
             : 0.0,
         preparationSeconds, (unsigned long long)compilerPipeBytes,
         (unsigned long long)compilerPipeTus,
         (unsigned long long)compilerPipeMeasuredBytes,
         (unsigned long long)compilerPipeSummaryBytes,
         (unsigned long long)compilerPipeSummaryTus,
         (unsigned long long)summaryLostWorkers);
  for (size_t i = 0; i < CK_COUNT; ++i) {
    const auto &part = components[i];
    printf("COMPONENT %-13s raw=%llu selected=%llu choices=%llu/%llu/%llu "
           "frames=%llu\n",
           COMPONENT_NAMES[i], (unsigned long long)part.raw,
           (unsigned long long)part.selected,
           (unsigned long long)part.raw_selected,
           (unsigned long long)part.z1_selected,
           (unsigned long long)part.z3_selected,
           (unsigned long long)part.count);
  }
  return exact ? 0 : 1;
}

static bool parse_u32(const char *text, uint32_t &value) {
  char *end = nullptr;
  unsigned long long parsed = strtoull(text, &end, 10);
  if (!end || *end || parsed > UINT32_MAX)
    return false;
  value = uint32_t(parsed);
  return true;
}
static bool parse_u64(const char *text, uint64_t &value) {
  char *end = nullptr;
  unsigned long long parsed = strtoull(text, &end, 10);
  if (!end || *end)
    return false;
  value = parsed;
  return true;
}
int main(int argc, char **argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--manifest") && i + 1 < argc)
      options.manifest = argv[++i];
    else if (!strcmp(argv[i], "--max-files") && i + 1 < argc) {
      uint64_t value = 0;
      if (!parse_u64(argv[++i], value) || !value)
        return 2;
      options.max_files = size_t(value);
    } else if (!strcmp(argv[i], "--repetitions") && i + 1 < argc) {
      if (!parse_u32(argv[++i], options.repetitions) || !options.repetitions)
        return 2;
    } else if (!strcmp(argv[i], "--workers") && i + 1 < argc) {
      if (!parse_u32(argv[++i], options.workers) || !options.workers ||
          options.workers > 32)
        return 2;
    } else if (!strcmp(argv[i], "--wave") && i + 1 < argc) {
      if (!parse_u32(argv[++i], options.wave) || !options.wave ||
          options.wave > 32)
        return 2;
    } else if (!strcmp(argv[i], "--codec") && i + 1 < argc) {
      const char *value = argv[++i];
      if (!strcmp(value, "raw"))
        options.policy = capp::CodecPolicy::Raw;
      else if (!strcmp(value, "z1"))
        options.policy = capp::CodecPolicy::Zstd1;
      else if (!strcmp(value, "z3"))
        options.policy = capp::CodecPolicy::Zstd3;
      else if (!strcmp(value, "best"))
        options.policy = capp::CodecPolicy::Best;
      else
        return 2;
    } else if (!strcmp(argv[i], "--order") && i + 1 < argc) {
      const char *value = argv[++i];
      if (!strcmp(value, "standard"))
        options.order = OrderMode::Standard;
      else if (!strcmp(value, "reverse"))
        options.order = OrderMode::Reverse;
      else if (!strcmp(value, "shuffle"))
        options.order = OrderMode::Shuffle;
      else if (!strcmp(value, "novelty-max"))
        options.order = OrderMode::Novelty;
      else
        return 2;
    } else if (!strcmp(argv[i], "--assignment") && i + 1 < argc) {
      const char *value = argv[++i];
      if (!strcmp(value, "roundrobin"))
        options.assignment = AssignmentMode::RoundRobin;
      else if (!strcmp(value, "sticky"))
        options.assignment = AssignmentMode::Sticky;
      else if (!strcmp(value, "random"))
        options.assignment = AssignmentMode::Random;
      else if (!strcmp(value, "failover"))
        options.assignment = AssignmentMode::Failover;
      else
        return 2;
    } else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
      if (!parse_u64(argv[++i], options.seed))
        return 2;
    } else if (!strcmp(argv[i], "--cache50") && i + 1 < argc) {
      const char *value = argv[++i];
      if (!strcmp(value, "0"))
        options.cache50 = 0;
      else if (!strcmp(value, "1"))
        options.cache50 = 1;
      else
        return 2;
    } else if (!strcmp(argv[i], "--region-bytes") && i + 1 < argc) {
      if (!parse_u64(argv[++i], options.cache_limits.region_bytes))
        return 2;
    } else if (!strcmp(argv[i], "--public-bytes") && i + 1 < argc) {
      if (!parse_u64(argv[++i], options.cache_limits.public_bytes))
        return 2;
    } else if (!strcmp(argv[i], "--block-bytes") && i + 1 < argc) {
      if (!parse_u64(argv[++i], options.cache_limits.block_bytes))
        return 2;
    } else if (!strcmp(argv[i], "--restart-at") && i + 1 < argc) {
      if (!parse_u32(argv[++i], options.restart_at))
        return 2;
    } else if (!strcmp(argv[i], "--latejoin-at") && i + 1 < argc) {
      if (!parse_u32(argv[++i], options.latejoin_at))
        return 2;
    } else if (!strcmp(argv[i], "--failover-at") && i + 1 < argc) {
      if (!parse_u32(argv[++i], options.failover_at))
        return 2;
    } else if (!strcmp(argv[i], "--corrupt-tu") && i + 1 < argc) {
      if (!parse_u32(argv[++i], options.corrupt_tu))
        return 2;
    } else if (!strcmp(argv[i], "--curve-out") && i + 1 < argc)
      options.curve_out = argv[++i];
    else if (!strcmp(argv[i], "--snapshot50-prefix") && i + 1 < argc)
      options.snapshot_prefix = argv[++i];
    else if (!strcmp(argv[i], "--real-pipes"))
      options.real_pipes = true;
    else {
      fprintf(stderr, "unknown option: %s\n", argv[i]);
      return 2;
    }
  }
  if (!options.manifest || options.wave > options.workers ||
      (options.assignment == AssignmentMode::Failover && options.workers < 2)) {
    fprintf(stderr,
            "usage: %s --manifest F [--workers 1..32] [--wave N] [--order "
            "standard|reverse|shuffle|novelty-max] [--assignment "
            "roundrobin|sticky|random|failover] [--snapshot50-prefix PATH] "
            "[--real-pipes]\n",
            argv[0]);
    return 2;
  }
  auto start = Clock::now();
  Corpus corpus;
  Interner dict;
  std::vector<std::vector<uint32_t>> regions;
  PreparationTiming preparation;
  if (options.real_pipes) {
    if (!load_corpus_via_pipe(options.manifest, options.max_files, corpus, dict,
                              regions, preparation))
      return 2;
  } else {
    auto loadBegin = Clock::now();
    corpus = load_corpus(options.manifest, options.max_files);
    preparation.source_pipe = seconds_since(loadBegin);
  }
  if (corpus.files.empty() || corpus.files.size() > UINT32_MAX)
    return 2;
  if (!options.real_pipes) {
    regions.resize(corpus.files.size());
    uint32_t maxLength = 0;
    for (const auto &file : corpus.files)
      maxLength = std::max(maxLength, file.len);
    std::vector<uint32_t> output(size_t(maxLength) + 1);
    uint64_t hits = 0;
    auto internBegin = Clock::now();
    for (size_t tu = 0; tu < corpus.files.size(); ++tu) {
      const auto &file = corpus.files[tu];
      size_t count = 0;
      const char *begin = corpus.bytes.data() + file.off;
      dict.process(begin, begin + file.len, output.data(), count, hits, true,
                   &regions[tu]);
    }
    preparation.interning = seconds_since(internBegin);
  }
  auto orderBegin = Clock::now();
  auto order = make_order(options.order, options.seed, regions, dict);
  preparation.ordering = seconds_since(orderBegin);
  std::vector<uint32_t> sequence;
  sequence.reserve(order.size() * options.repetitions);
  for (uint32_t repeat = 0; repeat < options.repetitions; ++repeat)
    sequence.insert(sequence.end(), order.begin(), order.end());
  if (sequence.size() > UINT32_MAX)
    return 2;
  if (options.restart_at >= sequence.size() && options.restart_at != UINT32_MAX)
    return 2;
  if (options.latejoin_at >= sequence.size() &&
      options.latejoin_at != UINT32_MAX)
    return 2;
  if (options.failover_at >= sequence.size() &&
      options.failover_at != UINT32_MAX)
    return 2;
  if (options.corrupt_tu >= sequence.size() && options.corrupt_tu != UINT32_MAX)
    return 2;
  auto factorBegin = Clock::now();
  auto factor = factor_sequence(regions, sequence);
  preparation.factorization = seconds_since(factorBegin);
  double preparationSeconds = seconds_since(start);
  fprintf(stderr,
          "M5 prepared %.3fs TUs=%zu raw=%llu regions=%llu blocks=%zu "
          "tokens=%zu order=%s\n",
          preparationSeconds, sequence.size(), (unsigned long long)corpus.raw,
          (unsigned long long)dict.region_count(),
          factor.block_offsets.size() - 1, factor.tokens.size(),
          order_name(options.order));
  fprintf(stderr,
          "PREP_STAGE manifest=%.6fs source_pipe=%.6fs interning=%.6fs "
          "ordering=%.6fs factorization=%.6fs accounted=%.6fs\n",
          preparation.manifest, preparation.source_pipe, preparation.interning,
          preparation.ordering, preparation.factorization,
          preparation.manifest + preparation.source_pipe +
              preparation.interning + preparation.ordering +
              preparation.factorization);
  fprintf(stderr,
          "PREP_RATE source_pipe=%.3f GB/s interning=%.3f GB/s "
          "factorization=%.3f GB/s\n",
          preparation.source_pipe ? corpus.raw / preparation.source_pipe / 1e9
                                  : 0.0,
          preparation.interning ? corpus.raw / preparation.interning / 1e9
                                : 0.0,
          preparation.factorization
              ? corpus.raw / preparation.factorization / 1e9
              : 0.0);
  return run_coordinator(corpus, dict, sequence, factor, options, start,
                         preparationSeconds);
}
