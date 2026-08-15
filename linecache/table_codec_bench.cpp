// Exact fixed-table range-coder benchmark for issue #16 pretrained superblock streams.
//
// The model is immutable: 4096 deterministic two-byte context buckets select one of K integer
// probability tables.  An independent decoder derives the same contexts solely from bytes already
// decoded.  This is the production-shaped check for the Python mixture/table teacher.
//
// Build: g++ -O3 -march=native -std=c++17 table_codec_bench.cpp -o table_codec_bench -lzstd

#include <zstd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/resource.h>
#include <vector>

using Clock = std::chrono::steady_clock;

static void fail(const std::string& message) {
    std::fprintf(stderr, "FATAL: %s\n", message.c_str());
    std::exit(2);
}

template<class T> static T read_scalar(std::istream& input) {
    T value{};
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!input) fail("truncated scalar");
    return value;
}

struct Model {
    static constexpr uint32_t Contexts = 4096;
    uint32_t tables = 0, total = 0;
    std::vector<uint16_t> mapping;
    std::vector<std::array<uint16_t, 257>> cumulative;
    std::vector<std::vector<uint8_t>> inverse;
    uint64_t bytes = 0;

    explicit Model(const std::string& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) fail("cannot open model " + path);
        char magic[8]{}; input.read(magic, sizeof(magic));
        if (std::memcmp(magic, "ICTBL1\0", 7)) fail("wrong model magic");
        const uint32_t version = read_scalar<uint32_t>(input);
        const uint32_t contexts = read_scalar<uint32_t>(input);
        tables = read_scalar<uint32_t>(input);
        total = read_scalar<uint32_t>(input);
        if (version != 1 || contexts != Contexts || !tables || tables > 256 ||
            total < 256 || total > 65535)
            fail("unsupported model dimensions");
        mapping.resize(contexts);
        input.read(reinterpret_cast<char*>(mapping.data()), mapping.size() * sizeof(uint16_t));
        cumulative.resize(tables);
        for (uint32_t table = 0; table < tables; ++table) {
            cumulative[table][0] = 0;
            for (uint32_t symbol = 0; symbol < 256; ++symbol) {
                const uint16_t frequency = read_scalar<uint16_t>(input);
                if (!frequency || uint32_t(cumulative[table][symbol]) + frequency > total)
                    fail("invalid frequency table");
                cumulative[table][symbol + 1] =
                    uint16_t(cumulative[table][symbol] + frequency);
            }
            if (cumulative[table][256] != total) fail("frequency total mismatch");
        }
        for (uint16_t value : mapping) if (value >= tables) fail("bad context mapping");
        if (input.peek() != std::char_traits<char>::eof()) fail("trailing model bytes");
        bytes = 8 + 4 * 4 + mapping.size() * 2 + uint64_t(tables) * 256 * 2;
        inverse.assign(tables, std::vector<uint8_t>(total));
        for (uint32_t table = 0; table < tables; ++table)
            for (uint32_t symbol = 0; symbol < 256; ++symbol)
                for (uint32_t i = cumulative[table][symbol]; i < cumulative[table][symbol + 1]; ++i)
                    inverse[table][i] = uint8_t(symbol);
    }

    uint16_t table(uint8_t previous2, uint8_t previous) const {
        return mapping[(uint32_t(previous2) * 257u + previous) & (Contexts - 1)];
    }
};

class RangeEncoder {
public:
    explicit RangeEncoder(const Model& model) : model_(model) { output_.reserve(1 << 20); }

    const std::vector<uint8_t>& encode(const uint8_t* data, size_t size) {
        output_.clear(); low_ = 0; range_ = 0xffffffffu; cache_ = 0; cache_size_ = 1;
        uint8_t a = 0, b = 0;
        for (size_t i = 0; i < size; ++i) {
            const uint8_t symbol = data[i];
            const uint16_t table = model_.table(a, b);
            const auto& cumulative = model_.cumulative[table];
            range_ /= model_.total;
            low_ += uint64_t(cumulative[symbol]) * range_;
            range_ *= uint32_t(cumulative[symbol + 1] - cumulative[symbol]);
            while (range_ < (1u << 24)) { range_ <<= 8; shift_low(); }
            a = b; b = symbol;
        }
        for (unsigned i = 0; i < 5; ++i) shift_low();
        return output_;
    }

private:
    void shift_low() {
        const uint32_t low32 = uint32_t(low_);
        const uint32_t high = uint32_t(low_ >> 32);
        if (low32 < 0xff000000u || high != 0) {
            uint8_t value = cache_;
            do {
                output_.push_back(uint8_t(value + high));
                value = 0xff;
            } while (--cache_size_ != 0);
            cache_ = uint8_t(low32 >> 24);
        }
        ++cache_size_;
        low_ = uint64_t(low32 << 8);
    }

    const Model& model_;
    std::vector<uint8_t> output_;
    uint64_t low_ = 0;
    uint32_t range_ = 0xffffffffu;
    uint8_t cache_ = 0;
    uint64_t cache_size_ = 1;
};

class RangeDecoder {
public:
    explicit RangeDecoder(const Model& model) : model_(model) {}

