#include "../p50_endpoint.h"
#include "../codec/p29_wire.h"
#include "../../services/digest128.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_future.hpp>

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(ICECC_P50SIM_ALLOCATION_COUNTER)
namespace {
std::atomic<bool> p50sim_count_allocations_enabled{false};
std::atomic<uint64_t> p50sim_allocation_count{0};
std::atomic<uint64_t> p50sim_p29_allocation_count{0};
constexpr unsigned p50sim_p29_allocation_entry_count = 9;
constexpr unsigned p50sim_p29_allocation_entry_none =
    p50sim_p29_allocation_entry_count;
std::array<std::atomic<uint64_t>, p50sim_p29_allocation_entry_count>
    p50sim_p29_allocation_entries{};
thread_local unsigned p50sim_p29_allocation_entry =
    p50sim_p29_allocation_entry_none;

void record_p50sim_allocation() noexcept {
    if (p50sim_count_allocations_enabled.load(std::memory_order_relaxed)) {
        p50sim_allocation_count.fetch_add(1, std::memory_order_relaxed);
        if (p50sim_p29_allocation_entry <
            p50sim_p29_allocation_entry_count) {
            p50sim_p29_allocation_count.fetch_add(1,
                                                  std::memory_order_relaxed);
            p50sim_p29_allocation_entries[p50sim_p29_allocation_entry]
                .fetch_add(1, std::memory_order_relaxed);
        }
    }
}
}  // namespace

extern "C" unsigned
icecc_p50sim_p29_allocation_scope_enter(unsigned entry) noexcept {
    const unsigned previous = p50sim_p29_allocation_entry;
    p50sim_p29_allocation_entry = entry;
    return previous;
}

extern "C" void
icecc_p50sim_p29_allocation_scope_leave(unsigned previous) noexcept {
    p50sim_p29_allocation_entry = previous;
}

