#pragma once

#include "p29_online_s1.h"
#include "services/digest128.h"

#include <zstd.h>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(ICECC_P50SIM_ALLOCATION_COUNTER)
extern "C" unsigned
icecc_p50sim_p29_allocation_scope_enter(unsigned entry) noexcept;
extern "C" void
icecc_p50sim_p29_allocation_scope_leave(unsigned previous) noexcept;
#endif

namespace icecc::codec {

enum class P29WireKind : std::uint8_t {
  Root = 1,
  BlockDefinition = 2,
  Need = 3,
  PathDefinition = 5,
  FillControl = 8,
  FillLiteral = 9,
  TuEnd = 0xfe,
};

struct P29SourceTextView {
  bool available = false;
  std::span<const std::uint8_t> bytes;
  // Line i is [offsets[i], offsets[i + 1]).  A complete view starts at zero
  // and ends at bytes.size().
  std::span<const std::uint32_t> offsets;
};

struct P29WireLimits {
  std::uint32_t max_regions = std::uint32_t{1} << 24;
  std::uint32_t max_blocks = std::uint32_t{1} << 24;
  std::uint32_t max_paths = std::uint32_t{1} << 20;
  std::uint32_t max_public_lines = std::uint32_t{1} << 24;
  std::size_t max_region_bytes = std::size_t{2} << 30;
  std::size_t max_tu_bytes = std::size_t{1} << 30;
  std::size_t max_block_children = std::size_t{1} << 27;
  std::size_t max_occurrences = std::size_t{1} << 30;
};

[[nodiscard]] inline std::size_t
p29v1_need_inner_bound(const P29WireLimits &limits) {
  const std::size_t occurrence_bound =
      limits.max_occurrences >
              (std::numeric_limits<std::size_t>::max() - 20) / 5
          ? std::numeric_limits<std::size_t>::max()
          : 20 + 5 * limits.max_occurrences;
  return std::min(limits.max_tu_bytes, occurrence_bound);
}

[[nodiscard]] inline std::size_t
p29v1_fill_inner_bound(const P29WireLimits &limits) {
  constexpr std::size_t path_allowance = std::size_t{1} << 20;
  if (limits.max_tu_bytes >
      (std::numeric_limits<std::size_t>::max() - path_allowance) / 2)
    return std::numeric_limits<std::size_t>::max();
  return 2 * limits.max_tu_bytes + path_allowance;
}

struct P29MixedCLineState {
  std::uint32_t source_region = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t source_offset = 0;
  std::uint32_t public_id = 0;
  bool operator==(const P29MixedCLineState &) const = default;
};

struct P29MixedFLineView {
  std::uint32_t source_region = 0;
  std::uint32_t source_offset = 0;
  std::uint32_t length = 0;
  bool operator==(const P29MixedFLineView &) const = default;
};

struct P29MixedFRegionView {
  std::uint32_t segment = 0;
  std::size_t offset = 0;
  std::uint32_t length = 0;
  bool known = false;
  bool operator==(const P29MixedFRegionView &) const = default;
};

struct P29FBlockView {
  std::size_t offset = 0;
  std::uint32_t length = 0;
  bool known = false;
  bool operator==(const P29FBlockView &) const = default;
};

// These structures deliberately live in the provider.  The algorithm owns no
// hidden per-route dictionary: product accounting can therefore charge the
// exact state without pretending that it is an immutable-object installation.
struct P29SenderRouteState {
  std::unordered_map<std::string, std::uint32_t> path_ids;
  std::vector<std::string> paths;
  std::vector<P29MixedCLineState> mixed_lines;
  std::uint32_t next_public = 1;
  std::vector<std::uint8_t> fknown_regions;
  std::vector<std::uint8_t> fknown_blocks;
  std::size_t known_region_count = 0;
  std::size_t known_block_count = 0;
  std::uint64_t revision = 0;
  bool system_source_reuse = false;
  bool operator==(const P29SenderRouteState &) const = default;
};

struct P29ReceiverRouteState {
  std::unordered_map<std::string, std::uint32_t> path_ids;
  std::vector<std::string> paths;
  // One immutable byte segment per committed TU.  Moving the staged segment
  // at commit avoids recopying every historical Region while preserving a
  // transaction boundary: pending bytes are not reachable through this state.
  std::vector<std::vector<std::uint8_t>> region_segments;
  std::vector<P29MixedFRegionView> regions;
  std::vector<P29MixedFLineView> public_lines{{}};
  std::vector<P29FBlockView> blocks;
  std::vector<std::uint32_t> block_children;
  std::vector<std::uint32_t> occurrences;
  std::size_t known_region_count = 0;
  std::size_t known_block_count = 0;
  std::uint64_t revision = 0;
  bool system_source_reuse = false;
  bool operator==(const P29ReceiverRouteState &) const = default;
};

static_assert(sizeof(P29MixedCLineState) == 12);

// Stable logical accounting for sender route state. Both path containers own
// their own string, so charge both copies; allocator-specific bucket/node
// slack is deliberately not part of the cross-build resource contract.
[[nodiscard]] inline std::uint64_t
p29_sender_route_state_bytes(const P29SenderRouteState &state) noexcept {
  std::uint64_t result = 0;
  const auto add = [&result](std::uint64_t bytes) {
    result = bytes > std::numeric_limits<std::uint64_t>::max() - result
                 ? std::numeric_limits<std::uint64_t>::max()
                 : result + bytes;
  };
  const auto multiply = [](std::uint64_t count,
                           std::uint64_t width) noexcept {
    return count != 0 &&
                   width > std::numeric_limits<std::uint64_t>::max() / count
               ? std::numeric_limits<std::uint64_t>::max()
               : count * width;
  };
  add(multiply(state.paths.size(), sizeof(std::string)));
  add(multiply(state.path_ids.size(),
               sizeof(std::string) + sizeof(std::uint32_t)));
  for (const std::string &path : state.paths)
    add(multiply(path.size(), 2));
  add(multiply(state.mixed_lines.size(), sizeof(P29MixedCLineState)));
  add(state.fknown_regions.size());
  add(state.fknown_blocks.size());
  return result;
}

template <class P>
concept P29SenderProvider = requires(P provider, std::string_view path) {
  { P::fail(static_cast<const char *>(nullptr)) } -> std::same_as<void>;
  { provider.sender_route() } -> std::same_as<P29SenderRouteState &>;
  { provider.source_text(path) } -> std::same_as<P29SourceTextView>;
  { provider.wire_limits() } -> std::same_as<P29WireLimits>;
};

template <class P>
concept P29ReceiverProvider =
    requires(P provider, std::string_view path, std::uint32_t id,
             std::span<const std::uint8_t> bytes,
             std::span<const std::uint32_t> children) {
  { P::fail(static_cast<const char *>(nullptr)) } -> std::same_as<void>;
  { provider.receiver_route() } -> std::same_as<P29ReceiverRouteState &>;
  { provider.source_text(path) } -> std::same_as<P29SourceTextView>;
  { provider.wire_limits() } -> std::same_as<P29WireLimits>;
  {provider.begin_wire_publish()};
  {provider.publish_wire_region(id, bytes)};
  {provider.publish_wire_block(id, children)};
  {provider.commit_wire_publish()};
  { provider.abandon_wire_publish() }
  noexcept;
};

template <class D>
concept P29WireDictionary = requires(const D dictionary, std::uint32_t id) {
  { dictionary.line(id) } -> std::convertible_to<std::span<const std::uint8_t>>;
  {
    dictionary.region_lines(id)
    } -> std::convertible_to<std::span<const std::uint32_t>>;
  {
    dictionary.region_bytes(id)
    } -> std::convertible_to<std::span<const std::uint8_t>>;
};

namespace p29_wire_detail {

#if defined(ICECC_P50SIM_ALLOCATION_COUNTER)
enum class P29AllocationEntry : unsigned {
  SenderBeginTu,
  SenderAnswerNeed,
  SenderCommit,
  SenderAbandon,
  ReceiverReceiveBody,
  ReceiverReceiveFill,
  ReceiverTakeMaterialized,
  ReceiverCommit,
  ReceiverAbandon,
};

class P29AllocationScope {
public:
  explicit P29AllocationScope(P29AllocationEntry entry) noexcept
      : previous_(icecc_p50sim_p29_allocation_scope_enter(
            static_cast<unsigned>(entry))) {
  }
  ~P29AllocationScope() {
    icecc_p50sim_p29_allocation_scope_leave(previous_);
  }
  P29AllocationScope(const P29AllocationScope &) = delete;
  P29AllocationScope &operator=(const P29AllocationScope &) = delete;

private:
  unsigned previous_;
};
#endif

constexpr std::uint64_t kTagIdLimit = std::uint64_t{1} << 31;

[[nodiscard]] inline bool is_block_tag(std::uint32_t tag) {
  return (tag & 1U) != 0;
}

[[nodiscard]] inline std::uint32_t tag_id(std::uint32_t tag) {
  return tag >> 1;
}

[[nodiscard]] inline std::uint32_t make_tag(std::uint64_t id, bool block) {
  if (id >= kTagIdLimit)
    throw std::overflow_error("P29 ordinal exceeds typed-tag space");
  return (static_cast<std::uint32_t>(id) << 1) | (block ? 1U : 0U);
}

inline void put_varint(std::vector<std::uint8_t> &output, std::uint64_t value) {
  while (value >= 0x80) {
    output.push_back(static_cast<std::uint8_t>(value) | 0x80);
    value >>= 7;
  }
  output.push_back(static_cast<std::uint8_t>(value));
}

inline void put_zigzag(std::vector<std::uint8_t> &output, std::int64_t value) {
  put_varint(output, (std::uint64_t(value) << 1) ^ std::uint64_t(value >> 63));
}

class Cursor {
public:
  explicit Cursor(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

  [[nodiscard]] bool empty() const { return position_ == bytes_.size(); }
  [[nodiscard]] std::size_t remaining() const {
    return bytes_.size() - position_;
  }

  [[nodiscard]] std::uint8_t byte() {
    if (empty())
      throw std::invalid_argument("P29 byte stream is truncated");
    return bytes_[position_++];
  }

  [[nodiscard]] std::uint64_t varint() {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift != 70; shift += 7) {
      const std::uint8_t next = byte();
      if (shift == 63 && (next & 0xfeU))
        throw std::invalid_argument("P29 varint overflows u64");
      value |= std::uint64_t(next & 0x7fU) << shift;
      if (!(next & 0x80U)) {
        if (shift && next == 0)
          throw std::invalid_argument("P29 varint is non-minimal");
        return value;
      }
    }
    throw std::invalid_argument("P29 varint is unterminated");
  }

  [[nodiscard]] std::int64_t zigzag() {
    const std::uint64_t value = varint();
    return std::int64_t(value >> 1) ^ -std::int64_t(value & 1);
  }