    bool decode(const uint8_t* data, size_t size, size_t output_size, std::vector<uint8_t>& out) {
        if (size < 5) return false;
        const uint8_t* cursor = data;
        const uint8_t* end = data + size;
        uint32_t range = 0xffffffffu, code = 0;
        for (unsigned i = 0; i < 5; ++i) code = (code << 8) | *cursor++;
        out.clear(); out.reserve(output_size);
        uint8_t a = 0, b = 0;
        for (size_t i = 0; i < output_size; ++i) {
            const uint16_t table = model_.table(a, b);
            range /= model_.total;
            if (!range) return false;
            const uint32_t scaled = code / range;
            if (scaled >= model_.total) return false;
            const uint8_t symbol = model_.inverse[table][scaled];
            const auto& cumulative = model_.cumulative[table];
            code -= uint32_t(cumulative[symbol]) * range;
            range *= uint32_t(cumulative[symbol + 1] - cumulative[symbol]);
            while (range < (1u << 24)) {
                range <<= 8;
                code = (code << 8) | (cursor < end ? *cursor++ : 0);
            }
            out.push_back(symbol);
            a = b; b = symbol;
        }
        return true;
    }

private:
    const Model& model_;
};

struct Frame { uint64_t raw = 0; std::vector<uint8_t> payload; };

static std::vector<Frame> read_frames(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) fail("cannot open frames " + path);
    char magic[8]{}; input.read(magic, sizeof(magic));
    if (std::memcmp(magic, "ICFRM1\0", 7)) fail("wrong frame magic");
    if (read_scalar<uint32_t>(input) != 1) fail("wrong frame version");
    const uint32_t count = read_scalar<uint32_t>(input);
    std::vector<Frame> frames; frames.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        Frame frame;
        frame.raw = read_scalar<uint64_t>(input);
        const uint32_t size = read_scalar<uint32_t>(input);
        frame.payload.resize(size);
        input.read(reinterpret_cast<char*>(frame.payload.data()), size);
        if (!input) fail("truncated frame payload");
        frames.push_back(std::move(frame));
    }
    if (input.peek() != std::char_traits<char>::eof()) fail("trailing frame bytes");
    return frames;
}

static double elapsed(Clock::time_point begin) {
    return std::chrono::duration<double>(Clock::now() - begin).count();
}

int main(int argc, char** argv) {
    std::string model_path, frames_path;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--model") && i + 1 < argc) model_path = argv[++i];
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames_path = argv[++i];
        else fail(std::string("unknown/incomplete argument ") + argv[i]);
    }
    if (model_path.empty() || frames_path.empty())
        fail("usage: table_codec_bench --model M --frames F");
    Model model(model_path);
    std::vector<Frame> frames = read_frames(frames_path);
    RangeEncoder encoder(model);
    RangeDecoder decoder(model);
    std::vector<std::vector<uint8_t>> compressed; compressed.reserve(frames.size());
    uint64_t raw_input = 0, payload = 0, wire = 0;
    Clock::time_point begin = Clock::now();
    for (const Frame& frame : frames) {
        const auto& value = encoder.encode(frame.payload.data(), frame.payload.size());
        compressed.push_back(value);
        raw_input += frame.raw; payload += frame.payload.size(); wire += value.size() + 8;
    }
    const double encode_seconds = elapsed(begin);
    std::vector<uint8_t> recovered;
    begin = Clock::now();
    bool exact = true;
    for (size_t i = 0; i < frames.size(); ++i) {
        exact &= decoder.decode(compressed[i].data(), compressed[i].size(),
                                frames[i].payload.size(), recovered);
        exact &= recovered == frames[i].payload;
    }
    const double decode_seconds = elapsed(begin);

    ZSTD_CCtx* zc = ZSTD_createCCtx();
    ZSTD_DCtx* zd = ZSTD_createDCtx();
    if (!zc || !zd) fail("zstd context allocation");
    uint64_t zstd_wire = 0;
    double zstd_encode = 0, zstd_decode = 0;
    std::vector<uint8_t> zbuffer, zout;
    for (const Frame& frame : frames) {
        zbuffer.resize(ZSTD_compressBound(frame.payload.size()));
        begin = Clock::now();
        const size_t n = ZSTD_compressCCtx(zc, zbuffer.data(), zbuffer.size(),
                                          frame.payload.data(), frame.payload.size(), 1);
        zstd_encode += elapsed(begin);
        if (ZSTD_isError(n)) fail(ZSTD_getErrorName(n));
        zstd_wire += n + 8;
        zout.resize(frame.payload.size());
        begin = Clock::now();
        const size_t d = ZSTD_decompressDCtx(zd, zout.data(), zout.size(), zbuffer.data(), n);
        zstd_decode += elapsed(begin);
        exact &= !ZSTD_isError(d) && d == frame.payload.size() && zout == frame.payload;
    }
    ZSTD_freeCCtx(zc); ZSTD_freeDCtx(zd);

    struct rusage usage{}; getrusage(RUSAGE_SELF, &usage);
    std::printf("TABLE_CODEC model=%s frames=%zu tables=%u model_bytes=%llu raw_input=%llu "
                "payload=%llu wire=%llu charged_wire=%llu ratio=%.3f charged_ratio=%.3f "
                "enc_GBps=%.3f dec_GBps=%.3f exact=%s\n",
                model_path.c_str(), frames.size(), model.tables,
                (unsigned long long)model.bytes, (unsigned long long)raw_input,
                (unsigned long long)payload, (unsigned long long)wire,
                (unsigned long long)(wire + model.bytes), double(raw_input) / wire,
                double(raw_input) / (wire + model.bytes), raw_input / encode_seconds / 1e9,
                raw_input / decode_seconds / 1e9, exact ? "PASS" : "FAIL");
    std::printf("ZSTD1 frames=%zu wire=%llu ratio=%.3f enc_GBps=%.3f dec_GBps=%.3f exact=%s\n",
                frames.size(), (unsigned long long)zstd_wire, double(raw_input) / zstd_wire,
                raw_input / zstd_encode / 1e9, raw_input / zstd_decode / 1e9,
                exact ? "PASS" : "FAIL");
    std::printf("peak_RSS_MiB=%.1f\n", usage.ru_maxrss / 1024.0);
    return exact ? 0 : 1;
}
