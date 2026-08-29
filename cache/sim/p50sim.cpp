#include "../p50_endpoint.h"
#include "../../services/digest128.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_future.hpp>

#include <cstdint>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using namespace icecc::p50;

namespace {

// SidecarRuntime's production route owner pins the current P50 source path to
// level 3.  Exact conformance must reproduce that product configuration, not
// P50PreparationAuthority's older level-1 unit-test default.
constexpr int kCurrentProductCompressionLevel = 3;

std::vector<uint8_t> read_bytes(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open input manifest payload");
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0)
        throw std::runtime_error("cannot determine input manifest payload size");
    input.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()), size))
        throw std::runtime_error("cannot read input manifest payload");
    return bytes;
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
           << "\"raw_digest\":\"" << icecc::digest128_hex(icecc::digest128(input))
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
};

ProfileId selected_profile() {
    const char* requested = std::getenv("ICECC_P50_PROFILE");
    if (requested == nullptr || *requested == '\0')
        return ProfileId::ZSTD_TU;
    const std::string value(requested);
    if (value == "P29")
        return ProfileId::P29;
    if (value == "ZSTD_TU")
        return ProfileId::ZSTD_TU;
    if (value == "ZSTD_ROUTE")
        return ProfileId::Z3_LONG;
    throw std::invalid_argument("unsupported ICECC_P50_PROFILE: " + value);
}

Arguments parse(int argc, char** argv) {
    Arguments result;
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
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
        }
        else
            throw std::invalid_argument("unknown option " + option);
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

}  // namespace

int main(int argc, char** argv) {
    try {
        const Arguments arguments = parse(argc, argv);
        const bool warm = !arguments.prewarm_input.empty();
        const std::vector<uint8_t> input = warm
            ? read_bytes(arguments.measured_input)
            : read_bytes(arguments.input);

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
        const PreparedTuHandle prewarm_prepared = warm
            ? authority->prepare(PrepareRequestKey{1, 1}, prewarm_input)
            : PreparedTuHandle{};
        const PreparedTuHandle prepared = authority->prepare(
            warm ? PrepareRequestKey{1, 2} : PrepareRequestKey{1, 1}, input);
        if ((warm && !prewarm_prepared) || !prepared)
            throw std::runtime_error("Protocol-50 preparation returned an invalid handle");

        P50ServerEndpointConfig config;
        config.input_job_state = [](CStoreGuid, const TxBegin&, const TxCommit&,
                                    std::span<const uint8_t>) { return InputJobState::Open; };
        P50ServerEndpoint server(arguments.f_store_guid, caps,
                                 &completions, &actions, std::move(config));
        P50ClientEndpoint client(authority, caps, arguments.history_nonce, &completions, &actions);

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
        write_summary(arguments.summary, input, client_result, server_result,
                      completions, actions, caps.profile);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "p50sim: " << error.what() << '\n';
        return 1;
    }
}
