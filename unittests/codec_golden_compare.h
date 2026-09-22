#ifndef ICECC_CODEC_GOLDEN_COMPARE_H
#define ICECC_CODEC_GOLDEN_COMPARE_H

#include "cache/codec/p29_wire.h"

#include <array>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace icecc::codec::test_golden {

namespace wire = p29_wire_detail;

struct FramePayload {
  P29WireKind kind{};
  std::vector<std::uint8_t> decoded;

  bool operator==(const FramePayload &) const = default;
};

struct StreamDecoder {
  std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> context{
      ZSTD_createDCtx(), ZSTD_freeDCtx};
  bool ended = false;
  std::size_t remaining = 1;

  StreamDecoder() {
    if (!context)
      throw std::bad_alloc();
  }
};

inline std::vector<std::uint8_t>
decode_stream_frame(StreamDecoder &decoder,
                    std::span<const std::uint8_t> encoded) {
  if (decoder.ended || encoded.empty())
    throw std::invalid_argument("empty or ended continuing zstd stream");
  ZSTD_inBuffer input{encoded.data(), encoded.size(), 0};
  std::array<std::uint8_t, 64 * 1024> buffer{};
  std::vector<std::uint8_t> decoded;
  while (true) {
    ZSTD_outBuffer output{buffer.data(), buffer.size(), 0};
    decoder.remaining =
        ZSTD_decompressStream(decoder.context.get(), &output, &input);
    if (ZSTD_isError(decoder.remaining))
      throw std::invalid_argument("invalid continuing zstd payload");
    decoded.insert(decoded.end(), buffer.begin(), buffer.begin() + output.pos);
    if (decoder.remaining == 0) {
      decoder.ended = true;
      if (input.pos != input.size)
        throw std::invalid_argument("trailing bytes after continuing zstd frame");
      break;
    }
    if (input.pos == input.size && output.pos < output.size)
      break;
  }
  return decoded;
}

inline bool is_message_frame(P29WireKind kind) {
  return kind == P29WireKind::Root || kind == P29WireKind::BlockDefinition ||
         kind == P29WireKind::Need || kind == P29WireKind::PathDefinition;
}

inline std::vector<FramePayload>
canonical_frames(std::span<const std::uint8_t> bytes,
                 std::optional<P29WireKind> only_kind = std::nullopt) {
  const std::vector<wire::FrameView> frames = wire::parse_frames(bytes);
  wire::MessageCodec messages;
  // These streams continue across TU_END; only the final TU closes them.
  std::optional<StreamDecoder> control;
  std::optional<StreamDecoder> literal;
  std::vector<FramePayload> result;
  for (const wire::FrameView &frame : frames) {
    if (only_kind.has_value() && frame.kind != *only_kind)
      continue;
    std::vector<std::uint8_t> decoded;
    if (is_message_frame(frame.kind)) {
      decoded = messages.decode(frame.payload);
    } else if (frame.kind == P29WireKind::FillControl) {
      if (!control.has_value())
        control.emplace();
      decoded = decode_stream_frame(*control, frame.payload);
    } else if (frame.kind == P29WireKind::FillLiteral) {
      if (!literal.has_value())
        literal.emplace();
      decoded = decode_stream_frame(*literal, frame.payload);
    } else {
      decoded.assign(frame.payload.begin(), frame.payload.end());
    }
    result.push_back({frame.kind, std::move(decoded)});
  }
  if (control.has_value() && !control->ended)
    throw std::invalid_argument("truncated FillControl stream");
  if (literal.has_value() && !literal->ended)
    throw std::invalid_argument("truncated FillLiteral stream");
  return result;
}

inline bool equivalent(std::span<const std::uint8_t> left,
                       std::span<const std::uint8_t> right,
                       std::optional<P29WireKind> only_kind = std::nullopt) {
  try {
    return canonical_frames(left, only_kind) == canonical_frames(right, only_kind);
  } catch (...) {
    return false;
  }
}

} // namespace icecc::codec::test_golden

#endif