  [[nodiscard]] std::span<const std::uint8_t> take(std::size_t size) {
    if (size > remaining())
      throw std::invalid_argument("P29 byte stream is truncated");
    const std::span<const std::uint8_t> result =
        bytes_.subspan(position_, size);
    position_ += size;
    return result;
  }

private:
  std::span<const std::uint8_t> bytes_;
  std::size_t position_ = 0;
};

struct FrameView {
  P29WireKind kind{};
  std::span<const std::uint8_t> payload;
};

inline void append_frame(std::vector<std::uint8_t> &output, P29WireKind kind,
                         std::span<const std::uint8_t> payload) {
  if (payload.size() > std::numeric_limits<std::uint32_t>::max())
    throw std::overflow_error("P29 frame exceeds u32 length");
  output.push_back(static_cast<std::uint8_t>(kind));
  for (unsigned byte = 0; byte != 4; ++byte)
    output.push_back(static_cast<std::uint8_t>(payload.size() >> (8 * byte)));
  if (!payload.empty())
    output.insert(output.end(), payload.begin(), payload.end());
}

[[nodiscard]] inline std::vector<FrameView>
parse_frames(std::span<const std::uint8_t> bytes,
             std::size_t max_frames = std::numeric_limits<std::size_t>::max()) {
  std::vector<FrameView> frames;
  Cursor cursor(bytes);
  while (!cursor.empty()) {
    if (frames.size() >= max_frames)
      throw std::invalid_argument("P29 frame count exceeds message limit");
    const auto kind = static_cast<P29WireKind>(cursor.byte());
    std::uint32_t length = 0;
    for (unsigned byte = 0; byte != 4; ++byte)
      length |= std::uint32_t(cursor.byte()) << (8 * byte);
    frames.push_back({kind, cursor.take(length)});
  }
  return frames;
}

class MessageCodec {
public:
  MessageCodec()
      : encoder_(ZSTD_createCCtx(), ZSTD_freeCCtx),
        decoder_(ZSTD_createDCtx(), ZSTD_freeDCtx) {
    if (!encoder_ || !decoder_)
      throw std::bad_alloc();
  }

  [[nodiscard]] std::vector<std::uint8_t>
  encode(std::span<const std::uint8_t> raw, int level = 3) {
    check(ZSTD_CCtx_reset(encoder_.get(), ZSTD_reset_session_and_parameters),
          "message encoder reset");
    check(
        ZSTD_CCtx_setParameter(encoder_.get(), ZSTD_c_compressionLevel, level),
        "message compression level");
    std::vector<std::uint8_t> output(ZSTD_compressBound(raw.size()));
    const std::uint8_t empty = 0;
    const void *source = raw.empty() ? static_cast<const void *>(&empty)
                                     : static_cast<const void *>(raw.data());
    const std::size_t size = ZSTD_compress2(encoder_.get(), output.data(),
                                            output.size(), source, raw.size());
    check(size, "message encode");
    output.resize(size);
    return output;
  }

  [[nodiscard]] std::vector<std::uint8_t>
  decode(std::span<const std::uint8_t> encoded,
         std::size_t max_output = std::numeric_limits<std::size_t>::max()) {
    if (encoded.empty())
      throw std::invalid_argument("P29 zstd message is absent");
    const std::size_t frame_size =
        ZSTD_findFrameCompressedSize(encoded.data(), encoded.size());
    check(frame_size, "message frame size");
    if (frame_size != encoded.size())
      throw std::invalid_argument("P29 zstd message has trailing bytes");
    const unsigned long long expected =
        ZSTD_getFrameContentSize(encoded.data(), encoded.size());
    if (expected == ZSTD_CONTENTSIZE_ERROR ||
        expected == ZSTD_CONTENTSIZE_UNKNOWN || expected > SIZE_MAX ||
        expected > max_output)
      throw std::invalid_argument("P29 zstd message has no bounded size");
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(expected));
    std::uint8_t empty = 0;
    void *destination = raw.empty() ? static_cast<void *>(&empty)
                                    : static_cast<void *>(raw.data());
    const std::size_t size =
        ZSTD_decompressDCtx(decoder_.get(), destination, raw.size(),
                            encoded.data(), encoded.size());
    check(size, "message decode");
    if (size != raw.size())
      throw std::invalid_argument("P29 zstd message size differs");
    return raw;
  }

private:
  static void check(std::size_t result, const char *operation) {
    if (ZSTD_isError(result))
      throw std::runtime_error(std::string("P29 zstd ") + operation + ": " +
                               ZSTD_getErrorName(result));
  }

  std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> encoder_;
  std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> decoder_;
};

class ContinuingEncoder {
public:
  ContinuingEncoder() : context_(ZSTD_createCCtx(), ZSTD_freeCCtx) {
    if (!context_)
      throw std::bad_alloc();
    reset();
  }

  void reset() {
    check(ZSTD_CCtx_reset(context_.get(), ZSTD_reset_session_and_parameters),
          "continuing encoder reset");
    check(ZSTD_CCtx_setParameter(context_.get(), ZSTD_c_compressionLevel, 3),
          "continuing compression level");
    check(ZSTD_CCtx_setParameter(context_.get(), ZSTD_c_contentSizeFlag, 0),
          "continuing content-size flag");
    active_ = false;
    closed_ = false;
  }

  [[nodiscard]] std::vector<std::uint8_t>
  encode(std::span<const std::uint8_t> raw, bool close) {
    if (closed_)
      throw std::logic_error("P29 continuing encoder is closed");
    std::vector<std::uint8_t> output;
    if (!raw.empty()) {
      append(raw, ZSTD_e_flush, output);
      active_ = true;
    }
    if (close && active_) {
      append({}, ZSTD_e_end, output);
      active_ = false;
      closed_ = true;
    } else if (close) {
      closed_ = true;
    }
    return output;
  }

  [[nodiscard]] bool active() const { return active_; }
  [[nodiscard]] bool closed() const { return closed_; }

private:
  void append(std::span<const std::uint8_t> raw, ZSTD_EndDirective directive,
              std::vector<std::uint8_t> &output) {
    ZSTD_inBuffer input{raw.data(), raw.size(), 0};
    std::vector<std::uint8_t> buffer(ZSTD_CStreamOutSize());
    std::size_t remaining = 0;
    do {
      ZSTD_outBuffer destination{buffer.data(), buffer.size(), 0};
      remaining =
          ZSTD_compressStream2(context_.get(), &destination, &input, directive);
      check(remaining, "continuing encode");
      output.insert(output.end(), buffer.begin(),
                    buffer.begin() + destination.pos);
    } while (input.pos < input.size || remaining != 0);
  }

  static void check(std::size_t result, const char *operation) {
    if (ZSTD_isError(result))
      throw std::runtime_error(std::string("P29 zstd ") + operation + ": " +
                               ZSTD_getErrorName(result));
  }

  std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> context_;
  bool active_ = false;
  bool closed_ = false;
};

class ContinuingDecoder {
public:
  ContinuingDecoder() : context_(ZSTD_createDCtx(), ZSTD_freeDCtx) {
    if (!context_)
      throw std::bad_alloc();
    reset();
  }

  void reset() {
    check(ZSTD_DCtx_reset(context_.get(), ZSTD_reset_session_and_parameters),
          "continuing decoder reset");
    active_ = false;
    closed_ = false;
  }

  [[nodiscard]] std::vector<std::uint8_t>
  decode(std::span<const std::uint8_t> encoded, bool close,
         std::size_t max_output = std::numeric_limits<std::size_t>::max()) {
    if (closed_)
      throw std::logic_error("P29 continuing decoder is closed");
    ZSTD_inBuffer input{encoded.data(), encoded.size(), 0};
    std::vector<std::uint8_t> raw;
    std::vector<std::uint8_t> buffer(ZSTD_DStreamOutSize());
    std::size_t remaining = 1;
    bool again = false;
    do {
      ZSTD_outBuffer output{buffer.data(), buffer.size(), 0};
      const std::size_t before = input.pos;
      remaining = ZSTD_decompressStream(context_.get(), &output, &input);
      check(remaining, "continuing decode");
      if (raw.size() > max_output || output.pos > max_output - raw.size())
        throw std::invalid_argument(
            "P29 continuing stream exceeds provider limit");
      raw.insert(raw.end(), buffer.begin(), buffer.begin() + output.pos);
      if (remaining == 0 && input.pos != input.size)
        throw std::invalid_argument(
            "P29 continuing payload contains multiple zstd frames");
      if (input.pos == before && output.pos == 0) {
        if (input.pos == input.size)
          break;
        throw std::invalid_argument("P29 zstd decoder made no progress");
      }
      again = input.pos < input.size || output.pos == output.size;
    } while (again);
    if (input.pos != input.size)
      throw std::invalid_argument("P29 zstd stream has trailing bytes");
    if (!close && remaining == 0)
      throw std::invalid_argument("P29 zstd stream closed before route close");
    if (close && remaining != 0)
      throw std::invalid_argument("P29 zstd stream did not close");
    active_ = remaining != 0;
    if (close) {
      active_ = false;
      closed_ = true;
    }
    return raw;
  }

  void close_without_frame() {
    if (active_)
      throw std::invalid_argument("P29 active zstd stream has no close tail");
    closed_ = true;
  }

  [[nodiscard]] bool active() const { return active_; }
  [[nodiscard]] bool closed() const { return closed_; }

private:
  static void check(std::size_t result, const char *operation) {
    if (ZSTD_isError(result))
      throw std::runtime_error(std::string("P29 zstd ") + operation + ": " +
                               ZSTD_getErrorName(result));
  }

