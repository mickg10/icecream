// icecream #16 — protocol-50 binding one-pass pipeline.
//
// The original M5 scenario harness intentionally prepares the complete corpus
// before starting its C<->F relationship.  That is useful for deterministic
// lifecycle tests, but it cannot prove the complete producer-to-compiler rate.
// This executable keeps the accepted M1-M4 object grammar and dialogue while
// removing the batch-only dependency on final Region/Block counts:
//
//   producer process -> bounded C reader/interner/factorizer queue
//                    -> C Root/Need/Fill transactions over socketpairs
//                    -> independent F stores -> verifier compiler pipes
//
// Region and Block ordinals are explicitly typed in Root.  Stores grow as
// definitions arrive; no final corpus census or second input pass is needed.

#include "cap_codec.h"
#include "cap_identity.h"
#include "cap_m5_metrics.h"
#include "cap_m5_state.h"
#include "cap_protocol.h"
#include "cap_transport.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <mutex>
#include <numeric>
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

struct Options {
  const char *manifest = nullptr;
  size_t max_files = SIZE_MAX;
  uint32_t workers = 8, wave = 8, queue_depth = 16, raw_queue_depth = 2;
  capp::CodecPolicy policy = capp::CodecPolicy::Zstd1;
  capm5::CacheLimits cache_limits;
  const char *curve_out = nullptr;
};

struct Manifest {
  std::vector<std::string> paths;
  std::vector<uint32_t> lengths;
  uint64_t raw = 0;
};

static bool read_manifest(const char *path, size_t maxFiles, Manifest &out) {
  FILE *input = fopen(path, "r");
  if (!input) {
    perror(path);
    return false;
  }
  char name[8192];
  while (fgets(name, sizeof name, input)) {
    size_t length = strlen(name);
    while (length && (name[length - 1] == '\n' || name[length - 1] == '\r'))
      name[--length] = 0;
    if (!length)
      continue;
    struct stat status {};
    if (stat(name, &status) != 0 || status.st_size < 0 ||
        uint64_t(status.st_size) > UINT32_MAX) {
      perror(name);
      fclose(input);
      return false;
    }
    out.paths.emplace_back(name);
    out.lengths.push_back(uint32_t(status.st_size));
    out.raw += uint64_t(status.st_size);
    if (out.paths.size() == maxFiles)
      break;
  }
  fclose(input);
  return !out.paths.empty() && out.paths.size() <= UINT32_MAX;
}

static void enlarge_pipe(int fd) {
#ifdef F_SETPIPE_SZ
  (void)fcntl(fd, F_SETPIPE_SZ, 1 << 20);
#else
  (void)fd;
#endif
}

struct FrameLedger {
  std::array<uint64_t, 7> bytes{}, count{};
  void note(cap::Frame frame, size_t payload) {
    size_t index = size_t(frame);
    bytes[index] += 4 + payload;
    ++count[index];
  }
  uint64_t total() const {
    return std::accumulate(bytes.begin(), bytes.end(), uint64_t(0));
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
                         std::vector<uint8_t> &payload,
                         FrameLedger &ledger) {
  if (!cap::recv_frame(fd, frame, payload))
    return false;
  ledger.note(frame, payload.size());
  return true;
}

struct CodecContexts {
  ZSTD_CCtx *z1 = ZSTD_createCCtx(), *z3 = ZSTD_createCCtx();
  ZSTD_DCtx *decoder = ZSTD_createDCtx();
  ~CodecContexts() {
    ZSTD_freeCCtx(z1);
    ZSTD_freeCCtx(z3);
    ZSTD_freeDCtx(decoder);
  }
};

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

struct ComponentLedger {
  uint64_t raw = 0, selected = 0, raw_selected = 0, z1_selected = 0,
           z3_selected = 0, count = 0;
  void note(const capp::EncodedComponent &component) {
    raw += component.raw_size;
    selected += component.wire.size();
    ++count;
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
    else if (!wire.empty() &&
             wire[0] == uint8_t(capp::ComponentCodec::Zstd1))
      ++z1_selected;
    else
      ++z3_selected;
  }
};

static capp::EncodedComponent
encode_component(const std::vector<uint8_t> &raw, capp::CodecPolicy policy,
                 CodecContexts &codec, ComponentLedger &ledger) {
  auto result = capp::encode_component(raw, policy, codec.z1, codec.z3, true);
  ledger.note(result);
  return result;
}

static cap::SourceGeneration make_generation() {
  cap::SourceGeneration result{};
  std::random_device random;
  for (uint8_t &byte : result)
    byte = uint8_t(random());
  if (std::all_of(result.begin(), result.end(),
                  [](uint8_t value) { return value == 0; }))
    result[0] = 1;
  return result;
}

static std::vector<uint8_t> build_need(const std::vector<uint32_t> &missing) {
  std::vector<uint8_t> result;
  put_varint(result, missing.size());
  for (uint32_t id : missing)
    put_varint(result, id);
  put_varint(result, 0); // public-Line drops are carried by final Ack in M5.
  return result;
}

static bool parse_need(const std::vector<uint8_t> &raw, uint32_t nreg,
                       std::vector<uint32_t> &missing) {
  const uint8_t *p = raw.data(), *e = p + raw.size();
  uint64_t count = 0;
  if (!get_varint_bounded(p, e, count) || count > size_t(e - p))
    return false;
  missing.clear();
  missing.reserve(size_t(count));
  for (uint64_t index = 0; index < count; ++index) {
    uint32_t id = 0;
    if (!get_u32_bounded(p, e, id) || id >= nreg)
      return false;
    missing.push_back(id);
  }
  uint64_t drops = 0;
  return get_varint_bounded(p, e, drops) && drops == 0 && p == e;
}

// Root tokens carry their object kind, so future Region ordinals cannot
// collide with already-published Block ordinals.
static uint64_t region_token(uint32_t id) { return uint64_t(id) << 1; }
static uint64_t block_token(uint32_t id) {
  return (uint64_t(id) << 1) | 1;
}

struct BlockDefinition {
  uint32_t id = 0;
  std::vector<uint32_t> children;
};

struct PreparedTU {
  uint32_t logical = 0;
  uint32_t raw_length = 0;
  uint32_t nreg = 0, nblk = 0, distinct_lines = 0;
  std::vector<uint64_t> tokens;
  std::vector<BlockDefinition> blocks;
  size_t buffered_bytes() const {
    size_t total = tokens.size() * sizeof(uint64_t);
    for (const auto &block : blocks)
      total += sizeof(BlockDefinition) +
               block.children.size() * sizeof(uint32_t);
    return total;
  }
};

class OnlineFactorizer {
public:
  OnlineFactorizer() : head(size_t(1) << HASH_BITS, UINT32_MAX) {}

