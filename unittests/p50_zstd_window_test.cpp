#include "cache/p50_zstd.h"

#include <zstd.h>

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

std::vector<uint8_t> large_window_frame(std::span<const uint8_t> input,
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

    std::vector<uint8_t> output(ZSTD_compressBound(input.size()));
    const size_t compressed = ZSTD_compress2(
        context.get(), output.data(), output.size(), input.data(), input.size());
    if (ZSTD_isError(compressed))
        throw std::runtime_error(ZSTD_getErrorName(compressed));
    output.resize(compressed);
    return output;
}

TxBegin describe(TxBegin begin, std::span<const uint8_t> body) {
    begin.body = describe_component(kZstdTuBodyEncoding, body,
                                    begin.raw_bytes);
    begin.transaction_digest = compute_transaction_digest(
        begin, std::span<const uint8_t>{}, body);
    return begin;
}

}  // namespace

int main() {
    std::vector<uint8_t> input(128 * 1024);
    for (size_t index = 0; index != input.size(); ++index)
        input[index] = static_cast<uint8_t>(index);

    const ZstdTuEnvelope baseline = encode_zstd_tu(
        HistoryNonce{1}, RelSeq{0}, TuSeq{1}, icecc::digest128("pre"), input);
    const std::vector<uint8_t> frame = large_window_frame(input, 20);
    const TxBegin begin = describe(baseline.begin, frame);

    require_throws<std::invalid_argument>(
        [&] { (void)decode_zstd_tu(begin, frame,
                                   {frame.size() + 1, input.size(), 10}); },
        "frame exceeding max_window_log was accepted");
    require(decode_zstd_tu(begin, frame,
                           {frame.size() + 1, input.size(), 20}) == input,
            "frame inside max_window_log did not round-trip");

    require_throws<std::invalid_argument>(
        [&] { (void)ZstdTuDialogue(profile_bit(ProfileId::ZSTD_TU),
                                    {1, 1, 9}); },
        "window log below the Zstd minimum was accepted");
    require_throws<std::invalid_argument>(
        [&] { (void)ZstdTuDialogue(profile_bit(ProfileId::ZSTD_TU),
                                    {1, 1, 32}); },
        "window log above the portable maximum was accepted");

    std::cout << "p50_zstd_window_test: streaming window bound is live\n";
    return 0;
}
