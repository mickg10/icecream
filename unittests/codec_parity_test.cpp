#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace {

using Bytes = std::vector<std::uint8_t>;

class Sha256 {
  public:
    void update(const std::uint8_t *input, std::size_t size) {
        total_bytes_ += size;
        while (size != 0) {
            const std::size_t take = std::min(size, block_.size() - used_);
            std::copy_n(input, take, block_.begin() + used_);
            input += take;
            size -= take;
            used_ += take;
            if (used_ == block_.size()) {
                transform();
                used_ = 0;
            }
        }
    }

    [[nodiscard]] std::array<std::uint8_t, 32> finish() {
        const std::uint64_t bit_count = total_bytes_ * 8;
        const std::uint8_t marker = 0x80;
        update_padding(&marker, 1);
        const std::uint8_t zero = 0;
        while (used_ != 56)
            update_padding(&zero, 1);
        std::array<std::uint8_t, 8> length{};
        for (unsigned i = 0; i != length.size(); ++i)
            length[length.size() - 1 - i] = static_cast<std::uint8_t>(bit_count >> (8 * i));
        update_padding(length.data(), length.size());

        std::array<std::uint8_t, 32> result{};
        for (std::size_t i = 0; i != state_.size(); ++i) {
            result[i * 4] = static_cast<std::uint8_t>(state_[i] >> 24);
            result[i * 4 + 1] = static_cast<std::uint8_t>(state_[i] >> 16);
            result[i * 4 + 2] = static_cast<std::uint8_t>(state_[i] >> 8);
            result[i * 4 + 3] = static_cast<std::uint8_t>(state_[i]);
        }
        return result;
    }

  private:
    static constexpr std::array<std::uint32_t, 64> constants_{
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
        0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
        0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
        0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
        0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
        0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
        0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
        0xc67178f2U,
    };

    void update_padding(const std::uint8_t *input, std::size_t size) {
        while (size != 0) {
            const std::size_t take = std::min(size, block_.size() - used_);
            std::copy_n(input, take, block_.begin() + used_);
            input += take;
            size -= take;
            used_ += take;
            if (used_ == block_.size()) {
                transform();
                used_ = 0;
            }
        }
    }

    void transform() {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t i = 0; i != 16; ++i) {
            words[i] = (std::uint32_t{block_[i * 4]} << 24) |
                       (std::uint32_t{block_[i * 4 + 1]} << 16) |
                       (std::uint32_t{block_[i * 4 + 2]} << 8) | std::uint32_t{block_[i * 4 + 3]};
        }
        for (std::size_t i = 16; i != words.size(); ++i) {
            const std::uint32_t s0 =
                std::rotr(words[i - 15], 7) ^ std::rotr(words[i - 15], 18) ^ (words[i - 15] >> 3);
            const std::uint32_t s1 =
                std::rotr(words[i - 2], 17) ^ std::rotr(words[i - 2], 19) ^ (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }

        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];
        for (std::size_t i = 0; i != words.size(); ++i) {
            const std::uint32_t sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const std::uint32_t choose = (e & f) ^ (~e & g);
            const std::uint32_t temporary1 = h + sum1 + choose + constants_[i] + words[i];
            const std::uint32_t sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temporary2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    };
    std::array<std::uint8_t, 64> block_{};
    std::uint64_t total_bytes_ = 0;
    std::size_t used_ = 0;
};

struct Entry {
    std::string path;
    std::uint64_t bytes = 0;
    std::string sha256;
};

Bytes read_file(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot read " + path.string());
    return Bytes(std::istreambuf_iterator<char>(input), {});
}

std::string sha256_hex(const Bytes &bytes) {
    Sha256 hash;
    hash.update(bytes.data(), bytes.size());
    const auto digest = hash.finish();
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const std::uint8_t byte : digest)
        output << std::setw(2) << static_cast<unsigned>(byte);
    return output.str();
}

std::string string_field(const std::string &manifest, std::string_view name) {
    const std::regex pattern("\\\"" + std::string(name) + "\\\"\\s*:\\s*\\\"([^\\\"]*)\\\"");
    std::smatch match;
    if (!std::regex_search(manifest, match, pattern))
        throw std::runtime_error("manifest lacks string field " + std::string(name));
    return match[1].str();
}