  std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> context_;
  bool active_ = false;
  bool closed_ = false;
};

struct Marker {
  std::string path;
  std::uint64_t line = 0;
  std::vector<std::uint8_t> flags;
};

[[nodiscard]] inline bool parse_marker(std::span<const std::uint8_t> bytes,
                                       Marker &marker) {
  if (bytes.size() < 4 || bytes[0] != '#' || bytes[1] != ' ')
    return false;
  std::size_t position = 2;
  if (position == bytes.size() || bytes[position] < '0' ||
      bytes[position] > '9')
    return false;
  marker.line = 0;
  while (position < bytes.size() && bytes[position] >= '0' &&
         bytes[position] <= '9') {
    const std::uint8_t digit = bytes[position] - '0';
    if (marker.line > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
      return false;
    marker.line = marker.line * 10 + digit;
    ++position;
  }
  if (position + 2 > bytes.size() || bytes[position] != ' ' ||
      bytes[position + 1] != '"')
    return false;
  position += 2;
  const std::size_t path_begin = position;
  while (position < bytes.size() && bytes[position] != '"')
    ++position;
  if (position == bytes.size())
    return false;
  marker.path.assign(reinterpret_cast<const char *>(bytes.data() + path_begin),
                     position - path_begin);
  ++position;
  marker.flags.clear();
  while (position < bytes.size() && bytes[position] == ' ') {
    ++position;
    if (position == bytes.size() || bytes[position] < '0' ||
        bytes[position] > '9')
      return false;
    unsigned flag = 0;
    while (position < bytes.size() && bytes[position] >= '0' &&
           bytes[position] <= '9') {
      flag = flag * 10 + (bytes[position] - '0');
      if (flag > std::numeric_limits<std::uint8_t>::max())
        return false;
      ++position;
    }
    marker.flags.push_back(static_cast<std::uint8_t>(flag));
  }
  return position + 1 == bytes.size() && bytes[position] == '\n';
}

inline void append_decimal(std::vector<std::uint8_t> &output,
                           std::uint64_t value) {
  char buffer[32];
  char *end = buffer + sizeof(buffer);
  char *position = end;
  do {
    *--position = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value);
  output.insert(output.end(), position, end);
}

inline void emit_marker(std::string_view path, std::uint64_t line,
                        std::span<const std::uint8_t> flags,
                        std::vector<std::uint8_t> &output) {
  output.push_back('#');
  output.push_back(' ');
  append_decimal(output, line);
  output.push_back(' ');
  output.push_back('"');
  output.insert(output.end(), path.begin(), path.end());
  output.push_back('"');
  for (std::uint8_t flag : flags) {
    output.push_back(' ');
    append_decimal(output, flag);
  }
  output.push_back('\n');
}

[[nodiscard]] inline bool system_source_path(std::string_view path) {
  return path.starts_with("/usr/include/") ||
         path.starts_with("/usr/lib/gcc/") ||
         path.starts_with("/usr/local/include/");
}

[[nodiscard]] inline bool valid_source_view(const P29SourceTextView &source) {
  if (!source.available)
    return false;
  if (source.offsets.empty() || source.offsets.front() != 0 ||
      source.offsets.back() != source.bytes.size())
    return false;
  for (std::size_t index = 1; index < source.offsets.size(); ++index)
    if (source.offsets[index] < source.offsets[index - 1])
      return false;
  return true;
}

[[nodiscard]] inline std::span<const std::uint8_t>
source_line(const P29SourceTextView &source, std::uint32_t line) {
  if (!valid_source_view(source) ||
      std::size_t(line) + 1 >= source.offsets.size())
    return {};
  const std::size_t begin = source.offsets[line];
  const std::size_t end = source.offsets[line + 1];
  return source.bytes.subspan(begin, end - begin);
}

[[nodiscard]] inline std::size_t varint_size(std::uint64_t value) {
  std::size_t size = 1;
  while (value >= 0x80) {
    value >>= 7;
    ++size;
  }
  return size;
}

[[nodiscard]] inline std::uint32_t
common_prefix(std::span<const std::uint8_t> left,
              std::span<const std::uint8_t> right) {
  const std::size_t limit = std::min(left.size(), right.size());
  std::size_t size = 0;
  while (size < limit && left[size] == right[size])
    ++size;
  return static_cast<std::uint32_t>(size);
}

[[nodiscard]] inline std::uint32_t
common_suffix(std::span<const std::uint8_t> left,
              std::span<const std::uint8_t> right, std::uint32_t prefix) {
  const std::size_t left_available =
      left.size() - std::min<std::size_t>(left.size(), prefix);
  const std::size_t right_available =
      right.size() - std::min<std::size_t>(right.size(), prefix);
  const std::size_t limit = std::min(left_available, right_available);
  std::size_t size = 0;
  while (size < limit &&
         left[left.size() - 1 - size] == right[right.size() - 1 - size])
    ++size;
  return static_cast<std::uint32_t>(size);
}

template <class P> [[noreturn]] void provider_fail(const char *reason) {
  P::fail(reason);
  throw std::runtime_error(reason);
}

template <class T, class Allocator>
inline void reserve_geometric(std::vector<T, Allocator> &values,
                              std::size_t required) {
  if (required <= values.capacity())
    return;
  const std::size_t capacity = values.capacity();
  const std::size_t doubled =
      capacity > std::numeric_limits<std::size_t>::max() / 2
          ? std::numeric_limits<std::size_t>::max()
          : std::max<std::size_t>(1, capacity * 2);
  values.reserve(std::max(required, doubled));
}

} // namespace p29_wire_detail

template <P29SenderProvider Provider, P29WireDictionary Dictionary>
class P29Serializer {
public:
  P29Serializer(Provider &provider, const Dictionary &dictionary)
      : provider_(provider), dictionary_(dictionary), matcher_({3, 1024, 22}) {}

  P29Serializer(Provider &provider, const Dictionary &dictionary,
                p29::OnlineS1::Config config,
                p29::BlockCatalogue &catalogue)
      : provider_(provider), dictionary_(dictionary),
        matcher_(config, catalogue) {}

  [[nodiscard]] bool has_pending() const { return pending_.active; }

  [[nodiscard]] const std::vector<std::uint8_t> &captured_body() const {
    require_pending();
    return pending_.body;
  }

  [[nodiscard]] const std::vector<std::uint8_t> &captured_fill() const {
    require_pending();
    return pending_.fill;
  }

  [[nodiscard]] std::size_t root_reference_count() const {
    require_pending();
    return pending_.plan->root.size();
  }

  // Project the logical committed route state while the TU is still
  // tentative. Callers enforce their hard route budget before exposing FILL,
  // so a peer can never commit a successor that C later rejects for size.
  [[nodiscard]] std::uint64_t pending_route_state_bytes() const {
    require_pending();
    const P29SenderRouteState &state = provider_.sender_route();
    std::uint64_t result = p29_sender_route_state_bytes(state);
    const auto add = [&result](std::uint64_t bytes) {
      result = bytes > std::numeric_limits<std::uint64_t>::max() - result
                   ? std::numeric_limits<std::uint64_t>::max()
                   : result + bytes;
    };
    const auto multiply = [](std::uint64_t count,
                             std::uint64_t width) noexcept {
      return count != 0 &&
                     width > std::numeric_limits<std::uint64_t>::max() / count
                 ? std::numeric_limits<std::uint64_t>::max()
                 : count * width;
    };
    for (const std::string &path : pending_.new_paths) {
      add(sizeof(std::string) * 2 + sizeof(std::uint32_t));
      add(multiply(path.size(), 2));
    }
    if (pending_.final_mixed_lines > state.mixed_lines.size())
      add(multiply(pending_.final_mixed_lines - state.mixed_lines.size(),
                   sizeof(P29MixedCLineState)));
    if (pending_.final_known_regions > state.fknown_regions.size())
      add(pending_.final_known_regions - state.fknown_regions.size());
    if (pending_.final_known_blocks > state.fknown_blocks.size())
      add(pending_.final_known_blocks - state.fknown_blocks.size());
    return result;
  }

  [[nodiscard]] std::vector<std::uint8_t>
  begin_tu(std::span<const std::uint32_t> regions) {
#if defined(ICECC_P50SIM_ALLOCATION_COUNTER)
    p29_wire_detail::P29AllocationScope allocation_scope(
        p29_wire_detail::P29AllocationEntry::SenderBeginTu);
#endif
    if (pending_.active)
      fail("P29 serializer already has a pending TU");
    if (control_encoder_.closed() || literal_encoder_.closed())
      fail("P29 serializer entropy stream is closed");
    clear_pending();
    pending_.active = true;
    snapshot_base();
    try {
      const P29WireLimits limits = provider_.wire_limits();
      if (regions.size() > limits.max_occurrences)
        fail("P29 input Root exceeds provider occurrence limit");
      std::size_t raw_bytes = 0;
      for (std::uint32_t id : regions) {
        if (id >= limits.max_regions)
          fail("P29 input Region exceeds provider limit");
        const std::size_t size = dictionary_.region_bytes(id).size();
        if (size > limits.max_region_bytes || raw_bytes > limits.max_tu_bytes ||
            size > limits.max_tu_bytes - raw_bytes)
          fail("P29 input TU bytes exceed provider limit");
        raw_bytes += size;
      }
      const std::vector<std::uint32_t> current(regions.begin(), regions.end());
      pending_.plan = &matcher_.prepare(current);
      build_body();
      return pending_.body;
    } catch (...) {
      if (matcher_.has_pending())
        matcher_.abort();
      clear_pending();
      throw;
    }
  }

  [[nodiscard]] const std::vector<std::uint8_t> &
  answer_need(std::span<const std::uint8_t> need_frames,
              bool close_entropy = false) {
#if defined(ICECC_P50SIM_ALLOCATION_COUNTER)
    p29_wire_detail::P29AllocationScope allocation_scope(
        p29_wire_detail::P29AllocationEntry::SenderAnswerNeed);
#endif
    require_pending();
    if (pending_.fill_ready)
      fail("P29 serializer answered NEED twice");
    try {
      validate_need(need_frames);
      build_fill(close_entropy);
      pending_.fill_ready = true;
      pending_.close_entropy = close_entropy;
      return pending_.fill;
    } catch (...) {
      // The caller may still abandon; logical provider state has not moved.
      throw;
    }
  }

  void commit() {
#if defined(ICECC_P50SIM_ALLOCATION_COUNTER)
    p29_wire_detail::P29AllocationScope allocation_scope(
        p29_wire_detail::P29AllocationEntry::SenderCommit);
#endif
    require_pending();
    if (!pending_.fill_ready)
      fail("P29 serializer commit precedes FILL");
    P29SenderRouteState &state = provider_.sender_route();
    if (state.revision != pending_.base_revision ||
        state.paths.size() != pending_.base_paths ||
        state.path_ids.size() != pending_.base_paths ||
        state.mixed_lines.size() != pending_.base_mixed_lines ||
        state.next_public != pending_.base_next_public ||
        state.fknown_regions.size() != pending_.base_known_regions ||
        state.fknown_blocks.size() != pending_.base_known_blocks ||
        state.known_region_count != pending_.base_known_region_count ||
        state.known_block_count != pending_.base_known_block_count)
      fail("P29 serializer redo base differs");

    p29_wire_detail::reserve_geometric(
        state.paths, state.paths.size() + pending_.new_paths.size());
    state.path_ids.reserve(state.path_ids.size() + pending_.new_paths.size());
    for (const std::string &path : pending_.new_paths) {
      const std::uint32_t id = static_cast<std::uint32_t>(state.paths.size());
      if (!state.path_ids.emplace(path, id).second)
        fail("P29 serializer path redo conflicts");
      state.paths.push_back(path);
    }
    if (pending_.final_mixed_lines > state.mixed_lines.size())
      state.mixed_lines.resize(pending_.final_mixed_lines);
    for (const auto &change : pending_.line_changes)
      state.mixed_lines[change.first] = change.second;
    state.next_public = pending_.next_public;
    if (pending_.final_known_regions > state.fknown_regions.size())
      state.fknown_regions.resize(pending_.final_known_regions, 0);
    if (pending_.final_known_blocks > state.fknown_blocks.size())
      state.fknown_blocks.resize(pending_.final_known_blocks, 0);
    for (std::uint32_t id : pending_.known_regions)
      state.fknown_regions[id] = 1;
    for (std::uint32_t id : pending_.known_blocks)
      state.fknown_blocks[id] = 1;
    state.known_region_count += pending_.known_regions.size();
    state.known_block_count += pending_.known_blocks.size();
    ++state.revision;
    matcher_.commit();
    clear_pending();
  }

  void abandon() {
#if defined(ICECC_P50SIM_ALLOCATION_COUNTER)
    p29_wire_detail::P29AllocationScope allocation_scope(
        p29_wire_detail::P29AllocationEntry::SenderAbandon);
#endif
    require_pending();
    if (matcher_.has_pending())
      matcher_.abort();
    control_encoder_.reset();
    literal_encoder_.reset();
    clear_pending();
  }

