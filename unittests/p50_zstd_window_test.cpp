#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

#include "cache/p50_zstd.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using namespace icecc::p50;

[[noreturn]] void fail(std::string_view text) {
    std::cerr << "p50_zstd_window_test: " << text << '\n';
    std::exit(1);
}

void require(bool condition, std::string_view text) {
    if (!condition) fail(text);
}

template<class Exception, class Callable>
void require_throws(Callable&& callable, std::string_view text) {
    try {
        callable();
    } catch (const Exception&) {
        return;
    } catch (...) {
        fail("window-limit negative failed through the wrong exception");
    }
    fail(text);
}

std::vector<uint8_t> declared_window_frame(std::span<const uint8_t> input,
                                           int window_log) {
    using Cctx = std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)>;
    Cctx context(ZSTD_createCCtx(), &ZSTD_freeCCtx);
    if (!context) throw std::bad_alloc();

    const size_t set_window = ZSTD_CCtx_setParameter(
        context.get(), ZSTD_c_windowLog, window_log);
    if (ZSTD_isError(set_window))
        throw std::runtime_error(ZSTD_getErrorName(set_window));
    const size_t omit_size = ZSTD_CCtx_setParameter(
        context.get(), ZSTD_c_contentSizeFlag, 0);
    if (ZSTD_isError(omit_size))
        throw std::runtime_error(ZSTD_getErrorName(omit_size));

    // Begin before announcing end-of-input. This keeps the requested window in
    // the frame header even though this discriminator's exact output is small.
    std::vector<uint8_t> output(
        ZSTD_compressBound(input.size()) + ZSTD_CStreamOutSize());
    ZSTD_inBuffer in{input.data(), input.size(), 0};
    ZSTD_outBuffer out{output.data(), output.size(), 0};
    while (in.pos != in.size) {
        const size_t remaining = ZSTD_compressStream2(
            context.get(), &out, &in, ZSTD_e_continue);
        if (ZSTD_isError(remaining))
            throw std::runtime_error(ZSTD_getErrorName(remaining));
    }

    ZSTD_inBuffer end{nullptr, 0, 0};
    size_t remaining = 1;
    while (remaining != 0) {
        remaining = ZSTD_compressStream2(
            context.get(), &out, &end, ZSTD_e_end);
        if (ZSTD_isError(remaining))
            throw std::runtime_error(ZSTD_getErrorName(remaining));
    }
    output.resize(out.pos);
    return output;
}

TxBegin describe(TxBegin begin, std::span<const uint8_t> body) {
    begin.body = describe_component(kZstdTuBodyEncoding, body,
                                    begin.raw_bytes);
    begin.transaction_digest = compute_transaction_digest(begin, body);
    return begin;
}

} // namespace

int main() {
    std::vector<uint8_t> input(128U << 10);
    for (size_t index = 0; index != input.size(); ++index)
        input[index] = static_cast<uint8_t>(index);

    const ZstdTuEnvelope baseline = encode_zstd_tu(
        HistoryNonce{1}, RelSeq{0}, TuSeq{1}, icecc::digest128("pre"), input);
    const std::vector<uint8_t> frame = declared_window_frame(input, 20);
    ZSTD_frameHeader header{};
    const size_t parsed = ZSTD_getFrameHeader(
        &header, frame.data(), frame.size());
    require(parsed == 0 && header.windowSize == (uint64_t{1} << 20),
            "streaming fixture did not declare the intended 1 MiB window");
    const TxBegin begin = describe(baseline.begin, frame);

    // One product codec reuses its DCtx. A permissive decode must not leave a
    // stale parameter that defeats the following stricter local bound.
    ZstdTuCodec codec;
    require(codec.decode(begin, frame,
                         {frame.size() + 1, input.size(), 20}) == input,
            "frame inside max_window_log did not round-trip");
    require_throws<std::invalid_argument>(
        [&] {
            (void)codec.decode(begin, frame,
                               {frame.size() + 1, input.size(), 10});
        },
        "reused decoder accepted a frame exceeding its current max_window_log");

    require_throws<std::invalid_argument>(
        [&] { (void)ZstdTuDialogue(profile_bit(ProfileId::ZSTD_TU),
                                    {1, 1, 9}); },
        "window log below the Zstd minimum was accepted");
    require_throws<std::invalid_argument>(
        [&] { (void)ZstdTuDialogue(profile_bit(ProfileId::ZSTD_TU),
                                    {1, 1, 32}); },
        "window log above the portable maximum was accepted");

    std::vector<uint8_t> cap10_input(1U << 20);
    for (size_t index = 0; index != cap10_input.size(); ++index)
        cap10_input[index] = static_cast<uint8_t>(
            (index * 29 + index / 257 + (index >> 11)) & 0xff);
    const ZstdTuLimits cap10_limits{uint64_t{2} << 20,
                                    cap10_input.size(), 10};
    ZstdTuCodec capped_codec;
    const ZstdTuEnvelope cap10 = capped_codec.encode(
        HistoryNonce{2}, RelSeq{0}, TuSeq{2}, icecc::digest128("cap10 pre"),
        cap10_input, cap10_limits);
    ZSTD_frameHeader cap10_header{};
    const size_t cap10_parsed = ZSTD_getFrameHeader(
        &cap10_header, cap10.body.data(), cap10.body.size());
    require(cap10_parsed == 0 &&
                cap10_header.windowSize <= (uint64_t{1} << 10),
            "encoder produced a frame above its configured window cap");
    require(capped_codec.decode(cap10.begin, cap10.body, cap10_limits) ==
                cap10_input,
            "cap=10 encoder output was rejected by the same bounded codec");

    std::cout << "p50_zstd_window_test: declared_window="
              << header.windowSize << " encoded_bytes=" << frame.size()
              << " cap10_window=" << cap10_header.windowSize
              << " strict reused-context bound is live\n";
    return 0;
}