void* operator new(std::size_t size) {
    if (void* result = std::malloc(size == 0 ? 1 : size)) {
        record_p50sim_allocation();
        return result;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

void* operator new(std::size_t size, std::align_val_t alignment) {
    void* result = nullptr;
    const std::size_t bytes = size == 0 ? 1 : size;
    if (::posix_memalign(&result, static_cast<std::size_t>(alignment), bytes) != 0)
        throw std::bad_alloc();
    record_p50sim_allocation();
    return result;
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}
void operator delete(void* value, std::align_val_t) noexcept { std::free(value); }
void operator delete[](void* value, std::align_val_t) noexcept { std::free(value); }
void operator delete(void* value, std::size_t, std::align_val_t) noexcept {
    std::free(value);
}
void operator delete[](void* value, std::size_t, std::align_val_t) noexcept {
    std::free(value);
}
#endif

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using namespace icecc::p50;

namespace {

// SidecarRuntime's production route owner pins the current P50 source path to
// level 3.  Exact conformance must reproduce that product configuration, not
// P50PreparationAuthority's older level-1 unit-test default.
constexpr int kCurrentProductCompressionLevel = 3;

class InputFd {
public:
    explicit InputFd(const std::string& path)
        : value_(::open(path.c_str(), O_RDONLY | O_CLOEXEC)) {
        if (value_ < 0)
            throw std::system_error(
                errno, std::generic_category(),
                "cannot open input manifest payload: " + path);
    }

    ~InputFd() {
        if (value_ >= 0)
            ::close(value_);
    }

    InputFd(const InputFd&) = delete;
    InputFd& operator=(const InputFd&) = delete;

    [[nodiscard]] int get() const noexcept { return value_; }

private:
    int value_ = -1;
};

void read_bytes(const std::string& path, std::vector<uint8_t>& bytes) {
    const InputFd input(path);
    struct stat status {};
    if (::fstat(input.get(), &status) != 0)
        throw std::system_error(
            errno, std::generic_category(),
            "cannot determine input manifest payload size: " + path);
    if (status.st_size < 0 ||
        static_cast<uintmax_t>(status.st_size) >
            std::numeric_limits<size_t>::max())
        throw std::length_error("input manifest payload is too large: " + path);
    bytes.resize(static_cast<size_t>(status.st_size));
    size_t offset = 0;
    while (offset < bytes.size()) {
        const size_t request = std::min(
            bytes.size() - offset,
            static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t received =
            ::pread(input.get(), bytes.data() + offset, request,
                    static_cast<off_t>(offset));
        if (received < 0) {
            if (errno == EINTR)
                continue;
            throw std::system_error(errno, std::generic_category(),
                                    "cannot read input manifest payload: " + path);
        }
        if (received == 0)
            throw std::runtime_error(
                "input manifest payload shortened while reading: " + path);
        offset += static_cast<size_t>(received);
    }
}

std::vector<uint8_t> read_bytes(const std::string& path) {
    std::vector<uint8_t> bytes;
    read_bytes(path, bytes);
    return bytes;
}

void write_bytes(const std::string& path, std::span<const uint8_t> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("cannot open P29V1 inner-stream output");
    if (bytes.size() >
        static_cast<size_t>(std::numeric_limits<std::streamsize>::max()))
        throw std::length_error("P29V1 inner-stream output is too large");
    if (!bytes.empty())
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    if (!output)
        throw std::runtime_error("cannot write P29V1 inner-stream output");
}

template <typename Guid> bool parse_guid(std::string_view text, Guid& result) {
    if (text.size() != result.bytes.size() * 2) return false;
    auto nibble = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    for (size_t index = 0; index != result.bytes.size(); ++index) {
        const int high = nibble(text[index * 2]);
        const int low = nibble(text[index * 2 + 1]);
        if (high < 0 || low < 0) return false;
        result.bytes[index] = static_cast<uint8_t>((high << 4) | low);
    }
    return result != Guid{};
}

void write_summary(const std::string& path, std::span<const uint8_t> input,
                   const Digest128& raw_digest,
                   const ClientRunResult& client, const ServerRunResult& server,
                   const CompletionLog& completions, const ActionTrace& actions,
                   ProfileId profile) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("cannot open p50sim summary output");
    uint64_t c_to_f_bytes = 0;
    uint64_t f_to_c_bytes = 0;
    for (const AsyncCompletion& completion : completions.completions()) {
        if (completion.stamp.actor == ActorSide::C)
            c_to_f_bytes += completion.transferred_bytes;
        else
            f_to_c_bytes += completion.transferred_bytes;
    }
    const Digest128 final_state = actions.records().empty()
                                      ? Digest128{}
                                      : actions.records().back().state_digest;
    const Digest128 final_transaction = actions.records().empty()
                                            ? Digest128{}
                                            : actions.records().back().transaction_digest;
    const Digest128 final_raw = actions.records().empty()
                                    ? Digest128{}
                                    : actions.records().back().raw_digest;
    output << "{\"schema\":\"icecream-p50sim-execution-v1\",\"profile\":\""
           << profile_name(profile) << "\","
           << "\"raw_bytes\":" << input.size() << ","
           << "\"raw_digest\":\"" << icecc::digest128_hex(raw_digest)
           << "\",\"client_status\":\""
           << (client.status == ClientRunStatus::Committed ? "Committed" :
               client.status == ClientRunStatus::Disconnected ? "Disconnected" :
                                                                  "TerminalError")
           << "\",\"server_status\":\""
           << (server.status == ServerRunStatus::Completed ? "Completed" :
               server.status == ServerRunStatus::Disconnected ? "Disconnected" :
                                                                  "TerminalError")
           << "\",\"action_records\":" << actions.records().size()
           << ",\"c_to_f_bytes\":" << c_to_f_bytes
           << ",\"f_to_c_bytes\":" << f_to_c_bytes
           << ",\"final_state_digest\":\"" << icecc::digest128_hex(final_state)
           << "\",\"final_transaction_digest\":\""
           << icecc::digest128_hex(final_transaction)
           << "\",\"final_raw_digest\":\"" << icecc::digest128_hex(final_raw)
           << "\"}\n";
    if (!output)
        throw std::runtime_error("cannot write p50sim summary output");
}

void write_action_slice(const std::string& path,
                        std::span<const ActionRecord> records) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("cannot open Protocol-50 action slice output");
    for (const ActionRecord& record : records)
        output << action_jsonl(record) << '\n';
    if (!output)
        throw std::runtime_error("cannot write Protocol-50 action slice output");
}

struct Arguments {
    std::string input;
    std::string prewarm_input;
    std::string measured_input;
    std::string actions;
    std::string summary;
    std::string prewarm_actions;
    std::string measured_actions;
    CStoreGuid c_store_guid = CStoreGuid::from_u64(0x505053494dULL);
    FStoreGuid f_store_guid = FStoreGuid::from_u64(0x505053494dULL);
    HistoryNonce history_nonce{1};
    std::string batch_manifest;
    std::string batch_manifest_2;
    std::string batch_manifest_3;
    std::string batch_assignment_map;
    std::string batch_assignment_map_2;
    std::string batch_assignment_map_3;
    std::string batch_output;
    bool batch_allow_repeated_inputs = false;
    bool count_allocations = false;
    std::string p29_fingerprint_cache_directory;
    std::string p29_inner_cf_output;
    std::string p29_inner_fc_output;
    std::string codec_method;
    std::string codec_prefix;
    std::string codec_output;
    uint64_t codec_rel_seq = 0;
    uint64_t codec_tu_seq = 0;
};

ProfileId selected_profile() {
    const char* requested = std::getenv("ICECC_P50_PROFILE");
    if (requested == nullptr || *requested == '\0')
        return ProfileId::ZSTD_TU;
    const std::string value(requested);
    if (value == "P29V1")
        return ProfileId::P29V1;
    if (value == "ZSTD_TU")
        return ProfileId::ZSTD_TU;
    if (value == "ZSTD_ROUTE")
        return ProfileId::ZSTD_ROUTE;
    throw std::invalid_argument("unsupported ICECC_P50_PROFILE: " + value);
}

std::string_view selected_profile_label() {
    const char* requested = std::getenv("ICECC_P50_PROFILE");
    if (requested == nullptr || *requested == '\0')
        return "ZSTD_TU";
    return requested;
}

Arguments parse(int argc, char** argv) {
    Arguments result;
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--count-allocations") {
            result.count_allocations = true;
            continue;
        }
        if (index + 1 >= argc)
            throw std::invalid_argument("missing value for " + option);
        if (option == "--input")
            result.input = argv[++index];
        else if (option == "--prewarm-input")
            result.prewarm_input = argv[++index];
        else if (option == "--measured-input")
            result.measured_input = argv[++index];
        else if (option == "--actions")
            result.actions = argv[++index];
        else if (option == "--summary")
            result.summary = argv[++index];
        else if (option == "--prewarm-actions")
            result.prewarm_actions = argv[++index];
        else if (option == "--measured-actions")
            result.measured_actions = argv[++index];
        else if (option == "--c-store-guid") {
            if (!parse_guid(argv[++index], result.c_store_guid))
                throw std::invalid_argument("invalid --c-store-guid");
        } else if (option == "--f-store-guid") {
            if (!parse_guid(argv[++index], result.f_store_guid))
                throw std::invalid_argument("invalid --f-store-guid");
        } else if (option == "--history-nonce") {
            try {
                result.history_nonce.value = std::stoull(argv[++index]);
            } catch (...) {
                throw std::invalid_argument("invalid --history-nonce");
            }
            if (result.history_nonce.value == 0)
                throw std::invalid_argument("--history-nonce must be nonzero");
        } else if (option == "--batch-manifest")
            result.batch_manifest = argv[++index];
        else if (option == "--batch-manifest-2")
            result.batch_manifest_2 = argv[++index];
        else if (option == "--batch-manifest-3")
            result.batch_manifest_3 = argv[++index];
        else if (option == "--batch-assignment-map")
            result.batch_assignment_map = argv[++index];
        else if (option == "--batch-assignment-map-2")
            result.batch_assignment_map_2 = argv[++index];
        else if (option == "--batch-assignment-map-3")
            result.batch_assignment_map_3 = argv[++index];
        else if (option == "--batch-output")
            result.batch_output = argv[++index];
        else if (option == "--p29-fingerprint-cache-directory")
            result.p29_fingerprint_cache_directory = argv[++index];
        else if (option == "--p29-inner-cf-output")
            result.p29_inner_cf_output = argv[++index];
        else if (option == "--p29-inner-fc-output")
            result.p29_inner_fc_output = argv[++index];
        else if (option == "--batch-allow-repeated-inputs") {
            const std::string value = argv[++index];
            if (value != "0" && value != "1")
                throw std::invalid_argument(
                    "--batch-allow-repeated-inputs must be 0 or 1");
            result.batch_allow_repeated_inputs = value == "1";
        } else if (option == "--codec-method")
            result.codec_method = argv[++index];
        else if (option == "--codec-prefix")
            result.codec_prefix = argv[++index];
        else if (option == "--codec-output")
            result.codec_output = argv[++index];
        else if (option == "--codec-rel-seq") {
            try { result.codec_rel_seq = std::stoull(argv[++index]); }
            catch (...) { throw std::invalid_argument("invalid --codec-rel-seq"); }
        } else if (option == "--codec-tu-seq") {
            try { result.codec_tu_seq = std::stoull(argv[++index]); }
            catch (...) { throw std::invalid_argument("invalid --codec-tu-seq"); }
        }
        else
            throw std::invalid_argument("unknown option " + option);
    }
    if (!result.p29_fingerprint_cache_directory.empty() &&
        result.p29_fingerprint_cache_directory.front() != '/')
        throw std::invalid_argument(
            "--p29-fingerprint-cache-directory must be absolute");
    const bool batch = !result.batch_manifest.empty() ||
                       !result.batch_manifest_2.empty() ||
                       !result.batch_manifest_3.empty() ||
                       !result.batch_assignment_map.empty() ||
                       !result.batch_assignment_map_2.empty() ||
                       !result.batch_assignment_map_3.empty() ||
                       !result.batch_output.empty() ||
                       !result.p29_inner_cf_output.empty() ||
                       !result.p29_inner_fc_output.empty() ||
                       result.batch_allow_repeated_inputs ||
                       result.count_allocations;
    const bool codec = !result.codec_method.empty() || !result.codec_output.empty() ||
                       !result.codec_prefix.empty();
    if (codec) {
        if (result.codec_method != "ZSTD_TU" && result.codec_method != "ZSTD_ROUTE")
            throw std::invalid_argument("codec mode requires ZSTD_TU or ZSTD_ROUTE");
        if (result.input.empty() || result.codec_output.empty() || batch ||
            !result.actions.empty() || !result.summary.empty() ||
            result.count_allocations ||
            !result.p29_fingerprint_cache_directory.empty())
            throw std::invalid_argument("codec mode requires --input/--codec-output only");
        return result;
    }
    if (batch) {
        if (result.batch_manifest.empty() || result.batch_assignment_map.empty() ||
            result.batch_output.empty() || !result.input.empty() ||
            !result.prewarm_input.empty() || !result.measured_input.empty() ||
            !result.actions.empty() || !result.summary.empty() ||
            !result.prewarm_actions.empty() || !result.measured_actions.empty())
            throw std::invalid_argument(
                "batch mode requires --batch-manifest, --batch-assignment-map, "
                "and --batch-output, and cannot be combined with scenario options");
        if (!result.batch_manifest_2.empty() && result.batch_assignment_map_2.empty())
            result.batch_assignment_map_2 = result.batch_assignment_map;
        if (result.batch_manifest_2.empty() && !result.batch_assignment_map_2.empty())
            throw std::invalid_argument("--batch-assignment-map-2 requires --batch-manifest-2");
        if (!result.batch_manifest_3.empty() && result.batch_assignment_map_3.empty())
            result.batch_assignment_map_3 = result.batch_assignment_map;
        if (result.batch_manifest_3.empty() && !result.batch_assignment_map_3.empty())
            throw std::invalid_argument("--batch-assignment-map-3 requires --batch-manifest-3");
        if (!result.batch_manifest_3.empty() && result.batch_manifest_2.empty())
            throw std::invalid_argument("--batch-manifest-3 requires --batch-manifest-2");
        if (result.p29_inner_cf_output.empty() !=
            result.p29_inner_fc_output.empty())
            throw std::invalid_argument(
                "P29V1 inner capture requires both CF and FC output paths");
#if !defined(ICECC_P50SIM_ALLOCATION_COUNTER)
        if (result.count_allocations)
            throw std::invalid_argument(
                "--count-allocations requires an allocation-counter build");
#endif
        return result;
    }
    const bool warm = !result.prewarm_input.empty() || !result.measured_input.empty() ||
                      !result.prewarm_actions.empty() || !result.measured_actions.empty();
    if (warm && (result.prewarm_input.empty() || result.measured_input.empty() ||
                 result.prewarm_actions.empty() || result.measured_actions.empty()))
        throw std::invalid_argument(
            "warm replay requires --prewarm-input, --measured-input, "
            "--prewarm-actions, and --measured-actions");
    if ((warm ? !result.input.empty() : result.input.empty()) ||
        result.actions.empty() || result.summary.empty())
        throw std::invalid_argument(
            warm ? "warm replay does not accept --input" :
                   "--input, --actions, and --summary are required");
    return result;
}

