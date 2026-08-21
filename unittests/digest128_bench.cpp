#include "services/digest128.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <vector>

int main() {
    constexpr size_t buffer_bytes = 32U << 20;
    constexpr size_t minimum_bytes = 512U << 20;
    std::vector<uint8_t> input(buffer_bytes);
    for (size_t i = 0; i != input.size(); ++i)
        input[i] = static_cast<uint8_t>((i * 1315423911ULL) >> 19);

    size_t processed = 0;
    uint8_t receipt = 0;
    const auto start = std::chrono::steady_clock::now();
    do {
        const icecc::Digest128 digest = icecc::digest128(input);
        receipt ^= digest.bytes[processed / buffer_bytes % digest.bytes.size()];
        processed += input.size();
    } while (processed < minimum_bytes);
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    const double gb_per_second = processed / seconds / 1.0e9;
    std::cout << std::fixed << std::setprecision(3)
              << "digest128_bench: " << gb_per_second << " GB/s over "
              << processed << " bytes; receipt=" << unsigned(receipt) << '\n';
    if (gb_per_second < 0.5) {
        std::cerr << "digest128_bench: common XXH3-128 wrapper is below the 0.5 GB/s gate\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