  PreparedTU process(uint32_t logical, uint32_t rawLength,
                     const std::vector<uint32_t> &regions,
                     uint32_t distinctLines, uint32_t nreg) {
    PreparedTU output;
    output.logical = logical;
    output.raw_length = rawLength;
    output.nreg = nreg;
    output.distinct_lines = distinctLines;
    const size_t begin = stream.size();
    stream.insert(stream.end(), regions.begin(), regions.end());
    previous.resize(stream.size(), UINT32_MAX);
    const size_t end = stream.size();
    size_t position = begin;
    std::vector<uint32_t> required;
    while (position < end) {
      size_t bestLength = 0;
      if (position + MIN_MATCH <= end) {
        uint32_t candidate = head[kgram(position)], chain = 0;
        while (candidate != UINT32_MAX && chain < MAX_CHAIN) {
          if (candidate < position) {
            size_t length = 0;
            size_t maximum =
                std::min(end - position, position - size_t(candidate));
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
        uint32_t id = intern_block(stream.data() + position, bestLength);
        output.tokens.push_back(block_token(id));
        required.push_back(id);
        step = bestLength;
      } else {
        output.tokens.push_back(region_token(stream[position]));
      }
      for (size_t index = position; index < position + step; ++index) {
        if (index + MIN_MATCH > stream.size())
          continue;
        uint32_t hash = kgram(index);
        previous[index] = head[hash];
        head[hash] = uint32_t(index);
      }
      position += step;
    }
    std::sort(required.begin(), required.end());
    required.erase(std::unique(required.begin(), required.end()),
                   required.end());
    output.blocks.reserve(required.size());
    for (uint32_t id : required) {
      const size_t first = block_offsets[id], last = block_offsets[id + 1];
      output.blocks.push_back(
          {id, std::vector<uint32_t>(block_children.begin() + first,
                                     block_children.begin() + last)});
    }
    output.nblk = uint32_t(block_offsets.size() - 1);
    return output;
  }

private:
  static constexpr uint32_t MIN_MATCH = 3, MAX_CHAIN = 64, HASH_BITS = 22;
  std::vector<uint32_t> stream, previous, head;
  std::vector<uint32_t> block_children;
  std::vector<size_t> block_offsets{0};
  std::unordered_map<uint64_t, uint32_t> dictionary;

  uint32_t kgram(size_t position) const {
    uint64_t hash = 1469598103934665603ull;
    for (uint32_t index = 0; index < MIN_MATCH; ++index) {
      hash ^= stream[position + index];
      hash *= 1099511628211ull;
    }
    return uint32_t((hash * 0x9e3779b97f4a7c15ull) >> (64 - HASH_BITS));
  }

  uint32_t intern_block(const uint32_t *values, size_t length) {
    uint64_t hash = 1469598103934665603ull ^ (length * 0x100000001b3ull);
    for (size_t index = 0; index < length; ++index) {
      hash ^= values[index];
      hash *= 1099511628211ull;
    }
    auto found = dictionary.find(hash);
    if (found != dictionary.end()) {
      uint32_t id = found->second;
      if (block_offsets[id + 1] - block_offsets[id] == length &&
          !memcmp(block_children.data() + block_offsets[id], values,
                  length * sizeof(uint32_t)))
        return id;
    }
    uint32_t id = uint32_t(block_offsets.size() - 1);
    block_children.insert(block_children.end(), values, values + length);
    block_offsets.push_back(block_children.size());
    if (found == dictionary.end())
      dictionary.emplace(hash, id);
    return id;
  }
};

class PreparedQueue {
public:
  explicit PreparedQueue(size_t capacity) : capacity_(capacity) {}

  bool push(std::shared_ptr<PreparedTU> value) {
    std::unique_lock<std::mutex> lock(mutex_);
    writable_.wait(lock, [&] { return failed_ || queue_.size() < capacity_; });
    if (failed_)
      return false;
    buffered_ += value->buffered_bytes();
    queue_.push_back(std::move(value));
    high_count_ = std::max(high_count_, queue_.size());
    high_bytes_ = std::max(high_bytes_, buffered_);
    readable_.notify_all();
    return true;
  }

  bool pop_batch(size_t wanted,
                 std::vector<std::shared_ptr<PreparedTU>> &result) {
    std::unique_lock<std::mutex> lock(mutex_);
    readable_.wait(lock, [&] {
      return failed_ || queue_.size() >= wanted ||
             (finished_ && !queue_.empty()) || (finished_ && queue_.empty());
    });
    if (failed_ || (finished_ && queue_.empty()))
      return false;
    size_t count = std::min(wanted, queue_.size());
    result.clear();
    result.reserve(count);
    for (size_t index = 0; index < count; ++index) {
      buffered_ -= queue_.front()->buffered_bytes();
      result.push_back(std::move(queue_.front()));
      queue_.pop_front();
    }
    writable_.notify_all();
    return true;
  }

  void finish() {
    std::lock_guard<std::mutex> lock(mutex_);
    finished_ = true;
    readable_.notify_all();
  }
  void fail() {
    std::lock_guard<std::mutex> lock(mutex_);
    failed_ = true;
    readable_.notify_all();
    writable_.notify_all();
  }
  bool failed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return failed_;
  }
  size_t high_count() const { return high_count_; }
  size_t high_bytes() const { return high_bytes_; }

private:
  const size_t capacity_;
  mutable std::mutex mutex_;
  std::condition_variable readable_, writable_;
  std::deque<std::shared_ptr<PreparedTU>> queue_;
  size_t buffered_ = 0, high_count_ = 0, high_bytes_ = 0;
  bool finished_ = false, failed_ = false;
};

struct RawTU {
  uint32_t logical = 0;
  std::vector<uint8_t> bytes;
  size_t buffered_bytes() const { return bytes.size(); }
};

class RawQueue {
public:
  explicit RawQueue(size_t capacity) : capacity_(capacity) {}

  bool push(std::shared_ptr<RawTU> value) {
    std::unique_lock<std::mutex> lock(mutex_);
    writable_.wait(lock, [&] { return failed_ || queue_.size() < capacity_; });
    if (failed_)
      return false;
    buffered_ += value->buffered_bytes();
    queue_.push_back(std::move(value));
    high_count_ = std::max(high_count_, queue_.size());
    high_bytes_ = std::max(high_bytes_, buffered_);
    readable_.notify_one();
    return true;
  }

  bool pop(std::shared_ptr<RawTU> &result) {
    std::unique_lock<std::mutex> lock(mutex_);
    readable_.wait(lock,
                   [&] { return failed_ || !queue_.empty() || finished_; });
    if (failed_ || queue_.empty())
      return false;
    buffered_ -= queue_.front()->buffered_bytes();
    result = std::move(queue_.front());
    queue_.pop_front();
    writable_.notify_one();
    return true;
  }

