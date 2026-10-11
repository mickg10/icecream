#include "cache/p50_endpoint.h"
#include "cache/p50_slice0.h"

#include <zstd.h>

#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <future>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <sstream>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using namespace icecc::p50;

[[noreturn]] void fail(std::string_view detail) {
    std::cerr << "p50_endpoint_test: " << detail << '\n';
    std::exit(1);
}

void require(bool value, std::string_view detail) {
    if (!value)
        fail(detail);
}

void test_action_hold_rendezvous() {
    char root_template[] = "/tmp/p50-action-hold-XXXXXX";
    char* root_value = ::mkdtemp(root_template);
    require(root_value != nullptr, "cannot create action-hold test directory");
    const std::filesystem::path root(root_value);
    const std::filesystem::path marker = root / "marker";
    const std::filesystem::path release = root / "release";
    const std::filesystem::path trace_path = root / "f-action-trace.jsonl";

    require(::setenv("ICECC_P50_TEST_ACTION_HOLD", "F:TX_BEGIN", 1) == 0 &&
                ::setenv("ICECC_P50_TEST_ACTION_HOLD_MARKER",
                         marker.c_str(), 1) == 0 &&
                ::setenv("ICECC_P50_TEST_ACTION_HOLD_RELEASE",
                         release.c_str(), 1) == 0 &&
                ::setenv("ICECC_P50_TEST_ACTION_HOLD_TIMEOUT_MS", "5000", 1) == 0 &&
                ::setenv("ICECC_P50_F_ACTION_TRACE", trace_path.c_str(), 1) == 0,
            "cannot configure action-hold rendezvous");

    std::atomic<bool> marker_observed{false};
    std::thread releaser([&] {
        for (size_t attempt = 0; attempt != 500; ++attempt) {
            std::ifstream input(marker);
            if (input) {
                std::string header;
                std::getline(input, header);
                std::string json;
                std::getline(input, json);
                std::ifstream trace_input(trace_path);
                std::string trace;
                std::getline(trace_input, trace);
                marker_observed = header.starts_with("P50_ACTION_HOLD pid=") &&
                                  header.find(" actor=F action=TX_BEGIN") !=
                                      std::string::npos &&
                                  json.starts_with(
                                      "{\"action\":\"TX_BEGIN\",\"actor\":\"F\",") &&
                                  trace == json;
                std::ofstream output(release);
                output << "release\n";
                output.close();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    ActionRecord record;
    record.actor = ActorSide::F;
    record.action = ActionType::TX_BEGIN;
    record.c_store_guid = Id128::from_u64(991);
    record.f_store_guid = Id128::from_u64(992);
    record.history_nonce = HistoryNonce{1};
    record.rel_seq = RelSeq{2};
    record.tu_seq = TuSeq{3};
    ActionTrace trace;
    trace.record(record);
    releaser.join();
    require(trace.valid() && marker_observed,
            "named action did not complete its exact rendezvous");

    // The marker is the one-shot fence inherited by a replacement sidecar.
    // A second matching action must not wait again.
    std::filesystem::remove(release);
    const auto second_start = std::chrono::steady_clock::now();
    trace.record(record);
    require(trace.valid() &&
                std::chrono::steady_clock::now() - second_start <
                    std::chrono::milliseconds(250),
            "one-shot action rendezvous blocked a replacement");

    const std::filesystem::path stale_marker = root / "stale-marker";
    {
        std::ofstream output(stale_marker);
        output << "incomplete\n";
    }
    require(::setenv("ICECC_P50_TEST_ACTION_HOLD_MARKER",
                     stale_marker.c_str(), 1) == 0,
            "cannot configure stale action-hold marker");
    ActionTrace stale;
    stale.record(record);
    require(!stale.valid(), "incomplete action-hold marker was accepted");

    const std::filesystem::path bad_marker = root / "bad-marker";
    const std::filesystem::path bad_release = root / "bad-release";
    std::filesystem::create_symlink(bad_marker, bad_release);
    require(::setenv("ICECC_P50_TEST_ACTION_HOLD_MARKER",
                     bad_marker.c_str(), 1) == 0 &&
                ::setenv("ICECC_P50_TEST_ACTION_HOLD_RELEASE",
                         bad_release.c_str(), 1) == 0,
            "cannot configure invalid action-hold release");
    ActionTrace invalid;
    invalid.record(record);
    require(!invalid.valid(), "symlink action-hold release was accepted");

    ::unsetenv("ICECC_P50_TEST_ACTION_HOLD");
    ::unsetenv("ICECC_P50_TEST_ACTION_HOLD_MARKER");
    ::unsetenv("ICECC_P50_TEST_ACTION_HOLD_RELEASE");
    ::unsetenv("ICECC_P50_TEST_ACTION_HOLD_TIMEOUT_MS");
    ::unsetenv("ICECC_P50_F_ACTION_TRACE");
    std::filesystem::remove_all(root);
}

template <class Exception, class Function>
void require_throws(Function&& function, std::string_view detail) {
    try {
        std::forward<Function>(function)();
    } catch (const Exception&) {
        return;
    } catch (...) {
        fail(std::string(detail) + " (wrong exception type)");
    }
    fail(std::string(detail) + " (no exception)");
}

std::vector<uint8_t> bytes(std::string_view text) { return {text.begin(), text.end()}; }

struct P5coStoreGuids {
    CStoreGuid c;
    FStoreGuid f;
};

P5coStoreGuids p5co_store_guids(uint8_t seed) {
    std::array<uint8_t, 16> client{};
    std::array<uint8_t, 16> file{};
    for (size_t index = 0; index != client.size(); ++index) {
        client[index] = static_cast<uint8_t>(seed + index);
        file[index] = static_cast<uint8_t>(seed + 41 + index);
    }
    client[kStoreIdentityRoleByte] &=
        static_cast<uint8_t>(~kStoreIdentityRoleMask);
    file[kStoreIdentityRoleByte] |= kStoreIdentityRoleMask;
    require(store_identity_guid_valid_for_role(client,
                                               kStoreIdentityClientRole) &&
                store_identity_guid_valid_for_role(file,
                                                   kStoreIdentityFileRole) &&
                !store_identity_file_guid_matches_client(client, file),
            "P5CO test StoreGuid identities are invalid or aliased");
    return {CStoreGuid{client}, FStoreGuid{file}};
}

struct TestClient {
    explicit TestClient(CStoreGuid c_store_guid, EndpointCaps caps = {},
                        HistoryNonce first_history_nonce = HistoryNonce{1},
                        CompletionLog* completions = nullptr, ActionTrace* actions = nullptr,
                        PreparationAuthorityLimits authority_limits = {},
                        TuSeq first_tu_seq = {})
        : authority(std::make_shared<P50PreparationAuthority>(
              c_store_guid, caps.zstd, authority_limits, 1,
              caps.profile, first_tu_seq)),
          endpoint(authority, caps, first_history_nonce, completions, actions) {}

    operator P50ClientEndpoint&() { return endpoint; }

    PreparedTuHandle prepare(PrepareRequestKey request, std::span<const uint8_t> input) {
        return authority->prepare(request, input);
    }

    [[nodiscard]] CStoreGuid c_store_guid() const { return endpoint.c_store_guid(); }
    [[nodiscard]] bool has_active_transaction() const {
        return endpoint.has_active_transaction();
    }
    [[nodiscard]] bool has_reconciliation_work() const {
        return endpoint.has_reconciliation_work();
    }

    std::shared_ptr<P50PreparationAuthority> authority;
    P50ClientEndpoint endpoint;
};

PreparedTuHandle admit(TestClient& client, std::span<const uint8_t> input) {
    static uint64_t next_request_token = 1;
    return client.prepare(PrepareRequestKey{1, next_request_token++}, input);
}

std::vector<uint8_t> pseudo_random_bytes(size_t count) {
    std::vector<uint8_t> result(count);
    uint32_t state = 0x6d2b79f5U;
    for (uint8_t& byte : result) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        byte = static_cast<uint8_t>(state);
    }
    return result;
}

std::vector<uint8_t> drain_input(InputCursor& cursor, size_t chunk_size = 113) {
    std::vector<uint8_t> result;
    std::vector<uint8_t> chunk(chunk_size);
    while (!cursor.eof()) {
        const size_t count = cursor.read(chunk);
        require(count != 0, "non-EOF InputRecord cursor made no progress");
        result.insert(result.end(), chunk.begin(), chunk.begin() + count);
    }
    return result;
}

std::optional<std::vector<uint8_t>> copy_input(P50ServerEndpoint& server,
                                                CStoreGuid c_store_guid) {
    const std::optional<InputRecordKey> key =
        server.last_committed_input(c_store_guid);
    if (!key)
        return std::nullopt;
    try {
        InputCursor cursor = server.attach_input(*key);
        return drain_input(cursor);
    } catch (const std::logic_error&) {
        return std::nullopt;
    }
}

std::vector<uint8_t> standalone_zstd_frame(std::span<const uint8_t> input) {
    std::vector<uint8_t> result(ZSTD_compressBound(input.size()));
    const size_t encoded =
        ZSTD_compress(result.data(), result.size(), input.data(), input.size(), 1);
    if (ZSTD_isError(encoded))
        fail(std::string("standalone zstd fixture failed: ") + ZSTD_getErrorName(encoded));
    result.resize(encoded);
    return result;
}

bool nonzero(Digest128 digest) {
    return std::any_of(digest.bytes.begin(), digest.bytes.end(),
                       [](uint8_t byte) { return byte != 0; });
}

struct PairResult {
    ClientRunResult client;
    ServerRunResult server;
};

asio::awaitable<void> raw_stall_after_connect(tcp::acceptor& acceptor);

struct CompetingPairResult {
    ClientRunResult first_client;
    ClientRunResult second_client;
    ServerRunResult first_server;
    ServerRunResult second_server;
    P50ServerOwnerUsage usage;
};

PairResult run_pair(P50ClientEndpoint& client, P50ServerEndpoint& server,
                    PreparedTuHandle prepared = {}, EndpointIoControl client_control = {},
                    EndpointIoControl server_control = {}) {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    std::future<ServerRunResult> server_result =
        asio::co_spawn(context, server.accept_one(acceptor, server_control), asio::use_future);
    std::future<ClientRunResult> client_result = asio::co_spawn(
        context, client.run(acceptor.local_endpoint(), prepared, client_control),
        asio::use_future);
    context.run();
    return {client_result.get(), server_result.get()};
}

size_t staging_file_count();
uint64_t resident_bytes();
void report_resource_checkpoint(std::string_view label,
                                const P50ServerOwnerUsage& usage);

void test_zstd_route_endpoint_continuation_and_retry() {
    const P5coStoreGuids guids = p5co_store_guids(211);
    EndpointCaps caps;
    caps.profile = ProfileId::ZSTD_ROUTE;
    caps.zstd.max_raw_bytes = 1U << 20;
    caps.zstd.max_encoded_body_bytes = 1U << 20;
    P50ServerEndpoint server(guids.f, caps);
    TestClient client(guids.c, caps);
    const std::vector<uint8_t> first(64 * 1024, 0x41);
    const std::vector<uint8_t> second(64 * 1024, 0x41);
    const PreparedTuHandle first_prepared = admit(client, first);
    require_throws<std::logic_error>(
        [&] { (void)admit(client, second); },
        "ZSTD_ROUTE prepared a successor before its predecessor committed");
    const PairResult first_pair = run_pair(client, server, first_prepared);
    require(first_pair.client.status == ClientRunStatus::Committed &&
                first_pair.server.status == ServerRunStatus::Completed &&
                copy_input(server, guids.c) == first,
            "ZSTD_ROUTE first endpoint TU did not commit exact bytes");

    const PreparedTuHandle second_prepared = admit(client, second);
    const PairResult second_pair = run_pair(client, server, second_prepared);
    require(second_pair.client.status == ClientRunStatus::Committed &&
                second_pair.server.status == ServerRunStatus::Completed &&
                copy_input(server, guids.c) == second,
            "ZSTD_ROUTE second endpoint TU did not use the live route");

    const std::vector<uint8_t> failed(64 * 1024, 0x42);
    const PreparedTuHandle failed_prepared = admit(client, failed);
    EndpointIoControl disconnect;
    disconnect.close_before_write = MessageType::BODY;
    const PairResult disconnected =
        run_pair(client, server, failed_prepared, disconnect);
    require(disconnected.client.status == ClientRunStatus::Disconnected &&
                disconnected.server.status == ServerRunStatus::Disconnected &&
                client.has_active_transaction() &&
                client.endpoint.next_rel_seq().value == 2,
            "ZSTD_ROUTE failed TU did not retain exact retry identity");
    const PairResult retried = run_pair(client, server, failed_prepared);
    require(retried.client.status == ClientRunStatus::Committed &&
                retried.server.status == ServerRunStatus::Completed &&
                copy_input(server, guids.c) == failed,
            "ZSTD_ROUTE retry did not discard tentative state and commit exact bytes");
}

void test_zstd_route_authority_bounded_history() {
    EndpointCaps caps;
    caps.profile = ProfileId::ZSTD_ROUTE;
    caps.zstd.max_raw_bytes = 2U << 20;
    caps.zstd.max_encoded_body_bytes = 2U << 20;
    caps.zstd.max_window_log = 12;
    caps.zstd.max_history_bytes = 4U << 10;
    PreparationAuthorityLimits authority_limits;
    authority_limits.max_live_entries = 1;
    authority_limits.max_retained_encoded_bytes = 8U << 20;
    P50PreparationAuthority authority(Id128::from_u64(18001), caps.zstd,
                                      authority_limits, 3, ProfileId::ZSTD_ROUTE);

    for (uint64_t index = 0; index != 129; ++index) {
        const std::vector<uint8_t> input(1U << 20,
                                         static_cast<uint8_t>('A' + index % 17));
        const PreparedTuHandle handle =
            authority.prepare(PrepareRequestKey{180, index + 1}, input);
        require(authority.live_entry_count() == 1,
                "route authority did not retain its one active preparation");
        authority.commit(handle);
        require(authority.route_history_bytes() <= caps.zstd.max_history_bytes &&
                    authority.route_history_entries() <= 1,
                "route authority retained cumulative predecessor state");
        require(authority.release(handle) == 0 && authority.live_entry_count() == 0 &&
                    authority.retained_encoded_bytes() == 0,
                "route authority did not release the committed preparation");
    }
}

void test_server_routes_are_isolated_by_profile() {
    const P5coStoreGuids guids = p5co_store_guids(212);
    EndpointCaps server_caps;
    server_caps.supported_profiles = kOperationalProfileMask;
    ActionTrace actions;
    P50ServerEndpoint server(guids.f, server_caps, nullptr, &actions);

    EndpointCaps p29_caps;
    p29_caps.profile = ProfileId::P29V1;
    EndpointCaps zstd_caps;
    zstd_caps.profile = ProfileId::ZSTD_TU;
    TestClient p29(guids.c, p29_caps, HistoryNonce{1}, nullptr, &actions);
    // Both clients deliberately represent the same C store.  Give the second
    // preparation authority a disjoint C-wide TU sequence so this test probes
    // route identity rather than duplicate input-record admission.
    TestClient zstd(guids.c, zstd_caps, HistoryNonce{100}, nullptr, &actions,
                    {}, TuSeq{100});

    const PairResult first_p29 =
        run_pair(p29, server, admit(p29, bytes("profile-isolated p29 first\n")));
    const PairResult first_zstd =
        run_pair(zstd, server, admit(zstd, bytes("profile-isolated zstd\n")));
    const PairResult second_p29 =
        run_pair(p29, server, admit(p29, bytes("profile-isolated p29 second\n")));

    require(first_p29.client.status == ClientRunStatus::Committed &&
                first_p29.server.status == ServerRunStatus::Completed &&
                first_zstd.client.status == ClientRunStatus::Committed &&
                first_zstd.server.status == ServerRunStatus::Completed &&
                second_p29.client.status == ClientRunStatus::Committed &&
                second_p29.server.status == ServerRunStatus::Completed &&
                second_p29.client.reconnect ==
                    EndpointReconnectOutcome::ExactMatch,
            "a second profile replaced the retained route for the first profile");
    if (const auto error = check_action_trace(actions.records()))
        fail("P29V1/ZSTD_TU/P29V1 profile-isolation trace: " + *error);
    const auto zstd_action = std::find_if(
        actions.records().begin(), actions.records().end(),
        [](const ActionRecord& record) {
            return record.profile == ProfileId::ZSTD_TU;
        });
    require(zstd_action != actions.records().end(),
            "mixed-profile action trace omitted every ZSTD_TU record");
    const std::string zstd_json = zstd_action == actions.records().end()
        ? std::string() : action_jsonl(*zstd_action);
    require(zstd_json.find("\"profile\":\"zstd_tu\"") != std::string::npos,
            "action JSON omitted the exact route profile");
}

void test_p29v1_endpoint_route_dialogue_lifetime() {
    const P5coStoreGuids guids = p5co_store_guids(230);
    EndpointCaps caps;
    caps.profile = ProfileId::P29V1;
    caps.zstd.max_raw_bytes = 1U << 20;
    caps.zstd.max_encoded_body_bytes = 1U << 20;
    GlobalResourceTrace global_trace;
    P50ServerEndpointConfig config;
    config.global_resource_trace = &global_trace;
    P50ServerEndpoint server(guids.f, caps, nullptr, nullptr,
                             std::move(config));
    PreparationAuthorityLimits authority_limits;
    authority_limits.max_interner_reserved_bytes = UINT64_C(1044398080);
    TestClient client(guids.c, caps, HistoryNonce{1}, nullptr, nullptr,
                      authority_limits);
    const std::vector<uint8_t> repeated = bytes(
        "# 1 \"/tmp/p29v1-fixture.cc\"\n"
        "same-line\nsame-line\nsame-line\nsame-line\ntail\n");

    const PairResult first = run_pair(client, server, admit(client, repeated));
    const std::optional<std::vector<uint8_t>> first_input =
        copy_input(server, guids.c);
    if (first.client.status != ClientRunStatus::Committed ||
        first.server.status != ServerRunStatus::Completed ||
        first_input != repeated || client.endpoint.next_rel_seq().value != 1) {
        std::cerr << "P29V1 first diagnostic: client_status="
                  << static_cast<unsigned>(first.client.status)
                  << " server_status="
                  << static_cast<unsigned>(first.server.status)
                  << " exact=" << (first_input == repeated)
                  << " next_rel=" << client.endpoint.next_rel_seq().value;
        if (first.client.terminal_error)
            std::cerr << " client_error=" << first.client.terminal_error->detail;
        if (first.server.terminal_error)
            std::cerr << " server_error=" << first.server.terminal_error->detail;
        std::cerr << '\n';
    }
    require(first.client.status == ClientRunStatus::Committed &&
                first.server.status == ServerRunStatus::Completed &&
                first_input == repeated &&
                client.endpoint.next_rel_seq().value == 1,
            "P29V1 endpoint first TU did not commit exact route state");
    std::optional<size_t> segment_present;
    std::optional<size_t> blob_present;
    Key64 segment_key;
    Key64 blob_key;
    for (size_t index = 0; index < global_trace.records().size(); ++index) {
        const GlobalActionRecord& record = global_trace.records()[index];
        if (record.action != GlobalActionType::ARENA_PRESENT)
            continue;
        if (record.key.type() == ObjectType::P29Segment) {
            segment_present = index;
            segment_key = record.key;
        } else if (record.key.type() == ObjectType::Blob) {
            blob_present = index;
            blob_key = record.key;
        }
    }
    require(segment_present && blob_present &&
                *segment_present < *blob_present &&
                segment_key.generation() == blob_key.generation() &&
                segment_key.ordinal() == blob_key.ordinal(),
            "P29V1 did not publish its same-TU segment before the input Blob");

    require(first.server.committed_input.has_value(),
            "P29V1 open job did not expose its committed input lease");
    server.close_input_job(*first.server.committed_input);
    server.collect_input_garbage();
    require(server.owner_usage().retained_input_records == 0 &&
                server.owner_usage().retained_input_bytes == 0 &&
                !server.global_resource_invariant_for_test(),
            "P29V1 Blob collection rejected its surviving route segment");
    const auto first_segment_release = std::find_if(
        global_trace.records().begin(), global_trace.records().end(),
        [segment_key](const GlobalActionRecord& record) {
            return record.action == GlobalActionType::ARENA_RELEASED &&
                   record.key == segment_key;
        });
    require(first_segment_release == global_trace.records().end(),
            "P29V1 Blob collection prematurely released route dictionary state");

    const PairResult second = run_pair(client, server, admit(client, repeated));
    require(second.client.status == ClientRunStatus::Committed &&
                second.server.status == ServerRunStatus::Completed &&
                copy_input(server, guids.c) == repeated &&
                client.endpoint.next_rel_seq().value == 2,
            "P29V1 endpoint same-route TU2 lost the committed codec state");

    const std::vector<uint8_t> retry_input = bytes(
        "# 1 \"/tmp/p29v1-retry.cc\"\nretry-v1\nretry-v1\n");
    const PreparedTuHandle retry_handle = admit(client, retry_input);
    EndpointIoControl disconnect;
    disconnect.close_before_write = MessageType::FILL;
    const PairResult interrupted =
        run_pair(client, server, retry_handle, disconnect);
    require(interrupted.client.status == ClientRunStatus::Disconnected &&
                interrupted.server.status == ServerRunStatus::Disconnected &&
                client.has_active_transaction() &&
                client.endpoint.next_rel_seq().value == 2,
            "P29V1 interrupted FILL did not retain exact retry identity");
    const PairResult retried = run_pair(client, server, retry_handle);
    const std::optional<std::vector<uint8_t>> retried_input =
        copy_input(server, guids.c);
    if (retried.client.status != ClientRunStatus::Committed ||
        retried.server.status != ServerRunStatus::Completed ||
        retried_input != retry_input ||
        client.endpoint.next_rel_seq().value != 3) {
        std::cerr << "P29V1 retry diagnostic: client_status="
                  << static_cast<unsigned>(retried.client.status)
                  << " server_status="
                  << static_cast<unsigned>(retried.server.status)
                  << " exact=" << (retried_input == retry_input)
                  << " next_rel=" << client.endpoint.next_rel_seq().value;
        if (retried.client.terminal_error)
            std::cerr << " client_error="
                      << retried.client.terminal_error->detail;
        if (retried.server.terminal_error)
            std::cerr << " server_error="
                      << retried.server.terminal_error->detail;
        std::cerr << '\n';
    }
    require(retried.client.status == ClientRunStatus::Committed &&
                retried.server.status == ServerRunStatus::Completed &&
                retried_input == retry_input &&
                client.endpoint.next_rel_seq().value == 3,
            "P29V1 exact retry did not reproduce and commit the input");

    server.reset_store(
        FStoreGuid::from_u64(UINT64_C(0x5032395631525354)));
    const std::vector<uint8_t> after_reset = bytes("after-p29v1-reset\n");
    const PairResult reset =
        run_pair(client, server, admit(client, after_reset));
    const std::optional<std::vector<uint8_t>> reset_input =
        copy_input(server, guids.c);
    if (reset.client.status != ClientRunStatus::Committed ||
        reset.client.reconnect != EndpointReconnectOutcome::ColdFStore ||
        reset.server.status != ServerRunStatus::Completed ||
        reset_input != after_reset || client.endpoint.next_rel_seq().value != 1) {
        std::cerr << "P29V1 reset diagnostic: client_status="
                  << static_cast<unsigned>(reset.client.status)
                  << " reconnect="
                  << static_cast<unsigned>(reset.client.reconnect)
                  << " server_status="
                  << static_cast<unsigned>(reset.server.status)
                  << " exact=" << (reset_input == after_reset)
                  << " next_rel=" << client.endpoint.next_rel_seq().value;
        if (reset.client.terminal_error)
            std::cerr << " client_error=" << reset.client.terminal_error->detail;
        if (reset.server.terminal_error)
            std::cerr << " server_error=" << reset.server.terminal_error->detail;
        std::cerr << '\n';
    }
    require(reset.client.status == ClientRunStatus::Committed &&
                reset.client.reconnect == EndpointReconnectOutcome::ColdFStore &&
                reset.server.status == ServerRunStatus::Completed &&
                reset_input == after_reset &&
                client.endpoint.next_rel_seq().value == 1,
            "P29V1 cold-store reset did not rebuild both codec routes");

    GlobalResourceTrace closed_trace;
    P50ServerEndpointConfig closed_config;
    closed_config.global_resource_trace = &closed_trace;
    closed_config.input_job_state =
        [](CStoreGuid, const TxBegin&, const TxCommit&,
           std::span<const uint8_t>) { return InputJobState::Closed; };
    const P5coStoreGuids closed_guids = p5co_store_guids(231);
    P50ServerEndpoint closed_server(closed_guids.f, caps, nullptr, nullptr,
                                    std::move(closed_config));
    TestClient closed_client(closed_guids.c, caps, HistoryNonce{1}, nullptr,
                             nullptr, authority_limits);
    const PairResult closed = run_pair(
        closed_client, closed_server, admit(closed_client, repeated));
    require(closed.client.status == ClientRunStatus::Committed &&
                closed.server.status == ServerRunStatus::Completed &&
                closed.server.completed_input.has_value() &&
                !closed.server.committed_input &&
                closed_server.owner_usage().retained_input_records == 0 &&
                !closed_server.global_resource_invariant_for_test(),
            "P29V1 closed job did not commit without a resident Blob");
    const auto closed_segment_present = std::find_if(
        closed_trace.records().begin(), closed_trace.records().end(),
        [](const GlobalActionRecord& record) {
            return record.action == GlobalActionType::ARENA_PRESENT &&
                   record.key.type() == ObjectType::P29Segment;
        });
    const auto closed_blob_crashed = std::find_if(
        closed_trace.records().begin(), closed_trace.records().end(),
        [](const GlobalActionRecord& record) {
            return record.action == GlobalActionType::INSTALL_CRASHED &&
                   record.key.type() == ObjectType::Blob;
        });
    const auto closed_blob_present = std::find_if(
        closed_trace.records().begin(), closed_trace.records().end(),
        [](const GlobalActionRecord& record) {
            return record.action == GlobalActionType::ARENA_PRESENT &&
                   record.key.type() == ObjectType::Blob;
        });
    require(closed_segment_present != closed_trace.records().end() &&
                closed_blob_crashed != closed_trace.records().end() &&
                closed_blob_present == closed_trace.records().end(),
            "P29V1 closed job did not publish Segment and crash Blob");
    closed_server.collect_input_garbage();
    require(!closed_server.global_resource_invariant_for_test(),
            "P29V1 closed-job garbage collection broke global invariants");

    GlobalResourceTrace capped_trace;
    P50ServerEndpointConfig capped_config;
    capped_config.global_resource_trace = &capped_trace;
    const P5coStoreGuids capped_guids = p5co_store_guids(232);
    P50ServerEndpoint capped_server(capped_guids.f, caps, nullptr, nullptr,
                                    std::move(capped_config));
    PreparationAuthorityLimits capped_limits = authority_limits;
    capped_limits.max_route_state_bytes = 1;
    TestClient capped_client(capped_guids.c, caps, HistoryNonce{1}, nullptr,
                             nullptr, capped_limits);
    const PairResult capped = run_pair(
        capped_client, capped_server, admit(capped_client, repeated));
    require(capped.client.status == ClientRunStatus::TerminalError &&
                capped.server.status == ServerRunStatus::Disconnected &&
                capped.client.terminal_error.has_value() &&
                capped.client.terminal_error->detail.find(
                    "P29V1 route state exceeds its budget") !=
                    std::string::npos &&
                capped_server.owner_usage().retained_input_records == 0 &&
                !capped_server.global_resource_invariant_for_test(),
            "P29V1 projected route cap did not reject before FILL publication");
    const bool capped_published = std::any_of(
        capped_trace.records().begin(), capped_trace.records().end(),
        [](const GlobalActionRecord& record) {
            return record.action == GlobalActionType::ARENA_PRESENT;
        });
    require(!capped_published,
            "P29V1 projected route-cap failure published receiver state");
}

void test_p29v1_pipelined_begin() {
    const P5coStoreGuids guids = p5co_store_guids(233);
    EndpointCaps caps;
    caps.profile = ProfileId::P29V1;
    caps.zstd.max_raw_bytes = 1U << 20;
    caps.zstd.max_encoded_body_bytes = 1U << 20;
    ActionTrace actions;
    P50ServerEndpoint server(guids.f, caps, nullptr, &actions);
    PreparationAuthorityLimits authority_limits;
    authority_limits.max_interner_reserved_bytes = UINT64_C(1044398080);
    CompletionLog completions;
    TestClient client(guids.c, caps, HistoryNonce{1}, &completions, &actions,
                      authority_limits);
    client.endpoint.enable_pipelined_begin();
    for (int run = 0; run != 3; ++run) {
        const std::vector<uint8_t> input = bytes(
            "# 1 \"/tmp/p29v1-pipelined.cc\"\nsame-line\nsame-line\nrun-" +
            std::to_string(run) + "\n");
        const PairResult pair = run_pair(client, server, admit(client, input));
        require(pair.client.status == ClientRunStatus::Committed &&
                    pair.server.status == ServerRunStatus::Completed &&
                    copy_input(server, guids.c) == input,
                "pipelined P29V1 TU did not commit exact bytes");
    }
    if (const auto error = check_action_trace(actions.records()))
        fail("pipelined P29V1 trace: " + *error);

    // C writes before its first read, per session in run order.
    std::vector<std::pair<uint64_t, size_t>> writes_before_read;
    std::set<uint64_t> read_seen;
    for (const AsyncCompletion& completion : completions.completions()) {
        if (completion.stamp.actor != ActorSide::C)
            continue;
        const uint64_t serial = completion.stamp.session_serial;
        if (writes_before_read.empty() || writes_before_read.back().first != serial)
            writes_before_read.emplace_back(serial, 0);
        if (completion.stamp.operation == AsyncOperationKind::ReadHeader)
            read_seen.insert(serial);
        else if (completion.stamp.operation == AsyncOperationKind::WriteFragment &&
                 !read_seen.contains(serial))
            ++writes_before_read.back().second;
    }
    require(writes_before_read.size() == 3 && writes_before_read[0].second < 3 &&
                writes_before_read[1].second >= 4 &&
                writes_before_read[2].second >= 4,
            "retained P29V1 route did not send TX_BEGIN, BODY and FILL before STATE");

    // Another F incarnation behind the same address must fail closed.
    const P5coStoreGuids other = p5co_store_guids(234);
    P50ServerEndpoint other_server(other.f, caps);
    const std::vector<uint8_t> refused = bytes("# 1 \"/tmp/p29v1-other.cc\"\nother\n");
    const PairResult mismatch = run_pair(client, other_server, admit(client, refused));
    require(mismatch.client.status == ClientRunStatus::TerminalError &&
                mismatch.server.status != ServerRunStatus::Completed &&
                !other_server.last_committed_input(guids.c),
            "pipelined TX_BEGIN committed on a different F");

    // The same F dropped the route to make room for another C: the early
    // begin is aborted, and the retry resets the route from STATE.
    ActionTrace evict_actions;
    P50ServerEndpointConfig evicting;
    evicting.owner_limits.max_retained_input_records = 1;
    const P5coStoreGuids evict = p5co_store_guids(235);
    P50ServerEndpoint small(evict.f, caps, nullptr, &evict_actions,
                            std::move(evicting));
    TestClient first(evict.c, caps, HistoryNonce{1}, nullptr, &evict_actions,
                     authority_limits);
    TestClient second(p5co_store_guids(236).c, caps, HistoryNonce{1}, nullptr,
                      &evict_actions, authority_limits);
    first.endpoint.enable_pipelined_begin();
    const PairResult warm =
        run_pair(first, small, admit(first, bytes("# 1 \"/tmp/a.cc\"\nfirst\n")));
    require(warm.server.committed_input.has_value(), "eviction fixture did not commit");
    small.close_input_job(*warm.server.committed_input);
    const PairResult displacing =
        run_pair(second, small, admit(second, bytes("# 1 \"/tmp/b.cc\"\nsecond\n")));
    require(displacing.server.committed_input.has_value(),
            "displacing namespace did not commit");
    small.close_input_job(*displacing.server.committed_input);
    const PreparedTuHandle next = admit(first, bytes("# 1 \"/tmp/a.cc\"\nagain\n"));
    const PairResult aborted = run_pair(first, small, next);
    const PairResult reset = run_pair(first, small, next);
    require(warm.client.status == ClientRunStatus::Committed &&
                displacing.client.status == ClientRunStatus::Committed &&
                aborted.client.status == ClientRunStatus::Disconnected &&
                reset.client.status == ClientRunStatus::Committed &&
                reset.client.reconnect == EndpointReconnectOutcome::RouteHistoryReset &&
                copy_input(small, evict.c) == bytes("# 1 \"/tmp/a.cc\"\nagain\n"),
            "pipelined TX_BEGIN on an evicted route did not recover by reset");
    if (const auto error = check_action_trace(evict_actions.records()))
        fail("pipelined abort trace: " + *error);
}

// P50 route pipelining (research/designs/P50_ROUTE_PIPELINING.md), step 5a.
//
// F has no awareness of K (ICECC_P50_ROUTE_PIPELINE_DEPTH) at all: the
// per-route sequencer added in step 5c only ever parks a TX_BEGIN that
// arrives strictly ahead of route.next_rel, which is structurally
// impossible at K=1 (one outstanding TX per route means every BEGIN F ever
// observes is already at-cursor). This test pins the K=1 observable shape
// -- route.next_rel advancing by exactly one per committed TX, and F never
// deferring its first write past its first read on any session -- for both
// an ordinary client and a client with the pre-existing
// enable_pipelined_begin() feature enabled (which can legitimately make C
// write BEGIN/BODY/FILL before reading F's STATE even at K=1, exercising
// the same "something is already in the socket buffer" condition the
// step-5c peek relies on, without ever requiring F to park). It must stay
// green, byte-for-byte in these observable terms, across every later
// step-5b/5c/5d change.
std::vector<uint64_t> run_route_pipeline_k1_sequence(bool use_pipelined_begin) {
    const P5coStoreGuids guids = p5co_store_guids(use_pipelined_begin ? 701 : 700);
    EndpointCaps caps;
    caps.profile = ProfileId::P29V1;
    caps.zstd.max_raw_bytes = 1U << 20;
    caps.zstd.max_encoded_body_bytes = 1U << 20;
    ActionTrace actions;
    CompletionLog completions;
    P50ServerEndpoint server(guids.f, caps, &completions, &actions);
    PreparationAuthorityLimits authority_limits;
    authority_limits.max_interner_reserved_bytes = UINT64_C(1044398080);
    TestClient client(guids.c, caps, HistoryNonce{1}, &completions, &actions,
                      authority_limits);
    if (use_pipelined_begin)
        client.endpoint.enable_pipelined_begin();
    for (int run = 0; run != 3; ++run) {
        const std::vector<uint8_t> input = bytes(
            "# 1 \"/tmp/p50-pipeline-k1.cc\"\nsame-line\nsame-line\nrun-" +
            std::to_string(run) + "\n");
        const PairResult pair = run_pair(client, server, admit(client, input));
        require(pair.client.status == ClientRunStatus::Committed &&
                    pair.server.status == ServerRunStatus::Completed &&
                    copy_input(server, guids.c) == input,
                "k1 identity: TU did not commit exact bytes");
    }
    if (const auto error = check_action_trace(actions.records()))
        fail("k1 identity: " + *error);

    // route.next_rel must advance 0, 1, 2 in strict order: no gap, repeat,
    // or reorder, i.e. nothing was ever held back waiting on a successor
    // that cannot exist at K=1.
    std::vector<uint64_t> f_begin_rel_seq;
    for (const ActionRecord& record : actions.records()) {
        if (record.actor == ActorSide::F && record.c_store_guid == guids.c &&
            (record.action == ActionType::TX_BEGIN ||
             record.action == ActionType::ACTIVE_REPLAYED))
            f_begin_rel_seq.push_back(record.rel_seq.value);
    }
    require(f_begin_rel_seq == std::vector<uint64_t>{0, 1, 2},
            "k1 identity: F route cursor did not advance 0,1,2 in order");

    // F's first write on a session is never recorded ahead of its first
    // read on that same session (SESSION_HELLO always precedes SESSION_STATE).
    std::set<uint64_t> f_sessions_seen;
    std::set<uint64_t> f_read_before_first_write;
    for (const AsyncCompletion& completion : completions.completions()) {
        if (completion.stamp.actor != ActorSide::F)
            continue;
        const uint64_t serial = completion.stamp.session_serial;
        if (completion.stamp.operation == AsyncOperationKind::ReadHeader)
            f_sessions_seen.insert(serial);
        else if (completion.stamp.operation == AsyncOperationKind::WriteFragment &&
                 f_sessions_seen.contains(serial))
            f_read_before_first_write.insert(serial);
    }
    require(f_sessions_seen.size() == 3 &&
                f_read_before_first_write == f_sessions_seen,
            "k1 identity: F wrote on a session before reading on it");
    return f_begin_rel_seq;
}

void test_route_pipeline_k1_identity() {
    const std::vector<uint64_t> plain = run_route_pipeline_k1_sequence(false);
    const std::vector<uint64_t> pipelined = run_route_pipeline_k1_sequence(true);
    require(plain == pipelined,
            "k1 identity: plain and pipelined_begin clients diverged on F's "
            "route cursor progression");
}

// Step 4 sub-step 4a (research/designs/P50_ROUTE_PIPELINING.md, "Step 4"):
// K=1 identity capture. This records the exact client+server wire bytes
// (via EndpointIoControl::outbound_message_observer, the existing
// production-safe test seam -- no new plumbing) plus the C-side action
// trace for a sequential multi-TX K=1 dialogue on one P29V1 route, folds
// everything into a single Digest128, and freezes it against a golden
// value captured from pre-Step-4 code. Every later Step 4 sub-step (4b-4e)
// must keep this test green: with K=1 (one TX fully committed before the
// next begins), observable wire bytes and action-trace content must stay
// byte-identical even once K>1 pipelining lands for K>=2.
void test_p29v1_k1_pipeline_wire_identity() {
    const P5coStoreGuids guids = p5co_store_guids(237);
    EndpointCaps caps;
    caps.profile = ProfileId::P29V1;
    caps.zstd.max_raw_bytes = 1U << 20;
    caps.zstd.max_encoded_body_bytes = 1U << 20;
    ActionTrace actions;
    P50ServerEndpoint server(guids.f, caps, nullptr, &actions);
    PreparationAuthorityLimits authority_limits;
    authority_limits.max_interner_reserved_bytes = UINT64_C(1044398080);
    TestClient client(guids.c, caps, HistoryNonce{1}, nullptr, &actions,
                      authority_limits);

    icecc::Digest128Builder builder;
    const auto observe = [&builder](ActorSide actor, const Message& message) {
        builder.append_u8(static_cast<uint8_t>(actor));
        const std::vector<uint8_t> frame = encode_frame(message);
        builder.append_u32(static_cast<uint32_t>(frame.size()));
        builder.append(std::span<const uint8_t>(frame));
    };
    EndpointIoControl client_control;
    client_control.outbound_message_observer = observe;
    EndpointIoControl server_control;
    server_control.outbound_message_observer = observe;

    const std::vector<uint8_t> tu1 =
        bytes("# 1 \"/tmp/k1-fixture.cc\"\nfirst-tu\n");
    const std::vector<uint8_t> tu2 =
        bytes("# 1 \"/tmp/k1-fixture.cc\"\nsecond-tu\n");
    const std::vector<uint8_t> tu3 =
        bytes("# 1 \"/tmp/k1-fixture.cc\"\nthird-tu\n");

    const PairResult first = run_pair(client, server, admit(client, tu1),
                                       client_control, server_control);
    require(first.client.status == ClientRunStatus::Committed &&
                first.server.status == ServerRunStatus::Completed &&
                first.server.committed_input.has_value(),
            "K1 identity: first TU did not commit");
    server.close_input_job(*first.server.committed_input);

    const PairResult second = run_pair(client, server, admit(client, tu2),
                                        client_control, server_control);
    require(second.client.status == ClientRunStatus::Committed &&
                second.server.status == ServerRunStatus::Completed &&
                second.server.committed_input.has_value(),
            "K1 identity: second TU did not commit");
    server.close_input_job(*second.server.committed_input);

    const PairResult third = run_pair(client, server, admit(client, tu3),
                                       client_control, server_control);
    require(third.client.status == ClientRunStatus::Committed &&
                third.server.status == ServerRunStatus::Completed &&
                third.server.committed_input.has_value(),
            "K1 identity: third TU did not commit");
    server.close_input_job(*third.server.committed_input);

    require(client.endpoint.next_rel_seq().value == 3,
            "K1 identity: route rel_seq did not advance 1,2,3 across "
            "sequential K=1 TUs");

    if (const auto error = check_action_trace(actions.records()))
        fail("K1 identity action trace: " + *error);
    for (const ActionRecord& record : actions.records())
        builder.append(action_jsonl(record));

    // Golden value captured from pre-Step-4 code (today's base). Any Step 4
    // sub-step that changes this value for a K=1 dialogue has broken the
    // "K=1 stays byte-identical" invariant and must be investigated, not
    // silently re-baselined.
    const icecc::Digest128 golden{{
        0x86, 0x88, 0x34, 0x20, 0xa6, 0x4c, 0x8c, 0xc5,
        0xc1, 0x42, 0xaf, 0x51, 0xa6, 0x6d, 0xcf, 0xc3,
    }};
    const icecc::Digest128 actual = builder.finish();
    require(actual == golden,
            "K1 wire+action identity drifted from the pre-Step-4 golden "
            "capture (actual_hex=" + icecc::digest128_hex(actual) +
            "); every Step 4 sub-step must keep K=1 byte-identical per "
            "P50_ROUTE_PIPELINING.md Step 4");
}

void test_profile_materialized_result_digest_gates() {
    uint64_t identity = 19000;
    for (const ProfileId profile : {ProfileId::P29V1, ProfileId::ZSTD_TU,
                                    ProfileId::ZSTD_ROUTE}) {
        EndpointCaps caps;
        caps.profile = profile;
        caps.zstd.max_raw_bytes = 1U << 20;
        caps.zstd.max_encoded_body_bytes = 1U << 20;
        const CStoreGuid c_guid = Id128::from_u64(identity++);
        P50ServerEndpoint server(Id128::from_u64(identity++), caps);
        TestClient client(c_guid, caps);
        const std::vector<uint8_t> input = bytes(
            "# 1 \"/tmp/profile-digest-mutant.cc\"\n"
            "same materialized bytes\nsame materialized bytes\n");
        const PreparedTuHandle prepared = admit(client, input);
        EndpointIoControl corrupt;
        corrupt.outbound_begin_transform =
            [](const TxBegin& original, std::span<const uint8_t> body) {
                TxBegin changed = original;
                changed.raw_digest.bytes[0] ^= 0x80;
                changed.transaction_digest =
                    compute_transaction_digest(changed, body);
                return changed;
            };
        const PairResult rejected =
            run_pair(client, server, prepared, corrupt);
        require(rejected.server.status == ServerRunStatus::TerminalError &&
                    !copy_input(server, c_guid).has_value(),
                std::string(profile_name(profile)) +
                    " accepted or published materialized bytes whose digest differed");
    }
}

void test_s3_resource_storm_product_path() {
    const char* requested = std::getenv("ICECC_P50_S3_RESOURCE_STORM");
    const bool storm_only = requested != nullptr &&
                            std::string_view(requested) == "storm";
    P50ServerEndpointConfig soak_config;
    soak_config.owner_limits.max_namespaces = 4;
    soak_config.owner_limits.max_retained_input_records = 1;
    soak_config.owner_limits.max_retained_input_bytes = 4096;
    P50ServerEndpoint soak_server(Id128::from_u64(12000), {}, nullptr, nullptr,
                                  std::move(soak_config));
    TestClient soak_client(Id128::from_u64(12001));
    const uint64_t soak_iterations = storm_only ? 0 : 10000;
    for (uint64_t index = 0; index != soak_iterations; ++index) {
        const std::vector<uint8_t> input{
            static_cast<uint8_t>(index),
            static_cast<uint8_t>(index >> 8),
            static_cast<uint8_t>(index >> 16),
            static_cast<uint8_t>(index >> 24)};
        const PreparedTuHandle prepared = admit(soak_client, input);
        const PairResult result = run_pair(soak_client, soak_server, prepared);
        require(result.client.status == ClientRunStatus::Committed &&
                    result.server.status == ServerRunStatus::Completed &&
                    result.server.committed_input.has_value() &&
                    copy_input(soak_server, soak_client.c_store_guid()) == input,
                "10,000-TU product soak failed exact endpoint commit");
        soak_server.close_input_job(*result.server.committed_input);
        soak_server.collect_input_garbage();
        require(soak_client.authority->release(prepared) == 0,
                "10,000-TU product soak retained a producer preparation");
        require(soak_server.owner_usage().retained_input_records == 0 &&
                    soak_server.owner_usage().retained_input_bytes == 0,
                "10,000-TU product soak retained a released input");
        if ((index + 1) % 2500 == 0)
            report_resource_checkpoint("soak-" + std::to_string(index + 1),
                                       soak_server.owner_usage());
    }
    if (!storm_only)
        report_resource_checkpoint("soak-final", soak_server.owner_usage());

    P50ServerEndpointConfig storm_config;
    storm_config.owner_limits.max_namespaces = 256;
    storm_config.owner_limits.max_retained_input_records = 8;
    storm_config.owner_limits.max_retained_input_bytes = 8 * 64;
    P50ServerEndpoint storm_server(Id128::from_u64(13000), {}, nullptr, nullptr,
                                   std::move(storm_config));
    std::vector<std::unique_ptr<TestClient>> pinned_clients;
    std::vector<InputCursor> pinned_cursors;
    pinned_clients.reserve(8);
    pinned_cursors.reserve(8);
    for (uint64_t index = 0; index != 8; ++index) {
        auto client = std::make_unique<TestClient>(Id128::from_u64(13100 + index));
        const std::vector<uint8_t> input(64, static_cast<uint8_t>(index + 1));
        const PairResult result =
            run_pair(*client, storm_server, admit(*client, input));
        require(result.client.status == ClientRunStatus::Committed &&
                    result.server.status == ServerRunStatus::Completed &&
                    result.server.committed_input.has_value(),
                "multi-C_GUID storm failed initial product commit");
        pinned_cursors.emplace_back(
            storm_server.attach_input(*result.server.committed_input));
        storm_server.close_input_job(*result.server.committed_input);
        pinned_clients.emplace_back(std::move(client));
    }
    report_resource_checkpoint("storm-pinned", storm_server.owner_usage());

    auto blocked_client = std::make_unique<TestClient>(Id128::from_u64(13200));
    const std::vector<uint8_t> blocked_input(64, 0xa5);
    const PreparedTuHandle blocked_prepared = admit(*blocked_client, blocked_input);
    const PairResult blocked = run_pair(*blocked_client, storm_server,
                                        blocked_prepared);
    require(blocked.client.status == ClientRunStatus::TerminalError &&
                blocked.server.status == ServerRunStatus::TerminalError &&
                blocked_client->has_active_transaction() &&
                storm_server.owner_usage().retained_input_records == 8 &&
                storm_server.owner_usage().retained_input_bytes == 8 * 64,
            "cursor-pinned product capacity failure changed retained state");
    report_resource_checkpoint("storm-failed-install", storm_server.owner_usage());

    pinned_cursors.clear();
    const PairResult recovered = run_pair(*blocked_client, storm_server);
    require(recovered.client.status == ClientRunStatus::Committed &&
                recovered.server.status == ServerRunStatus::Completed &&
                recovered.client.reconnect == EndpointReconnectOutcome::ExactMatch &&
                recovered.server.committed_input.has_value() &&
                copy_input(storm_server, blocked_client->c_store_guid()) == blocked_input,
            "failed product install did not retry exact bytes after cursor release");
    storm_server.close_input_job(*recovered.server.committed_input);
    report_resource_checkpoint("storm-recovered", storm_server.owner_usage());

    for (uint64_t index = 0; index != 512; ++index) {
        TestClient client(Id128::from_u64(14000 + index));
        const std::vector<uint8_t> input(64, static_cast<uint8_t>(index));
        const PairResult result =
            run_pair(client, storm_server, admit(client, input));
        require(result.client.status == ClientRunStatus::Committed &&
                    result.server.status == ServerRunStatus::Completed &&
                    result.server.committed_input.has_value(),
                "multi-C_GUID product eviction storm failed a commit");
        storm_server.close_input_job(*result.server.committed_input);
        require(storm_server.owner_usage().retained_input_records <= 8 &&
                    storm_server.owner_usage().retained_input_bytes <= 8 * 64,
                "multi-C_GUID product eviction storm exceeded retained bounds");
        if ((index + 1) % 128 == 0)
            report_resource_checkpoint("storm-" + std::to_string(index + 1),
                                       storm_server.owner_usage());
    }
    require(storm_server.owner_usage().namespaces <= 256,
            "multi-C_GUID product storm exceeded the namespace cap");
    report_resource_checkpoint("storm-final", storm_server.owner_usage());
}

size_t count_global_actions(const GlobalResourceTrace& trace,
                            GlobalActionType action, ObjectType type) {
    size_t result = 0;
    for (const GlobalActionRecord& record : trace.records())
        if (record.action == action && record.key.valid() &&
            record.key.type() == type)
            ++result;
    return result;
}

size_t retained_global_actions(const GlobalResourceTrace& trace,
                                ObjectType type) {
    return count_global_actions(trace, GlobalActionType::ARENA_PRESENT,
                                 type) -
           count_global_actions(trace, GlobalActionType::ARENA_RELEASED,
                                 type);
}

// 2026-10-10 F-side retained route-history bound. A C store that went away
// leaves its namespace's persistent P29V1 dialogue (region segments,
// occurrence streams, path tables) behind; nothing bounded that state while
// the namespace sat idle. The idle sweep in collect_input_garbage drops a
// dead store's codec history after
// P50ServerEndpointConfig::route_history_idle_grace_ms, so many successive
// fresh C stores against one F service retain at most the live store's
// history instead of one per store for the process's life.
void test_idle_p29v1_route_history_is_dropped_for_dead_stores() {
    const P5coStoreGuids guids = p5co_store_guids(236);
    EndpointCaps caps;
    caps.profile = ProfileId::P29V1;
    caps.zstd.max_raw_bytes = 1U << 20;
    caps.zstd.max_encoded_body_bytes = 1U << 20;
    const std::vector<uint8_t> input = bytes(
        "# 1 \"/tmp/p29v1-idle-history.cc\"\n"
        "same-line\nsame-line\nsame-line\nsame-line\ntail\n");
    constexpr uint64_t kStores = 4;

    // Retention with the sweep disabled (grace 0) is today's behaviour: every
    // dead store keeps its route-history segments for the process's life.
    GlobalResourceTrace legacy_trace;
    P50ServerEndpointConfig legacy_config;
    legacy_config.global_resource_trace = &legacy_trace;
    legacy_config.route_history_idle_grace_ms = 0;
    P50ServerEndpoint legacy(guids.f, caps, nullptr, nullptr,
                             std::move(legacy_config));
    for (uint64_t seed = 0; seed != kStores; ++seed) {
        TestClient client(Id128::from_u64(23600 + seed), caps);
        const PairResult result = run_pair(client, legacy, admit(client, input));
        require(result.client.status == ClientRunStatus::Committed &&
                    result.server.status == ServerRunStatus::Completed &&
                    result.server.committed_input.has_value(),
                "route-history legacy fixture store did not commit");
        legacy.close_input_job(*result.server.committed_input);
        legacy.collect_input_garbage();
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        legacy.collect_input_garbage();
    }
    require(count_global_actions(legacy_trace,
                                 GlobalActionType::ARENA_RELEASED,
                                 ObjectType::P29Segment) == 0 &&
                retained_global_actions(legacy_trace, ObjectType::P29Segment) >=
                    kStores,
            "grace-disabled F did not retain every dead store's route "
            "history (the unbounded retention this sweep bounds)");

    // Bounded: each idle store's history is dropped, so retained route
    // segments never exceed the live store's own.
    GlobalResourceTrace bounded_trace;
    P50ServerEndpointConfig bounded_config;
    bounded_config.global_resource_trace = &bounded_trace;
    bounded_config.route_history_idle_grace_ms = 20;
    P50ServerEndpoint bounded(guids.f, caps, nullptr, nullptr,
                              std::move(bounded_config));
    std::vector<std::unique_ptr<TestClient>> clients;
    for (uint64_t seed = 0; seed != kStores; ++seed) {
        auto client =
            std::make_unique<TestClient>(Id128::from_u64(23700 + seed), caps);
        const PairResult result =
            run_pair(*client, bounded, admit(*client, input));
        require(result.client.status == ClientRunStatus::Committed &&
                    result.server.status == ServerRunStatus::Completed &&
                    result.server.committed_input.has_value(),
                "route-history bounded fixture store did not commit");
        bounded.close_input_job(*result.server.committed_input);
        bounded.collect_input_garbage();
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        bounded.collect_input_garbage();
        require(retained_global_actions(bounded_trace,
                                        ObjectType::P29Segment) <= 1,
                "an idle F namespace's P29V1 route history was not dropped "
                "after its grace bound");
        clients.push_back(std::move(client));
    }

    // A store reconnecting after its idle-history drop reconciles through
    // HISTORY_RESET and commits again -- the same recovery the existing
    // input-payload eviction path exercises.
    const PairResult recovered =
        run_pair(*clients[0], bounded, admit(*clients[0], input));
    if (recovered.client.status != ClientRunStatus::Committed ||
        recovered.server.status != ServerRunStatus::Completed)
        std::cerr << "idle-history recovery diagnostic: client_status="
                  << static_cast<unsigned>(recovered.client.status)
                  << " reconnect="
                  << static_cast<unsigned>(recovered.client.reconnect)
                  << " server_status="
                  << static_cast<unsigned>(recovered.server.status);
    if (recovered.client.terminal_error)
        std::cerr << " client_error=" << recovered.client.terminal_error->detail;
    if (recovered.server.terminal_error)
        std::cerr << " server_error=" << recovered.server.terminal_error->detail;
    std::cerr << '\n';
    require(recovered.client.status == ClientRunStatus::Committed &&
                recovered.server.status == ServerRunStatus::Completed &&
                recovered.server.committed_input.has_value() &&
                copy_input(bounded, clients[0]->c_store_guid()) == input,
            "a store reconnecting after its idle-history drop did not "
            "recover by HISTORY_RESET");
    bounded.close_input_job(*recovered.server.committed_input);
    bounded.collect_input_garbage();
}

void test_live_global_resource_trace() {
    GlobalResourceTrace trace;
    P50ServerEndpointConfig config;
    config.global_resource_trace = &trace;
    P50ServerEndpoint server(Id128::from_u64(15000), {}, nullptr, nullptr,
                             std::move(config));
    TestClient client(Id128::from_u64(15001));
    const std::vector<uint8_t> input = bytes("live endpoint global trace\n");
    const PairResult result = run_pair(client, server, admit(client, input));
    require(result.client.status == ClientRunStatus::Committed &&
                result.server.status == ServerRunStatus::Completed &&
                result.server.committed_input.has_value() &&
                copy_input(server, client.c_store_guid()) == input,
            "live global trace transaction did not commit exact input");
    server.close_input_job(*result.server.committed_input);
    server.collect_input_garbage();
    server.reset_store(Id128::from_u64(15002));

    const auto action_index = [&](GlobalActionType action) {
        const auto position = std::find_if(
            trace.records().begin(), trace.records().end(),
            [action](const GlobalActionRecord& record) {
                return record.action == action;
            });
        require(position != trace.records().end(),
                std::string("live endpoint omitted global action ") +
                    std::string(global_action_name(action)));
        return static_cast<size_t>(position - trace.records().begin());
    };
    const size_t admitted = action_index(GlobalActionType::NAMESPACE_ADMITTED);
    const size_t started = action_index(GlobalActionType::TU_STARTED);
    const size_t installing = action_index(GlobalActionType::ARENA_INSTALLING);
    const size_t present = action_index(GlobalActionType::ARENA_PRESENT);
    const size_t finished = action_index(GlobalActionType::TU_FINISHED);
    const size_t released = action_index(GlobalActionType::ARENA_RELEASED);
    const size_t evicted = action_index(GlobalActionType::NAMESPACE_EVICTED);
    require(admitted < started && started < installing && installing < present &&
                present < finished && finished < released && released < evicted,
            "live endpoint global trace lifecycle order is invalid");

    if (const char* path = std::getenv("P50_ENDPOINT_GLOBAL_TRACE_PATH"))
        write_global_trace(trace, path);
}

void test_automatic_action_trace_is_complete_past_1024_records() {
    char trace_path[] = "/tmp/p50endpoint-f-action-trace-XXXXXX";
    const int trace_fd = ::mkstemp(trace_path);
    require(trace_fd >= 0, "automatic action-trace fixture could not create a path");
    require(::close(trace_fd) == 0 && ::unlink(trace_path) == 0,
            "automatic action-trace fixture could not prepare an absent path");
    require(::setenv("ICECC_P50_F_ACTION_TRACE", trace_path, 1) == 0,
            "automatic action-trace fixture could not enable the F sink");

    {
        P50ServerEndpoint server(Id128::from_u64(15101));
        TestClient client(Id128::from_u64(15102));
        for (uint64_t index = 0; index != 180; ++index) {
            const std::vector<uint8_t> input{
                static_cast<uint8_t>(index),
                static_cast<uint8_t>(index >> 8),
                0x50,
                0x38,
            };
            const PreparedTuHandle prepared = admit(client, input);
            const PairResult result = run_pair(client, server, prepared);
            require(result.client.status == ClientRunStatus::Committed &&
                        result.server.status == ServerRunStatus::Completed &&
                        result.server.committed_input.has_value(),
                    "automatic action-trace fixture failed a product transaction");
            server.close_input_job(*result.server.committed_input);
            server.collect_input_garbage();
            require(client.authority->release(prepared) == 0,
                    "automatic action-trace fixture retained a preparation");
        }
    }

    require(::unsetenv("ICECC_P50_F_ACTION_TRACE") == 0,
            "automatic action-trace fixture could not disable the F sink");
    std::ifstream trace(trace_path, std::ios::binary);
    require(static_cast<bool>(trace),
            "automatic action-trace fixture did not create its requested trace");
    const std::string contents((std::istreambuf_iterator<char>(trace)),
                               std::istreambuf_iterator<char>());
    require(static_cast<size_t>(std::count(contents.begin(), contents.end(), '\n')) > 1024 &&
                contents.find("\"tu_seq\":179") != std::string::npos,
            "automatically owned action trace truncated after 1024 records");
    require(::unlink(trace_path) == 0,
            "automatic action-trace fixture could not remove its trace");
}

bool s3_resource_storm_requested() {
    const char* value = std::getenv("ICECC_P50_S3_RESOURCE_STORM");
    return value != nullptr &&
           (std::string_view(value) == "1" || std::string_view(value) == "storm");
}

PairResult run_adopted_pair(P50ClientEndpoint& client, P50ServerEndpoint& server,
                            PreparedTuHandle prepared, bool use_native_adoption,
                            int* consumed_fd) {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    std::future<ServerRunResult> server_result = asio::co_spawn(
        context,
        [&]() -> asio::awaitable<ServerRunResult> {
            const auto executor = co_await asio::this_coro::executor;
            tcp::socket accepted(executor);
            boost::system::error_code error;
            co_await acceptor.async_accept(
                accepted, asio::redirect_error(asio::use_awaitable, error));
            require(!error, "loopback accept for adopted endpoint failed");
            const int accepted_fd = accepted.native_handle();
            if (!use_native_adoption) {
                if (consumed_fd != nullptr)
                    *consumed_fd = accepted_fd;
                co_return co_await server.run_adopted(std::move(accepted));
            }

            const int duplicate_fd = ::dup(accepted_fd);
            require(duplicate_fd >= 0, "duplicating accepted fd failed");
            boost::system::error_code adopt_error;
            auto adopted = P50ServerEndpoint::adopt_connected_fd(
                executor, duplicate_fd, adopt_error);
            require(adopted.has_value() && !adopt_error,
                    "native accepted fd was not adopted");
            require((::fcntl(adopted->native_handle(), F_GETFD) & FD_CLOEXEC) != 0,
                    "native adoption did not establish CLOEXEC");
            tcp::no_delay no_delay;
            adopted->get_option(no_delay, adopt_error);
            require(!adopt_error && no_delay.value(),
                    "native adoption did not disable Nagle");
            if (consumed_fd != nullptr)
                *consumed_fd = adopted->native_handle();
            boost::system::error_code close_error;
            accepted.close(close_error);
            co_return co_await server.run_adopted(std::move(*adopted));
        },
        asio::use_future);
    std::future<ClientRunResult> client_result = asio::co_spawn(
        context, client.run(acceptor.local_endpoint(), prepared), asio::use_future);
    context.run();
    return {client_result.get(), server_result.get()};
}

void require_closed_fd(int fd, std::string_view detail) {
    errno = 0;
    require(fd >= 0 && ::fcntl(fd, F_GETFD) < 0 && errno == EBADF, detail);
}

size_t open_fd_count() {
    DIR* directory = ::opendir("/proc/self/fd");
    require(directory != nullptr, "could not open /proc/self/fd");
    size_t count = 0;
    while (const dirent* entry = ::readdir(directory)) {
        if (std::string_view(entry->d_name) != "." &&
            std::string_view(entry->d_name) != "..")
            ++count;
    }
    require(::closedir(directory) == 0, "could not close /proc/self/fd");
    return count;
}

size_t staging_file_count() {
    std::error_code error;
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path(error);
    if (error)
        return 0;
    size_t count = 0;
    std::filesystem::directory_iterator entries(directory, error);
    for (const auto& entry : entries) {
        if (error)
            break;
        const std::string name = entry.path().filename().string();
        if (name.find("icecc") == std::string::npos &&
            name.find("p50") == std::string::npos)
            continue;
        std::error_code status_error;
        if (entry.is_regular_file(status_error) && !status_error)
            ++count;
    }
    return count;
}

uint64_t resident_bytes() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (!line.starts_with("VmRSS:"))
            continue;
        std::istringstream fields(line);
        std::string key;
        std::string unit;
        uint64_t value = 0;
        if (fields >> key >> value >> unit && key == "VmRSS:" && unit == "kB")
            return value * 1024;
        return 0;
    }
    return 0;
}

void report_resource_checkpoint(std::string_view label,
                                const P50ServerOwnerUsage& usage) {
    const uint64_t rss = resident_bytes();
    require(rss != 0, "could not read the product-gate RSS checkpoint");
    std::cerr << "p50_s3_resource checkpoint=" << label
              << " retained_records=" << usage.retained_input_records
              << " retained_bytes=" << usage.retained_input_bytes
              << " namespaces=" << usage.namespaces
              << " staging_files=" << staging_file_count()
              << " open_fds=" << open_fd_count()
              << " rss_bytes=" << rss << '\n';
}

void test_adopted_endpoint_exact_zstd_and_ownership() {
    for (const bool use_native_adoption : {false, true}) {
        CompletionLog completions;
        P50ServerEndpoint server(Id128::from_u64(use_native_adoption ? 910 : 911), {},
                                 &completions);
        TestClient client(Id128::from_u64(use_native_adoption ? 912 : 913));
        const std::vector<uint8_t> input = pseudo_random_bytes(8192);
        int consumed_fd = -1;
        const PairResult result = run_adopted_pair(
            client, server, admit(client, input), use_native_adoption, &consumed_fd);
        require(result.client.status == ClientRunStatus::Committed &&
                    result.server.status == ServerRunStatus::Completed &&
                    copy_input(server, client.c_store_guid()) == input,
                use_native_adoption ? "native adopted ZSTD_TU changed exact bytes"
                                    : "socket adopted ZSTD_TU changed exact bytes");
        require_closed_fd(consumed_fd, "adopted endpoint did not close its owned fd exactly once");
        require(!completions.completions().empty() &&
                    completions.completions().front().stamp.operation ==
                        AsyncOperationKind::ReadHeader,
                "adopted endpoint consumed an accept or input operation before ownership");
        require(std::none_of(completions.completions().begin(), completions.completions().end(),
                             [](const AsyncCompletion& completion) {
                                 return completion.stamp.operation == AsyncOperationKind::Accept;
                             }),
                "adopted endpoint unexpectedly performed a second accept");
    }
}

void test_client_deadline_and_direct_socket_ownership() {
    CompletionLog completions;
    TestClient client(Id128::from_u64(930), {}, HistoryNonce{1}, &completions);
    const PreparedTuHandle prepared = admit(client, bytes("deadline input\n"));

    // An already-expired deadline must not allocate a session, enqueue work, or
    // attempt a connect.
    {
        asio::io_context context;
        std::future<ClientRunResult> future = asio::co_spawn(
            context,
            client.endpoint.run(tcp::endpoint{asio::ip::address_v4::loopback(), 1}, prepared,
                                 {}, std::chrono::steady_clock::now() - std::chrono::seconds(1)),
            asio::use_future);
        context.run();
        const ClientRunResult result = future.get();
        require(result.status == ClientRunStatus::DeadlineExceeded &&
                    !client.has_reconciliation_work(),
                "deadline-before-start changed client reconciliation state");
    }

    // The peer accepts and then never replies.  The endpoint-owned timer must
    // close the socket and complete in a bounded interval, with no timer left
    // keeping the io_context alive after the run result is delivered.
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    std::future<void> peer = asio::co_spawn(context, raw_stall_after_connect(acceptor),
                                             asio::use_future);
    const auto started = std::chrono::steady_clock::now();
    std::future<ClientRunResult> future = asio::co_spawn(
        context,
        client.endpoint.run(acceptor.local_endpoint(), prepared, {},
                             std::chrono::steady_clock::now() + std::chrono::milliseconds(100)),
        asio::use_future);
    context.run();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    peer.get();
    const ClientRunResult result = future.get();
    require(result.status == ClientRunStatus::DeadlineExceeded &&
                elapsed < std::chrono::seconds(2) && client.has_reconciliation_work(),
            "unresponsive peer was not bounded by the client deadline");
    // No handler/timer may retain the socket after completion.
    require(context.poll() == 0, "deadline run left a live socket/timer callback");

    // The connected-socket overload consumes a descriptor that was connected
    // by the ordinary listener path and drives the same reducer/witness path.
    TestClient adopted_client(Id128::from_u64(931));
    const PreparedTuHandle adopted_prepared = admit(adopted_client, bytes("adopted input\n"));
    P50ServerEndpoint server(Id128::from_u64(932));
    asio::io_context adopted_context;
    tcp::acceptor adopted_acceptor(adopted_context,
                                   {asio::ip::address_v4::loopback(), 0});
    std::future<ServerRunResult> server_result = asio::co_spawn(
        adopted_context,
        [&]() -> asio::awaitable<ServerRunResult> {
            const auto executor = co_await asio::this_coro::executor;
            tcp::socket accepted(executor);
            co_await adopted_acceptor.async_accept(accepted, asio::use_awaitable);
            co_return co_await server.run_adopted(std::move(accepted));
        },
        asio::use_future);
    std::future<ClientRunResult> client_result = asio::co_spawn(
        adopted_context,
        [&]() -> asio::awaitable<ClientRunResult> {
            const auto executor = co_await asio::this_coro::executor;
            tcp::socket connected(executor);
            co_await connected.async_connect(adopted_acceptor.local_endpoint(), asio::use_awaitable);
            co_return co_await adopted_client.endpoint.run(std::move(connected),
                                                           adopted_prepared);
        },
        asio::use_future);
    adopted_context.run();
    const ClientRunResult adopted_result = client_result.get();
    require(adopted_result.status == ClientRunStatus::Committed &&
                adopted_result.committed_commit.has_value() &&
                adopted_result.committed_input.has_value() &&
                server_result.get().status == ServerRunStatus::Completed,
            "connected-socket client overload did not commit its direct witness");
}

void test_client_completion_deadline_is_fresh_before_first_write() {
    TestClient client(Id128::from_u64(933));
    const PreparedTuHandle prepared = admit(client, bytes("completion deadline input\n"));

    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    std::future<void> peer = asio::co_spawn(
        context, raw_stall_after_connect(acceptor), asio::use_future);

    bool delayed_connect_completion = false;
    bool attempted_first_remote_write = false;
    EndpointIoControl control;
    control.before_completion_check = [&](const CompletionStamp& observed) {
        if (delayed_connect_completion || observed.actor != ActorSide::C ||
            observed.operation != AsyncOperationKind::Connect)
            return;
        delayed_connect_completion = true;
        // The timer shares this owner executor, so it cannot run while this
        // completion validator is blocked. The reducer itself must freshly
        // sample the deadline before it initiates the first CacheWire write.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    };
    control.before_first_remote_write = [&] {
        attempted_first_remote_write = true;
    };

    std::future<ClientRunResult> run = asio::co_spawn(
        context,
        client.endpoint.run(
            acceptor.local_endpoint(), prepared, std::move(control),
            std::chrono::steady_clock::now() + std::chrono::milliseconds(50)),
        asio::use_future);
    context.run();
    peer.get();
    const ClientRunResult result = run.get();
    require(delayed_connect_completion &&
                result.status == ClientRunStatus::DeadlineExceeded &&
                !attempted_first_remote_write &&
                client.has_reconciliation_work() &&
                context.poll() == 0,
            "post-deadline client completion attempted the first CacheWire write");
}

void test_adopted_endpoint_disconnect_and_invalid_rows() {
    P50ServerEndpoint server(Id128::from_u64(920));
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    int consumed_fd = -1;
    std::future<ServerRunResult> server_result = asio::co_spawn(
        context,
        [&]() -> asio::awaitable<ServerRunResult> {
            const auto executor = co_await asio::this_coro::executor;
            tcp::socket accepted(executor);
            boost::system::error_code error;
            co_await acceptor.async_accept(
                accepted, asio::redirect_error(asio::use_awaitable, error));
            require(!error, "disconnect row accept failed");
            consumed_fd = accepted.native_handle();
            co_return co_await server.run_adopted(std::move(accepted));
        },
        asio::use_future);
    std::future<void> disconnecting_client = asio::co_spawn(
        context,
        [&]() -> asio::awaitable<void> {
            const auto executor = co_await asio::this_coro::executor;
            tcp::socket socket(executor);
            boost::system::error_code error;
            co_await socket.async_connect(acceptor.local_endpoint(),
                                          asio::redirect_error(asio::use_awaitable, error));
            require(!error, "disconnect row connect failed");
            socket.close(error);
            co_return;
        },
        asio::use_future);
    context.run();
    require(server_result.get().status == ServerRunStatus::Disconnected &&
                server.live_session_count() == 0,
            "adopted disconnect did not fail closed and release its session");
    disconnecting_client.get();
    require_closed_fd(consumed_fd, "adopted disconnect leaked its owned fd");

    asio::io_context invalid_context;
    boost::system::error_code error;
    auto reject_fd = [&](int fd, std::string_view detail) {
        auto adopted = P50ServerEndpoint::adopt_connected_fd(invalid_context.get_executor(), fd,
                                                              error);
        require(!adopted.has_value() && !!error, detail);
        require_closed_fd(fd, "invalid native adoption did not consume and close fd");
        error.clear();
    };

    int pipe_fds[2] = {-1, -1};
    require(::pipe(pipe_fds) == 0, "pipe row setup failed");
    reject_fd(pipe_fds[0], "non-socket native fd was adopted");
    (void)::close(pipe_fds[1]);

    int unix_pair[2] = {-1, -1};
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, unix_pair) == 0,
            "wrong-family row setup failed");
    reject_fd(unix_pair[0], "wrong-family native fd was adopted");
    (void)::close(unix_pair[1]);

    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    require(listener >= 0, "unconnected listener row setup failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
            "unconnected listener row bind failed");
    require(::listen(listener, 1) == 0, "unconnected listener row listen failed");
    reject_fd(listener, "unconnected listener fd was adopted");

    int closed_pair[2] = {-1, -1};
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, closed_pair) == 0,
            "closed-fd row setup failed");
    const int closed_fd = closed_pair[0];
    (void)::close(closed_fd);
    (void)::close(closed_pair[1]);
    auto adopted_closed = P50ServerEndpoint::adopt_connected_fd(
        invalid_context.get_executor(), closed_fd, error);
    require(!adopted_closed.has_value() && !!error,
            "closed native fd was adopted");
    error.clear();

    tcp::socket invalid_socket(invalid_context.get_executor());
    std::future<ServerRunResult> invalid_result = asio::co_spawn(
        invalid_context, server.run_adopted(std::move(invalid_socket)), asio::use_future);
    invalid_context.run();
    require(invalid_result.get().status == ServerRunStatus::Disconnected &&
                server.live_session_count() == 0,
            "closed adopted socket did not fail closed");
}

void test_adopted_cross_executor_releases_registration() {
    P50ServerEndpoint server(Id128::from_u64(930));
    // Bind the endpoint owner to this thread. The adopted socket's I/O below
    // runs on a different executor, forcing the reducer's owner check from a
    // completion callback without permitting a session-row leak.
    server.reset_store(Id128::from_u64(931));

    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    require(listener >= 0, "cross-executor listener setup failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
            "cross-executor listener bind failed");
    require(::listen(listener, 1) == 0, "cross-executor listener listen failed");
    socklen_t address_length = sizeof(address);
    require(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_length) == 0,
            "cross-executor listener address failed");
    const int client = ::socket(AF_INET, SOCK_STREAM, 0);
    require(client >= 0, "cross-executor client setup failed");
    require(::connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
            "cross-executor client connect failed");
    const int accepted_fd = ::accept(listener, nullptr, nullptr);
    require(accepted_fd >= 0, "cross-executor accept failed");
    (void)::close(listener);
    (void)::close(client);

    asio::io_context owner_context;
    asio::io_context worker_context;
    auto worker_work = asio::make_work_guard(worker_context);
    tcp::socket adopted(worker_context.get_executor());
    boost::system::error_code assign_error;
    adopted.assign(tcp::v4(), accepted_fd, assign_error);
    require(!assign_error, "cross-executor socket assign failed");

    EndpointIoControl control;
    control.before_completion_check = [](const CompletionStamp&) {
        throw std::logic_error("forced adopted reducer entry failure");
    };
    std::future<ServerRunResult> result = asio::co_spawn(
        owner_context, server.run_adopted(std::move(adopted), std::move(control)),
        asio::use_future);
    std::thread worker([&] { worker_context.run(); });
    owner_context.run();
    worker_work.reset();
    worker.join();
    require(result.get().status == ServerRunStatus::TerminalError,
            "cross-executor owner rejection was not terminal");
    require(server.live_session_count() == 0,
            "cross-executor owner rejection stranded a live session");
    require_closed_fd(accepted_fd, "cross-executor adopted fd was not closed");
}

void test_adopted_codec_runs_off_the_owner_thread() {
    const P5coStoreGuids guids = p5co_store_guids(241);
    P50ServerEndpoint server(guids.f);
    TestClient client(guids.c);
    const PreparedTuHandle prepared =
        admit(client, bytes("codec pool placement input\n"));
    // Codec work must run on the bounded pool, never on the endpoint owner
    // thread: an owner-executor post serializes every connection behind
    // materialization and gives a codec failure owner-state authority.
    const std::thread::id owner_thread = std::this_thread::get_id();
    std::atomic<bool> codec_job_ran{false};
    bool codec_job_on_owner_thread = false;
    EndpointIoControl server_control;
    server_control.before_materialize_on_worker = [&] {
        codec_job_on_owner_thread = std::this_thread::get_id() == owner_thread;
        codec_job_ran.store(true, std::memory_order_release);
    };
    const PairResult result =
        run_pair(client, server, prepared, {}, std::move(server_control));
    require(codec_job_ran.load(std::memory_order_acquire) &&
                !codec_job_on_owner_thread &&
                result.client.status == ClientRunStatus::Committed &&
                result.server.status == ServerRunStatus::Completed &&
                copy_input(server, guids.c) ==
                    bytes("codec pool placement input\n"),
            "adopted-socket codec materialization ran on the owner thread");
}

void test_adopted_codec_slots_release_per_job() {
    // The codec pool bounds concurrent jobs at eight.  Nine sequential
    // materializing runs must each find a free admission slot: a per-job
    // release that never ran strands the counter and rejects the ninth
    // admission with a full queue.
    const P5coStoreGuids guids = p5co_store_guids(242);
    P50ServerEndpoint server(guids.f);
    TestClient client(guids.c);
    for (size_t index = 0; index != 9; ++index) {
        const std::vector<uint8_t> input = pseudo_random_bytes(1024 + index);
        const PairResult result =
            run_pair(client, server, admit(client, input));
        require(result.client.status == ClientRunStatus::Committed &&
                    result.server.status == ServerRunStatus::Completed &&
                    copy_input(server, guids.c) == input,
                "adopted-socket codec admission slot was not released");
        // The worker releases its slot after the eventfd write that ends
        // this run; give it a moment before the next admission.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    require(server.live_session_count() == 0,
            "adopted-socket codec slot runs stranded a live session");
}

void test_adopted_selector_cancel_fences_publication() {
    const P5coStoreGuids guids = p5co_store_guids(243);
    ActionTrace actions;
    P50ServerEndpoint* server_slot = nullptr;
    P50ServerEndpointConfig config;
    size_t selector_calls = 0;
    config.input_job_state =
        [&](CStoreGuid, const TxBegin&, const TxCommit&, std::span<const uint8_t>) {
            ++selector_calls;
            server_slot->request_cancel_for_test();
            return InputJobState::Open;
        };
    P50ServerEndpoint server(guids.f, {}, nullptr, &actions, std::move(config));
    server_slot = &server;
    TestClient client(guids.c);
    const std::vector<uint8_t> input = pseudo_random_bytes(4096);
    const PreparedTuHandle prepared = admit(client, input);
    const PairResult result = run_pair(client, server, prepared);
    require(selector_calls == 1 &&
                result.server.status == ServerRunStatus::Disconnected &&
                result.client.status == ClientRunStatus::Disconnected &&
                !server.last_committed_input(guids.c).has_value() &&
                server.owner_usage().retained_input_records == 0 &&
                server.live_session_count() == 0 &&
                std::none_of(actions.records().begin(), actions.records().end(),
                             [](const ActionRecord& record) {
                                 return record.action ==
                                        ActionType::INPUT_COMMITTED;
                             }),
                "cancellation requested by the job-state selector published input");
}

void test_adopted_completion_cancel_is_rechecked() {
    const P5coStoreGuids guids = p5co_store_guids(244);
    ActionTrace actions;
    P50ServerEndpoint server(guids.f, {}, nullptr, &actions);
    TestClient client(guids.c);
    const std::vector<uint8_t> input = pseudo_random_bytes(2048);
    const PreparedTuHandle prepared = admit(client, input);
    size_t unbound_payload_completions = 0;
    bool cancelled_in_callback = false;
    EndpointIoControl server_control;
    server_control.before_live_identity_check =
        [&](const CompletionStamp& expected, CompletionLiveIdentity&) {
            if (expected.actor != ActorSide::F ||
                expected.operation != AsyncOperationKind::ReadPayload ||
                expected.transaction_bound)
                return;
            ++unbound_payload_completions;
            // Fresh-pair wire order: hello payload(1), HISTORY_RESET
            // payload(2), BEGIN payload(3). Cancelling on the BEGIN read
            // makes the final recheck fence that install.
            if (unbound_payload_completions != 3)
                return;
            cancelled_in_callback = true;
            server.request_cancel_for_test();
        };
    const PairResult result =
        run_pair(client, server, prepared, {}, std::move(server_control));
    require(cancelled_in_callback && unbound_payload_completions == 3 &&
                result.server.status == ServerRunStatus::Disconnected &&
                result.client.status == ClientRunStatus::Disconnected &&
                !server.last_committed_input(guids.c).has_value() &&
                server.owner_usage().retained_input_records == 0 &&
                server.live_session_count() == 0 &&
                std::none_of(actions.records().begin(), actions.records().end(),
                             [](const ActionRecord& record) {
                                 return record.action == ActionType::TX_BEGIN &&
                                        record.actor == ActorSide::F;
                             }),
                "cancellation inside a completion callback installed the begin");
}

CompetingPairResult run_competing_pair(P50ServerEndpointConfig config,
                                        uint64_t identity_base,
                                        size_t first_raw_bytes,
                                        size_t second_raw_bytes) {
    P50ServerEndpoint server(Id128::from_u64(identity_base), {}, nullptr, nullptr,
                             std::move(config));
    TestClient first(Id128::from_u64(identity_base + 1));
    TestClient second(Id128::from_u64(identity_base + 2));
    const PreparedTuHandle first_prepared =
        admit(first, pseudo_random_bytes(first_raw_bytes));
    const PreparedTuHandle second_prepared =
        admit(second, pseudo_random_bytes(second_raw_bytes));
    EndpointIoControl first_control;
    first_control.max_write_fragment = 1;
    EndpointIoControl second_control;
    second_control.max_write_fragment = 1;

    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    std::future<ServerRunResult> first_server =
        asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
    std::future<ServerRunResult> second_server =
        asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
    std::future<ClientRunResult> first_client = asio::co_spawn(
        context,
        first.endpoint.run(acceptor.local_endpoint(), first_prepared, first_control),
        asio::use_future);
    std::future<ClientRunResult> second_client = asio::co_spawn(
        context,
        second.endpoint.run(acceptor.local_endpoint(), second_prepared, second_control),
        asio::use_future);
    context.run();
    return {first_client.get(), second_client.get(), first_server.get(),
            second_server.get(), server.owner_usage()};
}

void require_trace(const ActionTrace& trace, std::string_view context) {
    if (const auto error = check_action_trace(trace.records()))
        fail(std::string(context) + ": " + *error);
}

TxBegin make_begin(const ZstdTuEnvelope& prepared, HistoryNonce nonce, Digest128 pre_state,
                   RelSeq rel = RelSeq{0}) {
    TxBegin begin = prepared.begin;
    begin.history_nonce = nonce;
    begin.rel_seq = rel;
    begin.pre_state_digest = pre_state;
    begin.transaction_digest = compute_transaction_digest(begin, prepared.body);
    return begin;
}

asio::awaitable<void> raw_write(tcp::socket& socket, Message message) {
    const std::vector<uint8_t> frame = encode_frame(message);
    co_await asio::async_write(socket, asio::buffer(frame), asio::use_awaitable);
    co_return;
}

asio::awaitable<void> raw_write_bytes(tcp::socket& socket,
                                      std::span<const uint8_t> bytes_to_write) {
    co_await asio::async_write(socket, asio::buffer(bytes_to_write.data(), bytes_to_write.size()),
                               asio::use_awaitable);
    co_return;
}

// One raw ::send loop on the native descriptor: asio's async_write caps
// every internal write_some near 64 KiB, which splits a multi-MiB batch
// into dozens of kernel deliveries -- F's pre-park drain then sees a
// small first chunk, completes the BEGIN frame inside a sub-1-MiB drain,
// and raises the drain ceiling before it is ever crossed. A single
// send() with a pinned-wide SO_SNDBUF hands the whole batch to the kernel
// in one delivery, so F's first drain carries BEGIN plus the BODY: the
// high-BDP farm shape, made deterministic.
//
// `sent_failed` records the failure instead of throwing: a red run (a
// reverted fix on the F side) can kill this socket's peer (an ERROR
// frame then EOF), and a coroutine that dies mid-dialogue leaves the
// leg's io_context busy until run_for expires -- a hang instead of the
// intended clean failure. Everything after the first recorded failure
// is skipped.
asio::awaitable<void> raw_send_all(tcp::socket& socket,
                                   std::span<const uint8_t> bytes_to_send,
                                   std::atomic<bool>* sent_failed = nullptr) {
    size_t offset = 0;
    while (offset != bytes_to_send.size()) {
        const ssize_t sent = ::send(socket.native_handle(),
                                     bytes_to_send.data() + offset,
                                     bytes_to_send.size() - offset, MSG_NOSIGNAL);
        if (sent <= 0) {
            if (sent_failed != nullptr)
                sent_failed->store(true, std::memory_order_release);
            break;
        }
        offset += static_cast<size_t>(sent);
    }
    co_return;
}

asio::awaitable<Frame> raw_read(tcp::socket& socket, uint32_t max_payload) {
    std::array<uint8_t, 4> raw_header{};
    co_await asio::async_read(socket, asio::buffer(raw_header), asio::use_awaitable);
    const FrameHeader header = decode_frame_header(raw_header, max_payload);
    Frame result{header.type, std::vector<uint8_t>(header.payload_bytes)};
    if (!result.payload.empty())
        co_await asio::async_read(socket, asio::buffer(result.payload), asio::use_awaitable);
    co_return result;
}

asio::awaitable<void> raw_cancel_client_after_hello(
    tcp::acceptor& acceptor, P50ClientEndpoint& client) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    co_await acceptor.async_accept(socket, asio::use_awaitable);
    const Frame hello = co_await raw_read(
        socket, EndpointCaps{}.wire.max_frame_payload);
    require(hello.type == MessageType::SESSION_HELLO,
            "cancellation peer did not observe the first CacheWire frame");
    client.request_cancel_for_test();
    boost::system::error_code ignored;
    socket.close(ignored);
    co_return;
}

template <class T> T raw_decode(const Frame& frame) {
    Message decoded = decode_payload(frame.type, frame.payload);
    T* result = std::get_if<T>(&decoded);
    if (!result)
        throw std::invalid_argument("raw peer received an unexpected message");
    return std::move(*result);
}

struct RawRoute {
    SessionHello hello;
    SessionState state;
};

asio::awaitable<RawRoute> raw_open(tcp::socket& socket, const tcp::endpoint& remote,
                                   CStoreGuid c_guid, SessionLimits limits = {});
asio::awaitable<SessionState> raw_reset(tcp::socket& socket, const RawRoute& open,
                                        HistoryNonce nonce);
asio::awaitable<void> raw_wait_for_close(tcp::socket& socket);

struct HelloRaceState {
    bool first_pending = false;
    bool second_finished = false;
};

asio::awaitable<void> raw_pending_then_finish(tcp::endpoint remote, CStoreGuid c_guid,
                                              std::span<const uint8_t> input,
                                              HelloRaceState& coordination) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    RawRoute open = co_await raw_open(socket, remote, c_guid);
    SessionState route = co_await raw_reset(socket, open, HistoryNonce{101});
    const ZstdTuEnvelope prepared = encode_zstd_tu(
        HistoryNonce{1}, RelSeq{0}, TuSeq{11}, Digest128{}, input);
    const TxBegin begin =
        make_begin(prepared, route.history_nonce, route.state_digest, route.next_rel_seq);
    co_await raw_write(socket, begin);
    coordination.first_pending = true;
    asio::steady_timer timer(executor);
    while (!coordination.second_finished) {
        timer.expires_after(std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
    }
    Message body_message = BodyMessage{prepared.body};
    co_await raw_write(socket, std::move(body_message));
    const TxCommit commit =
        raw_decode<TxCommit>(co_await raw_read(socket, route.limits.max_frame_payload));
    if (commit.transaction_digest != begin.transaction_digest ||
        commit.raw_digest != begin.raw_digest)
        throw std::logic_error("pending fixture received a different TX_COMMIT");
    boost::system::error_code ignored;
    socket.close(ignored);
    co_return;
}

asio::awaitable<void> raw_incompatible_hello(tcp::endpoint remote, CStoreGuid c_guid,
                                             HelloRaceState& coordination) {
    const auto executor = co_await asio::this_coro::executor;
    asio::steady_timer timer(executor);
    while (!coordination.first_pending) {
        timer.expires_after(std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
    }
    tcp::socket socket(executor);
    co_await socket.async_connect(remote, asio::use_awaitable);
    SessionHello hello;
    hello.c_store_guid = c_guid;
    // Use a reserved profile bit so this remains deliberately incompatible
    // when every currently implemented revision-1 profile is enabled.
    hello.supported_profiles = uint32_t{1} << 31;
    co_await raw_write(socket, hello);
    const Frame terminal = co_await raw_read(socket, hello.limits.max_frame_payload);
    coordination.second_finished = true;
    if (terminal.type != MessageType::ERROR)
        throw std::logic_error("incompatible HELLO did not receive terminal ERROR");
    (void)raw_decode<ErrorMessage>(terminal);
    std::array<uint8_t, 1> extra{};
    boost::system::error_code error;
    (void)co_await socket.async_read_some(
        asio::buffer(extra), asio::redirect_error(asio::use_awaitable, error));
    if (error != asio::error::eof && error != asio::error::connection_reset)
        throw std::logic_error("incompatible HELLO connection stayed open");
    co_return;
}

asio::awaitable<void> raw_invalid_first_mutation(tcp::endpoint remote,
                                                 CStoreGuid c_guid,
                                                 HelloRaceState& coordination) {
    const auto executor = co_await asio::this_coro::executor;
    asio::steady_timer timer(executor);
    while (!coordination.first_pending) {
        timer.expires_after(std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
    }
    tcp::socket socket(executor);
    const RawRoute open = co_await raw_open(socket, remote, c_guid);
    Message invalid = BodyMessage{bytes("not a first mutation")};
    co_await raw_write(socket, std::move(invalid));
    const Frame terminal =
        co_await raw_read(socket, open.state.limits.max_frame_payload);
    coordination.second_finished = true;
    if (terminal.type != MessageType::ERROR)
        throw std::logic_error(
            "invalid compatible candidate did not receive terminal ERROR");
    (void)raw_decode<ErrorMessage>(terminal);
    co_await raw_wait_for_close(socket);
}

struct CandidateRevisionRace {
    bool first_staged = false;
    bool route_changed = false;
};

asio::awaitable<void> raw_stale_candidate(tcp::endpoint remote,
                                          CStoreGuid c_guid,
                                          CandidateRevisionRace& coordination) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    const RawRoute open = co_await raw_open(socket, remote, c_guid);
    coordination.first_staged = true;
    asio::steady_timer timer(executor);
    while (!coordination.route_changed) {
        timer.expires_after(std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
    }

    const HistoryNonce replacement{open.state.history_nonce.value + 100};
    Message reset = HistoryReset{
        replacement, initial_route_digest(c_guid, replacement)};
    try {
        co_await raw_write(socket, std::move(reset));
        const Frame reply =
            co_await raw_read(socket, open.state.limits.max_frame_payload);
        if (reply.type == MessageType::ERROR) {
            (void)raw_decode<ErrorMessage>(reply);
            co_return;
        }
        throw std::logic_error("stale candidate received a non-ERROR reply");
    } catch (const boost::system::system_error& error) {
        if (error.code() != asio::error::eof &&
            error.code() != asio::error::connection_reset &&
            error.code() != asio::error::broken_pipe)
            throw;
    }
}

asio::awaitable<void> raw_commit_after_candidate_staged(
    tcp::endpoint remote, CStoreGuid c_guid, std::span<const uint8_t> input,
    CandidateRevisionRace& coordination) {
    const auto executor = co_await asio::this_coro::executor;
    asio::steady_timer timer(executor);
    while (!coordination.first_staged) {
        timer.expires_after(std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
    }
    tcp::socket socket(executor);
    const RawRoute open = co_await raw_open(socket, remote, c_guid);
    if (!open.state.route_present)
        throw std::logic_error("candidate-revision fixture has no established route");
    const ZstdTuEnvelope prepared = encode_zstd_tu(
        HistoryNonce{1}, RelSeq{0}, TuSeq{15}, Digest128{}, input);
    const TxBegin begin = make_begin(prepared, open.state.history_nonce,
                                     open.state.state_digest,
                                     open.state.next_rel_seq);
    co_await raw_write(socket, begin);
    Message body = BodyMessage{prepared.body};
    co_await raw_write(socket, std::move(body));
    const TxCommit commit = raw_decode<TxCommit>(
        co_await raw_read(socket, open.state.limits.max_frame_payload));
    if (commit.transaction_digest != begin.transaction_digest ||
        commit.raw_digest != begin.raw_digest)
        throw std::logic_error("candidate-revision fixture received another commit");
    boost::system::error_code ignored;
    socket.close(ignored);
    coordination.route_changed = true;
}

struct InterruptedRawTu {
    TxBegin begin;
    std::vector<uint8_t> body;
};

enum class TerminalBodyFailure {
    ZeroProgress,
    Excess,
    ComponentDigest,
    ZstdDecode,
    RawDigest,
};

asio::awaitable<InterruptedRawTu> raw_partial_body_disconnect(
    tcp::endpoint remote, CStoreGuid c_guid, std::span<const uint8_t> input) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    RawRoute open = co_await raw_open(socket, remote, c_guid);
    SessionState route = open.state;
    if (!route.route_present)
        route = co_await raw_reset(socket, open, HistoryNonce{121});
    ZstdTuEnvelope prepared = encode_zstd_tu(
        HistoryNonce{1}, RelSeq{0}, TuSeq{21}, Digest128{}, input);
    TxBegin begin = make_begin(prepared, route.history_nonce,
                               route.state_digest, route.next_rel_seq);
    co_await raw_write(socket, begin);
    if (prepared.body.size() < 2)
        throw std::logic_error("partial-BODY fixture encoded fewer than two bytes");
    BodyMessage partial;
    partial.bytes.assign(prepared.body.begin(),
                         prepared.body.begin() + prepared.body.size() / 2);
    Message partial_message = std::move(partial);
    co_await raw_write(socket, std::move(partial_message));
    boost::system::error_code ignored;
    socket.close(ignored);
    co_return InterruptedRawTu{std::move(begin), std::move(prepared.body)};
}

asio::awaitable<InterruptedRawTu> raw_terminal_body_failure(
    tcp::endpoint remote, CStoreGuid c_guid, std::span<const uint8_t> input,
    TerminalBodyFailure scenario, bool receive_terminal = true) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    RawRoute open = co_await raw_open(socket, remote, c_guid);
    SessionState route = open.state;
    if (!route.route_present)
        route = co_await raw_reset(socket, open, HistoryNonce{701});
    ZstdTuEnvelope prepared = encode_zstd_tu(
        HistoryNonce{1}, RelSeq{0}, TuSeq{81}, Digest128{}, input);
    TxBegin begin = make_begin(prepared, route.history_nonce,
                               route.state_digest, route.next_rel_seq);
    std::vector<uint8_t> sent_body = prepared.body;
    if (scenario == TerminalBodyFailure::ZstdDecode) {
        std::fill(sent_body.begin(), sent_body.end(), uint8_t{0});
        begin.body = describe_component(kZstdTuBodyEncoding, sent_body,
                                        begin.raw_bytes);
        begin.transaction_digest = compute_transaction_digest(begin, sent_body);
        prepared.body = sent_body;
    } else if (scenario == TerminalBodyFailure::RawDigest) {
        begin.raw_digest.bytes[0] ^= 0x80;
        begin.transaction_digest = compute_transaction_digest(begin, sent_body);
    }

    co_await raw_write(socket, begin);
    switch (scenario) {
    case TerminalBodyFailure::ZeroProgress:
        sent_body.clear();
        break;
    case TerminalBodyFailure::Excess:
        sent_body.push_back(0xff);
        break;
    case TerminalBodyFailure::ComponentDigest:
        if (sent_body.empty())
            throw std::logic_error("component-digest fixture encoded an empty BODY");
        sent_body.front() ^= 0x80;
        break;
    case TerminalBodyFailure::ZstdDecode:
    case TerminalBodyFailure::RawDigest:
        break;
    }
    Message body_message = BodyMessage{std::move(sent_body)};
    co_await raw_write(socket, std::move(body_message));
    if (!receive_terminal) {
        boost::system::error_code ignored;
        socket.close(ignored);
        co_return InterruptedRawTu{std::move(begin), std::move(prepared.body)};
    }
    const Frame terminal =
        co_await raw_read(socket, route.limits.max_frame_payload);
    if (terminal.type != MessageType::ERROR)
        throw std::logic_error("terminal BODY fixture did not receive ERROR");
    (void)raw_decode<ErrorMessage>(terminal);
    co_await raw_wait_for_close(socket);
    co_return InterruptedRawTu{std::move(begin), std::move(prepared.body)};
}

asio::awaitable<void> raw_different_begin_rejected(
    tcp::endpoint remote, CStoreGuid c_guid, const TxBegin& interrupted) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    const RawRoute open = co_await raw_open(socket, remote, c_guid);
    const std::vector<uint8_t> different_input =
        pseudo_random_bytes(4097);
    const ZstdTuEnvelope different = encode_zstd_tu(
        HistoryNonce{1}, RelSeq{0}, TuSeq{interrupted.tu_seq.value + 1},
        Digest128{}, different_input);
    const TxBegin begin = make_begin(different, open.state.history_nonce,
                                     open.state.state_digest,
                                     open.state.next_rel_seq);
    if (begin == interrupted)
        throw std::logic_error("different-begin fixture reproduced the same identity");
    co_await raw_write(socket, begin);
    const Frame terminal =
        co_await raw_read(socket, open.state.limits.max_frame_payload);
    if (terminal.type != MessageType::ERROR)
        throw std::logic_error("different begin did not receive terminal ERROR");
    (void)raw_decode<ErrorMessage>(terminal);
    co_await raw_wait_for_close(socket);
}

asio::awaitable<void> raw_exact_whole_tu_replay(
    tcp::endpoint remote, CStoreGuid c_guid, const InterruptedRawTu& interrupted) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    const RawRoute open = co_await raw_open(socket, remote, c_guid);
    if (open.state.history_nonce != interrupted.begin.history_nonce ||
        open.state.next_rel_seq != interrupted.begin.rel_seq ||
        open.state.state_digest != interrupted.begin.pre_state_digest)
        throw std::logic_error("interrupted route cursor changed before exact replay");
    co_await raw_write(socket, interrupted.begin);
    Message body = BodyMessage{interrupted.body};
    co_await raw_write(socket, std::move(body));
    const TxCommit commit = raw_decode<TxCommit>(
        co_await raw_read(socket, open.state.limits.max_frame_payload));
    if (commit.transaction_digest != interrupted.begin.transaction_digest ||
        commit.raw_digest != interrupted.begin.raw_digest)
        throw std::logic_error("exact replay received a different commit identity");
    boost::system::error_code ignored;
    socket.close(ignored);
}

asio::awaitable<void> raw_exact_whole_tu_rejected(
    tcp::endpoint remote, CStoreGuid c_guid,
    const InterruptedRawTu& interrupted) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    const RawRoute open = co_await raw_open(socket, remote, c_guid);
    if (open.state.history_nonce != interrupted.begin.history_nonce ||
        open.state.next_rel_seq != interrupted.begin.rel_seq ||
        open.state.state_digest != interrupted.begin.pre_state_digest)
        throw std::logic_error(
            "deterministic-failure route cursor changed before exact replay");
    co_await raw_write(socket, interrupted.begin);
    Message body_message = BodyMessage{interrupted.body};
    co_await raw_write(socket, std::move(body_message));
    const Frame terminal =
        co_await raw_read(socket, open.state.limits.max_frame_payload);
    if (terminal.type != MessageType::ERROR)
        throw std::logic_error(
            "deterministic-failure exact replay did not receive ERROR");
    (void)raw_decode<ErrorMessage>(terminal);
    co_await raw_wait_for_close(socket);
}

asio::awaitable<RawRoute> raw_open(tcp::socket& socket, const tcp::endpoint& remote,
                                   CStoreGuid c_guid, SessionLimits limits) {
    co_await socket.async_connect(remote, asio::use_awaitable);
    SessionHello hello;
    hello.c_store_guid = c_guid;
    hello.supported_profiles = profile_bit(ProfileId::ZSTD_TU);
    hello.limits = limits;
    co_await raw_write(socket, hello);
    SessionState state =
        raw_decode<SessionState>(co_await raw_read(socket, limits.max_frame_payload));
    validate_session_state(hello, state);
    co_return RawRoute{hello, state};
}

asio::awaitable<SessionState> raw_reset(tcp::socket& socket, const RawRoute& open,
                                        HistoryNonce nonce) {
    const HistoryReset reset{nonce, initial_route_digest(open.hello.c_store_guid, nonce)};
    co_await raw_write(socket, reset);
    SessionState state =
        raw_decode<SessionState>(co_await raw_read(socket, open.state.limits.max_frame_payload));
    if (!state.route_present || state.history_nonce != nonce || state.next_rel_seq.value != 0 ||
        state.state_digest != reset.initial_state_digest)
        throw std::logic_error("raw HISTORY_RESET acknowledgement differs");
    co_return state;
}

asio::awaitable<void> raw_reset_only(tcp::endpoint remote, CStoreGuid c_guid, HistoryNonce nonce,
                                     SessionLimits limits = {}) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    const RawRoute open = co_await raw_open(socket, remote, c_guid, limits);
    (void)co_await raw_reset(socket, open, nonce);
    boost::system::error_code ignored;
    socket.close(ignored);
    co_return;
}

asio::awaitable<bool> raw_reset_rejected(tcp::endpoint remote, CStoreGuid c_guid,
                                         HistoryNonce nonce, SessionLimits limits = {}) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    const RawRoute open = co_await raw_open(socket, remote, c_guid, limits);
    co_await raw_write(socket,
                       HistoryReset{nonce, initial_route_digest(c_guid, nonce)});
    const Frame reply = co_await raw_read(socket, open.state.limits.max_frame_payload);
    co_return reply.type == MessageType::ERROR;
}

asio::awaitable<void> raw_wait_for_close(tcp::socket& socket) {
    std::array<uint8_t, 1> extra{};
    boost::system::error_code error;
    (void)co_await socket.async_read_some(
        asio::buffer(extra), asio::redirect_error(asio::use_awaitable, error));
    if (error != asio::error::eof && error != asio::error::connection_reset)
        throw std::logic_error("client did not close after its terminal result");
}

enum class ResetAckMutation {
    WireRevision,
    ProfileMask,
    FrameLimit,
    FillLimit,
};

struct ScriptedSessionState {
    FStoreGuid f_guid{};
    bool route_present = false;
    HistoryNonce history_nonce{};
    RelSeq next_rel_seq{};
    Digest128 state_digest{};
};

asio::awaitable<void> raw_bad_reset_ack_peer(tcp::acceptor& acceptor,
                                             ScriptedSessionState scripted,
                                             ResetAckMutation mutation) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    co_await acceptor.async_accept(socket, asio::use_awaitable);
    const SessionHello hello = raw_decode<SessionHello>(
        co_await raw_read(socket, kInitialMaxFramePayload));

    SessionState initial;
    initial.wire_revision = kP50WireRevision;
    initial.negotiated_profiles = profile_bit(ProfileId::ZSTD_TU);
    initial.limits = hello.limits;
    initial.f_store_guid = scripted.f_guid;
    initial.namespace_present = scripted.route_present;
    initial.route_present = scripted.route_present;
    initial.history_nonce = scripted.history_nonce;
    initial.next_rel_seq = scripted.next_rel_seq;
    initial.state_digest = scripted.state_digest;
    co_await raw_write(socket, initial);

    const HistoryReset reset = raw_decode<HistoryReset>(
        co_await raw_read(socket, initial.limits.max_frame_payload));
    SessionState ack = initial;
    ack.namespace_present = true;
    ack.route_present = true;
    ack.history_nonce = reset.history_nonce;
    ack.next_rel_seq = RelSeq{0};
    ack.state_digest = reset.initial_state_digest;
    if (mutation == ResetAckMutation::ProfileMask) {
        // Use the other runnable source profile so the peer can encode the
        // state and the client, rather than the fixture itself, observes the
        // negotiated-profile mismatch.
        ack.negotiated_profiles = profile_bit(ProfileId::ZSTD_ROUTE);
        co_await raw_write(socket, ack);
    } else if (mutation == ResetAckMutation::FrameLimit) {
        require(ack.limits.max_frame_payload > kMandatoryControlFramePayload,
                "reset-ack frame-limit fixture lacks room to decrease");
        --ack.limits.max_frame_payload;
        co_await raw_write(socket, ack);
    } else if (mutation == ResetAckMutation::FillLimit) {
        require(ack.limits.max_fill_record_bytes > 32,
                "reset-ack FILL-limit fixture lacks room to decrease");
        --ack.limits.max_fill_record_bytes;
        co_await raw_write(socket, ack);
    } else {
        std::vector<uint8_t> frame = encode_frame(ack);
        frame[4] = 0;
        frame[5] = static_cast<uint8_t>(kP50WireRevision + 1);
        co_await raw_write_bytes(socket, frame);
    }
    co_await raw_wait_for_close(socket);
}

asio::awaitable<void> raw_bounded_error_peer(tcp::acceptor& acceptor, uint32_t payload_cap) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    co_await acceptor.async_accept(socket, asio::use_awaitable);
    const SessionHello hello = raw_decode<SessionHello>(
        co_await raw_read(socket, kInitialMaxFramePayload));
    if (hello.limits.max_frame_payload != payload_cap || payload_cap < 6)
        throw std::logic_error("bounded terminal fixture negotiated an unexpected cap");
    Message terminal = ErrorMessage{91, std::string(payload_cap - 6, 'e')};
    co_await raw_write(socket, std::move(terminal));
    co_await raw_wait_for_close(socket);
}

asio::awaitable<void> raw_unknown_frame_peer(tcp::acceptor& acceptor) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    co_await acceptor.async_accept(socket, asio::use_awaitable);
    (void)raw_decode<SessionHello>(
        co_await raw_read(socket, kInitialMaxFramePayload));
    const std::array<uint8_t, 4> unknown_header{0xff, 0, 0, 0};
    co_await raw_write_bytes(socket, unknown_header);
    co_await raw_wait_for_close(socket);
}

asio::awaitable<void> raw_route_mismatch_peer(tcp::acceptor& acceptor,
                                              ScriptedSessionState scripted,
                                              bool expect_error = true) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    co_await acceptor.async_accept(socket, asio::use_awaitable);
    const SessionHello hello = raw_decode<SessionHello>(
        co_await raw_read(socket, kInitialMaxFramePayload));

    SessionState state;
    state.wire_revision = kP50WireRevision;
    state.negotiated_profiles = profile_bit(ProfileId::ZSTD_TU);
    state.limits = hello.limits;
    state.f_store_guid = scripted.f_guid;
    state.namespace_present = true;
    state.route_present = true;
    state.history_nonce = scripted.history_nonce;
    state.next_rel_seq = scripted.next_rel_seq;
    state.state_digest = scripted.state_digest;
    co_await raw_write(socket, state);
    if (expect_error) {
        const ErrorMessage terminal = raw_decode<ErrorMessage>(
            co_await raw_read(socket, state.limits.max_frame_payload));
        if (terminal.code == 0)
            throw std::logic_error("route mismatch received a zero-code ERROR");
    }
    co_await raw_wait_for_close(socket);
}

asio::awaitable<void> raw_zero_f_store_state_peer(tcp::acceptor& acceptor) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    co_await acceptor.async_accept(socket, asio::use_awaitable);
    const SessionHello hello = raw_decode<SessionHello>(
        co_await raw_read(socket, kInitialMaxFramePayload));

    SessionState state;
    state.wire_revision = kP50WireRevision;
    state.negotiated_profiles = profile_bit(ProfileId::ZSTD_TU);
    state.limits = hello.limits;
    state.f_store_guid = Id128::from_u64(6000);
    std::vector<uint8_t> frame = encode_frame(Message{state});
    constexpr size_t frame_header_bytes = 4;
    constexpr size_t f_guid_payload_offset = 2 + 4 + 4 + 8;
    std::fill_n(frame.begin() + frame_header_bytes + f_guid_payload_offset,
                16, uint8_t{0});
    co_await raw_write_bytes(socket, frame);
    co_await raw_wait_for_close(socket);
}

asio::awaitable<void> raw_zero_c_store_hello(tcp::endpoint remote) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    co_await socket.async_connect(remote, asio::use_awaitable);
    SessionHello hello;
    hello.c_store_guid = Id128::from_u64(6001);
    hello.supported_profiles = profile_bit(ProfileId::ZSTD_TU);
    std::vector<uint8_t> frame = encode_frame(Message{hello});
    constexpr size_t frame_header_bytes = 4;
    constexpr size_t c_guid_payload_offset = 2;
    std::fill_n(frame.begin() + frame_header_bytes + c_guid_payload_offset,
                16, uint8_t{0});
    co_await raw_write_bytes(socket, frame);
    const ErrorMessage terminal = raw_decode<ErrorMessage>(
        co_await raw_read(socket, kInitialMaxFramePayload));
    if (terminal.code == 0)
        throw std::logic_error("zero C_STORE_GUID received a zero-code ERROR");
    co_await raw_wait_for_close(socket);
}

asio::awaitable<void> raw_zero_error_code_peer(tcp::acceptor& acceptor) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    co_await acceptor.async_accept(socket, asio::use_awaitable);
    (void)raw_decode<SessionHello>(
        co_await raw_read(socket, kInitialMaxFramePayload));

    std::vector<uint8_t> frame =
        encode_frame(Message{ErrorMessage{1, "reserved-code fixture"}});
    frame[4] = 0;
    frame[5] = 0;
    co_await raw_write_bytes(socket, frame);
    co_await raw_wait_for_close(socket);
}

asio::awaitable<void> raw_stall_after_connect(tcp::acceptor& acceptor) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    boost::system::error_code error;
    co_await acceptor.async_accept(socket, asio::redirect_error(asio::use_awaitable, error));
    if (error)
        co_return;
    std::array<uint8_t, 1> byte{};
    while (!error)
        co_await socket.async_read_some(asio::buffer(byte),
                                       asio::redirect_error(asio::use_awaitable, error));
    co_return;
}

asio::awaitable<void> raw_zero_history_reset(tcp::endpoint remote,
                                              CStoreGuid c_guid) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    const RawRoute open = co_await raw_open(socket, remote, c_guid);
    const HistoryReset valid{
        HistoryNonce{1}, initial_route_digest(c_guid, HistoryNonce{1})};
    std::vector<uint8_t> frame = encode_frame(Message{valid});
    std::fill_n(frame.begin() + 4, sizeof(uint64_t), uint8_t{0});
    co_await raw_write_bytes(socket, frame);
    const ErrorMessage terminal = raw_decode<ErrorMessage>(
        co_await raw_read(socket, open.state.limits.max_frame_payload));
    if (terminal.code == 0)
        throw std::logic_error("zero HISTORY_NONCE received a zero-code ERROR");
    co_await raw_wait_for_close(socket);
}

template <class Peer>
ClientRunResult run_client_with_raw_peer(P50ClientEndpoint& client,
                                         PreparedTuHandle prepared, Peer peer,
                                         EndpointIoControl control = {}) {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    std::future<void> peer_result =
        asio::co_spawn(context, peer(acceptor), asio::use_future);
    std::future<ClientRunResult> client_result = asio::co_spawn(
        context, client.run(acceptor.local_endpoint(), prepared, control),
        asio::use_future);
    context.run();
    peer_result.get();
    return client_result.get();
}

enum class RawCase {
    ShortBody,
    ExcessBody,
    EmptyBodyProgress,
    DescriptorCap,
    DecodedCap,
    FrameCap,
    ComponentDigest,
    TransactionDigest,
    RawDigest,
    WrongProfile,
    WrongEncoding,
    FrameContentSize,
    TrailingByte,
    AppendedEmptyFrame,
    AppendedNonemptyFrame,
};

struct RawCaseResult {
    bool received_error = false;
    bool closed_after_error = false;
    size_t error_payload_bytes = 0;
};

asio::awaitable<RawCaseResult> run_raw_case(tcp::endpoint remote, CStoreGuid c_guid,
                                            EndpointCaps server_caps, RawCase scenario) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    RawRoute open = co_await raw_open(socket, remote, c_guid, server_caps.wire);
    SessionState route = open.state;
    if (!route.route_present)
        route = co_await raw_reset(socket, open, HistoryNonce{71});

    if (scenario == RawCase::FrameCap) {
        const auto header =
            encode_frame_header(MessageType::TX_BEGIN, server_caps.wire.max_frame_payload + 1);
        co_await raw_write_bytes(socket, header);
    } else {
        const std::vector<uint8_t> raw = pseudo_random_bytes(512);
        const ZstdTuEnvelope prepared = encode_zstd_tu(
            HistoryNonce{1}, RelSeq{0}, TuSeq{9}, Digest128{}, raw);
        TxBegin begin =
            make_begin(prepared, route.history_nonce, route.state_digest, route.next_rel_seq);
        std::vector<uint8_t> body = prepared.body;
        std::vector<uint8_t> raw_begin_frame;
        if (scenario == RawCase::DescriptorCap) {
            body.assign(static_cast<size_t>(server_caps.zstd.max_encoded_body_bytes + 1),
                        uint8_t{0x5a});
            begin.body = describe_component(kZstdTuBodyEncoding, body, 1);
            begin.raw_bytes = 1;
            const std::array<uint8_t, 1> one_raw{0};
            begin.raw_digest = icecc::digest128(one_raw);
            begin.transaction_digest = compute_transaction_digest(begin, body);
        } else if (scenario == RawCase::DecodedCap) {
            begin.raw_bytes = server_caps.zstd.max_raw_bytes + 1;
            begin.body.decoded_bytes = begin.raw_bytes;
            begin.transaction_digest = compute_transaction_digest(begin, body);
        } else if (scenario == RawCase::WrongProfile) {
            raw_begin_frame = encode_frame(begin);
            raw_begin_frame[4 + 24] = 0;
            raw_begin_frame[4 + 25] = static_cast<uint8_t>(ProfileId::P29V1);
        } else if (scenario == RawCase::WrongEncoding) {
            begin.body.encoding = 77;
            begin.transaction_digest = compute_transaction_digest(begin, body);
        } else if (scenario == RawCase::ComponentDigest) {
            begin.body.digest.bytes[0] ^= 0x80;
        } else if (scenario == RawCase::TransactionDigest) {
            begin.transaction_digest.bytes[0] ^= 0x80;
        } else if (scenario == RawCase::RawDigest) {
            begin.raw_digest.bytes[0] ^= 0x80;
            begin.transaction_digest = compute_transaction_digest(begin, body);
        } else if (scenario == RawCase::FrameContentSize) {
            ++begin.raw_bytes;
            begin.body.decoded_bytes = begin.raw_bytes;
            begin.transaction_digest = compute_transaction_digest(begin, body);
        } else if (scenario == RawCase::TrailingByte) {
            body.push_back(0xa5);
            begin.body =
                describe_component(kZstdTuBodyEncoding, body, begin.raw_bytes);
            begin.transaction_digest = compute_transaction_digest(begin, body);
        } else if (scenario == RawCase::AppendedEmptyFrame) {
            const std::vector<uint8_t> appended = standalone_zstd_frame({});
            body.insert(body.end(), appended.begin(), appended.end());
            begin.body =
                describe_component(kZstdTuBodyEncoding, body, begin.raw_bytes);
            begin.transaction_digest = compute_transaction_digest(begin, body);
        } else if (scenario == RawCase::AppendedNonemptyFrame) {
            const std::vector<uint8_t> appended =
                standalone_zstd_frame(bytes("second frame"));
            body.insert(body.end(), appended.begin(), appended.end());
            begin.body =
                describe_component(kZstdTuBodyEncoding, body, begin.raw_bytes);
            begin.transaction_digest = compute_transaction_digest(begin, body);
        }
        if (raw_begin_frame.empty())
            co_await raw_write(socket, begin);
        else
            co_await raw_write_bytes(socket, raw_begin_frame);
        if (scenario == RawCase::DescriptorCap || scenario == RawCase::DecodedCap ||
            scenario == RawCase::WrongProfile ||
            scenario == RawCase::WrongEncoding) {
            // The descriptor itself is rejected before any component allocation.
        } else {
            if (scenario == RawCase::ShortBody) {
                require(body.size() > 1, "short-body fixture is too small");
                body.pop_back();
                BodyMessage short_body{body};
                co_await raw_write(socket, short_body);
                boost::system::error_code ignored;
                socket.close(ignored);
                co_return RawCaseResult{};
            }
            if (scenario == RawCase::ExcessBody)
                body.push_back(0xff);
            if (scenario == RawCase::EmptyBodyProgress)
                body.clear();
            Message body_message = BodyMessage{std::move(body)};
            co_await raw_write(socket, std::move(body_message));
        }
    }

    RawCaseResult result;
    Frame terminal = co_await raw_read(socket, server_caps.wire.max_frame_payload);
    result.received_error = terminal.type == MessageType::ERROR;
    result.error_payload_bytes = terminal.payload.size();
    if (result.received_error)
        (void)raw_decode<ErrorMessage>(terminal);
    std::array<uint8_t, 4> next{};
    boost::system::error_code error;
    (void)co_await asio::async_read(socket, asio::buffer(next),
                                    asio::redirect_error(asio::use_awaitable, error));
    result.closed_after_error = error == asio::error::eof || error == asio::error::connection_reset;
    co_return result;
}

std::pair<RawCaseResult, ServerRunResult> run_raw_server_case(EndpointCaps caps, RawCase scenario,
                                                              uint64_t c_id) {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    P50ServerEndpoint server(Id128::from_u64(900 + c_id), caps);
    std::future<ServerRunResult> server_future =
        asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
    std::future<RawCaseResult> peer_future = asio::co_spawn(
        context, run_raw_case(acceptor.local_endpoint(), Id128::from_u64(c_id), caps, scenario),
        asio::use_future);
    context.run();
    return {peer_future.get(), server_future.get()};
}

ServerRunResult force_route_reset(P50ServerEndpoint& server, CStoreGuid c_guid,
                                  HistoryNonce nonce) {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    std::future<ServerRunResult> server_future =
        asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
    std::future<void> peer_future = asio::co_spawn(
        context, raw_reset_only(acceptor.local_endpoint(), c_guid, nonce), asio::use_future);
    context.run();
    peer_future.get();
    return server_future.get();
}


bool force_rejected_route_reset(P50ServerEndpoint& server, CStoreGuid c_guid,
                                HistoryNonce nonce) {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    std::future<ServerRunResult> server_future =
        asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
    std::future<bool> peer_future = asio::co_spawn(
        context, raw_reset_rejected(acceptor.local_endpoint(), c_guid, nonce), asio::use_future);
    context.run();
    const bool rejected = peer_future.get();
    require(server_future.get().status == ServerRunStatus::TerminalError,
            "invalid route reset did not terminate the F session");
    return rejected;
}

std::pair<InterruptedRawTu, ServerRunResult> force_terminal_body_failure(
    P50ServerEndpoint& server, CStoreGuid c_guid,
    std::span<const uint8_t> input, TerminalBodyFailure scenario,
    bool receive_terminal = true) {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    std::future<ServerRunResult> server_future =
        asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
    std::future<InterruptedRawTu> peer_future = asio::co_spawn(
        context,
        raw_terminal_body_failure(acceptor.local_endpoint(), c_guid, input,
                                  scenario, receive_terminal),
        asio::use_future);
    context.run();
    InterruptedRawTu interrupted = peer_future.get();
    return {std::move(interrupted), server_future.get()};
}

ServerRunResult force_different_begin_rejected(
    P50ServerEndpoint& server, CStoreGuid c_guid, const TxBegin& interrupted) {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    std::future<ServerRunResult> server_future =
        asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
    std::future<void> peer_future = asio::co_spawn(
        context,
        raw_different_begin_rejected(acceptor.local_endpoint(), c_guid,
                                     interrupted),
        asio::use_future);
    context.run();
    peer_future.get();
    return server_future.get();
}

ServerRunResult force_exact_whole_tu_replay(
    P50ServerEndpoint& server, CStoreGuid c_guid,
    const InterruptedRawTu& interrupted, bool expect_commit) {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    std::future<ServerRunResult> server_future =
        asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
    std::future<void> peer_future;
    if (expect_commit) {
        peer_future = asio::co_spawn(
            context,
            raw_exact_whole_tu_replay(acceptor.local_endpoint(), c_guid,
                                      interrupted),
            asio::use_future);
    } else {
        peer_future = asio::co_spawn(
            context,
            raw_exact_whole_tu_rejected(acceptor.local_endpoint(), c_guid,
                                        interrupted),
            asio::use_future);
    }
    context.run();
    peer_future.get();
    return server_future.get();
}

void test_normal_zero_and_completion_stamps() {
    CompletionLog completions;
    ActionTrace actions;
    P50ServerEndpoint server(Id128::from_u64(200), {}, &completions, &actions);
    TestClient client(Id128::from_u64(100), {}, HistoryNonce{10}, &completions, &actions);
    const std::vector<uint8_t> input = pseudo_random_bytes(4096);
    const PairResult first = run_pair(client, server, admit(client, input));
    require(first.client.status == ClientRunStatus::Committed &&
                first.server.status == ServerRunStatus::Completed,
            "normal loopback did not complete");
    require(first.client.reconnect == EndpointReconnectOutcome::ColdFStore,
            "first loopback did not take the explicit cold-F path");
    require(copy_input(server, client.c_store_guid()) == input,
            "normal loopback did not retain exact input");

    const std::vector<uint8_t> empty;
    const PairResult second = run_pair(client, server, admit(client, empty));
    require(second.client.status == ClientRunStatus::Committed &&
                second.client.reconnect == EndpointReconnectOutcome::ExactMatch,
            "zero-length component transaction did not complete on the exact route");
    require(copy_input(server, client.c_store_guid()) == empty,
            "zero-length ZSTD_TU did not materialize exactly");

    size_t bound_c = 0;
    size_t bound_f = 0;
    for (const AsyncCompletion& completion : completions.completions()) {
        require(completion.stamp.session_serial != 0,
                "async completion omitted its session serial");
        (void)async_operation_name(completion.stamp.operation);
        if (completion.stamp.transaction_bound) {
            require(completion.stamp.history_nonce.value != 0 &&
                        nonzero(completion.stamp.transaction_digest),
                    "transaction-bound completion omitted route identity");
            if (completion.stamp.actor == ActorSide::C)
                ++bound_c;
            else
                ++bound_f;
        }
    }
    require(bound_c != 0 && bound_f != 0,
            "loopback did not capture full transaction identity on both sides");
    require_trace(actions, "normal/zero completion trace");
}

enum class CompletionStampField {
    Actor,
    Operation,
    CStoreGuid,
    FStoreGuid,
    SessionSerial,
    HistoryNonce,
    RelSeq,
    TuSeq,
    TransactionDigest,
    RawDigest,
    TransactionBound,
};

void change_completion_field(CompletionStamp& stamp, CompletionStampField field) {
    switch (field) {
    case CompletionStampField::Actor:
        stamp.actor = stamp.actor == ActorSide::C ? ActorSide::F : ActorSide::C;
        break;
    case CompletionStampField::Operation:
        stamp.operation = stamp.operation == AsyncOperationKind::WriteFragment
                              ? AsyncOperationKind::ReadPayload
                              : AsyncOperationKind::WriteFragment;
        break;
    case CompletionStampField::CStoreGuid:
        stamp.c_store_guid.bytes[0] ^= 0x80;
        break;
    case CompletionStampField::FStoreGuid:
        stamp.f_store_guid.bytes[0] ^= 0x80;
        break;
    case CompletionStampField::SessionSerial:
        ++stamp.session_serial;
        break;
    case CompletionStampField::HistoryNonce:
        ++stamp.history_nonce.value;
        break;
    case CompletionStampField::RelSeq:
        ++stamp.rel_seq.value;
        break;
    case CompletionStampField::TuSeq:
        ++stamp.tu_seq.value;
        break;
    case CompletionStampField::TransactionDigest:
        stamp.transaction_digest.bytes[0] ^= 0x80;
        break;
    case CompletionStampField::RawDigest:
        stamp.raw_digest.bytes[0] ^= 0x80;
        break;
    case CompletionStampField::TransactionBound:
        stamp.transaction_bound = !stamp.transaction_bound;
        break;
    }
}

void change_live_identity_field(CompletionLiveIdentity& identity,
                                CompletionStampField field) {
    switch (field) {
    case CompletionStampField::Actor:
        identity.actor = identity.actor == ActorSide::C ? ActorSide::F : ActorSide::C;
        break;
    case CompletionStampField::Operation:
        fail("operation is not part of the live endpoint identity");
    case CompletionStampField::CStoreGuid:
        identity.c_store_guid.bytes[0] ^= 0x80;
        break;
    case CompletionStampField::FStoreGuid:
        identity.f_store_guid.bytes[0] ^= 0x80;
        break;
    case CompletionStampField::SessionSerial:
        ++identity.session_serial;
        break;
    case CompletionStampField::HistoryNonce:
        ++identity.history_nonce.value;
        break;
    case CompletionStampField::RelSeq:
        ++identity.rel_seq.value;
        break;
    case CompletionStampField::TuSeq:
        ++identity.tu_seq.value;
        break;
    case CompletionStampField::TransactionDigest:
        identity.transaction_digest.bytes[0] ^= 0x80;
        break;
    case CompletionStampField::RawDigest:
        identity.raw_digest.bytes[0] ^= 0x80;
        break;
    case CompletionStampField::TransactionBound:
        identity.transaction_bound = !identity.transaction_bound;
        break;
    }
}

void test_completion_stamp_correspondence() {
    const std::array fields{
        std::pair{CompletionStampField::Actor, "actor"},
        std::pair{CompletionStampField::Operation, "operation"},
        std::pair{CompletionStampField::CStoreGuid, "C_STORE_GUID"},
        std::pair{CompletionStampField::FStoreGuid, "F_STORE_GUID"},
        std::pair{CompletionStampField::SessionSerial, "session serial"},
        std::pair{CompletionStampField::HistoryNonce, "HISTORY_NONCE"},
        std::pair{CompletionStampField::RelSeq, "REL_SEQ"},
        std::pair{CompletionStampField::TuSeq, "TU_SEQ"},
        std::pair{CompletionStampField::TransactionDigest, "transaction digest"},
        std::pair{CompletionStampField::RawDigest, "raw digest"},
        std::pair{CompletionStampField::TransactionBound, "transaction-bound flag"},
    };

    uint64_t identity = 3000;
    for (const ActorSide actor : {ActorSide::C, ActorSide::F}) {
        const AsyncOperationKind target = actor == ActorSide::C
                                              ? AsyncOperationKind::WriteFragment
                                              : AsyncOperationKind::ReadPayload;
        for (const auto& [field, name] : fields) {
            const CompletionStampField selected_field = field;
            const std::string_view field_name = name;
            P50ServerEndpoint server(Id128::from_u64(identity++));
            TestClient client(Id128::from_u64(identity++));
            const std::vector<uint8_t> input = pseudo_random_bytes(4096);
            bool changed = false;
            EndpointIoControl client_control;
            EndpointIoControl server_control;
            EndpointIoControl& selected =
                actor == ActorSide::C ? client_control : server_control;
            selected.before_completion_check = [&](CompletionStamp& observed) {
                if (changed || observed.actor != actor ||
                    observed.operation != target || !observed.transaction_bound)
                    return;
                change_completion_field(observed, selected_field);
                changed = true;
            };

            const PairResult rejected = run_pair(client, server, admit(client, input),
                                                 client_control, server_control);
            std::string context = actor == ActorSide::C ? "C " : "F ";
            context.append(field_name);
            require(changed, context + " completion mutation did not run");
            require(rejected.client.status == ClientRunStatus::Disconnected &&
                        rejected.server.status == ServerRunStatus::Disconnected &&
                        client.has_active_transaction() &&
                        !copy_input(server, client.c_store_guid()),
                    context + " completion mutation was not rejected exactly");

            const PairResult replayed = run_pair(client, server);
            require(replayed.client.status == ClientRunStatus::Committed &&
                        replayed.server.status == ServerRunStatus::Completed &&
                        replayed.client.reconnect == EndpointReconnectOutcome::ExactMatch &&
                        !client.has_active_transaction() &&
                        copy_input(server, client.c_store_guid()) == input,
                    context + " completion rejection did not permit exact follow-up");
        }
    }

    P50ServerEndpoint server(Id128::from_u64(identity++));
    TestClient client(Id128::from_u64(identity++));
    bool saw_unbound_c = false;
    bool saw_unbound_f = false;
    EndpointIoControl client_control;
    EndpointIoControl server_control;
    client_control.before_completion_check = [&](CompletionStamp& observed) {
        saw_unbound_c = saw_unbound_c ||
                        (observed.actor == ActorSide::C && !observed.transaction_bound);
    };
    server_control.before_completion_check = [&](CompletionStamp& observed) {
        saw_unbound_f = saw_unbound_f ||
                        (observed.actor == ActorSide::F && !observed.transaction_bound);
    };
    const std::vector<uint8_t> first_input = pseudo_random_bytes(1024);
    const PairResult first = run_pair(client, server, admit(client, first_input),
                                      client_control, server_control);
    require(first.client.status == ClientRunStatus::Committed && saw_unbound_c && saw_unbound_f,
            "legal non-transaction completions did not complete on both endpoints");
    const std::vector<uint8_t> second_input = pseudo_random_bytes(1536);
    const PairResult follow_up = run_pair(client, server, admit(client, second_input));
    require(follow_up.client.status == ClientRunStatus::Committed &&
                follow_up.server.status == ServerRunStatus::Completed &&
                follow_up.client.reconnect == EndpointReconnectOutcome::ExactMatch &&
                copy_input(server, client.c_store_guid()) == second_input,
            "legal non-transaction completions did not preserve exact follow-up");
}

void test_completion_live_identity_correspondence() {
    const std::array fields{
        std::pair{CompletionStampField::Actor, "actor"},
        std::pair{CompletionStampField::CStoreGuid, "C_STORE_GUID"},
        std::pair{CompletionStampField::FStoreGuid, "F_STORE_GUID"},
        std::pair{CompletionStampField::SessionSerial, "session serial"},
        std::pair{CompletionStampField::HistoryNonce, "HISTORY_NONCE"},
        std::pair{CompletionStampField::RelSeq, "REL_SEQ"},
        std::pair{CompletionStampField::TuSeq, "TU_SEQ"},
        std::pair{CompletionStampField::TransactionDigest, "transaction digest"},
        std::pair{CompletionStampField::RawDigest, "raw digest"},
        std::pair{CompletionStampField::TransactionBound, "transaction-bound state"},
    };

    uint64_t identity = 4000;
    size_t rejection_gaps = 0;
    for (const ActorSide actor : {ActorSide::C, ActorSide::F}) {
        const AsyncOperationKind target = actor == ActorSide::C
                                              ? AsyncOperationKind::WriteFragment
                                              : AsyncOperationKind::ReadPayload;
        for (const auto& [field, name] : fields) {
            const CompletionStampField selected_field = field;
            const std::string_view field_name = name;
            P50ServerEndpoint server(Id128::from_u64(identity++));
            TestClient client(Id128::from_u64(identity++));
            const std::vector<uint8_t> input = pseudo_random_bytes(4096);
            bool changed = false;
            EndpointIoControl client_control;
            EndpointIoControl server_control;
            EndpointIoControl& selected =
                actor == ActorSide::C ? client_control : server_control;
            selected.before_live_identity_check =
                [&](const CompletionStamp& expected, CompletionLiveIdentity& live) {
                    if (changed || expected.actor != actor ||
                        expected.operation != target || !expected.transaction_bound)
                        return;
                    change_live_identity_field(live, selected_field);
                    changed = true;
                };

            const PairResult rejected = run_pair(client, server, admit(client, input),
                                                 client_control, server_control);
            std::string context = actor == ActorSide::C ? "C live " : "F live ";
            context.append(field_name);
            require(changed, context + " mutation did not run");
            const bool rejected_exactly =
                rejected.client.status == ClientRunStatus::Disconnected &&
                rejected.server.status == ServerRunStatus::Disconnected &&
                client.has_active_transaction() &&
                !copy_input(server, client.c_store_guid());
            if (!rejected_exactly) {
                std::cerr << "p50_endpoint_test: " << context
                          << " mutation was not rejected exactly\n";
                ++rejection_gaps;
                continue;
            }

            const PairResult replayed = run_pair(client, server);
            require(replayed.client.status == ClientRunStatus::Committed &&
                        replayed.server.status == ServerRunStatus::Completed &&
                        replayed.client.reconnect == EndpointReconnectOutcome::ExactMatch &&
                        !client.has_active_transaction() &&
                        copy_input(server, client.c_store_guid()) == input,
                    context + " rejection did not permit exact follow-up");
        }
    }
    require(rejection_gaps == 0,
            "one or more live identity mutations were not rejected exactly");

    P50ServerEndpoint server(Id128::from_u64(identity++));
    TestClient client(Id128::from_u64(identity++));
    bool saw_unbound_c = false;
    bool saw_unbound_f = false;
    EndpointIoControl client_control;
    EndpointIoControl server_control;
    client_control.before_live_identity_check =
        [&](const CompletionStamp& expected, CompletionLiveIdentity& live) {
            saw_unbound_c = saw_unbound_c ||
                            (expected.actor == ActorSide::C &&
                             !expected.transaction_bound && !live.transaction_bound);
        };
    server_control.before_live_identity_check =
        [&](const CompletionStamp& expected, CompletionLiveIdentity& live) {
            saw_unbound_f = saw_unbound_f ||
                            (expected.actor == ActorSide::F &&
                             !expected.transaction_bound && !live.transaction_bound);
        };
    const std::vector<uint8_t> first_input = pseudo_random_bytes(1024);
    const PairResult first = run_pair(client, server, admit(client, first_input),
                                      client_control, server_control);
    require(first.client.status == ClientRunStatus::Committed && saw_unbound_c && saw_unbound_f,
            "legal unbound live identities did not complete on both endpoints");
    const std::vector<uint8_t> second_input = pseudo_random_bytes(1536);
    const PairResult follow_up = run_pair(client, server, admit(client, second_input));
    require(follow_up.client.status == ClientRunStatus::Committed &&
                follow_up.server.status == ServerRunStatus::Completed &&
                follow_up.client.reconnect == EndpointReconnectOutcome::ExactMatch &&
                copy_input(server, client.c_store_guid()) == second_input,
            "legal unbound live identities did not preserve exact follow-up");
}

void test_idempotent_prepare_admission() {
    TestClient client(Id128::from_u64(150));
    const PrepareRequestKey request{7, 91};
    const std::vector<uint8_t> input = bytes("one canonical local request\n");
    const PreparedTuHandle first = client.prepare(request, input);
    const PreparedTuHandle replay = client.prepare(request, input);
    require(first == replay && client.authority->live_entry_count() == 1 &&
                client.authority->retained_encoded_bytes() != 0,
            "same prepare key/input did not return one retained authority entry");
    std::vector<uint8_t> same_length_different = input;
    same_length_different[same_length_different.size() / 2] ^= 1;
    require_throws<std::invalid_argument>(
        [&] { (void)client.prepare(request, same_length_different); },
        "prepare key was rebound to different same-length input");

    auto other_authority =
        std::make_shared<P50PreparationAuthority>(Id128::from_u64(149));
    require_throws<std::invalid_argument>([&] { (void)other_authority->retain(first); },
                                          "foreign preparation handle was accepted");
    require(client.authority->release(replay) == 0 &&
                client.authority->live_entry_count() == 0 &&
                client.authority->retained_encoded_bytes() == 0,
            "idempotent replay acquired an additional reference");
    require_throws<std::invalid_argument>([&] { (void)client.authority->release(first); },
                                          "zero-reference handle was released twice");

    const PreparedTuHandle retained = client.prepare({7, 92}, input);
    require(client.authority->retain(retained) == 2 &&
                client.authority->release(retained) == 1 &&
                client.authority->release(retained) == 0,
            "explicit retain did not represent one additional owner");

    PreparationAuthorityLimits one_entry;
    one_entry.max_live_entries = 1;
    auto bounded = std::make_shared<P50PreparationAuthority>(
        Id128::from_u64(148), EndpointCaps{}.zstd, one_entry);
    const PreparedTuHandle bounded_first =
        bounded->prepare(PrepareRequestKey{9, 1}, bytes("bounded first\n"));
    const PreparedTuHandle bounded_replay =
        bounded->prepare(PrepareRequestKey{9, 1}, bytes("bounded first\n"));
    require(bounded_first == bounded_replay && bounded->live_entry_count() == 1,
            "idempotent replay consumed another bounded entry");
    require_throws<std::length_error>(
        [&] { (void)bounded->prepare(PrepareRequestKey{9, 2}, bytes("bounded second\n")); },
        "preparation authority exceeded its live-entry bound");
    require(bounded->release(bounded_replay) == 0,
            "bounded replay acquired an additional reference");
    require_throws<std::invalid_argument>(
        [&] { (void)bounded->release(bounded_first); },
        "bounded replay retained an invisible owner");

    PreparationAuthorityLimits one_byte;
    one_byte.max_retained_encoded_bytes = 1;
    P50PreparationAuthority byte_bounded(Id128::from_u64(147), EndpointCaps{}.zstd,
                                         one_byte);
    require_throws<std::length_error>(
        [&] {
            (void)byte_bounded.prepare(PrepareRequestKey{10, 1},
                                       bytes("retained bytes exceed one"));
        },
        "preparation authority exceeded its retained-byte bound");
    require(byte_bounded.live_entry_count() == 0 &&
                byte_bounded.retained_encoded_bytes() == 0,
            "failed retained-byte admission published a partial entry");

    EndpointCaps tight;
    tight.zstd.max_raw_bytes = 4;
    CompletionLog completions;
    TestClient capped(Id128::from_u64(151), tight, HistoryNonce{1}, &completions);
    const PrepareRequestKey retried{8, 1};
    require_throws<std::length_error>(
        [&] { (void)capped.prepare(retried, bytes("too long")); },
        "failed preparation ignored its decoded-input cap");
    const PreparedTuHandle admitted = capped.prepare(retried, bytes("okay"));
    P50ServerEndpoint server(Id128::from_u64(152), tight, &completions);
    require(run_pair(capped, server, admitted).client.status == ClientRunStatus::Committed,
            "admitted preparation did not complete after a bounded failure");
    require(std::any_of(completions.completions().begin(), completions.completions().end(),
                        [](const AsyncCompletion& completion) {
                            return completion.stamp.actor == ActorSide::C &&
                                   completion.stamp.transaction_bound &&
                                   completion.stamp.tu_seq.value == 0;
                        }),
            "failed preparation consumed the next TU_SEQ");
    require(capped.authority->release(admitted) == 0,
            "completed preparation did not release explicitly to zero");

    require_throws<std::invalid_argument>(
        [&] { P50PreparationAuthority rejected(CStoreGuid{}); },
        "zero C_STORE_GUID was accepted by the preparation authority");
    require_throws<std::invalid_argument>(
        [&] { (void)client.prepare({0, 1}, input); },
        "zero producer session was accepted by the preparation authority");
    require_throws<std::invalid_argument>(
        [&] { (void)client.prepare({1, 0}, input); },
        "zero request token was accepted by the preparation authority");
    require_throws<std::invalid_argument>(
        [&] { P50ServerEndpoint rejected(FStoreGuid{}); },
        "zero F_STORE_GUID was accepted by the endpoint");
    P50ServerEndpointConfig zero_error;
    zero_error.protocol_error_code = 0;
    require_throws<std::invalid_argument>(
        [&] {
            P50ServerEndpoint rejected(Id128::from_u64(153), {}, nullptr,
                                       nullptr, zero_error);
        },
        "zero protocol ERROR code was accepted by the endpoint");
    P50ServerEndpoint reset_target(Id128::from_u64(154));
    require_throws<std::invalid_argument>(
        [&] { reset_target.reset_store(FStoreGuid{}); },
        "zero F_STORE_GUID was accepted as an incarnation replacement");

    for (const int invalid_window_log : {9, 32}) {
        EndpointCaps invalid;
        invalid.zstd.max_window_log = invalid_window_log;
        require_throws<std::invalid_argument>(
            [&] {
                P50PreparationAuthority rejected(Id128::from_u64(155),
                                                 invalid.zstd);
            },
            "preparation authority accepted an out-of-range Zstd window log");

        auto valid_authority = std::make_shared<P50PreparationAuthority>(
            Id128::from_u64(156));
        require_throws<std::invalid_argument>(
            [&] { P50ClientEndpoint rejected(valid_authority, invalid); },
            "client endpoint accepted an out-of-range Zstd window log");
        require_throws<std::invalid_argument>(
            [&] { P50ServerEndpoint rejected(Id128::from_u64(157), invalid); },
            "server endpoint accepted an out-of-range Zstd window log");
    }
}

void test_candidate_stage_has_no_revision_residue() {
    P50ServerEndpoint server(Id128::from_u64(154));
    for (uint64_t index = 1; index != 17; ++index) {
        TestClient candidate(Id128::from_u64(154 + index));
        EndpointIoControl stop_after_hello;
        stop_after_hello.close_after_write = MessageType::SESSION_HELLO;
        const PairResult stopped = run_pair(
            candidate, server, admit(candidate, bytes("candidate-only input\n")),
            stop_after_hello);
        require(stopped.client.status == ClientRunStatus::Disconnected &&
                    stopped.server.status == ServerRunStatus::Disconnected &&
                    server.namespace_count() == 0 && server.revision_count() == 0 &&
                    server.live_session_count() == 0,
                "unactivated candidate retained namespace, revision, or live-session state");
    }

    TestClient active(Id128::from_u64(171));
    const PairResult committed =
        run_pair(active, server, admit(active, bytes("activated input\n")));
    require(committed.client.status == ClientRunStatus::Committed &&
                committed.server.status == ServerRunStatus::Completed &&
                server.namespace_count() == 1 && server.revision_count() == 1 &&
                server.live_session_count() == 0,
            "activated namespace did not acquire exactly one revision owner");
}

void test_input_record_owner_and_aggregate_limits() {
    {
        P50ServerEndpoint server(Id128::from_u64(172));
        TestClient client(Id128::from_u64(173));
        const std::vector<uint8_t> input = pseudo_random_bytes(32 * 1024);
        const PairResult completed = run_pair(client, server, admit(client, input));
        require(completed.client.status == ClientRunStatus::Committed &&
                    completed.server.status == ServerRunStatus::Completed &&
                    completed.server.committed_input.has_value() &&
                    completed.server.committed_input ==
                        server.last_committed_input(client.c_store_guid()) &&
                    server.owner_usage().retained_input_records == 1 &&
                    server.owner_usage().retained_input_bytes == input.size(),
                "route commit did not atomically publish one InputRecord key");

        InputCursor authorized = server.attach_input(*completed.server.committed_input);
        std::array<uint8_t, 257> prefix{};
        require(authorized.read(prefix) == prefix.size() &&
                    std::equal(prefix.begin(), prefix.end(), input.begin()),
                "authorized InputRecord cursor read the wrong prefix");
        server.close_input_job(*completed.server.committed_input);
        require_throws<std::logic_error>(
            [&] { (void)server.attach_input(*completed.server.committed_input); },
            "closed logical job accepted a new InputRecord attachment");
        server.collect_input_garbage();
        require(server.owner_usage().retained_input_records == 1,
                "job close reclaimed input owned by an authorized cursor");
        std::vector<uint8_t> reconstructed(prefix.begin(), prefix.end());
        std::vector<uint8_t> remainder = drain_input(authorized);
        reconstructed.insert(reconstructed.end(), remainder.begin(), remainder.end());
        require(reconstructed == input,
                "authorized cursor lost exact input after logical-job close");
        authorized = InputCursor{};
        server.collect_input_garbage();
        require(server.owner_usage().retained_input_records == 0 &&
                    server.owner_usage().retained_input_bytes == 0,
                "closed InputRecord did not collect after its final cursor");
    }

    {
        P50ServerEndpointConfig config;
        config.input_job_state =
            [](CStoreGuid, const TxBegin&, const TxCommit&, std::span<const uint8_t>) {
                return InputJobState::Closed;
            };
        P50ServerEndpoint server(Id128::from_u64(174), {}, nullptr, nullptr,
                                 std::move(config));
        TestClient client(Id128::from_u64(175));
        const PairResult completed =
            run_pair(client, server, admit(client, bytes("late closed job input\n")));
        require(completed.client.status == ClientRunStatus::Committed &&
                    completed.server.status == ServerRunStatus::Completed &&
                    !completed.server.committed_input &&
                    server.owner_usage().retained_input_records == 0 &&
                    server.owner_usage().retained_input_bytes == 0,
                "closed-before-commit job created a compiler-visible lease");
    }

    {
        P50ServerEndpoint server(Id128::from_u64(188));
        TestClient client(Id128::from_u64(189));
        const std::vector<uint8_t> input = pseudo_random_bytes(8192);
        const PairResult completed = run_pair(client, server, admit(client, input));
        require(completed.server.committed_input.has_value(),
                "store-reset fixture did not publish an InputRecord");
        InputCursor authorized = server.attach_input(*completed.server.committed_input);
        server.reset_store(Id128::from_u64(190));
        const P50ServerOwnerUsage reset_usage = server.owner_usage();
        require(reset_usage.live_sessions == 0 && reset_usage.namespaces == 0 &&
                    reset_usage.revisions == 0 && reset_usage.pending_encoded_bytes == 0 &&
                    reset_usage.pending_raw_bytes == 0 &&
                    reset_usage.decoder_window_bytes == 0 &&
                    reset_usage.retained_input_records == 0 &&
                    reset_usage.retained_input_bytes == 0 &&
                    drain_input(authorized) == input,
                "F-store replacement lost an authorized cursor or retained owner state");
    }

    {
        P50ServerEndpointConfig config;
        config.owner_limits.max_namespaces = 1;
        P50ServerEndpoint server(Id128::from_u64(176), {}, nullptr, nullptr,
                                 std::move(config));
        TestClient first(Id128::from_u64(177));
        TestClient second(Id128::from_u64(178));
        const PairResult retained =
            run_pair(first, server, admit(first, bytes("first namespace\n")));
        require(retained.client.status == ClientRunStatus::Committed &&
                    retained.server.committed_input.has_value(),
                "first namespace did not fit its aggregate bound");
        server.close_input_job(*retained.server.committed_input);
        const PairResult replacement =
            run_pair(second, server, admit(second, bytes("second namespace\n")));
        require(replacement.client.status == ClientRunStatus::Committed &&
                    replacement.server.status == ServerRunStatus::Completed &&
                    replacement.server.committed_input.has_value() &&
                    server.owner_usage().namespaces == 1 &&
                    server.owner_usage().revisions == 1 &&
                    !server.last_committed_input(first.c_store_guid()).has_value() &&
                    copy_input(server, second.c_store_guid()) ==
                        bytes("second namespace\n"),
                "whole-namespace LRU did not replace the closed oldest namespace");
    }

    {
        P50ServerEndpointConfig config;
        config.owner_limits.max_retained_input_records = 1;
        P50ServerEndpoint server(Id128::from_u64(208), {}, nullptr, nullptr,
                                 std::move(config));
        TestClient first(Id128::from_u64(209));
        TestClient second(Id128::from_u64(210));
        const PairResult retained =
            run_pair(first, server, admit(first, bytes("evictable retained input\n")));
        require(retained.server.committed_input.has_value(),
                "retained-input LRU fixture did not publish its first record");
        InputCursor pinned = server.attach_input(*retained.server.committed_input);
        server.close_input_job(*retained.server.committed_input);

        const PreparedTuHandle blocked_input =
            admit(second, bytes("blocked by cursor\n"));
        const PairResult held =
            run_pair(second, server, blocked_input);
        require(held.client.status == ClientRunStatus::TerminalError &&
                    held.server.status == ServerRunStatus::TerminalError &&
                    server.owner_usage().retained_input_records == 1 &&
                    server.owner_usage().retained_input_bytes ==
                        bytes("evictable retained input\n").size(),
                "cursor-pinned capacity failure evicted retained input or published new input");

        pinned = InputCursor{};
        const PairResult retried = run_pair(second, server);
        require(retried.client.status == ClientRunStatus::Committed &&
                    retried.server.status == ServerRunStatus::Completed &&
                    retried.client.reconnect == EndpointReconnectOutcome::ExactMatch &&
                    server.owner_usage().retained_input_records == 1 &&
                    server.owner_usage().namespaces == 2 &&
                    server.owner_usage().revisions == 2,
                "released cursor did not permit atomic whole-namespace capacity recovery");
        require(retried.server.committed_input.has_value(),
                "capacity-recovery retry did not publish its retained input");
        server.close_input_job(*retried.server.committed_input);
        const PairResult first_again =
            run_pair(first, server,
                     admit(first, bytes("first route after payload eviction\n")));
        require(first_again.client.status == ClientRunStatus::Committed &&
                    first_again.server.status == ServerRunStatus::Completed &&
                    first_again.client.reconnect == EndpointReconnectOutcome::ExactMatch &&
                    server.owner_usage().retained_input_records == 1 &&
                    server.owner_usage().namespaces == 2 &&
                    server.owner_usage().revisions == 2,
                "cache-payload eviction destroyed the surviving protocol route");
    }

    {
        P50ServerEndpointConfig config;
        config.owner_limits.max_pending_encoded_bytes = 1;
        P50ServerEndpoint server(Id128::from_u64(179), {}, nullptr, nullptr,
                                 std::move(config));
        TestClient client(Id128::from_u64(180));
        const PairResult rejected =
            run_pair(client, server, admit(client, pseudo_random_bytes(4096)));
        const P50ServerOwnerUsage usage = server.owner_usage();
        require(rejected.client.status == ClientRunStatus::TerminalError &&
                    rejected.server.status == ServerRunStatus::TerminalError &&
                    client.has_active_transaction() &&
                    usage.pending_encoded_bytes == 0 && usage.pending_raw_bytes == 0 &&
                    usage.decoder_window_bytes == 0 && usage.retained_input_records == 0,
                "pending-input cap failure leaked an aggregate reservation");
    }

    {
        P50ServerEndpointConfig config;
        config.owner_limits.max_pending_encoded_bytes = 4096;
        config.owner_limits.max_decoder_window_bytes =
            uint64_t{2} << EndpointCaps{}.zstd.max_window_log;
        const CompetingPairResult result =
            run_competing_pair(std::move(config), 196, 3000, 3001);
        const size_t committed =
            static_cast<size_t>(result.first_client.status ==
                                ClientRunStatus::Committed) +
            static_cast<size_t>(result.second_client.status ==
                                ClientRunStatus::Committed);
        const size_t completed =
            static_cast<size_t>(result.first_server.status ==
                                ServerRunStatus::Completed) +
            static_cast<size_t>(result.second_server.status ==
                                ServerRunStatus::Completed);
        const size_t terminal =
            static_cast<size_t>(result.first_server.status ==
                                ServerRunStatus::TerminalError) +
            static_cast<size_t>(result.second_server.status ==
                                ServerRunStatus::TerminalError);
        require(committed == 1 && completed == 1 && terminal == 1 &&
                    result.usage.pending_encoded_bytes == 0 &&
                    result.usage.pending_raw_bytes == 0 &&
                    result.usage.decoder_window_bytes == 0 &&
                    result.usage.retained_input_records == 1,
                "aggregate encoded-byte budget admitted overlapping dialogues");
    }

    {
        P50ServerEndpointConfig config;
        config.owner_limits.max_pending_encoded_bytes = 8192;
        config.owner_limits.max_pending_raw_bytes = 4096;
        config.owner_limits.max_decoder_window_bytes =
            uint64_t{2} << EndpointCaps{}.zstd.max_window_log;
        const CompetingPairResult result =
            run_competing_pair(std::move(config), 199, 3000, 3001);
        const size_t committed =
            static_cast<size_t>(result.first_client.status ==
                                ClientRunStatus::Committed) +
            static_cast<size_t>(result.second_client.status ==
                                ClientRunStatus::Committed);
        const size_t completed =
            static_cast<size_t>(result.first_server.status ==
                                ServerRunStatus::Completed) +
            static_cast<size_t>(result.second_server.status ==
                                ServerRunStatus::Completed);
        const size_t terminal =
            static_cast<size_t>(result.first_server.status ==
                                ServerRunStatus::TerminalError) +
            static_cast<size_t>(result.second_server.status ==
                                ServerRunStatus::TerminalError);
        require(committed == 1 && completed == 1 && terminal == 1 &&
                    result.usage.pending_encoded_bytes == 0 &&
                    result.usage.pending_raw_bytes == 0 &&
                    result.usage.decoder_window_bytes == 0 &&
                    result.usage.retained_input_records == 1,
                "aggregate raw-byte budget admitted overlapping dialogues");
    }

    {
        P50ServerEndpointConfig config;
        config.owner_limits.max_retained_input_bytes = 8;
        P50ServerEndpoint server(Id128::from_u64(181), {}, nullptr, nullptr,
                                 std::move(config));
        TestClient client(Id128::from_u64(182));
        const PairResult rejected =
            run_pair(client, server, admit(client, bytes("larger than eight bytes")));
        const P50ServerOwnerUsage usage = server.owner_usage();
        require(rejected.client.status == ClientRunStatus::TerminalError &&
                    rejected.server.status == ServerRunStatus::TerminalError &&
                    client.has_active_transaction() && usage.retained_input_records == 0 &&
                    usage.retained_input_bytes == 0 && usage.pending_encoded_bytes == 0 &&
                    usage.pending_raw_bytes == 0 && usage.decoder_window_bytes == 0,
                "InputRecord-cap failure committed or leaked owner resources");
    }

    {
        P50ServerEndpointConfig config;
        config.owner_limits.max_retained_input_records = 1;
        P50ServerEndpoint server(Id128::from_u64(202), {}, nullptr, nullptr,
                                 std::move(config));
        TestClient client(Id128::from_u64(203));
        const PairResult first =
            run_pair(client, server, admit(client, bytes("first retained input\n")));
        const PairResult second =
            run_pair(client, server, admit(client, bytes("second retained input\n")));
        const P50ServerOwnerUsage usage = server.owner_usage();
        require(first.client.status == ClientRunStatus::Committed &&
                    first.server.status == ServerRunStatus::Completed &&
                    second.client.status == ClientRunStatus::TerminalError &&
                    second.server.status == ServerRunStatus::TerminalError &&
                    usage.retained_input_records == 1 &&
                    usage.pending_encoded_bytes == 0 &&
                    usage.pending_raw_bytes == 0 &&
                    usage.decoder_window_bytes == 0,
                "InputRecord-count bound admitted a second retained input");
    }

    {
        CompletionLog completions(0);
        ActionTrace actions(0);
        P50ServerEndpoint server(Id128::from_u64(183), {}, &completions, &actions);
        TestClient client(Id128::from_u64(184), {}, HistoryNonce{1}, &completions,
                          &actions);
        const PairResult completed =
            run_pair(client, server, admit(client, bytes("diagnostic loss is not state\n")));
        require(completed.client.status == ClientRunStatus::Committed &&
                    completed.server.status == ServerRunStatus::Completed &&
                    !completions.valid() && completions.completions().empty() &&
                    !actions.valid() && actions.records().empty() &&
                    server.owner_usage().retained_input_records == 1,
                "bounded diagnostic loss changed the durable protocol result");
    }

    {
        P50ServerEndpointConfig config;
        config.owner_limits.max_live_sessions = 1;
        P50ServerEndpoint server(Id128::from_u64(191), {}, nullptr, nullptr,
                                 std::move(config));
        TestClient client(Id128::from_u64(192));
        const PreparedTuHandle prepared =
            admit(client, bytes("one accepted live session\n"));
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> accepted =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<ServerRunResult> excess =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<ClientRunResult> client_result = asio::co_spawn(
            context, client.endpoint.run(acceptor.local_endpoint(), prepared),
            asio::use_future);
        context.run();
        require(client_result.get().status == ClientRunStatus::Committed &&
                    accepted.get().status == ServerRunStatus::Completed,
                "live-session bound rejected its admitted dialogue");
        require_throws<std::length_error>(
            [&] { (void)excess.get(); },
            "live-session aggregate bound admitted an extra dialogue");
        require(server.owner_usage().live_sessions == 0,
                "live-session bound retained a completed session");
    }

    {
        P50ServerEndpointConfig config;
        config.owner_limits.max_decoder_window_bytes =
            uint64_t{1} << EndpointCaps{}.zstd.max_window_log;
        P50ServerEndpoint server(Id128::from_u64(193), {}, nullptr, nullptr,
                                 std::move(config));
        TestClient first(Id128::from_u64(194));
        TestClient second(Id128::from_u64(195));
        const PreparedTuHandle first_prepared =
            admit(first, pseudo_random_bytes(4096));
        const PreparedTuHandle second_prepared =
            admit(second, pseudo_random_bytes(4097));
        EndpointIoControl fragment_first;
        fragment_first.max_write_fragment = 1;
        EndpointIoControl fragment_second;
        fragment_second.max_write_fragment = 1;
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> first_server =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<ServerRunResult> second_server =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<ClientRunResult> first_client = asio::co_spawn(
            context,
            first.endpoint.run(acceptor.local_endpoint(), first_prepared,
                               fragment_first),
            asio::use_future);
        std::future<ClientRunResult> second_client = asio::co_spawn(
            context,
            second.endpoint.run(acceptor.local_endpoint(), second_prepared,
                                fragment_second),
            asio::use_future);
        context.run();
        const ClientRunResult first_client_result = first_client.get();
        const ClientRunResult second_client_result = second_client.get();
        const ServerRunResult first_server_result = first_server.get();
        const ServerRunResult second_server_result = second_server.get();
        const size_t committed_clients =
            static_cast<size_t>(first_client_result.status == ClientRunStatus::Committed) +
            static_cast<size_t>(second_client_result.status == ClientRunStatus::Committed);
        const size_t completed_servers =
            static_cast<size_t>(first_server_result.status == ServerRunStatus::Completed) +
            static_cast<size_t>(second_server_result.status == ServerRunStatus::Completed);
        const size_t terminal_servers =
            static_cast<size_t>(first_server_result.status == ServerRunStatus::TerminalError) +
            static_cast<size_t>(second_server_result.status == ServerRunStatus::TerminalError);
        const P50ServerOwnerUsage usage = server.owner_usage();
        require(committed_clients == 1 && completed_servers == 1 && terminal_servers == 1 &&
                    usage.pending_encoded_bytes == 0 && usage.pending_raw_bytes == 0 &&
                    usage.decoder_window_bytes == 0 &&
                    usage.retained_input_records == 1,
                "aggregate decoder-window budget admitted overlapping dialogues");
    }

    {
        auto authority =
            std::make_shared<P50PreparationAuthority>(Id128::from_u64(185));
        const PreparedTuHandle prepared =
            authority->prepare({1, 1}, bytes("single owner thread\n"));
        bool rejected = false;
        std::thread other([&] {
            try {
                (void)authority->retain(prepared);
            } catch (const std::logic_error&) {
                rejected = true;
            }
        });
        other.join();
        require(rejected && authority->release(prepared) == 0,
                "mutable preparation state crossed its established owner thread");
    }

    {
        P50ServerEndpoint server(Id128::from_u64(204));
        TestClient client(Id128::from_u64(205));
        require(run_pair(client, server,
                         admit(client, bytes("endpoint owner thread\n")))
                        .client.status == ClientRunStatus::Committed,
                "endpoint thread-owner fixture did not commit");
        bool client_rejected = false;
        bool server_rejected = false;
        std::thread other([&] {
            try {
                (void)client.endpoint.next_rel_seq();
            } catch (const std::logic_error&) {
                client_rejected = true;
            }
            try {
                server.collect_input_garbage();
            } catch (const std::logic_error&) {
                server_rejected = true;
            }
        });
        other.join();
        require(client_rejected && server_rejected,
                "client or server mutable state crossed its established owner thread");
    }

    {
        P50ServerEndpointConfig invalid;
        invalid.owner_limits.max_live_sessions = 0;
        require_throws<std::invalid_argument>(
            [&] {
                P50ServerEndpoint rejected(Id128::from_u64(186), {}, nullptr, nullptr,
                                           invalid);
            },
            "zero aggregate live-session bound was accepted");
        invalid = {};
        invalid.owner_limits.max_decoder_window_bytes =
            (uint64_t{1} << EndpointCaps{}.zstd.max_window_log) - 1;
        require_throws<std::invalid_argument>(
            [&] {
                P50ServerEndpoint rejected(Id128::from_u64(187), {}, nullptr, nullptr,
                                           invalid);
            },
            "decoder-window aggregate bound admitted no complete dialogue");
    }
}

void test_fragmentation_at_every_control_and_body_boundary() {
    const std::vector<uint8_t> input = pseudo_random_bytes(96);
    const ZstdTuEnvelope encoded = encode_zstd_tu(
        HistoryNonce{1}, RelSeq{0}, TuSeq{0}, Digest128{}, input);
    const size_t frame_bytes =
        std::max(4 + encoded.body.size(), size_t{4 + kMandatoryControlFramePayload});
    require(frame_bytes > 8, "fragmentation fixture is too small");
    for (size_t split = 1; split != frame_bytes; ++split) {
        P50ServerEndpoint server(Id128::from_u64(1000 + split));
        TestClient client(Id128::from_u64(2000 + split));
        EndpointIoControl client_control;
        client_control.max_write_fragment = split;
        EndpointIoControl server_control;
        server_control.max_write_fragment = split;
        const PairResult result =
            run_pair(client, server, admit(client, input), client_control, server_control);
        if (result.client.status != ClientRunStatus::Committed ||
            result.server.status != ServerRunStatus::Completed)
            fail("fragmentation failed at split " + std::to_string(split));
    }
}

void test_exact_replay_and_lost_final() {
    {
        ActionTrace actions;
        P50ServerEndpoint server(Id128::from_u64(300), {}, nullptr, &actions);
        TestClient client(Id128::from_u64(301), {}, HistoryNonce{20}, nullptr, &actions);
        EndpointIoControl stop;
        stop.close_after_write = MessageType::TX_BEGIN;
        const std::vector<uint8_t> input = bytes("exact replay input\n");
        const PairResult interrupted = run_pair(client, server, admit(client, input), stop);
        require(interrupted.client.status == ClientRunStatus::Disconnected &&
                    client.has_active_transaction(),
                "uncertain disconnect erased C's active transaction");
        const PairResult replayed = run_pair(client, server);
        require(replayed.client.status == ClientRunStatus::Committed &&
                    replayed.client.reconnect == EndpointReconnectOutcome::ExactMatch,
                "exact replay did not commit on the unchanged route");
        require(replayed.client.committed_commit.has_value() &&
                    replayed.client.committed_input.has_value() &&
                    replayed.client.committed_input ==
                        server.last_committed_input(client.c_store_guid()),
                "normal commit did not return its exact direct witness");
        require(copy_input(server, client.c_store_guid()) == input,
                "exact replay materialized different bytes");
        require(std::any_of(actions.records().begin(), actions.records().end(),
                            [](const ActionRecord& record) {
                                return record.action == ActionType::ACTIVE_REPLAYED;
                            }),
                "exact replay omitted ACTIVE_REPLAYED correspondence");
        require_trace(actions, "exact replay trace");
    }

    {
        ActionTrace actions;
        P50ServerEndpoint server(Id128::from_u64(400), {}, nullptr, &actions);
        TestClient client(Id128::from_u64(401), {}, HistoryNonce{30}, nullptr, &actions);
        EndpointIoControl lose_final;
        lose_final.close_before_write = MessageType::TX_COMMIT;
        const std::vector<uint8_t> input = bytes("lost final acknowledgement\n");
        const PairResult interrupted =
            run_pair(client, server, admit(client, input), {}, lose_final);
        require(interrupted.client.status == ClientRunStatus::Disconnected &&
                    client.has_active_transaction(),
                "lost-final window did not retain C active state");
        require(copy_input(server, client.c_store_guid()) == input,
                "F did not retain its completed input before final-message loss");
        const PairResult reconciled = run_pair(client, server);
        require(reconciled.client.status == ClientRunStatus::Committed &&
                    reconciled.client.reconnect ==
                        EndpointReconnectOutcome::LostFinalAcknowledgement &&
                    !client.has_active_transaction(),
                "lost-final reconciliation did not accept the exact retained commit");
        require(reconciled.client.committed_commit.has_value() &&
                    reconciled.client.committed_input.has_value() &&
                    reconciled.client.committed_input ==
                        server.last_committed_input(client.c_store_guid()),
                "lost-final reconciliation did not return its exact direct witness");
        require_trace(actions, "lost-final trace");
    }

    {
        ActionTrace actions;
        size_t publication_attempts = 0;
        const std::vector<uint8_t> input =
            bytes("publication must precede route commit\n");
        P50ServerEndpointConfig config;
        config.input_job_state =
            [&](CStoreGuid published_guid, const TxBegin& begin,
                const TxCommit& commit, std::span<const uint8_t> exact) {
                ++publication_attempts;
                require(published_guid == Id128::from_u64(411) &&
                            commit.history_nonce == begin.history_nonce &&
                            commit.rel_seq == begin.rel_seq &&
                            commit.tu_seq == begin.tu_seq &&
                            commit.transaction_digest == begin.transaction_digest &&
                            commit.raw_digest == begin.raw_digest &&
                            std::ranges::equal(exact, input),
                        "precommit publisher received the wrong exact tuple or bytes");
                if (publication_attempts == 1)
                    throw std::bad_alloc();
                return InputJobState::Open;
            };
        P50ServerEndpoint server(Id128::from_u64(410), {}, nullptr, &actions,
                                 std::move(config));
        TestClient client(Id128::from_u64(411), {}, HistoryNonce{31}, nullptr,
                          &actions);
        const PairResult rejected =
            run_pair(client, server, admit(client, input));
        require(rejected.client.status == ClientRunStatus::TerminalError &&
                    rejected.client.observation != ClientRunObservation::ExactCommitObserved &&
                    rejected.server.status == ServerRunStatus::TerminalError &&
                    client.has_active_transaction() &&
                    !copy_input(server, client.c_store_guid()) &&
                    publication_attempts == 1,
                "failed precommit publication advanced or discarded the transaction");
        const PairResult replayed = run_pair(client, server);
        require(replayed.client.status == ClientRunStatus::Committed &&
                    replayed.client.reconnect == EndpointReconnectOutcome::ExactMatch &&
                    copy_input(server, client.c_store_guid()) == input &&
                    publication_attempts == 2,
                "allocation-failure transaction did not replay and publish exactly");
        require(std::any_of(actions.records().begin(), actions.records().end(),
                            [](const ActionRecord& record) {
                                return record.actor == ActorSide::F &&
                                       record.action == ActionType::ACTIVE_REPLAYED;
                            }),
                "allocation-failure replay omitted ACTIVE_REPLAYED");
        require_trace(actions, "precommit publication replay trace");
    }
}

void test_completion_identity_and_store_replacement() {
    {
        ActionTrace actions;
        P50ServerEndpoint server(Id128::from_u64(450), {}, nullptr, &actions);
        TestClient client(Id128::from_u64(451), {}, HistoryNonce{31}, nullptr, &actions);
        EndpointIoControl wrong;
        wrong.wrong_digest_completion = AsyncOperationKind::WriteFragment;
        const std::vector<uint8_t> input = pseudo_random_bytes(2048);
        const PairResult rejected = run_pair(client, server, admit(client, input), wrong);
        require(rejected.client.status == ClientRunStatus::Disconnected &&
                    client.has_active_transaction() &&
                    !copy_input(server, client.c_store_guid()),
                "wrong-digest completion changed endpoint state");
        const PairResult replayed = run_pair(client, server);
        require(replayed.client.status == ClientRunStatus::Committed &&
                    copy_input(server, client.c_store_guid()) == input,
                "wrong-digest completion was not recoverable by exact replay");
        require_trace(actions, "wrong-digest completion trace");
    }

    for (bool alter_raw_digest : {false, true}) {
        P50ServerEndpoint server(Id128::from_u64(alter_raw_digest ? 455 : 453));
        TestClient client(Id128::from_u64(alter_raw_digest ? 456 : 454));
        EndpointIoControl wrong;
        if (alter_raw_digest)
            wrong.wrong_raw_digest_completion = AsyncOperationKind::ReadPayload;
        else
            wrong.wrong_digest_completion = AsyncOperationKind::ReadPayload;
        const std::vector<uint8_t> input = pseudo_random_bytes(3072);
        const PairResult rejected =
            run_pair(client, server, admit(client, input), {}, wrong);
        require(rejected.server.status == ServerRunStatus::Disconnected &&
                    rejected.client.status == ClientRunStatus::Disconnected &&
                    client.has_active_transaction() &&
                    !copy_input(server, client.c_store_guid()),
                alter_raw_digest
                    ? "late raw-digest completion changed endpoint state"
                    : "late transaction-digest completion changed endpoint state");
        const PairResult replayed = run_pair(client, server);
        require(replayed.client.status == ClientRunStatus::Committed &&
                    copy_input(server, client.c_store_guid()) == input,
                alter_raw_digest
                    ? "late raw-digest completion did not permit exact replay"
                    : "late transaction-digest completion did not permit exact replay");
    }

    {
        ActionTrace actions;
        CompletionLog completions;
        const FStoreGuid first_guid = Id128::from_u64(460);
        const FStoreGuid second_guid = Id128::from_u64(461);
        P50ServerEndpoint server(first_guid, {}, &completions, &actions);
        TestClient client(Id128::from_u64(462), {}, HistoryNonce{32}, &completions,
                                 &actions);
        bool reset_done = false;
        EndpointIoControl reset_during_body;
        reset_during_body.before_completion_check = [&](const CompletionStamp& stamp) {
            if (!reset_done && stamp.actor == ActorSide::F && stamp.transaction_bound &&
                stamp.operation == AsyncOperationKind::ReadPayload) {
                reset_done = true;
                server.reset_store(second_guid);
            }
        };
        const std::vector<uint8_t> input = pseudo_random_bytes(4096);
        const PairResult invalidated =
            run_pair(client, server, admit(client, input), {}, reset_during_body);
        require(reset_done && invalidated.server.status == ServerRunStatus::Disconnected &&
                    invalidated.client.status == ClientRunStatus::Disconnected &&
                    client.has_active_transaction() && server.f_store_guid() == second_guid &&
                    !copy_input(server, client.c_store_guid()),
                "F reset did not fence its old in-flight completion");
        const PairResult replaced = run_pair(client, server);
        require(replaced.client.status == ClientRunStatus::Committed &&
                    replaced.client.reconnect == EndpointReconnectOutcome::ColdFStore &&
                    replaced.client.observation == ClientRunObservation::ExactCommitObserved &&
                    replaced.server.session_serial > invalidated.server.session_serial &&
                    copy_input(server, client.c_store_guid()) == input,
                "precommit F-incarnation replacement did not preserve exact prepared work");
        require(std::any_of(actions.records().begin(), actions.records().end(),
                            [](const ActionRecord& record) {
                                return record.action == ActionType::F_STORE_INCAR_REPLACED;
                            }),
                "precommit F replacement omitted its explicit action");
        require_trace(actions, "precommit F replacement trace");

        bool saw_old_completion = false;
        bool saw_new_completion = false;
        for (const AsyncCompletion& completion : completions.completions()) {
            if (completion.stamp.actor != ActorSide::F)
                continue;
            if (completion.stamp.f_store_guid == first_guid)
                saw_old_completion = true;
            if (completion.stamp.f_store_guid == second_guid)
                saw_new_completion = true;
        }
        require(saw_old_completion && saw_new_completion,
                "completion log did not retain distinct old/new F identities");
    }

    {
        ActionTrace actions;
        P50ServerEndpoint server(Id128::from_u64(470), {}, nullptr, &actions);
        TestClient client(Id128::from_u64(471), {}, HistoryNonce{33}, nullptr, &actions);
        EndpointIoControl lose_final;
        lose_final.close_before_write = MessageType::TX_COMMIT;
        const std::vector<uint8_t> input = bytes("durable before F reset\n");
        const PairResult durable =
            run_pair(client, server, admit(client, input), {}, lose_final);
        require(durable.client.status == ClientRunStatus::Disconnected &&
                    client.has_active_transaction() &&
                    copy_input(server, client.c_store_guid()) == input,
                "postcommit/pre-ack replacement fixture did not reach its durable window");
        server.reset_store(Id128::from_u64(472));
        const PairResult replaced = run_pair(client, server);
        require(replaced.client.status == ClientRunStatus::Committed &&
                    replaced.client.reconnect == EndpointReconnectOutcome::ColdFStore &&
                    replaced.client.observation == ClientRunObservation::ExactCommitObserved &&
                    copy_input(server, client.c_store_guid()) == input,
                "postcommit/pre-ack F replacement did not replay exact prepared work");
        require_trace(actions, "postcommit/pre-ack F replacement trace");
    }
}

void test_same_f_route_reset() {
    {
        ActionTrace actions;
        P50ServerEndpoint server(Id128::from_u64(550), {}, nullptr, &actions);
        TestClient client(Id128::from_u64(551), {}, HistoryNonce{50}, nullptr, &actions);
        require(
            run_pair(client, server, admit(client, bytes("route before reset\n"))).client.status ==
                ClientRunStatus::Committed,
            "route-reset fixture did not establish its first route");
        const ServerRunResult forced =
            force_route_reset(server, client.c_store_guid(), HistoryNonce{999});
        require(forced.status == ServerRunStatus::Disconnected &&
                    forced.c_store_guid == client.c_store_guid(),
                "independent route reset did not close as expected");
        const std::vector<uint8_t> input = bytes("route after reset\n");
        const PairResult repaired = run_pair(client, server, admit(client, input));
        require(repaired.client.status == ClientRunStatus::Committed &&
                    repaired.client.reconnect == EndpointReconnectOutcome::RouteHistoryReset,
                "same-F route mismatch did not take the history-reset outcome");
        require(copy_input(server, client.c_store_guid()) == input,
                "route-reset retry did not materialize exact input");
        require_trace(actions, "route-history-reset trace");
    }
}

void test_reset_ack_equality_and_terminal_result() {
    const FStoreGuid f_guid = Id128::from_u64(570);
    for (const ResetAckMutation mutation :
         {ResetAckMutation::WireRevision, ResetAckMutation::ProfileMask,
          ResetAckMutation::FrameLimit, ResetAckMutation::FillLimit}) {
        TestClient client(Id128::from_u64(571 + static_cast<uint8_t>(mutation)));
        const std::vector<uint8_t> input = bytes("reset acknowledgement equality\n");
        const PreparedTuHandle prepared = admit(client, input);
        const ClientRunResult terminal = run_client_with_raw_peer(
            client, prepared, [&](tcp::acceptor& acceptor) {
                return raw_bad_reset_ack_peer(
                    acceptor, ScriptedSessionState{.f_guid = f_guid}, mutation);
            });
        require(terminal.status == ClientRunStatus::TerminalError &&
                    terminal.observation != ClientRunObservation::ExactCommitObserved && client.has_reconciliation_work() &&
                    !client.has_active_transaction(),
                "bad HISTORY_RESET acknowledgement lost queued reconciliation work");

        P50ServerEndpoint good(f_guid);
        const PairResult recovered = run_pair(client, good);
        require(recovered.client.status == ClientRunStatus::Committed &&
                    copy_input(good, client.c_store_guid()) == input,
                "bad HISTORY_RESET acknowledgement changed the queued PreparedTU");
    }

    {
        EndpointCaps exact_cap;
        exact_cap.wire.max_frame_payload = kMandatoryControlFramePayload;
        TestClient client(Id128::from_u64(580), exact_cap);
        const std::vector<uint8_t> input = bytes("bounded peer terminal result\n");
        const PreparedTuHandle prepared = admit(client, input);
        const ClientRunResult terminal = run_client_with_raw_peer(
            client, prepared, [&](tcp::acceptor& acceptor) {
                return raw_bounded_error_peer(acceptor, kMandatoryControlFramePayload);
            });
        require(terminal.status == ClientRunStatus::TerminalError &&
                    terminal.observation != ClientRunObservation::ExactCommitObserved && terminal.terminal_error &&
                    encode_payload(Message{*terminal.terminal_error}).size() ==
                        kMandatoryControlFramePayload &&
                    client.has_reconciliation_work(),
                "cap-sized peer ERROR did not use the bounded terminal-result path");
        P50ServerEndpoint good(Id128::from_u64(581), exact_cap);
        require(run_pair(client, good).client.status == ClientRunStatus::Committed &&
                    copy_input(good, client.c_store_guid()) == input,
                "terminal peer ERROR changed the queued PreparedTU");
    }

    {
        TestClient client(Id128::from_u64(585));
        const std::vector<uint8_t> input = bytes("client framing terminal result\n");
        const PreparedTuHandle prepared = admit(client, input);
        const ClientRunResult terminal = run_client_with_raw_peer(
            client, prepared,
            [&](tcp::acceptor& acceptor) { return raw_unknown_frame_peer(acceptor); });
        require(terminal.status == ClientRunStatus::TerminalError &&
                    terminal.observation != ClientRunObservation::ExactCommitObserved && terminal.terminal_error &&
                    client.has_reconciliation_work() && !client.has_active_transaction(),
                "client framing failure bypassed the bounded terminal-result path");
        P50ServerEndpoint good(Id128::from_u64(586));
        require(run_pair(client, good).client.status == ClientRunStatus::Committed &&
                    copy_input(good, client.c_store_guid()) == input,
                "client framing failure changed the queued PreparedTU");
    }

    {
        ActionTrace actions;
        P50ServerEndpoint established(f_guid, {}, nullptr, &actions);
        TestClient client(Id128::from_u64(590), {}, HistoryNonce{1}, nullptr, &actions);
        const std::vector<uint8_t> input = bytes("active identity survives route mismatch\n");
        EndpointIoControl stop;
        stop.close_after_write = MessageType::TX_BEGIN;
        require(run_pair(client, established, admit(client, input), stop).client.status ==
                        ClientRunStatus::Disconnected &&
                    client.has_active_transaction(),
                "active terminal fixture did not retain its original transaction");
        const auto initial_position = std::find_if(
            actions.records().begin(), actions.records().end(),
            [](const ActionRecord& record) {
                return record.actor == ActorSide::C && record.action == ActionType::TX_BEGIN;
            });
        require(initial_position != actions.records().end(),
                "active terminal fixture omitted its canonical C TX_BEGIN");
        const ActionRecord initial_identity = *initial_position;
        const RelSeq initial_next_rel = client.endpoint.next_rel_seq();
        const Digest128 initial_state = client.endpoint.state_digest();
        const size_t action_count_before_mismatch = actions.records().size();

        const ClientRunResult terminal = run_client_with_raw_peer(
            client, {}, [&](tcp::acceptor& acceptor) {
                return raw_route_mismatch_peer(
                    acceptor,
                    ScriptedSessionState{.f_guid = f_guid,
                                         .route_present = true,
                                         .history_nonce = HistoryNonce{991},
                                         .next_rel_seq = RelSeq{0},
                                         .state_digest = icecc::digest128("other route")});
            });
        require(terminal.status == ClientRunStatus::TerminalError &&
                    terminal.reconnect == EndpointReconnectOutcome::RouteHistoryReset &&
                    terminal.observation != ClientRunObservation::ExactCommitObserved && client.has_active_transaction() &&
                    client.has_reconciliation_work() &&
                    client.endpoint.next_rel_seq() == initial_next_rel &&
                    client.endpoint.state_digest() == initial_state &&
                    actions.records().size() == action_count_before_mismatch,
                "active same-F route mismatch erased reconciliation identity");
        const PairResult recovered = run_pair(client, established);
        require(recovered.client.status == ClientRunStatus::Committed &&
                    recovered.client.reconnect == EndpointReconnectOutcome::ExactMatch &&
                    copy_input(established, client.c_store_guid()) == input,
                "terminal route-ack error changed exact replay identity");
        const auto replay_position = std::find_if(
            actions.records().begin(), actions.records().end(),
            [](const ActionRecord& record) {
                return record.actor == ActorSide::F &&
                       record.action == ActionType::ACTIVE_REPLAYED;
            });
        require(replay_position != actions.records().end() &&
                    replay_position->c_store_guid == initial_identity.c_store_guid &&
                    replay_position->f_store_guid == initial_identity.f_store_guid &&
                    replay_position->history_nonce == initial_identity.history_nonce &&
                    replay_position->rel_seq == initial_identity.rel_seq &&
                    replay_position->tu_seq == initial_identity.tu_seq &&
                    replay_position->transaction_digest == initial_identity.transaction_digest &&
                    replay_position->raw_digest == initial_identity.raw_digest &&
                    replay_position->state_digest == initial_identity.state_digest,
                "same-F mismatch changed the digest-bound active tuple before exact replay");
        require_trace(actions, "same-F active route-mismatch trace");

        EndpointIoControl stop_terminal_report;
        stop_terminal_report.close_before_write = MessageType::ERROR;
        require(run_pair(client, established, admit(client, input), stop).client.status ==
                        ClientRunStatus::Disconnected &&
                    client.has_active_transaction(),
                "terminal-report fixture did not retain a second active transaction");
        const ClientRunResult local_terminal = run_client_with_raw_peer(
            client, {},
            [&](tcp::acceptor& acceptor) {
                return raw_route_mismatch_peer(
                    acceptor,
                    ScriptedSessionState{.f_guid = f_guid,
                                         .route_present = true,
                                         .history_nonce = HistoryNonce{992},
                                         .next_rel_seq = RelSeq{0},
                                         .state_digest = icecc::digest128("third route")},
                    false);
            },
            stop_terminal_report);
        require(local_terminal.status == ClientRunStatus::TerminalError &&
                    local_terminal.observation != ClientRunObservation::ExactCommitObserved &&
                    client.has_active_transaction(),
                "failed terminal-report write relabeled a known route mismatch");
    }
}

void test_handshake_binding_and_namespace_rules() {
    {
        P50ServerEndpoint server(Id128::from_u64(610));
        const CStoreGuid c_guid = Id128::from_u64(611);
        const std::vector<uint8_t> input = bytes("pending survives incompatible HELLO\n");
        HelloRaceState coordination;
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> first_server =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<ServerRunResult> second_server =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<void> first_peer = asio::co_spawn(
            context, raw_pending_then_finish(acceptor.local_endpoint(), c_guid, input, coordination),
            asio::use_future);
        std::future<void> incompatible_peer = asio::co_spawn(
            context, raw_incompatible_hello(acceptor.local_endpoint(), c_guid, coordination),
            asio::use_future);
        context.run();
        first_peer.get();
        incompatible_peer.get();
        const ServerRunResult first = first_server.get();
        const ServerRunResult second = second_server.get();
        require(first.status == ServerRunStatus::Completed &&
                    second.status == ServerRunStatus::TerminalError &&
                    copy_input(server, c_guid) == input,
                "incompatible HELLO bound or changed the live namespace");
    }

    {
        P50ServerEndpoint server(Id128::from_u64(612));
        const CStoreGuid c_guid = Id128::from_u64(613);
        const std::vector<uint8_t> input =
            bytes("pending survives invalid compatible candidate\n");
        HelloRaceState coordination;
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> first_server =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<ServerRunResult> second_server =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<void> first_peer = asio::co_spawn(
            context, raw_pending_then_finish(acceptor.local_endpoint(), c_guid, input, coordination),
            asio::use_future);
        std::future<void> invalid_peer = asio::co_spawn(
            context, raw_invalid_first_mutation(acceptor.local_endpoint(), c_guid, coordination),
            asio::use_future);
        context.run();
        first_peer.get();
        invalid_peer.get();
        const ServerRunResult first = first_server.get();
        const ServerRunResult second = second_server.get();
        require(first.status == ServerRunStatus::Completed &&
                    second.status == ServerRunStatus::TerminalError &&
                    server.namespace_count() == 1 &&
                    copy_input(server, c_guid) == input,
                "invalid compatible candidate installed or replaced the live namespace");
    }

    {
        P50ServerEndpoint server(Id128::from_u64(614));
        const CStoreGuid c_guid = Id128::from_u64(615);
        HelloRaceState coordination{.first_pending = true};
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> server_result =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<void> peer_result = asio::co_spawn(
            context, raw_invalid_first_mutation(acceptor.local_endpoint(), c_guid, coordination),
            asio::use_future);
        context.run();
        peer_result.get();
        require(server_result.get().status == ServerRunStatus::TerminalError &&
                    server.namespace_count() == 0 && !copy_input(server, c_guid),
                "cold compatible candidate installed a namespace before validation");
    }

    {
        P50ServerEndpoint server(Id128::from_u64(616));
        const CStoreGuid c_guid = Id128::from_u64(617);
        require(force_route_reset(server, c_guid, HistoryNonce{110}).status ==
                    ServerRunStatus::Disconnected,
                "candidate-revision fixture did not establish its route");
        const std::vector<uint8_t> input =
            bytes("newly activated session invalidates an older snapshot\n");
        CandidateRevisionRace coordination;
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> first_server =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<ServerRunResult> second_server =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<void> stale_peer = asio::co_spawn(
            context, raw_stale_candidate(acceptor.local_endpoint(), c_guid, coordination),
            asio::use_future);
        std::future<void> current_peer = asio::co_spawn(
            context, raw_commit_after_candidate_staged(
                         acceptor.local_endpoint(), c_guid, input, coordination),
            asio::use_future);
        context.run();
        stale_peer.get();
        current_peer.get();
        require(first_server.get().status == ServerRunStatus::Disconnected &&
                    second_server.get().status == ServerRunStatus::Completed &&
                    server.namespace_count() == 1 &&
                    copy_input(server, c_guid) == input,
                "stale candidate replaced state from the newer activated session");
    }

    {
        const FStoreGuid guid = Id128::from_u64(620);
        P50ServerEndpoint established(guid);
        TestClient client(Id128::from_u64(621));
        EndpointIoControl stop;
        stop.close_after_write = MessageType::TX_BEGIN;
        const std::vector<uint8_t> input = bytes("same GUID missing namespace\n");
        const PairResult interrupted =
            run_pair(client, established, admit(client, input), stop);
        require(interrupted.client.status == ClientRunStatus::Disconnected &&
                    client.has_active_transaction(),
                "same-GUID inconsistency fixture lost its active transaction");

        P50ServerEndpoint inconsistent(guid);
        const PairResult refused = run_pair(client, inconsistent);
        require(refused.client.status == ClientRunStatus::TerminalError &&
                    client.has_active_transaction() &&
                    !copy_input(inconsistent, client.c_store_guid()),
                "same GUID with an absent established namespace was treated as cold");

        const PairResult recovered = run_pair(client, established);
        require(recovered.client.status == ClientRunStatus::Committed &&
                    copy_input(established, client.c_store_guid()) == input,
                "same-GUID inconsistency changed the retained retry identity");
    }

    {
        P50ServerEndpoint server(Id128::from_u64(630));
        const CStoreGuid c_guid = Id128::from_u64(631);
        require(force_route_reset(server, c_guid, HistoryNonce{500}).status ==
                    ServerRunStatus::Disconnected,
                "monotonic nonce fixture did not establish its route");
        require(force_rejected_route_reset(server, c_guid, HistoryNonce{500}),
                "reused HISTORY_NONCE was accepted");
        require(force_rejected_route_reset(server, c_guid, HistoryNonce{499}),
                "decreasing HISTORY_NONCE was accepted");
        require(force_route_reset(server, c_guid, HistoryNonce{501}).status ==
                    ServerRunStatus::Disconnected,
                "strictly increasing HISTORY_NONCE was rejected");
    }
}


// Port of the transplant lane's unique mask-law assertions onto the accepted
// production endpoint (bigoracle S1 transplant ruling, bounded convergence):
// a TX_BEGIN whose profile is intrinsically valid on the wire but outside the
// session's negotiated
// mask must be rejected by the endpoint's own negotiated-mask law -- bound by
// EXACT detail text, because the ZSTD_TU shape validator behind it rejects
// the same frame with a different text ("transaction is not a ZSTD_TU
// profile"), and std::invalid_argument IS-A std::logic_error so exception
// classes cannot separate the layers. The rejection must not move the route:
// a second connection then completes a transaction at the untouched cursor.
asio::awaitable<void> raw_unnegotiated_begin_rejected(tcp::endpoint remote,
                                                      CStoreGuid c_guid,
                                                      std::span<const uint8_t> input) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    RawRoute open = co_await raw_open(socket, remote, c_guid);
    if ((open.state.negotiated_profiles & profile_bit(ProfileId::P29V1)) != 0)
        throw std::logic_error("fixture requires P29V1 outside the negotiated mask");
    SessionState route = co_await raw_reset(socket, open, HistoryNonce{771});
    const ZstdTuEnvelope prepared = encode_zstd_tu(
        HistoryNonce{1}, RelSeq{0}, TuSeq{41}, Digest128{}, input);
    TxBegin begin = make_begin(prepared, route.history_nonce, route.state_digest,
                               route.next_rel_seq);
    begin.profile = ProfileId::P29V1;
    co_await raw_write(socket, begin);
    // No further traffic: a server that wrongly admits this begin sees EOF
    // with an open transaction and fails through a DIFFERENT detail text, so
    // the exact-text assertion below stays the discriminator without a hang.
    boost::system::error_code shutdown_error;
    socket.shutdown(tcp::socket::shutdown_send, shutdown_error);
    const Frame terminal = co_await raw_read(socket, route.limits.max_frame_payload);
    if (terminal.type != MessageType::ERROR)
        throw std::logic_error("unnegotiated TX_BEGIN did not receive terminal ERROR");
    const ErrorMessage rejection = raw_decode<ErrorMessage>(terminal);
    if (rejection.detail != "TX_BEGIN profile was not negotiated")
        throw std::logic_error(
            "unnegotiated TX_BEGIN was not rejected by the endpoint mask law: " +
            rejection.detail);
    boost::system::error_code ignored;
    socket.close(ignored);
    co_return;
}

asio::awaitable<void> raw_complete_at_preserved_cursor(tcp::endpoint remote,
                                                       CStoreGuid c_guid,
                                                       std::span<const uint8_t> input) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    RawRoute open = co_await raw_open(socket, remote, c_guid);
    if (!open.state.route_present || open.state.history_nonce != HistoryNonce{771} ||
        open.state.next_rel_seq != RelSeq{0})
        throw std::logic_error("rejected unnegotiated TX_BEGIN moved the endpoint route");
    const ZstdTuEnvelope prepared = encode_zstd_tu(
        HistoryNonce{1}, RelSeq{0}, TuSeq{42}, Digest128{}, input);
    const TxBegin begin = make_begin(prepared, open.state.history_nonce,
                                     open.state.state_digest, open.state.next_rel_seq);
    co_await raw_write(socket, begin);
    Message body_message = BodyMessage{prepared.body};
    co_await raw_write(socket, std::move(body_message));
    const TxCommit commit = raw_decode<TxCommit>(
        co_await raw_read(socket, open.state.limits.max_frame_payload));
    if (commit.transaction_digest != begin.transaction_digest ||
        commit.raw_digest != begin.raw_digest)
        throw std::logic_error("preserved-cursor completion received a different TX_COMMIT");
    boost::system::error_code ignored;
    socket.close(ignored);
    co_return;
}

void test_unnegotiated_begin_rejected_and_route_preserved() {
    P50ServerEndpoint server(Id128::from_u64(770));
    const CStoreGuid c_guid = Id128::from_u64(771);
    const std::vector<uint8_t> input = bytes("route survives unnegotiated TX_BEGIN\n");
    {
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> serving =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<void> peer = asio::co_spawn(
            context,
            raw_unnegotiated_begin_rejected(acceptor.local_endpoint(), c_guid, input),
            asio::use_future);
        context.run();
        peer.get();
        require(serving.get().status == ServerRunStatus::TerminalError,
                "unnegotiated TX_BEGIN left the server run non-terminal");
    }
    {
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> serving =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<void> peer = asio::co_spawn(
            context,
            raw_complete_at_preserved_cursor(acceptor.local_endpoint(), c_guid, input),
            asio::use_future);
        context.run();
        peer.get();
        require(serving.get().status == ServerRunStatus::Completed &&
                    copy_input(server, c_guid) == input,
                "post-rejection completion at the preserved cursor failed");
    }
}


// local-oracle's b19e41a9 HOLD closure: the client-side outbound mask law was
// a deletion survivor (session-level ResetAckMutation::ProfileMask rejects a
// bad SESSION_STATE long before the outbound guard; the client's own prepare
// path only mints ZSTD_TU begins, so no public flow reaches the clause). The
// law now lives in the shared require_outbound_profile_negotiated seam --
// called by the production send path and driven here directly with a COPIED
// begin against a live established session, mutating no retained state. The
// wire-level backstop for a call-site deletion remains the server's own mask
// law, deletion-tested separately.
void test_client_outbound_mask_law() {
    TestClient client(Id128::from_u64(910));
    P50ServerEndpoint server(Id128::from_u64(911));
    const std::vector<uint8_t> input = bytes("outbound mask law: first exact input\n");
    const PairResult first = run_pair(client.endpoint, server, admit(client, input));
    require(first.client.status == ClientRunStatus::Committed &&
                first.server.status == ServerRunStatus::Completed,
            "outbound-mask fixture could not establish its live session");

    const ZstdTuEnvelope shaped = encode_zstd_tu(
        HistoryNonce{1}, RelSeq{0}, TuSeq{77}, Digest128{}, input);
    TxBegin crafted = make_begin(shaped, HistoryNonce{1}, Digest128{});
    crafted.profile = ProfileId::P29V1;  // wire-valid, outside the TU-only mask
    bool rejected_by_the_mask_law = false;
    try {
        require_outbound_profile_negotiated(profile_bit(ProfileId::ZSTD_TU), crafted);
    } catch (const std::logic_error& error) {
        rejected_by_the_mask_law =
            std::string_view(error.what()) ==
            "C selected a profile outside the negotiated mask";
    }
    require(rejected_by_the_mask_law,
            "client outbound mask law did not reject an unnegotiated begin copy");
    require_outbound_profile_negotiated(profile_bit(ProfileId::ZSTD_TU) |
                                            profile_bit(ProfileId::P29V1),
                                        crafted);  // negotiated -> must not throw

    const std::vector<uint8_t> second_input = bytes("outbound mask law: follow-up commit\n");
    const PairResult second =
        run_pair(client.endpoint, server, admit(client, second_input));
    require(second.client.status == ClientRunStatus::Committed &&
                second.server.status == ServerRunStatus::Completed &&
                copy_input(server, Id128::from_u64(910)) == second_input,
            "outbound mask probe disturbed the live route");
}


// local-oracle's 414a917e HOLD closure: the production CLIENT CALLSITE of the
// outbound mask law, exercised end-to-end. The copied-begin control hook
// flips the coroutine's own outbound TX_BEGIN copy to wire-valid P29V1 after
// construction and before validation/send; the production validator must
// reject through the production call, the fake F must observe complete
// silence after route establishment (no TX_BEGIN/BODY frame ever arrives),
// and the SAME prepared work must then commit exactly on a clean retry.
// Deleting either the predicate clause or the production callsite makes the
// begin reach the peer, which trips the silence assertion below.
asio::awaitable<void> raw_established_then_silent_peer(tcp::acceptor& acceptor,
                                                       FStoreGuid f_guid) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    co_await acceptor.async_accept(socket, asio::use_awaitable);
    const SessionHello hello = raw_decode<SessionHello>(
        co_await raw_read(socket, kInitialMaxFramePayload));

    SessionState fresh;
    fresh.wire_revision = kP50WireRevision;
    fresh.negotiated_profiles = profile_bit(ProfileId::ZSTD_TU);
    fresh.limits = hello.limits;
    fresh.f_store_guid = f_guid;
    co_await raw_write(socket, fresh);

    const HistoryReset reset = raw_decode<HistoryReset>(
        co_await raw_read(socket, fresh.limits.max_frame_payload));
    SessionState ack = fresh;
    ack.namespace_present = true;
    ack.route_present = true;
    ack.history_nonce = reset.history_nonce;
    ack.next_rel_seq = RelSeq{0};
    ack.state_digest = reset.initial_state_digest;
    co_await raw_write(socket, ack);

    // The rejected outbound begin must never reach the wire: the next event
    // on this socket has to be the client closing it, not a frame.
    co_await raw_wait_for_close(socket);
}

void test_client_outbound_callsite_law() {
    TestClient client(Id128::from_u64(920));
    const std::vector<uint8_t> input = bytes("callsite law: prepared survives local rejection\n");
    const PreparedTuHandle prepared = admit(client, input);

    EndpointIoControl unnegotiated_control;
    unnegotiated_control.outbound_begin_transform = [](
        const TxBegin& begin, std::span<const uint8_t>) {
        TxBegin crafted = begin;
        crafted.profile = ProfileId::P29V1;  // wire-valid, unnegotiated
        return crafted;
    };
    const ClientRunResult rejected = run_client_with_raw_peer(
        client, prepared,
        [&](tcp::acceptor& acceptor) {
            return raw_established_then_silent_peer(acceptor, Id128::from_u64(921));
        },
        unnegotiated_control);
    require(rejected.status == ClientRunStatus::TerminalError &&
                rejected.terminal_error &&
                rejected.terminal_error->detail ==
                    "C selected a profile outside the negotiated mask",
            "production callsite did not reject the transformed unnegotiated begin");
    require(!client.has_active_transaction() || client.has_reconciliation_work(),
            "local outbound rejection left the client without recoverable work");

    P50ServerEndpoint good(Id128::from_u64(922));
    require(run_pair(client, good).client.status == ClientRunStatus::Committed &&
                copy_input(good, client.c_store_guid()) == input,
            "outbound callsite rejection lost or changed the prepared work");
}

void test_reserved_zero_endpoint_values() {
    {
        P50ServerEndpoint server(Id128::from_u64(628));
        asio::io_context context;
        tcp::acceptor acceptor(context,
                               {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> server_result =
            asio::co_spawn(context, server.accept_one(acceptor),
                           asio::use_future);
        std::future<void> peer_result = asio::co_spawn(
            context,
            raw_zero_c_store_hello(acceptor.local_endpoint()),
            asio::use_future);
        context.run();
        peer_result.get();
        require(server_result.get().status == ServerRunStatus::TerminalError &&
                    server.namespace_count() == 0,
                "zero C_STORE_GUID left an F namespace or route");

        TestClient recovered(Id128::from_u64(629));
        const std::vector<uint8_t> input =
            bytes("valid relationship after reserved zero C GUID\n");
        require(run_pair(recovered, server, admit(recovered, input))
                            .client.status == ClientRunStatus::Committed &&
                    copy_input(server, recovered.c_store_guid()) == input,
                "zero C_STORE_GUID changed the subsequent cold relationship");
    }

    {
        P50ServerEndpoint server(Id128::from_u64(632));
        const CStoreGuid c_guid = Id128::from_u64(633);
        asio::io_context context;
        tcp::acceptor acceptor(context,
                               {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> server_result =
            asio::co_spawn(context, server.accept_one(acceptor),
                           asio::use_future);
        std::future<void> peer_result = asio::co_spawn(
            context,
            raw_zero_history_reset(acceptor.local_endpoint(), c_guid),
            asio::use_future);
        context.run();
        peer_result.get();
        require(server_result.get().status == ServerRunStatus::TerminalError &&
                    server.namespace_count() == 0 &&
                    !copy_input(server, c_guid),
                "zero HISTORY_NONCE left an F namespace or route");

        TestClient recovered(c_guid);
        const std::vector<uint8_t> input =
            bytes("valid reset after reserved zero nonce\n");
        require(run_pair(recovered, server, admit(recovered, input))
                            .client.status == ClientRunStatus::Committed &&
                    copy_input(server, c_guid) == input,
                "zero HISTORY_NONCE changed the subsequent cold relationship");
    }

    {
        TestClient client(Id128::from_u64(634));
        const std::vector<uint8_t> input =
            bytes("reserved zero F_STORE_GUID\n");
        const ClientRunResult terminal = run_client_with_raw_peer(
            client, admit(client, input), [](tcp::acceptor& acceptor) {
                return raw_zero_f_store_state_peer(acceptor);
            });
        require(terminal.status == ClientRunStatus::TerminalError &&
                    terminal.observation != ClientRunObservation::ExactCommitObserved &&
                    client.has_reconciliation_work() &&
                    !client.has_active_transaction() &&
                    !client.endpoint.f_store_guid() &&
                    client.endpoint.next_rel_seq() == RelSeq{0} &&
                    client.endpoint.state_digest() == Digest128{},
                "zero F_STORE_GUID left C relationship state");
        P50ServerEndpoint good(Id128::from_u64(635));
        require(run_pair(client, good).client.status ==
                        ClientRunStatus::Committed &&
                    copy_input(good, client.c_store_guid()) == input,
                "zero F_STORE_GUID changed queued retry work");
    }

    {
        TestClient client(Id128::from_u64(636));
        const std::vector<uint8_t> input = bytes("reserved zero ERROR code\n");
        const ClientRunResult terminal = run_client_with_raw_peer(
            client, admit(client, input), [](tcp::acceptor& acceptor) {
                return raw_zero_error_code_peer(acceptor);
            });
        require(terminal.status == ClientRunStatus::TerminalError &&
                    terminal.observation != ClientRunObservation::ExactCommitObserved && terminal.terminal_error &&
                    terminal.terminal_error->code != 0 &&
                    client.has_reconciliation_work() &&
                    !client.has_active_transaction() &&
                    !client.endpoint.f_store_guid() &&
                    client.endpoint.next_rel_seq() == RelSeq{0} &&
                    client.endpoint.state_digest() == Digest128{},
                "zero ERROR code left C relationship state");
        P50ServerEndpoint good(Id128::from_u64(637));
        require(run_pair(client, good).client.status ==
                        ClientRunStatus::Committed &&
                    copy_input(good, client.c_store_guid()) == input,
                "zero ERROR code changed queued retry work");
    }
}

void test_interrupted_begin_identity() {
    ActionTrace actions;
    P50ServerEndpoint server(Id128::from_u64(640), {}, nullptr, &actions);
    const CStoreGuid c_guid = Id128::from_u64(641);
    const std::vector<uint8_t> input = pseudo_random_bytes(8192);

    InterruptedRawTu interrupted;
    {
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> server_result =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<InterruptedRawTu> peer_result = asio::co_spawn(
            context,
            raw_partial_body_disconnect(acceptor.local_endpoint(), c_guid, input),
            asio::use_future);
        context.run();
        interrupted = peer_result.get();
        require(server_result.get().status == ServerRunStatus::Disconnected &&
                    !copy_input(server, c_guid),
                "partial BODY disconnect materialized or committed input");
    }

    {
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> server_result =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<void> peer_result = asio::co_spawn(
            context,
            raw_different_begin_rejected(acceptor.local_endpoint(), c_guid,
                                         interrupted.begin),
            asio::use_future);
        context.run();
        peer_result.get();
        require(server_result.get().status == ServerRunStatus::TerminalError &&
                    !copy_input(server, c_guid),
                "different begin at the interrupted cursor replaced retained identity");
    }

    {
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<ServerRunResult> server_result =
            asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
        std::future<void> peer_result = asio::co_spawn(
            context,
            raw_exact_whole_tu_replay(acceptor.local_endpoint(), c_guid, interrupted),
            asio::use_future);
        context.run();
        peer_result.get();
        require(server_result.get().status == ServerRunStatus::Completed &&
                    copy_input(server, c_guid) == input,
                "exact whole-TU replay did not commit after partial BODY disconnect");
    }

    require(std::any_of(actions.records().begin(), actions.records().end(),
                        [&](const ActionRecord& record) {
                            return record.actor == ActorSide::F &&
                                   record.action == ActionType::ACTIVE_REPLAYED &&
                                   record.transaction_digest ==
                                       interrupted.begin.transaction_digest;
                        }),
            "exact whole-TU replay omitted the retained transaction identity");
}

void test_terminal_body_failure_identity() {
    const std::vector<uint8_t> input = pseudo_random_bytes(8192);
    uint64_t identity = 650;
    for (const auto& [scenario, label] :
         std::array{std::pair{TerminalBodyFailure::ZeroProgress,
                              "zero-progress BODY"},
                    std::pair{TerminalBodyFailure::Excess,
                              "excess BODY"},
                    std::pair{TerminalBodyFailure::ComponentDigest,
                              "component digest"}}) {
        const FStoreGuid f_guid = Id128::from_u64(identity++);
        CompletionLog completions;
        P50ServerEndpoint server(f_guid, {}, &completions);
        const CStoreGuid c_guid = Id128::from_u64(identity++);
        auto failure = force_terminal_body_failure(server, c_guid, input, scenario);
        InterruptedRawTu interrupted = std::move(failure.first);
        ServerRunResult terminal = std::move(failure.second);
        require(terminal.status == ServerRunStatus::TerminalError &&
                    !copy_input(server, c_guid),
                std::string(label) +
                    " failure materialized or committed input");
        require(std::any_of(
                    completions.completions().begin(),
                    completions.completions().end(),
                    [&](const AsyncCompletion& completion) {
                        const CompletionStamp& stamp = completion.stamp;
                        return stamp.actor == ActorSide::F &&
                               stamp.operation == AsyncOperationKind::WriteFragment &&
                               stamp.c_store_guid == c_guid &&
                               stamp.f_store_guid == f_guid &&
                               stamp.session_serial == terminal.session_serial &&
                               stamp.history_nonce == interrupted.begin.history_nonce &&
                               stamp.rel_seq == interrupted.begin.rel_seq &&
                               stamp.tu_seq == interrupted.begin.tu_seq &&
                               stamp.transaction_digest ==
                                   interrupted.begin.transaction_digest &&
                               stamp.raw_digest == interrupted.begin.raw_digest &&
                               stamp.transaction_bound;
                    }),
                std::string(label) +
                    " terminal completion lost the installed operation identity");
        require(force_different_begin_rejected(server, c_guid,
                                               interrupted.begin)
                        .status == ServerRunStatus::TerminalError &&
                    !copy_input(server, c_guid),
                std::string(label) +
                    " failure did not retain the exact TX_BEGIN identity");
        require(force_exact_whole_tu_replay(server, c_guid, interrupted, true)
                            .status == ServerRunStatus::Completed &&
                    copy_input(server, c_guid) == input,
                std::string(label) +
                    " failure did not accept exact whole-TU replay");
    }

    {
        P50ServerEndpoint server(Id128::from_u64(identity++));
        const CStoreGuid c_guid = Id128::from_u64(identity++);
        const auto [interrupted, terminal] = force_terminal_body_failure(
            server, c_guid, input, TerminalBodyFailure::ZeroProgress, false);
        require(terminal.status == ServerRunStatus::TerminalError &&
                    !copy_input(server, c_guid),
                "lost terminal report changed the interrupted transaction");
        require(force_exact_whole_tu_replay(server, c_guid, interrupted, true)
                            .status == ServerRunStatus::Completed &&
                    copy_input(server, c_guid) == input,
                "lost terminal report did not permit exact whole-TU replay");
    }

    for (const auto& [scenario, label] :
         std::array{std::pair{TerminalBodyFailure::ZstdDecode,
                              "Zstd decode"},
                    std::pair{TerminalBodyFailure::RawDigest,
                              "raw digest"}}) {
        P50ServerEndpoint server(Id128::from_u64(identity++));
        const CStoreGuid c_guid = Id128::from_u64(identity++);
        const auto [interrupted, terminal] = force_terminal_body_failure(
            server, c_guid, input, scenario);
        require(terminal.status == ServerRunStatus::TerminalError &&
                    !copy_input(server, c_guid),
                std::string(label) +
                    " failure materialized or committed input");
        require(force_different_begin_rejected(server, c_guid,
                                               interrupted.begin)
                        .status == ServerRunStatus::TerminalError,
                std::string(label) +
                    " failure accepted a different TX_BEGIN at the retained cursor");
        require(force_exact_whole_tu_replay(server, c_guid, interrupted, false)
                            .status == ServerRunStatus::TerminalError &&
                    !copy_input(server, c_guid),
                std::string(label) +
                    " failure did not retain its deterministic exact identity");
    }
}

void test_disconnect_at_each_message_boundary() {
    const std::array<MessageType, 4> client_messages{
        MessageType::SESSION_HELLO, MessageType::HISTORY_RESET, MessageType::TX_BEGIN,
        MessageType::BODY};
    for (size_t index = 0; index != client_messages.size(); ++index) {
        P50ServerEndpoint server(Id128::from_u64(700 + index));
        TestClient client(Id128::from_u64(710 + index));
        EndpointIoControl stop;
        stop.close_after_write = client_messages[index];
        const std::vector<uint8_t> input = bytes("boundary disconnect\n");
        const PairResult result = run_pair(client, server, admit(client, input), stop);
        require(result.client.status == ClientRunStatus::Disconnected,
                "client message-boundary close did not stop the dialogue");
        if (client_messages[index] == MessageType::TX_BEGIN ||
            client_messages[index] == MessageType::BODY)
            require(client.has_active_transaction(),
                    "uncertain client boundary erased active identity");
        const PairResult recovered = run_pair(client, server);
        require(recovered.client.status == ClientRunStatus::Committed &&
                    copy_input(server, client.c_store_guid()) == input,
                "client message-boundary reconnect did not commit exact input");
    }

    const std::array<MessageType, 1> server_messages{MessageType::SESSION_STATE};
    for (size_t index = 0; index != server_messages.size(); ++index) {
        P50ServerEndpoint server(Id128::from_u64(720 + index));
        TestClient client(Id128::from_u64(730 + index));
        EndpointIoControl stop;
        stop.close_after_write = server_messages[index];
        const std::vector<uint8_t> input = bytes("server boundary\n");
        const PairResult result = run_pair(client, server, admit(client, input), {}, stop);
        require(result.client.status == ClientRunStatus::Disconnected,
                "server message-boundary close did not stop the dialogue");
        const PairResult recovered = run_pair(client, server);
        require(recovered.client.status == ClientRunStatus::Committed &&
                    copy_input(server, client.c_store_guid()) == input,
                "server message-boundary reconnect did not commit exact input");
    }

    {
        P50ServerEndpoint server(Id128::from_u64(740));
        TestClient client(Id128::from_u64(741));
        EndpointIoControl stop;
        stop.close_after_write = MessageType::TX_COMMIT;
        const std::vector<uint8_t> input = bytes("close after final commit frame\n");
        const PairResult result = run_pair(client, server, admit(client, input), {}, stop);
        require(result.client.status == ClientRunStatus::Committed &&
                    copy_input(server, client.c_store_guid()) == input,
                "close after the complete TX_COMMIT changed the committed result");
    }
}

void test_client_operation_scoped_cancellation() {
    const std::vector<uint8_t> input = bytes("operation scoped cancellation\n");

    {
        TestClient client(Id128::from_u64(742));
        EndpointIoControl control;
        control.before_first_remote_write = [&client] {
            client.endpoint.request_cancel_for_test();
        };
        asio::io_context context;
        // A listening socket is sufficient for connect completion; the
        // deterministic hook cancels before the first CacheWire write.
        tcp::acceptor acceptor(context,
                               {asio::ip::address_v4::loopback(), 0});
        std::future<ClientRunResult> run = asio::co_spawn(
            context,
            client.endpoint.run(acceptor.local_endpoint(),
                                admit(client, input), control),
            asio::use_future);
        context.run();
        const ClientRunResult cancelled = run.get();
        require(cancelled.status == ClientRunStatus::Disconnected &&
                    cancelled.cancellation ==
                        ClientCancellationDisposition::ReconcileRequired &&
                    (client.has_active_transaction() ||
                     client.has_reconciliation_work()),
                "pre-wire C cancellation was incorrectly settled locally");
    }

    {
        TestClient client(Id128::from_u64(743));
        const PreparedTuHandle prepared = admit(client, input);
        asio::io_context context;
        tcp::acceptor acceptor(context,
                               {asio::ip::address_v4::loopback(), 0});
        std::future<void> peer = asio::co_spawn(
            context, raw_cancel_client_after_hello(acceptor, client.endpoint),
            asio::use_future);
        std::future<ClientRunResult> run = asio::co_spawn(
            context,
            client.endpoint.run(acceptor.local_endpoint(), prepared, {}),
            asio::use_future);
        context.run();
        peer.get();
        const ClientRunResult cancelled = run.get();
        require(cancelled.status == ClientRunStatus::Disconnected &&
                    cancelled.cancellation ==
                        ClientCancellationDisposition::ReconcileRequired &&
                    client.has_reconciliation_work(),
                "post-hello C cancellation silently authorized an abort");

        P50ServerEndpoint server(Id128::from_u64(744));
        const PairResult reconciled = run_pair(client, server);
        require(reconciled.client.status == ClientRunStatus::Committed &&
                    reconciled.client.cancellation ==
                        ClientCancellationDisposition::None &&
                    !client.has_reconciliation_work() &&
                    copy_input(server, client.c_store_guid()) == input,
                "post-hello C cancellation did not preserve exact retry work");
    }
}

void test_component_and_allocation_caps() {
    EndpointCaps ordinary;
    {
        const auto [peer, server] = run_raw_server_case(ordinary, RawCase::ShortBody, 800);
        require(!peer.received_error && server.status == ServerRunStatus::Disconnected,
                "short component was allowed to materialize or commit");
    }
    {
        const auto [peer, server] = run_raw_server_case(ordinary, RawCase::ExcessBody, 801);
        require(peer.received_error && peer.closed_after_error &&
                    server.status == ServerRunStatus::TerminalError,
                "excess component did not terminate the message session");
    }
    for (const auto& [scenario, name, id] :
         std::array{std::tuple{RawCase::EmptyBodyProgress, "zero-progress BODY", uint64_t{805}},
                    std::tuple{RawCase::ComponentDigest, "component digest", uint64_t{806}},
                    std::tuple{RawCase::TransactionDigest, "transaction digest", uint64_t{807}},
                    std::tuple{RawCase::RawDigest, "raw digest", uint64_t{808}},
                    std::tuple{RawCase::WrongProfile, "profile", uint64_t{809}},
                    std::tuple{RawCase::WrongEncoding, "encoding", uint64_t{811}},
                    std::tuple{RawCase::FrameContentSize, "zstd frame size", uint64_t{812}},
                    std::tuple{RawCase::TrailingByte, "trailing zstd byte", uint64_t{813}},
                    std::tuple{RawCase::AppendedEmptyFrame, "appended empty zstd frame",
                               uint64_t{814}},
                    std::tuple{RawCase::AppendedNonemptyFrame, "appended nonempty zstd frame",
                               uint64_t{815}}}) {
        const auto [peer, server] = run_raw_server_case(ordinary, scenario, id);
        require(peer.received_error && peer.closed_after_error &&
                    server.status == ServerRunStatus::TerminalError,
                std::string("invalid ") + name + " did not close terminally");
    }

    EndpointCaps tight;
    tight.zstd.max_encoded_body_bytes = 32;
    tight.zstd.max_raw_bytes = 128;
    {
        const auto [peer, server] = run_raw_server_case(tight, RawCase::DescriptorCap, 802);
        require(peer.received_error && peer.closed_after_error &&
                    server.status == ServerRunStatus::TerminalError,
                "encoded-component descriptor cap was not enforced before data");
    }
    {
        const auto [peer, server] = run_raw_server_case(tight, RawCase::DecodedCap, 803);
        require(peer.received_error && peer.closed_after_error &&
                    server.status == ServerRunStatus::TerminalError,
                "decoded-input cap was not enforced before expansion");
    }

    EndpointCaps frame_tight;
    frame_tight.wire.max_frame_payload = kMandatoryControlFramePayload;
    {
        const auto [peer, server] = run_raw_server_case(frame_tight, RawCase::FrameCap, 804);
        require(peer.received_error && peer.closed_after_error &&
                    peer.error_payload_bytes <= kMandatoryControlFramePayload &&
                    server.status == ServerRunStatus::TerminalError,
                "declared frame cap did not reject before payload allocation");
    }


    EndpointCaps below_control;
    below_control.wire.max_frame_payload = kMandatoryControlFramePayload - 1;
    require_throws<std::invalid_argument>(
        [&] { TestClient rejected(Id128::from_u64(820), below_control); },
        "151-byte client frame cap was accepted");
    require_throws<std::invalid_argument>(
        [&] { P50ServerEndpoint rejected(Id128::from_u64(821), below_control); },
        "151-byte server frame cap was accepted");

    EndpointCaps exact_control;
    exact_control.wire.max_frame_payload = kMandatoryControlFramePayload;
    for (bool client_is_tight : {false, true}) {
        const EndpointCaps client_caps = client_is_tight ? exact_control : ordinary;
        const EndpointCaps server_caps = client_is_tight ? ordinary : exact_control;
        TestClient client(Id128::from_u64(830 + client_is_tight), client_caps);
        P50ServerEndpoint server(Id128::from_u64(840 + client_is_tight), server_caps);
        const std::vector<uint8_t> input = pseudo_random_bytes(4096);
        const PairResult result = run_pair(client, server, admit(client, input));
        require(result.client.status == ClientRunStatus::Committed &&
                    copy_input(server, client.c_store_guid()) == input,
                "asymmetric exact-152-byte negotiation did not complete");
    }
}

void test_two_client_one_server_isolation() {
    ActionTrace actions;
    CompletionLog completions;
    P50ServerEndpoint server(Id128::from_u64(860), {}, &completions, &actions);
    TestClient first(Id128::from_u64(861), {}, HistoryNonce{201},
                     &completions, &actions);
    TestClient second(Id128::from_u64(862), {}, HistoryNonce{301},
                      &completions, &actions);
    const std::vector<uint8_t> first_input =
        bytes("C-one exact input on shared F\n");
    const std::vector<uint8_t> second_input =
        bytes("C-two independent exact input on shared F\n");
    const PreparedTuHandle first_prepared = admit(first, first_input);
    const PreparedTuHandle second_prepared = admit(second, second_input);

    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    std::future<ServerRunResult> first_server =
        asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
    std::future<ServerRunResult> second_server =
        asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
    std::future<ClientRunResult> first_client = asio::co_spawn(
        context, first.endpoint.run(acceptor.local_endpoint(), first_prepared),
        asio::use_future);
    std::future<ClientRunResult> second_client = asio::co_spawn(
        context, second.endpoint.run(acceptor.local_endpoint(), second_prepared),
        asio::use_future);
    context.run();

    require(first_client.get().status == ClientRunStatus::Committed &&
                second_client.get().status == ClientRunStatus::Committed &&
                first_server.get().status == ServerRunStatus::Completed &&
                second_server.get().status == ServerRunStatus::Completed &&
                server.namespace_count() == 2 &&
                copy_input(server, first.c_store_guid()) == first_input &&
                copy_input(server, second.c_store_guid()) == second_input &&
                first.endpoint.next_rel_seq() == RelSeq{1} &&
                second.endpoint.next_rel_seq() == RelSeq{1},
            "concurrent C2F1 dialogues crossed namespace or route state");

    const std::vector<uint8_t> first_followup =
        bytes("C-one advances without changing C-two\n");
    const PairResult advanced =
        run_pair(first, server, admit(first, first_followup));
    require(advanced.client.status == ClientRunStatus::Committed &&
                server.namespace_count() == 2 &&
                copy_input(server, first.c_store_guid()) == first_followup &&
                copy_input(server, second.c_store_guid()) == second_input &&
                first.endpoint.next_rel_seq() == RelSeq{2} &&
                second.endpoint.next_rel_seq() == RelSeq{1},
            "one C route advance changed the other C namespace");

    bool first_f_completion = false;
    bool second_f_completion = false;
    for (const AsyncCompletion& completion : completions.completions()) {
        if (completion.stamp.actor != ActorSide::F)
            continue;
        first_f_completion = first_f_completion ||
                             completion.stamp.c_store_guid == first.c_store_guid();
        second_f_completion = second_f_completion ||
                              completion.stamp.c_store_guid == second.c_store_guid();
    }
    require(first_f_completion && second_f_completion,
            "C2F1 completion identities did not retain both C namespaces");
    require_trace(actions, "C2F1 endpoint-isolation trace");
}

void report_zstd1_metrics(bool enforce_performance_floor) {
    constexpr size_t input_bytes = size_t{16} << 20;
    constexpr unsigned iterations = 4;
    std::vector<uint8_t> input(input_bytes);
    static constexpr std::string_view line =
        "template<class T> inline T p50_value(T value) { return value + 1; }\n";
    for (size_t offset = 0; offset != input.size();) {
        const size_t count = std::min(line.size(), input.size() - offset);
        std::copy_n(line.begin(), count, input.begin() + offset);
        offset += count;
    }
    for (size_t offset = 4096; offset < input.size(); offset += 4096)
        input[offset] ^= static_cast<uint8_t>(offset >> 12);

    ZstdTuCodec codec(1);
    ZstdTuEnvelope prepared;
    const auto encode_start = std::chrono::steady_clock::now();
    for (unsigned iteration = 0; iteration != iterations; ++iteration)
        prepared = codec.encode(HistoryNonce{1}, RelSeq{0}, TuSeq{iteration}, Digest128{},
                                input, {uint64_t{64} << 20, uint64_t{64} << 20});
    const auto encode_stop = std::chrono::steady_clock::now();
    TxBegin begin = make_begin(prepared, HistoryNonce{1},
                               initial_route_digest(Id128::from_u64(1), HistoryNonce{1}));
    std::vector<uint8_t> decoded;
    const auto decode_start = std::chrono::steady_clock::now();
    for (unsigned iteration = 0; iteration != iterations; ++iteration)
        decoded = codec.decode(begin, prepared.body,
                               {uint64_t{64} << 20, uint64_t{64} << 20});
    const auto decode_stop = std::chrono::steady_clock::now();
    require(decoded == input, "Zstd1 metric loop did not decode exactly");

    const double encoded_seconds =
        std::chrono::duration<double>(encode_stop - encode_start).count();
    const double decoded_seconds =
        std::chrono::duration<double>(decode_stop - decode_start).count();
    const double total_gb = static_cast<double>(input.size()) * iterations / 1'000'000'000.0;
    const double encode_gbps = total_gb / encoded_seconds;
    const double decode_gbps = total_gb / decoded_seconds;
    std::cout << "p50_endpoint_test: zstd1 raw_bytes=" << input.size()
              << " compressed_bytes=" << prepared.body.size() << " encode_GBps=" << encode_gbps
              << " decode_GBps=" << decode_gbps << '\n';
    if (enforce_performance_floor)
        require(encode_gbps >= 0.5 && decode_gbps >= 0.5,
                "quietbox Zstd1 throughput is below 0.5 GB/s");
}

// Capture the interner's own stderr reports (one JSON line per "nonrunnable"
// event) for a bounded region, so a test can count them exactly.
class StderrCapture {
public:
    StderrCapture() {
        std::fflush(stderr);
        saved_ = ::dup(STDERR_FILENO);
        int pipefd[2];
        if (::pipe(pipefd) != 0 || saved_ < 0) {
            if (saved_ >= 0) ::close(saved_);
            saved_ = -1;
            return;
        }
        ::dup2(pipefd[1], STDERR_FILENO);
        ::close(pipefd[1]);
        reader_ = pipefd[0];
    }
    ~StderrCapture() {
        if (reader_ >= 0) {
            ::close(reader_);
        }
        if (saved_ >= 0) {
            ::dup2(saved_, STDERR_FILENO);
            ::close(saved_);
        }
        std::fflush(stderr);
    }
    [[nodiscard]] std::string drained() const {
        if (reader_ < 0)
            return {};
        std::string result;
        char buffer[4096];
        // The write end is the still-open fd 2, so a plain read() would
        // block once the pipe is empty; drain only what is already in
        // flight (ioctl FIONREAD then non-blocking semantics via O_NONBLOCK
        // on the reader).
        const int previous = ::fcntl(reader_, F_GETFL);
        if (previous >= 0)
            (void)::fcntl(reader_, F_SETFL, previous | O_NONBLOCK);
        for (;;) {
            const ssize_t got = ::read(reader_, buffer, sizeof(buffer));
            if (got <= 0)
                break;
            result.append(buffer, static_cast<size_t>(got));
        }
        return result;
    }
    [[nodiscard]] static size_t count_lines(const std::string& text,
                                            const char* needle) {
        size_t total = 0;
        std::string_view view(text);
        for (size_t position = 0; position < view.size();) {
            const size_t end = view.find('\n', position);
            const std::string_view line =
                view.substr(position, end == std::string_view::npos
                                           ? std::string_view::npos
                                           : end - position);
            if (line.find(needle) != std::string_view::npos)
                ++total;
            if (end == std::string_view::npos)
                break;
            position = end + 1;
        }
        return total;
    }

private:
    int saved_ = -1;
    int reader_ = -1;
};

void test_split_prepare_marks_nonrunnable_exactly_once() {
    // The split locked/unlocked prepare_source must reproduce the combined
    // path's failure shape exactly: only a failure in the guarded region
    // (the interner's own reserve/process/injected-failure) marks the
    // interner non-runnable and prints a "nonrunnable" line.  The
    // oversize-TU precondition stays an ordinary length_error (no mark, no
    // line), and a call after the interner is already non-runnable throws
    // the precondition logic_error WITHOUT re-marking (no second line).
    // prepare_source is the public path over the same split halves.
    const std::vector<uint8_t> oversize(2048, uint8_t{'x'});
    const std::vector<uint8_t> fine{uint8_t{'#'}, uint8_t{' '},
                                    uint8_t{'a'}, uint8_t{'\n'}};
    // Keep the raw-byte cap small so `oversize` trips the limit while
    // `fine` stays under it; both limits must be nonzero (the authority
    // rejects zero limits at construction).
    ZstdTuLimits intern_limits;
    intern_limits.max_raw_bytes = 1024;
    intern_limits.max_encoded_body_bytes = 64U << 10;
    intern_limits.max_window_log = 10;
    const PreparationAuthorityLimits limits{};
    {
        P50PreparationAuthority authority(CStoreGuid::from_u64(101),
                                          intern_limits, limits, 1,
                                          ProfileId::P29V1, TuSeq{0});
        {
            StderrCapture capture;
            // prepare_source refuses oversize input itself with a null
            // return (the max_raw_bytes check), so drive the underlying
            // typed path through prepare_for_route, which does NOT
            // pre-check the P29V1 TU limit and relies on the interner's
            // own length_error staying ordinary.
            const PreparationRouteKey route{
                CStoreGuid::from_u64(101),
                FStoreGuid::from_u64(202), 1, ProfileId::P29V1};
            const PrepareRequestKey key{1, 1};
            require_throws<std::length_error>(
                [&] {
                    (void)authority.prepare_for_route(route, key, oversize);
                },
                "oversize TU did not throw length_error");
            require(capture.drained().empty(),
                    "oversize TU printed an interner report");
        }
        // A valid TU still interns after the ordinary length_error.
        {
            StderrCapture capture;
            const PreparationRouteKey route{
                CStoreGuid::from_u64(101),
                FStoreGuid::from_u64(202), 1, ProfileId::P29V1};
            const PreparedTuHandle prepared =
                authority.prepare_for_route(route, PrepareRequestKey{1, 1},
                                            fine);
            (void)prepared;
            require(capture.drained().empty(),
                    "valid TU printed an interner report");
        }
    }
    {
        P50PreparationAuthority authority(
            CStoreGuid::from_u64(102), intern_limits, limits, 1,
            ProfileId::P29V1, TuSeq{10},
            P29InternerFaultInjection::FailOnce);
        const PreparationRouteKey route{
            CStoreGuid::from_u64(102), FStoreGuid::from_u64(203), 1,
            ProfileId::P29V1};
        {
            StderrCapture capture;
            require_throws<P29V1CapabilityUnavailable>(
                [&] {
                    (void)authority.prepare_for_route(
                        route, PrepareRequestKey{1, 1}, fine);
                },
                "injected failure did not surface");
            const std::string drained = capture.drained();
            require(StderrCapture::count_lines(
                        drained, "\"event\":\"nonrunnable\"") == 1,
                    "injected failure did not print exactly one "
                    "nonrunnable line");
        }
        {
            StderrCapture capture;
            require_throws<P29V1CapabilityUnavailable>(
                [&] {
                    (void)authority.prepare_for_route(
                        route, PrepareRequestKey{1, 2}, fine);
                },
                "non-runnable interner did not fail closed");
            require(capture.drained().empty(),
                    "post-failure call re-printed a nonrunnable line");
        }
    }
}

} // namespace


// ===== same_commit negative coverage: ordinary commit path =====
// local-oracle ruling (#16, 2026-08-23T16:54Z): a well-formed fake F must not be able to
// close C's active transaction with a TxCommit differing from the live TxBegin in any of the
// six identity fields.  Each mismatch must be rejected exactly, leave the active transaction
// unchanged, and permit a later exact retry to commit.  The baseline (exact commit) proves the
// fake F speaks the protocol; without it a broken fake would fake-pass the matrix.

enum class CommitMismatchField {
    None,
    HistoryNonce,
    RelSeq,
    TuSeq,
    TransactionDigest,
    RawDigest,
    PostStateDigest,
};

TxCommit correct_commit_for(const TxBegin& begin) {
    TxCommit commit;
    commit.history_nonce = begin.history_nonce;
    commit.rel_seq = begin.rel_seq;
    commit.tu_seq = begin.tu_seq;
    commit.transaction_digest = begin.transaction_digest;
    commit.raw_digest = begin.raw_digest;
    commit.post_state_digest =
        compute_post_state_digest(begin.pre_state_digest, begin.history_nonce, begin.rel_seq,
                                  begin.tu_seq, begin.transaction_digest);
    return commit;
}

void mutate_commit(TxCommit& commit, CommitMismatchField field) {
    switch (field) {
    case CommitMismatchField::None:
        break;
    case CommitMismatchField::HistoryNonce:
        commit.history_nonce.value += 1;
        break;
    case CommitMismatchField::RelSeq:
        commit.rel_seq.value += 1;
        break;
    case CommitMismatchField::TuSeq:
        commit.tu_seq.value += 1;
        break;
    case CommitMismatchField::TransactionDigest:
        commit.transaction_digest.bytes[0] ^= 0x80;
        break;
    case CommitMismatchField::RawDigest:
        commit.raw_digest.bytes[0] ^= 0x80;
        break;
    case CommitMismatchField::PostStateDigest:
        commit.post_state_digest.bytes[0] ^= 0x80;
        break;
    }
}

// A raw fake F: accepts one real-client connection, performs the exact fresh-route dance
// (HELLO -> fresh SESSION_STATE -> HISTORY_RESET -> bound acknowledgement), consumes the
// TX_BEGIN and BODY components, then answers with a commit derived from the received begin
// with exactly one field mutated (or none for the baseline), and waits for the peer to close.
asio::awaitable<void> raw_f_serve_and_commit(tcp::acceptor& acceptor, FStoreGuid f_guid,
                                             CommitMismatchField field, TxBegin& out_begin,
                                             std::vector<uint8_t>* out_body = nullptr) {
    tcp::socket socket = co_await acceptor.async_accept(asio::use_awaitable);
    const SessionHello hello =
        raw_decode<SessionHello>(co_await raw_read(socket, kInitialMaxFramePayload));
    const SessionSelection selection =
        negotiate_session(hello, kP50WireRevision, kKnownProfileMask);
    const uint32_t cap = selection.limits.max_frame_payload;
    SessionState fresh;
    fresh.wire_revision = selection.wire_revision;
    fresh.negotiated_profiles = selection.negotiated_profiles;
    fresh.limits = selection.limits;
    fresh.f_store_guid = f_guid;
    co_await raw_write(socket, Message{fresh});
    Frame next = co_await raw_read(socket, cap);
    TxBegin& begin = out_begin;
    if (next.type == MessageType::HISTORY_RESET) {
        const HistoryReset reset = raw_decode<HistoryReset>(next);
        SessionState ack = fresh;
        ack.namespace_present = true;
        ack.route_present = true;
        ack.history_nonce = reset.history_nonce;
        ack.next_rel_seq = RelSeq{0};
        ack.state_digest = reset.initial_state_digest;
        co_await raw_write(socket, Message{ack});
        begin = raw_decode<TxBegin>(co_await raw_read(socket, cap));
    } else {
        begin = raw_decode<TxBegin>(next);
    }
    uint64_t body_bytes = 0;
    while (body_bytes < begin.body.encoded_bytes) {
        const BodyMessage body = raw_decode<BodyMessage>(co_await raw_read(socket, cap));
        if (body.bytes.empty())
            break;
        body_bytes += body.bytes.size();
        if (out_body)
            out_body->insert(out_body->end(), body.bytes.begin(), body.bytes.end());
    }
    TxCommit commit = correct_commit_for(begin);
    mutate_commit(commit, field);
    co_await raw_write(socket, Message{commit});
    co_await raw_wait_for_close(socket);
    co_return;
}


// Ordinary same-F exact retry (bigoracle endpoint-HOLD shape B): after a mismatch refusal the
// SAME F presents the unchanged retained route; the client must replay the IDENTICAL TxBegin
// and BODY and commit with reconnect == ExactMatch.  The fake itself throws if C resets,
// rebinds, or reconstructs any begin/body identity.
asio::awaitable<void> raw_f_exact_retry(tcp::acceptor& acceptor, FStoreGuid same_f_guid,
                                        TxBegin captured_begin,
                                        std::vector<uint8_t> captured_body) {
    tcp::socket socket = co_await acceptor.async_accept(asio::use_awaitable);
    const SessionHello hello =
        raw_decode<SessionHello>(co_await raw_read(socket, kInitialMaxFramePayload));
    const SessionSelection selection =
        negotiate_session(hello, kP50WireRevision, kKnownProfileMask);
    SessionState peer;
    peer.wire_revision = selection.wire_revision;
    peer.negotiated_profiles = selection.negotiated_profiles;
    peer.limits = selection.limits;
    peer.f_store_guid = same_f_guid;
    peer.namespace_present = true;
    peer.route_present = true;
    peer.history_nonce = captured_begin.history_nonce;
    peer.next_rel_seq = captured_begin.rel_seq;
    peer.state_digest = captured_begin.pre_state_digest;
    co_await raw_write(socket, Message{peer});
    const TxBegin replayed =
        raw_decode<TxBegin>(co_await raw_read(socket, peer.limits.max_frame_payload));
    if (replayed != captured_begin)
        throw std::logic_error("ordinary retry changed retained TxBegin identity");
    std::vector<uint8_t> replay_body;
    while (replay_body.size() < replayed.body.encoded_bytes) {
        BodyMessage part = raw_decode<BodyMessage>(
            co_await raw_read(socket, peer.limits.max_frame_payload));
        if (part.bytes.empty())
            break;
        replay_body.insert(replay_body.end(), part.bytes.begin(), part.bytes.end());
    }
    if (replay_body != captured_body)
        throw std::logic_error("ordinary retry changed retained BODY bytes");
    co_await raw_write(socket, Message{correct_commit_for(captured_begin)});
    co_await raw_wait_for_close(socket);
    co_return;
}

void test_commit_identity_negative_matrix() {
    const std::array cases{
        std::pair{CommitMismatchField::HistoryNonce, "history nonce"},
        std::pair{CommitMismatchField::RelSeq, "REL_SEQ"},
        std::pair{CommitMismatchField::TuSeq, "TU_SEQ"},
        std::pair{CommitMismatchField::TransactionDigest, "transaction digest"},
        std::pair{CommitMismatchField::RawDigest, "raw digest"},
        std::pair{CommitMismatchField::PostStateDigest, "post-state digest"},
    };
    uint64_t identity = 7000;

    {
        TestClient client(Id128::from_u64(identity++));
        const std::vector<uint8_t> input = pseudo_random_bytes(4096);
        const PreparedTuHandle prepared = admit(client, input);
        TxBegin baseline_begin;
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<void> fake = asio::co_spawn(
            context,
            raw_f_serve_and_commit(acceptor, Id128::from_u64(identity++),
                                   CommitMismatchField::None, baseline_begin),
            asio::use_future);
        std::future<ClientRunResult> run = asio::co_spawn(
            context, client.endpoint.run(acceptor.local_endpoint(), prepared, {}),
            asio::use_future);
        context.run();
        fake.get();
        const ClientRunResult baseline = run.get();
        require(baseline.status == ClientRunStatus::Committed && !client.has_active_transaction(),
                "raw fake F baseline exact commit was not accepted");
    }

    for (const auto& [field, name] : cases) {
        // Client A: refusal -> SAME-F exact retry (the REQUIRED exact-retry close: identical
        // TxBegin and BODY replayed on the unchanged route, commit with ExactMatch).
        {
            TestClient client(Id128::from_u64(identity++));
            const std::vector<uint8_t> input = pseudo_random_bytes(4096);
            const PreparedTuHandle prepared = admit(client, input);
            TxBegin observed_begin;
            std::vector<uint8_t> observed_body;
            const FStoreGuid fake_f = Id128::from_u64(identity++);
            {
                asio::io_context context;
                tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
                std::future<void> fake = asio::co_spawn(
                    context,
                    raw_f_serve_and_commit(acceptor, fake_f, field, observed_begin,
                                           &observed_body),
                    asio::use_future);
                std::future<ClientRunResult> run = asio::co_spawn(
                    context, client.endpoint.run(acceptor.local_endpoint(), prepared, {}),
                    asio::use_future);
                context.run();
                fake.get();
                const ClientRunResult rejected = run.get();
                require(rejected.status == ClientRunStatus::TerminalError &&
                            client.has_active_transaction() &&
                            client.endpoint.next_rel_seq() == observed_begin.rel_seq &&
                            client.endpoint.state_digest() ==
                                observed_begin.pre_state_digest &&
                            client.endpoint.f_store_guid() ==
                                std::optional<FStoreGuid>(fake_f),
                        std::string(name) +
                            " commit mismatch rejection did not preserve the transaction cursor");
            }
            {
                asio::io_context context;
                tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
                std::future<void> fake = asio::co_spawn(
                    context,
                    raw_f_exact_retry(acceptor, fake_f, observed_begin, observed_body),
                    asio::use_future);
                std::future<ClientRunResult> run = asio::co_spawn(
                    context, client.endpoint.run(acceptor.local_endpoint(), {}, {}),
                    asio::use_future);
                context.run();
                fake.get();
                const ClientRunResult retried = run.get();
                require(retried.status == ClientRunStatus::Committed &&
                            retried.reconnect == EndpointReconnectOutcome::ExactMatch &&
                            !client.has_active_transaction(),
                        std::string(name) +
                            " refusal did not permit the same-F exact retry to commit");
            }
        }
        // Client B: refusal -> different-F ColdFStore row, retained as a DISTINCT control:
        // prepared work survives rejection across an incarnation replacement.
        {
            TestClient client(Id128::from_u64(identity++));
            const std::vector<uint8_t> input = pseudo_random_bytes(4096);
            const PreparedTuHandle prepared = admit(client, input);
            TxBegin observed_begin;
            const FStoreGuid fake_f = Id128::from_u64(identity++);
            asio::io_context context;
            tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
            std::future<void> fake = asio::co_spawn(
                context,
                raw_f_serve_and_commit(acceptor, fake_f, field, observed_begin),
                asio::use_future);
            std::future<ClientRunResult> run = asio::co_spawn(
                context, client.endpoint.run(acceptor.local_endpoint(), prepared, {}),
                asio::use_future);
            context.run();
            fake.get();
            require(run.get().status == ClientRunStatus::TerminalError &&
                        client.has_active_transaction(),
                    std::string(name) + " (cold-F row) mismatch was not refused");
            P50ServerEndpoint real_server(Id128::from_u64(identity++));
            const PairResult retried = run_pair(client, real_server);
            require(retried.client.status == ClientRunStatus::Committed &&
                        retried.client.reconnect == EndpointReconnectOutcome::ColdFStore &&
                        !client.has_active_transaction() &&
                        copy_input(real_server, client.c_store_guid()) == input,
                    std::string(name) +
                        " prepared work did not survive incarnation replacement");
        }
    }
}

// ===== Step 4 sub-step 4d core: K>1 successor window (P50_ROUTE_PIPELINING.md) =====
//
// The F side of these fixtures is a raw socket coroutine driving the REAL
// FStore state machine (begin/append_body/need_frames/append_fill_v1/
// materialize_and_verify/commit_input), not P50ServerEndpoint: the real
// server endpoint still rejects a successor TX_BEGIN while its predecessor
// is ACTIVE (FStore::begin's occupied-slot check, cache/p50_slice0.cpp) --
// F-side parking is Step 5. The fake implements the approved release shape
// instead (design section 1.2 point 4): a successor connection defers
// stage()+STATE until its predecessor's commit_input, so the successor
// observes the post-predecessor cursor, exactly what C's predicted witness
// expects.

struct RawParkedRouteState {
    explicit RawParkedRouteState(CStoreGuid c_guid_value, FStoreGuid f_guid_value)
        : store(f_guid_value), c_guid(c_guid_value) {
        session = store.connect(c_guid);
    }
    FStore store;
    CStoreGuid c_guid;
    SessionHandle session{};
    bool route_established = false;
    // Park release is a plain flag, polled with a 1 ms asio timer from the
    // successor connection (a std::future::get() would block the one
    // io_context thread these fixtures share and deadlock the head).
    std::atomic<bool> predecessor_committed{false};
    // T2: set by the successor after its COMMIT write; the head polls it
    // before writing its own COMMIT so the successor's COMMIT observably
    // precedes the head's on the wire.
    std::atomic<bool> successor_commit_written{false};
    // T3: poison the released successor's staged STATE digest.
    std::atomic<bool> corrupt_successor_state{false};
    // T4: the head connection drops right after NEED (no COMMIT), with the
    // successor already outstanding.
    std::atomic<bool> head_drop_after_need{false};
    // T5: the head connection never writes COMMIT; the successor's own wait
    // deadline must fire first.
    std::atomic<bool> head_never_commits{false};
    // T8 (review point 2): the head commits at F (materialize+commit_input)
    // but drops the connection without writing the COMMIT frame, so a later
    // STATE proves the commit via LOST_COMMIT_ACCEPTED. The route must stay
    // usable: no sticky terminal, no poison.
    std::atomic<bool> head_commit_but_drop_commit_frame{false};
    // T9 (follow-up review): the successor connection dies after HELLO --
    // before its released STATE -- with the head healthy. C must poison the
    // route (the successor's predict-commit advanced C's serializer past a
    // TX F will never apply), the head must still commit, and the next TX
    // must recover through the cold HISTORY_RESET path.
    std::atomic<bool> successor_drop_after_hello{false};
};

// One connection of the parked fake F. `park` is set for successor
// connections: STATE is deferred until the predecessor's commit_input (the
// release trigger of design section 1.2 point 5), then presents the
// post-predecessor cursor -- exactly C's predicted witness.
asio::awaitable<void> raw_parked_f_serve_connection(
    tcp::acceptor& acceptor, const std::shared_ptr<RawParkedRouteState>& state,
    bool park, bool wait_for_successor_commit, TxBegin& out_begin,
    std::vector<uint8_t>& out_body) {
    tcp::socket socket = co_await acceptor.async_accept(asio::use_awaitable);
    const auto executor = co_await asio::this_coro::executor;
    const SessionHello hello =
        raw_decode<SessionHello>(co_await raw_read(socket, kInitialMaxFramePayload));
    const SessionSelection selection =
        negotiate_session(hello, kP50WireRevision, kKnownProfileMask);
    const uint32_t cap = selection.limits.max_frame_payload;
    TxBegin& begin = out_begin;
    if (park && state->successor_drop_after_hello.load()) {
        // T9: the successor's own socket dies before its released STATE.
        boost::system::error_code ignored;
        socket.close(ignored);
        co_return;
    }
    bool have_begin = false;
    if (!state->route_established) {
        SessionState fresh;
        fresh.wire_revision = selection.wire_revision;
        fresh.negotiated_profiles = selection.negotiated_profiles;
        fresh.limits = selection.limits;
        fresh.f_store_guid = state->store.guid();
        co_await raw_write(socket, Message{fresh});
        const HistoryReset reset =
            raw_decode<HistoryReset>(co_await raw_read(socket, cap));
        state->store.start_route(state->session, reset.history_nonce,
                                 reset.initial_state_digest);
        state->route_established = true;
    } else if (!park) {
        // Established route: present the live cursor. A cold C endpoint
        // (post-pipeline-poison reset) answers with HISTORY_RESET; apply
        // it and re-present, exactly the unchanged server behavior.
        SessionState live = state->store.resume(state->session);
        live.wire_revision = selection.wire_revision;
        live.negotiated_profiles = selection.negotiated_profiles;
        co_await raw_write(socket, Message{live});
        Frame next = co_await raw_read(socket, cap);
        if (next.type == MessageType::HISTORY_RESET) {
            const HistoryReset reset = raw_decode<HistoryReset>(next);
            // The real server drops the retained route before installing
            // the reset one (FStore::start_route refuses an existing
            // route; forget_route is the same legality gate the server
            // applies: only outside ACTIVE_TX).
            state->store.forget_route(state->session);
            state->store.start_route(state->session, reset.history_nonce,
                                      reset.initial_state_digest);
        } else {
            begin = raw_decode<TxBegin>(next);
            have_begin = true;
        }
    }
    if (park) {
        // Deferred stage()/STATE: park (polling, never blocking) until the
        // predecessor committed, then present the post-predecessor cursor.
        asio::steady_timer park_timer(executor);
        while (!state->predecessor_committed.load()) {
            park_timer.expires_after(std::chrono::milliseconds(1));
            boost::system::error_code ignored;
            co_await park_timer.async_wait(
                asio::redirect_error(asio::use_awaitable, ignored));
        }
        SessionState staged = state->store.resume(state->session);
        staged.wire_revision = selection.wire_revision;
        staged.negotiated_profiles = selection.negotiated_profiles;
        if (state->corrupt_successor_state.load())
            staged.state_digest.bytes[0] ^= 0x80;
        co_await raw_write(socket, Message{staged});
    } else if (!have_begin) {
        // Fresh-route or reset-just-applied: acknowledge the new route with
        // its own cursor, then read the begin.
        SessionState ack = state->store.resume(state->session);
        ack.wire_revision = selection.wire_revision;
        ack.negotiated_profiles = selection.negotiated_profiles;
        co_await raw_write(socket, Message{ack});
    }
    if (!have_begin)
        begin = raw_decode<TxBegin>(co_await raw_read(socket, cap));
    state->store.begin(state->session, begin);
    uint64_t body_bytes = 0;
    while (body_bytes < begin.body.encoded_bytes) {
        const BodyMessage body =
            raw_decode<BodyMessage>(co_await raw_read(socket, cap));
        if (body.bytes.empty())
            break;
        body_bytes += body.bytes.size();
        state->store.append_body(state->session, body.bytes);
        out_body.insert(out_body.end(), body.bytes.begin(), body.bytes.end());
    }
    for (const NeedMessage& need :
         encode_p29v1_need_messages(
             state->store.p29v1_need_frames(state->session), cap, 1 << 20))
        co_await raw_write(socket, Message{need});
    if (state->head_drop_after_need.load() && !park) {
        // T4: abrupt head drop with the successor outstanding. Everything
        // after NEED (FILL/COMMIT) is withheld.
        boost::system::error_code ignored;
        socket.close(ignored);
        co_return;
    }
    // FILL: read the frames C sent (the successor sent them before NEED; the
    // head answers NEED with them) until the inner stream is complete.
    P29V1FillStreamDecoder fill_decoder(1 << 20);
    bool fill_seen = false;
    while (!fill_decoder.complete()) {
        const Frame frame = co_await raw_read(socket, cap);
        if (frame.type != MessageType::FILL)
            throw std::invalid_argument(
                "parked fake F expected FILL frames after NEED");
        fill_decoder.push(raw_decode<FillMessage>(frame));
        fill_seen = true;
    }
    if (fill_seen)
        state->store.append_fill_v1(state->session,
                                    fill_decoder.take_inner_frames());
    (void)state->store.materialize_and_verify(state->session);
    if (state->head_commit_but_drop_commit_frame.load() && !park) {
        // T8: F durably commits but never writes the COMMIT frame.
        (void)state->store.commit_input(state->session);
        state->predecessor_committed.store(true);
        boost::system::error_code ignored;
        socket.close(ignored);
        co_return;
    }
    const TxCommit commit = state->store.commit_input(state->session);
    if (park)
        state->successor_commit_written.store(true);
    else {
        state->predecessor_committed.store(true);
        if (wait_for_successor_commit) {
            asio::steady_timer order_timer(executor);
            while (!state->successor_commit_written.load()) {
                order_timer.expires_after(std::chrono::milliseconds(1));
                boost::system::error_code ignored;
                co_await order_timer.async_wait(
                    asio::redirect_error(asio::use_awaitable, ignored));
            }
        }
    }
    if (state->head_never_commits.load() && !park) {
        // T5: the head never writes its COMMIT; C's own transfer deadline
        // must be the thing that fails it. Sit in the peer-close wait until
        // C gives up and closes.
        co_await raw_wait_for_close(socket);
        co_return;
    }
    co_await raw_write(socket, Message{commit});
    co_await raw_wait_for_close(socket);
}


// Shared driver for the K=2 window scenarios: one endpoint, one parked
// fake F (real FStore), two TUs. The successor TX's run is spawned from the
// head's write_complete callback (the point the 4c route lock releases in
// production, and strictly after the head's predict-commit at Site B), so
// the successor's prepare_for_route sees the head predict-committed exactly
// as the production lock ordering guarantees.
struct K2PipelineScenario {
    enum class HeadOutcome { Commit, DropAfterNeed, NeverCommit };
    enum class SuccessorOutcome { Commit, CorruptStateDigest, DeadlineWait };

    HeadOutcome head_outcome = HeadOutcome::Commit;
    SuccessorOutcome successor_outcome = SuccessorOutcome::Commit;
    bool force_successor_commit_first = false;
    uint8_t seed = 240;
    std::chrono::steady_clock::time_point successor_deadline{};
    bool has_successor_deadline = false;
};

struct K2PipelineRunResult {
    ClientRunResult head;
    ClientRunResult successor;
    TxBegin head_begin;
    TxBegin successor_begin;
    // The bridge (client/p50_zstd_sender.cpp) does the poison teardown in
    // production; the tests own the same calls, so the client and the
    // successor handle outlive context.run().
    std::shared_ptr<TestClient> client;
    PreparedTuHandle successor_handle;
    std::function<void()> spawn_successor;
    std::shared_ptr<RawParkedRouteState> fake;
    std::shared_ptr<std::atomic<ClientStallStage>> successor_stall;
};

void k2_drive(const K2PipelineScenario& scenario, K2PipelineRunResult& out) {
    const P5coStoreGuids guids = p5co_store_guids(scenario.seed);
    EndpointCaps caps;
    caps.profile = ProfileId::P29V1;
    caps.zstd.max_raw_bytes = 4U << 20;
    caps.zstd.max_encoded_body_bytes = 4U << 20;
    PreparationAuthorityLimits limits;
    limits.route_pipeline_depth = 2;
    limits.max_interner_reserved_bytes = UINT64_C(1044398080);
    auto client = std::make_shared<TestClient>(guids.c, caps, HistoryNonce{1},
                                               nullptr, nullptr, limits);
    auto state = std::make_shared<RawParkedRouteState>(guids.c, guids.f);
    out.client = client;
    out.fake = state;
    if (scenario.head_outcome ==
        K2PipelineScenario::HeadOutcome::DropAfterNeed)
        state->head_drop_after_need.store(true);
    if (scenario.head_outcome ==
        K2PipelineScenario::HeadOutcome::NeverCommit)
        state->head_never_commits.store(true);
    if (scenario.successor_outcome ==
        K2PipelineScenario::SuccessorOutcome::CorruptStateDigest)
        state->corrupt_successor_state.store(true);

    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    const tcp::endpoint remote = acceptor.local_endpoint();

    const std::vector<uint8_t> tu1 = bytes("# 1 \"/tmp/k2-fixture.cc\"\nalpha-tu\n");
    const std::vector<uint8_t> tu2 = bytes("# 1 \"/tmp/k2-fixture.cc\"\nbeta-tu\n");
    const PreparedTuHandle head_handle = admit(*client, tu1);

    TxBegin succ_begin_capture;
    std::vector<uint8_t> succ_body_capture;
    std::promise<void> successor_spawned;
    std::optional<std::future<ClientRunResult>> successor_result;

    EndpointIoControl head_control;
    std::atomic<bool> head_write_complete_fired{false};
    out.successor_stall = std::make_shared<std::atomic<ClientStallStage>>(
        ClientStallStage::Connecting);
    head_control.write_complete = [&]() {
        // Production (client/p50_zstd_sender.cpp + p50_cache_service.cpp)
        // only RELEASES the route transfer lock here; TX n+1's transfer
        // then prepares and runs in its OWN coroutine on the owner. The
        // test models exactly that: never admit (prepare) inline from
        // inside the head's dialogue -- post the successor transfer.
        if (head_write_complete_fired.exchange(true))
            return;
        asio::post(context, [&]() {
            // The head predict-committed (Site B) immediately before this
            // post landed: the successor's prepare_for_route sees the
            // head's predicted_post_digest, exactly the production lock
            // ordering.
            out.successor_handle = admit(*client, tu2);
            const PreparedTuHandle succ_handle = out.successor_handle;
            EndpointIoControl successor_control;
            successor_control.stall_stage = out.successor_stall.get();
            successor_result.emplace(asio::co_spawn(
                context,
                client->endpoint.run(
                    remote, succ_handle, successor_control,
                    scenario.has_successor_deadline
                        ? std::optional<std::chrono::steady_clock::time_point>(
                              scenario.successor_deadline)
                        : std::nullopt),
                asio::use_future));
            successor_spawned.set_value();
        });
    };

    TxBegin head_begin_capture;
    std::vector<uint8_t> head_body_capture;
    std::future<void> head_fake = asio::co_spawn(
        context,
        raw_parked_f_serve_connection(acceptor, state, false,
                                      scenario.force_successor_commit_first,
                                      head_begin_capture, head_body_capture),
        asio::use_future);


    TxBegin succ_begin_wire;
    std::vector<uint8_t> succ_body_wire;
    std::future<void> successor_fake = asio::co_spawn(
        context,
        raw_parked_f_serve_connection(acceptor, state, true, false,
                                      succ_begin_wire, succ_body_wire),
        asio::use_future);
    (void)succ_begin_capture;
    (void)succ_body_capture;

    std::future<ClientRunResult> head_result = asio::co_spawn(
        context,
        client->endpoint.run(
            remote, head_handle, head_control,
            scenario.head_outcome == K2PipelineScenario::HeadOutcome::NeverCommit
                ? std::optional<std::chrono::steady_clock::time_point>(
                      std::chrono::steady_clock::now() + std::chrono::seconds(4))
                : std::nullopt),
        asio::use_future);

    context.run();
    out.head = head_result.get();
    if (successor_result.has_value())
        out.successor = successor_result->get();
    out.head_begin = head_begin_capture;
    out.successor_begin = succ_begin_wire;
    (void)successor_spawned;
}

// T1: K=2 straight line. The successor opens while the head is outstanding;
// both commit; retirement and confirmed state advance in rel_seq order.
void test_p29v1_k2_pipeline_straight_line() {
    K2PipelineScenario scenario;
    scenario.seed = 240;
    K2PipelineRunResult run;
    k2_drive(scenario, run);
    require(run.head.status == ClientRunStatus::Committed,
            "K2 straight line: head TX did not commit");
    require(run.successor.status == ClientRunStatus::Committed,
            std::string("K2 straight line: successor TX did not commit "
                        "(status=") +
                std::to_string(static_cast<uint8_t>(run.successor.status)) +
                " observation=" +
                std::to_string(
                    static_cast<uint8_t>(run.successor.observation)) +
                " reconnect=" +
                std::to_string(
                    static_cast<uint8_t>(run.successor.reconnect)) +
                " terminal=" +
                (run.successor.terminal_error.has_value()
                     ? run.successor.terminal_error->detail
                     : std::string("<none>")) +
                " stall=" +
                std::to_string(static_cast<uint8_t>(
                    run.successor_stall
                        ? run.successor_stall->load()
                        : ClientStallStage::Connecting)) +
                ")");
    require(run.successor_begin.rel_seq.value ==
                run.head_begin.rel_seq.value + 1,
            "K2 straight line: successor rel_seq was not head+1");
    require(run.head.committed_commit.has_value() &&
                run.successor.committed_commit.has_value(),
            "K2 straight line: commit witnesses missing");
    require(run.successor_begin.pre_state_digest ==
                run.head.committed_commit->post_state_digest,
            "K2 straight line: the successor's pre-state is not the head's "
            "predicted post-state (digest chain broken)");
    require(run.client->endpoint.state_digest() ==
                run.successor.committed_commit->post_state_digest,
            "K2 straight line: confirmed state did not advance to the "
            "successor's commit in rel_seq order");
    require(run.client->endpoint.next_rel_seq().value == 2,
            "K2 straight line: confirmed rel_seq did not advance past both "
            "TXs");
}

// T2: the successor's COMMIT is forced onto the wire before the head's; C
// must still retire strictly in rel_seq order (the successor waits, then
// confirms after the head), and the confirmed state stays correct.
void test_p29v1_k2_pipeline_successor_commit_dispatched_first() {
    K2PipelineScenario scenario;
    scenario.seed = 241;
    scenario.force_successor_commit_first = true;
    K2PipelineRunResult run;
    k2_drive(scenario, run);
    require(run.head.status == ClientRunStatus::Committed &&
                run.successor.status == ClientRunStatus::Committed,
            "K2 commit-order: both TXs did not commit");
    require(run.successor_begin.pre_state_digest ==
                run.head.committed_commit->post_state_digest,
            "K2 commit-order: digest chain broken");
    require(run.client->endpoint.state_digest() ==
                run.successor.committed_commit->post_state_digest,
            "K2 commit-order: confirmed state did not retire in rel_seq "
            "order despite the successor's COMMIT arriving first");
}

// T3: a successor deviation (STATE digest mismatch). The successor fails,
// the head still returns its own correct status, and after the bridge's
// abandon+reset the next TX runs K=1-cold on a fresh history nonce.
void test_p29v1_k2_pipeline_successor_state_mismatch_poisons() {
    K2PipelineScenario scenario;
    scenario.seed = 242;
    scenario.successor_outcome =
        K2PipelineScenario::SuccessorOutcome::CorruptStateDigest;
    K2PipelineRunResult run;
    k2_drive(scenario, run);
    require(run.head.status == ClientRunStatus::Committed,
            "K2 successor-deviation: head status was not its own "
            "correct Committed result");
    require(run.successor.status != ClientRunStatus::Committed,
            "K2 successor-deviation: successor did not fail");
    require(run.client->endpoint.has_pipeline_poison(),
            "K2 successor-deviation: route was not marked poisoned");
    const HistoryNonce poisoned_nonce =
        run.client->endpoint.f_store_guid().has_value()
            ? HistoryNonce{0}
            : HistoryNonce{0};
    (void)poisoned_nonce;
    // The sender bridge (client/p50_zstd_sender.cpp): retire this handle's
    // own uncommitted route state, then the cold endpoint reset. Performed
    // by the test exactly as transfer_bytes does, in the same order.
    run.client->authority->abandon_route(run.successor_handle);
    run.client->endpoint.reset_after_pipeline_poison();
    require(!run.client->endpoint.has_pipeline_poison(),
            "K2 successor-deviation: reset_after_pipeline_poison did not "
            "clear the poison");
    require(run.client->endpoint.pipeline_epoch_for_test() == 1,
            "K2 successor-deviation: reset did not bump the epoch");
    require(run.client->endpoint.next_rel_seq().value == 0,
            "K2 successor-deviation: reset did not rewind the rel_seq "
            "cursor");
    // The next TX runs K=1-cold against the SAME F (production keeps the
    // f_guid across a poison reset; only the route history is fresh): a
    // fresh full dance (STATE first, then HISTORY_RESET on a new nonce,
    // then the TU), exactly the K=1 cold path.
    const std::vector<uint8_t> third =
        bytes("# 1 \"/tmp/k2-fixture.cc\"\ncold-tx\n");
    const PreparedTuHandle third_handle = admit(*run.client, third);
    asio::io_context third_context;
    tcp::acceptor third_acceptor(third_context,
                                 {asio::ip::address_v4::loopback(), 0});
    TxBegin third_begin_wire;
    std::vector<uint8_t> third_body_wire;
    std::future<void> third_fake = asio::co_spawn(
        third_context,
        raw_parked_f_serve_connection(third_acceptor, run.fake, false,
                                      false, third_begin_wire,
                                      third_body_wire),
        asio::use_future);
    std::atomic<ClientStallStage> third_stage{ClientStallStage::Connecting};
    EndpointIoControl third_control;
    third_control.stall_stage = &third_stage;
    std::future<ClientRunResult> third_result = asio::co_spawn(
        third_context,
        run.client->endpoint.run(third_acceptor.local_endpoint(),
                                 third_handle, third_control),
        asio::use_future);
    third_context.run();
    const ClientRunResult cold = third_result.get();
    require(cold.status == ClientRunStatus::Committed,
            std::string("K2 successor-deviation: the post-reset TX did not "
                        "run cold and commit (status=") +
                std::to_string(static_cast<uint8_t>(cold.status)) +
                " reconnect=" +
                std::to_string(static_cast<uint8_t>(cold.reconnect)) +
                " terminal=" +
                (cold.terminal_error.has_value()
                     ? cold.terminal_error->detail
                     : std::string("<none>")) +
                ")");
    require(third_begin_wire.rel_seq.value == 0 &&
                third_begin_wire.history_nonce.value !=
                    run.head_begin.history_nonce.value,
            "K2 successor-deviation: the cold TX did not run on a fresh "
            "nonce at rel 0");
    (void)third_fake;
}

// T4: head disconnect with a successor outstanding. Everything is torn down
// (the successor fails), and the next TX runs cold after the bridge reset.
void test_p29v1_k2_pipeline_head_disconnect_tears_down_successor() {
    K2PipelineScenario scenario;
    scenario.seed = 243;
    scenario.head_outcome = K2PipelineScenario::HeadOutcome::DropAfterNeed;
    K2PipelineRunResult run;
    k2_drive(scenario, run);
    require(run.head.status == ClientRunStatus::Disconnected,
            std::string("K2 head-drop: head did not report its disconnect "
                        "(status=") +
                std::to_string(static_cast<uint8_t>(run.head.status)) +
                " terminal=" +
                (run.head.terminal_error.has_value()
                     ? run.head.terminal_error->detail
                     : std::string("<none>")) +
                ")");
    require(run.successor.status != ClientRunStatus::Committed,
            "K2 head-drop: successor was not torn down");
    require(run.client->endpoint.has_pipeline_poison(),
            "K2 head-drop: route was not poisoned");
    run.client->authority->abandon_route(run.successor_handle);
    run.client->endpoint.reset_after_pipeline_poison();
    require(run.client->endpoint.next_rel_seq().value == 0 &&
                run.client->endpoint.pipeline_epoch_for_test() == 1,
            "K2 head-drop: cold reset did not land");
}

// T5: the head never retires; the successor's wait times out by its own
// deadline, poisons, and the bridge resets the route cold.
void test_p29v1_k2_pipeline_successor_wait_deadline() {
    K2PipelineScenario scenario;
    scenario.seed = 244;
    scenario.head_outcome = K2PipelineScenario::HeadOutcome::NeverCommit;
    scenario.has_successor_deadline = true;
    scenario.successor_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
    K2PipelineRunResult run;
    k2_drive(scenario, run);
    require(run.successor.status == ClientRunStatus::DeadlineExceeded,
            "K2 wait-deadline: successor wait did not fail by its own "
            "deadline");
    require(run.client->endpoint.has_pipeline_poison(),
            "K2 wait-deadline: route was not poisoned");
    run.client->authority->abandon_route(run.successor_handle);
    run.client->endpoint.reset_after_pipeline_poison();
    require(!run.client->endpoint.has_pipeline_poison() &&
                run.client->endpoint.pipeline_epoch_for_test() == 1,
            "K2 wait-deadline: cold reset did not land");
}

// T8 (review point 2): K=2, head only, connection drops after FILL. F
// durably committed but never wrote the COMMIT frame; a later retry's
// STATE proves it via LOST_COMMIT_ACCEPTED. The route must stay usable
// (recoverable, not a sticky silent P29V1 loss), and a fresh third TX
// must still commit.
void test_p29v1_k2_pipeline_head_drop_after_fill_stays_recoverable() {
    const P5coStoreGuids guids = p5co_store_guids(246);
    EndpointCaps caps;
    caps.profile = ProfileId::P29V1;
    caps.zstd.max_raw_bytes = 4U << 20;
    caps.zstd.max_encoded_body_bytes = 4U << 20;
    PreparationAuthorityLimits limits;
    limits.route_pipeline_depth = 2;
    limits.max_interner_reserved_bytes = UINT64_C(1044398080);
    TestClient client(guids.c, caps, HistoryNonce{1}, nullptr, nullptr, limits);
    auto state = std::make_shared<RawParkedRouteState>(guids.c, guids.f);
    state->head_commit_but_drop_commit_frame.store(true);
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    const tcp::endpoint remote = acceptor.local_endpoint();
    const std::vector<uint8_t> tu1 = bytes("# 1 \"/tmp/k2-t8.cc\"\nhead-drop\n");
    const PreparedTuHandle head_handle = admit(client, tu1);

    TxBegin head_begin_capture, retry_begin_wire;
    std::vector<uint8_t> head_body_wire, retry_body_wire;
    std::future<void> head_fake = asio::co_spawn(
        context,
        raw_parked_f_serve_connection(acceptor, state, false, false,
                                      head_begin_capture, head_body_wire),
        asio::use_future);
    std::future<ClientRunResult> head_result = asio::co_spawn(
        context, client.endpoint.run(remote, head_handle, EndpointIoControl{}),
        asio::use_future);
    context.run();
    const ClientRunResult head = head_result.get();
    require(head.status == ClientRunStatus::Disconnected,
            "T8: head drop did not surface as a disconnect");
    require(client.has_active_transaction(),
            "T8: head drop lost the retained retry state");
    require(!client.endpoint.has_pipeline_poison(),
            "T8: head-only drop poisoned the route (no successor existed)");
    // One drop only: the retry connection must write its COMMIT normally.
    state->head_commit_but_drop_commit_frame.store(false);

    // Retry on the SAME retained handle: F's STATE now proves the commit.
    asio::io_context retry_context;
    tcp::acceptor retry_acceptor(retry_context,
                                  {asio::ip::address_v4::loopback(), 0});
    TxBegin retry_begin_capture;
    std::vector<uint8_t> retry_body_capture;
    std::future<void> retry_fake = asio::co_spawn(
        retry_context,
        raw_parked_f_serve_connection(retry_acceptor, state, false, false,
                                      retry_begin_capture, retry_body_capture),
        asio::use_future);
    std::future<ClientRunResult> retry_result = asio::co_spawn(
        retry_context,
        client.endpoint.run(retry_acceptor.local_endpoint(), {}, EndpointIoControl{}),
        asio::use_future);
    retry_context.run();
    const ClientRunResult retry = retry_result.get();
    require(retry.status == ClientRunStatus::Committed &&
                retry.reconnect ==
                    EndpointReconnectOutcome::LostFinalAcknowledgement,
            "T8: the lost-commit recovery did not commit through "
            "LOST_COMMIT_ACCEPTED");
    require(retry_begin_capture.rel_seq.value == head_begin_capture.rel_seq.value,
            "T8: retry replayed a different rel_seq");
    (void)retry_begin_wire;
    (void)retry_body_wire;
    (void)head_fake;
    (void)retry_fake;

    // The route is usable for a fresh next TX: no sticky terminal.
    asio::io_context third_context;
    tcp::acceptor third_acceptor(third_context,
                                 {asio::ip::address_v4::loopback(), 0});
    TxBegin third_begin_capture;
    std::vector<uint8_t> third_body_capture;
    std::future<void> third_fake = asio::co_spawn(
        third_context,
        raw_parked_f_serve_connection(third_acceptor, state, false, false,
                                      third_begin_capture, third_body_capture),
        asio::use_future);
    const std::vector<uint8_t> tu3 = bytes("# 1 \"/tmp/k2-t8.cc\"\nthird-tx\n");
    const PreparedTuHandle third_handle = admit(client, tu3);
    std::future<ClientRunResult> third_result = asio::co_spawn(
        third_context,
        client.endpoint.run(third_acceptor.local_endpoint(), third_handle,
                            EndpointIoControl{}),
        asio::use_future);
    third_context.run();
    require(third_result.get().status == ClientRunStatus::Committed,
            "T8: the route was NOT usable for the next TX after the "
            "lost-commit recovery (sticky route poison)");
}

// T9 (follow-up review): a successor's own io failure is a route
// deviation, not a local one. Its predict-commit already advanced C's
// serializer and predicted cursor past a TX F will never apply, so any
// later begin would build on state F does not have. The route must be
// poisoned (all remaining successors torn down), the head must still
// commit, and the next TX must recover through the cold HISTORY_RESET
// path with a fresh nonce -- never a sticky route.
void test_p29v1_k2_pipeline_successor_io_death_poisons_route() {
    const P5coStoreGuids guids = p5co_store_guids(247);
    EndpointCaps caps;
    caps.profile = ProfileId::P29V1;
    caps.zstd.max_raw_bytes = 4U << 20;
    caps.zstd.max_encoded_body_bytes = 4U << 20;
    PreparationAuthorityLimits limits;
    limits.route_pipeline_depth = 2;
    limits.max_interner_reserved_bytes = UINT64_C(1044398080);
    TestClient client(guids.c, caps, HistoryNonce{1}, nullptr, nullptr, limits);
    auto state = std::make_shared<RawParkedRouteState>(guids.c, guids.f);
    state->successor_drop_after_hello.store(true);
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    const tcp::endpoint remote = acceptor.local_endpoint();
    const std::vector<uint8_t> tu1 = bytes("# 1 \"/tmp/k2-t9.cc\"\nhead-ok\n");
    const std::vector<uint8_t> tu2 = bytes("# 1 \"/tmp/k2-t9.cc\"\nsucc-dies\n");
    const PreparedTuHandle head_handle = admit(client, tu1);

    TxBegin head_begin_capture, succ_begin_wire;
    std::vector<uint8_t> head_body_wire, succ_body_wire;
    std::future<void> head_fake = asio::co_spawn(
        context,
        raw_parked_f_serve_connection(acceptor, state, false, false,
                                      head_begin_capture, head_body_wire),
        asio::use_future);
    std::future<void> succ_fake = asio::co_spawn(
        context,
        raw_parked_f_serve_connection(acceptor, state, true, false,
                                      succ_begin_wire, succ_body_wire),
        asio::use_future);

    std::atomic<bool> head_write_complete_fired{false};
    std::optional<std::future<ClientRunResult>> successor_result;
    PreparedTuHandle successor_handle;
    EndpointIoControl head_control;
    head_control.write_complete = [&]() {
        // Post, never prepare inline: same rule as the k2 driver.
        if (head_write_complete_fired.exchange(true))
            return;
        asio::post(context, [&]() {
            successor_handle = admit(client, tu2);
            successor_result.emplace(asio::co_spawn(
                context,
                client.endpoint.run(remote, successor_handle,
                                    EndpointIoControl{}, std::nullopt),
                asio::use_future));
        });
    };
    std::future<ClientRunResult> head_result = asio::co_spawn(
        context, client.endpoint.run(remote, head_handle, head_control),
        asio::use_future);
    context.run();
    const ClientRunResult head = head_result.get();
    require(head.status == ClientRunStatus::Committed,
            "T9: the healthy head did not commit while its successor "
            "died");
    require(successor_result.has_value(), "T9: successor run never started");
    const ClientRunResult successor = successor_result->get();
    require(successor.status != ClientRunStatus::Committed,
            "T9: the io-dead successor was not failed");
    require(client.endpoint.has_pipeline_poison(),
            "T9: a successor io death did not poison the route");
    // Bridge: retire the successor's own uncommitted route state, then the
    // cold endpoint reset (fresh nonce, rel 0). abandon_route clears the
    // CRoute queue so the next prepare starts a fresh predicted line.
    client.authority->abandon_route(successor_handle);
    client.endpoint.reset_after_pipeline_poison();
    require(!client.endpoint.has_pipeline_poison() &&
                client.endpoint.next_rel_seq().value == 0,
            "T9: cold reset did not land after the successor io death");
    (void)head_fake;
    (void)succ_fake;

    // The next TX recovers through the cold HISTORY_RESET path: this fake
    // route is already established, so the endpoint's fresh nonce forces
    // the reset dance, and the TX commits with rel_seq 0 under the new
    // nonce.
    state->successor_drop_after_hello.store(false);
    asio::io_context third_context;
    tcp::acceptor third_acceptor(third_context,
                                 {asio::ip::address_v4::loopback(), 0});
    TxBegin third_begin_wire;
    std::vector<uint8_t> third_body_wire;
    std::future<void> third_fake = asio::co_spawn(
        third_context,
        raw_parked_f_serve_connection(third_acceptor, state, false, false,
                                      third_begin_wire, third_body_wire),
        asio::use_future);
    const std::vector<uint8_t> tu3 = bytes("# 1 \"/tmp/k2-t9.cc\"\nrecovered\n");
    const PreparedTuHandle third_handle = admit(client, tu3);
    std::future<ClientRunResult> third_result = asio::co_spawn(
        third_context,
        client.endpoint.run(third_acceptor.local_endpoint(), third_handle,
                            EndpointIoControl{}),
        asio::use_future);
    third_context.run();
    const ClientRunResult third = third_result.get();
    require(third.status == ClientRunStatus::Committed,
            "T9: the route did not recover for the next TX after the "
            "successor io death");
    require(third_begin_wire.rel_seq.value == 0 &&
                third_begin_wire.history_nonce.value !=
                    head_begin_capture.history_nonce.value,
            "T9: the recovered TX did not run cold (fresh nonce, rel 0)");
    require(third.committed_commit.has_value() &&
                third.committed_commit->rel_seq.value == 0,
            "T9: the recovered commit did not land on the cold cursor");
    (void)third_fake;
}

// T6b: at depth 1 the waiter machinery is never used -- the K=1 identity
// gate (T6a, test_p29v1_k1_pipeline_wire_identity) stays byte-identical,
// and both waiter structures stay empty through a full K=1 dialogue.
void test_p29v1_k1_pipeline_waiters_never_used() {
    const P5coStoreGuids guids = p5co_store_guids(239);
    EndpointCaps caps;
    caps.profile = ProfileId::P29V1;
    caps.zstd.max_raw_bytes = 1U << 20;
    caps.zstd.max_encoded_body_bytes = 1U << 20;
    TestClient client(guids.c, caps);
    P50ServerEndpoint server(guids.f, caps);
    const PairResult first =
        run_pair(client, server, admit(client, bytes("# 1 \"/tmp/k1-waiter.cc\"\nfirst\n")));
    require(first.client.status == ClientRunStatus::Committed &&
                first.server.status == ServerRunStatus::Completed,
            "K1 waiter probe: base dialogue did not commit");
    require(client.endpoint.pipeline_window_size_for_test() == 0 &&
                client.endpoint.pipeline_retire_waiter_count_for_test() == 0 &&
                client.endpoint.pipeline_window_slot_waiter_count_for_test() == 0 &&
                client.endpoint.pipeline_epoch_for_test() == 0,
            "K1 waiter probe: window/waiter bookkeeping was touched at "
            "depth 1");
    // wait_for_window_slot at depth 1 returns true without suspending.
    asio::io_context context;
    std::future<bool> slot = asio::co_spawn(
        context, client.endpoint.wait_for_window_slot(std::nullopt),
        asio::use_future);
    context.run();
    require(slot.get(), "K1 waiter probe: depth-1 window wait did not "
                        "resolve immediately");
}

// T7: a would-be successor that cannot confirm its NEED through an eager
// FILL (the early-FILL admission bound is exceeded) does not open a
// successor dialogue: it waits level-triggered for the window to empty and
// then runs the unchanged head path -- no poison, wire identical to K=1
// for that TX.
void test_p29v1_k2_pipeline_no_eager_fill_falls_back_to_head() {
    const P5coStoreGuids guids = p5co_store_guids(245);
    EndpointCaps caps;
    caps.profile = ProfileId::P29V1;
    caps.zstd.max_raw_bytes = 4U << 20;
    caps.zstd.max_encoded_body_bytes = 4U << 20;
    PreparationAuthorityLimits limits;
    limits.route_pipeline_depth = 2;
    limits.max_interner_reserved_bytes = UINT64_C(1044398080);
    TestClient client(guids.c, caps, HistoryNonce{1}, nullptr, nullptr, limits);
    auto state = std::make_shared<RawParkedRouteState>(guids.c, guids.f);
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    const tcp::endpoint remote = acceptor.local_endpoint();

    // Force the early-FILL admission bound to zero for this test only:
    // the fallback under test is "NEED could outgrow the bound -> stay on
    // the head path", and the bound is a plain admission number, so driving
    // it to 0 exercises the path deterministically instead of depending on
    // interner region shapes (a real production TU is bounded by the same
    // number on both the dispatch probe and the eager FILL itself).
    require(::setenv("ICECC_P50_EARLY_NEED_REGION_BOUND", "0", 1) == 0,
            "T7: could not lower the early-FILL bound");
    const struct BoundRestore {
        ~BoundRestore() { ::unsetenv("ICECC_P50_EARLY_NEED_REGION_BOUND"); }
    } bound_restore;
    const std::vector<uint8_t> huge = bytes(
        "# 1 \"/tmp/k2-t7-big.cc\"\nunique-fallback-source\n");
    const std::vector<uint8_t> small = bytes("# 1 \"/tmp/k2-t7.cc\"\nsmall\n");
    const PreparedTuHandle head_handle = admit(client, small);

    TxBegin head_begin_capture, succ_begin_wire;
    std::vector<uint8_t> head_body_wire, succ_body_wire;
    std::future<void> head_fake = asio::co_spawn(
        context,
        raw_parked_f_serve_connection(acceptor, state, false, false,
                                      head_begin_capture, head_body_wire),
        asio::use_future);
    std::future<void> succ_fake = asio::co_spawn(
        context,
        raw_parked_f_serve_connection(acceptor, state, true, false,
                                      succ_begin_wire, succ_body_wire),
        asio::use_future);

    std::atomic<bool> head_write_complete_fired{false};
    std::optional<std::future<ClientRunResult>> successor_result;
    std::atomic<bool> eager_fill_possible{true};
    EndpointIoControl head_control;
    head_control.write_complete = [&]() {
        // Post, never prepare inline: same rule as the k2 driver.
        if (head_write_complete_fired.exchange(true))
            return;
        asio::post(context, [&]() {
            const PreparedTuHandle succ_handle = admit(client, huge);
            eager_fill_possible.store(
                client.authority->p29v1_fill_before_need_possible(succ_handle));
            successor_result.emplace(asio::co_spawn(
                context,
                client.endpoint.run(remote, succ_handle, EndpointIoControl{},
                                    std::nullopt),
                asio::use_future));
        });
    };
    std::future<ClientRunResult> head_result = asio::co_spawn(
        context, client.endpoint.run(remote, head_handle, head_control),
        asio::use_future);
    context.run();
    const ClientRunResult head = head_result.get();
    require(head.status == ClientRunStatus::Committed,
            "T7: head TX did not commit");
    require(!eager_fill_possible.load(),
            "T7: fixture did not exceed the early-FILL admission bound "
            "(choose a bigger source)");
    require(successor_result.has_value(), "T7: successor run never started");
    const ClientRunResult successor = successor_result->get();
    require(successor.status == ClientRunStatus::Committed,
            "T7: fallback-to-head TX did not commit");
    require(!client.endpoint.has_pipeline_poison(),
            "T7: fallback poisoned the route (it must not)");
    require(client.endpoint.pipeline_epoch_for_test() == 0,
            "T7: fallback bumped the pipeline epoch (it must not)");
    (void)succ_fake;
    (void)head_fake;
}


// raw_commit_one_tu is defined further down (step-5d recovery fixture).
asio::awaitable<void> raw_commit_one_tu(tcp::endpoint remote, CStoreGuid c_guid,
                                        const ZstdTuEnvelope& tu, HistoryNonce nonce,
                                        RelSeq rel);

// ===== Admission-race fixes (2026-10-09): Fix F pre-park + Fix C dispatch =====
//
// Fix F (F side): the step-5b MSG_PEEK is instantaneous. A successor that
// writes SESSION_HELLO and then TX_BEGIN as two separate writes (BEGIN
// delayed) defeats it: when F has consumed HELLO the successor's BEGIN has
// not landed yet, so the old fall-through ran stage() -- a SESSION_STATE
// built from the still-outstanding head's cursor, which the C successor's
// exact-witness check rejects -- and then activate() REPLACED the live head
// session, dropping the head's pending TX. The level-triggered pre-park
// (cache/p50_endpoint.cpp step 5c) parks the session instead. Both tests
// below drive the REAL P50ServerEndpoint with a raw successor socket whose
// HELLO/BEGIN split is test-controlled, and the head's materialization
// gated on F's codec worker (the one blocking gate that is not the
// io_context thread) so route->pending is deterministically set.

// T-F (Fix F): successor HELLO, then a delayed BEGIN (100 ms) -- the exact
// missed-peek shape. F must pre-park: no SESSION_STATE byte may reach the
// successor socket while the head is still gated (a stage() would have
// written one immediately), and after the head commits the successor's STATE
// must carry its OWN predicted witness (cursor 1 + the offline-derived
// pre-state), never the head's cursor.
struct PreparkRace {
    // Set by the head's F-side materialize gate
    // (before_materialize_on_worker): the deterministic proof that the
    // head's TX is INSTALLED and pending on the route. The successor must
    // not write HELLO before this -- F's successor session would then
    // process HELLO with no pending TX on the route and (correctly, per
    // the design) fall through to stage() instead of pre-parking.
    std::atomic<bool>* head_pending = nullptr;
    // Set by the raw head right after its HISTORY_RESET was acknowledged:
    // the F route exists. A peek-time-park successor (whose HELLO shares
    // its single write with BEGIN) waits for this before writing anything:
    // its park decision needs an established route, and a HELLO processed
    // before the head's reset would (correctly) see no route and fall
    // through.
    std::atomic<bool>* head_route_ready = nullptr;
    std::atomic<bool> begin_written{false};
    // Recorded outcomes (never thrown from inside the coroutine): a red
    // run (a reverted fix) must land in these fields and let the successor
    // coroutine unwind through its whole dialogue, or the leg hangs inside
    // io_context::run_for instead of failing. The driver asserts them all
    // after run_for; `detail` is written at most once, before the
    // coroutine's future turns ready.
    std::atomic<bool> probe_saw_bytes{false};
    std::atomic<bool> send_failed{false};
    std::atomic<bool> state_witness_matched{false};
    std::atomic<bool> commit_witness_matched{false};
    std::string detail;
    // Set by the raw head after its TX_COMMIT read: the head has committed
    // and -- synchronously inside that commit -- release_parked_at() ran.
    // A straddling successor waits for it before writing its BODY-frame
    // tail, so the park/pre-park drain provably exits holding a partial
    // frame (the farm shape: one drain routinely stops mid-frame).
    std::atomic<bool> head_committed{false};
    // Set by the successor coroutine after its no-stage probe loop: kept
    // for the original test's shape (nothing waits on it any more -- the
    // releaser fires on a fixed delay so a dead successor cannot wedge the
    // leg); probe_done records that the whole window ran clean.
    std::atomic<bool> probe_done{false};
};

// How the raw successor lands its bytes on the wire. The production C side
// writes SESSION_HELLO, then TX_BEGIN, then BODY as separate async writes;
// a high-BDP route coalesces them into the same socket receive-buffer drain.
struct PreparkSuccessorShape {
    // HELLO in the same contiguous write as the BEGIN+BODY prefix: F reads
    // HELLO and its step-5b peek then sees the BEGIN -- the peek-time park
    // path (cache/p50_endpoint.cpp step 5c), not the pre-park.
    bool batch_hello_with_frames = false;
    // BEGIN and the BODY prefix as ONE contiguous write (vs the production
    // two-write order): they then land in the same drain.
    bool batch_begin_with_body = false;
    // Write only the first half of the first BODY frame in the prefix and
    // hold the rest until HEAD_COMMITTED: the drain then provably stops
    // holding a straddling frame (355a35aa regression shape).
    bool split_first_body_frame = false;
    // SO_SNDBUF on the successor's own socket (paired with SO_RCVBUF on
    // its acceptor, set by the driver): with both pinned wide, the whole
    // batch lands in ONE kernel delivery and F's first drain carries the
    // BEGIN plus the BODY. Auto-tuned buffers start near 128 KiB, which
    // splits the write into small deliveries and lets the pre-fix ceiling
    // check see the BEGIN frame complete (and raise the ceiling) inside a
    // small first drain -- the fixture would never trip it.
    size_t socket_buffer = 0;
};

// The successor's whole wire batch: the BEGIN frame plus its BODY frames
// (chunked at the negotiated frame cap, exactly as F's own
// async_write_component does for payloads above one frame).
std::vector<std::vector<uint8_t>> successor_wire_frames(
    const ZstdTuEnvelope& successor_tu, uint32_t max_frame_payload) {
    std::vector<std::vector<uint8_t>> frames;
    frames.push_back(encode_frame(Message{successor_tu.begin}));
    const std::vector<uint8_t>& body = successor_tu.body;
    for (size_t offset = 0; offset < body.size();) {
        const size_t count =
            std::min<size_t>(max_frame_payload, body.size() - offset);
        Message body_message = BodyMessage{
            std::vector<uint8_t>(body.begin() + offset,
                                 body.begin() + offset + count)};
        frames.push_back(encode_frame(std::move(body_message)));
        offset += count;
    }
    return frames;
}

asio::awaitable<void> raw_prepark_successor(tcp::endpoint remote, CStoreGuid c_guid,
                                            const ZstdTuEnvelope& successor_tu,
                                            HistoryNonce nonce,
                                            PreparkRace& race,
                                            PreparkSuccessorShape shape) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    co_await socket.async_connect(remote, asio::use_awaitable);
    if (shape.socket_buffer != 0) {
        boost::system::error_code sndbuf_error;
        socket.set_option(asio::socket_base::send_buffer_size(
                              shape.socket_buffer),
                          sndbuf_error);
        require(!sndbuf_error,
                "pre-park fixture could not enlarge the successor send "
                "buffer (netns wmem_max caps the option)");
    }
    SessionHello hello;
    hello.c_store_guid = c_guid;
    hello.supported_profiles = profile_bit(ProfileId::ZSTD_TU);
    hello.limits = {};
    const std::vector<std::vector<uint8_t>> frames =
        successor_wire_frames(successor_tu, hello.limits.max_frame_payload);
    // The prefix that lands before the head commits: the BEGIN frame plus
    // the BODY frames -- whole, or just the first half of the first one
    // when the shape straddles (the tail then waits for head_committed).
    std::vector<uint8_t> prefix;
    for (const std::vector<uint8_t>& frame : frames) {
        if (shape.split_first_body_frame && !prefix.empty()) {
            // Split INSIDE this BODY frame -- never at a frame boundary, or
            // the drain would stop clean and there would be nothing to
            // finish.
            const size_t keep = frame.size() / 2;
            prefix.insert(prefix.end(), frame.begin(), frame.begin() + keep);
            break;
        }
        prefix.insert(prefix.end(), frame.begin(), frame.end());
    }
    if (!shape.batch_hello_with_frames) {
        // Do not write HELLO until the head's TX is installed and pending on
        // the route at F (the gate-entered flag is that deterministic
        // signal): the pre-park condition is route->pending owned by another
        // serial.
        asio::steady_timer pending_wait(executor);
        while (!race.head_pending->load(std::memory_order_acquire)) {
            pending_wait.expires_after(std::chrono::milliseconds(1));
            boost::system::error_code ignored;
            co_await pending_wait.async_wait(
                asio::redirect_error(asio::use_awaitable, ignored));
        }
        co_await raw_write(socket, hello);
        // The hook-controlled delay: BEGIN (and BODY) land only after F has
        // certainly consumed HELLO -- beyond the peek's reach.
        asio::steady_timer delay(executor);
        delay.expires_after(std::chrono::milliseconds(100));
        co_await delay.async_wait(asio::use_awaitable);
    } else {
        // Peek-time-park shape: HELLO shares the single write with the
        // frames, after the head's route exists (its reset ack).
        asio::steady_timer route_wait(executor);
        while (!race.head_route_ready->load(std::memory_order_acquire)) {
            route_wait.expires_after(std::chrono::milliseconds(1));
            boost::system::error_code ignored;
            co_await route_wait.async_wait(
                asio::redirect_error(asio::use_awaitable, ignored));
        }
        const std::vector<uint8_t> hello_frame = encode_frame(Message{hello});
        prefix.insert(prefix.begin(), hello_frame.begin(), hello_frame.end());
    }
    if (shape.batch_hello_with_frames || shape.batch_begin_with_body) {
        // ONE contiguous send: HELLO/BEGIN/BODY all land in the same socket
        // receive buffer, so one drain carries them together -- the
        // high-BDP production shape.
        co_await raw_send_all(socket, prefix, &race.send_failed);
    } else {
        // The production order: BEGIN first, then the BODY frames, as two
        // writes.
        co_await raw_write(socket, successor_tu.begin);
        Message body_message = BodyMessage{successor_tu.body};
        co_await raw_write(socket, std::move(body_message));
    }
    race.begin_written.store(true);
    if (race.send_failed.load(std::memory_order_acquire)) {
        // The batch never fully landed (peer died first): record and
        // unwind -- the driver turns it into the leg's failure.
        race.detail = "pre-park successor could not land its wire batch";
        boost::system::error_code ignored;
        socket.close(ignored);
        co_return;
    }
    // Park proof: while the head's materialization is still gated (its TX
    // pending on the route), no byte may arrive on this socket. A
    // fall-through stage() would have written SESSION_STATE immediately
    // (microseconds after the missed peek), so a short level-triggered
    // probe window on available() is a complete refutation. Recorded, not
    // thrown: a red run must still unwind through the whole dialogue or
    // the leg hangs in run_for. The gate stays held through this window
    // (the releaser fires 400 ms after begin_written, well past the last
    // probe tick) so a clean-green probe really proves no stage() ran.
    asio::steady_timer probe(executor);
    for (int iteration = 0; iteration != 20; ++iteration) {
        probe.expires_after(std::chrono::milliseconds(10));
        boost::system::error_code ignored;
        co_await probe.async_wait(
            asio::redirect_error(asio::use_awaitable, ignored));
        boost::system::error_code available_error;
        const std::size_t available = socket.available(available_error);
        if (available_error != boost::system::errc::success ||
            available != 0) {
            race.probe_saw_bytes.store(true, std::memory_order_release);
            if (race.detail.empty()) {
                race.detail = available_error != boost::system::errc::success
                                   ? "probe tick errored"
                                   : ("pre-park successor received " +
                                      std::to_string(available) +
                                      " bytes while its predecessor was "
                                      "still pending (stage() ran early, or "
                                      "a pre-fix terminal ERROR frame "
                                      "arrived)");
            }
        }
    }
    race.probe_done.store(true);
    if (shape.split_first_body_frame) {
        // The head commits (release_parked_at ran inside that commit): the
        // drain has provably stopped holding half a BODY frame. Only now
        // write the tail.
        asio::steady_timer commit_wait(executor);
        while (!race.head_committed.load(std::memory_order_acquire)) {
            commit_wait.expires_after(std::chrono::milliseconds(1));
            boost::system::error_code ignored;
            co_await commit_wait.async_wait(
                asio::redirect_error(asio::use_awaitable, ignored));
        }
        std::vector<uint8_t> tail;
        // frames[0] is the BEGIN frame (already in the prefix); frames[1]
        // is the straddling BODY frame: its second half opens the tail.
        for (size_t index = 1; index != frames.size(); ++index) {
            const std::vector<uint8_t>& frame = frames[index];
            if (index == 1) {
                const size_t keep = frame.size() / 2;
                tail.insert(tail.end(), frame.begin() + keep, frame.end());
                continue;
            }
            tail.insert(tail.end(), frame.begin(), frame.end());
        }
        co_await raw_send_all(socket, tail);
    }
    if (race.probe_saw_bytes.load(std::memory_order_acquire) &&
        !shape.split_first_body_frame) {
        // Red path: F answered a terminal ERROR frame (or staged early) and
        // this socket's dialogue is dead. Drain whatever arrived (so the
        // close is orderly) and unwind; the driver asserts the recorded
        // outcome. A straddling shape never reaches here on red the same
        // way (its prefix half-frame stays parked), so it presses on to the
        // STATE read, which itself records a mismatch if F died.
        asio::steady_timer drain(executor);
        for (int iteration = 0; iteration != 40; ++iteration) {
            drain.expires_after(std::chrono::milliseconds(10));
            boost::system::error_code ignored;
            co_await drain.async_wait(
                asio::redirect_error(asio::use_awaitable, ignored));
            boost::system::error_code available_error;
            (void)socket.available(available_error);
        }
        boost::system::error_code ignored;
        socket.close(ignored);
        co_return;
    }
    // The gate released (the test's watcher frees it once begin_written);
    // the STATE read completes only through the head's commit release, and
    // it must carry this successor's OWN predicted cursor. Both reads are
    // wrapped: a red path can kill this half of the dialogue with an EOF
    // or a terminal ERROR frame instead of SESSION_STATE, and an exception
    // escaping a coroutine that still owes a close is exactly the
    // die-mid-dialogue shape that wedges the leg. Everything here records
    // and unwinds.
    try {
        const SessionState state = raw_decode<SessionState>(
            co_await raw_read(socket, hello.limits.max_frame_payload));
        const bool state_witness =
            state.route_present && state.history_nonce == nonce &&
            state.next_rel_seq.value == 1 &&
            state.state_digest == successor_tu.begin.pre_state_digest;
        race.state_witness_matched.store(state_witness,
                                         std::memory_order_release);
        if (!state_witness && race.detail.empty()) {
            race.detail =
                "pre-park successor observed a route cursor that never "
                "advanced to its own predicted witness";
        } else {
            const TxCommit commit = raw_decode<TxCommit>(
                co_await raw_read(socket, state.limits.max_frame_payload));
            const bool commit_witness =
                commit.transaction_digest ==
                    successor_tu.begin.transaction_digest &&
                commit.raw_digest == successor_tu.begin.raw_digest;
            race.commit_witness_matched.store(commit_witness,
                                              std::memory_order_release);
            if (!commit_witness && race.detail.empty())
                race.detail = "pre-park successor received a different "
                              "TX_COMMIT";
        }
    } catch (const std::exception& error) {
        if (race.detail.empty())
            race.detail = std::string("pre-park successor dialogue died: ") +
                          error.what();
    }
    boost::system::error_code ignored;
    socket.close(ignored);
    co_return;
}

// The gated raw head: HELLO + fresh-route reset + BEGIN + BODY, then the
// test holds its materialization on F's codec worker until the successor has
// landed its whole batch, then it commits and closes.
asio::awaitable<void> raw_prepark_head(tcp::endpoint remote, CStoreGuid c_guid,
                                       const ZstdTuEnvelope& head_tu,
                                       HistoryNonce nonce, PreparkRace& race) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    const RawRoute open = co_await raw_open(socket, remote, c_guid);
    const SessionState reset_ack =
        co_await raw_reset(socket, open, nonce);
    require(reset_ack.route_present && reset_ack.history_nonce == nonce &&
                reset_ack.state_digest == initial_route_digest(c_guid, nonce) &&
                reset_ack.next_rel_seq.value == 0,
            "pre-park head did not establish its fresh route");
    if (race.head_route_ready != nullptr)
        race.head_route_ready->store(true, std::memory_order_release);
    const TxBegin begin =
        make_begin(head_tu, nonce, reset_ack.state_digest,
                   reset_ack.next_rel_seq);
    co_await raw_write(socket, begin);
    Message body_message = BodyMessage{head_tu.body};
    co_await raw_write(socket, std::move(body_message));
    // Deterministic level-triggered flag wait (no sleep-based
    // synchronisation): the head's commit read happens only after the
    // successor's batch is on the wire and the gate released.
    asio::steady_timer wait(executor);
    while (!race.begin_written.load()) {
        wait.expires_after(std::chrono::milliseconds(1));
        boost::system::error_code ignored;
        co_await wait.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
    }
    const TxCommit commit = raw_decode<TxCommit>(
        co_await raw_read(socket, reset_ack.limits.max_frame_payload));
    require(commit.transaction_digest == begin.transaction_digest &&
                commit.raw_digest == begin.raw_digest,
            "pre-park head received a different TX_COMMIT");
    // The commit read completing proves F wrote TX_COMMIT, which is strictly
    // after release_parked_at() ran inside that commit: every released
    // successor (and its straddling drain) is now live.
    race.head_committed.store(true, std::memory_order_release);
    boost::system::error_code ignored;
    socket.close(ignored);
    co_return;
}

// One pre-park/park pair leg: a gated raw head (rel 0) plus one raw
// successor (rel 1) whose wire shape, payload and socket buffer the plan
// selects. Everything the original missed-peek test asserted is asserted
// here for every shape: park (no early stage), both commit in order, exact
// bytes, and the successor's STATE carrying its OWN predicted witness.
struct PreparkPairPlan {
    const char* name = "";
    std::vector<uint8_t> successor_input;
    PreparkSuccessorShape shape;
    // caps.zstd.max_raw_bytes == max_encoded_body_bytes.
    size_t zstd_cap = 1U << 20;
    // SO_RCVBUF set on the successor acceptor before accept (accepted
    // sockets inherit it): one big receive buffer makes one drain carry the
    // whole BEGIN+BODY batch, the high-BDP production shape.
    size_t successor_receive_buffer = 0;
    // The peek-time park (HELLO batched with BEGIN) records the peeked rel;
    // the pre-park (delayed BEGIN) must not.
    bool expect_peeked_begin = false;
};

void run_prepark_pair(const PreparkPairPlan& plan) {
    const P5coStoreGuids guids = p5co_store_guids(48);
    EndpointCaps caps;
    caps.profile = ProfileId::ZSTD_TU;
    caps.zstd.max_raw_bytes = plan.zstd_cap;
    caps.zstd.max_encoded_body_bytes = plan.zstd_cap;
    ActionTrace actions;
    P50ServerEndpoint server(guids.f, caps, nullptr, &actions);
    const HistoryNonce nonce{481};
    const ZstdTuEnvelope head_tu =
        encode_zstd_tu(nonce, RelSeq{0}, TuSeq{4810},
                        initial_route_digest(guids.c, nonce),
                        bytes("pre-park: head TU\n"));
    // The successor's pre-state is derived entirely offline from the head's
    // own begin fields -- the route-pipelining argument pattern.
    const Digest128 successor_pre_state = compute_post_state_digest(
        head_tu.begin.pre_state_digest, head_tu.begin.history_nonce,
        head_tu.begin.rel_seq, head_tu.begin.tu_seq,
        head_tu.begin.transaction_digest);
    const ZstdTuEnvelope successor_tu =
        encode_zstd_tu(nonce, RelSeq{1}, TuSeq{4811}, successor_pre_state,
                       plan.successor_input);

    std::promise<void> release_head;
    std::future<void> head_gate = release_head.get_future();
    std::atomic<bool> head_materialize_entered{false};
    std::atomic<bool> head_route_ready{false};
    EndpointIoControl head_server_control;
    head_server_control.before_materialize_on_worker = [&]() {
        if (!head_materialize_entered.exchange(true))
            head_gate.wait();
    };
    PreparkRace race;
    race.head_pending = &head_materialize_entered;
    race.head_route_ready = &head_route_ready;
    asio::io_context context;
    // Separate acceptors per leg: no accept-order race between head and
    // successor.
    tcp::acceptor head_acceptor(context, {asio::ip::address_v4::loopback(), 0});
    tcp::acceptor successor_acceptor(
        context, {asio::ip::address_v4::loopback(), 0});
    if (plan.successor_receive_buffer != 0) {
        boost::system::error_code buffer_error;
        successor_acceptor.set_option(
            asio::socket_base::receive_buffer_size(
                plan.successor_receive_buffer),
            buffer_error);
        require(!buffer_error,
                "pre-park fixture could not enlarge the successor receive "
                "buffer");
    }
    std::future<ServerRunResult> head_server = asio::co_spawn(
        context, server.accept_one(head_acceptor, head_server_control),
        asio::use_future);
    std::future<ServerRunResult> successor_server = asio::co_spawn(
        context, server.accept_one(successor_acceptor), asio::use_future);
    std::future<void> head_client = asio::co_spawn(
        context,
        raw_prepark_head(head_acceptor.local_endpoint(), guids.c, head_tu,
                         nonce, race),
        asio::use_future);
    // The same wide buffer on the sender side: one async_write then hands
    // the whole batch to the kernel in a single delivery.
    PreparkSuccessorShape successor_shape = plan.shape;
    if (plan.successor_receive_buffer != 0)
        successor_shape.socket_buffer = plan.successor_receive_buffer;
    std::future<void> successor_client = asio::co_spawn(
        context,
        raw_prepark_successor(successor_acceptor.local_endpoint(), guids.c,
                              successor_tu, nonce, race, successor_shape),
        asio::use_future);
    // Release the head's materialization on a fixed delay after the
    // successor landed its batch. The probe window (200 ms) always runs
    // inside that delay; the gate is freed even when the successor
    // coroutine dies early (a pre-fix red throws inside the probe or the
    // STATE read), where a releaser that waited for probe_done would
    // leave the head's codec worker blocked in the gate forever and the
    // leg would hang instead of failing.
    std::future<void> releaser = asio::co_spawn(
        context,
        [&]() -> asio::awaitable<void> {
            asio::steady_timer timer(co_await asio::this_coro::executor);
            while (!race.begin_written.load()) {
                timer.expires_after(std::chrono::milliseconds(1));
                boost::system::error_code ignored;
                co_await timer.async_wait(
                    asio::redirect_error(asio::use_awaitable, ignored));
            }
            timer.expires_after(std::chrono::milliseconds(400));
            boost::system::error_code ignored;
            co_await timer.async_wait(
                asio::redirect_error(asio::use_awaitable, ignored));
            release_head.set_value();
            co_return;
        },
        asio::use_future);
    context.run_for(std::chrono::seconds(20));
    // Nothing throws from inside the coroutines any more: a leg that ends
    // with anything less than all four futures ready means a coroutine died
    // mid-dialogue (an unexpected exception -- a fixture bug), not a
    // production regression.
    const bool head_server_done =
        head_server.wait_for(std::chrono::seconds(0)) ==
        std::future_status::ready;
    const bool successor_server_done =
        successor_server.wait_for(std::chrono::seconds(0)) ==
        std::future_status::ready;
    const bool head_client_done =
        head_client.wait_for(std::chrono::seconds(0)) ==
        std::future_status::ready;
    const bool successor_client_done =
        successor_client.wait_for(std::chrono::seconds(0)) ==
        std::future_status::ready;
    require(head_server_done && successor_server_done && head_client_done &&
                successor_client_done,
            std::string(plan.name) +
                ": hung (coroutine died mid-dialogue -- fixture bug, not a "
                "pre-park regression)");
    head_client.get();
    successor_client.get();
    const ServerRunResult head_run = head_server.get();
    const ServerRunResult successor_run = successor_server.get();
    if (successor_run.terminal_error.has_value())
        std::fprintf(stderr,
                     "p50_endpoint_test: %s: successor session ended "
                     "TerminalError code=%u detail=%s\n",
                     plan.name,
                     static_cast<unsigned>(successor_run.terminal_error->code),
                     successor_run.terminal_error->detail.c_str());
    // The successor's recorded outcomes, asserted OUTSIDE the io_context:
    // a red run (a reverted fix) records its failure here and unwinds
    // cleanly, so the leg fails -- not hangs -- with this exact message.
    require(!race.probe_saw_bytes.load(std::memory_order_acquire) &&
                !race.send_failed.load(std::memory_order_acquire) &&
                race.state_witness_matched.load(std::memory_order_acquire) &&
                race.commit_witness_matched.load(std::memory_order_acquire),
            std::string(plan.name) + ": " +
                (race.detail.empty()
                     ? std::string("pre-park successor outcome was not "
                                   "recorded (fixture bug)")
                     : race.detail));
    require(head_run.status == ServerRunStatus::Completed &&
                successor_run.status == ServerRunStatus::Completed,
            "pre-park pair did not complete both sessions");
    require(head_run.f_rel_seq.value == 0 &&
                successor_run.f_rel_seq.value == 1,
            "pre-park pair did not run at its declared rel_seq");
    // The successor's HELLO-time peek: the delayed-BEGIN shapes must NOT
    // have seen their own BEGIN (the 100 ms delay is the whole fixture);
    // the batched-HELLO shape MUST have (and exactly at its own rel 1 --
    // the peek-time park key).
    require(successor_run.f_hello_time_peeked_begin_rel_seq.has_value() ==
                plan.expect_peeked_begin,
            "pre-park fixture broke: the successor's BEGIN was "
            "(un)peekable at HELLO time against the plan");
    if (plan.expect_peeked_begin)
        require(successor_run.f_hello_time_peeked_begin_rel_seq->value == 1,
                "pre-park fixture broke: the peeked BEGIN was not the "
                "successor's own rel 1");
    (void)releaser;
    // Commit order and exact bytes on the F route.
    std::vector<uint64_t> f_commit_order;
    for (const ActionRecord& record : actions.records()) {
        if (record.actor != ActorSide::F || record.c_store_guid != guids.c)
            continue;
        if (record.action == ActionType::INPUT_COMMITTED)
            f_commit_order.push_back(record.rel_seq.value);
    }
    require(f_commit_order == std::vector<uint64_t>{0, 1},
            "pre-park pair committed its TUs out of route order");
    require(copy_input(server, guids.c) ==
                std::optional<std::vector<uint8_t>>(plan.successor_input),
            "pre-park successor did not materialize exact bytes");
}

void test_route_pipeline_prepark_when_begin_lands_after_hello() {
    PreparkPairPlan plan;
    plan.name = "test_route_pipeline_prepark_when_begin_lands_after_hello";
    plan.successor_input = bytes("pre-park: successor TU\n");
    run_prepark_pair(plan);
}

// ---- Triple-session pre-park legs (660cc787 regressions) ----
//
// Two holes the pair legs cannot reach, both fixed by the cursor-claim
// re-arm (660cc787):
//   (a) the release-to-install gap: release_parked_at() wakes the successor
//       whose BEGIN equals the new cursor, but it installs its pending TX
//       only after it resumes and finishes its straddling drain. A third
//       session's HELLO landing in that gap saw NO pending TX and staged
//       against a cursor the claimant was about to consume. Post-fix the
//       route carries cursor_claim_serial through the gap and the third
//       session pre-parks on the claim.
//   (b) K>2 key collision: a pre-park registers at next_rel + 1 BEFORE it
//       knows its own rel_seq -- at depth > 2 that is an earlier
//       successor's key. When that key is released, the pre-parked session
//       whose first frame is still unknown must RE-ARM one past the new
//       cursor (the route is still busy: the released successor's claim),
//       not fall through.
//
// Recorded-outcome style throughout (same as the pair legs): nothing
// throws inside a coroutine; every witness is compared after run_for.

struct PreparkTripleRace {
    // Set by the head's materialize gate: its TX is pending on the route.
    std::atomic<bool>* head_pending = nullptr;
    std::atomic<bool>* head_route_ready = nullptr;
    std::atomic<bool> s1_begin_written{false};
    // k3 storm: set right after S1's HELLO write (the releaser waits for
    // both HELLOs, then a short delay -- the head must commit inside the
    // successors' HELLO-to-BEGIN windows).
    std::atomic<bool> s1_hello_written{false};
    std::atomic<bool> s2_hello_written{false};
    // Stage-gap variant: set by S1's server-session outbound observer when
    // its SESSION_STATE (the fall-through stage's own write) has left F --
    // provably inside S1's stage-to-install window. S2's BEGIN is written
    // only after it.
    std::atomic<bool> s1_state_written{false};
    // Set by the raw head after its TX_COMMIT read (release_parked_at ran
    // synchronously inside that commit: the gap/collision window is open).
    std::atomic<bool> head_committed{false};
    // Set by S1 after ITS TX_COMMIT read: its install and commit have both
    // happened, so S2's re-armed key and its own BEGIN are the live ones.
    std::atomic<bool> s1_committed{false};
    std::atomic<bool> s1_probe_saw_bytes{false};
    std::atomic<bool> s1_state_witness_matched{false};
    std::atomic<bool> s1_commit_witness_matched{false};
    std::string s1_detail;
    std::atomic<bool> s2_begin_written{false};
    std::atomic<bool> s2_state_witness_matched{false};
    std::atomic<bool> s2_commit_witness_matched{false};
    std::string s2_detail;
};

// S1 (rel 1): the standard missed-peek successor -- HELLO while the head is
// pending, then a delayed BEGIN -- with one addition for the gap shape: its
// BODY frame straddles (only the first half lands up front), and the tail
// waits for s2_hello_written so the release-to-install gap provably covers
// S2's HELLO. S1's commit read is the gate for the whole leg's order.
asio::awaitable<void> raw_prepark_triple_s1(
    tcp::endpoint remote, CStoreGuid c_guid, const ZstdTuEnvelope& s1_tu,
    HistoryNonce nonce, PreparkTripleRace& race, bool hold_tail_for_gap,
    int begin_delay_ms = 100, bool probe = true) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    co_await socket.async_connect(remote, asio::use_awaitable);
    SessionHello hello;
    hello.c_store_guid = c_guid;
    hello.supported_profiles = profile_bit(ProfileId::ZSTD_TU);
    hello.limits = {};
    const std::vector<std::vector<uint8_t>> frames =
        successor_wire_frames(s1_tu, hello.limits.max_frame_payload);
    // HELLO only after the head's TX is installed and pending (the gate
    // flag): the pre-park condition is route-busy, and HELLO before the
    // head's reset would see no route at all.
    asio::steady_timer pending_wait(executor);
    while (!race.head_pending->load(std::memory_order_acquire)) {
        pending_wait.expires_after(std::chrono::milliseconds(1));
        boost::system::error_code ignored;
        co_await pending_wait.async_wait(
            asio::redirect_error(asio::use_awaitable, ignored));
    }
    // Every asio write on a raw peer is wrapped: a red run kills this
    // session at F (terminal ERROR -> socket closed), and an EPIPE escaping
    // the co_await resumption propagates straight out of the io_context
    // (terminate), not into the co_spawn future. Recorded, never thrown.
    try {
        co_await raw_write(socket, hello);
    } catch (const boost::system::system_error& error) {
        if (race.s1_detail.empty())
            race.s1_detail = std::string("S1 HELLO write died: ") +
                              error.what();
        boost::system::error_code ignored;
        socket.close(ignored);
        co_return;
    }
    race.s1_hello_written.store(true, std::memory_order_release);
    // The missed-peek delay: BEGIN lands only after F consumed HELLO. The
    // k3 storm passes its own (shorter) delay so the head's commit lands
    // inside the HELLO-to-BEGIN window while both successors are still
    // begin-unknown at their release.
    if (begin_delay_ms > 0) {
        asio::steady_timer delay(executor);
        delay.expires_after(std::chrono::milliseconds(begin_delay_ms));
        co_await delay.async_wait(asio::use_awaitable);
    }
    // The prefix: the BEGIN frame plus (for the gap shape) the first half
    // of the first BODY frame; the whole batch in ONE contiguous raw send
    // (the single-delivery shape, wide buffers pinned by the driver).
    std::vector<uint8_t> prefix;
    for (const std::vector<uint8_t>& frame : frames) {
        if (hold_tail_for_gap && !prefix.empty()) {
            const size_t keep = frame.size() / 2;
            prefix.insert(prefix.end(), frame.begin(), frame.begin() + keep);
            break;
        }
        prefix.insert(prefix.end(), frame.begin(), frame.end());
    }
    co_await raw_send_all(socket, prefix);
    race.s1_begin_written.store(true, std::memory_order_release);
    // No-stage probe FIRST: while the head is still gated (pending), S1 must
    // be parked, not staged. Recorded, never thrown. This has to run
    // BEFORE the gap hold below: past the hold, the head's commit has
    // released S1 and its STATE legitimately arrives -- a probe there
    // would false-red the green path. The k3 storm disables it: the head
    // commits inside S1's HELLO-to-BEGIN window by design, so its release
    // lands while the probe would still be ticking.
    if (probe) {
        asio::steady_timer probe_timer(executor);
        for (int iteration = 0; iteration != 20; ++iteration) {
            probe_timer.expires_after(std::chrono::milliseconds(10));
            boost::system::error_code ignored;
            co_await probe_timer.async_wait(
                asio::redirect_error(asio::use_awaitable, ignored));
            boost::system::error_code available_error;
            const std::size_t available = socket.available(available_error);
            if (available != 0) {
                race.s1_probe_saw_bytes.store(true,
                                              std::memory_order_release);
                if (race.s1_detail.empty())
                    race.s1_detail =
                        "S1 received bytes while its predecessor was still "
                        "pending (stage() ran early, or a terminal ERROR "
                        "arrived)";
            }
        }
    }
    if (hold_tail_for_gap) {
        // The release-to-install hold: the head commits (releases S1 at
        // the cursor; its claim is now the route's only owner) and S2's
        // HELLO lands in that gap -- only then does the tail complete S1's
        // frame so F can install. The extra 100 ms after both flags is
        // what makes the gap provably cover S2's HELLO PROCESSING (not
        // just its write): S1's finish_partial_frame read is already
        // armed at release time and epoll would happily service the
        // tail's readability first, installing S1 before F ever looked
        // at S2's HELLO -- the fixture would pass pre-fix. Withholding
        // the tail 100 ms after the HELLO write leaves F nothing to do
        // but service the ready HELLO read (loopback, an otherwise idle
        // io_context), so S2's park/stage decision provably precedes
        // S1's install. This is the same class of margin as the pair
        // legs' HELLO-consumed delay.
        asio::steady_timer gap_wait(executor);
        while (!(race.head_committed.load(std::memory_order_acquire) &&
                 race.s2_hello_written.load(std::memory_order_acquire))) {
            gap_wait.expires_after(std::chrono::milliseconds(1));
            boost::system::error_code ignored;
            co_await gap_wait.async_wait(
                asio::redirect_error(asio::use_awaitable, ignored));
        }
        gap_wait.expires_after(std::chrono::milliseconds(100));
        boost::system::error_code gap_ignored;
        co_await gap_wait.async_wait(
            asio::redirect_error(asio::use_awaitable, gap_ignored));
        std::vector<uint8_t> tail;
        for (size_t index = 1; index != frames.size(); ++index) {
            const std::vector<uint8_t>& frame = frames[index];
            if (index == 1) {
                const size_t keep = frame.size() / 2;
                tail.insert(tail.end(), frame.begin() + keep, frame.end());
                continue;
            }
            tail.insert(tail.end(), frame.begin(), frame.end());
        }
        co_await raw_send_all(socket, tail);
    }
    // STATE / TX_COMMIT witness (S1's OWN predicted cursor 1): wrapped, all
    // red shapes record and unwind.
    try {
        const SessionState state = raw_decode<SessionState>(
            co_await raw_read(socket, hello.limits.max_frame_payload));
        const bool state_witness =
            state.route_present && state.history_nonce == nonce &&
            state.next_rel_seq.value == 1 &&
            state.state_digest == s1_tu.begin.pre_state_digest;
        race.s1_state_witness_matched.store(state_witness,
                                            std::memory_order_release);
        if (state_witness) {
            const TxCommit commit = raw_decode<TxCommit>(co_await raw_read(
                socket, state.limits.max_frame_payload));
            const bool commit_witness =
                commit.transaction_digest == s1_tu.begin.transaction_digest &&
                commit.raw_digest == s1_tu.begin.raw_digest;
            race.s1_commit_witness_matched.store(commit_witness,
                                                 std::memory_order_release);
            if (!commit_witness && race.s1_detail.empty())
                race.s1_detail = "S1 received a different TX_COMMIT";
        } else if (race.s1_detail.empty()) {
            race.s1_detail =
                "S1 observed a route cursor that never advanced to its "
                "own predicted witness";
        }
    } catch (const std::exception& error) {
        if (race.s1_detail.empty())
            race.s1_detail =
                std::string("S1 dialogue died: ") + error.what();
    }
    race.s1_committed.store(true, std::memory_order_release);
    boost::system::error_code ignored;
    socket.close(ignored);
    co_return;
}

// S2 (rel 2): the third session. Its HELLO lands while the route is busy --
// for the gap shape inside S1's release-to-install window (the claim), for
// the collision shape while the head is still gated (pending). Its BEGIN is
// held back (the collision shape keeps S2's first frame unknown through the
// head's commit so the re-arm path is the one that runs). STATE must carry
// S2's OWN predicted cursor 2.
asio::awaitable<void> raw_prepark_triple_s2(
    tcp::endpoint remote, CStoreGuid c_guid, const ZstdTuEnvelope& s2_tu,
    HistoryNonce nonce, PreparkTripleRace& race, bool begin_after_s1_commit,
    bool hello_in_gap, int begin_delay_ms = 100,
    bool begin_after_s1_state = false,
    bool hello_after_s1_begin = false) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    co_await socket.async_connect(remote, asio::use_awaitable);
    SessionHello hello;
    hello.c_store_guid = c_guid;
    hello.supported_profiles = profile_bit(ProfileId::ZSTD_TU);
    hello.limits = {};
    if (hello_in_gap) {
        // The gap shape: HELLO lands strictly between the head's commit
        // (S1 released at the cursor, its claim set) and S1's install
        // (the tail). Both flags are level-triggered waits.
        asio::steady_timer gap_wait(executor);
        while (!race.head_committed.load(std::memory_order_acquire)) {
            gap_wait.expires_after(std::chrono::milliseconds(1));
            boost::system::error_code ignored;
            co_await gap_wait.async_wait(
                asio::redirect_error(asio::use_awaitable, ignored));
        }
    } else if (!hello_after_s1_begin) {
        // k3 storm and stage-gap variant: HELLO lands while the head's TX
        // is still pending, WITHOUT waiting for S1's batch (both
        // successors register their pre-park begin-unknown before either
        // BEGIN is on the wire).
        asio::steady_timer pending_wait(executor);
        while (!race.head_pending->load(std::memory_order_acquire)) {
            pending_wait.expires_after(std::chrono::milliseconds(1));
            boost::system::error_code ignored;
            co_await pending_wait.async_wait(
                asio::redirect_error(asio::use_awaitable, ignored));
        }
    } else {
        // The collision shape: HELLO lands while the head's TX is still
        // pending (the route busy on pending), alongside S1's own pre-park.
        asio::steady_timer pending_wait(executor);
        while (!(race.head_pending->load(std::memory_order_acquire) &&
                 race.s1_begin_written.load(std::memory_order_acquire))) {
            pending_wait.expires_after(std::chrono::milliseconds(1));
            boost::system::error_code ignored;
            co_await pending_wait.async_wait(
                asio::redirect_error(asio::use_awaitable, ignored));
        }
    }
    // Every asio write on a raw peer is wrapped (same rule as S1): a red
    // run kills this session at F and an EPIPE escaping the co_await
    // resumption would terminate the process instead of failing the leg.
    try {
        co_await raw_write(socket, hello);
    } catch (const boost::system::system_error& error) {
        if (race.s2_detail.empty())
            race.s2_detail = std::string("S2 HELLO write died: ") +
                             error.what();
        boost::system::error_code ignored;
        socket.close(ignored);
        co_return;
    }
    race.s2_hello_written.store(true, std::memory_order_release);
    if (begin_after_s1_commit) {
        // Collision shape: S2's BEGIN is held back until S1 has committed,
        // so through the head's commit release S2's first frame is unknown
        // and the RE-ARM path is the one that must run (a release of the
        // colliding key while the route is still busy).
        asio::steady_timer commit_wait(executor);
        while (!race.s1_committed.load(std::memory_order_acquire)) {
            commit_wait.expires_after(std::chrono::milliseconds(1));
            boost::system::error_code ignored;
            co_await commit_wait.async_wait(
                asio::redirect_error(asio::use_awaitable, ignored));
        }
    } else if (begin_after_s1_state) {
        // Stage-gap variant: S2's BEGIN is written only after S1's own
        // fall-through stage() SESSION_STATE has left F (the per-session
        // outbound observer flips s1_state_written) -- provably inside
        // S1's stage-to-install window, the exact gap the cursor claim
        // must cover. S2 then decides by its BEGIN's rel_seq after the
        // drain, never on the stale pre-release route.
        asio::steady_timer state_wait(executor);
        while (!race.s1_state_written.load(std::memory_order_acquire)) {
            state_wait.expires_after(std::chrono::milliseconds(1));
            boost::system::error_code ignored;
            co_await state_wait.async_wait(
                asio::redirect_error(asio::use_awaitable, ignored));
        }
    } else {
        // Gap shape and k3 storm: the BEGIN must NOT be peekable at HELLO
        // time (the step-5b peek path parks through it -- pre-fix AND
        // post-fix -- and the hole under test is the PRE-PARK decision).
        // The delay after the HELLO write keeps the peek empty, exactly
        // the pair legs' missed-peek delay: F processes the HELLO with
        // nothing behind it, and the pre-park/claim decision is the one
        // under test. The k3 storm passes its own (uneven) delay so both
        // successors are begin-unknown through the head's commit.
        asio::steady_timer begin_delay(executor);
        begin_delay.expires_after(
            std::chrono::milliseconds(begin_delay_ms));
        co_await begin_delay.async_wait(asio::use_awaitable);
    }
    // The production two-write order: BEGIN, then the BODY frames. S2 parks
    // (gap shape: on S1's claim; collision shape: re-armed at the new key
    // after the head's commit) and its BEGIN then drains in the park loop.
    try {
        co_await raw_write(socket, s2_tu.begin);
        Message body_message = BodyMessage{s2_tu.body};
        co_await raw_write(socket, std::move(body_message));
    } catch (const boost::system::system_error& error) {
        if (race.s2_detail.empty())
            race.s2_detail = std::string("S2 BEGIN/BODY write died: ") +
                             error.what();
        race.s2_begin_written.store(true, std::memory_order_release);
        boost::system::error_code ignored;
        socket.close(ignored);
        co_return;
    }
    race.s2_begin_written.store(true, std::memory_order_release);
    // No probe on S2: the head's gate is long released by the time S2's
    // BEGIN lands (both shapes), so "no bytes while pending" is not
    // provable here -- the red/green signal is S2's STATE witness (a
    // fall-through stage() against a foreign cursor answers with the wrong
    // cursor, and any terminal ERROR kills the dialogue).
    try {
        const SessionState state = raw_decode<SessionState>(
            co_await raw_read(socket, hello.limits.max_frame_payload));
        const bool state_witness =
            state.route_present && state.history_nonce == nonce &&
            state.next_rel_seq.value == 2 &&
            state.state_digest == s2_tu.begin.pre_state_digest;
        race.s2_state_witness_matched.store(state_witness,
                                            std::memory_order_release);
        if (state_witness) {
            const TxCommit commit = raw_decode<TxCommit>(co_await raw_read(
                socket, state.limits.max_frame_payload));
            const bool commit_witness =
                commit.transaction_digest == s2_tu.begin.transaction_digest &&
                commit.raw_digest == s2_tu.begin.raw_digest;
            race.s2_commit_witness_matched.store(commit_witness,
                                                 std::memory_order_release);
            if (!commit_witness && race.s2_detail.empty())
                race.s2_detail = "S2 received a different TX_COMMIT";
        } else if (race.s2_detail.empty()) {
            race.s2_detail =
                "S2 observed a route cursor that never advanced to its "
                "own predicted witness (staged against a foreign cursor: "
                "the release-to-install gap / K>2 key collision class)";
        }
    } catch (const std::exception& error) {
        if (race.s2_detail.empty())
            race.s2_detail =
                std::string("S2 dialogue died: ") + error.what();
    }
    boost::system::error_code ignored;
    socket.close(ignored);
    co_return;
}

// The raw head is exactly the pair leg's (gated materialize, commit read,
// head_committed flag). Reused verbatim through the same PreparkRace
// plumbing? The triple race has different field names, so a thin adapter:
// the head needs head_pending (gate), head_route_ready (unused), and
// head_committed.
asio::awaitable<void> raw_prepark_triple_head(
    tcp::endpoint remote, CStoreGuid c_guid, const ZstdTuEnvelope& head_tu,
    HistoryNonce nonce, PreparkTripleRace& race) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    const RawRoute open = co_await raw_open(socket, remote, c_guid);
    const SessionState reset_ack = co_await raw_reset(socket, open, nonce);
    require(reset_ack.route_present && reset_ack.history_nonce == nonce &&
                reset_ack.state_digest == initial_route_digest(c_guid, nonce) &&
                reset_ack.next_rel_seq.value == 0,
            "triple head did not establish its fresh route");
    const TxBegin begin =
        make_begin(head_tu, nonce, reset_ack.state_digest,
                   reset_ack.next_rel_seq);
    co_await raw_write(socket, begin);
    Message body_message = BodyMessage{head_tu.body};
    co_await raw_write(socket, std::move(body_message));
    // Deterministic level-triggered wait: the head's commit read happens
    // only after S1's batch is on the wire. NOT S2's: in the gap shape S2's
    // BEGIN lands only after head_committed (which this read sets), and in
    // the collision shape only after s1_committed -- waiting for it here
    // is a fixture deadlock. S2's HELLO registration is instead guaranteed
    // by the releaser's own wait (plus its fixed delay) before the gate
    // frees.
    asio::steady_timer wait(executor);
    while (!race.s1_begin_written.load()) {
        wait.expires_after(std::chrono::milliseconds(1));
        boost::system::error_code ignored;
        co_await wait.async_wait(
            asio::redirect_error(asio::use_awaitable, ignored));
    }
    const TxCommit commit = raw_decode<TxCommit>(
        co_await raw_read(socket, reset_ack.limits.max_frame_payload));
    require(commit.transaction_digest == begin.transaction_digest &&
                commit.raw_digest == begin.raw_digest,
            "triple head received a different TX_COMMIT");
    // Strictly after release_parked_at() ran inside that commit: the
    // release-to-install gap / the collision window is now provably open.
    race.head_committed.store(true, std::memory_order_release);
    boost::system::error_code ignored;
    socket.close(ignored);
    co_return;
}

struct PreparkTriplePlan {
    const char* name = "";
    std::vector<uint8_t> s1_input;
    std::vector<uint8_t> s2_input;
    // Gap shape (660cc787 hole 1): S1 straddles, S2's HELLO lands inside
    // S1's release-to-install window. Collision shape (hole 2): S2's
    // HELLO lands while the head is pending and its BEGIN waits for S1's
    // commit (the re-arm path).
    bool gap_shape = true;
    size_t zstd_cap = 1U << 20;
    // k3 storm (farm K>2 hole): both successors HELLO while the head is
    // pending, both pre-park begin-unknown at the SAME next_rel + 1 key,
    // and their BEGINs are delayed past the head's commit (uneven, so the
    // later one waits while the earlier one installs). The gate frees a
    // short fixed delay after BOTH HELLOs -- inside the HELLO-to-BEGIN
    // windows.
    bool k3_storm = false;
    int s1_begin_delay_ms = 100;
    int s2_begin_delay_ms = 100;
    int release_delay_ms = 25;
    // Stage-gap variant of the k3 storm: S2's BEGIN is written only after
    // S1's fall-through stage SESSION_STATE left F (the per-session
    // outbound observer) -- inside S1's stage-to-install window, the gap
    // the cursor claim must cover.
    bool s2_begin_after_s1_state = false;
};

void run_prepark_triple(const PreparkTriplePlan& plan) {
    const P5coStoreGuids guids = p5co_store_guids(52);
    EndpointCaps caps;
    caps.profile = ProfileId::ZSTD_TU;
    caps.zstd.max_raw_bytes = plan.zstd_cap;
    caps.zstd.max_encoded_body_bytes = plan.zstd_cap;
    ActionTrace actions;
    P50ServerEndpoint server(guids.f, caps, nullptr, &actions);
    const HistoryNonce nonce{482};
    const ZstdTuEnvelope head_tu =
        encode_zstd_tu(nonce, RelSeq{0}, TuSeq{4820},
                        initial_route_digest(guids.c, nonce),
                        bytes("triple: head TU\n"));
    const Digest128 s1_pre_state = compute_post_state_digest(
        head_tu.begin.pre_state_digest, head_tu.begin.history_nonce,
        head_tu.begin.rel_seq, head_tu.begin.tu_seq,
        head_tu.begin.transaction_digest);
    const ZstdTuEnvelope s1_tu =
        encode_zstd_tu(nonce, RelSeq{1}, TuSeq{4821}, s1_pre_state,
                       plan.s1_input);
    const Digest128 s2_pre_state = compute_post_state_digest(
        s1_tu.begin.pre_state_digest, s1_tu.begin.history_nonce,
        s1_tu.begin.rel_seq, s1_tu.begin.tu_seq,
        s1_tu.begin.transaction_digest);
    const ZstdTuEnvelope s2_tu =
        encode_zstd_tu(nonce, RelSeq{2}, TuSeq{4822}, s2_pre_state,
                       plan.s2_input);

    std::promise<void> release_head;
    std::future<void> head_gate = release_head.get_future();
    std::atomic<bool> head_materialize_entered{false};
    std::atomic<bool> head_route_ready{false};
    EndpointIoControl head_server_control;
    head_server_control.before_materialize_on_worker = [&]() {
        if (!head_materialize_entered.exchange(true))
            head_gate.wait();
    };
    PreparkTripleRace race;
    race.head_pending = &head_materialize_entered;
    race.head_route_ready = &head_route_ready;
    // Stage-gap variant only: the per-session outbound observer flips
    // s1_state_written when S1's fall-through stage SESSION_STATE has left
    // F -- inside S1's stage-to-install window (the write suspends, the
    // dispatch/install is the synchronous continuation right after).
    EndpointIoControl s1_server_control;
    if (plan.s2_begin_after_s1_state) {
        s1_server_control.outbound_message_observer =
            [&race](ActorSide, const Message& message) {
                if (std::holds_alternative<SessionState>(message))
                    race.s1_state_written.store(true,
                                                std::memory_order_release);
            };
    }
    asio::io_context context;
    tcp::acceptor head_acceptor(context, {asio::ip::address_v4::loopback(), 0});
    tcp::acceptor s1_acceptor(context, {asio::ip::address_v4::loopback(), 0});
    tcp::acceptor s2_acceptor(context, {asio::ip::address_v4::loopback(), 0});
    std::future<ServerRunResult> head_server = asio::co_spawn(
        context, server.accept_one(head_acceptor, head_server_control),
        asio::use_future);
    std::future<ServerRunResult> s1_server = asio::co_spawn(
        context, server.accept_one(s1_acceptor, s1_server_control),
        asio::use_future);
    std::future<ServerRunResult> s2_server = asio::co_spawn(
        context, server.accept_one(s2_acceptor), asio::use_future);
    std::future<void> head_client = asio::co_spawn(
        context,
        raw_prepark_triple_head(head_acceptor.local_endpoint(), guids.c,
                                head_tu, nonce, race),
        asio::use_future);
    std::future<void> s1_client = asio::co_spawn(
        context,
        raw_prepark_triple_s1(s1_acceptor.local_endpoint(), guids.c, s1_tu,
                              nonce, race, plan.gap_shape,
                              /*begin_delay_ms=*/
                              plan.k3_storm ? plan.s1_begin_delay_ms : 100,
                              /*probe=*/!plan.k3_storm),
        asio::use_future);
    std::future<void> s2_client = asio::co_spawn(
        context,
        raw_prepark_triple_s2(s2_acceptor.local_endpoint(), guids.c, s2_tu,
                              nonce, race,
                              /*begin_after_s1_commit=*/!plan.gap_shape &&
                                             !plan.k3_storm,
                              /*hello_in_gap=*/plan.gap_shape,
                              /*begin_delay_ms=*/
                              plan.k3_storm ? plan.s2_begin_delay_ms : 100,
                              /*begin_after_s1_state=*/
                              plan.s2_begin_after_s1_state,
                              /*hello_after_s1_begin=*/!plan.gap_shape &&
                                         !plan.k3_storm),
        asio::use_future);
    // Fixed-delay releaser (the pair legs' rule): the gate is freed even
    // when a red run kills a successor early, so the leg fails instead of
    // hanging. The delay covers S1's probe window. In the gap shape the
    // release must additionally come AFTER S2's HELLO landed (the gap is
    // only open once the head commits; before that the route is busy on
    // pending, not the claim -- waiting for s2_hello_written here would
    // deadlock: S2's HELLO in the gap shape needs head_committed, which
    // needs this release). So: wait for S1's batch, then a fixed delay
    // (400 ms) that S2's HELLO has certainly landed within by then --
    // in the gap shape S2 writes HELLO the instant head_committed flips,
    // and the 400 ms delay is plenty; in the collision shape S2's HELLO
    // lands while the head is still gated, well before the release.
    // The k3 storm instead frees the gate a short fixed delay after BOTH
    // HELLOs -- provably inside both successors' HELLO-to-BEGIN windows
    // (their BEGINs are withheld past it), so both pre-parks are
    // begin-unknown at their release.
    std::future<void> releaser = asio::co_spawn(
        context,
        [&]() -> asio::awaitable<void> {
            asio::steady_timer timer(co_await asio::this_coro::executor);
            if (plan.k3_storm) {
                while (!(race.s1_hello_written.load() &&
                         race.s2_hello_written.load())) {
                    timer.expires_after(std::chrono::milliseconds(1));
                    boost::system::error_code ignored;
                    co_await timer.async_wait(
                        asio::redirect_error(asio::use_awaitable, ignored));
                }
                timer.expires_after(
                    std::chrono::milliseconds(plan.release_delay_ms));
            } else {
                while (!race.s1_begin_written.load()) {
                    timer.expires_after(std::chrono::milliseconds(1));
                    boost::system::error_code ignored;
                    co_await timer.async_wait(
                        asio::redirect_error(asio::use_awaitable, ignored));
                }
                timer.expires_after(std::chrono::milliseconds(400));
            }
            boost::system::error_code ignored;
            co_await timer.async_wait(
                asio::redirect_error(asio::use_awaitable, ignored));
            release_head.set_value();
            co_return;
        },
        asio::use_future);
    context.run_for(std::chrono::seconds(30));
    const bool all_done =
        head_server.wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready &&
        s1_server.wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready &&
        s2_server.wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready &&
        head_client.wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready &&
        s1_client.wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready &&
        s2_client.wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready;
    require(all_done, std::string(plan.name) +
                          ": hung (coroutine died mid-dialogue -- fixture "
                          "bug, not a pre-park regression)");
    head_client.get();
    s1_client.get();
    s2_client.get();
    const ServerRunResult head_run = head_server.get();
    const ServerRunResult s1_run = s1_server.get();
    const ServerRunResult s2_run = s2_server.get();
    if (s1_run.terminal_error.has_value())
        std::fprintf(stderr,
                     "p50_endpoint_test: %s: S1 session ended TerminalError "
                     "code=%u detail=%s\n",
                     plan.name,
                     static_cast<unsigned>(s1_run.terminal_error->code),
                     s1_run.terminal_error->detail.c_str());
    if (s2_run.terminal_error.has_value())
        std::fprintf(stderr,
                     "p50_endpoint_test: %s: S2 session ended TerminalError "
                     "code=%u detail=%s\n",
                     plan.name,
                     static_cast<unsigned>(s2_run.terminal_error->code),
                     s2_run.terminal_error->detail.c_str());
    // S1's recorded outcomes (identical rule to the pair legs).
    require(!race.s1_probe_saw_bytes.load(std::memory_order_acquire) &&
                race.s1_state_witness_matched.load(
                    std::memory_order_acquire) &&
                race.s1_commit_witness_matched.load(
                    std::memory_order_acquire),
            std::string(plan.name) + ": S1: " +
                (race.s1_detail.empty()
                     ? std::string("outcome was not recorded (fixture bug)")
                     : race.s1_detail));
    // S2's recorded outcomes: the red class of BOTH holes lands here -- a
    // stage() against the claimant's cursor serves a STATE whose witness
    // is not S2's own (or kills the dialogue outright).
    require(race.s2_state_witness_matched.load(std::memory_order_acquire) &&
                race.s2_commit_witness_matched.load(
                    std::memory_order_acquire),
            std::string(plan.name) + ": S2: " +
                (race.s2_detail.empty()
                     ? std::string("outcome was not recorded (fixture bug)")
                     : race.s2_detail));
    require(head_run.status == ServerRunStatus::Completed &&
                s1_run.status == ServerRunStatus::Completed &&
                s2_run.status == ServerRunStatus::Completed,
            "triple did not complete all three sessions");
    require(head_run.f_rel_seq.value == 0 &&
                s1_run.f_rel_seq.value == 1 &&
                s2_run.f_rel_seq.value == 2,
            "triple did not run at its declared rel_seq");
    // All three commit in route order, and the route holds exactly the
    // last successor's bytes.
    std::vector<uint64_t> f_commit_order;
    for (const ActionRecord& record : actions.records()) {
        if (record.actor != ActorSide::F || record.c_store_guid != guids.c)
            continue;
        if (record.action == ActionType::INPUT_COMMITTED)
            f_commit_order.push_back(record.rel_seq.value);
    }
    require(f_commit_order == std::vector<uint64_t>{0, 1, 2},
            "triple committed its TUs out of route order");
    (void)releaser;
}

// 660cc787 hole 1 -- the release-to-install gap. S1 (rel 1) is released at
// the cursor when the head commits, but its install is held by its
// straddling BODY frame (the tail lands only after S2's HELLO). S2's HELLO
// lands in exactly that window: the route has NO pending TX, only S1's
// claim. Pre-fix S2 saw a free route and staged against the cursor S1 was
// about to consume ("TX_BEGIN differs from the F route cursor" on the
// K2-2 farm leg); post-fix it pre-parks on the claim, and all three commit
// in order with S2's STATE carrying its OWN cursor-2 witness.
void test_route_pipeline_prepark_third_hello_in_release_to_install_gap() {
    PreparkTriplePlan plan;
    plan.name =
        "test_route_pipeline_prepark_third_hello_in_release_to_install_"
        "gap";
    plan.gap_shape = true;
    plan.s1_input = bytes("triple gap: S1 TU\n");
    plan.s2_input = bytes("triple gap: S2 TU\n");
    run_prepark_triple(plan);
}

// 660cc787 hole 2 -- the K>2 key collision. S1 (rel 1) and S2 (rel 2) both
// HELLO while the head is gated; both pre-park at next_rel + 1 = 1 (S2's own
// rel_seq is unknown to it then) -- the SAME key. S2's BEGIN is held until
// S1 has committed, so through the head's commit S2's first frame is
// unknown: the release of key 1 must RE-ARM S2 one past the new cursor (the
// route is still busy -- first S1's claim, then S1's pending), not fall
// through. Pre-fix the collision release dropped S2 out of its pre-park
// and it staged against S1's in-flight cursor.
void test_route_pipeline_prepark_k3_collision_re_arms_on_busy_release() {
    PreparkTriplePlan plan;
    plan.name =
        "test_route_pipeline_prepark_k3_collision_re_arms_on_busy_"
        "release";
    plan.gap_shape = false;
    plan.s1_input = bytes("triple k3: S1 TU\n");
    plan.s2_input = bytes("triple k3: S2 TU\n");
    run_prepark_triple(plan);
}

// Farm K>2 hole (k24/k24st legs, te=3 "pipelined successor STATE differs
// from the predicted route witness", 2026-10-09): S1 (rel 1) and S2 (rel 2)
// both HELLO while the head's TX is pending, so both pre-park
// begin-unknown at the SAME next_rel + 1 key; the head commits inside
// both successors' HELLO-to-BEGIN windows (the gate frees a fixed 25 ms
// after both HELLOs, their BEGINs withheld 60/190 ms past their HELLOs --
// uneven, the farm shape). Neither released waiter is begin_known, so NO
// cursor claim is set and the route is momentarily free: pre-fix BOTH
// fell through and raced the same stage() against cursor 1, and S2's
// STATE carried the wrong cursor. Post-fix neither may fall through
// without its own first frame (has_other_begin_unknown_parked): S1's
// BEGIN drains first (rel == cursor -> fall-through, claim, install,
// commit), S2's then re-keys onto rel 2 and commits after it -- 0,1,2.
void test_route_pipeline_prepark_k3_storm_two_begin_unknown_release() {
    PreparkTriplePlan plan;
    plan.name =
        "test_route_pipeline_prepark_k3_storm_two_begin_unknown_"
        "release";
    plan.gap_shape = false;
    plan.k3_storm = true;
    plan.s1_begin_delay_ms = 60;
    plan.s2_begin_delay_ms = 190;
    plan.release_delay_ms = 25;
    plan.s1_input = bytes("k3 storm: S1 TU\n");
    plan.s2_input = bytes("k3 storm: S2 TU\n");
    run_prepark_triple(plan);
}

// The stage-to-install slice of the same hole: S1's fall-through stage()
// SESSION_STATE write suspends before the dispatch installs S1's pending
// TX, and S2's BEGIN (written only after the per-session outbound
// observer saw that STATE leave F -- provably inside the window) must be
// decided by its rel_seq AFTER the drain, never by the stale pre-drain
// route: it re-keys onto rel 2 and commits after S1, instead of racing
// S1's install with a fall-through stage against cursor 1.
void test_route_pipeline_prepark_k3_s2_begin_during_s1_stage_gap() {
    PreparkTriplePlan plan;
    plan.name =
        "test_route_pipeline_prepark_k3_s2_begin_during_s1_stage_"
        "gap";
    plan.gap_shape = false;
    plan.k3_storm = true;
    plan.s1_begin_delay_ms = 60;
    plan.s2_begin_delay_ms = 190;
    plan.release_delay_ms = 25;
    plan.s2_begin_after_s1_state = true;
    plan.s1_input = bytes("k3 stage gap: S1 TU\n");
    plan.s2_input = bytes("k3 stage gap: S2 TU\n");
    run_prepark_triple(plan);
}

// Committed-then-poisoned sender regression (55509637's committed-entry
// branch; the farm's 1x "prepared-TU handle is not active P29V1"): a TX
// whose run returned COMMITTED while ANOTHER TX's deviation had already
// set the endpoint's pipeline poison. Its sender then runs the production
// step-4d retirement -- has_pipeline_poison() -> abandon_route(its own
// committed handle) -> reset_after_pipeline_poison() -- exactly as
// transfer_bytes does, in that order. Pre-fix abandon_route rejected a
// committed handle outright (std::invalid_argument), which the sender's
// catch-all escalated to TerminalError + replacement_required -> sticky
// whole-C sidecar replacement. Post-fix the committed entry's own
// nothing-to-abandon case instead tears the remaining outstanding chain
// down uniformly (the poison has already invalidated it), the cold reset
// lands, and the next TX commits through the ordinary dance.
//
// Deterministic shape: a depth-2 P29V1 client against a real F. The head
// is gated at F's materialize (its FILL on the wire); a successor is
// admitted on the head's write-complete and its successor run is pointed
// at a DEAD port -- the connect refusal deviates it on the successor path
// (same epoch, so it poisons: the poison flag is set and NOBODY resets it
// yet, because the test IS the sender and delays both senders' step-4d
// until both runs returned). The gate then frees; the head commits. Now
// the head's handle is committed and the endpoint is poisoned -- the
// exact committed-then-poisoned sender state.
void test_p29v1_committed_sender_abandons_chain_under_foreign_poison() {
    const P5coStoreGuids guids = p5co_store_guids(253);
    EndpointCaps caps;
    caps.profile = ProfileId::P29V1;
    caps.zstd.max_raw_bytes = 4U << 20;
    caps.zstd.max_encoded_body_bytes = 4U << 20;
    PreparationAuthorityLimits limits;
    limits.route_pipeline_depth = 2;
    limits.max_interner_reserved_bytes = UINT64_C(1044398080);
    TestClient client(guids.c, caps, HistoryNonce{1}, nullptr, nullptr, limits);
    P50ServerEndpoint server(guids.f, caps);

    std::promise<void> release_materialize;
    std::future<void> materialize_gate = release_materialize.get_future();
    EndpointIoControl head_server_control;
    head_server_control.before_materialize_on_worker = [&materialize_gate]() {
        materialize_gate.wait();
    };

    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    const tcp::endpoint remote = acceptor.local_endpoint();
    std::future<ServerRunResult> head_server = asio::co_spawn(
        context, server.accept_one(acceptor, head_server_control),
        asio::use_future);
    // No second server session: the successor run below connects to a
    // dead port on purpose (its connect refusal is the deviation), so a
    // second accept_one would never complete and hang the leg.

    // A dead loopback port for the successor's connect refusal.
    tcp::acceptor dead_listener(context, {asio::ip::address_v4::loopback(), 0});
    const tcp::endpoint dead = dead_listener.local_endpoint();
    dead_listener.close();

    const std::vector<uint8_t> head_input =
        bytes("# 1 \"/tmp/committed-poison.cc\"\nshared-head\nhead-line\n");
    const std::vector<uint8_t> successor_input =
        bytes("# 1 \"/tmp/committed-poison.cc\"\nshared-head\nsuccessor-line\n");
    const PreparedTuHandle head_handle = admit(client, head_input);
    const PreparedTuHandle head_handle_sender_copy = head_handle;

    std::atomic<bool> dispatched{false};
    std::optional<std::future<ClientRunResult>> successor_result;
    std::optional<PreparedTuHandle> successor_handle;
    EndpointIoControl head_control;
    head_control.write_complete = [&]() {
        if (dispatched.exchange(true))
            return;
        asio::post(context, [&]() {
            successor_handle = admit(client, successor_input);
            // The successor run's own socket is the dead port: its connect
            // refusal deviates it on the successor path (same epoch) and
            // poisons the pipeline. Nothing resets the flag until the
            // test's sender-mirror below.
            successor_result.emplace(asio::co_spawn(
                context,
                client.endpoint.run(
                    dead, *successor_handle, EndpointIoControl{},
                    std::chrono::steady_clock::now() +
                        std::chrono::seconds(10)),
                asio::use_future));
        });
    };
    std::future<ClientRunResult> head_result = asio::co_spawn(
        context, client.endpoint.run(remote, head_handle, head_control),
        asio::use_future);

    // Wait for the successor's deviation (poison set), then free the head's
    // materialize so the head commits UNDER that poison: both sender-mirror
    // steps below then observe exactly the committed-then-poisoned state.
    while (!successor_result.has_value() ||
           successor_result->wait_for(std::chrono::seconds(0)) !=
               std::future_status::ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (!context.stopped())
            context.poll();
    }
    release_materialize.set_value();
    while (head_result.wait_for(std::chrono::seconds(0)) !=
           std::future_status::ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (!context.stopped())
            context.poll();
    }
    // Poll the head's server session to completion as well; context.run()
    // would hang (the acceptor stays armed and the successor's dead-port
    // run never contributes a connection to it).
    while (head_server.wait_for(std::chrono::seconds(0)) !=
           std::future_status::ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (!context.stopped())
            context.poll();
    }
    const ClientRunResult head = head_result.get();
    const ClientRunResult successor = successor_result->get();
    require(head.status == ClientRunStatus::Committed,
            "committed-then-poisoned: the healthy gated head did not commit");
    require(successor.status != ClientRunStatus::Committed,
            "committed-then-poisoned: the dead-port successor committed");
    require(client.endpoint.has_pipeline_poison(),
            "committed-then-poisoned: the successor deviation did not "
            "poison the pipeline");

    // The head's sender step-4d, exactly the transfer_bytes order.
    try {
        client.authority->abandon_route(head_handle_sender_copy);
    } catch (const std::exception& error) {
        require(false,
                std::string("committed-then-poisoned: a committed TX's "
                            "sender could not retire its route under "
                            "another TX's poison (pre-fix this threw: ") +
                    error.what() + ")");
    }
    client.endpoint.reset_after_pipeline_poison();
    require(!client.endpoint.has_pipeline_poison() &&
                client.endpoint.pipeline_epoch_for_test() >= 1 &&
                client.endpoint.next_rel_seq().value == 0,
            "committed-then-poisoned: the cold reset did not land");
    // The successor's sender mirror runs AFTER the head's: the flag is
    // already cleared, so its own step-4d sees no poison and abandons
    // nothing (its entry was torn down with the chain).
    require(!client.endpoint.has_pipeline_poison(),
            "committed-then-poisoned: poison re-armed after the reset");

    // The next TX after the cold reset commits through the ordinary dance.
    const std::vector<uint8_t> next_input =
        bytes("# 1 \"/tmp/committed-poison.cc\"\nshared-head\npost-reset-line\n");
    const PairResult next_pair = run_pair(client, server, admit(client, next_input));
    require(next_pair.client.status == ClientRunStatus::Committed &&
                next_pair.server.status == ServerRunStatus::Completed &&
                copy_input(server, guids.c) == next_input,
            "committed-then-poisoned: the next TX after the route reset did "
            "not commit");
}

// Large-TU regression (farm bug fixed in 54158e17): the pre-park drain
// ceiling used to be checked BEFORE the first-frame decision. A single
// drain carrying the successor's TX_BEGIN plus several MB of its BODY (one
// contiguous write into a 16 MiB receive buffer -- the high-BDP production
// shape) tripped the 1 MiB pre-BEGIN ceiling and F answered "pre-parked
// connection exceeded its declared TX size" (ERROR code 1) for exactly the
// successor whose BEGIN declares that size. The TU is >= 3 MiB of seeded
// pseudo-random bytes: incompressible, so the encoded body is the same
// order as the raw input and no test cap can be satisfied by accident.
void test_route_pipeline_prepark_large_tu_single_drain_decides_from_first_frame() {
    PreparkPairPlan plan;
    plan.name =
        "test_route_pipeline_prepark_large_tu_single_drain_decides_from_"
        "first_frame";
    plan.successor_input = pseudo_random_bytes(3U << 20);
    plan.shape.batch_begin_with_body = true;
    plan.zstd_cap = 16U << 20;
    plan.successor_receive_buffer = 16U << 20;
    run_prepark_pair(plan);
}

// Straddling-BODY regression for the PRE-PARK loop (farm bug fixed in
// 355a35aa): the successor lands BEGIN plus only the FIRST HALF of its
// first BODY frame; the head then commits and the pre-park is released
// holding that half frame in its parser. F must finish the frame from the
// socket (the tail is written only after the head's TX_COMMIT was read)
// before the unchanged dispatch sees it -- pre-fix the live reads resumed
// mid-frame and failed with "unknown frame type" / "frame payload exceeds
// configured cap".
void test_route_pipeline_prepark_straddling_body_frame_finishes_before_dispatch() {
    PreparkPairPlan plan;
    plan.name =
        "test_route_pipeline_prepark_straddling_body_frame_finishes_before_"
        "dispatch";
    plan.successor_input = pseudo_random_bytes(3U << 20);
    plan.shape.batch_begin_with_body = true;
    plan.shape.split_first_body_frame = true;
    plan.zstd_cap = 16U << 20;
    plan.successor_receive_buffer = 16U << 20;
    run_prepark_pair(plan);
}

// The same straddling shape through the PEEK-TIME park loop (step 5c, BEGIN
// readable at HELLO): HELLO, BEGIN and the half BODY frame land in one
// contiguous write, so the step-5b peek records the BEGIN and the
// peek-time park takes the connection. Its drain stops on the same
// straddling frame at the release, and the same finish_partial_frame must
// run.
void test_route_pipeline_park_straddling_body_frame_finishes_before_dispatch() {
    PreparkPairPlan plan;
    plan.name =
        "test_route_pipeline_park_straddling_body_frame_finishes_before_"
        "dispatch";
    plan.successor_input = pseudo_random_bytes(3U << 20);
    plan.shape.batch_hello_with_frames = true;
    plan.shape.split_first_body_frame = true;
    plan.zstd_cap = 16U << 20;
    plan.successor_receive_buffer = 16U << 20;
    plan.expect_peeked_begin = true;
    run_prepark_pair(plan);
}

// T-F grace leg (Fix F): HELLO with no further bytes while the head's TX is
// pending. The pre-park holds (nothing to fall through on), and when the
// head commits, release_parked_at -- level-triggered -- wakes it; the
// fall-through runs the unchanged stage()/STATE path against the advanced
// cursor. The successor then closes without a BEGIN, so its session ends in
// the ordinary mid-dialogue EOF: never a hang, never a terminal latch on
// the route (the next TU commits through the unchanged path).
void test_route_pipeline_prepark_grace_falls_through_after_head_commit() {
    const P5coStoreGuids guids = p5co_store_guids(49);
    EndpointCaps caps;
    caps.profile = ProfileId::ZSTD_TU;
    caps.zstd.max_raw_bytes = 1U << 20;
    caps.zstd.max_encoded_body_bytes = 1U << 20;
    P50ServerEndpoint server(guids.f, caps);
    const HistoryNonce nonce{491};
    const ZstdTuEnvelope head_tu =
        encode_zstd_tu(nonce, RelSeq{0}, TuSeq{4910},
                        initial_route_digest(guids.c, nonce),
                        bytes("pre-park grace: head TU\n"));

    std::promise<void> release_head;
    std::future<void> head_gate = release_head.get_future();
    std::atomic<bool> head_materialize_entered{false};
    std::atomic<bool> successor_hello_written{false};
    std::atomic<bool> successor_probe_done{false};
    std::atomic<bool> head_released{false};
    EndpointIoControl head_server_control;
    head_server_control.before_materialize_on_worker = [&]() {
        if (!head_materialize_entered.exchange(true))
            head_gate.wait();
    };
    asio::io_context context;
    tcp::acceptor head_acceptor(context, {asio::ip::address_v4::loopback(), 0});
    tcp::acceptor successor_acceptor(
        context, {asio::ip::address_v4::loopback(), 0});
    std::future<ServerRunResult> head_server = asio::co_spawn(
        context, server.accept_one(head_acceptor, head_server_control),
        asio::use_future);
    std::future<ServerRunResult> successor_server = asio::co_spawn(
        context, server.accept_one(successor_acceptor), asio::use_future);
    std::future<void> head_client = asio::co_spawn(
        context,
        [&]() -> asio::awaitable<void> {
            const auto executor = co_await asio::this_coro::executor;
            tcp::socket socket(executor);
            const tcp::endpoint remote = head_acceptor.local_endpoint();
            co_await socket.async_connect(remote, asio::use_awaitable);
            SessionHello hello;
            hello.c_store_guid = guids.c;
            hello.supported_profiles = profile_bit(ProfileId::ZSTD_TU);
            hello.limits = {};
            const RawRoute open = co_await raw_open(socket, remote, guids.c);
            const SessionState reset_ack =
                co_await raw_reset(socket, open, nonce);
            require(reset_ack.route_present &&
                        reset_ack.history_nonce == nonce,
                    "grace head did not establish its route");
            const TxBegin begin = make_begin(
                head_tu, nonce, reset_ack.state_digest,
                reset_ack.next_rel_seq);
            co_await raw_write(socket, begin);
            Message body_message = BodyMessage{head_tu.body};
            co_await raw_write(socket, std::move(body_message));
            const TxCommit commit = raw_decode<TxCommit>(
                co_await raw_read(socket, reset_ack.limits.max_frame_payload));
            require(commit.transaction_digest == begin.transaction_digest,
                    "grace head received a different TX_COMMIT");
            boost::system::error_code ignored;
            socket.close(ignored);
            co_return;
        },
        asio::use_future);
    // The HELLO-only successor: writes HELLO, probes that nothing is staged
    // while the head is gated (the pre-park must hold, not fall through:
    // falling through would stage() a SESSION_STATE onto this socket), then
    // closes -- surfacing the pre-park's readable-but-empty EOF path.
    std::future<void> successor_client = asio::co_spawn(
        context,
        [&]() -> asio::awaitable<void> {
            const auto executor = co_await asio::this_coro::executor;
            tcp::socket socket(executor);
            const tcp::endpoint remote =
                successor_acceptor.local_endpoint();
            co_await socket.async_connect(remote, asio::use_awaitable);
            // Same deterministic ordering as the park test: HELLO only
            // after the head's TX is pending on the route at F.
            asio::steady_timer pending_wait(executor);
            while (!head_materialize_entered.load(std::memory_order_acquire)) {
                pending_wait.expires_after(std::chrono::milliseconds(1));
                boost::system::error_code ignored;
                co_await pending_wait.async_wait(
                    asio::redirect_error(asio::use_awaitable, ignored));
            }
            SessionHello hello;
            hello.c_store_guid = guids.c;
            hello.supported_profiles = profile_bit(ProfileId::ZSTD_TU);
            hello.limits = {};
            co_await raw_write(socket, hello);
            successor_hello_written.store(true);
            // Park proof while gated: no SESSION_STATE byte may arrive.
            asio::steady_timer probe(executor);
            for (int iteration = 0; iteration != 20; ++iteration) {
                probe.expires_after(std::chrono::milliseconds(10));
                boost::system::error_code ignored;
                co_await probe.async_wait(
                    asio::redirect_error(asio::use_awaitable, ignored));
                boost::system::error_code available_error;
                const std::size_t available =
                    socket.available(available_error);
                require(!available_error && available == 0,
                        "grace successor received bytes while the head was "
                        "still pending (stage() ran early)");
            }
            successor_probe_done.store(true);
            // Stay connected through the release: the head commits, the
            // pre-park wakes through release_parked_at, falls through, and
            // stage() writes the post-commit STATE onto THIS socket (the
            // grace fall-through). Only then close -- never sending a
            // BEGIN -- so F ends in the ordinary mid-dialogue EOF.
            asio::steady_timer wait_release(executor);
            while (!head_released.load()) {
                wait_release.expires_after(std::chrono::milliseconds(1));
                boost::system::error_code ignored;
                co_await wait_release.async_wait(
                    asio::redirect_error(asio::use_awaitable, ignored));
            }
            // The released fall-through stages STATE here; drain it so the
            // session proceeds to the BEGIN wait, then close: EOF.
            boost::system::error_code ignored;
            const SessionState staged = raw_decode<SessionState>(
                co_await raw_read(socket, hello.limits.max_frame_payload));
            require(staged.route_present && staged.next_rel_seq.value == 1,
                    "grace successor's fall-through STATE did not carry the "
                    "post-commit cursor");
            socket.close(ignored);
            co_return;
        },
        asio::use_future);
    // Release the head's materialization only after the successor's whole
    // probe window has run (level-triggered flags, the same deterministic
    // handoff as the park test): the head then commits, release_parked_at
    // wakes the pre-parked successor, the fall-through stages the
    // post-commit STATE onto its socket, and its close ends the session in
    // the ordinary EOF.
    std::future<void> releaser = asio::co_spawn(
        context,
        [&]() -> asio::awaitable<void> {
            asio::steady_timer timer(co_await asio::this_coro::executor);
            while (!successor_hello_written.load() ||
                   !successor_probe_done.load()) {
                timer.expires_after(std::chrono::milliseconds(1));
                boost::system::error_code ignored;
                co_await timer.async_wait(
                    asio::redirect_error(asio::use_awaitable, ignored));
            }
            release_head.set_value();
            head_released.store(true);
            co_return;
        },
        asio::use_future);
    context.run_for(std::chrono::seconds(10));
    require(head_server.wait_for(std::chrono::seconds(0)) ==
                    std::future_status::ready &&
                successor_server.wait_for(std::chrono::seconds(0)) ==
                    std::future_status::ready &&
                head_client.wait_for(std::chrono::seconds(0)) ==
                    std::future_status::ready &&
                successor_client.wait_for(std::chrono::seconds(0)) ==
                    std::future_status::ready,
            "test_route_pipeline_prepark_grace_falls_through_after_head_"
            "commit: hung (pre-park grace regression?)");
    head_client.get();
    successor_client.get();
    const ServerRunResult head_run = head_server.get();
    const ServerRunResult successor_run = successor_server.get();
    require(head_run.status == ServerRunStatus::Completed,
            "grace pair: the gated head did not commit");
    require(successor_run.status == ServerRunStatus::Disconnected,
            "grace pair: the HELLO-only successor did not end in the "
            "ordinary disconnect");
    require(head_run.f_rel_seq.value == 0,
            "grace pair: the head did not run at rel 0");
    require(copy_input(server, guids.c) ==
                std::optional<std::vector<uint8_t>>(
                    bytes("pre-park grace: head TU\n")),
            "grace pair: the head did not materialize exact bytes");
    // The route stays fully usable through the unchanged path.
    const std::vector<uint8_t> recovery_input =
        bytes("pre-park grace: recovery TU\n");
    const ZstdTuEnvelope recovery_tu =
        encode_zstd_tu(nonce, RelSeq{1}, TuSeq{4911},
                       compute_post_state_digest(
                           head_tu.begin.pre_state_digest,
                           head_tu.begin.history_nonce,
                           head_tu.begin.rel_seq, head_tu.begin.tu_seq,
                           head_tu.begin.transaction_digest),
                       recovery_input);
    asio::io_context recovery;
    tcp::acceptor recovery_acceptor(
        recovery, {asio::ip::address_v4::loopback(), 0});
    std::future<ServerRunResult> recovery_server = asio::co_spawn(
        recovery, server.accept_one(recovery_acceptor), asio::use_future);
    std::future<void> recovery_client = asio::co_spawn(
        recovery,
        raw_commit_one_tu(recovery_acceptor.local_endpoint(), guids.c,
                          recovery_tu, nonce, RelSeq{1}),
        asio::use_future);
    recovery.run_for(std::chrono::seconds(5));
    require(recovery_server.wait_for(std::chrono::seconds(0)) ==
                    std::future_status::ready &&
                recovery_client.wait_for(std::chrono::seconds(0)) ==
                    std::future_status::ready,
            "grace pair: the recovery leg hung");
    recovery_client.get();
    const ServerRunResult recovery_run = recovery_server.get();
    require(recovery_run.status == ServerRunStatus::Completed &&
                recovery_run.f_rel_seq.value == 1,
            "grace pair: the route was not usable after the pre-park "
            "grace fall-through");
    require(copy_input(server, guids.c) ==
                std::optional<std::vector<uint8_t>>(recovery_input),
            "grace pair: the recovery TU did not materialize exact bytes");
    (void)releaser;
}

// T-F grace-expiry leg (Fix F fixup, sorbet-main review): the pre-park's
// bounded grace must FALL THROUGH when it expires -- not throw. The timer
// handler cancels the socket wait; without the grace-expired flag the loop
// would see operation_aborted, released == false, and throw system_error,
// killing the session before stage() ever ran. Here the grace bound is
// shortened through the endpoint config (the test-only override; production
// keeps 2000 ms), the head stays gated (its TX pending) for longer than the
// grace, and the successor sends HELLO and nothing else. F must fall
// through at the expiry, run the unchanged stage(), and write a SESSION_STATE
// carrying the HEAD's cursor (the head never committed) onto the successor
// socket -- the session then ends in the ordinary mid-dialogue EOF, never a
// pre-park error, and the head's pending TX is untouched (no activate() ran:
// the successor sent no BEGIN), so the head still commits once released.
void test_route_pipeline_prepark_grace_expiry_falls_through_not_throws() {
    const P5coStoreGuids guids = p5co_store_guids(50);
    EndpointCaps caps;
    caps.profile = ProfileId::ZSTD_TU;
    caps.zstd.max_raw_bytes = 1U << 20;
    caps.zstd.max_encoded_body_bytes = 1U << 20;
    P50ServerEndpointConfig server_config;
    server_config.prepark_grace_bound_ms = 150;
    P50ServerEndpoint server(guids.f, caps, nullptr, nullptr, server_config);
    const HistoryNonce nonce{501};
    const ZstdTuEnvelope head_tu =
        encode_zstd_tu(nonce, RelSeq{0}, TuSeq{5010},
                        initial_route_digest(guids.c, nonce),
                        bytes("pre-park expiry: head TU\n"));

    std::promise<void> release_head;
    std::future<void> head_gate = release_head.get_future();
    std::atomic<bool> head_materialize_entered{false};
    std::atomic<bool> successor_state_read{false};
    std::atomic<bool> successor_closed{false};
    EndpointIoControl head_server_control;
    head_server_control.before_materialize_on_worker = [&]() {
        if (!head_materialize_entered.exchange(true))
            head_gate.wait();
    };

    asio::io_context context;
    tcp::acceptor head_acceptor(context, {asio::ip::address_v4::loopback(), 0});
    tcp::acceptor successor_acceptor(
        context, {asio::ip::address_v4::loopback(), 0});
    std::future<ServerRunResult> head_server = asio::co_spawn(
        context, server.accept_one(head_acceptor, head_server_control),
        asio::use_future);
    std::future<ServerRunResult> successor_server = asio::co_spawn(
        context, server.accept_one(successor_acceptor), asio::use_future);
    // The gated raw head: HELLO + route reset + BEGIN + BODY, then held.
    std::future<void> head_client = asio::co_spawn(
        context,
        [&]() -> asio::awaitable<void> {
            const auto executor = co_await asio::this_coro::executor;
            tcp::socket socket(executor);
            const RawRoute open =
                co_await raw_open(socket, head_acceptor.local_endpoint(),
                                  guids.c);
            const SessionState reset_ack =
                co_await raw_reset(socket, open, nonce);
            const TxBegin begin =
                make_begin(head_tu, nonce, reset_ack.state_digest,
                           reset_ack.next_rel_seq);
            co_await raw_write(socket, begin);
            Message body_message = BodyMessage{head_tu.body};
            co_await raw_write(socket, std::move(body_message));
            // The commit read completes only after the test releases the
            // gate (the successor has then observed the grace fall-through).
            const TxCommit commit = raw_decode<TxCommit>(
                co_await raw_read(socket, reset_ack.limits.max_frame_payload));
            require(commit.transaction_digest == begin.transaction_digest,
                    "pre-park expiry head received a different TX_COMMIT");
            boost::system::error_code ignored;
            socket.close(ignored);
            co_return;
        },
        asio::use_future);
    // The HELLO-only successor: HELLO after the head's TX is pending, then
    // nothing. The 150 ms grace expires (the head stays gated far longer);
    // the fall-through stages a STATE with the HEAD's cursor onto this
    // socket. Read it, then close: the ordinary EOF.
    std::future<void> successor_client = asio::co_spawn(
        context,
        [&]() -> asio::awaitable<void> {
            const auto executor = co_await asio::this_coro::executor;
            tcp::socket socket(executor);
            const tcp::endpoint remote =
                successor_acceptor.local_endpoint();
            co_await socket.async_connect(remote, asio::use_awaitable);
            asio::steady_timer pending_wait(executor);
            while (!head_materialize_entered.load(std::memory_order_acquire)) {
                pending_wait.expires_after(std::chrono::milliseconds(1));
                boost::system::error_code ignored;
                co_await pending_wait.async_wait(
                    asio::redirect_error(asio::use_awaitable, ignored));
            }
            SessionHello hello;
            hello.c_store_guid = guids.c;
            hello.supported_profiles = profile_bit(ProfileId::ZSTD_TU);
            hello.limits = {};
            co_await raw_write(socket, hello);
            // The grace expiry falls through: the staged STATE arrives
            // WITHOUT any release (the head is still gated). A regression
            // that throws on the grace instead would leave this read
            // pending forever (the future-readiness assertion below then
            // fails instead of hanging the leg).
            const SessionState staged = raw_decode<SessionState>(
                co_await raw_read(socket, hello.limits.max_frame_payload));
            successor_state_read.store(true);
            // Today's fall-through semantics: the head never committed, so
            // the staged cursor is still the HEAD's (rel 0) -- NOT the
            // successor's.
            require(staged.route_present && staged.history_nonce == nonce &&
                        staged.next_rel_seq.value == 0,
                    "pre-park expiry fall-through staged a cursor it must "
                    "not have (the head never committed)");
            boost::system::error_code ignored;
            socket.close(ignored);
            successor_closed.store(true);
            co_return;
        },
        asio::use_future);
    // Release the head's materialization only after the successor observed
    // the grace fall-through STATE and closed: the head then commits
    // (proving its pending TX survived the fall-through untouched).
    std::future<void> releaser = asio::co_spawn(
        context,
        [&]() -> asio::awaitable<void> {
            asio::steady_timer timer(co_await asio::this_coro::executor);
            while (!successor_state_read.load() ||
                   !successor_closed.load()) {
                timer.expires_after(std::chrono::milliseconds(1));
                boost::system::error_code ignored;
                co_await timer.async_wait(
                    asio::redirect_error(asio::use_awaitable, ignored));
            }
            release_head.set_value();
            co_return;
        },
        asio::use_future);
    context.run_for(std::chrono::seconds(10));
    require(head_server.wait_for(std::chrono::seconds(0)) ==
                    std::future_status::ready &&
                successor_server.wait_for(std::chrono::seconds(0)) ==
                    std::future_status::ready &&
                head_client.wait_for(std::chrono::seconds(0)) ==
                    std::future_status::ready &&
                successor_client.wait_for(std::chrono::seconds(0)) ==
                    std::future_status::ready,
            "test_route_pipeline_prepark_grace_expiry_falls_through_not_"
            "throws: hung (grace-expiry threw instead of falling through?)");
    head_client.get();
    successor_client.get();
    const ServerRunResult head_run = head_server.get();
    const ServerRunResult successor_run = successor_server.get();
    // The head committed exactly (its pending TX was never replaced: the
    // fall-through's stage() ran, but no BEGIN was read from the successor,
    // so activate()/install never ran).
    require(head_run.status == ServerRunStatus::Completed &&
                head_run.f_rel_seq.value == 0,
            "pre-park expiry: the head did not commit after the grace "
            "fall-through (its pending TX must be untouched)");
    // The successor session ended in the ordinary mid-dialogue EOF -- never
    // a pre-park error (a grace-expiry throw would have surfaced as a
    // TerminalError before any STATE was staged, and the successor's STATE
    // read above would never have completed).
    require(successor_run.status == ServerRunStatus::Disconnected,
            "pre-park expiry: the successor session did not end in the "
            "ordinary disconnect");
    require(copy_input(server, guids.c) ==
                std::optional<std::vector<uint8_t>>(
                    bytes("pre-park expiry: head TU\n")),
            "pre-park expiry: the head did not materialize exact bytes");
    (void)releaser;
}

// T-C (Fix C): three same-route TXs at depth 2, the third dispatched while
// the second is a NOT-YET-PROMOTED successor -- the commit-to-promotion
// gap. The head commits (Impl::accept resets `active`, wakes the slot
// waiters) and returns (active_session 0), but TX2 is promoted only when
// its own coroutine resumes. TX3's dispatch (after its production-shaped
// wait_for_window_slot) lands in the gap or just after TX2's promotion --
// in either order the entry dispatch must take the pipelined branch
// (Impl::outstanding: the successors deque / active head) and admit TX3 as
// the next successor. The old dispatch chose the HEAD path from
// active_session == 0 (then TX2's promotion threw "promoted over a live
// head dialogue") or the full-window throw, both poisoning the route.
//
// Determinism: the head's materialization is gated on F's codec worker, so
// TX2's admission (posted from the head's write_complete) happens strictly
// while the head is outstanding, and TX3 is posted from TX2's write_complete
// -- TX2 is then in the successors deque, suspended at its STATE read (F
// parks it), unpromoted. TX3 arms a registered window-slot wait before the
// gate releases (the test waits on the waiter-count test hook, not a
// sleep), so the head's accept is what wakes it.
void test_p29v1_k2_real_three_tx_dispatch_across_promotion_gap() {
    const P5coStoreGuids guids = p5co_store_guids(253);
    EndpointCaps caps;
    caps.profile = ProfileId::P29V1;
    caps.zstd.max_raw_bytes = 4U << 20;
    caps.zstd.max_encoded_body_bytes = 4U << 20;
    PreparationAuthorityLimits limits;
    limits.route_pipeline_depth = 2;
    limits.max_interner_reserved_bytes = UINT64_C(1044398080);
    TestClient client(guids.c, caps, HistoryNonce{1}, nullptr, nullptr, limits);
    P50ServerEndpoint server(guids.f, caps);

    std::promise<void> release_head;
    std::future<void> head_gate = release_head.get_future();
    std::atomic<bool> head_materialize_entered{false};
    EndpointIoControl head_server_control;
    head_server_control.before_materialize_on_worker = [&]() {
        if (!head_materialize_entered.exchange(true))
            head_gate.wait();
    };

    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    const tcp::endpoint remote = acceptor.local_endpoint();
    std::future<ServerRunResult> head_server = asio::co_spawn(
        context, server.accept_one(acceptor, head_server_control),
        asio::use_future);
    std::future<ServerRunResult> tx2_server = asio::co_spawn(
        context, server.accept_one(acceptor), asio::use_future);
    std::future<ServerRunResult> tx3_server = asio::co_spawn(
        context, server.accept_one(acceptor), asio::use_future);

    const std::vector<uint8_t> head_input =
        bytes("# 1 \"/tmp/k2-real-d.cc\"\nshared-d-head\nhead-line\n");
    const std::vector<uint8_t> tx2_input =
        bytes("# 1 \"/tmp/k2-real-d.cc\"\nshared-d-head\ntx2-line\n");
    const std::vector<uint8_t> tx3_input =
        bytes("# 1 \"/tmp/k2-real-d.cc\"\nshared-d-head\ntx3-line\n");
    const PreparedTuHandle head_handle = admit(client, head_input);

    std::atomic<bool> tx2_dispatched{false};
    std::atomic<bool> tx3_dispatched{false};
    std::atomic<bool> tx3_observed_active{false};
    std::atomic<bool> tx3_observed_gap{false};
    std::optional<std::future<ClientRunResult>> tx2_result;
    std::optional<std::future<ClientRunResult>> tx3_result;
    std::optional<PreparedTuHandle> tx2_handle;
    std::optional<PreparedTuHandle> tx3_handle;

    EndpointIoControl head_control;
    head_control.write_complete = [&]() {
        if (tx2_dispatched.exchange(true))
            return;
        asio::post(context, [&]() {
            tx2_handle = admit(client, tx2_input);
            EndpointIoControl tx2_control;
            // TX2's own write_complete posts TX3: at that point TX2 is an
            // admitted-but-unpromoted successor (in the deque, suspended at
            // its STATE read, parked on F) and the head is still gated.
            tx2_control.write_complete = [&]() {
                if (tx3_dispatched.exchange(true))
                    return;
                asio::post(context, [&]() {
                    tx3_result.emplace(asio::co_spawn(
                        context,
                        [&]() -> asio::awaitable<ClientRunResult> {
                            // The production sender shape: wait for a free
                            // window slot BEFORE prepare_for_route and the
                            // run -- the authority's own uncommitted-successor
                            // bound (prepare_for_route's admission) is only
                            // reachable once the head's accept freed its
                            // entry, which is exactly what the slot wait
                            // gates on. A bounded wait so the waiter
                            // registers (wait_for_window_slot only
                            // registers its timer under a deadline), and so
                            // a regression that loses the wake fails
                            // instead of hanging.
                            const bool slot =
                                co_await client.endpoint.wait_for_window_slot(
                                    std::chrono::steady_clock::now() +
                                    std::chrono::seconds(10));
                            require(slot,
                                    "T-C: TX3 window slot wait failed");
                            tx3_handle = admit(client, tx3_input);
                            // Built inside the coroutine: this body resumes
                            // after the slot-wait suspension, by which time
                            // the posting lambda's frame is gone, so the
                            // control must not dangle from it.
                            EndpointIoControl tx3_control;
                            tx3_control.outbound_message_observer =
                                [&](ActorSide actor, const Message&) {
                                    if (actor != ActorSide::C)
                                        return;
                                    // Which dispatch order happened: TX3
                                    // admitted while TX2 was already
                                    // promoted (active), or while TX2 was
                                    // still unpromoted (the gap). Both are
                                    // valid post-fix; both must commit.
                                    if (client.endpoint.has_active_transaction())
                                        tx3_observed_active.store(true);
                                    else
                                        tx3_observed_gap.store(true);
                                };
                            co_return co_await client.endpoint.run(
                                remote, *tx3_handle, tx3_control,
                                std::nullopt);
                        },
                        asio::use_future));
                });
            };
            tx2_result.emplace(asio::co_spawn(
                context,
                client.endpoint.run(remote, *tx2_handle, tx2_control),
                asio::use_future));
        });
    };
    std::future<ClientRunResult> head_result = asio::co_spawn(
        context, client.endpoint.run(remote, head_handle, head_control),
                asio::use_future);
    // Release the head's materialization only once TX3 is posted AND its
    // window-slot wait is armed (the waiter-count test hook -- deterministic
    // release point, no sleep): the head then commits, its accept wakes
    // TX3's slot wait, and TX3 dispatches across the gap.
    std::future<void> releaser = asio::co_spawn(
        context,
        [&]() -> asio::awaitable<void> {
            asio::steady_timer timer(co_await asio::this_coro::executor);
            while (!tx3_dispatched.load() ||
                   client.endpoint
                           .pipeline_window_slot_waiter_count_for_test() ==
                       0) {
                timer.expires_after(std::chrono::milliseconds(1));
                boost::system::error_code ignored;
                co_await timer.async_wait(
                    asio::redirect_error(asio::use_awaitable, ignored));
            }
            release_head.set_value();
            co_return;
        },
        asio::use_future);
    context.run();

    const ClientRunResult head = head_result.get();
    require(tx2_result.has_value() && tx3_result.has_value(),
            "T-C: TX2 or TX3 were never posted");
    const ClientRunResult tx2 = tx2_result->get();
    const ClientRunResult tx3 = tx3_result->get();
    (void)head_server;
    (void)tx2_server;
    (void)tx3_server;
    (void)releaser;
    const auto describe = [](const ClientRunResult& result) {
        return std::string("status=") +
               std::to_string(static_cast<uint8_t>(result.status)) +
               " terminal=" +
               (result.terminal_error.has_value() ? result.terminal_error->detail
                                                  : std::string("<none>"));
    };
    require(head.status == ClientRunStatus::Committed,
            "T-C: the head TX did not commit (" + describe(head) + ")");
    require(tx2.status == ClientRunStatus::Committed,
            "T-C: TX2 did not commit (" + describe(tx2) + ")");
    require(tx3.status == ClientRunStatus::Committed,
            "T-C: TX3 did not commit (" + describe(tx3) + ")");
    // All three committed with an unbroken digest chain in rel order, and
    // no poison / epoch change ever latched.
    require(head.committed_commit.has_value() &&
                tx2.committed_commit.has_value() &&
                tx3.committed_commit.has_value(),
            "T-C: commit witnesses missing");
    require(tx2.committed_commit->tu_seq.value >
                head.committed_commit->tu_seq.value &&
                tx3.committed_commit->tu_seq.value >
                    tx2.committed_commit->tu_seq.value,
            "T-C: TUs did not commit in order");
    require(client.endpoint.next_rel_seq().value == 3,
            "T-C: the confirmed rel_seq did not advance past all three TXs");
    require(!client.endpoint.has_pipeline_poison() &&
                client.endpoint.pipeline_epoch_for_test() == 0,
            "T-C: the dispatch gap poisoned the route (it must not)");
    require(tx3_observed_active.load() || tx3_observed_gap.load(),
            "T-C: TX3's dispatch-order observation never fired");
    std::fprintf(stderr,
                 "p50_endpoint_test: T-C dispatch order: %s\n",
                 tx3_observed_gap.load() ? "gap (TX2 unpromoted)"
                                         : "promoted-first (TX2 active)");
    // F applied all three in route order: the final route input is TX3's.
    require(copy_input(server, guids.c) == tx3_input,
            "T-C: F did not apply the three TUs in order (final input "
            "differs)");
}

// ===== K=2 integration: real C <-> real F, no fake (P50_ROUTE_PIPELINING.md) =====
//
// The four tests below drive the merged step-4 (C-side window) and step-5
// (F-side park/release sequencer) halves against each other through the
// REAL P50ServerEndpoint, which now parks a successor TX_BEGIN that arrives
// ahead of the route cursor and releases it exactly at the predecessor's
// commit (release_parked_at). No raw-socket fake is involved anywhere.
//
// The driver mirrors production (client/p50_zstd_sender.cpp + step 4c):
// the head's EndpointIoControl::write_complete only POSTS the successor
// transfer onto the io_context -- never admits, prepares, or runs anything
// inline (the write_complete contract in cache/p50_endpoint.h).

namespace {

// One K=2 pair against a real P50ServerEndpoint: TU `head_index` runs as
// the head dialogue, TU `head_index + 1` is posted from the head's
// write_complete and runs as the pipelined successor on its own real
// connection. Both must commit. `observe_pipelining` records whether the
// successor was actually dispatched while the head was still outstanding
// (window_size() >= 2 at the successor's first outbound message).
struct K2RealPairResult {
    ClientRunResult head;
    ClientRunResult successor;
    bool successor_dispatched_while_head_outstanding = false;
    uint64_t c_to_f_bytes = 0;
};

K2RealPairResult k2_real_pair(TestClient& client,
                              P50ServerEndpoint& server,
                              const std::vector<uint8_t>& head_input,
                              const std::vector<uint8_t>& successor_input) {
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    const tcp::endpoint remote = acceptor.local_endpoint();
    std::future<ServerRunResult> head_server = asio::co_spawn(
        context, server.accept_one(acceptor), asio::use_future);
    std::future<ServerRunResult> successor_server = asio::co_spawn(
        context, server.accept_one(acceptor), asio::use_future);

    const PreparedTuHandle head_handle = admit(client, head_input);
    std::atomic<bool> dispatched{false};
    std::atomic<bool> pipelining_observed{false};
    std::atomic<uint64_t> c_to_f_bytes{0};
    std::optional<std::future<ClientRunResult>> successor_result;

    EndpointIoControl head_control;
    head_control.write_complete = [&]() {
        // Post, never inline: production releases the route transfer lock
        // here and TX n+1's transfer runs in its own coroutine.
        if (dispatched.exchange(true))
            return;
        asio::post(context, [&]() {
            const PreparedTuHandle successor_handle = admit(client, successor_input);
            EndpointIoControl successor_control;
            successor_control.outbound_message_observer =
                [&](ActorSide actor, const Message& message) {
                    if (actor != ActorSide::C)
                        return;
                    // The successor is writing while the head is still
                    // outstanding: the pipeline window holds both.
                    if (client.endpoint.pipeline_window_size_for_test() >= 2)
                        pipelining_observed.store(true);
                    const std::vector<uint8_t> frame = encode_frame(message);
                    c_to_f_bytes.fetch_add(frame.size());
                };
            successor_result.emplace(asio::co_spawn(
                context,
                client.endpoint.run(remote, successor_handle, successor_control),
                asio::use_future));
        });
    };
    head_control.outbound_message_observer =
        [&](ActorSide actor, const Message& message) {
            if (actor != ActorSide::C)
                return;
            const std::vector<uint8_t> frame = encode_frame(message);
            c_to_f_bytes.fetch_add(frame.size());
        };
    std::future<ClientRunResult> head_result = asio::co_spawn(
        context, client.endpoint.run(remote, head_handle, head_control),
                asio::use_future);
    context.run();
    K2RealPairResult out;
    out.head = head_result.get();
    require(successor_result.has_value(),
            "K2 real pair: write_complete never posted the successor");
    out.successor = successor_result->get();
    out.successor_dispatched_while_head_outstanding = pipelining_observed.load();
    out.c_to_f_bytes = c_to_f_bytes.load();
    (void)head_server;
    (void)successor_server;
    return out;
}

std::vector<std::vector<uint8_t>> k2_real_tu_inputs(size_t count, uint8_t seed) {
    // Mixed shared/novel lines: every TU repeats the same prologue lines
    // (shared) plus one unique novel line, so P29V1 interning sees both
    // hit and novel regions across the run.
    std::vector<std::vector<uint8_t>> inputs;
    inputs.reserve(count);
    for (size_t index = 0; index != count; ++index) {
        std::string text = "# 1 \"/tmp/k2-real.cc\"\nshared-alpha\nshared-beta\nshared-gamma\n";
        text += "novel-" + std::to_string(static_cast<unsigned>(seed)) + "-" +
                std::to_string(index) + "\n";
        inputs.push_back(bytes(text));
    }
    return inputs;
}

void require_real_pair_committed(const K2RealPairResult& pair, size_t head_index,
                                 const std::vector<uint8_t>& head_input,
                                 const std::vector<uint8_t>& successor_input,
                                 P50ServerEndpoint& server, CStoreGuid c_guid) {
    const std::string prefix =
        "K2 real pair " + std::to_string(head_index) + ": ";
    require(pair.head.status == ClientRunStatus::Committed &&
                pair.successor.status == ClientRunStatus::Committed,
            prefix + "head/successor did not both commit (head=" +
                std::to_string(static_cast<uint8_t>(pair.head.status)) +
                " successor=" +
                std::to_string(static_cast<uint8_t>(pair.successor.status)) +
                ")");
    require(copy_input(server, c_guid) == successor_input,
            prefix + "F's last committed input is not the successor TU");
    (void)head_input;
}

}  // namespace

// (a) Straight line: 30 TUs on one route at K=2, mixed shared/novel lines.
// Every TU commits, F's route cursor advances 0..29 in order, pipelining
// observably HAPPENED (a successor dispatched while its head was
// outstanding), and the C->F wire byte total stays within +-5% of the same
// 30 TUs driven sequentially at K=1.
void test_p29v1_k2_real_straight_line_30tu() {
    const P5coStoreGuids guids = p5co_store_guids(250);
    EndpointCaps caps;
    caps.profile = ProfileId::P29V1;
    caps.zstd.max_raw_bytes = 4U << 20;
    caps.zstd.max_encoded_body_bytes = 4U << 20;
    PreparationAuthorityLimits limits;
    limits.route_pipeline_depth = 2;
    limits.max_interner_reserved_bytes = UINT64_C(1044398080);

    const std::vector<std::vector<uint8_t>> inputs = k2_real_tu_inputs(30, 7);

    ActionTrace actions;
    P50ServerEndpoint server(guids.f, caps, nullptr, &actions);
    TestClient client(guids.c, caps, HistoryNonce{1}, nullptr, &actions, limits);

    size_t pipelined_pairs = 0;
    uint64_t k2_wire_bytes = 0;
    for (size_t pair_index = 0; pair_index != 15; ++pair_index) {
        const K2RealPairResult pair =
            k2_real_pair(client, server, inputs[2 * pair_index],
                         inputs[2 * pair_index + 1]);
        require_real_pair_committed(pair, pair_index, inputs[2 * pair_index],
                                   inputs[2 * pair_index + 1], server,
                                   guids.c);
        require(pair.successor.committed_commit.has_value() &&
                    pair.head.committed_commit.has_value(),
                "K2 real pair: commit witnesses missing");
        require(pair.successor.committed_commit->rel_seq.value ==
                    pair.head.committed_commit->rel_seq.value + 1,
                "K2 real pair: successor rel_seq was not head+1");
        pipelined_pairs += pair.successor_dispatched_while_head_outstanding ? 1 : 0;
        k2_wire_bytes += pair.c_to_f_bytes;
    }
    require(client.endpoint.next_rel_seq().value == 30,
            "K2 real straight line: confirmed cursor did not advance past "
            "all 30 TUs");
    require(pipelined_pairs != 0,
            "K2 real straight line: pipelining never observably happened "
            "(no successor was dispatched while a head was outstanding)");
    require(!client.endpoint.has_pipeline_poison(),
            "K2 real straight line: route was poisoned on the happy path");

    // F's route cursor must have advanced strictly 0..29 in order.
    std::vector<uint64_t> f_begin_rel_seq;
    for (const ActionRecord& record : actions.records()) {
        if (record.actor == ActorSide::F && record.c_store_guid == guids.c &&
            (record.action == ActionType::TX_BEGIN ||
             record.action == ActionType::ACTIVE_REPLAYED))
            f_begin_rel_seq.push_back(record.rel_seq.value);
    }
    std::vector<uint64_t> expected;
    for (uint64_t index = 0; index != 30; ++index)
        expected.push_back(index);
    require(f_begin_rel_seq == expected,
            "K2 real straight line: F route cursor did not advance 0..29 "
            "in order");

    // Wire-budget comparison: the same 30 TUs at K=1.
    ActionTrace k1_actions;
    P50ServerEndpoint k1_server(guids.f, caps, nullptr, &k1_actions);
    TestClient k1_client(guids.c, caps, HistoryNonce{1}, nullptr, &k1_actions);
    uint64_t k1_wire_bytes = 0;
    for (const std::vector<uint8_t>& input : inputs) {
        EndpointIoControl control;
        control.outbound_message_observer =
            [&](ActorSide actor, const Message& message) {
                if (actor != ActorSide::C)
                    return;
                const std::vector<uint8_t> frame = encode_frame(message);
                k1_wire_bytes += frame.size();
            };
        const PairResult sequential =
            run_pair(k1_client, k1_server, admit(k1_client, input), control);
        require(sequential.client.status == ClientRunStatus::Committed &&
                    copy_input(k1_server, guids.c) == input,
                "K1 wire-budget leg: TU did not commit exact bytes");
    }
    require(k1_wire_bytes != 0, "K1 wire-budget leg measured no bytes");
    const double drift =
        100.0 * static_cast<double>(k2_wire_bytes < k1_wire_bytes
                                        ? k1_wire_bytes - k2_wire_bytes
                                        : k2_wire_bytes - k1_wire_bytes) /
        static_cast<double>(k1_wire_bytes);
    require(drift <= 5.0,
            "K2 real straight line: C->F wire bytes drifted more than 5% "
            "vs the same TUs at K=1 (k2=" + std::to_string(k2_wire_bytes) +
            " k1=" + std::to_string(k1_wire_bytes) + " drift=" +
            std::to_string(drift) + "%)");
}

// (b) Successor socket closed by C while parked on F (poison): F must not
// touch route state; the next TX after the route reset commits.
void test_p29v1_k2_real_successor_closed_while_parked() {
    const P5coStoreGuids guids = p5co_store_guids(251);
    EndpointCaps caps;
    caps.profile = ProfileId::P29V1;
    caps.zstd.max_raw_bytes = 4U << 20;
    caps.zstd.max_encoded_body_bytes = 4U << 20;
    PreparationAuthorityLimits limits;
    limits.route_pipeline_depth = 2;
    limits.max_interner_reserved_bytes = UINT64_C(1044398080);
    TestClient client(guids.c, caps, HistoryNonce{1}, nullptr, nullptr, limits);
    P50ServerEndpoint server(guids.f, caps);

    // Hold the head's materialization on F's codec worker so the successor
    // parks (F defers stage()+STATE until the predecessor commits).
    std::promise<void> release_materialize;
    std::future<void> materialize_gate = release_materialize.get_future();
    EndpointIoControl head_server_control;
    head_server_control.before_materialize_on_worker = [&materialize_gate]() {
        materialize_gate.wait();
    };

    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    const tcp::endpoint remote = acceptor.local_endpoint();
    std::future<ServerRunResult> head_server = asio::co_spawn(
        context, server.accept_one(acceptor, head_server_control),
        asio::use_future);
    std::future<ServerRunResult> successor_server = asio::co_spawn(
        context, server.accept_one(acceptor), asio::use_future);

    const std::vector<uint8_t> head_input =
        bytes("# 1 \"/tmp/k2-real-b.cc\"\nshared-head\nhead-line\n");
    const std::vector<uint8_t> successor_input =
        bytes("# 1 \"/tmp/k2-real-b.cc\"\nshared-head\nsuccessor-line\n");
    const PreparedTuHandle head_handle = admit(client, head_input);

    std::atomic<bool> dispatched{false};
    std::optional<std::future<ClientRunResult>> successor_result;
    std::optional<PreparedTuHandle> successor_handle;
    EndpointIoControl head_control;
    head_control.write_complete = [&]() {
        if (dispatched.exchange(true))
            return;
        asio::post(context, [&]() {
            successor_handle = admit(client, successor_input);
            // Short deadline: while parked on F (no STATE until the
            // predecessor commits), this deadline closes C's successor
            // socket -- the C-side io death this test drives.
            successor_result.emplace(asio::co_spawn(
                context,
                client.endpoint.run(remote, *successor_handle,
                                    EndpointIoControl{},
                                    std::chrono::steady_clock::now() +
                                        std::chrono::milliseconds(400)),
                asio::use_future));
        });
    };
    std::future<ClientRunResult> head_result = asio::co_spawn(
        context, client.endpoint.run(remote, head_handle, head_control),
                asio::use_future);
    // Release F's materialization once the successor has died, from inside
    // the io_context so the head then commits normally.
    std::future<void> successor_watcher = asio::co_spawn(
        context,
        [&]() -> asio::awaitable<void> {
            // Poll (level-triggered) until the successor run finished.
            asio::steady_timer timer(co_await asio::this_coro::executor);
            while (!successor_result.has_value() ||
                   successor_result->wait_for(std::chrono::seconds(0)) !=
                       std::future_status::ready) {
                timer.expires_after(std::chrono::milliseconds(5));
                boost::system::error_code ignored;
                co_await timer.async_wait(
                    asio::redirect_error(asio::use_awaitable, ignored));
            }
            release_materialize.set_value();
            co_return;
        },
        asio::use_future);
    context.run();

    const ClientRunResult head = head_result.get();
    const ClientRunResult successor = successor_result->get();
    (void)successor_watcher;
    (void)head_server;
    (void)successor_server;
    require(head.status == ClientRunStatus::Committed,
            "K2 real parked-close: the healthy head did not commit while "
            "its parked successor was closed");
    require(successor.status != ClientRunStatus::Committed,
            "K2 real parked-close: the closed successor committed");
    require(client.endpoint.has_pipeline_poison(),
            "K2 real parked-close: route was not poisoned by the "
            "successor-side io death");
    // F never applied the successor: the last committed input is still the
    // head's.
    require(copy_input(server, guids.c) == head_input,
            "K2 real parked-close: F applied route state for the closed "
            "successor (route state must be untouched)");
    // Bridge teardown, exactly the production order: retire the
    // successor's own uncommitted route state (abandon_route clears the
    // CRoute serializer queue and the authority's successor deque), then
    // the cold endpoint reset.
    require(successor_handle.has_value(),
            "K2 real parked-close: successor handle missing");
    client.authority->abandon_route(*successor_handle);
    client.endpoint.reset_after_pipeline_poison();
    require(!client.endpoint.has_pipeline_poison() &&
                client.endpoint.pipeline_epoch_for_test() == 1 &&
                client.endpoint.next_rel_seq().value == 0,
            "K2 real parked-close: cold reset did not land");

    // The next TX after the reset commits through the cold dance.
    const std::vector<uint8_t> next_input =
        bytes("# 1 \"/tmp/k2-real-b.cc\"\nshared-head\npost-reset-line\n");
    const PairResult next_pair =
        run_pair(client, server, admit(client, next_input));
    require(next_pair.client.status == ClientRunStatus::Committed &&
                next_pair.server.status == ServerRunStatus::Completed &&
                copy_input(server, guids.c) == next_input,
            "K2 real parked-close: the next TX after the route reset did "
            "not commit");
}

// (c) Head connection killed after its FILL write while the successor is
// parked: route poison, cold reset, recovery, next TX commits.
void test_p29v1_k2_real_head_killed_after_fill() {
    const P5coStoreGuids guids = p5co_store_guids(252);
    EndpointCaps caps;
    caps.profile = ProfileId::P29V1;
    caps.zstd.max_raw_bytes = 4U << 20;
    caps.zstd.max_encoded_body_bytes = 4U << 20;
    PreparationAuthorityLimits limits;
    limits.route_pipeline_depth = 2;
    limits.max_interner_reserved_bytes = UINT64_C(1044398080);
    TestClient client(guids.c, caps, HistoryNonce{1}, nullptr, nullptr, limits);
    P50ServerEndpoint server(guids.f, caps);

    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    const tcp::endpoint remote = acceptor.local_endpoint();
    std::future<ServerRunResult> head_server = asio::co_spawn(
        context, server.accept_one(acceptor), asio::use_future);
    std::future<ServerRunResult> successor_server = asio::co_spawn(
        context, server.accept_one(acceptor), asio::use_future);

    const std::vector<uint8_t> head_input =
        bytes("# 1 \"/tmp/k2-real-c.cc\"\nshared-c-head\nhead-line\n");
    const std::vector<uint8_t> successor_input =
        bytes("# 1 \"/tmp/k2-real-c.cc\"\nshared-c-head\nsuccessor-line\n");
    const PreparedTuHandle head_handle = admit(client, head_input);

    std::atomic<bool> dispatched{false};
    std::atomic<bool> killed{false};
    std::optional<std::future<ClientRunResult>> successor_result;
    std::optional<PreparedTuHandle> successor_handle;
    EndpointIoControl head_control;
    head_control.write_complete = [&]() {
        if (dispatched.exchange(true))
            return;
        asio::post(context, [&]() {
            successor_handle = admit(client, successor_input);
            successor_result.emplace(asio::co_spawn(
                context,
                client.endpoint.run(remote, *successor_handle,
                                    EndpointIoControl{}),
                asio::use_future));
            // Kill the head's own connection right after its FILL write:
            // the successor is now outstanding (parked on F).
            killed.store(true);
            client.endpoint.request_cancel_for_test();
        });
    };
    std::future<ClientRunResult> head_result = asio::co_spawn(
        context, client.endpoint.run(remote, head_handle, head_control),
                asio::use_future);
    context.run();
    require(killed.load(), "K2 real head-kill: the kill never fired");

    const ClientRunResult head = head_result.get();
    const ClientRunResult successor = successor_result->get();
    (void)head_server;
    (void)successor_server;
    require(head.status == ClientRunStatus::Disconnected,
            "K2 real head-kill: the killed head did not surface its "
            "disconnect");
    require(successor.status != ClientRunStatus::Committed,
            "K2 real head-kill: the parked successor committed despite the "
            "head's death");
    require(client.endpoint.has_pipeline_poison(),
            "K2 real head-kill: route was not poisoned by the head death "
            "with a successor outstanding");
    require(successor_handle.has_value(),
            "K2 real head-kill: successor handle missing");
    client.authority->abandon_route(*successor_handle);
    client.endpoint.reset_after_pipeline_poison();
    require(!client.endpoint.has_pipeline_poison() &&
                client.endpoint.pipeline_epoch_for_test() == 1,
            "K2 real head-kill: cold reset did not land");

    // Recovery: the next TX commits through the cold HISTORY_RESET path.
    const std::vector<uint8_t> next_input =
        bytes("# 1 \"/tmp/k2-real-c.cc\"\nshared-c-head\nrecovered-line\n");
    const PairResult next_pair =
        run_pair(client, server, admit(client, next_input));
    require(next_pair.client.status == ClientRunStatus::Committed &&
                copy_input(server, guids.c) == next_input,
            "K2 real head-kill: the next TX after the cold reset did not "
            "commit");
}

// (d) A K=2 run whose TUs are large enough that fill_before_need is
// impossible for some TX (the nullopt fallback path). All commit.
void test_p29v1_k2_real_no_eager_fill_all_commit() {
    require(::setenv("ICECC_P50_EARLY_NEED_REGION_BOUND", "0", 1) == 0,
            "K2 real no-eager-fill: could not lower the early-FILL bound");
    const struct BoundRestore {
        ~BoundRestore() { ::unsetenv("ICECC_P50_EARLY_NEED_REGION_BOUND"); }
    } bound_restore;

    const P5coStoreGuids guids = p5co_store_guids(253);
    EndpointCaps caps;
    caps.profile = ProfileId::P29V1;
    caps.zstd.max_raw_bytes = 4U << 20;
    caps.zstd.max_encoded_body_bytes = 4U << 20;
    PreparationAuthorityLimits limits;
    limits.route_pipeline_depth = 2;
    limits.max_interner_reserved_bytes = UINT64_C(1044398080);
    TestClient client(guids.c, caps, HistoryNonce{1}, nullptr, nullptr, limits);
    P50ServerEndpoint server(guids.f, caps);

    for (int pair_index = 0; pair_index != 3; ++pair_index) {
        const std::vector<uint8_t> head_input =
            bytes("# 1 \"/tmp/k2-real-d.cc\"\nshared-d\nhead-" +
                  std::to_string(pair_index) + "\n");
        const std::vector<uint8_t> successor_input =
            bytes("# 1 \"/tmp/k2-real-d.cc\"\nshared-d\nsuccessor-" +
                  std::to_string(pair_index) + "\n");
        const K2RealPairResult pair =
            k2_real_pair(client, server, head_input, successor_input);
        require_real_pair_committed(pair, static_cast<size_t>(pair_index),
                                    head_input, successor_input, server,
                                    guids.c);
        require(!client.endpoint.has_pipeline_poison(),
                "K2 real no-eager-fill: fallback poisoned the route");
        require(client.endpoint.pipeline_epoch_for_test() == 0,
                "K2 real no-eager-fill: fallback bumped the pipeline epoch");
    }
}

// ===== same_commit negative coverage: lost-final reconnect path =====
// Ruling part 2: after a lost final commit, a fake F presenting a well-formed
// SessionState.last_commit differing from the active TxBegin in any identity field must be
// refused (no LostFinalAcknowledgement), must leave the active transaction in place, and an
// exact subsequent reconciliation must commit.  The presented peer state is kept
// self-consistent (state_digest == last_commit.post_state_digest, next_rel_seq == begin+1,
// route nonce echoed) so only the same_commit comparison against the live begin can object.

asio::awaitable<void> raw_f_receive_then_drop(tcp::acceptor& acceptor, FStoreGuid f_guid,
                                              TxBegin& out_begin) {
    tcp::socket socket = co_await acceptor.async_accept(asio::use_awaitable);
    const SessionHello hello =
        raw_decode<SessionHello>(co_await raw_read(socket, kInitialMaxFramePayload));
    const SessionSelection selection =
        negotiate_session(hello, kP50WireRevision, kKnownProfileMask);
    const uint32_t cap = selection.limits.max_frame_payload;
    SessionState fresh;
    fresh.wire_revision = selection.wire_revision;
    fresh.negotiated_profiles = selection.negotiated_profiles;
    fresh.limits = selection.limits;
    fresh.f_store_guid = f_guid;
    co_await raw_write(socket, Message{fresh});
    Frame next = co_await raw_read(socket, cap);
    if (next.type == MessageType::HISTORY_RESET) {
        const HistoryReset reset = raw_decode<HistoryReset>(next);
        SessionState ack = fresh;
        ack.namespace_present = true;
        ack.route_present = true;
        ack.history_nonce = reset.history_nonce;
        ack.next_rel_seq = RelSeq{0};
        ack.state_digest = reset.initial_state_digest;
        co_await raw_write(socket, Message{ack});
        next = co_await raw_read(socket, cap);
    }
    out_begin = raw_decode<TxBegin>(next);
    uint64_t body_bytes = 0;
    while (body_bytes < out_begin.body.encoded_bytes) {
        const BodyMessage body = raw_decode<BodyMessage>(co_await raw_read(socket, cap));
        if (body.bytes.empty())
            break;
        body_bytes += body.bytes.size();
    }
    // Lost final: the F "commits" but the acknowledgement never reaches C.
    boost::system::error_code ignored;
    socket.shutdown(tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
    co_return;
}

asio::awaitable<void> raw_f_present_last_commit(tcp::acceptor& acceptor, FStoreGuid f_guid,
                                                TxBegin begin, CommitMismatchField field) {
    tcp::socket socket = co_await acceptor.async_accept(asio::use_awaitable);
    const SessionHello hello =
        raw_decode<SessionHello>(co_await raw_read(socket, kInitialMaxFramePayload));
    const SessionSelection selection =
        negotiate_session(hello, kP50WireRevision, kKnownProfileMask);
    SessionState peer;
    peer.wire_revision = selection.wire_revision;
    peer.negotiated_profiles = selection.negotiated_profiles;
    peer.limits = selection.limits;
    peer.f_store_guid = f_guid;
    peer.namespace_present = true;
    peer.route_present = true;
    TxCommit last = correct_commit_for(begin);
    mutate_commit(last, field);
    // Wire rule: a retained commit must be internally consistent with its route
    // (nonce equal, next_rel = commit.rel + 1, state digest = commit post-state), so the
    // presented state derives from the (possibly mutated) commit.  Nonce/REL_SEQ mismatches
    // are then refused by the client's route checks; TU_SEQ and the digests are decided by
    // same_commit alone.
    peer.history_nonce = last.history_nonce;
    peer.next_rel_seq = RelSeq{last.rel_seq.value + 1};
    peer.state_digest = last.post_state_digest;
    peer.last_commit = last;
    co_await raw_write(socket, Message{peer});
    // Acceptance closes silently; refusal first sends a terminal ERROR frame.  Consume
    // whatever arrives until the peer closes.
    try {
        for (;;)
            (void)co_await raw_read(socket, peer.limits.max_frame_payload);
    } catch (const boost::system::system_error&) {
        // peer closed
    }
    co_return;
}

void test_lost_final_commit_identity_negative_matrix() {
    const std::array cases{
        std::pair{CommitMismatchField::HistoryNonce, "history nonce"},
        std::pair{CommitMismatchField::RelSeq, "REL_SEQ"},
        std::pair{CommitMismatchField::TuSeq, "TU_SEQ"},
        std::pair{CommitMismatchField::TransactionDigest, "transaction digest"},
        std::pair{CommitMismatchField::RawDigest, "raw digest"},
        std::pair{CommitMismatchField::PostStateDigest, "post-state digest"},
    };
    uint64_t identity = 7100;

    // Control: the exact last_commit closes the lost-final window.
    {
        TestClient client(Id128::from_u64(identity++));
        const FStoreGuid f_guid = Id128::from_u64(identity++);
        const std::vector<uint8_t> input = pseudo_random_bytes(4096);
        const PreparedTuHandle prepared = admit(client, input);
        TxBegin begin;
        {
            asio::io_context context;
            tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
            std::future<void> fake = asio::co_spawn(
                context, raw_f_receive_then_drop(acceptor, f_guid, begin), asio::use_future);
            std::future<ClientRunResult> run = asio::co_spawn(
                context, client.endpoint.run(acceptor.local_endpoint(), prepared, {}),
                asio::use_future);
            context.run();
            fake.get();
            const ClientRunResult dropped = run.get();
            require(dropped.status == ClientRunStatus::Disconnected &&
                        client.has_active_transaction(),
                    "lost-final setup did not leave an active transaction");
        }
        asio::io_context context;
        tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
        std::future<void> fake = asio::co_spawn(
            context,
            raw_f_present_last_commit(acceptor, f_guid, begin, CommitMismatchField::None),
            asio::use_future);
        std::future<ClientRunResult> run = asio::co_spawn(
            context, client.endpoint.run(acceptor.local_endpoint(), {}, {}), asio::use_future);
        context.run();
        fake.get();
        const ClientRunResult accepted = run.get();
        require(accepted.status == ClientRunStatus::Committed &&
                    accepted.reconnect == EndpointReconnectOutcome::LostFinalAcknowledgement &&
                    !client.has_active_transaction(),
                "exact lost-final acknowledgement was not accepted");
    }

    for (const auto& [field, name] : cases) {
        TestClient client(Id128::from_u64(identity++));
        const FStoreGuid f_guid = Id128::from_u64(identity++);
        const std::vector<uint8_t> input = pseudo_random_bytes(4096);
        const PreparedTuHandle prepared = admit(client, input);
        TxBegin begin;
        {
            asio::io_context context;
            tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
            std::future<void> fake = asio::co_spawn(
                context, raw_f_receive_then_drop(acceptor, f_guid, begin), asio::use_future);
            std::future<ClientRunResult> run = asio::co_spawn(
                context, client.endpoint.run(acceptor.local_endpoint(), prepared, {}),
                asio::use_future);
            context.run();
            fake.get();
            require(run.get().status == ClientRunStatus::Disconnected &&
                        client.has_active_transaction(),
                    std::string(name) + " lost-final setup did not retain the transaction");
        }
        {
            const std::optional<FStoreGuid> f_before = client.endpoint.f_store_guid();
            asio::io_context context;
            tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
            std::future<void> fake = asio::co_spawn(
                context, raw_f_present_last_commit(acceptor, f_guid, begin, field),
                asio::use_future);
            std::future<ClientRunResult> run = asio::co_spawn(
                context, client.endpoint.run(acceptor.local_endpoint(), {}, {}),
                asio::use_future);
            context.run();
            fake.get();
            const ClientRunResult refused = run.get();
            // RouteHistoryReset is the SITE's refusal shape (:1508); a bypassed site would
            // fall through to accept()'s internal check, which throws with the default
            // reconnect outcome instead -- so this assertion proves the call site itself.
            // Cursor preservation: the refusal must leave rel/state/route untouched.
            require(refused.status == ClientRunStatus::TerminalError &&
                        refused.reconnect == EndpointReconnectOutcome::RouteHistoryReset &&
                        client.has_active_transaction() &&
                        client.endpoint.next_rel_seq() == begin.rel_seq &&
                        client.endpoint.state_digest() == begin.pre_state_digest &&
                        client.endpoint.f_store_guid() == f_before,
                    std::string(name) +
                        " mismatched last_commit refusal did not preserve the cursor");
        }
        {
            asio::io_context context;
            tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
            std::future<void> fake = asio::co_spawn(
                context,
                raw_f_present_last_commit(acceptor, f_guid, begin, CommitMismatchField::None),
                asio::use_future);
            std::future<ClientRunResult> run = asio::co_spawn(
                context, client.endpoint.run(acceptor.local_endpoint(), {}, {}),
                asio::use_future);
            context.run();
            fake.get();
            const ClientRunResult reconciled = run.get();
            require(reconciled.status == ClientRunStatus::Committed &&
                        reconciled.reconnect ==
                            EndpointReconnectOutcome::LostFinalAcknowledgement &&
                        !client.has_active_transaction(),
                    std::string(name) + " refusal blocked the exact reconciliation");
        }
    }
}

// Write-ahead batch for the step-5d reset-before-begin test: HELLO, then a
// cold HISTORY_RESET, then a TX_BEGIN whose rel_seq is ahead of the PRE-reset
// cursor -- all before this client reads anything from F, so the whole batch
// is already in the socket's receive buffer at F's step-5b peek.
asio::awaitable<void> raw_reset_then_ahead_begin(tcp::endpoint remote, CStoreGuid c_guid,
                                                 const HistoryReset& reset,
                                                 const ZstdTuEnvelope& tu) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    co_await socket.async_connect(remote, asio::use_awaitable);
    SessionHello hello;
    hello.c_store_guid = c_guid;
    hello.supported_profiles = profile_bit(ProfileId::ZSTD_TU);
    hello.limits = {};
    co_await raw_write(socket, hello);
    co_await raw_write(socket, reset);
    co_await raw_write(socket, tu.begin);
    Message body_message = BodyMessage{tu.body};
    co_await raw_write(socket, std::move(body_message));
    // First SESSION_STATE reflects the pre-reset route (the reset has not
    // been processed yet); the reset acknowledgement is the second one.
    const SessionState pre_reset = raw_decode<SessionState>(
        co_await raw_read(socket, hello.limits.max_frame_payload));
    require(pre_reset.route_present,
            "reset-before-begin fixture did not reach an established route");
    const SessionState reset_ack = raw_decode<SessionState>(
        co_await raw_read(socket, hello.limits.max_frame_payload));
    require(reset_ack.route_present && reset_ack.history_nonce == reset.history_nonce &&
                reset_ack.next_rel_seq.value == 0 &&
                reset_ack.state_digest == reset.initial_state_digest,
            "reset-before-begin fixture did not process its cold HISTORY_RESET");
    // The post-reset BEGIN (one ahead of the fresh cursor) is rejected through
    // the ordinary cursor-mismatch path, bounded, never a hang.
    const Frame terminal =
        co_await raw_read(socket, reset_ack.limits.max_frame_payload);
    require(terminal.type == MessageType::ERROR,
            "reset-before-begin fixture did not reject its post-reset BEGIN "
            "through the ordinary cursor path");
    (void)raw_decode<ErrorMessage>(terminal);
    boost::system::error_code ignored;
    socket.close(ignored);
    co_return;
}

// P50 route pipelining (research/designs/P50_ROUTE_PIPELINING.md), step 5d.
//
// F-side unit tests for the step-5c per-route sequencer (the park/release
// admission gate in run_connected). Park/release deliberately has no
// dedicated ActionTrace event (the ActionType set has no PARKED action), so
// both tests assert it indirectly:
//
//  - test_route_pipeline_park_releases_in_commit_order proves a successor
//    TX_BEGIN that arrives ahead of the route cursor is not processed before
//    its predecessor commits -- the successor's SESSION_STATE is only staged
//    after the predecessor's TX_COMMIT advanced the cursor -- and that it then
//    commits exact bytes in route order.
//  - test_route_pipeline_reset_before_begin_does_not_park_on_stale_cursor
//    proves the coordinator's HISTORY_RESET-before-TX_BEGIN rule: a cold
//    epoch change inside one already-buffered batch must never be gated
//    behind the pre-reset cursor. The batch below carries a HISTORY_RESET
//    followed by a TX_BEGIN one rel_seq AHEAD of the pre-reset cursor --
//    exactly the shape the step-5c gate would otherwise park (and then
//    strand forever: the reset that would advance the epoch lives in this
//    same parked connection's own buffer). It must instead take the unchanged
//    dispatch path: process the reset, then evaluate the post-reset BEGIN
//    against the fresh epoch's own cursor (an ordinary bounded rejection,
//    never a hang), leaving the new epoch fully usable afterwards.
//
// Both tests use the bounded-wait idiom (context.run_for + future readiness
// checks) instead of a plain blocking context.run(): ordinary accept_one
// sessions have no internal watchdog (EndpointCaps has no deadline field), so
// a park/release regression would otherwise hang the whole test binary
// instead of failing it. A failed require() exits the process directly, so no
// io_context teardown is needed on that path.
struct ParkReleaseRace {
    bool successor_batch_written = false;
};

asio::awaitable<void> raw_pipelined_successor(tcp::endpoint remote, CStoreGuid c_guid,
                                              const ZstdTuEnvelope& successor_tu,
                                              HistoryNonce nonce,
                                              ParkReleaseRace& race) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    co_await socket.async_connect(remote, asio::use_awaitable);
    SessionHello hello;
    hello.c_store_guid = c_guid;
    hello.supported_profiles = profile_bit(ProfileId::ZSTD_TU);
    hello.limits = {};
    co_await raw_write(socket, hello);
    // Write-ahead: BEGIN and BODY land in the socket's receive buffer before
    // this client ever reads SESSION_STATE, so F's step-5b MSG_PEEK (taken
    // immediately before its SESSION_STATE write) observes the successor
    // rel_seq and the step-5c gate parks this session instead of processing
    // it against a cursor the predecessor has not advanced yet.
    co_await raw_write(socket, successor_tu.begin);
    Message body_message = BodyMessage{successor_tu.body};
    co_await raw_write(socket, std::move(body_message));
    race.successor_batch_written = true;
    const SessionState state = raw_decode<SessionState>(
        co_await raw_read(socket, hello.limits.max_frame_payload));
    // Indirect parking proof: this session's SESSION_STATE is only staged
    // after the predecessor's commit released it, so the route cursor F
    // advertises here has already advanced past the predecessor. A
    // regression that processes the write-ahead BEGIN immediately would
    // advertise the pre-predecessor cursor here instead.
    require(state.route_present && state.history_nonce == nonce &&
                state.next_rel_seq.value == 1,
            "parked successor observed a route cursor that never advanced");
    const TxCommit commit = raw_decode<TxCommit>(
        co_await raw_read(socket, state.limits.max_frame_payload));
    require(commit.transaction_digest == successor_tu.begin.transaction_digest &&
                commit.raw_digest == successor_tu.begin.raw_digest,
            "parked successor received a different TX_COMMIT");
    boost::system::error_code ignored;
    socket.close(ignored);
    co_return;
}

asio::awaitable<void> raw_pipelined_predecessor(tcp::endpoint remote, CStoreGuid c_guid,
                                                const ZstdTuEnvelope& predecessor_tu,
                                                HistoryNonce nonce,
                                                ParkReleaseRace& race) {
    const auto executor = co_await asio::this_coro::executor;
    asio::steady_timer timer(executor);
    while (!race.successor_batch_written) {
        timer.expires_after(std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
    }
    // Deterministic ordering margin: the successor's write-ahead batch is
    // already in its socket's receive buffer, and this delay lets F accept and
    // peek that connection (and park it) before the predecessor connects.
    timer.expires_after(std::chrono::milliseconds(50));
    co_await timer.async_wait(asio::use_awaitable);
    tcp::socket socket(executor);
    const RawRoute open = co_await raw_open(socket, remote, c_guid);
    require(open.state.route_present && open.state.history_nonce == nonce &&
                open.state.next_rel_seq.value == 0,
            "predecessor observed a route cursor its successor consumed");
    co_await raw_write(socket, predecessor_tu.begin);
    Message body_message = BodyMessage{predecessor_tu.body};
    co_await raw_write(socket, std::move(body_message));
    const TxCommit commit = raw_decode<TxCommit>(
        co_await raw_read(socket, open.state.limits.max_frame_payload));
    require(commit.transaction_digest == predecessor_tu.begin.transaction_digest &&
                commit.raw_digest == predecessor_tu.begin.raw_digest,
            "predecessor received a different TX_COMMIT");
    boost::system::error_code ignored;
    socket.close(ignored);
    co_return;
}

// One ordinary committed TU over a raw socket: HELLO + SESSION_STATE round
// trip, then BEGIN/BODY at the route's current cursor, then TX_COMMIT. Used
// by the step-5d reset-before-begin test for its post-rejection recovery leg
// (proving the fresh epoch is fully usable through the unchanged path).
asio::awaitable<void> raw_commit_one_tu(tcp::endpoint remote, CStoreGuid c_guid,
                                        const ZstdTuEnvelope& tu, HistoryNonce nonce,
                                        RelSeq rel) {
    const auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    const RawRoute open = co_await raw_open(socket, remote, c_guid);
    require(open.state.route_present && open.state.history_nonce == nonce &&
                open.state.next_rel_seq == rel &&
                open.state.state_digest == tu.begin.pre_state_digest,
            "recovery TU did not meet the route cursor it was prepared for");
    co_await raw_write(socket, tu.begin);
    Message body_message = BodyMessage{tu.body};
    co_await raw_write(socket, std::move(body_message));
    const TxCommit commit = raw_decode<TxCommit>(
        co_await raw_read(socket, open.state.limits.max_frame_payload));
    require(commit.transaction_digest == tu.begin.transaction_digest &&
                commit.raw_digest == tu.begin.raw_digest,
            "recovery TU received a different TX_COMMIT");
    boost::system::error_code ignored;
    socket.close(ignored);
    co_return;
}

void test_route_pipeline_park_releases_in_commit_order() {
    const P5coStoreGuids guids = p5co_store_guids(46);
    EndpointCaps caps;
    caps.profile = ProfileId::ZSTD_TU;
    caps.zstd.max_raw_bytes = 1U << 20;
    caps.zstd.max_encoded_body_bytes = 1U << 20;
    ActionTrace actions;
    P50ServerEndpoint server(guids.f, caps, nullptr, &actions);
    const HistoryNonce nonce{461};
    require(force_route_reset(server, guids.c, nonce).status ==
                ServerRunStatus::Disconnected,
            "park/release fixture did not establish its route");
    const Digest128 initial_state = initial_route_digest(guids.c, nonce);
    // The predecessor TU commits at rel_seq 0. encode_zstd_tu produces a
    // complete, ready-to-send TxBegin from the bootstrapped route cursor.
    const ZstdTuEnvelope predecessor_tu =
        encode_zstd_tu(nonce, RelSeq{0}, TuSeq{4610}, initial_state,
                       bytes("park/release ordering: predecessor TU\n"));
    // The successor's required pre_state_digest is derived entirely offline
    // (no round trip to F), from the predecessor's OWN begin fields plus its
    // transaction digest -- the same argument pattern every production
    // commit path uses. That offline-only derivation is the point of route
    // pipelining: C can prepare the successor before the predecessor commits.
    const Digest128 successor_pre_state = compute_post_state_digest(
        predecessor_tu.begin.pre_state_digest, predecessor_tu.begin.history_nonce,
        predecessor_tu.begin.rel_seq, predecessor_tu.begin.tu_seq,
        predecessor_tu.begin.transaction_digest);
    const ZstdTuEnvelope successor_tu =
        encode_zstd_tu(nonce, RelSeq{1}, TuSeq{4611}, successor_pre_state,
                       bytes("park/release ordering: successor TU\n"));
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    const tcp::endpoint remote = acceptor.local_endpoint();
    ParkReleaseRace race;
    // Two concurrent accept_one sessions on one acceptor: whichever raw
    // connection arrives first binds to the first spawned future. The
    // successor connects first (the predecessor waits on the race flag plus
    // 50ms), so the first spawn is the successor's server session.
    std::future<ServerRunResult> successor_server =
        asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
    std::future<ServerRunResult> predecessor_server =
        asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
    std::future<void> successor_client = asio::co_spawn(
        context, raw_pipelined_successor(remote, guids.c, successor_tu, nonce, race),
        asio::use_future);
    std::future<void> predecessor_client = asio::co_spawn(
        context, raw_pipelined_predecessor(remote, guids.c, predecessor_tu, nonce, race),
        asio::use_future);
    context.run_for(std::chrono::seconds(5));
    require(successor_server.wait_for(std::chrono::seconds(0)) ==
                std::future_status::ready &&
                predecessor_server.wait_for(std::chrono::seconds(0)) ==
                    std::future_status::ready &&
                successor_client.wait_for(std::chrono::seconds(0)) ==
                    std::future_status::ready &&
                predecessor_client.wait_for(std::chrono::seconds(0)) ==
                    std::future_status::ready,
            "test_route_pipeline_park_releases_in_commit_order: hung "
            "(park/release regression?)");
    const ServerRunResult successor_result = successor_server.get();
    const ServerRunResult predecessor_result = predecessor_server.get();
    const auto describe_status = [](ServerRunStatus status) {
        switch (status) {
        case ServerRunStatus::Completed: return "Completed";
        case ServerRunStatus::Disconnected: return "Disconnected";
        case ServerRunStatus::TerminalError: return "TerminalError";
        case ServerRunStatus::DeadlineExceeded: return "DeadlineExceeded";
        }
        return "unknown";
    };
    const auto describe_peeked = [](const std::optional<RelSeq>& peeked) {
        return peeked ? std::to_string(peeked->value) : std::string("none");
    };
    require(successor_result.status == ServerRunStatus::Completed &&
                predecessor_result.status == ServerRunStatus::Completed,
            std::string("park/release pair did not complete both sessions: "
                        "successor status=") +
                describe_status(successor_result.status) + " peeked=" +
                describe_peeked(successor_result.f_hello_time_peeked_begin_rel_seq) +
                " f_rel=" + std::to_string(successor_result.f_rel_seq.value) +
                ", predecessor status=" + describe_status(predecessor_result.status) +
                " peeked=" +
                describe_peeked(predecessor_result.f_hello_time_peeked_begin_rel_seq) +
                " f_rel=" + std::to_string(predecessor_result.f_rel_seq.value));
    successor_client.get();
    predecessor_client.get();
    require(successor_result.c_store_guid == guids.c &&
                predecessor_result.c_store_guid == guids.c,
            "park/release pair bound the wrong C store");
    require(predecessor_result.f_rel_seq.value == 0 &&
                successor_result.f_rel_seq.value == 1,
            "park/release pair did not run at its declared rel_seq");
    // Commit-order proof: despite the successor's bytes physically arriving
    // on the wire first, F must record the predecessor's begin and commit
    // before the successor's.
    std::vector<uint64_t> f_begin_order;
    std::vector<uint64_t> f_commit_order;
    for (const ActionRecord& record : actions.records()) {
        if (record.actor != ActorSide::F || record.c_store_guid != guids.c)
            continue;
        if (record.action == ActionType::TX_BEGIN)
            f_begin_order.push_back(record.rel_seq.value);
        else if (record.action == ActionType::INPUT_COMMITTED)
            f_commit_order.push_back(record.rel_seq.value);
    }
    require(f_begin_order == std::vector<uint64_t>{0, 1},
            "park/release pair began its TUs out of route order");
    require(f_commit_order == std::vector<uint64_t>{0, 1},
            "park/release pair committed its TUs out of route order");
    require(copy_input(server, guids.c) ==
                std::optional<std::vector<uint8_t>>(
                    bytes("park/release ordering: successor TU\n")),
            "parked successor did not materialize exact bytes");
    // No require_trace here: this fixture drives raw sockets only, so the
    // ActionTrace has F records and no C records, and check_action_trace's
    // cross-actor model ("F TX_BEGIN names C's active transaction") cannot
    // apply to a raw-only trace. Commit order is asserted explicitly above.
}

void test_route_pipeline_reset_before_begin_does_not_park_on_stale_cursor() {
    const P5coStoreGuids guids = p5co_store_guids(47);
    EndpointCaps caps;
    caps.profile = ProfileId::ZSTD_TU;
    caps.zstd.max_raw_bytes = 1U << 20;
    caps.zstd.max_encoded_body_bytes = 1U << 20;
    ActionTrace actions;
    P50ServerEndpoint server(guids.f, caps, nullptr, &actions);
    // Bootstrap a stale route (a genuinely older epoch) so there IS a
    // pre-reset cursor for the write-ahead batch to be ahead of.
    const HistoryNonce stale_nonce{471};
    require(force_route_reset(server, guids.c, stale_nonce).status ==
                ServerRunStatus::Disconnected,
            "reset-before-begin fixture did not establish its stale route");
    const HistoryNonce fresh_nonce{472};
    const HistoryReset reset{fresh_nonce,
                             initial_route_digest(guids.c, fresh_nonce)};
    // The write-ahead batch: HISTORY_RESET cold-starting a new epoch, then a
    // TX_BEGIN one rel_seq AHEAD of the pre-reset cursor (rel 1 vs cursor 0).
    // This is the exact shape the step-5c admission gate parks -- and, without
    // the HISTORY_RESET-precedes-TX_BEGIN rule, strands forever (the reset
    // that advances the epoch sits in this same parked connection's buffer).
    // With the rule, the unchanged dispatch path runs: reset first, then the
    // BEGIN evaluated against the fresh epoch's own cursor.
    const ZstdTuEnvelope tu =
        encode_zstd_tu(fresh_nonce, RelSeq{1}, TuSeq{4720}, reset.initial_state_digest,
                       bytes("reset-before-begin: cold-epoch TU\n"));
    asio::io_context context;
    tcp::acceptor acceptor(context, {asio::ip::address_v4::loopback(), 0});
    const tcp::endpoint remote = acceptor.local_endpoint();
    std::future<ServerRunResult> server_future =
        asio::co_spawn(context, server.accept_one(acceptor), asio::use_future);
    std::future<void> client_future = asio::co_spawn(
        context, raw_reset_then_ahead_begin(remote, guids.c, reset, tu),
        asio::use_future);
    context.run_for(std::chrono::seconds(5));
    require(server_future.wait_for(std::chrono::seconds(0)) ==
                std::future_status::ready &&
                client_future.wait_for(std::chrono::seconds(0)) ==
                    std::future_status::ready,
            "test_route_pipeline_reset_before_begin_does_not_park_on_stale_"
            "cursor: hung (HISTORY_RESET-before-TX_BEGIN no-park "
            "regression?)");
    client_future.get();
    const ServerRunResult rejected = server_future.get();
    // The no-park path's outcome is the ORDINARY one the pre-existing cursor
    // logic would have produced post-reset: a bounded terminal rejection,
    // never a hang and never a park-then-release.
    require(rejected.status == ServerRunStatus::TerminalError,
            "reset-before-begin batch did not surface its ordinary rejection");
    // The fresh epoch must be fully usable afterwards: an ordinary TU at the
    // fresh cursor commits exact bytes through the unchanged path.
    const std::vector<uint8_t> input =
        bytes("reset-before-begin: first committed TU\n");
    const ZstdTuEnvelope first =
        encode_zstd_tu(fresh_nonce, RelSeq{0}, TuSeq{4721},
                       reset.initial_state_digest, input);
    asio::io_context recovery;
    tcp::acceptor recovery_acceptor(
        recovery, {asio::ip::address_v4::loopback(), 0});
    std::future<ServerRunResult> recovery_server = asio::co_spawn(
        recovery, server.accept_one(recovery_acceptor), asio::use_future);
    std::future<void> recovery_client = asio::co_spawn(
        recovery,
        raw_commit_one_tu(recovery_acceptor.local_endpoint(), guids.c, first,
                          fresh_nonce, RelSeq{0}),
        asio::use_future);
    recovery.run_for(std::chrono::seconds(5));
    require(recovery_server.wait_for(std::chrono::seconds(0)) ==
                std::future_status::ready &&
                recovery_client.wait_for(std::chrono::seconds(0)) ==
                    std::future_status::ready,
            "test_route_pipeline_reset_before_begin_does_not_park_on_stale_"
            "cursor: recovery leg hung");
    const ServerRunResult recovery_result = recovery_server.get();
    recovery_client.get();
    require(recovery_result.status == ServerRunStatus::Completed &&
                recovery_result.f_rel_seq.value == 0,
            "fresh-epoch recovery TU did not commit");
    require(copy_input(server, guids.c) ==
                std::optional<std::vector<uint8_t>>(input),
            "fresh-epoch recovery TU did not materialize exact bytes");
}

int main(int argc, char** argv) {
    const bool performance_gate = argc == 2 && std::string_view(argv[1]) == "--performance";
    if (argc > 2 || (argc == 2 && !performance_gate))
        fail("usage: p50endpoint [--performance]");
    if (std::getenv("ICECC_P50_ENDPOINT_MUTANT_FOCUS") != nullptr) {
        test_client_completion_deadline_is_fresh_before_first_write();
        test_adopted_endpoint_exact_zstd_and_ownership();
        test_adopted_endpoint_disconnect_and_invalid_rows();
        test_adopted_cross_executor_releases_registration();
        test_adopted_codec_runs_off_the_owner_thread();
        test_adopted_codec_slots_release_per_job();
        test_adopted_selector_cancel_fences_publication();
        test_adopted_completion_cancel_is_rechecked();
        std::cout << "p50_endpoint_test: focused adopted-socket PASS\n";
        return 0;
    }
    if (std::getenv("ICECC_P50_P29_DIALOGUE_FOCUS") != nullptr) {
        test_p29v1_endpoint_route_dialogue_lifetime();
        test_p29v1_pipelined_begin();
        test_p29v1_k1_pipeline_wire_identity();
        std::cout << "p50_endpoint_test: focused P29V1 dialogue PASS\n";
        return 0;
    }
    if (std::getenv("ICECC_P50_ROUTE_PIPELINE_FOCUS") != nullptr) {
        test_route_pipeline_k1_identity();
        test_route_pipeline_park_releases_in_commit_order();
        test_route_pipeline_reset_before_begin_does_not_park_on_stale_cursor();
        test_p29v1_k1_pipeline_waiters_never_used();
        test_p29v1_k2_pipeline_straight_line();
        test_p29v1_k2_pipeline_successor_commit_dispatched_first();
        test_p29v1_k2_pipeline_successor_state_mismatch_poisons();
        test_p29v1_k2_pipeline_head_disconnect_tears_down_successor();
        test_p29v1_k2_pipeline_successor_wait_deadline();
        test_p29v1_k2_pipeline_head_drop_after_fill_stays_recoverable();
        test_p29v1_k2_pipeline_successor_io_death_poisons_route();
        test_p29v1_k2_pipeline_no_eager_fill_falls_back_to_head();
        test_p29v1_k1_pipeline_wire_identity();
        test_p29v1_k2_real_straight_line_30tu();
        test_p29v1_k2_real_successor_closed_while_parked();
        test_p29v1_k2_real_head_killed_after_fill();
        test_p29v1_k2_real_no_eager_fill_all_commit();
        test_route_pipeline_prepark_when_begin_lands_after_hello();
        test_route_pipeline_prepark_grace_falls_through_after_head_commit();
        test_route_pipeline_prepark_grace_expiry_falls_through_not_throws();
        test_route_pipeline_prepark_large_tu_single_drain_decides_from_first_frame();
        test_route_pipeline_prepark_straddling_body_frame_finishes_before_dispatch();
        test_route_pipeline_park_straddling_body_frame_finishes_before_dispatch();
        test_route_pipeline_prepark_third_hello_in_release_to_install_gap();
        test_route_pipeline_prepark_k3_collision_re_arms_on_busy_release();
        test_route_pipeline_prepark_k3_storm_two_begin_unknown_release();
        test_route_pipeline_prepark_k3_s2_begin_during_s1_stage_gap();
        test_p29v1_committed_sender_abandons_chain_under_foreign_poison();
        test_p29v1_k2_real_three_tx_dispatch_across_promotion_gap();
        std::cout << "p50_endpoint_test: focused route pipeline PASS\n";
        return 0;
    }
    if (std::getenv("ICECC_P50_PROFILE_DIGEST_MUTANT_FOCUS") != nullptr) {
        test_profile_materialized_result_digest_gates();
        std::cout << "p50_endpoint_test: focused profile digest gates PASS\n";
        return 0;
    }
    test_action_hold_rendezvous();
    test_split_prepare_marks_nonrunnable_exactly_once();
    test_normal_zero_and_completion_stamps();
    test_completion_stamp_correspondence();
    test_completion_live_identity_correspondence();
    test_commit_identity_negative_matrix();
    test_lost_final_commit_identity_negative_matrix();
    test_idempotent_prepare_admission();
    test_candidate_stage_has_no_revision_residue();
    test_input_record_owner_and_aggregate_limits();
    test_fragmentation_at_every_control_and_body_boundary();
    test_exact_replay_and_lost_final();
    test_completion_identity_and_store_replacement();
    test_same_f_route_reset();
    test_reset_ack_equality_and_terminal_result();
    test_handshake_binding_and_namespace_rules();
    test_unnegotiated_begin_rejected_and_route_preserved();
    test_client_outbound_mask_law();
    test_client_outbound_callsite_law();
    test_reserved_zero_endpoint_values();
    test_interrupted_begin_identity();
    test_terminal_body_failure_identity();
    test_disconnect_at_each_message_boundary();
    test_client_operation_scoped_cancellation();
    test_component_and_allocation_caps();
    test_client_deadline_and_direct_socket_ownership();
    test_client_completion_deadline_is_fresh_before_first_write();
    test_adopted_endpoint_exact_zstd_and_ownership();
    test_adopted_endpoint_disconnect_and_invalid_rows();
    test_adopted_cross_executor_releases_registration();
    test_adopted_codec_runs_off_the_owner_thread();
    test_adopted_codec_slots_release_per_job();
    test_adopted_selector_cancel_fences_publication();
    test_adopted_completion_cancel_is_rechecked();
    test_two_client_one_server_isolation();
    test_zstd_route_endpoint_continuation_and_retry();
    test_zstd_route_authority_bounded_history();
    test_server_routes_are_isolated_by_profile();
    test_p29v1_endpoint_route_dialogue_lifetime();
    test_p29v1_pipelined_begin();
    test_route_pipeline_park_releases_in_commit_order();
    test_route_pipeline_reset_before_begin_does_not_park_on_stale_cursor();
    test_p29v1_k1_pipeline_wire_identity();
    test_p29v1_k1_pipeline_waiters_never_used();
    test_p29v1_k2_pipeline_straight_line();
    test_p29v1_k2_pipeline_successor_commit_dispatched_first();
    test_p29v1_k2_pipeline_successor_state_mismatch_poisons();
    test_p29v1_k2_pipeline_head_disconnect_tears_down_successor();
    test_p29v1_k2_pipeline_successor_wait_deadline();
    test_p29v1_k2_pipeline_head_drop_after_fill_stays_recoverable();
    test_p29v1_k2_pipeline_successor_io_death_poisons_route();
    test_p29v1_k2_pipeline_no_eager_fill_falls_back_to_head();
    test_p29v1_k2_real_straight_line_30tu();
    test_p29v1_k2_real_successor_closed_while_parked();
    test_p29v1_k2_real_head_killed_after_fill();
    test_p29v1_k2_real_no_eager_fill_all_commit();
    test_route_pipeline_prepark_when_begin_lands_after_hello();
    test_route_pipeline_prepark_grace_falls_through_after_head_commit();
    test_route_pipeline_prepark_grace_expiry_falls_through_not_throws();
    test_route_pipeline_prepark_large_tu_single_drain_decides_from_first_frame();
    test_route_pipeline_prepark_straddling_body_frame_finishes_before_dispatch();
    test_route_pipeline_park_straddling_body_frame_finishes_before_dispatch();
    test_route_pipeline_prepark_third_hello_in_release_to_install_gap();
    test_route_pipeline_prepark_k3_collision_re_arms_on_busy_release();
    test_route_pipeline_prepark_k3_storm_two_begin_unknown_release();
    test_route_pipeline_prepark_k3_s2_begin_during_s1_stage_gap();
    test_p29v1_committed_sender_abandons_chain_under_foreign_poison();
    test_p29v1_k2_real_three_tx_dispatch_across_promotion_gap();
    test_idle_p29v1_route_history_is_dropped_for_dead_stores();
    test_live_global_resource_trace();
    test_automatic_action_trace_is_complete_past_1024_records();
    if (s3_resource_storm_requested())
        test_s3_resource_storm_product_path();
    report_zstd1_metrics(performance_gate);
    std::cout << "p50_endpoint_test: PASS\n";
    return 0;
}