std::uint64_t uint_field(const std::string &manifest, std::string_view name) {
    const std::regex pattern("\\\"" + std::string(name) + "\\\"\\s*:\\s*([0-9]+)");
    std::smatch match;
    if (!std::regex_search(manifest, match, pattern))
        throw std::runtime_error("manifest lacks integer field " + std::string(name));
    return std::stoull(match[1].str());
}

std::vector<Entry> entries(const std::string &manifest, std::string_view key) {
    const std::regex pattern("\\{\\s*\\\"" + std::string(key) +
                             "\\\"\\s*:\\s*\\\"([^\\\"]+)\\\"\\s*,\\s*"
                             "\\\"bytes\\\"\\s*:\\s*([0-9]+)\\s*,\\s*"
                             "\\\"sha256\\\"\\s*:\\s*\\\"([0-9a-f]{64})\\\"\\s*\\}");
    std::vector<Entry> result;
    for (std::sregex_iterator it(manifest.begin(), manifest.end(), pattern), end; it != end; ++it) {
        result.push_back({(*it)[1].str(), std::stoull((*it)[2].str()), (*it)[3].str()});
    }
    return result;
}

void verify_entry(const fs::path &root, const Entry &entry, std::unordered_set<std::string> &seen) {
    const fs::path relative(entry.path);
    if (relative.is_absolute() || entry.path.find("..") != std::string::npos)
        throw std::runtime_error("manifest path is not confined: " + entry.path);
    if (!seen.insert(entry.path).second)
        throw std::runtime_error("manifest path is duplicated: " + entry.path);
    const Bytes bytes = read_file(root / relative);
    if (bytes.size() != entry.bytes)
        throw std::runtime_error("byte count differs: " + entry.path);
    if (sha256_hex(bytes) != entry.sha256)
        throw std::runtime_error("SHA-256 differs: " + entry.path);
}

fs::path default_golden_root() {
    if (const char *source = std::getenv("ICECC_TEST_TOP_SRCDIR"))
        return fs::path(source) / "unittests/codec_golden";
    return fs::path("codec_golden");
}

} // namespace

int main(int argc, char **argv) {
    try {
        fs::path root = default_golden_root();
        for (int i = 1; i != argc; ++i) {
            const std::string_view argument(argv[i]);
            if (argument == "--golden" && i + 1 != argc) {
                root = argv[++i];
            } else {
                std::cerr << "usage: codec_parity [--golden DIR]\n";
                return 2;
            }
        }

        const Bytes manifest_bytes = read_file(root / "MANIFEST.json");
        const std::string manifest(manifest_bytes.begin(), manifest_bytes.end());
        if (string_field(manifest, "schema") != "codec-golden-manifest-v1")
            throw std::runtime_error("manifest schema is not v1");
        if (string_field(manifest, "corpus_rule") !=
            "gate corpora >= 1000 TUs; goldens are identity and smoke inputs only")
            throw std::runtime_error("manifest corpus rule differs");

        const std::vector<Entry> inputs = entries(manifest, "path");
        const std::vector<Entry> streams = entries(manifest, "name");
        if (inputs.size() != uint_field(manifest, "input_count"))
            throw std::runtime_error("manifest input cardinality differs");
        if (streams.size() != uint_field(manifest, "stream_count"))
            throw std::runtime_error("manifest stream cardinality differs");
        if (inputs.empty() || streams.empty())
            throw std::runtime_error("manifest has no golden entries");

        std::unordered_set<std::string> seen;
        for (const Entry &input : inputs)
            verify_entry(root, input, seen);
        for (const Entry &stream : streams)
            verify_entry(root, stream, seen);

        constexpr std::size_t registered_instantiations = 0;
        std::cout << "codec_parity: " << inputs.size() << " inputs, " << streams.size()
                  << " streams, " << registered_instantiations << " instantiations; PASS\n";
        if (registered_instantiations == 0)
            std::cout << "codec_parity: Phase 0 loaders and SHA-256 witnesses "
                         "verified; codec registrations begin in Phase 1\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "codec_parity: " << error.what() << '\n';
        return 1;
    }
}