  [[nodiscard]] const p29::BlockCatalogue &catalogue() const {
    return matcher_.catalogue();
  }

private:
  struct Pending {
    bool active = false;
    bool fill_ready = false;
    bool close_entropy = false;
    std::uint64_t base_revision = 0;
    std::size_t base_paths = 0;
    std::size_t base_mixed_lines = 0;
    std::size_t base_known_regions = 0;
    std::size_t base_known_blocks = 0;
    std::size_t base_known_region_count = 0;
    std::size_t base_known_block_count = 0;
    std::uint32_t base_next_public = 1;
    std::size_t final_mixed_lines = 0;
    std::size_t final_known_regions = 0;
    std::size_t final_known_blocks = 0;
    const p29::TuPlan *plan = nullptr;
    std::vector<std::uint32_t> required_regions;
    std::vector<std::uint32_t> required_blocks;
    std::vector<std::uint32_t> manifest_blocks;
    std::vector<std::uint32_t> missing_regions;
    std::vector<std::uint32_t> known_regions;
    std::vector<std::uint32_t> known_blocks;
    std::vector<std::string> new_paths;
    std::unordered_map<std::string, std::uint32_t> new_path_ids;
    std::vector<std::pair<std::uint32_t, P29MixedCLineState>> line_changes;
    std::unordered_map<std::uint32_t, std::size_t> line_change_index;
    std::uint32_t next_public = 1;
    std::vector<std::uint8_t> root_raw;
    std::vector<std::uint8_t> control_raw;
    std::vector<std::uint8_t> literal_raw;
    std::vector<std::uint8_t> path_raw;
    std::vector<std::uint8_t> body;
    std::vector<std::uint8_t> fill;
  };

  [[noreturn]] static void fail(const char *reason) {
    p29_wire_detail::provider_fail<Provider>(reason);
  }

  void require_pending() const {
    if (!pending_.active)
      fail("P29 serializer has no pending TU");
  }

  void clear_pending() noexcept {
#if defined(ICECC_P29V1_MUTANT_NO_RETENTION)
    pending_ = {};
    return;
#endif
    pending_.active = false;
    pending_.fill_ready = false;
    pending_.close_entropy = false;
    pending_.base_revision = 0;
    pending_.base_paths = 0;
    pending_.base_mixed_lines = 0;
    pending_.base_known_regions = 0;
    pending_.base_known_blocks = 0;
    pending_.base_known_region_count = 0;
    pending_.base_known_block_count = 0;
    pending_.base_next_public = 1;
    pending_.final_mixed_lines = 0;
    pending_.final_known_regions = 0;
    pending_.final_known_blocks = 0;
    pending_.plan = nullptr;
    pending_.required_regions.clear();
    pending_.required_blocks.clear();
    pending_.manifest_blocks.clear();
    pending_.missing_regions.clear();
    pending_.known_regions.clear();
    pending_.known_blocks.clear();
    pending_.new_paths.clear();
    pending_.new_path_ids.clear();
    pending_.line_changes.clear();
    pending_.line_change_index.clear();
    pending_.next_public = 1;
    pending_.root_raw.clear();
    pending_.control_raw.clear();
    pending_.literal_raw.clear();
    pending_.path_raw.clear();
    pending_.body.clear();
    pending_.fill.clear();
  }

  void snapshot_base() {
    const P29SenderRouteState &state = provider_.sender_route();
    pending_.base_revision = state.revision;
    pending_.base_paths = state.paths.size();
    pending_.base_mixed_lines = state.mixed_lines.size();
    pending_.base_next_public = state.next_public;
    pending_.base_known_regions = state.fknown_regions.size();
    pending_.base_known_blocks = state.fknown_blocks.size();
    pending_.base_known_region_count = state.known_region_count;
    pending_.base_known_block_count = state.known_block_count;
    pending_.final_mixed_lines = state.mixed_lines.size();
    pending_.final_known_regions = state.fknown_regions.size();
    pending_.final_known_blocks = state.fknown_blocks.size();
    pending_.next_public = state.next_public;
  }

  [[nodiscard]] bool known_region(std::uint32_t id) const {
    const auto &known = provider_.sender_route().fknown_regions;
    return id < known.size() && known[id];
  }

  [[nodiscard]] bool known_block(std::uint32_t id) const {
    const auto &known = provider_.sender_route().fknown_blocks;
    return id < known.size() && known[id];
  }

  void build_body() {
    using namespace p29_wire_detail;
    begin_requirement_set();
    const P29WireLimits limits = provider_.wire_limits();
    if (pending_.plan->root.size() > limits.max_occurrences)
      fail("P29 Root exceeds provider occurrence limit");
    for (const p29::Ref &reference : pending_.plan->root) {
      if (reference.kind == p29::RefKind::Region) {
        if (reference.id >= limits.max_regions)
          fail("P29 Root Region exceeds provider limit");
        put_varint(pending_.root_raw, make_tag(reference.id, false));
        require_region(reference.id);
      } else {
        if (reference.id >= limits.max_blocks)
          fail("P29 Root Block exceeds provider limit");
        put_varint(pending_.root_raw, make_tag(reference.id, true));
        require_block(reference.id);
        const auto &block = matcher_.catalogue().block(reference.id);
        for (std::uint32_t child : block.regions) {
          if (child >= limits.max_regions)
            fail("P29 Block child exceeds provider limit");
          require_region(child);
        }
      }
    }
    for (std::uint32_t block : pending_.required_blocks)
      if (!known_block(block))
        pending_.manifest_blocks.push_back(block);
    for (std::uint32_t region : pending_.required_regions)
      if (!known_region(region))
        pending_.missing_regions.push_back(region);

    const std::vector<std::uint8_t> root_encoded =
        messages_.encode(pending_.root_raw);
    append_frame(pending_.body, P29WireKind::Root, root_encoded);
    if (!pending_.manifest_blocks.empty()) {
      std::vector<std::uint8_t> raw;
      put_varint(raw, pending_.manifest_blocks.size());
      for (std::uint32_t id : pending_.manifest_blocks) {
        put_varint(raw, id);
        const auto &block = matcher_.catalogue().block(id);
        bool copy = false;
        std::uint32_t source = 0;
        for (const p29::BlockUse &use : pending_.plan->block_uses) {
          if (use.block_id != id || !use.source_precedes_current_tu)
            continue;
          if (use.length != block.regions.size())
            fail("P29 BlockUse length differs");
          source = use.source_position;
          copy = true;
          break;
        }
        raw.push_back(copy ? 1 : 0);
        if (copy) {
          put_varint(raw, source);
          put_varint(raw, block.regions.size());
        } else {
          put_varint(raw, block.regions.size());
          for (std::uint32_t child : block.regions)
            put_varint(raw, child);
        }
      }
      const std::vector<std::uint8_t> encoded = messages_.encode(raw);
      append_frame(pending_.body, P29WireKind::BlockDefinition, encoded);
    }
    pending_.final_known_blocks =
        std::max(pending_.final_known_blocks,
                 static_cast<std::size_t>(matcher_.catalogue().size()));
    pending_.known_blocks = pending_.manifest_blocks;
  }

  void begin_requirement_set() {
    if (++requirement_stamp_ == 0) {
      std::fill(region_stamps_.begin(), region_stamps_.end(), 0);
      std::fill(block_stamps_.begin(), block_stamps_.end(), 0);
      requirement_stamp_ = 1;
    }
  }

  void require_region(std::uint32_t id) {
    if (id >= region_stamps_.size())
      region_stamps_.resize(std::size_t(id) + 1);
    if (region_stamps_[id] != requirement_stamp_) {
      region_stamps_[id] = requirement_stamp_;
      pending_.required_regions.push_back(id);
    }
  }

  void require_block(std::uint32_t id) {
    if (id >= block_stamps_.size())
      block_stamps_.resize(std::size_t(id) + 1);
    if (block_stamps_[id] != requirement_stamp_) {
      block_stamps_[id] = requirement_stamp_;
      pending_.required_blocks.push_back(id);
    }
  }

  void validate_need(std::span<const std::uint8_t> bytes) {
    using namespace p29_wire_detail;
    const bool expected =
        !pending_.manifest_blocks.empty() || !pending_.missing_regions.empty();
    if (!expected) {
      if (!bytes.empty())
        fail("P29 received an unrequested NEED");
      return;
    }
    std::vector<FrameView> frames = parse_frames(bytes, 1);
    if (frames.size() != 1 || frames[0].kind != P29WireKind::Need)
      fail("P29 NEED frame set differs");
    const std::vector<std::uint8_t> raw = messages_.decode(
        frames[0].payload, provider_.wire_limits().max_tu_bytes);
    Cursor cursor(raw);
    const std::uint64_t count = cursor.varint();
    if (count != pending_.missing_regions.size())
      fail("P29 NEED Region count differs");
    for (std::uint32_t expected_id : pending_.missing_regions)
      if (cursor.varint() != expected_id)
        fail("P29 NEED Region identity differs");
    if (cursor.varint() != 0 || !cursor.empty())
      fail("P29 NEED terminator differs");
  }

  [[nodiscard]] P29MixedCLineState &line_state(std::uint32_t id) {
    auto found = pending_.line_change_index.find(id);
    if (found != pending_.line_change_index.end())
      return pending_.line_changes[found->second].second;
    P29MixedCLineState value;
    const auto &base = provider_.sender_route().mixed_lines;
    if (id < base.size())
      value = base[id];
    const std::size_t index = pending_.line_changes.size();
    pending_.line_changes.emplace_back(id, value);
    pending_.line_change_index.emplace(id, index);
    pending_.final_mixed_lines =
        std::max(pending_.final_mixed_lines, std::size_t(id) + 1);
    return pending_.line_changes.back().second;
  }

  [[nodiscard]] std::uint32_t ensure_path(const std::string &path) {
    const auto &state = provider_.sender_route();
    auto found = state.path_ids.find(path);
    if (found != state.path_ids.end())
      return found->second;
    auto pending_found = pending_.new_path_ids.find(path);
    if (pending_found != pending_.new_path_ids.end())
      return pending_found->second;
    if (state.paths.size() + pending_.new_paths.size() >=
        provider_.wire_limits().max_paths)
      fail("P29 Path space exceeds provider limit");
    const std::uint32_t id = static_cast<std::uint32_t>(
        state.paths.size() + pending_.new_paths.size());
    pending_.new_paths.push_back(path);
    pending_.new_path_ids.emplace(path, id);
    return id;
  }

  void flush_literal(std::uint32_t &length) {
    if (!length)
      return;
    pending_.control_raw.push_back(0);
    p29_wire_detail::put_varint(pending_.control_raw, length);
    length = 0;
  }