  void finish() {
    std::lock_guard<std::mutex> lock(mutex_);
    finished_ = true;
    readable_.notify_all();
  }
  void fail() {
    std::lock_guard<std::mutex> lock(mutex_);
    failed_ = true;
    readable_.notify_all();
    writable_.notify_all();
  }
  bool failed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return failed_;
  }
  size_t high_count() const { return high_count_; }
  size_t high_bytes() const { return high_bytes_; }

private:
  const size_t capacity_;
  mutable std::mutex mutex_;
  std::condition_variable readable_, writable_;
  std::deque<std::shared_ptr<RawTU>> queue_;
  size_t buffered_ = 0, high_count_ = 0, high_bytes_ = 0;
  bool finished_ = false, failed_ = false;
};

struct ProducerTiming {
  double source_pipe = 0, interning = 0, factorization = 0;
};

struct WorkerSummary {
  bool exact = false;
  uint64_t verified = 0, raw = 0, decode_ns = 0, path_ns = 0, failures = 0,
           peak_rss_kib = 0, compiler_pipe_bytes = 0, compiler_pipe_tus = 0,
           compiler_pipe_ns = 0;
  capm5::CacheTotals cache;
};

static std::vector<uint8_t> pack_worker_summary(const WorkerSummary &value) {
  std::vector<uint8_t> output;
  output.push_back(value.exact ? 1 : 0);
  const std::array<uint64_t, 19> fields = {
      value.verified,
      value.raw,
      value.decode_ns,
      value.path_ns,
      value.failures,
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
    put_u64le(output, field);
  return output;
}

static bool unpack_worker_summary(const std::vector<uint8_t> &payload,
                                  WorkerSummary &value) {
  constexpr size_t FIELD_COUNT = 19;
  if (payload.size() != 1 + FIELD_COUNT * sizeof(uint64_t) || payload[0] > 1)
    return false;
  value.exact = payload[0] != 0;
  const uint8_t *p = payload.data() + 1, *end = payload.data() + payload.size();
  std::array<uint64_t, FIELD_COUNT> fields{};
  for (uint64_t &field : fields) {
    if (size_t(end - p) < sizeof(uint64_t))
      return false;
    for (unsigned byte = 0; byte < sizeof(uint64_t); ++byte)
      field |= uint64_t(*p++) << (8 * byte);
  }
  if (fields[12] > UINT32_MAX || fields[13] > UINT32_MAX ||
      fields[14] > UINT32_MAX || p != end)
    return false;
  value.verified = fields[0];
  value.raw = fields[1];
  value.decode_ns = fields[2];
  value.path_ns = fields[3];
  value.failures = fields[4];
  value.peak_rss_kib = fields[5];
  value.compiler_pipe_bytes = fields[6];
  value.compiler_pipe_tus = fields[7];
  value.compiler_pipe_ns = fields[8];
  value.cache.region_bytes = fields[9];
  value.cache.public_bytes = fields[10];
  value.cache.block_bytes = fields[11];
  value.cache.regions = uint32_t(fields[12]);
  value.cache.public_lines = uint32_t(fields[13]);
  value.cache.blocks = uint32_t(fields[14]);
  value.cache.region_removals = fields[15];
  value.cache.public_removals = fields[16];
  value.cache.block_removals = fields[17];
  value.cache.compactions = fields[18];
  return true;
}

class CompilerVerifier {
public:
  CompilerVerifier() = default;
  CompilerVerifier(const CompilerVerifier &) = delete;
  CompilerVerifier &operator=(const CompilerVerifier &) = delete;
  ~CompilerVerifier() { abandon(); }

  bool start(int relationshipFd, const Manifest &manifest) {
    int descriptors[2], acknowledgements[2];
    if (pipe(descriptors) != 0)
      return false;
    if (pipe(acknowledgements) != 0) {
      close(descriptors[0]);
      close(descriptors[1]);
      return false;
    }
    enlarge_pipe(descriptors[1]);
    pid_ = fork();
    if (pid_ < 0) {
      close(descriptors[0]);
      close(descriptors[1]);
      close(acknowledgements[0]);
      close(acknowledgements[1]);
      return false;
    }
    if (pid_ == 0) {
      close(descriptors[1]);
      close(acknowledgements[0]);
      close(relationshipFd);
      std::vector<uint8_t> actual(1 << 20), expected(1 << 20);
      for (;;) {
        uint8_t header[8];
        if (!cap::read_all(descriptors[0], header, sizeof header))
          _exit(2);
        uint32_t logical = 0, length = 0;
        for (unsigned byte = 0; byte < 4; ++byte) {
          logical |= uint32_t(header[byte]) << (8 * byte);
          length |= uint32_t(header[4 + byte]) << (8 * byte);
        }
        if (logical == UINT32_MAX && length == 0)
          break;
        if (logical >= manifest.paths.size() ||
            length != manifest.lengths[logical])
          _exit(2);
        FILE *source = fopen(manifest.paths[logical].c_str(), "rb");
        if (!source)
          _exit(2);
        uint32_t consumed = 0;
        while (consumed < length) {
          uint32_t chunk =
              std::min<uint32_t>(length - consumed, uint32_t(actual.size()));
          if (!cap::read_all(descriptors[0], actual.data(), chunk) ||
              fread(expected.data(), 1, chunk, source) != chunk ||
              memcmp(actual.data(), expected.data(), chunk))
            _exit(2);
          consumed += chunk;
        }
        if (fgetc(source) != EOF || ferror(source))
          _exit(2);
        fclose(source);
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
    fd_ = descriptors[1];
    ack_fd_ = acknowledgements[0];
    return true;
  }

  bool emit(uint32_t logical, const std::vector<uint8_t> &bytes) {
    if (fd_ < 0 || ack_fd_ < 0 || bytes.size() > UINT32_MAX)
      return false;
    uint8_t header[8];
    uint32_t length = uint32_t(bytes.size());
    for (unsigned byte = 0; byte < 4; ++byte) {
      header[byte] = uint8_t(logical >> (8 * byte));
      header[4 + byte] = uint8_t(length >> (8 * byte));
    }
    if (!cap::write_all(fd_, header, sizeof header) ||
        (length && !cap::write_all(fd_, bytes.data(), length)))
      return false;
    uint8_t accepted = 0;
    if (!cap::read_all(ack_fd_, &accepted, 1) || accepted != 1)
      return false;
    transferred += length;
    ++tus;
    return true;
  }

  bool finish() {
    if (fd_ < 0 || ack_fd_ < 0 || pid_ < 0)
      return false;
    uint8_t header[8]{};
    for (unsigned byte = 0; byte < 4; ++byte)
      header[byte] = 0xff;
    bool sent = cap::write_all(fd_, header, sizeof header);
    close(fd_);
    close(ack_fd_);
    fd_ = ack_fd_ = -1;
    int status = 0;
    waitpid(pid_, &status, 0);
    pid_ = -1;
    return sent && WIFEXITED(status) && WEXITSTATUS(status) == 0;
  }

  uint64_t transferred = 0, tus = 0;

private:
  int fd_ = -1, ack_fd_ = -1;
  pid_t pid_ = -1;
  void abandon() {
    if (fd_ >= 0)
      close(fd_);
    if (ack_fd_ >= 0)
      close(ack_fd_);
    fd_ = ack_fd_ = -1;
    if (pid_ >= 0) {
      int status = 0;
      waitpid(pid_, &status, 0);
      pid_ = -1;
    }
  }
};

static bool scan_typed_dimensions(const std::vector<uint8_t> &root,
                                  const std::vector<uint8_t> &blocks,
                                  uint32_t &nreg, uint32_t &nblk) {
  nreg = nblk = 0;
  const uint8_t *p = root.data(), *end = p + root.size();
  while (p < end) {
    uint64_t token = 0;
    if (!get_varint_bounded(p, end, token) || (token >> 1) > UINT32_MAX)
      return false;
    uint32_t id = uint32_t(token >> 1);
    if (token & 1)
      nblk = std::max(nblk, id + 1);
    else
      nreg = std::max(nreg, id + 1);
  }
  p = blocks.data();
  end = p + blocks.size();
  if (p == end)
    return true;
  uint64_t count = 0;
  if (!get_varint_bounded(p, end, count) || count > size_t(end - p))
    return false;
  for (uint64_t item = 0; item < count; ++item) {
    uint32_t id = 0;
    if (!get_u32_bounded(p, end, id) || p == end || *p++ != 0)
      return false;
    nblk = std::max(nblk, id + 1);
    uint64_t length = 0;
    if (!get_varint_bounded(p, end, length) || length > size_t(end - p))
      return false;
    for (uint64_t child = 0; child < length; ++child) {
      uint32_t region = 0;
      if (!get_u32_bounded(p, end, region))
        return false;
      nreg = std::max(nreg, region + 1);
    }
  }
  return p == end;
}

static int worker_loop(int fd, uint32_t workerId, const Manifest &manifest,
                       const Options &options) {
  CodecContexts codec;
  FrameLedger frames;
  cap::Frame frame;
  std::vector<uint8_t> payload;
  if (!recv_counted(fd, frame, payload, frames) || frame != cap::Frame::Hello)
    return 2;
  cap::SourceGeneration generation{};
  uint32_t nreg = 0, nblk = 0, physicalTus = 0, repetitions = 0;
  if (!capp::try_unpack_hello_m4(payload, generation, nreg, nblk, physicalTus,
                                 repetitions) ||
      nreg != 0 || nblk != 0 || physicalTus != manifest.paths.size() ||
      repetitions != 1)
    return 2;

  FStore store;
  store.init(0, 0);
  capm5::CacheState cache;
  cache.init(0, 0, options.cache_limits);
  CompilerVerifier compiler;
  if (!compiler.start(fd, manifest))
    return 2;

  uint64_t verified = 0, verifiedRaw = 0, failures = 0;
  double decodeSeconds = 0, pathSeconds = 0, compilerSeconds = 0;
  bool exact = true;
  std::vector<uint8_t> reconstructed;
  std::vector<uint32_t> occurrences;
  for (;;) {
    if (!recv_counted(fd, frame, payload, frames))
      return 2;
    if (frame == cap::Frame::Done) {
      if (!payload.empty() || !compiler.finish())
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
      summary.peak_rss_kib = uint64_t(usage.ru_maxrss);
      summary.compiler_pipe_bytes = compiler.transferred;
      summary.compiler_pipe_tus = compiler.tus;
      summary.compiler_pipe_ns = uint64_t(compilerSeconds * 1e9);
      summary.cache = cache.totals;
      return send_counted(fd, cap::Frame::Ack, pack_worker_summary(summary),
                          frames)
                 ? (exact ? 0 : 1)
                 : 2;
    }
    if (frame != cap::Frame::Root)
      return 2;

    auto pathBegin = Clock::now();
    auto decodeBegin = Clock::now();
    uint32_t logical = 0;
    std::vector<uint8_t> rootWire, blockWire, rootRaw, blockRaw;
    auto reject = [&](FStore::FillTransaction *fill, const char *reason) {
      if (fill && fill->active)
        store.rollback_fill(*fill);
      decodeSeconds += seconds_since(decodeBegin);
      pathSeconds += seconds_since(pathBegin);
      ++failures;
      fprintf(stderr, "stream F%u reject TU %u (%s)\n", workerId, logical,
              reason);
      capp::CacheDropsM5 none;
      return send_counted(fd, cap::Frame::Ack,
                          capp::pack_tu_ack_m5(logical, false, none), frames);
    };
    if (!capp::try_unpack_root_m4(payload, logical, rootWire, blockWire) ||
        logical >= manifest.paths.size() ||
        !capp::decode_component(rootWire, codec.decoder, rootRaw) ||
        !capp::decode_component(blockWire, codec.decoder, blockRaw)) {
      if (!reject(nullptr, "Root/component"))
        return 2;
      continue;
    }
    uint32_t requiredNreg = 0, requiredNblk = 0;
    if (!scan_typed_dimensions(rootRaw, blockRaw, requiredNreg,
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
    if (!store.typed_requirements(rootRaw, &blockTx, requiredRegions,
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
    bool decoded =
        capp::try_unpack_fill_m4(payload, fillTU, pathBase, publicBase,
                                 pathWire, mixedWire, 4) &&
        fillTU == logical &&
        capp::decode_component(pathWire, codec.decoder, pathRaw);
    if (decoded)
      for (size_t part = 0; part < 4; ++part)
        if (!capp::decode_component(mixedWire[part], codec.decoder,
                                    mixedRaw[part])) {
          decoded = false;
          break;
        }
    if (!decoded) {
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
    bool expanded = store.reconstruct_typed_staged(
        rootRaw, &blockTx, reconstructed, occurrences);
    if (!expanded || reconstructed.size() != manifest.lengths[logical]) {
      if (!reject(&fillTx, "exact expansion"))
        return 2;
      continue;
    }
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
    auto compilerBegin = Clock::now();
    if (!compiler.emit(logical, reconstructed))
      return 2;
    compilerSeconds += seconds_since(compilerBegin);
    auto commitBegin = Clock::now();
    store.commit_fill(fillTx);
    store.commit_blocks(blockTx);
    cache.account_fill(store, missing, blockTx, fillTx,
                       uint64_t(logical) + 1);
    cache.touch_committed(store, occurrences, requiredBlocks,
                          uint64_t(logical) + 1);
    auto drops = cache.evict(store);
    decodeSeconds += seconds_since(commitBegin);
    pathSeconds += seconds_since(pathBegin);
    if (!send_counted(fd, cap::Frame::Ack,
                      capp::pack_tu_ack_m5(logical, true, drops), frames))
      return 2;
    ++verified;
    verifiedRaw += reconstructed.size();
  }
}

struct Worker {
  pid_t pid = -1;
  int fd = -1;
  FrameLedger frames;
  capm5::ReceiverMirror mirror;
  WorkerSummary summary;
  uint64_t pending_control = 0, accepted = 0, accepted_raw = 0;
};

static bool spawn_worker(Worker &worker, uint32_t id,
                         const std::vector<Worker> &existing,
                         const Manifest &manifest, const Options &options,
                         const cap::SourceGeneration &generation) {
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
    close(sockets[0]);
    for (const auto &other : existing)
      if (other.fd >= 0)
        close(other.fd);
    int result = worker_loop(sockets[1], id, manifest, options);
    close(sockets[1]);
    _exit(result);
  }
  close(sockets[1]);
  worker.pid = child;
  worker.fd = sockets[0];
  worker.mirror.init(0, 0);
  auto hello = capp::pack_hello_m4(generation, 0, 0,
                                   uint32_t(manifest.paths.size()), 1);
  if (!send_counted(worker.fd, cap::Frame::Hello, hello, worker.frames))
    return false;
  worker.pending_control = worker.frames.total();
  return true;
}

static bool finish_worker(Worker &worker) {
  uint64_t before = worker.frames.total();
  if (!send_counted(worker.fd, cap::Frame::Done, {}, worker.frames))
    return false;
  cap::Frame frame;
  std::vector<uint8_t> payload;
  if (!recv_counted(worker.fd, frame, payload, worker.frames) ||
      frame != cap::Frame::Ack ||
      !unpack_worker_summary(payload, worker.summary))
    return false;
  worker.pending_control += worker.frames.total() - before;
  close(worker.fd);
  worker.fd = -1;
  int status = 0;
  waitpid(worker.pid, &status, 0);
  worker.pid = -1;
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

class SourcePipeline {
public:
  SourcePipeline(const Manifest &manifest, const Options &options,
                 const std::vector<Worker> &workers, PreparedQueue &queue,
                 Interner &dict, std::mutex &dictMutex,
                 ProducerTiming &timing)
      : manifest_(manifest), queue_(queue), raw_queue_(options.raw_queue_depth),
        dict_(dict), dict_mutex_(dictMutex), timing_(timing) {
    if (pipe(descriptors_) != 0) {
      perror("source pipe");
      queue_.fail();
      return;
    }
    enlarge_pipe(descriptors_[1]);
    pid_ = fork();
    if (pid_ < 0) {
      perror("source fork");
      close(descriptors_[0]);
      close(descriptors_[1]);
      descriptors_[0] = descriptors_[1] = -1;
      queue_.fail();
      return;
    }
    if (pid_ == 0) {
      close(descriptors_[0]);
      for (const auto &worker : workers)
        if (worker.fd >= 0)
          close(worker.fd);
      std::vector<uint8_t> buffer(1 << 20);
      for (const std::string &path : manifest_.paths) {
        FILE *source = fopen(path.c_str(), "rb");
        if (!source)
          _exit(2);
        for (;;) {
          size_t count = fread(buffer.data(), 1, buffer.size(), source);
          if (count &&
              !cap::write_all(descriptors_[1], buffer.data(), count))
            _exit(2);
          if (count != buffer.size()) {
            if (ferror(source))
              _exit(2);
            break;
          }
        }
        fclose(source);
      }
      close(descriptors_[1]);
      _exit(0);
    }
    close(descriptors_[1]);
    descriptors_[1] = -1;
    reader_ = std::thread([this] { read_loop(); });
    interner_ = std::thread([this] { intern_loop(); });
  }

  SourcePipeline(const SourcePipeline &) = delete;
  SourcePipeline &operator=(const SourcePipeline &) = delete;

  bool finish() {
    if (reader_.joinable())
      reader_.join();
    if (interner_.joinable())
      interner_.join();
    int status = 0;
    if (pid_ >= 0) {
      waitpid(pid_, &status, 0);
      pid_ = -1;
    }
    return !queue_.failed() && !raw_queue_.failed() && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
  }

  size_t raw_high_count() const { return raw_queue_.high_count(); }
  size_t raw_high_bytes() const { return raw_queue_.high_bytes(); }

private:
  const Manifest &manifest_;
  PreparedQueue &queue_;
  RawQueue raw_queue_;
  Interner &dict_;
  std::mutex &dict_mutex_;
  ProducerTiming &timing_;
  int descriptors_[2]{-1, -1};
  pid_t pid_ = -1;
  std::thread reader_, interner_;

  void read_loop() {
    for (uint32_t logical = 0; logical < manifest_.lengths.size(); ++logical) {
      uint32_t length = manifest_.lengths[logical];
      auto raw = std::make_shared<RawTU>();
      raw->logical = logical;
      raw->bytes.resize(length);
      auto sourceBegin = Clock::now();
      if (length &&
          !cap::read_all(descriptors_[0], raw->bytes.data(), size_t(length))) {
        raw_queue_.fail();
        queue_.fail();
        close(descriptors_[0]);
        descriptors_[0] = -1;
        return;
      }
      timing_.source_pipe += seconds_since(sourceBegin);
      if (!raw_queue_.push(std::move(raw))) {
        close(descriptors_[0]);
        descriptors_[0] = -1;
        return;
      }
    }
    uint8_t extra = 0;
    ssize_t count;
    do {
      count = read(descriptors_[0], &extra, 1);
    } while (count < 0 && errno == EINTR);
    close(descriptors_[0]);
    descriptors_[0] = -1;
    if (count != 0) {
      raw_queue_.fail();
      queue_.fail();
      return;
    }
    raw_queue_.finish();
  }

  void intern_loop() {
    std::vector<uint32_t> lineIds;
    uint64_t hits = 0;
    OnlineFactorizer factorizer;
    uint32_t expected = 0;
    std::shared_ptr<RawTU> raw;
    while (raw_queue_.pop(raw)) {
      if (!raw || raw->logical != expected++) {
        raw_queue_.fail();
        queue_.fail();
        return;
      }
      if (lineIds.size() < raw->bytes.size() + 1)
        lineIds.resize(raw->bytes.size() + 1);
      std::vector<uint32_t> regions;
      size_t lineCount = 0;
      uint32_t distinct = 0, nreg = 0;
      auto internBegin = Clock::now();
      {
        std::lock_guard<std::mutex> lock(dict_mutex_);
        const char empty = 0;
        const char *begin = raw->bytes.empty()
                                ? &empty
                                : reinterpret_cast<const char *>(
                                      raw->bytes.data());
        dict_.process(begin, begin + raw->bytes.size(), lineIds.data(),
                      lineCount, hits, true, &regions);
        distinct = dict_.distinct();
        nreg = uint32_t(dict_.region_count());
      }
      timing_.interning += seconds_since(internBegin);
      auto factorBegin = Clock::now();
      auto prepared = std::make_shared<PreparedTU>(factorizer.process(
          raw->logical, uint32_t(raw->bytes.size()), regions, distinct,
          nreg));
      timing_.factorization += seconds_since(factorBegin);
      raw.reset();
      if (!queue_.push(std::move(prepared))) {
        raw_queue_.fail();
        return;
      }
    }
    if (raw_queue_.failed() || expected != manifest_.paths.size()) {
      queue_.fail();
      return;
    }
    queue_.finish();
  }
};

struct CurveRow {
  uint32_t logical = 0, worker = 0;
  uint64_t raw = 0, wire = 0, latency_ns = 0;
};

struct Pending {
  std::shared_ptr<PreparedTU> prepared;
  uint32_t worker = 0;
  uint64_t wire_start = 0;
  std::vector<uint32_t> missing;
  MixedEncoder::AuthorityTransaction authority;
  Clock::time_point started;
};

struct PreparedFill {
  uint32_t path_base = 0, public_base = 0;
  std::vector<uint8_t> paths;
  std::array<std::vector<uint8_t>, 4> raw;
  capp::EncodedComponent encoded_paths;
  std::array<capp::EncodedComponent, 4> encoded;
};

static int run_pipeline(const Manifest &manifest, const Options &options,
                        Clock::time_point processStart) {
  const cap::SourceGeneration generation = make_generation();
  std::vector<Worker> workers(options.workers);
  for (uint32_t id = 0; id < options.workers; ++id)
    if (!spawn_worker(workers[id], id, workers, manifest, options, generation))
      return 2;

  Interner dict;
  std::mutex dictMutex;
  MixedEncoder encoder;
  encoder.init(0, 0);
  CodecContexts codec;
  std::array<ComponentLedger, CK_COUNT> components{};
  PreparedQueue queue(options.queue_depth);
  ProducerTiming producerTiming;
  auto activeStart = Clock::now();
  SourcePipeline source(manifest, options, workers, queue, dict, dictMutex,
                        producerTiming);
  if (queue.failed())
    return 2;

  std::vector<CurveRow> curve;
  curve.reserve(manifest.paths.size());
  uint64_t rawTotal = 0, preparedAccepted = 0, committed = 0,
           decodeRejected = 0, aborted = 0;
  double transformSeconds = 0;
  size_t cursor = 0;
  bool relationshipExact = true;
  while (cursor < manifest.paths.size()) {
    size_t wanted = std::min<size_t>(options.wave,
                                     manifest.paths.size() - cursor);
    std::vector<std::shared_ptr<PreparedTU>> batch;
    if (!queue.pop_batch(wanted, batch) || batch.empty()) {
      relationshipExact = false;
      break;
    }
    std::vector<Pending> pending(batch.size());
    std::set<uint32_t> assigned;
    for (size_t index = 0; index < batch.size(); ++index) {
      auto &job = pending[index];
      job.prepared = batch[index];
      if (job.prepared->logical != cursor + index) {
        relationshipExact = false;
        break;
      }
      job.worker = job.prepared->logical % options.workers;
      if (!assigned.insert(job.worker).second) {
        relationshipExact = false;
        break;
      }
      auto &worker = workers[job.worker];
      worker.mirror.ensure_dimensions(job.prepared->nreg,
                                      job.prepared->nblk);
      job.wire_start = worker.frames.total();
      job.started = Clock::now();
      auto transformBegin = Clock::now();
      std::vector<uint8_t> rootRaw;
      for (uint64_t token : job.prepared->tokens)
        put_varint(rootRaw, token);
      std::vector<const BlockDefinition *> definitions;
      for (const auto &block : job.prepared->blocks)
        if (!worker.mirror.blocks[block.id])
          definitions.push_back(&block);
      std::vector<uint8_t> blockRaw;
      if (!definitions.empty()) {
        put_varint(blockRaw, definitions.size());
        for (const BlockDefinition *block : definitions) {
          put_varint(blockRaw, block->id);
          blockRaw.push_back(0); // literal Region-child vector
          put_varint(blockRaw, block->children.size());
          for (uint32_t child : block->children)
            put_varint(blockRaw, child);
          worker.mirror.blocks[block->id] = 1;
        }
      }
      auto root = encode_component(rootRaw, options.policy, codec,
                                   components[CK_ROOT]);
      auto blocks = encode_component(blockRaw, options.policy, codec,
                                     components[CK_BLOCK]);
      transformSeconds += seconds_since(transformBegin);
      if (!send_counted(worker.fd, cap::Frame::Root,
                        capp::pack_root_m4(job.prepared->logical, root.wire,
                                           blocks.wire),
                        worker.frames)) {
        relationshipExact = false;
        break;
      }
    }
    if (!relationshipExact)
      break;

    for (auto &job : pending) {
      auto &worker = workers[job.worker];
      cap::Frame frame;
      std::vector<uint8_t> payload, needWire, needRaw;
      uint32_t logical = 0;
      if (!recv_counted(worker.fd, frame, payload, worker.frames) ||
          frame != cap::Frame::Need ||
          !capp::try_unpack_need_m4(payload, logical, needWire) ||
          logical != job.prepared->logical ||
          !capp::decode_component(needWire, codec.decoder, needRaw) ||
          !parse_need(needRaw, job.prepared->nreg, job.missing)) {
        relationshipExact = false;
        break;
      }
      components[CK_NEED].received(needWire, needRaw.size());
    }
    if (!relationshipExact)
      break;

    std::vector<PreparedFill> fills(pending.size());
    for (size_t index = 0; index < pending.size(); ++index) {
      auto &job = pending[index];
      auto &worker = workers[job.worker];
      auto &fill = fills[index];
      auto transformBegin = Clock::now();
      {
        std::lock_guard<std::mutex> lock(dictMutex);
        uint32_t currentNreg = uint32_t(dict.region_count());
        encoder.ensure_dimensions(dict.distinct(), currentNreg);
        worker.mirror.ensure_dimensions(currentNreg,
                                        job.prepared->nblk);
        fill.path_base = worker.mirror.path_count;
        capm5::MirrorScope scope(encoder, worker.mirror);
        encoder.begin_authority_transaction(job.authority);
        encoder.materialize(dict, job.missing, job.prepared->logical,
                            &job.authority);
        fill.public_base = encoder.fill_public_base;
        if (fill.path_base > encoder.paths.size()) {
          relationshipExact = false;
          break;
        }
        for (size_t path = fill.path_base; path < encoder.paths.size(); ++path) {
          put_varint(fill.paths, encoder.paths[path].size());
          fill.paths.insert(fill.paths.end(), encoder.paths[path].begin(),
                            encoder.paths[path].end());
        }
        worker.mirror.path_count = uint32_t(encoder.paths.size());
        for (size_t part = 0; part < 4; ++part)
          fill.raw[part] = encoder.mixedRaw[part];
      }
      fill.encoded_paths = encode_component(fill.paths, options.policy, codec,
                                            components[CK_PATH]);
      for (size_t part = 0; part < 4; ++part)
        fill.encoded[part] =
            encode_component(fill.raw[part], options.policy, codec,
                             components[CK_CONTROL + part]);
      transformSeconds += seconds_since(transformBegin);
    }
    if (!relationshipExact)
      break;

    std::vector<uint8_t> sent(pending.size(), 0);
    std::vector<std::thread> senders;
    senders.reserve(pending.size());
    for (size_t index = 0; index < pending.size(); ++index)
      senders.emplace_back([&, index] {
        auto &job = pending[index];
        auto &worker = workers[job.worker];
        auto &fill = fills[index];
        std::array<std::vector<uint8_t>, 6> mixed;
        for (size_t part = 0; part < 4; ++part)
          mixed[part] = fill.encoded[part].wire;
        sent[index] = send_counted(
            worker.fd, cap::Frame::Fill,
            capp::pack_fill_m4(job.prepared->logical, fill.path_base,
                               fill.public_base, fill.encoded_paths.wire, mixed,
                               4),
            worker.frames);
      });
    for (auto &sender : senders)
      sender.join();
    if (std::find(sent.begin(), sent.end(), uint8_t(0)) != sent.end()) {
      relationshipExact = false;
      break;
    }

    std::vector<uint8_t> preparedReplies(pending.size(), 0);
    for (size_t index = 0; index < pending.size(); ++index) {
      auto &job = pending[index];
      auto &worker = workers[job.worker];
      cap::Frame frame;
      std::vector<uint8_t> payload;
      capp::CacheDropsM5 drops;
      uint32_t logical = 0;
      bool accepted = false;
      if (!recv_counted(worker.fd, frame, payload, worker.frames) ||
          frame != cap::Frame::Ack ||
          !capp::try_unpack_tu_ack_m5(
              payload, logical, accepted, drops,
              uint32_t(worker.mirror.regions.size()),
              uint32_t(worker.mirror.blocks.size())) ||
          logical != job.prepared->logical || !drops.regions.empty() ||
          !drops.public_lines.empty() || !drops.blocks.empty()) {
        relationshipExact = false;
        break;
      }
      preparedReplies[index] = accepted ? 1 : 0;
      if (accepted)
        ++preparedAccepted;
      else
        ++decodeRejected;
    }
    if (!relationshipExact)
      break;

    size_t firstRejected = pending.size();
    for (size_t index = 0; index < pending.size(); ++index)
      if (!preparedReplies[index]) {
        firstRejected = index;
        break;
      }
    for (size_t index = 0; index < pending.size(); ++index) {
      if (!preparedReplies[index])
        continue;
      auto &job = pending[index];
      auto &worker = workers[job.worker];
      bool commit = index < firstRejected;
      if (!send_counted(worker.fd, cap::Frame::Ack,
                        capp::pack_tu_ack(job.prepared->logical, commit),
                        worker.frames)) {
        relationshipExact = false;
        break;
      }
    }
    if (!relationshipExact)
      break;

    struct FinalReply {
      bool accepted = false;
      capp::CacheDropsM5 drops;
      Clock::time_point received;
    };
    std::vector<FinalReply> replies(pending.size());
    for (size_t index = 0; index < pending.size(); ++index) {
      if (!preparedReplies[index])
        continue;
      auto &job = pending[index];
      auto &worker = workers[job.worker];
      cap::Frame frame;
      std::vector<uint8_t> payload;
      uint32_t logical = 0;
      if (!recv_counted(worker.fd, frame, payload, worker.frames) ||
          frame != cap::Frame::Ack ||
          !capp::try_unpack_tu_ack_m5(
              payload, logical, replies[index].accepted,
              replies[index].drops, uint32_t(worker.mirror.regions.size()),
              uint32_t(worker.mirror.blocks.size())) ||
          logical != job.prepared->logical ||
          replies[index].accepted != (index < firstRejected)) {
        relationshipExact = false;
        break;
      }
      replies[index].received = Clock::now();
      if (replies[index].accepted)
        ++committed;
      else
        ++aborted;
    }
    if (!relationshipExact)
      break;

    for (size_t index = 0; index < firstRejected; ++index) {
      auto &job = pending[index];
      auto &worker = workers[job.worker];
      encoder.commit_authority_transaction(job.authority);
      worker.mirror.apply(replies[index].drops);
      uint64_t wire = worker.frames.total() - job.wire_start +
                      worker.pending_control;
      worker.pending_control = 0;
      uint64_t latency = uint64_t(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              replies[index].received - job.started)
              .count());
      curve.push_back({job.prepared->logical, job.worker,
                       job.prepared->raw_length, wire, latency});
      rawTotal += job.prepared->raw_length;
      ++worker.accepted;
      worker.accepted_raw += job.prepared->raw_length;
    }
    if (firstRejected != pending.size()) {
      for (size_t index = pending.size(); index-- > firstRejected;)
        encoder.rollback_authority_transaction(pending[index].authority);
      relationshipExact = false;
      break;
    }
    cursor += batch.size();
  }

  if (!relationshipExact)
    queue.fail();
  bool sourceExact = source.finish();
  for (auto &worker : workers)
    if (!finish_worker(worker))
      relationshipExact = false;

  std::sort(curve.begin(), curve.end(),
            [](const CurveRow &left, const CurveRow &right) {
              return left.logical < right.logical;
            });
  for (uint32_t id = 0; id < workers.size(); ++id) {
    if (!workers[id].pending_control)
      continue;
    auto found = std::find_if(curve.rbegin(), curve.rend(),
                              [&](const CurveRow &row) {
                                return row.worker == id;
                              });
    if (found == curve.rend()) {
      relationshipExact = false;
      continue;
    }
    found->wire += workers[id].pending_control;
    workers[id].pending_control = 0;
  }
  const double activeSeconds = seconds_since(activeStart);
  const double completeSeconds = seconds_since(processStart);
  struct rusage coordinatorUsage {};
  getrusage(RUSAGE_SELF, &coordinatorUsage);

  uint64_t socketBytes = 0, compilerBytes = 0, compilerTus = 0,
           compilerNs = 0, decodeNs = 0, pathNs = 0, failures = 0,
           peakRss = 0;
  capm5::CacheTotals cacheTotals;
  std::array<uint64_t, 7> frameBytes{}, frameCount{};
  for (const auto &worker : workers) {
    socketBytes += worker.frames.total();
    compilerBytes += worker.summary.compiler_pipe_bytes;
    compilerTus += worker.summary.compiler_pipe_tus;
    compilerNs += worker.summary.compiler_pipe_ns;
    decodeNs += worker.summary.decode_ns;
    pathNs += worker.summary.path_ns;
    failures += worker.summary.failures;
    peakRss = std::max(peakRss, worker.summary.peak_rss_kib);
    cacheTotals.region_bytes += worker.summary.cache.region_bytes;
    cacheTotals.public_bytes += worker.summary.cache.public_bytes;
    cacheTotals.block_bytes += worker.summary.cache.block_bytes;
    cacheTotals.regions += worker.summary.cache.regions;
    cacheTotals.public_lines += worker.summary.cache.public_lines;
    cacheTotals.blocks += worker.summary.cache.blocks;
    cacheTotals.region_removals += worker.summary.cache.region_removals;
    cacheTotals.public_removals += worker.summary.cache.public_removals;
    cacheTotals.block_removals += worker.summary.cache.block_removals;
    cacheTotals.compactions += worker.summary.cache.compactions;
    for (size_t index = 0; index < frameBytes.size(); ++index) {
      frameBytes[index] += worker.frames.bytes[index];
      frameCount[index] += worker.frames.count[index];
    }
    if (!worker.summary.exact || worker.summary.verified != worker.accepted ||
        worker.summary.raw != worker.accepted_raw ||
        worker.summary.compiler_pipe_bytes != worker.accepted_raw ||
        worker.summary.compiler_pipe_tus != worker.accepted)
      relationshipExact = false;
  }
  uint64_t curveRaw = 0, curveWire = 0;
  for (const auto &row : curve) {
    curveRaw += row.raw;
    curveWire += row.wire;
  }
  if (!sourceExact || rawTotal != manifest.raw || curveRaw != rawTotal ||
      curveWire != socketBytes || compilerBytes != rawTotal ||
      compilerTus != curve.size() || committed != curve.size() ||
      preparedAccepted != committed + aborted || decodeRejected != 0 ||
      aborted != 0)
    relationshipExact = false;

  if (options.curve_out) {
    std::ofstream output(options.curve_out);
    output << "logical\tphysical\tworker\traw\twire\tlatency_ns\t"
              "cumulative_raw\tcumulative_wire\n";
    uint64_t cumulativeRaw = 0, cumulativeWire = 0;
    for (const auto &row : curve) {
      cumulativeRaw += row.raw;
      cumulativeWire += row.wire;
      output << row.logical << '\t' << row.logical << '\t' << row.worker
             << '\t' << row.raw << '\t' << row.wire << '\t'
             << row.latency_ns << '\t' << cumulativeRaw << '\t'
             << cumulativeWire << '\n';
    }
    if (!output)
      relationshipExact = false;
  }

  static const char *const frameNames[] = {"Hello", "Root", "Need", "Fill",
                                           "Done",  "Ack",  "Rejoin"};
  printf("FRAME_LEDGER");
  for (size_t index = 0; index < frameBytes.size(); ++index)
    printf(" %s=%llu/%llu", frameNames[index],
           (unsigned long long)frameBytes[index],
           (unsigned long long)frameCount[index]);
  printf(" total=%llu closure=%s\n", (unsigned long long)socketBytes,
         curveWire == socketBytes ? "OK" : "FAIL");
  printf("RESULT exact=%s codec=%s order=standard assignment=roundrobin "
         "workers=%u wave=%u tus=%zu raw=%llu actual_socket=%llu ratio=%.3f "
         "failures=%llu snapshot=none\n",
         relationshipExact ? "OK" : "FAIL",
         options.policy == capp::CodecPolicy::Zstd1 ? "z1" : "z3",
         options.workers, options.wave, curve.size(),
         (unsigned long long)rawTotal, (unsigned long long)socketBytes,
         socketBytes ? double(rawTotal) / socketBytes : 0.0,
         (unsigned long long)failures);
  printf("TRANSACTIONS prepared_accepted=%llu decode_rejected=%llu "
         "committed=%llu aborted=%llu closure=%s\n",
         (unsigned long long)preparedAccepted,
         (unsigned long long)decodeRejected, (unsigned long long)committed,
         (unsigned long long)aborted,
         preparedAccepted == committed + aborted && committed == curve.size()
             ? "OK"
             : "FAIL");
  auto summary = capm5::summarize_curve(curve);
  printf("CURVE_SUMMARY C50=%.3f C50_TU=%u H200=", summary.c50,
         summary.c50_tu);
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
  printf("PREP_RATE source_pipe=%.3f GB/s interning=%.3f GB/s "
         "factorization=%.3f GB/s\n",
         producerTiming.source_pipe
             ? rawTotal / producerTiming.source_pipe / 1e9
             : 0.0,
         producerTiming.interning ? rawTotal / producerTiming.interning / 1e9
                                  : 0.0,
         producerTiming.factorization
             ? rawTotal / producerTiming.factorization / 1e9
             : 0.0);
  printf("THROUGHPUT C_transform=%.3f GB/s F_decode_cpu=%.3f GB/s "
         "relationship=%.3f GB/s wall=%.6fs C_peak=%.1fMiB "
         "F_peak=%.1fMiB\n",
         transformSeconds ? rawTotal / transformSeconds / 1e9 : 0.0,
         decodeNs ? rawTotal / (double(decodeNs) / 1e9) / 1e9 : 0.0,
         activeSeconds ? rawTotal / activeSeconds / 1e9 : 0.0, activeSeconds,
         coordinatorUsage.ru_maxrss / 1024.0, peakRss / 1024.0);
  printf("PIPE_PATH mode=real C_pipe_to_wire=%.3f GB/s "
         "F_wire_to_compiler_pipe=%.3f GB/s F_aggregate=%.3f GB/s "
         "F_pipe_write=%.3f GB/s complete=%.3f GB/s prepare=0.000000s "
         "compiler_bytes=%llu compiler_tus=%llu "
         "compiler_measured_bytes=%llu compiler_summary_bytes=%llu "
         "compiler_summary_tus=%llu summary_lost_workers=0 "
         "queue_high=%zu/%zu "
         "raw_queue_high=%zu/%zu\n",
         activeSeconds ? rawTotal / activeSeconds / 1e9 : 0.0,
         pathNs ? rawTotal / (double(pathNs) / 1e9) / 1e9 : 0.0,
         activeSeconds ? rawTotal / activeSeconds / 1e9 : 0.0,
         compilerNs ? rawTotal / (double(compilerNs) / 1e9) / 1e9 : 0.0,
         completeSeconds ? rawTotal / completeSeconds / 1e9 : 0.0,
         (unsigned long long)compilerBytes, (unsigned long long)compilerTus,
         (unsigned long long)compilerBytes, (unsigned long long)compilerBytes,
         (unsigned long long)compilerTus, queue.high_count(), queue.high_bytes(),
         source.raw_high_count(), source.raw_high_bytes());
  for (size_t index = 0; index < CK_COUNT; ++index) {
    const auto &part = components[index];
    printf("COMPONENT %-13s raw=%llu selected=%llu choices=%llu/%llu/%llu "
           "frames=%llu\n",
           COMPONENT_NAMES[index], (unsigned long long)part.raw,
           (unsigned long long)part.selected,
           (unsigned long long)part.raw_selected,
           (unsigned long long)part.z1_selected,
           (unsigned long long)part.z3_selected,
           (unsigned long long)part.count);
  }
  return relationshipExact ? 0 : 1;
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
  auto processStart = Clock::now();
  Options options;
  for (int index = 1; index < argc; ++index) {
    if (!strcmp(argv[index], "--manifest") && index + 1 < argc)
      options.manifest = argv[++index];
    else if (!strcmp(argv[index], "--max-files") && index + 1 < argc) {
      uint64_t value = 0;
      if (!parse_u64(argv[++index], value) || !value || value > SIZE_MAX)
        return 2;
      options.max_files = size_t(value);
    } else if (!strcmp(argv[index], "--workers") && index + 1 < argc) {
      if (!parse_u32(argv[++index], options.workers) || !options.workers ||
          options.workers > 32)
        return 2;
    } else if (!strcmp(argv[index], "--wave") && index + 1 < argc) {
      if (!parse_u32(argv[++index], options.wave) || !options.wave ||
          options.wave > 32)
        return 2;
    } else if (!strcmp(argv[index], "--queue-depth") && index + 1 < argc) {
      if (!parse_u32(argv[++index], options.queue_depth) ||
          !options.queue_depth || options.queue_depth > 1024)
        return 2;
    } else if (!strcmp(argv[index], "--raw-queue-depth") &&
               index + 1 < argc) {
      if (!parse_u32(argv[++index], options.raw_queue_depth) ||
          !options.raw_queue_depth || options.raw_queue_depth > 32)
        return 2;
    } else if (!strcmp(argv[index], "--codec") && index + 1 < argc) {
      const char *value = argv[++index];
      if (!strcmp(value, "z1"))
        options.policy = capp::CodecPolicy::Zstd1;
      else if (!strcmp(value, "z3"))
        options.policy = capp::CodecPolicy::Zstd3;
      else
        return 2;
    } else if (!strcmp(argv[index], "--region-bytes") && index + 1 < argc) {
      if (!parse_u64(argv[++index], options.cache_limits.region_bytes))
        return 2;
    } else if (!strcmp(argv[index], "--public-bytes") && index + 1 < argc) {
      if (!parse_u64(argv[++index], options.cache_limits.public_bytes))
        return 2;
    } else if (!strcmp(argv[index], "--block-bytes") && index + 1 < argc) {
      if (!parse_u64(argv[++index], options.cache_limits.block_bytes))
        return 2;
    } else if (!strcmp(argv[index], "--curve-out") && index + 1 < argc) {
      options.curve_out = argv[++index];
    } else if (!strcmp(argv[index], "--real-pipes")) {
      // Required by the shared launcher; this executable is always pipe-shaped.
    } else {
      fprintf(stderr, "unknown option: %s\n", argv[index]);
      return 2;
    }
  }
  if (!options.manifest || options.wave > options.workers ||
      options.queue_depth < options.wave) {
    fprintf(stderr,
            "usage: %s --manifest F [--workers N --wave N --queue-depth N] "
            "[--raw-queue-depth N] [--codec z1|z3] [--curve-out F] "
            "[--real-pipes]\n",
            argv[0]);
    return 2;
  }
  Manifest manifest;
  if (!read_manifest(options.manifest, options.max_files, manifest) ||
      options.workers > manifest.paths.size())
    return 2;
  return run_pipeline(manifest, options, processStart);
}