void run_codec(const Arguments& arguments) {
    const std::vector<uint8_t> input = read_bytes(arguments.input);
    std::vector<uint8_t> prefix;
    if (!arguments.codec_prefix.empty())
        prefix = read_bytes(arguments.codec_prefix);
    const ZstdTuLimits limits{std::numeric_limits<uint64_t>::max(),
                              std::numeric_limits<uint64_t>::max(), 27,
                              uint64_t{128} << 20};
    const Digest128 pre_state{};
    std::vector<uint8_t> encoded;
    if (arguments.codec_method == "ZSTD_ROUTE") {
        ZstdRouteCodec codec(3);
        encoded = codec.encode(HistoryNonce{1}, RelSeq{arguments.codec_rel_seq},
                               TuSeq{arguments.codec_tu_seq}, pre_state, prefix,
                               input, limits).body;
    } else {
        ZstdTuCodec codec(3);
        encoded = codec.encode(HistoryNonce{1}, RelSeq{arguments.codec_rel_seq},
                              TuSeq{arguments.codec_tu_seq}, pre_state, input,
                              limits).body;
    }
    std::ofstream output(arguments.codec_output, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot open codec output");
    output.write(reinterpret_cast<const char*>(encoded.data()),
                 static_cast<std::streamsize>(encoded.size()));
    if (!output) throw std::runtime_error("cannot write codec output");
}

std::string id_hex(const Id128& id) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(id.bytes.size() * 2);
    for (uint8_t byte : id.bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}

FStoreGuid relation_guid(FStoreGuid base, size_t relation) {
    uint64_t suffix = 0;
    for (size_t index = 8; index < base.bytes.size(); ++index)
        suffix = (suffix << 8) | base.bytes[index];
    suffix += static_cast<uint64_t>(relation);
    for (size_t index = 0; index != 8; ++index)
        base.bytes[15 - index] = static_cast<uint8_t>(suffix >> (index * 8));
    return base;
}

std::string trim_copy(std::string value) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::vector<std::string> read_batch_manifest(const std::string& path,
                                             bool allow_repeated_inputs) {
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("cannot open batch TU manifest: " + path);
    std::vector<std::string> result;
    std::string line;
    while (std::getline(input, line)) {
        line = trim_copy(std::move(line));
        if (line.empty() || line[0] == '#')
            continue;
        result.push_back(std::move(line));
    }
    if (result.empty())
        throw std::invalid_argument("batch TU manifest is empty");
    std::vector<std::string> sorted = result;
    std::sort(sorted.begin(), sorted.end());
    if (!allow_repeated_inputs &&
        std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
        throw std::invalid_argument("batch TU manifest contains a duplicate path");
    return result;
}

uint64_t parse_decimal(std::string_view text, size_t& cursor) {
    while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor])))
        ++cursor;
    const size_t begin = cursor;
    while (cursor < text.size() && std::isdigit(static_cast<unsigned char>(text[cursor])))
        ++cursor;
    if (begin == cursor)
        throw std::invalid_argument("assignment map contains a non-numeric value");
    try {
        return std::stoull(std::string(text.substr(begin, cursor - begin)));
    } catch (...) {
        throw std::invalid_argument("assignment map numeric value is out of range");
    }
}