  void encode_missing_regions() {
    using namespace p29_wire_detail;
    if (pending_.missing_regions.empty())
      return;
    put_varint(pending_.control_raw, pending_.missing_regions.size());
    for (std::uint32_t region : pending_.missing_regions) {
      const std::span<const std::uint32_t> lines =
          dictionary_.region_lines(region);
      const std::span<const std::uint8_t> region_bytes =
          dictionary_.region_bytes(region);
      if (region_bytes.size() > std::numeric_limits<std::uint32_t>::max())
        fail("P29 Region exceeds u32 length");
      put_varint(pending_.control_raw, region_bytes.size());
      Marker region_marker;
      bool region_marker_ok = false;
      if (!lines.empty())
        region_marker_ok =
            parse_marker(dictionary_.line(lines.front()), region_marker);
      P29SourceTextView region_source;
      if (provider_.sender_route().system_source_reuse && region_marker_ok &&
          system_source_path(region_marker.path))
        region_source = provider_.source_text(region_marker.path);

      std::uint32_t offset = 0;
      std::uint32_t literal_length = 0;
      for (std::size_t index = 0; index < lines.size(); ++index) {
        const std::uint32_t line_id = lines[index];
        const std::span<const std::uint8_t> line = dictionary_.line(line_id);
        if (line.size() > std::numeric_limits<std::uint32_t>::max() ||
            offset > std::numeric_limits<std::uint32_t>::max() - line.size())
          fail("P29 Line offset exceeds u32");
        P29MixedCLineState &state = line_state(line_id);
        if (state.public_id) {
          flush_literal(literal_length);
          pending_.control_raw.push_back(2);
          put_varint(pending_.control_raw, state.public_id);
        } else if (state.source_region !=
                       std::numeric_limits<std::uint32_t>::max() &&
                   state.source_region != region) {
          const std::span<const std::uint8_t> source =
              dictionary_.region_bytes(state.source_region);
          if (state.source_offset > source.size() ||
              line.size() > source.size() - state.source_offset ||
              !std::equal(line.begin(), line.end(),
                          source.begin() + state.source_offset))
            fail("P29 mixed source Line differs");
          flush_literal(literal_length);
          pending_.control_raw.push_back(1);
          put_zigzag(pending_.control_raw,
                     std::int64_t(state.source_region) - std::int64_t(region));
          put_varint(pending_.control_raw, state.source_offset);
          put_varint(pending_.control_raw, line.size());
          if (pending_.next_public == 0 ||
              pending_.next_public >= provider_.wire_limits().max_public_lines)
            fail("P29 public Line space exceeds provider limit");
          state.public_id = pending_.next_public++;
        } else {
          if (state.source_region ==
              std::numeric_limits<std::uint32_t>::max()) {
            state.source_region = region;
            state.source_offset = offset;
          }
          Marker line_marker;
          const bool marker_line = parse_marker(line, line_marker);
          bool source_copy = false;
          bool source_patch = false;
          std::uint32_t source_line_id = 0;
          std::uint32_t prefix = 0;
          std::uint32_t suffix = 0;
          std::uint32_t middle = 0;
          if (region_marker_ok && index > 0 &&
              valid_source_view(region_source)) {
            const std::uint64_t candidate =
                index - 1 > std::numeric_limits<std::uint64_t>::max() -
                                region_marker.line
                    ? 0
                    : region_marker.line + index - 1;
            if (candidate &&
                candidate - 1 <= std::numeric_limits<std::uint32_t>::max()) {
              source_line_id = static_cast<std::uint32_t>(candidate - 1);
              const std::span<const std::uint8_t> source =
                  source_line(region_source, source_line_id);
              if (!source.empty() ||
                  (std::size_t(source_line_id) + 1 <
                       region_source.offsets.size() &&
                   region_source.offsets[source_line_id] ==
                       region_source.offsets[source_line_id + 1])) {
                if (source.size() == line.size() &&
                    std::equal(source.begin(), source.end(), line.begin())) {
                  source_copy = true;
                } else {
                  prefix = common_prefix(line, source);
                  suffix = common_suffix(line, source, prefix);
                  middle =
                      static_cast<std::uint32_t>(line.size() - prefix - suffix);
                  const std::size_t cost =
                      1 +
                      varint_size(provider_.sender_route().paths.size() +
                                  pending_.new_paths.size()) +
                      varint_size(source_line_id) + varint_size(prefix) +
                      varint_size(suffix) + varint_size(middle) + middle;
                  source_patch = cost < line.size();
                }
              }
            }
          }
          if (source_copy) {
            const std::uint32_t path = ensure_path(region_marker.path);
            flush_literal(literal_length);
            pending_.control_raw.push_back(5);
            put_varint(pending_.control_raw, path);
            put_varint(pending_.control_raw, source_line_id);
          } else if (marker_line) {
            const std::uint32_t path = ensure_path(line_marker.path);
            flush_literal(literal_length);
            pending_.control_raw.push_back(4);
            put_varint(pending_.control_raw, path);
            put_varint(pending_.control_raw, line_marker.line);
            put_varint(pending_.control_raw, line_marker.flags.size());
            pending_.control_raw.insert(pending_.control_raw.end(),
                                        line_marker.flags.begin(),
                                        line_marker.flags.end());
          } else if (source_patch) {
            const std::uint32_t path = ensure_path(region_marker.path);
            flush_literal(literal_length);
            pending_.control_raw.push_back(6);
            put_varint(pending_.control_raw, path);
            put_varint(pending_.control_raw, source_line_id);
            put_varint(pending_.control_raw, prefix);
            put_varint(pending_.control_raw, suffix);
            put_varint(pending_.control_raw, middle);
            if (middle)
              pending_.literal_raw.insert(pending_.literal_raw.end(),
                                          line.begin() + prefix,
                                          line.begin() + prefix + middle);
          } else {
            if (!line.empty())
              pending_.literal_raw.insert(pending_.literal_raw.end(),
                                          line.begin(), line.end());
            if (line.size() >
                std::numeric_limits<std::uint32_t>::max() - literal_length)
              fail("P29 literal run exceeds u32");
            literal_length += static_cast<std::uint32_t>(line.size());
          }
        }
        offset += static_cast<std::uint32_t>(line.size());
      }
      flush_literal(literal_length);
      if (offset != region_bytes.size())
        fail("P29 Region Line lengths differ");
      pending_.known_regions.push_back(region);
    }
    pending_.final_known_regions = std::max(
        pending_.final_known_regions,
        pending_.required_regions.empty()
            ? std::size_t{0}
            : std::size_t(*std::max_element(pending_.required_regions.begin(),
                                            pending_.required_regions.end())) +
                  1);
  }

  void build_fill(bool close_entropy) {
    using namespace p29_wire_detail;
    encode_missing_regions();
    for (const std::string &path : pending_.new_paths) {
      put_varint(pending_.path_raw, path.size());
      pending_.path_raw.insert(pending_.path_raw.end(), path.begin(),
                               path.end());
    }
    const std::vector<std::uint8_t> control_encoded =
        control_encoder_.encode(pending_.control_raw, close_entropy);
    const std::vector<std::uint8_t> literal_encoded =
        literal_encoder_.encode(pending_.literal_raw, close_entropy);
    std::uint8_t mask = 0;
    if (!control_encoded.empty()) {
      append_frame(pending_.fill, P29WireKind::FillControl, control_encoded);
      mask |= 1;
    }
    if (!literal_encoded.empty()) {
      append_frame(pending_.fill, P29WireKind::FillLiteral, literal_encoded);
      mask |= 2;
    }
    if (!pending_.path_raw.empty()) {
      const std::vector<std::uint8_t> encoded =
          messages_.encode(pending_.path_raw);
      append_frame(pending_.fill, P29WireKind::PathDefinition, encoded);
    }
    append_frame(pending_.fill, P29WireKind::TuEnd,
                 std::span<const std::uint8_t>(&mask, 1));
  }

  Provider &provider_;
  const Dictionary &dictionary_;
  p29::OnlineS1 matcher_;
  p29_wire_detail::MessageCodec messages_;
  p29_wire_detail::ContinuingEncoder control_encoder_;
  p29_wire_detail::ContinuingEncoder literal_encoder_;
  std::vector<std::uint32_t> region_stamps_;
  std::vector<std::uint32_t> block_stamps_;
  std::uint32_t requirement_stamp_ = 0;
  Pending pending_;
};

template <P29ReceiverProvider Provider> class P29Deserializer {
public:
  explicit P29Deserializer(Provider &provider) : provider_(provider) {}

  [[nodiscard]] bool has_pending() const { return pending_.active; }

  [[nodiscard]] const std::vector<std::uint8_t> &captured_need() const {
    require_pending();
    return pending_.need;
  }

  [[nodiscard]] const std::vector<std::uint8_t> &captured_close() const {
    require_pending();
    return pending_.close;
  }

  [[nodiscard]] std::size_t root_reference_count() const {
    require_pending();
    return pending_.root.size();
  }

  [[nodiscard]] std::vector<std::uint8_t>
  receive_body(std::span<const std::uint8_t> body) {
#if defined(ICECC_P50SIM_ALLOCATION_COUNTER)
    p29_wire_detail::P29AllocationScope allocation_scope(
        p29_wire_detail::P29AllocationEntry::ReceiverReceiveBody);
#endif
    if (pending_.active)
      fail("P29 deserializer already has a pending TU");
    if (control_decoder_.closed() || literal_decoder_.closed())
      fail("P29 deserializer entropy stream is closed");
    clear_pending();
    pending_.active = true;
    snapshot_base();
    try {
      parse_body(body);
      build_need();
      return pending_.need;
    } catch (...) {
      clear_pending();
      throw;
    }
  }

  [[nodiscard]] std::span<const std::uint8_t>
  receive_fill(std::span<const std::uint8_t> fill, bool close_entropy = false,
               std::size_t expected_materialized_bytes = 0) {
#if defined(ICECC_P50SIM_ALLOCATION_COUNTER)
    p29_wire_detail::P29AllocationScope allocation_scope(
        p29_wire_detail::P29AllocationEntry::ReceiverReceiveFill);
#endif
    require_pending();
    if (pending_.fill_ready)
      fail("P29 deserializer received FILL twice");
    if (expected_materialized_bytes > provider_.wire_limits().max_tu_bytes)
      fail("P29 expected materialization exceeds provider TU limit");
    if (expected_materialized_bytes > pending_.materialized.capacity())
      pending_.materialized.reserve(expected_materialized_bytes);
    parse_fill(fill, close_entropy);
    if (expected_materialized_bytes != 0 &&
        pending_.materialized.size() != expected_materialized_bytes)
      fail("P29 materialized TU differs from its expected length");
    pending_.fill_ready = true;
    pending_.close_entropy = close_entropy;
    p29_wire_detail::append_frame(pending_.close, P29WireKind::TuEnd, {});
    return pending_.materialized;
  }

  [[nodiscard]] std::vector<std::uint8_t> take_materialized() {
#if defined(ICECC_P50SIM_ALLOCATION_COUNTER)
    p29_wire_detail::P29AllocationScope allocation_scope(
        p29_wire_detail::P29AllocationEntry::ReceiverTakeMaterialized);
#endif
    require_pending();
    if (!pending_.fill_ready)
      fail("P29 materialization precedes FILL");
    return std::move(pending_.materialized);
  }

#if defined(ICECC_P29V1_MUTANT_DOUBLE_MATERIALIZE)
  struct MutantMaterialization {
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint32_t> occurrences;
  };

  [[nodiscard]] MutantMaterialization rematerialize_for_mutant() const {
    require_pending();
    if (!pending_.fill_ready)
      fail("P29 mutant rematerialization precedes FILL");
    const P29WireLimits limits = provider_.wire_limits();
    MutantMaterialization result;
    result.bytes.reserve(pending_.materialized.size());
    result.occurrences.reserve(pending_.occurrences.size());
    const auto append_region = [&](std::uint32_t id) {
      const auto bytes = region_bytes(id);
      if (result.bytes.size() > limits.max_tu_bytes ||
          bytes.size() > limits.max_tu_bytes - result.bytes.size() ||
          bytes.size() > std::numeric_limits<std::size_t>::max() -
                             result.bytes.size())
        fail("P29 mutant materialized TU exceeds addressable size");
      result.bytes.insert(result.bytes.end(), bytes.begin(), bytes.end());
      result.occurrences.push_back(id);
    };
    for (const RootReference &reference : pending_.root) {
      if (!reference.block) {
        append_region(reference.id);
      } else {
        for (std::uint32_t child : block_children(reference.id))
          append_region(child);
      }
    }
    return result;
  }

  [[nodiscard]] std::size_t mutant_occurrence_count() const {
    require_pending();
    return pending_.occurrences.size();
  }
#endif

  [[nodiscard]] std::span<const std::uint8_t> pending_segment() const {
    require_pending();
    if (!pending_.fill_ready)
      fail("P29 segment observation precedes FILL");
    return pending_.region_data;
  }

  [[nodiscard]] Digest128 pending_segment_digest() const {
    require_pending();
    if (!pending_.fill_ready)
      fail("P29 segment digest observation precedes FILL");
    return pending_.segment_digest;
  }

  void commit() {
#if defined(ICECC_P50SIM_ALLOCATION_COUNTER)
    p29_wire_detail::P29AllocationScope allocation_scope(
        p29_wire_detail::P29AllocationEntry::ReceiverCommit);
#endif
    require_pending();
    if (!pending_.fill_ready)
      fail("P29 deserializer commit precedes FILL");
    P29ReceiverRouteState &state = provider_.receiver_route();
    if (state.revision != pending_.base_revision ||
        state.paths.size() != pending_.base_paths ||
        state.path_ids.size() != pending_.base_paths ||
        state.region_segments.size() != pending_.base_region_segments ||
        state.regions.size() != pending_.base_regions ||
        state.public_lines.size() != pending_.base_public_lines ||
        state.blocks.size() != pending_.base_blocks ||
        state.block_children.size() != pending_.base_block_children ||
        state.occurrences.size() != pending_.base_occurrences ||
        state.known_region_count != pending_.base_known_region_count ||
        state.known_block_count != pending_.base_known_block_count)
      fail("P29 deserializer redo base differs");

    // Reserve every vector before making the provider publication visible.
    // The subsequent inserts are therefore non-allocating for scalar arenas.
    std::size_t added_block_children = 0;
    for (const StagedBlock &block : pending_.blocks)
      added_block_children += block.children.size();
    p29_wire_detail::reserve_geometric(
        state.paths, state.paths.size() + pending_.new_paths.size());
    state.path_ids.reserve(state.path_ids.size() + pending_.new_paths.size());
    std::unordered_map<std::string, std::uint32_t> staged_path_ids;
    staged_path_ids.reserve(pending_.new_paths.size());
    for (std::size_t index = 0; index < pending_.new_paths.size(); ++index) {
      const std::size_t wide_id = state.paths.size() + index;
      if (wide_id > std::numeric_limits<std::uint32_t>::max() ||
          state.path_ids.contains(pending_.new_paths[index]) ||
          !staged_path_ids
               .emplace(pending_.new_paths[index],
                        static_cast<std::uint32_t>(wide_id))
               .second)
        fail("P29 receiver Path index conflicts");
    }
    p29_wire_detail::reserve_geometric(state.region_segments,
                                       state.region_segments.size() + 1);
    p29_wire_detail::reserve_geometric(state.public_lines,
                                       state.public_lines.size() +
                                           pending_.public_lines.size());
    p29_wire_detail::reserve_geometric(state.block_children,
                                       state.block_children.size() +
                                           added_block_children);
    p29_wire_detail::reserve_geometric(state.occurrences,
                                       state.occurrences.size() +
                                           pending_.occurrences.size());
    std::size_t region_slots = state.regions.size();
    for (const StagedRegion &region : pending_.regions)
      region_slots = std::max(region_slots, std::size_t(region.id) + 1);
    std::size_t block_slots = state.blocks.size();
    for (const StagedBlock &block : pending_.blocks)
      block_slots = std::max(block_slots, std::size_t(block.id) + 1);
    p29_wire_detail::reserve_geometric(state.regions, region_slots);
    p29_wire_detail::reserve_geometric(state.blocks, block_slots);
    if (state.region_segments.size() >=
        std::numeric_limits<std::uint32_t>::max())
      fail("P29 Region segment ordinal exceeds u32");

    bool publishing = false;
    try {
      provider_.begin_wire_publish();
      publishing = true;
      for (const StagedRegion &region : pending_.regions)
        provider_.publish_wire_region(
            region.id, std::span<const std::uint8_t>(pending_.region_data)
                           .subspan(region.offset, region.length));
      for (const StagedBlock &block : pending_.blocks)
        provider_.publish_wire_block(block.id, block.children);
      provider_.commit_wire_publish();
      publishing = false;
    } catch (...) {
      if (publishing)
        provider_.abandon_wire_publish();
      throw;
    }

    state.path_ids.merge(staged_path_ids);
    if (!staged_path_ids.empty())
      fail("P29 receiver Path index redo conflicts");
    for (std::string &path : pending_.new_paths)
      state.paths.push_back(std::move(path));
    if (state.regions.size() < region_slots)
      state.regions.resize(region_slots);
    const std::uint32_t region_segment =
        static_cast<std::uint32_t>(state.region_segments.size());
    state.region_segments.push_back(std::move(pending_.region_data));
    for (const StagedRegion &region : pending_.regions) {
      state.regions[region.id] = {region_segment, region.offset, region.length,
                                  true};
    }
    state.public_lines.insert(state.public_lines.end(),
                              pending_.public_lines.begin(),
                              pending_.public_lines.end());
    if (state.blocks.size() < block_slots)
      state.blocks.resize(block_slots);
    for (const StagedBlock &block : pending_.blocks) {
      const std::size_t offset = state.block_children.size();
      state.block_children.insert(state.block_children.end(),
                                  block.children.begin(), block.children.end());
      state.blocks[block.id] = {
          offset, static_cast<std::uint32_t>(block.children.size()), true};
    }
    state.occurrences.insert(state.occurrences.end(),
                             pending_.occurrences.begin(),
                             pending_.occurrences.end());
    state.known_region_count += pending_.regions.size();
    state.known_block_count += pending_.blocks.size();
    ++state.revision;
    clear_pending();
  }

  void abandon() {
#if defined(ICECC_P50SIM_ALLOCATION_COUNTER)
    p29_wire_detail::P29AllocationScope allocation_scope(
        p29_wire_detail::P29AllocationEntry::ReceiverAbandon);
#endif
    require_pending();
    control_decoder_.reset();
    literal_decoder_.reset();
    clear_pending();
  }

private:
  struct RootReference {
    bool block = false;
    std::uint32_t id = 0;
  };

  struct StagedBlock {
    std::uint32_t id = 0;
    std::vector<std::uint32_t> children;
  };

  struct StagedRegion {
    std::uint32_t id = 0;
    std::size_t offset = 0;
    std::uint32_t length = 0;
  };

  struct Pending {
    bool active = false;
    bool fill_ready = false;
    bool close_entropy = false;
    std::uint64_t base_revision = 0;
    std::size_t base_paths = 0;
    std::size_t base_region_segments = 0;
    std::size_t base_regions = 0;
    std::size_t base_public_lines = 0;
    std::size_t base_blocks = 0;
    std::size_t base_block_children = 0;
    std::size_t base_occurrences = 0;
    std::size_t base_known_region_count = 0;
    std::size_t base_known_block_count = 0;
    std::vector<RootReference> root;
    std::vector<std::uint32_t> required_regions;
    std::vector<std::uint32_t> required_blocks;
    std::vector<std::uint32_t> missing_regions;
    std::vector<StagedBlock> blocks;
    std::unordered_map<std::uint32_t, std::size_t> block_index;
    std::vector<std::string> new_paths;
    std::unordered_map<std::string, std::uint32_t> new_path_ids;
    std::vector<StagedRegion> regions;
    std::vector<std::uint8_t> region_data;
    Digest128 segment_digest{};
    std::unordered_map<std::uint32_t, std::size_t> region_index;
    std::vector<P29MixedFLineView> public_lines;
    std::vector<std::uint32_t> occurrences;
    std::vector<std::uint8_t> need;
    std::vector<std::uint8_t> close;
    std::vector<std::uint8_t> materialized;
  };

  [[noreturn]] static void fail(const char *reason) {
    p29_wire_detail::provider_fail<Provider>(reason);
  }

  void require_pending() const {
    if (!pending_.active)
      fail("P29 deserializer has no pending TU");
  }

  void clear_pending() noexcept {
#if defined(ICECC_P29V1_MUTANT_NO_RETENTION)
    pending_ = {};
    return;
#endif
    pending_.active = false;
    pending_.fill_ready = false;
    pending_.close_entropy = false;
    pending_.base_revision = 0;
    pending_.base_paths = 0;
    pending_.base_region_segments = 0;
    pending_.base_regions = 0;
    pending_.base_public_lines = 0;
    pending_.base_blocks = 0;
    pending_.base_block_children = 0;
    pending_.base_occurrences = 0;
    pending_.base_known_region_count = 0;
    pending_.base_known_block_count = 0;
    pending_.root.clear();
    pending_.required_regions.clear();
    pending_.required_blocks.clear();
    pending_.missing_regions.clear();
    pending_.blocks.clear();
    pending_.block_index.clear();
    pending_.new_paths.clear();
    pending_.new_path_ids.clear();
    pending_.regions.clear();
    pending_.region_data.clear();
    pending_.segment_digest = {};
    pending_.region_index.clear();
    pending_.public_lines.clear();
    pending_.occurrences.clear();
    pending_.need.clear();
    pending_.close.clear();
    pending_.materialized.clear();
  }

  void snapshot_base() {
    const P29ReceiverRouteState &state = provider_.receiver_route();
    if (state.path_ids.size() != state.paths.size())
      fail("P29 receiver Path index differs from its arena");
    pending_.base_revision = state.revision;
    pending_.base_paths = state.paths.size();
    pending_.base_region_segments = state.region_segments.size();
    pending_.base_regions = state.regions.size();
    pending_.base_public_lines = state.public_lines.size();
    pending_.base_blocks = state.blocks.size();
    pending_.base_block_children = state.block_children.size();
    pending_.base_occurrences = state.occurrences.size();
    pending_.base_known_region_count = state.known_region_count;
    pending_.base_known_block_count = state.known_block_count;
    if (state.public_lines.empty())
      fail("P29 receiver public-Line sentinel is absent");
  }

  [[nodiscard]] bool committed_region_known(std::uint32_t id) const {
    const auto &regions = provider_.receiver_route().regions;
    return id < regions.size() && regions[id].known;
  }

  [[nodiscard]] bool committed_block_known(std::uint32_t id) const {
    const auto &blocks = provider_.receiver_route().blocks;
    return id < blocks.size() && blocks[id].known;
  }

  [[nodiscard]] const StagedBlock *staged_block(std::uint32_t id) const {
    const auto found = pending_.block_index.find(id);
    return found == pending_.block_index.end()
               ? nullptr
               : &pending_.blocks[found->second];
  }

  [[nodiscard]] std::span<const std::uint32_t>
  block_children(std::uint32_t id) const {
    if (const StagedBlock *block = staged_block(id))
      return block->children;
    const P29ReceiverRouteState &state = provider_.receiver_route();
    if (id >= state.blocks.size() || !state.blocks[id].known)
      fail("P29 Root names an undefined Block");
    const P29FBlockView &view = state.blocks[id];
    if (view.offset > state.block_children.size() ||
        view.length > state.block_children.size() - view.offset)
      fail("P29 committed Block view is invalid");
    return std::span<const std::uint32_t>(state.block_children)
        .subspan(view.offset, view.length);
  }

  void parse_body(std::span<const std::uint8_t> body) {
    using namespace p29_wire_detail;
    begin_requirement_set();
    const std::vector<FrameView> frames = parse_frames(body, 2);
    if (frames.empty() || frames.size() > 2 ||
        frames[0].kind != P29WireKind::Root ||
        (frames.size() == 2 && frames[1].kind != P29WireKind::BlockDefinition))
      fail("P29 BODY frame order differs");
    const P29WireLimits limits = provider_.wire_limits();
    const std::vector<std::uint8_t> root_raw =
        messages_.decode(frames[0].payload, limits.max_tu_bytes);
    Cursor root(root_raw);
    while (!root.empty()) {
      if (pending_.root.size() >= limits.max_occurrences)
        fail("P29 Root exceeds provider occurrence limit");
      const std::uint64_t wide = root.varint();
      if (wide > std::numeric_limits<std::uint32_t>::max())
        fail("P29 Root tag exceeds u32");
      const std::uint32_t tag = static_cast<std::uint32_t>(wide);
      const bool block = is_block_tag(tag);
      const std::uint32_t id = tag_id(tag);
      if ((!block && id >= limits.max_regions) ||
          (block && id >= limits.max_blocks))
        fail("P29 Root ordinal exceeds provider limit");
      pending_.root.push_back({block, id});
      if (block)
        require_block(id);
      else
        require_region(id);
    }

    std::vector<std::uint32_t> expected_blocks;
    for (std::uint32_t id : pending_.required_blocks)
      if (!committed_block_known(id))
        expected_blocks.push_back(id);
    if (expected_blocks.empty() != (frames.size() == 1))
      fail("P29 BLOCKDEF presence differs from Root closure");
    if (!expected_blocks.empty()) {
      const std::vector<std::uint8_t> raw =
          messages_.decode(frames[1].payload, limits.max_tu_bytes);
      Cursor blocks(raw);
      if (blocks.varint() != expected_blocks.size())
        fail("P29 BLOCKDEF count differs");
      for (std::uint32_t expected : expected_blocks) {
        const std::uint64_t wide_id = blocks.varint();
        if (wide_id != expected)
          fail("P29 BLOCKDEF identity differs");
        if (committed_block_known(expected) || staged_block(expected))
          fail("P29 BLOCKDEF redefines a known Block");
        StagedBlock block;
        block.id = expected;
        const std::uint8_t kind = blocks.byte();
        if (kind == 1) {
          const std::uint64_t source = blocks.varint();
          const std::uint64_t length = blocks.varint();
          const auto &occurrences = provider_.receiver_route().occurrences;
          if (source > occurrences.size() ||
              length > occurrences.size() - source ||
              length > limits.max_block_children)
            fail("P29 BLOCKDEF copy is out of range");
          block.children.assign(occurrences.begin() + source,
                                occurrences.begin() + source + length);
        } else if (kind == 0) {
          const std::uint64_t length = blocks.varint();
          if (length > limits.max_block_children)
            fail("P29 BLOCKDEF child list exceeds provider limit");
          block.children.reserve(static_cast<std::size_t>(length));
          for (std::uint64_t child = 0; child != length; ++child) {
            const std::uint64_t wide_child = blocks.varint();
            if (wide_child >= limits.max_regions)
              fail("P29 BLOCKDEF child exceeds provider limit");
            block.children.push_back(static_cast<std::uint32_t>(wide_child));
          }
        } else {
          fail("P29 BLOCKDEF kind is invalid");
        }
        pending_.block_index.emplace(block.id, pending_.blocks.size());
        pending_.blocks.push_back(std::move(block));
      }
      if (!blocks.empty())
        fail("P29 BLOCKDEF has trailing bytes");
    }

    // Match the real F ordering: direct Root Regions first, then each
    // first-seen Block's children in first-use order.
    for (std::uint32_t block : pending_.required_blocks)
      for (std::uint32_t child : block_children(block))
        require_region(child);
    for (std::uint32_t region : pending_.required_regions)
      if (!committed_region_known(region))
        pending_.missing_regions.push_back(region);
  }

  void begin_requirement_set() {
    if (++requirement_stamp_ == 0) {
      std::fill(region_stamps_.begin(), region_stamps_.end(), 0);
      std::fill(block_stamps_.begin(), block_stamps_.end(), 0);
      requirement_stamp_ = 1;
    }
  }

  void require_region(std::uint32_t id) {
    if (id >= region_stamps_.size())
      region_stamps_.resize(std::size_t(id) + 1);
    if (region_stamps_[id] != requirement_stamp_) {
      region_stamps_[id] = requirement_stamp_;
      pending_.required_regions.push_back(id);
    }
  }

  void require_block(std::uint32_t id) {
    if (id >= block_stamps_.size())
      block_stamps_.resize(std::size_t(id) + 1);
    if (block_stamps_[id] != requirement_stamp_) {
      block_stamps_[id] = requirement_stamp_;
      pending_.required_blocks.push_back(id);
    }
  }

  void build_need() {
    using namespace p29_wire_detail;
    if (pending_.blocks.empty() && pending_.missing_regions.empty())
      return;
    std::vector<std::uint8_t> raw;
    put_varint(raw, pending_.missing_regions.size());
    for (std::uint32_t id : pending_.missing_regions)
      put_varint(raw, id);
    put_varint(raw, 0);
    const std::vector<std::uint8_t> encoded = messages_.encode(raw);
    append_frame(pending_.need, P29WireKind::Need, encoded);
  }

  [[nodiscard]] std::vector<std::string>
  decode_paths(std::span<const std::uint8_t> encoded) {
    using namespace p29_wire_detail;
    std::vector<std::string> paths;
    if (encoded.empty())
      return paths;
    const std::vector<std::uint8_t> raw =
        messages_.decode(encoded, provider_.wire_limits().max_tu_bytes);
    Cursor cursor(raw);
    const P29ReceiverRouteState &state = provider_.receiver_route();
    const auto &committed = state.paths;
    while (!cursor.empty()) {
      const std::uint64_t length = cursor.varint();
      if (length > cursor.remaining())
        fail("P29 PATHDEF is truncated");
      const std::span<const std::uint8_t> bytes =
          cursor.take(static_cast<std::size_t>(length));
      std::string path(bytes.size(), '\0');
      if (!bytes.empty())
        std::memcpy(path.data(), bytes.data(), bytes.size());
      const std::size_t wide_id = committed.size() + paths.size();
      if (state.path_ids.contains(path) ||
          wide_id > std::numeric_limits<std::uint32_t>::max() ||
          !pending_.new_path_ids
               .emplace(path, static_cast<std::uint32_t>(wide_id))
               .second)
        fail("P29 PATHDEF redefines a Path");
      if (committed.size() + paths.size() >= provider_.wire_limits().max_paths)
        fail("P29 Path space exceeds provider limit");
      paths.push_back(std::move(path));
    }
    return paths;
  }

  [[nodiscard]] std::string_view path(std::uint64_t id) const {
    const auto &committed = provider_.receiver_route().paths;
    if (id < committed.size())
      return committed[static_cast<std::size_t>(id)];
    id -= committed.size();
    if (id >= pending_.new_paths.size())
      fail("P29 marker names an undefined Path");
    return pending_.new_paths[static_cast<std::size_t>(id)];
  }

  [[nodiscard]] std::span<const std::uint8_t>
  region_bytes(std::uint32_t id) const {
    const auto staged = pending_.region_index.find(id);
    if (staged != pending_.region_index.end()) {
      const StagedRegion &view = pending_.regions[staged->second];
      return std::span<const std::uint8_t>(pending_.region_data)
          .subspan(view.offset, view.length);
    }
    const P29ReceiverRouteState &state = provider_.receiver_route();
    if (id >= state.regions.size() || !state.regions[id].known)
      fail("P29 reference names an undefined Region");
    const P29MixedFRegionView &view = state.regions[id];
    if (view.segment >= state.region_segments.size())
      fail("P29 committed Region segment is invalid");
    const auto &segment = state.region_segments[view.segment];
    if (view.offset > segment.size() ||
        view.length > segment.size() - view.offset)
      fail("P29 committed Region view is invalid");
    return std::span<const std::uint8_t>(segment).subspan(view.offset,
                                                          view.length);
  }

  void require_region_room(std::size_t region_end, std::uint64_t length) const {
    if (pending_.region_data.size() > region_end ||
        length > region_end - pending_.region_data.size())
      fail("P29 mixed Region overruns its declared length");
  }

  // A source can be an earlier Region staged in the same TU.  Appending a
  // span into region_data with vector::insert would then be self-insertion,
  // whose iterator preconditions do not permit overlap.  Retain offsets
  // across resize and perform the explicitly overlapping-safe copy instead.
  void append_region_slice(std::uint32_t id, std::size_t offset,
                           std::size_t length) {
    const auto staged = pending_.region_index.find(id);
    if (staged != pending_.region_index.end()) {
      const StagedRegion &view = pending_.regions[staged->second];
      if (offset > view.length || length > view.length - offset)
        fail("P29 staged Region view is out of range");
      const std::size_t source = view.offset + offset;
      const std::size_t destination = pending_.region_data.size();
      if (length > std::numeric_limits<std::size_t>::max() - destination)
        fail("P29 staged Region copy exceeds addressable size");
      pending_.region_data.resize(destination + length);
      if (length)
        std::memmove(pending_.region_data.data() + destination,
                     pending_.region_data.data() + source, length);
      return;
    }
    const auto source = region_bytes(id);
    if (offset > source.size() || length > source.size() - offset)
      fail("P29 committed Region view is out of range");
    if (length)
      pending_.region_data.insert(pending_.region_data.end(),
                                  source.begin() + offset,
                                  source.begin() + offset + length);
  }

  [[nodiscard]] P29MixedFLineView public_line(std::uint64_t id) const {
    const auto &committed = provider_.receiver_route().public_lines;
    if (!id)
      fail("P29 public Line zero is invalid");
    if (id < committed.size())
      return committed[static_cast<std::size_t>(id)];
    id -= committed.size();
    if (id >= pending_.public_lines.size())
      fail("P29 public Line id is undefined");
    return pending_.public_lines[static_cast<std::size_t>(id)];
  }

  void decode_regions(std::span<const std::uint8_t> control_raw,
                      std::span<const std::uint8_t> literal_raw) {
    using namespace p29_wire_detail;
    if (control_raw.empty()) {
      if (!pending_.missing_regions.empty() || !literal_raw.empty())
        fail("P29 mixed Region streams are partial");
      return;
    }
    Cursor control(control_raw);
    Cursor literal(literal_raw);
    Digest128Builder segment_digest;
    if (control.varint() != pending_.missing_regions.size())
      fail("P29 mixed Region count differs");
    const P29WireLimits limits = provider_.wire_limits();
    for (std::uint32_t id : pending_.missing_regions) {
      if (committed_region_known(id) || pending_.region_index.contains(id))
        fail("P29 FILL redefines a known Region");
      const std::uint64_t raw_length = control.varint();
      if (raw_length > limits.max_region_bytes ||
          raw_length > std::numeric_limits<std::uint32_t>::max())
        fail("P29 Region bytes exceed provider limit");
      if (pending_.region_data.size() > limits.max_tu_bytes ||
          raw_length > limits.max_tu_bytes - pending_.region_data.size())
        fail("P29 staged Region bytes exceed provider TU limit");
      StagedRegion region;
      region.id = id;
      region.offset = pending_.region_data.size();
      region.length = static_cast<std::uint32_t>(raw_length);
      if (raw_length >
          std::numeric_limits<std::size_t>::max() - pending_.region_data.size())
        fail("P29 staged Region bytes exceed addressable size");
      // Reserve enough for the whole Region before expanding it so views
      // of earlier staged Regions remain stable.  Grow geometrically:
      // reserving the exact cumulative size here would copy the arena
      // once per Region.
      const std::size_t region_end =
          pending_.region_data.size() + static_cast<std::size_t>(raw_length);
      if (region_end > pending_.region_data.capacity()) {
        const std::size_t capacity = pending_.region_data.capacity();
        const std::size_t doubled =
            capacity > std::numeric_limits<std::size_t>::max() / 2
                ? std::numeric_limits<std::size_t>::max()
                : std::max<std::size_t>(1, capacity * 2);
        pending_.region_data.reserve(std::max(region_end, doubled));
      }
      while (pending_.region_data.size() < region_end) {
        const std::uint8_t opcode = control.byte();
        if (opcode == 0) {
          const std::uint64_t length = control.varint();
          if (length > literal.remaining())
            fail("P29 FILL literal run is truncated");
          require_region_room(region_end, length);
          const auto bytes = literal.take(static_cast<std::size_t>(length));
          if (!bytes.empty())
            pending_.region_data.insert(pending_.region_data.end(),
                                        bytes.begin(), bytes.end());
        } else if (opcode == 1) {
          const std::int64_t delta = control.zigzag();
          const std::int64_t source_id = std::int64_t(id) + delta;
          const std::uint64_t offset = control.varint();
          const std::uint64_t length = control.varint();
          if (source_id < 0 || std::uint64_t(source_id) >= limits.max_regions)
            fail("P29 published Line source is invalid");
          const auto source =
              region_bytes(static_cast<std::uint32_t>(source_id));
          if (offset > source.size() || length > source.size() - offset ||
              offset > std::numeric_limits<std::uint32_t>::max() ||
              length > std::numeric_limits<std::uint32_t>::max())
            fail("P29 published Line view is out of range");
          require_region_room(region_end, length);
          append_region_slice(static_cast<std::uint32_t>(source_id),
                              static_cast<std::size_t>(offset),
                              static_cast<std::size_t>(length));
          if (provider_.receiver_route().public_lines.size() +
                  pending_.public_lines.size() >=
              limits.max_public_lines)
            fail("P29 public Line space exceeds provider limit");
          pending_.public_lines.push_back(
              {static_cast<std::uint32_t>(source_id),
               static_cast<std::uint32_t>(offset),
               static_cast<std::uint32_t>(length)});
        } else if (opcode == 2) {
          const P29MixedFLineView view = public_line(control.varint());
          const auto source = region_bytes(view.source_region);
          if (view.source_offset > source.size() ||
              view.length > source.size() - view.source_offset)
            fail("P29 public Line view is out of range");
          require_region_room(region_end, view.length);
          append_region_slice(view.source_region, view.source_offset,
                              view.length);
        } else if (opcode == 4) {
          const std::uint64_t path_id = control.varint();
          const std::uint64_t line = control.varint();
          const std::uint64_t flags = control.varint();
          if (flags > control.remaining())
            fail("P29 marker flags are truncated");
          const auto flag_bytes = control.take(static_cast<std::size_t>(flags));
          std::vector<std::uint8_t> marker;
          emit_marker(path(path_id), line, flag_bytes, marker);
          require_region_room(region_end, marker.size());
          pending_.region_data.insert(pending_.region_data.end(),
                                      marker.begin(), marker.end());
        } else if (opcode == 5) {
          if (!provider_.receiver_route().system_source_reuse)
            fail("P29 system-source reuse was not negotiated");
          const std::string_view source_path = path(control.varint());
          const std::uint64_t line_id = control.varint();
          if (!system_source_path(source_path) ||
              line_id > std::numeric_limits<std::uint32_t>::max())
            fail("P29 system-source Line is invalid");
          const P29SourceTextView source = provider_.source_text(source_path);
          const auto bytes =
              source_line(source, static_cast<std::uint32_t>(line_id));
          if (!valid_source_view(source) ||
              std::size_t(line_id) + 1 >= source.offsets.size() ||
              (bytes.empty() &&
               source.offsets[static_cast<std::size_t>(line_id)] !=
                   source.offsets[static_cast<std::size_t>(line_id) + 1]))
            fail("P29 system-source Line is unavailable");
          require_region_room(region_end, bytes.size());
          if (!bytes.empty())
            pending_.region_data.insert(pending_.region_data.end(),
                                        bytes.begin(), bytes.end());
        } else if (opcode == 6) {
          if (!provider_.receiver_route().system_source_reuse)
            fail("P29 system-source reuse was not negotiated");
          const std::string_view source_path = path(control.varint());
          const std::uint64_t line_id = control.varint();
          const std::uint64_t prefix = control.varint();
          const std::uint64_t suffix = control.varint();
          const std::uint64_t middle = control.varint();
          if (!system_source_path(source_path) ||
              line_id > std::numeric_limits<std::uint32_t>::max() ||
              middle > literal.remaining())
            fail("P29 system-source patch is invalid");
          const P29SourceTextView source = provider_.source_text(source_path);
          const auto bytes =
              source_line(source, static_cast<std::uint32_t>(line_id));
          if (!valid_source_view(source) ||
              std::size_t(line_id) + 1 >= source.offsets.size() ||
              prefix > bytes.size() || suffix > bytes.size() - prefix)
            fail("P29 system-source patch view is unavailable");
          std::size_t available = region_end - pending_.region_data.size();
          const std::size_t prefix_size = static_cast<std::size_t>(prefix);
          const std::size_t middle_size = static_cast<std::size_t>(middle);
          const std::size_t suffix_size = static_cast<std::size_t>(suffix);
          if (prefix_size > available)
            fail("P29 mixed Region overruns its declared length");
          available -= prefix_size;
          if (middle_size > available)
            fail("P29 mixed Region overruns its declared length");
          available -= middle_size;
          if (suffix_size > available)
            fail("P29 mixed Region overruns its declared length");
          if (prefix_size)
            pending_.region_data.insert(pending_.region_data.end(),
                                        bytes.begin(),
                                        bytes.begin() + prefix_size);
          const auto patch = literal.take(middle_size);
          if (!patch.empty())
            pending_.region_data.insert(pending_.region_data.end(),
                                        patch.begin(), patch.end());
          if (suffix_size)
            pending_.region_data.insert(pending_.region_data.end(),
                                        bytes.end() - suffix_size, bytes.end());
        } else {
          fail("P29 mixed Region opcode is outside the v1 tuple");
        }
        if (pending_.region_data.size() > region_end)
          fail("P29 mixed Region overruns its declared length");
      }
      if (region.length != 0)
        segment_digest.append(
            std::span<const std::uint8_t>(pending_.region_data)
                .subspan(region.offset, region.length));
      pending_.region_index.emplace(id, pending_.regions.size());
      pending_.regions.push_back(std::move(region));
    }
    if (!control.empty() || !literal.empty())
      fail("P29 mixed Region streams have trailing bytes");
    pending_.segment_digest = pending_.region_data.empty()
                                  ? Digest128{}
                                  : segment_digest.finish();
  }

  void materialize() {
    const P29WireLimits limits = provider_.wire_limits();
    auto append_region = [&](std::vector<std::uint8_t> &output,
                             bool record_occurrence, std::uint32_t id) {
      const auto bytes = region_bytes(id);
      if (output.size() > limits.max_tu_bytes ||
          bytes.size() > limits.max_tu_bytes - output.size() ||
          bytes.size() > std::numeric_limits<std::size_t>::max() -
                             output.size())
        fail("P29 materialized TU exceeds addressable size");
      if (!bytes.empty())
        output.insert(output.end(), bytes.begin(), bytes.end());
      if (record_occurrence) {
        if (provider_.receiver_route().occurrences.size() +
                pending_.occurrences.size() >=
            limits.max_occurrences)
          fail("P29 occurrence stream exceeds provider limit");
        pending_.occurrences.push_back(id);
      }
    };
    const auto assemble = [&](std::vector<std::uint8_t> &output,
                              bool record_occurrences) {
      for (const RootReference &reference : pending_.root) {
        if (!reference.block) {
          append_region(output, record_occurrences, reference.id);
        } else {
          const auto children = block_children(reference.id);
          for (std::uint32_t child : children)
            append_region(output, record_occurrences, child);
        }
      }
    };
    assemble(pending_.materialized, true);
  }

  void parse_fill(std::span<const std::uint8_t> fill, bool close_entropy) {
    using namespace p29_wire_detail;
    const std::vector<FrameView> frames = parse_frames(fill, 4);
    if (frames.empty() || frames.back().kind != P29WireKind::TuEnd ||
        frames.back().payload.size() != 1)
      fail("P29 FILL has no valid TU_END");
    std::span<const std::uint8_t> control_encoded;
    std::span<const std::uint8_t> literal_encoded;
    std::span<const std::uint8_t> paths_encoded;
    int prior_rank = -1;
    std::uint8_t mask = 0;
    for (std::size_t index = 0; index + 1 < frames.size(); ++index) {
      int rank = -1;
      if (frames[index].kind == P29WireKind::FillControl) {
        rank = 0;
        control_encoded = frames[index].payload;
        mask |= 1;
      } else if (frames[index].kind == P29WireKind::FillLiteral) {
        rank = 1;
        literal_encoded = frames[index].payload;
        mask |= 2;
      } else if (frames[index].kind == P29WireKind::PathDefinition) {
        rank = 2;
        paths_encoded = frames[index].payload;
      } else {
        fail("P29 FILL contains an unrequested frame kind");
      }
      if (rank <= prior_rank)
        fail("P29 FILL frame order differs");
      prior_rank = rank;
    }
    if (frames.back().payload[0] != mask)
      fail("P29 TU_END stream mask differs");

    // PATHDEF is physically after the continuing streams, but its path ids
    // are needed to interpret marker opcodes.  The whole TU is staged, so it
    // is decoded first without changing provider state.
    pending_.new_paths = decode_paths(paths_encoded);

    std::vector<std::uint8_t> control_raw;
    std::vector<std::uint8_t> literal_raw;
    if (!control_encoded.empty())
      control_raw = control_decoder_.decode(
          control_encoded, close_entropy, provider_.wire_limits().max_tu_bytes);
    else if (close_entropy)
      control_decoder_.close_without_frame();
    if (!literal_encoded.empty())
      literal_raw = literal_decoder_.decode(
          literal_encoded, close_entropy, provider_.wire_limits().max_tu_bytes);
    else if (close_entropy)
      literal_decoder_.close_without_frame();
    decode_regions(control_raw, literal_raw);
    materialize();
    if (pending_.region_data.size() > pending_.materialized.size())
      fail("P29 staged Region segment exceeds materialized TU");
  }

  Provider &provider_;
  p29_wire_detail::MessageCodec messages_;
  p29_wire_detail::ContinuingDecoder control_decoder_;
  p29_wire_detail::ContinuingDecoder literal_decoder_;
  std::vector<std::uint32_t> region_stamps_;
  std::vector<std::uint32_t> block_stamps_;
  std::uint32_t requirement_stamp_ = 0;
  Pending pending_;
};

} // namespace icecc::codec