bool digest_is_zero(const Digest128& digest) {
    return digest == Digest128{};
}

std::vector<size_t> read_batch_assignments(const std::string& path,
                                           size_t expected, size_t* declared_cardinality) {
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("cannot open batch assignment map: " + path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string text = buffer.str();
    if (text.empty())
        throw std::invalid_argument("batch assignment map is empty");

    uint64_t cardinality = 0;
    std::vector<size_t> assignments;
    const size_t json_cardinality = text.find("\"cardinality\"");
    const size_t json_assignments = text.find("\"assignments\"");
    if (json_cardinality != std::string::npos && json_assignments != std::string::npos) {
        size_t cursor = text.find(':', json_cardinality);
        if (cursor == std::string::npos)
            throw std::invalid_argument("assignment map cardinality is malformed");
        ++cursor;
        cardinality = parse_decimal(text, cursor);
        const size_t open = text.find('[', json_assignments);
        const size_t close = text.find(']', open == std::string::npos ? 0 : open + 1);
        if (open == std::string::npos || close == std::string::npos)
            throw std::invalid_argument("assignment map assignments are malformed");
        cursor = open + 1;
        while (cursor < close) {
            while (cursor < close && (std::isspace(static_cast<unsigned char>(text[cursor])) ||
                                      text[cursor] == ','))
                ++cursor;
            if (cursor == close)
                break;
            assignments.push_back(static_cast<size_t>(parse_decimal(text, cursor)));
            while (cursor < close && std::isspace(static_cast<unsigned char>(text[cursor])))
                ++cursor;
            if (cursor < close && text[cursor] != ',')
                throw std::invalid_argument("assignment map assignments are malformed");
        }
    } else {
        std::istringstream lines(text);
        std::string line;
        bool first = true;
        while (std::getline(lines, line)) {
            line = trim_copy(std::move(line));
            if (line.empty() || line[0] == '#')
                continue;
            if (first) {
                first = false;
                const std::string prefix = "cardinality=";
                if (line.rfind(prefix, 0) == 0)
                    cardinality = std::stoull(line.substr(prefix.size()));
                else
                    throw std::invalid_argument(
                        "text assignment map must start with cardinality=<1|20>");
            } else {
                size_t cursor = 0;
                assignments.push_back(static_cast<size_t>(parse_decimal(line, cursor)));
                if (!trim_copy(line.substr(cursor)).empty())
                    throw std::invalid_argument("assignment map line has trailing data");
            }
        }
    }
    if (cardinality != 1 && cardinality != 20)
        throw std::invalid_argument("batch relationship cardinality must be 1 or 20");
    if (assignments.size() != expected)
        throw std::invalid_argument("assignment map length does not match TU manifest");
    for (size_t assignment : assignments)
        if (assignment >= cardinality)
            throw std::invalid_argument("assignment map selects an unknown relationship");
    *declared_cardinality = static_cast<size_t>(cardinality);
    return assignments;
}

struct BatchRelation {
    PreparationRouteKey route{};
    std::unique_ptr<P50ServerEndpoint> server;
    std::unique_ptr<P50ClientEndpoint> client;
    ActionTrace actions{256};
    CompletionLog completions{256};
    uint64_t request_token = 0;
    std::optional<Digest128> last_state_digest;
};

class P29V1InnerTrace {
public:
    explicit P29V1InnerTrace(uint64_t max_tu_bytes) {
        if (max_tu_bytes == 0 ||
            max_tu_bytes > std::numeric_limits<size_t>::max())
            throw std::invalid_argument(
                "P29V1 trace TU limit is not addressable");
        icecc::codec::P29WireLimits limits;
        limits.max_tu_bytes = static_cast<size_t>(max_tu_bytes);
        limits.max_region_bytes = limits.max_tu_bytes;
        need_bound_ = icecc::codec::p29v1_need_inner_bound(limits);
        fill_bound_ = icecc::codec::p29v1_fill_inner_bound(limits);
    }

    void observe(ActorSide actor, const Message& message) {
        if (actor == ActorSide::C) {
            if (const auto* begin = std::get_if<TxBegin>(&message)) {
                if (begin->profile != ProfileId::P29V1 || active_ || need_ || fill_)
                    throw std::logic_error(
                        "P29V1 inner trace observed an invalid TX_BEGIN boundary");
                active_ = true;
                ++begins_;
                return;
            }
            if (const auto* body = std::get_if<BodyMessage>(&message)) {
                require_active();
                append(cf_, body->bytes);
                return;
            }
            if (const auto* fill = std::get_if<FillMessage>(&message)) {
                require_active();
                if (!fill_)
                    fill_.emplace(fill_bound_);
                fill_->push(*fill);
                if (fill_->complete()) {
                    append(cf_, fill_->inner_frames());
                    fill_.reset();
                }
                return;
            }
        } else {
            if (const auto* need = std::get_if<NeedMessage>(&message)) {
                require_active();
                if (!need_)
                    need_.emplace(need_bound_);
                need_->push(*need);
                if (need_->complete()) {
                    append(fc_, need_->inner_frames());
                    need_.reset();
                }
                return;
            }
            if (std::holds_alternative<TxCommit>(message)) {
                require_active();
                if (need_ || fill_)
                    throw std::logic_error(
                        "P29V1 inner trace reached commit mid-continuation");
                active_ = false;
                ++commits_;
            }
        }
    }

    void finish() const {
        if (active_ || need_ || fill_ || begins_ != commits_)
            throw std::logic_error(
                "P29V1 inner trace ended outside an exact TU boundary");
    }

    [[nodiscard]] std::span<const uint8_t> cf() const noexcept { return cf_; }
    [[nodiscard]] std::span<const uint8_t> fc() const noexcept { return fc_; }

private:
    static void append(std::vector<uint8_t>& target,
                       std::span<const uint8_t> bytes) {
        if (bytes.size() > target.max_size() - target.size())
            throw std::overflow_error(
                "P29V1 inner trace exceeds addressable size");
        target.insert(target.end(), bytes.begin(), bytes.end());
    }

    void require_active() const {
        if (!active_)
            throw std::logic_error(
                "P29V1 inner trace observed data outside a transaction");
    }

    uint64_t need_bound_ = 0;
    uint64_t fill_bound_ = 0;
    std::optional<P29V1NeedStreamDecoder> need_;
    std::optional<P29V1FillStreamDecoder> fill_;
    std::vector<uint8_t> cf_;
    std::vector<uint8_t> fc_;
    uint64_t begins_ = 0;
    uint64_t commits_ = 0;
    bool active_ = false;
};

std::string_view client_status_name(ClientRunStatus status) {
    switch (status) {
    case ClientRunStatus::Committed: return "Committed";
    case ClientRunStatus::Disconnected: return "Disconnected";
    case ClientRunStatus::DeadlineExceeded: return "DeadlineExceeded";
    case ClientRunStatus::TerminalError: return "TerminalError";
    }
    return "unknown";
}

std::string_view server_status_name(ServerRunStatus status) {
    switch (status) {
    case ServerRunStatus::Completed: return "Completed";
    case ServerRunStatus::Disconnected: return "Disconnected";
    case ServerRunStatus::TerminalError: return "TerminalError";
    case ServerRunStatus::DeadlineExceeded: return "DeadlineExceeded";
    }
    return "unknown";
}

std::string bounded_context(std::string_view value) {
    constexpr size_t kContextLimit = 256;
    if (value.size() <= kContextLimit)
        return std::string(value);
    return std::string(value.substr(0, kContextLimit)) + "...";
}

void write_batch_row(std::ostream& output, std::string_view segment,
                     std::string_view relation_id, size_t index,
                     size_t expected_tu_seq,
                     const std::vector<uint8_t>& input,
                     const Digest128& raw_digest,
                     const ClientRunResult& client, const ServerRunResult& server,
                     const CompletionLog& completions, const ActionTrace& actions,
                     const P50ClientEndpoint& client_endpoint,
                     const std::optional<Digest128>& expected_before,
                     size_t prefix_before_bytes, const Digest128& prefix_before_digest,
                     size_t prefix_after_bytes, const Digest128& prefix_after_digest,
                     std::chrono::steady_clock::duration prepare_elapsed,
                     std::chrono::steady_clock::duration elapsed,
                     uint64_t p29v1_interner_reserved_bytes,
                     uint64_t p29v1_interner_committed_bytes,
                     ProfileId profile,
                     std::optional<uint64_t> allocations,
                     std::optional<uint64_t> p29v1_codec_allocations,
                     std::optional<std::array<uint64_t, 9>>
                         p29v1_codec_allocation_entries) {
    Digest128 tx_digest{};
    Digest128 begin_tx_digest{};
    Digest128 action_raw_digest{};
    Digest128 state_before{};
    Digest128 state_after{};
    RelSeq begin_rel_seq{};
    RelSeq commit_rel_seq{};
    uint64_t encoded_source_bytes = 0;
    bool tx_begin_found = false;
    bool commit_found = false;
    for (auto position = actions.records().rbegin(); position != actions.records().rend(); ++position) {
        if (position->actor == ActorSide::C &&
            position->action == ActionType::COMMIT_ACCEPTED && position->raw_digest == raw_digest &&
            position->tu_seq.value == expected_tu_seq) {
            tx_digest = position->transaction_digest;
            action_raw_digest = position->raw_digest;
            state_after = position->state_digest;
            commit_rel_seq = position->rel_seq;
            commit_found = true;
        }
        if (position->actor == ActorSide::C &&
            position->action == ActionType::TX_BEGIN && position->raw_digest == raw_digest &&
            position->tu_seq.value == expected_tu_seq) {
            state_before = position->state_digest;
            begin_tx_digest = position->transaction_digest;
            begin_rel_seq = position->rel_seq;
            encoded_source_bytes = position->stage_bytes;
            tx_begin_found = true;
        }
    }
    uint64_t c_writes = 0;
    uint64_t f_writes = 0;
    uint64_t c_reads = 0;
    uint64_t f_reads = 0;
    for (const AsyncCompletion& completion : completions.completions()) {
        if (completion.stamp.operation == AsyncOperationKind::WriteFragment) {
            if (completion.stamp.actor == ActorSide::C)
                c_writes += completion.transferred_bytes;
            else
                f_writes += completion.transferred_bytes;
        } else if (completion.stamp.operation == AsyncOperationKind::ReadHeader ||
                   completion.stamp.operation == AsyncOperationKind::ReadPayload) {
            if (completion.stamp.actor == ActorSide::C)
                c_reads += completion.transferred_bytes;
            else
                f_reads += completion.transferred_bytes;
        }
    }
    if ((digest_is_zero(state_before) ||
        (expected_before.has_value() && state_before != *expected_before) ||
        state_after != client_endpoint.state_digest()) ||
        !client.committed_input || !server.committed_input ||
        client.committed_input != server.committed_input ||
        client.committed_input->tu_seq.value != expected_tu_seq || action_raw_digest != raw_digest ||
        !tx_begin_found || !commit_found || !client.committed_commit ||
        begin_tx_digest != tx_digest || begin_rel_seq != commit_rel_seq ||
        begin_rel_seq != client.committed_commit->rel_seq ||
        digest_is_zero(tx_digest) || digest_is_zero(state_after) ||
        c_writes != f_reads || f_writes != c_reads) {
        std::ostringstream detail;
        detail << "product completion did not authenticate exact TU (client="
               << static_cast<bool>(client.committed_input) << ",server="
               << static_cast<bool>(server.committed_input) << ",same="
               << (client.committed_input && server.committed_input &&
                   *client.committed_input == *server.committed_input)
               << ",seq=" << (client.committed_input ?
                                  std::to_string(client.committed_input->tu_seq.value) : "none")
               << ",index=" << expected_tu_seq << ",raw=" << (action_raw_digest == raw_digest)
               << ",rel=" << begin_rel_seq.value << "/" << commit_rel_seq.value
               << ",tx_begin=" << tx_begin_found << ",commit=" << commit_found
               << ",wire=" << (c_writes == f_reads && f_writes == c_reads) << ")";
        throw std::runtime_error(detail.str());
    }
    output << "{\"schema\":\"icecream-p50sim-batch-v1\",\"segment\":\""
           << segment << "\",\"profile\":\"" << selected_profile_label()
           << "\",\"relationship_id\":\"" << relation_id << "\",\"tu_index\":"
           << index << ",\"tu_seq\":" << client.committed_input->tu_seq.value
           << ",\"rel_seq\":" << begin_rel_seq.value
           << ",\"c_store_guid\":\"" << id_hex(client_endpoint.c_store_guid())
           << "\",\"f_store_guid\":\""
           << id_hex(client_endpoint.f_store_guid().value_or(FStoreGuid{})) << "\""
           << ",\"raw_bytes\":" << input.size() << ",\"raw_digest\":\""
           << icecc::digest128_hex(raw_digest) << "\",\"encoded_source_bytes\":"
           << encoded_source_bytes << ",\"c_to_f_bytes\":" << c_writes
           << ",\"f_to_c_bytes\":" << f_writes << ",\"peer_c_read_bytes\":" << c_reads
           << ",\"peer_f_read_bytes\":" << f_reads << ",\"prepare_ns\":"
           << std::chrono::duration_cast<std::chrono::nanoseconds>(prepare_elapsed).count()
           << ",\"simulator_execution_ns\":"
           << std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()
           << ",\"f_apply_materialize_ns\":"
           << server.f_apply_materialize_ns
           << ",\"p29v1_interner_reserved_bytes\":"
           << p29v1_interner_reserved_bytes
           << ",\"p29v1_interner_committed_bytes\":"
           << p29v1_interner_committed_bytes
           << ",\"negotiated_profile_mask\":" << profile_bit(profile)
           << ",\"system_source_reuse\":"
           << (profile == ProfileId::P29V1 &&
                       !digest_is_zero(p29_system_source_fingerprint())
                   ? "true"
                   : "false")
           << ",\"action_records\":" << actions.records().size()
           << ",\"state_before_digest\":\"" << icecc::digest128_hex(state_before)
           << "\",\"state_digest\":\"" << icecc::digest128_hex(state_after)
           << "\",\"transaction_digest\":\"" << icecc::digest128_hex(tx_digest)
           << "\",\"committed\":true";
    if (allocations)
        output << ",\"allocations\":" << *allocations;
    if (p29v1_codec_allocations)
        output << ",\"p29v1_codec_allocations\":"
               << *p29v1_codec_allocations;
    if (p29v1_codec_allocation_entries) {
        static constexpr std::array<std::string_view, 9> names{
            "sender_begin_tu", "sender_answer_need", "sender_commit",
            "sender_abandon", "receiver_receive_body",
            "receiver_receive_fill", "receiver_take_materialized",
            "receiver_commit", "receiver_abandon"};
        output << ",\"p29v1_codec_allocation_entries\":{";
        for (size_t index = 0; index != names.size(); ++index) {
            if (index != 0)
                output << ',';
            output << '\"' << names[index] << "\":"
                   << (*p29v1_codec_allocation_entries)[index];
        }
        output << '}';
    }
    if (profile == ProfileId::ZSTD_ROUTE) {
        output << ",\"committed_raw_prefix_before_descriptor\":{\"schema\":\"icecream-s8-route-prefix-descriptor-v1\",\"bytes\":"
               << prefix_before_bytes << ",\"digest128\":\""
               << icecc::digest128_hex(prefix_before_digest)
               << "\"},\"committed_raw_prefix_descriptor\":{\"schema\":\"icecream-s8-route-prefix-descriptor-v1\",\"bytes\":"
               << prefix_after_bytes << ",\"digest128\":\""
               << icecc::digest128_hex(prefix_after_digest) << "\"}";
    }
    output << "}\n";
    if (!output)
        throw std::runtime_error("cannot write batch output");
}

void run_batch(const Arguments& arguments) {
    const ProfileId profile = selected_profile();
#if defined(ICECC_P50SIM_ALLOCATION_COUNTER)
    p50sim_count_allocations_enabled.store(arguments.count_allocations,
                                           std::memory_order_relaxed);
#endif
    const bool retains_relationship_state =
        profile == ProfileId::ZSTD_ROUTE || profile == ProfileId::P29V1;
    const std::vector<std::string> first = read_batch_manifest(
        arguments.batch_manifest, arguments.batch_allow_repeated_inputs);
    size_t relationship_count = 0;
    const std::vector<size_t> first_assignments = read_batch_assignments(
        arguments.batch_assignment_map, first.size(), &relationship_count);
    std::vector<std::string> second;
    std::vector<size_t> second_assignments;
    std::vector<std::string> third;
    std::vector<size_t> third_assignments;
    if (!arguments.batch_manifest_2.empty()) {
        second = read_batch_manifest(arguments.batch_manifest_2,
                                     arguments.batch_allow_repeated_inputs);
        size_t second_relationship_count = 0;
        second_assignments = read_batch_assignments(arguments.batch_assignment_map_2,
                                                    second.size(), &second_relationship_count);
        if (second_relationship_count != relationship_count)
            throw std::invalid_argument("repeat segment relationship cardinality differs");
        // A repeat build may legitimately dispatch the same TUs to different
        // relationships/slots.  Cardinality and range are checked by
        // read_batch_assignments; relationship state is retained by index.
    }
    if (!arguments.batch_manifest_3.empty()) {
        third = read_batch_manifest(arguments.batch_manifest_3,
                                    arguments.batch_allow_repeated_inputs);
        size_t third_relationship_count = 0;
        third_assignments = read_batch_assignments(
            arguments.batch_assignment_map_3, third.size(), &third_relationship_count);
        if (third_relationship_count != relationship_count)
            throw std::invalid_argument("third segment relationship cardinality differs");
    }
    std::ofstream output(arguments.batch_output, std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("cannot open batch output: " + arguments.batch_output);

    EndpointCaps caps{};
    caps.profile = profile;
    caps.supported_profiles = profile_bit(profile);
    PreparationAuthorityLimits authority_limits{};
    authority_limits.max_live_entries = 1;
    auto authority = std::make_shared<P50PreparationAuthority>(
        arguments.c_store_guid, caps.zstd, authority_limits,
        kCurrentProductCompressionLevel, profile);
    std::optional<P29V1InnerTrace> inner_trace;
    if (!arguments.p29_inner_cf_output.empty())
        inner_trace.emplace(caps.zstd.max_raw_bytes);
    asio::io_context context;
    tcp::acceptor acceptor(context,
                           tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const tcp::endpoint endpoint = acceptor.local_endpoint();
    std::vector<BatchRelation> relations;
    relations.reserve(relationship_count);
    for (size_t relation = 0; relation < relationship_count; ++relation) {
        const FStoreGuid f_guid = relation_guid(arguments.f_store_guid, relation);
        relations.emplace_back();
        BatchRelation& item = relations.back();
        item.route = PreparationRouteKey{f_guid, 1, profile};
        P50ServerEndpointConfig config;
        config.input_job_state = [](CStoreGuid, const TxBegin&, const TxCommit&,
                                    std::span<const uint8_t>) { return InputJobState::Open; };
        item.server = std::make_unique<P50ServerEndpoint>(f_guid, caps,
                                                           &item.completions, &item.actions,
                                                           std::move(config));
        item.client = std::make_unique<P50ClientEndpoint>(authority, caps,
                                                           arguments.history_nonce,
                                                           &item.completions, &item.actions,
                                                           std::nullopt,
                                                           std::function<void(EndpointCancelPermit)>{},
                                                           std::function<void(EndpointCancelPermit,
                                                                              EndpointTerminalResult)>{},
                                                           item.route);
    }
    uint64_t global_tu_seq = 0;
    const auto process = [&](std::string_view segment, const std::vector<std::string>& manifest,
                             const std::vector<size_t>& assignments) {
        std::vector<uint8_t> input;
        for (size_t index = 0; index < manifest.size(); ++index) {
            BatchRelation& relation = relations[assignments[index]];
            const std::string relation_id =
                "c1f" + std::to_string(relationship_count) + "-r" +
                (assignments[index] < 10 ? "0" : "") + std::to_string(assignments[index]);
            read_bytes(manifest[index], input);
            const Digest128 raw_digest = icecc::digest128(input);
            relation.actions.clear();
            relation.completions.clear();
            const std::optional<Digest128> expected_before =
                retains_relationship_state ? relation.last_state_digest : std::nullopt;
            const size_t prefix_before_bytes = authority->route_history_bytes(relation.route);
            const Digest128 prefix_before_digest = authority->route_history_digest(relation.route);
            const uint64_t expected_tu_seq = global_tu_seq;
#if defined(ICECC_P50SIM_ALLOCATION_COUNTER)
            const uint64_t p29v1_codec_allocations_before =
                p50sim_p29_allocation_count.load(std::memory_order_relaxed);
            std::array<uint64_t, p50sim_p29_allocation_entry_count>
                p29v1_codec_allocation_entries_before{};
            for (size_t entry = 0;
                 entry != p29v1_codec_allocation_entries_before.size();
                 ++entry)
                p29v1_codec_allocation_entries_before[entry] =
                    p50sim_p29_allocation_entries[entry].load(
                        std::memory_order_relaxed);
#endif
            const auto prepare_started = std::chrono::steady_clock::now();
            auto prepared = authority->prepare_for_route(
                relation.route,
                PrepareRequestKey{static_cast<uint64_t>(assignments[index] + 1),
                                  ++relation.request_token}, input);
            const auto prepare_elapsed = std::chrono::steady_clock::now() - prepare_started;
            if (!prepared)
                throw std::runtime_error("product preparation returned an invalid handle");
            ++global_tu_seq;
            context.restart();
            const auto started = std::chrono::steady_clock::now();
#if defined(ICECC_P50SIM_ALLOCATION_COUNTER)
            const uint64_t allocations_before =
                p50sim_allocation_count.load(std::memory_order_relaxed);
#endif
            EndpointIoControl server_control;
            EndpointIoControl client_control;
            if (inner_trace) {
                const auto observe = [&inner_trace](ActorSide actor,
                                                    const Message& message) {
                    inner_trace->observe(actor, message);
                };
                server_control.outbound_message_observer = observe;
                client_control.outbound_message_observer = observe;
            }
            auto server_future = asio::co_spawn(context,
                                                relation.server->accept_one(
                                                    acceptor,
                                                    std::move(server_control)),
                                                asio::use_future);
            auto client_future = asio::co_spawn(context,
                                                relation.client->run(
                                                    endpoint, prepared,
                                                    std::move(client_control)),
                                                asio::use_future);
            context.run();
            const auto elapsed = std::chrono::steady_clock::now() - started;
            const ServerRunResult server_result = server_future.get();
            const ClientRunResult client_result = client_future.get();
            std::optional<uint64_t> allocations;
            std::optional<uint64_t> p29v1_codec_allocations;
            std::optional<std::array<uint64_t, 9>>
                p29v1_codec_allocation_entries;
#if defined(ICECC_P50SIM_ALLOCATION_COUNTER)
            if (arguments.count_allocations) {
                const uint64_t after =
                    p50sim_allocation_count.load(std::memory_order_relaxed);
                allocations = after - allocations_before;
                const uint64_t p29v1_codec_after =
                    p50sim_p29_allocation_count.load(std::memory_order_relaxed);
                p29v1_codec_allocations =
                    p29v1_codec_after - p29v1_codec_allocations_before;
                p29v1_codec_allocation_entries.emplace();
                for (size_t entry = 0;
                     entry != p29v1_codec_allocation_entries->size();
                     ++entry) {
                    const uint64_t entry_after =
                        p50sim_p29_allocation_entries[entry].load(
                            std::memory_order_relaxed);
                    (*p29v1_codec_allocation_entries)[entry] =
                        entry_after -
                        p29v1_codec_allocation_entries_before[entry];
                }
            }
#endif
            if (client_result.status != ClientRunStatus::Committed ||
                server_result.status != ServerRunStatus::Completed ||
                !client_result.committed_input || !server_result.committed_input ||
                client_result.committed_input != server_result.committed_input ||
                !relation.actions.valid() || !relation.completions.valid())
            {
                const P50ServerOwnerUsage usage = relation.server->owner_usage();
                std::ostringstream detail;
                detail << "product batch TU did not commit on both endpoints"
                       << " (segment=" << bounded_context(segment)
                       << ",index=" << index
                       << ",path=" << bounded_context(manifest[index])
                       << ",relationship=" << relation_id
                       << ",client_status=" << client_status_name(client_result.status)
                       << ",server_status=" << server_status_name(server_result.status)
                       << ",client_error="
                       << (client_result.terminal_error
                               ? bounded_context(client_result.terminal_error->detail) : "none")
                       << ",server_error="
                       << (server_result.terminal_error
                               ? bounded_context(server_result.terminal_error->detail) : "none")
                       << ",actions_valid=" << relation.actions.valid()
                       << ",action_records=" << relation.actions.records().size()
                       << ",completions_valid=" << relation.completions.valid()
                       << ",completion_records=" << relation.completions.completions().size()
                       << ",owner_retained_records=" << usage.retained_input_records
                       << ",owner_retained_bytes=" << usage.retained_input_bytes
                       << ",owner_live_sessions=" << usage.live_sessions
                       << ",owner_namespaces=" << usage.namespaces << ")";
                throw std::runtime_error(detail.str());
            }
            const size_t prefix_after_bytes = authority->route_history_bytes(relation.route);
            const Digest128 prefix_after_digest = authority->route_history_digest(relation.route);
            write_batch_row(output, segment, relation_id, index,
                            expected_tu_seq, input, raw_digest,
                            client_result, server_result, relation.completions,
                            relation.actions, *relation.client, expected_before,
                            prefix_before_bytes, prefix_before_digest,
                            prefix_after_bytes, prefix_after_digest,
                            prepare_elapsed, elapsed,
                            authority->p29v1_interner_reserved_bytes(),
                            authority->p29v1_interner_committed_bytes(),
                            profile, allocations, p29v1_codec_allocations,
                            p29v1_codec_allocation_entries);
            output.flush();
            if (!output)
                throw std::runtime_error("cannot flush batch output before input reclamation");

            for (const ActionRecord& action : relation.actions.records()) {
                if (action.action == ActionType::COMMIT_ACCEPTED &&
                    action.raw_digest == raw_digest) {
                    if (retains_relationship_state)
                        relation.last_state_digest = action.state_digest;
                    break;
                }
            }

            // The batch row has authenticated and persisted the exact
            // compiler-visible key.  Retire that logical input lease before
            // reading the next TU.  This reclaims F InputRecordStore bytes;
            // relationship/profile state remains owned by the endpoint and
            // preparation authority for repeat/full-2 continuity.
            if (!server_result.committed_input)
                throw std::runtime_error(
                    "product batch committed row has no compiler input key (relationship=" +
                    relation_id + ")");
            relation.server->close_input_job(*server_result.committed_input);
            relation.server->collect_input_garbage();
            const P50ServerOwnerUsage usage = relation.server->owner_usage();
            if (usage.retained_input_records != 0 || usage.retained_input_bytes != 0) {
                std::ostringstream detail;
                detail << "product batch input reclamation left retained records"
                       << " (segment=" << bounded_context(segment)
                       << ",index=" << index
                       << ",path=" << bounded_context(manifest[index])
                       << ",relationship=" << relation_id
                       << ",retained_records=" << usage.retained_input_records
                       << ",retained_bytes=" << usage.retained_input_bytes << ")";
                throw std::runtime_error(detail.str());
            }
            if (authority->release(prepared) != 0 ||
                authority->live_entry_count() != 0 ||
                authority->retained_encoded_bytes() != 0)
                throw std::runtime_error(
                    "product batch retained a committed preparation");
        }
    };
    if (!third.empty()) {
        process("prewarm", first, first_assignments);
        process("full-1", second, second_assignments);
        process("full-2", third, third_assignments);
    } else {
        process("full-1", first, first_assignments);
        if (!second.empty())
            process("full-2", second, second_assignments);
    }
    if (inner_trace) {
        inner_trace->finish();
        write_bytes(arguments.p29_inner_cf_output, inner_trace->cf());
        write_bytes(arguments.p29_inner_fc_output, inner_trace->fc());
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Arguments arguments = parse(argc, argv);
        if (!arguments.codec_method.empty()) {
            run_codec(arguments);
            return 0;
        }
        const ProfileId profile = selected_profile();
        if (!arguments.p29_fingerprint_cache_directory.empty() &&
            profile != ProfileId::P29V1)
            throw std::invalid_argument(
                "--p29-fingerprint-cache-directory requires P29V1");
        if (!arguments.p29_inner_cf_output.empty() &&
            profile != ProfileId::P29V1)
            throw std::invalid_argument(
                "P29V1 inner capture requires ICECC_P50_PROFILE=P29V1");
        if (profile == ProfileId::P29V1) {
            start_p29_system_source_fingerprint(
                arguments.p29_fingerprint_cache_directory);
            // The simulator is an offline measurement harness.  Waiting here
            // keeps its P29V1 byte goldens deterministic while the product
            // daemon remains strictly zero-until-ready and nonblocking.
            (void)wait_p29_system_source_fingerprint_for(
                std::chrono::seconds(5));
        }
        if (!arguments.batch_manifest.empty()) {
            run_batch(arguments);
            return 0;
        }
        const bool warm = !arguments.prewarm_input.empty();
        const std::vector<uint8_t> input = warm
            ? read_bytes(arguments.measured_input)
            : read_bytes(arguments.input);
        const Digest128 raw_digest = icecc::digest128(input);

        ActionTrace actions(1024);
        CompletionLog completions(4096);
        EndpointCaps caps{};
        caps.profile = selected_profile();
        caps.supported_profiles = profile_bit(caps.profile);
        auto authority = std::make_shared<P50PreparationAuthority>(
            arguments.c_store_guid, caps.zstd,
            PreparationAuthorityLimits{}, kCurrentProductCompressionLevel,
            caps.profile);
        const std::vector<uint8_t> prewarm_input = warm
            ? read_bytes(arguments.prewarm_input) : std::vector<uint8_t>{};

        P50ServerEndpointConfig config;
        config.input_job_state = [](CStoreGuid, const TxBegin&, const TxCommit&,
                                    std::span<const uint8_t>) { return InputJobState::Open; };
        P50ServerEndpoint server(arguments.f_store_guid, caps,
                                 &completions, &actions, std::move(config));
        P50ClientEndpoint client(authority, caps, arguments.history_nonce, &completions, &actions);

        // Prepare only after the relationship endpoint exists so warm
        // execution cannot create stale route state.
        const PreparedTuHandle prewarm_prepared = warm
            ? authority->prepare(PrepareRequestKey{1, 1}, prewarm_input)
            : PreparedTuHandle{};
        if (warm && !prewarm_prepared)
            throw std::runtime_error("Protocol-50 preparation returned an invalid handle");

        asio::io_context context;
        tcp::acceptor acceptor(context, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
        const tcp::endpoint endpoint = acceptor.local_endpoint();
        ClientRunResult client_result;
        ServerRunResult server_result;
        const auto run_one = [&](PreparedTuHandle handle) {
            auto server_future = asio::co_spawn(context, server.accept_one(acceptor),
                                                asio::use_future);
            auto client_future = asio::co_spawn(context, client.run(endpoint, handle),
                                                asio::use_future);
            context.run();
            server_result = server_future.get();
            client_result = client_future.get();
            if (client_result.status != ClientRunStatus::Committed ||
                server_result.status != ServerRunStatus::Completed)
                throw std::runtime_error(
                    "Protocol-50 execution did not commit on both endpoints");
            if (warm)
                context.restart();
        };
        const size_t prewarm_begin = actions.records().size();
        if (warm)
            run_one(prewarm_prepared);
        const size_t measured_begin = actions.records().size();
        // Route-history profiles may prepare only one tentative TU at a time.
        // The live warm lifecycle commits TU0 before the next compiler wrapper
        // prepares TU1, so predictive/exact execution must preserve that order.
        const PreparedTuHandle prepared = authority->prepare(
            warm ? PrepareRequestKey{1, 2} : PrepareRequestKey{1, 1}, input);
        if (!prepared)
            throw std::runtime_error("Protocol-50 preparation returned an invalid handle");
        run_one(prepared);
        const size_t measured_end = actions.records().size();

        if (client_result.status != ClientRunStatus::Committed ||
            server_result.status != ServerRunStatus::Completed)
            throw std::runtime_error(
                "Protocol-50 execution did not commit on both endpoints (client=" +
                std::to_string(static_cast<unsigned>(client_result.status)) +
                ", server=" + std::to_string(static_cast<unsigned>(server_result.status)) +
                (server_result.terminal_error ? ", server_error=" +
                     server_result.terminal_error->detail : std::string{}) +
                (client_result.terminal_error ? ", client_error=" +
                     client_result.terminal_error->detail : std::string{}) +
                ")");
        if (!actions.valid())
            throw std::runtime_error("Protocol-50 action trace exceeded its bound");
        if (const auto error = check_action_trace(actions.records()); error)
            throw std::runtime_error("Protocol-50 action trace mismatch: " + *error);
        write_action_trace(actions, arguments.actions);
        if (warm) {
            write_action_slice(arguments.prewarm_actions,
                               std::span<const ActionRecord>(actions.records().data() + prewarm_begin,
                                                             measured_begin - prewarm_begin));
            write_action_slice(arguments.measured_actions,
                               std::span<const ActionRecord>(actions.records().data() + measured_begin,
                                                             measured_end - measured_begin));
        }
        write_summary(arguments.summary, input, raw_digest,
                      client_result, server_result,
                      completions, actions, caps.profile);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "p50sim: " << error.what() << '\n';
        return 1;
    }
}
